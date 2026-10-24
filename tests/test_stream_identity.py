# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Tests for opt-in stable Python impairment random-stream identities."""

import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from signal_lab.awgn import Awgn  # noqa: E402
from signal_lab.cw import Cw  # noqa: E402
from signal_lab.fade import Fade  # noqa: E402
from signal_lab.impulse import Impulse  # noqa: E402
from signal_lab.rng import effective_stream_seed, generator  # noqa: E402
from signal_lab.scenario import ScenarioError, validate  # noqa: E402
from signal_lab.stream import Pipeline  # noqa: E402


class StreamIdentityTests(unittest.TestCase):
    @staticmethod
    def _render_stage(cls, params, seed, ordinal, frames=4096, instance_index=0):
        stage = cls(params)
        stage.instance_index = instance_index
        stage.prepare(0.25, frames, seed, ordinal)
        source = 0.2 * np.sin(2.0 * np.pi * 700.0 * np.arange(frames) / 48000.0)
        output = stage.process(source.copy(), 0)
        return stage, output

    def test_effective_seed_known_answers(self):
        vectors = (
            (0, 1, 0x5A93E3278FED8726),
            (0x0123456789ABCDEF, 42, 0x005F869427719E8F),
            ((1 << 64) - 1, (1 << 32) - 1, 0xC736A20832A4BD0A),
            (17, 101, 0xD06B33F8173Bcb28),
            (17, 102, 0x573BE5591BE7413F),
            (17, 103, 0xEED46674B852140C),
        )
        for seed, stream_id, expected in vectors:
            with self.subTest(seed=seed, stream_id=stream_id):
                self.assertEqual(effective_stream_seed(seed, stream_id), expected)

    def test_schema_rejects_bad_or_duplicate_ids_even_on_deterministic_stages(self):
        base = {"schema": "signal-lab.scenario/1", "test_id": "stream-test", "seed": 1,
                "source": {"path": "source.wav"}}
        invalid = (True, False, 0, -1, 1.5, "3", None, 1 << 32, 1 << 2000,
                   float("nan"), float("inf"), -float("inf"))
        for stream_id in invalid:
            scenario = {**base, "impairments": [{"type": "cw", "stream_id": stream_id}]}
            with self.subTest(stream_id=stream_id), self.assertRaises(ScenarioError):
                validate(scenario)
        duplicate = {**base, "impairments": [
            {"type": "cw", "stream_id": 12},
            {"type": "sample_slip", "stream_id": 12, "events": []},
        ]}
        with self.assertRaisesRegex(ScenarioError, "unique"):
            validate(duplicate)

    def test_direct_pipeline_validation_and_legacy_awgn_instance_key(self):
        first = Cw({"frequency_hz": 900, "ci_db": 0, "stream_id": 9})
        second = Fade({"depth_db": 1, "duration_ms": 10, "stream_id": 9})
        with self.assertRaisesRegex(ValueError, "unique"):
            Pipeline([first, second]).prepare(0.25, 64, 11)

        awgn = Awgn({"snr_db": 20})
        awgn.instance_index = 3
        awgn.prepare(0.25, 64, 11, 6)
        self.assertEqual(awgn.index, 6)
        self.assertEqual(awgn.rng_seed, 11)
        self.assertEqual(awgn.rng_index, 6)
        expected_rng = generator(11, 1, 3)
        expected_rng.standard_normal(awgn.noise.taps - 1)
        self.assertEqual(awgn.noise.rng.bit_generator.state, expected_rng.bit_generator.state)

    def test_direct_prepare_validates_id_and_accepts_boundaries(self):
        for stream_id in (True, 0, -1, 1.25, 1 << 32):
            with self.subTest(stream_id=stream_id), self.assertRaises(ValueError):
                Cw({"stream_id": stream_id}).prepare(0.25, 64, 17, 2)
        for stream_id in (1, (1 << 32) - 1):
            with self.subTest(stream_id=stream_id):
                stage = Cw({"frequency_hz": 1000, "ci_db": 0, "stream_id": stream_id})
                stage.prepare(0.25, 64, 17, 2)
                self.assertEqual(stage.stream_id, stream_id)
                self.assertEqual(stage.rng_index, 0)

    def test_explicit_ids_keep_all_python_stage_families_stable_when_reordered(self):
        cases = (
            (Awgn, {"snr_db": 20, "stream_id": 101}),
            (Fade, {"depth_db": 2, "duration_ms": 5, "rate_per_sec": 150, "stream_id": 102}),
            (Impulse, {"peak_db": -5, "decay_ms": 0.5, "rate_per_sec": 120,
                       "ring": "noise", "stream_id": 103}),
            (Cw, {"frequency_hz": 12000, "ci_db": 5, "phase_degrees": "random",
                  "ramp_seconds": 0, "fsk": {"baud": 300, "shift_hz": 100, "pattern": "random"},
                  "markov_on_off": {"mean_on_seconds": 5 / 48000, "mean_off_seconds": 7 / 48000},
                  "fading": {"spread_hz": 1.0}, "stream_id": 104}),
        )
        for cls, params in cases:
            with self.subTest(stage=cls.type_name):
                stage_a, out_a = self._render_stage(cls, params, 17, 0)
                stage_b, out_b = self._render_stage(cls, params, 17, 6)
                changed_id = {**params, "stream_id": params["stream_id"] + 100}
                _, out_changed = self._render_stage(cls, changed_id, 17, 0)
                source = 0.2 * np.sin(2.0 * np.pi * 700.0 * np.arange(len(out_a)) / 48000.0)
                np.testing.assert_array_equal(out_a, out_b)
                self.assertFalse(np.array_equal(out_a, source), "stage must have a measurable component")
                self.assertFalse(np.array_equal(out_a, out_changed), "stream_id must select a distinct stream")
                self.assertEqual(stage_a.index, 0)
                self.assertEqual(stage_b.index, 6)
                self.assertEqual(stage_a.rng_index, 0)
                self.assertEqual(stage_b.rng_index, 0)
                self.assertEqual(stage_a.rng_seed, stage_b.rng_seed)

    def test_impulse_random_tone_phase_is_also_stable_by_stream_id(self):
        params = {"peak_db": -5, "decay_ms": 0.5, "rate_per_sec": 120,
                  "ring": "tone", "phase_degrees": "random", "stream_id": 203}
        first, out_first = self._render_stage(Impulse, params, 23, 0)
        moved, out_moved = self._render_stage(Impulse, params, 23, 7)
        other, out_other = self._render_stage(Impulse, {**params, "stream_id": 204}, 23, 0)
        self.assertGreater(len(first.starts), 0)
        self.assertEqual(first.starts, moved.starts)
        np.testing.assert_array_equal(out_first, out_moved)
        self.assertFalse(np.array_equal(out_first, out_other))

    def test_actual_pipeline_keeps_explicit_streams_and_moves_legacy_keys(self):
        explicit_noise = {"snr_db": 18, "stream_id": 301}
        explicit_cw = {"frequency_hz": 12000, "ci_db": 3, "phase_degrees": "random",
                       "ramp_seconds": 0, "stream_id": 302}
        legacy_noise = {"snr_db": 18}
        legacy_cw = {"frequency_hz": 9000, "ci_db": 3, "phase_degrees": "random", "ramp_seconds": 0}
        first = Pipeline([Awgn(explicit_noise), Cw(explicit_cw), Awgn(legacy_noise), Cw(legacy_cw)])
        moved = Pipeline([Awgn(legacy_noise), Fade({"depth_db": 1, "duration_ms": 5, "period_seconds": 0.1}),
                          Awgn(explicit_noise), Cw(explicit_cw), Awgn(legacy_noise), Cw(legacy_cw)])
        first.prepare(0.25, 4096, 31)
        moved.prepare(0.25, 4096, 31)

        def component(stage):
            source = np.zeros(4096, dtype=np.float64)
            return stage.process(source, 0)

        np.testing.assert_array_equal(component(first.stages[0]), component(moved.stages[2]))
        np.testing.assert_array_equal(component(first.stages[1]), component(moved.stages[3]))
        self.assertEqual(first.stages[0].instance_index, 0)
        self.assertEqual(moved.stages[2].instance_index, 1)
        self.assertNotEqual(first.stages[2].instance_index, moved.stages[4].instance_index)
        self.assertNotEqual(first.stages[3].phase_degrees, moved.stages[5].phase_degrees)
        self.assertFalse(np.array_equal(component(first.stages[2]), component(moved.stages[4])))

    def test_untagged_cw_retains_order_key_and_legacy_golden(self):
        params = {"frequency_hz": 12000, "ci_db": 0, "phase_degrees": "random", "ramp_seconds": 0}
        low, out_low = self._render_stage(Cw, params, 17, 0)
        high, out_high = self._render_stage(Cw, params, 17, 3)
        self.assertNotEqual(low.phase_degrees, high.phase_degrees)
        self.assertFalse(np.array_equal(out_low, out_high))
        self.assertEqual(low.phase_degrees, generator(17, 7, 0).uniform(0.0, 360.0))


if __name__ == "__main__":
    unittest.main()
