# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Transport and replay regressions; no serial dependency or hardware access.

The host's copies of the firmware's wire values (the STATUS end states, the
PLAYLIST? states and end causes) and the protocol document's player state and
error numbers are checked against the firmware's own tables, read from
rt1170/player.hpp and common/playlist.hpp.
"""

import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
import live_control as live


IDENTITY = {"kind": "waveform-player", "board": "MIMXRT1170-EVK", "sample_rate_hz": 48000,
            "usb_serial": "fixture-serial", "build_id": "wfg-test"}
RUN_ID = "0123456789abcdef" * 2
# A USB? reply's "usb" object: stream diagnostics and miss attribution.
USB_DIAGNOSTICS = {"format": "usb-diagnostics", "uac_stream_open": 1, "uac_missed_intervals": 4,
                   "uac_last_miss_frame_index": 9648, "uac_last_miss_unprimed": 0, "uac_last_miss_play": 0,
                   "uac_max_gap_frames": 4, "uac_watchdog_expiries": 3, "uac_unprimed_intervals": 0,
                   "uac_play_unprimed_intervals": 0}
# An ENGINE? reply's "engine" object: the play's engine_state and engine_error, and why.
ENGINE_DIAGNOSTICS = {"format": "engine-diagnostics", "engine_state": 2, "engine_error": 5,
                      "engine_reason": "watterson: needs a WattersonChannel (StageResources.watterson)"}


def response(sequence, command, *, ok=True, run_id=None, data=None):
    field = live.QUERIES.get(live._query_command(command), "result") if ok else "error"
    if data is None:
        data = {"accepted": True} if ok else {"code": "BAD_STATE", "message": "stopped only"}
    return {"seq": sequence, "protocol": live.PROTOCOL, "ok": ok,
            "state": 1, "run_id": run_id, field: data}


class FakeTransport:
    def __init__(self, handler=None, *, write_chunk=999, read_chunk=999):
        self.handler = handler or self.default_response
        self.write_chunk = write_chunk
        self.read_chunk = read_chunk
        self.input = bytearray()
        self.output = bytearray()
        self.requests = []
        self.run_id = None
        self.frame = 100
        self.closed = False
        self.info = dict(IDENTITY)

    def default_response(self, sequence, text):
        command = live.parse_command(text)
        if text == "INFO?":
            return response(sequence, text, run_id=self.run_id, data=dict(self.info))
        if text in ("STATUS?", "COUNTERS?"):
            return response(sequence, text, run_id=self.run_id,
                            data={"live_frame": self.frame, "live_seed": 23, "live_reference_rms": 0.25})
        if text == "USB?":
            return response(sequence, text, run_id=self.run_id, data=dict(USB_DIAGNOSTICS))
        if text.startswith("PLAY"):
            self.run_id = text.split(":", 1)[1] if ":" in text else RUN_ID
            return response(sequence, text, run_id=self.run_id,
                            data={"accepted": True, "live_seed": 23, "live_reference_rms": 0.25})
        if command.live:
            return response(sequence, text, run_id=self.run_id,
                            data={"accepted": True, "apply_frame": command.at_frame if command.at_frame is not None else self.frame,
                                  "events": command.events})
        return response(sequence, text, run_id=self.run_id)

    def write(self, data):
        count = min(len(data), self.write_chunk)
        self.input.extend(data[:count])
        if b"\n" in self.input:
            line, _, tail = self.input.partition(b"\n")
            self.input = bytearray(tail)
            sequence, text = line.decode("ascii").split(" ", 1)
            self.requests.append((int(sequence), text))
            reply = self.handler(int(sequence), text)
            if isinstance(reply, dict):
                reply = json.dumps(reply, separators=(",", ":")).encode() + b"\n"
            self.output.extend(reply)
        return count

    def read(self, count):
        count = min(count, self.read_chunk, len(self.output))
        chunk = bytes(self.output[:count])
        del self.output[:count]
        return chunk

    def close(self):
        self.closed = True


class CommandTests(unittest.TestCase):
    def test_supported_commands_and_sweep_schedule(self):
        for text in ("INFO?", "STATUS?", "COUNTERS?", "USB?", "LOAD:C600L.WAV", "PLAY", f"PLAY:{RUN_ID}",
                     "STOP", "MEDIA HOST", "MEDIA LOCAL", "SEED:18446744073709551615", "REFERENCE:0.125",
                     "GENERATE:600:long:INPUT.BIN:OUT.WAV", "ENCODE TONE 1000 INPUT.BIN OUT.WAV",
                     "ENCODE CUSTOM R50:S170:F1000 INPUT.BIN OUT.WAV",
                     "ENCODE FUTURE custom:profile INPUT.BIN OUT.WAV",
                     "FILES?", "FILES? 0", "FILES? 18446744073709551615", "PREPARE?", "PREPAREPLAY?",
                     "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP", "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:18446744073709551615",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:4294967295:4294967295:18446744073709551615",
                     "CW ON", "CW OFF", "CW FREQ 1800", "CW CI -3", "CW 1 ON", "CW 3 FREQ 2800",
                     "AT:42 CW 2 CI 3", "STATIC ON", "STATIC OFF", "STATIC RATE 1", "STATIC PEAK 20",
                     "FADE NOW 18 250", "OUTPUT CODEC", "OUTPUT USB"):
            with self.subTest(text=text):
                self.assertEqual(live.parse_command(text).text, text)
        self.assertTrue(live.parse_command("OUTPUT USB").mutates)
        self.assertFalse(live.parse_command("OUTPUT USB").live)
        sweep = live.parse_command("AT:48000 SWEEP CW FREQ 300 3400 16 250")
        self.assertEqual(sweep.events, 16)
        self.assertEqual(sweep.event_frames(48000), [48000 + 12000 * index for index in range(16)])
        self.assertEqual(live.parse_command("SWEEP CW 2 CI 20 -3 4 100").events, 4)
        self.assertEqual(live.parse_command("SWEEP FADE 6 30 5 1000 250").events, 5)

    def test_invalid_commands_fail_before_serial(self):
        for text in ("CW ON\nPLAY", "cw on", "CW  ON", " LOAD:X.WAV", "LOAD:../X.WAV", "LOAD:tool.exe",
                     "M110:600:long:X.BIN", "AT:01 CW ON", "AT:1 STOP", "SEED:-1", "SEED:18446744073709551616",
                     "GENERATE:700:long:X.BIN:Y.WAV", "GENERATE:600:long:../X.BIN:Y.WAV",
                     "AT:0 GENERATE:600:long:X.BIN:Y.WAV", "AT:0 ENCODE M110B 600:long X.BIN Y.WAV",
                     "ENCODE M110B bad\\profile X.BIN Y.WAV", 'ENCODE M110B bad"profile X.BIN Y.WAV',
                     "REFERENCE:nan", "REFERENCE:0", "CW FREQ 24000", "CW FREQ 0", "CW CI 121",
                     "STATIC RATE 0", "STATIC PEAK 61", "FADE NOW 121 250", "FADE NOW 6 0.0001",
                     "CW -1 ON", "CW 01 ON", "CW 4 ON", "CW 4 FREQ 1800", "SWEEP CW 4 CI 20 -3 4 100",
                     "SWEEP CW CI 20 -3 0 100", "SWEEP FADE 6 30 17 1000 250", "CW ON" + " " * 190,
                     "AT:18446744073709551615 SWEEP CW CI 20 -3 2 1",
                     "OUTPUT", "OUTPUT usb", "OUTPUT  USB", "OUTPUT USB ", "OUTPUT SPEAKER", "AT:0 OUTPUT USB",
                     "USB", "usb?", "USB? ", "USB??", "AT:0 USB?", "FILES", "files?", "FILES??", "FILES? ",
                     "FILES? +1", "FILES? -1", "FILES? 01", "FILES? 18446744073709551616", "FILES? 1 2", "AT:0 FILES?", "AT:0 FILES? 1",
                     "PREPARE??", "prepare?", "PREPARE? ", "AT:0 PREPARE?", "PREPAREPLAY??",
                     "prepareplay?", "PREPAREPLAY? ", "AT:0 PREPAREPLAY?"):
            with self.subTest(text=text):
                with self.assertRaises(live.ControlError):
                    live.parse_command(text)

        for text in ("PREPARE", "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:keep", "PREPARE:OUT.WAV:SRC.WAV:SCN.WAV:KEEP",
                     "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:01", "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:-1",
                     "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:18446744073709551616", "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:",
                     "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:2", "AT:0 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP"):
            with self.subTest(text=text):
                with self.assertRaises(live.ControlError):
                    live.parse_command(text)

        for text in ("PREPAREPLAY", "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:0:0",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:01:0", "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:+1:0",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:-1:0", "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:4294967296:0",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:01", "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:+1",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:-1", "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:4294967296",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1", "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0:01",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0:18446744073709551616",
                     "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0:1:2", "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:keep:1:0",
                     "PREPAREPLAY:out.WAV:SRC.WAV:SCN.SCN:KEEP:1:0", "AT:0 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0"):
            with self.subTest(text=text):
                with self.assertRaises(live.ControlError):
                    live.parse_command(text)

    def test_prepare_command_builder(self):
        self.assertEqual(live.prepare_command("SRC.WAV", "SCN.SCN", "OUT.WAV"),
                         "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP")
        self.assertEqual(live.prepare_command("SRC.WAV", "SCN.SCN", "OUT.WAV", retention="temporary", seed=0),
                         "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:0")
        for arguments in (("src.wav", "SCN.SCN", "OUT.WAV"), ("SRC.WAV", "SCN.WAV", "OUT.WAV")):
            with self.assertRaises(live.ControlError):
                live.prepare_command(*arguments)
        with self.assertRaises(live.ControlError):
            live.prepare_command("SRC.WAV", "SCN.SCN", "OUT.WAV", retention="archive")
        with self.assertRaises(live.ControlError):
            live.prepare_command("SRC.WAV", "SCN.SCN", "OUT.WAV", seed=-1)

    def test_prepare_play_command_builder_and_ranges(self):
        self.assertEqual(live.prepare_play_command("SRC.WAV", "SCN.SCN", "OUT.WAV", rate=1, margin_frames=0),
                         "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0")
        self.assertEqual(live.prepare_play_command("SRC.WAV", "SCN.SCN", "OUT.WAV", rate=live.UINT32_MAX,
                                                   margin_frames=live.UINT32_MAX, retention="temporary", seed=live.UINT64_MAX),
                         "PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:4294967295:4294967295:18446744073709551615")
        for options in ({"rate": 0, "margin_frames": 0}, {"rate": live.UINT32_MAX + 1, "margin_frames": 0},
                        {"rate": True, "margin_frames": 0}, {"rate": 1, "margin_frames": -1},
                        {"rate": 1, "margin_frames": live.UINT32_MAX + 1}):
            with self.subTest(options=options):
                with self.assertRaises(live.ControlError):
                    live.prepare_play_command("SRC.WAV", "SCN.SCN", "OUT.WAV", **options)
        for token in ("+1", "01", "4294967296", "-1"):
            with self.subTest(token=token):
                with self.assertRaises(SystemExit):
                    live.main(["prepare-play", "--source", "SRC.WAV", "--scenario", "SCN.SCN", "--output", "OUT.WAV",
                               "--rate", token, "--margin-frames", "0"])
        for token in ("+1", "01", "4294967296", "-1"):
            with self.subTest(margin=token):
                with self.assertRaises(SystemExit):
                    live.main(["prepare-play", "--source", "SRC.WAV", "--scenario", "SCN.SCN", "--output", "OUT.WAV",
                               "--rate", "1", "--margin-frames", token])

    def test_cut_commands(self):
        widest = "CUT:" + ",".join(f"{4294967280 + index}:4800:zero" for index in range(16))
        for text in ("CUT", "CUT:0:1:zero", "CUT:48000:480:zero,48000:480:drop,96000:4800:drop", widest):
            with self.subTest(text=text):
                command = live.parse_command(text)
                self.assertEqual(command.body, text)
                self.assertTrue(command.mutates)
                self.assertFalse(command.live)
        self.assertLessEqual(len(f"{live.UINT32_MAX} {widest}\n"), live.REQUEST_LIMIT)
        self.assertEqual(live.REQUEST_LIMIT, 384)
        for text in ("CUT:", "CUT:1:1:drop,", "CUT:1:1:DROP", "CUT:01:1:drop", "CUT:10:1:zero,9:1:zero", "CUT:10:5:drop,12:1:zero",
                     "CUT:10:5:zero,10:1:zero", "CUT:1:0:drop", "CUT:1:4801:zero", "AT:0 CUT", "CUT ", "cut",
                     "CUT:" + ",".join(["1:1:zero"] * 17), "CW ON" + " " * 400):
            with self.subTest(text=text):
                with self.assertRaises(live.ControlError):
                    live.parse_command(text)

    def test_cut_set_and_clear(self):
        self.assertEqual(live.cut_command("set", "48000:480:zero,96000:480:drop"), "CUT:48000:480:zero,96000:480:drop")
        self.assertEqual(live.cut_command("clear"), "CUT")
        for entries in ("96000:480:drop,48000:480:zero", "1:1", "", "1:4801:zero"):
            with self.subTest(entries=entries):
                with self.assertRaises(live.ControlError):
                    live.cut_command("set", entries)
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr):
            self.assertEqual(live.main(["cut", "set", "1:1:zero"]), 2)
            self.assertEqual(live.main(["--port", "COMX", "--expected-serial", "S", "--record", "x.jsonl", "cut", "set", "2:1:zero,1:1:zero"]), 2)
        self.assertIn("physical control requires --port", stderr.getvalue())
        self.assertIn("CUT_UNSORTED", stderr.getvalue())

    def test_json_rejects_duplicates_nonfinite_and_excessive_depth(self):
        for text in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":1e999}', '[' * 18 + '0' + ']' * 18):
            with self.subTest(text=text):
                with self.assertRaises(live.ProtocolError):
                    live.strict_json(text)


class PrepareProtocolTests(unittest.TestCase):
    @staticmethod
    def snapshot(phase):
        return {"phase": phase, "error": "", "source": "SRC.WAV", "scenario": "SCN.SCN", "output": "OUT.WAV",
                "retention": "keep", "seed": 7, "input_frames": 48000, "output_frames": 48000,
                "total_input_frames": 48000, "wav_sha256": "a" * 64, "pcm_sha256": "b" * 64}

    def test_prepare_capability_and_query_shape(self):
        phases = iter((5, 8))
        def handler(sequence, text):
            if text == "INFO?":
                info = dict(IDENTITY, capabilities=["prepare"])
                return response(sequence, text, data=info)
            if text == "PREPARE?":
                return response(sequence, text, data=self.snapshot(next(phases)))
            return response(sequence, text)
        transport = FakeTransport(handler)
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        accepted = controller.execute(live.prepare_command("SRC.WAV", "SCN.SCN", "OUT.WAV"))
        self.assertTrue(accepted["result"]["accepted"])
        current = controller.execute("PREPARE?")
        self.assertEqual(current["prepare"]["phase"], 5)
        self.assertEqual(controller.execute("PREPARE?")["prepare"]["phase"], 8)
        self.assertEqual([request[1] for request in transport.requests],
                         ["INFO?", "PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP", "PREPARE?", "PREPARE?"])

    def test_prepare_requires_advertised_capability(self):
        controller = live.Controller(FakeTransport(), "fixture-serial")
        controller.identify()
        with self.assertRaisesRegex(live.ControlError, "prepare capability"):
            controller.execute("PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP")

    def test_prepare_status_strict_validation_and_bounded_wait(self):
        snapshot = self.snapshot(8)
        reply = response(1, "PREPARE?", data=snapshot)
        live.Controller._validate_response(reply, 1, live.parse_command("PREPARE?"))
        for changed in ({**snapshot, "phase": True}, {**snapshot, "seed": "7"}, {**snapshot, "wav_sha256": "A" * 64},
                        {**snapshot, "source": "../SRC.WAV"}, {**snapshot, "retention": "archive"}, {**snapshot, "extra": 1},
                        {**snapshot, "wav_sha256": ""}):
            with self.subTest(changed=changed):
                with self.assertRaises(live.ProtocolError):
                    live.Controller._validate_response(response(1, "PREPARE?", data=changed), 1, live.parse_command("PREPARE?"))
        current_time = [0.0]
        phases = iter((5, 5, 8))
        def handler(sequence, text):
            if text == "INFO?":
                return response(sequence, text, data=dict(IDENTITY, capabilities=["prepare"]))
            return response(sequence, text, data=self.snapshot(next(phases)))
        controller = live.Controller(FakeTransport(handler), "fixture-serial", clock=lambda: current_time[0])
        controller.identify()
        result = live.wait_for_prepare(controller, timeout=4.0, interval=1.0,
                                       sleep=lambda duration: current_time.__setitem__(0, current_time[0] + duration),
                                       clock=lambda: current_time[0])
        self.assertEqual(result["prepare"]["phase"], 8)
        self.assertEqual(live.prepare_wait_exit_code(result), 0)
        failed = response(1, "PREPARE?", data=self.snapshot(10))
        self.assertEqual(live.prepare_wait_exit_code(failed), 4)

    def test_prepare_play_requires_experimental_capability_and_acceptance(self):
        command = live.prepare_play_command("SRC.WAV", "SCN.SCN", "OUT.WAV", rate=1200, margin_frames=4096)
        transport = FakeTransport(lambda sequence, text: response(sequence, text,
            data=dict(IDENTITY, capabilities=[live.PREPARE_PLAY_CAPABILITY]) if text == "INFO?" else {"accepted": True}))
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        accepted = controller.execute(command)
        self.assertTrue(accepted["result"]["accepted"])
        self.assertEqual([request[1] for request in transport.requests], ["INFO?", command])

        controller = live.Controller(FakeTransport(), "fixture-serial")
        controller.identify()
        with self.assertRaisesRegex(live.ControlError, "prepare-play-experimental capability"):
            controller.execute(command)

        rejected = response(1, command, data={"accepted": False})
        with self.assertRaises(live.ProtocolError):
            live.Controller._validate_response(rejected, 1, live.parse_command(command))


class PreparePlayStatusProtocolTests(unittest.TestCase):
    @staticmethod
    def timing():
        return {"cycle_hz": 996000000, "tick_hz": 1000,
                "phases": {phase: {"calls": 0, "total_cycles": 0, "max_cycles": 0,
                                   "rejected": 0, "saturated": False} for phase in live.PREPARE_TIMING_PHASES}}

    @staticmethod
    def snapshot():
        return {"active": True, "state": 4, "rate": 48000, "margin_frames": 2048,
                "total_frames": 96000, "required_lead_frames": 2048, "produced_frames": 64000,
                "written_frames": 60000, "committed_frames": 58000, "queued_frames": 55000,
                "consumed_frames": 50000, "started": True, "artifact_finalized": False,
                "action_pending": False, "owner_released": False, "operation_id": 1,
                "timing": PreparePlayStatusProtocolTests.timing(), "status": 0, "reason": ""}

    def test_query_is_read_only_and_capability_gated(self):
        snapshot = self.snapshot()

        def handler(sequence, text):
            if text == "INFO?":
                return response(sequence, text, data=dict(IDENTITY, capabilities=[live.PREPARE_PLAY_CAPABILITY]))
            return response(sequence, text, data=snapshot)

        transport = FakeTransport(handler)
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        result = controller.execute("PREPAREPLAY?")
        self.assertEqual(result["prepare_play"], snapshot)
        self.assertEqual([request[1] for request in transport.requests], ["INFO?", "PREPAREPLAY?"])
        self.assertFalse(live.parse_command("PREPAREPLAY?").mutates)

        controller = live.Controller(FakeTransport(), "fixture-serial")
        controller.identify()
        with self.assertRaisesRegex(live.ControlError, "prepare-play-experimental capability"):
            controller.execute("PREPAREPLAY?")

    def test_snapshot_strict_schema_types_ranges_and_watermarks(self):
        snapshot = self.snapshot()
        inactive = {key: False if key in ("active", "started", "artifact_finalized", "action_pending", "owner_released")
                    else "" if key == "reason" else 0 for key in snapshot}
        inactive["timing"] = dict(self.timing(), cycle_hz=0, tick_hz=0)
        upper_bounds = {**snapshot, "state": 6, "status": 8, "reason": "r" * 128}
        for value in (snapshot, inactive, upper_bounds):
            with self.subTest(snapshot=value):
                live.Controller._validate_response(response(1, "PREPAREPLAY?", data=value), 1,
                                                  live.parse_command("PREPAREPLAY?"))

        invalid = (
            {**snapshot, "extra": 1},
            {key: value for key, value in snapshot.items() if key != "reason"},
            {**snapshot, "active": 1},
            {**snapshot, "state": True},
            {**snapshot, "state": 7},
            {**snapshot, "rate": 1 << 32},
            {**snapshot, "margin_frames": -1},
            {**snapshot, "total_frames": True},
            {**snapshot, "status": 9},
            {**snapshot, "reason": None},
            {**snapshot, "reason": "r" * 129},
            {**snapshot, "reason": "not-ascii-μ"},
            {**snapshot, "consumed_frames": 55001},
            {**snapshot, "queued_frames": 58001},
            {**snapshot, "committed_frames": 60001},
            {**snapshot, "written_frames": 64001},
            {**snapshot, "produced_frames": 96001},
            {**snapshot, "required_lead_frames": 96001},
            {**snapshot, "operation_id": True},
            {**snapshot, "operation_id": 1 << 64},
            {**snapshot, "operation_id": 0},
        )
        for changed in invalid:
            with self.subTest(changed=changed):
                with self.assertRaises(live.ProtocolError):
                    live.Controller._validate_response(response(1, "PREPAREPLAY?", data=changed), 1,
                                                      live.parse_command("PREPAREPLAY?"))

    def test_timing_accepts_rejected_intervals_and_saturated_totals(self):
        timing = self.timing()
        timing["phases"]["commit"] = {"calls": 3, "total_cycles": 30, "max_cycles": 20,
                                       "rejected": 1, "saturated": False}
        live._check_prepare_play_snapshot(dict(self.snapshot(), timing=timing))
        timing["phases"]["commit"] = {"calls": live.UINT64_MAX, "total_cycles": live.UINT64_MAX,
                                       "max_cycles": live.UINT32_MAX, "rejected": live.UINT64_MAX,
                                       "saturated": True}
        live._check_prepare_play_snapshot(dict(self.snapshot(), timing=timing))
        timing = dict(self.timing(), cycle_hz=0)
        timing["phases"]["commit"].update(calls=1, rejected=1)
        live._check_prepare_play_snapshot(dict(self.snapshot(), timing=timing))

    def test_timing_rejects_malformed_clock_and_phase_schema(self):
        invalid = (None, {}, dict(self.timing(), cycle_hz=True), dict(self.timing(), tick_hz=1 << 32),
                   dict(self.timing(), extra=1), dict(self.timing(), phases=[]),
                   dict(self.timing(), phases={}), dict(self.timing(), phases={"surprise": {}}))
        for timing in invalid:
            with self.subTest(timing=timing), self.assertRaises(live.ProtocolError):
                live._check_prepare_play_snapshot(dict(self.snapshot(), timing=timing))

    def test_timing_rejects_invalid_counts_and_inconsistent_intervals(self):
        invalid = (None, {}, {"extra": 1}, {"calls": True}, {"total_cycles": 1 << 64},
                   {"rejected": -1}, {"max_cycles": 1 << 32}, {"saturated": 0},
                   {"calls": 1, "rejected": 2}, {"total_cycles": 1, "max_cycles": 2},
                   {"calls": 2, "rejected": 2, "total_cycles": 3})
        for changes in invalid:
            timing = self.timing()
            timing["phases"]["commit"] = (None if changes is None else {} if not changes
                                            else dict(timing["phases"]["commit"], **changes))
            with self.subTest(changes=changes), self.assertRaises(live.ProtocolError):
                live._check_prepare_play_snapshot(dict(self.snapshot(), timing=timing))
        timing = dict(self.timing(), cycle_hz=0)
        timing["phases"]["read"]["calls"] = 1
        with self.assertRaises(live.ProtocolError):
            live._check_prepare_play_snapshot(dict(self.snapshot(), timing=timing))

    def test_cli_prepare_play_status_queries_once_and_records_session(self):
        snapshot = self.snapshot()
        transport_holder = []

        def serial(port, timeout):
            transport = FakeTransport(lambda sequence, text: response(
                sequence, text,
                data=dict(IDENTITY, capabilities=[live.PREPARE_PLAY_CAPABILITY]) if text == "INFO?" else snapshot))
            transport_holder.append(transport)
            return transport

        stdout = io.StringIO()
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(live, "SerialTransport", serial), \
                mock.patch("sys.stdout", stdout):
            result = live.main(["--port", "COMX", "--expected-serial", "fixture-serial",
                                "--record", str(Path(folder) / "status.jsonl"), "prepare-play-status"])
            records = [json.loads(line) for line in (Path(folder) / "status.jsonl").read_text().splitlines()]
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(stdout.getvalue())["prepare_play"], snapshot)
        self.assertEqual([record.get("command") for record in records if record.get("record") == "command"],
                         ["INFO?", "PREPAREPLAY?", "STATUS?"])
        self.assertTrue(transport_holder[0].closed)


class PreparePlayWaitTests(unittest.TestCase):
    @staticmethod
    def completed():
        snapshot = PreparePlayStatusProtocolTests.snapshot()
        snapshot.update(active=False, state=4, started=True, artifact_finalized=True,
                        owner_released=True, action_pending=False, status=0, reason="ok")
        for field in ("produced_frames", "written_frames", "committed_frames", "queued_frames", "consumed_frames"):
            snapshot[field] = snapshot["total_frames"]
        return snapshot

    def controller(self, snapshots, journal=None):
        pending = iter(snapshots)
        def handler(sequence, text):
            if text == "INFO?":
                return response(sequence, text, data=dict(IDENTITY, capabilities=[live.PREPARE_PLAY_CAPABILITY]))
            self.assertEqual(text, "PREPAREPLAY?", "waiting must not start, stop or retry an operation")
            return response(sequence, text, data=next(pending))
        transport = FakeTransport(handler)
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        return controller, transport

    def test_wait_requires_owner_release_after_artifact_finalization(self):
        complete = self.completed()
        snapshots = [dict(PreparePlayStatusProtocolTests.snapshot(), state=2),
                     dict(complete, active=True, owner_released=False), complete]
        journal = io.StringIO()
        controller, transport = self.controller(snapshots, journal)
        clock = FakeClock()
        result = live.wait_for_prepare_play(controller, timeout=4.0, interval=0.25,
                                            sleep=clock.sleep, clock=clock)
        self.assertEqual(result["prepare_play"], complete)
        self.assertEqual(live.prepare_play_wait_exit_code(result), 0)
        self.assertEqual(clock.events, [("sleep", 0.25), ("sleep", 0.25)])
        self.assertEqual([text for _, text in transport.requests], ["INFO?"] + ["PREPAREPLAY?"] * 3)
        recorded = [json.loads(line) for line in journal.getvalue().splitlines()]
        queries = [row for row in recorded if row.get("command") == "PREPAREPLAY?"]
        self.assertEqual([row["response"]["prepare_play"] for row in queries], snapshots)

    def test_wait_rejects_operation_change_before_active_or_terminal_result(self):
        changed_snapshots = (dict(PreparePlayStatusProtocolTests.snapshot(), operation_id=42),
                             dict(self.completed(), operation_id=42),
                             dict(self.completed(), state=5, status=5, operation_id=42))
        for changed in changed_snapshots:
            with self.subTest(active=changed["active"], state=changed["state"]):
                first = dict(PreparePlayStatusProtocolTests.snapshot(), operation_id=41)
                controller, transport = self.controller([first, changed])
                clock = FakeClock()
                with self.assertRaisesRegex(live.ProtocolError, "operation_id changed"):
                    live.wait_for_prepare_play(controller, timeout=4, interval=0.25,
                                               sleep=clock.sleep, clock=clock)
                self.assertEqual(clock.events, [("sleep", 0.25)])
                self.assertEqual([text for _, text in transport.requests], ["INFO?", "PREPAREPLAY?", "PREPAREPLAY?"])

    def test_wait_ignores_unrelated_run_id_and_accepts_repeated_operation_id(self):
        complete = self.completed()
        snapshots = [dict(PreparePlayStatusProtocolTests.snapshot(), operation_id=17),
                     dict(complete, operation_id=17)]
        pending = iter(snapshots)

        def handler(sequence, text):
            if text == "INFO?":
                return response(sequence, text, run_id="a" * 32,
                                data=dict(IDENTITY, capabilities=[live.PREPARE_PLAY_CAPABILITY]))
            return response(sequence, text, run_id="b" * 32, data=next(pending))

        controller = live.Controller(FakeTransport(handler), "fixture-serial")
        controller.identify()
        clock = FakeClock()
        result = live.wait_for_prepare_play(controller, timeout=2, sleep=clock.sleep, clock=clock)
        self.assertEqual(result["prepare_play"]["operation_id"], 17)
        self.assertEqual(live.prepare_play_wait_exit_code(result), 0)

    def test_failure_and_cancellation_wait_for_cleanup_and_return_nonzero(self):
        for state, status in ((5, 5), (6, 0)):
            with self.subTest(state=state):
                terminal = dict(self.completed(), state=state, status=status, artifact_finalized=False)
                controller, _ = self.controller([dict(terminal, active=True, owner_released=False), terminal])
                clock = FakeClock()
                result = live.wait_for_prepare_play(controller, timeout=2, sleep=clock.sleep, clock=clock)
                self.assertEqual(live.prepare_play_wait_exit_code(result), 4)
                self.assertEqual(clock.events, [("sleep", 1.0)])

    def test_idle_and_preconfiguration_failure_do_not_claim_success(self):
        for status in (0, 2):
            with self.subTest(status=status):
                snapshot = dict(self.completed(), state=0, status=status, started=False,
                                owner_released=False, artifact_finalized=False)
                for field in ("total_frames", "required_lead_frames", "produced_frames", "written_frames",
                              "committed_frames", "queued_frames", "consumed_frames"):
                    snapshot[field] = 0
                controller, transport = self.controller([snapshot])
                clock = FakeClock()
                result = live.wait_for_prepare_play(controller, timeout=2, sleep=clock.sleep, clock=clock)
                self.assertEqual(live.prepare_play_wait_exit_code(result), 4)
                self.assertEqual(clock.events, [])
                self.assertEqual(len(transport.requests), 2)

    def test_setup_failure_may_have_operation_id_without_being_active(self):
        setup_failure = dict(self.completed(), state=0, status=2, operation_id=8,
                             started=False, owner_released=False, artifact_finalized=False)
        for field in ("total_frames", "required_lead_frames", "produced_frames", "written_frames",
                      "committed_frames", "queued_frames", "consumed_frames"):
            setup_failure[field] = 0
        live.Controller._validate_response(response(1, "PREPAREPLAY?", data=setup_failure), 1,
                                           live.parse_command("PREPAREPLAY?"))

    def test_success_requires_every_completion_fact(self):
        for changes in ({"active": True}, {"state": 5}, {"status": 5}, {"started": False},
                        {"artifact_finalized": False}, {"owner_released": False}, {"action_pending": True},
                        {"consumed_frames": self.completed()["total_frames"] - 1}):
            with self.subTest(changes=changes):
                self.assertEqual(live.prepare_play_wait_exit_code({"prepare_play": dict(self.completed(), **changes)}), 4)

    def test_completion_check_rejects_malformed_snapshots(self):
        for reply in ({}, {"prepare_play": None}, {"prepare_play": dict(self.completed(), status=False)},
                      {"prepare_play": dict(self.completed(), total_frames="96000")}):
            with self.subTest(reply=reply):
                self.assertEqual(live.prepare_play_wait_exit_code(reply), 4)

    def test_timeout_bounds_cleanup_wait_without_sending_stop(self):
        active_failure = dict(self.completed(), active=True, state=5, status=5, owner_released=False)
        controller, transport = self.controller([active_failure] * 3)
        clock = FakeClock()
        with self.assertRaises(TimeoutError):
            live.wait_for_prepare_play(controller, timeout=2, interval=3, sleep=clock.sleep, clock=clock)
        self.assertEqual(clock.now, 102.0)
        self.assertEqual(clock.events, [("sleep", 2.0)])
        self.assertTrue(all(text in ("INFO?", "PREPAREPLAY?") for _, text in transport.requests))
        self.assertLessEqual(len(transport.requests), 3)

    def test_invalid_wait_arguments_do_not_query(self):
        for parameter in ("timeout", "interval"):
            for value in (0, -1, float("nan"), float("inf")):
                with self.subTest(parameter=parameter, value=value):
                    controller = mock.Mock()
                    with self.assertRaises(live.ControlError):
                        live.wait_for_prepare_play(controller, **{"timeout": 1, "interval": 1, parameter: value})
                    controller.execute.assert_not_called()

    def test_malformed_reply_stops_polling_without_mutation(self):
        controller, transport = self.controller([dict(self.completed(), active=1)])
        clock = FakeClock()
        with self.assertRaises(live.UncertainCommand):
            live.wait_for_prepare_play(controller, timeout=2, sleep=clock.sleep, clock=clock)
        self.assertEqual(clock.events, [])
        self.assertTrue(controller.poisoned)
        self.assertEqual([text for _, text in transport.requests], ["INFO?", "PREPAREPLAY?"])

    def test_cli_wait_reports_terminal_exit_status_and_keeps_final_status_query(self):
        for state, expected in ((4, 0), (6, 4)):
            with self.subTest(state=state):
                terminal = dict(self.completed(), state=state)
                transport = FakeTransport(lambda sequence, text: response(sequence, text, data=(
                    dict(IDENTITY, capabilities=[live.PREPARE_PLAY_CAPABILITY]) if text == "INFO?" else terminal)))
                stdout, stderr = io.StringIO(), io.StringIO()
                with tempfile.TemporaryDirectory() as folder, mock.patch.object(live, "SerialTransport", return_value=transport), \
                        mock.patch("sys.stdout", stdout), mock.patch("sys.stderr", stderr):
                    code = live.main(["--port", "COMX", "--expected-serial", "fixture-serial",
                                      "--record", str(Path(folder) / "wait.jsonl"), "prepare-play-status",
                                      "--wait-timeout", "2", "--interval", "0.25"])
                self.assertEqual(code, expected, stderr.getvalue())
                self.assertEqual(json.loads(stdout.getvalue())["prepare_play"], terminal)
                self.assertEqual([text for _, text in transport.requests], ["INFO?", "PREPAREPLAY?", "STATUS?"])
                self.assertTrue(transport.closed)

    def test_cli_refuses_invalid_wait_before_opening_a_port(self):
        for flag in ("--wait-timeout", "--interval"):
            for value in ("0", "-1", "nan", "inf"):
                with self.subTest(flag=flag, value=value), tempfile.TemporaryDirectory() as folder, \
                        mock.patch.object(live, "SerialTransport") as serial, mock.patch("sys.stderr", io.StringIO()):
                    code = live.main(["--port", "COMX", "--expected-serial", "fixture-serial",
                                      "--record", str(Path(folder) / "unused.jsonl"), "prepare-play-status", flag, value])
                    self.assertEqual(code, 2)
                    serial.assert_not_called()


class ControllerTests(unittest.TestCase):
    def test_board_model_must_match_explicit_selection(self):
        for observed in ("MIMXRT1170-EVK", "MIMXRT1064-EVK"):
            for expected in ("MIMXRT1170-EVK", "MIMXRT1064-EVK"):
                with self.subTest(observed=observed, expected=expected):
                    transport = FakeTransport()
                    transport.info["board"] = observed
                    controller = live.Controller(transport, "fixture-serial", expected_board=expected)
                    if observed == expected:
                        self.assertEqual(controller.identify()["board"], expected)
                        controller.execute("PLAY")
                    else:
                        with self.assertRaises(live.ProtocolError):
                            controller.identify()
                        with self.assertRaises(live.UncertainCommand):
                            controller.execute("PLAY")
                        self.assertEqual(transport.requests, [(1, "INFO?")])

    def test_rt1064_still_requires_matching_serial(self):
        transport = FakeTransport()
        transport.info["board"] = "MIMXRT1064-EVK"
        controller = live.Controller(transport, "wrong", expected_board="MIMXRT1064-EVK")
        with self.assertRaises(live.ProtocolError):
            controller.identify()
        self.assertTrue(controller.poisoned)

    def test_identity_precedes_mutations_and_partial_io_is_supported(self):
        transport = FakeTransport(write_chunk=3, read_chunk=7)
        controller = live.Controller(transport, "fixture-serial")
        with self.assertRaises(live.ControlError):
            controller.execute("PLAY")
        self.assertEqual(transport.requests, [])
        self.assertEqual(controller.identify(), IDENTITY)
        reply = controller.execute("AT:1000 CW ON")
        self.assertEqual(reply["result"]["apply_frame"], 1000)
        self.assertEqual(transport.requests, [(1, "INFO?"), (2, "AT:1000 CW ON")])

    def test_wrong_serial_poisoning_blocks_mutations(self):
        transport = FakeTransport()
        controller = live.Controller(transport, "wrong-fixture")
        with self.assertRaises(live.ProtocolError):
            controller.identify()
        with self.assertRaises(live.UncertainCommand):
            controller.execute("PLAY")
        self.assertEqual(transport.requests, [(1, "INFO?")])

    def test_media_local_requires_operator_dismount_assertion(self):
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        with self.assertRaises(live.ControlError):
            controller.execute("MEDIA LOCAL")
        self.assertEqual(len(transport.requests), 1)
        controller.volume_dismounted = True
        controller.execute("MEDIA LOCAL")
        self.assertEqual(transport.requests[-1], (2, "MEDIA LOCAL"))

    def test_explicit_rejection_consumes_sequence_without_retry(self):
        transport = FakeTransport()
        default = transport.default_response
        transport.handler = lambda seq, cmd: response(seq, cmd, ok=False) if cmd == "PLAY" else default(seq, cmd)
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        with self.assertRaises(live.CommandRejected):
            controller.execute("PLAY")
        controller.execute("STATUS?")
        self.assertEqual(transport.requests, [(1, "INFO?"), (2, "PLAY"), (3, "STATUS?")])

    def test_malformed_response_is_uncertain_and_never_retried(self):
        cases = [
            lambda seq, cmd: response(seq + 1, cmd, data=IDENTITY),
            lambda seq, cmd: {**response(seq, cmd, data=IDENTITY), "protocol": "WFG/1"},
            lambda seq, cmd: {**response(seq, cmd, data=IDENTITY), "run_id": "invalid"},
            lambda seq, cmd: {**response(seq, cmd, data=IDENTITY), "extra": 1},
            lambda seq, cmd: b'{"seq":1,"seq":1}\n',
            lambda seq, cmd: b'{"notfinite":NaN}\n',
            lambda seq, cmd: b"x" * 4097 + b"\n",
            lambda seq, cmd: json.dumps(response(seq, cmd, data=IDENTITY)).encode() + b"\nunsolicited\n",
        ]
        for handler in cases:
            with self.subTest(handler=handler):
                transport = FakeTransport(handler)
                journal = io.StringIO()
                controller = live.Controller(transport, "fixture-serial", journal=journal)
                with self.assertRaises(live.UncertainCommand):
                    controller.identify()
                with self.assertRaises(live.UncertainCommand):
                    controller.execute("INFO?")
                self.assertEqual(transport.requests, [(1, "INFO?")])
                self.assertTrue(json.loads(journal.getvalue())["uncertain"])

    def test_timeout_records_uncertain_request(self):
        instant = [0.0]
        def clock():
            instant[0] += 0.01
            return instant[0]
        transport = FakeTransport(lambda seq, cmd: b"")
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal, timeout=0.1, clock=clock)
        with self.assertRaises(live.UncertainCommand):
            controller.identify()
        self.assertTrue(controller.poisoned)
        self.assertEqual(controller.next_sequence, 2)
        self.assertIn("deadline", json.loads(journal.getvalue())["error"])

    def test_one_request_outstanding(self):
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        default = transport.default_response
        def handler(seq, cmd):
            with self.assertRaises(live.ControlError):
                controller.execute("STATUS?")
            return default(seq, cmd)
        transport.handler = handler
        controller.identify()
        self.assertEqual(transport.requests, [(1, "INFO?")])

    def test_usb_query_is_read_only_and_answers_with_its_own_member(self):
        command = live.parse_command("USB?")
        self.assertFalse(command.mutates)
        self.assertFalse(command.live)
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        # A query: allowed before the identity check, like STATUS?.
        reply = controller.execute("USB?")
        self.assertEqual(reply["usb"], USB_DIAGNOSTICS)
        self.assertEqual(transport.requests, [(1, "USB?")])
        for wrong in ("status", "result"):
            with self.subTest(member=wrong):
                transport = FakeTransport(lambda seq, cmd, wrong=wrong: {**{key: value for key, value in response(seq, cmd).items() if key != "usb"},
                                                                         wrong: dict(USB_DIAGNOSTICS)})
                controller = live.Controller(transport, "fixture-serial")
                with self.assertRaises(live.UncertainCommand):
                    controller.execute("USB?")

    def test_engine_query_is_read_only_and_answers_with_its_own_member(self):
        command = live.parse_command("ENGINE?")
        self.assertFalse(command.mutates)
        self.assertFalse(command.live)
        # A refusal's reason, no reason (null), and a reason whose quotes and backslash the wire escapes.
        replies = (ENGINE_DIAGNOSTICS, {**ENGINE_DIAGNOSTICS, "engine_state": 1, "engine_error": 0, "engine_reason": None},
                   {**ENGINE_DIAGNOSTICS, "engine_error": 3, "engine_reason": 'cw.phase_degrees must be a number or "random" (C:\\WG)'})
        for engine in replies:
            with self.subTest(engine=engine):
                transport = FakeTransport(lambda seq, cmd, engine=engine: response(seq, cmd, data=dict(engine)))
                controller = live.Controller(transport, "fixture-serial")
                # A query: allowed before the identity check, like STATUS?.
                reply = controller.execute("ENGINE?")
                self.assertEqual(reply["engine"], engine)
                self.assertEqual(transport.requests, [(1, "ENGINE?")])
        for wrong in ("status", "result", "usb"):
            with self.subTest(member=wrong):
                transport = FakeTransport(lambda seq, cmd, wrong=wrong: {**{key: value for key, value in response(seq, cmd).items() if key != "engine"},
                                                                         wrong: dict(ENGINE_DIAGNOSTICS)})
                controller = live.Controller(transport, "fixture-serial")
                with self.assertRaises(live.UncertainCommand):
                    controller.execute("ENGINE?")
        for text in ("ENGINE", "engine?", "ENGINE? ", "ENGINE??", "AT:0 ENGINE?"):
            with self.subTest(text=text):
                with self.assertRaises(live.ControlError):
                    live.parse_command(text)

    def test_files_query_is_read_only_and_validates_its_member(self):
        command = live.parse_command("FILES? 24")
        self.assertFalse(command.mutates)
        self.assertFalse(command.live)
        listing = {"offset": 24, "total": 25, "next_offset": None,
                   "entries": [{"name": "TONE.WAV", "size_bytes": live.UINT64_MAX}]}
        transport = FakeTransport(lambda seq, cmd: response(seq, cmd, data=listing))
        reply = live.Controller(transport, "fixture-serial").execute("FILES? 24")
        self.assertEqual(reply["files"], listing)
        self.assertEqual(transport.requests, [(1, "FILES? 24")])
        for data in ({**listing, "extra": 1}, {**listing, "offset": 23}, {**listing, "next_offset": 25},
                     {**listing, "entries": [{"name": "TONE.WAV", "size_bytes": True}]}):
            with self.subTest(data=data):
                transport = FakeTransport(lambda seq, cmd, data=data: response(seq, cmd, data=data))
                controller = live.Controller(transport, "fixture-serial")
                with self.assertRaises(live.UncertainCommand):
                    controller.execute("FILES? 24")
                self.assertTrue(controller.poisoned)


class FilesTests(unittest.TestCase):
    def test_page_validation_rejects_bad_shapes_offsets_names_sizes_and_progress(self):
        valid = {"offset": 0, "total": 2, "next_offset": 1,
                 "entries": [{"name": "Ä.WAV", "size_bytes": 123}]}
        self.assertIs(live._check_files_page(valid, 0), valid)
        # exFAT has no 8.3 short alias; the firmware returns fname in the same
        # field when altname is empty. Its fixed field is bounded by UTF-8 bytes.
        for name in ("CANARY2400L.WAV", "M110B24S.JSON", "é" * 18):
            page = {"offset": 0, "total": 1, "next_offset": None,
                    "entries": [{"name": name, "size_bytes": 123}]}
            self.assertIs(live._check_files_page(page, 0), page)
        bad_pages = (
            {**valid, "offset": 1}, {**valid, "total": True}, {**valid, "next_offset": 0},
            {**valid, "next_offset": 2}, {**valid, "entries": [{"name": "A.WAV", "size_bytes": True}]},
            {**valid, "entries": [{"name": "", "size_bytes": 1}]},
            {**valid, "entries": [{"name": "A" * 37, "size_bytes": 1}]},
            {**valid, "entries": [{"name": "\u0085.WAV", "size_bytes": 1}]},
            {**valid, "entries": [{"name": "𐀀" * 12, "size_bytes": 1}]},
            {**valid, "entries": [{"name": "../A.WAV", "size_bytes": 1}]},
            {**valid, "entries": [{"name": "A\\B.WAV", "size_bytes": 1}]},
            {**valid, "entries": [{"name": "A.WAV", "size_bytes": 1}] * 25},
            {**valid, "entries": [{"name": "A.WAV", "size_bytes": -1}]},
            {**valid, "entries": [{"name": "A.WAV", "size_bytes": 1}, {"name": "a.wav", "size_bytes": 2}]},
            {**valid, "entries": [{"name": "A.WAV", "size_bytes": 1, "extra": 0}]},
        )
        for page in bad_pages:
            with self.subTest(page=page), self.assertRaises(live.ProtocolError):
                live._check_files_page(page, 0)

    def test_files_page_accepts_bounded_exfat_fname_fallback(self):
        name = "CANARY2400L.WAV"
        self.assertGreater(len(name), 12)
        page = {"offset": 0, "total": 1, "next_offset": None,
                "entries": [{"name": name, "size_bytes": 576044}]}
        transport = FakeTransport(lambda seq, cmd: response(seq, cmd, data=page) if cmd == "FILES?"
                                  else transport.default_response(seq, cmd))
        transport.info["capabilities"] = [live.FILES_CAPABILITY]
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        self.assertEqual(live.list_files(controller), {"total": 1, "entries": page["entries"]})

    def test_listing_checks_info_capability_and_strictly_paginates(self):
        pages = {
            0: {"offset": 0, "total": 3, "next_offset": 2,
                "entries": [{"name": "A.WAV", "size_bytes": 10}, {"name": "B.BIN", "size_bytes": 20}]},
            2: {"offset": 2, "total": 3, "next_offset": None,
                "entries": [{"name": "Ä.CFG", "size_bytes": 30}]},
        }

        def handler(sequence, text):
            if text.startswith("FILES?"):
                offset = 0 if text == "FILES?" else int(text[7:])
                return response(sequence, text, data=pages[offset])
            return transport.default_response(sequence, text)

        transport = FakeTransport(handler)
        transport.info["capabilities"] = [live.FILES_CAPABILITY]
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        self.assertEqual(live.list_files(controller), {"total": 3, "entries": [*pages[0]["entries"], *pages[2]["entries"]]})
        self.assertEqual(transport.requests, [(1, "INFO?"), (2, "FILES?"), (3, "FILES? 2")])

        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        with self.assertRaises(live.ControlError):
            live.list_files(controller)
        self.assertEqual(transport.requests, [(1, "INFO?")])

    def test_listing_rejects_total_changes_and_duplicate_aliases_across_pages(self):
        first = {"offset": 0, "total": 2, "next_offset": 1, "entries": [{"name": "A.WAV", "size_bytes": 1}]}
        second_variants = (
            {"offset": 1, "total": 3, "next_offset": 2, "entries": [{"name": "B.WAV", "size_bytes": 2}]},
            {"offset": 1, "total": 2, "next_offset": None, "entries": [{"name": "a.wav", "size_bytes": 2}]},
        )
        for second in second_variants:
            with self.subTest(second=second):
                pages = {0: first, 1: second}
                transport = None

                def handler(sequence, text):
                    if text.startswith("FILES?"):
                        offset = 0 if text == "FILES?" else int(text[7:])
                        return response(sequence, text, data=pages[offset])
                    return transport.default_response(sequence, text)

                transport = FakeTransport(handler)
                transport.info["capabilities"] = [live.FILES_CAPABILITY]
                controller = live.Controller(transport, "fixture-serial")
                controller.identify()
                with self.assertRaises(live.ProtocolError):
                    live.list_files(controller)

    def test_bad_live_ack_is_uncertain(self):
        for result in ({"accepted": True, "apply_frame": 101, "events": 1},
                       {"accepted": True, "apply_frame": 100, "events": True},
                       {"accepted": True, "events": 1}):
            with self.subTest(result=result):
                transport = FakeTransport()
                controller = live.Controller(transport, "fixture-serial")
                controller.identify()
                transport.handler = lambda seq, cmd: response(seq, cmd, data=result)
                with self.assertRaises(live.UncertainCommand):
                    controller.execute("AT:100 CW ON")


class ReplayTests(unittest.TestCase):
    def journal(self):
        transport = FakeTransport()
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in ("LOAD:C600L.WAV", "SEED:23", "REFERENCE:0.25", "AT:0 CW ON", f"PLAY:{RUN_ID}",
                        "CW CI 3", "AT:200 SWEEP CW FREQ 300 3400 16 250", "STATUS?"):
            controller.execute(command)
        return [json.loads(line) for line in journal.getvalue().splitlines()]

    def test_export_uses_acknowledged_output_frames_and_enqueue_order(self):
        plan = live.export_replay(self.journal())
        self.assertEqual(plan["recorded_run_id"], RUN_ID)
        self.assertEqual(plan["source_commands"], ["LOAD:C600L.WAV", "SEED:23", "REFERENCE:0.25"])
        self.assertEqual([item["apply_frame"] for item in plan["controls"]], [0, 100, 200])
        self.assertEqual(plan["controls"][2]["events"], 16)
        self.assertEqual(plan["live_seed"], 23)
        self.assertEqual(plan["live_reference_rms"], 0.25)

    def test_export_rejects_uncertainty_and_multiple_playbacks(self):
        records = self.journal()
        records[3]["uncertain"] = True
        with self.assertRaises(live.ControlError):
            live.export_replay(records)
        records = self.journal()
        duplicate_play = dict(next(record for record in records if record.get("command") == f"PLAY:{RUN_ID}"))
        duplicate_play["seq"] = 100
        duplicate_play["response"] = {**duplicate_play["response"], "seq": 100}
        records.append(duplicate_play)
        with self.assertRaises(live.ControlError):
            live.export_replay(records)

    def test_export_load_discards_previous_preloaded_controls(self):
        transport = FakeTransport()
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in ("LOAD:A.WAV", "CW ON", "LOAD:B.WAV", f"PLAY:{RUN_ID}", "STATUS?"):
            controller.execute(command)
        plan = live.export_replay([json.loads(line) for line in journal.getvalue().splitlines()])
        self.assertEqual(plan["controls"], [])

    def test_export_does_not_repeat_artifact_creation(self):
        transport = FakeTransport()
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in ("GENERATE:600:long:INPUT.BIN:OUT.WAV", "LOAD:OUT.WAV", f"PLAY:{RUN_ID}", "STATUS?"):
            controller.execute(command)
        plan = live.export_replay([json.loads(line) for line in journal.getvalue().splitlines()])
        self.assertEqual(plan["source_commands"], ["LOAD:OUT.WAV"])

    def test_export_and_replay_keep_the_output_route(self):
        transport = FakeTransport()
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in ("OUTPUT USB", "LOAD:OUT.WAV", f"PLAY:{RUN_ID}", "STATUS?"):
            controller.execute(command)
        plan = live.export_replay([json.loads(line) for line in journal.getvalue().splitlines()])
        self.assertEqual(plan["source_commands"], ["OUTPUT USB", "LOAD:OUT.WAV"])
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        run_id = live.replay(controller, plan)
        self.assertEqual([text for _, text in transport.requests], ["INFO?", "OUTPUT USB", "LOAD:OUT.WAV", f"PLAY:{run_id}"])

    def test_export_and_replay_keep_the_cut_schedule(self):
        transport = FakeTransport()
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in ("OUTPUT USB", "CUT:48000:480:zero", "LOAD:OUT.WAV", "CUT:48000:480:zero,48000:480:drop", f"PLAY:{RUN_ID}", "STATUS?"):
            controller.execute(command)
        plan = live.export_replay([json.loads(line) for line in journal.getvalue().splitlines()])
        # Each CUT replaces the list: the last accepted one is what PLAY took, sent last.
        self.assertEqual(plan["source_commands"], ["OUTPUT USB", "LOAD:OUT.WAV", "CUT:48000:480:zero,48000:480:drop"])
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        run_id = live.replay(controller, plan)
        self.assertEqual([text for _, text in transport.requests][-2:], ["CUT:48000:480:zero,48000:480:drop", f"PLAY:{run_id}"])

    def cut_journal(self, commands, *, capable=True, usb=None, reject=()):
        """A journal recorded on firmware whose INFO lists cut-injection (or not).

        `usb` is the "usb" object USB? answers with; commands in `reject` are
        refused by the board.
        """
        transport = FakeTransport()
        if capable:
            transport.info["capabilities"] = ["wav-files", live.CUT_CAPABILITY, "usb-audio"]
        default = transport.default_response

        def handler(sequence, text):
            if text in reject:
                return response(sequence, text, ok=False, run_id=transport.run_id)
            if text == "USB?" and usb is not None:
                return response(sequence, text, run_id=transport.run_id, data={**USB_DIAGNOSTICS, **usb})
            return default(sequence, text)

        transport.handler = handler
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in commands:
            try:
                controller.execute(command)
            except live.CommandRejected:
                pass
        return [json.loads(line) for line in journal.getvalue().splitlines()]

    def test_export_pins_no_cuts_on_cut_capable_firmware(self):
        # The board may hold a list from an earlier connection or a refused PLAY:
        # a plan from a journal without CUT clears it right before its PLAY.
        plan = live.export_replay(self.cut_journal(("LOAD:X.WAV", f"PLAY:{RUN_ID}", "STATUS?")))
        self.assertEqual(plan["source_commands"], ["LOAD:X.WAV", "CUT"])
        transport = FakeTransport()
        transport.info["capabilities"] = [live.CUT_CAPABILITY]
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        run_id = live.replay(controller, plan)
        self.assertEqual([text for _, text in transport.requests], ["INFO?", "LOAD:X.WAV", "CUT", f"PLAY:{run_id}"])
        plan = live.export_replay(self.cut_journal(("LOAD:X.WAV", f"PLAY:{RUN_ID}", "STATUS?"), capable=False))
        self.assertEqual(plan["source_commands"], ["LOAD:X.WAV"], "firmware without CUT gets no CUT")

    def test_export_keeps_the_last_accepted_cut_only(self):
        records = self.cut_journal(("CUT:100:1:zero", "LOAD:X.WAV", "CUT:200:2:drop", f"PLAY:{RUN_ID}", "STATUS?"),
                                   reject=("CUT:200:2:drop",))
        self.assertEqual(live.export_replay(records)["source_commands"], ["LOAD:X.WAV", "CUT:100:1:zero"],
                         "a refused CUT left the list as it was")
        records = self.cut_journal(("CUT:100:1:zero", "LOAD:X.WAV", "CUT", f"PLAY:{RUN_ID}", "STATUS?"))
        self.assertEqual(live.export_replay(records)["source_commands"], ["LOAD:X.WAV", "CUT"], "CUT alone cleared it")

    def test_export_refuses_cuts_set_on_another_connection(self):
        # The lane sent `cut set` as its own invocation: this journal has no CUT,
        # but the play's USB? counts the entries it took.
        took = {"uac_play_cut_count": 1, "uac_play_cut_rejected": 1}
        with self.assertRaisesRegex(live.ControlError, "another connection"):
            live.export_replay(self.cut_journal(("LOAD:X.WAV", f"PLAY:{RUN_ID}", "USB?", "STATUS?"), usb=took))
        with self.assertRaisesRegex(live.ControlError, "another connection"):
            live.export_replay(self.cut_journal(("CUT:1:1:zero", "LOAD:X.WAV", f"PLAY:{RUN_ID}", "USB?", "STATUS?"), usb=took))
        plan = live.export_replay(self.cut_journal(("CUT:1:1:zero,9:1:drop", "LOAD:X.WAV", f"PLAY:{RUN_ID}", "USB?", "STATUS?"), usb=took))
        self.assertEqual(plan["source_commands"], ["LOAD:X.WAV", "CUT:1:1:zero,9:1:drop"])
        none = {"uac_play_cut_count": 0, "uac_play_cut_rejected": 0}
        plan = live.export_replay(self.cut_journal(("LOAD:X.WAV", f"PLAY:{RUN_ID}", "USB?", "STATUS?"), usb=none))
        self.assertEqual(plan["source_commands"], ["LOAD:X.WAV", "CUT"])

    def test_replay_requires_the_cut_of_cut_capable_firmware(self):
        plan = live.export_replay(self.cut_journal(("LOAD:X.WAV", f"PLAY:{RUN_ID}", "STATUS?")))
        plan["source_commands"] = ["LOAD:X.WAV"]  # A plan exported before CUT was pinned.
        transport = FakeTransport()
        transport.info["capabilities"] = [live.CUT_CAPABILITY]
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        with self.assertRaisesRegex(live.ControlError, "CUT schedule"):
            live.replay(controller, plan)
        self.assertEqual(transport.requests, [(1, "INFO?")])

    def test_export_ignores_usb_diagnostics(self):
        transport = FakeTransport()
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in ("LOAD:OUT.WAV", "USB?", f"PLAY:{RUN_ID}", "USB?", "STATUS?"):
            controller.execute(command)
        plan = live.export_replay([json.loads(line) for line in journal.getvalue().splitlines()])
        self.assertEqual(plan["source_commands"], ["LOAD:OUT.WAV"])
        self.assertEqual(plan["controls"], [])
        self.assertEqual(plan["last_snapshot"]["live_frame"], transport.frame, "the STATUS? snapshot, not the USB? reply")

    def route_journal(self, output, commands):
        """A journal recorded on firmware whose INFO reports the route output."""
        transport = FakeTransport()
        transport.info["output"] = output
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in commands:
            controller.execute(command)
        return [json.loads(line) for line in journal.getvalue().splitlines()]

    def test_export_pins_an_inherited_route(self):
        # The route was set by an earlier connection; this journal never sent OUTPUT.
        plan = live.export_replay(self.route_journal("usb", ("LOAD:X.WAV", f"PLAY:{RUN_ID}", "STATUS?")))
        self.assertEqual(plan["source_commands"], ["OUTPUT USB", "LOAD:X.WAV"])
        plan = live.export_replay(self.route_journal("codec", ("LOAD:X.WAV", f"PLAY:{RUN_ID}", "STATUS?")))
        self.assertEqual(plan["source_commands"], ["OUTPUT CODEC", "LOAD:X.WAV"])
        transport = FakeTransport()
        transport.info["output"] = "usb"  # The board was left on USB after the recording.
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        run_id = live.replay(controller, plan)
        self.assertEqual([text for _, text in transport.requests], ["INFO?", "OUTPUT CODEC", "LOAD:X.WAV", f"PLAY:{run_id}"])

    def test_export_keeps_the_last_accepted_route_only(self):
        records = self.route_journal("codec", ("OUTPUT USB", "LOAD:X.WAV", "OUTPUT CODEC", f"PLAY:{RUN_ID}", "STATUS?"))
        self.assertEqual(live.export_replay(records)["source_commands"], ["OUTPUT CODEC", "LOAD:X.WAV"])
        rejected = next(record for record in records if record.get("command") == "OUTPUT CODEC")
        rejected["response"] = response(rejected["seq"], "OUTPUT CODEC", ok=False)
        self.assertEqual(live.export_replay(records)["source_commands"], ["OUTPUT USB", "LOAD:X.WAV"],
                         "a rejected OUTPUT left the route as it was")

    def test_export_rejects_an_unknown_recorded_route(self):
        with self.assertRaisesRegex(live.ControlError, "unknown output route"):
            live.export_replay(self.route_journal("speaker", ("LOAD:X.WAV", f"PLAY:{RUN_ID}", "STATUS?")))

    def test_replay_requires_the_route_of_route_capable_firmware(self):
        plan = live.export_replay(self.route_journal("usb", ("LOAD:X.WAV", f"PLAY:{RUN_ID}", "STATUS?")))
        plan["source_commands"] = ["LOAD:X.WAV"]
        transport = FakeTransport()
        transport.info["output"] = "usb"
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        with self.assertRaisesRegex(live.ControlError, "output route"):
            live.replay(controller, plan)
        self.assertEqual(transport.requests, [(1, "INFO?")])

    def test_identity_check_ignores_additive_info_fields(self):
        transport = FakeTransport()
        info = {**IDENTITY, "usb_pid": 0x4012, "output": "codec",
                "capabilities": ["wav-files", "usb-audio"]}
        transport.handler = lambda seq, cmd: response(seq, cmd, data=info)
        controller = live.Controller(transport, "fixture-serial")
        self.assertEqual(controller.identify(), info)
        transport = FakeTransport()
        transport.handler = lambda seq, cmd: response(seq, cmd, data={**info, "kind": "waveform-sound-card"})
        with self.assertRaises(live.ProtocolError):
            live.Controller(transport, "fixture-serial").identify()

    def test_export_rejects_snapshot_from_another_run(self):
        records = self.journal()
        records[-1]["response"]["run_id"] = "f" * 32
        with self.assertRaisesRegex(live.ControlError, "different playback"):
            live.export_replay(records)
    def test_replay_validates_entire_plan_before_mutating(self):
        plan = live.export_replay(self.journal())
        plan["controls"][-1]["apply_frame"] = -1
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        with self.assertRaises(live.ControlError):
            live.replay(controller, plan)
        self.assertEqual(transport.requests, [(1, "INFO?")])

    def test_replay_checks_firmware_identity_and_uses_fresh_run_id(self):
        plan = live.export_replay(self.journal())
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        run_id = live.replay(controller, plan)
        self.assertNotEqual(run_id, RUN_ID)
        self.assertEqual(transport.requests[-1][1], f"PLAY:{run_id}")
        self.assertIn((6, "AT:100 CW CI 3"), transport.requests)
        plan["identity"]["build_id"] = "wfg-other"
        before = list(transport.requests)
        with self.assertRaises(live.ControlError):
            live.replay(controller, plan)
        self.assertEqual(transport.requests, before)

    def test_replay_refills_pending_queue_using_output_cursor(self):
        plan = live.export_replay(self.journal())
        plan["controls"] = [{"command": "CW CI 3", "apply_frame": index * 100, "events": 1} for index in range(65)]
        transport = FakeTransport()
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        live.replay(controller, plan, sleep=lambda duration: self.fail("queue has free space"))
        self.assertEqual(transport.requests[-2][1], "STATUS?")
        self.assertEqual(transport.requests[-1][1], "AT:6400 CW CI 3")

    def test_replay_fails_if_output_cursor_passes_a_recorded_event(self):
        plan = live.export_replay(self.journal())
        plan["controls"] = [{"command": "CW CI 3", "apply_frame": index * 100, "events": 1} for index in range(65)]
        transport = FakeTransport()
        transport.frame = 6401
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        with self.assertRaisesRegex(live.ControlError, "missed"):
            live.replay(controller, plan)


REPOSITORY = Path(__file__).resolve().parent.parent
END_STATES = ("done", "aborted", "fault")


def source(path):
    return (REPOSITORY / path).read_text(encoding="utf-8")


def block(text, pattern):
    """The text a pattern's group 1 holds; the test fails if the firmware source no longer has it."""
    found = re.search(pattern, text, re.S)
    if found is None:
        raise AssertionError(f"not found in the firmware source: {pattern}")
    return found.group(1)


def enumerators(text, declaration):
    """The enumerators of `enum class <declaration> { ... };`, in order."""
    return re.findall(r"^\s*(\w+) = ", block(text, rf"enum class {declaration}\s*\{{(.*?)\}};"), re.M)


def published_wire_numbers(enum):
    """rt1170/player.hpp's published table of PlayerState or PlayerError: each enumerator's wire number."""
    rows = block(source("rt1170/player.hpp"), rf"= std::to_array<WireNumber<{enum}>>\(\s*\{{(.*?)\}}\);")
    return {name: int(number) for name, number in re.findall(rf"\{{{enum}::(\w+), (\d+)U\}}", rows)}


def documented_player_states():
    """docs/live-control-protocol.md's sentence of player state numbers: 'N name' items."""
    sentence = block(source("docs/live-control-protocol.md"), r"Playback states retain the diagnostic numeric mapping: (.*?)\.")
    items = re.split(r", | and ", " ".join(sentence.split()))
    return {words.replace(" ", "_"): int(number) for number, words in (item.split(" ", 1) for item in items)}


def documented_player_errors():
    """docs/live-control-protocol.md's player error table: a code, a list ('1, 2') or a range ('5 to 9') per row."""
    section = block(source("docs/live-control-protocol.md"), r"### Player errors\n(.*?)\n## ")
    numbers = {}
    for codes, names in re.findall(r"^\| ([0-9][0-9, to]*) \| (`\w+`(?:, `\w+`)*) \|", section, re.M):
        values = []
        for part in codes.split(", "):
            first, _, last = part.partition(" to ")
            values += range(int(first), int(last or first) + 1)
        names = re.findall(r"`(\w+)`", names)
        if len(values) != len(names):
            raise AssertionError(f"error table row {codes!r} names {len(names)} errors")
        numbers.update(zip(names, values))
    return numbers


class FirmwareWireValueTests(unittest.TestCase):
    """The host's and the protocol document's copies against the firmware's published tables."""

    def test_the_end_states_are_the_firmwares(self):
        states = published_wire_numbers("PlayerState")
        self.assertEqual(sorted(states), sorted(enumerators(source("rt1170/player.hpp"), r"PlayerState : std::uint32_t")),
                         "every player state has its row")
        self.assertEqual(live.PLAYER_END_STATES, {states[name]: name.upper() for name in END_STATES})

    def test_replay_stops_at_each_end_state_and_only_there(self):
        plan = live.export_replay(ReplayTests.journal(self))
        plan["controls"] = [{"command": "CW CI 3", "apply_frame": index * 100, "events": 1} for index in range(65)]
        for name, number in published_wire_numbers("PlayerState").items():
            for reported in (number, name.upper()):
                with self.subTest(state=reported):
                    transport = FakeTransport()
                    answer = transport.default_response

                    def reporting(sequence, text, answer=answer, reported=reported):
                        reply = answer(sequence, text)
                        if text == "STATUS?":
                            reply["state"] = reported
                        return reply

                    transport.handler = reporting
                    controller = live.Controller(transport, "fixture-serial")
                    controller.identify()
                    if name in END_STATES:
                        with self.assertRaisesRegex(live.ControlError, "playback ended"):
                            live.replay(controller, plan, sleep=lambda duration: None)
                    else:
                        live.replay(controller, plan, sleep=lambda duration: None)
                        self.assertEqual(transport.requests[-1][1], "AT:6400 CW CI 3")

    def test_the_protocol_document_numbers_the_states_and_errors_as_the_firmware(self):
        self.assertEqual(documented_player_states(), published_wire_numbers("PlayerState"))
        errors = published_wire_numbers("PlayerError")
        self.assertEqual(sorted(errors), sorted(enumerators(source("rt1170/player.hpp"), r"PlayerError : std::uint32_t")),
                         "every player error has its row")
        self.assertEqual(documented_player_errors(), errors)

    def test_the_playlist_states_and_end_causes_are_the_firmwares(self):
        playlist = source("common/playlist.hpp")
        names = dict(re.findall(r'case State::(\w+):\s*return "(\w+)";', block(playlist, r"state_name\(State state\) noexcept\s*\{(.*?)\n\}")))
        self.assertEqual(live.PLAYLIST_STATES, tuple(names[state] for state in enumerators(playlist, r"State : std::uint8_t")))
        causes = re.findall(r'"(\w+)"', block(playlist, r"end_cause_names\s*=\s*std::to_array<std::string_view>\(\s*\{(.*?)\}\s*\);"))
        self.assertEqual(live.PLAYLIST_END_CAUSES, tuple(causes))
        self.assertEqual(causes, enumerators(playlist, r"EndCause : std::uint8_t"), "one name per cause, in order")


class FakePlaylistBoard:
    """The board's result ring behind PLAYLIST? and PLAYLIST ACK, as the player keeps it.

    It holds `blocks` blocks numbered from 1, shows up to `reply_limit` oldest per
    PLAYLIST?, and frees up to an acknowledged number; a number not given yet
    is refused. While `running_polls` PLAYLIST? replies remain it reports the
    playlist running with no block; then its blocks appear and it is done.
    """

    def __init__(self, blocks: int, *, running_polls: int = 0, reply_limit: int = 3, ack_busy_replies: int = 0):
        self.next_seq = 1
        self.pending = []
        self.run = 1
        self.running_polls = running_polls
        self.reply_limit = reply_limit
        self.ack_busy_replies = ack_busy_replies
        self.later = blocks
        if not running_polls:
            self._produce()
        self.requests = []

    def _produce(self):
        for _ in range(self.later):
            self.pending.append({"seq": self.next_seq, "run": 1, "item": self.next_seq - 1, "label": f"L{self.next_seq}", "file": "A.WAV",
                                 "route": "usb", "result": "done", "uac_play_crc32_l": 1000 + self.next_seq})
            self.next_seq += 1
        self.later = 0

    def handle(self, sequence, text):
        self.requests.append(text)
        if text == "INFO?":
            return response(sequence, text, data=dict(IDENTITY))
        if text == "PLAYLIST?":
            state = "running" if self.running_polls else "done"
            if self.running_polls:
                self.running_polls -= 1
                if not self.running_polls:
                    self._produce()
            return response(sequence, text, data={"format": "playlist", "state": state, "name": "BATTERY.PL", "run": self.run, "policy": "stop",
                                                  "items": 7, "index": 7, "completed": 7, "failed": 0, "pending": len(self.pending),
                                                  "oldest_seq": self.pending[0]["seq"] if self.pending else 0, "next_seq": self.next_seq,
                                                  "blocks": [dict(block) for block in self.pending[:self.reply_limit]]})
        if text.startswith(live.PLAYLIST_ACK):
            through = int(text[len(live.PLAYLIST_ACK):])
            if self.ack_busy_replies:
                self.ack_busy_replies -= 1
                return response(sequence, text, ok=False, data={"code": "BUSY", "message": "BUSY"})
            if through >= self.next_seq:
                return response(sequence, text, ok=False, data={"code": "PLAYLIST_ACK_RANGE", "message": "PLAYLIST_ACK_RANGE"})
            freed = [block for block in self.pending if block["seq"] <= through]
            self.pending = [block for block in self.pending if block["seq"] > through]
            return response(sequence, text, data={"accepted": True, "apply_frame": 0, "events": len(freed)})
        return response(sequence, text)


class PlaylistTests(unittest.TestCase):
    def controller(self, board):
        transport = FakeTransport(board.handle)
        controller = live.Controller(transport, "fixture-serial")
        controller.identify()
        return controller

    def test_playlist_commands(self):
        for text in ("PLAYLIST?", "PLAYLIST:BATTERY.PL", "PLAYLIST:BATTERY.PL:CONTINUE", "PLAYLIST:LADDER", "PLAYLIST ACK:0", "PLAYLIST ACK:4294967295"):
            with self.subTest(text=text):
                self.assertEqual(live.parse_command(text).body, text)
        self.assertFalse(live.parse_command("PLAYLIST?").mutates)
        self.assertTrue(live.parse_command("PLAYLIST:BATTERY.PL").mutates and live.parse_command("PLAYLIST ACK:3").mutates)
        for text in ("PLAYLIST", "PLAYLIST:battery.pl", "PLAYLIST:BATTERY.PL:continue", "PLAYLIST:ABCDEFGHI.PL", "PLAYLIST:A.PLAY",
                     "PLAYLIST ACK:01", "PLAYLIST ACK:4294967296", "PLAYLIST ACK:", "AT:0 PLAYLIST:A.PL", "AT:0 PLAYLIST?", "PLAYLIST? "):
            with self.subTest(text=text):
                with self.assertRaises(live.ControlError):
                    live.parse_command(text)
        self.assertEqual(live.playlist_command("start", "BATTERY.PL"), "PLAYLIST:BATTERY.PL")
        self.assertEqual(live.playlist_command("start", "BATTERY.PL", continue_on_error=True), "PLAYLIST:BATTERY.PL:CONTINUE")
        self.assertEqual(live.playlist_command("status"), "PLAYLIST?")
        self.assertEqual(live.playlist_command("ack", seq=6), "PLAYLIST ACK:6")
        with self.assertRaises(live.ControlError):
            live.playlist_command("start", "battery.pl")
        with self.assertRaises(live.ControlError):
            live.playlist_command("ack", seq=-1)

    def test_a_refused_start_names_its_line(self):
        def handler(sequence, text):
            if text.startswith("PLAYLIST:"):
                return response(sequence, text, ok=False, data={"code": "PLAYLIST_WAV_INVALID", "message": "PLAYLIST_WAV_INVALID line 12: unsupported_format"})
            return response(sequence, text, data=dict(IDENTITY))
        controller = live.Controller(FakeTransport(handler), "fixture-serial")
        controller.identify()
        with self.assertRaisesRegex(live.CommandRejected, "PLAYLIST_WAV_INVALID line 12: unsupported_format"):
            controller.execute("PLAYLIST:BATTERY.PL")

    def test_drain_saves_every_block_once_then_acknowledges_it(self):
        board = FakePlaylistBoard(7)
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            result = live.drain_playlist(self.controller(board), blocks)
            records = [json.loads(line) for line in blocks.read_text().splitlines()]
        self.assertEqual((result["written"], result["acknowledged"], result["state"]), (7, 7, "done"))
        self.assertEqual([record["block"]["seq"] for record in records], list(range(1, 8)))
        self.assertTrue(all(record["schema"] == live.PLAYLIST_BLOCK_SCHEMA and record["playlist"] == "BATTERY.PL" for record in records))
        self.assertEqual(board.requests[1:], ["PLAYLIST?", "PLAYLIST ACK:3", "PLAYLIST?", "PLAYLIST ACK:6", "PLAYLIST?", "PLAYLIST ACK:7", "PLAYLIST?"],
                         "each block is acknowledged once it is saved, and the drain stops at an empty ring")
        self.assertEqual(board.pending, [])

    def test_drain_retries_only_explicit_busy_ack_after_saving_page(self):
        board = FakePlaylistBoard(2, reply_limit=2, ack_busy_replies=1)
        sleeps = []
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            result = live.drain_playlist(self.controller(board), blocks, interval=0.25, sleep=sleeps.append)
            records = [json.loads(line) for line in blocks.read_text().splitlines()]
        acks = [request for request in board.requests if request.startswith(live.PLAYLIST_ACK)]
        self.assertEqual((result["written"], result["acknowledged"], len(records)), (2, 2, 2))
        self.assertEqual(acks, ["PLAYLIST ACK:2", "PLAYLIST ACK:2"], "only a definite BUSY refusal retries the same saved page")
        self.assertEqual(sleeps, [0.25])
        self.assertEqual(board.pending, [])

    def test_drain_busy_ack_retries_are_bounded_and_leave_saved_blocks(self):
        board = FakePlaylistBoard(1, ack_busy_replies=live.DRAIN_ACK_BUSY_RETRIES + 1)
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            with self.assertRaisesRegex(live.CommandRejected, "BUSY"):
                live.drain_playlist(self.controller(board), blocks, sleep=lambda _: None)
            records = [json.loads(line) for line in blocks.read_text().splitlines()]
        acks = [request for request in board.requests if request.startswith(live.PLAYLIST_ACK)]
        self.assertEqual((len(records), records[0]["block"]["seq"]), (1, 1), "the result is durable before BUSY ACK retries")
        self.assertEqual(len(acks), live.DRAIN_ACK_BUSY_RETRIES + 1)
        self.assertEqual(board.pending[0]["seq"], 1, "a refused ACK must leave the result available")

    def test_a_block_saved_before_is_never_saved_twice(self):
        board = FakePlaylistBoard(5)
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            # An earlier drain saved the first three, and its ACK was lost.
            blocks.write_text("".join(json.dumps({"schema": live.PLAYLIST_BLOCK_SCHEMA, "playlist": "BATTERY.PL", "block": block}) + "\n"
                                      for block in board.pending[:3]))
            result = live.drain_playlist(self.controller(board), blocks)
            seqs = [json.loads(line)["block"]["seq"] for line in blocks.read_text().splitlines()]
        self.assertEqual((result["written"], result["acknowledged"]), (2, 5))
        self.assertEqual(seqs, [1, 2, 3, 4, 5])

    def test_drain_names_only_the_running_playlists_blocks(self):
        # LADDER.PL (run 1) left two blocks unacknowledged; BATTERY.PL (run 2)
        # runs now, and PLAYLIST? names only it.
        board = FakePlaylistBoard(5)
        board.run = 2
        for block in board.pending:
            block["run"] = 1 if block["seq"] <= 2 else 2
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            result = live.drain_playlist(self.controller(board), blocks)
            records = [json.loads(line) for line in blocks.read_text().splitlines()]
        self.assertEqual(result["written"], 5)
        self.assertEqual([(record["playlist"], record["block"]["run"]) for record in records],
                         [(None, 1), (None, 1), ("BATTERY.PL", 2), ("BATTERY.PL", 2), ("BATTERY.PL", 2)],
                         "an earlier run's block is saved with no name, never the running playlist's")

    def test_drain_until_done_polls_while_the_playlist_runs(self):
        sleeps = []
        board = FakePlaylistBoard(4, running_polls=3)
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            result = live.drain_playlist(self.controller(board), blocks, until_done=True, interval=0.5, sleep=sleeps.append)
        self.assertEqual((result["written"], result["state"]), (4, "done"))
        self.assertEqual(sleeps, [0.5, 0.5], "it sleeps between empty polls while the playlist runs")
        board = FakePlaylistBoard(4, running_polls=3)
        with tempfile.TemporaryDirectory() as folder:
            result = live.drain_playlist(self.controller(board), Path(folder) / "blocks.jsonl", sleep=sleeps.append)
        self.assertEqual((result["written"], result["state"]), (0, "running"), "without --until-done one empty poll ends the drain")
        board = FakePlaylistBoard(4, running_polls=10)
        with tempfile.TemporaryDirectory() as folder:
            result = live.drain_playlist(self.controller(board), Path(folder) / "blocks.jsonl", until_done=True, max_polls=2, sleep=sleeps.append)
        self.assertEqual(result["state"], "running", "--max-polls bounds the polls with nothing to save")

    def test_drain_shows_each_blocks_end_and_length(self):
        board = FakePlaylistBoard(4)
        looped = {"att_tenths_db": 200, "dur_frames": 172_800_000, "loops": 59, "end_cause": "missed_interval", "engine_deadline_misses": 0,
                  "uac_play_sent_frames": 2_855_520, "player_error": 23, "result": "error"}
        board.pending[1].update(looped)
        board.pending[2].update({"att_tenths_db": 0, "dur_frames": None, "loops": 1, "end_cause": "end", "engine_deadline_misses": 2})
        with tempfile.TemporaryDirectory() as folder:
            result = live.drain_playlist(self.controller(board), Path(folder) / "blocks.jsonl")
        self.assertEqual(len(result["blocks"]), 4)
        self.assertEqual(result["blocks"][1], {"seq": 2, "item": 1, "label": "L2", **looped}, "a looped item's cause, loops, att= and dur=")
        self.assertEqual((result["blocks"][2]["dur_frames"], result["blocks"][2]["engine_deadline_misses"]), (None, 2))
        self.assertNotIn("end_cause", result["blocks"][0], "a block of an image before att= and dur= shows what it has")

    def test_drain_preserves_optional_watterson_and_engine_identities(self):
        board = FakePlaylistBoard(1)
        identity = {"watterson_version": 1, "watterson_digest_hi": 0x12345678, "watterson_digest_lo": 0x9abcdef0,
                    "watterson_latency_frames": 1176, "watterson_paths": 4, "engine_digest_hi": 0x10203040,
                    "engine_digest_lo": 0x50607080}
        board.pending[0].update(identity)
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            result = live.drain_playlist(self.controller(board), blocks)
            saved = json.loads(blocks.read_text().splitlines()[0])["block"]
        self.assertEqual({name: saved[name] for name in identity}, identity)
        self.assertEqual({name: result["blocks"][0][name] for name in identity}, identity,
                         "drain summaries retain both the Watterson channel identity and engine digest")

    def test_drain_accepts_new_two_block_pages_and_deduplicates_after_lost_ack(self):
        board = FakePlaylistBoard(5, reply_limit=2)
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            # The previous drain saved blocks 1 and 2, then lost their ACK.
            blocks.write_text("".join(json.dumps({"schema": live.PLAYLIST_BLOCK_SCHEMA, "playlist": "BATTERY.PL", "block": item}) + "\n"
                                      for item in board.pending[:2]))
            result = live.drain_playlist(self.controller(board), blocks)
            seqs = [json.loads(line)["block"]["seq"] for line in blocks.read_text().splitlines()]
        self.assertEqual((result["written"], result["acknowledged"], seqs), (3, 5, [1, 2, 3, 4, 5]))
        self.assertEqual([request for request in board.requests if request.startswith(live.PLAYLIST_ACK)],
                         ["PLAYLIST ACK:2", "PLAYLIST ACK:4", "PLAYLIST ACK:5"],
                         "the board's two-block pages are ACKed cumulatively and previously saved blocks are not written twice")

    def test_drain_validates_optional_identity_uint32_and_absence_rules(self):
        valid = {"watterson_version": 1, "watterson_digest_hi": 0, "watterson_digest_lo": 1,
                 "watterson_latency_frames": 0, "watterson_paths": 1, "engine_digest_hi": 0, "engine_digest_lo": 1}
        malformed = []
        for name in (*live.PLAYLIST_WATTERSON_IDENTITY, *live.PLAYLIST_ENGINE_DIGEST):
            for value in (True, -1, 1 << 32):
                malformed.append({name: value})
        malformed.append({"watterson_version": 0, "watterson_digest_hi": 0, "watterson_digest_lo": 0,
                          "watterson_latency_frames": 0, "watterson_paths": 1})
        for bad in malformed:
            with self.subTest(bad=bad):
                board = FakePlaylistBoard(1)
                board.pending[0].update(valid)
                board.pending[0].update(bad)
                with tempfile.TemporaryDirectory() as folder, self.assertRaises(live.ProtocolError):
                    live.drain_playlist(self.controller(board), Path(folder) / "blocks.jsonl")

        for omitted in ("watterson_version", "engine_digest_lo"):
            with self.subTest(omitted=omitted):
                board = FakePlaylistBoard(1)
                board.pending[0].update(valid)
                board.pending[0].pop(omitted)
                with tempfile.TemporaryDirectory() as folder, self.assertRaises(live.ProtocolError):
                    live.drain_playlist(self.controller(board), Path(folder) / "partial.jsonl")

        board = FakePlaylistBoard(1)
        board.pending[0].update({"watterson_version": 9, "watterson_digest_hi": 0, "watterson_digest_lo": 1,
                                 "watterson_latency_frames": 0, "watterson_paths": 17,
                                 "engine_digest_hi": 0, "engine_digest_lo": 1})
        with tempfile.TemporaryDirectory() as folder:
            result = live.drain_playlist(self.controller(board), Path(folder) / "future.jsonl")
        self.assertEqual(result["blocks"][0]["watterson_paths"], 17,
                         "validate field widths without hard-coding today's algorithm versions or path counts")

    def test_drain_preserves_second_source_identity_and_single_block_pages(self):
        board = FakePlaylistBoard(3, reply_limit=1)
        identity = {"second_source_version": 1, "second_source_source_frames": 96000,
                    "second_source_excerpt_start_frame": 12000, "second_source_excerpt_frames": 24000,
                    "second_source_digest_hi": 0x12345678, "second_source_digest_lo": 0xabcdef01,
                    "second_source_latency_frames": 215, "second_source_reference_rms": 0.125}
        board.pending[0].update(identity)
        board.pending[1].update({name: 0 for name in live.PLAYLIST_SECOND_SOURCE_IDENTITY})
        board.pending[2].update({**identity, "second_source_version": 9})
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            # A previous attempt persisted the first result but lost its ACK.
            blocks.write_text(json.dumps({"schema": live.PLAYLIST_BLOCK_SCHEMA, "playlist": "BATTERY.PL",
                                          "block": board.pending[0]}) + "\n")
            result = live.drain_playlist(self.controller(board), blocks)
            saved = [json.loads(line)["block"] for line in blocks.read_text().splitlines()]
        self.assertEqual((result["written"], result["acknowledged"]), (2, 3))
        self.assertEqual([item["seq"] for item in saved], [1, 2, 3])
        self.assertEqual({name: saved[0][name] for name in identity}, identity)
        self.assertEqual({name: result["blocks"][0][name] for name in identity},
                         {name: 0 for name in identity}, "summaries contain newly written results only")
        self.assertEqual(result["blocks"][1]["second_source_version"], 9)
        self.assertEqual([request for request in board.requests if request.startswith(live.PLAYLIST_ACK)],
                         ["PLAYLIST ACK:1", "PLAYLIST ACK:2", "PLAYLIST ACK:3"])

    def test_drain_refuses_incomplete_or_invalid_second_source_identity(self):
        valid = {"second_source_version": 1, "second_source_source_frames": 96000,
                 "second_source_excerpt_start_frame": 12000, "second_source_excerpt_frames": 24000,
                 "second_source_digest_hi": 0, "second_source_digest_lo": 1,
                 "second_source_latency_frames": 0, "second_source_reference_rms": 0.25}
        malformed = [{name: value} for name in live.PLAYLIST_SECOND_SOURCE_UINT32
                     for value in (True, -1, 1 << 32)]
        malformed += [{"second_source_reference_rms": value} for value in (True, None, "0.25", -1, 0, 1.01, 10 ** 500)]
        malformed += [{"second_source_version": 0}, {"second_source_excerpt_frames": 0},
                      {"second_source_excerpt_start_frame": 95999}, {"second_source_source_frames": 0}]
        for bad in malformed:
            with self.subTest(bad=bad):
                board = FakePlaylistBoard(1)
                board.pending[0].update({**valid, **bad})
                with tempfile.TemporaryDirectory() as folder, self.assertRaises(live.ProtocolError):
                    live.drain_playlist(self.controller(board), Path(folder) / "invalid.jsonl")
        for omitted in live.PLAYLIST_SECOND_SOURCE_IDENTITY:
            with self.subTest(omitted=omitted):
                board = FakePlaylistBoard(1)
                board.pending[0].update(valid)
                del board.pending[0][omitted]
                with tempfile.TemporaryDirectory() as folder, self.assertRaises(live.ProtocolError):
                    live.drain_playlist(self.controller(board), Path(folder) / "partial.jsonl")

    def test_drain_refuses_unknown_block_members(self):
        for bad in ({"end_cause": "lost"}, {"att_tenths_db": 401}, {"att_tenths_db": -1}, {"loops": -1}, {"dur_frames": "1"}, {"engine_deadline_misses": None}):
            with self.subTest(bad=bad):
                board = FakePlaylistBoard(1)
                board.pending[0].update(bad)
                with tempfile.TemporaryDirectory() as folder, self.assertRaises(live.ProtocolError):
                    live.drain_playlist(self.controller(board), Path(folder) / "blocks.jsonl")

    def test_drain_refuses_a_malformed_reply(self):
        for blocks in ([{"seq": 2}, {"seq": 1}], [{"seq": 1}] * 4, [{"seq": 3}]):
            with self.subTest(blocks=blocks):
                def handler(sequence, text, blocks=blocks):
                    if text == "PLAYLIST?":
                        return response(sequence, text, data={"format": "playlist", "state": "done", "name": "X.PL", "run": 1, "policy": "stop",
                                                              "items": 4, "index": 4, "completed": 4, "failed": 0, "pending": 4,
                                                              "oldest_seq": 1, "next_seq": 5, "blocks": blocks})
                    return response(sequence, text, data=dict(IDENTITY))
                controller = live.Controller(FakeTransport(handler), "fixture-serial")
                controller.identify()
                with tempfile.TemporaryDirectory() as folder:
                    with self.assertRaises(live.ProtocolError):
                        live.drain_playlist(controller, Path(folder) / "blocks.jsonl")

    def test_export_refuses_a_playlist_run(self):
        board = FakePlaylistBoard(1)
        transport = FakeTransport(lambda seq, cmd: board.handle(seq, cmd) if cmd.startswith("PLAYLIST") or cmd == "INFO?" else response(seq, cmd))
        journal = io.StringIO()
        controller = live.Controller(transport, "fixture-serial", journal=journal)
        controller.identify()
        for command in ("LOAD:X.WAV", "PLAYLIST:BATTERY.PL", "PLAYLIST?", "STATUS?"):
            controller.execute(command)
        with self.assertRaisesRegex(live.ControlError, "PLAYLIST run"):
            live.export_replay([json.loads(line) for line in journal.getvalue().splitlines()])

    def test_physical_playlist_commands_need_a_port(self):
        stderr = io.StringIO()
        with mock.patch("sys.stderr", stderr):
            self.assertEqual(live.main(["playlist", "start", "BATTERY.PL"]), 2)
            self.assertEqual(live.main(["--port", "COMX", "--expected-serial", "S", "--record", "x.jsonl", "playlist", "start", "battery.pl"]), 2)
            self.assertEqual(live.main(["--port", "COMX", "--expected-serial", "S", "--record", "x.jsonl", "playlist", "drain", "--blocks", "b.jsonl",
                                        "--interval", "0"]), 2)
        self.assertIn("physical control requires --port", stderr.getvalue())
        self.assertIn("8.3", stderr.getvalue())
        self.assertIn("--interval", stderr.getvalue())
        with mock.patch("sys.stderr", stderr):
            self.assertEqual(live.main(["--port", "COMX", "--expected-serial", "S", "--record", "x.jsonl", "playlist", "drain", "--blocks", "b.jsonl",
                                        "--reconnect-timeout", "-1"]), 2)
        self.assertIn("--reconnect-timeout", stderr.getvalue())


class FakeClock:
    """Host time for a drain: sleep() advances it, and every request takes `request_s`."""

    def __init__(self, request_s=0.002):
        self.now = 100.0
        self.request_s = request_s
        self.events = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.events.append(("sleep", seconds))
        self.now += seconds


class DrainBoard:
    """A board running a playlist for the drain tests: its ring, its RTOS tick, a reset and a lost link.

    While `running_polls` PLAYLIST? replies remain it reports the playlist
    running and makes a block every `every` replies; then the rest of its
    `blocks` and it is done. Its tick is ms since its last boot on `clock`.
    `reset()` is a board reset: the ring, the run and the tick start again.
    `fail_at` names request counts at which the link goes (the write raises
    OSError, as pyserial does when the port vanishes). `with_tick` False is an
    image from before PLAYLIST? reported its tick. `info` is its INFO? reply.
    """

    def __init__(self, clock, blocks, *, running_polls=0, every=1, with_tick=True):
        self.clock = clock
        self.boot = clock.now
        self.blocks = blocks
        self.made = 0
        self.running_polls = running_polls
        self.every = every
        self.with_tick = with_tick
        self.pending = []
        self.next_seq = 1
        self.run = 1
        self.state = "running" if running_polls else "done"
        self.replies = 0
        self.requests = []
        self.fail_at = set()
        self.info = dict(IDENTITY)
        if not running_polls:
            self._make(blocks)

    def tick(self):
        return int(round((self.clock.now - self.boot) * 1000)) & live.UINT32_MAX

    def reset(self):
        self.boot = self.clock.now
        self.pending = []
        self.next_seq = 1
        self.run = 0
        self.state = "idle"
        self.running_polls = 0
        self.made = self.blocks

    def _make(self, count):
        for _ in range(min(count, self.blocks - self.made)):
            self.pending.append({"seq": self.next_seq, "run": self.run, "item": self.made, "label": f"L{self.made}", "file": "A.WAV",
                                 "route": "usb", "result": "done", "uac_play_crc32_l": 7000 + self.made})
            self.next_seq += 1
            self.made += 1

    def _progress(self):
        if self.state == "running":
            self.replies += 1
            if self.replies % self.every == 0:
                self._make(1)
            if self.replies >= self.running_polls:
                self._make(self.blocks)
                self.state = "done"
        reply = {"format": "playlist", "state": self.state, "name": "SOAK.PL" if self.run else "", "run": self.run, "policy": "stop",
                 "items": self.blocks if self.run else 0, "index": self.made, "completed": self.made, "failed": 0, "pending": len(self.pending),
                 "oldest_seq": self.pending[0]["seq"] if self.pending else 0, "next_seq": self.next_seq, "blocks": [dict(b) for b in self.pending[:3]]}
        if self.with_tick:
            reply["tick"] = self.tick()
        return reply

    def handle(self, sequence, text):
        self.clock.now += self.clock.request_s
        self.requests.append((sequence, text))
        if len(self.requests) in self.fail_at:
            self.clock.events.append(("lost", text))
            raise OSError("ClearCommError failed (the device does not recognize the command)")
        self.clock.events.append(("request", text))
        if text == "INFO?":
            return response(sequence, text, data=dict(self.info))
        if text == "PLAYLIST?":
            return response(sequence, text, data=self._progress())
        if text.startswith(live.PLAYLIST_ACK):
            through = int(text[len(live.PLAYLIST_ACK):])
            freed = [block for block in self.pending if block["seq"] <= through]
            self.pending = [block for block in self.pending if block["seq"] > through]
            return response(sequence, text, data={"accepted": True, "apply_frame": 0, "events": len(freed)})
        return response(sequence, text, data={"live_frame": 0, "live_seed": 1, "live_reference_rms": 0.25})


class DrainSessionTests(unittest.TestCase):
    """The drain on one CDC session: the poll bound, a lost link, a board reset. No port is opened."""

    def session(self, board, clock, *, absent=0, journal=None):
        """The first identified session, and a reconnect that finds the port absent `absent` times first."""
        opened = []

        def connect(first_sequence=1):
            opened.append(first_sequence)
            if 1 < len(opened) <= 1 + absent:
                raise OSError("could not open port 'COMX': FileNotFoundError")
            controller = live.Controller(FakeTransport(board.handle), "fixture-serial", journal=journal, clock=clock)
            controller.next_sequence = first_sequence
            controller.identify()
            return controller

        return connect(), connect, opened

    def test_a_long_run_keeps_one_session_and_polls_at_most_once_per_interval(self):
        clock = FakeClock()
        board = DrainBoard(clock, 40, running_polls=1200, every=30)
        controller, connect, opened = self.session(board, clock)
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            result = live.drain_playlist(controller, blocks, until_done=True, interval=1.0, sleep=clock.sleep, clock=clock, reconnect=connect)
            seqs = [json.loads(line)["block"]["seq"] for line in blocks.read_text().splitlines()]
        self.assertEqual((result["ended"], result["state"], result["sessions"], result["links"]), ("done", "done", 1, []))
        self.assertEqual(opened, [1], "one session for the whole run: no port is opened again")
        self.assertGreaterEqual(result["polls"], 1000)
        self.assertEqual(seqs, list(range(1, 41)), "every block saved once, in order")
        self.assertEqual(result["acknowledged"], 40)
        # The bound: a PLAYLIST? that showed no block is followed by a sleep of
        # the interval before the next one; only a reply with blocks is
        # followed at once (by its ACK and the next PLAYLIST?).
        events = clock.events
        polls = [index for index, event in enumerate(events) if event == ("request", "PLAYLIST?")]
        for first, second in zip(polls, polls[1:]):
            between = events[first + 1:second]
            if not any(event[1].startswith(live.PLAYLIST_ACK) for event in between if event[0] == "request"):
                self.assertEqual(between, [("sleep", 1.0)], "an empty poll waits one interval before the next")
        sleeps = [event for event in events if event[0] == "sleep"]
        acks = [event for event in events if event[0] == "request" and event[1].startswith(live.PLAYLIST_ACK)]
        self.assertEqual(len(sleeps), result["polls"] - len(acks) - 1, "one sleep after each empty poll but the last")
        self.assertGreaterEqual(clock.now - 100.0, len(sleeps), "so the run took at least a second per empty poll")

    def test_a_lost_link_without_a_reset_goes_on_and_saves_no_block_twice(self):
        clock = FakeClock()
        board = DrainBoard(clock, 6, running_polls=40, every=10)
        journal = io.StringIO()
        controller, connect, opened = self.session(board, clock, absent=2, journal=journal)
        # The link goes on the ACK of the first block: it was saved, but the
        # board never freed it, so the next session is shown it again.
        first_ack = None
        original = board.handle

        def handle(sequence, text):
            nonlocal first_ack
            if text.startswith(live.PLAYLIST_ACK) and first_ack is None:
                first_ack = sequence
                board.fail_at.add(len(board.requests) + 1)
            return original(sequence, text)

        controller.transport.handler = handle
        with tempfile.TemporaryDirectory() as folder:
            blocks = Path(folder) / "blocks.jsonl"
            result = live.drain_playlist(controller, blocks, until_done=True, interval=1.0, sleep=clock.sleep, clock=clock, reconnect=connect)
            seqs = [json.loads(line)["block"]["seq"] for line in blocks.read_text().splitlines()]
        self.assertEqual((result["ended"], result["sessions"]), ("done", 2))
        self.assertEqual(seqs, list(range(1, 7)), "the block shown again is not saved twice, and none is lost")
        self.assertEqual(len(result["links"]), 1)
        link = result["links"][0]
        self.assertEqual((link["restored"], link["board_reset"], link["rules"], link["reconnect_attempts"]), (True, False, [], 3),
                         "two attempts found no port, the third a board that ran on")
        self.assertEqual(len(opened), 4)
        records = [json.loads(line) for line in journal.getvalue().splitlines()]
        sequences = [record["seq"] for record in records if record.get("record") == "command"]
        self.assertEqual(sequences, sorted(set(sequences)), "the new session numbers on: the journal's sequences keep increasing")
        self.assertEqual([record["event"] for record in records if record.get("record") == "link"], ["lost", "restored"])
        self.assertEqual(sum(1 for record in records if record.get("record") == "identity"), 2)

    def test_reconnect_attempts_wait_an_interval_each_and_give_up_at_the_timeout(self):
        clock = FakeClock()
        board = DrainBoard(clock, 3, running_polls=20, every=50)
        controller, connect, opened = self.session(board, clock, absent=1000)
        board.fail_at.add(5)
        with tempfile.TemporaryDirectory() as folder:
            result = live.drain_playlist(controller, Path(folder) / "blocks.jsonl", until_done=True, interval=2.0, sleep=clock.sleep, clock=clock,
                                         reconnect=connect, reconnect_timeout=29.0)
        self.assertEqual(result["ended"], "link_lost")
        link = result["links"][0]
        self.assertEqual((link["restored"], link["reconnect_attempts"]), (False, 15), "one attempt per 2 s until 29 s have passed")
        self.assertEqual(len(opened), 1 + 15)
        after = clock.events[clock.events.index(("lost", "PLAYLIST?")) + 1:]
        self.assertEqual(after, [("sleep", 2.0)] * 15, "every attempt is preceded by an interval")
        self.assertTrue(controller.transport.closed, "the lost session's port is closed before the next is opened")

    def test_a_board_reset_ends_the_drain_and_names_the_lost_blocks(self):
        for with_tick, rules in ((True, ["tick", "sequence", "idle"]), (False, ["sequence", "idle"])):
            with self.subTest(with_tick=with_tick):
                clock = FakeClock()
                # Seven blocks pending at once (made between two polls), so the
                # drain sees three, saves and acknowledges them, and loses the link.
                board = DrainBoard(clock, 12, running_polls=30, every=1000, with_tick=with_tick)
                controller, connect, opened = self.session(board, clock)
                clock.now += 600.0
                board._make(7)
                original = board.handle

                def handle(sequence, text, board=board, original=original):
                    reply = original(sequence, text)
                    if text.startswith(live.PLAYLIST_ACK):
                        board.reset()
                        board.fail_at.add(len(board.requests) + 1)
                    return reply

                controller.transport.handler = handle
                with tempfile.TemporaryDirectory() as folder:
                    blocks = Path(folder) / "blocks.jsonl"
                    result = live.drain_playlist(controller, blocks, until_done=True, interval=1.0, sleep=clock.sleep, clock=clock, reconnect=connect)
                    seqs = [json.loads(line)["block"]["seq"] for line in blocks.read_text().splitlines()]
                self.assertEqual(result["ended"], "board_reset")
                self.assertEqual(seqs, [1, 2, 3], "the blocks shown before the reset were saved")
                link = result["links"][0]
                self.assertEqual((link["board_reset"], link["rules"], link["lost_seqs"], link["playlist_running"]), (True, rules, [4, 7], True),
                                 "the ring held 4..7 unseen, and the playlist was still running")
                self.assertEqual(link["reply_after"]["state"], "idle")

    def test_board_reset_rules(self):
        last = {"state": "running", "run": 2, "next_seq": 40, "tick": 4_000_000}
        self.assertEqual(live.board_reset(last, {**last, "tick": 4_030_100}, 30.0), [], "the tick ran on with the host's time")
        self.assertEqual(live.board_reset(last, {**last, "tick": 25_000}, 30.0), ["tick"], "a reset restarted it")
        self.assertEqual(live.board_reset(last, {**last, "tick": 4_000_000 + 10_000}, 30.0), ["tick"], "too little: it restarted and ran 10 s")
        wrapping = {**last, "tick": live.UINT32_MAX - 1_000}
        self.assertEqual(live.board_reset(wrapping, {**last, "tick": 29_000}, 30.0), [], "the 49.7-day wrap is not a reset")
        old = {key: value for key, value in last.items() if key != "tick"}
        self.assertEqual(live.board_reset(old, old, 30.0), [], "an image without the tick: the run and sequence ran on")
        self.assertEqual(live.board_reset(old, {**old, "run": 1, "next_seq": 90}, 30.0), ["sequence"])
        self.assertEqual(live.board_reset(old, {**old, "next_seq": 12}, 30.0), ["sequence"])
        self.assertEqual(live.board_reset(old, {**old, "state": "idle", "run": 0, "next_seq": 1}, 30.0), ["sequence", "idle"])

    def test_the_tick_rule_allows_its_slack_and_one_percent_of_the_gap(self):
        last = {"state": "running", "run": 2, "next_seq": 40, "tick": 4_000_000}

        def advanced(milliseconds):
            return {**last, "tick": (last["tick"] + milliseconds) & live.UINT32_MAX}

        # The slack alone: no time passed on the host, so 2000 ms either way is the bound.
        self.assertEqual(live.board_reset(last, advanced(2000), 0.0), [], "the slack is inclusive")
        self.assertEqual(live.board_reset(last, advanced(2001), 0.0), ["tick"])
        self.assertEqual(live.board_reset(last, last, 0.0), [])
        self.assertEqual(live.board_reset(last, advanced(-1), 0.0), ["tick"], "a tick 1 ms behind wrapped: it restarted")
        self.assertEqual(live.board_reset(last, advanced(1500), -5.0), [], "a host clock that went back counts as no time")
        # A 1000 s gap: 0.5 % drift between the two clocks is 5 s, beyond the slack
        # but within 2000 ms + 1 % of 1 000 000 ms = 12 000 ms.
        for drift_ms in (5_000, -5_000):
            with self.subTest(drift_ms=drift_ms):
                self.assertEqual(live.board_reset(last, advanced(1_000_000 + drift_ms), 1000.0), [], "0.5 % drift is no reset")
        self.assertEqual(live.board_reset(last, advanced(1_012_000), 1000.0), [], "the bound itself")
        self.assertEqual(live.board_reset(last, advanced(1_012_001), 1000.0), ["tick"])
        self.assertEqual(live.board_reset(last, advanced(988_000), 1000.0), [])
        self.assertEqual(live.board_reset(last, advanced(987_999), 1000.0), ["tick"])
        self.assertEqual(live.board_reset(last, advanced(1_015_000), 1000.0), ["tick"], "1.5 % is more than the clocks drift")

    def test_idle_before_and_after_a_relink_is_no_reset(self):
        idle = {"state": "idle", "run": 0, "next_seq": 1, "pending": 0, "oldest_seq": 0, "tick": 50_000}
        self.assertEqual(live.board_reset(idle, {**idle, "tick": 80_000}, 30.0), [], "no playlist ran before or after")
        done = {**idle, "state": "done", "run": 3, "next_seq": 9}
        self.assertEqual(live.board_reset(done, {**done, "tick": 80_000}, 30.0), [])
        self.assertEqual(live.board_reset(done, {**done, "state": "idle", "tick": 80_000}, 30.0), ["idle"],
                         "a playlist's end state back to idle is a reset")

    def test_lost_blocks_start_after_the_newest_saved_and_the_acknowledged(self):
        # Blocks 1..5 were acknowledged before this drain (an earlier drain saved
        # them), so none of them is lost, though this drain never saw one.
        self.assertIsNone(live._lost_blocks({"pending": 0, "oldest_seq": 0, "next_seq": 6}, 0))
        self.assertEqual(live._lost_blocks({"pending": 4, "oldest_seq": 6, "next_seq": 10}, 0), [6, 9],
                         "the ring held 6..9 and the drain saw none of them")
        self.assertEqual(live._lost_blocks({"pending": 4, "oldest_seq": 6, "next_seq": 10}, 7), [8, 9], "the drain saved 6 and 7")
        self.assertIsNone(live._lost_blocks({"pending": 2, "oldest_seq": 8, "next_seq": 10}, 9), "every block was saved")

    def test_a_reset_after_blocks_acknowledged_earlier_names_none_of_them_lost(self):
        clock = FakeClock()
        board = DrainBoard(clock, 12, running_polls=30, every=1000)
        board._make(5)
        board.pending = []  # an earlier drain saved and acknowledged blocks 1..5
        controller, connect, _ = self.session(board, clock)
        polls = 0
        original = board.handle

        def handle(sequence, text):
            nonlocal polls
            if text == "PLAYLIST?":
                polls += 1
                if polls == 2:
                    board.reset()
                    board.fail_at.add(len(board.requests) + 1)
            return original(sequence, text)

        controller.transport.handler = handle
        with tempfile.TemporaryDirectory() as folder:
            result = live.drain_playlist(controller, Path(folder) / "blocks.jsonl", until_done=True, interval=1.0, sleep=clock.sleep,
                                         clock=clock, reconnect=connect)
        self.assertEqual((result["ended"], result["written"]), ("board_reset", 0))
        link = result["links"][0]
        self.assertEqual((link["last_reply"]["pending"], link["last_reply"]["next_seq"]), (0, 6))
        self.assertIsNone(link["lost_seqs"], "blocks 1..5 were acknowledged before the reset: none was lost")

    def test_the_new_session_numbers_on_from_the_lost_one(self):
        clock = FakeClock()
        board = DrainBoard(clock, 4, running_polls=20, every=5)
        journal = io.StringIO()
        controller, connect, opened = self.session(board, clock, journal=journal)
        board.fail_at.add(6)
        with tempfile.TemporaryDirectory() as folder:
            result = live.drain_playlist(controller, Path(folder) / "blocks.jsonl", until_done=True, interval=1.0, sleep=clock.sleep,
                                         clock=clock, reconnect=connect)
        self.assertEqual((result["ended"], result["sessions"]), ("done", 2))
        lost_sequence = board.requests[5][0]
        self.assertEqual(opened, [1, lost_sequence + 1], "the new session's first request is the lost one's number + 1")
        self.assertEqual(board.requests[6], (lost_sequence + 1, "INFO?"))
        records = [json.loads(line) for line in journal.getvalue().splitlines()]
        sequences = [record["seq"] for record in records if record.get("record") == "command"]
        self.assertEqual(sequences, list(range(1, len(sequences) + 1)), "one run of request numbers across both sessions")

    def test_another_image_after_the_relink_ends_the_drain(self):
        for field, value in (("usb_serial", "other-serial"), ("build_id", "wfg-other"), ("fw_commit", "f" * 40), (None, None)):
            with self.subTest(field=field):
                clock = FakeClock()
                board = DrainBoard(clock, 6, running_polls=40, every=10)
                board.info["fw_commit"] = "0" * 40
                controller, _, _ = self.session(board, clock)
                original = board.handle

                def handle(sequence, text, board=board, original=original, field=field, value=value):
                    if len(board.requests) + 1 in board.fail_at:
                        if field is not None:
                            board.info[field] = value  # the board came back with another image
                        else:
                            board.info["capabilities"] = ["playlist"]  # an INFO field the image check does not read
                    return original(sequence, text)

                def connect(first_sequence=1, board=board, clock=clock):
                    reconnected = live.Controller(FakeTransport(board.handle), board.info["usb_serial"], clock=clock)
                    reconnected.next_sequence = first_sequence
                    reconnected.identify()
                    return reconnected

                controller.transport.handler = handle
                board.fail_at.add(4)
                with tempfile.TemporaryDirectory() as folder:
                    result = live.drain_playlist(controller, Path(folder) / "blocks.jsonl", until_done=True, interval=1.0,
                                                 sleep=clock.sleep, clock=clock, reconnect=connect)
                link = result["links"][0]
                if field is None:
                    self.assertEqual(result["ended"], "done")
                    self.assertNotIn("image_changed", link)
                    self.assertEqual(link["rules"], [])
                else:
                    self.assertEqual(result["ended"], "board_reset", f"{field} changed: the ring is another image's")
                    self.assertTrue(link["image_changed"])
                    self.assertEqual(link["rules"], ["image"], "the board itself ran on: only the image rule holds")

    def test_a_malformed_reply_is_no_lost_link(self):
        clock = FakeClock()
        board = DrainBoard(clock, 2)
        controller, connect, opened = self.session(board, clock)
        controller.transport.handler = lambda sequence, text: b'{"seq":' + str(sequence).encode() + b',"oops"\n'
        with tempfile.TemporaryDirectory() as folder, self.assertRaises(live.UncertainCommand):
            live.drain_playlist(controller, Path(folder) / "blocks.jsonl", interval=1.0, sleep=clock.sleep, clock=clock, reconnect=connect)
        self.assertEqual(opened, [1], "it never reconnects to paper over a reply it cannot trust")

    def test_the_command_line_drain_reconnects_and_reports_a_reset(self):
        clock = FakeClock()
        for reset, code in ((False, 0), (True, 3)):
            with self.subTest(reset=reset):
                board = DrainBoard(clock, 5, running_polls=6, every=2)
                transports = []

                def serial(port, timeout, board=board, transports=transports):
                    if len(transports) == 1 and not transports[0].closed:
                        raise OSError("access denied: the lost session still holds the port")
                    transport = FakeTransport(board.handle)
                    transports.append(transport)
                    return transport

                board.fail_at.add(4)
                if reset:
                    original = board.handle

                    def handle(sequence, text, board=board, original=original):
                        if len(board.requests) + 1 in board.fail_at:
                            board.reset()
                        return original(sequence, text)

                    board.handle = handle
                stdout, stderr = io.StringIO(), io.StringIO()
                with tempfile.TemporaryDirectory() as folder, mock.patch.object(live, "SerialTransport", serial), \
                        mock.patch("sys.stdout", stdout), mock.patch("sys.stderr", stderr):
                    code_seen = live.main(["--port", "COMX", "--expected-serial", "fixture-serial", "--record", str(Path(folder) / "drain.jsonl"),
                                           "playlist", "drain", "--blocks", str(Path(folder) / "blocks.jsonl"), "--until-done",
                                           "--interval", "0.01"])
                    records = [json.loads(line) for line in (Path(folder) / "drain.jsonl").read_text().splitlines()]
                result = json.loads(stdout.getvalue().splitlines()[0])
                sequences = [record["seq"] for record in records if record.get("record") == "command"]
                self.assertEqual(sequences, list(range(1, len(sequences) + 1)), "open_session numbers the new session on from the lost one")
                self.assertEqual(code_seen, code, stderr.getvalue())
                self.assertEqual((result["sessions"], len(transports)), (2, 2))
                self.assertTrue(all(transport.closed for transport in transports), "both sessions' ports are closed at the end")
                self.assertEqual(result["ended"], "board_reset" if reset else "done")


if __name__ == "__main__":
    unittest.main()
