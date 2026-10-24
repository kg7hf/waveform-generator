# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Offline PREPAREPLAY journal summarizer checks; no serial or device access."""

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
import live_control as live
import summarize_prepare_play as summary


SERIAL = "fixture-serial"
COMMIT = "a" * 40
SOURCE_HASH = "b" * 64
OPERATION_ID = 17
CAPABILITY = live.PREPARE_PLAY_CAPABILITY
PHASES = live.PREPARE_TIMING_PHASES


def _info(**changes):
    value = {
        "kind": "waveform-player", "board": "MIMXRT1170-EVK", "sample_rate_hz": 48000,
        "usb_serial": SERIAL, "build_id": "wfg-test-build", "fw_commit": COMMIT,
        "fw_dirty": 1,
        "capabilities": [CAPABILITY],
    }
    value.update(changes)
    return value


def _stats(*, calls=0, cycles=0, maximum=0, rejected=0, saturated=False):
    return {"calls": calls, "total_cycles": cycles, "max_cycles": maximum,
            "rejected": rejected, "saturated": saturated}


def _timing(*, zero_clock=False, rejected=False, saturated=False):
    phases = {name: _stats() for name in PHASES}
    if zero_clock:
        pass
    elif rejected:
        phases["render"] = _stats(calls=1, rejected=1)
    elif saturated:
        phases["render"] = _stats(calls=2, cycles=2**64 - 1, maximum=2**32 - 1, saturated=True)
    else:
        phases["setup"] = _stats(calls=1, cycles=30, maximum=30)
        phases["render"] = _stats(calls=2, cycles=150, maximum=100)
        phases["cleanup"] = _stats(calls=1, cycles=20, maximum=20)
    return {"cycle_hz": 0 if zero_clock else 1_000_000,
            "tick_hz": 0 if zero_clock else 1000, "phases": phases}


def _snapshot(*, state=3, active=True, operation_id=OPERATION_ID, status=0, reason="ok",
              complete=False, timing=None):
    total = 4096
    produced = written = committed = queued = consumed = 0
    if complete:
        produced = written = committed = queued = consumed = total
    elif state != 0:
        produced, written, committed, queued, consumed = 700, 650, 600, 500, 450
    return {
        "active": active, "state": state, "rate": 48000, "margin_frames": 2048,
        "total_frames": total, "required_lead_frames": 2048,
        "produced_frames": produced, "written_frames": written, "committed_frames": committed,
        "queued_frames": queued, "consumed_frames": consumed,
        "started": state in (3, 4, 5, 6), "artifact_finalized": state == 4,
        "action_pending": active, "owner_released": not active,
        "operation_id": operation_id, "timing": timing or _timing(),
        "status": status, "reason": reason,
    }


def _response(sequence, command, data, *, ok=True):
    field = live.QUERIES.get(live._query_command(command), "result") if ok else "error"
    payload = data if ok else {"code": "BUSY", "message": "fixture busy"}
    return {"seq": sequence, "protocol": live.PROTOCOL, "ok": ok,
            "state": 1, "run_id": None, field: payload}


def _command(sequence, text, data, *, ok=True):
    return {"schema": "wfg-live.session/1", "record": "command", "seq": sequence,
            "command": text, "sent_utc": "2026-10-12T11:00:00+00:00",
            "response": _response(sequence, text, data, ok=ok)}


def _records(*, snapshots=None, info=None):
    pinned = info or _info()
    records = [_command(1, "INFO?", pinned)]
    records.append({"schema": "wfg-live.session/1", "record": "identity", "protocol": live.PROTOCOL,
                    "observed_utc": "2026-10-12T11:00:00+00:00", "info": pinned})
    for sequence, snapshot in enumerate(snapshots or [_snapshot(complete=True, active=False, state=4)], start=2):
        records.append(_command(sequence, "PREPAREPLAY?", snapshot))
    return records


def _write_journal(path: Path, records):
    path.write_bytes(b"".join(json.dumps(record, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"
                            for record in records))


class SummarizePreparePlayTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "session.jsonl"

    def tearDown(self):
        self.directory.cleanup()

    def summarize(self, records):
        _write_journal(self.path, records)
        return summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                                 operation_id=OPERATION_ID)

    def test_successfully_summarizes_valid_active_then_terminal_snapshots(self):
        active = _snapshot()
        terminal = _snapshot(complete=True, active=False, state=4)
        report = self.summarize(_records(snapshots=[active, terminal]))
        self.assertEqual(report["schema"], summary.SCHEMA)
        self.assertTrue(report["operation_complete"])
        self.assertTrue(report["timing_complete"])
        self.assertTrue(report["outcome"]["successful"])
        self.assertEqual(report["snapshot_count"], 2)
        self.assertEqual(report["identity"]["fw_dirty"], 1)
        self.assertEqual(report["identity"]["build_id"], "wfg-test-build")
        self.assertEqual(report["timing"]["phases"]["render"]["accepted_calls"], 2)
        self.assertAlmostEqual(report["timing"]["phases"]["render"]["accepted_elapsed_seconds"], 0.00015)
        self.assertEqual(report["journal_sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_rt1064_summary_requires_explicit_board_selection(self):
        records = _records(info=_info(board="MIMXRT1064-EVK"))
        with self.assertRaises(summary.SummaryError):
            self.summarize(records)
        report = summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                                   operation_id=OPERATION_ID,
                                   expected_board="MIMXRT1064-EVK")
        self.assertEqual(report["identity"]["board"], "MIMXRT1064-EVK")
        self.assertTrue(report["outcome"]["successful"])

    def test_single_terminal_snapshot_is_a_valid_report(self):
        report = self.summarize(_records())
        self.assertTrue(report["outcome"]["successful"])
        repeated = self.summarize(_records(snapshots=[
            _snapshot(complete=True, active=False, state=4),
            _snapshot(complete=True, active=False, state=4),
        ]))
        self.assertEqual(repeated["snapshot_count"], 2)

    def test_valid_but_active_or_failed_operation_is_not_clean(self):
        active = self.summarize(_records(snapshots=[_snapshot()]))
        self.assertFalse(active["operation_complete"])
        self.assertFalse(active["outcome"]["successful"])
        failed = _snapshot(complete=True, active=False, state=5, status=8, reason="read failed")
        report = self.summarize(_records(snapshots=[failed]))
        self.assertTrue(report["operation_complete"])
        self.assertFalse(report["outcome"]["successful"])

    def test_rejected_or_saturated_timing_is_valid_but_incomplete(self):
        for timing in (_timing(rejected=True), _timing(saturated=True)):
            with self.subTest(timing=timing):
                terminal = _snapshot(complete=True, active=False, state=4, timing=timing)
                report = self.summarize(_records(snapshots=[terminal]))
                self.assertFalse(report["timing_complete"])
                if timing["phases"]["render"]["saturated"]:
                    self.assertIsNone(report["timing"]["phases"]["render"]["accepted_calls"])
                    self.assertIsNone(report["timing"]["phases"]["render"]["maximum_elapsed_seconds"])

    def test_zero_clock_timing_has_no_derived_durations(self):
        terminal = _snapshot(complete=True, active=False, state=4, timing=_timing(zero_clock=True))
        report = self.summarize(_records(snapshots=[terminal]))
        self.assertFalse(report["timing_complete"])
        self.assertIsNone(report["timing"]["phases"]["setup"]["accepted_elapsed_seconds"])

    def test_identity_must_match_pins_and_preceding_info(self):
        for changed in ({"fw_commit": "c" * 40}, {"usb_serial": "other"}, {"capabilities": []}):
            with self.subTest(changed=changed):
                records = _records(info=_info(**changed))
                _write_journal(self.path, records)
                with self.assertRaises(summary.SummaryError):
                    summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                                      operation_id=OPERATION_ID)
        records = _records()
        records[1]["info"] = _info(build_id="different")
        _write_journal(self.path, records)
        with self.assertRaisesRegex(summary.SummaryError, "preceding"):
            summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                              operation_id=OPERATION_ID)

    def test_immutable_info_fields_may_not_change_after_identity(self):
        records = _records()
        records.append(_command(3, "INFO?", _info(fw_dirty=0)))
        _write_journal(self.path, records)
        with self.assertRaisesRegex(summary.SummaryError, "immutable INFO"):
            summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                              operation_id=OPERATION_ID)

    def test_mutable_info_and_all_dirty_markers_are_preserved(self):
        for dirty in (0, 1, 2, 3):
            with self.subTest(dirty=dirty):
                records = _records(info=_info(fw_dirty=dirty, media="local"))
                records.append(_command(3, "INFO?", _info(fw_dirty=dirty, media="host")))
                report = self.summarize(records)
                self.assertEqual(report["identity"]["fw_dirty"], dirty)
                self.assertTrue(report["timing_complete"])

    def test_identity_requires_adjacent_info_and_complete_sequence_prefix(self):
        records = _records()
        records.insert(1, _command(2, "INFO?", {}, ok=False))
        records[-1]["seq"] = records[-1]["response"]["seq"] = 3
        with self.assertRaisesRegex(summary.SummaryError, "preceding successful INFO"):
            self.summarize(records)
        records = _records()
        records[0]["seq"] = records[0]["response"]["seq"] = 2
        with self.assertRaisesRegex(summary.SummaryError, "begin at one"):
            self.summarize(records)
        records = _records()
        records[-1]["seq"] = records[-1]["response"]["seq"] = 3
        with self.assertRaisesRegex(summary.SummaryError, "sequence gap"):
            self.summarize(records)

    def test_repeated_terminal_poll_is_not_added_twice(self):
        terminal = _snapshot(complete=True, active=False, state=4)
        report = self.summarize(_records(snapshots=[terminal, terminal]))
        self.assertEqual(report["snapshot_count"], 2)
        self.assertEqual(report["timing"]["phases"]["render"]["total_cycles"], 150)
        self.assertAlmostEqual(report["timing"]["phases"]["render"]["mean_elapsed_seconds"], 0.000075)
        self.assertAlmostEqual(report["timing"]["phases"]["render"]["maximum_elapsed_seconds"], 0.0001)

    def test_rejected_calls_cannot_add_elapsed_cycles(self):
        first = _snapshot()
        second = copy.deepcopy(first)
        stats = second["timing"]["phases"]["render"]
        stats.update(calls=3, rejected=1, total_cycles=200, max_cycles=120)
        with self.assertRaisesRegex(summary.SummaryError, "cycles exceed its new accepted calls"):
            self.summarize(_records(snapshots=[first, second]))
        stats.update(total_cycles=150, max_cycles=100)
        report = self.summarize(_records(snapshots=[first, second]))
        self.assertFalse(report["timing_complete"])
        self.assertEqual(report["timing"]["phases"]["render"]["accepted_calls"], 2)

    def test_single_snapshot_cannot_report_mean_above_maximum(self):
        terminal = _snapshot(complete=True, active=False, state=4)
        terminal["timing"]["phases"]["render"]["calls"] = 1
        with self.assertRaisesRegex(summary.SummaryError, "total exceeds accepted calls times maximum"):
            self.summarize(_records(snapshots=[terminal]))

    def test_accepted_call_count_and_saturation_flag_cannot_regress(self):
        first = _snapshot()
        second = copy.deepcopy(first)
        second["timing"]["phases"]["render"]["rejected"] = 1
        with self.assertRaisesRegex(summary.SummaryError, "accepted_calls moved backwards"):
            self.summarize(_records(snapshots=[first, second]))
        first["timing"]["phases"]["render"]["saturated"] = True
        with self.assertRaisesRegex(summary.SummaryError, "saturated cleared"):
            self.summarize(_records(snapshots=[first, _snapshot()]))

    def test_saturated_call_counts_are_not_exact_accepted_counts(self):
        first = _snapshot()
        first["timing"]["phases"]["render"].update(calls=live.UINT64_MAX, rejected=1, saturated=True)
        second = copy.deepcopy(first)
        second["timing"]["phases"]["render"]["rejected"] = 2
        report = self.summarize(_records(snapshots=[first, second]))
        stats = report["timing"]["phases"]["render"]
        for field in ("accepted_calls", "accepted_elapsed_seconds", "mean_elapsed_seconds", "maximum_elapsed_seconds"):
            self.assertIsNone(stats[field])
        self.assertFalse(report["timing_complete"])

    def test_record_and_total_byte_limits_are_enforced(self):
        for limit, value, message in (("MAX_RECORDS", 2, "record limit"),
                                       ("MAX_JOURNAL_BYTES", 1, "size limit")):
            with self.subTest(limit=limit), mock.patch.object(summary, limit, value):
                with self.assertRaisesRegex(summary.SummaryError, message):
                    self.summarize(_records())

    def test_malformed_numeric_unicode_and_nested_json_are_refused(self):
        for payload in (b'{"value":NaN}\n', b'{"value":1e999}\n', b'{"value":"\xff"}\n',
                        b'[' * 1500 + b'0' + b']' * 1500 + b'\n'):
            with self.subTest(payload=payload[:30]):
                self.path.write_bytes(payload)
                with self.assertRaisesRegex(summary.SummaryError, "invalid"):
                    summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                                      operation_id=OPERATION_ID)

    def test_rejected_command_keeps_timing_quality_separate(self):
        records = _records()
        records.append(_command(3, "INFO?", {}, ok=False))
        report = self.summarize(records)
        self.assertTrue(report["timing_complete"])
        self.assertTrue(report["outcome"]["successful"])
        self.assertEqual(report["command_rejections"], 1)

    def test_uncertain_duplicate_identity_and_other_operation_are_rejected(self):
        records = _records()
        uncertain = dict(records[-1], uncertain=True, error="lost reply")
        for fixture in ([*records[:-1], uncertain], [*records[:2], records[1], records[2]],
                        _records(snapshots=[_snapshot(complete=True, active=False, state=4, operation_id=18)])):
            with self.subTest(fixture=fixture):
                _write_journal(self.path, fixture)
                with self.assertRaises(summary.SummaryError):
                    summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                                      operation_id=OPERATION_ID)

    def test_nonmonotonic_counters_watermarks_clocks_and_terminal_resume_are_rejected(self):
        first = _snapshot()
        second = _snapshot()
        second["produced_frames"] = first["produced_frames"] - 1
        lower_calls = _snapshot()
        lower_calls["timing"]["phases"]["render"]["calls"] = 1
        lower_cycles = _snapshot()
        lower_cycles["timing"]["phases"]["render"]["total_cycles"] = 149
        changed_clock = _snapshot()
        changed_clock["timing"]["cycle_hz"] += 1
        changed_rate = _snapshot()
        changed_rate["rate"] = 44100
        for changed in (second, lower_calls, lower_cycles, changed_clock, changed_rate):
            _write_journal(self.path, _records(snapshots=[first, changed]))
            with self.assertRaises(summary.SummaryError):
                summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                                  operation_id=OPERATION_ID)
        first = _snapshot(complete=True, active=False, state=4)
        second = _snapshot()
        _write_journal(self.path, _records(snapshots=[first, second]))
        with self.assertRaisesRegex(summary.SummaryError, "terminal owner-released"):
            summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                              operation_id=OPERATION_ID)
        failed = _snapshot(complete=True, active=False, state=5, status=8, reason="read failed")
        completed = _snapshot(complete=True, active=False, state=4)
        _write_journal(self.path, _records(snapshots=[failed, completed]))
        with self.assertRaisesRegex(summary.SummaryError, "terminal owner-released"):
            summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                              operation_id=OPERATION_ID)

    def test_sequence_errors_and_missing_matching_poll_are_rejected(self):
        records = _records()
        records[-1]["seq"] = 1
        records[-1]["response"]["seq"] = 1
        _write_journal(self.path, records)
        with self.assertRaises(summary.SummaryError):
            summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                              operation_id=OPERATION_ID)
        records = _records()
        idle = _snapshot(state=0, active=False, operation_id=0, timing=_timing(zero_clock=True))
        for field in ("rate", "margin_frames", "total_frames", "required_lead_frames", "produced_frames",
                      "written_frames", "committed_frames", "queued_frames", "consumed_frames"):
            idle[field] = 0
        idle["started"] = False
        idle["action_pending"] = False
        idle["artifact_finalized"] = False
        records[-1] = _command(2, "PREPAREPLAY?", idle)
        _write_journal(self.path, records)
        with self.assertRaisesRegex(summary.SummaryError, "no poll"):
            summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                              operation_id=OPERATION_ID)
        records = _records()
        records[-1]["response"]["protocol"] = "OTHER/1"
        _write_journal(self.path, records)
        with self.assertRaisesRegex(summary.SummaryError, "invalid"):
            summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                              operation_id=OPERATION_ID)

    def test_malformed_duplicate_key_truncated_and_oversized_records_reject(self):
        records = _records()
        _write_journal(self.path, records)
        original = self.path.read_bytes()
        duplicate = original.replace(b'"record":"command"', b'"record":"command","record":"command"', 1)
        for payload in (duplicate, original[:-1], b" " * (summary.MAX_LINE_BYTES + 1) + b"\n"):
            self.path.write_bytes(payload)
            with self.subTest(length=len(payload)):
                with self.assertRaises(summary.SummaryError):
                    summary.summarize(self.path, expected_serial=SERIAL, firmware_commit=COMMIT,
                                      operation_id=OPERATION_ID)

    def test_cli_exit_codes_and_read_only_input(self):
        records = _records()
        _write_journal(self.path, records)
        before = self.path.read_bytes()
        args = [str(self.path), "--expected-serial", SERIAL, "--firmware-commit", COMMIT,
"--operation-id", str(OPERATION_ID)]
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(summary.main(args), 0)
        self.assertEqual(json.loads(stdout.getvalue())["schema"], summary.SCHEMA)
        self.assertEqual(self.path.read_bytes(), before)
        incomplete = _snapshot(complete=True, active=False, state=4, timing=_timing(rejected=True))
        _write_journal(self.path, _records(snapshots=[incomplete]))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(summary.main(args), 1)
        bad_args = [*args[:-1], "0"]
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                summary.main(bad_args)
        self.path.write_text("not json\n", encoding="utf-8")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(summary.main(args), 2)


if __name__ == "__main__":
    unittest.main()
