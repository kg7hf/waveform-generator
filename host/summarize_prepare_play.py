# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Summarize one operation from a saved PREPAREPLAY? journal.

This is a read-only offline report. It never opens a device or imports pyserial.
The caller must pin the expected fixture and firmware because operation_id is only boot-local. Timing values are elapsed
task-action measurements, not CPU utilization or a producer-rate estimate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

if __package__:
    from . import live_control
else:
    import live_control


SCHEMA = "wfg-live.prepare-play-summary/2"
MAX_LINE_BYTES = 64 * 1024
MAX_RECORDS = 100_000
MAX_JOURNAL_BYTES = 256 * 1024 * 1024
FW_COMMIT = re.compile(r"[0-9a-f]{40}")
IDENTITY_FIELDS = ("kind", "board", "sample_rate_hz", "usb_serial", "build_id", "fw_commit", "fw_dirty")
PHASES = live_control.PREPARE_TIMING_PHASES
TERMINAL_STATES = {4, 5, 6}


class SummaryError(ValueError):
    """A journal cannot be safely bound to one PREPAREPLAY operation."""


def _require_string(value: object, name: str, *, maximum: int = 256) -> str:
    if (not isinstance(value, str) or not value or not value.isascii() or len(value) > maximum or
            any(ord(character) < 32 or ord(character) == 127 for character in value)):
        raise SummaryError(f"{name} must be bounded nonempty ASCII text")
    return value


def _validate_expected(expected_serial: str, firmware_commit: str, operation_id: int) -> None:
    _require_string(expected_serial, "expected serial", maximum=128)
    if not isinstance(firmware_commit, str) or FW_COMMIT.fullmatch(firmware_commit) is None:
        raise SummaryError("firmware commit must be 40 lowercase hexadecimal characters")
    if type(operation_id) is not int or not 1 <= operation_id <= live_control.UINT64_MAX:
        raise SummaryError("operation ID must be a nonzero uint64")


def _validate_info(info: object, expected_serial: str, firmware_commit: str, expected_board: str) -> dict:
    if not isinstance(info, dict):
        raise SummaryError("INFO response must contain an object")
    if (info.get("kind") != "waveform-player" or info.get("board") != expected_board or
            type(info.get("sample_rate_hz")) is not int or info["sample_rate_hz"] != 48000 or
            info.get("usb_serial") != expected_serial):
        raise SummaryError("INFO identity differs from the pinned waveform-player fixture")
    if info.get("fw_commit") != firmware_commit:
        raise SummaryError("INFO firmware commit differs from the pinned identity")
    if (not isinstance(info.get("build_id"), str) or not info["build_id"].isascii() or
            not 1 <= len(info["build_id"]) <= 128 or type(info.get("fw_dirty")) is not int or
            info["fw_dirty"] not in (0, 1, 2, 3)):
        raise SummaryError("INFO build identity fields are malformed")
    capabilities = info.get("capabilities")
    if (not isinstance(capabilities, list) or any(not isinstance(value, str) for value in capabilities) or
            live_control.PREPARE_PLAY_CAPABILITY not in capabilities):
        raise SummaryError("INFO does not advertise PREPAREPLAY experimental capability")
    return info


def _immutable_info(info: dict) -> tuple:
    return tuple(info.get(field) for field in IDENTITY_FIELDS)


def _terminal(snapshot: dict) -> bool:
    return not snapshot["active"] and snapshot["state"] in TERMINAL_STATES and snapshot["owner_released"]


def _check_snapshot_transition(previous: dict | None, current: dict, operation_id: int) -> None:
    if current["operation_id"] != operation_id:
        raise SummaryError("PREPAREPLAY operation_id differs from the requested operation")
    if previous is None:
        return
    if _terminal(previous) and current != previous:
        raise SummaryError("PREPAREPLAY snapshot changed after its terminal owner-released state")
    if current["rate"] != previous["rate"] and previous["rate"] != 0:
        raise SummaryError("PREPAREPLAY rate changed after configuration")
    if current["margin_frames"] != previous["margin_frames"] and previous["rate"] != 0:
        raise SummaryError("PREPAREPLAY margin changed after configuration")
    if current["total_frames"] != previous["total_frames"] and previous["total_frames"] != 0:
        raise SummaryError("PREPAREPLAY total frame count changed after configuration")
    if current["required_lead_frames"] < previous["required_lead_frames"]:
        raise SummaryError("PREPAREPLAY required lead watermark moved backwards")
    for field in ("produced_frames", "written_frames", "committed_frames", "queued_frames", "consumed_frames"):
        if current[field] < previous[field]:
            raise SummaryError(f"PREPAREPLAY {field} watermark moved backwards")
    previous_timing = previous["timing"]
    timing = current["timing"]
    for clock in ("cycle_hz", "tick_hz"):
        if timing[clock] != previous_timing[clock]:
            raise SummaryError(f"PREPAREPLAY {clock} changed during one operation")
    for phase in PHASES:
        before = previous_timing["phases"][phase]
        after = timing["phases"][phase]
        for field in ("calls", "total_cycles", "max_cycles", "rejected"):
            if after[field] < before[field]:
                raise SummaryError(f"PREPAREPLAY timing {phase}.{field} moved backwards")
        if not before["saturated"] and not after["saturated"]:
            accepted_delta = (after["calls"] - after["rejected"]) - (before["calls"] - before["rejected"])
            if accepted_delta < 0:
                raise SummaryError(f"PREPAREPLAY timing {phase}.accepted_calls moved backwards")
            cycle_delta = after["total_cycles"] - before["total_cycles"]
            if cycle_delta > accepted_delta * after["max_cycles"]:
                raise SummaryError(f"PREPAREPLAY timing {phase} cycles exceed its new accepted calls")
            if after["max_cycles"] > before["max_cycles"] and (accepted_delta == 0 or cycle_delta < after["max_cycles"]):
                raise SummaryError(f"PREPAREPLAY timing {phase} maximum lacks a new accepted interval")
        if before["saturated"] and not after["saturated"]:
            raise SummaryError(f"PREPAREPLAY timing {phase}.saturated cleared")


def _read_journal(path: Path, metadata: dict):
    digest = hashlib.sha256()
    total_bytes = 0
    count = 0
    try:
        with path.open("rb") as stream:
            while True:
                line = stream.readline(MAX_LINE_BYTES + 1)
                if not line:
                    break
                if len(line) > MAX_LINE_BYTES:
                    raise SummaryError("journal record exceeds the 64 KiB line limit")
                total_bytes += len(line)
                if total_bytes > MAX_JOURNAL_BYTES:
                    raise SummaryError("journal exceeds the 256 MiB size limit")
                if not line.endswith(b"\n"):
                    raise SummaryError("journal ends with a truncated JSONL record")
                count += 1
                if count > MAX_RECORDS:
                    raise SummaryError("journal exceeds the 100000-record limit")
                digest.update(line)
                payload = line[:-1].removesuffix(b"\r")
                if not payload:
                    raise SummaryError(f"journal record {count} is empty")
                try:
                    yield count, live_control.strict_json(payload)
                except live_control.ControlError as error:
                    raise SummaryError(f"journal record {count} is invalid: {error}") from error
    except OSError as error:
        raise SummaryError(f"cannot read journal: {error}") from error
    metadata.update(digest_hex=digest.hexdigest(), total_bytes=total_bytes, record_count=count)


def _validate_record_schema(record: object, number: int) -> dict:
    if not isinstance(record, dict) or record.get("schema") != "wfg-live.session/1":
        raise SummaryError(f"journal record {number} has an unsupported schema")
    return record


def _stats_report(timing: dict) -> tuple[dict, bool]:
    cycle_hz = timing["cycle_hz"]
    tick_hz = timing["tick_hz"]
    accepted_calls_total = 0
    complete = cycle_hz > 0 and tick_hz > 0
    phases = {}
    for phase in PHASES:
        stats = timing["phases"][phase]
        accepted_calls = None if stats["saturated"] else stats["calls"] - stats["rejected"]
        if accepted_calls is not None:
            if stats["total_cycles"] > accepted_calls * stats["max_cycles"]:
                raise SummaryError(f"PREPAREPLAY timing {phase} total exceeds accepted calls times maximum")
            accepted_calls_total += accepted_calls
        if stats["rejected"] or stats["saturated"]:
            complete = False
        derivatives_valid = cycle_hz > 0 and tick_hz > 0 and not stats["saturated"]
        phases[phase] = {
            **stats,
            "accepted_calls": accepted_calls,
            "accepted_elapsed_seconds": stats["total_cycles"] / cycle_hz if derivatives_valid else None,
            "mean_elapsed_seconds": (stats["total_cycles"] / cycle_hz / accepted_calls
                                     if derivatives_valid and accepted_calls else None),
            "maximum_elapsed_seconds": (stats["max_cycles"] / cycle_hz if derivatives_valid else None),
        }
    if accepted_calls_total == 0:
        complete = False
    return {"cycle_hz": cycle_hz, "tick_hz": tick_hz, "phases": phases}, complete


def summarize(path: str | Path, *, expected_serial: str, firmware_commit: str,
              operation_id: int, expected_board: str = "MIMXRT1170-EVK") -> dict:
    """Validate and summarize one caller-pinned operation from a Controller journal."""
    if expected_board not in ("MIMXRT1170-EVK", "MIMXRT1064-EVK"):
        raise SummaryError("unsupported expected board")
    _validate_expected(expected_serial, firmware_commit, operation_id)
    info_response = None
    preceding_info_response = False
    pinned_info = None
    identity_count = 0
    previous_sequence = None
    last_snapshot = None
    snapshot_count = 0
    command_rejections = 0
    admission_count = 0
    saw_target = False
    journal_metadata = {}
    for number, record in _read_journal(Path(path), journal_metadata):
        record = _validate_record_schema(record, number)
        kind = record.get("record")
        if kind == "identity":
            if set(record) != {"schema", "record", "protocol", "observed_utc", "info"}:
                raise SummaryError(f"journal record {number} has an invalid identity shape")
            identity_count += 1
            if identity_count != 1 or record.get("protocol") != live_control.PROTOCOL:
                raise SummaryError("journal must contain exactly one supported identity record")
            _require_string(record.get("observed_utc"), "identity timestamp", maximum=64)
            if (not preceding_info_response or info_response is None or not info_response.get("ok") or
                    record["info"] != info_response.get("info")):
                raise SummaryError("identity record does not match the preceding successful INFO response")
            pinned_info = _validate_info(record["info"], expected_serial, firmware_commit, expected_board)
            preceding_info_response = False
            continue
        if kind != "command":
            raise SummaryError(f"journal record {number} is not a command or identity record")
        if "uncertain" in record or set(record) != {"schema", "record", "seq", "command", "sent_utc", "response"}:
            raise SummaryError(f"journal record {number} is uncertain or has an invalid command shape")
        _require_string(record.get("sent_utc"), f"journal record {number} timestamp", maximum=64)
        sequence = record["seq"]
        if not live_control._integer(sequence, live_control.UINT32_MAX) or sequence == 0:
            raise SummaryError(f"journal record {number} has an invalid sequence")
        if previous_sequence is None and sequence != 1:
            raise SummaryError("journal command sequence does not begin at one")
        if previous_sequence is not None and sequence <= previous_sequence:
            raise SummaryError(f"journal record {number} sequence is duplicate or reordered")
        if previous_sequence is not None and sequence != previous_sequence + 1:
            raise SummaryError(f"journal record {number} has a sequence gap")
        previous_sequence = sequence
        try:
            command = live_control.parse_command(record["command"])
            response = live_control.Controller._validate_response(record["response"], sequence, command)
        except (live_control.ControlError, KeyError, TypeError) as error:
            raise SummaryError(f"journal record {number} command response is invalid: {error}") from error
        if command.body == "INFO?" and response["ok"]:
            checked_info = _validate_info(response["info"], expected_serial, firmware_commit, expected_board)
            if pinned_info is not None and _immutable_info(checked_info) != _immutable_info(pinned_info):
                raise SummaryError("immutable INFO identity fields changed within the journal")
            info_response = response
            preceding_info_response = True
        elif command.body == "INFO?":
            info_response = response
            preceding_info_response = False
            command_rejections += 1
        else:
            preceding_info_response = False
            if not response["ok"]:
                command_rejections += 1

        if command.body.startswith("PREPAREPLAY:") and response["ok"]:
            admission_count += 1
            if admission_count > 1 or saw_target:
                raise SummaryError("journal contains a repeated PREPAREPLAY admission")
        if command.body != "PREPAREPLAY?" or not response["ok"]:
            continue
        if pinned_info is None:
            raise SummaryError("PREPAREPLAY? poll precedes the verified identity record")
        snapshot = response["prepare_play"]
        current_id = snapshot["operation_id"]
        if current_id == 0:
            if saw_target:
                raise SummaryError("PREPAREPLAY operation ID returned to zero")
            continue
        if current_id != operation_id:
            raise SummaryError("journal contains a different nonzero PREPAREPLAY operation ID")
        _check_snapshot_transition(last_snapshot, snapshot, operation_id)
        last_snapshot = snapshot
        snapshot_count += 1
        saw_target = True

    if identity_count != 1 or pinned_info is None:
        raise SummaryError("journal does not contain one verified INFO identity")
    if not saw_target or last_snapshot is None:
        raise SummaryError("journal has no poll for the requested PREPAREPLAY operation")
    timing, timing_complete = _stats_report(last_snapshot["timing"])
    operation_complete = _terminal(last_snapshot)
    successful = live_control.prepare_play_wait_exit_code({"prepare_play": last_snapshot}) == 0
    journal_sha256 = journal_metadata["digest_hex"]
    return {
        "schema": SCHEMA,
        "journal_sha256": journal_sha256,
        "journal_bytes": journal_metadata["total_bytes"],
        "record_count": journal_metadata["record_count"],
        "snapshot_count": snapshot_count,
        "operation_id": operation_id,
        "identity": {field: pinned_info[field] for field in IDENTITY_FIELDS},
        "last_snapshot": last_snapshot,
        "outcome": {"state": last_snapshot["state"], "status": last_snapshot["status"],
                    "reason": last_snapshot["reason"], "successful": successful},
        "operation_complete": operation_complete,
        "timing_complete": timing_complete,
        "command_rejections": command_rejections,
        "timing": timing,
    }


def _operation_id(token: str) -> int:
    if len(token) > len(str(live_control.UINT64_MAX)) or not re.fullmatch(r"[1-9][0-9]*", token):
        raise argparse.ArgumentTypeError("operation ID must be canonical nonzero uint64 decimal")
    value = int(token)
    if value > live_control.UINT64_MAX:
        raise argparse.ArgumentTypeError("operation ID exceeds uint64")
    return value


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path)
    parser.add_argument("--expected-serial", required=True)
    parser.add_argument("--expected-board", choices=("MIMXRT1170-EVK", "MIMXRT1064-EVK"),
                        default="MIMXRT1170-EVK")
    parser.add_argument("--firmware-commit", required=True)
    parser.add_argument("--operation-id", type=_operation_id, required=True)
    args = parser.parse_args(argv)
    try:
        report = summarize(args.journal, expected_serial=args.expected_serial, firmware_commit=args.firmware_commit,
                           operation_id=args.operation_id, expected_board=args.expected_board)
    except (SummaryError, OSError, ValueError) as error:
        print(f"prepare-play summary: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, ensure_ascii=True, allow_nan=False, separators=(",", ":")))
    return 0 if (report["outcome"]["successful"] and report["operation_complete"] and
                  report["timing_complete"] and report["command_rejections"] == 0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
