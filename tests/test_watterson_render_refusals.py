#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""wfg_watterson_render's refusals, by exit status and text (#50).

The driver's readers and writers report a failure as a Status: a source that
ends inside its data chunk, an output, sidecar or truth file that cannot be
created, an output whose writes fail after its open, an option without its
value or with a bad one. Each case here checks the exit status (1 a file
failed, 2 a usage, scenario or engine error), the lines on stderr and what was
written, so a failure reported as success, or a message that changes, fails.

Usage: test_watterson_render_refusals.py --renderer <wfg_watterson_render>
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from failing_output import failing_output

RENDERER: Path
SAMPLE_RATE = 48000
PCM24_BYTES = 3
# A fixed path, so the render needs no reference measurement.
FIXED = {"reference_rms": 0.1, "impairments": [{"type": "watterson", "paths": [{"delay_ms": 1, "fading": "fixed"}]}]}
# No reference: the driver measures the source first.
MEASURED = {"seed": 4, "reference": "auto", "impairments": [{"type": "awgn", "snr_db": 10}]}


def pcm24_wav(path: Path, frames: int, *, missing_frames: int = 0) -> None:
    """A mono 48 kHz PCM24 WAV of a tone whose data chunk declares `frames`
    frames and holds `missing_frames` fewer."""
    data = bytearray()
    for index in range(frames - missing_frames):
        value = round(0.25 * math.sin(2.0 * math.pi * 1000.0 * index / SAMPLE_RATE) * (1 << 23))
        data += struct.pack("<i", value)[:PCM24_BYTES]
    fmt = struct.pack("<HHIIHH", 1, 1, SAMPLE_RATE, SAMPLE_RATE * PCM24_BYTES, PCM24_BYTES, 8 * PCM24_BYTES)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", frames * PCM24_BYTES) + bytes(data)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


class RefusalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixed = self.write_scenario("fixed.json", FIXED)
        self.measured = self.write_scenario("measured.json", MEASURED)
        self.source = self.root / "source.wav"
        pcm24_wav(self.source, 24000)
        self.truncated = self.root / "truncated.wav"
        pcm24_wav(self.truncated, 24000, missing_frames=10000)
        self.output = self.root / "out.wav"

    def write_scenario(self, name: str, document: dict) -> Path:
        path = self.root / name
        path.write_bytes(json.dumps(document).encode("utf-8"))
        return path

    def render(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(RENDERER), *arguments], cwd=self.root, capture_output=True, text=True, check=False)

    def assert_refused(self, result: subprocess.CompletedProcess[str], status: int, *lines: str) -> None:
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        self.assertEqual(result.stderr.splitlines(), list(lines))
        self.assertEqual(result.stdout, "")

    def test_a_source_that_ends_inside_its_data_is_a_file_failure(self):
        result = self.render("--scenario", str(self.fixed), "--source", str(self.truncated), "--output", str(self.output))
        self.assert_refused(result, 1, f"cannot read {self.truncated}", "render failed")

    def test_second_source_requires_normal_renderer_before_any_output_is_created(self):
        scenario = self.write_scenario("secondary.json", {"reference_rms": 0.1, "impairments": [
            {"type": "second_source", "source_path": "secondary.wav", "si_db": 12}]})
        sidecar = self.root / "out.json"
        truth = self.root / "taps.bin"
        result = self.render("--scenario", str(scenario), "--source", str(self.source),
                             "--output", str(self.output), "--sidecar", str(sidecar), "--truth-out", str(truth))
        self.assert_refused(result, 2,
                            "second_source requires signal_lab_render; this validation driver has no secondary source provider")
        for path in (self.output, sidecar, truth):
            self.assertFalse(path.exists())

    def test_a_source_that_ends_inside_its_data_refuses_the_reference(self):
        result = self.render("--scenario", str(self.measured), "--source", str(self.truncated), "--output", str(self.output))
        self.assert_refused(result, 2, "cannot read the source")
        self.assertFalse(self.output.exists(), "nothing is written")

    def test_the_whole_source_renders(self):
        sidecar = self.root / "out.json"
        truth = self.root / "taps.bin"
        result = self.render("--scenario", str(self.fixed), "--source", str(self.source), "--output", str(self.output),
                             "--sidecar", str(sidecar), "--truth-out", str(truth))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertTrue(result.stdout.startswith(f"output={self.output} frames=24000 "), result.stdout)
        wav = self.output.read_bytes()
        self.assertEqual(len(wav), 44 + 24000 * PCM24_BYTES)
        self.assertEqual(struct.unpack_from("<I", wav, 4)[0], len(wav) - 8, "the RIFF size is patched at close")
        self.assertEqual(struct.unpack_from("<I", wav, 40)[0], 24000 * PCM24_BYTES, "the data size is patched at close")
        self.assertEqual(json.loads(sidecar.read_text(encoding="utf-8"))["frames_out"], 24000)
        records = json.loads(sidecar.read_text(encoding="utf-8"))["truth_records"]
        self.assertEqual(truth.stat().st_size, len(b"WFGTAPS1") + 20 * records)

    def test_an_output_that_cannot_be_created_is_a_file_failure(self):
        output = self.root / "missing" / "out.wav"
        result = self.render("--scenario", str(self.fixed), "--source", str(self.source), "--output", str(output))
        self.assert_refused(result, 1, f"cannot write {output}")

    def test_an_output_whose_writes_fail_after_its_open_is_a_file_failure(self):
        # QA on b057e7b: Pcm24Writer reports a failed write and a failed close; the missing directory above is
        # refused at the open. Here the open succeeds and the writes fail (tests/failing_output.py).
        with failing_output(self.root) as output:
            if output is None:
                self.skipTest("no output here that fails after its open")
            result = self.render("--scenario", str(self.fixed), "--source", str(self.source), "--output", str(output))
        self.assert_refused(result, 1, f"cannot write {output}", f"cannot finish {output}")

    def test_a_sidecar_that_cannot_be_created_is_a_file_failure(self):
        sidecar = self.root / "missing" / "out.json"
        result = self.render("--scenario", str(self.fixed), "--source", str(self.source), "--output", str(self.output),
                             "--sidecar", str(sidecar))
        self.assert_refused(result, 1, f"cannot write {sidecar}")
        self.assertTrue(self.output.exists(), "the render itself completed")

    def test_a_truth_file_that_cannot_be_created_is_a_file_failure(self):
        truth = self.root / "missing" / "taps.bin"
        result = self.render("--scenario", str(self.fixed), "--source", str(self.source), "--output", str(self.output),
                             "--truth-out", str(truth))
        self.assert_refused(result, 1, f"cannot write {truth}")
        self.assertFalse(self.output.exists(), "nothing is rendered")

    def test_an_option_without_its_value(self):
        result = self.render("--scenario", str(self.fixed), "--output")
        self.assert_refused(result, 2, "missing value for --output")

    def test_a_bad_option_value_names_its_flag(self):
        for flag, value in (("--block", "0"), ("--block", "2049"), ("--block", "x"), ("--seed", "-1"),
                            ("--silence-seconds", "0"), ("--reference-rms", "nan"), ("--bogus", "1")):
            with self.subTest(flag=flag, value=value):
                result = self.render("--scenario", str(self.fixed), "--source", str(self.source), "--output", str(self.output),
                                     flag, value)
                self.assert_refused(result, 2, f"unknown flag or bad value: {flag}")
                self.assertFalse(self.output.exists(), "nothing is written")

    def test_one_source_and_an_output_are_required(self):
        usage = ("usage: wfg_watterson_render --scenario case.json (--source in.wav | --silence-seconds S) --output out.wav"
                 " [--sidecar out.json] [--truth-out taps.bin] [--frequency-truth-out frequency.csv]"
                 " [--block 1..2048] [--seed N] [--reference-rms R]")
        for arguments in (("--source", str(self.source), "--silence-seconds", "1", "--output", str(self.output)),
                          ("--output", str(self.output)), ("--source", str(self.source))):
            with self.subTest(arguments=arguments):
                self.assert_refused(self.render("--scenario", str(self.fixed), *arguments), 2, usage)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
