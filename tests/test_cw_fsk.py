# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Tests for phase-continuous binary FSK carried by Python CW."""

import math
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from signal_lab import FS  # noqa: E402
from signal_lab.cw import Cw, FSK_RNG_IMPLEMENTATION  # noqa: E402
from signal_lab.rng import FAMILY_FSK_SYMBOL, generator  # noqa: E402


class CwFskTests(unittest.TestCase):
    @staticmethod
    def params(*, center=1000, baud=1000, shift=200, pattern="alternating", **extra):
        return {"frequency_hz": center, "ci_db": 0, "phase_degrees": 30,
                "ramp_seconds": 0, "fsk": {"baud": baud, "shift_hz": shift,
                                               "pattern": pattern}, **extra}

    @staticmethod
    def prepared(params, *, total_frames, seed=37, index=0, reference_rms=0.25):
        stage = Cw(params)
        stage.prepare(reference_rms, total_frames, seed, index)
        return stage

    def test_fsk_schema_frequency_bounds_and_exact_symbol_period(self):
        invalid = (
            {"frequency_hz": 1000, "ci_db": 0, "fsk": None},
            {"frequency_hz": 1000, "ci_db": 0, "fsk": {"baud": 1000, "shift_hz": 200}},
            {"frequency_hz": 1000, "ci_db": 0,
             "fsk": {"baud": 1000, "shift_hz": 200, "pattern": "random", "extra": 1}},
            self.params(baud=True),
            self.params(baud=0),
            self.params(baud=FS + 1),
            self.params(baud=110),
            self.params(baud=1e-20),
            self.params(shift=0),
            self.params(center=12000, shift=1e-300),
            self.params(center=2.0, shift=5e-16),
            self.params(shift=math.nan),
            self.params(shift=True),
            self.params(center=100, shift=200),
            self.params(center=23900, shift=200),
            self.params(pattern="random-ish"),
            self.params(pattern=True),
            {"frequency_sweep": {"low_hz": 300, "high_hz": 3300,
                                 "slope_hz_per_second": 300},
             "ci_db": 0, "fsk": {"baud": 1000, "shift_hz": 200,
                                  "pattern": "alternating"}},
        )
        for params in invalid:
            with self.subTest(params=params), self.assertRaises(ValueError):
                self.prepared(params, total_frames=200)

        fractional = self.prepared(self.params(baud=37.5), total_frames=1280)
        self.assertEqual(fractional.resolved()["fsk"]["frames_per_symbol"], 1280)

    def test_alternating_symbol_truth_full_shift_and_phase_continuity(self):
        params = self.params(start_seconds=7 / FS, duration_seconds=133 / FS)
        stage = self.prepared(params, total_frames=160)
        output = stage.process(np.zeros(160), 0)
        peak = math.sqrt(2.0) * 0.25
        phase0 = math.radians(30)
        expected = np.zeros(160)
        for frame in range(7, 140):
            relative = frame - 7
            symbol = relative // 48
            offset = relative % 48
            frequency = 1100.0 if symbol % 2 == 0 else 900.0
            cycles = (symbol // 2) * (48 * (1100.0 + 900.0) / FS)
            if symbol % 2 == 1:
                cycles += 48 * 1100.0 / FS
            cycles += offset * frequency / FS
            expected[frame] = peak * math.sin(2.0 * math.pi * cycles + phase0)
        np.testing.assert_allclose(output, expected, rtol=0, atol=2e-14)
        self.assertEqual(stage.frames_seen, 133)
        resolved = stage.resolved()["fsk"]
        self.assertEqual(resolved, {
            "baud": 1000.0,
            "shift_hz": 200.0,
            "mark_hz": 1100.0,
            "space_hz": 900.0,
            "frames_per_symbol": 48,
            "pattern": "alternating",
            "algorithm_version": 1,
            "rng_family": FAMILY_FSK_SYMBOL,
            "rng_implementation": FSK_RNG_IMPLEMENTATION,
        })

    def test_random_symbols_reset_and_partition_identically(self):
        params = self.params(center=1800, baud=750, shift=400, pattern="random",
                             start_seconds=13 / FS, duration_seconds=8000 / FS)
        count = 9000
        whole_stage = self.prepared(params, total_frames=count, seed=812, index=4)
        whole = whole_stage.process(np.zeros(count), 0)

        chunked_stage = self.prepared(params, total_frames=count, seed=812, index=4)
        chunked_stage.process(np.zeros(0), 0)
        parts = []
        boundaries = (0, 13, 14, 57, 61, 1001, 4097, count)
        for start, end in zip(boundaries, boundaries[1:]):
            parts.append(chunked_stage.process(np.zeros(end - start), start))
        np.testing.assert_array_equal(whole, np.concatenate(parts))
        reset = self.prepared(params, total_frames=count, seed=812, index=4).process(np.zeros(count), 0)
        np.testing.assert_array_equal(whole, reset)

    def test_random_symbol_truth_uses_one_draw_per_symbol_and_preserves_phase(self):
        seed = 91
        index = 5
        count = 10 * 48
        stage = self.prepared(self.params(pattern="random"), total_frames=count,
                              seed=seed, index=index)
        actual = stage.process(np.zeros(count), 0)
        marks = generator(seed, FAMILY_FSK_SYMBOL, index).random(10) >= 0.5
        expected = np.empty(count)
        cycles = 0.0
        peak = math.sqrt(2.0) * 0.25
        for symbol_index, is_mark in enumerate(marks):
            frequency = 1100.0 if is_mark else 900.0
            offsets = np.arange(48, dtype=np.float64)
            expected[symbol_index * 48:(symbol_index + 1) * 48] = peak * np.sin(
                2.0 * np.pi * (cycles + offsets * frequency / FS) + math.radians(30))
            cycles = math.remainder(cycles + 48 * frequency / FS, 1.0)
        np.testing.assert_allclose(actual, expected, rtol=0, atol=2e-14)

    def test_random_fsk_phase_and_symbol_clock_continue_through_gate_off(self):
        count = 200
        fsk = {"baud": 1000, "shift_hz": 200, "pattern": "random"}
        gated_params = {"frequency_hz": 1000, "ci_db": 0, "phase_degrees": 30,
                        "ramp_seconds": 0,
                        "markov_on_off": {"mean_on_seconds": 1 / FS,
                                          "mean_off_seconds": 1 / FS},
                        "fsk": fsk}
        gated = self.prepared(gated_params, total_frames=count, seed=56, index=3)
        actual = gated.process(np.zeros(count), 0)
        continuous_params = {key: value for key, value in gated_params.items() if key != "markov_on_off"}
        continuous = self.prepared(continuous_params, total_frames=count, seed=56, index=3)
        expected = continuous.process(np.zeros(count), 0)
        on = np.arange(count) % 2 == 0
        np.testing.assert_array_equal(actual[on], expected[on])
        np.testing.assert_array_equal(actual[~on], np.zeros(np.count_nonzero(~on)))
        self.assertEqual(gated.frames_seen, int(np.count_nonzero(on)))

    def test_periodic_gate_combines_and_sweep_is_exclusive(self):
        params = self.params(pulse_period_seconds=96 / FS, pulse_duty_cycle=0.5)
        stage = self.prepared(params, total_frames=200)
        output = stage.process(np.zeros(200), 0)
        self.assertNotEqual(output[0], 0.0)
        self.assertTrue(np.any(output[1:48] != 0.0))
        self.assertTrue(np.all(output[48:96] == 0.0))

        with self.assertRaises(ValueError):
            self.prepared({"frequency_hz": 1000, "frequency_sweep": {
                "low_hz": 300, "high_hz": 3300, "slope_hz_per_second": 300},
                "fsk": {"baud": 1000, "shift_hz": 200, "pattern": "alternating"}, "ci_db": 0},
                total_frames=100)


if __name__ == "__main__":
    unittest.main()
