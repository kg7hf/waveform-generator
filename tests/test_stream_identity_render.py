#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Verify stream-ID stability across renderers and stage insertion from PCM24."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

for _variable in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
    os.environ[_variable] = "1"

RATE = 48_000
SCALE = 1 << 23
SOURCE_LEVEL = 1 / 16
RENDERER: Path
WATTERSON_RENDERER: Path
MEASUREMENTS: list[dict] = []
EXPECTED_KEYS = {
    101: "d06b33f8173bcb28",
    102: "573be5591be7413f",
    103: "eed46674b852140c",
}


def scenario(stages: list[dict], seed: int = 17) -> dict:
    return {
        "schema": "signal-lab.scenario/1",
        "test_id": "stream-identity-render",
        "seed": seed,
        "source_gain_db": 0,
        "reference_rms": 0.1,
        "clip_policy": "reject",
        "impairments": stages,
    }


def inert_cw() -> dict:
    return {
        "type": "cw",
        "frequency_hz": 9000,
        "ci_db": 6,
        "phase_degrees": 0,
        "ramp_seconds": 0,
        "start_seconds": 1.0,
        "duration_seconds": 0.1,
    }


def random_stage(kind: str, stream_id: int | None) -> dict:
    identity = {} if stream_id is None else {"stream_id": stream_id}
    if kind == "awgn":
        return {"type": "awgn", "snr_db": 3, **identity}
    if kind == "cw":
        return {
            "type": "cw",
            "frequency_hz": 12_000,
            "ci_db": 12,
            "phase_degrees": "random",
            "ramp_seconds": 0,
            "start_seconds": 0.003,
            "duration_seconds": 0.25,
            "fsk": {"baud": 300, "shift_hz": 300, "pattern": "random"},
            "markov_on_off": {"mean_on_seconds": 0.005, "mean_off_seconds": 0.008},
            "fading": {"spread_hz": 10},
            **identity,
        }
    if kind == "impulse":
        return {
            "type": "impulse",
            "peak_db": -12,
            "decay_ms": 8,
            "ring": "noise",
            "ring_hz": 1700,
            "truncate_tau": 8,
            "phase_degrees": "random",
            "rate_per_sec": 100,
            **identity,
        }
    if kind == "fade":
        return {
            "type": "fade",
            "depth_db": 12,
            "duration_ms": 5,
            "rate_per_sec": 100,
            **identity,
        }
    raise AssertionError(f"unknown stage kind: {kind}")


def pcm24_payload(path: Path) -> bytes:
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("expected mono PCM24 at 48 kHz")
        return reader.readframes(reader.getnframes())


def assert_random_key(test: unittest.TestCase, stage: dict, stream_id: int) -> None:
    expected = {
        "mode": "stream-id-v1",
        "stream_id": stream_id,
        "effective_seed_hex": EXPECTED_KEYS.get(stream_id),
        "index": 0,
        "algorithm_version": 1,
    }
    test.assertIsNotNone(expected["effective_seed_hex"], "add a verified key vector before asserting it")
    test.assertEqual(stage["random_key"], expected)


class StreamIdentityRenderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.wav"
        self.write_source(0.3)

    def write_source(self, seconds: float) -> None:
        frames = round(seconds * RATE)
        sample = round(SOURCE_LEVEL * SCALE).to_bytes(3, "little", signed=True)
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

    @staticmethod
    def stage_with_key(sidecar: Path, stream_id: int) -> dict:
        stages = json.loads(sidecar.read_text(encoding="utf-8"))["stages"]
        matches = [stage for stage in stages if stage.get("random_key", {}).get("stream_id") == stream_id]
        if len(matches) != 1:
            raise AssertionError(f"expected one sidecar stage with stream_id={stream_id}, found {len(matches)}")
        return matches[0]

    def test_explicit_keys_survive_inert_cw_insertion_for_random_stage_types(self) -> None:
        for kind, stream_id in (("awgn", 101), ("cw", 102), ("impulse", 103), ("fade", 101)):
            base = scenario([random_stage(kind, stream_id)])
            inserted = scenario([inert_cw(), random_stage(kind, stream_id)])
            outputs: list[bytes] = []
            base_digest: str | None = None
            inserted_digest: str | None = None
            replay_doc: dict | None = None

            for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
                for block in (1, 127, 2048):
                    suffix = f"{kind}-{renderer_name}-{block}"
                    export = kind == "cw" and renderer_name == "normal" and block == 127
                    base_result, base_output, base_sidecar, _ = self.render(renderer, base, f"base-{suffix}", block)
                    self.assertEqual(base_result.returncode, 0, base_result.stdout + base_result.stderr)
                    inserted_result, inserted_output, inserted_sidecar, target = self.render(
                        renderer, inserted, f"inserted-{suffix}", block, export=export)
                    self.assertEqual(inserted_result.returncode, 0, inserted_result.stdout + inserted_result.stderr)
                    self.assertEqual(base_output.read_bytes(), inserted_output.read_bytes())
                    self.assertNotEqual(pcm24_payload(base_output),
                                        round(SOURCE_LEVEL * SCALE).to_bytes(3, "little", signed=True) * (RATE * 3 // 10))
                    outputs.append(inserted_output.read_bytes())
                    assert_random_key(self, self.stage_with_key(base_sidecar, stream_id), stream_id)
                    assert_random_key(self, self.stage_with_key(inserted_sidecar, stream_id), stream_id)
                    if kind == "cw" and renderer_name == "normal" and block == 127:
                        inserted_stages = json.loads(inserted_sidecar.read_text(encoding="utf-8"))["stages"]
                        self.assertNotIn("random_key", inserted_stages[0])
                        replay_doc = json.loads(target.read_text(encoding="utf-8"))
                    if renderer_name == "normal" and block == 2048:
                        base_digest = json.loads(base_sidecar.read_text(encoding="utf-8"))["output_digest"]
                        inserted_digest = json.loads(inserted_sidecar.read_text(encoding="utf-8"))["output_digest"]

            self.assertTrue(all(data == outputs[0] for data in outputs[1:]))
            self.assertEqual(base_digest, inserted_digest)
            changed_id = scenario([random_stage(kind, stream_id + 100)])
            changed_result, changed_output, _, _ = self.render(RENDERER, changed_id,
                                                               f"{kind}-different-id", 127)
            self.assertEqual(changed_result.returncode, 0, changed_result.stdout + changed_result.stderr)
            self.assertNotEqual(changed_output.read_bytes(), outputs[0],
                                f"{kind} output should respond to a different explicit stream ID")
            if replay_doc is not None:
                self.assertEqual(replay_doc["impairments"][1]["stream_id"], stream_id)
                replay, replay_output, replay_sidecar, _ = self.render(RENDERER, replay_doc, "cw-export-replay", 127)
                self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
                self.assertEqual(replay_output.read_bytes(), outputs[0])
                assert_random_key(self, self.stage_with_key(replay_sidecar, stream_id), stream_id)
            MEASUREMENTS.append({"stage_type": kind, "stream_id": stream_id,
                                 "partitions_and_renderers_equal": True, "output_digest_stable": base_digest == inserted_digest})

    def test_legacy_awgn_ordinal_still_changes_after_stage_insertion(self) -> None:
        base = scenario([random_stage("awgn", None)])
        inserted = scenario([inert_cw(), random_stage("awgn", None)])
        base_result, base_output, base_sidecar, _ = self.render(RENDERER, base, "legacy-awgn-base", 127)
        inserted_result, inserted_output, inserted_sidecar, _ = self.render(RENDERER, inserted, "legacy-awgn-inserted", 127)
        self.assertEqual(base_result.returncode, 0, base_result.stdout + base_result.stderr)
        self.assertEqual(inserted_result.returncode, 0, inserted_result.stdout + inserted_result.stderr)
        self.assertNotEqual(base_output.read_bytes(), inserted_output.read_bytes())
        base_stages = json.loads(base_sidecar.read_text(encoding="utf-8"))["stages"]
        inserted_stages = json.loads(inserted_sidecar.read_text(encoding="utf-8"))["stages"]
        self.assertNotIn("random_key", base_stages[0])
        self.assertNotIn("random_key", inserted_stages[0])
        self.assertNotIn("random_key", inserted_stages[1])

    def test_watterson_digest_survives_later_stage_insertion(self) -> None:
        channel = {"type": "watterson", "stream_id": 101,
                   "paths": [{"fading": "rayleigh", "spread_hz": 1}]}
        base = scenario([channel])
        appended = scenario([channel, inert_cw()])
        digests: list[tuple[str, str, bytes]] = []

        for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
            for variant, document in (("base", base), ("appended", appended)):
                result, output, sidecar, _ = self.render(renderer, document,
                                                         f"watterson-digest-{renderer_name}-{variant}", 127)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                report = json.loads(sidecar.read_text(encoding="utf-8"))
                channel_report = report["watterson"]
                stages = report["stages"]
                watterson_stage = next(stage for stage in stages if stage["type"] == "watterson")
                assert_random_key(self, watterson_stage, 101)
                self.assertNotEqual(pcm24_payload(output),
                                    round(SOURCE_LEVEL * SCALE).to_bytes(3, "little", signed=True) * (RATE * 3 // 10))
                digests.append((report["output_digest"], channel_report["parameter_digest"], output.read_bytes()))

        self.assertEqual(digests[0], digests[1])
        self.assertEqual(digests[2], digests[3])
        self.assertEqual(digests[0], digests[2])
        MEASUREMENTS.append({"watterson_later_stage_digest_stable": True,
                             "output_digest": digests[0][0], "parameter_digest": digests[0][1]})

    def test_mixed_stream_ids_match_verified_seed_vectors(self) -> None:
        document = scenario([
            random_stage("awgn", 101),
            random_stage("cw", 102),
            random_stage("impulse", 103),
        ])
        result, _, sidecar, _ = self.render(RENDERER, document, "mixed-streams", 127)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for stream_id in (101, 102, 103):
            assert_random_key(self, self.stage_with_key(sidecar, stream_id), stream_id)
        MEASUREMENTS.append({"verified_stream_ids": [101, 102, 103]})

    def test_boundary_ids_and_duplicate_ids(self) -> None:
        keys: list[dict] = []
        for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
            document = scenario([random_stage("awgn", 1), random_stage("cw", 4_294_967_295)])
            result, _, sidecar, _ = self.render(renderer, document, f"bounds-{renderer_name}", 127)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            stages = json.loads(sidecar.read_text(encoding="utf-8"))["stages"]
            first = self.stage_with_key(sidecar, 1)["random_key"]
            last = self.stage_with_key(sidecar, 4_294_967_295)["random_key"]
            for key, stream_id in ((first, 1), (last, 4_294_967_295)):
                self.assertEqual(key["mode"], "stream-id-v1")
                self.assertEqual(key["stream_id"], stream_id)
                self.assertEqual(key["index"], 0)
                self.assertEqual(key["algorithm_version"], 1)
                self.assertRegex(key["effective_seed_hex"], r"^[0-9a-f]{16}$")
            keys.append({"first": first, "last": last})
            self.assertEqual(stages[0]["random_key"], first)
        self.assertEqual(keys[0], keys[1])

        duplicate = scenario([random_stage("awgn", 77), random_stage("cw", 77)])
        for renderer_name, renderer in (("normal", RENDERER), ("validation", WATTERSON_RENDERER)):
            result, output, sidecar, _ = self.render(renderer, duplicate, f"duplicate-{renderer_name}", 127)
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
