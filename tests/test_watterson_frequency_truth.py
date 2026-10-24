#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""The separate frequency-truth export, without changing WFGTAPS1."""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from failing_output import failing_output

RENDERER: Path
SAMPLE_RATE = 48000
FRAMES = 4800
FIXED_PATH = {"type": "watterson", "paths": [{"delay_ms": 1, "fading": "fixed"}]}


def pcm24_wav(path: Path) -> None:
    data = bytearray()
    for frame in range(FRAMES):
        value = round(0.25 * math.sin(2.0 * math.pi * 1000.0 * frame / SAMPLE_RATE) * (1 << 23))
        data += struct.pack("<i", value)[:3]
    fmt = struct.pack("<HHIIHH", 1, 1, SAMPLE_RATE, SAMPLE_RATE * 3, 3, 24)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


class FrequencyTruthTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.wav"
        pcm24_wav(self.source)
        self.output = self.root / "out.wav"
        self.truth = self.root / "taps.bin"
        self.frequency_truth = self.root / "frequency.csv"

    def scenario(self, name: str, impairments: list[dict]) -> Path:
        path = self.root / name
        path.write_text(json.dumps({"reference_rms": 0.1, "impairments": impairments}), encoding="utf-8")
        return path

    def render(self, scenario: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(RENDERER), "--scenario", str(scenario), "--source", str(self.source),
                               "--output", str(self.output), *arguments], cwd=self.root,
                              capture_output=True, text=True, check=False)

    def frequency_rows(self) -> tuple[list[str], list[tuple[int, float]]]:
        lines = self.frequency_truth.read_text(encoding="ascii").splitlines()
        header_end = next(index for index, line in enumerate(lines) if line == "engine_input_frame,instantaneous_offset_hz")
        metadata = lines[:header_end]
        rows = [(int(frame), float(frequency)) for frame, frequency in csv.reader(io.StringIO("\n".join(lines[header_end + 1:])))]
        return metadata, rows

    def test_frequency_shift_only_exports_absolute_frame_truth_and_alignment(self):
        scenario = self.scenario("offset.json", [{"type": "frequency_shift", "mode": "offset", "offset_hz": -12.5}])
        result = self.render(scenario, "--frequency-truth-out", str(self.frequency_truth))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        metadata, rows = self.frequency_rows()
        self.assertEqual(metadata[0], "WFGFREQ1")
        self.assertIn("# sample_rate_hz=48000", metadata)
        self.assertIn("# stride_frames=64", metadata)
        self.assertIn("# engine_input_frames=4800", metadata)
        self.assertIn("# mode=offset", metadata)
        self.assertIn("# frame_coordinate=absolute_engine_input_frame_at_frequency_shift", metadata)
        self.assertIn("# frequency_shift_hilbert_delay_frames=215", metadata)
        self.assertIn("# watterson_hilbert_delay_frames=0", metadata)
        self.assertIn("# total_hilbert_delay_frames=215", metadata)
        self.assertEqual([frame for frame, _ in rows], list(range(0, FRAMES, 64)))
        self.assertTrue(all(math.isclose(frequency, -12.5, abs_tol=1e-12) for _, frequency in rows))

    def test_watterson_and_frequency_shift_emit_both_truth_files_and_keep_tap_bytes(self):
        scenario = self.scenario("channel-offset.json", [FIXED_PATH,
            {"type": "frequency_shift", "mode": "offset", "offset_hz": 20.0}])
        result = self.render(scenario, "--truth-out", str(self.truth), "--frequency-truth-out", str(self.frequency_truth))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.truth.read_bytes().startswith(b"WFGTAPS1"))
        self.assertGreater(self.truth.stat().st_size, len(b"WFGTAPS1"))
        metadata, rows = self.frequency_rows()
        self.assertIn("# watterson_hilbert_delay_frames=215", metadata)
        self.assertIn("# total_hilbert_delay_frames=430", metadata)
        self.assertEqual([frame for frame, _ in rows], list(range(0, FRAMES, 64)))
        taps_with_both = self.truth.read_bytes()
        audio_with_both = self.output.read_bytes()

        baseline_truth = self.root / "baseline-taps.bin"
        baseline = self.render(scenario, "--truth-out", str(baseline_truth))
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        self.assertEqual(taps_with_both, baseline_truth.read_bytes(), "the separate export does not change WFGTAPS1 records")
        self.assertEqual(audio_with_both, self.output.read_bytes(), "the separate export does not change the rendered WAV")

    def test_sweep_values_are_sampled_at_the_documented_engine_frames(self):
        scenario = self.scenario("sweep.json", [{"type": "frequency_shift", "mode": "sweep",
                                                 "amplitude_hz": 75, "slope_hz_per_second": 3.5}])
        result = self.render(scenario, "--frequency-truth-out", str(self.frequency_truth), "--block", "37")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        metadata, rows = self.frequency_rows()
        self.assertIn("# mode=sweep", metadata)
        self.assertEqual([frame for frame, _ in rows], list(range(0, FRAMES, 64)))
        for frame, frequency in rows:
            with self.subTest(frame=frame):
                self.assertAlmostEqual(frequency, -75.0 + 3.5 * frame / SAMPLE_RATE, delta=1e-12)

    def test_frequency_truth_refuses_a_scenario_without_the_stage(self):
        scenario = self.scenario("clean.json", [])
        result = self.render(scenario, "--frequency-truth-out", str(self.frequency_truth))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--frequency-truth-out requires a frequency_shift scenario stage", result.stderr)
        self.assertFalse(self.frequency_truth.exists())
        self.assertFalse(self.output.exists())

    def test_frequency_truth_cannot_clobber_another_path(self):
        scenario = self.scenario("offset.json", [{"type": "frequency_shift", "mode": "offset", "offset_hz": 1.0}])
        result = self.render(scenario, "--frequency-truth-out", str(self.output))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("artifact paths must not overwrite inputs or one another", result.stderr)
        self.assertFalse(self.output.exists())

    def test_output_hardlink_to_source_is_refused(self):
        scenario = self.scenario("offset.json", [{"type": "frequency_shift", "mode": "offset", "offset_hz": 1.0}])
        alias = self.root / "source-alias.wav"

        try:
            os.link(self.source, alias)
        except OSError as error:
            self.skipTest(f"hardlink creation is unavailable: {error}")

        original = self.source.read_bytes()
        result = subprocess.run([str(RENDERER), "--scenario", str(scenario), "--source", str(self.source),
                                 "--output", str(alias)], cwd=self.root, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("artifact paths must not overwrite inputs or one another", result.stderr)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(alias.read_bytes(), original)

    def test_frequency_truth_open_failure_is_a_file_failure(self):
        scenario = self.scenario("offset.json", [{"type": "frequency_shift", "mode": "offset", "offset_hz": 1.0}])
        missing = self.root / "missing" / "frequency.csv"
        result = self.render(scenario, "--frequency-truth-out", str(missing))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"cannot write {missing}", result.stderr)
        self.assertFalse(self.output.exists())

    def test_frequency_truth_write_or_close_failure_is_a_file_failure(self):
        scenario = self.scenario("offset.json", [{"type": "frequency_shift", "mode": "offset", "offset_hz": 1.0}])
        with failing_output(self.root, "locked-frequency.csv") as blocked:
            if blocked is None:
                self.skipTest("no output here that fails after its open")
            result = self.render(scenario, "--frequency-truth-out", str(blocked))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertTrue(any(line.startswith(("cannot write ", "cannot finish ")) for line in result.stderr.splitlines()), result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
