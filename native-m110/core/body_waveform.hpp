#pragma once

#include "core/status.hpp"
#include "core/types.hpp"
#include "core/waveform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// =============================================================================
// body_waveform.hpp - serial-tone body waveform: plan, preamble, encode/decode,
//                     framing, and audio rendering
// =============================================================================
// The structural backbone of a serial-tone body burst. body_block_plan() computes
// the per-mode geometry (info/coded/data/probe counts) that sizes every receiver
// stage; the encode/decode/soft-metric functions build and parse a block; the
// EOM/flush + pack_body_payload functions frame the payload; BodyAudioStreamModulator
// renders symbols to 48 kHz audio. Constants (symbol rate 2400, segment 480, EOM
// 0x4B65A5B2) are here. Teaching walkthrough:
//   core/body-waveform-and-scrambler-explainer.md
// =============================================================================

namespace m110
{

constexpr std::uint32_t body_symbol_rate_baud = 2400U;
constexpr std::uint32_t body_carrier_hz = 1800U;
constexpr std::uint32_t body_audio_sample_rate_hz = 48000U;
constexpr std::size_t body_preamble_acquisition_prefix_symbols = 9U * 32U;
constexpr std::size_t body_preamble_segment_symbols = 15U * 32U;
constexpr std::uint32_t body_eom_word = 0x4B65A5B2U;
constexpr std::size_t body_eom_bits = 32U;
constexpr std::size_t body_flush_bits = 144U;
constexpr std::size_t body_audio_samples_per_symbol = body_audio_sample_rate_hz / body_symbol_rate_baud;
constexpr std::size_t body_audio_shaping_span_symbols = 16U;
constexpr std::size_t body_audio_shaping_taps = 2U * body_audio_shaping_span_symbols * body_audio_samples_per_symbol + 1U;

struct BodyMode
{
    DataRate data_rate{};
    BodyInterleave interleave{};
};

struct BodyDesignators
{
    std::uint8_t d1{};
    std::uint8_t d2{};
};

enum class BodyDesignatorState : std::uint8_t
{
    supported,
    recognized_unsupported,
    invalid
};

struct BodyModeRecognition
{
    BodyDesignatorState state{BodyDesignatorState::invalid};
    BodyMode mode{};
};

struct BodyBlockPlan
{
    BodyMode mode{};
    BodyDesignators designators{};
    std::size_t information_bits{};
    std::size_t coded_bits{};
    std::size_t data_channel_symbols{};
    std::size_t transmitted_symbols{};
    std::uint16_t unknown_symbols_per_probe{};
    std::uint16_t known_symbols_per_probe{};
    std::uint8_t information_bits_per_channel_symbol{};
    // Coded-pair copies emitted per information bit: 1 for 75/600/1200/2400, 2 for
    // the 300 repetition mode, 4 for the 150 repetition mode. 4800 is uncoded and
    // leaves this 0. body_block_soft_metrics sums the repeated copies on receive.
    std::uint8_t fec_pair_repetitions{};
};

struct BodyEncodeScratch
{
    MutableBitSpan coded{};
    MutableBitSpan interleaver_matrix{};
    MutableBitSpan interleaved{};
};

struct BodyEncodeState
{
    // MIL-STD-188-110B routes UNKNOWN DATA, EOM, and FLUSH through one
    // continuous K=7 encoder. Zero is our transmission-start convention;
    // the standard does not specify an initial encoder register value.
    std::uint8_t fec_state{};
};

struct BodyDecodeScratch
{
    std::span<float> interleaved_soft{};
    std::span<float> interleaver_matrix{};
    std::span<float> coded_soft{};
    std::span<float> rate_half_soft{};
    MutableBitSpan survivors{};
};

struct BodyDecodeState
{
    std::uint8_t fec_state{};
};

[[nodiscard]] Result<BodyBlockPlan> body_block_plan(BodyMode mode) noexcept;
[[nodiscard]] Result<BodyDesignators> body_designators(BodyMode mode) noexcept;
[[nodiscard]] BodyModeRecognition recognize_body_designators(std::uint8_t d1, std::uint8_t d2, bool long_preamble) noexcept;

[[nodiscard]] std::size_t body_preamble_segments(BodyInterleave interleave) noexcept;
[[nodiscard]] std::size_t body_preamble_symbols(BodyInterleave interleave) noexcept;
[[nodiscard]] std::uint8_t body_channel_symbol_tribit(std::uint8_t channel_symbol, std::size_t spread_index) noexcept;
[[nodiscard]] std::uint8_t body_75_spread_tribit(std::uint8_t information_dibit, bool exceptional_set, std::size_t spread_index) noexcept;
[[nodiscard]] Status generate_body_preamble(BodyMode mode, MutableBitSpan output) noexcept;
// The last tail_segments segments of the preamble, exactly as
// generate_body_preamble would emit them. A streaming receiver trains on a
// bounded segment-aligned suffix of a long preamble without a buffer for the
// 11,520-symbol whole.
[[nodiscard]] Status generate_body_preamble_tail(BodyMode mode, std::size_t tail_segments, MutableBitSpan output) noexcept;

[[nodiscard]] Status encode_body_block(BitSpan information_bits, const BodyBlockPlan& plan, BodyEncodeScratch scratch, MutableBitSpan transmitted_tribits) noexcept;
[[nodiscard]] Status encode_body_block(BitSpan information_bits, const BodyBlockPlan& plan, BodyEncodeScratch scratch, MutableBitSpan transmitted_tribits, BodyEncodeState& state) noexcept;
[[nodiscard]] Status decode_body_block(IQSampleSpan received_symbols, const BodyBlockPlan& plan, BodyDecodeScratch scratch, MutableBitSpan information_bits) noexcept;
[[nodiscard]] Status decode_body_block(IQSampleSpan received_symbols, const BodyBlockPlan& plan, BodyDecodeScratch scratch, MutableBitSpan information_bits, BodyDecodeState& state) noexcept;
// One block's combined rate-1/2 soft metrics (extract, deinterleave,
// repetition-combine) without the Viterbi pass. The transmit FEC is one
// continuous stream across blocks with flush at the transmission end, so a
// receiver can concatenate these per-block metrics and run a single
// continuous Viterbi over the whole transmission: per-block decoding leaves
// each boundary's trailing bits unterminated and pins the next block to a
// single possibly-wrong state, which measurably concentrates rare-tail
// errors in the ~30 bits straddling every block boundary. Coded rates only.
[[nodiscard]] Status body_block_soft_metrics(IQSampleSpan received_symbols, const BodyBlockPlan& plan, BodyDecodeScratch scratch, std::span<float> rate_half_soft) noexcept;

[[nodiscard]] Result<std::size_t> append_body_eom_and_flush(BitSpan payload, std::size_t block_information_bits, MutableBitSpan framed_bits) noexcept;

// The EOM search on a stream of decoded information bits, for a receiver
// that holds a burst's decisions (the host-side whole-burst decoders; the
// streaming receiver's holdback matcher is its own exact-match path): the
// position of the best match of body_eom_word (MSB-first, as
// append_body_eom_and_flush frames it) and its Hamming distance. The scan
// stops at the first exact match and otherwise keeps the earliest position
// of the smallest distance; a stream too short for one 32-bit window
// reports first_bit = bits.size() and body_eom_bits + 1 errors. This was
// m110_rx_decode's find_eom (WBS 6.16 S6 moved it here).
struct BodyEomMatch
{
    std::size_t first_bit{};
    std::size_t bit_errors{body_eom_bits + 1U};
};

[[nodiscard]] std::size_t body_eom_errors_at(BitSpan bits, std::size_t first) noexcept;
[[nodiscard]] BodyEomMatch find_body_eom(BitSpan bits) noexcept;

// The payload's extent under an EOM tolerance: the bits before an accepted
// EOM (within maximum_bit_errors), or every decoded bit when the stream
// carries none - the EOM and flush are never payload.
[[nodiscard]] inline std::size_t body_payload_bits(BitSpan bits, const BodyEomMatch& eom, std::size_t maximum_bit_errors) noexcept
{
    return eom.bit_errors <= maximum_bit_errors ? eom.first_bit : bits.size();
}

// Packs octets[i] from bits[(first_octet + i) * 8 ...] LSB-first: the first
// decoded bit is bit 0 of the octet, the DTE byte order every rx_decode
// verdict and payload hash used (extract_payload). buffer_too_small when the
// bits do not cover the requested octets.
[[nodiscard]] Status pack_body_payload(BitSpan bits, std::size_t first_octet, MutableBitSpan octets) noexcept;
[[nodiscard]] Status body_tribits_to_iq(BitSpan tribits, MutableIQSampleSpan output) noexcept;
[[nodiscard]] Status body_tribits_to_audio(BitSpan tribits, MutableSampleSpan output, float& carrier_phase_radians) noexcept;

// Stateful, allocation-free pulse shaper for a long transmission rendered in
// codec-sized windows. The caller supplies the output symbols plus up to
// body_audio_shaping_span_symbols of real context on each side. Symbols outside
// that window are treated as zero. Carrier phase is retained across calls.
class BodyAudioStreamModulator
{
  public:
    [[nodiscard]] Status initialize(float carrier_phase_radians = 0.0F) noexcept;
    void reset() noexcept;

    [[nodiscard]] float carrier_phase_radians() const noexcept { return carrier_phase_radians_; }

    [[nodiscard]] Status render_window(BitSpan symbols, std::size_t first_output_symbol, std::size_t output_symbols, MutableSampleSpan output) noexcept;

  private:
    std::array<float, body_audio_shaping_taps> taps_{};
    float amplitude_scale_{};
    float carrier_phase_radians_{};
    bool initialized_{};
};

} // namespace m110
