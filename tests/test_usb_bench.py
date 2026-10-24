# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Fixture isolation and media handoff refusal tests; no hardware is opened."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import usb_bench as bench


def port(device, serial, vid=0xCAFE, pid=0x4012):
    return SimpleNamespace(device=device, serial_number=serial, vid=vid, pid=pid)


class FixtureSelectionTests(unittest.TestCase):
    def test_each_serial_selects_only_its_board_even_when_both_are_present(self):
        ports = [port("COM8", "1170serial"), port("COM15", "1064serial")]
        self.assertEqual(bench.select_port(ports, "1064SERIAL"), "COM15")
        self.assertEqual(bench.select_port(ports, "1170serial"), "COM8")

    def test_missing_or_duplicate_serial_never_falls_back(self):
        for ports in ([], [port("COM8", "other")], [port("COM8", "same"), port("COM9", "same")]):
            with self.subTest(ports=ports), self.assertRaises(ValueError):
                bench.select_port(ports, "same")

    def test_probe_or_other_product_with_same_serial_is_not_a_player(self):
        for ports in ([port("COM1", "same", vid=0x0D28)], [port("COM2", "same", pid=0xFFFF)]):
            with self.subTest(ports=ports), self.assertRaises(ValueError):
                bench.select_port(ports, "same")

    def test_readonly_player_can_be_selected_for_identity_checks(self):
        self.assertEqual(bench.select_port([port("COM9", "serial", pid=0x4013)], "serial"), "COM9")

    def test_unconfigured_missing_and_duplicate_audio_refuse(self):
        for name, devices in ((None, []), ("RT1064 Player", [{"native_name": "Remote Audio"}]),
                              ("RT1064 Player", [{"native_name": "RT1064 Player"}] * 2)):
            with self.subTest(name=name, devices=devices), self.assertRaises(ValueError):
                bench.capture_device(devices, {"capture_name": name})

    def test_exact_audio_selection_does_not_use_first_endpoint(self):
        devices = [{"native_name": "Microphone"}, {"native_name": "Line (RT1064 Player)"}]
        self.assertEqual(bench.capture_device(devices, {"capture_name": "Line (RT1064 Player)"}),
                         "Line (RT1064 Player)")

    def test_profiles_accept_each_board_and_reject_wrong_family_or_remote_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            for board, label in bench.BOARDS.items():
                profile = {"board": board, "serial": "A1B2", "capture_name": f"Line ({label} Player)"}
                path.write_text(json.dumps(profile), encoding="utf-8")
                self.assertEqual(bench.read_profile(path), profile)
            for change in ({"board": "unknown"}, {"serial": ""}, {"serial": "wrong serial"},
                           {"capture_name": "Remote Audio"}, {"capture_name": "Line (RT1170 Player)"},
                           {"unexpected": 1}):
                profile = {"board": "MIMXRT1064-EVK", "serial": "A1B2", "capture_name": None, **change}
                path.write_text(json.dumps(profile), encoding="utf-8")
                with self.subTest(change=change), self.assertRaises(ValueError):
                    bench.read_profile(path)

    def test_control_forwards_board_serial_and_dismount_only_when_asserted(self):
        for board in bench.BOARDS:
            profile = {"board": board, "serial": "A1B2"}
            args = bench.control_args(profile, "COM9", Path("journal.jsonl"), ["STATUS?"])
            self.assertEqual(args[args.index("--expected-board") + 1], board)
            self.assertEqual(args[args.index("--expected-serial") + 1], "A1B2")
            self.assertNotIn("--host-volume-dismounted", args)
            self.assertIn("--host-volume-dismounted", bench.control_args(profile, "COM9", Path("j"), ["MEDIA LOCAL"], True))


class HandoffTests(unittest.TestCase):
    profile = {"board": "MIMXRT1064-EVK", "serial": "A1B2", "capture_name": None}

    def test_failed_identity_prevents_staging(self):
        args = argparse.Namespace(action="stage", file=Path("test.wav"), name="TEST.WAV")
        with mock.patch.object(bench, "logged_run", side_effect=subprocess.CalledProcessError(2, "identity")) as call:
            with self.assertRaises(subprocess.CalledProcessError):
                bench.perform(args, self.profile, "COM9", Path("run"))
        self.assertEqual(call.call_count, 1)

    def test_failed_stage_never_hands_media_to_firmware(self):
        args = argparse.Namespace(action="stage", file=Path("test.wav"), name="TEST.WAV")
        calls = []

        def execute(command, run, label, **kwargs):
            calls.append(command)
            if label == "stage":
                raise subprocess.CalledProcessError(4, command)

        with mock.patch.object(bench, "logged_run", side_effect=execute), self.assertRaises(subprocess.CalledProcessError):
            bench.perform(args, self.profile, "COM9", Path("run"))
        self.assertEqual(len(calls), 2)
        self.assertFalse(any("MEDIA LOCAL" in command for command in calls))

    def test_successful_stage_pins_serial_and_hands_off_after_ejection(self):
        args = argparse.Namespace(action="stage", file=Path("test.wav"), name="TEST.WAV")
        with mock.patch.object(bench, "logged_run") as call:
            bench.perform(args, self.profile, "COM9", Path("run"))
        commands = [entry.args[0] for entry in call.call_args_list]
        self.assertIn("--eject", commands[1])
        self.assertEqual(commands[1][commands[1].index("--expected-serial") + 1], "A1B2")
        self.assertEqual(commands[2][-1], "MEDIA LOCAL")
        self.assertIn("--host-volume-dismounted", commands[2])


class BoundedCaptureTests(unittest.TestCase):
    def test_expected_duration_limit_keeps_completed_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)

            def recorder(command, *, stdout, stderr, check, timeout):
                self.assertFalse(check)
                self.assertEqual(timeout, 25)
                stdout.write(b'{"record":"READY","capture_id":"one"}\n')
                stdout.write(b'{"record":"FINAL","capture_id":"one","completion_reason":"DURATION_LIMIT","samples_delivered":480,"samples_written":480}\n')
                (run / "capture.wav").write_bytes(b"RIFF" + bytes(44))
                return subprocess.CompletedProcess(command, 1)

            with mock.patch.object(bench.subprocess, "run", side_effect=recorder):
                bench.logged_bounded_capture(["recorder"], run, timeout=25)

    def test_capture_error_and_missing_wav_do_not_pass(self):
        for reason, wav in (("CAPTURE_ERROR", True), ("DURATION_LIMIT", False)):
            with self.subTest(reason=reason, wav=wav), tempfile.TemporaryDirectory() as directory:
                run = Path(directory)

                def recorder(command, *, stdout, stderr, check, timeout):
                    stdout.write(b'{"record":"READY","capture_id":"one"}\n')
                    stdout.write((json.dumps({"record": "FINAL", "capture_id": "one", "completion_reason": reason,
                                              "samples_delivered": 480, "samples_written": 480}) + "\n").encode())
                    if wav:
                        (run / "capture.wav").write_bytes(b"RIFF" + bytes(44))
                    return subprocess.CompletedProcess(command, 1)

                with mock.patch.object(bench.subprocess, "run", side_effect=recorder), self.assertRaises(ValueError):
                    bench.logged_bounded_capture(["recorder"], run, timeout=25)

    def test_missing_capture_identity_does_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)

            def recorder(command, *, stdout, stderr, check, timeout):
                stdout.write(b'{"record":"READY"}\n')
                stdout.write(b'{"record":"FINAL","completion_reason":"DURATION_LIMIT","samples_delivered":480,"samples_written":480}\n')
                (run / "capture.wav").write_bytes(b"RIFF" + bytes(44))
                return subprocess.CompletedProcess(command, 1)

            with mock.patch.object(bench.subprocess, "run", side_effect=recorder), self.assertRaises(ValueError):
                bench.logged_bounded_capture(["recorder"], run, timeout=25)


if __name__ == "__main__":
    unittest.main()
