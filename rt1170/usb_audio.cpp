// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// USB Audio Class 2 stream and controls: request encoding, the firmware
// instance and its C entry points
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: as usb_audio.hpp. Each C entry point runs in its caller's context,
// documented in usb/usb_audio_callbacks.h; TinyUSB's assert hook, which may run
// in a task or in the USB ISR, takes its own critical section.
// Contract: GET answers are built at compile time, byte for byte as UAC2 lays
// them out. The IN FIFO is reached only through the m110_tinyusb_audio_in_* shim;
// an armed play's frames come from player_consume_from_isr().

#include "usb_audio.hpp"

#include "os/critical_section.hpp"
#include "player.hpp"
#include "usb/usb_audio_callbacks.h"
#include "usb/usb_tinyusb_port.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace waveform_generator::usb_audio
{
namespace
{
constexpr std::uint8_t request_cur = wfg_uac_request_cur;
constexpr std::uint8_t request_range = wfg_uac_request_range;
constexpr std::uint8_t clock_source = wfg_uac_entity_clock_source;
constexpr std::uint8_t feature_unit = wfg_uac_entity_feature_unit;
constexpr std::uint8_t clock_frequency = wfg_uac_clock_frequency;
constexpr std::uint8_t clock_validity = wfg_uac_clock_validity;
constexpr std::uint8_t feature_mute = wfg_uac_feature_mute;
constexpr std::uint8_t feature_volume = wfg_uac_feature_volume;
constexpr std::uint8_t master_channel = wfg_uac_master_channel;
constexpr std::uint8_t mute_off = 0U;
constexpr std::uint8_t mute_on = 1U;
constexpr std::uint8_t clock_is_valid = 1U;
constexpr std::uint16_t one_subrange = 1U;
constexpr std::uint32_t fixed_rate_resolution_hz = 0U;

constexpr m110::Status unsupported{.code = m110::StatusCode::invalid_argument, .message = "UAC2 request not supported"};
constexpr m110::Status rejected_value{.code = m110::StatusCode::invalid_argument, .message = "UAC2 value rejected"};

template <class T> [[nodiscard]] constexpr std::array<std::uint8_t, sizeof(T)> le_bytes(T value) noexcept
{
    return std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
}

template <std::size_t... Sizes> [[nodiscard]] constexpr std::array<std::uint8_t, (Sizes + ...)> join(const std::array<std::uint8_t, Sizes>&... parts) noexcept
{
    std::array<std::uint8_t, (Sizes + ...)> joined{};
    auto out = joined.begin();
    ((out = std::ranges::copy(parts, out).out), ...);
    return joined;
}

template <std::size_t Size> [[nodiscard]] constexpr ControlResponse make_response(const std::array<std::uint8_t, Size>& payload) noexcept
{
    static_assert(Size <= control_response_capacity);
    ControlResponse response{};
    std::ranges::copy(payload, response.bytes.begin());
    response.length = ByteCount{.value = Size};
    return response;
}

// UAC2 layout 3 (4-byte) and layout 2 (2-byte) parameter blocks: CUR is the
// value; RANGE is wNumSubRanges, then MIN, MAX and RES of each subrange.
constexpr ControlResponse sample_rate_cur = make_response(le_bytes(sample_rate_hz));
constexpr ControlResponse sample_rate_range = make_response(join(le_bytes(one_subrange), le_bytes(sample_rate_hz), le_bytes(sample_rate_hz), le_bytes(fixed_rate_resolution_hz)));
constexpr ControlResponse clock_valid_cur = make_response(le_bytes(clock_is_valid));
constexpr ControlResponse volume_range = make_response(join(le_bytes(one_subrange), le_bytes(volume_min.value), le_bytes(volume_max.value), le_bytes(volume_resolution.value)));

static_assert(sample_rate_range.length.value == control_response_capacity, "the clock RANGE is the longest answer");
static_assert(control_response_capacity <= std::numeric_limits<std::uint16_t>::max());

// Exactly sizeof(T) little-endian bytes, or a failure.
template <class T> [[nodiscard]] m110::Result<T> from_le_bytes(std::span<const std::uint8_t> data) noexcept
{
    if (data.size() != sizeof(T))
    {
        return rejected_value;
    }

    std::array<std::uint8_t, sizeof(T)> raw{};
    std::ranges::copy(data, raw.begin());
    return std::bit_cast<T>(raw);
}

[[nodiscard]] bool is_cur(const ControlRequest& request, std::uint8_t selector) noexcept
{
    return request.selector == selector && request.code == request_cur;
}

[[nodiscard]] bool is_range(const ControlRequest& request, std::uint8_t selector) noexcept
{
    return request.selector == selector && request.code == request_range;
}

[[nodiscard]] m110::Result<ControlResponse> clock_get(const ControlRequest& request) noexcept
{
    if (is_cur(request, clock_frequency))
    {
        return sample_rate_cur;
    }

    if (is_range(request, clock_frequency))
    {
        return sample_rate_range;
    }

    if (is_cur(request, clock_validity))
    {
        return clock_valid_cur;
    }

    return unsupported;
}

[[nodiscard]] m110::Result<ControlResponse> feature_get(const ControlRequest& request, const HostSettings& settings) noexcept
{
    if (is_cur(request, feature_mute))
    {
        return make_response(le_bytes(settings.muted ? mute_on : mute_off));
    }

    if (is_cur(request, feature_volume))
    {
        return make_response(le_bytes(settings.volume.value));
    }

    if (is_range(request, feature_volume))
    {
        return volume_range;
    }

    return unsupported;
}

// The clock is fixed: SET CUR succeeds only when it names the current rate.
[[nodiscard]] m110::Status check_sample_rate(std::span<const std::uint8_t> data) noexcept
{
    const auto rate = from_le_bytes<std::uint32_t>(data);
    return rate && rate.value() == sample_rate_hz ? m110::Status::success() : rejected_value;
}

[[nodiscard]] m110::Status store_mute(std::span<const std::uint8_t> data, HostSettings& settings) noexcept
{
    const auto mute = from_le_bytes<std::uint8_t>(data);

    if (!mute || mute.value() > mute_on)
    {
        return rejected_value;
    }

    settings.muted = mute.value() == mute_on;
    return m110::Status::success();
}

[[nodiscard]] m110::Status store_volume(std::span<const std::uint8_t> data, HostSettings& settings) noexcept
{
    const auto volume = from_le_bytes<std::int16_t>(data);

    if (!volume || volume.value() < volume_min.value || volume.value() > volume_max.value)
    {
        return rejected_value;
    }

    settings.volume = VolumeDb256{.value = volume.value()};
    return m110::Status::success();
}

// The C shim carries a write's length as uint16_t, like TinyUSB's FIFO. A
// packet is never longer than the largest format's (pack_frames()), which
// fits, so this is that boundary's one cast.
[[nodiscard]] std::uint16_t in_write_length(PacketBytes packet) noexcept
{
    static_assert(max_packet_byte_count <= std::numeric_limits<std::uint16_t>::max());
    return static_cast<std::uint16_t>(std::min(packet.size(), max_packet_byte_count));
}

// TinyUSB's IN FIFO through the C shim: this module's only path to TinyUSB.
class TinyUsbInFifo
{
public:
    [[nodiscard]] ByteCount queued() const noexcept { return ByteCount{.value = m110_tinyusb_audio_in_queued_bytes()}; }

    [[nodiscard]] ByteCount free_space() const noexcept { return ByteCount{.value = m110_tinyusb_audio_in_free_bytes()}; }

    [[nodiscard]] ByteCount write(PacketBytes packet) noexcept { return ByteCount{.value = m110_tinyusb_audio_in_write(packet.data(), in_write_length(packet))}; }
};

// The player's output FIFO, the stream's source while a play is armed.
struct PlayerSource
{
    [[nodiscard]] static FrameCount fill(ChunkFrames frames) noexcept { return player_consume_from_isr(frames); }
};

static_assert(InFifo<TinyUsbInFifo>);
static_assert(FrameSource<Silence> && FrameSource<PlayerSource>);

// The firmware instances. Constant-initialized, so the ISR never runs a guard.
[[nodiscard]] UacStream& stream() noexcept
{
    static constinit UacStream instance;
    return instance;
}

[[nodiscard]] UacControls& controls() noexcept
{
    static UacControls instance;
    return instance;
}

// Recorded from any context: TinyUSB asserts in the wfg_usb task and in the
// USB ISR. Its record is several words, so it is written and read under
// AnyContextCriticalSection.
[[nodiscard]] StackAssertLog& stack_assert_log() noexcept
{
    static constinit StackAssertLog instance;
    return instance;
}

// The return address __builtin_return_address() gives as a pointer, as the
// 32-bit value a CodeAddress holds: the compiler boundary's one cast. On the
// device it keeps every bit; a host build keeps the low 32.
[[nodiscard]] std::uint32_t code_address(const void* return_address) noexcept
{
    return static_cast<std::uint32_t>(std::bit_cast<std::uintptr_t>(return_address));
}

[[nodiscard]] constexpr ControlRequest to_request(const wfg_uac_request_t& request) noexcept
{
    return {.code = request.code, .entity = request.entity, .selector = request.selector, .channel = request.channel};
}

// The C ABI carries the endpoint's state as a bool.
[[nodiscard]] constexpr InTransfer to_in_transfer(bool transfer_primed) noexcept
{
    return transfer_primed ? InTransfer::primed : InTransfer::unprimed;
}

// The C ABI carries lengths as uint16_t, like TinyUSB's control transfer API.
// Every answer fits (static_assert above), so this is the C ABI's one cast.
[[nodiscard]] constexpr std::uint16_t c_length(ByteCount length) noexcept
{
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(length.value, control_response_capacity));
}

constexpr std::size_t record_index_mask = chunk_record_capacity - 1U;
// A record whose chunk is further behind frames_sent than every queued chunk
// can be is not one of this stream's: never retired as sent.
constexpr std::uint32_t max_outstanding_frames = std::uint32_t{chunk_record_capacity} * chunk_frames.value;
} // namespace

m110::Status ChunkRecords::push(const ChunkRecord& record) noexcept
{
    if (full())
    {
        return chunk_records_full;
    }

    const std::span<ChunkRecord, chunk_record_capacity> slots{records_};
    slots[(first_ + count_) & record_index_mask] = record;
    ++count_;
    return m110::Status::success();
}

std::optional<ChunkRecord> ChunkRecords::front() const noexcept
{
    if (empty())
    {
        return std::nullopt;
    }

    const std::span<const ChunkRecord, chunk_record_capacity> slots{records_};
    return slots[first_];
}

void ChunkRecords::pop() noexcept
{
    if (empty())
    {
        return;
    }

    first_ = (first_ + 1U) & record_index_mask;
    --count_;
}

void ChunkRecords::drop_newest() noexcept
{
    if (empty())
    {
        return;
    }

    --count_;
}

void ChunkRecords::forget_file_frames() noexcept
{
    const std::span<ChunkRecord, chunk_record_capacity> slots{records_};

    for (std::size_t queued = 0U; queued < count_; ++queued)
    {
        slots[(first_ + queued) & record_index_mask].file_frames = {};
    }
}

// The stream stops for any reason, a change of format included: TinyUSB drops
// its FIFO, so the queued chunks will never go out, and an armed play is
// interrupted.
void UacStream::end_stream() noexcept
{
    counters_.state = StreamState::closed;
    counters_.format = {};
    records_.clear();
    play_.pending_frames = {};
    play_judged_ = false;
    in_flight_ = false;
    start_at_.reset();

    if (play_.armed)
    {
        play_.armed = false;
        play_.interrupted = true;
    }
}

// A PLAY was accepted, on either route: every play statistic restarts, and the
// chunks still queued for an earlier play carry no file frames for any play,
// so the new play's statistics never mix in an earlier play's frames.
void UacStream::restart_play(FrameCount accept_frame_index) noexcept
{
    play_ = PlayStatistics{.accept_frame_index = accept_frame_index};
    play_judged_ = false;
    play_mismatched_ = false;
    queued_crcs_ = ChannelCrcs{};
    start_at_.reset();
    records_.forget_file_frames();
}

// `fifo_level` is the FIFO as TinyUSB left it, having loaded the next packet.
void UacStream::commit(const Completion& completion, ByteCount fifo_level) noexcept
{
    note_progress(completion.tick);
    const FrameCount confirmed = confirm_loaded(fifo_level);
    note_mismatch(completion.sent, confirmed);

    // TinyUSB queues a zero-length packet before the stream opens and repeats it
    // until the pre-fill reaches the endpoint; those primers are not data.
    if (counters_.state == StreamState::priming && confirmed == FrameCount{})
    {
        return;
    }

    if (completion.sent != packet_bytes(counters_.format))
    {
        ++counters_.short_packets;
    }

    counters_.state = StreamState::streaming;
    track_interval(completion);
    counters_.frames_sent.value += confirmed.value;
    retire_sent_chunks();
}

// The frames a completion confirms sent: the packet TinyUSB loaded at the
// completion before it, one chunk or none (in_flight_). Then whether the
// packet TinyUSB has just loaded holds a chunk: TinyUSB has taken more of the
// stream's frames than are confirmed and still in the FIFO. A packet is
// never more than one chunk: TinyUSB loads at most the alternate's
// wMaxPacketSize, its format's packet (usb/usb_descriptors.c), and the
// producer writes whole packets. Only a closed stream has no format, and a
// closed stream commits nothing (on_tx_done), so the frame size is never
// zero here.
FrameCount UacStream::confirm_loaded(ByteCount fifo_level) noexcept
{
    const FrameCount confirmed = in_flight_ ? chunk_frames : FrameCount{};
    const std::uint32_t unconfirmed = counters_.frames_queued.value - (counters_.frames_sent.value + confirmed.value);
    in_flight_ = unconfirmed > fifo_level.value / frame_bytes(counters_.format).value;
    return confirmed;
}

// A completion whose reported size is not the packet TinyUSB loaded for it:
// counted since boot, and, while the play has file frames queued (before
// this completion retires any), the play's delivery is unconfirmed: the host
// may not have received the packet, whose frames still count as sent.
void UacStream::note_mismatch(ByteCount reported, FrameCount confirmed) noexcept
{
    const ByteCount loaded{.value = confirmed.value * frame_bytes(counters_.format).value};

    if (reported == loaded)
    {
        return;
    }

    if (counters_.sent_mismatches != std::numeric_limits<std::uint16_t>::max())
    {
        ++counters_.sent_mismatches;
    }

    play_mismatched_ = play_mismatched_ || play_.pending_frames != FrameCount{};
}

// Restarts the stall clock at a completion. A gap longer than the stall
// timeout while armed is judged here, when the completion that ends it comes,
// so it counts even when the player task only looks after the gap has ended.
void UacStream::note_progress(TickCount tick) noexcept
{
    if (play_.armed && tick.value - play_.progress_tick.value > stall_timeout_.value)
    {
        play_.stalled = true;
    }

    play_.progress_tick = tick;
}

void UacStream::on_watchdog(InTransfer transfer) noexcept
{
    if (counters_.state != StreamState::streaming)
    {
        return;
    }

    ++counters_.watchdog_expiries;
    GapSamples& samples = interval_.samples;

    if (transfer == InTransfer::primed)
    {
        ++samples.host_skipped.value;
    }
    else
    {
        samples.unprimed = true;
    }
}

// Extends the 11-bit frame number by the frames elapsed since the stream's
// previous completion (one per millisecond while the host polls; past 2047
// frames by the ticks, dealiased_gap), from the stream's first data completion
// on, and judges the gap for the stream and the play. The watchdog's samples
// since the previous completion belong to this gap, and to no later one.
void UacStream::track_interval(const Completion& completion) noexcept
{
    const std::uint32_t number = completion.frame.value & usb_frame_number_mask;
    const GapSamples samples = std::exchange(interval_.samples, GapSamples{});
    // Each completion tells the latest gap's late read from a miss; a gap
    // that adds to the play's count leaves it unconfirmed (count_play_missed).
    play_.miss_unconfirmed = false;

    if (!interval_.valid)
    {
        interval_ = {.last = UsbFrameCount{.value = number}, .last_tick = completion.tick, .valid = true};
        return;
    }

    const UsbFrameSpan aliased{.value = (number - interval_.last.value) & usb_frame_number_mask};
    const UsbFrameSpan elapsed = dealiased_gap(aliased, TickSpan{.value = completion.tick.value - interval_.last_tick.value});
    interval_.last.value += elapsed.value;
    interval_.last_tick = completion.tick;
    counters_.max_gap = std::max(counters_.max_gap, elapsed);
    const bool for_play = judges_for_play(elapsed);
    const IntervalCount stream_unprimed_before = interval_.stream_unprimed;
    count_missed(elapsed, samples, for_play);

    if (for_play)
    {
        count_play_missed(elapsed, stream_unprimed_before);
    }
}

// Whether the play judges this gap: each gap that ends while the play has
// file frames queued (before this completion retires any), since they reach
// the PC that much later, and a zero gap right after one, which may take back
// a late read the play counted (count_play_missed).
bool UacStream::judges_for_play(UsbFrameSpan elapsed) noexcept
{
    const bool delays_file = play_.pending_frames != FrameCount{};
    play_judged_ = delays_file || (play_judged_ && elapsed == UsbFrameSpan{});
    return play_judged_;
}

// The stream's count by missed_after_gap(): its frames spanned minus its
// completions, at most one high for the 1 ms until the next completion. The
// count since boot moves with it. A zero gap takes a late read back from the
// latest gap that counts one (LatestGap). A gap that misses intervals is
// located, its unprimed part counted (unprimed_part). A gap whose primed
// samples prove more host skips than it misses holds an interval that an
// earlier late read counted in the latest gap: that interval moves here, as
// the host's, and the totals stay.
void UacStream::count_missed(UsbFrameSpan elapsed, const GapSamples& samples, bool for_play) noexcept
{
    const IntervalCount before = interval_.stream_missed;
    interval_.stream_missed = missed_after_gap(before, elapsed);
    counters_.missed_intervals = with_part_updated(counters_.missed_intervals, before, interval_.stream_missed);

    if (interval_.stream_missed < before)
    {
        take_back_latest_gap();
        return;
    }

    IntervalCount added{.value = interval_.stream_missed.value - before.value};

    if (samples.host_skipped > added && interval_.latest_gap.counted != IntervalCount{})
    {
        take_back_latest_gap();
        ++added.value;
    }

    if (added != IntervalCount{})
    {
        locate_miss(added, unprimed_part(added, samples), for_play);
    }
}

// The gap this completion ends counts `added` intervals, `unprimed` of them
// the device's: it is the latest, at the first frame this completion sent.
void UacStream::locate_miss(IntervalCount added, IntervalCount unprimed, bool for_play) noexcept
{
    interval_.latest_gap = {.counted = added, .unprimed = unprimed, .earlier = counters_.last_miss};
    counters_.last_miss = {.frame_index = counters_.frames_sent, .unprimed = unprimed, .play = for_play};
    set_stream_unprimed(IntervalCount{.value = interval_.stream_unprimed.value + unprimed.value});
}

// One interval of the latest gap was a late read: a zero gap took it back
// from the stream's count, or a later gap's host skip holds it. An unprimed
// one goes first, with the stream's unprimed count; a gap that no longer
// counts any interval gives the location back to the one before it.
void UacStream::take_back_latest_gap() noexcept
{
    LatestGap& gap = interval_.latest_gap;

    if (gap.counted == IntervalCount{})
    {
        return;
    }

    gap.counted = missed_after_gap(gap.counted, UsbFrameSpan{});

    if (gap.unprimed != IntervalCount{})
    {
        gap.unprimed = missed_after_gap(gap.unprimed, UsbFrameSpan{});
        set_stream_unprimed(missed_after_gap(interval_.stream_unprimed, UsbFrameSpan{}));
    }

    if (gap.counted == IntervalCount{})
    {
        counters_.last_miss = gap.earlier;
        gap = {};
        return;
    }

    counters_.last_miss.unprimed = gap.unprimed;
}

// The unprimed count since boot moves with the open stream's, as the missed
// count does.
void UacStream::set_stream_unprimed(IntervalCount updated) noexcept
{
    counters_.unprimed_intervals = with_part_updated(counters_.unprimed_intervals, interval_.stream_unprimed, updated);
    interval_.stream_unprimed = updated;
}

// The play's own count, by the stream's rule over the gaps that delayed its
// file (judges_for_play): each gap that ends while the play has file frames
// queued, since they reach the PC that much later. Armed or not: the chunks a
// STOP, a fault or a stall leaves queued still go out and still count, so the
// gaps before them count too, whoever notices a stall first. No file frame is
// queued before the play's first completion after arming, so a skip that
// ended before the arming, or was still going on at it, stays the stream's;
// nor after the completion that confirms its last file frame sent, so a skip
// after that, before the player disarms, stays the stream's too. A completion
// after a judged one may still take back a late read the play counted. The
// first miss is located at the completion whose gap lifts the count from
// zero; a take-back to zero clears it again (that interval was a late read,
// not a miss). The unprimed part follows the stream's unprimed count over the
// same gap (from `stream_unprimed_before`), take-backs and moves included,
// and never exceeds the play's count. A gap that adds to the count leaves
// it unconfirmed until the next completion (confirmed_missed()).
void UacStream::count_play_missed(UsbFrameSpan elapsed, IntervalCount stream_unprimed_before) noexcept
{
    const IntervalCount missed = missed_after_gap(play_.missed_intervals, elapsed);

    if (missed == IntervalCount{})
    {
        play_.first_miss_frame_index = {};
    }
    else if (play_.missed_intervals == IntervalCount{})
    {
        play_.first_miss_frame_index = counters_.frames_sent;
    }

    play_.miss_unconfirmed = missed > play_.missed_intervals;
    play_.missed_intervals = missed;
    play_.unprimed_intervals = std::min(moved_with(play_.unprimed_intervals, stream_unprimed_before, interval_.stream_unprimed), missed);
}

void UacStream::retire_sent_chunks() noexcept
{
    for (auto oldest = records_.front(); oldest.has_value(); oldest = records_.front())
    {
        const std::uint32_t sent_since_start = counters_.frames_sent.value - oldest->start.value;

        if (sent_since_start < chunk_frames.value || sent_since_start > max_outstanding_frames)
        {
            return;
        }

        apply(*oldest);
        records_.pop();
    }
}

// A chunk is confirmed sent: its file frames, which lead the chunk, join the
// play's sent prefix and its CRCs become the play's. The first file frame
// latches the format it went out in: a chunk is sent in the format it was
// written in, since a change of format drops every queued chunk.
void UacStream::apply(const ChunkRecord& record) noexcept
{
    if (record.file_frames == FrameCount{})
    {
        return;
    }

    if (play_.file_frames == FrameCount{})
    {
        play_.first_frame_index = record.start;
        play_.first_frame_sof = interval_.last;
        play_.format = counters_.format;
    }

    play_.file_frames.value += record.file_frames.value;
    play_.last_frame_index.value = record.start.value + record.file_frames.value - 1U;
    play_.crc32_left = record.crc32_left;
    play_.crc32_right = record.crc32_right;
    play_.pending_frames.value -= std::min(play_.pending_frames.value, record.file_frames.value);
}

// The play's CRCs once the chunk's file frames, which lead it, are added as
// the PCM24 bytes of their source samples, whatever the stream's format.
// fill() never claims more than the chunk holds.
UacStream::ChannelCrcs UacStream::crcs_with(FrameCount file_frames) const noexcept
{
    ChannelCrcs crcs = queued_crcs_;

    for (const StereoFrame& frame : std::span{chunk_}.first(file_frames.value))
    {
        crcs.left.update(pcm24_bytes(frame.left));
        crcs.right.update(pcm24_bytes(frame.right));
    }

    return crcs;
}

// The chunk and its record are queued: it holds the next chunk_frames stream
// frames, and its file frames wait for their completion.
void UacStream::count_queued(FrameCount file_frames, const ChannelCrcs& crcs) noexcept
{
    queued_crcs_ = crcs;
    counters_.frames_queued.value += chunk_frames.value;
    counters_.source_frames.value += file_frames.value;
    play_.pending_frames.value += file_frames.value;
}

m110::Status UacStream::accept_play() noexcept
{
    if (counters_.state == StreamState::closed)
    {
        return stream_not_open;
    }

    restart_play(counters_.frames_sent);
    play_opens_ = counters_.opens;
    return m110::Status::success();
}

void UacStream::clear_play() noexcept
{
    restart_play(FrameCount{});
}

m110::Status UacStream::arm(TickCount now, TickSpan stall_timeout, std::optional<ScheduledStart> start) noexcept
{
    if (counters_.state == StreamState::closed || counters_.opens != play_opens_)
    {
        play_.armed = false;
        play_.interrupted = true;
        return stream_interrupted;
    }

    // The next chunk to fill starts at frames_queued: a start this stream
    // has already passed, or one planned on an earlier stream, waits for none.
    start_at_.reset();

    if (start.has_value() && start->stream_opens == counters_.opens && !index_reached(counters_.frames_queued, start->index))
    {
        start_at_ = start->index;
    }

    stall_timeout_ = stall_timeout;
    play_.armed = true;
    play_.progress_tick = now;
    return m110::Status::success();
}

void StackAssertLog::record(CodeAddress pc, FrameCount frame_index, TickCount tick) noexcept
{
    if (asserts_.count == 0U)
    {
        asserts_.first_pc = pc;
    }

    if (asserts_.count != std::numeric_limits<std::uint32_t>::max())
    {
        ++asserts_.count;
    }

    asserts_.last_pc = pc;
    asserts_.last_frame_index = frame_index;
    asserts_.last_tick = tick;
}

m110::Result<ControlResponse> UacControls::get(const ControlRequest& request) const noexcept
{
    if (request.channel != master_channel)
    {
        return unsupported;
    }

    if (request.entity == clock_source)
    {
        return clock_get(request);
    }

    if (request.entity == feature_unit)
    {
        return feature_get(request, settings_);
    }

    return unsupported;
}

m110::Status UacControls::set(const ControlRequest& request, std::span<const std::uint8_t> data) noexcept
{
    if (request.code != request_cur || request.channel != master_channel)
    {
        return unsupported;
    }

    if (request.entity == clock_source && request.selector == clock_frequency)
    {
        return check_sample_rate(data);
    }

    if (request.entity == feature_unit && request.selector == feature_mute)
    {
        return store_mute(data, settings_);
    }

    if (request.entity == feature_unit && request.selector == feature_volume)
    {
        return store_volume(data, settings_);
    }

    return unsupported;
}

StreamCounters stream_counters() noexcept
{
    return stream().counters();
}

PlayStatistics play_statistics() noexcept
{
    return stream().play();
}

bool play_mismatched() noexcept
{
    return stream().play_mismatched();
}

m110::Status accept_play() noexcept
{
    return stream().accept_play();
}

void clear_play() noexcept
{
    stream().clear_play();
}

m110::Status arm_play(TickCount now, TickSpan stall_timeout, std::optional<ScheduledStart> start) noexcept
{
    return stream().arm(now, stall_timeout, start);
}

void disarm_play() noexcept
{
    stream().disarm();
}

HostSettings host_settings() noexcept
{
    return controls().settings();
}

StackAsserts stack_asserts() noexcept
{
    const AnyContextCriticalSection lock;
    return stack_assert_log().asserts();
}

} // namespace waveform_generator::usb_audio

// The C entry points. They keep the C linkage of their declarations in
// usb/usb_audio_callbacks.h and adapt C values to the module's types.
namespace usb_audio = waveform_generator::usb_audio;

wfg_uac_response_t wfg_usb_audio_get_request(wfg_uac_request_t request)
{
    wfg_uac_response_t answer{.result = kWFG_UacRequestStall, .length = 0U, .bytes = {}};
    const auto response = usb_audio::controls().get(usb_audio::to_request(request));

    if (response)
    {
        std::ranges::copy(response.value().bytes, std::span{answer.bytes}.begin());
        answer.length = usb_audio::c_length(response.value().length);
        answer.result = kWFG_UacRequestAccepted;
    }

    return answer;
}

wfg_uac_request_result_t wfg_usb_audio_set_request(wfg_uac_request_t request, const std::uint8_t* data, std::uint16_t length)
{
    // TinyUSB's control buffer, valid for the duration of this call.
    const std::span<const std::uint8_t> payload{data, length};
    const auto status = usb_audio::controls().set(usb_audio::to_request(request), payload);
    return status.is_ok() ? kWFG_UacRequestAccepted : kWFG_UacRequestStall;
}

void wfg_usb_audio_configure_stream()
{
    usb_audio::stream().configure();
}

void wfg_usb_audio_deconfigure_stream()
{
    usb_audio::stream().deconfigure();
}

wfg_uac_request_result_t wfg_usb_audio_open_stream(std::uint8_t alternate)
{
    const auto format = usb_audio::offered_format(alternate);

    if (!format)
    {
        return kWFG_UacRequestStall;
    }

    usb_audio::TinyUsbInFifo fifo;
    return usb_audio::stream().open(fifo, format.value()).is_ok() ? kWFG_UacRequestAccepted : kWFG_UacRequestStall;
}

void wfg_usb_audio_close_stream()
{
    usb_audio::stream().close();
}

void wfg_usb_audio_tx_done(std::uint16_t bytes_sent, std::uint16_t frame_number, std::uint32_t tick)
{
    usb_audio::TinyUsbInFifo fifo;
    usb_audio::PlayerSource player;
    const usb_audio::Completion completion{.sent = usb_audio::ByteCount{.value = bytes_sent}, .frame = usb_audio::UsbFrameNumber{.value = frame_number}, .tick = usb_audio::TickCount{.value = tick}};
    usb_audio::stream().on_tx_done(fifo, player, completion);
}

void wfg_usb_audio_watchdog_expired(bool transfer_primed)
{
    usb_audio::stream().on_watchdog(usb_audio::to_in_transfer(transfer_primed));
}

// With CFG_TUSB_DEBUG 0, TU_ASSERT(cond, ret) expands to
// if (!(cond)) { TU_BREAKPOINT(); return ret; }
// (third_party/tinyusb/src/common/tusb_verify.h), and TU_BREAKPOINT() to a
// plain call of this hook (usb/tusb_config.h); TinyUSB also calls
// TU_BREAKPOINT() directly, for instance where it stalls a host request it
// does not support. The hook's return address is therefore in the asserting
// function, or in the function it was inlined into, whose inline chain
// addr2line -i shows. In a function returning nothing that call is a tail
// call, which -O3 would make a jump, returning to the asserting function's
// caller instead, and -O3 would merge the identical calls of several asserts
// into one: rt1170/CMakeLists.txt builds TinyUSB without sibling calls, tail
// merging or cross-jumping. Never inlined, so the return address is always
// this call's. The tick is read with the FromISR call, which this port also
// allows in a task; TinyUSB runs only once the scheduler has started.
[[gnu::noinline]] void wfg_usb_count_stack_assert()
{
    const usb_audio::CodeAddress pc = usb_audio::call_site(usb_audio::code_address(__builtin_return_address(0)));
    const waveform_generator::AnyContextCriticalSection lock;
    const usb_audio::TickCount tick{.value = xTaskGetTickCountFromISR()};
    usb_audio::stack_assert_log().record(pc, usb_audio::stream().counters().frames_sent, tick);
}
