"""Transport and replay regressions; no serial dependency or hardware access."""

import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
import live_control as live


IDENTITY = {"kind": "waveform-player", "board": "MIMXRT1170-EVK", "sample_rate_hz": 48000,
            "usb_serial": "fixture-serial", "source_manifest_sha256": "a" * 64}
RUN_ID = "0123456789abcdef" * 2


def response(sequence, command, *, ok=True, run_id=None, data=None):
    field = live.QUERIES.get(command, "result") if ok else "error"
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

    def default_response(self, sequence, text):
        command = live.parse_command(text)
        if text == "INFO?":
            return response(sequence, text, run_id=self.run_id, data=dict(IDENTITY))
        if text in ("STATUS?", "COUNTERS?"):
            return response(sequence, text, run_id=self.run_id,
                            data={"live_frame": self.frame, "live_seed": 23, "live_reference_rms": 0.25})
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
        for text in ("INFO?", "STATUS?", "COUNTERS?", "LOAD:C600L.WAV", "PLAY", f"PLAY:{RUN_ID}",
                     "STOP", "MEDIA HOST", "MEDIA LOCAL", "SEED:18446744073709551615", "REFERENCE:0.125",
                     "GENERATE:600:long:INPUT.BIN:OUT.WAV", "ENCODE M110B 600:long INPUT.BIN OUT.WAV",
                     "ENCODE FUTURE custom:profile INPUT.BIN OUT.WAV",
                     "CW ON", "CW OFF", "CW FREQ 1800", "CW CI -3", "STATIC ON", "STATIC OFF",
                     "STATIC RATE 1", "STATIC PEAK 20", "FADE NOW 18 250", "AT:42 CW CI 3"):
            with self.subTest(text=text):
                self.assertEqual(live.parse_command(text).text, text)
        sweep = live.parse_command("AT:48000 SWEEP CW FREQ 300 3400 16 250")
        self.assertEqual(sweep.events, 16)
        self.assertEqual(sweep.event_frames(48000), [48000 + 12000 * index for index in range(16)])
        self.assertEqual(live.parse_command("SWEEP FADE 6 30 5 1000 250").events, 5)

    def test_invalid_commands_fail_before_serial(self):
        for text in ("CW ON\nPLAY", "cw on", "CW  ON", " LOAD:X.WAV", "LOAD:../X.WAV", "LOAD:tool.exe",
                     "M110:600:long:X.BIN", "AT:01 CW ON", "AT:1 STOP", "SEED:-1", "SEED:18446744073709551616",
                     "GENERATE:700:long:X.BIN:Y.WAV", "GENERATE:600:long:../X.BIN:Y.WAV",
                     "AT:0 GENERATE:600:long:X.BIN:Y.WAV", "AT:0 ENCODE M110B 600:long X.BIN Y.WAV",
                     "ENCODE M110B bad\\profile X.BIN Y.WAV", 'ENCODE M110B bad"profile X.BIN Y.WAV',
                     "REFERENCE:nan", "REFERENCE:0", "CW FREQ 24000", "CW FREQ 0", "CW CI 121",
                     "STATIC RATE 0", "STATIC PEAK 61", "FADE NOW 121 250", "FADE NOW 6 0.0001",
                     "SWEEP CW CI 20 -3 0 100", "SWEEP FADE 6 30 17 1000 250", "CW ON" + " " * 190,
                     "AT:18446744073709551615 SWEEP CW CI 20 -3 2 1"):
            with self.subTest(text=text):
                with self.assertRaises(live.ControlError):
                    live.parse_command(text)

    def test_json_rejects_duplicates_nonfinite_and_excessive_depth(self):
        for text in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":1e999}', '[' * 18 + '0' + ']' * 18):
            with self.subTest(text=text):
                with self.assertRaises(live.ProtocolError):
                    live.strict_json(text)


class ControllerTests(unittest.TestCase):
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
        plan["identity"]["source_manifest_sha256"] = "b" * 64
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


if __name__ == "__main__":
    unittest.main()
