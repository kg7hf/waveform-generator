#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker


from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import struct
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("analyze_capture", ROOT / "host" / "analyze_capture.py")
assert SPEC is not None and SPEC.loader is not None
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def write_wav(path: Path, fmt_tag: int, bits: int, samples: list[float | int],
              rate: int = 48_000) -> None:
    if bits == 16:
        payload = b"".join(struct.pack("<h", int(value)) for value in samples)
        channels = 1
    elif bits == 24:
        payload = b"".join(int(value).to_bytes(3, "little", signed=True) for value in samples)
        channels = 1
    else:
        payload = b"".join(struct.pack("<f", float(value)) for value in samples)
        channels = 1
    block_align = channels * bits // 8
    fmt = struct.pack("<HHIIHH", fmt_tag, channels, rate, rate * block_align,
                      block_align, bits)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(payload)) + payload
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)


class AnalyzeCaptureTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix='wfg-test-')
        self.addCleanup(temporary.cleanup)
        self.temp_dir = Path(temporary.name)

    def test_streamed_metrics_hash_and_run_counts(self) -> None:
        path = self.temp_dir / "capture.wav"
        samples = [0.25, -0.25, 0.0] * 3 + [0.125] * 5 + [0.0] * 4
        write_wav(path, 3, 32, samples)
        result = ANALYZER.analyze_capture(path, zero_threshold=4,
                                           constant_threshold=4,
                                           dropout_threshold=1.0e-6)
        self.assertEqual(result["file"]["sample_count"], len(samples))
        self.assertEqual(result["metrics"]["finite_samples"], len(samples))
        self.assertEqual(result["metrics"]["nonfinite_samples"], 0)
        self.assertEqual(result["metrics"]["clipped_samples"], 0)
        self.assertAlmostEqual(result["metrics"]["peak_linear"], 0.25)
        self.assertAlmostEqual(result["metrics"]["dc_mean"], sum(samples) / len(samples))
        self.assertEqual(result["runs"]["exact_zero_runs"], 1)
        self.assertEqual(result["runs"]["maximum_exact_zero_run_samples"], 4)
        self.assertEqual(result["runs"]["dropout_runs"], 1)
        self.assertEqual(result["runs"]["constant_runs"], 2)
        with path.open("rb") as stream:
            self.assertEqual(result["file"]["sha256"], hashlib.sha256(stream.read()).hexdigest())

    def test_rejects_non_float_capture(self) -> None:
        path = self.temp_dir / "pcm.wav"
        write_wav(path, 1, 16, [0, 100, -100])
        with self.assertRaisesRegex(ANALYZER.WavError, "capture must be"):
            ANALYZER.analyze_capture(path)

    def test_bounded_two_anchor_correlation_reports_offset_and_drift(self) -> None:
        root = self.temp_dir
        state = 0x13579BDF
        source = []
        for _ in range(2_000):
            state = (1664525 * state + 1013904223) & 0xFFFFFFFF
            source.append((state >> 16) % 2000 - 1000)
        # The capture has a deterministic 50-sample pre-roll and 10 extra
        # samples by the second anchor.  The short windows keep this test
        # quick while exercising the same bounded-window method.
        capture = [0] * 60 + source[:1_000] + [0] * 10 + source[1_000:]
        source_path = root / "source.wav"
        capture_path = root / "capture.wav"
        write_wav(source_path, 1, 24, [value * 256 for value in source], rate=48_000)
        write_wav(capture_path, 3, 32, [value / 32768.0 for value in capture], rate=48_000)
        result = ANALYZER.estimate_correlation(
            ANALYZER.inspect_wav(capture_path), ANALYZER.inspect_wav(source_path),
            window_seconds=0.01, search_seconds=0.01)
        self.assertTrue(result["available"])
        self.assertEqual(result["start"]["capture_sample"], 60)
        self.assertEqual(result["end"]["capture_sample"], 1_590)
        self.assertAlmostEqual(result["relative_ppm"], 6_578.947, places=2)


if __name__ == "__main__":
    unittest.main()
