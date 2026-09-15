# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""PCM24 boundary tests: independent packed bytes, signed extrema, precision and RIFF geometry."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
from signal_lab.render import render
from signal_lab.wav_io import (WavError, pcm_region_sha256, quantize_pcm24,
                               read_float, require_canonical, wav_info, write_pcm16, write_pcm24)
from prepare_demo_wav import prepare
from m110_reference import generate


class Pcm24Tests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_signed_bytes_preserve_low_eight_bits_and_odd_padding(self):
        values = [-8388608, -257, -1, 0, 1, 257, 8388607]
        path = self.root / "edge.wav"
        self.assertEqual(write_pcm24(path, np.array(values) / 8388608), 0)
        raw = path.read_bytes()
        expected = b"".join(value.to_bytes(3, "little", signed=True) for value in values)
        self.assertEqual(raw[44:-1], expected)
        self.assertEqual(raw[-1:], b"\0")
        self.assertEqual(struct.unpack_from("<I", raw, 4)[0], len(raw) - 8)
        self.assertEqual(struct.unpack_from("<I", raw, 40)[0], len(expected))
        decoded, info = read_float(path)
        require_canonical(info)
        np.testing.assert_array_equal(decoded * 8388608, values)
        self.assertEqual(pcm_region_sha256(path), hashlib.sha256(expected).hexdigest())

    def test_half_even_rounding_and_saturation(self):
        pcm, clipped = quantize_pcm24(np.array([-2.5, -1.5, -0.5, 0.5, 1.5, 2.5]) / 8388608)
        np.testing.assert_array_equal(pcm, [-2, -2, 0, 0, 2, 2])
        self.assertEqual(clipped, 0)
        pcm, clipped = quantize_pcm24([-2, 2, -1, 8388607 / 8388608])
        np.testing.assert_array_equal(pcm, [-8388608, 8388607, -8388608, 8388607])
        self.assertEqual(clipped, 2)

    def test_nonfinite_rejected_before_conversion(self):
        for value in [np.nan, np.inf, -np.inf]:
            with self.subTest(value=value), self.assertRaises(WavError):
                quantize_pcm24([value])

    def test_truncated_sample_rejected(self):
        path = self.root / "short.wav"
        write_pcm24(path, [0.1, -0.1])
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaises(WavError):
            read_float(path)

    def test_renderer_preserves_one_lsb_pcm24_and_reports_format(self):
        source = self.root / "source.wav"
        write_pcm24(source, np.array([-257, -1, 0, 1, 257]) / 8388608)
        scenario = {"test_id": "lsb", "source": {"path": "source.wav"}, "source_gain_db": 0}
        first = render(scenario, self.root / "first.wav", base_dir=self.root, block=1)
        second = render(scenario, self.root / "second.wav", base_dir=self.root, block=4)
        self.assertEqual(first["output"]["pcm_format"], "pcm_s24le")
        self.assertEqual(first["output"]["pcm"]["sha256"], pcm_region_sha256(source))
        self.assertEqual(first["output"]["pcm"]["sha256"], second["output"]["pcm"]["sha256"])

    def test_legacy_input_promoted_to_pcm24_output(self):
        source = self.root / "legacy.wav"
        write_pcm16(source, [-0.25, 0.25])
        with self.assertRaises(WavError):
            require_canonical(wav_info(source))
        require_canonical(wav_info(source), allow_legacy_pcm16=True)
        result = render({"test_id": "legacy", "source": {"path": "legacy.wav"}, "source_gain_db": 0},
                        self.root / "out.wav", base_dir=self.root)
        self.assertEqual(result["output"]["encoding"], "pcm_s24le")
        self.assertEqual(result["output"]["pcm"]["data_bytes"], 6)

    def test_demo_conversion_produces_stageable_mono_pcm24(self):
        source = self.root / "stereo.wav"
        with wave.open(str(source), "wb") as writer:
            writer.setparams((2, 2, 11025, 0, "NONE", "not compressed"))
            writer.writeframes(struct.pack("<hhhh", 8192, 4096, -8192, -4096) * 100)
        output = self.root / "demo.wav"
        metadata = prepare(source, output, 0.1)
        samples, info = read_float(output)
        require_canonical(info)
        self.assertEqual(len(samples), 4800)
        self.assertLessEqual(float(np.max(np.abs(samples))), 0.25)
        self.assertEqual(metadata["source_channels"], 2)
        with self.assertRaises(ValueError):
            prepare(source, output, 0.1)

    def test_external_reference_retains_legacy_bytes_and_promotes_output(self):
        source = self.root / "external.wav"
        payload = self.root / "payload.bin"
        payload.write_bytes(b"TEST")
        transmitter = self.root / "fake-transmitter"
        transmitter.write_bytes(b"test fixture")
        def producer(*args, **kwargs):
            write_pcm16(source, [0.125, -0.25, 0.25, -0.125, 0, 0])
            return type("Result", (), {"returncode": 0, "stdout": "wrote 4 waveform samples at 48000 Hz (1 body blocks)", "stderr": ""})()
        with patch("m110_reference.subprocess.run", side_effect=producer):
            metadata = generate(transmitter, "600L", source, payload, 4, 0.1, payload)
        require_canonical(wav_info(source))
        original = source.with_suffix(".producer-pcm16.wav")
        self.assertEqual(wav_info(original)["encoding"], "pcm_s16le")
        np.testing.assert_array_equal(read_float(source)[0], read_float(original)[0])
        self.assertIsNotNone(metadata["producer"]["original_pcm16"])


if __name__ == "__main__":
    unittest.main()
