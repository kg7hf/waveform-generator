#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Normal-renderer integration tests for the bounded second-source input."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

os.environ["OMP_NUM_THREADS"] = "1"
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["MKL_NUM_THREADS"] = "1"
os.environ["NUMEXPR_NUM_THREADS"] = "1"

import numpy as np

RENDERER: Path


def write_pcm24(path: Path, samples: np.ndarray) -> None:
    quantized = np.clip(np.rint(samples * (1 << 23)), -(1 << 23), (1 << 23) - 1).astype(np.int32)
    unsigned = quantized & 0xFFFFFF
    packed = np.empty((len(samples), 3), dtype=np.uint8)
    packed[:, 0] = unsigned & 0xFF
    packed[:, 1] = (unsigned >> 8) & 0xFF
    packed[:, 2] = (unsigned >> 16) & 0xFF
    with wave.open(str(path), "wb") as writer:
        writer.setparams((1, 3, 48000, 0, "NONE", "not compressed"))
        writer.writeframes(packed.tobytes())


def read_pcm24(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as reader:
        raw = reader.readframes(reader.getnframes())
    packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
    values = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
    values = (values ^ 0x800000) - 0x800000
    return values.astype(np.float64) / (1 << 23)


def _fit_frequency(samples: np.ndarray, center_hz: float) -> float:
    window = np.hanning(len(samples))
    indices = np.arange(len(samples), dtype=np.float64)
    candidates = center_hz + np.arange(-0.5, 0.5001, 0.005)
    scores = [abs(np.sum(samples * window * np.exp(-2j * np.pi * frequency * indices / 48000.0)))
              for frequency in candidates]
    return float(candidates[int(np.argmax(scores))])


class SecondSourceRenderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scenario_dir = self.root / "scenario folder"
        self.scenario_dir.mkdir()
        frames = 48000
        index = np.arange(frames, dtype=np.float64)
        self.primary = 0.12 * np.sin(2.0 * np.pi * 1200.0 * index / 48000.0)
        self.secondary = 0.08 * np.sin(2.0 * np.pi * 600.0 * index / 48000.0 + 0.31)
        self.source = self.root / "primary.wav"
        self.secondary_path = self.scenario_dir / "secondary input.wav"
        write_pcm24(self.source, self.primary)
        write_pcm24(self.secondary_path, self.secondary)
        self.primary_q = read_pcm24(self.source)
        self.secondary_q = read_pcm24(self.secondary_path)

    def render(self, *, offset=0.0, block=2048, extra=None, duration=24000,
               source_start=1000, start=2048, source_path="secondary input.wav",
               source_gain_db=0.0, reference_rms=0.2, si_db=0.0,
               prefix="render", cwd=None, include_second=True,
               leading_stages=(), fading=None, markov_on_off=None, stream_id=None, seed=None,
               raw_scenario=None):
        stage = {"type": "second_source", "source_path": source_path, "si_db": si_db,
                 "start_frame": start, "source_start_frame": source_start,
                 "frequency_offset_hz": offset}
        if fading is not None:
            stage["fading"] = fading
        if markov_on_off is not None:
            stage["markov_on_off"] = markov_on_off
        if stream_id is not None:
            stage["stream_id"] = stream_id
        if duration is not None:
            stage["duration_frames"] = duration
        stages = list(leading_stages) + ([stage] if include_second else [])
        document = {"reference_rms": reference_rms, "source_gain_db": source_gain_db,
                    "clip_policy": "reject", "impairments": stages}
        if seed is not None:
            document["seed"] = seed
        scenario = self.scenario_dir / "case.json"
        scenario.write_text(raw_scenario if raw_scenario is not None else json.dumps(document), encoding="utf-8")
        output = self.root / f"{prefix}-{offset}-{block}.wav"
        sidecar = self.root / f"{prefix}-{offset}-{block}.json"
        target = self.root / f"{prefix}-{offset}-{block}.target.json"
        command = [str(RENDERER), "--scenario", str(scenario), "--source", str(self.source),
                   "--output", str(output), "--sidecar", str(sidecar), "--target-scenario", str(target),
                   "--block", str(block)]
        if extra:
            command.extend(extra)
        result = subprocess.run(command, capture_output=True, text=True, check=False, cwd=cwd)
        return result, output, sidecar, target, document

    def test_unshifted_overlay_has_independent_sample_and_si_oracle(self):
        result, output, sidecar, target, document = self.render(offset=0.0, block=127)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        actual = read_pcm24(output)
        rms = math.sqrt(float(np.mean(self.secondary_q[1000:25000] ** 2)))
        scale = 0.2 / rms
        expected = self.primary_q.copy()
        expected[2048:2048 + 24000] += self.secondary_q[1000:25000] * scale
        self.assertLessEqual(float(np.max(np.abs(actual - expected))), 1.1 / (1 << 23))
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        source = metadata["secondary_source"]
        self.assertEqual(source["source_frames"], len(self.secondary_q))
        self.assertEqual(source["excerpt_start_frame"], 1000)
        self.assertEqual(source["excerpt_frames"], 24000)
        self.assertEqual(source["start_frame"], 2048)
        self.assertAlmostEqual(source["excerpt_reference_rms"], rms, places=14)
        self.assertEqual(source["digest_format"], "fnv1a64-canonical-pcm24")
        self.assertEqual(len(source["pcm_digest"]), 16)
        exported = json.loads(target.read_text(encoding="utf-8"))
        self.assertEqual(Path(exported["impairments"][0]["source_path"]).resolve(), self.secondary_path.resolve())
        self.assertEqual(exported["impairments"][0]["si_db"], document["impairments"][0]["si_db"])

    def test_offset_sign_and_block_partition_are_visible_in_output_spectrum(self):
        excerpt_rms = math.sqrt(float(np.mean(self.secondary_q[1000:25000] ** 2)))
        unshifted_component = self.secondary_q[1000 + 1024:1000 + 1024 + 16000] * (0.2 / excerpt_rms)
        reference_power = float(np.mean(unshifted_component ** 2))
        for offset in (75.0, -75.0):
            result, output, sidecar, target, _ = self.render(offset=offset, block=127, prefix=f"offset-{int(offset)}")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output_samples = read_pcm24(output)
            np.testing.assert_array_equal(output_samples[:2048], self.primary_q[:2048])
            np.testing.assert_array_equal(output_samples[26048:], self.primary_q[26048:])
            metadata = json.loads(sidecar.read_text(encoding="utf-8"))
            self.assertEqual(Path(metadata["secondary_source"]["source_path"]).resolve(), self.secondary_path.resolve())
            exported_path = json.loads(target.read_text(encoding="utf-8"))["impairments"][0]["source_path"]
            self.assertEqual(Path(exported_path).resolve(), self.secondary_path.resolve())
            first = 2048 + 1024
            primary_only = self.primary_q[first:first + 16000]
            component = output_samples[first:first + 16000] - primary_only
            power = np.abs(np.fft.rfft(component)) ** 2
            target_hz = 600.0 + offset
            target_bin = round(target_hz * len(component) / 48000.0)
            self.assertGreater(power[target_bin], float(np.sum(power)) * 0.95)
            estimated_frequency = _fit_frequency(component, target_hz)
            self.assertLess(abs(estimated_frequency - target_hz), 0.1)
            component_power = float(np.mean(component ** 2))
            component_db = 10.0 * math.log10(component_power / reference_power)
            self.assertLessEqual(abs(component_db), 0.1)
            self.assertTrue(math.isfinite(estimated_frequency) and math.isfinite(component_db))
            print("second_source_measurement=" + json.dumps({"frequency_offset_hz": offset,
                  "estimated_frequency_hz": estimated_frequency, "component_power_delta_db": component_db}))
            partition_bytes = output.read_bytes()

            for block in (1, 2048):
                partition, partition_output, _, _, _ = self.render(offset=offset, block=block,
                                                                     prefix=f"partition-{int(offset)}-{block}")
                self.assertEqual(partition.returncode, 0, partition.stdout + partition.stderr)
                self.assertEqual(partition_bytes, partition_output.read_bytes())

            replay_output = self.root / f"replay-target-{int(offset)}.wav"
            replay_sidecar = self.root / f"replay-target-{int(offset)}.json"
            replay = subprocess.run([str(RENDERER), "--scenario", str(target), "--source", str(self.source),
                                     "--output", str(replay_output), "--sidecar", str(replay_sidecar),
                                     "--block", "2048"], capture_output=True, text=True, check=False, cwd=self.root)
            self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
            self.assertEqual(partition_bytes, replay_output.read_bytes())

    def test_zero_duration_selects_to_eof_without_extending_primary(self):
        result, output, sidecar, _, _ = self.render(offset=0.0, block=2048, duration=None,
                                                    source_start=40000, start=32000)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(read_pcm24(output)), len(self.primary_q))
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        self.assertEqual(metadata["secondary_source"]["excerpt_start_frame"], 40000)
        self.assertEqual(metadata["secondary_source"]["excerpt_frames"], 8000)
        samples = read_pcm24(output)
        np.testing.assert_array_equal(samples[40000:], self.primary_q[40000:])

    def test_si_scales_with_source_gain_applied_to_the_desired_channel(self):
        gain_db = -6.0
        result, output, _, _, _ = self.render(offset=0.0, block=127, source_gain_db=gain_db,
                                               prefix="source-gain")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        gain = 10.0 ** (gain_db / 20.0)
        excerpt_rms = math.sqrt(float(np.mean(self.secondary_q[1000:25000] ** 2)))
        expected = self.primary_q * gain
        expected[2048:2048 + 24000] += self.secondary_q[1000:25000] * (0.2 * gain / excerpt_rms)
        self.assertLessEqual(float(np.max(np.abs(read_pcm24(output) - expected))), 1.1 / (1 << 23))

    def test_measured_si_uses_measured_desired_rms_at_nonzero_source_gain(self):
        desired_rms = math.sqrt(float(np.mean(self.primary_q ** 2)))
        gain_db = -6.0
        gain = 10.0 ** (gain_db / 20.0)
        excerpt_rms = math.sqrt(float(np.mean(self.secondary_q[1000:25000] ** 2)))
        for si_db in (12.0, -3.0):
            with self.subTest(si_db=si_db):
                result, output, sidecar, _, _ = self.render(offset=0.0, block=127,
                    source_gain_db=gain_db, reference_rms=desired_rms, si_db=si_db,
                    prefix=f"measured-si-{int(si_db)}")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                actual = read_pcm24(output)[2048:2048 + 24000]
                desired = self.primary_q[2048:2048 + 24000] * gain
                measured_component = actual - desired
                desired_power = float(np.mean(desired ** 2))
                component_power = float(np.mean(measured_component ** 2))
                measured_si_db = 10.0 * math.log10(desired_power / component_power)
                self.assertLessEqual(abs(measured_si_db - si_db), 0.1)
                self.assertTrue(math.isfinite(measured_si_db))
                metadata = json.loads(sidecar.read_text(encoding="utf-8"))
                self.assertAlmostEqual(metadata["secondary_source"]["si_db"], si_db)
                self.assertAlmostEqual(metadata["secondary_source"]["excerpt_reference_rms"], excerpt_rms, places=14)
                print("second_source_measurement=" + json.dumps({"requested_si_db": si_db,
                      "measured_si_db": measured_si_db, "desired_rms": math.sqrt(desired_power),
                      "component_rms": math.sqrt(component_power)}))

    def test_secondary_addition_does_not_change_primary_watterson_channel(self):
        watterson = {"type": "watterson", "paths": [{"fading": "fixed", "delay_ms": 0,
                                                         "power_db": 0, "phase_deg": 0}]}
        base, base_output, _, _, _ = self.render(include_second=False, block=127, prefix="watterson-base",
                                                  leading_stages=(watterson,))
        mixed, mixed_output, _, _, _ = self.render(include_second=True, block=127, prefix="watterson-mixed",
                                                    leading_stages=(watterson,))
        self.assertEqual(base.returncode, 0, base.stdout + base.stderr)
        self.assertEqual(mixed.returncode, 0, mixed.stdout + mixed.stderr)
        desired = read_pcm24(base_output)
        combined = read_pcm24(mixed_output)
        np.testing.assert_array_equal(combined[:2048], desired[:2048])
        np.testing.assert_array_equal(combined[2048 + 24000:], desired[2048 + 24000:])
        excerpt_rms = math.sqrt(float(np.mean(self.secondary_q[1000:25000] ** 2)))
        overlay = self.secondary_q[1000:25000] * (0.2 / excerpt_rms)
        error = combined[2048:2048 + 24000] - desired[2048:2048 + 24000] - overlay
        self.assertLessEqual(float(np.max(np.abs(error))), 1.1 / (1 << 23))

    def test_rayleigh_fading_is_partition_invariant_replayable_and_window_relative(self):
        fading = {"spread_hz": 0.1}
        plain, plain_output, _, _, _ = self.render(offset=0.0, block=127,
            prefix="fading-plain-reference", stream_id=101, seed=17)
        self.assertEqual(plain.returncode, 0, plain.stdout + plain.stderr)
        plain_samples = read_pcm24(plain_output)
        reference = None
        reference_sidecar = None
        reference_random_key = None
        for block in (1, 127, 2048):
            result, output, sidecar, target, _ = self.render(offset=0.0, block=block,
                prefix=f"fading-block-{block}", fading=fading, stream_id=101, seed=17)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            samples = read_pcm24(output)
            np.testing.assert_array_equal(samples[:2048], self.primary_q[:2048])
            np.testing.assert_array_equal(samples[2048 + 24000:], self.primary_q[2048 + 24000:])
            component = samples[2048:2048 + 24000] - self.primary_q[2048:2048 + 24000]
            plain_component = plain_samples[2048:2048 + 24000] - self.primary_q[2048:2048 + 24000]
            self.assertGreater(float(np.max(np.abs(component - plain_component))), 1.0 / (1 << 23))
            metadata = json.loads(sidecar.read_text(encoding="utf-8"))
            source = metadata["secondary_source"]
            self.assertEqual(source["latency_frames"], 215)
            self.assertEqual(source["fading"], {"spread_hz": 0.1, "mean_power": 1,
                                                   "rng_family": 13, "algorithm_version": 1,
                                                   "rng_implementation": "det-pcg32"})
            random_key = metadata["stages"][0]["random_key"]
            self.assertEqual(random_key["mode"], "stream-id-v1")
            self.assertEqual(random_key["stream_id"], 101)
            self.assertEqual(random_key["index"], 0)
            self.assertEqual(len(random_key["effective_seed_hex"]), 16)
            exported = json.loads(target.read_text(encoding="utf-8"))
            self.assertEqual(exported["impairments"][0]["fading"], fading)
            if reference is None:
                reference = output.read_bytes()
                reference_sidecar = source["fading"]
                reference_random_key = random_key
            else:
                self.assertEqual(output.read_bytes(), reference)
                self.assertEqual(source["fading"], reference_sidecar)
                self.assertEqual(random_key, reference_random_key)

        replay_output = self.root / "fading-replay.wav"
        replay_sidecar = self.root / "fading-replay.json"
        replay = subprocess.run([str(RENDERER), "--scenario", str(target), "--source", str(self.source),
                                 "--output", str(replay_output), "--sidecar", str(replay_sidecar),
                                 "--block", "127"], capture_output=True, text=True, check=False, cwd=self.root)
        self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
        self.assertEqual(replay_output.read_bytes(), reference)

        changed_key, changed_output, changed_sidecar, _, _ = self.render(offset=0.0, block=127,
            prefix="fading-other-id", fading=fading, stream_id=102, seed=17)
        self.assertEqual(changed_key.returncode, 0, changed_key.stdout + changed_key.stderr)
        self.assertNotEqual(changed_output.read_bytes(), reference)
        changed_metadata = json.loads(changed_sidecar.read_text(encoding="utf-8"))
        self.assertNotEqual(changed_metadata["stages"][0]["random_key"], reference_random_key)

        shifted, shifted_output, shifted_sidecar, _, _ = self.render(offset=75.0, block=127,
            prefix="fading-shifted", fading=fading, stream_id=101, seed=17)
        self.assertEqual(shifted.returncode, 0, shifted.stdout + shifted.stderr)
        shifted_metadata = json.loads(shifted_sidecar.read_text(encoding="utf-8"))["secondary_source"]
        self.assertEqual(shifted_metadata["latency_frames"], 215)
        self.assertEqual(shifted_metadata["fading"]["rng_family"], 13)
        shifted_samples = read_pcm24(shifted_output)
        np.testing.assert_array_equal(shifted_samples[:2048], self.primary_q[:2048])

        early, early_output, _, _, _ = self.render(offset=0.0, block=127, start=0,
            prefix="fading-window-early", fading=fading, stream_id=101, seed=17)
        late, late_output, _, _, _ = self.render(offset=0.0, block=127, start=2048,
            prefix="fading-window-late", fading=fading, stream_id=101, seed=17)
        self.assertEqual(early.returncode, 0, early.stdout + early.stderr)
        self.assertEqual(late.returncode, 0, late.stdout + late.stderr)
        early_samples = read_pcm24(early_output)
        late_samples = read_pcm24(late_output)
        early_component = early_samples[:24000] - self.primary_q[:24000]
        late_component = late_samples[2048:2048 + 24000] - self.primary_q[2048:2048 + 24000]
        self.assertLessEqual(float(np.max(np.abs(early_component - late_component))), 1.1 / (1 << 23))

    def test_absent_fading_keeps_legacy_metadata_and_zero_offset_latency(self):
        result, output, sidecar, _, _ = self.render(offset=0.0, block=127, prefix="no-fading")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        source = json.loads(sidecar.read_text(encoding="utf-8"))["secondary_source"]
        self.assertEqual(source["latency_frames"], 0)
        self.assertNotIn("fading", source)
        samples = read_pcm24(output)
        np.testing.assert_array_equal(samples[:2048], self.primary_q[:2048])

    def test_markov_gate_is_window_relative_partitioned_and_does_not_freeze_fading(self):
        fading = {"spread_hz": 1.0}
        gate = {"mean_on_seconds": 0.001, "mean_off_seconds": 0.001}
        for offset in (0.0, 75.0, -75.0):
            baseline, baseline_output, _, _, _ = self.render(offset=offset, block=127,
                prefix=f"markov-baseline-{int(offset)}", fading=fading, stream_id=213, seed=41)
            self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
            baseline_samples = read_pcm24(baseline_output)
            reference = None
            for block in ((1, 127, 2048) if offset == 0.0 else (127,)):
                result, output, sidecar, target, _ = self.render(offset=offset, block=block,
                    prefix=f"markov-{int(offset)}-{block}", fading=fading, markov_on_off=gate,
                    stream_id=213, seed=41)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                samples = read_pcm24(output)
                active_start = 2048
                active_end = active_start + 24000
                np.testing.assert_array_equal(samples[:active_start], self.primary_q[:active_start])
                np.testing.assert_array_equal(samples[active_end:], self.primary_q[active_end:])
                active = samples[active_start:active_end]
                primary = self.primary_q[active_start:active_end]
                ungated = baseline_samples[active_start:active_end]
                off = active == primary
                on = ~off
                self.assertGreater(int(np.count_nonzero(off)), 1000)
                self.assertGreater(int(np.count_nonzero(on)), 1000)
                off_indices = np.flatnonzero(off)
                off_runs = np.split(off_indices, np.flatnonzero(np.diff(off_indices) > 1) + 1)
                longest_off = max(off_runs, key=len)
                self.assertGreaterEqual(len(longest_off), 8)
                self.assertTrue(np.any(ungated[longest_off] != primary[longest_off]))
                # ON output is bit-identical to the same source/filter/fade stream
                # with no gate; OFF output is exactly the primary, with no Hilbert tail.
                np.testing.assert_array_equal(active[on], ungated[on])
                self.assertEqual(int(np.count_nonzero(active[off] - primary[off])), 0)

                metadata = json.loads(sidecar.read_text(encoding="utf-8"))["secondary_source"]
                self.assertEqual(metadata["latency_frames"], 215)
                markov = metadata["markov_on_off"]
                for key, value in {"mean_on_seconds": 0.001, "mean_off_seconds": 0.001,
                                   "rng_family": 10, "algorithm_version": 1,
                                   "initial_state": "on", "timeline": "destination-window"}.items():
                    self.assertEqual(markov[key], value)
                self.assertEqual(metadata["fading"]["rng_family"], 13)
                exported = json.loads(target.read_text(encoding="utf-8"))["impairments"][0]
                self.assertEqual(exported["markov_on_off"], gate)
                self.assertEqual(exported["fading"], fading)
                if reference is None:
                    reference = output.read_bytes()
                else:
                    self.assertEqual(output.read_bytes(), reference)

        replay_output = self.root / "markov-replay.wav"
        replay_sidecar = self.root / "markov-replay.json"
        replay = subprocess.run([str(RENDERER), "--scenario", str(target), "--source", str(self.source),
                                 "--output", str(replay_output), "--sidecar", str(replay_sidecar),
                                 "--block", "2048"], capture_output=True, text=True, check=False, cwd=self.root)
        self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
        self.assertEqual(replay_output.read_bytes(), reference)

    def test_invalid_second_source_markov_gate_refuses_without_artifacts(self):
        bad_gate = [[], True, {}, {"mean_on_seconds": True, "mean_off_seconds": 0.001},
                    {"mean_on_seconds": 0.001}, {"mean_on_seconds": 0.0, "mean_off_seconds": 0.001},
                    {"mean_on_seconds": 0.001, "mean_off_seconds": 0.0},
                    {"mean_on_seconds": 1.0e-6, "mean_off_seconds": 0.001},
                    {"mean_on_seconds": 0.001, "mean_off_seconds": 0.001, "unknown": 1}]
        for index, gate in enumerate(bad_gate):
            with self.subTest(gate=gate):
                result, output, sidecar, target, _ = self.render(markov_on_off=gate,
                    prefix=f"bad-markov-{index}")
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())
                self.assertFalse(target.exists())

        raw_null = ('{"reference_rms":0.2,"source_gain_db":0,"clip_policy":"reject","impairments":['
                    '{"type":"second_source","source_path":"secondary input.wav","si_db":0,'
                    '"start_frame":2048,"source_start_frame":1000,"duration_frames":24000,'
                    '"frequency_offset_hz":0,"markov_on_off":null}]}')
        result, output, sidecar, target, _ = self.render(prefix="bad-markov-null", raw_scenario=raw_null)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertFalse(output.exists())
        self.assertFalse(sidecar.exists())
        self.assertFalse(target.exists())

    def test_fading_matches_retained_literal_pcm24_vector(self):
        frames = 96000
        index = np.arange(frames, dtype=np.int64)
        self.primary = np.zeros(frames, dtype=np.float64)
        self.secondary = 0.2 * np.cos(2.0 * np.pi * (index % 40) / 40.0)
        write_pcm24(self.source, self.primary)
        write_pcm24(self.secondary_path, self.secondary)
        self.primary_q = read_pcm24(self.source)
        self.secondary_q = read_pcm24(self.secondary_path)

        # Retained oracle: seed=17, stream_id=101, RNG family=13, spread=10 Hz,
        # silent 2 s primary, 1200 Hz / 0.2-amplitude cosine secondary, 12 dB S/I,
        # reference RMS 0.1, source/destination frame 0. Compare PCM24 integers
        # captured from the independent 600 s retained render at frame 48000.
        expected_pcm24 = [154035, 131556, 105866, 77604, 47474, 16224, -15372, -46533,
                          -76489, -104502, -129883, -152009, -170340, -184428, -193934, -198629]
        for block in (127, 2048):
            result, output, _, _, _ = self.render(offset=0.0, block=block, duration=frames,
                source_start=0, start=0, reference_rms=0.1, si_db=12.0,
                prefix=f"fading-known-answer-{block}", fading={"spread_hz": 10.0},
                stream_id=101, seed=17)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            observed_pcm24 = np.rint(read_pcm24(output)[48000:48016] * (1 << 23)).astype(np.int32).tolist()
            self.assertEqual(observed_pcm24, expected_pcm24)

    def test_invalid_second_source_fading_refuses_without_artifacts(self):
        bad_fading = [[], True, {"spread_hz": True}, {"spread_hz": 0.0},
                      {"spread_hz": 10.0001}, {"spread_hz": 1.0, "unknown": 2}]
        for index, fading in enumerate(bad_fading):
            with self.subTest(fading=fading):
                result, output, sidecar, target, _ = self.render(fading=fading,
                    prefix=f"bad-fading-{index}")
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())
                self.assertFalse(target.exists())

        raw_null = ('{"reference_rms":0.2,"source_gain_db":0,"clip_policy":"reject","impairments":['
                    '{"type":"second_source","source_path":"secondary input.wav","si_db":0,'
                    '"start_frame":2048,"source_start_frame":1000,"duration_frames":24000,'
                    '"frequency_offset_hz":0,"fading":null}]}')
        result, output, sidecar, target, _ = self.render(prefix="bad-fading-null", raw_scenario=raw_null)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertFalse(output.exists())
        self.assertFalse(sidecar.exists())
        self.assertFalse(target.exists())

    def test_bad_secondary_sources_refuse_without_publishing_artifacts(self):
        silent = self.scenario_dir / "silent.wav"
        write_pcm24(silent, np.zeros(4096))
        truncated = self.scenario_dir / "truncated.wav"
        truncated.write_bytes(self.secondary_path.read_bytes()[:-6])
        cases = [("missing.wav", 0, 100), ("silent.wav", 0, 100), ("truncated.wav", 0, 100),
                 ("secondary input.wav", 47000, 2000)]
        for path, source_start, duration in cases:
            with self.subTest(path=path, source_start=source_start):
                result, output, sidecar, target, _ = self.render(source_path=path, source_start=source_start,
                                                                 duration=duration)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())
                self.assertFalse(target.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
