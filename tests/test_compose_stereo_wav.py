#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Offline checks for composing two renderer PCM24 files into stereo."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import zlib
from unittest.mock import patch

HOST = Path(__file__).resolve().parents[1] / "host"
sys.path.insert(0, str(HOST))

import compose_stereo_wav as stereo  # noqa: E402

RENDERER: Path | None = None


def pack_pcm24(samples: list[int]) -> bytes:
    return b"".join(int(sample).to_bytes(3, "little", signed=True) for sample in samples)


def fnv1a64(data: bytes) -> str:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def write_mono_pcm24(path: Path, samples: list[int]) -> bytes:
    packed = pack_pcm24(samples)
    with wave.open(str(path), "wb") as output:
        output.setparams((1, 3, 48000, 0, "NONE", "not compressed"))
        output.writeframes(packed)
    if len(packed) & 1:
        padded = bytearray(path.read_bytes())
        padded.append(0)
        struct.pack_into("<I", padded, 4, len(padded) - 8)
        path.write_bytes(padded)
    return packed


def read_wave(path: Path) -> tuple[tuple[int, int, int, int], bytes]:
    with wave.open(str(path), "rb") as source:
        geometry = (source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getnframes())
        raw = source.readframes(source.getnframes())
        if source.readframes(1):
            raise AssertionError("WAV reader returned data beyond declared frames")
    return geometry, raw


class ComposeStereoWavTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.left_samples = [-8388608, -1, 0, 8388607, -257, 1]
        self.right_samples = [1, -257, 8388607, -8388608, 0, -1]
        self.left_wav = self.root / "left.wav"
        self.right_wav = self.root / "right.wav"
        self.left_pcm = write_mono_pcm24(self.left_wav, self.left_samples)
        self.right_pcm = write_mono_pcm24(self.right_wav, self.right_samples)
        self.left_scenario = self.root / "left.scn.json"
        self.right_scenario = self.root / "right.scn.json"
        self.left_scenario.write_text('{"seed":101,"impairments":[]}', encoding="utf-8")
        self.right_scenario.write_text('{"seed":202,"impairments":[]}', encoding="utf-8")
        self.left_sidecar = self.root / "left.render.json"
        self.right_sidecar = self.root / "right.render.json"
        self._write_sidecar(self.left_sidecar, self.left_wav, self.left_pcm, self.left_scenario, 101)
        self._write_sidecar(self.right_sidecar, self.right_wav, self.right_pcm, self.right_scenario, 202)
        self.output_wav = self.root / "stereo.wav"
        self.output_sidecar = self.root / "stereo.json"

    @staticmethod
    def _write_sidecar(path: Path, wav_path: Path, pcm: bytes, scenario: Path, seed: int) -> None:
        document = {
            "schema": "signal-lab.render/1",
            "generator": "signal-lab-cpp/0.2.0",
            "output_encoding": "pcm_s24le",
            "scenario_path": str(scenario),
            "source_path": f"{seed}.source.wav",
            "output_path": str(wav_path),
            "seed": seed,
            "frames_in": len(pcm) // 3,
            "frames_out": len(pcm) // 3,
            "output_digest": fnv1a64(pcm),
            "source_digest": f"{seed:016x}",
            "stages": [],
        }
        path.write_text(json.dumps(document, sort_keys=True), encoding="utf-8")

    def compose(self, *, left_wav: Path | None = None, left_sidecar: Path | None = None,
                left_scenario: Path | None = None, right_wav: Path | None = None,
                right_sidecar: Path | None = None, right_scenario: Path | None = None,
                output_wav: Path | None = None, output_sidecar: Path | None = None):
        return stereo.compose(
            left_wav or self.left_wav, left_sidecar or self.left_sidecar,
            left_scenario or self.left_scenario, right_wav or self.right_wav,
            right_sidecar or self.right_sidecar, right_scenario or self.right_scenario,
            output_wav or self.output_wav, output_sidecar or self.output_sidecar,
        )

    def test_literal_signed_extremes_are_interleaved_left_then_right_without_scaling(self):
        report = self.compose()
        geometry, raw = read_wave(self.output_wav)
        expected = b"".join(pack_pcm24([left, right]) for left, right in zip(self.left_samples, self.right_samples))
        self.assertEqual(geometry, (2, 3, 48000, len(self.left_samples)))
        self.assertEqual(raw, expected)
        self.assertEqual(report["schema"], "waveform-generator.stereo-compose/1")
        self.assertEqual(report["sample_rate_hz"], 48000)
        self.assertEqual(report["channels"], 2)
        self.assertEqual(report["bits_per_sample"], 24)
        self.assertEqual(report["frames"], len(self.left_samples))
        self.assertEqual(report["channel_order"], {"left": "channel0_upper", "right": "channel1_lower"})
        self.assertEqual(report["left"]["render"]["seed"], 101)
        self.assertEqual(report["right"]["render"]["seed"], 202)
        self.assertEqual(report["left"]["render"]["source_digest"], "0000000000000065")
        self.assertEqual(report["right"]["render"]["source_digest"], "00000000000000ca")
        for side, scenario in (("left", self.left_scenario), ("right", self.right_scenario)):
            provenance = report[side]["scenario"]
            self.assertEqual(provenance["sha256"], hashlib.sha256(scenario.read_bytes()).hexdigest())
            self.assertEqual(provenance["binding"], "current_file_hash_unverified_against_render")
            sidecar_path = self.left_sidecar if side == "left" else self.right_sidecar
            wav_path = self.left_wav if side == "left" else self.right_wav
            pcm = self.left_pcm if side == "left" else self.right_pcm
            self.assertEqual(report[side]["render_sidecar_sha256"], hashlib.sha256(sidecar_path.read_bytes()).hexdigest())
            self.assertEqual(report[side]["wav_file_sha256"], hashlib.sha256(wav_path.read_bytes()).hexdigest())
            self.assertEqual(report[side]["wav_pcm_sha256"], hashlib.sha256(pcm).hexdigest())

        output = report["output"]
        left_crc = zlib.crc32(self.left_pcm) & 0xFFFFFFFF
        right_crc = zlib.crc32(self.right_pcm) & 0xFFFFFFFF
        self.assertEqual(output["crc32_left"], f"{left_crc:08x}")
        self.assertEqual(output["crc32_right"], f"{right_crc:08x}")
        self.assertEqual(output["crc32_format"], "zlib-crc32-packed-pcm24")
        self.assertEqual(output["data_sha256"], hashlib.sha256(expected).hexdigest())
        self.assertEqual(output["file_sha256"], hashlib.sha256(self.output_wav.read_bytes()).hexdigest())
        self.assertEqual(output["crc32_left"], "9cf02c9d")
        self.assertEqual(output["crc32_right"], "a8bbe59d")

    def test_identical_inputs_produce_identical_channels_and_reported_crc(self):
        same_wav = self.root / "same.wav"
        same_pcm = write_mono_pcm24(same_wav, self.left_samples)
        same_scenario = self.root / "same.json"
        same_scenario.write_text('{"seed":303}', encoding="utf-8")
        same_sidecar = self.root / "same.render.json"
        self._write_sidecar(same_sidecar, same_wav, same_pcm, same_scenario, 303)
        report = self.compose(right_wav=same_wav, right_sidecar=same_sidecar, right_scenario=same_scenario)
        geometry, raw = read_wave(self.output_wav)
        self.assertEqual(geometry, (2, 3, 48000, len(self.left_samples)))
        self.assertEqual(raw, b"".join(pack_pcm24([sample, sample]) for sample in self.left_samples))
        self.assertEqual(report["output"]["crc32_left"], report["output"]["crc32_right"])

    def test_odd_frame_inputs_with_riff_pad_compose_without_padding_the_pcm_digest(self):
        left_values = [-1, 0x123456, -8388608]
        right_values = [8388607, -257, 7]
        left_wav, right_wav = self.root / "odd-left.wav", self.root / "odd-right.wav"
        left_data = write_mono_pcm24(left_wav, left_values)
        right_data = write_mono_pcm24(right_wav, right_values)
        self.assertEqual(left_wav.stat().st_size, 44 + len(left_data) + 1)
        self.assertEqual(right_wav.stat().st_size, 44 + len(right_data) + 1)
        left_sidecar, right_sidecar = self.root / "odd-left.json", self.root / "odd-right.json"
        self._write_sidecar(left_sidecar, left_wav, left_data, self.left_scenario, 101)
        self._write_sidecar(right_sidecar, right_wav, right_data, self.right_scenario, 202)
        report = self.compose(left_wav=left_wav, left_sidecar=left_sidecar,
                              right_wav=right_wav, right_sidecar=right_sidecar)
        geometry, raw = read_wave(self.output_wav)
        self.assertEqual(geometry, (2, 3, 48000, 3))
        self.assertEqual(raw, b"".join(pack_pcm24([left, right]) for left, right in zip(left_values, right_values)))
        self.assertEqual(report["left"]["wav_pcm_sha256"], hashlib.sha256(left_data).hexdigest())
        self.assertEqual(report["right"]["wav_pcm_sha256"], hashlib.sha256(right_data).hexdigest())
        self.assertEqual(len(raw), 18)  # Stereo data is frame-aligned; the mono input pad bytes are not copied.

    def test_odd_frame_mono_inputs_require_a_present_zero_riff_pad(self):
        values = [-17, 0x123456, -8388608]
        valid = self.root / "pad-valid.wav"
        pcm = write_mono_pcm24(valid, values)
        missing_pad = self.root / "pad-missing.wav"
        missing_pad.write_bytes(valid.read_bytes()[:-1])
        nonzero_pad = self.root / "pad-nonzero.wav"
        damaged = bytearray(valid.read_bytes())
        damaged[-1] = 0x7F
        nonzero_pad.write_bytes(damaged)
        right = self.root / "pad-right.wav"
        right_pcm = write_mono_pcm24(right, values)
        right_sidecar = self.root / "pad-right.json"
        self._write_sidecar(right_sidecar, right, right_pcm, self.right_scenario, 202)

        for label, left in (("missing", missing_pad), ("nonzero", nonzero_pad)):
            left_sidecar = self.root / f"pad-{label}.json"
            self._write_sidecar(left_sidecar, left, pcm, self.left_scenario, 101)
            output_wav = self.root / f"pad-{label}-stereo.wav"
            output_sidecar = self.root / f"pad-{label}-stereo.json"
            with self.subTest(pad=label), self.assertRaises(stereo.ComposeError):
                self.compose(left_wav=left, left_sidecar=left_sidecar,
                             right_wav=right, right_sidecar=right_sidecar,
                             output_wav=output_wav, output_sidecar=output_sidecar)
            self.assertFalse(output_wav.exists())
            self.assertFalse(output_sidecar.exists())

    def test_compose_refuses_geometry_format_digest_and_provenance_mismatches(self):
        changed = self.root / "short.wav"
        write_mono_pcm24(changed, self.right_samples[:-1])
        wrong_rate = self.root / "wrong-rate-input.wav"
        with wave.open(str(wrong_rate), "wb") as output:
            output.setparams((1, 3, 44100, 0, "NONE", "not compressed"))
            output.writeframes(self.right_pcm)
        wrong_rate_sidecar = self.root / "wrong-rate-input.json"
        self._write_sidecar(wrong_rate_sidecar, wrong_rate, self.right_pcm, self.right_scenario, 202)
        stereo_input = self.root / "stereo-input.wav"
        with wave.open(str(stereo_input), "wb") as output:
            output.setparams((2, 3, 48000, 0, "NONE", "not compressed"))
            output.writeframes(self.right_pcm)
        stereo_sidecar = self.root / "stereo-input.json"
        self._write_sidecar(stereo_sidecar, stereo_input, self.right_pcm, self.right_scenario, 202)
        pcm16_input = self.root / "pcm16-input.wav"
        with wave.open(str(pcm16_input), "wb") as output:
            output.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            output.writeframes(b"\x00\x00" * len(self.right_samples))
        pcm16_sidecar = self.root / "pcm16-input.json"
        self._write_sidecar(pcm16_sidecar, pcm16_input, self.right_pcm, self.right_scenario, 202)
        cases = [
            ("unequal frames", {"right_wav": changed}),
            ("wrong WAV sample rate", {"right_wav": wrong_rate, "right_sidecar": wrong_rate_sidecar}),
            ("stereo input", {"right_wav": stereo_input, "right_sidecar": stereo_sidecar}),
            ("PCM16 input", {"right_wav": pcm16_input, "right_sidecar": pcm16_sidecar}),
            ("wrong output encoding", {"left_sidecar": self._mutated_sidecar(
                "format", lambda d: d.update(output_encoding="pcm_s16le"))}),
            ("frame count mismatch", {"left_sidecar": self._mutated_sidecar("frames", lambda d: d.update(frames_out=999))}),
            ("output digest mismatch", {"left_sidecar": self._mutated_sidecar(
                "digest", lambda d: d.update(output_digest="0000000000000000"))}),
            ("source digest mismatch", {"left_sidecar": self._mutated_sidecar(
                "source-digest", lambda d: d.update(source_digest="xyz"))}),
            ("wrong renderer provenance", {"left_sidecar": self._mutated_sidecar(
                "generator", lambda d: d.update(generator="fake-renderer/1"))}),
        ]
        for name, overrides in cases:
            with self.subTest(name=name):
                out_wav = self.root / f"bad-{name.replace(' ', '-')}.wav"
                out_sidecar = self.root / f"bad-{name.replace(' ', '-')}.json"
                with self.assertRaises(stereo.ComposeError):
                    self.compose(**overrides, output_wav=out_wav, output_sidecar=out_sidecar)
                self.assertFalse(out_wav.exists())
                self.assertFalse(out_sidecar.exists())

        for label, raw in (
            ("duplicate-key", self.left_sidecar.read_text(encoding="utf-8").replace('"seed": 101', '"seed": 101, "seed": 101')),
            ("nested-duplicate-key", self.left_sidecar.read_text(encoding="utf-8").replace(
                '"stages": []', '"stages": [{"nested": {"value": 1, "value": 2}}]')),
            ("nan", self.left_sidecar.read_text(encoding="utf-8").replace('"seed": 101', '"seed": NaN')),
            ("positive-infinity", self.left_sidecar.read_text(encoding="utf-8").replace('"seed": 101', '"seed": Infinity')),
            ("negative-infinity", self.left_sidecar.read_text(encoding="utf-8").replace('"seed": 101', '"seed": -Infinity')),
            ("overflow-float", self.left_sidecar.read_text(encoding="utf-8").replace(
                '"stages": []', '"stages": [{"value": 1e9999}]')),
        ):
            invalid = self.root / f"invalid-json-{label}.json"
            invalid.write_text(raw, encoding="utf-8")
            out_wav = self.root / f"invalid-{label}.wav"
            out_sidecar = self.root / f"invalid-{label}.out.json"
            with self.subTest(sidecar=label), self.assertRaises(stereo.ComposeError):
                self.compose(left_sidecar=invalid, output_wav=out_wav, output_sidecar=out_sidecar)
            self.assertFalse(out_wav.exists())
            self.assertFalse(out_sidecar.exists())

        with self.assertRaises(stereo.ComposeError):
            self.compose(left_scenario=self.root / "missing-scenario.json")
        self.assertFalse(self.output_wav.exists())
        self.assertFalse(self.output_sidecar.exists())

    def _mutated_sidecar(self, label: str, mutate, *, base: Path | None = None) -> Path:
        path = self.root / f"mutated-{label}.json"
        document = json.loads((base or self.left_sidecar).read_text(encoding="utf-8"))
        mutate(document)
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_existing_outputs_and_path_aliases_are_refused_without_modification(self):
        self.output_wav.write_bytes(b"keep wav")
        with self.assertRaises(stereo.ComposeError):
            self.compose()
        self.assertEqual(self.output_wav.read_bytes(), b"keep wav")
        self.output_wav.unlink()
        self.output_sidecar.write_bytes(b"keep metadata")
        with self.assertRaises(stereo.ComposeError):
            self.compose()
        self.assertEqual(self.output_sidecar.read_bytes(), b"keep metadata")
        self.output_sidecar.unlink()

        aliases = (self.left_wav, self.left_sidecar, self.left_scenario,
                   self.right_wav, self.right_sidecar, self.right_scenario)
        for alias in aliases:
            before = alias.read_bytes()
            with self.subTest(alias=alias.name), self.assertRaises(stereo.ComposeError):
                self.compose(output_wav=alias)
            self.assertEqual(alias.read_bytes(), before)
        for alias in aliases:
            before = alias.read_bytes()
            with self.subTest(output_sidecar_alias=alias.name), self.assertRaises(stereo.ComposeError):
                self.compose(output_sidecar=alias)
            self.assertEqual(alias.read_bytes(), before)

    def test_sidecar_write_failure_removes_only_the_new_stereo_wav(self):
        out_wav = self.root / "cleanup.wav"
        out_json = self.root / "cleanup.json"
        original_left = self.left_wav.read_bytes()
        original_right = self.right_wav.read_bytes()
        with patch.object(stereo, "_write_sidecar_exclusive", side_effect=OSError("injected metadata write failure")):
            with self.assertRaises((stereo.ComposeError, OSError)):
                self.compose(output_wav=out_wav, output_sidecar=out_json)
        self.assertFalse(out_wav.exists())
        self.assertFalse(out_json.exists())
        self.assertEqual(self.left_wav.read_bytes(), original_left)
        self.assertEqual(self.right_wav.read_bytes(), original_right)
        self.assertTrue(self.left_sidecar.exists())
        self.assertTrue(self.right_sidecar.exists())

    def test_capture_verifier_accepts_exact_pcm24_and_wire_pcm16_then_rejects_shift_or_length(self):
        self.compose()
        expected_geometry, expected = read_wave(self.output_wav)
        capture24 = self.root / "capture24.wav"
        with wave.open(str(capture24), "wb") as output:
            output.setparams((2, 3, 48000, 0, "NONE", "not compressed"))
            output.writeframes(expected)
        result24 = stereo.verify(self.output_wav, capture24)
        self.assertTrue(result24["ok"])
        self.assertEqual(result24["frames"], expected_geometry[3])
        self.assertEqual(result24["capture_encoding"], "pcm_s24le")
        self.assertEqual(result24["crc32_format"], "zlib-crc32-packed-pcm24")
        self.assertEqual(result24["capture_crc32_format"], "zlib-crc32-packed-pcm24")
        self.assertEqual(result24["capture_native_crc32_left"], "9cf02c9d")
        self.assertEqual(result24["capture_native_crc32_right"], "a8bbe59d")

        wire16 = b"".join(expected[index + 1:index + 3] for index in range(0, len(expected), 3))
        capture16 = self.root / "capture16.wav"
        with wave.open(str(capture16), "wb") as output:
            output.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
            output.writeframes(wire16)
        result16 = stereo.verify(self.output_wav, capture16)
        self.assertTrue(result16["ok"])
        self.assertEqual(result16["capture_encoding"], "pcm_s16le_top16")
        self.assertEqual(result16["crc32_left"], "9cf02c9d")
        self.assertEqual(result16["crc32_right"], "a8bbe59d")
        self.assertEqual(result16["crc32_format"], "zlib-crc32-packed-pcm24")
        self.assertEqual(result16["capture_crc32_format"], "zlib-crc32-packed-pcm16")
        self.assertEqual(result16["capture_native_crc32_left"], "e35ba104")
        self.assertEqual(result16["capture_native_crc32_right"], "0fb47247")
        self.assertNotEqual(result16["capture_native_crc32_left"], result16["crc32_left"])

        shifted = self.root / "shifted.wav"
        with wave.open(str(shifted), "wb") as output:
            output.setparams((2, 3, 48000, 0, "NONE", "not compressed"))
            output.writeframes(expected[6:] + expected[:6])
        with self.assertRaises(stereo.ComposeError):
            stereo.verify(self.output_wav, shifted)

        short = self.root / "short-capture.wav"
        with wave.open(str(short), "wb") as output:
            output.setparams((2, 3, 48000, 0, "NONE", "not compressed"))
            output.writeframes(expected[:-6])
        with self.assertRaises(stereo.ComposeError):
            stereo.verify(self.output_wav, short)

        wrong_rate = self.root / "wrong-rate.wav"
        with wave.open(str(wrong_rate), "wb") as output:
            output.setparams((2, 3, 44100, 0, "NONE", "not compressed"))
            output.writeframes(expected)
        with self.assertRaises(stereo.ComposeError):
            stereo.verify(self.output_wav, wrong_rate)
        self.assertEqual(expected_geometry, (2, 3, 48000, len(self.left_samples)))

    def test_real_cpp_renderer_outputs_compose_and_verify(self):
        if RENDERER is None:
            self.skipTest("pass --renderer to exercise real C++ render sidecars")
        source_left = self.root / "real-left.wav"
        source_right = self.root / "real-right.wav"
        left_samples = [0, 1000, -1000, 0, 500, -500, 0, 1, -1]
        right_samples = [0, -777, 777, 0, -333, 333, 0, -1, 1]
        write_mono_pcm24(source_left, left_samples)
        write_mono_pcm24(source_right, right_samples)
        scenario_left = self.root / "real-left.json"
        scenario_right = self.root / "real-right.json"
        rms_left = math.sqrt(sum(sample * sample for sample in left_samples) / len(left_samples)) / (1 << 23)
        rms_right = math.sqrt(sum(sample * sample for sample in right_samples) / len(right_samples)) / (1 << 23)
        scenario_left.write_text(json.dumps({"seed": 11, "source_gain_db": 0, "reference_rms": rms_left,
                                            "impairments": []}), encoding="utf-8")
        scenario_right.write_text(json.dumps({"seed": 29, "source_gain_db": 0, "reference_rms": rms_right,
                                              "impairments": []}), encoding="utf-8")
        rendered = []
        for side, source, scenario in (("left", source_left, scenario_left), ("right", source_right, scenario_right)):
            output = self.root / f"real-{side}-render.wav"
            sidecar = self.root / f"real-{side}-render.json"
            command = [str(RENDERER), "--scenario", str(scenario), "--source", str(source),
                       "--output", str(output), "--sidecar", str(sidecar), "--block", "3"]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            rendered.append((output, sidecar))
            self.assertEqual(read_wave(output)[0][3], len(left_samples))
            self.assertEqual(output.stat().st_size, 44 + len(left_samples) * 3 + 1)
        stereo.compose(rendered[0][0], rendered[0][1], scenario_left,
                       rendered[1][0], rendered[1][1], scenario_right,
                       self.output_wav, self.output_sidecar)
        _, actual = read_wave(self.output_wav)
        _, left = read_wave(rendered[0][0])
        _, right = read_wave(rendered[1][0])
        self.assertEqual(actual, b"".join(left[i:i + 3] + right[i:i + 3] for i in range(0, len(left), 3)))
        capture = self.root / "real-capture.wav"
        shutil.copyfile(self.output_wav, capture)
        verify = stereo.verify(self.output_wav, capture)
        self.assertTrue(verify["ok"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path)
    arguments, unittest_arguments = parser.parse_known_args()
    if arguments.renderer is not None:
        RENDERER = arguments.renderer.resolve(strict=True)
    result = unittest.main(argv=[__file__, *unittest_arguments], exit=False)
    raise SystemExit(not result.result.wasSuccessful())
