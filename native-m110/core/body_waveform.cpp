/*
================================================================================
body_waveform.cpp - the serial-tone body waveform (build and parse a burst)
================================================================================

WHAT THIS FILE IS
-----------------
The structural backbone of the serial-tone body: the block plan, preamble
generation, per-block encode/decode, the probe framing, EOM framing, and audio
rendering. It is the integrator that wires the FEC, interleaver, demapper, and
scrambler into an on-air burst - and it computes the BodyBlockPlan that sizes every
receiver stage.

BODY BURST ANATOMY
------------------
  PREAMBLE (N x 480 sym, countdown) | BODY blocks | EOM (32b 0x4B65A5B2) | FLUSH (144b)
  Each block = data frames of [unknown DATA symbols | known PROBE symbols]; most rates
  20/20, 2400/4800 32/16; every short block = 1440 symbols (0.6 s), long = x8.

  INPUT  : (encode) payload bits + BodyBlockPlan; (decode) received symbols + plan.
  OUTPUT : transmitted tribits / audio; or soft metrics / info bits / payload octets.
  DEPENDS: fec, interleaver, demapper, scrambler, synchronization (RRC taps).

KEY FUNCTIONS
-------------
  body_block_plan()          THE per-mode geometry table (read this first).
  encode_body_block()        FEC -> interleave -> map+whiten + insert probes.
  extract_soft_data()        mirror: dewhiten + soft-demap, skip probes.
  body_block_soft_metrics()  extract+deinterleave+repetition-combine (no Viterbi), so
                             the receiver can run ONE continuous Viterbi over the burst.
  append_body_eom_and_flush / find_body_eom / pack_body_payload   framing + payload.
  BodyAudioStreamModulator   RRC pulse-shape + 1800 Hz upconvert (stateful, windowed).

Teaching walkthrough: core/body-waveform-and-scrambler-explainer.md
================================================================================
*/

#include "core/body_waveform.hpp"

#include "core/demapper.hpp"
#include "core/fec.hpp"
#include "core/interleaver.hpp"
#include "core/scrambler.hpp"
#include "core/synchronization.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace m110
{

constexpr float pi = 3.14159265358979323846F;
constexpr float two_pi = 2.0F * pi;

struct DesignatorRow
{
    DataRate rate;
    BodyDesignators short_code;
    BodyDesignators long_code;
};

constexpr std::array<DesignatorRow, 7> designator_rows{{
    {DataRate::bps75, {7U, 5U}, {5U, 5U}},
    {DataRate::bps150, {7U, 4U}, {5U, 4U}},
    {DataRate::bps300, {6U, 7U}, {4U, 7U}},
    {DataRate::bps600, {6U, 6U}, {4U, 6U}},
    {DataRate::bps1200, {6U, 5U}, {4U, 5U}},
    {DataRate::bps2400, {6U, 4U}, {4U, 4U}},
    {DataRate::bps4800, {7U, 6U}, {7U, 6U}},
}};

constexpr std::array<std::array<std::uint8_t, 8>, 8> channel_symbol_patterns{{
    {{0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
    {{0U, 4U, 0U, 4U, 0U, 4U, 0U, 4U}},
    {{0U, 0U, 4U, 4U, 0U, 0U, 4U, 4U}},
    {{0U, 4U, 4U, 0U, 0U, 4U, 4U, 0U}},
    {{0U, 0U, 0U, 0U, 4U, 4U, 4U, 4U}},
    {{0U, 4U, 0U, 4U, 4U, 0U, 4U, 0U}},
    {{0U, 0U, 4U, 4U, 4U, 4U, 0U, 0U}},
    {{0U, 4U, 4U, 0U, 4U, 0U, 0U, 4U}},
}};

constexpr std::array<std::uint8_t, 9> preamble_prefix{0U, 1U, 3U, 0U, 1U, 3U, 1U, 2U, 0U};

static const DesignatorRow* find_designator_row(DataRate rate) noexcept
{
    for (const auto& row : designator_rows)
    {
        if (row.rate == rate)
        {
            return &row;
        }
    }

    return nullptr;
}

static std::uint8_t mapped_tribit(std::uint8_t value, std::uint8_t width) noexcept
{
    if (width == 1U)
    {
        return modified_gray_decode(value, width);
    }

    if (width == 2U)
    {
        return static_cast<std::uint8_t>(modified_gray_decode(value, width) << 1U);
    }

    return modified_gray_decode(value, width);
}

static std::uint8_t bit_group(BitSpan bits, std::size_t offset, std::uint8_t width) noexcept
{
    std::uint8_t value{};

    for (std::uint8_t bit = 0U; bit < width; ++bit)
    {
        value = static_cast<std::uint8_t>((value << 1U) | (bits[offset + bit] & 1U));
    }

    return value;
}

static Status encode_coded_bits(BitSpan information_bits, const BodyBlockPlan& plan, MutableBitSpan coded, BodyEncodeState& state) noexcept
{
    if (coded.size() < plan.coded_bits)
    {
        return {StatusCode::buffer_too_small, "body coded-bit buffer too small"};
    }

    if (plan.mode.data_rate == DataRate::bps4800)
    {
        for (std::size_t index = 0U; index < information_bits.size(); ++index)
        {
            coded[index] = information_bits[index] & 1U;
        }

        return Status::success();
    }

    ConvolutionalEncoderK7 encoder(state.fec_state);
    std::size_t written{};

    for (const auto bit : information_bits)
    {
        const auto pair = encoder.push(bit);

        for (std::uint8_t repeat = 0U; repeat < plan.fec_pair_repetitions; ++repeat)
        {
            coded[written++] = pair.t1;
            coded[written++] = pair.t2;
        }
    }

    if (written != plan.coded_bits)
    {
        return {StatusCode::internal_error, "body FEC output length does not match block plan"};
    }

    state.fec_state = encoder.state();
    return Status::success();
}

// -----------------------------------------------------------------------------
// extract_soft_data  (receive mirror of encode_body_block's mapping)
// -----------------------------------------------------------------------------
// 50K view: Turn one received block's symbols into interleaved-order soft bits.
// Detailed view: For each frame, derotate the unknown data symbols by the data-
//   randomizer tribit (dewhiten) and soft-demap them (psk8_soft_demapper), and SKIP
//   the known probe symbols (they were the equalizer's, not payload). 75 bps
//   correlates against the 4 candidate dibits over 32 chips.
// 5th-grade view: Un-shuffle the secret voice, read each data symbol's confidence,
//   and ignore the checkpoint phrases.
// -----------------------------------------------------------------------------
static Status extract_soft_data(IQSampleSpan received, const BodyBlockPlan& plan, std::span<float> soft_bits) noexcept
{
    if (received.size() != plan.transmitted_symbols || soft_bits.size() < plan.coded_bits)
    {
        return {StatusCode::invalid_argument, "body receive buffers do not match block plan"};
    }

    BodyDataRandomizer randomizer;
    std::size_t received_index{};
    std::size_t soft_index{};

    if (plan.mode.data_rate == DataRate::bps75)
    {
        for (std::size_t dibit = 0U; dibit < plan.data_channel_symbols; ++dibit)
        {
            std::array<float, 4> candidate_costs{};
            const bool exceptional_set = dibit + 1U == plan.data_channel_symbols;

            for (std::uint8_t candidate = 0U; candidate < 4U; ++candidate)
            {
                auto candidate_randomizer = randomizer;
                float cost{};

                for (std::size_t spread = 0U; spread < 32U; ++spread)
                {
                    const auto expected = static_cast<std::uint8_t>((body_75_spread_tribit(candidate, exceptional_set, spread) + candidate_randomizer.next_tribit()) & 7U);
                    const auto reference = psk8_symbol(expected);
                    const auto error = received[received_index + spread] - reference;
                    cost += std::norm(error);
                }

                candidate_costs[candidate] = cost;
            }

            for (std::uint8_t bit = 0U; bit < 2U; ++bit)
            {
                float zero_cost = candidate_costs[0];
                float one_cost = candidate_costs[0];
                bool zero_set = false;
                bool one_set = false;

                for (std::uint8_t candidate = 0U; candidate < 4U; ++candidate)
                {
                    const bool value = ((candidate >> (1U - bit)) & 1U) != 0U;
                    auto& target = value ? one_cost : zero_cost;
                    auto& set = value ? one_set : zero_set;

                    if (!set || candidate_costs[candidate] < target)
                    {
                        target = candidate_costs[candidate];
                        set = true;
                    }
                }

                soft_bits[soft_index++] = one_cost - zero_cost;
            }

            for (std::size_t spread = 0U; spread < 32U; ++spread)
            {
                static_cast<void>(randomizer.next_tribit());
            }

            received_index += 32U;
        }

        return Status::success();
    }

    const auto frames = plan.data_channel_symbols / plan.unknown_symbols_per_probe;

    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        for (std::size_t symbol = 0U; symbol < plan.unknown_symbols_per_probe; ++symbol)
        {
            const auto derotated = received[received_index++] * std::conj(psk8_symbol(randomizer.next_tribit()));
            const auto status = psk8_soft_demapper(derotated, plan.information_bits_per_channel_symbol, soft_bits.subspan(soft_index, plan.information_bits_per_channel_symbol));

            if (!status.is_ok())
            {
                return status;
            }

            soft_index += plan.information_bits_per_channel_symbol;
        }

        for (std::size_t symbol = 0U; symbol < plan.known_symbols_per_probe; ++symbol)
        {
            static_cast<void>(randomizer.next_tribit());
            ++received_index;
        }
    }

    return soft_index == plan.coded_bits ? Status::success() : Status{StatusCode::internal_error, "body soft extraction count mismatch"};
}

Result<BodyDesignators> body_designators(BodyMode mode) noexcept
{
    const auto* row = find_designator_row(mode.data_rate);

    if (row == nullptr || (mode.data_rate == DataRate::bps4800 && mode.interleave != BodyInterleave::zero))
    {
        return Status{StatusCode::invalid_configuration, "unsupported body mode"};
    }

    return mode.interleave == BodyInterleave::long_block ? row->long_code : row->short_code;
}

BodyModeRecognition recognize_body_designators(std::uint8_t d1, std::uint8_t d2, bool long_preamble) noexcept
{
    if ((d1 == 5U && (d2 == 6U || d2 == 7U)) || (d1 == 7U && d2 == 7U))
    {
        return {BodyDesignatorState::recognized_unsupported, {}};
    }

    for (const auto& row : designator_rows)
    {
        const auto expected = long_preamble ? row.long_code : row.short_code;

        if (expected.d1 == d1 && expected.d2 == d2)
        {
            const auto interleave = row.rate == DataRate::bps4800 ? BodyInterleave::zero : long_preamble ? BodyInterleave::long_block : BodyInterleave::short_block;
            return {BodyDesignatorState::supported, {row.rate, interleave}};
        }
    }

    return {BodyDesignatorState::invalid, {}};
}

// -----------------------------------------------------------------------------
// body_block_plan  (THE per-mode geometry - read this first)
// -----------------------------------------------------------------------------
// 50K view: Fill the BodyBlockPlan that sizes EVERY receiver stage: information/
//   coded bits, data/probe symbols per frame, bits per symbol, FEC repetitions,
//   transmitted symbols per block.
// Detailed view: Short block = 1440 transmitted symbols (0.6 s); long interleave is
//   x8. Frames are 20 data / 20 probe for 150..1200, 32 data / 16 probe for
//   2400/4800; 75 bps is Walsh-spread dibits. FEC: rate 1/2, x2 (300), x4 (150),
//   uncoded (4800).
// Mode families: 150 (x4) and 300 (x2) are the REPETITION modes - rate 1/2 then
//   repeated coded copies that body_block_soft_metrics sums (repetition-combine);
//   they are the modes the receiver's optional exact log-MAP SISO lever helps (that
//   lever is opt-in and default-off, shipped defaults unchanged - see
//   docs/process/M110-receiver-improvement-findings.md; not detailed here).
//   600/1200/2400 are plain rate 1/2; 75 is Walsh orthogonal spreading and is
//   turbo/SISO-unsupported; 4800 is uncoded.
// 5th-grade view: Fill in the recipe card for this mode - how many of each kind of
//   symbol - so every other part knows what to expect.
// -----------------------------------------------------------------------------
Result<BodyBlockPlan> body_block_plan(BodyMode mode) noexcept
{
    const auto designators = body_designators(mode);

    if (!designators)
    {
        return designators.status();
    }

    const bool is_long = mode.interleave == BodyInterleave::long_block;
    const auto multiplier = is_long ? 8U : 1U;
    BodyBlockPlan plan{mode, designators.value()};
    plan.transmitted_symbols = 1440U * multiplier;

    switch (mode.data_rate)
    {
    case DataRate::bps75:
        plan.information_bits = 45U * multiplier;
        plan.coded_bits = 90U * multiplier;
        plan.data_channel_symbols = 45U * multiplier;
        plan.information_bits_per_channel_symbol = 2U;
        plan.fec_pair_repetitions = 1U;
        break;

    case DataRate::bps150:
        plan.information_bits = 90U * multiplier;
        plan.coded_bits = 720U * multiplier;
        plan.data_channel_symbols = plan.coded_bits;
        plan.unknown_symbols_per_probe = 20U;
        plan.known_symbols_per_probe = 20U;
        plan.information_bits_per_channel_symbol = 1U;
        plan.fec_pair_repetitions = 4U;
        break;

    case DataRate::bps300:
        plan.information_bits = 180U * multiplier;
        plan.coded_bits = 720U * multiplier;
        plan.data_channel_symbols = plan.coded_bits;
        plan.unknown_symbols_per_probe = 20U;
        plan.known_symbols_per_probe = 20U;
        plan.information_bits_per_channel_symbol = 1U;
        plan.fec_pair_repetitions = 2U;
        break;

    case DataRate::bps600:
        plan.information_bits = 360U * multiplier;
        plan.coded_bits = 720U * multiplier;
        plan.data_channel_symbols = plan.coded_bits;
        plan.unknown_symbols_per_probe = 20U;
        plan.known_symbols_per_probe = 20U;
        plan.information_bits_per_channel_symbol = 1U;
        plan.fec_pair_repetitions = 1U;
        break;

    case DataRate::bps1200:
        plan.information_bits = 720U * multiplier;
        plan.coded_bits = 1440U * multiplier;
        plan.data_channel_symbols = plan.coded_bits / 2U;
        plan.unknown_symbols_per_probe = 20U;
        plan.known_symbols_per_probe = 20U;
        plan.information_bits_per_channel_symbol = 2U;
        plan.fec_pair_repetitions = 1U;
        break;

    case DataRate::bps2400:
        plan.information_bits = 1440U * multiplier;
        plan.coded_bits = 2880U * multiplier;
        plan.data_channel_symbols = plan.coded_bits / 3U;
        plan.unknown_symbols_per_probe = 32U;
        plan.known_symbols_per_probe = 16U;
        plan.information_bits_per_channel_symbol = 3U;
        plan.fec_pair_repetitions = 1U;
        break;

    case DataRate::bps4800:
        plan.information_bits = 2880U;
        plan.coded_bits = 2880U;
        plan.data_channel_symbols = plan.coded_bits / 3U;
        plan.unknown_symbols_per_probe = 32U;
        plan.known_symbols_per_probe = 16U;
        plan.information_bits_per_channel_symbol = 3U;
        break;

    default:
        return Status{StatusCode::invalid_configuration, "rate is not a serial-tone body mode"};
    }

    return plan;
}

std::size_t body_preamble_segments(BodyInterleave interleave) noexcept { return interleave == BodyInterleave::long_block ? 24U : 3U; }

std::size_t body_preamble_symbols(BodyInterleave interleave) noexcept { return body_preamble_segments(interleave) * 15U * 32U; }

std::uint8_t body_channel_symbol_tribit(std::uint8_t channel_symbol, std::size_t spread_index) noexcept
{
    const auto& pattern = channel_symbol_patterns[channel_symbol & 7U];
    return pattern[spread_index % pattern.size()];
}

std::uint8_t body_75_spread_tribit(std::uint8_t information_dibit, bool exceptional_set, std::size_t spread_index) noexcept
{
    // MIL-STD-188-110B Table XIII first maps the raw information dibit to a
    // modified-Gray channel symbol. Tables XIVa/XIVb are indexed by that
    // channel symbol, not directly by the two interleaver-output bits.
    const auto channel_symbol = modified_gray_decode(static_cast<std::uint8_t>(information_dibit & 3U), 2U);
    const auto pattern_index = static_cast<std::uint8_t>(channel_symbol + (exceptional_set ? 4U : 0U));
    return body_channel_symbol_tribit(pattern_index, spread_index);
}

static void emit_body_preamble_segment(const BodyDesignators& designators, std::uint8_t countdown, SyncRandomizer& randomizer, MutableBitSpan output) noexcept
{
    std::array<std::uint8_t, 15> channel_symbols{};

    for (std::size_t index = 0U; index < preamble_prefix.size(); ++index)
    {
        channel_symbols[index] = preamble_prefix[index];
    }

    channel_symbols[9] = designators.d1;
    channel_symbols[10] = designators.d2;
    channel_symbols[11] = static_cast<std::uint8_t>(4U | ((countdown >> 4U) & 3U));
    channel_symbols[12] = static_cast<std::uint8_t>(4U | ((countdown >> 2U) & 3U));
    channel_symbols[13] = static_cast<std::uint8_t>(4U | (countdown & 3U));
    channel_symbols[14] = 0U;
    std::size_t written{};

    for (const auto channel_symbol : channel_symbols)
    {
        for (std::size_t spread = 0U; spread < 32U; ++spread)
        {
            output[written++] = static_cast<std::uint8_t>((body_channel_symbol_tribit(channel_symbol, spread) + randomizer.next_tribit()) & 7U);
        }
    }
}

Status generate_body_preamble(BodyMode mode, MutableBitSpan output) noexcept
{
    return generate_body_preamble_tail(mode, body_preamble_segments(mode.interleave), output);
}

Status generate_body_preamble_tail(BodyMode mode, std::size_t tail_segments, MutableBitSpan output) noexcept
{
    const auto plan = body_block_plan(mode);
    const auto segments = body_preamble_segments(mode.interleave);

    if (!plan)
    {
        return plan.status();
    }

    if (tail_segments == 0U || tail_segments > segments)
    {
        return {StatusCode::invalid_argument, "body preamble tail segment count is invalid"};
    }

    if (output.size() < tail_segments * body_preamble_segment_symbols)
    {
        return {StatusCode::buffer_too_small, "body preamble output too small"};
    }

    // Every segment consumes 480 scrambler tribits, a whole multiple of the
    // 32-entry sync sequence, so the scramble alignment at any segment
    // boundary equals the alignment at the preamble start and a fresh
    // randomizer generates any segment-aligned suffix exactly.
    SyncRandomizer randomizer;
    std::size_t written{};

    for (std::size_t segment = segments - tail_segments; segment < segments; ++segment)
    {
        const auto countdown = static_cast<std::uint8_t>((segments - segment - 1U) & 0x1FU);
        emit_body_preamble_segment(plan.value().designators, countdown, randomizer, output.subspan(written, body_preamble_segment_symbols));
        written += body_preamble_segment_symbols;
    }

    return Status::success();
}

Status encode_body_block(BitSpan information_bits, const BodyBlockPlan& plan, BodyEncodeScratch scratch, MutableBitSpan transmitted_tribits) noexcept
{
    BodyEncodeState state{};
    return encode_body_block(information_bits, plan, scratch, transmitted_tribits, state);
}

// -----------------------------------------------------------------------------
// encode_body_block  (transmit one body block)
// -----------------------------------------------------------------------------
// 50K view: Build the transmitted symbols for one block: FEC -> interleave ->
//   map+whiten, inserting the known probe symbols each frame.
// Detailed view: encode_coded_bits (rate 1/2 x reps, threading BodyEncodeState) ->
//   body_interleave -> for each frame emit the unknown data symbols (modified-Gray
//   mapped + data-randomizer whitening) then the known probe symbols; the last two
//   frames' probes carry D2/D1 for mid-body mode re-ID. 75 bps takes the Walsh path.
//   Exact mirror of extract_soft_data.
// 5th-grade view: Encode the words, shuffle them, add the secret voice-shuffle, and
//   drop in the memorized checkpoint phrases.
// -----------------------------------------------------------------------------
Status encode_body_block(BitSpan information_bits, const BodyBlockPlan& plan, BodyEncodeScratch scratch, MutableBitSpan transmitted_tribits, BodyEncodeState& state) noexcept
{
    if (information_bits.size() != plan.information_bits || scratch.coded.size() < plan.coded_bits || scratch.interleaved.size() < plan.coded_bits ||
        transmitted_tribits.size() < plan.transmitted_symbols)
    {
        return {StatusCode::invalid_argument, "body encode buffers do not match block plan"};
    }

    const auto encode_status = encode_coded_bits(information_bits, plan, scratch.coded, state);

    if (!encode_status.is_ok())
    {
        return encode_status;
    }

    const WaveformConfig config{WaveformFamily::serial_tone, plan.mode.data_rate, plan.mode.interleave, ChannelSidebandPolicy::detect};
    const auto interleaver = interleaver_for(config);

    if (!interleaver)
    {
        return interleaver.status();
    }

    const auto interleave_status = body_interleave(scratch.coded.first(plan.coded_bits), scratch.interleaver_matrix, scratch.interleaved.first(plan.coded_bits), interleaver.value());

    if (!interleave_status.is_ok())
    {
        return interleave_status;
    }

    BodyDataRandomizer randomizer;
    std::size_t bit_index{};
    std::size_t written{};

    if (plan.mode.data_rate == DataRate::bps75)
    {
        for (std::size_t dibit = 0U; dibit < plan.data_channel_symbols; ++dibit)
        {
            const auto value = bit_group(scratch.interleaved, bit_index, 2U);
            const bool exceptional_set = dibit + 1U == plan.data_channel_symbols;

            for (std::size_t spread = 0U; spread < 32U; ++spread)
            {
                transmitted_tribits[written++] = static_cast<std::uint8_t>((body_75_spread_tribit(value, exceptional_set, spread) + randomizer.next_tribit()) & 7U);
            }

            bit_index += 2U;
        }

        return written == plan.transmitted_symbols ? Status::success() : Status{StatusCode::internal_error, "75-bps transmit count mismatch"};
    }

    const auto frames = plan.data_channel_symbols / plan.unknown_symbols_per_probe;

    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        for (std::size_t symbol = 0U; symbol < plan.unknown_symbols_per_probe; ++symbol)
        {
            const auto value = bit_group(scratch.interleaved, bit_index, plan.information_bits_per_channel_symbol);
            const auto tribit = mapped_tribit(value, plan.information_bits_per_channel_symbol);
            transmitted_tribits[written++] = static_cast<std::uint8_t>((tribit + randomizer.next_tribit()) & 7U);
            bit_index += plan.information_bits_per_channel_symbol;
        }

        const auto known_value = frame + 2U == frames ? plan.designators.d1 : frame + 1U == frames ? plan.designators.d2 : 0U;

        for (std::size_t symbol = 0U; symbol < plan.known_symbols_per_probe; ++symbol)
        {
            const auto probe_tribit = plan.known_symbols_per_probe == 20U && symbol >= 16U ? 0U : body_channel_symbol_tribit(known_value, symbol);
            transmitted_tribits[written++] = static_cast<std::uint8_t>((probe_tribit + randomizer.next_tribit()) & 7U);
        }
    }

    return bit_index == plan.coded_bits && written == plan.transmitted_symbols ? Status::success() : Status{StatusCode::internal_error, "body transmit count mismatch"};
}

Status decode_body_block(IQSampleSpan received_symbols, const BodyBlockPlan& plan, BodyDecodeScratch scratch, MutableBitSpan information_bits) noexcept
{
    BodyDecodeState state{};
    return decode_body_block(received_symbols, plan, scratch, information_bits, state);
}

Status body_block_soft_metrics(IQSampleSpan received_symbols, const BodyBlockPlan& plan, BodyDecodeScratch scratch, std::span<float> rate_half_soft) noexcept
{
    if (scratch.interleaved_soft.size() < plan.coded_bits || scratch.coded_soft.size() < plan.coded_bits || scratch.interleaver_matrix.size() < plan.coded_bits ||
        received_symbols.size() != plan.transmitted_symbols || plan.mode.data_rate == DataRate::bps4800 || rate_half_soft.size() < plan.information_bits * 2U)
    {
        return {StatusCode::invalid_argument, "body soft-metric buffers do not match block plan"};
    }

    const auto extract_status = extract_soft_data(received_symbols, plan, scratch.interleaved_soft);

    if (!extract_status.is_ok())
    {
        return extract_status;
    }

    const WaveformConfig config{WaveformFamily::serial_tone, plan.mode.data_rate, plan.mode.interleave, ChannelSidebandPolicy::detect};
    const auto interleaver = interleaver_for(config);

    if (!interleaver)
    {
        return interleaver.status();
    }

    const auto deinterleave_status =
        body_deinterleave_soft(scratch.interleaved_soft.first(plan.coded_bits), scratch.interleaver_matrix, scratch.coded_soft.first(plan.coded_bits), interleaver.value());

    if (!deinterleave_status.is_ok())
    {
        return deinterleave_status;
    }

    for (std::size_t information_index = 0U; information_index < plan.information_bits; ++information_index)
    {
        float t1{};
        float t2{};
        const auto base = information_index * 2U * plan.fec_pair_repetitions;

        for (std::size_t repeat = 0U; repeat < plan.fec_pair_repetitions; ++repeat)
        {
            t1 += scratch.coded_soft[base + repeat * 2U];
            t2 += scratch.coded_soft[base + repeat * 2U + 1U];
        }

        rate_half_soft[information_index * 2U] = t1;
        rate_half_soft[information_index * 2U + 1U] = t2;
    }

    return Status::success();
}

Status decode_body_block(IQSampleSpan received_symbols, const BodyBlockPlan& plan, BodyDecodeScratch scratch, MutableBitSpan information_bits, BodyDecodeState& state) noexcept
{
    if (information_bits.size() != plan.information_bits || scratch.interleaved_soft.size() < plan.coded_bits || scratch.coded_soft.size() < plan.coded_bits ||
        scratch.interleaver_matrix.size() < plan.coded_bits || received_symbols.size() != plan.transmitted_symbols)
    {
        return {StatusCode::invalid_argument, "body decode buffers do not match block plan"};
    }

    if (plan.mode.data_rate == DataRate::bps4800)
    {
        const auto extract_status = extract_soft_data(received_symbols, plan, scratch.interleaved_soft);

        if (!extract_status.is_ok())
        {
            return extract_status;
        }

        const WaveformConfig config{WaveformFamily::serial_tone, plan.mode.data_rate, plan.mode.interleave, ChannelSidebandPolicy::detect};
        const auto interleaver = interleaver_for(config);

        if (!interleaver)
        {
            return interleaver.status();
        }

        const auto deinterleave_status =
            body_deinterleave_soft(scratch.interleaved_soft.first(plan.coded_bits), scratch.interleaver_matrix, scratch.coded_soft.first(plan.coded_bits), interleaver.value());

        if (!deinterleave_status.is_ok())
        {
            return deinterleave_status;
        }

        for (std::size_t index = 0U; index < plan.information_bits; ++index)
        {
            information_bits[index] = scratch.coded_soft[index] < 0.0F ? 1U : 0U;
        }

        return Status::success();
    }

    if (scratch.rate_half_soft.size() < plan.information_bits * 2U || scratch.survivors.size() < plan.information_bits * 64U)
    {
        return {StatusCode::buffer_too_small, "body Viterbi scratch too small"};
    }

    const auto metrics_status = body_block_soft_metrics(received_symbols, plan, scratch, scratch.rate_half_soft.first(plan.information_bits * 2U));

    if (!metrics_status.is_ok())
    {
        return metrics_status;
    }

    const auto decode_status =
        viterbi_decode_rate_half(scratch.rate_half_soft.first(plan.information_bits * 2U), information_bits, scratch.survivors.first(plan.information_bits * 64U), state.fec_state);

    if (!decode_status.is_ok())
    {
        return decode_status;
    }

    ConvolutionalEncoderK7 encoder(state.fec_state);

    for (const auto bit : information_bits)
    {
        static_cast<void>(encoder.push(bit));
    }

    state.fec_state = encoder.state();
    return Status::success();
}

// -----------------------------------------------------------------------------
// append_body_eom_and_flush
// -----------------------------------------------------------------------------
// 50K view: Frame the payload with the end-of-message marker and trellis flush.
// Detailed view: payload bits, then the 32-bit EOM word 0x4B65A5B2 (MSB-first),
//   then 144 flush bits, zero-padded up to a whole block_information_bits multiple.
//   find_body_eom reverses it (minimum Hamming distance to the EOM word).
// 5th-grade view: Add "THE END" and some cleanup marks, padded to a whole page.
// -----------------------------------------------------------------------------
Result<std::size_t> append_body_eom_and_flush(BitSpan payload, std::size_t block_information_bits, MutableBitSpan framed_bits) noexcept
{
    if (block_information_bits == 0U)
    {
        return Status{StatusCode::invalid_argument, "body frame block size is zero"};
    }

    const auto unpadded = payload.size() + body_eom_bits + body_flush_bits;
    const auto required = ((unpadded + block_information_bits - 1U) / block_information_bits) * block_information_bits;

    if (framed_bits.size() < required)
    {
        return Status{StatusCode::buffer_too_small, "body frame output too small"};
    }

    std::size_t written{};

    for (const auto bit : payload)
    {
        framed_bits[written++] = bit & 1U;
    }

    for (std::size_t bit = 0U; bit < body_eom_bits; ++bit)
    {
        framed_bits[written++] = static_cast<std::uint8_t>((body_eom_word >> (body_eom_bits - bit - 1U)) & 1U);
    }

    while (written < required)
    {
        framed_bits[written++] = 0U;
    }

    return written;
}

Status body_tribits_to_iq(BitSpan tribits, MutableIQSampleSpan output) noexcept
{
    if (output.size() < tribits.size())
    {
        return {StatusCode::buffer_too_small, "body IQ output too small"};
    }

    for (std::size_t index = 0U; index < tribits.size(); ++index)
    {
        output[index] = psk8_symbol(tribits[index]);
    }

    return Status::success();
}

Status body_tribits_to_audio(BitSpan tribits, MutableSampleSpan output, float& carrier_phase_radians) noexcept
{
    BodyAudioStreamModulator modulator;
    auto status = modulator.initialize(carrier_phase_radians);

    if (!status.is_ok())
    {
        return status;
    }

    status = modulator.render_window(tribits, 0U, tribits.size(), output);
    carrier_phase_radians = modulator.carrier_phase_radians();
    return status;
}

Status BodyAudioStreamModulator::initialize(float carrier_phase_radians) noexcept
{
    constexpr float shaping_rolloff = 0.25F;
    const auto status = make_root_raised_cosine_taps(body_audio_samples_per_symbol, shaping_rolloff, taps_);

    if (!status.is_ok())
    {
        return status;
    }

    float worst_polyphase_sum = 0.0F;

    for (std::size_t phase = 0U; phase < body_audio_samples_per_symbol; ++phase)
    {
        float phase_sum = 0.0F;

        for (std::size_t tap = phase; tap < taps_.size(); tap += body_audio_samples_per_symbol)
        {
            phase_sum += std::fabs(taps_[tap]);
        }

        worst_polyphase_sum = std::max(worst_polyphase_sum, phase_sum);
    }

    amplitude_scale_ = 0.98F / worst_polyphase_sum;
    carrier_phase_radians_ = carrier_phase_radians;
    initialized_ = true;
    return Status::success();
}

void BodyAudioStreamModulator::reset() noexcept { carrier_phase_radians_ = 0.0F; }

// -----------------------------------------------------------------------------
// BodyAudioStreamModulator::render_window  (symbols -> 48 kHz audio)
// -----------------------------------------------------------------------------
// 50K view: Render a window of tribit symbols to real passband audio, stateful
//   across windows (retains carrier phase).
// Detailed view: Convolve the 8-PSK symbols with the root-raised-cosine taps
//   (rolloff 0.25, 20 samples/symbol), scale by amplitude_scale_ (0.98 of the worst
//   polyphase peak - headroom), and upconvert to the 1800 Hz carrier. Symbols outside
//   the window are treated as zero; carrier phase carries to the next call.
// 5th-grade view: Smooth each symbol into a pulse and ride it up onto the 1800 Hz
//   tone, remembering where the wave was so windows join seamlessly.
// -----------------------------------------------------------------------------
Status BodyAudioStreamModulator::render_window(BitSpan symbols, std::size_t first_output_symbol, std::size_t output_symbols, MutableSampleSpan output) noexcept
{
    if (!initialized_)
    {
        return {StatusCode::invalid_argument, "body audio stream modulator is not initialized"};
    }

    if (first_output_symbol > symbols.size() || output_symbols > symbols.size() - first_output_symbol)
    {
        return {StatusCode::invalid_argument, "body audio output range is outside the symbol window"};
    }

    const auto required_samples = output_symbols * body_audio_samples_per_symbol;

    if (output.size() < required_samples)
    {
        return {StatusCode::buffer_too_small, "body audio output too small"};
    }

    constexpr float carrier_step = two_pi * static_cast<float>(body_carrier_hz) / static_cast<float>(body_audio_sample_rate_hz);
    const auto first_output_sample = first_output_symbol * body_audio_samples_per_symbol;

    for (std::size_t output_index = 0U; output_index < required_samples; ++output_index)
    {
        IQSample baseband{};
        const auto window_output_index = first_output_sample + output_index;
        const auto center = static_cast<std::ptrdiff_t>(window_output_index) - static_cast<std::ptrdiff_t>(body_audio_shaping_span_symbols * body_audio_samples_per_symbol);

        for (std::ptrdiff_t symbol = (center + static_cast<std::ptrdiff_t>(body_audio_samples_per_symbol) - 1) / static_cast<std::ptrdiff_t>(body_audio_samples_per_symbol);
             symbol * static_cast<std::ptrdiff_t>(body_audio_samples_per_symbol) <= center + static_cast<std::ptrdiff_t>(2U * body_audio_shaping_span_symbols * body_audio_samples_per_symbol);
             ++symbol)
        {
            if (symbol < 0 || static_cast<std::size_t>(symbol) >= symbols.size())
            {
                continue;
            }

            const auto tap = static_cast<std::ptrdiff_t>(window_output_index) - symbol * static_cast<std::ptrdiff_t>(body_audio_samples_per_symbol) +
                             static_cast<std::ptrdiff_t>(body_audio_shaping_span_symbols * body_audio_samples_per_symbol);

            if (tap >= 0 && static_cast<std::size_t>(tap) < taps_.size())
            {
                baseband += psk8_symbol(symbols[static_cast<std::size_t>(symbol)]) * taps_[static_cast<std::size_t>(tap)];
            }
        }

        output[output_index] = amplitude_scale_ * std::real(baseband * std::polar(1.0F, carrier_phase_radians_));
        carrier_phase_radians_ += carrier_step;

        if (carrier_phase_radians_ >= two_pi)
        {
            carrier_phase_radians_ -= two_pi;
        }
    }

    return Status::success();
}

std::size_t body_eom_errors_at(BitSpan bits, std::size_t first) noexcept
{
    if (first + body_eom_bits > bits.size())
    {
        return body_eom_bits + 1U;
    }

    std::size_t errors{};

    for (std::size_t bit = 0U; bit < body_eom_bits; ++bit)
    {
        if ((bits[first + bit] & 1U) != ((body_eom_word >> (body_eom_bits - bit - 1U)) & 1U))
        {
            ++errors;
        }
    }

    return errors;
}

BodyEomMatch find_body_eom(BitSpan bits) noexcept
{
    BodyEomMatch best{bits.size(), body_eom_bits + 1U};

    for (std::size_t first = 0U; first + body_eom_bits <= bits.size(); ++first)
    {
        const auto errors = body_eom_errors_at(bits, first);

        if (errors < best.bit_errors)
        {
            best = {first, errors};

            if (errors == 0U)
            {
                break;
            }
        }
    }

    return best;
}

Status pack_body_payload(BitSpan bits, std::size_t first_octet, MutableBitSpan octets) noexcept
{
    const auto whole_octets = bits.size() / 8U;

    if (first_octet > whole_octets || octets.size() > whole_octets - first_octet)
    {
        return {StatusCode::buffer_too_small, "payload bits do not cover the requested octets"};
    }

    for (std::size_t index = 0U; index < octets.size(); ++index)
    {
        std::uint8_t octet{};

        for (std::size_t bit = 0U; bit < 8U; ++bit)
        {
            octet = static_cast<std::uint8_t>(octet | ((bits[(first_octet + index) * 8U + bit] & 1U) << bit));
        }

        octets[index] = octet;
    }

    return Status::success();
}

} // namespace m110
