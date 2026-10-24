# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Tests for bounded Rayleigh Doppler fading on Python CW interferers."""

import math
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from signal_lab import FS  # noqa: E402
from signal_lab.cw import Cw, CW_DOPPLER_RNG_IMPLEMENTATION  # noqa: E402
from signal_lab.cw_doppler import RayleighDoppler  # noqa: E402
from signal_lab.rng import FAMILY_CW_DOPPLER, generator  # noqa: E402


class CwFadedTests(unittest.TestCase):
    @staticmethod
    def fixed(**extra):
        return {"frequency_hz": 1000, "ci_db": 12, "phase_degrees": 30,
                "ramp_seconds": 0, "fading": {"spread_hz": 1.0}, **extra}

    @staticmethod
    def prepared(params, *, total_frames, seed=71, index=2, reference_rms=0.25):
        stage = Cw(params)
        stage.prepare(reference_rms, total_frames, seed, index)
        return stage

    def test_fading_schema_is_strict_and_range_checked(self):
        invalid = (
            None,
            {},
            {"spread_hz": 1.0, "extra": 0},
            {"spread_hz": True},
            {"spread_hz": "1"},
            {"spread_hz": math.nan},
            {"spread_hz": math.inf},
            {"spread_hz": 0.099},
            {"spread_hz": 10.001},
        )
        for fading in invalid:
            with self.subTest(fading=fading), self.assertRaises(ValueError):
                self.prepared({**self.fixed(), "fading": fading}, total_frames=128)

    def test_metadata_and_doppler_design_match_independent_formula(self):
        stage = self.prepared(self.fixed(), total_frames=1000)
        resolved = stage.resolved()["fading"]
        self.assertEqual(resolved, {
            "spread_hz": 1.0,
            "algorithm_version": 1,
            "rng_family": FAMILY_CW_DOPPLER,
            "mean_power": 1.0,
            "distribution": "rayleigh",
            "rng_implementation": CW_DOPPLER_RNG_IMPLEMENTATION,
        })

        design = stage._rayleigh_doppler
        frames_per_node = math.floor((FS / 64.0) / 1.0)
        node_rate = FS / frames_per_node
        multiple = node_rate / 1.0
        half = math.ceil(multiple * math.sqrt(2.0 * math.log(10.0)) / math.pi)
        offsets = np.arange(-half, half + 1, dtype=np.float64)
        expected = np.exp(-np.square(math.pi * offsets / multiple))
        expected /= math.sqrt(float(np.dot(expected, expected)))
        np.testing.assert_allclose(design.coefficients, expected.astype(np.float32), rtol=0, atol=0)
        self.assertEqual((design.frames_per_node, design.half_taps, len(design.coefficients)),
                         (frames_per_node, half, 2 * half + 1))
        self.assertAlmostEqual(design.interpolation_power,
                               1.0 - (1.0 - design.lag_one_correlation) *
                               (frames_per_node ** 2 - 1) / (3 * frames_per_node ** 2), delta=1e-15)

    def test_complex_gain_distribution_is_unit_power_and_rayleigh(self):
        # 2^18 node samples at 10 Hz give about 7,200 effective independent
        # samples after the Gaussian tap correlation; tolerances are several
        # standard errors for power, envelope mean/RMS, and fourth moment.
        spread = 10.0
        node_count = 1 << 18
        fader = RayleighDoppler(spread, generator(1771, FAMILY_CW_DOPPLER, 0),
                                node_count * 75)
        sum_power = 0.0
        sum_envelope = 0.0
        sum_fourth = 0.0
        sample_count = 0
        cursor = 0
        block_size = 4096
        while cursor < fader.window_frames:
            count = min(block_size, fader.window_frames - cursor)
            gains = fader.render(cursor, count)
            envelope = np.abs(gains).astype(np.float64)
            power_samples = envelope * envelope
            sum_power += float(np.sum(power_samples))
            sum_envelope += float(np.sum(envelope))
            sum_fourth += float(np.dot(power_samples, power_samples))
            sample_count += count
            cursor += count
        self.assertEqual(sample_count, node_count * fader.frames_per_node)
        power = sum_power / sample_count
        mean_over_rms = (sum_envelope / sample_count) / math.sqrt(power)
        fourth_over_power_squared = (sum_fourth / sample_count) / (power * power)
        self.assertAlmostEqual(power, 1.0, delta=0.04)
        self.assertAlmostEqual(mean_over_rms, math.sqrt(math.pi) / 2.0, delta=0.02)
        self.assertAlmostEqual(fourth_over_power_squared, 2.0, delta=0.15)

    def test_seeded_repeatability_chunk_partition_and_seed_separation(self):
        params = self.fixed(start_seconds=11 / FS, duration_seconds=4096 / FS)
        count = 4200
        whole = self.prepared(params, total_frames=count, seed=91, index=4).process(np.zeros(count), 0)
        chunked_stage = self.prepared(params, total_frames=count, seed=91, index=4)
        chunked_stage.process(np.zeros(0), 0)
        boundaries = (0, 11, 12, 74, 819, 2049, count)
        chunks = [chunked_stage.process(np.zeros(end - start), start)
                  for start, end in zip(boundaries, boundaries[1:])]
        np.testing.assert_array_equal(whole, np.concatenate(chunks))
        repeated = self.prepared(params, total_frames=count, seed=91, index=4).process(np.zeros(count), 0)
        np.testing.assert_array_equal(whole, repeated)
        other_seed = self.prepared(params, total_frames=count, seed=92, index=4).process(np.zeros(count), 0)
        self.assertFalse(np.array_equal(whole, other_seed))

    def test_fading_starts_at_window_zero_and_does_not_advance_before_it(self):
        seed = 113
        index = 6
        params = self.fixed(start_seconds=20 / FS, duration_seconds=32 / FS)
        stage = self.prepared(params, total_frames=64, seed=seed, index=index)
        prefix = stage.process(np.zeros(20), 0)
        np.testing.assert_array_equal(prefix, np.zeros(20))
        actual = stage.process(np.zeros(32), 20)

        fader = RayleighDoppler(1.0, generator(seed, FAMILY_CW_DOPPLER, index), 32)
        gain = fader.render(0, 32)
        phase = 2.0 * math.pi * 1000.0 * np.arange(32) / FS + math.radians(30)
        amplitude = math.sqrt(2.0) * 0.25 * 10.0 ** (-12.0 / 20.0)
        expected = amplitude * (gain.real * np.sin(phase) + gain.imag * np.cos(phase))
        np.testing.assert_array_equal(actual, expected)

    def test_fading_advances_through_gate_off_for_fixed_sweep_and_fsk(self):
        tones = (
            self.fixed(),
            {"frequency_sweep": {"low_hz": 900, "high_hz": 3900,
                                 "slope_hz_per_second": 250},
             "ci_db": 12, "phase_degrees": 30, "ramp_seconds": 0,
             "fading": {"spread_hz": 1.0}},
            {"frequency_hz": 1000, "ci_db": 12, "phase_degrees": 30,
             "ramp_seconds": 0, "fsk": {"baud": 1000, "shift_hz": 200,
                                           "pattern": "random"},
             "fading": {"spread_hz": 1.0}},
        )
        count = 500
        active = np.arange(count) % 2 == 0
        for params in tones:
            gated_params = dict(params)
            gated_params["markov_on_off"] = {"mean_on_seconds": 1 / FS,
                                              "mean_off_seconds": 1 / FS}
            gated = self.prepared(gated_params, total_frames=count, seed=203, index=5)
            gated_output = gated.process(np.zeros(count), 0)
            continuous = self.prepared(params, total_frames=count, seed=203, index=5)
            continuous_output = continuous.process(np.zeros(count), 0)
            with self.subTest(impairment="FSK" if "fsk" in params else "sweep" if "frequency_sweep" in params else "fixed"):
                np.testing.assert_array_equal(gated_output[active], continuous_output[active])
                np.testing.assert_array_equal(gated_output[~active], np.zeros(np.count_nonzero(~active)))


if __name__ == "__main__":
    unittest.main()
