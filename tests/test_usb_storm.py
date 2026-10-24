# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""host/usb_storm.py against a fake board: no port, audio device or process is opened."""

import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
import live_control as live
import usb_storm

IDENTITY = {"kind": "waveform-player", "board": "MIMXRT1170-EVK", "sample_rate_hz": 48000, "usb_serial": "fixture-serial", "build_id": "wfg-test"}


class StormBoard:
    """The board's USB? and STATUS? counters, which `on_session` may change as each session opens."""

    def __init__(self, *, with_ep0_order=True):
        self.counters = {"usb_stack_asserts": 0, "usb_assert_count": 0, "uac_stream_opens": 10, "uac_short_packets": 0, "uac_missed_intervals": 3}
        if with_ep0_order:
            self.counters.update({"usb_ep0_reordered": 0, "usb_ep0_unordered": 0})
        self.status = {"uac_play_missed_intervals": 0}
        self.sessions = 0
        self.closed = 0
        self.on_session = None
        self.refuse = set()

    def handle(self, sequence, text):
        field = live.QUERIES.get(text, "result")
        data = {"INFO?": IDENTITY, "USB?": {"format": "usb-diagnostics", **self.counters}, "STATUS?": self.status}.get(text, {})
        return json.dumps({"seq": sequence, "protocol": live.PROTOCOL, "ok": True, "state": 1, "run_id": None, field: dict(data)},
                          separators=(",", ":")).encode() + b"\n"


class Transport:
    def __init__(self, board):
        self.board = board
        self.output = bytearray()
        self.pending = bytearray()

    def write(self, data):
        self.pending.extend(data)
        if b"\n" in self.pending:
            line, _, rest = self.pending.partition(b"\n")
            self.pending = bytearray(rest)
            sequence, text = line.decode().split(" ", 1)
            self.output.extend(self.board.handle(int(sequence), text))
        return len(data)

    def read(self, count):
        chunk = bytes(self.output[:count])
        del self.output[:count]
        return chunk

    def close(self):
        self.board.closed += 1


def connector(board):
    def connect(journal):
        board.sessions += 1
        if board.sessions in board.refuse:
            raise OSError("could not open port 'COMX': PermissionError")
        if board.on_session is not None:
            board.on_session(board)
        controller = live.Controller(Transport(board), "fixture-serial", journal=journal)
        controller.identify()
        return controller
    return connect


class Process:
    """A capture command: it runs for `runs` polls, then exits with `code`."""

    def __init__(self, board, runs, code):
        self.board, self.runs, self.returncode = board, runs, None
        self.code = code
        board.counters["uac_stream_opens"] += 1

    def poll(self):
        if self.runs:
            self.runs -= 1
            return None
        self.returncode = self.code
        return self.code

    def wait(self, timeout=None):
        self.returncode = self.code
        return self.code


class StormTests(unittest.TestCase):
    def test_bursts_report_each_change_and_pass_without_asserts(self):
        board = StormBoard()
        launched = []

        def launch(command):
            launched.append(command)
            return Process(board, runs=3, code=0)

        def on_session(board):
            if board.sessions % 7 == 0:
                board.counters["usb_ep0_reordered"] += 1

        board.on_session = on_session
        with tempfile.TemporaryDirectory() as folder:
            churn = usb_storm.CaptureChurn(["capture", "--seconds", "2"], 6, launch=launch)
            result = usb_storm.run_storm(connector(board), Path(folder), bursts=3, sessions=24, capture=churn, sleep=lambda seconds: None)
            journals = sorted(path.name for path in Path(folder).iterdir())
        self.assertTrue(result["pass"])
        self.assertEqual((result["sessions"], result["failed_sessions"], result["asserts_rose"]), (72, 0, False))
        self.assertEqual(board.sessions, 1 + 3 * 25, "a baseline, then each burst's sessions and its closing read")
        self.assertEqual(board.closed, board.sessions, "every session's port is closed")
        self.assertEqual(journals, ["storm-00.jsonl", "storm-01.jsonl", "storm-02.jsonl", "storm-03.jsonl"])
        self.assertGreaterEqual(result["captures_started"], 3)
        self.assertTrue(all(command == ["capture", "--seconds", "2"] for command in launched))
        self.assertEqual(result["total_delta"]["usb_stack_asserts"], 0)
        self.assertEqual(result["total_delta"]["uac_stream_opens"], result["captures_started"], "each capture opened the stream once")
        self.assertEqual(sum(burst["delta"]["usb_ep0_reordered"] for burst in result["bursts"]), result["total_delta"]["usb_ep0_reordered"])
        self.assertEqual(result["total_delta"]["usb_ep0_reordered"], (1 + 3 * 25) // 7)

    def test_a_rising_assert_count_fails_the_storm(self):
        board = StormBoard(with_ep0_order=False)

        def on_session(board):
            if board.sessions == 30:
                board.counters["usb_stack_asserts"] += 2
                board.counters["usb_assert_count"] += 2

        board.on_session = on_session
        with tempfile.TemporaryDirectory() as folder:
            result = usb_storm.run_storm(connector(board), Path(folder), bursts=2, sessions=20, capture=usb_storm.CaptureChurn(None, 12),
                                         sleep=lambda seconds: None)
        self.assertFalse(result["pass"])
        self.assertTrue(result["asserts_rose"])
        self.assertEqual([burst["delta"]["usb_stack_asserts"] for burst in result["bursts"]], [0, 2])
        self.assertIsNone(result["total_delta"]["usb_ep0_reordered"], "an image without the EP0 order reports no such counter")

    def test_a_failed_session_is_reported_and_fails_the_storm(self):
        board = StormBoard()
        board.refuse = {5}
        with tempfile.TemporaryDirectory() as folder:
            result = usb_storm.run_storm(connector(board), Path(folder), bursts=1, sessions=10, capture=usb_storm.CaptureChurn(None, 12),
                                         sleep=lambda seconds: None)
        self.assertFalse(result["pass"])
        self.assertEqual(result["bursts"][0]["failed_sessions"][0]["session"], 4)

    def test_a_capture_still_running_is_not_started_again(self):
        board = StormBoard()
        launched = []
        churn = usb_storm.CaptureChurn(["capture"], 2, launch=lambda command: launched.append(command) or Process(board, runs=100, code=0))
        for index in range(10):
            churn.before_session(index)
        self.assertEqual(len(launched), 1, "one capture at a time")
        churn.finish()
        self.assertIsNone(churn.process)

    def test_the_command_line(self):
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr):
            self.assertEqual(usb_storm.main(["--port", "COMX", "--expected-serial", "S", "--record-dir", "x", "--sessions", "0"]), 2)
        self.assertIn("at least 1", stderr.getvalue())
        board = StormBoard()
        stdout = io.StringIO()
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(live, "SerialTransport", lambda port, timeout: Transport(board)), \
                mock.patch("sys.stdout", stdout):
            code = usb_storm.main(["--port", "COMX", "--expected-serial", "fixture-serial", "--record-dir", str(Path(folder) / "storm"),
                                   "--bursts", "2", "--sessions", "3", "--pause", "0"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(stdout.getvalue())["sessions"], 6)


if __name__ == "__main__":
    unittest.main()
