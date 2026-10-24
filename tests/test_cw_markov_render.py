#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Measure Markov-gated CW timing and renderer parity from PCM24 output."""

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

for _variable in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
    os.environ[_variable] = "1"

import numpy as np

RATE = 48_000
PCM24_SCALE = 1 << 23
RENDERER: Path
WATTERSON_RENDERER: Path
MEASUREMENTS: list[dict] = []


def pcm24_chunks(path: Path, chunk_frames: int = 65_536):
    """Yield bounded signed-float PCM24 chunks from a mono 48-kHz WAV."""
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("expected mono PCM24 at 48 kHz")
        remaining = reader.getnframes()
        while remaining:
            count = min(remaining, chunk_frames)
            raw = reader.readframes(count)
            if len(raw) != count * 3:
                raise AssertionError("short PCM24 read")
            packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
            codes = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
            signed = (codes ^ 0x800000) - 0x800000
            yield signed.astype(np.float64) / PCM24_SCALE
            remaining -= count


def scenario(*, swept: bool = False, seed: int = 17, long_run: bool = False) -> dict:
    cw = {
        "type": "cw",
        "ci_db": 12,
        "phase_degrees": 45,
        "ramp_seconds": 0,
        "markov_on_off": {"mean_on_seconds": 0.005, "mean_off_seconds": 0.008},
    }
    if swept:
        cw["frequency_sweep"] = {"low_hz": 11000, "high_hz": 13000, "slope_hz_per_second": 2000}
    else:
        cw["frequency_hz"] = 12000
    if not long_run:
        cw.update({"start_seconds": 0.003, "duration_seconds": 0.25, "ramp_seconds": 0.0005})
    return {"schema": "signal-lab.scenario/1", "test_id": "CW-MARKOV-render", "seed": seed,
            "source_gain_db": 0, "reference_rms": 0.1, "clip_policy": "reject", "impairments": [cw]}


class CwMarkovRenderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.wav"

    def write_source(self, seconds: float) -> None:
        frames = round(seconds * RATE)
        with wave.open(str(self.source), "wb") as writer:
            writer.setparams((1, 3, RATE, 0, "NONE", "not compressed"))
            writer.writeframes((PCM24_SCALE // 16).to_bytes(3, "little", signed=True) * frames)

    def render(self, renderer: Path, document: dict, name: str, block: int = 2048, *,
               silence_seconds: float | None = None, export: bool = False):
        config = self.root / f"{name}.json"
        output = self.root / f"{name}.wav"
        sidecar = self.root / f"{name}.sidecar.json"
        target = self.root / f"{name}.target.json"
        config.write_text(json.dumps(document), encoding="utf-8")
        command = [str(renderer), "--scenario", str(config), "--output", str(output),
                   "--sidecar", str(sidecar), "--block", str(block)]
        if silence_seconds is None:
            command += ["--source", str(self.source)]
        else:
            command += ["--silence-seconds", str(silence_seconds), "--reference-rms", "0.1"]
        if export:
            command += ["--target-scenario", str(target)]
        result = subprocess.run(command, capture_output=True, text=True, cwd=self.root, check=False,
                                creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
        return result, output, sidecar, target

    def assert_gate_metadata(self, sidecar: Path) -> dict:
        stage = json.loads(sidecar.read_text(encoding="utf-8"))["stages"][0]
        self.assertEqual(stage["type"], "cw")
        gate = stage["markov_on_off"]
        self.assertAlmostEqual(gate["mean_on_seconds"], 0.005, places=15)
        self.assertAlmostEqual(gate["mean_off_seconds"], 0.008, places=15)
        self.assertAlmostEqual(gate["mean_on_frames"], 240.0, places=12)
        self.assertAlmostEqual(gate["mean_off_frames"], 384.0, places=12)
        self.assertEqual(gate["initial_state"], "on")
        self.assertEqual(gate["dwell_distribution"], "geometric")
        self.assertEqual(gate["rng_family"], 10)
        self.assertEqual(gate["algorithm_version"], 1)
        return gate

    def test_long_silence_render_has_geometric_dwells_calibrated_tone_and_true_off_silence(self) -> None:
        document = scenario(long_run=True)
        result, output, sidecar, _ = self.render(WATTERSON_RENDERER, document, "long", 2048,
                                                 silence_seconds=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        gate = self.assert_gate_metadata(sidecar)
        self.assertEqual(gate["initial_state"], "on")

        threshold = 0.5 * 0.1 * 10 ** (-12 / 20)
        runs: list[tuple[bool, int]] = []
        current_state: bool | None = None
        current_length = 0
        on_energy = 0.0
        on_count = 0
        off_count = 0
        off_nonzero = 0
        frame_count = 0

        for samples in pcm24_chunks(output):
            on_mask = np.abs(samples) > threshold
            on_values = samples[on_mask]
            on_energy += float(np.dot(on_values, on_values))
            on_count += len(on_values)
            off_values = samples[~on_mask]
            off_count += len(off_values)
            off_nonzero += int(np.count_nonzero(off_values))
            edges = np.flatnonzero(on_mask[1:] != on_mask[:-1]) + 1
            boundaries = np.concatenate((np.array([0]), edges, np.array([len(on_mask)])))
            for first, last in zip(boundaries[:-1], boundaries[1:], strict=True):
                state = bool(on_mask[first])
                length = int(last - first)
                if current_state is None:
                    current_state = state
                    current_length = length
                elif state == current_state:
                    current_length += length
                else:
                    runs.append((current_state, current_length))
                    current_state = state
                    current_length = length
            frame_count += len(samples)

        if current_state is not None:
            runs.append((current_state, current_length))

        self.assertEqual(frame_count, 300 * RATE)
        self.assertGreaterEqual(len(runs) - 1, 10_000)
        self.assertTrue(runs[0][0], "Markov gate must start ON")
        self.assertEqual(off_nonzero, 0, "OFF-state output must be digital silence")
        self.assertGreater(on_count, 0)
        self.assertGreater(off_count, 0)

        # The first ON and final run are censored by the finite render; omit both.
        complete = runs[1:-1]
        on_dwells = np.asarray([length for state, length in complete if state], dtype=np.float64)
        off_dwells = np.asarray([length for state, length in complete if not state], dtype=np.float64)
        self.assertGreater(len(on_dwells), 10_000)
        self.assertGreater(len(off_dwells), 10_000)
        mean_on = float(np.mean(on_dwells))
        mean_off = float(np.mean(off_dwells))
        self.assertLessEqual(abs(mean_on / 240.0 - 1.0), 0.02)
        self.assertLessEqual(abs(mean_off / 384.0 - 1.0), 0.02)

        cdf_errors: dict[str, float] = {}
        for label, dwells, mean_frames in (("on", on_dwells, 240), ("off", off_dwells, 384)):
            for multiple in (0.5, 1.0, 2.0):
                bound = math.floor(multiple * mean_frames)
                measured = float(np.mean(dwells <= bound))
                expected = 1.0 - (1.0 - 1.0 / mean_frames) ** bound
                error = abs(measured - expected)
                cdf_errors[f"{label}_{multiple:g}_mean"] = error
                self.assertLessEqual(error, 0.02, (label, multiple, measured, expected))

        measured_ci = 20.0 * math.log10(0.1 / math.sqrt(on_energy / on_count))
        self.assertLessEqual(abs(measured_ci - 12.0), 0.1)
        MEASUREMENTS.append({"long_run_frames": frame_count, "run_count_including_censored": len(runs),
                             "mean_on_frames": mean_on, "mean_off_frames": mean_off,
                             "on_state_ci_db": measured_ci, "off_nonzero_samples": off_nonzero,
                             "geometric_cdf_abs_errors": cdf_errors})

    def test_short_fixed_and_swept_gate_match_across_renderers_blocks_export_and_seed(self) -> None:
        self.write_source(0.3)
        for swept in (False, True):
            document = scenario(swept=swept)
            outputs: list[bytes] = []
            target: Path | None = None
            for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                for block in (1, 127, 2048):
                    export = renderer_name == "normal" and block == 127
                    result, output, sidecar, target_path = self.render(renderer, document,
                                                                        f"{'sweep' if swept else 'fixed'}-{renderer_name}-{block}",
                                                                        block, export=export)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assert_gate_metadata(sidecar)
                    outputs.append(output.read_bytes())
                    if export:
                        target = target_path

            self.assertTrue(all(data == outputs[0] for data in outputs[1:]))
            self.assertIsNotNone(target)
            exported = json.loads(target.read_text(encoding="utf-8"))
            self.assertIn("markov_on_off", exported["impairments"][0])
            replay, replay_output, replay_sidecar, _ = self.render(RENDERER, exported,
                                                                    f"{'sweep' if swept else 'fixed'}-replay", 127)
            self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
            self.assertEqual(replay_output.read_bytes(), outputs[0])
            self.assert_gate_metadata(replay_sidecar)

            changed_seed = dict(document)
            changed_seed["seed"] = 18
            different, different_output, _, _ = self.render(RENDERER, changed_seed,
                                                             f"{'sweep' if swept else 'fixed'}-seed18", 127)
            self.assertEqual(different.returncode, 0, different.stdout + different.stderr)
            self.assertNotEqual(different_output.read_bytes(), outputs[0])

    def test_malformed_or_periodic_pulse_combination_refuses_without_outputs(self) -> None:
        self.write_source(0.01)
        malformed = []
        for key, value in (("mean_on_seconds", 0), ("mean_off_seconds", True), ("future", 1)):
            document = scenario()
            document["impairments"][0]["markov_on_off"][key] = value
            malformed.append(document)
        combined = scenario()
        combined["impairments"][0].update({"pulse_period_seconds": 0.01, "pulse_duty_cycle": 0.5})
        malformed.append(combined)

        for index, document in enumerate(malformed):
            for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                name = f"invalid-{renderer_name}-{index}"
                result, output, sidecar, _ = self.render(renderer, document, name)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--watterson-renderer", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    WATTERSON_RENDERER = arguments.watterson_renderer.resolve(strict=True)
    suite = unittest.main(argv=[__file__, *remaining], exit=False)
    print(json.dumps({"measurements": MEASUREMENTS}, sort_keys=True))
    raise SystemExit(0 if suite.result.wasSuccessful() else 1)
