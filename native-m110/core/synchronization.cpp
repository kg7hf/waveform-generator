/*
================================================================================
synchronization.cpp - body preamble acquisition, delay-spread estimation,
                      timing recovery, and carrier tracking
================================================================================

WHAT THIS FILE IS
-----------------
The front door of the MIL-STD-188-110B serial-tone body receiver. Before any
payload bit can be decoded, this file answers, from complex baseband samples
alone: is a burst present, where does it start, what is its carrier
frequency/phase, what mode/rate is it, and where does the data body begin. It
also measures the channel delay spread (so the caller can pick the equalizer
geometry) and provides the demodulation loops (matched filter + symbol timing +
carrier PLL).

  INPUT  : complex baseband samples. Acquisition runs at the SYMBOL RATE (one
           complex value per 2400-baud symbol); timing recovery runs at T/2.
  OUTPUT : a BodyAcquisition (start, frequency, phase, mode, first_body_symbol,
           anchor diagnostics); a delay-spread symbol count; recovered symbols
           and a running carrier estimate.
  DEPENDS: body_waveform.hpp (preamble structure, psk8_symbol,
           body_channel_symbol_tribit, designator/countdown decode),
           scrambler.hpp (SyncRandomizer), demapper.hpp.
  NOT here: no payload decode. This produces the locked reference the equalizer,
           demapper, and FEC decoder are built on top of.

50,000-FOOT VIEW
----------------
The transmitter always begins with a known preamble made of repeating 480-symbol
segments. The first 288 chips of each segment are a fixed acquisition prefix.
This file correlates the incoming samples against that known prefix to find the
burst, estimates the carrier offset differentially, decodes the D1/D2 mode
designators and the C1/C2/C3 countdown, and back-computes where the body starts.
On multipath it re-anchors to the earliest (direct) path so later paths become
usable postcursor energy, and it majority-votes the mode/boundary across every
segment so one low-SNR mis-decode cannot mis-frame training.

5th-GRADE VERSION
-----------------
A friend always sings the same jingle before a secret message. Listen for the
jingle; when you hear it clearly you learn it is starting, how high their voice
is (carrier frequency), which code they are using (mode), and a countdown to the
real words. Also decide whether you are hearing the true voice or a wall-echo,
and measure how echoey the room is so you bring the right-sized cleanup tool.

WORKED EXAMPLE / DEEP DIVE
--------------------------
The full step-by-step "long division" walkthrough - with toy numbers, equations,
fading-failure modes, and debugging signals - lives in the standalone explainer:
    core/synchronization-and-acquisition-explainer.md

HIGH-LEVEL PASS THROUGH THIS FILE
---------------------------------
 1. make_body_prefix_reference()             build the known 288-chip template.
 2. estimate_body_prefix_frequency_radians() differential carrier estimate.
 3. body_prefix_correlation()                segmented NONCOHERENT match score.
 4. decode_channel_symbol()                  8-code + echo-combining symbol decode.
 5. characterize_body_candidate()            D1/D2 + countdown -> body boundary.
 6. acquire_body_preamble()                  scan -> consensus -> earliest-path
                                             re-anchor -> precursor walk -> vote.
 7. estimate_body_preamble_delay_spread()    whole-symbol multipath spread.
 8. make_root_raised_cosine_taps()           receive matched filter.
 9. matched_filter_and_recover_timing()      coarse phase + early/late loop (T/2).
10. CarrierTracker                           decision-directed carrier PLL.

Governed-evidence note: the long comments inside acquire_body_preamble() cite
measured before/after BER for specific WBS 6.16 seeds. They are the "why" behind
every non-obvious choice; do not simplify them away without re-running the cited
campaigns.
================================================================================
*/

#include "core/synchronization.hpp"

#include "core/demapper.hpp"
#include "core/scrambler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace m110
{

constexpr float pi = 3.14159265358979323846F;
constexpr float two_pi = 2.0F * pi;
constexpr std::array<std::uint8_t, 9> acquisition_prefix{0U, 1U, 3U, 0U, 1U, 3U, 1U, 2U, 0U};
constexpr std::size_t prefix_symbols = body_preamble_acquisition_prefix_symbols;
constexpr std::size_t first_segment_symbols = body_preamble_segment_symbols;

static float wrap_phase(float phase) noexcept
{
    while (phase > pi)
    {
        phase -= two_pi;
    }

    while (phase < -pi)
    {
        phase += two_pi;
    }

    return phase;
}

static IQSample interpolate(IQSampleSpan samples, float position) noexcept
{
    const auto lower = static_cast<std::size_t>(position);
    const auto fraction = position - static_cast<float>(lower);
    return samples[lower] + fraction * (samples[lower + 1U] - samples[lower]);
}

// -----------------------------------------------------------------------------
// make_body_prefix_reference
// -----------------------------------------------------------------------------
// 50K view: Build the known 288-chip acquisition-prefix template that every
//   correlation in this file compares against. Constant for a given build.
// Detailed view: Walk the 9 prefix channel symbols {0,1,3,0,1,3,1,2,0}; spread
//   each over 32 chips (body_channel_symbol_tribit), add the running
//   SyncRandomizer tribit (the sync scramble), reduce mod 8, and map to an 8-PSK
//   point (psk8_symbol). Result: 288 complex reference symbols.
// 5th-grade view: Write down the exact jingle on paper so you can compare what
//   you hear against it.
// -----------------------------------------------------------------------------
Status make_body_prefix_reference(MutableIQSampleSpan reference) noexcept
{
    if (reference.size() != prefix_symbols)
    {
        return {StatusCode::invalid_argument, "body prefix reference span must hold exactly the acquisition prefix"};
    }

    SyncRandomizer randomizer;
    std::size_t index{};

    for (const auto channel_symbol : acquisition_prefix)
    {
        for (std::size_t spread = 0U; spread < 32U; ++spread)
        {
            const auto tribit = static_cast<std::uint8_t>((body_channel_symbol_tribit(channel_symbol, spread) + randomizer.next_tribit()) & 7U);
            reference[index++] = psk8_symbol(tribit);
        }
    }

    return Status::success();
}

// -----------------------------------------------------------------------------
// estimate_body_prefix_frequency_radians
// -----------------------------------------------------------------------------
// 50K view: Estimate the carrier frequency error over the prefix, in radians per
//   symbol, at a candidate start.
// Detailed view: Differential (Kay-style) estimator. Multiply each received
//   sample by the conjugate of the previous one AND strip the known reference
//   modulation: r[k]*conj(r[k-1])*conj(c[k])*c[k-1]. When the received signal is
//   the reference times a carrier e^{j*omega*k}, the known parts cancel to a
//   positive magnitude and the sum's angle is ~= omega. arg(sum) returns omega.
// 5th-grade view: Figure out how much the pitch drifted by comparing each note
//   to the one before it.
// -----------------------------------------------------------------------------
float estimate_body_prefix_frequency_radians(IQSampleSpan input, std::size_t start, IQSampleSpan reference) noexcept
{
    IQSample differential{};

    for (std::size_t index = 1U; index < reference.size(); ++index)
    {
        differential += input[start + index] * std::conj(input[start + index - 1U]) * std::conj(reference[index]) * reference[index - 1U];
    }

    return std::arg(differential);
}

// -----------------------------------------------------------------------------
// body_prefix_correlation
// -----------------------------------------------------------------------------
// 50K view: Return a normalized [0,1] "is the prefix here?" score and, as a side
//   effect, the coherent carrier phase for equalizer seeding.
// Detailed view: Segmented NONCOHERENT correlation - correlate each 32-chip
//   block coherently (derotated by the frequency estimate) and sum the block
//   MAGNITUDES. On a static channel this equals a full coherent correlation;
//   under fast fading the coherent sum self-cancels but the per-block magnitudes
//   survive (the measured 5 Hz graceful-degradation behavior). See the inline
//   note below for the fading rationale.
// 5th-grade view: Instead of matching the whole jingle at once (which a wavering
//   signal ruins), match it in nine short pieces and add up how good each was.
// -----------------------------------------------------------------------------
float body_prefix_correlation(IQSampleSpan input, std::size_t start, IQSampleSpan reference, float frequency_radians, float& phase) noexcept
{
    // Segmented noncoherent prefix correlation: each 32-chip sub-block is
    // correlated coherently and the block magnitudes are summed. On a static
    // channel the block phases align, so the metric equals the full coherent
    // correlation; under fast fading (a 5 Hz spread decorrelates the channel
    // well inside the 120 ms prefix) the coherent sum self-cancels while the
    // per-block gains survive, which is exactly the measured 5 Hz
    // acquisition loss. The reported carrier phase remains the coherent
    // estimate for equalizer seeding.
    constexpr std::size_t block_chips = 32U;
    IQSample coherent{};
    IQSample block_correlation{};
    float noncoherent{};
    float input_energy{};

    for (std::size_t index = 0U; index < reference.size(); ++index)
    {
        const auto rotation = std::polar(1.0F, frequency_radians * static_cast<float>(index));
        const auto product = input[start + index] * std::conj(reference[index] * rotation);
        coherent += product;
        block_correlation += product;
        input_energy += std::norm(input[start + index]);

        if ((index + 1U) % block_chips == 0U)
        {
            noncoherent += std::abs(block_correlation);
            block_correlation = IQSample{};
        }
    }

    noncoherent += std::abs(block_correlation);
    phase = std::arg(coherent);
    return noncoherent / rmath::sqrt(std::max(input_energy * static_cast<float>(reference.size()), 1.0e-12F));
}

// -----------------------------------------------------------------------------
// decode_channel_symbol
// -----------------------------------------------------------------------------
// 50K view: Decode one preamble channel symbol (a D1/D2 designator or a C1/C2/C3
//   countdown digit) by asking which of the 8 orthogonal spreading codes matches.
// Detailed view: For each candidate 0..7 rebuild the expected 32-chip spread
//   (code + sync scramble + within-block frequency ramp) and correlate. The 8
//   codes are orthogonal, so max-magnitude correlation is the ML decision under
//   an unknown constant phase. NONCOHERENT echo combining scores each candidate
//   as |Lag0|^2 + max_{3<=lag<=15}|Lag|^2 (direct + best echo) so a long-delay
//   two-path channel with a faded direct path can still read the symbol.
// 5th-grade view: Hold up all 8 possible letters, see which the sound matches
//   best, and if the direct sound is weak let the wall-echo vote too.
// -----------------------------------------------------------------------------
static std::uint8_t decode_channel_symbol(IQSampleSpan input, std::size_t start, std::size_t channel_index, float frequency_radians) noexcept
{
    std::array<float, 8> correlation_energies{};
    SyncRandomizer randomizer;
    const auto spread_offset = channel_index * 32U;

    for (std::size_t index = 0U; index < spread_offset; ++index)
    {
        static_cast<void>(randomizer.next_tribit());
    }

    std::array<std::uint8_t, 32> randomizer_values{};

    for (auto& value : randomizer_values)
    {
        value = randomizer.next_tribit();
    }

    // The eight 32-chip spreading sequences are mutually orthogonal, so the
    // maximum-magnitude correlation is the maximum-likelihood decision under an
    // unknown constant carrier phase. Extrapolating an absolute phase from the
    // acquisition prefix across the whole segment is not reliable over a fading
    // channel; only the residual frequency ramp within one 32-symbol block is
    // applied, which tolerates several hertz of frequency-estimate error.
    //
    // Long-delay two-path channels (a 5 ms echo is twelve symbols) put nearly
    // half the received energy in a delayed copy the lag-0 correlator treats
    // as interference, and a faded direct path then loses the countdown
    // decode entirely. Each candidate is therefore scored with noncoherent
    // direct-plus-echo combining over a small lag window, without needing the
    // (yet unknown) delay; on a clean channel the echo term adds one noise
    // sample and the orthogonal-code margin absorbs it.
    constexpr std::size_t maximum_echo_lag = 15U;

    for (std::uint8_t candidate = 0U; candidate < 8U; ++candidate)
    {
        std::array < IQSample, maximum_echo_lag + 1U > lag_correlations{};

        for (std::size_t spread = 0U; spread < 32U; ++spread)
        {
            const auto tribit = static_cast<std::uint8_t>((body_channel_symbol_tribit(candidate, spread) + randomizer_values[spread]) & 7U);
            const auto expected = psk8_symbol(tribit) * std::polar(1.0F, frequency_radians * static_cast<float>(spread));

            for (std::size_t lag = 0U; lag <= maximum_echo_lag; ++lag)
            {
                const auto sample_index = start + spread_offset + spread + lag;

                if (sample_index < input.size())
                {
                    lag_correlations[lag] += input[sample_index] * std::conj(expected);
                }
            }
        }

        float best_echo_energy{};

        for (std::size_t lag = 3U; lag <= maximum_echo_lag; ++lag)
        {
            best_echo_energy = std::max(best_echo_energy, std::norm(lag_correlations[lag]));
        }

        correlation_energies[candidate] = std::norm(lag_correlations[0]) + best_echo_energy;
    }

    std::uint8_t best{};

    for (std::uint8_t candidate = 1U; candidate < 8U; ++candidate)
    {
        if (correlation_energies[candidate] > correlation_energies[best])
        {
            best = candidate;
        }
    }

    return best;
}

// -----------------------------------------------------------------------------
// characterize_body_candidate
// -----------------------------------------------------------------------------
// 50K view: Turn a raw "there might be a prefix at start" into a fully described
//   candidate: mode, countdown, preamble start, and body boundary.
// Detailed view: Decode D1(9), D2(10), C1/C2/C3(11/12/13). Each countdown symbol
//   is sent as 4|digit, so a decoded value < 4 means "not a credible segment".
//   Pack the countdown, resolve the mode (recognize_body_designators, LONG tried
//   first), then back-extrapolate: body begins (countdown+1)*480 symbols after
//   this segment; the true preamble start is elapsed_segments*480 earlier (with
//   the carrier phase rewound to match).
// 5th-grade view: Read the dialect and the "...2 more" countdown, then do the
//   arithmetic for where the real words start and where the jingle began.
// -----------------------------------------------------------------------------
BodyAcquisition characterize_body_candidate(IQSampleSpan input, BodyAcquisition candidate, const BodyAcquisitionConfig& config) noexcept
{
    const auto frequency_radians = candidate.frequency_offset_hz * two_pi / config.symbol_rate_hz;
    const auto detected_segment = candidate.first_preamble_symbol;
    const auto d1 = decode_channel_symbol(input, detected_segment, 9U, frequency_radians);
    const auto d2 = decode_channel_symbol(input, detected_segment, 10U, frequency_radians);
    const auto c1 = decode_channel_symbol(input, detected_segment, 11U, frequency_radians);
    const auto c2 = decode_channel_symbol(input, detected_segment, 12U, frequency_radians);
    const auto c3 = decode_channel_symbol(input, detected_segment, 13U, frequency_radians);

    // Every countdown symbol is transmitted as 4 | digit; a decoded value
    // below 4 means this position is not a credible preamble segment.
    if (c1 < 4U || c2 < 4U || c3 < 4U)
    {
        candidate.detected_segment_symbol = detected_segment;
        candidate.preamble_countdown = std::numeric_limits<std::uint8_t>::max();
        candidate.symbol_rate_hz = config.symbol_rate_hz;
        candidate.recognition = {BodyDesignatorState::invalid, {}};
        return candidate;
    }

    const auto countdown = static_cast<std::uint8_t>(((c1 & 3U) << 4U) | ((c2 & 3U) << 2U) | (c3 & 3U));
    const auto short_recognition = recognize_body_designators(d1, d2, false);
    const auto long_recognition = recognize_body_designators(d1, d2, true);
    candidate.detected_segment_symbol = detected_segment;
    candidate.preamble_countdown = countdown;
    candidate.symbol_rate_hz = config.symbol_rate_hz;

    if (long_recognition.state == BodyDesignatorState::supported)
    {
        candidate.recognition = long_recognition;
    }
    else if (short_recognition.state == BodyDesignatorState::supported)
    {
        candidate.recognition = short_recognition;
    }
    else if (long_recognition.state == BodyDesignatorState::recognized_unsupported || short_recognition.state == BodyDesignatorState::recognized_unsupported)
    {
        candidate.recognition = {BodyDesignatorState::recognized_unsupported, {}};
    }
    else
    {
        candidate.recognition = {BodyDesignatorState::invalid, {}};
    }

    if (candidate.recognition.state == BodyDesignatorState::supported)
    {
        const auto segments = body_preamble_segments(candidate.recognition.mode.interleave);

        if (countdown < segments)
        {
            const auto elapsed_segments = segments - static_cast<std::size_t>(countdown) - 1U;
            const auto elapsed_symbols = elapsed_segments * first_segment_symbols;
            candidate.first_body_symbol = detected_segment + (static_cast<std::size_t>(countdown) + 1U) * first_segment_symbols;

            if (candidate.first_preamble_symbol >= elapsed_symbols)
            {
                candidate.first_preamble_symbol -= elapsed_symbols;
                candidate.preamble_start_in_buffer = true;
                candidate.carrier_phase_radians = wrap_phase(candidate.carrier_phase_radians - frequency_radians * static_cast<float>(elapsed_symbols));
            }
        }
    }

    return candidate;
}

bool body_candidate_is_valid(const BodyAcquisition& candidate) noexcept
{
    if (candidate.recognition.state != BodyDesignatorState::supported)
    {
        return false;
    }

    const auto segments = body_preamble_segments(candidate.recognition.mode.interleave);
    return candidate.preamble_countdown < segments && candidate.first_body_symbol >= candidate.detected_segment_symbol + first_segment_symbols;
}

struct AcquisitionConsensus
{
    BodyAcquisition representative{};
    std::size_t supporting_segments{};
    float correlation_sum{};
    float best_correlation{};
};

static bool same_consensus(const AcquisitionConsensus& consensus, const BodyAcquisition& candidate) noexcept
{
    return consensus.supporting_segments != 0U && consensus.representative.first_body_symbol == candidate.first_body_symbol &&
           consensus.representative.recognition.mode.data_rate == candidate.recognition.mode.data_rate &&
           consensus.representative.recognition.mode.interleave == candidate.recognition.mode.interleave;
}

// -----------------------------------------------------------------------------
// acquire_body_preamble  (the main acquisition entry point)
// -----------------------------------------------------------------------------
// 50K view: Scan every candidate start, keep the ones past the frequency and
//   correlation gates, group them by which transmission they describe (consensus
//   voting), pick the strongest group, then re-anchor to the EARLIEST path and
//   repair single-segment mis-decodes with a whole-preamble majority vote.
// Detailed view (the pipeline inside):
//   1. Scan: estimate freq -> gate |f|<=120Hz -> correlate -> gate rho>=0.75 ->
//      characterize -> gate valid.
//   2. Consensus: survivors sharing body boundary + mode fall in one bucket; the
//      representative is the HIGHEST-correlation member (NOT the earliest).
//   3. Pick best group by (supporting segments, corr_sum, best_corr).
//   4. Earliest-path re-anchor (S2a): an earlier same-mode group within
//      [anchor_min, anchor_max] symbols is the same burst through an earlier path.
//   5. Precursor search: correlate negative lags across every segment (sidelobe-
//      corrected) and "walk" each significant peak to rescue a faded direct path.
//   6. vote_mode_and_boundary(): majority mode+boundary over all segments, run
//      before AND after the re-anchor.
// 5th-grade view: Listen everywhere, keep the clear catches, group the agreeing
//   ones, pick the strongest, make sure you locked the FIRST voice not an echo,
//   and let the other segments outvote a segment that misheard the countdown.
// -----------------------------------------------------------------------------
Result<BodyAcquisition> acquire_body_preamble(IQSampleSpan symbol_rate_input, const BodyAcquisitionConfig& config) noexcept
{
    if (config.symbol_rate_hz <= 0.0F || config.minimum_correlation <= 0.0F || config.minimum_correlation > 1.0F || config.maximum_frequency_offset_hz <= 0.0F ||
            symbol_rate_input.size() < first_segment_symbols)
    {
        return Status{StatusCode::invalid_argument, "invalid body acquisition request"};
    }

    std::array<IQSample, prefix_symbols> reference{};
    static_cast<void>(make_body_prefix_reference(reference));
    const auto available_starts = symbol_rate_input.size() - first_segment_symbols + 1U;
    const auto search_starts = config.maximum_start_symbol == 0U ? available_starts : std::min(available_starts, config.maximum_start_symbol + 1U);
    std::array<AcquisitionConsensus, 32> consensus_groups{};
    std::size_t consensus_count{};

    for (std::size_t start = 0U; start < search_starts; ++start)
    {
        const auto frequency_radians = estimate_body_prefix_frequency_radians(symbol_rate_input, start, reference);
        const auto frequency_hz = frequency_radians * config.symbol_rate_hz / two_pi;

        if (std::fabs(frequency_hz) > config.maximum_frequency_offset_hz)
        {
            continue;
        }

        float phase{};
        const auto correlation = body_prefix_correlation(symbol_rate_input, start, reference, frequency_radians, phase);

        if (correlation < config.minimum_correlation)
        {
            continue;
        }

        BodyAcquisition candidate{};
        candidate.first_preamble_symbol = start;
        candidate.frequency_offset_hz = frequency_hz;
        candidate.carrier_phase_radians = phase;
        candidate.normalized_correlation = correlation;
        candidate = characterize_body_candidate(symbol_rate_input, candidate, config);

        if (!body_candidate_is_valid(candidate))
        {
            continue;
        }

        std::size_t group_index{};

        while (group_index < consensus_count && !same_consensus(consensus_groups[group_index], candidate))
        {
            ++group_index;
        }

        if (group_index == consensus_count)
        {
            if (consensus_count == consensus_groups.size())
            {
                continue;
            }

            ++consensus_count;
        }

        auto& consensus = consensus_groups[group_index];

        // The representative supplies the frequency/phase estimates the
        // equalizer trains with, and every member already back-extrapolates
        // its position to the full preamble start - so quality, not
        // earliness, must pick it. A fade-distorted early segment that
        // clears the threshold with a wrong frequency estimate otherwise
        // displaces a clean later segment and poisons the training
        // (measured: a +7.6 Hz representative lost a block a 1.0 Hz
        // same-group member decoded exactly).
        if (consensus.supporting_segments == 0U || candidate.normalized_correlation > consensus.representative.normalized_correlation)
        {
            consensus.representative = candidate;
        }

        ++consensus.supporting_segments;
        consensus.correlation_sum += correlation;
        consensus.best_correlation = std::max(consensus.best_correlation, correlation);
    }

    if (consensus_count == 0U)
    {
        return Status{StatusCode::unavailable, "body preamble countdown not acquired"};
    }

    std::size_t best_group{};

    for (std::size_t group = 1U; group < consensus_count; ++group)
    {
        const auto& candidate = consensus_groups[group];
        const auto& best = consensus_groups[best_group];

        if (candidate.supporting_segments > best.supporting_segments ||
                (candidate.supporting_segments == best.supporting_segments && candidate.correlation_sum > best.correlation_sum) ||
                (candidate.supporting_segments == best.supporting_segments && candidate.correlation_sum == best.correlation_sum && candidate.best_correlation > best.best_correlation))
        {
            best_group = group;
        }
    }

    if (config.anchor_earliest_path)
    {
        // WBS 6.16 S2a: a second consensus group of the same mode whose body
        // boundary precedes the winner's by at most one path delay is the same
        // transmission seen through an earlier path (a false-position candidate
        // cannot clear minimum_correlation on the 288-chip prefix, so any such
        // group is a genuine path). Anchoring on the earliest keeps every other
        // path as postcursor energy the feedback taps and the delay-spread
        // estimator can see.
        const auto& chosen = consensus_groups[best_group].representative;
        auto earliest_group = best_group;

        for (std::size_t group = 0U; group < consensus_count; ++group)
        {
            const auto& candidate = consensus_groups[group].representative;
            const auto& earliest = consensus_groups[earliest_group].representative;

            if (candidate.recognition.mode.data_rate == chosen.recognition.mode.data_rate && candidate.recognition.mode.interleave == chosen.recognition.mode.interleave &&
                    candidate.first_body_symbol < earliest.first_body_symbol && chosen.first_body_symbol - candidate.first_body_symbol <= config.anchor_maximum_path_delay_symbols &&
                    chosen.first_body_symbol - candidate.first_body_symbol >= config.anchor_minimum_path_delay_symbols)
            {
                earliest_group = group;
            }
        }

        best_group = earliest_group;

        // The consensus scan only sees the first few segments, and a direct
        // path faded during exactly those segments leaves no group of its own
        // even though it carries half the burst energy. Search for an earlier
        // path across EVERY preamble segment from the winner's detected segment
        // to its body boundary: the noncoherent multi-segment prefix correlation
        // at negative lags (the estimate_body_preamble_delay_spread method,
        // mirrored to the precursor side), with the reference's own period-8
        // autocorrelation sidelobes removed. A significant precursor path moves
        // the anchor to it and re-decodes the designators and countdown there,
        // where the former anchor is combinable postcursor energy.
        auto winner = consensus_groups[best_group].representative;
        // Consensus over the whole preamble: mode AND body boundary. The mode
        // designators and the countdown come from one segment's decode at the
        // lowest SNR the standard allows; a single mis-decode (measured at
        // 75L: a wrong mode, or a countdown two segments off -> preamble
        // training on a mis-framed reference, residual ~1.0, whole payload
        // lost) is invisible to the consensus scan, which only covers the
        // first two or three segments. Every later segment carries the same
        // designators and a consistent boundary, so decode them all from the
        // current anchor and let the majority rule; segments past the true
        // boundary decode as invalid and abstain. Run before the re-anchor
        // (so the re-anchor's mode test uses the majority mode, not a
        // mis-decode) and again after it (on the new anchor's segments).
        const auto vote_mode_and_boundary = [&](BodyAcquisition & current) noexcept
        {
            struct BoundaryVote
            {
                BodyAcquisition representative{};
                std::size_t votes{};
            };
            std::array<BoundaryVote, 8U> boundaries{};
            std::size_t boundary_count = 0U;
            const auto current_frequency_radians = current.frequency_offset_hz * two_pi / config.symbol_rate_hz;

            for (std::size_t segment_start = current.detected_segment_symbol; segment_start + first_segment_symbols <= symbol_rate_input.size();
                    segment_start += first_segment_symbols)
            {
                BodyAcquisition probe{};
                probe.first_preamble_symbol = segment_start;
                probe.frequency_offset_hz = current.frequency_offset_hz;
                float phase{};
                probe.normalized_correlation = body_prefix_correlation(symbol_rate_input, segment_start, reference, current_frequency_radians, phase);
                probe.carrier_phase_radians = phase;
                probe = characterize_body_candidate(symbol_rate_input, probe, config);

                if (!body_candidate_is_valid(probe))
                {
                    continue;
                }

                std::size_t index = 0U;

                while (index < boundary_count && (boundaries[index].representative.first_body_symbol != probe.first_body_symbol ||
                        boundaries[index].representative.recognition.mode.data_rate != probe.recognition.mode.data_rate ||
                        boundaries[index].representative.recognition.mode.interleave != probe.recognition.mode.interleave))
                {
                    ++index;
                }

                if (index == boundary_count)
                {
                    if (boundary_count == boundaries.size())
                    {
                        continue;
                    }

                    boundaries[index].representative = probe;
                    ++boundary_count;
                }
                else if (probe.normalized_correlation > boundaries[index].representative.normalized_correlation)
                {
                    boundaries[index].representative = probe;
                }

                ++boundaries[index].votes;
            }

            std::size_t best = boundary_count;

            for (std::size_t index = 0U; index < boundary_count; ++index)
            {
                if (best == boundary_count || boundaries[index].votes > boundaries[best].votes)
                {
                    best = index;
                }
            }

            const bool differs = best != boundary_count &&
                                 (boundaries[best].representative.first_body_symbol != current.first_body_symbol ||
                                  boundaries[best].representative.recognition.mode.data_rate != current.recognition.mode.data_rate ||
                                  boundaries[best].representative.recognition.mode.interleave != current.recognition.mode.interleave);

            if (differs && boundaries[best].votes >= 2U)
            {
                // The majority disagrees with the anchored winner: keep the
                // winner's anchor (its path, its frequency) but adopt the
                // majority mode, boundary and the preamble start they imply.
                auto corrected = boundaries[best].representative;
                corrected.frequency_offset_hz = current.frequency_offset_hz;
                // The anchor diagnostics describe the winner's search, not
                // the majority's probe: they survive the correction.
                corrected.anchor = current.anchor;
                current = corrected;
            }
        };

        vote_mode_and_boundary(winner);
        const auto maximum_lag = std::min<std::size_t>(config.anchor_maximum_path_delay_symbols, 15U);
        const auto frequency_radians = winner.frequency_offset_hz * two_pi / config.symbol_rate_hz;
        std::array<IQSample, prefix_symbols> rotated_reference{};

        for (std::size_t index = 0U; index < prefix_symbols; ++index)
        {
            rotated_reference[index] = std::conj(reference[index] * std::polar(1.0F, frequency_radians * static_cast<float>(index)));
        }

        std::array<float, 16U> precursor_powers{};
        std::array<float, 16U> sidelobe_powers{};
        // Structural sidelobes of the main path at negative lags: the prefix
        // reference shifted INTO the previous segment's tail. That tail is
        // known - every segment ends with channel symbol 0 (an all-zero chip
        // pattern, i.e. the bare sync-randomizer tribits at phases 17..31 for
        // the last fifteen chips, since a segment consumes a whole multiple of
        // the 32-entry sync sequence) - and its chips coincide with the
        // prefix's own first block at several lags, which a prefix-only model
        // misses (measured: a false precursor at lag -14 on direct-anchored
        // seeds). Model the tail exactly so the subtraction is honest.
        constexpr std::size_t tail_chips = 15U;
        std::array < IQSample, tail_chips + prefix_symbols > extended_reference{};
        {
            SyncRandomizer tail_randomizer;

            for (std::size_t skip = 0U; skip < 32U - tail_chips; ++skip)
            {
                static_cast<void>(tail_randomizer.next_tribit());
            }

            for (std::size_t chip = 0U; chip < tail_chips; ++chip)
            {
                extended_reference[chip] = psk8_symbol(static_cast<std::uint8_t>((body_channel_symbol_tribit(0U, 32U - tail_chips + chip) + tail_randomizer.next_tribit()) & 7U));
            }

            for (std::size_t index = 0U; index < prefix_symbols; ++index)
            {
                extended_reference[tail_chips + index] = reference[index];
            }
        }

        for (std::size_t lag = 0U; lag <= maximum_lag; ++lag)
        {
            for (std::size_t block = 0U; block < prefix_symbols / 32U; ++block)
            {
                IQSample correlation{};

                for (std::size_t chip = 0U; chip < 32U; ++chip)
                {
                    const auto index = block * 32U + chip;
                    correlation += extended_reference[tail_chips + index - lag] * std::conj(reference[index]);
                }

                sidelobe_powers[lag] += std::norm(correlation);
            }
        }

        const auto segment_limit = std::min(symbol_rate_input.size(), winner.first_body_symbol);
        std::size_t segments_used = 0U;

        for (std::size_t segment_start = winner.detected_segment_symbol; segment_start + prefix_symbols <= segment_limit; segment_start += first_segment_symbols)
        {
            if (segment_start < maximum_lag)
            {
                continue;
            }

            ++segments_used;

            for (std::size_t lag = 0U; lag <= maximum_lag; ++lag)
            {
                float power = 0.0F;

                for (std::size_t block = 0U; block < prefix_symbols / 32U; ++block)
                {
                    IQSample correlation{};

                    for (std::size_t chip = 0U; chip < 32U; ++chip)
                    {
                        const auto index = block * 32U + chip;
                        correlation += symbol_rate_input[segment_start - lag + index] * rotated_reference[index];
                    }

                    power += std::norm(correlation);
                }

                precursor_powers[lag] += power;
            }
        }

        winner.anchor = BodyAnchorDiagnostics{};
        winner.anchor.precursor_segments = segments_used;

        if (segments_used != 0U)
        {
            for (std::size_t lag = 1U; lag <= maximum_lag; ++lag)
            {
                precursor_powers[lag] = std::max(0.0F, precursor_powers[lag] - (sidelobe_powers[lag] / sidelobe_powers[0]) * precursor_powers[0]);
            }

            constexpr float significant_path_fraction = 0.125F;
            const auto minimum_lag = std::max<std::size_t>(3U, config.anchor_minimum_path_delay_symbols);

            for (std::size_t lag = 0U; lag <= maximum_lag; ++lag)
            {
                winner.anchor.precursor_powers[lag] = precursor_powers[0] > 0.0F ? precursor_powers[lag] / precursor_powers[0] : 0.0F;
            }

            // The significant PEAKS, earliest first (largest lag down to the
            // minimum), not merely the lags above the threshold: the
            // pulse-shaping tails put a -8 dB sidelobe one symbol either side
            // of a real path, which clears the 12.5 % floor and, taken as
            // "earlier", anchors one symbol off - every re-characterization
            // then fails the correlation floor and the burst keeps its echo
            // anchor (measured on 600L seed 60080: path at lag 5 with 1.7x
            // the anchored power, sidelobe 0.16 at lag 6). Every qualifying
            // peak is tried in turn, because a marginal spurious peak far out
            // can clear the floor ahead of the genuine path (600L seed 60215:
            // lag 14 at 0.132 of the anchor with the real path at lag 5) and
            // the walk below is the test that tells them apart.
            std::array<std::size_t, 4U> candidate_lags{};
            std::size_t candidate_count = 0U;

            for (auto lag = maximum_lag; lag >= minimum_lag && candidate_count < candidate_lags.size(); --lag)
            {
                const auto before = lag + 1U <= maximum_lag ? precursor_powers[lag + 1U] : 0.0F;
                const auto after = precursor_powers[lag - 1U];

                if (precursor_powers[lag] >= significant_path_fraction * precursor_powers[0] && precursor_powers[lag] > 0.0F &&
                        precursor_powers[lag] >= before && precursor_powers[lag] >= after)
                {
                    candidate_lags[candidate_count++] = lag;
                }
            }

            winner.anchor.precursor_candidates = candidate_count;
            winner.anchor.precursor_lag = candidate_count != 0U ? candidate_lags[0] : 0U;
            winner.anchor.precursor_fraction = candidate_count != 0U && precursor_powers[0] > 0.0F ? precursor_powers[candidate_lags[0]] / precursor_powers[0] : 0.0F;

            for (std::size_t candidate = 0U; candidate < candidate_count && !winner.anchor.reanchored; ++candidate)
            {
                const auto earliest_lag = candidate_lags[candidate];
                // Re-decode the designators and countdown on the earlier path.
                // The precursor path may itself be faded through the segment
                // the consensus scan anchored on (that is often why it never
                // formed a group), so walk forward one segment at a time and
                // keep the first segment whose decode is valid for the same
                // mode; characterize_body_candidate back-extrapolates the
                // preamble start and the body boundary from that segment's
                // countdown. The walk starts at the first segment the lag
                // reaches back from: a detection on the very first segment
                // cannot shift earlier, its successors can.
                auto walk_start = winner.detected_segment_symbol;

                while (walk_start < earliest_lag)
                {
                    walk_start += first_segment_symbols;
                }

                for (std::size_t segment_start = walk_start - earliest_lag; segment_start + first_segment_symbols <= segment_limit; segment_start += first_segment_symbols)
                {
                    // Each shifted segment is evaluated exactly as the
                    // acquisition scan evaluates a candidate start: its own
                    // differential frequency estimate (the winner's estimate
                    // came from the other path and can be fading-biased by
                    // tens of hertz, which alone drops a strong path below
                    // the correlation floor), then the prefix correlation.
                    const auto segment_frequency_radians = estimate_body_prefix_frequency_radians(symbol_rate_input, segment_start, reference);
                    const auto segment_frequency_hz = segment_frequency_radians * config.symbol_rate_hz / two_pi;
                    ++winner.anchor.walk_segments;

                    if (std::fabs(segment_frequency_hz) > config.maximum_frequency_offset_hz)
                    {
                        ++winner.anchor.walk_rejected_frequency;
                        continue;
                    }

                    BodyAcquisition shifted{};
                    shifted.first_preamble_symbol = segment_start;
                    shifted.frequency_offset_hz = segment_frequency_hz;
                    float phase{};
                    shifted.normalized_correlation = body_prefix_correlation(symbol_rate_input, segment_start, reference, segment_frequency_radians, phase);
                    shifted.carrier_phase_radians = phase;

                    // The designator/countdown decode combines echoes over
                    // lags 0..15, so it validates any anchor up to a path
                    // delay EARLY as readily as the true one; the prefix
                    // correlation at the shifted start is the discriminating
                    // test (a genuine path clears the acquisition floor there,
                    // a phantom does not). A faded segment simply fails it and
                    // the walk moves on to the next.
                    if (shifted.normalized_correlation < config.minimum_correlation)
                    {
                        ++winner.anchor.walk_rejected_correlation;
                        continue;
                    }

                    shifted = characterize_body_candidate(symbol_rate_input, shifted, config);

                    if (!body_candidate_is_valid(shifted))
                    {
                        ++winner.anchor.walk_rejected_invalid;
                        continue;
                    }

                    if (shifted.recognition.mode.data_rate != winner.recognition.mode.data_rate || shifted.recognition.mode.interleave != winner.recognition.mode.interleave)
                    {
                        ++winner.anchor.walk_rejected_mode;
                        continue;
                    }

                    const auto diagnostics = winner.anchor;
                    winner = shifted;
                    winner.anchor = diagnostics;
                    winner.anchor.reanchored = true;
                    winner.anchor.reanchored_lag = earliest_lag;
                    break;
                }
            }
        }

        vote_mode_and_boundary(winner);

        return winner;
    }

    return consensus_groups[best_group].representative;
}

// -----------------------------------------------------------------------------
// estimate_body_preamble_delay_spread
// -----------------------------------------------------------------------------
// 50K view: Measure how many whole symbols of multipath spread the channel has,
//   so the caller can pick the equalizer geometry (short vs long) before training.
// Detailed view: Correlate the known prefix at symbol lags 0..15, accumulated
//   NONCOHERENTLY across every preamble segment (a path can vanish for one
//   segment under fast fading). Subtract the reference's own period-8
//   autocorrelation sidelobes (which otherwise masquerade as echoes near lags
//   8-9). Return the largest lag >= 3 whose surviving power reaches 12.5% (~-9dB)
//   of the strongest path; 0 means a single clean path.
// 5th-grade view: Clap once and time the echoes to learn how boomy the room is,
//   so you bring the right-sized cleanup tool.
// -----------------------------------------------------------------------------
Result<std::size_t> estimate_body_preamble_delay_spread(IQSampleSpan symbol_rate_input, const BodyAcquisition& acquisition, std::size_t maximum_delay_symbols,
        BodyDelaySpreadProfile* profile) noexcept
{
    // A path is significant when its noncoherent correlation power reaches
    // this fraction of the strongest path (about -9 dB), well above the
    // ~1/288 cross-correlation floor of the prefix sequence.
    constexpr float significant_path_fraction = 0.125F;
    const auto segment = acquisition.detected_segment_symbol;

    if (acquisition.recognition.state != BodyDesignatorState::supported ||
            segment + prefix_symbols + maximum_delay_symbols > symbol_rate_input.size())
    {
        return Status{StatusCode::invalid_argument, "delay-spread estimate needs the full acquisition prefix plus the delay search span"};
    }

    std::array<IQSample, prefix_symbols> reference{};
    static_cast<void>(make_body_prefix_reference(reference));
    const auto frequency_radians = acquisition.frequency_offset_hz * two_pi / acquisition.symbol_rate_hz;
    float strongest_power = 0.0F;
    std::size_t delay_spread = 0U;
    std::array<float, 16U> lag_powers{};

    if (maximum_delay_symbols >= lag_powers.size())
    {
        return Status{StatusCode::invalid_argument, "delay-spread search span exceeds the supported lag table"};
    }

    // The prefix chip patterns repeat with period eight, so the reference has
    // deterministic autocorrelation sidelobes (notably near lags 8-9) that
    // scale with the main path and would otherwise masquerade as an echo.
    // Model the segment's clean continuation (the D1 block follows the
    // prefix) and measure the structural sidelobe profile so it can be
    // subtracted from the received lag powers.
    std::array < IQSample, prefix_symbols + lag_powers.size() > extended_reference{};
    {
        SyncRandomizer extension_randomizer;

        for (std::size_t index = 0U; index < prefix_symbols; ++index)
        {
            extended_reference[index] = reference[index];
            static_cast<void>(extension_randomizer.next_tribit());
        }

        const auto designators = body_designators(acquisition.recognition.mode);

        if (!designators)
        {
            return designators.status();
        }

        const auto d1 = designators.value().d1;

        for (std::size_t spread = 0U; spread < lag_powers.size(); ++spread)
        {
            const auto tribit = static_cast<std::uint8_t>((body_channel_symbol_tribit(d1, spread) + extension_randomizer.next_tribit()) & 7U);
            extended_reference[prefix_symbols + spread] = psk8_symbol(tribit);
        }
    }
    std::array<float, 16U> sidelobe_powers{};

    for (std::size_t lag = 0U; lag <= maximum_delay_symbols; ++lag)
    {
        for (std::size_t block = 0U; block < prefix_symbols / 32U; ++block)
        {
            IQSample correlation{};

            for (std::size_t chip = 0U; chip < 32U; ++chip)
            {
                const auto index = block * 32U + chip;
                correlation += extended_reference[index + lag] * std::conj(reference[index]);
            }

            sidelobe_powers[lag] += std::norm(correlation);
        }
    }

    // Accumulate over every remaining preamble segment: under fast fading a
    // path can vanish for the duration of one segment, so a single-segment
    // profile under-reports the spread; the noncoherent multi-segment sum
    // recovers it.
    const auto segment_limit = std::min(symbol_rate_input.size(), acquisition.first_body_symbol);

    // WP 5.6 (R1) - the per-chip rotation std::polar(1, frequency_radians*index)
    // depends only on the symbol index, not on the lag or segment, so it is
    // identical across every (lag, segment) pass below. Precompute the conjugated,
    // derotated reference once and reuse it: this removes the ~48x recomputation of
    // std::polar (13,824 -> prefix_symbols calls) with byte-identical arithmetic
    // (rotated_reference[index] == conj(reference[index] * rotation) exactly).
    std::array<IQSample, prefix_symbols> rotated_reference{};

    for (std::size_t index = 0U; index < prefix_symbols; ++index)
    {
        rotated_reference[index] = std::conj(reference[index] * std::polar(1.0F, frequency_radians * static_cast<float>(index)));
    }

    std::size_t accumulated_segments = 0U;

    for (std::size_t segment_start = segment; segment_start + prefix_symbols + maximum_delay_symbols <= segment_limit; segment_start += first_segment_symbols)
    {
        ++accumulated_segments;

        for (std::size_t lag = 0U; lag <= maximum_delay_symbols; ++lag)
        {
            float power = 0.0F;

            for (std::size_t block = 0U; block < prefix_symbols / 32U; ++block)
            {
                IQSample correlation{};

                for (std::size_t chip = 0U; chip < 32U; ++chip)
                {
                    const auto index = block * 32U + chip;
                    correlation += symbol_rate_input[segment_start + lag + index] * rotated_reference[index];
                }

                power += std::norm(correlation);
            }

            lag_powers[lag] += power;
        }
    }

    // Remove each lag's structural-sidelobe share of the main-path power, then
    // pick the strongest surviving echo outside the main-path lobe (a
    // fractional-symbol path splits across adjacent lags, so the lobe spans
    // lags 0-2).
    for (std::size_t lag = 1U; lag <= maximum_delay_symbols; ++lag)
    {
        lag_powers[lag] = std::max(0.0F, lag_powers[lag] - (sidelobe_powers[lag] / sidelobe_powers[0]) * lag_powers[0]);
    }

    for (std::size_t lag = 0U; lag <= maximum_delay_symbols; ++lag)
    {
        strongest_power = std::max(strongest_power, lag_powers[lag]);
    }

    std::size_t echo_lag = 0U;
    float echo_power = 0.0F;

    for (std::size_t lag = 3U; lag <= maximum_delay_symbols; ++lag)
    {
        if (lag_powers[lag] > echo_power)
        {
            echo_power = lag_powers[lag];
            echo_lag = lag;
        }
    }

    if (echo_power >= significant_path_fraction * strongest_power)
    {
        delay_spread = echo_lag;
    }

    if (profile != nullptr)
    {
        profile->lag_powers = lag_powers;
        profile->strongest_power = strongest_power;
        profile->segments = accumulated_segments;
    }

    return delay_spread;
}

Status make_root_raised_cosine_taps(std::size_t samples_per_symbol, float rolloff, std::span<float> taps) noexcept
{
    if (samples_per_symbol < 2U || rolloff <= 0.0F || rolloff > 1.0F || taps.size() < 3U || (taps.size() & 1U) == 0U)
    {
        return {StatusCode::invalid_argument, "invalid root-raised-cosine filter request"};
    }

    const auto center = static_cast<float>(taps.size() - 1U) * 0.5F;
    float energy{};

    for (std::size_t index = 0U; index < taps.size(); ++index)
    {
        const auto time = (static_cast<float>(index) - center) / static_cast<float>(samples_per_symbol);
        float value{};

        if (std::fabs(time) < 1.0e-6F)
        {
            value = 1.0F + rolloff * (4.0F / pi - 1.0F);
        }
        else if (std::fabs(std::fabs(4.0F * rolloff * time) - 1.0F) < 1.0e-5F)
        {
            const auto angle = pi / (4.0F * rolloff);
            value = (rolloff / rmath::sqrt(2.0F)) * ((1.0F + 2.0F / pi) * rmath::sin(angle) + (1.0F - 2.0F / pi) * rmath::cos(angle));
        }
        else
        {
            const auto numerator = rmath::sin(pi * time * (1.0F - rolloff)) + 4.0F * rolloff * time * rmath::cos(pi * time * (1.0F + rolloff));
            const auto denominator = pi * time * (1.0F - 16.0F * rolloff * rolloff * time * time);
            value = numerator / denominator;
        }

        taps[index] = value;
        energy += value * value;
    }

    if (energy <= 0.0F)
    {
        return {StatusCode::internal_error, "root-raised-cosine filter has no energy"};
    }

    const auto normalization = 1.0F / rmath::sqrt(energy);

    for (auto& tap : taps)
    {
        tap *= normalization;
    }

    return Status::success();
}

// -----------------------------------------------------------------------------
// matched_filter_and_recover_timing
// -----------------------------------------------------------------------------
// 50K view: Run the matched filter, find the best sampling phase, then track
//   symbol timing with an early/late loop and emit recovered symbols (+ optional
//   T/2 half-symbols).
// Detailed view: (1) FIR-filter the input with the RRC taps. (2) Coarse timing:
//   try each integer sub-sample phase over phase_search_symbols and keep the one
//   with the most energy. (3) Fine timing: at each symbol interpolate early,
//   on_time, late; the Gardner-style error Re{(late-early)*conj(on_time)} nudges
//   the sampling position (clamped) on top of the ~2 samples/symbol advance. The
//   on_time value is the recovered symbol; (early,on_time) is the T/2 pair.
// 5th-grade view: Slide your finger along the wave to land on each peak; if you
//   drift early or late, nudge back on beat.
// -----------------------------------------------------------------------------
Result<TimingRecoveryProgress> matched_filter_and_recover_timing(IQSampleSpan input, std::span<const float> matched_filter_taps, const TimingRecoveryConfig& config,
        MutableIQSampleSpan filtered_scratch, MutableIQSampleSpan recovered_symbols, MutableIQSampleSpan recovered_half_symbols) noexcept
{
    if (config.samples_per_symbol < 2U || config.phase_search_symbols == 0U || config.loop_gain < 0.0F || config.maximum_step_correction <= 0.0F || matched_filter_taps.empty() ||
            filtered_scratch.size() < input.size() || recovered_symbols.empty() || (!recovered_half_symbols.empty() && recovered_half_symbols.size() < recovered_symbols.size() * 2U) ||
            input.size() < matched_filter_taps.size() + config.samples_per_symbol)
    {
        return Status{StatusCode::invalid_argument, "invalid matched-filter timing-recovery request"};
    }

    for (std::size_t output_index = 0U; output_index < input.size(); ++output_index)
    {
        IQSample sum{};
        const auto available_taps = std::min(output_index + 1U, matched_filter_taps.size());

        for (std::size_t tap = 0U; tap < available_taps; ++tap)
        {
            sum += matched_filter_taps[tap] * input[output_index - tap];
        }

        filtered_scratch[output_index] = sum;
    }

    const auto half_symbol = static_cast<float>(config.samples_per_symbol) * 0.5F;

    if (config.first_symbol_sample < static_cast<std::size_t>(std::ceil(half_symbol)) || config.first_symbol_sample + config.samples_per_symbol >= input.size())
    {
        return Status{StatusCode::invalid_argument, "timing-recovery first-symbol sample is out of range"};
    }

    std::size_t selected_phase{};
    float best_energy = -1.0F;

    for (std::size_t phase = 0U; phase < config.samples_per_symbol; ++phase)
    {
        float energy{};
        std::size_t samples{};

        for (std::size_t symbol = 0U; symbol < config.phase_search_symbols; ++symbol)
        {
            const auto index = config.first_symbol_sample + phase + symbol * config.samples_per_symbol;

            if (index >= input.size())
            {
                break;
            }

            energy += std::norm(filtered_scratch[index]);
            ++samples;
        }

        if (samples != 0U)
        {
            energy /= static_cast<float>(samples);
        }

        if (energy > best_energy)
        {
            best_energy = energy;
            selected_phase = phase;
        }
    }

    auto position = static_cast<float>(config.first_symbol_sample + selected_phase);
    float final_correction{};
    std::size_t written{};

    while (written < recovered_symbols.size() && position >= half_symbol && position + half_symbol + 1.0F < static_cast<float>(input.size()))
    {
        const auto early = interpolate(filtered_scratch.first(input.size()), position - half_symbol);
        const auto on_time = interpolate(filtered_scratch.first(input.size()), position);
        const auto late = interpolate(filtered_scratch.first(input.size()), position + half_symbol);
        recovered_symbols[written] = on_time;

        if (!recovered_half_symbols.empty())
        {
            recovered_half_symbols[written * 2U] = early;
            recovered_half_symbols[written * 2U + 1U] = on_time;
        }

        ++written;
        const auto timing_error = std::real((late - early) * std::conj(on_time));
        const auto normalization = std::max(std::norm(early) + std::norm(on_time) + std::norm(late), 1.0e-6F);
        final_correction = std::clamp(config.loop_gain * timing_error / normalization, -config.maximum_step_correction, config.maximum_step_correction);
        position += static_cast<float>(config.samples_per_symbol) + final_correction;
    }

    if (written == 0U)
    {
        return Status{StatusCode::unavailable, "timing recovery produced no symbols"};
    }

    return TimingRecoveryProgress{written, position, final_correction, selected_phase};
}

Status CarrierTracker::configure(float proportional_gain, float integral_gain, float maximum_frequency_radians_per_symbol) noexcept
{
    if (!std::isfinite(proportional_gain) || !std::isfinite(integral_gain) || !std::isfinite(maximum_frequency_radians_per_symbol) || proportional_gain < 0.0F ||
            integral_gain < 0.0F || maximum_frequency_radians_per_symbol <= 0.0F || maximum_frequency_radians_per_symbol > pi)
    {
        return {StatusCode::invalid_argument, "carrier tracker configuration is invalid"};
    }

    proportional_gain_ = proportional_gain;
    integral_gain_ = integral_gain;
    maximum_frequency_radians_per_symbol_ = maximum_frequency_radians_per_symbol;
    frequency_radians_per_symbol_ = std::clamp(frequency_radians_per_symbol_, -maximum_frequency_radians_per_symbol_, maximum_frequency_radians_per_symbol_);
    return Status::success();
}

void CarrierTracker::reset(float phase_radians, float frequency_radians_per_symbol) noexcept
{
    phase_radians_ = std::isfinite(phase_radians) ? wrap_phase(phase_radians) : 0.0F;
    frequency_radians_per_symbol_ = std::isfinite(frequency_radians_per_symbol)
                                    ? std::clamp(frequency_radians_per_symbol, -maximum_frequency_radians_per_symbol_, maximum_frequency_radians_per_symbol_)
                                    : 0.0F;
}

void CarrierTracker::restore(float phase_radians, float frequency_radians_per_symbol) noexcept
{
    phase_radians_ = phase_radians;
    frequency_radians_per_symbol_ = frequency_radians_per_symbol;
}

// -----------------------------------------------------------------------------
// CarrierTracker::update  (decision-directed 2nd-order carrier PLL)
// -----------------------------------------------------------------------------
// 50K view: Remove residual carrier phase/frequency during demodulation, learning
//   from each symbol decision.
// Detailed view: Derotate the input by the current phase; form the phase error
//   arg(corrected*conj(decision)); integrate it into the frequency estimate
//   (clamped to +/- max rad/symbol); advance the phase by frequency +
//   proportional*error. advance() free-wheels the phase for known/skipped
//   symbols; reset() wraps/clamps a fresh state; restore() re-installs a captured
//   state bit-for-bit (no re-wrap) for exact streaming replay.
// 5th-grade view: Keep gently turning a dial so each symbol lands upright; if the
//   dial keeps needing the same turn, learn that drift and turn ahead of it.
// -----------------------------------------------------------------------------
IQSample CarrierTracker::update(IQSample input, IQSample decision) noexcept
{
    const auto corrected = input * std::polar(1.0F, -phase_radians_);
    const auto error = std::arg(corrected * std::conj(decision));
    frequency_radians_per_symbol_ = std::clamp(frequency_radians_per_symbol_ + integral_gain_ * error, -maximum_frequency_radians_per_symbol_,
                                    maximum_frequency_radians_per_symbol_);
    phase_radians_ = wrap_phase(phase_radians_ + frequency_radians_per_symbol_ + proportional_gain_ * error);
    return corrected;
}

void CarrierTracker::advance() noexcept
{
    phase_radians_ = wrap_phase(phase_radians_ + frequency_radians_per_symbol_);
}

} // namespace m110
