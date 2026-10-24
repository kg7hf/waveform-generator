# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Tests for sample-aligned periodic CW gating in the Python reference renderer."""

import math
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from signal_lab import FS  # noqa: E402
from signal_lab.cw import Cw  # noqa: E402


class CwPulseTests(unittest.TestCase):
    @staticmethod
    def prepared(params, *, total_frames=64, reference_rms=0.25):
        stage = Cw(params)
        stage.prepare(reference_rms, total_frames, 4, 0)
        return stage

    def test_pulse_fields_must_be_paired_numeric_and_frame_aligned(self):
        invalid = (
            {"pulse_period_seconds": 4 / FS},
            {"pulse_duty_cycle": 0.5},
            {"pulse_period_seconds": True, "pulse_duty_cycle": 0.5},
            {"pulse_period_seconds": 4 / FS, "pulse_duty_cycle": False},
            {"pulse_period_seconds": math.nan, "pulse_duty_cycle": 0.5},
            {"pulse_period_seconds": 0, "pulse_duty_cycle": 0.5},
            {"pulse_period_seconds": 1.0e9 + 1, "pulse_duty_cycle": 0.5},
            {"pulse_period_seconds": 1.1 / FS, "pulse_duty_cycle": 0.5},
            {"pulse_period_seconds": (1.0 - 1e-7) / FS, "pulse_duty_cycle": 1.0},
            {"pulse_period_seconds": 1 / FS, "pulse_duty_cycle": 1.0 - 1e-7},
            {"pulse_period_seconds": 4 / FS, "pulse_duty_cycle": 0},
            {"pulse_period_seconds": 4 / FS, "pulse_duty_cycle": 1.01},
            {"pulse_period_seconds": 10 / FS, "pulse_duty_cycle": 0.35},
        )
        for pulse in invalid:
            params = {"frequency_hz": 1000, "ci_db": 6, **pulse}
            with self.subTest(pulse=pulse), self.assertRaises(ValueError):
                self.prepared(params)

    def test_valid_pulse_phase_is_relative_to_window_and_continues_when_off(self):
        stage = self.prepared({"frequency_hz": 6000, "ci_db": 0, "phase_degrees": 90,
                               "start_seconds": 7 / FS, "duration_seconds": 24 / FS,
                               "ramp_seconds": 0, "pulse_period_seconds": 10 / FS,
                               "pulse_duty_cycle": 0.4}, total_frames=40)
        source = np.zeros(40)
        actual = stage.process(source, 0)
        peak = math.sqrt(2) * 0.25
        expected = np.zeros(40)
        for n in range(7, 31):
            relative = n - 7
            if relative % 10 < 4:
                expected[n] = peak * math.sin(2 * math.pi * 6000 * relative / FS + math.pi / 2)
        np.testing.assert_allclose(actual, expected, rtol=0, atol=1e-14)
        resolved = stage.resolved()
        self.assertEqual(resolved["pulse_period_frames"], 10)
        self.assertEqual(resolved["pulse_on_frames"], 4)
        self.assertEqual(resolved["pulse_realized_duty_cycle"], 0.4)

    def test_raised_cosine_is_applied_per_on_window_and_clamped_on_truncated_final_pulse(self):
        stage = self.prepared({"frequency_hz": 1, "ci_db": 0, "phase_degrees": 90,
                               "duration_seconds": 15 / FS, "ramp_seconds": 100 / FS,
                               "pulse_period_seconds": 12 / FS, "pulse_duty_cycle": 0.5}, total_frames=20)
        actual = stage.process(np.zeros(20), 0)
        peak = math.sqrt(2) * 0.25
        expected_envelope = np.array([0, 0.25, 0.75, 0.75, 0.25, 0, 0, 0, 0, 0, 0, 0,
                                      0, 1, 0, 0, 0, 0, 0, 0], dtype=float)
        expected = peak * np.sin(2 * np.pi * np.arange(20) / FS + math.pi / 2) * expected_envelope
        np.testing.assert_allclose(actual, expected, rtol=0, atol=1e-14)

    def test_chunk_partition_is_exact_and_off_phase_does_not_restart(self):
        params = {"frequency_hz": 1250, "ci_db": 12, "phase_degrees": -23,
                  "start_seconds": 3 / FS, "duration_seconds": 61 / FS, "ramp_seconds": 4 / FS,
                  "pulse_period_seconds": 13 / FS, "pulse_duty_cycle": 5 / 13}
        source = np.linspace(-0.1, 0.1, 80)
        whole_stage = self.prepared(params, total_frames=len(source))
        whole = whole_stage.process(source.copy(), 0)
        chunks_stage = self.prepared(params, total_frames=len(source))
        chunks = np.concatenate((chunks_stage.process(source[:11].copy(), 0),
                                 chunks_stage.process(source[11:28].copy(), 11),
                                 chunks_stage.process(source[28:53].copy(), 28),
                                 chunks_stage.process(source[53:].copy(), 53)))
        np.testing.assert_array_equal(whole, chunks)

    def test_c_i_is_unramped_on_state_and_duty_one_is_continuous_cw(self):
        reference = 0.5
        ci_db = 7.0
        base = {"frequency_hz": 1000, "ci_db": ci_db, "phase_degrees": 90, "ramp_seconds": 0}
        continuous = self.prepared(base, total_frames=48, reference_rms=reference)
        pulse = self.prepared({**base, "pulse_period_seconds": 8 / FS, "pulse_duty_cycle": 1.0}, total_frames=48,
                              reference_rms=reference)
        plain = continuous.process(np.zeros(48), 0)
        pulsed = pulse.process(np.zeros(48), 0)
        np.testing.assert_array_equal(plain, pulsed)
        resolved = pulse.resolved()
        self.assertAlmostEqual(resolved["tone_rms_dbfs"], 20 * math.log10(reference) - ci_db, delta=1e-12)
        self.assertEqual(resolved["pulse_realized_duty_cycle"], 1.0)

        half_duty = self.prepared({**base, "frequency_hz": 6000, "pulse_period_seconds": 8 / FS,
                                   "pulse_duty_cycle": 0.5}, total_frames=48, reference_rms=reference)
        half_output = half_duty.process(np.zeros(48), 0)
        self.assertAlmostEqual(half_duty.resolved()["tone_rms_dbfs"], 20 * math.log10(reference) - ci_db, delta=1e-12)
        self.assertAlmostEqual(half_duty.stats()["component_rms_rel_reference_db"],
                               -ci_db, delta=1e-12)
        self.assertEqual(half_duty.frames_seen, 24)
        measured_total_db = 20 * math.log10(math.sqrt(float(np.mean(half_output ** 2))) / reference)
        self.assertAlmostEqual(measured_total_db, -ci_db + 10 * math.log10(0.5), delta=1e-12)

    def test_omitted_pulse_preserves_legacy_resolved_metadata_and_output(self):
        params = {"frequency_hz": 1200, "ci_db": 3, "phase_degrees": 17,
                  "start_seconds": 5 / FS, "duration_seconds": 31 / FS, "ramp_seconds": 2 / FS}
        first = self.prepared(params, total_frames=48)
        second = self.prepared(params, total_frames=48)
        source = np.linspace(-0.2, 0.2, 48)
        first_output = first.process(source.copy(), 0)
        second_output = second.process(source.copy(), 0)
        np.testing.assert_array_equal(first_output, second_output)
        self.assertEqual(set(first.resolved()), {"type", "frequency_hz", "ci_db", "phase_degrees", "start_frame",
                                                 "end_frame", "ramp_frames", "tone_peak_amplitude",
                                                 "tone_rms_dbfs", "convention"})


if __name__ == "__main__":
    unittest.main()
