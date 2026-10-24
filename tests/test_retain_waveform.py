#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Offline retention workflow tests; no device or audio APIs are used."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
import retain_waveform as retained


RENDERER: Path | None = None


def write_pcm24(path: Path, samples: list[int]) -> None:
    with wave.open(str(path), "wb") as output:
        output.setparams((1, 3, 48000, 0, "NONE", "not compressed"))
        output.writeframes(b"".join(int(sample).to_bytes(3, "little", signed=True) for sample in samples))
    data = bytearray(path.read_bytes())
    data_size = struct.unpack_from("<I", data, 40)[0]
    if data_size & 1:
        data.append(0)
    struct.pack_into("<I", data, 4, 36 + data_size + (data_size & 1))
    path.write_bytes(data)


def fake_render(command, *, stdout, stderr, check, shell):
    args = dict(zip(command[1::2], command[2::2]))
    pcm = b"\x01\x02\x03\x04\x05\x06\x07\x08\x09"
    output = Path(args["--output"])
    with wave.open(str(output), "wb") as writer:
        writer.setparams((1, 3, 48000, 0, "NONE", "not compressed"))
        writer.writeframes(pcm)
    data = bytearray(output.read_bytes())
    data_size = struct.unpack_from("<I", data, 40)[0]
    if data_size & 1:
        data.append(0)
    struct.pack_into("<I", data, 4, 36 + data_size + (data_size & 1))
    output.write_bytes(data)
    fnv = retained.FNV_OFFSET
    for byte in pcm:
        fnv = ((fnv ^ byte) * retained.FNV_PRIME) & retained.UINT64_MAX
    scenario = json.loads(Path(args["--scenario"]).read_text(encoding="utf-8"))
    seed = int(args.get("--seed", scenario.get("seed", 0)))
    sidecar = {"schema": retained.RENDER_SCHEMA, "generator": "signal-lab-cpp/0.2.0",
               "output_encoding": "pcm_s24le", "seed": seed, "frames_out": 3,
               "block_frames": retained.BLOCK_FRAMES, "scenario_path": args["--scenario"],
               "source_path": args["--source"], "output_digest": f"{fnv:016x}"}
    Path(args["--sidecar"]).write_text(json.dumps(sidecar), encoding="utf-8")
    stdout.write(b"rendered\n")
    stderr.write(b"")
    return subprocess.CompletedProcess(command, 0)


class RetainWaveformTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.renderer = self.root / "renderer.exe"
        self.renderer.write_bytes(b"test renderer identity")
        self.source = self.root / "source.wav"
        write_pcm24(self.source, [1, -2, 3])
        self.scenario = self.root / "scenario.json"
        self.scenario_bytes = b'{"seed":7,"source_gain_db":0,"reference_rms":0.25,"impairments":[]}\n'
        self.scenario.write_bytes(self.scenario_bytes)
        self.output_dir = self.root / "artifact"

    def render_mocked(self, **kwargs):
        with mock.patch.object(retained.subprocess, "run", side_effect=fake_render) as runner:
            result = retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, **kwargs)
        return result, runner

    def test_prepare_and_verify_retain_exact_provenance_and_odd_pcm(self):
        result, runner = self.render_mocked(seed=19)
        self.assertEqual(result["frames"], 3)
        command = runner.call_args.args[0]
        self.assertEqual(command[command.index("--seed") + 1], "19")
        self.assertIn("--block", command)
        self.assertEqual(command[command.index("--block") + 1], "2048")
        manifest = result["manifest"]
        self.assertEqual(manifest["scenario"], json.loads(self.scenario_bytes))
        self.assertEqual((self.output_dir / "scenario.json").read_bytes(), self.scenario_bytes)
        self.assertEqual(manifest["generation"]["seed"], 19)
        self.assertEqual(retained.verify(self.output_dir)["wav_sha256"], manifest["wav_sha256"])
        self.assertEqual((self.output_dir / "output.wav").stat().st_size, 54)

    def test_existing_directory_is_never_overwritten_and_reuse_checks_identity(self):
        result, _ = self.render_mocked()
        marker = self.output_dir / "keep.txt"
        marker.write_text("user data")
        with self.assertRaisesRegex(retained.RetainError, "refusing to overwrite"):
            retained.prepare(self.renderer, self.source, self.scenario, self.output_dir)
        self.assertEqual(marker.read_text(), "user data")
        marker.unlink()
        with mock.patch.object(retained.subprocess, "run") as runner:
            reused = retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, reuse=True)
        runner.assert_not_called()
        self.assertEqual(reused["output_digest"], result["output_digest"])
        self.source.write_bytes(self.source.read_bytes() + b"changed")
        with self.assertRaisesRegex(retained.RetainError, "identity differs"):
            retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, reuse=True)

    def test_archive_copies_inputs_and_verify_checks_them(self):
        result, _ = self.render_mocked(retention="archive")
        self.assertEqual((self.output_dir / "inputs/primary.wav").read_bytes(), self.source.read_bytes())
        self.assertEqual((self.output_dir / "inputs/scenario.json").read_bytes(), self.scenario_bytes)
        self.assertEqual(retained.verify(self.output_dir)["retention"], "archive")
        archived = self.output_dir / "inputs/primary.wav"
        archived.write_bytes(b"tampered")
        with self.assertRaisesRegex(retained.RetainError, "archived input hash"):
            retained.verify(self.output_dir)

    def test_archive_missing_required_copy_is_rejected(self):
        self.render_mocked(retention="archive")
        manifest_path = self.output_dir / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["archived_inputs"] = []
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(retained.RetainError, "exact required source"):
            retained.verify(self.output_dir)

    def test_reuse_rejects_changed_secondary_source_and_seed(self):
        secondary = self.root / "secondary.wav"
        write_pcm24(secondary, [4, 5, 6])
        scenario = {"seed": 7, "source_gain_db": 0, "reference_rms": 0.25,
                    "impairments": [{"type": "second_source", "source_path": "secondary.wav",
                                     "si_db": 0, "start_frame": 0, "source_start_frame": 0,
                                     "duration_frames": 0, "frequency_offset_hz": 0}]}
        self.scenario.write_text(json.dumps(scenario), encoding="utf-8")
        self.render_mocked()
        with mock.patch.object(retained.subprocess, "run") as runner:
            with self.assertRaisesRegex(retained.RetainError, "identity differs"):
                retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, reuse=True, seed=8)
            runner.assert_not_called()
        secondary.write_bytes(secondary.read_bytes() + b"changed")
        with mock.patch.object(retained.subprocess, "run") as runner:
            with self.assertRaisesRegex(retained.RetainError, "identity differs"):
                retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, reuse=True)
            runner.assert_not_called()

    def test_incomplete_stage_is_preserved_and_never_verifies(self):
        def fail_render(command, *, stdout, stderr, check, shell):
            stdout.write(b"partial\n")
            stderr.write(b"failure\n")
            return subprocess.CompletedProcess(command, 2)
        with mock.patch.object(retained.subprocess, "run", side_effect=fail_render):
            with self.assertRaisesRegex(retained.RetainError, "incomplete work retained"):
                retained.prepare(self.renderer, self.source, self.scenario, self.output_dir)
        stages = list(self.root.glob(".artifact.incomplete-*"))
        self.assertEqual(len(stages), 1)
        self.assertTrue((stages[0] / "INCOMPLETE.json").exists())
        with self.assertRaises(retained.RetainError):
            retained.verify(stages[0])

    def test_inputs_changed_during_render_leave_only_incomplete_stage(self):
        def mutate_source(command, *, stdout, stderr, check, shell):
            result = fake_render(command, stdout=stdout, stderr=stderr, check=check, shell=shell)
            self.source.write_bytes(self.source.read_bytes() + b"changed")
            return result
        with mock.patch.object(retained.subprocess, "run", side_effect=mutate_source):
            with self.assertRaisesRegex(retained.RetainError, "changed while rendering"):
                retained.prepare(self.renderer, self.source, self.scenario, self.output_dir)
        stages = list(self.root.glob(".artifact.incomplete-*"))
        self.assertEqual(len(stages), 1)
        self.assertTrue((stages[0] / "INCOMPLETE.json").exists())

    def test_verify_refuses_corrupt_waveform_and_bad_sidecar(self):
        self.render_mocked()
        manifest_path = self.output_dir / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["request_identity"]["renderer_sha256"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(retained.RetainError, "hash links are inconsistent"):
            retained.verify(self.output_dir)
        manifest["request_identity"]["renderer_sha256"] = manifest["renderer"]["sha256"]
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        output = self.output_dir / "output.wav"
        output.write_bytes(output.read_bytes()[:-1])
        with self.assertRaisesRegex(retained.RetainError, "RIFF size or file length"):
            retained.verify(self.output_dir)

    def test_zero_frame_canonical_render_is_a_valid_pcm_artifact(self):
        empty = self.root / "empty.wav"
        with wave.open(str(empty), "wb") as writer:
            writer.setparams((1, 3, 48000, 0, "NONE", "not compressed"))
            writer.writeframes(b"")
        frames, _, pcm_hash, fnv = retained._wav_payload(empty)
        self.assertEqual((frames, pcm_hash, fnv), (0, hashlib.sha256(b"").hexdigest(), retained.FNV_OFFSET))

    def test_reuse_needs_no_original_files_but_checks_all_request_identity_fields(self):
        self.render_mocked()
        scenario_bytes = self.scenario.read_bytes()
        renderer_bytes = self.renderer.read_bytes()
        self.scenario.write_bytes(scenario_bytes + b" ")
        with self.assertRaisesRegex(retained.RetainError, "identity differs"):
            retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, reuse=True)
        self.scenario.write_bytes(scenario_bytes)
        self.renderer.write_bytes(renderer_bytes + b" changed")
        with self.assertRaisesRegex(retained.RetainError, "identity differs"):
            retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, reuse=True)
        self.renderer.write_bytes(renderer_bytes)
        with self.assertRaisesRegex(retained.RetainError, "identity differs"):
            retained.prepare(self.renderer, self.source, self.scenario, self.output_dir, reuse=True, retention="archive")
        self.source.unlink()
        self.scenario.unlink()
        self.renderer.unlink()
        self.assertTrue(retained.verify(self.output_dir)["ok"], "keep artifacts verify without original input files")

    def test_racing_destination_creation_is_never_replaced(self):
        original_mkdir = Path.mkdir
        def competing_creator(path, *args, **kwargs):
            if path == self.output_dir:
                original_mkdir(path, *args, **kwargs)
                (path / "competitor.txt").write_text("preserve")
                raise FileExistsError(path)
            return original_mkdir(path, *args, **kwargs)
        with mock.patch.object(Path, "mkdir", new=competing_creator):
            with mock.patch.object(retained.subprocess, "run", side_effect=fake_render):
                with self.assertRaisesRegex(retained.RetainError, "cannot reserve output directory"):
                    retained.prepare(self.renderer, self.source, self.scenario, self.output_dir)
        self.assertEqual((self.output_dir / "competitor.txt").read_text(), "preserve")
        self.assertEqual(len(list(self.root.glob(".artifact.incomplete-*"))), 1)

    def test_verify_rejects_symlinked_retained_files(self):
        self.render_mocked()
        output = self.output_dir / "output.wav"
        outside = self.root / "outside.wav"
        output.replace(outside)
        try:
            output.symlink_to(outside)
        except OSError as error:
            self.skipTest(f"symlink creation unavailable: {error}")
        with self.assertRaisesRegex(retained.RetainError, "symlink"):
            retained.verify(self.output_dir)

    def test_duplicate_or_nonfinite_scenario_json_refuses_before_render(self):
        for content in (b'{"seed":1,"seed":2}', b'{"value":1e999}', b'{"seed":NaN}'):
            self.scenario.write_bytes(content)
            with mock.patch.object(retained.subprocess, "run") as runner:
                with self.assertRaises(retained.RetainError):
                    retained.prepare(self.renderer, self.source, self.scenario, self.output_dir)
            runner.assert_not_called()

    def test_real_cpp_renderer_and_seed_override(self):
        if RENDERER is None:
            self.skipTest("pass --renderer to exercise the C++ renderer")
        try:
            result = retained.prepare(RENDERER, self.source, self.scenario, self.output_dir, seed=23)
        except retained.RetainError as error:
            # The implementation deliberately retains failed private staging
            # data; include the captured renderer diagnostics before unittest
            # cleanup removes that temporary directory.
            stages = list(self.root.glob(".artifact.incomplete-*"))
            diagnostics = "\n".join(
                (stage / "renderer.stderr.txt").read_text(encoding="utf-8", errors="replace")
                for stage in stages if (stage / "renderer.stderr.txt").is_file()
            )
            self.fail(f"{error}\nrenderer stderr:\n{diagnostics}")
        self.assertEqual(result["manifest"]["generation"]["seed"], 23)
        self.assertEqual(result["frames"], 3)
        self.assertTrue(retained.verify(self.output_dir)["ok"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path)
    arguments, unittest_arguments = parser.parse_known_args()
    if arguments.renderer is not None:
        RENDERER = arguments.renderer.resolve(strict=True)
    outcome = unittest.main(argv=[__file__, *unittest_arguments], exit=False)
    raise SystemExit(not outcome.result.wasSuccessful())
