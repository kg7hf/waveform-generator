#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Offline comparison tests for board and retained waveform artifacts."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))

import compare_prepared as comparison  # noqa: E402
import prepared_artifacts as board_artifacts  # noqa: E402
import retain_waveform as retained  # noqa: E402


RENDERER: Path | None = None
FIRMWARE_COMMIT = "c974d71" + "0" * 33
DEFAULT_PCM = b"\x01\x02\x03\x04\x05\x06\x07\x08\x09"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_pcm24(path: Path, pcm: bytes) -> None:
    if len(pcm) % 3:
        raise ValueError("packed PCM24 needs whole samples")
    with wave.open(str(path), "wb") as output:
        output.setparams((1, 3, retained.SAMPLE_RATE_HZ, 0, "NONE", "not compressed"))
        output.writeframes(pcm)
    data = bytearray(path.read_bytes())
    data_size = struct.unpack_from("<I", data, 40)[0]
    if data_size & 1:
        data.append(0)
    struct.pack_into("<I", data, 4, 36 + data_size + (data_size & 1))
    path.write_bytes(data)


def wav_pcm24(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE" or data[36:40] != b"data":
        raise AssertionError("fixture WAV is not canonical PCM24")
    data_size = struct.unpack_from("<I", data, 40)[0]
    return data[44:44 + data_size]


def fake_render(command, *, stdout, stderr, check, shell):
    del check, shell
    arguments = dict(zip(command[1::2], command[2::2]))
    pcm = DEFAULT_PCM
    output = Path(arguments["--output"])
    write_pcm24(output, pcm)
    digest = retained.FNV_OFFSET
    for byte in pcm:
        digest = ((digest ^ byte) * retained.FNV_PRIME) & retained.UINT64_MAX
    scenario = json.loads(Path(arguments["--scenario"]).read_text(encoding="utf-8"))
    seed = int(arguments.get("--seed", scenario.get("seed", 0)))
    sidecar = {
        "schema": retained.RENDER_SCHEMA,
        "generator": "signal-lab-cpp/0.2.0",
        "output_encoding": "pcm_s24le",
        "seed": seed,
        "frames_out": len(pcm) // 3,
        "block_frames": retained.BLOCK_FRAMES,
        "scenario_path": arguments["--scenario"],
        "source_path": arguments["--source"],
        "output_digest": f"{digest:016x}",
    }
    Path(arguments["--sidecar"]).write_text(json.dumps(sidecar), encoding="utf-8")
    stdout.write(b"rendered\n")
    stderr.write(b"")
    return subprocess.CompletedProcess(command, 0)


class PreparedComparisonTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.inputs = self.root / "inputs"
        self.inputs.mkdir()
        self.source = self.inputs / "SOURCE.WAV"
        self.secondary = self.inputs / "SECOND.WAV"
        self.scenario = self.inputs / "CASE.SCN"
        write_pcm24(self.source, b"\x10\x20\x30" * 4)
        write_pcm24(self.secondary, b"\x40\x50\x60" * 4)
        self.scenario_bytes = (
            b'{"seed":17,"source_gain_db":0,"reference_rms":0.1,"impairments":'
            b'[{"type":"second_source","source_path":"SECOND.WAV","si_db":0,'
            b'"start_frame":0,"source_start_frame":0,"duration_frames":4,'
            b'"frequency_offset_hz":0}]}\n'
        )
        self.scenario.write_bytes(self.scenario_bytes)
        self.renderer = self.root / "signal_lab_render.exe"
        self.renderer.write_bytes(b"fake renderer identity")
        self.reference = self.root / "reference"
        with mock.patch.object(retained.subprocess, "run", side_effect=fake_render):
            retained.prepare(self.renderer, self.source, self.scenario, self.reference, retention="keep")
        self.board = self.root / "board"
        self.board.mkdir()
        self.write_board(DEFAULT_PCM)

    def write_board(self, pcm: bytes, *, seed: int = 17, source_hash: str | None = None,
                    scenario_hash: str | None = None, secondary_name: str = "SECOND.WAV",
                    secondary_hash: str | None = None, firmware_commit: str = FIRMWARE_COMMIT,
) -> None:
        wav_path = self.board / "OUT.WAV"
        write_pcm24(wav_path, pcm)
        secondary_present = bool(secondary_name)
        manifest = {
            "schema": board_artifacts.SCHEMA,
            "complete": True,
            "retention": "keep",
            "source": "SOURCE.WAV",
            "source_sha256": source_hash or sha256(self.source.read_bytes()),
            "scenario": "CASE.SCN",
            "scenario_sha256": scenario_hash or sha256(self.scenario_bytes),
            "secondary": secondary_name,
            "secondary_sha256": (secondary_hash or sha256(self.secondary.read_bytes())) if secondary_present else "",
            "seed": seed,
            "sample_rate_hz": 48_000,
            "channels": 1,
            "bits_per_sample": 24,
            "input_frames": 4,
            "output_frames": len(pcm) // 3,
            "wav_sha256": sha256(wav_path.read_bytes()),
            "pcm_sha256": sha256(pcm),
            "engine_output_fnv1a64": "0123456789abcdef",
            "engine_algorithm_version": 1,
            "firmware_commit": firmware_commit,
        }
        (self.board / "OUT.JSN").write_text(json.dumps(manifest), encoding="utf-8")

    def create_reference(self, directory: Path, *, scenario_bytes: bytes | None = None,
                         seed: int | None = None, secondary_bytes: bytes | None = None) -> Path:
        if scenario_bytes is not None:
            self.scenario.write_bytes(scenario_bytes)
        if secondary_bytes is not None:
            write_pcm24(self.secondary, secondary_bytes)
        options = {} if seed is None else {"seed": seed}
        with mock.patch.object(retained.subprocess, "run", side_effect=fake_render):
            retained.prepare(self.renderer, self.source, self.scenario, directory, **options)
        return directory

    def compare(self) -> dict:
        return comparison.compare(self.board, "OUT", self.reference,
                                  firmware_commit=FIRMWARE_COMMIT)

    def test_equal_valid_artifacts_match_all_output_checks(self) -> None:
        result = self.compare()
        self.assertTrue(result["comparable"])
        self.assertTrue(result["match"])
        self.assertEqual(result["checks"], {"frames": True, "wav_sha256": True, "pcm_sha256": True})
        self.assertEqual(result["board"]["secondary"], {
            "present": True, "sha256": sha256(self.secondary.read_bytes())})
        self.assertEqual(result["reference"]["frames"], 3)

    def test_valid_output_and_frame_differences_are_not_invalid_inputs(self) -> None:
        self.write_board(b"\x09\x08\x07" * 3)
        different_pcm = self.compare()
        self.assertTrue(different_pcm["comparable"])
        self.assertFalse(different_pcm["match"])
        self.assertTrue(different_pcm["checks"]["frames"])
        self.assertFalse(different_pcm["checks"]["wav_sha256"])
        self.assertFalse(different_pcm["checks"]["pcm_sha256"])

        self.write_board(b"\x09\x08\x07" * 5)
        different_frames = self.compare()
        self.assertTrue(different_frames["comparable"])
        self.assertFalse(different_frames["match"])
        self.assertFalse(different_frames["checks"]["frames"])

    def test_invalid_artifacts_and_incomplete_markers_are_rejected(self) -> None:
        with (self.board / "OUT.WAV").open("r+b") as stream:
            stream.seek(45)
            stream.write(b"X")
        with self.assertRaises(comparison.ComparisonError):
            self.compare()

        self.write_board(DEFAULT_PCM)
        (self.board / "incomplete.JSON").write_text('{"complete":false}', encoding="utf-8")
        with self.assertRaises(comparison.ComparisonError):
            self.compare()
        (self.board / "incomplete.JSON").unlink()

        self.write_board(DEFAULT_PCM)
        manifest_path = self.board / "OUT.JSN"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["wav_sha256"] = "not-a-sha256"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(comparison.ComparisonError):
            self.compare()

        manifest_path.write_text(json.dumps(self.board_manifest_with_valid_hash()), encoding="utf-8")
        output = self.reference / "output.wav"
        output.write_bytes(output.read_bytes()[:-1])
        with self.assertRaises(comparison.ComparisonError):
            self.compare()

    def board_manifest_with_valid_hash(self) -> dict:
        original = json.loads((self.board / "OUT.JSN").read_text(encoding="utf-8"))
        original["wav_sha256"] = sha256((self.board / "OUT.WAV").read_bytes())
        return original

    def test_input_and_expected_board_identity_mismatches_are_uncomparable(self) -> None:
        cases = [
            {"source_hash": "b" * 64},
            {"scenario_hash": "c" * 64},
            {"seed": 18},
            {"secondary_name": ""},
            {"secondary_hash": "d" * 64},
            {"firmware_commit": "e" * 40},
        ]
        for values in cases:
            with self.subTest(values=values):
                self.write_board(DEFAULT_PCM, **values)
                with self.assertRaises(comparison.ComparisonError):
                    self.compare()

    def test_reference_scenario_seed_and_secondary_identity_mismatches_are_uncomparable(self) -> None:
        changed_scenario = self.scenario_bytes.replace(b'"source_gain_db":0', b'"source_gain_db":1')
        changed_scenario_reference = self.create_reference(self.root / "scenario-mismatch",
                                                           scenario_bytes=changed_scenario)
        with self.assertRaises(comparison.ComparisonError):
            comparison.compare(self.board, "OUT", changed_scenario_reference,
                               firmware_commit=FIRMWARE_COMMIT)

        self.scenario.write_bytes(self.scenario_bytes)
        changed_seed_reference = self.create_reference(self.root / "seed-mismatch", seed=18)
        with self.assertRaises(comparison.ComparisonError):
            comparison.compare(self.board, "OUT", changed_seed_reference,
                               firmware_commit=FIRMWARE_COMMIT)

        self.secondary.write_bytes(b"\x40\x50\x60" * 4)
        changed_secondary_reference = self.create_reference(
            self.root / "secondary-mismatch", secondary_bytes=b"\x61\x62\x63" * 4)
        with self.assertRaises(comparison.ComparisonError):
            comparison.compare(self.board, "OUT", changed_secondary_reference,
                               firmware_commit=FIRMWARE_COMMIT)

    def test_reference_metadata_over_one_mib_is_refused_before_retained_verifier(self) -> None:
        metadata = self.reference / "renderer.json"
        original = metadata.read_bytes()
        metadata.write_bytes(b" " * (comparison.MAX_REFERENCE_JSON_BYTES + 1))
        with mock.patch.object(comparison.retain_waveform, "verify") as verifier:
            with self.assertRaises(comparison.ComparisonError):
                self.compare()
        verifier.assert_not_called()
        metadata.write_bytes(original)

    def test_malformed_expected_identity_is_uncomparable(self) -> None:
        with self.assertRaises(comparison.ComparisonError):
            comparison.compare(self.board, "OUT", self.reference, firmware_commit="not-a-commit")

    def test_no_secondary_source_is_comparable_when_both_artifacts_omit_it(self) -> None:
        scenario = (b'{"seed":17,"source_gain_db":0,"reference_rms":0.1,'
                    b'"impairments":[]}\n')
        self.scenario.write_bytes(scenario)
        reference = self.root / "no-secondary-reference"
        with mock.patch.object(retained.subprocess, "run", side_effect=fake_render):
            retained.prepare(self.renderer, self.source, self.scenario, reference, retention="keep")
        self.write_board(DEFAULT_PCM, secondary_name="", scenario_hash=sha256(scenario))

        result = comparison.compare(self.board, "OUT", reference,
                                    firmware_commit=FIRMWARE_COMMIT)

        self.assertTrue(result["match"])
        self.assertEqual(result["board"]["secondary"], {"present": False, "sha256": ""})

    def test_multiple_reference_secondary_sources_are_refused(self) -> None:
        verified = retained.verify(self.reference)
        manifest = verified["manifest"]
        identity = manifest["request_identity"]
        identity["secondary_sources"] = [identity["secondary_sources"][0]] * 2
        with mock.patch.object(comparison.retain_waveform, "verify", return_value=verified):
            with self.assertRaises(comparison.ComparisonError):
                self.compare()

    def test_cli_returns_zero_for_match_one_for_difference_and_two_for_invalid(self) -> None:
        common = [str(self.board), "OUT", str(self.reference), "--firmware-commit", FIRMWARE_COMMIT,
]
        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch("sys.stdout", output), mock.patch("sys.stderr", errors):
            match_status = comparison.main(common)
        self.assertEqual(match_status, 0)
        self.assertTrue(json.loads(output.getvalue())["match"])

        self.write_board(b"\x09\x08\x07" * 3)
        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch("sys.stdout", output), mock.patch("sys.stderr", errors):
            different_status = comparison.main(common)
        self.assertEqual(different_status, 1)
        self.assertFalse(json.loads(output.getvalue())["match"])

        output = io.StringIO()
        errors = io.StringIO()
        with mock.patch("sys.stdout", output), mock.patch("sys.stderr", errors):
            invalid_status = comparison.main(common[:-1] + ["0" * 40])
        self.assertEqual(invalid_status, 2)
        self.assertNotEqual(errors.getvalue(), "")

    def test_actual_cpp_renderer_can_create_reference_for_synthetic_board_pair(self) -> None:
        if RENDERER is None:
            self.skipTest("pass --renderer to exercise the C++ renderer; board side remains synthetic")
        actual_reference = self.root / "actual-reference"
        try:
            retained.prepare(RENDERER, self.source, self.scenario, actual_reference, retention="keep")
        except retained.RetainError as error:
            message = str(error)
            marker = "incomplete work retained at "
            if marker in message:
                stage = Path(message.split(marker, 1)[1])
                stderr_path = stage / "renderer.stderr.txt"
                if stderr_path.is_file():
                    message += "\nrenderer stderr:\n" + stderr_path.read_text(encoding="utf-8", errors="replace")
            self.fail(message)
        actual_pcm = wav_pcm24(actual_reference / "output.wav")
        board_directory = self.root / "synthetic-board"
        board_directory.mkdir()
        previous = self.board
        self.board = board_directory
        self.write_board(actual_pcm)
        try:
            result = comparison.compare(board_directory, "OUT", actual_reference,
                                        firmware_commit=FIRMWARE_COMMIT)
        finally:
            self.board = previous
        self.assertTrue(result["match"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path)
    arguments, unittest_arguments = parser.parse_known_args()
    if arguments.renderer is not None:
        RENDERER = arguments.renderer.resolve(strict=True)
    outcome = unittest.main(argv=[__file__, *unittest_arguments], exit=False)
    raise SystemExit(not outcome.result.wasSuccessful())
