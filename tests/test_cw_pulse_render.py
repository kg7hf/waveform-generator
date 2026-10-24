#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Renderer integration tests for sample-exact pulsed CW scenarios."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

RATE = 48_000
PCM24_SCALE = 1 << 23
RENDERER: Path
WATTERSON_RENDERER: Path


def write_source(path: Path, frames: int = 9600) -> list[int]:
    samples = [round(0.07 * PCM24_SCALE * math.sin(2.0 * math.pi * 440.0 * frame / RATE)) for frame in range(frames)]
    with wave.open(str(path), "wb") as writer:
        writer.setparams((1, 3, RATE, 0, "NONE", "not compressed"))
        writer.writeframes(b"".join(int(sample).to_bytes(3, "little", signed=True) for sample in samples))
    return samples


def read_pcm24(path: Path) -> list[int]:
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("renderer output is not mono PCM24 at 48 kHz")
        raw = reader.readframes(reader.getnframes())
    return [int.from_bytes(raw[offset:offset + 3], "little", signed=True)
            for offset in range(0, len(raw), 3)]


def scenario(*, ramp_seconds: float = 0.0005) -> dict:
    return {
        "schema": "signal-lab.scenario/1",
        "test_id": "CW-PULSE-render-integration",
        "seed": 17,
        "source_gain_db": 0,
        "reference_rms": 0.1,
        "clip_policy": "reject",
        "impairments": [{
            "type": "cw",
            "frequency_hz": 1000,
            "ci_db": 12,
            "phase_degrees": 37,
            "ramp_seconds": ramp_seconds,
            "start_seconds": 0.003,
            "duration_seconds": 0.09,
            "pulse_period_seconds": 0.01,
            "pulse_duty_cycle": 0.4,
        }],
    }


class CwPulseRenderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "pulse source.wav"
        self.input_samples = write_source(self.source)

    def render(self, renderer: Path, document: dict, name: str, block: int = 2048, target: Path | None = None):
        scenario_path = self.root / f"{name}.json"
        output = self.root / f"{name}.wav"
        sidecar = self.root / f"{name}.sidecar.json"
        scenario_path.write_text(json.dumps(document), encoding="utf-8")
        command = [str(renderer), "--scenario", str(scenario_path), "--source", str(self.source),
                   "--output", str(output), "--sidecar", str(sidecar), "--block", str(block)]
        if target is not None:
            command.extend(("--target-scenario", str(target)))
        result = subprocess.run(command, cwd=self.root, capture_output=True, text=True, check=False,
                                creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
        return result, output, sidecar

    def assert_pulse_report(self, sidecar: Path) -> None:
        stage = json.loads(sidecar.read_text(encoding="utf-8"))["stages"][0]
        self.assertEqual(stage["type"], "cw")
        self.assertEqual(stage["pulse_period_frames"], 480)
        self.assertEqual(stage["pulse_on_frames"], 192)
        self.assertAlmostEqual(stage["pulse_realized_duty_cycle"], 0.4, places=15)

    def test_ramp_zero_is_a_sample_masked_continuous_tone_and_blocks_match(self) -> None:
        document = scenario(ramp_seconds=0.0)
        outputs = []
        for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
            for block in (1, 127, 2048):
                with self.subTest(renderer=renderer_name, block=block):
                    result, output, sidecar = self.render(renderer, document, f"{renderer_name}-{block}", block)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assert_pulse_report(sidecar)
                    outputs.append(output.read_bytes())

        self.assertTrue(all(data == outputs[0] for data in outputs[1:]))
        actual = read_pcm24(self.root / "normal-2048.wav")
        amplitude = math.sqrt(2.0) * 0.1 * 10.0 ** (-12.0 / 20.0)
        start = 144  # 3 ms at 48 kHz.
        duration = 4320  # 90 ms.
        period, on = 480, 192
        for frame, (source, rendered) in enumerate(zip(self.input_samples, actual, strict=True)):
            elapsed = frame - start
            tone = 0.0
            if 0 <= elapsed < duration and elapsed % period < on:
                tone = amplitude * math.sin(2.0 * math.pi * 1000.0 * elapsed / RATE + math.radians(37.0))
            expected = source + round(tone * PCM24_SCALE)
            with self.subTest(frame=frame):
                self.assertLessEqual(abs(rendered - expected), 2, (frame, rendered, expected))

    def test_ramped_render_is_partition_independent_and_target_scenario_replays(self) -> None:
        document = scenario()
        outputs = []
        target: Path | None = None
        for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
            for block in (1, 127, 2048):
                current_target = self.root / f"{renderer_name}-{block}.target.json" if renderer_name == "normal" and block == 127 else None
                result, output, sidecar = self.render(renderer, document, f"ramped-{renderer_name}-{block}", block, current_target)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_pulse_report(sidecar)
                outputs.append(output.read_bytes())
                if current_target is not None:
                    target = current_target

        self.assertTrue(all(data == outputs[0] for data in outputs[1:]))
        self.assertIsNotNone(target)
        exported = json.loads(target.read_text(encoding="utf-8"))
        exported_cw = exported["impairments"][0]
        self.assertEqual(exported_cw["pulse_period_seconds"], 0.01)
        self.assertEqual(exported_cw["pulse_duty_cycle"], 0.4)
        replay, replay_output, replay_sidecar = self.render(RENDERER, exported, "target-replay", 127)
        self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
        self.assertEqual(replay_output.read_bytes(), outputs[0])
        self.assert_pulse_report(replay_sidecar)

    def test_invalid_pair_and_fractional_sample_period_refuse_without_output(self) -> None:
        invalid_documents = [
            {**scenario(), "impairments": [{key: value for key, value in scenario()["impairments"][0].items()
                                             if key != "pulse_duty_cycle"}]},
            {**scenario(), "impairments": [{**scenario()["impairments"][0],
                                             "pulse_period_seconds": 1.5 / RATE}]},
        ]
        for index, document in enumerate(invalid_documents):
            with self.subTest(index=index):
                result, output, sidecar = self.render(RENDERER, document, f"invalid-{index}")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--watterson-renderer", type=Path, required=True)
    args, unittest_args = parser.parse_known_args()
    RENDERER = args.renderer.resolve(strict=True)
    WATTERSON_RENDERER = args.watterson_renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_args])
