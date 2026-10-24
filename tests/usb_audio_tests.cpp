// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// UAC2 logic of rt1170/usb_audio.cpp on the host: GET answers byte for byte,
// SET validation, the silence pre-fill, packet accounting, open/close, opening
// only inside a configuration (bus reset, unplug, SET_CONFIGURATION), that the
// IN FIFO is never filled past its target, and the play statistics: chunk
// records retired only when sent (and a full record ring refusing a push),
// per-channel CRCs of the sent prefix, the first/last frame indices and USB
// frame, arming and its interruption, a scheduled start (the first file
// frame at the first chunk at or after its stream index; a passed start, one
// planned on an earlier stream and one a new PLAY forgot start at once), the
// stall mark of a polling gap past the
// timeout, missed service intervals for the stream and for the play (both ends
// of the play's window, the gaps before the chunks a disarmed play leaves
// queued, and none after its last file frame), the miss attribution (the
// latest miss's location, the largest gap, and the service watchdog's primed
// or unprimed finding that tells a host skip from a late ISR), clearing the
// play for a codec PLAY, the chunks a stalled play leaves queued, and
// completions reported short of the packet TinyUSB loaded for them, which
// still confirm it and leave no record behind (#45). The
// capture formats: packing (24-bit unchanged, 16-bit truncated, mono the left
// channel), every format's pre-fill, packets and frame count, the per-play
// CRCs that stay over the 24-bit source in every format, the play's latched
// format, a change of format mid-play, and the alternates the C entry point
// opens with and without WFG_UAC_MONO_ALT. A software FIFO stands in for
// TinyUSB's behind the m110_tinyusb_audio_in_* shim, and a test ramp for the
// player's source. TinyUSB's assert hook is called through its C entry point:
// the count, the first and latest call sites (each in the calling function),
// and the stream frame index and tick (a stub) of the latest.
// Interrupt timing and the real controller remain bench checks.

#include "common/crc32.hpp"
#include "common/pcm24.hpp"
#include "rt1170/player.hpp"
#include "rt1170/usb_audio.hpp"
#include "usb/usb_audio_callbacks.h"
#include "usb/usb_tinyusb_port.h"

#include "task.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace ua = waveform_generator::usb_audio;

class Checks
{
public:
    void expect(bool condition, const char* what) noexcept
    {
        if (!condition)
        {
            ++failures_;
            std::fputs("FAIL: ", stdout);
            std::fputs(what, stdout);
            std::fputs("\n", stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

// The 24-bit stereo format (alternate 1), in which most tests stream.
constexpr ua::ByteCount pcm24_packet = ua::packet_bytes(ua::pcm24_stereo);
constexpr ua::ByteCount pcm24_frame = ua::frame_bytes(ua::pcm24_stereo);
constexpr ua::ByteCount pcm24_target = ua::fifo_target_bytes(ua::pcm24_stereo);

// Whether this build offers alternate 3, 16-bit mono: the build option, read
// independently of usb/usb_composite.h.
#if defined(WFG_UAC_MONO_ALT)
constexpr bool mono_offered = true;
#else
constexpr bool mono_offered = false;
#endif

static_assert((ua::offered_format_count == ua::stream_formats.size()) == mono_offered, "the header offers the mono alternate exactly with the option");

// A byte count of the host's sizes: the test boundary's one cast. Every
// buffer here is far below 4 GiB.
[[nodiscard]] ua::ByteCount byte_count(std::size_t bytes) noexcept
{
    return {.value = static_cast<std::uint32_t>(bytes)};
}

// A software stand-in for TinyUSB's IN FIFO. Unlike TinyUSB's, it refuses a
// write that does not fit instead of overwriting, and it records its highest
// level, so an overfill shows up as a refusal or a level above the target.
// Like TinyUSB's with flow control off, it hands the controller at most the
// selected alternate's wMaxPacketSize per packet.
class FakeFifo
{
public:
    explicit FakeFifo(std::uint32_t capacity) noexcept : capacity_{capacity} {}

    [[nodiscard]] ua::ByteCount queued() const noexcept { return {.value = level_}; }

    [[nodiscard]] ua::ByteCount free_space() const noexcept { return {.value = capacity_ - level_}; }

    [[nodiscard]] ua::ByteCount write(ua::PacketBytes packet) noexcept
    {
        const ua::ByteCount size = byte_count(packet.size());

        if (refuse_writes_ || size.value > capacity_ - level_)
        {
            ++refusals_;
            return {};
        }

        contents_.insert(contents_.end(), packet.begin(), packet.end());
        level_ += size.value;
        high_water_ = std::max(high_water_, level_);
        ++writes_;
        write_sizes_.push_back(size.value);
        return size;
    }

    // The host selected an alternate: the endpoint's wMaxPacketSize is its
    // format's packet.
    void select(const ua::StreamFormat& format) noexcept { max_packet_ = ua::packet_bytes(format); }

    // What the controller loads for the next packet: up to one packet's bytes.
    [[nodiscard]] std::vector<std::byte> load_packet()
    {
        const std::uint32_t count = std::min(level_, max_packet_.value);
        const auto end = std::next(contents_.begin(), count);
        std::vector<std::byte> packet(contents_.begin(), end);
        contents_.erase(contents_.begin(), end);
        level_ -= count;
        return packet;
    }

    // Leftover bytes at a level that is not a whole number of packets.
    void preload(std::uint32_t count)
    {
        contents_.insert(contents_.end(), count, std::byte{});
        level_ += count;
        high_water_ = std::max(high_water_, level_);
    }

    void clear() noexcept
    {
        contents_.clear();
        write_sizes_.clear();
        level_ = 0U;
        high_water_ = 0U;
        writes_ = 0U;
        refusals_ = 0U;
        refuse_writes_ = false;
    }

    // Model a FIFO that stops accepting data, to force an underrun.
    void refuse_writes() noexcept { refuse_writes_ = true; }

    void accept_writes() noexcept { refuse_writes_ = false; }

    [[nodiscard]] const std::vector<std::byte>& contents() const noexcept { return contents_; }

    [[nodiscard]] std::uint32_t high_water() const noexcept { return high_water_; }

    [[nodiscard]] std::uint32_t writes() const noexcept { return writes_; }

    [[nodiscard]] std::uint32_t refusals() const noexcept { return refusals_; }

    // The size of each write accepted since the last clear().
    [[nodiscard]] const std::vector<std::uint32_t>& write_sizes() const noexcept { return write_sizes_; }

private:
    std::vector<std::byte> contents_;
    std::vector<std::uint32_t> write_sizes_;
    ua::ByteCount max_packet_{pcm24_packet};
    std::uint32_t capacity_{};
    std::uint32_t level_{};
    std::uint32_t high_water_{};
    std::uint32_t writes_{};
    std::uint32_t refusals_{};
    bool refuse_writes_{};
};

// Reports an empty FIFO with unlimited room whatever was written: a broken
// level must not keep the producer looping.
class LyingFifo
{
public:
    [[nodiscard]] static ua::ByteCount queued() noexcept { return {}; }

    [[nodiscard]] static ua::ByteCount free_space() noexcept { return {.value = std::numeric_limits<std::uint32_t>::max() / 2U}; }

    [[nodiscard]] ua::ByteCount write(ua::PacketBytes packet) noexcept
    {
        ++writes;
        return byte_count(packet.size());
    }

    std::uint32_t writes{};
};

static_assert(ua::InFifo<FakeFifo> && ua::InFifo<LyingFifo>);

constexpr std::int32_t pcm24_min = waveform_generator::audio::pcm24_min;
constexpr std::int32_t pcm24_max = waveform_generator::audio::pcm24_max;
constexpr std::int32_t pcm24_span = pcm24_max - pcm24_min + 1;

// A 24-bit ramp from -2^23 that wraps at 2^23 - 1.
[[nodiscard]] constexpr std::int32_t ramp_next(std::int32_t sample, std::int32_t step) noexcept
{
    const std::int32_t next = sample + step;
    return next > pcm24_max ? next - pcm24_span : next;
}

// File audio: a 24-bit ramp, left = right = sample << 8. The first
// `file_frames` frames of each chunk count as file audio.
class RampSource
{
public:
    explicit RampSource(std::uint32_t file_frames) noexcept : file_frames_{file_frames} {}

    [[nodiscard]] ua::FrameCount fill(ua::ChunkFrames frames) noexcept
    {
        constexpr std::int32_t step = 4099;

        for (auto& frame : frames)
        {
            frame = {.left = sample_ * ua::subslot_scale, .right = sample_ * ua::subslot_scale};
            sample_ = ramp_next(sample_, step);
        }

        return {.value = file_frames_};
    }

    [[nodiscard]] static std::int32_t first_sample() noexcept { return pcm24_min; }

private:
    std::int32_t sample_{pcm24_min};
    std::uint32_t file_frames_{};
};

// A file of `total` frames: full chunks until the last, then silence. Left and
// right are different ramps, so a mixed-up channel shows in its CRC. Every
// frame it supplies is kept, in order.
class FileSource
{
public:
    explicit FileSource(std::uint32_t total) noexcept : remaining_{total} {}

    [[nodiscard]] ua::FrameCount fill(ua::ChunkFrames frames) noexcept
    {
        constexpr std::int32_t left_step = 70001;
        constexpr std::int32_t right_step = 3;
        const std::uint32_t file = std::min(remaining_, ua::chunk_frames.value);
        std::ranges::fill(frames, ua::StereoFrame{});

        for (auto& frame : frames.first(file))
        {
            frame = {.left = left_ * ua::subslot_scale, .right = right_ * ua::subslot_scale};
            supplied_.push_back(frame);
            left_ = ramp_next(left_, left_step);
            right_ = ramp_next(right_, right_step);
        }

        remaining_ -= file;
        ++calls_;
        return {.value = file};
    }

    [[nodiscard]] const std::vector<ua::StereoFrame>& supplied() const noexcept { return supplied_; }

    [[nodiscard]] std::uint32_t calls() const noexcept { return calls_; }

private:
    std::vector<ua::StereoFrame> supplied_;
    std::uint32_t remaining_{};
    std::uint32_t calls_{};
    std::int32_t left_{pcm24_min};
    std::int32_t right_{pcm24_max};
};

static_assert(ua::FrameSource<RampSource> && ua::FrameSource<FileSource> && ua::FrameSource<ua::Silence>);

// The 11-bit frame number of a frame count: the masked value always fits.
[[nodiscard]] ua::UsbFrameNumber frame_number(std::uint32_t frame) noexcept
{
    return {.value = static_cast<std::uint16_t>(frame & ua::usb_frame_number_mask)};
}

// One completion as the controller saw it: the frame it went out in (not
// wrapped) and the stream frames sent once it completed.
struct CompletionLog
{
    std::uint32_t frame{};
    std::size_t frames_sent{};
};

// The controller side of a stream. TinyUSB's ISR loads the next packet from
// the FIFO and then reports the packet that just completed; an alternate with
// a format starts with a zero-length primer in flight. Each millisecond is one
// USB frame and one tick. Frames start just below the 11-bit wrap, so a stream
// crosses it within its first file chunks. The log counts frames of the
// stream's format (24-bit stereo unless constructed with another).
class Controller
{
public:
    static constexpr std::uint32_t start_frame = 2044U;

    Controller() = default;

    explicit Controller(const ua::StreamFormat& format) noexcept : frame_bytes_{ua::frame_bytes(format)} {}

    template <ua::FrameSource Source> void millisecond(ua::UacStream& stream, FakeFifo& fifo, Source& source) { complete(stream, fifo, source, frame_); }

    // The ISR handles this completion just after the next frame boundary.
    template <ua::FrameSource Source> void millisecond_read_late(ua::UacStream& stream, FakeFifo& fifo, Source& source) { complete(stream, fifo, source, frame_ + 1U); }

    // The host takes the packet in flight whole, but the controller reports
    // `reported` bytes for it, fewer than TinyUSB loaded: as the ChipIdea
    // driver reports a transfer that ended in error, expected minus remaining
    // (#45).
    template <ua::FrameSource Source> void millisecond_reporting(ua::UacStream& stream, FakeFifo& fifo, Source& source, ua::ByteCount reported)
    {
        reported_ = reported;
        complete(stream, fifo, source, frame_);
    }

    // The host lets `frames` frames go by without polling.
    void skip(std::uint32_t frames) noexcept
    {
        frame_ += frames;
        tick_ += frames;
    }

    // As skip(), with the service watchdog: half a frame after each poll the
    // host leaves out it samples the endpoint, and finds the packet the last
    // completion primed still waiting.
    void skip_watched(ua::UacStream& stream, std::uint32_t frames) noexcept
    {
        for (std::uint32_t frame = 0U; frame < frames; ++frame)
        {
            stream.on_watchdog(ua::InTransfer::primed);
            skip(1U);
        }
    }

    // The host polls in this frame, but the USB ISR that handles the
    // completion is held off `late_by` frames. The service watchdog expired
    // meanwhile, so the ISR reports it first, with no transfer primed; the
    // host's polls while it waited found nothing to send. When
    // `primed_before_poll`, the ISR primes the next packet before the host's
    // poll in the frame it runs in, so the next completion comes in that same
    // frame (a zero gap); otherwise that frame is missed too.
    template <ua::FrameSource Source> void millisecond_device_late(ua::UacStream& stream, FakeFifo& fifo, Source& source, std::uint32_t late_by, bool primed_before_poll)
    {
        const std::uint32_t isr_frame = frame_ + late_by;
        stream.on_watchdog(ua::InTransfer::unprimed);
        complete(stream, fifo, source, isr_frame);
        const std::uint32_t next_poll = primed_before_poll ? isr_frame : isr_frame + 1U;
        tick_ += next_poll - frame_;
        frame_ = next_poll;
    }

    // The ISR handles this completion within a frame of the poll but past the
    // watchdog's deadline, and just after the next frame boundary: the sample
    // finds the completion pending (nothing primed), yet the next packet is
    // primed before the host's next poll. The device was on time; the gap
    // reads one frame long until a later completion takes it back.
    template <ua::FrameSource Source> void millisecond_held_in_time(ua::UacStream& stream, FakeFifo& fifo, Source& source)
    {
        stream.on_watchdog(ua::InTransfer::unprimed);
        complete(stream, fifo, source, frame_ + 1U);
    }

    [[nodiscard]] const std::vector<std::byte>& sent() const noexcept { return sent_; }

    [[nodiscard]] const std::vector<CompletionLog>& log() const noexcept { return log_; }

    [[nodiscard]] std::uint32_t tick() const noexcept { return tick_; }

private:
    // The ISR reads the frame number and the tick when it runs, in
    // `read_frame` (this frame or a later one).
    template <ua::FrameSource Source> void complete(ua::UacStream& stream, FakeFifo& fifo, Source& source, std::uint32_t read_frame)
    {
        // The packet in flight completes whole, or empty as a primer, and is
        // reported so unless millisecond_reporting() said otherwise.
        const ua::ByteCount completed = std::exchange(reported_, std::nullopt).value_or(byte_count(in_flight_.size()));
        const ua::TickCount read_tick{.value = tick_ + (read_frame - frame_)};
        sent_.insert(sent_.end(), in_flight_.begin(), in_flight_.end());
        log_.push_back({.frame = frame_, .frames_sent = sent_.size() / frame_bytes_.value});
        in_flight_ = fifo.load_packet();
        stream.on_tx_done(fifo, source, ua::Completion{.sent = completed, .frame = frame_number(read_frame), .tick = read_tick});
        ++frame_;
        ++tick_;
    }

    std::vector<std::byte> in_flight_;
    std::vector<std::byte> sent_;
    std::vector<CompletionLog> log_;
    std::optional<ua::ByteCount> reported_{};
    ua::ByteCount frame_bytes_{pcm24_frame};
    std::uint32_t frame_{start_frame};
    std::uint32_t tick_{};
};

bool all_zero(std::span<const std::byte> bytes)
{
    return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{}; });
}

// The subslot at `index` (little-endian int32) of a byte stream.
std::int32_t subslot(std::span<const std::byte> bytes, std::size_t index)
{
    std::array<std::byte, sizeof(std::int32_t)> raw{};
    std::ranges::copy(bytes.subspan(index * raw.size(), raw.size()), raw.begin());
    return std::bit_cast<std::int32_t>(raw);
}

// The stereo frame at `index` of a byte stream.
ua::StereoFrame wire_frame(std::span<const std::byte> bytes, std::size_t index)
{
    return {.left = subslot(bytes, 2U * index), .right = subslot(bytes, 2U * index + 1U)};
}

// zlib CRC-32 of one channel's samples written as a PCM24 WAV stores them
// (common/pcm24.hpp), independently of the stream's own serialization.
std::uint32_t channel_crc(std::span<const ua::StereoFrame> frames, std::int32_t ua::StereoFrame::* channel)
{
    waveform_generator::Crc32 crc;

    for (const auto& frame : frames)
    {
        std::array<std::uint8_t, waveform_generator::audio::pcm24_bytes_per_sample> bytes{};
        waveform_generator::audio::write_pcm24_le(bytes.data(), frame.*channel / ua::subslot_scale);
        crc.update(bytes);
    }

    return crc.value();
}

// The frame the controller logged for the completion that sent stream frame `index`.
std::uint32_t frame_that_sent(const Controller& controller, std::uint32_t index)
{
    const auto found = std::ranges::find_if(controller.log(), [index](const CompletionLog& entry) { return entry.frames_sent > index; });

    return found == controller.log().end() ? 0U : found->frame;
}

// SET_CONFIGURATION, then the alternate of `format` (alternate 1 unless given).
template <ua::InFifo Fifo> void configure_and_open(Checks& checks, ua::UacStream& stream, Fifo& fifo, const ua::StreamFormat& format = ua::pcm24_stereo)
{
    stream.configure();
    checks.expect(stream.open(fifo, format).is_ok(), "an alternate with a format opens a configured stream");
}

// The player's stall timeout, in ticks (one per millisecond).
constexpr ua::TickSpan stall_timeout{.value = waveform_generator::usb_stall_timeout_ms};

// A count of missed 1 ms intervals.
[[nodiscard]] constexpr ua::IntervalCount intervals(std::uint32_t count) noexcept
{
    return {.value = count};
}

// PLAY accepted, then the player arms the stream.
void accept_and_arm(Checks& checks, ua::UacStream& stream, ua::TickCount now)
{
    checks.expect(stream.accept_play().is_ok(), "PLAY is accepted on an open stream");
    checks.expect(stream.arm(now, stall_timeout).is_ok(), "the accepted play arms");
}

// Every play statistic at zero and no flag set: no USB play. progress_tick is
// the stall clock, which every completion restarts, not a statistic.
[[nodiscard]] bool is_no_play(const ua::PlayStatistics& play) noexcept
{
    return play.format == ua::StreamFormat{} && play.accept_frame_index == ua::FrameCount{} && play.file_frames == ua::FrameCount{} && play.first_frame_index == ua::FrameCount{} &&
           play.first_frame_sof == ua::UsbFrameCount{} && play.last_frame_index == ua::FrameCount{} && play.crc32_left == 0U && play.crc32_right == 0U &&
           play.missed_intervals == ua::IntervalCount{} && play.unprimed_intervals == ua::IntervalCount{} && play.first_miss_frame_index == ua::FrameCount{} &&
           play.pending_frames == ua::FrameCount{} && !play.armed && !play.interrupted && !play.stalled;
}

// A file longer than any StreamingStream test plays.
constexpr std::uint32_t long_file_frames = 1000U * ua::chunk_frame_count;

// An open stream past its primer and its first data packet (the stream's own
// baseline), so every later completion is judged for the stream. Its source is
// a file of `file_frames` frames, which the stream takes only while a play is
// armed, as the player's: silence until then and after the file. Each
// completion is one USB frame and one tick.
class StreamingStream
{
public:
    explicit StreamingStream(Checks& checks, std::uint32_t file_frames = long_file_frames) : file_{file_frames}, file_frames_{file_frames}
    {
        configure_and_open(checks, stream_, fifo_);
        on_time();
        on_time();
    }

    void on_time() { controller_.millisecond(stream_, fifo_, file_); }

    // A completion the ISR handles just after the next frame boundary.
    void late() { controller_.millisecond_read_late(stream_, fifo_, file_); }

    void skip(std::uint32_t frames) noexcept { controller_.skip(frames); }

    // The host does not poll; the service watchdog finds the packet waiting.
    void host_skips(std::uint32_t frames) noexcept { controller_.skip_watched(stream_, frames); }

    // The USB ISR is held off `late_by` frames past a completion.
    void device_late(std::uint32_t late_by, bool primed_before_poll) { controller_.millisecond_device_late(stream_, fifo_, file_, late_by, primed_before_poll); }

    // The USB ISR is held off past the watchdog's deadline but primes in time.
    void held_in_time() { controller_.millisecond_held_in_time(stream_, fifo_, file_); }

    // The service watchdog expires and finds the endpoint `transfer`.
    void watchdog(ua::InTransfer transfer) noexcept { stream_.on_watchdog(transfer); }

    void arm_play(Checks& checks) { accept_and_arm(checks, stream_, ua::TickCount{.value = controller_.tick()}); }

    void disarm() noexcept { stream_.disarm(); }

    // Alternate 0, then alternate 1 again: TinyUSB drops its FIFO and the
    // packet in flight and primes the endpoint afresh, whose zero-length
    // primer completes here.
    void reopen(Checks& checks)
    {
        stream_.close();
        fifo_.clear();
        controller_ = Controller{};
        checks.expect(stream_.open(fifo_, ua::pcm24_stereo).is_ok(), "the host reopens the stream");
        on_time();
    }

    // Completions on time until the play's file frames reach `confirmed`, or
    // the FIFO could have sent the whole file twice over.
    void run_until_confirmed(ua::FrameCount confirmed)
    {
        const std::uint32_t limit = 2U * (file_frames_ / ua::chunk_frames.value + ua::fifo_capacity_chunks);

        for (std::uint32_t tick = 0U; tick < limit && stream_.play().file_frames < confirmed; ++tick)
        {
            on_time();
        }
    }

    // Completions on time until the play has no file frame queued.
    void drain()
    {
        for (std::uint32_t tick = 0U; tick <= ua::fifo_capacity_chunks && stream_.play().pending_frames != ua::FrameCount{}; ++tick)
        {
            on_time();
        }
    }

    [[nodiscard]] ua::IntervalCount stream_missed() const noexcept { return stream_.counters().missed_intervals; }

    [[nodiscard]] ua::StreamCounters counters() const noexcept { return stream_.counters(); }

    [[nodiscard]] ua::FrameCount frames_sent() const noexcept { return stream_.counters().frames_sent; }

    [[nodiscard]] ua::PlayStatistics play() const noexcept { return stream_.play(); }

    [[nodiscard]] std::span<const ua::StereoFrame> supplied() const noexcept { return file_.supplied(); }

private:
    FakeFifo fifo_{ua::fifo_capacity_bytes.value};
    ua::UacStream stream_;
    FileSource file_;
    Controller controller_;
    std::uint32_t file_frames_{};
};

// The record ring refuses a push it cannot take and has no oldest record when
// empty, instead of dropping or inventing one.
void test_chunk_records(Checks& checks)
{
    ua::ChunkRecords records;
    checks.expect(records.empty() && !records.front().has_value(), "an empty ring has no oldest record");
    bool pushed = true;

    for (std::uint32_t index = 0U; index < ua::chunk_record_capacity; ++index)
    {
        pushed = records.push({.start = ua::FrameCount{.value = index * ua::chunk_frames.value}, .file_frames = ua::chunk_frames}).is_ok() && pushed;
    }

    constexpr ua::FrameCount refused_start{.value = 1U};
    const auto refused = records.push({.start = refused_start});
    checks.expect(pushed && records.full() && refused.code == ua::chunk_records_full.code, "a full ring refuses the next record");
    records.drop_newest();
    std::vector<std::uint32_t> starts;

    for (auto oldest = records.front(); oldest.has_value(); oldest = records.front())
    {
        starts.push_back(oldest->start.value);
        records.pop();
    }

    bool in_order = starts.size() == ua::chunk_record_capacity - 1U;

    for (std::size_t index = 0U; in_order && index < starts.size(); ++index)
    {
        in_order = starts[index] == index * ua::chunk_frames.value;
    }

    checks.expect(in_order && records.empty(), "the refused record left no trace, and dropping the newest kept the others in order");
    records.drop_newest();
    records.pop();
    checks.expect(records.empty() && !records.front().has_value(), "dropping from an empty ring leaves it empty");
}

// While armed, a completion more than the stall timeout after the previous one
// or the arming marks the play stalled, and the mark stays when the host polls
// again. Exactly the timeout is not a stall; an unarmed stream never stalls.
void test_stall_flag(Checks& checks)
{
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    configure_and_open(checks, stream, fifo);
    controller.millisecond(stream, fifo, silence);
    // After a completion at tick t, skip(k) puts the next one at t + 1 + k.
    controller.skip(stall_timeout.value);
    controller.millisecond(stream, fifo, silence);
    checks.expect(!stream.play().stalled, "a polling gap while no play is armed is not a stall");
    accept_and_arm(checks, stream, ua::TickCount{.value = controller.tick()});
    controller.skip(stall_timeout.value);
    controller.millisecond(stream, fifo, silence);
    checks.expect(!stream.play().stalled, "a first completion exactly the stall timeout after the arming is not a stall");
    controller.skip(stall_timeout.value - 1U);
    controller.millisecond(stream, fifo, silence);
    checks.expect(!stream.play().stalled, "nor is a completion exactly the stall timeout after the previous one");
    controller.skip(stall_timeout.value);
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.play().stalled, "a completion one tick past the stall timeout marks the armed play stalled");

    for (std::uint32_t tick = 0U; tick < ua::fifo_capacity_chunks; ++tick)
    {
        controller.millisecond(stream, fifo, silence);
    }

    checks.expect(stream.play().stalled && stream.play().armed, "the mark stays once the host polls again");
    checks.expect(stream.accept_play().is_ok() && !stream.play().stalled && !stream.play().armed, "the next PLAY clears it");
    // The host pauses before the next play is armed: the gap since the last
    // completion is long, but the clock restarts at the arming.
    controller.skip(2U * stall_timeout.value);
    checks.expect(stream.arm(ua::TickCount{.value = controller.tick()}, stall_timeout).is_ok(), "the next play arms");
    controller.millisecond(stream, fifo, silence);
    checks.expect(!stream.play().stalled, "the arming restarts the stall clock");
}

void test_prefill(Checks& checks)
{
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    configure_and_open(checks, stream, fifo);
    const auto counters = stream.counters();
    checks.expect(fifo.queued() == pcm24_target && fifo.writes() == ua::fifo_target_chunks, "opening pre-fills exactly two packets");
    checks.expect(all_zero(fifo.contents()), "the pre-fill is exact digital silence");
    checks.expect(counters.state == ua::StreamState::priming && counters.opens == 1U && counters.frames_queued.value == ua::fifo_target_chunks * ua::chunk_frames.value &&
                      counters.frames_sent.value == 0U && counters.source_frames.value == 0U,
                  "an open stream is priming with two chunks queued and nothing sent");
}

void test_streaming(Checks& checks)
{
    constexpr std::uint32_t milliseconds = 50U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    configure_and_open(checks, stream, fifo);
    bool level_held = true;

    for (std::uint32_t tick = 0U; tick < milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, silence);
        level_held = level_held && fifo.queued() == pcm24_target;
    }

    const auto counters = stream.counters();
    checks.expect(level_held && fifo.high_water() == pcm24_target.value && fifo.refusals() == 0U, "every completion tops the FIFO back up to two packets, never more");
    // The first completion is the zero-length primer.
    checks.expect(counters.state == ua::StreamState::streaming && counters.short_packets == 0U && counters.frames_sent.value == (milliseconds - 1U) * ua::chunk_frames.value,
                  "each full packet counts 48 frames and the primer is not a short packet");
    checks.expect(counters.frames_queued.value == (milliseconds + ua::fifo_target_chunks) * ua::chunk_frames.value && counters.source_frames.value == 0U && counters.rejected_chunks == 0U,
                  "the pre-fill plus one chunk per completion is queued, and silence supplies no file frames");
    checks.expect(controller.sent().size() == std::size_t{milliseconds - 1U} * pcm24_packet.value && all_zero(controller.sent()), "an idle stream sends only zeros");
    checks.expect(counters.missed_intervals == ua::IntervalCount{}, "one completion per frame, across the frame-number wrap, misses no interval");
}

// The primer can complete before the alternate-1 callback runs: TinyUSB then
// reports it to a closed stream and queues another zero-length packet.
void test_primer_before_open(Checks& checks)
{
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    controller.millisecond(stream, fifo, silence);
    checks.expect(fifo.writes() == 0U && stream.counters().opens == 0U, "a closed stream ignores a completion");
    configure_and_open(checks, stream, fifo);
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.counters().state == ua::StreamState::priming && stream.counters().short_packets == 0U, "a second primer keeps the stream priming without a short packet");
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.counters().state == ua::StreamState::streaming && stream.counters().short_packets == 0U && stream.counters().frames_sent == ua::chunk_frames,
                  "the first data packet starts streaming");
}

// The first data packet after the zero-length primer is reported as 0 bytes
// (#45). What the completion confirms tells a primer from data, not the size
// reported: this one confirms the chunk TinyUSB loaded, so it starts
// streaming, counts that chunk sent, and is a short packet and a mismatch.
// The stream then keeps pace with the host.
void test_first_data_completion_reported_short(Checks& checks)
{
    constexpr std::uint32_t milliseconds = 10U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    configure_and_open(checks, stream, fifo);
    controller.millisecond(stream, fifo, silence);
    const auto primed = stream.counters();
    checks.expect(primed.state == ua::StreamState::priming && primed.frames_sent == ua::FrameCount{} && primed.sent_mismatches == 0U,
                  "the primer completes: still priming, nothing sent and no mismatch");
    controller.millisecond_reporting(stream, fifo, silence, ua::ByteCount{});
    const auto first = stream.counters();
    checks.expect(first.state == ua::StreamState::streaming && first.frames_sent == ua::chunk_frames, "the first data packet reported as 0 bytes starts streaming and counts its chunk sent");
    checks.expect(first.short_packets == 1U && first.sent_mismatches == 1U, "it is a short packet and a mismatch");

    for (std::uint32_t tick = 0U; tick < milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, silence);
    }

    const auto after = stream.counters();
    checks.expect(after.frames_sent.value == controller.sent().size() / pcm24_frame.value && fifo.queued() == pcm24_target && after.short_packets == first.short_packets && after.rejected_chunks == 0U,
                  "the stream then keeps pace with the host, the FIFO at its target");
}

// sent_mismatches is 16 bits, in the padding after the format: it counts to
// 65535 and stays there. A mismatch at that maximum still leaves a play with
// file frames queued unconfirmed. The controller here loads each packet and
// reports it as 0 bytes, one per millisecond, and the host's bytes are not
// kept.
void test_sent_mismatches_saturate(Checks& checks)
{
    constexpr std::uint32_t most = std::numeric_limits<std::uint16_t>::max();
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    FileSource file{long_file_frames};
    std::uint32_t millisecond = 0U;
    std::size_t loaded_bytes = 0U;
    const auto report_zero = [&stream, &fifo, &millisecond, &loaded_bytes]<ua::FrameSource Source>(Source& source, std::uint32_t completions)
    {
        for (std::uint32_t completion = 0U; completion < completions; ++completion)
        {
            loaded_bytes += fifo.load_packet().size();
            stream.on_tx_done(fifo, source, ua::Completion{.sent = ua::ByteCount{}, .frame = frame_number(millisecond), .tick = ua::TickCount{.value = millisecond}});
            ++millisecond;
        }
    };

    configure_and_open(checks, stream, fifo);
    // The primer loaded nothing, so its 0 bytes are no mismatch.
    report_zero(silence, 1U);
    report_zero(silence, most - 1U);
    // The chunk loaded last is still in flight.
    checks.expect(stream.counters().sent_mismatches == most - 1U && stream.counters().frames_sent.value + ua::chunk_frames.value == loaded_bytes / pcm24_frame.value,
                  "each completion reported short of its chunk counts, up to 65534, and still confirms the chunk");
    report_zero(silence, 1U);
    checks.expect(stream.counters().sent_mismatches == most, "the next one reaches 65535");
    report_zero(silence, 1U);
    checks.expect(stream.counters().sent_mismatches == most && !stream.play_mismatched(), "one more stays at 65535, without wrapping to 0");
    accept_and_arm(checks, stream, ua::TickCount{.value = millisecond});
    report_zero(file, 1U);
    checks.expect(stream.play().pending_frames != ua::FrameCount{} && !stream.play_mismatched(), "an armed play has file frames queued");
    report_zero(file, 1U);
    checks.expect(stream.counters().sent_mismatches == most && stream.play_mismatched(), "a mismatch at the maximum still leaves the play's delivery unconfirmed");
}

void test_underrun(Checks& checks)
{
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    configure_and_open(checks, stream, fifo);
    controller.millisecond(stream, fifo, silence);
    controller.millisecond(stream, fifo, silence);
    fifo.refuse_writes();
    const auto before = stream.counters();

    for (std::uint32_t tick = 0U; tick < ua::fifo_target_chunks + 2U; ++tick)
    {
        controller.millisecond(stream, fifo, silence);
    }

    const auto after = stream.counters();
    checks.expect(after.rejected_chunks == ua::fifo_target_chunks + 2U, "each refused chunk is counted once");
    checks.expect(after.short_packets == before.short_packets + 1U && fifo.queued().value == 0U, "a drained FIFO sends a zero-length packet, counted as short");
    checks.expect(after.missed_intervals == ua::IntervalCount{}, "a zero-length packet still answers its interval");
}

void test_close_and_reopen(Checks& checks)
{
    constexpr std::uint32_t file_frames_per_chunk = 10U;
    constexpr ua::FrameCount prefill_frames{.value = ua::fifo_target_chunks * ua::chunk_frames.value};
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    RampSource ramp{file_frames_per_chunk};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < ua::fifo_target_chunks + 1U; ++tick)
    {
        controller.millisecond(stream, fifo, ramp);
    }

    // Drain the FIFO until one zero-length packet completes.
    fifo.refuse_writes();

    for (std::uint32_t tick = 0U; tick < ua::fifo_target_chunks + 2U; ++tick)
    {
        controller.millisecond(stream, fifo, ramp);
    }

    const auto streamed = stream.counters();
    stream.close();
    fifo.clear();
    controller.millisecond(stream, fifo, ramp);
    const auto closed = stream.counters();
    checks.expect(closed.state == ua::StreamState::closed && fifo.writes() == 0U && closed.frames_sent == streamed.frames_sent && closed.short_packets == streamed.short_packets,
                  "a closed stream writes nothing and keeps its counters readable");
    checks.expect(!stream.play().armed && stream.play().interrupted && stream.play().pending_frames == ua::FrameCount{}, "closing interrupts the armed play and drops its queued chunks");
    checks.expect(stream.open(fifo, ua::pcm24_stereo).is_ok(), "alternate 0 keeps the configuration, so alternate 1 reopens");
    const auto reopened = stream.counters();
    checks.expect(reopened.opens == 2U && reopened.frames_sent.value == 0U && reopened.state == ua::StreamState::priming && reopened.short_packets == streamed.short_packets &&
                      streamed.short_packets != 0U,
                  "reopening restarts the frame index and keeps the short-packet history");
    checks.expect(streamed.source_frames.value != 0U && streamed.frames_queued > prefill_frames && reopened.source_frames.value == 0U && reopened.frames_queued == prefill_frames,
                  "reopening restarts the queued and file-frame totals at the silent pre-fill");
    checks.expect(all_zero(fifo.contents()), "a reopened stream is not armed: its pre-fill is silence");
}

// TinyUSB resets its audio driver on bus reset, unplug and SET_CONFIGURATION(0)
// without an interface callback, and a SET_INTERFACE queued before a reset can
// still reach the callbacks: the stream opens only inside a configuration.
void test_configuration(Checks& checks)
{
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    const auto unconfigured = stream.open(fifo, ua::pcm24_stereo);
    checks.expect(unconfigured.code == ua::stream_not_configured.code && fifo.writes() == 0U && stream.counters().opens == 0U && stream.counters().state == ua::StreamState::closed,
                  "alternate 1 before SET_CONFIGURATION is refused and changes nothing");
    configure_and_open(checks, stream, fifo);
    controller.millisecond(stream, fifo, silence);
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.counters().state == ua::StreamState::streaming, "a configured stream streams");
    accept_and_arm(checks, stream, {});
    stream.deconfigure();
    const auto writes = fifo.writes();
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.counters().state == ua::StreamState::closed && fifo.writes() == writes, "a bus reset, unplug or SET_CONFIGURATION(0) closes the stream and stops the producer");
    checks.expect(!stream.play().armed && stream.play().interrupted, "a bus reset, unplug or SET_CONFIGURATION(0) interrupts an armed play");
    const auto stale = stream.open(fifo, ua::pcm24_stereo);
    checks.expect(stale.code == ua::stream_not_configured.code && stream.counters().state == ua::StreamState::closed && stream.counters().opens == 1U && fifo.writes() == writes,
                  "a SET_INTERFACE queued before the reset cannot reopen the stream");
    // TinyUSB's reset empties its FIFO.
    fifo.clear();
    configure_and_open(checks, stream, fifo);
    checks.expect(stream.counters().state == ua::StreamState::priming && stream.counters().opens == 2U, "alternate 1 opens again after the next SET_CONFIGURATION");
    stream.configure();
    checks.expect(stream.counters().state == ua::StreamState::closed, "a new configuration starts with the stream closed");
}

void test_never_overfill(Checks& checks)
{
    constexpr std::uint32_t odd_level = 700U;
    constexpr std::uint32_t small_capacity = 500U;
    constexpr std::uint32_t small_level = 200U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    fifo.preload(odd_level);
    configure_and_open(checks, stream, fifo);
    checks.expect(fifo.writes() == 0U && fifo.queued().value == odd_level, "no chunk is added once one would pass the target");
    // Below the target, but with less free space than a chunk.
    FakeFifo small{small_capacity};
    small.preload(small_level);
    configure_and_open(checks, stream, small);
    checks.expect(small.writes() == 0U && small.refusals() == 0U, "no chunk is written without room for all of it");
    LyingFifo liar;
    ua::Silence silence;
    configure_and_open(checks, stream, liar);
    stream.on_tx_done(liar, silence, ua::Completion{.sent = pcm24_packet});
    checks.expect(liar.writes == 2U * ua::fifo_target_chunks, "each top-up writes at most two chunks");
    // A FIFO that claims to be empty looks drained after every load, so each
    // completion confirms a chunk; the chunks outstanding never pass the
    // record ring.
    constexpr std::uint32_t max_outstanding = ua::chunk_record_capacity * ua::chunk_frames.value;
    bool bounded = true;

    for (std::uint32_t completion = 0U; completion < 2U * ua::chunk_record_capacity; ++completion)
    {
        const std::uint32_t writes = liar.writes;
        stream.on_tx_done(liar, silence, ua::Completion{});
        const auto counters = stream.counters();
        bounded = bounded && liar.writes - writes <= ua::fifo_target_chunks && counters.frames_queued.value - counters.frames_sent.value <= max_outstanding;
    }

    checks.expect(bounded, "a FIFO that misreports its level gets at most two chunks a completion, within the record ring");
    // A FIFO the controller never loads from keeps its level: nothing is
    // confirmed, and no chunk is written past the target.
    FakeFifo stuck{ua::fifo_capacity_bytes.value};
    configure_and_open(checks, stream, stuck);

    for (std::uint32_t completion = 0U; completion < ua::chunk_record_capacity; ++completion)
    {
        stream.on_tx_done(stuck, silence, ua::Completion{});
    }

    checks.expect(stuck.writes() == ua::fifo_target_chunks && stream.counters().frames_sent == ua::FrameCount{}, "a FIFO that never drains gets no chunk past its target, and none counts as sent");
}

void test_source(Checks& checks)
{
    constexpr std::uint32_t file_frames_per_chunk = 10U;
    constexpr std::uint32_t milliseconds = 6U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    RampSource ramp{file_frames_per_chunk};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, ramp);
    }

    const auto& sent = controller.sent();
    const auto prefill = ua::fifo_target_chunks * pcm24_packet.value;
    const std::span<const std::byte> file{std::span{sent}.subspan(prefill)};
    checks.expect(all_zero(std::span{sent}.first(prefill)), "the pre-fill precedes the first source chunk");
    checks.expect(subslot(file, 0U) == RampSource::first_sample() * ua::subslot_scale && subslot(file, 1U) == subslot(file, 0U), "a source frame is sent as sample << 8, left then right");
    bool stereo = true;

    for (std::size_t frame = 0U; frame < file.size() / pcm24_frame.value; ++frame)
    {
        stereo = stereo && subslot(file, 2U * frame) == subslot(file, 2U * frame + 1U);
    }

    checks.expect(stereo, "left equals right in every frame");
    checks.expect(stream.counters().source_frames.value == milliseconds * file_frames_per_chunk, "the frames a source reports as file audio are accumulated");
}

// Silence until the play is armed, the file, then silence: the statistics
// cover exactly the sent file frames and the wire carries them bit for bit.
void test_play_statistics(Checks& checks)
{
    constexpr std::uint32_t total = 2U * ua::chunk_frame_count + 34U;
    constexpr std::uint32_t idle_milliseconds = 3U;
    constexpr std::uint32_t play_milliseconds = 10U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);

    for (std::uint32_t tick = 0U; tick < idle_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    checks.expect(file.calls() == 0U && all_zero(controller.sent()), "an open stream that is not armed never asks the source");
    const auto accepted_at = stream.counters().frames_sent;
    accept_and_arm(checks, stream, ua::TickCount{.value = controller.tick()});
    checks.expect(stream.play().armed && stream.play().pending_frames == ua::FrameCount{} && stream.play().progress_tick.value == controller.tick(),
                  "arming starts the stall clock with nothing pending");

    for (std::uint32_t tick = 0U; tick < play_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    const auto play = stream.play();
    const auto sent = std::span{controller.sent()};
    const std::uint32_t first = play.first_frame_index.value;
    const std::span<const ua::StereoFrame> supplied{file.supplied()};
    checks.expect(play.file_frames.value == total && play.pending_frames == ua::FrameCount{} && supplied.size() == total, "every file frame is confirmed sent and none is pending");
    checks.expect(play.accept_frame_index == accepted_at && first >= accepted_at.value && first % ua::chunk_frame_count == 0U, "the first file frame leads a chunk queued after PLAY was accepted");
    checks.expect(play.last_frame_index.value == first + total - 1U, "the last index closes the play's span");
    bool exact = sent.size() >= (std::size_t{first} + total) * pcm24_frame.value;

    for (std::size_t frame = 0U; exact && frame < sent.size() / pcm24_frame.value; ++frame)
    {
        const auto on_wire = wire_frame(sent, frame);
        const bool in_file = frame >= first && frame < first + total;
        const auto expected = in_file ? supplied[frame - first] : ua::StereoFrame{};
        exact = on_wire.left == expected.left && on_wire.right == expected.right;
    }

    checks.expect(exact, "zeros, then each file frame bit for bit, then zeros");
    checks.expect(play.crc32_left == channel_crc(supplied, &ua::StereoFrame::left) && play.crc32_right == channel_crc(supplied, &ua::StereoFrame::right) && play.crc32_left != play.crc32_right,
                  "each channel's CRC is zlib's over its samples as 3-byte little-endian PCM24");
    checks.expect(play.first_frame_sof.value == frame_that_sent(controller, first) && play.first_frame_sof.value >= ua::usb_frame_number_modulus,
                  "the first file frame's USB frame is extended across the 11-bit wrap");
}

// A play armed with a scheduled start (the PLAYLIST sequencer's device-time
// start): the stream after `idle_milliseconds`, PLAY accepted, then armed for
// the stream index `lead` frames past the next chunk to fill (none: at once;
// `behind`: that far before it instead), planned on this stream or on the
// one before it.
struct ScheduledCase
{
    std::string_view name;
    std::optional<std::uint32_t> lead{};
    std::uint32_t behind{};
    bool earlier_stream{};
    std::uint32_t expected_offset{}; // the first file frame's index past the next chunk at the arming
};

// Every scheduled case plays a file of three chunks.
constexpr std::uint32_t scheduled_file_frames = 3U * ua::chunk_frames.value;

struct ScheduledRun
{
    ua::PlayStatistics play{};
    ua::FrameCount queued_at_arm{};
    std::vector<std::byte> sent;
    std::vector<ua::StereoFrame> supplied;
};

[[nodiscard]] ScheduledRun run_scheduled(Checks& checks, const ScheduledCase& item)
{
    constexpr std::uint32_t total = scheduled_file_frames;
    constexpr std::uint32_t idle_milliseconds = 3U;
    // The longest case waits 150 chunks, past the stall timeout.
    constexpr std::uint32_t most_milliseconds = 200U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);

    for (std::uint32_t tick = 0U; tick < idle_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    checks.expect(stream.accept_play().is_ok(), "PLAY is accepted on an open stream");
    const auto counters = stream.counters();
    std::optional<ua::ScheduledStart> start{};

    if (item.lead.has_value() || item.behind != 0U)
    {
        const std::uint32_t index = counters.frames_queued.value + item.lead.value_or(0U) - item.behind;
        start = ua::ScheduledStart{.index = ua::FrameCount{.value = index}, .stream_opens = counters.opens - (item.earlier_stream ? 1U : 0U)};
    }

    checks.expect(stream.arm(ua::TickCount{.value = controller.tick()}, stall_timeout, start).is_ok(), "the accepted play arms for its start");

    for (std::uint32_t tick = 0U; tick < most_milliseconds && stream.play().file_frames.value < total; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    return {.play = stream.play(), .queued_at_arm = counters.frames_queued, .sent = controller.sent(), .supplied = file.supplied()};
}

void test_scheduled_start(Checks& checks)
{
    constexpr std::uint32_t chunk = ua::chunk_frames.value;
    constexpr std::uint32_t partial = 5U;
    constexpr std::uint32_t past_stall_chunks = 150U;
    const auto cases = std::to_array<ScheduledCase>({
        {.name = "no start", .expected_offset = 0U},
        {.name = "a start 10 chunks ahead", .lead = 10U * chunk, .expected_offset = 10U * chunk},
        {.name = "a start at the next chunk", .lead = 0U, .expected_offset = 0U},
        {.name = "a start inside a chunk takes the next one", .lead = 10U * chunk + partial, .expected_offset = 11U * chunk},
        {.name = "a start the stream has passed", .behind = 2U * chunk, .expected_offset = 0U},
        {.name = "a start planned on an earlier stream", .lead = 10U * chunk, .earlier_stream = true, .expected_offset = 0U},
        {.name = "a start past the stall timeout", .lead = past_stall_chunks * chunk, .expected_offset = past_stall_chunks * chunk},
    });

    for (const auto& item : cases)
    {
        const std::string name{item.name};
        const auto run = run_scheduled(checks, item);
        const std::uint32_t first = run.play.first_frame_index.value;
        const auto sent = std::span{run.sent};
        bool exact = run.supplied.size() == scheduled_file_frames && sent.size() >= (std::size_t{first} + run.supplied.size()) * pcm24_frame.value;

        for (std::size_t frame = 0U; exact && frame < sent.size() / pcm24_frame.value; ++frame)
        {
            const auto on_wire = wire_frame(sent, frame);
            const bool in_file = frame >= first && frame - first < run.supplied.size();
            const auto expected = in_file ? run.supplied[frame - first] : ua::StereoFrame{};
            exact = on_wire.left == expected.left && on_wire.right == expected.right;
        }

        checks.expect((first == run.queued_at_arm.value + item.expected_offset), (name + ": the first file frame goes out where the start puts it").c_str());
        checks.expect(exact && run.play.file_frames.value == scheduled_file_frames, (name + ": exact silence before it, the file, then silence").c_str());
        checks.expect(!run.play.stalled && run.play.missed_intervals == ua::IntervalCount{}, (name + ": the silent wait neither stalls nor misses").c_str());
    }

    // A new PLAY forgets a start still to come.
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{chunk};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    controller.millisecond(stream, fifo, file);
    checks.expect(stream.accept_play().is_ok(), "PLAY is accepted on an open stream");
    const auto counters = stream.counters();
    const ua::ScheduledStart far{.index = ua::FrameCount{.value = counters.frames_queued.value + past_stall_chunks * chunk}, .stream_opens = counters.opens};
    checks.expect(stream.arm({}, stall_timeout, far).is_ok() && stream.accept_play().is_ok(), "a play armed for a far start, then PLAY again");
    const auto queued = stream.counters().frames_queued;
    checks.expect(stream.arm({}, stall_timeout).is_ok(), "the new play arms without a start");

    for (std::uint32_t tick = 0U; tick < ua::fifo_capacity_chunks + 2U; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    checks.expect(stream.play().file_frames.value == chunk && stream.play().first_frame_index == queued, "the new play starts at once: PLAY forgot the start");
}

// STOP: the player disarms; chunks already queued still go out and count, and
// the statistics stay the sent prefix of what the source supplied.
void test_disarm_keeps_a_consistent_prefix(Checks& checks)
{
    constexpr std::uint32_t total = 100U * ua::chunk_frame_count;
    constexpr std::uint32_t armed_milliseconds = 5U;
    constexpr std::uint32_t after_milliseconds = 6U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    stream.disarm();
    const auto at_disarm = stream.play();
    const auto calls = file.calls();

    for (std::uint32_t tick = 0U; tick < after_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    const auto play = stream.play();
    const std::span<const ua::StereoFrame> supplied{file.supplied()};
    checks.expect(at_disarm.pending_frames != ua::FrameCount{} && play.pending_frames == ua::FrameCount{} && file.calls() == calls,
                  "after disarming the source is not asked again and the queued file chunks drain");
    checks.expect(play.file_frames.value == supplied.size() && play.file_frames > at_disarm.file_frames, "the chunks queued before the disarm count once sent");
    checks.expect(play.crc32_left == channel_crc(supplied, &ua::StereoFrame::left) && play.crc32_right == channel_crc(supplied, &ua::StereoFrame::right) &&
                      play.last_frame_index.value == play.first_frame_index.value + play.file_frames.value - 1U,
                  "the statistics describe the sent prefix exactly");
}

// The FIFO refuses a chunk of an armed play: the chunk and its record are
// dropped together, so its file frames are lost, nothing is left pending, and
// the statistics describe exactly the file frames on the wire.
void test_refused_chunk_is_lost(Checks& checks)
{
    constexpr std::uint32_t total = 20U * ua::chunk_frame_count;
    constexpr std::uint32_t before_refusal_milliseconds = 3U;
    constexpr std::uint32_t after_refusal_milliseconds = 30U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < before_refusal_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    const auto rejected = stream.counters().rejected_chunks;
    fifo.refuse_writes();
    controller.millisecond(stream, fifo, file);
    fifo.accept_writes();

    for (std::uint32_t tick = 0U; tick < after_refusal_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    const auto play = stream.play();
    const auto sent = std::span{controller.sent()};
    std::vector<ua::StereoFrame> on_wire;

    for (std::size_t frame = play.first_frame_index.value; frame < std::size_t{play.first_frame_index.value} + play.file_frames.value; ++frame)
    {
        on_wire.push_back(frame < sent.size() / pcm24_frame.value ? wire_frame(sent, frame) : ua::StereoFrame{});
    }

    checks.expect(stream.counters().rejected_chunks == rejected + 1U && play.file_frames.value == total - ua::chunk_frames.value && play.pending_frames == ua::FrameCount{},
                  "a refused chunk's file frames are lost and leave nothing pending");
    checks.expect(play.crc32_left == channel_crc(on_wire, &ua::StereoFrame::left) && play.crc32_right == channel_crc(on_wire, &ua::StereoFrame::right) &&
                      play.last_frame_index.value == play.first_frame_index.value + play.file_frames.value - 1U,
                  "the statistics describe exactly the file frames on the wire");
}

// A completion that reports fewer bytes than the packet TinyUSB loaded for it
// (0 after a ChipIdea transfer error, #45) still confirms that packet: TinyUSB
// took it from the FIFO, so its chunk record retires and leaves no ghost.
// Twenty of them used to fill the 8-record ring, empty the FIFO and freeze
// frames_sent, with pending_frames stuck and only zero-length packets going
// out. Each is a short packet and a mismatch, and the play's statistics
// describe every file frame TinyUSB took.
void test_short_completion_leaves_no_ghost(Checks& checks)
{
    constexpr std::uint32_t total = 100U * ua::chunk_frame_count;
    constexpr std::uint32_t armed_milliseconds = 5U;
    constexpr std::uint32_t short_completions = 20U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    const auto before = stream.counters();

    for (std::uint32_t completion = 0U; completion < short_completions; ++completion)
    {
        controller.millisecond_reporting(stream, fifo, file, ua::ByteCount{});
        controller.millisecond(stream, fifo, file);
    }

    const auto after = stream.counters();
    const std::size_t received_frames = controller.sent().size() / pcm24_frame.value;
    checks.expect(after.frames_sent.value == received_frames && fifo.queued() == pcm24_target && after.rejected_chunks == before.rejected_chunks,
                  "completions reported short still confirm the packets TinyUSB loaded: the stream keeps pace and the FIFO its target");
    checks.expect(after.short_packets == before.short_packets + short_completions && after.sent_mismatches == before.sent_mismatches + short_completions,
                  "each one reported short is a short packet and a mismatch");
    stream.disarm();

    for (std::uint32_t tick = 0U; tick <= ua::chunk_record_capacity && stream.play().pending_frames != ua::FrameCount{}; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    const auto play = stream.play();
    const std::span<const ua::StereoFrame> supplied{file.supplied()};
    checks.expect(play.pending_frames == ua::FrameCount{} && play.file_frames.value == supplied.size() && play.crc32_left == channel_crc(supplied, &ua::StereoFrame::left) &&
                      play.crc32_right == channel_crc(supplied, &ua::StereoFrame::right),
                  "once disarmed nothing is left pending, and the statistics describe every file frame the host received");
}

// The completions on time that play `stream`'s armed play from `file` out
// to its last file frame confirmed, or as many as could send the whole
// file twice over.
template <ua::FrameSource Source> void play_out(ua::UacStream& stream, FakeFifo& fifo, Source& file, Controller& controller, std::uint32_t file_frames)
{
    const std::uint32_t limit = 2U * (file_frames / ua::chunk_frames.value + ua::fifo_capacity_chunks);

    for (std::uint32_t tick = 0U; tick < limit && (stream.play().file_frames.value < file_frames || stream.play().pending_frames != ua::FrameCount{}); ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }
}

// A completion reported short of its packet while the play has file frames
// queued leaves the play's delivery unconfirmed (play_mismatched()): the host
// may not have received that packet, though its frames still count as sent.
// One before the play's first file frame is queued, or after its last is
// confirmed, is the stream's alone (sent_mismatches). Each accept_play() and
// clear_play() starts a play with its delivery confirmed.
void test_short_completion_marks_the_play(Checks& checks)
{
    constexpr std::uint32_t total = 10U * ua::chunk_frame_count;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource first{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    controller.millisecond(stream, fifo, first);
    controller.millisecond(stream, fifo, first);
    checks.expect(stream.accept_play().is_ok(), "PLAY is accepted");
    const auto before = stream.counters();
    controller.millisecond_reporting(stream, fifo, first, ua::ByteCount{});
    checks.expect(stream.counters().sent_mismatches == before.sent_mismatches + 1U && !stream.play_mismatched(), "a short report before the play's first file frame is queued is the stream's alone");
    checks.expect(stream.arm(ua::TickCount{.value = controller.tick()}, stall_timeout).is_ok(), "the play arms");
    play_out(stream, fifo, first, controller, total);
    controller.millisecond_reporting(stream, fifo, first, ua::ByteCount{});
    checks.expect(stream.play().file_frames.value == total && stream.counters().sent_mismatches == before.sent_mismatches + 2U && !stream.play_mismatched(),
                  "and so is one after the play's last file frame is confirmed");
    FileSource second{total};
    accept_and_arm(checks, stream, ua::TickCount{.value = controller.tick()});
    controller.millisecond(stream, fifo, second);
    checks.expect(stream.play().pending_frames != ua::FrameCount{} && !stream.play_mismatched(), "the next play has file frames queued");
    controller.millisecond_reporting(stream, fifo, second, ua::ByteCount{});
    checks.expect(stream.play_mismatched() && stream.counters().sent_mismatches == before.sent_mismatches + 3U, "a short report while the play has file frames queued leaves its delivery unconfirmed");
    play_out(stream, fifo, second, controller, total);
    const std::span<const ua::StereoFrame> supplied{second.supplied()};
    checks.expect(stream.play_mismatched() && stream.play().file_frames.value == total && stream.play().crc32_left == channel_crc(supplied, &ua::StereoFrame::left),
                  "the play's frames still count as sent, the reported packet's included, and it stays unconfirmed to its end");
    controller.millisecond_reporting(stream, fifo, second, ua::ByteCount{});
    checks.expect(stream.play_mismatched() && stream.play().pending_frames == ua::FrameCount{} && stream.counters().sent_mismatches == before.sent_mismatches + 4U,
                  "a later short report, after the play's last file frame is confirmed, leaves the mark: it never confirms the play again");
    stream.disarm();
    checks.expect(stream.accept_play().is_ok() && !stream.play_mismatched(), "accepting the next play confirms its delivery afresh");
    FileSource third{total};
    checks.expect(stream.arm(ua::TickCount{.value = controller.tick()}, stall_timeout).is_ok(), "the third play arms");
    controller.millisecond(stream, fifo, third);
    controller.millisecond_reporting(stream, fifo, third, ua::ByteCount{});
    checks.expect(stream.play_mismatched(), "a short report marks the third play");
    stream.clear_play();
    checks.expect(!stream.play_mismatched(), "and a play on the codec route clears it");
}

// A close part way: the chunks still queued never go out, so the statistics
// stop at the last confirmed chunk.
void test_close_mid_play(Checks& checks)
{
    constexpr std::uint32_t total = 100U * ua::chunk_frame_count;
    constexpr std::uint32_t armed_milliseconds = 6U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    const auto before = stream.play();
    stream.close();
    const auto play = stream.play();
    const auto confirmed = std::span{file.supplied()}.first(play.file_frames.value);
    checks.expect(before.pending_frames != ua::FrameCount{} && play.interrupted && !play.armed && play.pending_frames == ua::FrameCount{},
                  "closing drops the queued file chunks and interrupts the play");
    checks.expect(play.file_frames == before.file_frames && play.file_frames.value < file.supplied().size() && play.crc32_left == channel_crc(confirmed, &ua::StereoFrame::left) &&
                      play.crc32_right == channel_crc(confirmed, &ua::StereoFrame::right),
                  "the statistics stay those of the confirmed prefix");
    // TinyUSB empties its FIFO and primes the reopened endpoint afresh; the
    // next play on the reopened stream counts from its own chunks only.
    constexpr std::uint32_t next_total = 2U * ua::chunk_frames.value;
    fifo.clear();
    Controller reopened;
    FileSource next{next_total};
    checks.expect(stream.open(fifo, ua::pcm24_stereo).is_ok(), "the host reopens the stream");
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
    {
        reopened.millisecond(stream, fifo, next);
    }

    const auto again = stream.play();
    checks.expect(again.file_frames.value == next_total && again.pending_frames == ua::FrameCount{} && again.crc32_left == channel_crc(next.supplied(), &ua::StereoFrame::left),
                  "a play on the reopened stream is counted from its own chunks");
}

void test_accept_and_arm(Checks& checks)
{
    constexpr std::uint32_t file_chunks = 10U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{file_chunks * ua::chunk_frames.value};
    Controller controller;
    const auto closed = stream.accept_play();
    checks.expect(closed.code == ua::stream_not_open.code && std::string_view{closed.message} == "NO_STREAM", "PLAY on a closed stream is NO_STREAM");
    configure_and_open(checks, stream, fifo);
    checks.expect(stream.accept_play().is_ok(), "an open stream accepts PLAY");
    stream.close();
    // TinyUSB empties its FIFO when the alternate closes.
    fifo.clear();
    checks.expect(stream.open(fifo, ua::pcm24_stereo).is_ok(), "the host reopens the stream");
    const auto reopened = stream.arm({}, stall_timeout);
    checks.expect(reopened.code == ua::stream_interrupted.code && !stream.play().armed && stream.play().interrupted, "a stream that closed and reopened after PLAY was accepted cannot be armed");
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < 3U; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    stream.disarm();
    const auto old_pending = stream.play().pending_frames;
    checks.expect(stream.accept_play().is_ok() && stream.play().file_frames == ua::FrameCount{} && stream.play().pending_frames == ua::FrameCount{}, "a new PLAY restarts the statistics");
    ua::Silence silence;

    for (std::uint32_t tick = 0U; tick < 4U; ++tick)
    {
        controller.millisecond(stream, fifo, silence);
    }

    checks.expect(old_pending != ua::FrameCount{} && stream.play().file_frames == ua::FrameCount{} && stream.play().crc32_left == 0U, "chunks queued for the last play never count for the new one");
}

// One completion per frame; gaps count missed intervals, a late read does not.
void test_missed_intervals(Checks& checks)
{
    constexpr std::uint32_t skipped = 3U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    configure_and_open(checks, stream, fifo);

    for (std::uint32_t tick = 0U; tick < 3U; ++tick)
    {
        controller.millisecond(stream, fifo, silence);
    }

    const auto before = stream.counters().missed_intervals.value;
    controller.skip(skipped);
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.counters().missed_intervals == intervals(before + skipped), "frames the host polled nothing in are missed intervals");
    controller.millisecond_read_late(stream, fifo, silence);
    checks.expect(stream.counters().missed_intervals == intervals(before + skipped + 1U), "a completion read one frame late looks like a gap at first");
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.counters().missed_intervals == intervals(before + skipped), "the next completion in the same frame takes the late read back");
    // A new stream whose first data completion is read late.
    stream.close();
    fifo.clear();
    Controller fresh;
    checks.expect(stream.open(fifo, ua::pcm24_stereo).is_ok(), "the stream reopens");
    fresh.millisecond(stream, fifo, silence);
    fresh.millisecond_read_late(stream, fifo, silence);

    for (std::uint32_t tick = 0U; tick < 3U; ++tick)
    {
        fresh.millisecond(stream, fifo, silence);
    }

    checks.expect(stream.counters().missed_intervals == intervals(before + skipped), "a late first read takes back nothing the stream never counted");
}

// Where the stream missed last and its largest gap: the first frame sent after
// the latest gap that still counts a miss, whether that gap delayed the play's
// file, and the largest gap since the stream opened. A late read is located
// until the next completion takes it back, which returns the location before
// it. Both restart with the stream; the missed count runs on.
void test_miss_location(Checks& checks)
{
    constexpr std::uint32_t idle_skipped = 3U;
    constexpr std::uint32_t armed_skipped = 2U;
    StreamingStream host{checks};
    host.on_time();
    const auto clean = host.counters();
    checks.expect(clean.last_miss.frame_index == ua::FrameCount{} && !clean.last_miss.play && clean.max_gap == ua::UsbFrameSpan{.value = 1U},
                  "a stream that missed nothing has no miss location, and a completion every frame is a gap of one");
    const ua::FrameCount idle_index = host.frames_sent();
    host.skip(idle_skipped);
    host.on_time();
    const auto idle = host.counters();
    checks.expect(idle.last_miss.frame_index == idle_index && idle_index != ua::FrameCount{} && !idle.last_miss.play && idle.max_gap.value == idle_skipped + 1U,
                  "an idle gap is located at the first frame sent after it, not the play's, and is the largest gap");
    host.arm_play(checks);
    host.on_time();
    const ua::FrameCount armed_index = host.frames_sent();
    host.skip(armed_skipped);
    host.on_time();
    const auto armed = host.counters();
    checks.expect(armed.last_miss.frame_index == armed_index && armed.last_miss.play && armed.max_gap == idle.max_gap,
                  "a later gap that delays the play's file is the latest miss, the play's; a shorter gap keeps the largest");
    const ua::FrameCount late_index = host.frames_sent();
    host.late();
    const auto looked_late = host.counters();
    host.on_time();
    const auto taken_back = host.counters();
    checks.expect(looked_late.last_miss.frame_index == late_index && looked_late.missed_intervals == intervals(idle_skipped + armed_skipped + 1U),
                  "a late read looks like a miss, located, until the next completion");
    checks.expect(taken_back.last_miss.frame_index == armed_index && taken_back.last_miss.play && taken_back.missed_intervals == intervals(idle_skipped + armed_skipped) &&
                      taken_back.max_gap == idle.max_gap,
                  "taking the late read back returns the location before it; the largest gap is as read");
    host.disarm();
    host.drain();
    host.reopen(checks);
    host.on_time();
    host.on_time();
    const auto reopened = host.counters();
    checks.expect(reopened.last_miss.frame_index == ua::FrameCount{} && !reopened.last_miss.play && reopened.max_gap == ua::UsbFrameSpan{.value = 1U} &&
                      reopened.missed_intervals == taken_back.missed_intervals,
                  "a reopened stream starts without a miss location and with its own largest gap; the missed count since boot stays");
}

// The location's play flag says whether the gap delayed the play's file, as
// the play's own count judges it, not whether the play was armed: a gap still
// going on when the play arms ends armed but delays none of its file, and a
// gap after a disarm delays the file chunks it left queued.
void test_miss_location_play(Checks& checks)
{
    constexpr std::uint32_t skipped = 4U;
    constexpr std::uint32_t armed_milliseconds = 3U;
    StreamingStream host{checks};
    host.skip(skipped);
    host.arm_play(checks);
    host.on_time();
    const auto spanning = host.counters();
    checks.expect(spanning.last_miss.frame_index != ua::FrameCount{} && !spanning.last_miss.play && host.play().armed && host.play().missed_intervals == ua::IntervalCount{},
                  "a gap still going on as the play arms ends armed, but is not the play's: it delays none of its file");

    for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
    {
        host.on_time();
    }

    host.disarm();
    const ua::FrameCount resume_index = host.frames_sent();
    host.skip(skipped);
    host.on_time();
    const auto leftover = host.counters();
    checks.expect(leftover.last_miss.frame_index == resume_index && leftover.last_miss.play && !host.play().armed && host.play().missed_intervals.value == skipped,
                  "a gap after the disarm that delays the queued file chunks is the play's, although no play is armed");
}

// A pause of 2048 frames or more wraps the 11-bit frame number; the RTOS ticks
// between the two completions tell the wraps, so the largest gap and the
// missed intervals are whole.
void test_long_gap(Checks& checks)
{
    constexpr std::uint32_t skipped = 2499U;
    StreamingStream host{checks};
    host.on_time();
    const auto before = host.counters();
    host.skip(skipped);
    host.on_time();
    const auto after = host.counters();
    checks.expect(after.max_gap.value == skipped + 1U && after.missed_intervals == intervals(before.missed_intervals.value + skipped),
                  "a pause of 2.5 s is 2500 frames and 2499 missed intervals, not its frame numbers' 452");
}

// The service watchdog tells the device's misses from the host's. Its samples
// that find the packet still primed prove polls the host left out; one that
// finds nothing primed (the ISR that handles a completion held off past its
// deadline) counts the gap's other missed intervals as unprimed. A gap with no
// sample of nothing primed is the host's, and so is an expiry in a gap that
// missed nothing. A late read taken back from an unprimed gap leaves the
// unprimed count too, so it never exceeds the missed count.
void test_unprimed_intervals(Checks& checks)
{
    constexpr std::uint32_t host_skipped = 4U;
    constexpr std::uint32_t late_by = 3U;
    StreamingStream host{checks};
    host.on_time();
    host.host_skips(host_skipped);
    host.on_time();
    const auto host_gap = host.counters();
    checks.expect(host_gap.missed_intervals == intervals(host_skipped) && host_gap.unprimed_intervals == ua::IntervalCount{} && host_gap.watchdog_expiries == host_skipped &&
                      host_gap.last_miss.unprimed == ua::IntervalCount{},
                  "a gap the host did not poll, sampled primed each millisecond, misses intervals, none of them unprimed");
    const ua::FrameCount late_index = host.frames_sent();
    host.device_late(late_by, false);
    host.on_time();
    const auto device_gap = host.counters();
    checks.expect(device_gap.missed_intervals == intervals(host_skipped + late_by) && device_gap.unprimed_intervals == intervals(late_by) && device_gap.watchdog_expiries == host_skipped + 1U &&
                      device_gap.last_miss.frame_index == late_index && device_gap.last_miss.unprimed == intervals(late_by),
                  "an ISR held off past the host's polls misses each unprimed: the device was late");
    host.device_late(late_by, true);
    host.on_time();
    const auto in_time = host.counters();
    checks.expect(in_time.missed_intervals == intervals(host_skipped + 2U * late_by - 1U) && in_time.unprimed_intervals == intervals(2U * late_by - 1U) &&
                      in_time.last_miss.unprimed == intervals(late_by - 1U),
                  "an ISR that primes before the host's poll in its frame misses one fewer: the zero gap after it takes the late read back from both counts");
    const ua::FrameCount before_late_one = host.counters().last_miss.frame_index;
    host.device_late(1U, true);
    host.on_time();
    const auto late_one = host.counters();
    checks.expect(late_one.missed_intervals == in_time.missed_intervals && late_one.unprimed_intervals == in_time.unprimed_intervals && late_one.last_miss.frame_index == before_late_one &&
                      late_one.watchdog_expiries == in_time.watchdog_expiries + 1U,
                  "an ISR one frame late that still primes in time misses nothing in the end, and the location returns to the one before");
    host.watchdog(ua::InTransfer::unprimed);
    host.on_time();
    host.skip(2U);
    host.on_time();
    const auto later = host.counters();
    checks.expect(later.missed_intervals == intervals(late_one.missed_intervals.value + 2U) && later.unprimed_intervals == late_one.unprimed_intervals,
                  "a finding belongs to the gap the next completion ends, which missed nothing here, and to no later gap");
    checks.expect(later.unprimed_intervals <= later.missed_intervals, "the unprimed count is part of the missed count");
}

// A gap in which the host pauses first and the ISR is held off at its end: the
// samples taken while the packet waited prove the host's part, and only the
// intervals after them are unprimed. Re-armed after each primed sample, the
// watchdog sees the late ISR although the gap began with the host's pause.
void test_unprimed_after_host_pause(Checks& checks)
{
    constexpr std::uint32_t host_skipped = 5U;
    constexpr std::uint32_t late_by = 2U;
    StreamingStream host{checks};
    host.on_time();
    const auto before = host.counters();
    const ua::FrameCount index = host.frames_sent();
    host.host_skips(host_skipped);
    host.device_late(late_by, false);
    host.on_time();
    const auto after = host.counters();
    checks.expect(after.missed_intervals == intervals(before.missed_intervals.value + host_skipped + late_by) && after.unprimed_intervals == intervals(before.unprimed_intervals.value + late_by) &&
                      after.watchdog_expiries == before.watchdog_expiries + host_skipped + 1U && after.last_miss.frame_index == index && after.last_miss.unprimed == intervals(late_by),
                  "a host pause then a late ISR: the host's intervals are proven primed, the device's are unprimed");
}

// An ISR held off past the watchdog's deadline but in time for the next poll
// reads its frame late: the gap counts one interval, unprimed, until a later
// completion takes it back. When the host then leaves out a poll, the gap
// after reads one frame while its primed sample proves a host skip: the
// interval moves there, as the host's.
void test_late_read_before_host_skip(Checks& checks)
{
    StreamingStream host{checks};
    host.on_time();
    const auto before = host.counters();
    host.held_in_time();
    const auto held = host.counters();
    host.host_skips(1U);
    host.on_time();
    const auto moved = host.counters();
    checks.expect(held.missed_intervals == intervals(before.missed_intervals.value + 1U) && held.unprimed_intervals == intervals(before.unprimed_intervals.value + 1U),
                  "a held-off ISR that read its frame late looks like an unprimed miss at first");
    checks.expect(moved.missed_intervals == held.missed_intervals && moved.unprimed_intervals == before.unprimed_intervals && moved.last_miss.frame_index == held.frames_sent &&
                      moved.last_miss.unprimed == ua::IntervalCount{},
                  "the host's skip after it holds that interval: the total stays, as the host's, located after the skip");
}

// The play's unprimed part follows the stream's over the gaps that delay its
// file, take-backs included, and restarts with the next PLAY.
void test_play_unprimed_intervals(Checks& checks)
{
    constexpr std::uint32_t host_skipped = 3U;
    constexpr std::uint32_t late_by = 2U;
    StreamingStream host{checks};
    host.device_late(late_by, false);
    host.on_time();
    const auto idle = host.counters();
    host.arm_play(checks);
    host.on_time();
    host.on_time();
    checks.expect(idle.unprimed_intervals == intervals(late_by) && host.play().unprimed_intervals == ua::IntervalCount{}, "a device-late gap before the play is the stream's alone");
    host.host_skips(host_skipped);
    host.on_time();
    host.device_late(late_by, true);
    const auto looked_late = host.play();
    host.on_time();
    const auto play = host.play();
    checks.expect(looked_late.unprimed_intervals == intervals(late_by) && play.missed_intervals == intervals(host_skipped + late_by - 1U) && play.unprimed_intervals == intervals(late_by - 1U) &&
                      play.unprimed_intervals <= play.missed_intervals,
                  "the play counts the host's skip as missed only, the late ISR as unprimed too, and the take-back from both");
    checks.expect(host.counters().unprimed_intervals == intervals(idle.unprimed_intervals.value + late_by - 1U), "the stream counts the same unprimed intervals since boot");
    checks.expect(host.counters().last_miss.play && host.counters().last_miss.unprimed == intervals(late_by - 1U), "the latest miss is the play's and carries its unprimed part");
    host.disarm();
    host.drain();
    host.arm_play(checks);
    checks.expect(host.play().unprimed_intervals == ua::IntervalCount{}, "the next PLAY restarts the play's unprimed part");
}

// The watchdog counts only while the stream streams: an expiry while it is
// closed, or primes before its first data packet, changes nothing and marks no
// later gap. The unprimed count runs since boot, and a reopened stream takes
// nothing back from an earlier one's.
void test_watchdog_outside_streaming(Checks& checks)
{
    constexpr std::uint32_t skipped = 2U;
    constexpr std::uint32_t late_by = 3U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller;
    stream.on_watchdog(ua::InTransfer::unprimed);
    configure_and_open(checks, stream, fifo);
    stream.on_watchdog(ua::InTransfer::unprimed);
    controller.millisecond(stream, fifo, silence);
    stream.on_watchdog(ua::InTransfer::unprimed);
    controller.millisecond(stream, fifo, silence);
    controller.skip(skipped);
    controller.millisecond(stream, fifo, silence);
    checks.expect(stream.counters().watchdog_expiries == 0U && stream.counters().unprimed_intervals == ua::IntervalCount{} && stream.counters().missed_intervals == intervals(skipped),
                  "expiries before the stream opened or while it primes are not counted, nor do they mark its first gap");
    controller.millisecond_device_late(stream, fifo, silence, late_by, false);
    controller.millisecond(stream, fifo, silence);
    const auto first_stream = stream.counters();
    checks.expect(first_stream.unprimed_intervals == intervals(late_by) && first_stream.missed_intervals == intervals(skipped + late_by) && first_stream.watchdog_expiries == 1U,
                  "a late ISR while streaming counts");
    stream.close();
    stream.on_watchdog(ua::InTransfer::unprimed);
    fifo.clear();
    Controller fresh;
    checks.expect(stream.open(fifo, ua::pcm24_stereo).is_ok(), "the stream reopens");
    fresh.millisecond(stream, fifo, silence);
    fresh.millisecond_read_late(stream, fifo, silence);
    fresh.millisecond(stream, fifo, silence);
    fresh.millisecond(stream, fifo, silence);
    const auto reopened = stream.counters();
    checks.expect(reopened.unprimed_intervals == first_stream.unprimed_intervals && reopened.missed_intervals == first_stream.missed_intervals &&
                      reopened.watchdog_expiries == first_stream.watchdog_expiries,
                  "a closed stream ignores an expiry, and the reopened stream's late first read takes nothing back from the earlier counts");
}

// The play's own missed intervals: the stream's rule over the gaps that end
// while the play has file frames queued, the first miss located at the first
// frame sent after it. The stream keeps counting its own.
void test_play_missed_intervals(Checks& checks)
{
    constexpr std::uint32_t startup_skipped = 4U;
    constexpr std::uint32_t first_skipped = 2U;
    constexpr std::uint32_t second_skipped = 3U;
    StreamingStream host{checks};
    host.skip(startup_skipped);
    host.on_time();
    host.arm_play(checks);
    host.on_time();
    host.on_time();
    checks.expect(host.stream_missed() == intervals(startup_skipped) && host.play().missed_intervals == ua::IntervalCount{} && host.play().first_miss_frame_index == ua::FrameCount{},
                  "the host's start-up skips before the play armed count for the stream, never for the play");
    const ua::FrameCount after_first_miss = host.frames_sent();
    host.skip(first_skipped);
    host.on_time();
    checks.expect(host.play().missed_intervals.value == first_skipped && host.play().first_miss_frame_index == after_first_miss && after_first_miss != ua::FrameCount{} &&
                      host.stream_missed() == intervals(startup_skipped + first_skipped),
                  "a gap while armed counts for the play and the stream, located at the first frame sent after it");
    host.late();
    host.on_time();
    host.skip(second_skipped);
    host.on_time();
    checks.expect(host.play().missed_intervals.value == first_skipped + second_skipped && host.play().first_miss_frame_index == after_first_miss &&
                      host.stream_missed() == intervals(startup_skipped + first_skipped + second_skipped),
                  "a late read inside the play is taken back, and a later gap adds to the count without moving the first miss");
}

// The play's window at both ends. Its start: no file frame is queued before
// the play's first completion after arming (its baseline), so a gap still open
// when the play arms, a late read before it, taken back by that completion,
// and a late read of that completion, taken back by the next, change only the
// stream's count, and the play's never goes below zero. Its end: the
// completion that confirms the last file frame sent, armed or not; the next
// may still take back a late read the play counted, later ones judge nothing
// for the play, so a late read followed by a real gap stays counted.
void test_play_missed_window(Checks& checks)
{
    {
        constexpr std::uint32_t spanning_skip = 3U;
        StreamingStream host{checks};
        host.skip(spanning_skip);
        host.arm_play(checks);
        host.on_time();
        host.on_time();
        checks.expect(host.stream_missed() == intervals(spanning_skip) && host.play().missed_intervals == ua::IntervalCount{} && host.play().first_miss_frame_index == ua::FrameCount{},
                      "a gap still open when the play arms ends at the play's first completion and is the stream's alone");
    }

    {
        StreamingStream host{checks};
        host.late();
        const auto counted = host.stream_missed();
        host.arm_play(checks);
        host.on_time();
        host.on_time();
        checks.expect(counted == intervals(1U) && host.stream_missed() == ua::IntervalCount{} && host.play().missed_intervals == ua::IntervalCount{} &&
                          host.play().first_miss_frame_index == ua::FrameCount{},
                      "a late read just before arming, taken back just after, is the stream's alone: the play's count neither drops nor rises");
    }

    {
        StreamingStream host{checks};
        host.arm_play(checks);
        host.late();
        host.on_time();
        checks.expect(host.stream_missed() == ua::IntervalCount{} && host.play().missed_intervals == ua::IntervalCount{} && host.play().first_miss_frame_index == ua::FrameCount{},
                      "a late read of the play's first completion is taken back without the play's count going below zero");
    }

    {
        StreamingStream host{checks};
        host.arm_play(checks);
        host.on_time();
        host.late();
        const auto looked_late = host.play();
        host.on_time();
        checks.expect(looked_late.missed_intervals.value == 1U && looked_late.first_miss_frame_index != ua::FrameCount{} && host.play().missed_intervals == ua::IntervalCount{} &&
                          host.play().first_miss_frame_index == ua::FrameCount{},
                      "a late read inside the play looks like a miss until the next completion takes it back, location included");
        checks.expect(looked_late.miss_unconfirmed && ua::confirmed_missed(looked_late) == ua::IntervalCount{} && !host.play().miss_unconfirmed &&
                          ua::confirmed_missed(host.play()) == ua::IntervalCount{},
                      "until the next completion the late read is no confirmed miss, and after it there is none");
    }

    {
        constexpr std::uint32_t skipped = 2U;
        StreamingStream host{checks};
        host.arm_play(checks);
        host.on_time();
        host.skip(1U);
        host.on_time();
        const auto one_gap = host.play();
        host.on_time();
        const auto confirmed = host.play();
        host.skip(skipped);
        host.on_time();
        const auto longer_gap = host.play();
        checks.expect(one_gap.missed_intervals.value == 1U && one_gap.miss_unconfirmed && ua::confirmed_missed(one_gap) == ua::IntervalCount{} && !confirmed.miss_unconfirmed &&
                          ua::confirmed_missed(confirmed).value == 1U,
                      "a gap of one missed interval is confirmed by the next completion, whose gap is not 0");
        checks.expect(longer_gap.missed_intervals.value == 1U + skipped && longer_gap.miss_unconfirmed && ua::confirmed_missed(longer_gap).value == skipped,
                      "a gap of two missed intervals confirms one at once: a late read inflates a gap by one interval at most");
    }

    {
        constexpr std::uint32_t skipped = 2U;
        constexpr std::uint32_t skipped_after = 5U;
        StreamingStream host{checks};
        host.arm_play(checks);
        host.on_time();
        host.skip(skipped);
        host.on_time();
        host.late();
        host.disarm();
        host.on_time();
        const auto taken_back = host.play();
        host.drain();
        const auto drained = host.play();
        host.late();
        host.on_time();
        host.skip(skipped_after);
        host.on_time();
        checks.expect(taken_back.missed_intervals.value == skipped && taken_back.pending_frames != ua::FrameCount{} && drained.pending_frames == ua::FrameCount{} &&
                          host.play().missed_intervals.value == skipped && host.stream_missed() == intervals(skipped + skipped_after),
                      "after disarming, the next completion takes back the play's late read; once its last file frame is sent, gaps and late reads are the stream's alone");
    }

    {
        constexpr std::uint32_t total = 3U * ua::chunk_frame_count;
        constexpr std::uint32_t skipped_after_file = 3U;
        StreamingStream host{checks, total};
        host.arm_play(checks);
        host.run_until_confirmed(ua::FrameCount{.value = total - ua::chunk_frames.value});
        host.late();
        const auto delivered = host.play();
        host.skip(skipped_after_file);
        host.on_time();
        checks.expect(delivered.file_frames.value == total && delivered.pending_frames == ua::FrameCount{} && delivered.armed && host.play().missed_intervals.value == 1U &&
                          host.stream_missed() == intervals(skipped_after_file),
                      "a late read of the completion that confirms the last file frame, then a gap: the next completion is not the play's, so the late read stays counted");
        checks.expect(delivered.miss_unconfirmed && ua::confirmed_missed(delivered) == ua::IntervalCount{} && !host.play().miss_unconfirmed && ua::confirmed_missed(host.play()).value == 1U,
                      "a completion that judges nothing for the play still confirms its latest gap");
    }

    {
        constexpr std::uint32_t skipped = 2U;
        StreamingStream host{checks};
        host.arm_play(checks);
        host.on_time();
        host.skip(skipped);
        host.on_time();
        host.reopen(checks);
        host.late();
        host.on_time();
        checks.expect(host.play().interrupted && host.play().missed_intervals.value == skipped && host.stream_missed() == intervals(skipped),
                      "a close ends the play's window: the reopened stream's late first read is taken back from neither the stream nor the interrupted play");
    }
}

// The file chunks a play leaves queued when it disarms (STOP, a fault, or a
// stall the player finds while the host is still silent) still go out and
// still count for it, so the gaps before them count too: the same stall reads
// the same whoever notices it first. Once the last of them is sent, later gaps
// and late reads are the stream's alone.
void test_play_missed_after_disarm(Checks& checks)
{
    constexpr std::uint32_t armed_milliseconds = 3U;
    {
        constexpr std::uint32_t stall_frames = stall_timeout.value + 49U;
        StreamingStream host{checks};
        host.arm_play(checks);

        for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
        {
            host.on_time();
        }

        host.skip(stall_frames);
        host.disarm();
        const auto left = host.play();
        const auto resume_index = host.frames_sent();
        host.drain();
        const auto play = host.play();
        const auto supplied = host.supplied();
        checks.expect(left.pending_frames != ua::FrameCount{} && play.pending_frames == ua::FrameCount{} && play.file_frames.value == supplied.size() &&
                          play.crc32_left == channel_crc(supplied, &ua::StereoFrame::left) && play.crc32_right == channel_crc(supplied, &ua::StereoFrame::right),
                      "a stalled play disarmed before the host polls again: its queued chunks go out after the stall and count");
        checks.expect(play.missed_intervals.value == stall_frames && play.first_miss_frame_index == resume_index && play.first_miss_frame_index <= play.last_frame_index &&
                          host.stream_missed() == intervals(stall_frames),
                      "the stall counts for the play although it disarmed first, located at the first frame sent after it");
    }

    {
        constexpr std::uint32_t skipped = 7U;
        StreamingStream host{checks};
        host.arm_play(checks);

        for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
        {
            host.on_time();
        }

        host.disarm();
        host.skip(skipped);
        const auto resume_index = host.frames_sent();
        host.drain();
        const auto drained = host.play();
        checks.expect(drained.pending_frames == ua::FrameCount{} && drained.file_frames.value == host.supplied().size() && drained.missed_intervals.value == skipped &&
                          drained.first_miss_frame_index == resume_index && drained.first_miss_frame_index <= drained.last_frame_index,
                      "STOP, then a gap before the queued file chunks go out: the gap counts for the play, located before its last file frame");
        host.skip(skipped);
        host.on_time();
        host.late();
        host.on_time();
        checks.expect(host.play().missed_intervals == drained.missed_intervals && host.play().first_miss_frame_index == drained.first_miss_frame_index &&
                          host.stream_missed() == intervals(2U * skipped),
                      "once the last file frame is sent, later gaps and late reads are the stream's alone");
    }
}

// Once the play's last file frame is confirmed sent, a gap delays none of its
// file: a skip before the player task notices and disarms is the stream's
// alone. A late read of the completion that confirmed it is still taken back
// by the next.
void test_play_missed_after_delivery(Checks& checks)
{
    constexpr std::uint32_t total = 3U * ua::chunk_frame_count;
    {
        constexpr std::uint32_t skipped = 1U;
        StreamingStream host{checks, total};
        host.arm_play(checks);
        host.run_until_confirmed(ua::FrameCount{.value = total});
        const auto delivered = host.play();
        host.skip(skipped);
        host.on_time();
        checks.expect(delivered.file_frames.value == total && delivered.pending_frames == ua::FrameCount{} && delivered.armed && host.play().missed_intervals == ua::IntervalCount{} &&
                          host.play().first_miss_frame_index == ua::FrameCount{} && host.stream_missed() == intervals(skipped),
                      "a skip after the last file frame, while the play is still armed, is the stream's alone");
    }

    {
        StreamingStream host{checks, total};
        host.arm_play(checks);
        host.run_until_confirmed(ua::FrameCount{.value = total - ua::chunk_frames.value});
        host.late();
        const auto looked_late = host.play();
        host.on_time();
        checks.expect(looked_late.file_frames.value == total && looked_late.pending_frames == ua::FrameCount{} && looked_late.missed_intervals.value == 1U &&
                          host.play().missed_intervals == ua::IntervalCount{} && host.play().first_miss_frame_index == ua::FrameCount{} && host.stream_missed() == ua::IntervalCount{},
                      "a late read of the completion that confirms the last file frame is taken back by the next");
    }
}

// A codec PLAY clears every play statistic, the missed intervals included,
// whatever the stream's state: STATUS then reports no USB play. File chunks
// still queued for the cleared play go out and count for no play.
void test_clear_play(Checks& checks)
{
    constexpr std::uint32_t total = 100U * ua::chunk_frame_count;
    constexpr std::uint32_t armed_milliseconds = 6U;
    constexpr std::uint32_t skipped = 2U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource file{total};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    controller.skip(skipped);
    controller.millisecond(stream, fifo, file);
    stream.disarm();
    const auto played = stream.play();
    checks.expect(played.file_frames != ua::FrameCount{} && played.missed_intervals.value == skipped && played.first_miss_frame_index != ua::FrameCount{} && played.pending_frames != ua::FrameCount{},
                  "the USB play sent file frames, missed intervals and left file chunks queued");
    stream.clear_play();
    checks.expect(is_no_play(stream.play()), "clearing the play zeroes every statistic");

    for (std::uint32_t tick = 0U; tick <= ua::fifo_capacity_chunks; ++tick)
    {
        controller.millisecond(stream, fifo, file);
    }

    checks.expect(is_no_play(stream.play()) && file.supplied().size() > played.file_frames.value, "the cleared play's queued file chunks go out and count for no play");
    accept_and_arm(checks, stream, ua::TickCount{.value = controller.tick()});
    controller.millisecond(stream, fifo, file);
    stream.close();
    checks.expect(stream.play().interrupted && stream.play().accept_frame_index != ua::FrameCount{}, "a play interrupted by a close keeps its statistics");
    stream.clear_play();
    checks.expect(is_no_play(stream.play()), "a closed stream's play clears too");
}

// A stalled play (error 20) is disarmed with its last chunks still queued. PLAY
// on the USB route restarts the statistics; when the host polls again, the
// leftover file frames go out after the new play's accept index and count for
// no play, the stall gap counts only for the stream, and the new play's
// statistics describe exactly its own file.
void test_stalled_play_leftovers(Checks& checks)
{
    constexpr std::uint32_t total = 100U * ua::chunk_frame_count;
    constexpr std::uint32_t armed_milliseconds = 5U;
    constexpr std::uint32_t stall_frames = 2U * stall_timeout.value;
    constexpr std::uint32_t next_total = 3U * ua::chunk_frame_count;
    constexpr std::uint32_t next_milliseconds = 10U;
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    FileSource stalled{total};
    FileSource next{next_total};
    Controller controller;
    configure_and_open(checks, stream, fifo);
    accept_and_arm(checks, stream, {});

    for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, stalled);
    }

    controller.skip(stall_frames);
    stream.disarm();
    const auto left = stream.play();
    const auto missed_before = stream.counters().missed_intervals.value;
    checks.expect(stream.accept_play().is_ok(), "PLAY is accepted while the stalled play's chunks are still queued");
    const auto accepted = stream.play();
    checks.expect(left.pending_frames != ua::FrameCount{} && accepted.pending_frames == ua::FrameCount{} && accepted.file_frames == ua::FrameCount{} &&
                      accepted.accept_frame_index == stream.counters().frames_sent,
                  "the new play starts with nothing pending: the leftovers are not its frames");

    for (std::uint32_t tick = 0U; tick <= ua::fifo_capacity_chunks; ++tick)
    {
        controller.millisecond(stream, fifo, next);
    }

    checks.expect(stream.play().file_frames == ua::FrameCount{} && stream.play().pending_frames == ua::FrameCount{} && stream.play().missed_intervals == ua::IntervalCount{} &&
                      stream.counters().missed_intervals == intervals(missed_before + stall_frames),
                  "the leftovers go out and count for no play, and the stall gap counts only for the stream");
    checks.expect(stream.arm(ua::TickCount{.value = controller.tick()}, stall_timeout).is_ok(), "the new play arms");

    for (std::uint32_t tick = 0U; tick < next_milliseconds; ++tick)
    {
        controller.millisecond(stream, fifo, next);
    }

    const auto play = stream.play();
    const auto sent = std::span{controller.sent()};
    const std::span<const ua::StereoFrame> leftovers = std::span{stalled.supplied()}.subspan(left.file_frames.value);
    const std::size_t accept = accepted.accept_frame_index.value;
    const std::size_t first = play.first_frame_index.value;
    bool on_wire = sent.size() >= (first + next_total) * pcm24_frame.value && next.supplied().size() == next_total && first >= accept + leftovers.size();

    for (std::size_t frame = accept; on_wire && frame < first + next_total; ++frame)
    {
        const bool leftover = frame - accept < leftovers.size();
        const bool in_next = frame >= first;
        const auto expected = leftover ? leftovers[frame - accept] : (in_next ? next.supplied()[frame - first] : ua::StereoFrame{});
        const auto received = wire_frame(sent, frame);
        on_wire = received.left == expected.left && received.right == expected.right;
    }

    checks.expect(leftovers.size() == left.pending_frames.value && on_wire, "after the accept index: the leftover file frames, silence, then the new file bit for bit");
    checks.expect(play.file_frames.value == next_total && play.pending_frames == ua::FrameCount{} && play.missed_intervals == ua::IntervalCount{} &&
                      play.crc32_left == channel_crc(next.supplied(), &ua::StereoFrame::left) && play.crc32_right == channel_crc(next.supplied(), &ua::StereoFrame::right),
                  "the new play's statistics describe exactly its own file");
}

void test_controls(Checks& checks)
{
    constexpr std::uint8_t cur = wfg_uac_request_cur;
    constexpr std::uint8_t range = wfg_uac_request_range;
    constexpr std::uint8_t clock = wfg_uac_entity_clock_source;
    constexpr std::uint8_t feature = wfg_uac_entity_feature_unit;
    constexpr std::uint8_t left_channel = 1U;
    constexpr ua::ControlRequest rate_cur{.code = cur, .entity = clock, .selector = wfg_uac_clock_frequency};
    constexpr ua::ControlRequest rate_range{.code = range, .entity = clock, .selector = wfg_uac_clock_frequency};
    constexpr ua::ControlRequest valid_cur{.code = cur, .entity = clock, .selector = wfg_uac_clock_validity};
    constexpr ua::ControlRequest mute_cur{.code = cur, .entity = feature, .selector = wfg_uac_feature_mute};
    constexpr ua::ControlRequest volume_cur{.code = cur, .entity = feature, .selector = wfg_uac_feature_volume};
    constexpr ua::ControlRequest volume_range{.code = range, .entity = feature, .selector = wfg_uac_feature_volume};
    constexpr std::array<std::uint8_t, 4> rate_bytes{0x80, 0xBB, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 14> rate_range_bytes{0x01, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 1> one{0x01};
    constexpr std::array<std::uint8_t, 1> zero{0x00};
    constexpr std::array<std::uint8_t, 2> zero_db{0x00, 0x00};
    constexpr std::array<std::uint8_t, 8> volume_range_bytes{0x01, 0x00, 0x00, 0x9C, 0x00, 0x00, 0x00, 0x01};
    constexpr std::array<std::uint8_t, 2> minus_10_db{0x00, 0xF6};
    constexpr std::array<std::uint8_t, 2> minus_100_db{0x00, 0x9C};
    constexpr std::array<std::uint8_t, 2> plus_1_db{0x00, 0x01};
    constexpr std::array<std::uint8_t, 2> minus_101_db{0x00, 0x9B};
    constexpr std::array<std::uint8_t, 4> rate_44100{0x44, 0xAC, 0x00, 0x00};
    constexpr std::int16_t minus_10_db_value = -2560;
    ua::UacControls controls;
    const auto answers = [&controls](const ua::ControlRequest& request, std::span<const std::uint8_t> expected)
    {
        const auto response = controls.get(request);
        return response && std::ranges::equal(std::span{response.value().bytes}.first(response.value().length.value), expected);
    };

    const auto stalls = [&controls](const ua::ControlRequest& request) { return !controls.get(request); };

    checks.expect(answers(rate_cur, rate_bytes), "clock GET CUR is 48000 as four little-endian bytes");
    checks.expect(answers(rate_range, rate_range_bytes), "clock GET RANGE is one subrange {48000, 48000, 0}");
    checks.expect(answers(valid_cur, one), "clock validity GET CUR is 1");
    checks.expect(answers(mute_cur, zero) && answers(volume_cur, zero_db), "the feature unit starts unmuted at 0 dB");
    checks.expect(answers(volume_range, volume_range_bytes), "volume GET RANGE is -100 dB to 0 dB in 1 dB steps");
    checks.expect(stalls({.code = range, .entity = clock, .selector = wfg_uac_clock_validity}) && stalls({.code = range, .entity = feature, .selector = wfg_uac_feature_mute}),
                  "RANGE of a control without one stalls");
    checks.expect(stalls({.code = cur, .entity = clock, .selector = wfg_uac_clock_frequency, .channel = left_channel}) &&
                      stalls({.code = cur, .entity = feature, .selector = wfg_uac_feature_volume, .channel = left_channel}),
                  "only the master channel has controls");
    checks.expect(stalls({.code = cur, .entity = wfg_uac_entity_input_terminal, .selector = 1U}) && stalls({.code = cur, .entity = wfg_uac_entity_output_terminal, .selector = 1U}) &&
                      stalls({.code = cur, .entity = clock, .selector = 3U}) && stalls({.code = 3U, .entity = clock, .selector = wfg_uac_clock_frequency}),
                  "terminals, unknown selectors and unknown request codes stall");
    checks.expect(controls.set(rate_cur, rate_bytes).is_ok(), "clock SET CUR accepts 48000");
    checks.expect(!controls.set(rate_cur, rate_44100).is_ok() && !controls.set(rate_cur, std::span{rate_bytes}.first(3U)).is_ok() && !controls.set(valid_cur, one).is_ok() &&
                      !controls.set(rate_range, rate_range_bytes).is_ok(),
                  "the clock rejects other rates, short payloads, validity and RANGE");
    checks.expect(controls.set(mute_cur, one).is_ok() && answers(mute_cur, one), "mute is stored and reported");
    checks.expect(!controls.set(mute_cur, std::array<std::uint8_t, 1>{2U}).is_ok() && answers(mute_cur, one) && !controls.set(mute_cur, zero_db).is_ok(),
                  "mute takes exactly one byte, 0 or 1; a rejected SET changes nothing");
    checks.expect(controls.set(volume_cur, minus_100_db).is_ok() && answers(volume_cur, minus_100_db) && controls.set(volume_cur, zero_db).is_ok() && answers(volume_cur, zero_db),
                  "volume accepts both ends of the offered range, -100 dB and 0 dB");
    checks.expect(controls.set(volume_cur, minus_10_db).is_ok() && answers(volume_cur, minus_10_db), "volume is stored and reported");
    checks.expect(!controls.set(volume_cur, plus_1_db).is_ok() && !controls.set(volume_cur, minus_101_db).is_ok() && !controls.set(volume_cur, one).is_ok() && answers(volume_cur, minus_10_db),
                  "volume outside the range, or not two bytes, is rejected and changes nothing");
    checks.expect(!controls.set({.code = cur, .entity = feature, .selector = wfg_uac_feature_volume, .channel = left_channel}, zero_db).is_ok(), "SET on a channel without controls stalls");
    const auto settings = controls.settings();
    checks.expect(settings.muted && settings.volume.value == minus_10_db_value, "the host settings are readable for STATUS");
}

// The 16-bit sample at `index` (little-endian int16) of a byte stream.
std::int16_t pcm16_at(std::span<const std::byte> bytes, std::size_t index)
{
    std::array<std::byte, sizeof(std::int16_t)> raw{};
    std::ranges::copy(bytes.subspan(index * raw.size(), raw.size()), raw.begin());
    return std::bit_cast<std::int16_t>(raw);
}

// The 24-bit sample a 24-in-32 subslot carries: its low byte is the zero pad,
// so the division is exact.
[[nodiscard]] constexpr std::int32_t sample_of(std::int32_t subslot) noexcept
{
    return subslot / ua::subslot_scale;
}

// The 16-bit wire value of a 24-bit sample, through wire_subslot().
[[nodiscard]] constexpr std::int16_t wire16(std::int32_t sample) noexcept
{
    const auto source = std::bit_cast<ua::SubslotBytes>(sample * ua::subslot_scale);
    std::array<std::byte, sizeof(std::int16_t)> raw{};
    std::ranges::copy(ua::wire_subslot(source, raw.size()), raw.begin());
    return std::bit_cast<std::int16_t>(raw);
}

// A 24-bit sample and the 16-bit sample the wire carries for it.
struct Truncation
{
    std::int32_t sample{};
    std::int16_t wire{};
};

// sample >> 8, truncated toward minus infinity, with no rounding: both 24-bit
// extremes become the 16-bit ones; -1 and -256 become -1, -257 becomes -2;
// 255 becomes 0 and 256 becomes 1.
constexpr auto truncations = std::to_array<Truncation>({{.sample = pcm24_min, .wire = std::numeric_limits<std::int16_t>::min()},
                                                        {.sample = pcm24_max, .wire = std::numeric_limits<std::int16_t>::max()},
                                                        {.sample = 0, .wire = 0},
                                                        {.sample = -1, .wire = -1},
                                                        {.sample = 1, .wire = 0},
                                                        {.sample = -256, .wire = -1},
                                                        {.sample = -257, .wire = -2},
                                                        {.sample = 255, .wire = 0},
                                                        {.sample = 256, .wire = 1}});

static_assert(std::ranges::all_of(truncations, [](const Truncation& truncation) { return wire16(truncation.sample) == truncation.wire; }), "16-bit samples are the 24-bit ones >> 8, truncated");

// Whether one frame on the wire (`bytes`, one frame of `format`) carries
// `source` as the format should: each channel the format has, left first, as
// the 24-in-32 subslot itself (4-byte subslots) or the 24-bit sample >> 8
// (2-byte subslots). Written from the format's rule, not from pack_frames().
[[nodiscard]] bool wire_frame_is(std::span<const std::byte> bytes, const ua::StereoFrame& source, const ua::StreamFormat& format)
{
    if (bytes.size() != ua::frame_bytes(format).value)
    {
        return false;
    }

    const auto channels = std::to_array<std::int32_t>({source.left, source.right});
    bool same = true;

    for (std::size_t channel = 0U; channel < format.channels; ++channel)
    {
        const std::int32_t subslot_value = std::span{channels}[channel];
        same = same && (format.subslot_bytes == sizeof(std::int32_t) ? subslot(bytes, channel) == subslot_value : pcm16_at(bytes, channel) == (sample_of(subslot_value) >> CHAR_BIT));
    }

    return same;
}

// A chunk of edge samples, then a ramp: both 24-bit extremes, zero, the
// truncation edges around zero and around one 16-bit step. The right channel
// runs the edges backwards, then mirrors the ramp (-1 - left, which stays
// within 24 bits), so the channels differ.
[[nodiscard]] ua::Chunk packing_chunk() noexcept
{
    constexpr auto edges = std::to_array<std::int32_t>({pcm24_min, pcm24_max, 0, -1, 1, -256, -257, 255, 256, -255, 257, pcm24_min + 1, pcm24_max - 1});
    constexpr std::int32_t ramp_step = 349525;
    ua::Chunk chunk{};
    const std::span<ua::StereoFrame, ua::chunk_frame_count> frames{chunk};
    std::int32_t ramp = pcm24_min + 1;

    for (std::size_t index = 0U; index < frames.size(); ++index)
    {
        const bool edge = index < edges.size();
        const std::int32_t left = edge ? std::span{edges}[index] : ramp;
        const std::int32_t right = edge ? std::span{edges}[edges.size() - 1U - index] : -1 - ramp;
        frames[index] = {.left = left * ua::subslot_scale, .right = right * ua::subslot_scale};
        ramp = ramp_next(ramp, ramp_step);
    }

    return chunk;
}

// pack_frames(): the 24-bit stereo packet is the source bytes unchanged; the
// 16-bit packets hold each sample's top 16 bits (sample >> 8, truncated, the
// lane's --expect-truncation-bits 8), and the mono one the left channel alone.
// Every packet is 48 frames of its format.
void test_pack_frames(Checks& checks)
{
    const ua::Chunk chunk = packing_chunk();
    std::array<std::byte, ua::max_packet_byte_count> wire{};
    const auto packed24 = ua::pack_frames(chunk, ua::pcm24_stereo, wire);
    checks.expect(packed24.size() == pcm24_packet.value && std::ranges::equal(packed24, std::as_bytes(std::span{chunk})),
                  "the 24-bit stereo packet is the source frames unchanged: sample << 8, left then right, extremes included");

    for (const auto& format : {ua::pcm16_stereo, ua::pcm16_mono})
    {
        const auto packed = ua::pack_frames(chunk, format, wire);
        const std::size_t frame = ua::frame_bytes(format).value;
        bool exact = packed.size() == ua::packet_bytes(format).value && packed.size() == ua::chunk_frame_count * frame;

        for (std::size_t index = 0U; exact && index < chunk.size(); ++index)
        {
            exact = wire_frame_is(packed.subspan(index * frame, frame), std::span{chunk}[index], format);
        }

        checks.expect(exact, format.channels == wfg_uac_channels ? "16-bit stereo: 48 frames of each channel's sample >> 8, left then right"
                                                                 : "16-bit mono: 48 frames of the left channel's sample >> 8, the right one dropped");
    }

    // The truncation table on both channels of a chunk, through the packer.
    ua::Chunk edges{};

    for (std::size_t index = 0U; index < truncations.size(); ++index)
    {
        const std::int32_t subslot_value = std::span{truncations}[index].sample * ua::subslot_scale;
        std::span{edges}[index] = {.left = subslot_value, .right = subslot_value};
    }

    for (const auto& format : {ua::pcm16_stereo, ua::pcm16_mono})
    {
        const auto packed = ua::pack_frames(edges, format, wire);
        bool truncated = true;

        for (std::size_t index = 0U; index < truncations.size(); ++index)
        {
            for (std::size_t channel = 0U; channel < format.channels; ++channel)
            {
                truncated = truncated && pcm16_at(packed, index * format.channels + channel) == std::span{truncations}[index].wire;
            }
        }

        checks.expect(truncated, "16-bit samples truncate: +/-2^23 extremes, -1 -> -1, -256 -> -1, -257 -> -2, 255 -> 0, 256 -> 1");
    }
}

// Every format streams as alternate 1 does: a pre-fill of two of its packets,
// the FIFO held there, every write and every completed packet one packet of
// the format, 48 frames each, and exact zeros while idle.
void test_idle_stream_in_each_format(Checks& checks)
{
    constexpr std::uint32_t milliseconds = 20U;

    for (const auto& format : ua::stream_formats)
    {
        const ua::ByteCount packet = ua::packet_bytes(format);
        FakeFifo fifo{ua::fifo_capacity_bytes.value};
        fifo.select(format);
        ua::UacStream stream;
        ua::Silence silence;
        Controller controller{format};
        configure_and_open(checks, stream, fifo, format);
        checks.expect(fifo.queued() == ua::fifo_target_bytes(format) && fifo.writes() == ua::fifo_target_chunks && stream.counters().format == format &&
                          stream.counters().frames_queued.value == ua::fifo_target_chunks * ua::chunk_frames.value,
                      "each format opens with two of its packets of silence queued: 96 frames, as alternate 1");
        bool level_held = true;

        for (std::uint32_t tick = 0U; tick < milliseconds; ++tick)
        {
            controller.millisecond(stream, fifo, silence);
            level_held = level_held && fifo.queued() == ua::fifo_target_bytes(format);
        }

        const auto counters = stream.counters();
        const bool whole_packets = std::ranges::all_of(fifo.write_sizes(), [packet](std::uint32_t size) { return size == packet.value; });

        checks.expect(level_held && whole_packets && fifo.refusals() == 0U, "every write is one packet of the format, and the FIFO stays at two");
        checks.expect(counters.state == ua::StreamState::streaming && counters.short_packets == 0U && counters.frames_sent.value == (milliseconds - 1U) * ua::chunk_frames.value &&
                          counters.missed_intervals == ua::IntervalCount{},
                      "each completed packet of the format counts 48 frames and none is short");
        checks.expect(controller.sent().size() == std::size_t{milliseconds - 1U} * packet.value && all_zero(controller.sent()), "an idle stream sends exact zeros in every format");
    }
}

// A completed packet is short when the size reported for it is not one
// packet of the stream's format. The frames it confirms are those of the
// packet TinyUSB loaded for it, one 16-bit stereo packet here, whatever size
// is reported, and a size other than that packet's is also a mismatch.
void test_short_packets_follow_the_format(Checks& checks)
{
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    fifo.select(ua::pcm16_stereo);
    ua::UacStream stream;
    ua::Silence silence;
    Controller controller{ua::pcm16_stereo};
    configure_and_open(checks, stream, fifo, ua::pcm16_stereo);
    controller.millisecond(stream, fifo, silence);
    controller.millisecond(stream, fifo, silence);
    const auto whole = stream.counters();
    checks.expect(whole.short_packets == 0U && whole.sent_mismatches == 0U && whole.frames_sent == ua::chunk_frames, "a 192-byte packet is whole in 16-bit stereo: 48 frames");
    controller.millisecond_reporting(stream, fifo, silence, pcm24_packet);
    controller.millisecond_reporting(stream, fifo, silence, ua::packet_bytes(ua::pcm16_mono));
    const auto odd = stream.counters();
    checks.expect(odd.short_packets == 2U && odd.sent_mismatches == 2U && odd.frames_sent.value == 3U * ua::chunk_frames.value,
                  "384 and 96 bytes reported for 16-bit stereo packets: each is short and a mismatch, and confirms the packet's 48 frames");
}

// The same file in every format: on the wire as each format carries it, and
// the same per-play CRCs, over the 24-bit source samples of both channels,
// even where the format truncates them or drops the right one. The play's
// format is latched when its first file frame goes out.
void test_play_in_each_format(Checks& checks)
{
    constexpr std::uint32_t total = 3U * ua::chunk_frame_count + 17U;
    constexpr std::uint32_t idle_milliseconds = 2U;
    constexpr std::uint32_t play_milliseconds = 10U;
    std::vector<ua::PlayStatistics> plays;

    for (const auto& format : ua::stream_formats)
    {
        const std::size_t frame_size = ua::frame_bytes(format).value;
        FakeFifo fifo{ua::fifo_capacity_bytes.value};
        fifo.select(format);
        ua::UacStream stream;
        FileSource file{total};
        Controller controller{format};
        configure_and_open(checks, stream, fifo, format);

        for (std::uint32_t tick = 0U; tick < idle_milliseconds; ++tick)
        {
            controller.millisecond(stream, fifo, file);
        }

        accept_and_arm(checks, stream, ua::TickCount{.value = controller.tick()});
        controller.millisecond(stream, fifo, file);
        checks.expect(stream.play().format == ua::StreamFormat{} && stream.play().file_frames == ua::FrameCount{}, "a play's format reads all zero until its first file frame has gone out");

        for (std::uint32_t tick = 0U; tick < play_milliseconds; ++tick)
        {
            controller.millisecond(stream, fifo, file);
        }

        const auto play = stream.play();
        const std::span<const ua::StereoFrame> supplied{file.supplied()};
        const auto sent = std::span{controller.sent()};
        const std::size_t first = play.first_frame_index.value;
        bool exact = supplied.size() == total && sent.size() >= (first + total) * frame_size;

        for (std::size_t index = 0U; exact && index < sent.size() / frame_size; ++index)
        {
            const bool in_file = index >= first && index - first < total;
            exact = wire_frame_is(sent.subspan(index * frame_size, frame_size), in_file ? supplied[index - first] : ua::StereoFrame{}, format);
        }

        checks.expect(exact, "zeros, then each file frame as the format carries it, then zeros");
        checks.expect(play.format == format && play.file_frames.value == total && play.pending_frames == ua::FrameCount{} && play.crc32_left == channel_crc(supplied, &ua::StereoFrame::left) &&
                          play.crc32_right == channel_crc(supplied, &ua::StereoFrame::right),
                      "the play latches its format, and each channel's CRC is over the 24-bit source samples (3-byte LE) in every format");
        plays.push_back(play);
    }

    const bool same_crcs =
        std::ranges::all_of(plays, [&plays](const ua::PlayStatistics& play) { return play.crc32_left == plays.front().crc32_left && play.crc32_right == plays.front().crc32_right; });

    checks.expect(plays.size() == ua::stream_formats.size() && same_crcs && plays.front().crc32_left != plays.front().crc32_right,
                  "the same file gives the same uac_play_crc32_l/_r in 24-bit stereo, 16-bit stereo and 16-bit mono");
}

// The host goes from one format straight to another while a play is armed:
// TinyUSB's close callback, its FIFO dropped, then the new alternate. The play
// ends as on a close (the player's error 19), its statistics the exact prefix
// sent before, its format the one that prefix went out in; the stream reopens
// in the new format. open() alone, without the close callback, does the same.
void test_format_change_mid_play(Checks& checks)
{
    constexpr std::uint32_t total = 100U * ua::chunk_frame_count;
    constexpr std::uint32_t armed_milliseconds = 6U;

    for (const bool close_first : {true, false})
    {
        FakeFifo fifo{ua::fifo_capacity_bytes.value};
        ua::UacStream stream;
        FileSource file{total};
        Controller controller;
        configure_and_open(checks, stream, fifo);
        accept_and_arm(checks, stream, {});

        for (std::uint32_t tick = 0U; tick < armed_milliseconds; ++tick)
        {
            controller.millisecond(stream, fifo, file);
        }

        const auto before = stream.play();

        if (close_first)
        {
            stream.close();
            checks.expect(stream.counters().format == ua::StreamFormat{}, "a closed stream has no format");
        }

        fifo.clear();
        fifo.select(ua::pcm16_stereo);
        checks.expect(stream.open(fifo, ua::pcm16_stereo).is_ok(), "the host selects alternate 2 straight from alternate 1");
        const auto play = stream.play();
        const auto counters = stream.counters();
        const auto prefix = std::span{file.supplied()}.first(play.file_frames.value);
        checks.expect(play.interrupted && !play.armed && play.pending_frames == ua::FrameCount{} && before.file_frames != ua::FrameCount{} && play.file_frames == before.file_frames &&
                          play.file_frames.value < file.supplied().size(),
                      close_first ? "a direct change of format interrupts the armed play: its queued chunks never count" : "open() alone interrupts the armed play too");
        checks.expect(play.crc32_left == channel_crc(prefix, &ua::StereoFrame::left) && play.crc32_right == channel_crc(prefix, &ua::StereoFrame::right) && play.format == ua::pcm24_stereo,
                      "the statistics are the exact prefix sent, in the format it went out in");
        checks.expect(counters.format == ua::pcm16_stereo && counters.opens == 2U && counters.state == ua::StreamState::priming && counters.frames_sent == ua::FrameCount{} &&
                          fifo.queued() == ua::fifo_target_bytes(ua::pcm16_stereo) && all_zero(fifo.contents()),
                      "the stream reopens in the new format, its frame index restarted, pre-filled with silence");
        checks.expect(stream.arm(ua::TickCount{}, stall_timeout).code == ua::stream_interrupted.code && !stream.play().armed, "the interrupted play cannot arm on the reopened stream");
    }
}

// The stream opens only in a format it knows, and a refusal changes nothing.
void test_unknown_format(Checks& checks)
{
    constexpr ua::StreamFormat pcm24_mono{.alternate = 4U, .channels = 1U, .subslot_bytes = 4U, .bits = 24U};
    FakeFifo fifo{ua::fifo_capacity_bytes.value};
    ua::UacStream stream;
    configure_and_open(checks, stream, fifo);
    const auto open = stream.counters();
    const auto refused = stream.open(fifo, pcm24_mono);
    checks.expect(refused.code == ua::stream_format_unknown.code && stream.counters().format == ua::pcm24_stereo && stream.counters().opens == open.opens && fifo.writes() == ua::fifo_target_chunks,
                  "a format outside stream_formats is refused and the open stream stays as it was");
    checks.expect(!ua::offered_format(0U).has_value() && ua::offered_format(1U).value() == ua::pcm24_stereo && ua::offered_format(2U).value() == ua::pcm16_stereo &&
                      !ua::offered_format(4U).has_value(),
                  "alternates 1 and 2 are always offered, 0 and 4 never");
    checks.expect(ua::offered_format(3U).has_value() == mono_offered && (!mono_offered || ua::offered_format(3U).value() == ua::pcm16_mono),
                  "alternate 3, 16-bit mono, is offered exactly in a WFG_UAC_MONO_ALT build");
}

// The firmware's software FIFO behind the shim, shared with the C entry points.
FakeFifo& shim_fifo()
{
    static FakeFifo fifo{ua::fifo_capacity_bytes.value};
    return fifo;
}

// The player behind player_consume_from_isr(): a file of `remaining` frames.
struct StubPlayer
{
    FileSource file{0U};
};

StubPlayer& stub_player()
{
    static StubPlayer player;
    return player;
}

// The RTOS tick TinyUSB's assert hook reads (xTaskGetTickCountFromISR).
std::uint32_t& stub_tick()
{
    static std::uint32_t tick{};
    return tick;
}

// TinyUSB's FIFO counts are 16-bit; the fake never holds more than its capacity.
std::uint16_t to_u16(ua::ByteCount count) noexcept
{
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(count.value, std::numeric_limits<std::uint16_t>::max()));
}

// What the USB ISR does each millisecond, through the C entry point.
class ShimController
{
public:
    void millisecond()
    {
        const ua::ByteCount completed = in_flight_;
        in_flight_ = byte_count(shim_fifo().load_packet().size());
        wfg_usb_audio_tx_done(to_u16(completed), frame_number(frame_).value, frame_);
        ++frame_;
    }

    // The host lets `frames` frames go by without polling.
    void skip(std::uint32_t frames) noexcept { frame_ += frames; }

private:
    ua::ByteCount in_flight_{};
    std::uint32_t frame_{Controller::start_frame};
};

void test_c_entry_points(Checks& checks)
{
    constexpr std::array<std::uint8_t, 4> rate_bytes{0x80, 0xBB, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 2> minus_10_db{0x00, 0xF6};
    constexpr std::uint16_t volume_payload_bytes = 2U;
    constexpr std::int16_t minus_10_db_value = -2560;
    constexpr std::uint32_t milliseconds = 5U;
    const wfg_uac_request_t rate{.code = wfg_uac_request_cur, .entity = wfg_uac_entity_clock_source, .selector = wfg_uac_clock_frequency, .channel = 0U};
    const auto answer = wfg_usb_audio_get_request(rate);
    checks.expect(answer.result == kWFG_UacRequestAccepted && answer.length == rate_bytes.size() && std::ranges::equal(std::span{answer.bytes}.first(rate_bytes.size()), rate_bytes),
                  "the C GET entry returns the answer bytes and length");
    const wfg_uac_request_t terminal{.code = wfg_uac_request_cur, .entity = wfg_uac_entity_input_terminal, .selector = 1U, .channel = 0U};
    const auto stall = wfg_usb_audio_get_request(terminal);
    checks.expect(stall.result == kWFG_UacRequestStall && stall.length == 0U, "the C GET entry reports a stall with no bytes");
    const wfg_uac_request_t volume{.code = wfg_uac_request_cur, .entity = wfg_uac_entity_feature_unit, .selector = wfg_uac_feature_volume, .channel = 0U};
    checks.expect(wfg_usb_audio_set_request(volume, minus_10_db.data(), volume_payload_bytes) == kWFG_UacRequestAccepted && ua::host_settings().volume.value == minus_10_db_value,
                  "the C SET entry stores the host volume");
    checks.expect(wfg_usb_audio_set_request(volume, minus_10_db.data(), 1U) == kWFG_UacRequestStall, "the C SET entry stalls a bad payload");
    shim_fifo().clear();
    ShimController controller;
    controller.millisecond();
    checks.expect(shim_fifo().writes() == 0U, "nothing is written before the stream opens");
    checks.expect(wfg_usb_audio_open_stream(wfg_uac_alternate_pcm24_stereo) == kWFG_UacRequestStall && shim_fifo().writes() == 0U, "the alternate-1 entry stalls before SET_CONFIGURATION");
    wfg_usb_audio_configure_stream();
    checks.expect(wfg_usb_audio_open_stream(wfg_uac_alternate_pcm24_stereo) == kWFG_UacRequestAccepted, "the alternate-1 entry accepts inside a configuration");
    checks.expect(shim_fifo().queued() == pcm24_target && all_zero(shim_fifo().contents()), "the alternate-1 entry pre-fills two silent packets through the shim");

    for (std::uint32_t tick = 0U; tick < milliseconds; ++tick)
    {
        controller.millisecond();
    }

    auto counters = ua::stream_counters();
    checks.expect(counters.state == ua::StreamState::streaming && counters.frames_sent.value == (milliseconds - 1U) * ua::chunk_frames.value && counters.short_packets == 0U &&
                      shim_fifo().high_water() == pcm24_target.value,
                  "the tx-done entry counts packets and keeps the shim FIFO at its target");
    checks.expect(stub_player().file.calls() == 0U, "the tx-done entry asks the player for frames only while a play is armed");
    constexpr std::uint32_t host_skipped = 2U;
    constexpr std::uint32_t device_late = 1U;
    wfg_usb_audio_watchdog_expired(true);
    controller.skip(host_skipped);
    controller.millisecond();
    wfg_usb_audio_watchdog_expired(false);
    controller.skip(device_late);
    controller.millisecond();
    const auto watched = ua::stream_counters();
    checks.expect(watched.watchdog_expiries == counters.watchdog_expiries + 2U && watched.missed_intervals == intervals(counters.missed_intervals.value + host_skipped + device_late) &&
                      watched.unprimed_intervals == intervals(counters.unprimed_intervals.value + device_late),
                  "the watchdog entry passes the endpoint's state: a primed endpoint leaves the gap the host's, an unprimed one makes it the device's");
    stub_player().file = FileSource{ua::chunk_frames.value};
    checks.expect(ua::accept_play().is_ok() && ua::arm_play(ua::TickCount{}, stall_timeout).is_ok(), "the player accepts and arms a play on the open stream");

    for (std::uint32_t tick = 0U; tick < milliseconds; ++tick)
    {
        controller.millisecond();
    }

    checks.expect(stub_player().file.calls() != 0U && ua::play_statistics().file_frames == ua::chunk_frames, "an armed play takes the player's frames through the tx-done entry");
    ua::disarm_play();
    checks.expect(!ua::play_statistics().armed, "the player disarms the play");
    ua::clear_play();
    checks.expect(is_no_play(ua::play_statistics()), "the player clears the play for a PLAY on the codec route");
    wfg_usb_audio_close_stream();
    auto writes = shim_fifo().writes();
    controller.millisecond();
    counters = ua::stream_counters();
    checks.expect(counters.state == ua::StreamState::closed && shim_fifo().writes() == writes, "the close entry (alternate 0) stops the producer");
    checks.expect(wfg_usb_audio_open_stream(wfg_uac_alternate_pcm24_stereo) == kWFG_UacRequestAccepted, "alternate 1 reopens inside the same configuration");
    wfg_usb_audio_deconfigure_stream();
    writes = shim_fifo().writes();
    controller.millisecond();
    checks.expect(ua::stream_counters().state == ua::StreamState::closed && shim_fifo().writes() == writes, "the deconfigure entry (bus reset, unplug, SET_CONFIGURATION(0)) closes the stream");
    checks.expect(wfg_usb_audio_open_stream(wfg_uac_alternate_pcm24_stereo) == kWFG_UacRequestStall && ua::stream_counters().state == ua::StreamState::closed,
                  "after the deconfigure entry, a stale alternate-1 request stalls");
    wfg_usb_audio_configure_stream();
    checks.expect(ua::stream_counters().state == ua::StreamState::closed && wfg_usb_audio_open_stream(wfg_uac_alternate_pcm24_stereo) == kWFG_UacRequestAccepted &&
                      ua::stream_counters().state == ua::StreamState::priming,
                  "the configure entry (SET_CONFIGURATION) lets alternate 1 open again");
    const auto asserts = ua::stack_asserts();
    wfg_usb_count_stack_assert();
    wfg_usb_count_stack_assert();
    checks.expect(ua::stack_asserts().count == asserts.count + 2U, "TinyUSB assertion failures are counted");
}

// TinyUSB as it opens an alternate: the open stream is left, its FIFO
// dropped, the endpoint opened with the alternate's wMaxPacketSize, then the
// open entry. Returns that entry's result.
wfg_uac_request_result_t select_alternate(std::uint8_t alternate, const ua::StreamFormat& endpoint_format)
{
    wfg_usb_audio_close_stream();
    shim_fifo().clear();
    shim_fifo().select(endpoint_format);
    return wfg_usb_audio_open_stream(alternate);
}

// The open entry opens the alternate the host selected, in its format, and
// stalls alternate 0 and any alternate the descriptors do not offer: the mono
// one (3) unless the build offers it (WFG_UAC_MONO_ALT).
void test_c_entry_alternates(Checks& checks)
{
    const auto opens_in = [](const ua::StreamFormat& format)
    {
        const auto packet = ua::packet_bytes(format).value;
        const bool whole = std::ranges::all_of(shim_fifo().write_sizes(), [packet](std::uint32_t size) { return size == packet; });

        return ua::stream_counters().format == format && ua::stream_counters().state == ua::StreamState::priming && shim_fifo().queued() == ua::fifo_target_bytes(format) && whole &&
               all_zero(shim_fifo().contents());
    };

    wfg_usb_audio_configure_stream();
    checks.expect(select_alternate(wfg_uac_alternate_pcm24_stereo, ua::pcm24_stereo) == kWFG_UacRequestAccepted && opens_in(ua::pcm24_stereo),
                  "alternate 1 opens 24-bit stereo, pre-filled with two 384-byte packets of silence");
    checks.expect(select_alternate(wfg_uac_alternate_pcm16_stereo, ua::pcm16_stereo) == kWFG_UacRequestAccepted && opens_in(ua::pcm16_stereo),
                  "alternate 2 opens 16-bit stereo, pre-filled with two 192-byte packets of silence");
    const auto mono = select_alternate(wfg_uac_alternate_pcm16_mono, ua::pcm16_mono);

    if (mono_offered)
    {
        checks.expect(mono == kWFG_UacRequestAccepted && opens_in(ua::pcm16_mono), "a WFG_UAC_MONO_ALT build opens alternate 3 as 16-bit mono, pre-filled with two 96-byte packets of silence");
    }
    else
    {
        checks.expect(mono == kWFG_UacRequestStall && ua::stream_counters().state == ua::StreamState::closed && shim_fifo().writes() == 0U,
                      "without WFG_UAC_MONO_ALT, alternate 3 stalls and opens nothing");
    }

    constexpr std::uint8_t beyond = 4U;
    checks.expect(select_alternate(0U, ua::pcm24_stereo) == kWFG_UacRequestStall && select_alternate(beyond, ua::pcm24_stereo) == kWFG_UacRequestStall &&
                      ua::stream_counters().state == ua::StreamState::closed && ua::stream_counters().format == ua::StreamFormat{} && shim_fifo().writes() == 0U,
                  "alternate 0 and alternates beyond the descriptors stall, leaving the stream closed with no format");
    shim_fifo().select(ua::pcm24_stereo);
}

// The record keeps the first call site, and the latest's with the stream
// frame index and tick passed in; a new log is all zero.
void test_stack_assert_log(Checks& checks)
{
    constexpr ua::CodeAddress first_site{.value = 0x30009D24U};
    constexpr ua::CodeAddress later_site{.value = 0x3001F8DAU};
    constexpr ua::FrameCount first_index{.value = 96U};
    constexpr ua::FrameCount later_index{.value = 4800U};
    constexpr ua::TickCount first_tick{.value = 1000U};
    constexpr ua::TickCount later_tick{.value = 1100U};
    ua::StackAssertLog log;
    const auto none = log.asserts();
    checks.expect(none.count == 0U && none.first_pc == ua::CodeAddress{} && none.last_pc == ua::CodeAddress{} && none.last_frame_index == ua::FrameCount{} && none.last_tick.value == 0U,
                  "before any assertion failure every field is 0");
    log.record(first_site, first_index, first_tick);
    const auto one = log.asserts();
    checks.expect(one.count == 1U && one.first_pc == first_site && one.last_pc == first_site && one.last_frame_index == first_index && one.last_tick.value == first_tick.value,
                  "the first failure is both the first and the latest");
    log.record(later_site, later_index, later_tick);
    log.record(later_site, later_index, later_tick);
    const auto three = log.asserts();
    checks.expect(three.count == 3U && three.first_pc == first_site && three.last_pc == later_site && three.last_frame_index == later_index && three.last_tick.value == later_tick.value,
                  "later failures become the latest with their frame index and tick; the first keeps its call site");
}

// A Thumb-2 BL at call_at is 4 bytes and returns to call_at + 4 with the Thumb
// bit set; a BLX through a register is 2 bytes. Either call site lies inside
// its call instruction, where addr2line finds the call's line.
void test_call_site()
{
    constexpr std::uint32_t call_at = 0x30009D22U;
    constexpr std::uint32_t bl_bytes = 4U;
    constexpr std::uint32_t blx_register_bytes = 2U;
    static_assert(ua::call_site(call_at + bl_bytes + ua::thumb_state_bit) == ua::CodeAddress{.value = call_at + ua::thumb_halfword_bytes}, "after a BL: the call's second halfword");
    static_assert(ua::call_site(call_at + blx_register_bytes + ua::thumb_state_bit) == ua::CodeAddress{.value = call_at}, "after a BLX register: the call itself");
}

// Two call sites shaped like TU_ASSERT's failure path: the hook, then the
// return, so the hook call is never a tail call. Never inlined, and their
// results differ and depend on the record, so the compiler can neither fold
// them into one function nor fold their results into their callers: each is
// its own function with its own call site.
[[gnu::noinline]] std::uint32_t assert_then_count()
{
    wfg_usb_count_stack_assert();
    return ua::stack_asserts().count;
}

[[gnu::noinline]] ua::CodeAddress assert_then_site()
{
    wfg_usb_count_stack_assert();
    return ua::stack_asserts().last_pc;
}

// The low 32 bits of a function's entry address, as a CodeAddress keeps them.
template <class Function> [[nodiscard]] std::uint32_t entry_address(Function* function) noexcept
{
    // A function pointer as an integer: the test boundary's one cast.
    return static_cast<std::uint32_t>(std::bit_cast<std::uintptr_t>(function));
}

// Whether `site` lies in the first bytes of the function at `entry`: the
// helpers above make their call before anything else.
[[nodiscard]] bool near_entry(ua::CodeAddress site, std::uint32_t entry) noexcept
{
    constexpr std::uint32_t call_within_bytes = 64U;
    return site.value - entry < call_within_bytes;
}

// The hook through its C entry point: each call counts, records its own call
// site in the calling function (the same site twice gives the same address),
// and the stream frame index and tick at that moment; the first call site
// stays the first.
void test_stack_assert_hook(Checks& checks)
{
    constexpr std::uint32_t streamed_milliseconds = 6U;
    constexpr std::uint32_t first_tick = 4321U;
    constexpr std::uint32_t later_tick = 4325U;
    shim_fifo().clear();
    ShimController controller;
    wfg_usb_audio_configure_stream();
    checks.expect(wfg_usb_audio_open_stream(wfg_uac_alternate_pcm24_stereo) == kWFG_UacRequestAccepted, "the stream opens for the assert hook");

    for (std::uint32_t tick = 0U; tick < streamed_milliseconds; ++tick)
    {
        controller.millisecond();
    }

    const auto before = ua::stack_asserts();
    const auto count_index = ua::stream_counters().frames_sent;
    stub_tick() = first_tick;
    const std::uint32_t counted = assert_then_count();
    const auto in_count = ua::stack_asserts();
    const auto first_site = before.count == 0U ? in_count.last_pc : before.first_pc;
    checks.expect(counted == before.count + 1U && in_count.count == counted && in_count.first_pc == first_site && count_index != ua::FrameCount{} && in_count.last_frame_index == count_index &&
                      in_count.last_tick.value == first_tick,
                  "the hook counts the failure with the stream frame index and tick at that moment");
    checks.expect(near_entry(in_count.last_pc, entry_address(&assert_then_count)), "the call site lies in the function that called the hook");
    controller.millisecond();
    controller.millisecond();
    const auto site_index = ua::stream_counters().frames_sent;
    stub_tick() = later_tick;
    const ua::CodeAddress site = assert_then_site();
    const auto in_site = ua::stack_asserts();
    checks.expect(in_site.count == before.count + 2U && in_site.first_pc == first_site && in_site.last_pc == site && in_site.last_frame_index == site_index && site_index != count_index &&
                      in_site.last_tick.value == later_tick,
                  "a later failure is the latest, with its own frame index and tick; the first keeps its call site");
    checks.expect(near_entry(site, entry_address(&assert_then_site)) && site != in_count.last_pc, "each call site is its own, in its own function");
    checks.expect(assert_then_count() == before.count + 3U && ua::stack_asserts().last_pc == in_count.last_pc, "the same call site gives the same address");
    wfg_usb_audio_close_stream();
}
} // namespace

namespace waveform_generator
{
usb_audio::FrameCount player_consume_from_isr(usb_audio::ChunkFrames frames) noexcept
{
    return stub_player().file.fill(frames);
}
} // namespace waveform_generator

std::uint16_t m110_tinyusb_audio_in_queued_bytes()
{
    return to_u16(shim_fifo().queued());
}

TickType_t xTaskGetTickCountFromISR()
{
    return stub_tick();
}

std::uint16_t m110_tinyusb_audio_in_free_bytes()
{
    return to_u16(shim_fifo().free_space());
}

std::uint16_t m110_tinyusb_audio_in_write(const void* bytes, std::uint16_t count)
{
    if (count > ua::max_packet_byte_count)
    {
        return 0U;
    }

    // The C shim passes the packet as const void*; this is that boundary's one cast.
    const ua::PacketBytes packet{static_cast<const std::byte*>(bytes), count};
    return to_u16(shim_fifo().write(packet));
}

int main()
{
    Checks checks;
    test_chunk_records(checks);
    test_prefill(checks);
    test_streaming(checks);
    test_primer_before_open(checks);
    test_first_data_completion_reported_short(checks);
    test_underrun(checks);
    test_close_and_reopen(checks);
    test_configuration(checks);
    test_never_overfill(checks);
    test_source(checks);
    test_play_statistics(checks);
    test_scheduled_start(checks);
    test_disarm_keeps_a_consistent_prefix(checks);
    test_refused_chunk_is_lost(checks);
    test_short_completion_leaves_no_ghost(checks);
    test_short_completion_marks_the_play(checks);
    test_sent_mismatches_saturate(checks);
    test_close_mid_play(checks);
    test_accept_and_arm(checks);
    test_missed_intervals(checks);
    test_miss_location(checks);
    test_miss_location_play(checks);
    test_long_gap(checks);
    test_unprimed_intervals(checks);
    test_unprimed_after_host_pause(checks);
    test_late_read_before_host_skip(checks);
    test_play_unprimed_intervals(checks);
    test_watchdog_outside_streaming(checks);
    test_play_missed_intervals(checks);
    test_play_missed_window(checks);
    test_play_missed_after_disarm(checks);
    test_play_missed_after_delivery(checks);
    test_clear_play(checks);
    test_stalled_play_leftovers(checks);
    test_stall_flag(checks);
    test_controls(checks);
    test_pack_frames(checks);
    test_idle_stream_in_each_format(checks);
    test_short_packets_follow_the_format(checks);
    test_play_in_each_format(checks);
    test_format_change_mid_play(checks);
    test_unknown_format(checks);
    test_c_entry_points(checks);
    test_c_entry_alternates(checks);
    test_stack_assert_log(checks);
    test_call_site();
    test_stack_assert_hook(checks);

    if (checks.failures() != 0)
    {
        std::fputs("USB audio tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("USB audio tests: PASS\n", stdout);
    return 0;
}
