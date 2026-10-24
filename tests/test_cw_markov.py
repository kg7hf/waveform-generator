# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Tests for the streaming geometric Markov gate on Python CW impairments."""

import math
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from signal_lab import FS  # noqa: E402
from signal_lab.cw import Cw, MARKOV_GATE_RNG_IMPLEMENTATION  # noqa: E402
from signal_lab.markov import MarkovOnOffGate  # noqa: E402
from signal_lab.rng import FAMILY_MARKOV_GATE  # noqa: E402
from signal_lab.rng import generator  # noqa: E402


class CwMarkovTests(unittest.TestCase):
    @staticmethod
    def base(**extra):
        return {"frequency_hz": 12000, "ci_db": 0, "phase_degrees": 45,
                "ramp_seconds": 0, **extra}

    @staticmethod
    def prepared(params, *, total_frames, seed=29, index=0, reference_rms=0.25):
        stage = Cw(params)
        stage.prepare(reference_rms, total_frames, seed, index)
        return stage

    @staticmethod
    def markov(on_frames=5, off_frames=8):
        return {"markov_on_off": {"mean_on_seconds": on_frames / FS,
                                  "mean_off_seconds": off_frames / FS}}

    def test_strict_schema_bounds_and_mutual_exclusion(self):
        bad_markov = (
            None,
            {},
            {"mean_on_seconds": 1 / FS},
            {"mean_on_seconds": 1 / FS, "mean_off_seconds": 1 / FS, "extra": 0},
            {"mean_on_seconds": True, "mean_off_seconds": 1 / FS},
            {"mean_on_seconds": "1", "mean_off_seconds": 1 / FS},
            {"mean_on_seconds": math.nan, "mean_off_seconds": 1 / FS},
            {"mean_on_seconds": math.inf, "mean_off_seconds": 1 / FS},
            {"mean_on_seconds": 0, "mean_off_seconds": 1 / FS},
            {"mean_on_seconds": (1 - 1e-8) / FS, "mean_off_seconds": 1 / FS},
            {"mean_on_seconds": 1.0e6 + 1, "mean_off_seconds": 1 / FS},
        )
        for gate in bad_markov:
            params = {**self.base(), "markov_on_off": gate}
            with self.subTest(gate=gate), self.assertRaises(ValueError):
                self.prepared(params, total_frames=32)

        params = {**self.base(), **self.markov(), "pulse_period_seconds": 10 / FS,
                  "pulse_duty_cycle": 0.5}
        with self.assertRaisesRegex(ValueError, "cannot be combined"):
            self.prepared(params, total_frames=32)

    def test_metadata_names_model_family_and_resolved_mean_frames(self):
        stage = self.prepared({**self.base(), **self.markov(17, 31)}, total_frames=200)
        resolved = stage.resolved()["markov_on_off"]
        self.assertEqual(resolved, {
            "mean_on_seconds": 17 / FS,
            "mean_off_seconds": 31 / FS,
            "mean_on_frames": 17.0,
            "mean_off_frames": 31.0,
            "initial_state": "on",
            "dwell_distribution": "geometric",
            "rng_family": FAMILY_MARKOV_GATE,
            "algorithm_version": 1,
            "rng_implementation": MARKOV_GATE_RNG_IMPLEMENTATION,
        })

    def test_gate_helper_rejects_spans_outside_its_window(self):
        gate = MarkovOnOffGate(4, 7, 20, generator(29, FAMILY_MARKOV_GATE, 0))
        for first, count in ((-1, 1), (0, -1), (19, 2), (21, 0)):
            with self.subTest(first=first, count=count), self.assertRaises(ValueError):
                gate.render(first, count, 0)
        self.assertFalse(gate.started)
        self.assertEqual(gate.cursor, 0)

    def test_waveform_gate_has_geometric_mean_and_cdf_over_many_dwell_events(self):
        # At 12 kHz, 45 degrees, |sin| is sqrt(1/2) at every sample. The
        # nonzero output mask therefore observes the gate without consulting
        # the implementation's transition state.
        mean_on = 5
        mean_off = 8
        stage = self.prepared({**self.base(), **self.markov(mean_on, mean_off)},
                              total_frames=2_000_000, seed=7123)
        dwell_lengths = {True: [], False: []}
        state = None
        run_length = 0
        transitions = 0
        cursor = 0
        block_size = 2048
        while transitions < 100_000 and cursor < stage.total_frames:
            count = min(block_size, stage.total_frames - cursor)
            output = stage.process(np.zeros(count, dtype=np.float64), cursor)
            observed = output != 0.0
            for sample in observed:
                sample_state = bool(sample)
                if state is None:
                    self.assertTrue(sample_state, "Markov gate must start on")
                    state = sample_state
                    run_length = 1
                elif sample_state == state:
                    run_length += 1
                else:
                    dwell_lengths[state].append(run_length)
                    transitions += 1
                    state = sample_state
                    run_length = 1
            cursor += count
        self.assertGreaterEqual(transitions, 100_000, "test stream did not reach the required dwell count")
        means = {True: mean_on, False: mean_off}
        for on_state in (True, False):
            observed_dwells = np.asarray(dwell_lengths[on_state], dtype=np.float64)
            expected_mean = means[on_state]
            with self.subTest(on_state=on_state):
                self.assertGreaterEqual(len(observed_dwells), 49_000)
                self.assertLessEqual(abs(float(np.mean(observed_dwells)) / expected_mean - 1.0), 0.02)
                threshold = int(expected_mean)
                observed_survival = float(np.mean(observed_dwells > threshold))
                expected_survival = (1.0 - 1.0 / expected_mean) ** threshold
                self.assertLessEqual(abs(observed_survival - expected_survival), 0.01)

    def test_streaming_partition_reset_and_zero_length_calls_are_exact(self):
        params = {**self.base(), **self.markov(13, 19), "start_seconds": 7 / FS,
                  "duration_seconds": 3000 / FS}
        count = 3500
        whole_stage = self.prepared(params, total_frames=count, seed=813)
        whole = whole_stage.process(np.zeros(count), 0)

        chunked_stage = self.prepared(params, total_frames=count, seed=813)
        empty = np.zeros(0, dtype=np.float64)
        chunked_stage.process(empty, 0)
        parts = []
        boundaries = (0, 5, 7, 8, 63, 127, 1021, 2049, count)
        for start, end in zip(boundaries, boundaries[1:]):
            parts.append(chunked_stage.process(np.zeros(end - start), start))
        np.testing.assert_array_equal(whole, np.concatenate(parts))

        reset = self.prepared(params, total_frames=count, seed=813).process(np.zeros(count), 0)
        np.testing.assert_array_equal(whole, reset)
        with self.assertRaisesRegex(ValueError, "frame zero"):
            self.prepared({**self.base(), **self.markov()}, total_frames=20).process(np.zeros(2), 1)
        noncontiguous = self.prepared({**self.base(), **self.markov()}, total_frames=20)
        noncontiguous.process(np.zeros(2), 0)
        with self.assertRaisesRegex(ValueError, "contiguous"):
            noncontiguous.process(np.zeros(2), 3)

    def test_phase_continues_while_off_and_markov_combines_with_sweep(self):
        gate = {"markov_on_off": {"mean_on_seconds": 1 / FS,
                                  "mean_off_seconds": 1 / FS}}
        count = 40
        fixed = self.prepared({**self.base(), **gate}, total_frames=count, seed=21)
        fixed_output = fixed.process(np.zeros(count), 0)
        ungated = self.prepared(self.base(), total_frames=count, seed=21).process(np.zeros(count), 0)
        active = np.arange(count) % 2 == 0
        np.testing.assert_array_equal(fixed_output[active], ungated[active])
        np.testing.assert_array_equal(fixed_output[~active], np.zeros(np.count_nonzero(~active)))

        sweep_params = {
            "frequency_sweep": {"low_hz": 900, "high_hz": 3900, "slope_hz_per_second": 250},
            "ci_db": 0, "phase_degrees": 45, "ramp_seconds": 0,
            "markov_on_off": {"mean_on_seconds": 1 / FS, "mean_off_seconds": 1 / FS},
        }
        swept = self.prepared(sweep_params, total_frames=count, seed=21).process(np.zeros(count), 0)
        continuous_params = {key: value for key, value in sweep_params.items() if key != "markov_on_off"}
        continuous = self.prepared(continuous_params, total_frames=count, seed=21).process(np.zeros(count), 0)
        np.testing.assert_array_equal(swept[active], continuous[active])
        np.testing.assert_array_equal(swept[~active], np.zeros(np.count_nonzero(~active)))

    def test_ramp_applies_to_each_on_dwell_and_final_window_fragment(self):
        params = {**self.base(), **self.markov(1_000_000, 1_000_000),
                  "duration_seconds": 10 / FS, "ramp_seconds": 100 / FS}
        stage = self.prepared(params, total_frames=10, seed=37)
        output = stage.process(np.zeros(10), 0)
        peak = math.sqrt(2.0) * 0.25
        phase = 2 * math.pi * 12000 * np.arange(10) / FS + math.radians(45)
        distance = np.minimum(np.arange(10), 9 - np.arange(10))
        envelope = np.sin(np.pi / 2 * np.minimum(1.0, distance / 5)) ** 2
        np.testing.assert_allclose(output, peak * np.sin(phase) * envelope, rtol=0, atol=1e-14)


if __name__ == "__main__":
    unittest.main()
