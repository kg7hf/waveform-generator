#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Table D-LXV wideband scenario configuration and realised channel reports."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "fixtures" / "scenarios" / "wideband"
RENDERER: Path
SAMPLE_RATE = 48_000
FRAME_COUNT = 48_000
PCM24_SCALE = 1 << 23
PCM24_BYTES = 3

# Table D-LXV, D.6.2. The entries are path delays in milliseconds.
OFFICIAL_PROFILES = {
    "6k-wid2": (6000, (0, 2, 9)),
    "6k-wid10": (6000, (0, 1.5, 3, 5)),
    "6k-wid12": (6000, (0, 1.5)),
    "9k-wid3": (9000, (0, 3, 8)),
    "9k-wid10": (9000, (0, 2, 4.5)),
    "9k-wid12": (9000, (0, 1.5)),
    "12k-wid2": (12000, (0, 2.5, 8.5)),
    "12k-wid10": (12000, (0, 1.5, 3, 5)),
    "12k-wid12": (12000, (0, 1.5)),
}


def write_source(path: Path) -> None:
    """Write a short deterministic PCM24 tone for configuration/report checks."""
    samples = bytearray()
    for frame in range(FRAME_COUNT):
        value = round(0.1 * math.sin(2.0 * math.pi * 1000.0 * frame / SAMPLE_RATE) * PCM24_SCALE)
        samples.extend(struct.pack("<i", value)[:PCM24_BYTES])
    fmt = struct.pack("<HHIIHH", 1, 1, SAMPLE_RATE, SAMPLE_RATE * PCM24_BYTES, PCM24_BYTES, 24)
    body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(samples)) + samples
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def expected_band_metadata(bandwidth_hz: int) -> dict[str, int]:
    return {
        "noise_bandwidth_hz": bandwidth_hz,
        "noise_band_low_hz": 300,
        "noise_band_high_hz": 300 + bandwidth_hz,
        "noise_band_center_hz": 300 + bandwidth_hz // 2,
    }


class WidebandProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.wav"
        write_source(self.source)

    def render(self, scenario: Path, stem: str, target_scenario: Path | None = None) -> dict:
        output = self.root / f"{stem}.wav"
        sidecar = self.root / f"{stem}.json"
        arguments = [str(RENDERER), "--scenario", str(scenario), "--source", str(self.source),
                     "--output", str(output), "--sidecar", str(sidecar)]

        if target_scenario is not None:
            arguments.extend(("--target-scenario", str(target_scenario)))

        result = subprocess.run(
            arguments,
            cwd=self.root, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertTrue(output.is_file())
        return json.loads(sidecar.read_text(encoding="utf-8"))

    def assert_band_metadata(self, report: dict, bandwidth_hz: int) -> None:
        for name, expected in expected_band_metadata(bandwidth_hz).items():
            with self.subTest(field=name):
                self.assertEqual(report[name], expected)
        enbw = report["noise_band_enbw_hz"]
        self.assertTrue(math.isfinite(enbw) and enbw > 0.0, enbw)

    def test_table_d_lxv_equal_power_rows_have_their_whole_sample_paths(self) -> None:
        for name, (bandwidth_hz, delays_ms) in OFFICIAL_PROFILES.items():
            with self.subTest(profile=name):
                scenario_path = FIXTURES / f"{name}.json"
                scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
                self.assertEqual(scenario["noise_bandwidth_hz"], bandwidth_hz)
                self.assertEqual(scenario["test_id"], f"D.6.2-Table-D-LXV-{bandwidth_hz // 1000}kHz-{name.split('-')[1].upper()}")
                report = self.render(scenario_path, name)
                self.assert_band_metadata(report, bandwidth_hz)
                paths = report["watterson"]["paths"]
                self.assertEqual(len(paths), len(delays_ms))
                expected_path_power = 1.0 / len(delays_ms)
                expected_path_gain_db = 10.0 * math.log10(expected_path_power)
                for index, (path, delay_ms) in enumerate(zip(paths, delays_ms, strict=True)):
                    expected_frames = round(delay_ms * SAMPLE_RATE / 1000)
                    with self.subTest(path=index):
                        self.assertEqual(path["delay_frames"], expected_frames)
                        self.assertTrue(math.isclose(path["amplitude"] ** 2, expected_path_power, rel_tol=1e-12), path)
                        self.assertTrue(math.isclose(path["gain_db"], expected_path_gain_db, abs_tol=1e-12), path)

    def test_unequal_wid12_fixture_checks_path_geometry_and_power_only(self) -> None:
        scenario_path = FIXTURES / "12k-wid12-unequal-path-geometry.json"
        scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
        self.assertEqual(scenario["test_id"], "D.6.2-Table-D-LXV-48kHz-WID12-unequal-path-geometry-only")
        self.assertEqual([stage["type"] for stage in scenario["impairments"]], ["watterson"])
        report = self.render(scenario_path, "12k-wid12-unequal-path-geometry")
        self.assert_band_metadata(report, 12000)
        paths = report["watterson"]["paths"]
        self.assertEqual([path["delay_frames"] for path in paths], [0, 72])
        power_ratio = paths[1]["amplitude"] ** 2 / paths[0]["amplitude"] ** 2
        self.assertAlmostEqual(10.0 * math.log10(power_ratio), -3.0, delta=1e-10)

    def test_omitting_bandwidth_keeps_the_legacy_three_kilohertz_default(self) -> None:
        source = json.loads((FIXTURES / "6k-wid12.json").read_text(encoding="utf-8"))
        source.pop("noise_bandwidth_hz")
        scenario = self.root / "default-band.json"
        scenario.write_text(json.dumps(source), encoding="utf-8")
        report = self.render(scenario, "default-band")
        self.assert_band_metadata(report, 3000)

    def test_selected_band_survives_target_scenario_serialization(self) -> None:
        scenario = FIXTURES / "9k-wid10.json"
        target_scenario = self.root / "normalized.json"
        self.render(scenario, "target-scenario", target_scenario)
        normalized = json.loads(target_scenario.read_text(encoding="utf-8"))
        self.assertEqual(normalized["noise_bandwidth_hz"], 9000)
        report = self.render(target_scenario, "normalized-target")
        self.assert_band_metadata(report, 9000)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
