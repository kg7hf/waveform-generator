#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Input-referenced SNR checks through the normal Watterson renderer.

The channel and global frequency shift are present in both renders. Their
paired output difference isolates the post-channel AWGN while preserving the
scenario's source-referenced SNR contract.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave

for _thread_variable in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
    os.environ[_thread_variable] = "1"

import numpy as np

RENDERER: Path
SAMPLE_RATE = 48_000
SOURCE_SECONDS = 9
TRIM_FRAMES = SAMPLE_RATE
PSD_SEGMENT_FRAMES = 4096
PSD_STEP_FRAMES = PSD_SEGMENT_FRAMES // 2
PCM24_SCALE = 1 << 23
SELECTED_BANDWIDTHS_HZ = (6000, 9000, 12000)
INPUT_RMS_TARGET = 0.125
SOURCE_TONE_HZ = 1800
GLOBAL_OFFSET_HZ = 75
SNR_DB = 12.0
SEED = 0x5A17
MEASUREMENTS: list[dict] = []


def write_coherent_pcm24(path: Path) -> float:
    """Write a 9-second quantized coherent tone and return its measured RMS."""
    frame_count = SOURCE_SECONDS * SAMPLE_RATE
    phase_step = 2.0 * math.pi * SOURCE_TONE_HZ / SAMPLE_RATE
    peak = math.sqrt(2.0) * INPUT_RMS_TARGET
    codes = np.rint(peak * np.sin(phase_step * np.arange(frame_count)) * PCM24_SCALE).astype(np.int32)
    encoded = bytearray()
    for code in codes:
        encoded.extend(struct.pack("<i", int(code))[:3])
    with wave.open(str(path), "wb") as writer:
        writer.setparams((1, 3, SAMPLE_RATE, frame_count, "NONE", "not compressed"))
        writer.writeframes(encoded)
    return float(np.sqrt(np.mean((codes.astype(np.float64) / PCM24_SCALE) ** 2)))


def read_pcm24(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 3, SAMPLE_RATE):
            raise AssertionError("renderer output must be mono PCM24 at 48 kHz")
        packed = np.frombuffer(reader.readframes(reader.getnframes()), dtype=np.uint8).reshape(-1, 3)
    codes = packed[:, 0].astype(np.int32) | (packed[:, 1].astype(np.int32) << 8) | (packed[:, 2].astype(np.int32) << 16)
    codes = (codes ^ 0x800000) - 0x800000
    return codes.astype(np.float64) / PCM24_SCALE


def one_sided_welch_density(samples: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Estimate one-sided PSD using periodic Hann windows and 50% overlap."""
    if samples.size < PSD_SEGMENT_FRAMES:
        raise ValueError("not enough samples for one PSD segment")
    window = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(PSD_SEGMENT_FRAMES) / PSD_SEGMENT_FRAMES)
    scale = SAMPLE_RATE * float(np.sum(window * window))
    starts = range(0, samples.size - PSD_SEGMENT_FRAMES + 1, PSD_STEP_FRAMES)
    accumulated = np.zeros(PSD_SEGMENT_FRAMES // 2 + 1, dtype=np.float64)
    count = 0
    for start in starts:
        transformed = np.fft.rfft(samples[start:start + PSD_SEGMENT_FRAMES] * window)
        accumulated += (transformed.real * transformed.real + transformed.imag * transformed.imag) / scale
        count += 1
    density = accumulated / count
    density[1:-1] *= 2.0
    frequencies = np.fft.rfftfreq(PSD_SEGMENT_FRAMES, 1.0 / SAMPLE_RATE)
    return frequencies, density


class WidebandInputReferencedSNRTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "coherent-source.wav"
        self.measured_input_rms = write_coherent_pcm24(self.source)

    def render(self, bandwidth_hz: int, *, include_noise: bool, stem: str) -> tuple[Path, dict]:
        stages = [
            {"type": "watterson", "paths": [
                {"fading": "fixed", "delay_ms": 0, "power_db": 0},
                {"fading": "fixed", "delay_ms": 2, "power_db": 0},
            ]},
            {"type": "frequency_shift", "mode": "offset", "offset_hz": GLOBAL_OFFSET_HZ},
        ]
        if include_noise:
            stages.append({"type": "awgn", "snr_db": SNR_DB})
        scenario = {
            "schema": "signal-lab.scenario/1",
            "test_id": f"wideband-input-snr-{bandwidth_hz}-{'noisy' if include_noise else 'clean'}",
            "seed": SEED,
            "noise_bandwidth_hz": bandwidth_hz,
            "source_gain_db": 0,
            # Pin the full source interval; automatic active-window detection
            # excludes endpoint zero samples and has a slightly different RMS.
            "reference": {"start_seconds": 0, "duration_seconds": SOURCE_SECONDS},
            "clip_policy": "reject",
            "impairments": stages,
        }
        scenario_path = self.root / f"{stem}.json"
        output_path = self.root / f"{stem}.wav"
        sidecar_path = self.root / f"{stem}-sidecar.json"
        scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
        result = subprocess.run(
            [str(RENDERER), "--scenario", str(scenario_path), "--source", str(self.source),
             "--output", str(output_path), "--sidecar", str(sidecar_path)],
            cwd=self.root, capture_output=True, text=True, check=False,
            creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertTrue(output_path.is_file())
        metadata = json.loads(sidecar_path.read_text(encoding="utf-8"))
        self.assertEqual(metadata["frames_in"], SOURCE_SECONDS * SAMPLE_RATE)
        self.assertEqual(metadata["frames_out"], SOURCE_SECONDS * SAMPLE_RATE)
        self.assertEqual(metadata["source_gain_db"], 0)
        self.assertAlmostEqual(metadata["reference_rms"], self.measured_input_rms, delta=1e-12)
        self.assertEqual(metadata["noise_bandwidth_hz"], bandwidth_hz)
        return output_path, metadata

    def test_snr_uses_pre_channel_source_rms_for_all_widebands(self) -> None:
        for bandwidth_hz in SELECTED_BANDWIDTHS_HZ:
            with self.subTest(bandwidth_hz=bandwidth_hz):
                clean_path, clean_metadata = self.render(bandwidth_hz, include_noise=False,
                                                         stem=f"{bandwidth_hz}-clean")
                noisy_path, noisy_metadata = self.render(bandwidth_hz, include_noise=True,
                                                         stem=f"{bandwidth_hz}-noisy")
                self.assertAlmostEqual(clean_metadata["reference_rms"], noisy_metadata["reference_rms"], delta=1e-15)

                clean = read_pcm24(clean_path)[TRIM_FRAMES:]
                noisy = read_pcm24(noisy_path)[TRIM_FRAMES:]
                self.assertEqual(clean.size, 8 * SAMPLE_RATE)
                self.assertEqual(noisy.size, clean.size)

                # Two equal fixed paths interfere at the 1800 Hz input tone;
                # this guards against silently redefining SNR from channel output.
                clean_channel_rms = float(np.sqrt(np.mean(clean * clean)))
                channel_delta_db = 20.0 * math.log10(clean_channel_rms / self.measured_input_rms)
                self.assertGreater(abs(channel_delta_db), 1.0, channel_delta_db)

                frequencies, noise_density = one_sided_welch_density(noisy - clean)
                band_high_hz = 300 + bandwidth_hz
                interior = (frequencies >= 800.0) & (frequencies <= band_high_hz - 500.0)
                self.assertGreater(int(np.count_nonzero(interior)), 0, "PSD has no selected-band interior bins")
                estimated_n0 = float(np.mean(noise_density[interior]))
                self.assertTrue(math.isfinite(estimated_n0) and estimated_n0 > 0.0, estimated_n0)

                measured_snr_db = 10.0 * math.log10(
                    self.measured_input_rms ** 2 / (estimated_n0 * bandwidth_hz))
                MEASUREMENTS.append({"bandwidth_hz": bandwidth_hz, "input_rms": self.measured_input_rms,
                                     "channel_gain_db": channel_delta_db, "snr_db": measured_snr_db})
                self.assertAlmostEqual(measured_snr_db, SNR_DB, delta=0.1,
                                       msg=f"band={bandwidth_hz}Hz, estimated N0={estimated_n0:.12g}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    result = unittest.main(argv=[__file__, *unittest_arguments], exit=False)
    print("WIDEBAND_INPUT_SNR " + json.dumps(MEASUREMENTS, separators=(",", ":")))
    raise SystemExit(not result.result.wasSuccessful())
