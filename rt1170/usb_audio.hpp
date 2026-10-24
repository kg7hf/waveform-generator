// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// USB Audio Class 2 capture stream and controls of the player's composite device
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: a UacStream belongs to the USB interrupt level: the USB ISR, or a
// task (wfg_usb, or the player arming a play) inside a critical section that
// masks it. UacControls belongs to the wfg_usb task, where TinyUSB runs control
// requests.
// Contract: portable logic over plain types. TinyUSB is reached only through
// usb/usb_tinyusb_port.h (the IN FIFO) and usb/usb_audio_callbacks.h; the
// player supplies file frames through player_consume_from_isr() (player.hpp).

#pragma once

#include "common/crc32.hpp"
#include "common/pcm24.hpp"
#include "common/status.hpp"
#include "usb/usb_composite.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace waveform_generator::usb_audio
{

// Frames and bytes meet at the FIFO; each count carries its unit in its type.
struct FrameCount
{
    std::uint32_t value{};

    constexpr auto operator<=>(const FrameCount&) const noexcept = default;
};

struct ByteCount
{
    std::uint32_t value{};

    constexpr auto operator<=>(const ByteCount&) const noexcept = default;
};

// The controller's USB frame number (the frame of the last SOF): 11 bits, one
// frame per millisecond at both speeds, wrapping every 2.048 s.
struct UsbFrameNumber
{
    std::uint16_t value{};
};

// A USB frame number extended to 32 bits across its wraps.
struct UsbFrameCount
{
    std::uint32_t value{};

    constexpr auto operator<=>(const UsbFrameCount&) const noexcept = default;
};

// The RTOS tick count: one tick per millisecond (FreeRTOSConfig.h).
struct TickCount
{
    std::uint32_t value{};
};

// A span of RTOS ticks, in milliseconds like TickCount.
struct TickSpan
{
    std::uint32_t value{};
};

// A span of USB frames: the frames from one IN completion to the next.
struct UsbFrameSpan
{
    std::uint32_t value{};

    constexpr auto operator<=>(const UsbFrameSpan&) const noexcept = default;
};

// 1 ms service intervals (USB frames) in which the host completed no packet.
struct IntervalCount
{
    std::uint32_t value{};

    constexpr auto operator<=>(const IntervalCount&) const noexcept = default;
};

// One completed transfer on the IN endpoint, as the USB ISR reports it.
struct Completion
{
    ByteCount sent{};       // bytes of the packet that completed; 0 for a primer
    UsbFrameNumber frame{}; // the controller's frame number when the ISR ran
    TickCount tick{};       // the RTOS tick when the ISR ran
};

// The IN endpoint as the USB ISR finds it when the service watchdog expires
// (no completion handled for 1.5 service intervals since the last one, or one
// interval after a late one or after the previous sample; see
// usb/usb_tinyusb_port.h): the packet the last completion primed still waits
// for the host's poll, or no transfer is primed because the completion that
// took the packet is still waiting for its ISR.
enum class InTransfer : std::uint8_t
{
    unprimed = 0U, // the USB ISR is held off with a completion pending: polls meanwhile found nothing to send
    primed = 1U,   // a packet waits: the host has not polled for it
};

// Where the stream found a missed interval: the stream index of the first frame
// sent after the gap (the packet whose completion ended it), how many of the
// intervals the gap still counts are unprimed (the device's; see
// unprimed_part()), and whether the gap delayed the file frames of the USB
// play current then, so that it counts in that play's missed intervals.
struct MissLocation
{
    FrameCount frame_index{};
    IntervalCount unprimed{};
    bool play{};
};

// One stereo frame as the player supplies it and the 24-bit format carries it:
// two 4-byte subslots, left then right, each a 24-bit sample in bits 31..8
// (sample << 8), little-endian on the wire. The other formats are made from
// it (pack_frames()).
struct StereoFrame
{
    std::int32_t left{};
    std::int32_t right{};
};

// A capture format: what one alternate setting of the streaming interface
// carries (usb/usb_composite.h). On the wire each frame is its left sample,
// then its right one when the format is stereo, each the top subslot_bytes
// bytes of the sample's 24-in-32 subslot (wire_subslot()).
struct StreamFormat
{
    std::uint8_t alternate{};     // the streaming interface's alternate setting; 0 for none
    std::uint8_t channels{};      // 2: left and right; 1: the left channel alone
    std::uint8_t subslot_bytes{}; // bytes per sample on the wire (bSubslotSize)
    std::uint8_t bits{};          // valid bits, the subslot's top ones (bBitResolution)

    constexpr bool operator==(const StreamFormat&) const noexcept = default;
};

inline constexpr std::uint32_t sample_rate_hz = wfg_uac_sample_rate_hz;
inline constexpr std::uint32_t usb_frames_per_second = 1000U;
inline constexpr std::size_t chunk_frame_count = wfg_uac_packet_frames;
inline constexpr FrameCount chunk_frames{.value = wfg_uac_packet_frames};
// The largest packet: alternate 1's, one chunk of source frames.
inline constexpr std::size_t max_packet_byte_count = wfg_uac_max_packet_bytes;

inline constexpr StreamFormat pcm24_stereo{
    .alternate = wfg_uac_alternate_pcm24_stereo, .channels = wfg_uac_channels, .subslot_bytes = wfg_uac_pcm24_subslot_bytes, .bits = wfg_uac_pcm24_resolution_bits};
inline constexpr StreamFormat pcm16_stereo{
    .alternate = wfg_uac_alternate_pcm16_stereo, .channels = wfg_uac_channels, .subslot_bytes = wfg_uac_pcm16_subslot_bytes, .bits = wfg_uac_pcm16_resolution_bits};
inline constexpr StreamFormat pcm16_mono{
    .alternate = wfg_uac_alternate_pcm16_mono, .channels = wfg_uac_mono_channels, .subslot_bytes = wfg_uac_pcm16_subslot_bytes, .bits = wfg_uac_pcm16_resolution_bits};
// Every format, by alternate setting from 1. A player image offers the first
// offered_format_count of them: the mono one only in a WFG_UAC_MONO_ALT build.
inline constexpr auto stream_formats = std::to_array<StreamFormat>({pcm24_stereo, pcm16_stereo, pcm16_mono});

inline constexpr std::size_t offered_format_count = wfg_uac_last_alternate;

[[nodiscard]] constexpr ByteCount frame_bytes(const StreamFormat& format) noexcept
{
    return {.value = std::uint32_t{format.channels} * std::uint32_t{format.subslot_bytes}};
}

// One packet, one USB millisecond: 48 frames.
[[nodiscard]] constexpr ByteCount packet_bytes(const StreamFormat& format) noexcept
{
    return {.value = chunk_frames.value * frame_bytes(format).value};
}

// The producer keeps two packets of the stream's format queued in a FIFO that
// holds four of the largest.
inline constexpr std::uint32_t fifo_target_chunks = 2U;
inline constexpr ByteCount fifo_capacity_bytes{.value = wfg_uac_fifo_bytes};
inline constexpr std::uint32_t fifo_capacity_chunks = wfg_uac_fifo_bytes / wfg_uac_max_packet_bytes;

[[nodiscard]] constexpr ByteCount fifo_target_bytes(const StreamFormat& format) noexcept
{
    return {.value = fifo_target_chunks * packet_bytes(format).value};
}

// Records of chunks written and not yet confirmed sent: at most a full FIFO
// of the largest packets plus the packet TinyUSB has loaded, rounded up to a
// power of two. The producer keeps fewer queued in every format.
inline constexpr std::size_t chunk_record_capacity = 8U;
// A 24-bit sample sits in the high bytes of its source subslot, above a zero
// pad byte: the subslot is sample << 8, sample * subslot_scale.
inline constexpr std::size_t pcm24_sample_bytes = audio::pcm24_bytes_per_sample;
inline constexpr std::size_t subslot_pad_bytes = std::size_t{wfg_uac_pcm24_subslot_bytes} - pcm24_sample_bytes;
inline constexpr std::int32_t subslot_scale = std::int32_t{1} << (CHAR_BIT * subslot_pad_bytes);
inline constexpr std::uint32_t usb_frame_number_modulus = 2048U;
inline constexpr std::uint32_t usb_frame_number_mask = usb_frame_number_modulus - 1U;

using Chunk = std::array<StereoFrame, chunk_frame_count>;
using ChunkFrames = std::span<StereoFrame, chunk_frame_count>;
using SourceChunk = std::span<const StereoFrame, chunk_frame_count>;
// One packet as the stream's format carries it: packet_bytes() of that format.
using PacketBytes = std::span<const std::byte>;
using PacketBuffer = std::array<std::byte, max_packet_byte_count>;
using WireBuffer = std::span<std::byte, max_packet_byte_count>;
using SubslotBytes = std::array<std::byte, sizeof(std::int32_t)>;
using Pcm24Bytes = std::array<std::uint8_t, pcm24_sample_bytes>;

// What a format must be for the producer: a packet that fits the endpoint
// buffer, a top-up that never overwrites the FIFO, and samples that are the
// top bytes of the source subslot with every bit valid but the 24-bit
// format's pad byte.
[[nodiscard]] constexpr bool producer_supports(const StreamFormat& format) noexcept
{
    const bool known_layout = (format.channels == wfg_uac_channels || format.channels == wfg_uac_mono_channels) && format.subslot_bytes != 0U && format.subslot_bytes <= sizeof(std::int32_t) &&
                              std::size_t{format.bits} == CHAR_BIT * std::min(std::size_t{format.subslot_bytes}, pcm24_sample_bytes);
    return known_layout && packet_bytes(format).value <= max_packet_byte_count && fifo_target_bytes(format).value + packet_bytes(format).value <= fifo_capacity_bytes.value;
}

static_assert(std::endian::native == std::endian::little, "subslots are stored in wire order");
static_assert(std::is_trivially_copyable_v<StereoFrame> && std::is_standard_layout_v<StereoFrame>);
static_assert(std::is_trivially_copyable_v<StreamFormat> && std::is_standard_layout_v<StreamFormat>);
static_assert(sizeof(StereoFrame) == frame_bytes(pcm24_stereo).value, "a source frame is a frame of the 24-bit format");
static_assert(sizeof(Chunk) == max_packet_byte_count && packet_bytes(pcm24_stereo).value == max_packet_byte_count, "a source chunk is exactly one packet of the 24-bit format, the largest");
static_assert(packet_bytes(pcm16_stereo).value == wfg_uac_pcm16_stereo_packet_bytes && packet_bytes(pcm16_mono).value == wfg_uac_pcm16_mono_packet_bytes, "each format's packet is 48 of its frames");
static_assert(std::ranges::all_of(stream_formats, producer_supports), "the producer carries every format");
static_assert(offered_format_count >= 2U && offered_format_count <= stream_formats.size(), "24- and 16-bit stereo always, mono by option");
static_assert(chunk_frames.value * usb_frames_per_second == sample_rate_hz, "one chunk per USB millisecond");
static_assert(chunk_record_capacity >= fifo_capacity_chunks + 1U, "every queued chunk and the one in flight has a record");
static_assert(std::has_single_bit(chunk_record_capacity) && std::has_single_bit(usb_frame_number_modulus));
static_assert(subslot_pad_bytes == 1U && std::size_t{wfg_uac_pcm24_resolution_bits} == CHAR_BIT * pcm24_sample_bytes, "24 valid bits above one zero pad byte");

// offered_format() of an alternate that carries no format: alternate 0 has no
// endpoint, and the descriptors offer only alternates 1 to offered_format_count.
inline constexpr m110::Status format_not_offered{.code = m110::StatusCode::invalid_argument, .message = "UAC2 alternate setting not offered"};

// The format of the alternate setting the host selected, or format_not_offered.
[[nodiscard]] constexpr m110::Result<StreamFormat> offered_format(std::uint32_t alternate) noexcept
{
    if (alternate == 0U || alternate > offered_format_count)
    {
        return format_not_offered;
    }

    return std::span{stream_formats}[alternate - 1U];
}

static_assert(offered_format(wfg_uac_alternate_pcm24_stereo).value() == pcm24_stereo && offered_format(wfg_uac_alternate_pcm16_stereo).value() == pcm16_stereo,
              "alternate 1 is the 24-bit stereo format, alternate 2 the 16-bit one");
static_assert(!offered_format(0U).has_value() && !offered_format(offered_format_count + 1U).has_value(), "no format beyond the offered alternates");

// A sample as a format with `subslot_bytes`-byte subslots carries it: the top
// subslot_bytes bytes of its 24-in-32 source subslot, little-endian. Four are
// the subslot itself, sample << 8 with its zero pad byte; two are the 24-bit
// sample's top 16 bits, sample >> 8: arithmetic truncation toward minus
// infinity, with no rounding and no dither, so the 16-bit value is exactly
// the 24-bit one >> 8. (Never more than the subslot's four bytes.)
[[nodiscard]] constexpr std::span<const std::byte> wire_subslot(const SubslotBytes& source, std::size_t subslot_bytes) noexcept
{
    return std::span{source}.last(std::min(subslot_bytes, source.size()));
}

// Writes the chunk's frames as `format` carries them to the front of `wire`
// and returns those bytes: per frame the left channel's wire_subslot(), then
// the right one's when the format is stereo (a mono format carries the left
// channel). The source frames stay as they are; for the 24-bit stereo format
// the packet is their bytes unchanged. Any format fits: at most four bytes
// per sample, two samples per frame.
[[nodiscard]] constexpr PacketBytes pack_frames(SourceChunk frames, const StreamFormat& format, WireBuffer wire) noexcept
{
    const bool stereo = format.channels == wfg_uac_channels;
    auto out = wire.begin();

    for (const StereoFrame& frame : frames)
    {
        const auto left = std::bit_cast<SubslotBytes>(frame.left);
        out = std::ranges::copy(wire_subslot(left, format.subslot_bytes), out).out;

        if (stereo)
        {
            const auto right = std::bit_cast<SubslotBytes>(frame.right);
            out = std::ranges::copy(wire_subslot(right, format.subslot_bytes), out).out;
        }
    }

    return std::span<const std::byte>{wire.begin(), out};
}

// The bytes a PCM24 WAV stores for the sample in subslot: its three high bytes
// on the wire (the low byte is the zero pad), little-endian.
[[nodiscard]] constexpr Pcm24Bytes pcm24_bytes(std::int32_t subslot) noexcept
{
    const auto wire = std::bit_cast<std::array<std::uint8_t, sizeof(std::int32_t)>>(subslot);
    return {wire[1], wire[2], wire[3]};
}

// pcm24_bytes() of a sample's subslot is what common/pcm24.hpp writes to a WAV.
[[nodiscard]] constexpr bool matches_wav_bytes(audio::Pcm24Sample sample) noexcept
{
    Pcm24Bytes wav{};
    audio::write_pcm24_le(wav.data(), sample);
    return pcm24_bytes(sample * subslot_scale) == wav;
}

static_assert(matches_wav_bytes(audio::pcm24_min) && matches_wav_bytes(audio::pcm24_max) && matches_wav_bytes(0) && matches_wav_bytes(-1), "a sample's wire bytes are its PCM24 WAV bytes");

// The missed-interval rule: `missed` once the next completion comes `gap`
// frames after the previous one. One packet completes per 1 ms interval, so a
// gap of n >= 1 frames missed n - 1 intervals. The ISR reads the frame number
// when it runs, so a completion handled just after a frame boundary looks one
// frame late: a gap of 2, then a gap of 0 to the next completion. The 0 takes
// that interval back, but only from a count that holds one, so a count never
// goes below zero for a late read it did not count.
[[nodiscard]] constexpr IntervalCount missed_after_gap(IntervalCount missed, UsbFrameSpan gap) noexcept
{
    if (gap == UsbFrameSpan{})
    {
        return {.value = missed == IntervalCount{} ? 0U : missed.value - 1U};
    }

    return {.value = missed.value + gap.value - 1U};
}

static_assert(missed_after_gap(IntervalCount{}, UsbFrameSpan{.value = 1U}) == IntervalCount{}, "one completion per frame misses nothing");
static_assert(missed_after_gap(IntervalCount{.value = 1U}, UsbFrameSpan{.value = 3U}) == IntervalCount{.value = 3U}, "a gap of n frames adds n - 1");
static_assert(missed_after_gap(IntervalCount{.value = 1U}, UsbFrameSpan{}) == IntervalCount{}, "a zero gap takes a late read back");
static_assert(missed_after_gap(IntervalCount{}, UsbFrameSpan{}) == IntervalCount{}, "a take-back never goes below zero");

// A total that includes the count `part`, once that count has become
// `updated`: the count since boot follows the open stream's count, a take-back
// included. `part` never exceeds `total`.
[[nodiscard]] constexpr IntervalCount with_part_updated(IntervalCount total, IntervalCount part, IntervalCount updated) noexcept
{
    return {.value = total.value - part.value + updated.value};
}

static_assert(with_part_updated(IntervalCount{.value = 2U}, IntervalCount{.value = 1U}, IntervalCount{.value = 3U}) == IntervalCount{.value = 4U},
              "a gap in the open stream adds to the total, which keeps the earlier streams' count");
static_assert(with_part_updated(IntervalCount{.value = 3U}, IntervalCount{.value = 2U}, IntervalCount{.value = 1U}) == IntervalCount{.value = 2U},
              "a take-back in the open stream lowers the total by one");

// `count` moved by the change from `before` to `after`, never below zero: a
// play's count follows the stream's over each gap the play judges.
[[nodiscard]] constexpr IntervalCount moved_with(IntervalCount count, IntervalCount before, IntervalCount after) noexcept
{
    if (after >= before)
    {
        return {.value = count.value + (after.value - before.value)};
    }

    return {.value = count.value - std::min(count.value, before.value - after.value)};
}

static_assert(moved_with(IntervalCount{.value = 1U}, IntervalCount{.value = 2U}, IntervalCount{.value = 4U}) == IntervalCount{.value = 3U}, "a count follows a rise");
static_assert(moved_with(IntervalCount{.value = 2U}, IntervalCount{.value = 4U}, IntervalCount{.value = 3U}) == IntervalCount{.value = 1U} &&
                  moved_with(IntervalCount{.value = 1U}, IntervalCount{.value = 4U}, IntervalCount{.value = 2U}) == IntervalCount{},
              "a count follows a fall, never below zero");

inline constexpr std::uint32_t usb_frame_number_half = usb_frame_number_modulus / 2U;

// The frames from one completion to the next. Their 11-bit frame numbers alias
// once a gap reaches 2048 frames (2.048 s), so of the spans congruent to the
// frame numbers' difference `aliased` this is the one nearest `ticks`, the RTOS
// ticks (1 ms each, on the device's clock) read in the same two ISRs: exact
// while the two clocks disagree by less than 1024 ms over the gap.
[[nodiscard]] constexpr UsbFrameSpan dealiased_gap(UsbFrameSpan aliased, TickSpan ticks) noexcept
{
    if (ticks.value <= aliased.value + usb_frame_number_half)
    {
        return aliased;
    }

    const std::uint32_t wraps = (ticks.value - aliased.value + usb_frame_number_half) / usb_frame_number_modulus;
    return {.value = aliased.value + wraps * usb_frame_number_modulus};
}

static_assert(dealiased_gap(UsbFrameSpan{.value = 1U}, TickSpan{.value = 1U}) == UsbFrameSpan{.value = 1U} && dealiased_gap(UsbFrameSpan{.value = 2U}, TickSpan{}) == UsbFrameSpan{.value = 2U},
              "a short gap is the frame numbers' difference, whatever a lagging tick says");
static_assert(dealiased_gap(UsbFrameSpan{.value = usb_frame_number_mask}, TickSpan{.value = usb_frame_number_mask - 1U}) == UsbFrameSpan{.value = usb_frame_number_mask},
              "the longest gap the frame numbers tell apart stays");
static_assert(dealiased_gap(UsbFrameSpan{}, TickSpan{.value = usb_frame_number_modulus}) == UsbFrameSpan{.value = usb_frame_number_modulus}, "a gap of 2048 frames is not a zero gap");
static_assert(dealiased_gap(UsbFrameSpan{.value = usb_frame_number_half}, TickSpan{.value = usb_frame_number_modulus + usb_frame_number_half - 1U}) ==
                  UsbFrameSpan{.value = usb_frame_number_modulus + usb_frame_number_half},
              "a pause of 3072 frames is not the 1024 its frame numbers tell, even with the tick a frame behind");
static_assert(dealiased_gap(UsbFrameSpan{.value = 4U}, TickSpan{.value = 2U * usb_frame_number_modulus + 3U}) == UsbFrameSpan{.value = 2U * usb_frame_number_modulus + 4U},
              "a gap of two wraps or more is whole");

// The service watchdog's samples during one gap between completions
// (UacStream::on_watchdog). Each comes at least one service interval after the
// gap began or after the sample before it. One that found the packet still
// primed proves a poll the host left out; one that found nothing primed means
// the USB ISR was held off with a completion pending.
struct GapSamples
{
    IntervalCount host_skipped{}; // samples that found the packet still primed
    bool unprimed{};              // a sample found nothing primed
};

// The part of a gap's `missed` intervals counted unprimed, the device's: none
// unless a sample found nothing primed, and never those the primed samples
// prove the host left out. A held-off ISR hides whether the host polled
// before the completion it holds, so this is an upper bound on the device's
// own intervals: one more where the host skipped while the ISR was held off,
// or where a held-off ISR still primed in time but read its frame late.
[[nodiscard]] constexpr IntervalCount unprimed_part(IntervalCount missed, const GapSamples& samples) noexcept
{
    if (!samples.unprimed)
    {
        return {};
    }

    return {.value = missed.value - std::min(missed.value, samples.host_skipped.value)};
}

static_assert(unprimed_part(IntervalCount{.value = 3U}, GapSamples{.unprimed = true}) == IntervalCount{.value = 3U}, "an ISR held off from the gap's start: every interval it missed is the device's");
static_assert(unprimed_part(IntervalCount{.value = 4U}, GapSamples{.host_skipped = IntervalCount{.value = 3U}, .unprimed = true}) == IntervalCount{.value = 1U},
              "an ISR held off at the end of a host pause: the intervals the primed samples prove are the host's");
static_assert(unprimed_part(IntervalCount{.value = 4U}, GapSamples{.host_skipped = IntervalCount{.value = 4U}}) == IntervalCount{} &&
                  unprimed_part(IntervalCount{.value = 1U}, GapSamples{.host_skipped = IntervalCount{.value = 2U}, .unprimed = true}) == IntervalCount{},
              "a host pause alone is the host's, and the device's part never goes below zero");

// The UAC2 IN FIFO as the producer sees it, in bytes. write() returns the bytes
// accepted: the whole packet whenever free_space() had room for it.
template <class T>
concept InFifo = requires(T& fifo, const T& view, PacketBytes packet) {
    {
        view.queued()
    }

    noexcept -> std::same_as<ByteCount>;
    {
        view.free_space()
    }

    noexcept -> std::same_as<ByteCount>;
    {
        fifo.write(packet)
    }

    noexcept -> std::same_as<ByteCount>;
};

// Supplies the next chunk of an armed play. fill() writes every frame and
// returns how many carry file audio: the leading frames, the rest are silence.
// It runs in the USB ISR, so it never blocks, allocates, logs or touches I/O.
// The firmware's source is the player (player_consume_from_isr); a stream that
// is not armed plays Silence whatever the source.
template <class T>
concept FrameSource = requires(T& source, ChunkFrames frames) {
    {
        source.fill(frames)
    }

    noexcept -> std::same_as<FrameCount>;
};

// Exact digital silence.
struct Silence
{
    [[nodiscard]] FrameCount fill(ChunkFrames frames) const noexcept
    {
        std::ranges::fill(frames, StereoFrame{});
        return {};
    }
};

enum class StreamState : std::uint8_t
{
    closed = 0U,    // alternate 0, bus reset, unplug, a configuration change, or never opened
    priming = 1U,   // an alternate with a format; only TinyUSB's zero-length primers have completed
    streaming = 2U, // a data packet has completed
};

// open() outside a configuration: before the first SET_CONFIGURATION, or a
// request queued before a bus reset or unplug that TinyUSB has not processed yet.
inline constexpr m110::Status stream_not_configured{.code = m110::StatusCode::unavailable, .message = "UAC2 stream outside a configuration"};
// open() with a format the producer does not know (not one of stream_formats).
inline constexpr m110::Status stream_format_unknown{.code = m110::StatusCode::invalid_argument, .message = "UAC2 stream format unknown"};
// accept_play() on a closed stream. The message is the PLAY reply code.
inline constexpr m110::Status stream_not_open{.code = m110::StatusCode::unavailable, .message = "NO_STREAM"};
// arm() after the stream accepted for the play closed, or closed and reopened.
inline constexpr m110::Status stream_interrupted{.code = m110::StatusCode::unavailable, .message = "USB_STREAM_CLOSED"};
// ChunkRecords::push() on a full ring: the chunk gets no record and is not written.
inline constexpr m110::Status chunk_records_full{.code = m110::StatusCode::buffer_too_small, .message = "UAC2 chunk records full"};
// The IN FIFO did not accept a whole chunk.
inline constexpr m110::Status chunk_refused{.code = m110::StatusCode::io_error, .message = "UAC2 IN FIFO refused a chunk"};

// Whether stream index `position` has reached `target`. Indices wrap after
// 2^32 frames: a target less than half that range ahead is still to come,
// any other has passed. A playlist schedules at most an hour's gap ahead.
[[nodiscard]] constexpr bool index_reached(FrameCount position, FrameCount target) noexcept
{
    constexpr std::uint32_t half_range = std::numeric_limits<std::uint32_t>::max() / 2U + 1U;
    return position.value - target.value < half_range;
}

static_assert(index_reached(FrameCount{.value = 2U * chunk_frames.value}, FrameCount{.value = 2U * chunk_frames.value}) && !index_reached(chunk_frames, FrameCount{.value = 2U * chunk_frames.value}) &&
                  index_reached(FrameCount{.value = 2U}, FrameCount{.value = std::numeric_limits<std::uint32_t>::max()}),
              "a target is reached at and after it, across the index's wrap");

// The stream index from which an armed play may take the source's frames, on
// the stream that `stream_opens` (StreamCounters::opens) names: the PLAYLIST
// sequencer's device-time start of an item an exact number of frames after
// the one before it. A play's first file frame leads a chunk, so it goes out
// at the first chunk that starts at or after `index`.
struct ScheduledStart
{
    FrameCount index{};
    std::uint32_t stream_opens{};
};

// Stream frame indices (FrameCount) count frames since the stream opened, the
// silent pre-fill included; they wrap after 24.8 h. Frames, not bytes: every
// packet of every format is 48 frames.
// The miss attribution (last_miss .. unprimed_intervals) and the format are
// reported by the WFG-LIVE/1 USB? query, not by STATUS. last_miss and max_gap
// restart when the stream opens, as its frame indices do; the two counts run
// since boot.
struct StreamCounters
{
    StreamState state{StreamState::closed};
    StreamFormat format{};              // the open stream's format (its alternate's); all zero while closed
    std::uint16_t sent_mismatches{};    // completions whose reported size was not the packet TinyUSB loaded for them (#45), since boot; stays at its maximum
    std::uint32_t opens{};              // selections of an alternate with a format since boot
    FrameCount frames_sent{};           // frames in completed packets since the last open, as TinyUSB loaded them (UacStream::on_tx_done); wraps after 24.8 h
    FrameCount frames_queued{};         // frames written to the FIFO since the last open
    FrameCount source_frames{};         // file frames the source supplied since the last open
    std::uint32_t short_packets{};      // completed packets other than one packet of the format while streaming, since boot
    std::uint32_t rejected_chunks{};    // chunks not written: refused by the FIFO or without a free record (frames lost), since boot
    IntervalCount missed_intervals{};   // 1 ms service intervals without a completion while streaming (missed_after_gap), since boot
    MissLocation last_miss{};           // the latest gap that still counts a missed interval, since the last open; all 0 when none
    UsbFrameSpan max_gap{};             // the largest gap between completions as the ISR read them (dealiased_gap), since the last open
    std::uint32_t watchdog_expiries{};  // service watchdog samples while streaming, since boot
    IntervalCount unprimed_intervals{}; // the part of missed_intervals counted unprimed (unprimed_part), since boot
};

// The current or last play on the USB route: what the device confirmed sent.
// Every field restarts whenever PLAY is accepted, on either route: accept_play()
// on the USB route, clear_play() on the codec route, which leaves them all zero
// (no USB play). The first and last indices are 0 until a file frame is
// confirmed. The CRCs are zlib CRC-32 over each channel's samples of the frames
// confirmed sent, as the producer took them from the player: the 24-bit source
// samples, each serialized as 3 bytes little-endian, before any conversion to
// the stream's format. So they do not depend on the format: a 16-bit format's
// truncation and a mono format's dropped right channel leave both CRCs as the
// 24-bit stereo format would have them, and for a mono PCM24 WAV played with
// L = R both equal zlib.crc32(<data chunk>). The missed intervals are the
// play's own: those of the gaps that delayed its file frames, armed or not
// (UacStream::count_play_missed); the stream's count since boot is
// StreamCounters::missed_intervals. Their unprimed part follows the stream's
// unprimed count over the same gaps; the USB? query reports it and the format,
// STATUS does not. With a CUT schedule (player.hpp) the player's frames are
// the engine's output after its cuts: a DROP's frames never reach the
// stream, a ZERO's zeros do, so these "file frames" are the play's frames
// sent (USB? uac_play_sent_frames) and the CRCs cover the zeros too.
struct PlayStatistics
{
    StreamFormat format{};           // the stream's format when the first file frame went out; all zero until then
    FrameCount accept_frame_index{}; // frames_sent when PLAY was accepted
    FrameCount file_frames{};        // file frames confirmed sent
    FrameCount first_frame_index{};  // stream index of the first file frame
    UsbFrameCount first_frame_sof{}; // the extended USB frame in which it went out
    FrameCount last_frame_index{};   // stream index of the last file frame sent so far
    std::uint32_t crc32_left{};
    std::uint32_t crc32_right{};
    IntervalCount missed_intervals{};    // 1 ms service intervals missed while the play had file frames queued
    IntervalCount unprimed_intervals{};  // the part of missed_intervals counted unprimed (the device's); never more than it
    FrameCount first_miss_frame_index{}; // stream index of the first frame sent after the first interval it still counts; 0 when none
    FrameCount pending_frames{};         // file frames queued and not yet confirmed sent
    TickCount progress_tick{};           // arming, or the latest completion since
    bool armed{};                        // the player feeds the stream
    bool interrupted{};                  // the stream closed while armed
    bool stalled{};                      // while armed, a completion came more than the stall timeout after the last one or the arming
    bool miss_unconfirmed{};             // the latest completion's gap added to missed_intervals, and no completion has come since (confirmed_missed())
};

// The play's missed intervals that a later completion has confirmed. A late
// read inflates only the latest gap, by one interval, and the next
// completion tells it from a miss: its gap of 0 takes it back, any other gap
// confirms it. So this is all of missed_intervals once a completion has come
// after the gap that last added to them, one fewer until then. A looped
// PLAYLIST item ends on these (player.cpp), never on a late read the next
// completion takes back.
[[nodiscard]] constexpr IntervalCount confirmed_missed(const PlayStatistics& play) noexcept
{
    return play.miss_unconfirmed ? missed_after_gap(play.missed_intervals, UsbFrameSpan{}) : play.missed_intervals;
}

static_assert(confirmed_missed(PlayStatistics{.missed_intervals = IntervalCount{.value = 1U}, .miss_unconfirmed = true}) == IntervalCount{},
              "a gap of 2 frames is no confirmed miss until the next completion");
static_assert(confirmed_missed(PlayStatistics{.missed_intervals = IntervalCount{.value = 2U}, .miss_unconfirmed = true}) == IntervalCount{.value = 1U},
              "a gap of 3 frames misses at least one interval at once");
static_assert(confirmed_missed(PlayStatistics{.missed_intervals = IntervalCount{.value = 1U}}) == IntervalCount{.value = 1U}, "once a completion has come, the count is confirmed");

// A chunk written to the IN FIFO and not yet confirmed sent.
struct ChunkRecord
{
    FrameCount start{};         // stream index of its first frame
    FrameCount file_frames{};   // its leading frames that carry file audio
    std::uint32_t crc32_left{}; // the play's CRCs once this chunk is sent
    std::uint32_t crc32_right{};
};

// The ISR copies these into rings and out under critical sections: plain
// values whose copies can never run code. A ChunkRecord is four words.
static_assert(std::is_trivially_copyable_v<Completion> && std::is_standard_layout_v<Completion>);
static_assert(std::is_trivially_copyable_v<MissLocation> && std::is_standard_layout_v<MissLocation>);
static_assert(std::is_trivially_copyable_v<StreamCounters> && std::is_standard_layout_v<StreamCounters>);
// The 16-bit mismatch count fills the padding after the state and the
// format, so it takes no more of the DTCM the firmware's stream lives in.
static_assert(offsetof(StreamCounters, opens) == 2U * sizeof(std::uint32_t), "the state, the format and the mismatch count share the first two words");
static_assert(std::is_trivially_copyable_v<PlayStatistics> && std::is_standard_layout_v<PlayStatistics>);
static_assert(std::is_trivially_copyable_v<ChunkRecord> && std::is_standard_layout_v<ChunkRecord>);
static_assert(sizeof(ChunkRecord) == 4U * sizeof(std::uint32_t) && alignof(ChunkRecord) == alignof(std::uint32_t));

// Chunk records, oldest first; a fixed ring of chunk_record_capacity.
class ChunkRecords
{
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0U; }

    [[nodiscard]] bool full() const noexcept { return count_ == chunk_record_capacity; }

    // Appends `record` as the newest. Fails with chunk_records_full, changing
    // nothing, when full().
    [[nodiscard]] m110::Status push(const ChunkRecord& record) noexcept;

    // The oldest record, or none when empty().
    [[nodiscard]] std::optional<ChunkRecord> front() const noexcept;

    // Drops the oldest record; nothing to drop when empty().
    void pop() noexcept;

    // Drops the newest record, for a chunk the FIFO refused; nothing to drop
    // when empty().
    void drop_newest() noexcept;

    void clear() noexcept
    {
        first_ = 0U;
        count_ = 0U;
    }

    // A new play starts: chunks still queued for the last one carry no file
    // frames for it.
    void forget_file_frames() noexcept;

private:
    std::array<ChunkRecord, chunk_record_capacity> records_{};
    std::size_t first_{};
    std::size_t count_{};
};

// The capture stream: counts completed packets, keeps the IN FIFO topped up to
// fifo_target_bytes() of its format, and feeds an armed play's file frames
// into it. Each gap between completions is judged for missed intervals,
// located (last_miss, max_gap) and, by the service watchdog's samples, split
// into the device's and the host's (on_watchdog, unprimed_intervals), for the
// stream and for the play whose file it delayed.
// The stream opens in the format of the alternate the host selected, and every
// byte count follows that format: the packets it writes (pack_frames()), the
// pre-fill and top-up level, the size a completed packet must have, and the
// frames a completion sends. Frames (48 per packet, whatever the format), not
// bytes, are what the frame indices, the records, the missed intervals and
// the play's statistics count. Selecting any alternate while open, another
// format or the same one, closes the stream first: an armed play is
// interrupted, as by alternate 0.
// The FIFO is written only here, by open() (the pre-fill) and on_tx_done()
// (the USB ISR), and never beyond its free space. Each chunk
// written gets a record; a completion retires the records of the chunks it
// finished, so the play statistics cover exactly the frames confirmed sent,
// also when a play stops or the stream closes part way. A completion
// finishes the packet TinyUSB loaded for it from the FIFO, whatever size it
// reports (on_tx_done()), so no record outlives its chunk. Chunks of an earlier
// play still queued when PLAY is accepted (on either route; at most three,
// left by a stall) count for no play: the earlier play's statistics have been
// replaced, and the new play's describe exactly its own frames. If the host
// completes them, their file frames reach the wire after the new play's accept
// index and before its first file frame.
// The stream can open only inside a configuration: between SET_CONFIGURATION
// (configure) and the next bus reset, unplug or SET_CONFIGURATION(0)
// (deconfigure). TinyUSB resets its audio driver on those without an interface
// callback, and its task reaches a bus reset or unplug only after any
// SET_INTERFACE queued before it, which must not reopen a stream that no longer
// exists. Closing, for any reason, interrupts an armed play.
// Caller: the USB interrupt level only (see the file banner).
class UacStream
{
public:
    // The host selected a configuration: the stream starts closed and may open.
    void configure() noexcept
    {
        configured_ = true;
        end_stream();
    }

    // Bus reset, unplug or SET_CONFIGURATION(0): close, and refuse to open until
    // the next configure(). The counters stay readable.
    void deconfigure() noexcept
    {
        configured_ = false;
        end_stream();
    }

    // An alternate with a format selected (offered_format()): close the stream
    // if it was open (interrupting an armed play), restart the per-stream
    // counters in `format`, and pre-fill the FIFO with two packets of silence.
    // Fails, changing nothing, with stream_not_configured outside a
    // configuration, or stream_format_unknown for a format not in
    // stream_formats.
    template <InFifo Fifo> [[nodiscard]] m110::Status open(Fifo& fifo, const StreamFormat& format) noexcept
    {
        if (!configured_)
        {
            return stream_not_configured;
        }

        if (std::ranges::find(stream_formats, format) == stream_formats.end())
        {
            return stream_format_unknown;
        }

        end_stream();
        ++counters_.opens;
        counters_.state = StreamState::priming;
        counters_.format = format;
        counters_.frames_sent = {};
        counters_.frames_queued = {};
        counters_.source_frames = {};
        counters_.last_miss = {};
        counters_.max_gap = {};
        interval_ = {};
        Silence silence;
        top_up(fifo, silence);
        return m110::Status::success();
    }

    // Alternate 0, or TinyUSB leaving the open alternate before it opens the
    // next one. The counters stay readable; the format reads all zero.
    void close() noexcept { end_stream(); }

    // A transfer completed and TinyUSB already loaded the next packet from the
    // FIFO: count it, retire the chunks it finished, then refill from `source`
    // if a play is armed, else with silence. Ignored while closed, so a late
    // completion after a close changes nothing. The completion finishes the
    // packet TinyUSB loaded at the completion before it, which `fifo`'s level
    // tells (confirm_loaded()): one chunk, or none for a zero-length packet.
    // Its frames are sent whatever size the completion reports, since TinyUSB
    // took them from the FIFO either way; a size other than that packet's is
    // a short packet and a mismatch (StreamCounters::sent_mismatches; the
    // ChipIdea driver reports a transfer that ended in error as its expected
    // minus remaining bytes, #45). So a short report never leaves a record
    // behind its chunk, to fill the ring and stop the stream. But the host
    // may not have received that packet: a mismatch while the play has file
    // frames queued leaves its delivery unconfirmed (play_mismatched()).
    template <InFifo Fifo, FrameSource Source> void on_tx_done(Fifo& fifo, Source& source, const Completion& completion) noexcept
    {
        if (counters_.state == StreamState::closed)
        {
            return;
        }

        commit(completion, fifo.queued());
        top_up(fifo, source);
    }

    // The service watchdog took a sample (usb/usb_tinyusb_port.h): the USB ISR
    // found the IN endpoint `transfer`, before handling any completion pending
    // with the expiry. Counted only while streaming, for the gap that the next
    // completion ends (GapSamples): primed proves a poll the host left out;
    // unprimed counts the gap's other missed intervals as unprimed_intervals
    // too (unprimed_part).
    void on_watchdog(InTransfer transfer) noexcept;

    // PLAY on the USB route was accepted. Needs an open stream, else fails with
    // stream_not_open, changing nothing. Restarts the play statistics at the
    // current frames_sent; chunks still queued for an earlier play no longer
    // count for any play.
    [[nodiscard]] m110::Status accept_play() noexcept;

    // PLAY was accepted on the codec route: there is no USB play. Restarts the
    // play statistics at zero whatever the stream's state, closed included;
    // chunks still queued for an earlier play no longer count for any play.
    // arm() is only for a play accept_play() accepted.
    void clear_play() noexcept;

    // From the next chunk on, take file frames from the source; with `start`,
    // from the first chunk that starts at or after its index instead, silence
    // until then. A start on another stream than this one (the stream has
    // reopened since it was planned), or one whose index has passed
    // (index_reached()), is no start: the play takes the next chunk, and its
    // first file frame then shows it started late. Fails with
    // stream_interrupted, leaving the play disarmed and interrupted, when the
    // stream accepted for this play has closed or reopened since. `now` starts
    // the stall clock (progress_tick). While armed, a completion that comes
    // more than `stall_timeout` after the previous one (or the arming) marks
    // the play stalled until the next accept_play(), so a polling gap is seen
    // even when it has ended before anyone looks; the silent chunks before a
    // scheduled start keep the clock running like any other.
    [[nodiscard]] m110::Status arm(TickCount now, TickSpan stall_timeout, std::optional<ScheduledStart> start = std::nullopt) noexcept;

    // Back to silence from the next chunk on. Chunks already queued still go
    // out and still count, and so do the gaps that delay them
    // (count_play_missed).
    void disarm() noexcept { play_.armed = false; }

    [[nodiscard]] StreamCounters counters() const noexcept { return counters_; }

    [[nodiscard]] PlayStatistics play() const noexcept { return play_; }

    // A completion reported a size other than the packet TinyUSB loaded for
    // it while the play had file frames queued (silence before them
    // included): the play's statistics count that packet sent, but the host
    // may not have received it, so the play's delivery is unconfirmed. Until
    // the next accept_play() or clear_play(); a close keeps it, as it keeps
    // the statistics.
    [[nodiscard]] bool play_mismatched() const noexcept { return play_mismatched_; }

private:
    // The latest gap that added to the stream's missed intervals: how many of
    // them it still counts, how many of those are unprimed, and the location
    // before it. A zero gap takes a late read back from it, because a late
    // read inflates the latest gap of two or more frames (the gaps of one
    // frame after it only pass the lateness on); the read was late because
    // its ISR was, so an unprimed interval goes first. Once it counts none,
    // last_miss returns to the location before it.
    struct LatestGap
    {
        IntervalCount counted{};
        IntervalCount unprimed{};
        MissLocation earlier{};
    };

    // The extended frame number and the tick of the stream's last completion,
    // the missed and unprimed intervals counted since the stream opened, the
    // latest gap that missed any, and the watchdog's samples since the last
    // completion.
    struct IntervalTracker
    {
        UsbFrameCount last{};
        TickCount last_tick{};
        IntervalCount stream_missed{};
        IntervalCount stream_unprimed{};
        LatestGap latest_gap{};
        GapSamples samples{};
        bool valid{};
    };

    // The play's CRCs over the file frames queued so far, one per channel.
    struct ChannelCrcs
    {
        Crc32 left{};
        Crc32 right{};
    };

    void end_stream() noexcept;
    void restart_play(FrameCount accept_frame_index) noexcept;
    void commit(const Completion& completion, ByteCount fifo_level) noexcept;
    [[nodiscard]] FrameCount confirm_loaded(ByteCount fifo_level) noexcept;
    void note_mismatch(ByteCount reported, FrameCount confirmed) noexcept;
    void note_progress(TickCount tick) noexcept;
    void track_interval(const Completion& completion) noexcept;
    [[nodiscard]] bool judges_for_play(UsbFrameSpan elapsed) noexcept;
    void count_missed(UsbFrameSpan elapsed, const GapSamples& samples, bool for_play) noexcept;
    void locate_miss(IntervalCount added, IntervalCount unprimed, bool for_play) noexcept;
    void take_back_latest_gap() noexcept;
    void set_stream_unprimed(IntervalCount updated) noexcept;
    void count_play_missed(UsbFrameSpan elapsed, IntervalCount stream_unprimed_before) noexcept;
    void retire_sent_chunks() noexcept;
    void apply(const ChunkRecord& record) noexcept;
    [[nodiscard]] ChannelCrcs crcs_with(FrameCount file_frames) const noexcept;
    void count_queued(FrameCount file_frames, const ChannelCrcs& crcs) noexcept;

    // Room for one more packet of the stream's format, within the target.
    template <InFifo Fifo> [[nodiscard]] bool has_room(const Fifo& fifo) const noexcept
    {
        const ByteCount packet = packet_bytes(counters_.format);
        return !records_.full() && fifo.queued().value + packet.value <= fifo_target_bytes(counters_.format).value && fifo.free_space() >= packet;
    }

    // Writes the chunk just filled, as the stream's format carries it,
    // together with its record, so a chunk is in the FIFO exactly when it has
    // one. Without a free record nothing is written (chunk_records_full); a
    // chunk the FIFO refuses loses its record again (chunk_refused). Either way
    // the chunk is lost, and the counters and the play's CRCs stay as they
    // were, so the statistics remain those of the frames actually sent. The
    // CRCs take the source frames, before the format's conversion.
    template <InFifo Fifo> [[nodiscard]] m110::Status queue_chunk(Fifo& fifo, FrameCount file_frames) noexcept
    {
        const ChannelCrcs crcs = crcs_with(file_frames);
        const auto recorded = records_.push({.start = counters_.frames_queued, .file_frames = file_frames, .crc32_left = crcs.left.value(), .crc32_right = crcs.right.value()});

        if (!recorded.is_ok())
        {
            return recorded;
        }

        const PacketBytes packet = pack_frames(chunk_, counters_.format, wire_);

        if (fifo.write(packet) != packet_bytes(counters_.format))
        {
            records_.drop_newest();
            return chunk_refused;
        }

        count_queued(file_frames, crcs);
        return m110::Status::success();
    }

    // The next chunk from the source while armed and past a scheduled start,
    // else silence. A source never claims more file frames than the chunk
    // holds.
    template <FrameSource Source> [[nodiscard]] FrameCount fill(Source& source) noexcept
    {
        Silence silence;
        const FrameCount supplied = play_.armed && start_reached() ? source.fill(chunk_) : silence.fill(chunk_);
        return std::min(supplied, chunk_frames);
    }

    // Whether the chunk about to be filled, which starts at frames_queued,
    // may carry the play's frames: no start is scheduled, or this chunk is
    // the first at or after it (the schedule is then done with).
    [[nodiscard]] bool start_reached() noexcept
    {
        if (!start_at_.has_value())
        {
            return true;
        }

        if (!index_reached(counters_.frames_queued, *start_at_))
        {
            return false;
        }

        start_at_.reset();
        return true;
    }

    // At most fifo_target_chunks writes, so a FIFO that misreports its level
    // cannot keep the ISR looping.
    template <InFifo Fifo, FrameSource Source> void top_up(Fifo& fifo, Source& source) noexcept
    {
        for (std::uint32_t written = 0U; written < fifo_target_chunks && has_room(fifo); ++written)
        {
            const FrameCount file_frames = fill(source);

            if (!queue_chunk(fifo, file_frames).is_ok())
            {
                ++counters_.rejected_chunks;
                return;
            }
        }
    }

    StreamCounters counters_{};
    PlayStatistics play_{};
    IntervalTracker interval_{};
    ChunkRecords records_{};
    ChannelCrcs queued_crcs_{};            // over every file frame of the play queued so far
    std::uint32_t play_opens_{};           // counters_.opens when the play was accepted
    TickSpan stall_timeout_{};             // the armed play's, from arm()
    std::optional<FrameCount> start_at_{}; // the armed play's scheduled start, until its chunk is filled
    Chunk chunk_{};                        // the source frames; static storage in the firmware instance, never on the ISR stack
    PacketBuffer wire_{};                  // the chunk as the format carries it (pack_frames()); static as chunk_
    bool play_judged_{};                   // the last completion's gap was judged for the play: a zero gap next may take back its late read
    bool configured_{};                    // between configure() and deconfigure()
    bool in_flight_{};                     // the packet TinyUSB loaded at the last completion holds a chunk; false for a zero-length one
    bool play_mismatched_{};               // play_mismatched(); with the three flags above it fills the stream's last word
};

// Volume in 1/256 dB (UAC2 Q8.8 decibels).
struct VolumeDb256
{
    std::int16_t value{};

    constexpr auto operator<=>(const VolumeDb256&) const noexcept = default;
};

// The range offered to the host: -100 dB to 0 dB in 1 dB steps.
inline constexpr VolumeDb256 volume_min{.value = -25600};
inline constexpr VolumeDb256 volume_max{.value = 0};
inline constexpr VolumeDb256 volume_resolution{.value = 256};
inline constexpr std::size_t control_response_capacity = wfg_uac_control_response_max_bytes;

// One class-specific entity request (bRequest, entity, control selector, channel).
struct ControlRequest
{
    std::uint8_t code{};
    std::uint8_t entity{};
    std::uint8_t selector{};
    std::uint8_t channel{};
};

// A GET answer, little-endian as on the wire; the host may read fewer bytes.
struct ControlResponse
{
    std::array<std::uint8_t, control_response_capacity> bytes{};
    ByteCount length{};
};

// What the host programmed into the feature unit. Reported, never applied: the
// stream stays bit-exact whatever the host's level slider says.
struct HostSettings
{
    VolumeDb256 volume{volume_max};
    bool muted{};
};

// Clock source 4 (internal fixed 48 kHz) and feature unit 2 (master mute and
// volume). Caller: the wfg_usb task only.
class UacControls
{
public:
    // GET CUR or RANGE. Clock: sampling frequency (CUR 48000, RANGE {48000}) and
    // validity (CUR 1). Feature unit: mute (CUR) and volume (CUR, RANGE) on the
    // master channel. Anything else fails.
    [[nodiscard]] m110::Result<ControlResponse> get(const ControlRequest& request) const noexcept;

    // SET CUR with exactly the control's payload. The clock accepts only 48000;
    // mute takes 0 or 1; volume must lie within the offered range. A failure
    // changes nothing.
    [[nodiscard]] m110::Status set(const ControlRequest& request, std::span<const std::uint8_t> data) noexcept;

    [[nodiscard]] HostSettings settings() const noexcept { return settings_; }

private:
    HostSettings settings_{};
};

// An address in the image's code, which arm-none-eabi-addr2line resolves to a
// function and source line in the image's ELF. The device's addresses are 32
// bits; a host build keeps the low 32 bits of its own.
struct CodeAddress
{
    std::uint32_t value{};

    constexpr auto operator<=>(const CodeAddress&) const noexcept = default;
};

// A Thumb-2 return address is the instruction after the call with bit 0 set
// (the Thumb state bit). Every call instruction ends there: BL is 4 bytes,
// BLX through a register 2.
inline constexpr std::uint32_t thumb_state_bit = 1U;
inline constexpr std::uint32_t thumb_halfword_bytes = 2U;

// The call that `return_address` returns from: the halfword below the
// instruction it returns to lies inside the call instruction itself, so
// addr2line names the call's own line rather than the line of whatever
// follows it. (On a host, whose calls are 5 bytes or more, it lies inside the
// call as well.)
[[nodiscard]] constexpr CodeAddress call_site(std::uint32_t return_address) noexcept
{
    return {.value = (return_address & ~thumb_state_bit) - thumb_halfword_bytes};
}

// TinyUSB's assertion failures since boot (failed TU_ASSERT checks and its
// direct TU_BREAKPOINT() calls), as its breakpoint hook
// (wfg_usb_count_stack_assert) records them: how many, the call site
// (call_site()) of the first and of the latest, each inside the TinyUSB
// function that called the hook, and the stream frame index
// (StreamCounters::frames_sent, which keeps its value while the stream is
// closed and restarts when it opens) and RTOS tick when the latest failed.
// All 0 until the first. The WFG-LIVE/1 USB? query reports them.
struct StackAsserts
{
    std::uint32_t count{};         // since boot; stays at its maximum once there
    CodeAddress first_pc{};        // the first failure's call site
    CodeAddress last_pc{};         // the latest failure's call site
    FrameCount last_frame_index{}; // frames_sent when the latest failed
    TickCount last_tick{};         // the RTOS tick when the latest failed
};

static_assert(std::is_trivially_copyable_v<StackAsserts> && std::is_standard_layout_v<StackAsserts>);

// The TinyUSB assertion failures recorded so far. Caller: one context at a
// time; the firmware instance records and reads it under
// AnyContextCriticalSection (usb_audio.cpp), since a failure can come in a
// task or in the USB ISR.
class StackAssertLog
{
public:
    // One more failure, at call site `pc`, when the stream had sent
    // `frame_index` frames, at RTOS tick `tick`. The first keeps its call site.
    void record(CodeAddress pc, FrameCount frame_index, TickCount tick) noexcept;

    [[nodiscard]] StackAsserts asserts() const noexcept { return asserts_; }

private:
    StackAsserts asserts_{};
};

// Firmware instance (usb_audio.cpp), driven by the C entry points in
// usb/usb_audio_callbacks.h; its play source is player_consume_from_isr().
// The stream functions belong to the USB interrupt level: the USB ISR, or a
// task holding a critical section that masks it (TaskCriticalSection). The
// player task accepts, arms and disarms a play, or clears it for a PLAY on the
// codec route; the wfg_usb task reads the counters for STATUS.
// host_settings(): the wfg_usb task. stack_asserts(): any context, a
// consistent copy of the TinyUSB assertion failures its breakpoint hook
// recorded.
[[nodiscard]] StreamCounters stream_counters() noexcept;
[[nodiscard]] PlayStatistics play_statistics() noexcept;
[[nodiscard]] bool play_mismatched() noexcept;
[[nodiscard]] m110::Status accept_play() noexcept;
void clear_play() noexcept;
[[nodiscard]] m110::Status arm_play(TickCount now, TickSpan stall_timeout, std::optional<ScheduledStart> start = std::nullopt) noexcept;
void disarm_play() noexcept;
[[nodiscard]] HostSettings host_settings() noexcept;
[[nodiscard]] StackAsserts stack_asserts() noexcept;

} // namespace waveform_generator::usb_audio
