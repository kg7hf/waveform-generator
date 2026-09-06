#!/usr/bin/env python3
"""Integration checks for the portable renderer's artifacts and target scenarios."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave


RENDERER: Path


class RendererTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "clean source.wav"
        self.pcm = [12000, -10000, 6000, -4000] * 1024
        with wave.open(str(self.source), "wb") as writer:
            writer.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            writer.writeframes(struct.pack(f"<{len(self.pcm)}h", *self.pcm))

    def render(self, document, *, name="render", extra=(), expected_exit=0):
        scenario = self.root / f"{name} scenario.json"
        if isinstance(document, str):
            scenario.write_text(document, encoding="utf-8")
        else:
            scenario.write_text(json.dumps(document), encoding="utf-8")
        output = self.root / f"{name} output.wav"
        sidecar = self.root / f"{name} output.json"
        target = self.root / f"{name} target.scn"
        result = subprocess.run(
            [str(RENDERER), "--scenario", str(scenario), "--source", str(self.source),
             "--output", str(output), "--sidecar", str(sidecar),
             "--target-scenario", str(target), *extra],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
        return output, sidecar, target, scenario, result

    def test_sidecar_preserves_json_strings_and_native_paths(self):
        test_id = 'review "quoted" \\path\n\t\b\f\r \u2603'
        output, sidecar, _, scenario, _ = self.render({"test_id": test_id, "impairments": []})
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        self.assertEqual(metadata["test_id"], test_id)
        self.assertEqual(metadata["scenario_path"], str(scenario))
        self.assertEqual(metadata["source_path"], str(self.source))
        self.assertEqual(metadata["output_path"], str(output))

    def test_override_updates_existing_reference_and_replays_exactly(self):
        override = "0.38034074368389242"
        document = {
            "reference_rms": 0.125,
            "impairments": [{"type": "cw", "frequency_hz": 1800, "ci_db": 6}],
        }
        first, first_metadata, target, _, _ = self.render(
            document, name="override", extra=("--reference-rms", override))
        exported = json.loads(target.read_text(encoding="utf-8"))
        self.assertEqual(exported["reference_rms"], float(override))
        replay, replay_metadata, _, _, _ = self.render(exported, name="replay")
        self.assertEqual(first.read_bytes(), replay.read_bytes())
        self.assertEqual(json.loads(first_metadata.read_text())["output_digest"],
                         json.loads(replay_metadata.read_text())["output_digest"])

    def test_target_edits_only_the_root_reference_member(self):
        documents = [
            {"metadata": {"reference_rms": 0.75}, "note": "reference_rms", "impairments": []},
            '{"ref\\u0065rence_rms": 0.75, "metadata": {"reference_rms": 0.5}, "impairments": []}',
            {"\reference_rms": 0.75, "impairments": []},
            {},
        ]
        for index, document in enumerate(documents):
            with self.subTest(document=document):
                _, _, target, _, _ = self.render(
                    document, name=f"root{index}", extra=("--reference-rms", "0.125"))
                pairs = json.loads(target.read_text(), object_pairs_hook=lambda values: values)
                self.assertEqual(sum(key == "reference_rms" for key, _ in pairs), 1)
                actual = json.loads(target.read_text())
                expected = json.loads(document) if isinstance(document, str) else dict(document)
                expected["reference_rms"] = 0.125
                self.assertEqual(actual, expected)

    def test_rejected_clipping_preserves_existing_artifacts(self):
        expected = {}
        for suffix in ("output.wav", "output.json", "target.scn"):
            path = self.root / f"reject {suffix}"
            expected[path] = f"existing {suffix}".encode()
            path.write_bytes(expected[path])
        *_, result = self.render(
            {"source_gain_db": 20, "clip_policy": "reject", "impairments": []},
            name="reject", expected_exit=2)
        self.assertIn("clipping", result.stderr)
        for path, contents in expected.items():
            self.assertEqual(path.read_bytes(), contents)

    def test_saturation_metrics_describe_emitted_pcm(self):
        output, sidecar, _, _, _ = self.render(
            {"source_gain_db": 20, "clip_policy": "saturate", "impairments": []})
        with wave.open(str(output), "rb") as reader:
            pcm = struct.unpack(f"<{reader.getnframes()}h", reader.readframes(reader.getnframes()))
        metadata = json.loads(sidecar.read_text())
        peak = max(abs(sample) for sample in pcm) / 32768.0
        rms = math.sqrt(sum((sample / 32768.0) ** 2 for sample in pcm) / len(pcm))
        self.assertGreater(metadata["clipped_samples"], 0)
        self.assertAlmostEqual(metadata["peak_dbfs"], 20 * math.log10(peak), delta=1e-6)
        self.assertAlmostEqual(metadata["rms_dbfs"], 20 * math.log10(rms), delta=1e-6)

    def test_metadata_write_failure_is_not_success(self):
        bad_destination = self.root / "missing directory" / "metadata.json"
        *_, result = self.render({"impairments": []},
                                extra=("--sidecar", str(bad_destination)), expected_exit=2)
        self.assertIn("cannot write", result.stderr)

    def test_reference_override_requires_a_finite_positive_number(self):
        for index, value in enumerate(("nan", "inf", "-inf", "0", "-1", "0.25junk", "")):
            with self.subTest(value=value):
                output, sidecar, target, _, result = self.render(
                    {"impairments": []}, name=f"invalid{index}",
                    extra=("--reference-rms", value), expected_exit=2)
                self.assertIn("finite positive number", result.stderr)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())
                self.assertFalse(target.exists())

    def live_render(self, plan, *, name="live", block=2048, scenario=None, expected_exit=0):
        events = self.root / f"{name} replay.json"
        events.write_text(plan if isinstance(plan, str) else json.dumps(plan), encoding="utf-8")
        output = self.root / f"{name} live.wav"
        sidecar = self.root / f"{name} live.json"
        arguments = [str(RENDERER), "--live-events", str(events), "--source", str(self.source),
                     "--output", str(output), "--sidecar", str(sidecar), "--block", str(block)]
        if scenario is not None:
            source_scenario = self.root / f"{name} source.json"
            source_scenario.write_text(json.dumps(scenario), encoding="utf-8")
            arguments.extend(("--scenario", str(source_scenario)))
        result = subprocess.run(arguments, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
        return output, sidecar, result

    @staticmethod
    def live_plan(controls=(), **overrides):
        plan = {"schema": "wfg-live.replay/1", "protocol": "WFG-LIVE/1",
                "live_seed": 18446744073709551615, "live_reference_rms": 0.125,
                "controls": list(controls)}
        plan.update(overrides)
        return plan

    def test_live_disabled_replay_preserves_source_and_uint64_seed(self):
        output, sidecar, _ = self.live_render(self.live_plan())
        self.assertEqual(output.read_bytes(), self.source.read_bytes())
        metadata = json.loads(sidecar.read_text())
        self.assertEqual(metadata["live_replay"]["seed"], 18446744073709551615)
        self.assertEqual(metadata["live_replay"]["reference_rms"], 0.125)
        self.assertEqual(metadata["output_digest"], metadata["source_digest"])

    def test_live_sweeps_and_static_replay_are_block_independent(self):
        plan = self.live_plan([
            {"command": "CW ON", "apply_frame": 101, "events": 1},
            {"command": "SWEEP CW FREQ 300 3400 8 7", "apply_frame": 101, "events": 8},
            {"command": "SWEEP CW CI 20 3 4 17", "apply_frame": 101, "events": 4},
            {"command": "STATIC RATE 100", "apply_frame": 257, "events": 1},
            {"command": "STATIC PEAK 6", "apply_frame": 257, "events": 1},
            {"command": "STATIC ON", "apply_frame": 257, "events": 1},
            {"command": "SWEEP FADE 6 30 5 11 8", "apply_frame": 503, "events": 5},
            {"command": "CW OFF", "apply_frame": 3500, "events": 1},
            {"command": "STATIC OFF", "apply_frame": 3500, "events": 1},
        ])
        first, first_sidecar, _ = self.live_render(plan, name="mix2048")
        second, second_sidecar, _ = self.live_render(plan, name="mix100", block=100)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        a = json.loads(first_sidecar.read_text())
        b = json.loads(second_sidecar.read_text())
        self.assertEqual(a["output_digest"], b["output_digest"])
        self.assertEqual(a["live_replay"]["controls_applied"], 23)
        self.assertGreater(a["live_replay"]["static_events_started"], 0)
        with wave.open(str(first), "rb") as reader:
            pcm = struct.unpack(f"<{reader.getnframes()}h", reader.readframes(reader.getnframes()))
        self.assertEqual(list(pcm[:101]), self.pcm[:101])
        self.assertEqual(list(pcm[3500:]), self.pcm[3500:])

    def test_live_replay_streams_all_128_captured_controls(self):
        plan = self.live_plan([
            {"command": f"CW CI {index % 20}", "apply_frame": index * 20, "events": 1}
            for index in range(128)
        ])
        output, sidecar, _ = self.live_render(plan, block=100)
        self.assertEqual(output.read_bytes(), self.source.read_bytes())
        metadata = json.loads(sidecar.read_text())["live_replay"]
        self.assertEqual(metadata["controls_accepted"], 128)
        self.assertEqual(metadata["controls_applied"], 128)
        self.assertEqual(metadata["pending_controls"], 0)

    def test_live_replay_uses_output_timeline_after_source_slips(self):
        plan = self.live_plan([
            {"command": "CW FREQ 12000", "apply_frame": 200, "events": 1},
            {"command": "CW ON", "apply_frame": 200, "events": 1},
        ], live_frames=1777)
        scenario = {"source_gain_db": 0, "impairments": [{"type": "sample_slip", "kind": "delete",
                                                         "at_seconds": 0, "length_samples": 100}]}
        output, sidecar, _ = self.live_render(plan, scenario=scenario)
        with wave.open(str(output), "rb") as reader:
            self.assertEqual(reader.getnframes(), 1777)
            pcm = struct.unpack("<1777h", reader.readframes(1777))
        self.assertEqual(list(pcm[:200]), self.pcm[100:300])
        self.assertNotEqual(list(pcm[201:220]), self.pcm[301:320])
        self.assertEqual(json.loads(sidecar.read_text())["frames_out"], 1777)

    def test_live_replay_rejects_invalid_plans_before_writing(self):
        invalid = [
            self.live_plan(protocol="WFG/1"),
            self.live_plan(live_seed=18446744073709551616),
            self.live_plan(live_reference_rms=0),
            self.live_plan([{"command": "CW ON", "apply_frame": 0, "events": 2}]),
            self.live_plan([{"command": "AT:12 CW ON", "apply_frame": 0, "events": 1}]),
            self.live_plan([{"command": "PLAY", "apply_frame": 0, "events": 1}]),
            self.live_plan([{"command": "CW ON", "apply_frame": -1, "events": 1}]),
            self.live_plan(live_frames=len(self.pcm) + 1),
            self.live_plan([{"command": "CW CI 20", "apply_frame": 0, "events": 1}] * 65),
            '{"schema":"wfg-live.replay/1","schema":"wfg-live.replay/1"}',
        ]
        for index, plan in enumerate(invalid):
            with self.subTest(plan=plan):
                output, sidecar, _ = self.live_render(plan, name=f"badlive{index}", expected_exit=2)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
