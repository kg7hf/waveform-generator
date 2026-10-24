# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Tests for analytic, block-independent swept CW in the Python renderer."""

import math
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from signal_lab import FS  # noqa: E402
from signal_lab.cw import Cw, SWEEP_EXACT_INTEGER_LIMIT, SWEEP_PHASE_TURN  # noqa: E402


class CwSweepTests(unittest.TestCase):
    @staticmethod
    def prepared(params, *, total_frames, reference_rms=0.25):
        stage = Cw(params)
        stage.prepare(reference_rms, total_frames, 17, 0)
        return stage

    @staticmethod
    def base_sweep(low=300, high=3300, slope=300, **extra):
        return {"frequency_sweep": {"low_hz": low, "high_hz": high,
                                    "slope_hz_per_second": slope},
                "ci_db": 12, "phase_degrees": 0, "ramp_seconds": 0, **extra}

    def test_requires_exactly_one_frequency_form_and_strict_finite_sweep_fields(self):
        invalid = (
            {"ci_db": 12},
            {"frequency_hz": 1000, "frequency_sweep": {"low_hz": 300, "high_hz": 3300,
                                                          "slope_hz_per_second": 300}, "ci_db": 12},
            {"frequency_sweep": None, "ci_db": 12},
            {"frequency_sweep": {"low_hz": 300, "high_hz": 3300}, "ci_db": 12},
            {"frequency_sweep": {"low_hz": 300, "high_hz": 3300,
                                  "slope_hz_per_second": 300, "extra": 1}, "ci_db": 12},
            {"frequency_sweep": {"low_hz": True, "high_hz": 3300,
                                  "slope_hz_per_second": 300}, "ci_db": 12},
            {"frequency_sweep": {"low_hz": 300, "high_hz": "3300",
                                  "slope_hz_per_second": 300}, "ci_db": 12},
            {"frequency_sweep": {"low_hz": 300, "high_hz": 3300,
                                  "slope_hz_per_second": math.nan}, "ci_db": 12},
            {"frequency_sweep": {"low_hz": 300, "high_hz": 3300,
                                  "slope_hz_per_second": math.inf}, "ci_db": 12},
            self.base_sweep(low=0),
            self.base_sweep(low=300, high=300),
            self.base_sweep(low=300, high=24000),
            self.base_sweep(low=300, high=23999.1),
            self.base_sweep(slope=0),
            self.base_sweep(slope=-1),
            self.base_sweep(slope=1e-20),
            self.base_sweep(low=1, high=2, slope=1e6),
        )
        for params in invalid:
            with self.subTest(params=params), self.assertRaises(ValueError):
                self.prepared(params, total_frames=64)

    def test_full_twenty_second_triangle_uses_q64_period_and_starts_low_ascending(self):
        params = self.base_sweep()
        stage = self.prepared(params, total_frames=20 * FS)
        resolved = stage.resolved()
        sweep = resolved["frequency_sweep"]
        self.assertNotIn("frequency_hz", resolved)
        self.assertEqual(resolved["sweep_algorithm_version"], 1)
        self.assertEqual((sweep["low_hz"], sweep["high_hz"], sweep["slope_hz_per_second"]),
                         (300.0, 3300.0, 300.0))
        self.assertLess(sweep["cycle_step_q64"], SWEEP_EXACT_INTEGER_LIMIT)
        self.assertGreater(sweep["cycle_step_q64"], 0)
        expected_step = round((300.0 / (2.0 * 3000.0 * FS)) * SWEEP_PHASE_TURN)
        self.assertEqual(sweep["cycle_step_q64"], expected_step)
        self.assertAlmostEqual(sweep["realized_period_seconds"], 20.0, delta=2e-12)

        # At t=.25 s the requested continuous integral is 84.375 cycles;
        # Q0.64 quantization changes it only below the precision of this check.
        output = stage.process(np.zeros(20 * FS, dtype=np.float64), 0)
        peak = math.sqrt(2.0) * 0.25 * 10.0 ** (-12.0 / 20.0)
        self.assertAlmostEqual(output[FS // 4] / peak, math.sin(2.0 * math.pi * 0.375), delta=2e-10)
        self.assertAlmostEqual(output[0], 0.0, delta=1e-15)
        crossing_indices = np.flatnonzero((output[:-1] <= 0.0) & (output[1:] > 0.0))
        crossing_times = crossing_indices.astype(np.float64) + (
            -output[crossing_indices] / (output[crossing_indices + 1] - output[crossing_indices]))

        def measured_frequency(center_seconds, half_window=0.2):
            interval = crossing_times[(crossing_times >= (center_seconds - half_window) * FS) &
                                      (crossing_times <= (center_seconds + half_window) * FS)]
            self.assertGreater(len(interval), 100)
            return (len(interval) - 1) * FS / (interval[-1] - interval[0])

        for time_seconds, expected_hz in ((2.5, 1050.0), (7.5, 2550.0),
                                          (12.5, 2550.0), (17.5, 1050.0)):
            with self.subTest(time_seconds=time_seconds):
                self.assertAlmostEqual(measured_frequency(time_seconds), expected_hz, delta=3.0)
        measured_slope_up = (measured_frequency(7.5) - measured_frequency(2.5)) / 5.0
        measured_slope_down = (measured_frequency(12.5) - measured_frequency(17.5)) / 5.0
        self.assertAlmostEqual(measured_slope_up, 300.0, delta=1.0)
        self.assertAlmostEqual(measured_slope_down, 300.0, delta=1.0)

    def test_fractional_period_turns_repeat_from_phase_without_frame_rounding(self):
        stage = self.prepared(self.base_sweep(slope=317), total_frames=40 * FS)
        sweep = stage.resolved()["frequency_sweep"]
        period = sweep["realized_period_seconds"]
        self.assertNotAlmostEqual(period * FS, round(period * FS), delta=1e-4)
        samples = stage.process(np.zeros(40 * FS), 0)
        peak = math.sqrt(2.0) * 0.25 * 10.0 ** (-12.0 / 20.0)

        def expected_at(seconds):
            cycles = math.floor(seconds / period)
            remainder = seconds - cycles * period
            half_period = period / 2.0
            slope = 2.0 * (3300.0 - 300.0) / period
            if remainder <= half_period:
                partial = 300.0 * remainder + 0.5 * slope * remainder * remainder
            else:
                rising = 300.0 * half_period + 0.5 * slope * half_period * half_period
                falling = remainder - half_period
                partial = rising + 3300.0 * falling - 0.5 * slope * falling * falling
            area = (300.0 + 3300.0) * 0.5 * period
            return peak * math.sin(2.0 * math.pi * math.remainder(cycles * area + partial, 1.0))

        for frame in (FS // 4, 19 * FS + 123, 20 * FS + 777, 39 * FS + 11):
            with self.subTest(frame=frame):
                self.assertAlmostEqual(samples[frame], expected_at(frame / FS), delta=2e-9)

    def test_sweep_integral_is_partition_independent_and_continues_during_pulse_off(self):
        params = self.base_sweep(low=900, high=3900, slope=250,
                                 start_seconds=7 / FS, duration_seconds=90000 / FS,
                                 ramp_seconds=24 / FS,
                                 pulse_period_seconds=1200 / FS, pulse_duty_cycle=0.25)
        count = 100000
        source = np.linspace(-0.1, 0.1, count)
        whole_stage = self.prepared(params, total_frames=count)
        whole = whole_stage.process(source.copy(), 0)
        chunk_stage = self.prepared(params, total_frames=count)
        parts = []
        boundaries = (0, 17, 611, 1207, 8191, 48003, count)
        for start, end in zip(boundaries, boundaries[1:]):
            parts.append(chunk_stage.process(source[start:end].copy(), start))
        chunked = np.concatenate(parts)
        np.testing.assert_array_equal(whole, chunked)

        continuous_params = {key: value for key, value in params.items()
                             if key not in ("pulse_period_seconds", "pulse_duty_cycle")}
        continuous_params["ramp_seconds"] = 0
        continuous = self.prepared(continuous_params, total_frames=count).process(np.zeros(count), 0)
        relative = np.arange(count) - 7
        in_window = (relative >= 0) & (relative < 90000)
        pulse_phase = relative % 1200
        on = in_window & (pulse_phase >= 24) & (pulse_phase < 276)
        np.testing.assert_allclose((whole - source)[on], continuous[on], rtol=0, atol=2e-16)
        self.assertAlmostEqual((whole - source)[7], 0.0, delta=1e-15)
        self.assertAlmostEqual((whole - source)[19], continuous[19] * 0.5, delta=2e-16)

    def test_ci_uses_input_reference_and_fixed_tone_metadata_output_stays_legacy(self):
        reference = 0.4
        ci_db = 8.0
        sweep_params = {**self.base_sweep(), "ci_db": ci_db}
        sweep = self.prepared(sweep_params, total_frames=20 * FS, reference_rms=reference)
        sweep_output = sweep.process(np.zeros(20 * FS), 0)
        measured_ci = 20.0 * math.log10(reference / math.sqrt(float(np.mean(sweep_output ** 2))))
        self.assertAlmostEqual(measured_ci, ci_db, delta=0.1)
        self.assertAlmostEqual(sweep.resolved()["tone_rms_dbfs"],
                               20.0 * math.log10(reference) - ci_db, delta=1e-12)

        fixed_params = {"frequency_hz": 1250, "ci_db": 8, "phase_degrees": -23,
                        "start_seconds": 5 / FS, "duration_seconds": 31 / FS, "ramp_seconds": 0}
        fixed = self.prepared(fixed_params, total_frames=48)
        actual = fixed.process(np.zeros(48), 0)
        relative = np.arange(31, dtype=np.float64)
        expected = math.sqrt(2.0) * 0.25 * 10.0 ** (-8.0 / 20.0) * np.sin(
            2.0 * np.pi * 1250.0 * relative / FS + math.radians(-23.0))
        np.testing.assert_array_equal(actual[5:36], expected)
        self.assertEqual(set(fixed.resolved()), {"type", "frequency_hz", "ci_db", "phase_degrees", "start_frame",
                                                 "end_frame", "ramp_frames", "tone_peak_amplitude",
                                                 "tone_rms_dbfs", "convention"})


if __name__ == "__main__":
    unittest.main()
