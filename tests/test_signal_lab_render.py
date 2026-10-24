#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Integration checks for the portable renderer's artifacts and target scenarios."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))

from signal_lab.awgn import SNR_BAND_HZ, SNR_REFERENCE_BANDWIDTH_HZ, noise_band_enbw_hz  # noqa: E402
from signal_lab.scenario import build_pipeline  # noqa: E402
from failing_output import failing_output  # noqa: E402

RENDERER: Path
WATTERSON_RENDERER: Path


class RendererTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "clean source.wav"
        self.pcm = [12000, -10000, 6000, -4000] * 1024
        with wave.open(str(self.source), "wb") as writer:
            writer.setparams((1, 3, 48000, 0, "NONE", "not compressed"))
            writer.writeframes(b"".join(
                (sample * 256).to_bytes(3, "little", signed=True) for sample in self.pcm))

    def read_pcm24_as_legacy(self, path):
        with wave.open(str(path), "rb") as reader:
            self.assertEqual(reader.getsampwidth(), 3)
            packed = reader.readframes(reader.getnframes())
        return [int.from_bytes(packed[index:index + 3], "little", signed=True) // 256
                for index in range(0, len(packed), 3)]

    def render(self, document, *, name="render", extra=(), expected_exit=0):
        scenario = self.root / f"{name} scenario.json"
        if isinstance(document, str):
            scenario.write_text(document, encoding="utf-8")
        else:
            scenario.write_text(json.dumps(document), encoding="utf-8")
        output = self.root / f"{name} output.wav"
        sidecar = self.root / f"{name} output.json"
        target = self.root / f"{name} target.scn"
        result = subprocess.run(
            [str(RENDERER), "--scenario", str(scenario), "--source", str(self.source),
             "--output", str(output), "--sidecar", str(sidecar),
             "--target-scenario", str(target), *extra],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
        return output, sidecar, target, scenario, result

    def test_sidecar_preserves_json_strings_and_native_paths(self):
        test_id = 'review "quoted" \\path\n\t\b\f\r \u2603'
        output, sidecar, _, scenario, _ = self.render({"test_id": test_id, "impairments": []})
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        self.assertEqual(metadata["test_id"], test_id)
        self.assertEqual(metadata["scenario_path"], str(scenario))
        self.assertEqual(metadata["source_path"], str(self.source))
        self.assertEqual(metadata["output_path"], str(output))

    def test_watterson_matches_validation_driver_at_every_partition(self):
        cases = [
            [{"fading": "fixed", "delay_ms": 2}],
            [{"spread_hz": 1}, {"delay_ms": 2, "spread_hz": 1}],
            [{"spread_hz": 0.1, "shift_hz": -75}, {"spread_hz": 10, "delay_ms": 20},
             {"fading": "fixed", "power_db": -6}, {"spread_hz": 2, "shift_hz": 75}],
        ]
        for index, paths in enumerate(cases):
            document = {"seed": 12345, "source_gain_db": -24,
                        "reference_rms": 0.25, "clip_policy": "reject",
                        "impairments": [{"type": "watterson", "paths": paths},
                                        {"type": "awgn", "snr_db": 24}]}
            scenario = self.root / "oracle.json"
            scenario.write_text(json.dumps(document))
            oracle_wav, oracle_json = self.root / "oracle.wav", self.root / "oracle-sidecar.json"
            result = subprocess.run([str(WATTERSON_RENDERER), "--scenario", str(scenario),
                                     "--source", str(self.source), "--output", str(oracle_wav),
                                     "--sidecar", str(oracle_json), "--block", "2048"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            oracle = json.loads(oracle_json.read_text())
            for block in (1, 127, 2048):
                with self.subTest(paths=index, block=block):
                    output, sidecar, _, _, _ = self.render(document, name=f"wat-{index}-{block}",
                                                          extra=("--block", str(block)))
                    metadata = json.loads(sidecar.read_text())
                    self.assertEqual(output.read_bytes(), oracle_wav.read_bytes())
                    for key in ("output_digest", "source_digest", "frames_in", "frames_out"):
                        self.assertEqual(metadata[key], oracle[key])
                    channel = metadata["watterson"]
                    self.assertLessEqual(channel.pop("channel_bytes"), 20 * 1024)
                    self.assertEqual(channel, oracle["watterson"])

    def test_frequency_shift_matches_driver_and_preserves_scenario_identity(self):
        for mode in ({"mode": "offset", "offset_hz": 75},
                     {"mode": "offset", "offset_hz": -75}, {"mode": "sweep"}):
            document = {"source_gain_db": -24, "reference_rms": 0.25,
                        "impairments": [{"type": "frequency_shift", **mode}]}
            source_scenario = self.root / "shift.json"
            source_scenario.write_text(json.dumps(document))
            oracle = self.root / "shift-oracle.wav"
            result = subprocess.run([str(WATTERSON_RENDERER), "--scenario", str(source_scenario),
                                     "--source", str(self.source), "--output", str(oracle)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for block in (1, 127, 2048):
                with self.subTest(mode=mode, block=block):
                    output, sidecar, target, _, _ = self.render(document, extra=("--block", str(block)))
                    self.assertEqual(output.read_bytes(), oracle.read_bytes())
                    metadata = json.loads(sidecar.read_text())["frequency_shift"]
                    self.assertEqual(metadata["mode"], mode["mode"])
                    self.assertEqual(metadata["algorithm_version"], 1)
                    self.assertEqual(metadata["latency_frames"], 215)
                    self.assertEqual(json.loads(target.read_text())["impairments"], document["impairments"])

    def test_streaming_reference_keeps_interior_silence_and_explicit_window(self):
        # Nonzero samples straddle three I/O blocks; interior zeroes count in RMS.
        samples = [0] * 2201 + [8192] + [0] * 4097 + [-4096] + [0] * 80
        for width in (2, 3):
            with wave.open(str(self.source), "wb") as writer:
                writer.setparams((1, width, 48000, 0, "NONE", "not compressed"))
                writer.writeframes(b"".join((s * (256 if width == 3 else 1)).to_bytes(width, "little", signed=True) for s in samples))
            for reference, divisor in (("auto", 4099), ({"start_seconds": 2201 / 48000, "duration_seconds": 4100 / 48000}, 4100)):
                with self.subTest(width=width, reference=reference):
                    _, sidecar, _, _, _ = self.render({"source_gain_db": 0, "reference": reference, "impairments": []})
                    metadata = json.loads(sidecar.read_text())
                    self.assertAlmostEqual(metadata["reference_rms"], math.sqrt((0.25 ** 2 + 0.125 ** 2) / divisor), places=15)

    def test_odd_streamed_pcm24_is_padded_and_can_be_rendered_again(self):
        output, _, _ = self.live_render(self.live_plan(live_frames=1777), block=127)
        data = output.read_bytes()
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], len(data) - 8)
        self.assertEqual(struct.unpack_from("<I", data, 40)[0], 1777 * 3)
        self.assertEqual(data[-1], 0)
        self.source = output
        second, _, _, _, _ = self.render({"source_gain_db": 0, "impairments": []})
        self.assertEqual(second.read_bytes(), data)

    def test_streamed_render_can_replace_its_source_after_reading(self):
        expected = self.source.read_bytes()
        scenario = self.root / "unity.json"
        scenario.write_text(json.dumps({"source_gain_db": 0, "impairments": []}))
        result = self.run_renderer("--scenario", str(scenario), "--source", str(self.source), "--output", str(self.source))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.source.read_bytes(), expected)

    def test_late_clipping_discards_spool_and_preserves_existing_artifacts(self):
        samples = [100] * 5000 + [16000]
        with wave.open(str(self.source), "wb") as writer:
            writer.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            writer.writeframes(struct.pack(f"<{len(samples)}h", *samples))
        existing = self.root / "late output.wav"
        existing.write_bytes(b"keep the accepted artifact")
        _, sidecar, target, _, result = self.render({"source_gain_db": 20, "clip_policy": "reject", "impairments": []},
                                                   name="late", expected_exit=2)
        self.assertIn("clipping at output frame 5000", result.stderr)
        self.assertEqual(existing.read_bytes(), b"keep the accepted artifact")
        self.assertFalse(sidecar.exists())
        self.assertFalse(target.exists())

    def test_five_hour_sparse_source_reads_reference_beyond_two_gib(self):
        # A logical 5 h WAV, backed by only a few filesystem blocks. Read its
        # reference near EOF (> signed 32-bit byte offset), then a short replay.
        # This checks long-file seeking/bounded input without a 5 h DSP run.
        frames = 5 * 3600 * 48000
        data_bytes = frames * 3
        with self.source.open("w+b") as source:
            if os.name == "nt":
                import ctypes
                from ctypes import wintypes
                import msvcrt
                ioctl = ctypes.WinDLL("kernel32", use_last_error=True).DeviceIoControl
                ioctl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
                ioctl.restype = wintypes.BOOL
                returned = wintypes.DWORD()
                if not ioctl(msvcrt.get_osfhandle(source.fileno()), 0x900C4, None, 0, None, 0, ctypes.byref(returned), None):
                    self.skipTest("temporary filesystem does not support FSCTL_SET_SPARSE")
            source.write(struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + data_bytes, b"WAVE", b"fmt ", 16,
                                     1, 1, 48000, 144000, 3, 24, b"data", data_bytes))
            source.truncate(44 + data_bytes)
            source.seek(44 + data_bytes - 6)
            source.write((2 ** 21).to_bytes(3, "little", signed=True) + (-2 ** 20).to_bytes(3, "little", signed=True))
        _, sidecar, _ = self.live_render(self.live_plan(live_frames=2), block=1,
            scenario={"source_gain_db": 0, "reference": {"start_seconds": (frames - 48) / 48000,
                                                        "duration_seconds": 0.001}, "impairments": []})
        metadata = json.loads(sidecar.read_text())
        self.assertEqual(metadata["frames_in"], 2)
        self.assertEqual(metadata["frames_out"], 2)
        self.assertAlmostEqual(metadata["reference_rms"], math.sqrt((0.25 ** 2 + 0.125 ** 2) / 48), places=15)

    def test_override_updates_existing_reference_and_replays_exactly(self):
        override = "0.38034074368389242"
        document = {
            "reference_rms": 0.125,
            "impairments": [{"type": "cw", "frequency_hz": 1800, "ci_db": 6}],
        }
        first, first_metadata, target, _, _ = self.render(
            document, name="override", extra=("--reference-rms", override))
        exported = json.loads(target.read_text(encoding="utf-8"))
        self.assertEqual(exported["reference_rms"], float(override))
        replay, replay_metadata, _, _, _ = self.render(exported, name="replay")
        self.assertEqual(first.read_bytes(), replay.read_bytes())
        self.assertEqual(json.loads(first_metadata.read_text())["output_digest"],
                         json.loads(replay_metadata.read_text())["output_digest"])

    def test_target_edits_only_the_root_reference_member(self):
        documents = [
            {"metadata": {"reference_rms": 0.75}, "note": "reference_rms", "impairments": []},
            '{"ref\\u0065rence_rms": 0.75, "metadata": {"reference_rms": 0.5}, "impairments": []}',
            {"\reference_rms": 0.75, "impairments": []},
            {},
        ]
        for index, document in enumerate(documents):
            with self.subTest(document=document):
                _, _, target, _, _ = self.render(
                    document, name=f"root{index}", extra=("--reference-rms", "0.125"))
                pairs = json.loads(target.read_text(), object_pairs_hook=lambda values: values)
                self.assertEqual(sum(key == "reference_rms" for key, _ in pairs), 1)
                actual = json.loads(target.read_text())
                expected = json.loads(document) if isinstance(document, str) else dict(document)
                expected["reference_rms"] = 0.125
                self.assertEqual(actual, expected)

    def test_a_name_with_a_wildcard_is_taken_as_written(self):
        # The MinGW C runtime would expand '*' to the files it matches before main()
        # runs: "out-4*.wav" as the output would name out-48k.wav and overwrite it,
        # and "src-*.wav" as the source would read src-1.wav.
        existing = self.root / "out-48k.wav"
        existing.write_bytes(b"existing output")
        scenario = self.root / "wildcard scenario.json"
        scenario.write_text(json.dumps({"impairments": []}), encoding="utf-8")
        wildcard = self.root / "out-4*.wav"
        result = subprocess.run(
            [str(RENDERER), "--scenario", str(scenario), "--source", str(self.source), "--output", str(wildcard)],
            capture_output=True, text=True, check=False)
        self.assertEqual(existing.read_bytes(), b"existing output", "the file the name would match keeps every byte")
        if os.name == "nt":
            # '*' cannot be in a Windows file name, so the literal name cannot be written.
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("cannot write " + str(wildcard), result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(wildcard.exists())
        (self.root / "src-1.wav").write_bytes(self.source.read_bytes())
        output = self.root / "from wildcard.wav"
        result = subprocess.run(
            [str(RENDERER), "--scenario", str(scenario), "--source", str(self.root / "src-*.wav"), "--output", str(output)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("src-*.wav", result.stderr)
        self.assertFalse(output.exists(), "nothing is written")

    def test_rejected_clipping_preserves_existing_artifacts(self):
        expected = {}
        for suffix in ("output.wav", "output.json", "target.scn"):
            path = self.root / f"reject {suffix}"
            expected[path] = f"existing {suffix}".encode()
            path.write_bytes(expected[path])
        *_, result = self.render(
            {"source_gain_db": 20, "clip_policy": "reject", "impairments": []},
            name="reject", expected_exit=2)
        self.assertIn("clipping", result.stderr)
        for path, contents in expected.items():
            self.assertEqual(path.read_bytes(), contents)

    def test_saturation_metrics_describe_emitted_pcm(self):
        output, sidecar, _, _, _ = self.render(
            {"source_gain_db": 20, "clip_policy": "saturate", "impairments": []})
        with wave.open(str(output), "rb") as reader:
            self.assertEqual(reader.getsampwidth(), 3)
            packed = reader.readframes(reader.getnframes())
            pcm = [int.from_bytes(packed[index:index + 3], "little", signed=True)
                   for index in range(0, len(packed), 3)]
        metadata = json.loads(sidecar.read_text())
        peak = max(abs(sample) for sample in pcm) / 8388608.0
        rms = math.sqrt(sum((sample / 8388608.0) ** 2 for sample in pcm) / len(pcm))
        self.assertGreater(metadata["clipped_samples"], 0)
        self.assertAlmostEqual(metadata["peak_dbfs"], 20 * math.log10(peak), delta=1e-6)
        self.assertAlmostEqual(metadata["rms_dbfs"], 20 * math.log10(rms), delta=1e-6)

    def test_metadata_write_failure_is_not_success(self):
        bad_destination = self.root / "missing directory" / "metadata.json"
        *_, result = self.render({"impairments": []},
                                extra=("--sidecar", str(bad_destination)), expected_exit=2)
        self.assertIn("cannot write", result.stderr)

    def run_renderer(self, *arguments):
        return subprocess.run([str(RENDERER), *arguments], capture_output=True, text=True, check=False)

    def test_a_write_that_fails_after_the_open_is_not_success(self):
        # QA on 1495bec: write_wav checks the stream after its write and close; the missing directories above
        # are refused at the open. Here the open succeeds and the write fails (tests/failing_output.py).
        scenario = self.root / "plain.json"
        scenario.write_text(json.dumps({"impairments": []}), encoding="utf-8")
        with failing_output(self.root) as output:
            if output is None:
                self.skipTest("no output here that fails after its open")
            result = self.run_renderer("--scenario", str(scenario), "--source", str(self.source), "--output", str(output))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(result.stderr, f"cannot write {output}\n")

    def test_file_and_argument_refusals_are_reported_by_text(self):
        # Each refusal: exit status 2, its one line on stderr, nothing written.
        scenario = self.root / "plain.json"
        scenario.write_text(json.dumps({"impairments": []}), encoding="utf-8")
        output = self.root / "refused.wav"
        missing = self.root / "missing.json"
        missing_source = self.root / "missing.wav"
        unwritable = self.root / "missing directory" / "out.wav"
        usage = ("usage: signal_lab_render --scenario case.json --source clean.wav --output impaired.wav"
                 " [--sidecar out.json] [--reference-rms R] [--target-scenario PLAY.SCN] [--block N] [--live-events replay.json] [--seed N]")
        base = ("--scenario", str(scenario), "--source", str(self.source), "--output", str(output))
        cases = [
            (("--scenario", str(missing), "--source", str(self.source), "--output", str(output)), [f"cannot read scenario {missing}"]),
            (("--scenario", str(scenario), "--source", str(missing_source), "--output", str(output)), [f"cannot read source {missing_source}"]),
            (("--scenario", str(scenario), "--source", str(self.source), "--output", str(unwritable)), [f"cannot write {unwritable}"]),
            ((*base, "--block", "0"), ["--block must be 1..2048"]),
            ((*base, "--block", "2049"), ["--block must be 1..2048"]),
            ((*base, "--bogus", "1"), ["unknown or incomplete argument: --bogus"]),
            ((*base, "--sidecar"), ["unknown or incomplete argument: --sidecar"]),
            (("--scenario", str(scenario), "--source", str(self.source)), [usage, "With --live-events, omit --scenario for unity-gain clean-source replay."]),
        ]
        for arguments, lines in cases:
            with self.subTest(arguments=arguments[-2:]):
                result = self.run_renderer(*arguments)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(result.stderr.splitlines(), lines)
                self.assertEqual(result.stdout, "")
                self.assertFalse(output.exists())
        # And a render writes a WAV whose RIFF and data sizes match its frames.
        result = self.run_renderer(*base)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = output.read_bytes()
        self.assertEqual(len(data), 44 + 3 * len(self.pcm))
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], len(data) - 8)
        self.assertEqual(struct.unpack_from("<I", data, 40)[0], 3 * len(self.pcm))

    def test_reference_override_requires_a_finite_positive_number(self):
        for index, value in enumerate(("nan", "inf", "-inf", "0", "-1", "0.25junk", "")):
            with self.subTest(value=value):
                output, sidecar, target, _, result = self.render(
                    {"impairments": []}, name=f"invalid{index}",
                    extra=("--reference-rms", value), expected_exit=2)
                self.assertIn("finite positive number", result.stderr)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())
                self.assertFalse(target.exists())

    def seeded_render(self, document, name, *extra):
        scenario = self.root / f"{name} scenario.json"
        scenario.write_text(json.dumps(document), encoding="utf-8")
        output = self.root / f"{name} output.wav"
        sidecar = self.root / f"{name} output.json"
        result = subprocess.run(
            [str(RENDERER), "--scenario", str(scenario), "--source", str(self.source),
             "--output", str(output), "--sidecar", str(sidecar), *extra],
            capture_output=True, text=True, check=False,
        )
        return output, sidecar, result

    def test_seed_replaces_the_documents_seed(self):
        # As a PLAYLIST item's seed= on the board: the render is the document's
        # with that seed, from the unchanged document.
        noise = {"seed": 42, "reference_rms": 0.125, "impairments": [{"type": "awgn", "snr_db": 20}]}
        own, _, result = self.seeded_render(noise, "own")
        self.assertEqual(result.returncode, 0, result.stderr)
        seeded, seeded_metadata, result = self.seeded_render(noise, "seeded", "--seed", "7")
        self.assertEqual(result.returncode, 0, result.stderr)
        as_document, _, result = self.seeded_render(dict(noise, seed=7), "document")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(seeded.read_bytes(), as_document.read_bytes())
        self.assertNotEqual(seeded.read_bytes(), own.read_bytes())
        self.assertEqual(json.loads(seeded_metadata.read_text())["seed"], 7)
        _, _, result = self.seeded_render(noise, "widest", "--seed", "18446744073709551615")
        self.assertEqual(result.returncode, 0, result.stderr)
        for index, value in enumerate(("07", "-1", "+7", "18446744073709551616", "7x", "")):
            with self.subTest(value=value):
                output, _, result = self.seeded_render(noise, f"badseed{index}", "--seed", value)
                self.assertEqual(result.returncode, 2)
                self.assertIn("canonical decimal", result.stderr)
                self.assertFalse(output.exists())
        output, _, result = self.seeded_render(noise, "target", "--seed", "7", "--target-scenario", str(self.root / "target.scn"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("--seed needs --scenario and no --target-scenario", result.stderr)
        self.assertFalse(output.exists())

    def live_render(self, plan, *, name="live", block=2048, scenario=None, expected_exit=0):
        events = self.root / f"{name} replay.json"
        events.write_text(plan if isinstance(plan, str) else json.dumps(plan), encoding="utf-8")
        output = self.root / f"{name} live.wav"
        sidecar = self.root / f"{name} live.json"
        arguments = [str(RENDERER), "--live-events", str(events), "--source", str(self.source),
                     "--output", str(output), "--sidecar", str(sidecar), "--block", str(block)]
        if scenario is not None:
            source_scenario = self.root / f"{name} source.json"
            source_scenario.write_text(json.dumps(scenario), encoding="utf-8")
            arguments.extend(("--scenario", str(source_scenario)))
        result = subprocess.run(arguments, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
        return output, sidecar, result

    @staticmethod
    def live_plan(controls=(), **overrides):
        plan = {"schema": "wfg-live.replay/1", "protocol": "WFG-LIVE/1",
                "live_seed": 18446744073709551615, "live_reference_rms": 0.125,
                "controls": list(controls)}
        plan.update(overrides)
        return plan

    def test_live_disabled_replay_preserves_source_and_uint64_seed(self):
        output, sidecar, _ = self.live_render(self.live_plan())
        self.assertEqual(output.read_bytes(), self.source.read_bytes())
        metadata = json.loads(sidecar.read_text())
        self.assertEqual(metadata["live_replay"]["seed"], 18446744073709551615)
        self.assertEqual(metadata["live_replay"]["reference_rms"], 0.125)
        self.assertEqual(metadata["output_digest"], metadata["source_digest"])

    def test_live_sweeps_and_static_replay_are_block_independent(self):
        plan = self.live_plan([
            {"command": "CW ON", "apply_frame": 101, "events": 1},
            {"command": "SWEEP CW FREQ 300 3400 8 7", "apply_frame": 101, "events": 8},
            {"command": "SWEEP CW CI 20 3 4 17", "apply_frame": 101, "events": 4},
            {"command": "STATIC RATE 100", "apply_frame": 257, "events": 1},
            {"command": "STATIC PEAK 6", "apply_frame": 257, "events": 1},
            {"command": "STATIC ON", "apply_frame": 257, "events": 1},
            {"command": "SWEEP FADE 6 30 5 11 8", "apply_frame": 503, "events": 5},
            {"command": "CW OFF", "apply_frame": 3500, "events": 1},
            {"command": "STATIC OFF", "apply_frame": 3500, "events": 1},
        ])
        first, first_sidecar, _ = self.live_render(plan, name="mix2048")
        second, second_sidecar, _ = self.live_render(plan, name="mix100", block=100)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        a = json.loads(first_sidecar.read_text())
        b = json.loads(second_sidecar.read_text())
        self.assertEqual(a["output_digest"], b["output_digest"])
        self.assertEqual(a["live_replay"]["controls_applied"], 23)
        self.assertGreater(a["live_replay"]["static_events_started"], 0)
        pcm = self.read_pcm24_as_legacy(first)
        self.assertEqual(list(pcm[:101]), self.pcm[:101])
        self.assertEqual(list(pcm[3500:]), self.pcm[3500:])

    def test_live_replay_streams_all_128_captured_controls(self):
        plan = self.live_plan([
            {"command": f"CW CI {index % 20}", "apply_frame": index * 20, "events": 1}
            for index in range(128)
        ])
        output, sidecar, _ = self.live_render(plan, block=100)
        self.assertEqual(output.read_bytes(), self.source.read_bytes())
        metadata = json.loads(sidecar.read_text())["live_replay"]
        self.assertEqual(metadata["controls_accepted"], 128)
        self.assertEqual(metadata["controls_applied"], 128)
        self.assertEqual(metadata["pending_controls"], 0)

    def test_live_replay_uses_output_timeline_after_source_slips(self):
        plan = self.live_plan([
            {"command": "CW FREQ 12000", "apply_frame": 200, "events": 1},
            {"command": "CW ON", "apply_frame": 200, "events": 1},
        ], live_frames=1777)
        scenario = {"source_gain_db": 0, "impairments": [{"type": "sample_slip", "kind": "delete",
                                                         "at_seconds": 0, "length_samples": 100}]}
        output, sidecar, _ = self.live_render(plan, scenario=scenario)
        pcm = self.read_pcm24_as_legacy(output)
        self.assertEqual(len(pcm), 1777)
        self.assertEqual(list(pcm[:200]), self.pcm[100:300])
        self.assertNotEqual(list(pcm[201:220]), self.pcm[301:320])
        self.assertEqual(json.loads(sidecar.read_text())["frames_out"], 1777)

    def test_live_replay_rejects_invalid_plans_before_writing(self):
        invalid = [
            self.live_plan(protocol="WFG/1"),
            self.live_plan(live_seed=18446744073709551616),
            self.live_plan(live_reference_rms=0),
            self.live_plan([{"command": "CW ON", "apply_frame": 0, "events": 2}]),
            self.live_plan([{"command": "AT:12 CW ON", "apply_frame": 0, "events": 1}]),
            self.live_plan([{"command": "PLAY", "apply_frame": 0, "events": 1}]),
            self.live_plan([{"command": "CW ON", "apply_frame": -1, "events": 1}]),
            self.live_plan(live_frames=len(self.pcm) + 1),
            self.live_plan([{"command": "CW CI 20", "apply_frame": 0, "events": 1}] * 65),
            '{"schema":"wfg-live.replay/1","schema":"wfg-live.replay/1"}',
        ]
        for index, plan in enumerate(invalid):
            with self.subTest(plan=plan):
                output, sidecar, _ = self.live_render(plan, name=f"badlive{index}", expected_exit=2)
                self.assertFalse(output.exists())
                self.assertFalse(sidecar.exists())


# The live-replay reader (host/render/live_replay.cpp) on plans written as
# text, so the JSON itself can be malformed: what each path of its JSON reader,
# its integers and its controls gives. A plan it refuses writes nothing.
REPLAY_DOCUMENT = "invalid wfg-live.replay/1 JSON document or protocol"
REPLAY_SEED = "live replay requires an exact uint64 live_seed"
REPLAY_REFERENCE = "live replay requires the effective positive live_reference_rms"
REPLAY_CONTROLS = "live replay controls must be an array of at most 128 records"
REPLAY_RECORD = "invalid live replay command, apply_frame, or event count"
REPLAY_UNSCHEDULED = "replay command must be an unscheduled live control; apply_frame supplies its recorded time"
REPLAY_EVENTS = "live replay control was invalid or its event count disagrees with the recorded acknowledgement"
REPLAY_PENDING = "live replay exceeds the 64-event pending capacity at one output frame"
REPLAY_GOOD = ('{"schema":"wfg-live.replay/1","protocol":"WFG-LIVE/1","live_seed":18446744073709551615,'
               '"live_reference_rms":0.125,"controls":[]}')
REPLAY_SOURCE_FRAMES = 4096


def replay_text(controls="[]", **members):
    """A plan as JSON text: the members of a good plan, each replaced (None drops it) or added as raw JSON."""
    plan = {"schema": '"wfg-live.replay/1"', "protocol": '"WFG-LIVE/1"', "live_seed": "18446744073709551615",
            "live_reference_rms": "0.125", "controls": controls}
    plan.update(members)
    return "{" + ",".join(f'"{key}":{value}' for key, value in plan.items() if value is not None) + "}"


def replay_control(command='"CW ON"', apply_frame="0", events="1", extra=""):
    return '{"command":' + command + ',"apply_frame":' + apply_frame + ',"events":' + events + extra + "}"


def replay_controls(*records):
    return "[" + ",".join(records) + "]"


# (case, plan text, the reader's refusal or None when it takes the plan)
REPLAY_CASES = (
    ("not an object", "[]", REPLAY_DOCUMENT),
    ("empty", "", REPLAY_DOCUMENT),
    ("text after the object", REPLAY_GOOD + " x", REPLAY_DOCUMENT),
    ("whitespace after the object", REPLAY_GOOD + " \r\n\t", None),
    ("an unterminated key", '{"schema', REPLAY_DOCUMENT),
    ("no colon", '{"schema" "wfg-live.replay/1"}', REPLAY_DOCUMENT),
    ("no comma", '{"schema":"wfg-live.replay/1" "protocol":"WFG-LIVE/1"}', REPLAY_DOCUMENT),
    ("a member twice", '{"schema":"wfg-live.replay/1","schema":"wfg-live.replay/1"}', REPLAY_DOCUMENT),
    ("a member of a good plan twice", REPLAY_GOOD[:-1] + ',"controls":[]}', REPLAY_DOCUMENT),
    ("an empty object", "{}", REPLAY_DOCUMENT),
    ("another schema", replay_text(schema='"wfg-live.replay/2"'), REPLAY_DOCUMENT),
    ("another protocol", replay_text(protocol='"WFG/1"'), REPLAY_DOCUMENT),
    ("a schema that is no string", replay_text(schema="1"), REPLAY_DOCUMENT),
    ("an escaped solidus as \\u002f", replay_text(schema='"wfg-live.replay\\u002f1"'), None),
    ("an escaped solidus as \\/", replay_text(schema='"wfg-live.replay\\/1"'), None),
    ("a non-ASCII escape in the schema", replay_text(schema='"wfg-live.replay\\u00e91"'), REPLAY_DOCUMENT),
    ("an unknown escape", replay_text(schema='"wfg-live.replay\\q1"'), REPLAY_DOCUMENT),
    ("a short \\u escape", replay_text(schema='"\\u00"'), REPLAY_DOCUMENT),
    ("a \\u escape that is no hex", replay_text(schema='"\\u00g0"'), REPLAY_DOCUMENT),
    ("a control character in a string", replay_text(meta='"a\tb"'), REPLAY_DOCUMENT),
    ("an unterminated string", '{"schema":"wfg-live.replay/1', REPLAY_DOCUMENT),
    ("metadata of every JSON kind", replay_text(meta='{"a":[1,-2.5e+3,true,false,null,{"b":"\\u00e9\\n"}],"c":{}}'), None),
    ("metadata nested 33 deep", replay_text(meta="[" * 33 + "]" * 33), None),
    ("metadata nested 34 deep", replay_text(meta="[" * 34 + "]" * 34), REPLAY_DOCUMENT),
    ("a number with a leading zero", replay_text(meta="01"), REPLAY_DOCUMENT),
    ("a bare minus", replay_text(meta="-"), REPLAY_DOCUMENT),
    ("a number without its fraction", replay_text(meta="1."), REPLAY_DOCUMENT),
    ("a number without its exponent", replay_text(meta="1e"), REPLAY_DOCUMENT),
    ("an exponent of a sign alone", replay_text(meta="1E-"), REPLAY_DOCUMENT),
    ("a literal cut short", replay_text(meta="nul"), REPLAY_DOCUMENT),
    ("an array left open", replay_text(meta="[1,2"), REPLAY_DOCUMENT),
    ("a nested member without its colon", replay_text(meta='{"a" 1}'), REPLAY_DOCUMENT),
    ("a key that is no string", replay_text(meta="{1:2}"), REPLAY_DOCUMENT),
    ("a trailing comma", replay_text(meta="[1,]"), REPLAY_DOCUMENT),
    ("no seed", replay_text(live_seed=None), REPLAY_SEED),
    ("a quoted seed", replay_text(live_seed='"18446744073709551615"'), None),
    ("a seed of 2^64", replay_text(live_seed="18446744073709551616"), REPLAY_SEED),
    ("a negative seed", replay_text(live_seed="-1"), REPLAY_SEED),
    ("a seed with a fraction", replay_text(live_seed="1.5"), REPLAY_SEED),
    ("a quoted seed that is no number", replay_text(live_seed='"x"'), REPLAY_SEED),
    ("a quoted seed with an escape", replay_text(live_seed='"1\\u0032"'), None),
    ("a reference of zero", replay_text(live_reference_rms="0"), REPLAY_REFERENCE),
    ("no reference", replay_text(live_reference_rms=None), REPLAY_REFERENCE),
    ("a quoted reference", replay_text(live_reference_rms='"0.125"'), REPLAY_REFERENCE),
    ("a reference with an exponent", replay_text(live_reference_rms="1.25e-1"), None),
    ("live_frames that is no number", replay_text(live_frames='"x"'), "invalid live_frames"),
    ("quoted live_frames", replay_text(live_frames=f'"{REPLAY_SOURCE_FRAMES}"'), None),
    ("no controls", replay_text(controls=None), REPLAY_CONTROLS),
    ("controls that are an object", replay_text(controls="{}"), REPLAY_CONTROLS),
    ("128 controls", replay_text(controls=replay_controls(*(replay_control('"CW CI 1"', str(index * 20)) for index in range(128)))), None),
    ("129 controls", replay_text(controls=replay_controls(*(replay_control('"CW CI 1"', str(index * 20)) for index in range(129)))),
     REPLAY_CONTROLS),
    ("controls left open", replay_text(controls="[" + replay_control()), REPLAY_DOCUMENT),
    ("a control that is no object", replay_text(controls="[1]"), REPLAY_RECORD),
    ("a control with a member twice", replay_text(controls=replay_controls(replay_control(extra=',"events":1'))), REPLAY_RECORD),
    ("a command that is no string", replay_text(controls=replay_controls(replay_control(command="1"))), REPLAY_RECORD),
    ("a non-ASCII escape in a command", replay_text(controls=replay_controls(replay_control(command='"CW \\u00c9N"'))), REPLAY_RECORD),
    ("an ASCII escape in a command", replay_text(controls=replay_controls(replay_control(command='"CW \\u004fN"'))), None),
    ("a quoted apply_frame", replay_text(controls=replay_controls(replay_control(apply_frame='"7"'))), None),
    ("a negative apply_frame", replay_text(controls=replay_controls(replay_control(apply_frame="-1"))), REPLAY_RECORD),
    ("an event count that is no uint64", replay_text(controls=replay_controls(replay_control(events='"x"'))), REPLAY_RECORD),
    ("an event count the control does not give", replay_text(controls=replay_controls(replay_control(events="2"))), REPLAY_EVENTS),
    ("a scheduled command", replay_text(controls=replay_controls(replay_control(command='"AT:12 CW ON"'))), REPLAY_UNSCHEDULED),
    ("a command that is no live control", replay_text(controls=replay_controls(replay_control(command='"PLAY"'))), REPLAY_UNSCHEDULED),
    ("a control with a member of its own", replay_text(controls=replay_controls(replay_control(extra=',"note":{"x":[1]}'))), None),
    ("64 events at one frame", replay_text(controls=replay_controls(*(replay_control('"CW CI 20"') for _ in range(64)))), None),
    ("65 events at one frame", replay_text(controls=replay_controls(*(replay_control('"CW CI 20"') for _ in range(65)))), REPLAY_PENDING),
)


class LiveReplayReaderTests(unittest.TestCase):
    def test_each_plan_is_taken_or_refused_with_its_reason(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.wav"
            with wave.open(str(source), "wb") as writer:
                writer.setparams((1, 3, 48000, 0, "NONE", "not compressed"))
                writer.writeframes(bytes(3 * REPLAY_SOURCE_FRAMES))
            for index, (case, text, refusal) in enumerate(REPLAY_CASES):
                with self.subTest(case=case):
                    events = root / f"plan{index}.json"
                    events.write_bytes(text.encode("utf-8"))
                    output = root / f"out{index}.wav"
                    result = subprocess.run([str(RENDERER), "--live-events", str(events), "--source", str(source),
                                             "--output", str(output)], capture_output=True, text=True, check=False)
                    if refusal is None:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stderr, "")
                        self.assertTrue(output.exists())
                    else:
                        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                        self.assertEqual(result.stderr, f"live replay error: {refusal}\n")
                        self.assertFalse(output.exists())


def read_pcm24(path):
    """A mono PCM24 WAV's samples as float64 in [-1, 1)."""
    with wave.open(str(path), "rb") as reader:
        packed = np.frombuffer(reader.readframes(reader.getnframes()), dtype=np.uint8)
    triples = packed.reshape(-1, 3).astype(np.int32)
    code = triples[:, 0] | (triples[:, 1] << 8) | (triples[:, 2] << 16)
    return ((code ^ 0x800000) - 0x800000) / 8388608.0


class AwgnCrossCheckTests(unittest.TestCase):
    """host/signal_lab/awgn.py against signal_lab_render on one scenario, statistically.

    The two are not sample-exact: Python draws NumPy PCG64 (SeedSequence([seed, 1, index]))
    and filters in float64, C++ draws PCG32 Box-Muller pairs and filters in float. Each
    renders 2^22 frames (87 s) of noise alone at 10 dB against a reference RMS of 0.125,
    and one Welch estimate (1024 Hann-windowed 4096-point segments) measures both.
    Measured when written: N0 x 3000 Hz +0.004 (C++) and -0.000 dB (Python); 500 Hz
    sub-bands within 0.05 dB; ENBW 3229 Hz for both; -77 dB at 3.7-6 kHz and -100 dB
    above 6 kHz.
    """

    FRAMES = 1 << 22
    SEGMENT = 4096
    RATE_HZ = 48000.0
    REFERENCE_RMS = 0.125
    SNR_DB = 10.0
    SCENARIO = {"seed": 7, "source_gain_db": 0, "reference_rms": REFERENCE_RMS,
                "impairments": [{"type": "awgn", "snr_db": SNR_DB}]}
    # N0 by the Welch mean over 500-3100 Hz (U30's interior): a relative s.e. near
    # 0.01 dB, so 0.05 dB on each engine and on their difference.
    INTERIOR_HZ = (500.0, 3100.0)
    N0_BOUND_DB = 0.05
    # Flatness: each 500 Hz sub-band of 300-3300 Hz against N0; about 0.025 dB is one
    # s.e. per sub-band, and the kernel itself is flat to 0.01 dB.
    SUB_BAND_HZ = 500.0
    FLATNESS_BOUND_DB = 0.1
    # ENBW = total power / N0: 3233.4 Hz by design, estimated to about 0.3 %.
    ENBW_BOUND = 0.01
    # Stopband against N0: the kernel averages -77 dB over 3.7-6 kHz and -100 dB above.
    STOP_NEAR_HZ = (3700.0, 6000.0)
    STOP_NEAR_LIMIT_DB = -60.0
    STOP_FAR_LIMIT_DB = -80.0

    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "silence.wav"
            with wave.open(str(source), "wb") as writer:
                writer.setparams((1, 3, int(cls.RATE_HZ), 0, "NONE", "not compressed"))
                writer.writeframes(bytes(3 * cls.FRAMES))
            scenario = root / "awgn.json"
            scenario.write_text(json.dumps(cls.SCENARIO), encoding="utf-8")
            output = root / "awgn.wav"
            result = subprocess.run(
                [str(RENDERER), "--scenario", str(scenario), "--source", str(source),
                 "--output", str(output), "--sidecar", str(root / "awgn sidecar.json")],
                capture_output=True, text=True, check=False)
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            cls.cpp = read_pcm24(output)
        pipeline = build_pipeline({"impairments": cls.SCENARIO["impairments"]})
        pipeline.prepare(cls.REFERENCE_RMS, cls.FRAMES, cls.SCENARIO["seed"])
        cls.python = np.concatenate(list(pipeline.run(np.zeros(cls.FRAMES))))

    @classmethod
    def density(cls, samples):
        """One-sided power density per Hz (Welch, Hann, no overlap) and its bin frequencies."""
        window = np.hanning(cls.SEGMENT)
        segments = samples[:len(samples) // cls.SEGMENT * cls.SEGMENT].reshape(-1, cls.SEGMENT) * window
        power = np.mean(np.abs(np.fft.rfft(segments, axis=1)) ** 2, axis=0)
        return 2.0 * power / (cls.RATE_HZ * float(np.dot(window, window))), np.fft.rfftfreq(cls.SEGMENT, 1.0 / cls.RATE_HZ)

    @staticmethod
    def mean_in(density, frequencies, low, high):
        return float(np.mean(density[(frequencies >= low) & (frequencies < high)]))

    def measure(self, samples):
        density, frequencies = self.density(samples)
        n0 = self.mean_in(density, frequencies, *self.INTERIOR_HZ)
        edges = np.arange(SNR_BAND_HZ[0], SNR_BAND_HZ[1], self.SUB_BAND_HZ)
        return {"n0": n0,
                "sub_bands_db": [10 * math.log10(self.mean_in(density, frequencies, low, low + self.SUB_BAND_HZ) / n0)
                                 for low in edges],
                "enbw_hz": float(np.mean(samples * samples)) / n0,
                "stop_near_db": 10 * math.log10(self.mean_in(density, frequencies, *self.STOP_NEAR_HZ) / n0),
                "stop_far_db": 10 * math.log10(self.mean_in(density, frequencies, self.STOP_NEAR_HZ[1], self.RATE_HZ) / n0)}

    def test_python_and_cpp_awgn_meet_the_same_noise_targets(self):
        commanded = self.REFERENCE_RMS ** 2 * 10 ** (-self.SNR_DB / 10)
        measured = {"cpp": self.measure(self.cpp), "python": self.measure(self.python)}
        for engine, values in measured.items():
            with self.subTest(engine=engine):
                self.assertLessEqual(abs(10 * math.log10(values["n0"] * SNR_REFERENCE_BANDWIDTH_HZ / commanded)),
                                     self.N0_BOUND_DB)
                for level in values["sub_bands_db"]:
                    self.assertLessEqual(abs(level), self.FLATNESS_BOUND_DB)
                self.assertAlmostEqual(values["enbw_hz"] / noise_band_enbw_hz(), 1.0, delta=self.ENBW_BOUND)
                self.assertLess(values["stop_near_db"], self.STOP_NEAR_LIMIT_DB)
                self.assertLess(values["stop_far_db"], self.STOP_FAR_LIMIT_DB)
        self.assertLessEqual(abs(10 * math.log10(measured["python"]["n0"] / measured["cpp"]["n0"])), self.N0_BOUND_DB)
        self.assertAlmostEqual(measured["python"]["enbw_hz"] / measured["cpp"]["enbw_hz"], 1.0, delta=self.ENBW_BOUND)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--watterson-renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    WATTERSON_RENDERER = arguments.watterson_renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
