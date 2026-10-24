#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Measure CW Rayleigh fading from PCM24 and compare both C++ renderers."""

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
SOURCE_LEVEL = 1 / 16
REFERENCE_RMS = 0.1
CI_DB = 12.0
AMPLITUDE_PEAK = math.sqrt(2) * REFERENCE_RMS * 10 ** (-CI_DB / 20)
DECIMATION_FRAMES = RATE // 2
LONG_SECONDS = 600
RENDERER: Path
WATTERSON_RENDERER: Path
MEASUREMENTS: list[dict] = []


def pcm24_chunks(path: Path, chunk_frames: int = RATE * 2):
    """Yield bounded signed-float PCM24 chunks at global 48-kHz frame offsets."""
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("expected mono PCM24 at 48 kHz")
        frame = 0
        remaining = reader.getnframes()
        while remaining:
            count = min(remaining, chunk_frames)
            raw = reader.readframes(count)
            if len(raw) != count * 3:
                raise AssertionError("short PCM24 read")
            packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
            codes = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
            samples = (codes ^ 0x800000) - 0x800000
            yield frame, samples.astype(np.float64) / SCALE
            frame += count
            remaining -= count


def faded_scenario(*, mode: str = "fixed", gate: str | None = None, seed: int = 17) -> dict:
    cw: dict = {
        "type": "cw",
        "ci_db": CI_DB,
        "phase_degrees": 0,
        "ramp_seconds": 0,
        "fading": {"spread_hz": 10},
    }
    if mode == "sweep":
        cw["frequency_sweep"] = {"low_hz": 11_000, "high_hz": 13_000,
                                 "slope_hz_per_second": 2000}
    else:
        cw["frequency_hz"] = 12_000
    if mode == "fsk":
        cw["fsk"] = {"baud": 300, "shift_hz": 300, "pattern": "alternating"}
    if gate == "pulse":
        cw.update({"pulse_period_seconds": 0.01, "pulse_duty_cycle": 0.5})
    elif gate == "markov":
        cw["markov_on_off"] = {"mean_on_seconds": 0.005, "mean_off_seconds": 0.008}
    if mode != "long":
        cw.update({"start_seconds": 0.003, "duration_seconds": 0.25, "ramp_seconds": 0.0005})
    return {
        "schema": "signal-lab.scenario/1",
        "test_id": "CW-faded-render",
        "seed": seed,
        "source_gain_db": 0,
        "reference_rms": REFERENCE_RMS,
        "clip_policy": "reject",
        "impairments": [cw],
    }


class CwFadedRenderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.wav"

    def write_source(self, seconds: int | float, level: float = SOURCE_LEVEL) -> None:
        frames = round(seconds * RATE)
        sample = round(level * SCALE).to_bytes(3, "little", signed=True)
        with wave.open(str(self.source), "wb") as writer:
            writer.setparams((1, 3, RATE, 0, "NONE", "not compressed"))
            remaining = frames
            while remaining:
                count = min(remaining, 65_536)
                writer.writeframesraw(sample * count)
                remaining -= count

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

    def assert_fading_metadata(self, sidecar: Path) -> dict:
        stage = json.loads(sidecar.read_text(encoding="utf-8"))["stages"][0]
        self.assertEqual(stage["type"], "cw")
        fading = stage["fading"]
        self.assertEqual(fading["spread_hz"], 10)
        self.assertEqual(fading["algorithm_version"], 1)
        self.assertEqual(fading["rng_family"], 12)
        self.assertEqual(fading["mean_power"], 1.0)
        self.assertEqual(fading["distribution"], "rayleigh")
        self.assertEqual(fading["rng_implementation"], "det-pcg32")
        return fading

    def test_family12_pcm24_known_answer_vector(self) -> None:
        self.write_source(0.005, level=0.0)
        document = faded_scenario(mode="long")
        expected = [-62410, -28437, 62510, 28093, -62610, -27749, 62710, 27405,
                    -62810, -27061, 62910, 26717, -63010, -26373, 63110, 26029]
        for name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
            result, output, _, _ = self.render(renderer, document, f"known-answer-{name}", block=127)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with wave.open(str(output), "rb") as reader:
                packed = reader.readframes(len(expected))
            actual = [int.from_bytes(packed[offset:offset + 3], "little", signed=True)
                      for offset in range(0, len(packed), 3)]
            self.assertEqual(actual, expected)

    def test_long_render_matches_independent_rayleigh_moments_cdf_and_ci(self) -> None:
        self.write_source(LONG_SECONDS)
        document = faded_scenario(mode="long")
        result, output, sidecar, _ = self.render(WATTERSON_RENDERER, document, "long-fading", block=2048)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_fading_metadata(sidecar)

        total_square = 0.0
        total_frames = 0
        real_parts: list[np.ndarray] = []
        imag_parts: list[np.ndarray] = []
        for first, samples in pcm24_chunks(output):
            tone = samples - SOURCE_LEVEL
            total_square += float(np.dot(tone, tone))
            total_frames += len(tone)

            # At 12 kHz and phase zero, consecutive tone samples expose imag(g),
            # real(g), -imag(g), -real(g). Pairing introduces at most a one-frame
            # skew in the slowly varying 10-Hz process.
            positions = np.arange(0, len(tone), DECIMATION_FRAMES, dtype=np.int64)
            valid = positions + 1 < len(tone)
            positions = positions[valid]
            real_parts.append(tone[positions + 1] / AMPLITUDE_PEAK)
            imag_parts.append(tone[positions] / AMPLITUDE_PEAK)

        self.assertEqual(total_frames, LONG_SECONDS * RATE)
        measured_rms = math.sqrt(total_square / total_frames)
        measured_ci = 20 * math.log10(REFERENCE_RMS / measured_rms)
        self.assertLessEqual(abs(measured_ci - CI_DB), 0.1)

        real = np.concatenate(real_parts)
        imag = np.concatenate(imag_parts)
        powers = real * real + imag * imag
        magnitudes = np.sqrt(powers)
        sample_count = len(powers)
        self.assertEqual(sample_count, LONG_SECONDS * RATE // DECIMATION_FRAMES)

        mean_power = float(np.mean(powers))
        fourth_moment_ratio = float(np.mean(powers * powers) / (mean_power * mean_power))
        mean_over_rms = float(np.mean(magnitudes) / math.sqrt(mean_power))
        ordered = np.sort(magnitudes)
        empirical = np.arange(1, sample_count + 1, dtype=np.float64) / sample_count
        theoretical = -np.expm1(-(ordered * ordered))
        ks_distance = float(max(np.max(empirical - theoretical),
                                np.max(theoretical - (empirical - 1 / sample_count))))

        # Half-second spacing at a 10-Hz spread gives 1200 observations. This is
        # over 300 FIR-node intervals, so samples share no Gaussian draws from
        # the 91-node filter support. Pairing adds at most one frame of skew.
        # Bounds are five sampling standard errors for the first three moments;
        # KS uses the 99% threshold.
        self.assertLessEqual(abs(mean_power - 1.0), 5 / math.sqrt(sample_count))
        self.assertLessEqual(abs(fourth_moment_ratio - 2.0), 10 / math.sqrt(sample_count))
        self.assertLessEqual(abs(mean_over_rms - math.sqrt(math.pi) / 2),
                             5 * math.sqrt(1 - 5 * math.pi / 16) / math.sqrt(sample_count))
        self.assertLessEqual(ks_distance, 1.63 / math.sqrt(sample_count))
        MEASUREMENTS.append({"frames": total_frames, "duration_seconds": LONG_SECONDS,
                             "decimated_samples": sample_count,
                             "decimation_seconds": DECIMATION_FRAMES / RATE,
                             "measured_ci_db": measured_ci, "mean_complex_power": mean_power,
                             "normalized_fourth_moment": fourth_moment_ratio,
                             "mean_envelope_over_rms": mean_over_rms,
                             "rayleigh_ks_distance": ks_distance,
                             "rayleigh_ks_99_limit": 1.63 / math.sqrt(sample_count)})

    def test_modes_gates_renderers_partitions_and_export_replay_match(self) -> None:
        self.write_source(0.3)
        combinations = tuple((mode, gate) for mode in ("fixed", "sweep", "fsk")
                              for gate in (None, "pulse", "markov"))
        for mode, gate in combinations:
            document = faded_scenario(mode=mode, gate=gate)
            outputs: list[bytes] = []
            target: Path | None = None
            for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                for block in (1, 127, 2048):
                    name = f"{mode}-{gate or 'none'}-{renderer_name}-{block}"
                    export = renderer_name == "normal" and block == 127
                    result, output, sidecar, target_path = self.render(renderer, document, name,
                                                                        block=block, export=export)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assert_fading_metadata(sidecar)
                    outputs.append(output.read_bytes())
                    if export:
                        target = target_path

            self.assertTrue(all(data == outputs[0] for data in outputs[1:]))
            self.assertIsNotNone(target)
            replayed = json.loads(target.read_text(encoding="utf-8"))
            self.assertEqual(replayed["impairments"][0]["fading"],
                             document["impairments"][0]["fading"])
            replay, replay_output, replay_sidecar, _ = self.render(RENDERER, replayed,
                                                                   f"{mode}-{gate or 'none'}-replay", block=127)
            self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
            self.assertEqual(replay_output.read_bytes(), outputs[0])
            self.assert_fading_metadata(replay_sidecar)

        base = faded_scenario()
        changed_seed = copy.deepcopy(base)
        changed_seed["seed"] = 18
        original_result, original_output, _, _ = self.render(RENDERER, base, "seed17")
        changed_result, changed_output, _, _ = self.render(RENDERER, changed_seed, "seed18")
        self.assertEqual(original_result.returncode, 0, original_result.stdout + original_result.stderr)
        self.assertEqual(changed_result.returncode, 0, changed_result.stdout + changed_result.stderr)
        self.assertNotEqual(original_output.read_bytes(), changed_output.read_bytes())

    def test_fading_is_additive_and_window_outside_samples_remain_source(self) -> None:
        self.write_source(0.3)
        document = faded_scenario()
        self.write_source(0.3, level=0.0)
        zero_result, zero_output, zero_sidecar, _ = self.render(RENDERER, document, "source-zero", block=127)
        self.assertEqual(zero_result.returncode, 0, zero_result.stdout + zero_result.stderr)
        self.assert_fading_metadata(zero_sidecar)
        zero_samples = np.concatenate([chunk for _, chunk in pcm24_chunks(zero_output, chunk_frames=RATE)])

        self.write_source(0.3, level=SOURCE_LEVEL)
        result, output, sidecar, _ = self.render(RENDERER, document, "source-plus", block=127)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_fading_metadata(sidecar)
        samples = np.concatenate([chunk for _, chunk in pcm24_chunks(output, chunk_frames=RATE)])
        start = round(0.003 * RATE)
        end = start + round(0.25 * RATE)
        self.assertTrue(np.all(samples[:start] == SOURCE_LEVEL))
        self.assertTrue(np.all(samples[end:] == SOURCE_LEVEL))
        self.assertGreater(float(np.max(np.abs(samples[start:end] - SOURCE_LEVEL))), AMPLITUDE_PEAK * 0.1)
        max_additivity_error_lsb = float(np.max(np.abs((samples - zero_samples) - SOURCE_LEVEL)) * SCALE)
        self.assertLessEqual(max_additivity_error_lsb, 1.0)
        MEASUREMENTS.append({"source_isolation_max_additivity_error_lsb": max_additivity_error_lsb})

    def test_gate_off_advances_fader_without_changing_source_or_restarting_gain(self) -> None:
        self.write_source(0.3, level=0.0)
        for mode in ("fixed", "sweep", "fsk"):
            base = faded_scenario(mode=mode)
            reference_result, reference_output, _, _ = self.render(RENDERER, base,
                                                                    f"{mode}-ungated-reference", block=127)
            self.assertEqual(reference_result.returncode, 0, reference_result.stdout + reference_result.stderr)
            reference = np.concatenate([chunk for _, chunk in pcm24_chunks(reference_output, chunk_frames=RATE)])
            start = round(0.003 * RATE)
            end = start + round(0.25 * RATE)
            meaningful = np.abs(reference) > AMPLITUDE_PEAK * 0.2
            frames = np.arange(len(reference))

            for gate in ("pulse", "markov"):
                document = faded_scenario(mode=mode, gate=gate)
                result, output, sidecar, _ = self.render(RENDERER, document,
                                                         f"{mode}-{gate}-continuity", block=127)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_fading_metadata(sidecar)
                gated = np.concatenate([chunk for _, chunk in pcm24_chunks(output, chunk_frames=RATE)])
                source_equal = gated[start:end] == 0.0
                equal_indices = np.flatnonzero(source_equal) + start
                equal_runs = np.split(equal_indices, np.flatnonzero(np.diff(equal_indices) > 1) + 1)
                off_plateaus = [run for run in equal_runs if len(run) >= 48]
                self.assertTrue(off_plateaus, f"{mode} {gate} gate must have a genuine OFF plateau")
                off_end = int(off_plateaus[0][-1])
                after_off = np.flatnonzero((gated == reference) & meaningful &
                                           (frames > off_end) & (frames < end))
                self.assertGreater(len(after_off), 500)
                self.assertTrue(np.all(gated[:start] == 0.0))
                self.assertTrue(np.all(gated[end:] == 0.0))
                MEASUREMENTS.append({"mode": mode, "gate": gate,
                                     "off_plateau_frames": len(off_plateaus[0]),
                                     "meaningful_gain_matches_after_off": len(after_off)})

    def test_invalid_fading_is_refused_before_either_renderer_creates_outputs(self) -> None:
        self.write_source(0.01)
        malformed: list[dict] = []
        for value in (0, 0.099, 10.001, True, None, "1"):
            document = faded_scenario()
            document["impairments"][0]["fading"]["spread_hz"] = value
            malformed.append(document)
        unknown = faded_scenario()
        unknown["impairments"][0]["fading"]["future"] = 1
        malformed.append(unknown)
        missing = faded_scenario()
        missing["impairments"][0]["fading"] = {}
        malformed.append(missing)

        for index, document in enumerate(malformed):
            for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                result, output, sidecar, _ = self.render(renderer, document,
                                                         f"invalid-{renderer_name}-{index}")
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
