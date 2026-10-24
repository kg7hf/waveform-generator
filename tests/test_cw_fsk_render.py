#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Measure two-tone CW FSK phase, baud, calibration and renderer parity from PCM24."""

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
SYMBOL_FRAMES = 160
MARK_HZ = 13_500
SPACE_HZ = 10_500
AMPLITUDE_PEAK = math.sqrt(2) * 0.1 * 10 ** (-12 / 20)


def read_pcm24(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("expected mono PCM24 at 48 kHz")
        packed = np.frombuffer(reader.readframes(reader.getnframes()), dtype=np.uint8).reshape(-1, 3)
    codes = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
    return ((codes ^ 0x800000) - 0x800000).astype(np.float64) / SCALE


def fsk_scenario(*, pattern: str = "alternating", seed: int = 17) -> dict:
    return {
        "schema": "signal-lab.scenario/1",
        "test_id": "CW-FSK-render",
        "seed": seed,
        "source_gain_db": 0,
        "reference_rms": 0.1,
        "clip_policy": "reject",
        "impairments": [{
            "type": "cw",
            "frequency_hz": 12_000,
            "ci_db": 12,
            "phase_degrees": 45,
            "ramp_seconds": 0,
            "fsk": {"baud": 300, "shift_hz": 3000, "pattern": pattern},
        }],
    }


def expected_alternating_tone(frame_count: int) -> np.ndarray:
    frames = np.arange(frame_count, dtype=np.int64)
    symbol = frames // SYMBOL_FRAMES
    within_symbol = frames % SYMBOL_FRAMES
    frequency = np.where(symbol % 2 == 0, MARK_HZ, SPACE_HZ)
    pair_cycles = (symbol // 2) * 80.0
    symbol_cycles = np.where(symbol % 2 == 0, 0.0, 45.0)
    cycles = pair_cycles + symbol_cycles + frequency * within_symbol / RATE
    return AMPLITUDE_PEAK * np.sin(2 * np.pi * cycles + math.pi / 4)


class CwFskRenderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.wav"

    def write_source(self, seconds: float) -> None:
        frames = round(seconds * RATE)
        with wave.open(str(self.source), "wb") as writer:
            writer.setparams((1, 3, RATE, 0, "NONE", "not compressed"))
            writer.writeframes((SCALE // 16).to_bytes(3, "little", signed=True) * frames)

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

    def assert_metadata(self, sidecar: Path, pattern: str) -> dict:
        stage = json.loads(sidecar.read_text(encoding="utf-8"))["stages"][0]
        self.assertEqual(stage["type"], "cw")
        fsk = stage["fsk"]
        self.assertEqual(fsk["baud"], 300)
        self.assertEqual(fsk["shift_hz"], 3000)
        self.assertEqual(fsk["mark_hz"], MARK_HZ)
        self.assertEqual(fsk["space_hz"], SPACE_HZ)
        self.assertEqual(fsk["frames_per_symbol"], SYMBOL_FRAMES)
        self.assertEqual(fsk["pattern"], pattern)
        self.assertEqual(fsk["rng_family"], 11)
        self.assertEqual(fsk["algorithm_version"], 1)
        return fsk

    def test_alternating_fsk_pcm_matches_independent_phase_and_tone_oracle(self) -> None:
        self.write_source(2.0)
        result, output, sidecar, _ = self.render(RENDERER, fsk_scenario(), "analytic", block=2048)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_metadata(sidecar, "alternating")

        tone = read_pcm24(output) - 1 / 16
        expected = expected_alternating_tone(len(tone))
        max_error_lsb = float(np.max(np.abs(tone - expected)) * SCALE)
        self.assertLessEqual(max_error_lsb, 1.0)

        symbol_tones: list[int] = []
        symbol_rms: list[float] = []
        for start in range(0, len(tone), SYMBOL_FRAMES):
            samples = tone[start:start + SYMBOL_FRAMES]
            self.assertEqual(len(samples), SYMBOL_FRAMES)
            spectrum = np.abs(np.fft.rfft(samples))
            peak_bin = int(np.argmax(spectrum))
            symbol_tones.append(peak_bin * RATE / SYMBOL_FRAMES)
            symbol_rms.append(float(np.sqrt(np.mean(samples * samples))))

        self.assertEqual(symbol_tones[:8], [MARK_HZ, SPACE_HZ] * 4)
        self.assertEqual(set(symbol_tones), {MARK_HZ, SPACE_HZ})
        self.assertEqual(symbol_tones[0] - symbol_tones[1], 3000)
        measured_ci = [20 * math.log10(0.1 / rms) for rms in symbol_rms]
        self.assertLessEqual(max(abs(value - 12.0) for value in measured_ci), 0.1)
        MEASUREMENTS.append({"frames": len(tone), "symbol_frames": SYMBOL_FRAMES,
                             "first_symbol_tones_hz": symbol_tones[:8],
                             "measured_ci_db_min": min(measured_ci),
                             "measured_ci_db_max": max(measured_ci),
                             "max_phase_oracle_error_lsb": max_error_lsb})

    def test_both_patterns_gates_window_ramp_blocks_export_and_replay(self) -> None:
        self.write_source(0.3)
        for pattern in ("alternating", "random"):
            document = fsk_scenario(pattern=pattern)
            cw = document["impairments"][0]
            cw.update({"start_seconds": 0.003, "duration_seconds": 0.25, "ramp_seconds": 0.0005})
            outputs: list[bytes] = []
            exported_scenario: Path | None = None
            for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                for block in (1, 127, 2048):
                    name = f"{pattern}-{renderer_name}-{block}"
                    export = renderer_name == "normal" and block == 127
                    result, output, sidecar, target = self.render(renderer, document, name, block, export=export)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assert_metadata(sidecar, pattern)
                    outputs.append(output.read_bytes())
                    if export:
                        exported_scenario = target

            self.assertTrue(all(data == outputs[0] for data in outputs[1:]))
            self.assertIsNotNone(exported_scenario)
            exported = json.loads(exported_scenario.read_text(encoding="utf-8"))
            self.assertEqual(exported["impairments"][0]["fsk"], cw["fsk"])
            replay, replay_output, replay_sidecar, _ = self.render(RENDERER, exported,
                                                                   f"{pattern}-replay", block=127)
            self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
            self.assertEqual(replay_output.read_bytes(), outputs[0])
            self.assert_metadata(replay_sidecar, pattern)

            if pattern == "random":
                changed_seed = copy.deepcopy(document)
                changed_seed["seed"] = 18
                result, changed_output, _, _ = self.render(RENDERER, changed_seed, "random-seed18", block=127)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotEqual(changed_output.read_bytes(), outputs[0])
            else:
                changed_seed = copy.deepcopy(document)
                changed_seed["seed"] = 18
                result, same_output, _, _ = self.render(RENDERER, changed_seed, "alternating-seed18", block=127)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(same_output.read_bytes(), outputs[0])

    def test_pulse_and_markov_gating_preserve_fsk_phase_while_on(self) -> None:
        self.write_source(0.3)
        base = fsk_scenario()
        base_cw = base["impairments"][0]
        base_cw.update({"start_seconds": 0.003, "duration_seconds": 0.25, "ramp_seconds": 0.0005})
        ungated, base_output, _, _ = self.render(RENDERER, base, "ungated-reference", block=127)
        self.assertEqual(ungated.returncode, 0, ungated.stdout + ungated.stderr)
        ungated_samples = read_pcm24(base_output)

        pulse = copy.deepcopy(base)
        pulse["impairments"][0].update({"pulse_period_seconds": 0.1, "pulse_duty_cycle": 0.5})

        markov = copy.deepcopy(base)
        markov["impairments"][0]["markov_on_off"] = {"mean_on_seconds": 0.005, "mean_off_seconds": 0.008}
        for gate_name, document in (("pulse", pulse), ("markov", markov)):
            outputs: list[bytes] = []
            samples_by_render: list[np.ndarray] = []
            for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                for block in (1, 127, 2048):
                    result, output, sidecar, _ = self.render(renderer, document,
                                                             f"{gate_name}-{renderer_name}-{block}", block=block)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assert_metadata(sidecar, "alternating")
                    outputs.append(output.read_bytes())
                    samples_by_render.append(read_pcm24(output))

            self.assertTrue(all(data == outputs[0] for data in outputs[1:]))
            gated = samples_by_render[0]
            start = round(0.003 * RATE)
            if gate_name == "pulse":
                # Compare the second pulse after a 50 ms OFF interval. Skip each
                # 0.5 ms envelope edge; the remaining FSK phase must stay aligned.
                second_on = start + 4800
                pulse_interior = gated[second_on + 24:second_on + 2400 - 24]
                reference_interior = ungated_samples[second_on + 24:second_on + 2400 - 24]
                np.testing.assert_array_equal(pulse_interior, reference_interior)
                MEASUREMENTS.append({"gate": gate_name, "compared_on_interior_frames": len(pulse_interior),
                                     "off_interval_frames": 2400})
            else:
                # Markov resumes at pseudorandom frames. Exact agreement during
                # later ON interiors proves the carrier kept advancing through OFF.
                # Restrict the search to the configured CW window so post-window
                # source-only samples cannot count as carrier agreement.
                end = start + round(0.25 * RATE)
                source_equal = gated[start:end] == (1 / 16)
                equal_indices = np.flatnonzero(source_equal) + start
                equal_runs = np.split(equal_indices, np.flatnonzero(np.diff(equal_indices) > 1) + 1)
                off_plateaus = [run for run in equal_runs if len(run) >= 48]
                self.assertTrue(off_plateaus, "Markov render must contain a genuine OFF plateau")
                off_end = int(off_plateaus[0][-1])
                meaningful_tone = np.abs(ungated_samples - (1 / 16)) > 0.2 * AMPLITUDE_PEAK
                frames = np.arange(len(gated))
                matching_after_off = np.flatnonzero(
                    (gated == ungated_samples) & meaningful_tone &
                    (frames > off_end) & (frames < end))
                self.assertGreater(len(matching_after_off), 1000)
                MEASUREMENTS.append({"gate": gate_name,
                                     "verified_off_plateau_frames": len(off_plateaus[0]),
                                     "exact_meaningful_tone_matches_after_off": len(matching_after_off)})

    def test_malformed_fsk_refuses_without_creating_outputs(self) -> None:
        self.write_source(0.01)
        malformed: list[dict] = []
        bad_baud = fsk_scenario()
        bad_baud["impairments"][0]["fsk"]["baud"] = 110
        malformed.append(bad_baud)
        bad_edge = fsk_scenario()
        bad_edge["impairments"][0]["fsk"]["shift_hz"] = 24_000
        malformed.append(bad_edge)
        sweep_conflict = fsk_scenario()
        sweep_conflict["impairments"][0].pop("frequency_hz")
        sweep_conflict["impairments"][0]["frequency_sweep"] = {
            "low_hz": 11_000, "high_hz": 13_000, "slope_hz_per_second": 2000,
        }
        malformed.append(sweep_conflict)

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
