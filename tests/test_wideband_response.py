#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Renderer-backed, channel-only spot checks for wideband Watterson response.

These checks compare coherent tone magnitudes with an independent ideal
fixed-path complex comb. They are filter-accuracy spot checks, not full E.7
conformance or board acceptance.
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
import wave

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "fixtures" / "scenarios" / "wideband"
RENDERER: Path
RATE = 48_000
N = 96_000
ANALYZE_START = 48_000  # >= 215-frame Hilbert delay + the longest 9-ms path.
ANALYZE_COUNT = 48_000
SCALE = 1 << 23
TOLERANCE_DB = 0.5
NULL_LIMIT = 0.01
BANDS = {6000: (1800, 3300, 4800), 9000: (2550, 4800, 7050),
         12000: (3300, 6300, 9300)}
OFFICIAL = ("6k-wid2", "6k-wid10", "6k-wid12", "9k-wid3", "9k-wid10",
            "9k-wid12", "12k-wid2", "12k-wid10", "12k-wid12")
MEASUREMENTS: list[str] = []


def write_pcm24(path: Path, frequencies: tuple[int, ...]) -> None:
    # Coherent integer-Hz cosines, low enough to keep the sum comfortably unclipped.
    data = bytearray()
    for frame in range(N):
        t = frame / RATE
        value = sum(0.025 * math.cos(2.0 * math.pi * f * t) for f in frequencies)
        sample = max(-SCALE, min(SCALE - 1, round(value * SCALE)))
        data.extend((sample & 0xFFFFFF).to_bytes(3, "little"))
    fmt = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 3, 3, 24)
    body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def deterministic_probe_codes() -> list[int]:
    """A repeatable, broadband, low-level PCM24 probe with no short tone period."""
    state = 0x9E3779B97F4A7C15
    values = []
    for _ in range(N):
        state ^= state >> 12
        state ^= (state << 25) & 0xFFFFFFFFFFFFFFFF
        state ^= state >> 27
        word = (state * 0x2545F4914F6CDD1D) & 0xFFFFFFFFFFFFFFFF
        values.append((((word >> 40) & 0xFFFF) - 32768) * 4)
    return values


def write_pcm24_codes(path: Path, codes: list[int]) -> None:
    data = b"".join((sample & 0xFFFFFF).to_bytes(3, "little") for sample in codes)
    fmt = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 3, 3, 24)
    body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def read_pcm24(path: Path) -> list[float]:
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("renderer output is not mono PCM24 at 48 kHz")
        raw = reader.readframes(reader.getnframes())
    values = []
    for offset in range(0, len(raw), 3):
        value = int.from_bytes(raw[offset:offset + 3], "little")
        if value & 0x800000:
            value -= 0x1000000
        values.append(value / SCALE)
    return values


def read_pcm24_codes(path: Path) -> list[int]:
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, RATE):
            raise AssertionError("renderer output is not mono PCM24 at 48 kHz")
        raw = reader.readframes(reader.getnframes())
    values = []
    for offset in range(0, len(raw), 3):
        value = int.from_bytes(raw[offset:offset + 3], "little")
        values.append(value - 0x1000000 if value & 0x800000 else value)
    return values


def dft_bin(samples: list[float], frequency: int) -> complex:
    total = 0j
    stop = ANALYZE_START + ANALYZE_COUNT
    for frame in range(ANALYZE_START, stop):
        total += samples[frame] * complex(math.cos(2 * math.pi * frequency * frame / RATE),
                                           -math.sin(2 * math.pi * frequency * frame / RATE))
    return total


def ideal_magnitude(paths: list[dict], frequency: int) -> float:
    powers = [10.0 ** (path["power_db"] / 10.0) for path in paths]
    norm = math.sqrt(sum(powers))
    response = sum((math.sqrt(power) / norm) * complex(
        math.cos(math.radians(path.get("phase_deg", 0.0)) - 2 * math.pi * frequency * path["delay_ms"] / 1000.0),
        math.sin(math.radians(path.get("phase_deg", 0.0)) - 2 * math.pi * frequency * path["delay_ms"] / 1000.0),
    ) for power, path in zip(powers, paths, strict=True))
    return abs(response)


class WidebandResponseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def render_gain(self, scenario: dict, frequencies: tuple[int, ...], label: str) -> dict[int, float]:
        # Measure channel response only; scenario source gain is upstream of the channel.
        scenario["source_gain_db"] = 0
        source = self.root / f"{label}-source.wav"
        output = self.root / f"{label}-out.wav"
        sidecar = self.root / f"{label}.json"
        scenario_path = self.root / f"{label}-scenario.json"
        write_pcm24(source, frequencies)
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        result = subprocess.run([str(RENDERER), "--scenario", str(scenario_path), "--source", str(source),
                                 "--output", str(output), "--sidecar", str(sidecar)],
                                cwd=self.root, capture_output=True, text=True, check=False,
                                creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rendered = read_pcm24(output)
        report = json.loads(sidecar.read_text(encoding="utf-8"))
        self.assertEqual(len(rendered), N)
        self.assertEqual(report["frames_in"], N)
        self.assertEqual(report["frames_out"], N)
        self.assertEqual(report["clipped_samples"], 0)
        self.assertEqual(report["noise_bandwidth_hz"], scenario["noise_bandwidth_hz"])
        input_samples = read_pcm24(source)
        gains = {}
        for frequency in frequencies:
            x, y = dft_bin(input_samples, frequency), dft_bin(rendered, frequency)
            self.assertGreater(abs(x), 1.0)
            gains[frequency] = abs(y / x)
        return gains

    def render_probe(self, scenario: dict, codes: list[int], label: str) -> tuple[list[int], dict]:
        scenario["source_gain_db"] = 0
        source = self.root / f"{label}-source.wav"
        output = self.root / f"{label}-out.wav"
        sidecar = self.root / f"{label}.json"
        scenario_path = self.root / f"{label}-scenario.json"
        write_pcm24_codes(source, codes)
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        result = subprocess.run([str(RENDERER), "--scenario", str(scenario_path), "--source", str(source),
                                 "--output", str(output), "--sidecar", str(sidecar)],
                                cwd=self.root, capture_output=True, text=True, check=False,
                                creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rendered = read_pcm24_codes(output)
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        self.assertEqual(len(rendered), len(codes))
        self.assertEqual(metadata["frames_in"], len(codes))
        self.assertEqual(metadata["frames_out"], len(codes))
        self.assertEqual(metadata["clipped_samples"], 0)
        return rendered, metadata

    def test_phase_quadrature_single_path_has_unit_gain(self) -> None:
        for bandwidth, frequencies in BANDS.items():
            scenario = json.loads((FIXTURES / f"{bandwidth // 1000}k-wid12.json").read_text(encoding="utf-8"))
            scenario["test_id"] += "-response-phase90"
            scenario["impairments"] = [{"type": "watterson", "paths": [
                {"fading": "fixed", "delay_ms": 0, "power_db": 0, "phase_deg": 90}]}]
            gains = self.render_gain(scenario, frequencies, f"phase90-{bandwidth}")
            for frequency, gain in gains.items():
                error_db = 20.0 * math.log10(gain)
                MEASUREMENTS.append(f"phase90 {bandwidth}Hz {frequency}Hz={error_db:+.3f}dB")
                self.assertLessEqual(abs(error_db), TOLERANCE_DB, (bandwidth, frequency, error_db))

    def test_official_equal_power_channels_match_ideal_comb_magnitude(self) -> None:
        for name in OFFICIAL:
            scenario = json.loads((FIXTURES / f"{name}.json").read_text(encoding="utf-8"))
            scenario["test_id"] += "-channel-response-only"
            scenario["impairments"] = [stage for stage in scenario["impairments"] if stage["type"] != "awgn"]
            watterson = next(stage for stage in scenario["impairments"] if stage["type"] == "watterson")
            bandwidth = scenario["noise_bandwidth_hz"]
            frequencies = BANDS[bandwidth]
            # Add a coherent deep-null probe for the two equal paths at 0 and 1.5 ms.
            if name == "6k-wid12":
                frequencies += (1000,)
            gains = self.render_gain(scenario, frequencies, f"{name}-comb")
            for frequency, gain in gains.items():
                expected = ideal_magnitude(watterson["paths"], frequency)
                MEASUREMENTS.append(f"{name} {frequency}Hz measured={gain:.5f} ideal={expected:.5f}")
                if expected <= NULL_LIMIT:
                    self.assertLessEqual(gain, NULL_LIMIT, (name, frequency, gain, expected))
                    continue
                error_db = 20.0 * math.log10(gain / expected)
                self.assertLessEqual(abs(error_db), TOLERANCE_DB, (name, frequency, gain, expected, error_db))

    def test_48khz_unequal_wid12_render_is_the_normalized_72_frame_delayed_path_sum(self) -> None:
        scenario = json.loads((FIXTURES / "12k-wid12-unequal-path-geometry.json").read_text(encoding="utf-8"))
        paths = next(stage for stage in scenario["impairments"] if stage["type"] == "watterson")["paths"]
        self.assertEqual([round(path["delay_ms"] * RATE / 1000.0) for path in paths], [0, 72])
        powers = [10.0 ** (path["power_db"] / 10.0) for path in paths]
        total_power = sum(powers)
        path_gains = [math.sqrt(power / total_power) for power in powers]
        self.assertAlmostEqual(path_gains[1] / path_gains[0], 10.0 ** (-3.0 / 20.0), places=14)

        probe = deterministic_probe_codes()
        single_path_scenario = json.loads(json.dumps(scenario))
        single_path_scenario["test_id"] += "-single-path-reference"
        single_path = next(stage for stage in single_path_scenario["impairments"] if stage["type"] == "watterson")
        single_path["paths"] = [single_path["paths"][0]]
        single_path_codes, _ = self.render_probe(single_path_scenario, probe, "unequal-single-path")
        two_path_codes, report = self.render_probe(scenario, probe, "unequal-two-path")
        self.assertEqual([path["delay_frames"] for path in report["watterson"]["paths"]], [0, 72])
        self.assertAlmostEqual(report["watterson"]["paths"][1]["gain_db"] -
                               report["watterson"]["paths"][0]["gain_db"], -3.0, delta=1e-10)

        # The two scenarios use the same source, reference RMS, zero phase and
        # zero Doppler. Watterson computes its analytic input once, so the
        # two-path output is the normalized sum of the one-path response and
        # that same response delayed by 72 frames. Quantizing the baseline
        # contributes at most 0.5*(a0+a1) code; output quantization adds <=0.5
        # code. A three-code bound allows another ~1.8 codes for binary32
        # multiply/add rounding at the normalized signal amplitude.
        delay = 72
        expected = [path_gains[0] * sample +
                    (path_gains[1] * single_path_codes[index - delay] if index >= delay else 0.0)
                    for index, sample in enumerate(single_path_codes)]
        errors = [abs(actual - wanted) for actual, wanted in zip(two_path_codes, expected, strict=True)]
        measured_max_error = max(errors[ANALYZE_START:])
        self.assertLessEqual(measured_max_error, 3.0,
                             f"max settled sample error {measured_max_error:.3f} PCM24 codes")

        wrong_delay = [path_gains[0] * sample +
                       (path_gains[1] * single_path_codes[index - delay + 1] if index >= delay - 1 else 0.0)
                       for index, sample in enumerate(single_path_codes)]
        missing_path = [path_gains[0] * sample for sample in single_path_codes]
        wrong_delay_error = max(abs(actual - wanted) for actual, wanted in
                                zip(two_path_codes[ANALYZE_START:], wrong_delay[ANALYZE_START:], strict=True))
        missing_path_error = max(abs(actual - wanted) for actual, wanted in
                                 zip(two_path_codes[ANALYZE_START:], missing_path[ANALYZE_START:], strict=True))
        MEASUREMENTS.append(f"unequal-wid12 max_error={measured_max_error:.6f}pcm24 "
                            f"wrong_delay_error={wrong_delay_error:.3f}pcm24 "
                            f"missing_path_error={missing_path_error:.3f}pcm24")
        self.assertGreater(wrong_delay_error, 1000.0,
                           "a 71-frame path must fail the sample-level oracle, not pass by metadata alone")
        self.assertGreater(missing_path_error, 1000.0,
                           "omitting the -3 dB second path must fail the sample-level oracle")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    args, unittest_args = parser.parse_known_args()
    RENDERER = args.renderer.resolve(strict=True)
    try:
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)
    except (AttributeError, OSError):
        pass
    result = unittest.main(argv=[__file__, *unittest_args], exit=False)
    print("RESPONSE_GAINS " + json.dumps(MEASUREMENTS, separators=(",", ":")))
    raise SystemExit(not result.result.wasSuccessful())
