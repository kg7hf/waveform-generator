#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Measure swept CW from PCM24, independently of the engine's NCO truth."""

from __future__ import annotations

import argparse
import copy
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
SCALE = 1 << 23
RENDERER: Path
WATTERSON_RENDERER: Path
MEASUREMENTS: list[dict] = []


def read_pcm24(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("expected mono PCM24 at 48 kHz")
        packed = np.frombuffer(reader.readframes(reader.getnframes()), dtype=np.uint8).reshape(-1, 3)
    codes = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
    return ((codes ^ 0x800000) - 0x800000).astype(np.float64) / SCALE


def triangle_oracle(frames: np.ndarray, low: float, high: float, slope: float) -> tuple[np.ndarray, np.ndarray]:
    """Closed-form commanded triangle and its integral; no engine phase-word code."""
    times = frames / RATE
    half_period = (high - low) / slope
    cycles = np.floor(times / (2 * half_period))
    within = times - cycles * (2 * half_period)
    ascending = within <= half_period
    descending_time = within - half_period
    frequency = np.where(ascending, low + slope * within, high - slope * descending_time)
    integral = cycles * (low + high) * half_period
    integral += np.where(ascending, low * within + 0.5 * slope * within * within,
                         0.5 * (low + high) * half_period + high * descending_time
                         - 0.5 * slope * descending_time * descending_time)
    return frequency, integral


def analytic_signal(samples: np.ndarray) -> np.ndarray:
    spectrum = np.fft.fft(samples)
    weights = np.zeros(len(samples))
    weights[0] = 1
    weights[1:(len(samples) + 1) // 2] = 2
    if len(samples) % 2 == 0:
        weights[len(samples) // 2] = 1
    return np.fft.ifft(spectrum * weights)


def scenario(slope: float = 300.0) -> dict:
    return {"schema": "signal-lab.scenario/1", "test_id": "CW-SWEEP-render", "seed": 17,
            "source_gain_db": 0, "reference_rms": 0.1, "clip_policy": "reject",
            "impairments": [{"type": "cw", "ci_db": 12, "phase_degrees": 37, "ramp_seconds": 0,
                             "frequency_sweep": {"low_hz": 300, "high_hz": 3300,
                                                 "slope_hz_per_second": slope}}]}


class CwSweepRenderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.wav"

    def source_for(self, seconds: float) -> None:
        # Exact binary source level permits independent subtraction after mixing.
        with wave.open(str(self.source), "wb") as writer:
            writer.setparams((1, 3, RATE, 0, "NONE", "not compressed"))
            writer.writeframes((SCALE // 16).to_bytes(3, "little", signed=True) * round(seconds * RATE))

    def render(self, renderer: Path, document: dict, name: str, block: int = 2048, *, export: bool = False):
        config = self.root / f"{name}.json"
        output = self.root / f"{name}.wav"
        sidecar = self.root / f"{name}.sidecar.json"
        target = self.root / f"{name}.target.json"
        config.write_text(json.dumps(document), encoding="utf-8")
        command = [str(renderer), "--scenario", str(config), "--source", str(self.source),
                   "--output", str(output), "--sidecar", str(sidecar), "--block", str(block)]
        if export:
            command += ["--target-scenario", str(target)]
        result = subprocess.run(command, capture_output=True, text=True, cwd=self.root, check=False,
                                creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
        return result, output, sidecar, target

    def test_full_default_cycle_measured_frequency_phase_and_ci(self) -> None:
        self.source_for(20)
        result, output, sidecar, _ = self.render(RENDERER, scenario(), "full")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        tone = read_pcm24(output) - 1 / 16
        frames = np.arange(len(tone), dtype=np.float64)
        frequency, cycles = triangle_oracle(frames, 300, 3300, 300)
        amplitude = math.sqrt(2) * 0.1 * 10 ** (-12 / 20)
        expected = amplitude * np.sin(2 * np.pi * cycles + math.radians(37))
        max_pcm_error = float(np.max(np.abs(tone - expected)) * SCALE)
        self.assertLessEqual(max_pcm_error, 3.0)
        measured_ci = 20 * math.log10(0.1 / float(np.sqrt(np.mean(tone * tone))))
        self.assertLessEqual(abs(measured_ci - 12), 0.1)

        # The periodic full cycle avoids an artificial FFT endpoint jump.
        # A symmetric 16-frame phase difference measures the local average;
        # compare with the independently integrated average at the same frames.
        phase = np.unwrap(np.angle(analytic_signal(tone)))
        width = 8
        measured_frequency = (phase[2 * width:] - phase[:-2 * width]) * RATE / (4 * np.pi * width)
        expected_frequency = (cycles[2 * width:] - cycles[:-2 * width]) * RATE / (2 * width)
        relative_error = np.abs(measured_frequency - expected_frequency) / expected_frequency
        max_frequency_error = float(np.max(relative_error))
        self.assertLessEqual(max_frequency_error, 0.001)
        self.assertAlmostEqual(float(frequency[0]), 300)
        self.assertAlmostEqual(float(frequency[10 * RATE]), 3300)
        report = json.loads(sidecar.read_text(encoding="utf-8"))["stages"][0]
        self.assertEqual(report["sweep_algorithm_version"], 1)
        self.assertEqual(report["frequency_sweep"]["low_hz"], 300)
        self.assertEqual(report["frequency_sweep"]["high_hz"], 3300)
        self.assertAlmostEqual(report["frequency_sweep"]["realized_period_seconds"], 20, places=8)
        MEASUREMENTS.append({"full_cycle_seconds": 20, "max_pcm_error_lsb": max_pcm_error,
                             "max_measured_frequency_relative_error": max_frequency_error,
                             "measured_ci_db": measured_ci})

    def test_fractional_turn_pulses_partitions_renderer_parity_and_export(self) -> None:
        self.source_for(2.1)
        document = scenario(3007)
        cw = document["impairments"][0]
        cw.update({"start_seconds": 0.003, "duration_seconds": 2.05,
                   "pulse_period_seconds": 0.01, "pulse_duty_cycle": 0.4, "ramp_seconds": 0.0005})
        outputs = []
        for name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
            for block in (1, 127, 2048):
                result, output, sidecar, target = self.render(renderer, document, f"{name}-{block}", block,
                                                              export=name == "normal" and block == 127)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                outputs.append(output.read_bytes())
                report = json.loads(sidecar.read_text(encoding="utf-8"))["stages"][0]
                self.assertEqual(report["frequency_sweep"]["slope_hz_per_second"], 3007)
                self.assertEqual(report["pulse_on_frames"], 192)
                if target.exists():
                    exported = json.loads(target.read_text(encoding="utf-8"))
                    self.assertEqual(exported["impairments"], document["impairments"])
                    replay, replay_output, _, _ = self.render(RENDERER, exported, "replay", 127)
                    self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
                    outputs.append(replay_output.read_bytes())
        self.assertTrue(all(content == outputs[0] for content in outputs[1:]))

    def test_ambiguous_or_malformed_sweep_refuses_before_output(self) -> None:
        self.source_for(0.01)
        malformed = []
        ambiguous = scenario()
        ambiguous["impairments"][0]["frequency_hz"] = 300
        malformed.append(ambiguous)
        for field, value in (("high_hz", 299), ("low_hz", True), ("slope_hz_per_second", 0), ("extra", 1)):
            document = copy.deepcopy(scenario())
            document["impairments"][0]["frequency_sweep"][field] = value
            malformed.append(document)
        for index, document in enumerate(malformed):
            for name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                result, output, sidecar, _ = self.render(renderer, document, f"bad-{name}-{index}")
                self.assertNotEqual(result.returncode, 0)
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
