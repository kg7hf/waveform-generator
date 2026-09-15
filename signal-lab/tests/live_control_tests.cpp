// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "signal_lab/live_control.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;

        if (failures < 30)
        {
            std::printf("FAIL: %s\n", message);
        }
    }
}

void near(double actual, double expected, double tolerance, const char* message)
{
    check(std::fabs(actual - expected) <= tolerance, message);
}

using signal_lab::ControlEvent;
using signal_lab::ControlKind;
using signal_lab::ControlResult;
using signal_lab::ControlSweep;
using signal_lab::LiveController;

void accepted(LiveController& controller, const ControlEvent& event)
{
    check(controller.enqueue(event) == ControlResult::accepted, "control accepted");
}

std::vector<std::int16_t> render(LiveController& controller, const std::vector<std::int16_t>& input, std::size_t block)
{
    std::vector<std::int16_t> output = input;

    for (std::size_t cursor = 0U; cursor < output.size();)
    {
        const std::size_t count = std::min(block, output.size() - cursor);
        check(controller.process(output.data() + cursor, count), "live block processes");
        cursor += count;
    }

    return output;
}

void test_disabled_path_and_validation()
{
    LiveController controller;
    check(controller.enqueue({}) == ControlResult::not_configured, "unconfigured controls are rejected");
    check(!controller.process(nullptr, 0U), "unconfigured processing is rejected");
    check(!controller.reset(42U, 0.0) && !controller.reset(42U, std::numeric_limits<double>::infinity()), "invalid reference RMS is rejected");
    check(controller.reset(42U, 0.25), "live reset succeeds");
    const std::vector<std::int16_t> source{-32768, -12345, -1, 0, 1, 12345, 32767};
    const auto output = render(controller, source, 3U);
    check(output == source, "disabled overlay is bit exact including both PCM rails");
    signal_lab::det::StreamDigest digest;
    digest.update(source.data(), source.size());
    check(controller.stats().output_digest == digest.value() && controller.stats().clipped_samples == 0U && controller.stats().peak == 1.0f,
          "disabled output digest and level statistics match emitted PCM");
    double energy = 0.0;

    for (const auto sample : source)
    {
        const double value = static_cast<double>(sample) / 32768.0;
        energy += value * value;
    }

    check(controller.stats().output_energy == energy, "live energy uses quantized output samples");
    check(controller.enqueue({6U, ControlKind::cw_enable, 1.0, 0U}) == ControlResult::late_event, "past events are rejected");
    check(controller.enqueue({7U, ControlKind::cw_enable, 2.0, 0U}) == ControlResult::invalid_event, "enable is exactly zero or one");
    check(controller.enqueue({7U, ControlKind::cw_frequency, 24000.0, 0U}) == ControlResult::invalid_event, "Nyquist CW is rejected");
    check(controller.enqueue({7U, ControlKind::cw_frequency, std::numeric_limits<double>::quiet_NaN(), 0U}) == ControlResult::invalid_event,
          "nonfinite control is rejected");
    check(controller.enqueue({7U, ControlKind::static_rate, 0.0, 0U}) == ControlResult::invalid_event, "zero static rate is rejected");
    check(controller.enqueue({7U, ControlKind::static_peak, 61.0, 0U}) == ControlResult::invalid_event, "out of range static peak is rejected");
    check(controller.enqueue({7U, ControlKind::fade_now, 20.0, 0U}) == ControlResult::invalid_event, "zero duration fade is rejected");
    check(controller.enqueue({signal_lab::unbounded_frames - 10U, ControlKind::fade_now, 20.0, 11U}) == ControlResult::invalid_event,
          "fade end overflow is rejected");
    check(controller.enqueue({7U, static_cast<ControlKind>(255), 1.0, 0U}) == ControlResult::invalid_event, "unknown control kind is rejected");
    const auto frame = controller.frame();
    check(!controller.process(nullptr, 1U) && !controller.process(nullptr, signal_lab::engine_capacity_frames + 1U) && controller.frame() == frame,
          "bad processing buffers do not advance the timeline");
    check(controller.process(nullptr, 0U) && controller.frame() == frame, "empty block preserves the timeline");
    check(!controller.reset(1U, -1.0) && controller.frame() == frame && controller.seed() == 42U, "invalid reset is transactional");
}

void test_cw_phase_and_switches()
{
    LiveController controller;
    check(controller.reset(1U, 0.25), "CW reset succeeds");
    accepted(controller, {0U, ControlKind::cw_frequency, 12000.0, 0U});
    accepted(controller, {0U, ControlKind::cw_ci, 0.0, 0U});
    accepted(controller, {0U, ControlKind::cw_enable, 1.0, 0U});
    accepted(controller, {1U, ControlKind::cw_frequency, 6000.0, 0U});
    accepted(controller, {2U, ControlKind::cw_ci, 20.0, 0U});
    accepted(controller, {4U, ControlKind::cw_enable, 0.0, 0U});
    accepted(controller, {6U, ControlKind::cw_enable, 1.0, 0U});
    const auto output = render(controller, std::vector<std::int16_t>(8U), 8U);
    const std::int16_t expected[] = {0, 11585, 819, 0, 0, 0, -819, 0};

    for (std::size_t index = 0U; index < output.size(); ++index)
    {
        near(output[index], expected[index], 1.0, "CW events retain oscillator phase and apply before the designated sample");
    }

    check(controller.state().cw[0].enabled && controller.state().cw[0].frequency_hz == 6000.0 && controller.state().cw[0].ci_db == 20.0 &&
          controller.stats().controls_applied == 7U,
          "CW state and applied count reflect every control");
    LiveController level;
    check(level.reset(1U, 0.25), "CW level reset succeeds");
    accepted(level, {0U, ControlKind::cw_frequency, 1000.0, 0U});
    accepted(level, {0U, ControlKind::cw_ci, 6.0, 0U});
    accepted(level, {0U, ControlKind::cw_enable, 1.0, 0U});
    const auto tone = render(level, std::vector<std::int16_t>(48000U), 2304U);
    double energy = 0.0;

    for (const auto sample : tone)
    {
        const double value = static_cast<double>(sample) / 32768.0;
        energy += value * value;
    }

    near(20.0 * std::log10(0.25 / std::sqrt(energy / tone.size())), 6.0, 0.002, "CW RMS matches the requested C/I");
}

void test_multi_cw_bank()
{
    LiveController combined;
    check(combined.reset(1U, 0.1), "multi-CW reset succeeds");
    accepted(combined, {0U, ControlKind::cw_frequency, 12000.0, 0U, 0U});
    accepted(combined, {0U, ControlKind::cw_ci, 0.0, 0U, 0U});
    accepted(combined, {0U, ControlKind::cw_enable, 1.0, 0U, 0U});
    accepted(combined, {0U, ControlKind::cw_frequency, 6000.0, 0U, 1U});
    accepted(combined, {0U, ControlKind::cw_ci, 0.0, 0U, 1U});
    accepted(combined, {0U, ControlKind::cw_enable, 1.0, 0U, 1U});
    const auto output = render(combined, std::vector<std::int16_t>(8U), 3U);
    const std::int16_t expected[] = {0, 7911, 4634, -1357, 0, 1357, -4634, -7911};

    for (std::size_t index = 0U; index < output.size(); ++index)
    {
        near(output[index], expected[index], 2.0, "CW bank sums independent phase-continuous oscillators");
    }

    check(combined.state().cw[0].enabled && combined.state().cw[1].enabled &&
          combined.state().cw[0].frequency_hz == 12000.0 && combined.state().cw[1].frequency_hz == 6000.0,
          "CW bank retains independent slot state");
    check(combined.enqueue({8U, ControlKind::cw_enable, 1.0, 0U, signal_lab::live_cw_capacity}) == ControlResult::invalid_event,
          "CW slot outside the bounded bank is rejected");
    check(combined.enqueue({8U, ControlKind::static_enable, 1.0, 0U, 1U}) == ControlResult::invalid_event,
          "non-CW controls cannot carry a CW slot");
    ControlSweep sweep;
    sweep.first_frame = 8U;
    sweep.kind = ControlKind::cw_frequency;
    sweep.values[0] = 300.0;
    sweep.values[1] = 3400.0;
    sweep.count = 2U;
    sweep.step_frames = 2U;
    sweep.oscillator = 2U;
    check(combined.enqueue_sweep(sweep) == ControlResult::accepted, "CW sweep targets one oscillator slot");
    (void)render(combined, std::vector<std::int16_t>(3U), 1U);
    check(combined.state().cw[2].frequency_hz == 3400.0 && combined.state().cw[0].frequency_hz == 12000.0,
          "slot sweep does not change another oscillator");
}

void test_fade_shape_and_replacement()
{
    LiveController controller;
    check(controller.reset(1U, 0.25), "fade reset succeeds");
    accepted(controller, {17U, ControlKind::fade_now, 20.0, 400U});
    const auto output = render(controller, std::vector<std::int16_t>(500U, 10000), 113U);
    check(output[16] == 10000 && output[17] == 10000 && output[117] == 1000 && output[316] == 1000 && output[417] == 10000,
          "fade has exact start hold and recovery boundaries");
    near(output[67], 3162.0, 1.0, "half attack has half depth in dB");
    near(output[367], 3162.0, 1.0, "half recovery has half depth in dB");
    check(!controller.state().fade_active, "completed fade is inactive at a block boundary");
    check(controller.reset(1U, 0.25), "replacement fade reset succeeds");
    accepted(controller, {0U, ControlKind::fade_now, 20.0, 400U});
    accepted(controller, {200U, ControlKind::fade_now, 6.0, 1U});
    const auto replaced = render(controller, std::vector<std::int16_t>(402U, 10000), 31U);
    check(replaced[199] == 1000 && replaced[201] == 10000, "new fade replaces the previous envelope without retaining its tail");
    near(replaced[200], 5012.0, 1.0, "single sample fade applies its requested full depth");
}

void fill_scenario(LiveController& controller)
{
    accepted(controller, {0U, ControlKind::cw_enable, 1.0, 0U});
    accepted(controller, {0U, ControlKind::cw_ci, 20.0, 0U});
    accepted(controller, {0U, ControlKind::static_rate, 20.0, 0U});
    accepted(controller, {0U, ControlKind::static_peak, 6.0, 0U});
    accepted(controller, {0U, ControlKind::static_enable, 1.0, 0U});
    // Intentionally enqueue the off event before controls at earlier frames.
    accepted(controller, {42000U, ControlKind::static_enable, 0.0, 0U});
    accepted(controller, {17003U, ControlKind::static_rate, 35.0, 0U});
    accepted(controller, {19111U, ControlKind::static_peak, 12.0, 0U});
    ControlSweep frequencies;
    frequencies.first_frame = 101U;
    frequencies.kind = ControlKind::cw_frequency;
    frequencies.values[0] = 300.0;
    frequencies.values[1] = 1700.0;
    frequencies.values[2] = 3400.0;
    frequencies.count = 3U;
    frequencies.step_frames = 11003U;
    check(controller.enqueue_sweep(frequencies) == ControlResult::accepted, "frequency sweep accepted");
    ControlSweep ci;
    ci.first_frame = 307U;
    ci.kind = ControlKind::cw_ci;
    const double levels[] = {20.0, 10.0, 6.0, 3.0, 0.0, -3.0};
    std::copy(levels, levels + 6U, ci.values);
    ci.count = 6U;
    ci.step_frames = 7001U;
    check(controller.enqueue_sweep(ci) == ControlResult::accepted, "C/I sweep accepted");
    ControlSweep fades;
    fades.first_frame = 503U;
    fades.kind = ControlKind::fade_now;
    const double depths[] = {6.0, 12.0, 18.0, 24.0, 30.0};
    std::copy(depths, depths + 5U, fades.values);
    fades.count = 5U;
    fades.step_frames = 8001U;
    fades.fade_duration_frames = 4001U;
    check(controller.enqueue_sweep(fades) == ControlResult::accepted, "fade-depth sweep accepted");
    accepted(controller, {42000U, ControlKind::cw_enable, 0.0, 0U});
}

void test_replay_block_independence_and_static()
{
    LiveController original;
    check(original.reset(234U, 0.08), "mixed live reset succeeds");
    fill_scenario(original);
    std::vector<std::int16_t> source(48000U);

    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        source[index] = static_cast<std::int16_t>((index * 17U) % 12000U - 6000);
    }

    const auto expected = render(original, source, 2304U);
    check(original.stats().static_events_started > 5U && original.stats().controls_applied == original.capture_count(),
          "mixed run applies controls and produces static crashes");
    check(std::equal(expected.begin() + 42000U, expected.end(), source.begin() + 42000U), "STATIC OFF terminates tails and disabled overlay returns to source");

    for (const std::size_t block :
{
    1U, 17U, 1000U, 2048U
})
    {
        LiveController replay;
        check(replay.reset(original.seed(), original.reference_rms()), "replay reset succeeds");

        for (std::size_t index = 0U; index < original.capture_count(); ++index)
        {
            accepted(replay, original.capture_data()[index]);
        }

        const auto output = render(replay, source, block);
        check(output == expected && replay.stats().output_digest == original.stats().output_digest, "captured controls replay bit exactly at every block size");
        check(replay.stats().static_events_started == original.stats().static_events_started && replay.stats().static_events_dropped == original.stats().static_events_dropped &&
              replay.stats().clipped_samples == original.stats().clipped_samples && replay.stats().output_energy == original.stats().output_energy,
              "replayed static and emitted metrics are block independent");
    }
    LiveController other_seed;
    check(other_seed.reset(235U, 0.08), "other seed reset succeeds");
    fill_scenario(other_seed);
    check(render(other_seed, source, 2304U) != expected, "static sequence changes with the seed");
    // Parameter changes to disabled CW do not draw from the static RNG families.
    LiveController static_a;
    LiveController static_b;
    check(static_a.reset(42U, 0.05) && static_b.reset(42U, 0.05), "independent RNG resets succeed");

    for (auto* controller :
            {
                &static_a, &static_b
            })
    {
        accepted(*controller, {0U, ControlKind::static_rate, 1000.0, 0U});
        accepted(*controller, {0U, ControlKind::static_peak, 0.0, 0U});
        accepted(*controller, {0U, ControlKind::static_enable, 1.0, 0U});
    }
    accepted(static_b, {17U, ControlKind::cw_frequency, 300.0, 0U});
    accepted(static_b, {317U, ControlKind::cw_ci, 30.0, 0U});
    const std::vector<std::int16_t> silence(16000U);
    const auto static_output = render(static_a, silence, 2304U);
    check(static_output == render(static_b, silence, 97U) && static_a.stats().static_events_dropped > 0U,
          "static uses independent RNG streams and bounded concurrent crashes");
}

void test_sweep_atomicity_order_and_limits()
{
    LiveController controller;
    check(controller.reset(1U, 0.25), "sweep reset succeeds");
    accepted(controller, {10U, ControlKind::cw_frequency, 1000.0, 0U});
    accepted(controller, {10U, ControlKind::cw_frequency, 2000.0, 0U});
    auto output = render(controller, std::vector<std::int16_t>(11U), 11U);
    check(controller.state().cw[0].frequency_hz == 2000.0, "equal-frame controls retain enqueue order");
    ControlSweep sweep;
    sweep.first_frame = 11U;
    sweep.kind = ControlKind::cw_frequency;
    sweep.values[0] = 300.0;
    sweep.values[1] = 3400.0;
    sweep.count = 2U;
    sweep.step_frames = 5U;
    check(controller.enqueue_sweep(sweep) == ControlResult::accepted && controller.capture_data()[2].frame == 11U && controller.capture_data()[3].frame == 16U,
          "sweep capture stores expanded absolute event times");
    output = render(controller, std::vector<std::int16_t>(5U), 5U);
    check(controller.state().cw[0].frequency_hz == 300.0, "first sweep step persists for its exact dwell");
    output = render(controller, std::vector<std::int16_t>(1U), 1U);
    check(controller.state().cw[0].frequency_hz == 3400.0, "next sweep step applies at the designated frame");
    const auto count = controller.capture_count();
    sweep.first_frame = controller.frame();
    sweep.values[1] = -1.0;
    check(controller.enqueue_sweep(sweep) == ControlResult::invalid_event && controller.capture_count() == count && controller.pending_count() == 0U,
          "invalid sweep has no partially accepted controls");
    sweep.values[1] = 3400.0;
    sweep.first_frame = signal_lab::unbounded_frames - 1U;
    check(controller.enqueue_sweep(sweep) == ControlResult::invalid_event, "sweep timeline overflow is rejected");
    check(controller.reset(1U, 0.25), "queue limit reset succeeds");

    for (std::size_t index = 0U; index < signal_lab::live_pending_capacity - 1U; ++index)
    {
        accepted(controller, {0U, ControlKind::cw_ci, 20.0, 0U});
    }

    sweep.first_frame = 0U;
    check(controller.enqueue_sweep(sweep) == ControlResult::pending_full && controller.pending_count() == signal_lab::live_pending_capacity - 1U &&
          controller.capture_count() == signal_lab::live_pending_capacity - 1U,
          "pending capacity rejects a whole sweep atomically");
    accepted(controller, {0U, ControlKind::cw_ci, 20.0, 0U});
    check(controller.enqueue({0U, ControlKind::cw_ci, 20.0, 0U}) == ControlResult::pending_full, "full pending queue rejects controls explicitly");
    output = render(controller, std::vector<std::int16_t>(1U), 1U);

    for (std::size_t index = 0U; index < signal_lab::live_pending_capacity; ++index)
    {
        accepted(controller, {1U, ControlKind::cw_ci, 20.0, 0U});
    }

    output = render(controller, std::vector<std::int16_t>(1U), 1U);
    check(controller.capture_count() == signal_lab::live_capture_capacity && controller.enqueue({2U, ControlKind::cw_ci, 20.0, 0U}) == ControlResult::capture_full,
          "capture exhaustion never silently loses replay evidence");
    check(controller.reset(42U, 0.25) && controller.frame() == 0U && controller.capture_count() == 0U && controller.pending_count() == 0U &&
          !controller.state().cw[0].enabled && !controller.state().cw[1].enabled &&
          !controller.state().static_enabled && !controller.state().fade_active,
          "reset clears all state queues and capture for reproducible replay");
}

} // namespace

int main()
{
    test_disabled_path_and_validation();
    test_cw_phase_and_switches();
    test_multi_cw_bank();
    test_fade_shape_and_replacement();
    test_replay_block_independence_and_static();
    test_sweep_atomicity_order_and_limits();

    if (failures == 0)
    {
        std::printf("live_control tests: PASS (controller bytes=%zu)\n", sizeof(LiveController));
        return 0;
    }

    std::printf("live_control tests: %d failures\n", failures);
    return 1;
}
