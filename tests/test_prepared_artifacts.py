#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Host-only checks for copied RT1170 prepared-artifact pairs."""

from __future__ import annotations

import hashlib
import contextlib
import io
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from host import prepared_artifacts as prepared  # noqa: E402


FIRMWARE_COMMIT = "c974d71" + "0" * 33


def pcm24_wav(samples: bytes) -> bytes:
    assert len(samples) % 3 == 0
    padded = samples + (b"\0" if len(samples) & 1 else b"")
    return (struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(padded), b"WAVE", b"fmt ", 16,
                        1, 1, 48_000, 144_000, 3, 24, b"data", len(samples)) + padded)


class PreparedArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "copied"
        self.root.mkdir()
        self.source = Path(self.temporary.name) / "SOURCE.WAV"
        self.scenario = Path(self.temporary.name) / "CASE.SCN"
        self.secondary = Path(self.temporary.name) / "SECOND.WAV"
        self.source.write_bytes(pcm24_wav(b"\x01\x02\x03" * 3))
        self.secondary.write_bytes(pcm24_wav(b"\x04\x05\x06" * 3))
        self.scenario.write_text(json.dumps({
            "seed": 17,
            "reference_rms": 0.1,
            "impairments": [{"type": "second_source", "source_path": "SECOND.WAV"}],
        }), encoding="utf-8")
        self.write_valid_pair()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def sha(data: bytes) -> str:
        return hashlib.sha256(data).hexdigest()

    def write_valid_pair(self) -> None:
        source_bytes = self.source.read_bytes()
        scenario_bytes = self.scenario.read_bytes()
        secondary_bytes = self.secondary.read_bytes()
        output_wav = pcm24_wav(b"\x10\x20\x30" * 3)
        manifest = {
            "schema": prepared.SCHEMA,
            "complete": True,
            "retention": "keep",
            "source": "SOURCE.WAV",
            "source_sha256": self.sha(source_bytes),
            "scenario": "CASE.SCN",
            "scenario_sha256": self.sha(scenario_bytes),
            "secondary": "SECOND.WAV",
            "secondary_sha256": self.sha(secondary_bytes),
            "seed": 17,
            "sample_rate_hz": 48_000,
            "channels": 1,
            "bits_per_sample": 24,
            "input_frames": 3,
            "output_frames": 3,
            "wav_sha256": self.sha(output_wav),
            "pcm_sha256": self.sha(b"\x10\x20\x30" * 3),
            "engine_output_fnv1a64": "0123456789abcdef",
            "engine_algorithm_version": 1,
            "firmware_commit": FIRMWARE_COMMIT,
        }
        (self.root / "OUT.WAV").write_bytes(output_wav)
        (self.root / "OUT.JSN").write_text(json.dumps(manifest), encoding="utf-8")

    def manifest(self) -> dict:
        return json.loads((self.root / "OUT.JSN").read_text(encoding="utf-8"))

    def save_manifest(self, value: dict) -> None:
        (self.root / "OUT.JSN").write_text(json.dumps(value), encoding="utf-8")

    def test_actual_manifest_shape_lists_and_verifies(self) -> None:
        listed = prepared.list_artifacts(self.root)
        self.assertEqual(listed["count"], 1)
        result = prepared.verify(self.root, "OUT.JSN")
        self.assertEqual(result["frames"], 3)
        self.assertEqual(result["manifest"]["schema"], "waveform-generator.prepared/2")

    def test_reuse_checks_firmware_seed_and_current_inputs(self) -> None:
        result = prepared.reuse(self.root, "OUT", firmware_commit=FIRMWARE_COMMIT,
                                seed=17, source=self.source,
                                scenario=self.scenario, secondary=self.secondary)
        self.assertTrue(result["reusable"])
        self.assertEqual(set(result["current_inputs"]), {"source", "scenario", "secondary"})
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(self.root, "OUT", firmware_commit="wrong",                            seed=17, source=self.source, scenario=self.scenario, secondary=self.secondary)
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(self.root, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=18,
                           source=self.source, scenario=self.scenario, secondary=self.secondary)

    def test_changed_supplied_inputs_refuse_reuse(self) -> None:
        self.source.write_bytes(pcm24_wav(b"\x09\x08\x07" * 3))
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(self.root, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=17, source=self.source,
                           scenario=self.scenario, secondary=self.secondary)
        self.write_valid_pair()
        self.scenario.write_text('{"seed":18,"impairments":[]}', encoding="utf-8")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(self.root, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=17, source=self.source,
                           scenario=self.scenario, secondary=self.secondary)

    def test_missing_corrupt_and_unpaired_components_fail_closed(self) -> None:
        (self.root / "OUT.WAV").unlink()
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(self.root, "OUT")
        self.write_valid_pair()
        with (self.root / "OUT.WAV").open("r+b") as stream:
            stream.seek(45)
            stream.write(b"X")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.list_artifacts(self.root)
        (self.root / "ORPHAN.JSN").write_text("{}", encoding="utf-8")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.list_artifacts(self.root)

    def test_output_scenario_companion_is_rejected(self) -> None:
        (self.root / "out.scn").write_text("{}", encoding="utf-8")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(self.root, "OUT")

    def test_schema_path_hash_and_frame_errors_are_rejected(self) -> None:
        invalid_values = [
            {"source": "../SOURCE.WAV"},
            {"engine_algorithm_version": 2},
            {"output_frames": True},
            {"seed": 1.0},
        ]
        for change in invalid_values:
            with self.subTest(change=change):
                manifest = self.manifest()
                manifest.update(change)
                self.save_manifest(manifest)
                with self.assertRaises(prepared.PreparedArtifactError):
                    prepared.verify(self.root, "OUT")
                self.write_valid_pair()

    def test_duplicate_manifest_keys_fail_and_current_scenario_is_required(self) -> None:
        manifest_bytes = (self.root / "OUT.JSN").read_text(encoding="utf-8")
        (self.root / "OUT.JSN").write_text(manifest_bytes[:-1] + ',"seed":17}', encoding="utf-8")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(self.root, "OUT")
        self.write_valid_pair()
        self.scenario.write_text('{"impairments":[]}', encoding="utf-8")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(self.root, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=17, source=self.source,
                           scenario=self.scenario, secondary=self.secondary)

    def test_case_collisions_symlinks_and_traversal_names_refuse(self) -> None:
        collision = self.root / "out.jsn"
        if not collision.exists():
            collision.write_text("{}", encoding="utf-8")
            with self.assertRaises(prepared.PreparedArtifactError):
                prepared.list_artifacts(self.root)
            with self.assertRaises(prepared.PreparedArtifactError):
                prepared.verify(self.root, "OUT")
            collision.unlink()
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(self.root, "../OUT")

    def test_symlink_entry_refuses_with_an_otherwise_valid_pair(self) -> None:
        try:
            (self.root / "ALIAS.JSN").symlink_to(self.root / "OUT.JSN")
        except (OSError, NotImplementedError):
            self.skipTest("symlink creation is not available")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.list_artifacts(self.root)

    def test_changed_secondary_and_missing_required_secondary_refuse(self) -> None:
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(self.root, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=17,
                           source=self.source, scenario=self.scenario)
        self.secondary.write_bytes(pcm24_wav(b"\x09\x08\x07" * 3))
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(self.root, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=17,
                           source=self.source, scenario=self.scenario, secondary=self.secondary)

    def test_oversized_manifest_and_trailing_wav_bytes_refuse(self) -> None:
        (self.root / "OUT.JSN").write_bytes(b" " * (prepared.MAX_MANIFEST_BYTES + 1))
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(self.root, "OUT")
        self.write_valid_pair()
        with (self.root / "OUT.WAV").open("ab") as stream:
            stream.write(b"x")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(self.root, "OUT")

    def test_cli_reports_reuse_mismatch_as_failure(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = prepared.main([
                "reuse", str(self.root), "OUT", "--firmware-commit", FIRMWARE_COMMIT,
"--seed", "18",
                "--source", str(self.source), "--scenario", str(self.scenario),
                "--secondary", str(self.secondary),
            ])
        self.assertEqual(result, 1)
        self.assertIn("expected seed differs", stderr.getvalue())

    def test_archive_preserves_pair_and_can_be_verified_and_reused(self) -> None:
        destination = Path(self.temporary.name) / "archive"
        source_wav = (self.root / "OUT.WAV").read_bytes()
        source_manifest = (self.root / "OUT.JSN").read_bytes()
        retention = self.manifest()["retention"]

        result = prepared.archive(self.root, "OUT", destination)

        self.assertTrue(result["ok"])
        self.assertEqual(sorted(path.name for path in destination.iterdir()), ["OUT.JSN", "OUT.WAV"])
        self.assertEqual((destination / "OUT.WAV").read_bytes(), source_wav)
        self.assertEqual((destination / "OUT.JSN").read_bytes(), source_manifest)
        self.assertEqual(prepared.verify(destination, "OUT")["manifest"]["retention"], retention)
        reused = prepared.reuse(destination, "OUT", firmware_commit=FIRMWARE_COMMIT,
                                seed=17, source=self.source,
                                scenario=self.scenario, secondary=self.secondary)
        self.assertTrue(reused["reusable"])

    def test_archive_preserves_temporary_retention_bytes(self) -> None:
        manifest = self.manifest()
        manifest["retention"] = "temporary"
        self.save_manifest(manifest)
        source_manifest = (self.root / "OUT.JSN").read_bytes()
        destination = Path(self.temporary.name) / "temporary-archive"

        prepared.archive(self.root, "OUT", destination)

        self.assertEqual((destination / "OUT.JSN").read_bytes(), source_manifest)
        self.assertEqual(prepared.verify(destination, "OUT")["manifest"]["retention"], "temporary")

    def test_archive_preflight_and_existing_destination_are_non_mutating(self) -> None:
        bad_source = Path(self.temporary.name) / "bad-source"
        bad_source.mkdir()
        (bad_source / "OUT.WAV").write_bytes(b"not a wav")
        (bad_source / "OUT.JSN").write_bytes((self.root / "OUT.JSN").read_bytes())
        rejected_destination = Path(self.temporary.name) / "rejected-archive"
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.archive(bad_source, "OUT", rejected_destination)
        self.assertFalse(rejected_destination.exists())

        existing = Path(self.temporary.name) / "existing-archive"
        existing.mkdir()
        sentinel = existing / "keep.txt"
        sentinel.write_text("existing data", encoding="utf-8")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.archive(self.root, "OUT", existing)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "existing data")
        self.assertEqual(sorted(path.name for path in existing.iterdir()), ["keep.txt"])

        empty_existing = Path(self.temporary.name) / "empty-existing-archive"
        empty_existing.mkdir()
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.archive(self.root, "OUT", empty_existing)
        self.assertEqual(list(empty_existing.iterdir()), [])

        (self.root / "incomplete.JSON").write_text('{"complete":false}', encoding="utf-8")
        marked_source_destination = Path(self.temporary.name) / "marked-source-archive"
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.archive(self.root, "OUT", marked_source_destination)
        self.assertFalse(marked_source_destination.exists())

    def test_incomplete_archive_marker_blocks_all_public_read_paths(self) -> None:
        destination = Path(self.temporary.name) / "partial-archive"
        destination.mkdir()
        self.write_valid_pair()
        shutil.copy2(self.root / "OUT.WAV", destination / "OUT.WAV")
        shutil.copy2(self.root / "OUT.JSN", destination / "OUT.JSN")
        (destination / "incomplete.json").write_text('{"complete":false}', encoding="utf-8")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(destination, "OUT")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.list_artifacts(destination)
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(destination, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=17, source=self.source,
                           scenario=self.scenario, secondary=self.secondary)

    def test_archive_growth_and_short_copy_fail_exact_size_snapshot(self) -> None:
        original_copy = prepared._copy_file

        for mode in ("growth", "short"):
            with self.subTest(mode=mode):
                self.write_valid_pair()
                destination = Path(self.temporary.name) / f"{mode}-archive"

                def change_source_before_copy(source: Path, output: Path, *, expected_sha256: str,
                                              expected_size: int) -> tuple[str, int]:
                    if source.name == "OUT.WAV":
                        if mode == "growth":
                            with source.open("ab") as stream:
                                stream.write(b"x")
                        else:
                            source.write_bytes(source.read_bytes()[:50])
                    return original_copy(source, output, expected_sha256=expected_sha256,
                                         expected_size=expected_size)

                with mock.patch.object(prepared, "_copy_file", side_effect=change_source_before_copy):
                    with self.assertRaises(prepared.PreparedArtifactError):
                        prepared.archive(self.root, "OUT", destination)
                self.assertTrue((destination / "INCOMPLETE.json").is_file())
                with self.assertRaises(prepared.PreparedArtifactError):
                    prepared.verify(destination, "OUT")

    def test_marker_unlink_failure_keeps_completed_pair_unreadable(self) -> None:
        destination = Path(self.temporary.name) / "unlink-failure"
        marker = destination / "INCOMPLETE.json"
        original_unlink = Path.unlink

        def fail_marker_unlink(path: Path, missing_ok: bool = False) -> None:
            if path == marker:
                raise OSError("injected marker publication failure")
            original_unlink(path, missing_ok=missing_ok)

        with mock.patch.object(Path, "unlink", autospec=True, side_effect=fail_marker_unlink):
            with self.assertRaisesRegex(prepared.PreparedArtifactError, "cannot publish archive"):
                prepared.archive(self.root, "OUT", destination)

        self.assertEqual((destination / "OUT.WAV").read_bytes(), (self.root / "OUT.WAV").read_bytes())
        self.assertEqual((destination / "OUT.JSN").read_bytes(), (self.root / "OUT.JSN").read_bytes())
        self.assertTrue(marker.is_file())
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(destination, "OUT")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.list_artifacts(destination)
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.reuse(destination, "OUT", firmware_commit=FIRMWARE_COMMIT,
                           seed=17, source=self.source,
                           scenario=self.scenario, secondary=self.secondary)

    def test_partial_copy_failure_keeps_incomplete_marker_and_partial_bytes(self) -> None:
        destination = Path(self.temporary.name) / "partial-copy"
        original_copy = prepared._copy_file

        def fail_after_partial_write(source: Path, output: Path, *, expected_sha256: str,
                                     expected_size: int) -> tuple[str, int]:
            if source.name != "OUT.WAV":
                return original_copy(source, output, expected_sha256=expected_sha256, expected_size=expected_size)
            del expected_sha256, expected_size
            output.write_bytes(source.read_bytes()[:7])
            raise prepared.PreparedArtifactError("injected partial copy failure")

        with mock.patch.object(prepared, "_copy_file", side_effect=fail_after_partial_write):
            with self.assertRaisesRegex(prepared.PreparedArtifactError, "injected partial copy failure"):
                prepared.archive(self.root, "OUT", destination)

        self.assertTrue((destination / "INCOMPLETE.json").is_file())
        self.assertEqual((destination / "OUT.WAV").stat().st_size, 7)
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(destination, "OUT")
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.list_artifacts(destination)

    def test_archive_fsync_failure_preserves_incomplete_marker(self) -> None:
        destination = Path(self.temporary.name) / "sync-failure"
        real_fsync = prepared.os.fsync
        calls = 0

        def fail_second_sync(descriptor: int) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("injected fsync failure")
            real_fsync(descriptor)

        with mock.patch.object(prepared.os, "fsync", side_effect=fail_second_sync):
            with self.assertRaises(prepared.PreparedArtifactError):
                prepared.archive(self.root, "OUT", destination)

        self.assertGreaterEqual(calls, 2)
        self.assertTrue((destination / "INCOMPLETE.json").is_file())
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(destination, "OUT")

    def test_source_change_after_preflight_is_detected_and_not_published(self) -> None:
        destination = Path(self.temporary.name) / "source-change"
        original_copy = prepared._copy_file
        pinned_source = (self.root / "OUT.WAV").read_bytes()

        def mutate_source_before_copy(source: Path, output: Path, *, expected_sha256: str,
                                      expected_size: int) -> tuple[str, int]:
            if source.name == "OUT.WAV":
                source.write_bytes(pcm24_wav(b"\x7f\x01\x02" * 3))
            return original_copy(source, output, expected_sha256=expected_sha256, expected_size=expected_size)

        with mock.patch.object(prepared, "_copy_file", side_effect=mutate_source_before_copy):
            with self.assertRaises(prepared.PreparedArtifactError):
                prepared.archive(self.root, "OUT", destination)

        self.assertTrue((destination / "INCOMPLETE.json").is_file())
        self.assertNotEqual((self.root / "OUT.WAV").read_bytes(), pinned_source)
        with self.assertRaises(prepared.PreparedArtifactError):
            prepared.verify(destination, "OUT")

    def test_archive_cli_reports_success_and_destination_conflict(self) -> None:
        destination = Path(self.temporary.name) / "cli-archive"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = prepared.main(["archive", str(self.root), "OUT", str(destination)])
        self.assertEqual(result, 0, stderr.getvalue())
        self.assertTrue(json.loads(stdout.getvalue())["ok"])

        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = prepared.main(["archive", str(self.root), "OUT", str(destination)])
        self.assertEqual(result, 1)
        self.assertNotEqual(stderr.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
