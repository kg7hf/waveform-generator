#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Bounded WFG-LIVE/1 engineering controller and acknowledged-frame replay.

Physical use requires pyserial, an explicit port, and the expected fixture USB serial.
No board is opened when importing this module or exporting a replay plan.

Examples (replace the port and serial with the actual device identity):
  python host/live_control.py --port COM42 --expected-serial SERIAL --record run.jsonl command 'STATUS?'
  python host/live_control.py --port COM42 --expected-serial SERIAL --record run.jsonl interactive
  python host/live_control.py export run.jsonl replay.json
  python host/live_control.py --port COM42 --expected-serial SERIAL --record replay-run.jsonl replay replay.json
  python host/live_control.py --port COM42 --expected-serial SERIAL --record tx.jsonl command 'GENERATE:600:long:INPUT.BIN:OUT.WAV'

Requests are never retried after an uncertain response. Reconnect explicitly,
inspect INFO/STATUS, and retain the failed journal. Replay uses acknowledged
output-frame positions, not wall-clock arrival times or DAC audibility times.
The caller retains the selected WAV/SCN files alongside the journal; a
filename alone is not proof that the source or scenario bytes are unchanged.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import sys
import time
from typing import Protocol, TextIO
import uuid


PROTOCOL = "WFG-LIVE/1"
REQUEST_LIMIT = 192
RESPONSE_LIMIT = 4096
PENDING_LIMIT = 64
CAPTURE_LIMIT = 128
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
QUERIES = {"INFO?": "info", "STATUS?": "status", "COUNTERS?": "counters"}
NUMBER = r"-?[0-9]+(?:\.[0-9]+)?"


class ControlError(RuntimeError):
    """A command or artifact does not satisfy the engineering control contract."""


class ProtocolError(ControlError):
    pass


class UncertainCommand(ControlError):
    """A transmitted command has no trustworthy acknowledgment; never retry it."""


class CommandRejected(ControlError):
    def __init__(self, response: dict):
        self.response = response
        error = response["error"]
        super().__init__(f"{error['code']}: {error['message']}")


def _integer(value, maximum=UINT64_MAX):
    return type(value) is int and 0 <= value <= maximum


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def strict_json(text: str | bytes):
    def bad_constant(value):
        raise ProtocolError(f"nonfinite JSON number {value}")

    def finite_float(value):
        result = float(value)
        if not math.isfinite(result):
            raise ProtocolError("overflowing JSON number")
        return result

    try:
        value = json.loads(text, object_pairs_hook=_unique_object,
                           parse_constant=bad_constant, parse_float=finite_float)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ProtocolError(f"invalid JSON: {error}") from error
    count = 0
    pending = [(value, 0)]
    while pending:
        item, depth = pending.pop()
        count += 1
        if depth > 16 or count > 1024:
            raise ProtocolError("JSON exceeds structural limits")
        if isinstance(item, dict):
            pending.extend((child, depth + 1) for child in item.values())
        elif isinstance(item, list):
            pending.extend((child, depth + 1) for child in item)
    return value


def _decimal(token: str, maximum=UINT64_MAX):
    if not re.fullmatch(r"0|[1-9][0-9]*", token):
        raise ControlError("integer must use canonical unsigned decimal spelling")
    value = int(token)
    if value > maximum:
        raise ControlError("integer exceeds the protocol range")
    return value


def _number(token: str, minimum: float, maximum: float, *, positive=False):
    if not re.fullmatch(NUMBER, token) or len(token.removeprefix("-")) > 16:
        raise ControlError("invalid numeric command argument")
    value = float(token)
    if not math.isfinite(value) or value < minimum or value > maximum or (positive and value == 0):
        raise ControlError("numeric command argument is out of range")
    return value


@dataclass(frozen=True)
class Command:
    text: str
    body: str
    live: bool = False
    at_frame: int | None = None
    events: int = 0
    step_frames: int = 0

    @property
    def mutates(self):
        return self.body not in QUERIES

    def event_frames(self, first_frame):
        last = first_frame + (self.events - 1) * self.step_frames
        if not _integer(first_frame) or last > UINT64_MAX:
            raise ControlError("scheduled output frame overflows uint64")
        return [first_frame + index * self.step_frames for index in range(self.events)]


def parse_command(text: str) -> Command:
    if not isinstance(text, str) or not text or not text.isascii() or any(ord(c) < 32 or ord(c) > 126 for c in text):
        raise ControlError("command must be one printable ASCII line")
    if len((f"{UINT32_MAX} {text}\n").encode("ascii")) > REQUEST_LIMIT:
        raise ControlError("command exceeds the 192-byte wire limit")
    body = text
    at = None
    if body.startswith("AT:"):
        prefix, separator, body = body.partition(" ")
        if not separator:
            raise ControlError("AT requires a live command")
        at = _decimal(prefix[3:])
    if body in QUERIES or body in ("PLAY", "STOP", "MEDIA HOST", "MEDIA LOCAL"):
        if at is not None:
            raise ControlError("AT applies only to live impairment commands")
        return Command(text, body)
    if re.fullmatch(r"PLAY:[0-9a-f]{32}", body):
        if at is not None:
            raise ControlError("AT cannot schedule PLAY")
        return Command(text, body)
    if re.fullmatch(r"LOAD:[A-Z0-9_]{1,8}\.WAV", body):
        if at is not None:
            raise ControlError("AT cannot select a source")
        return Command(text, body)
    if re.fullmatch(r"GENERATE:(?:75|150|300|600|1200|2400|4800):(?:short|long|zero):[A-Z0-9_]{1,8}\.BIN:[A-Z0-9_]{1,8}\.WAV", body):
        if at is not None:
            raise ControlError("AT cannot schedule artifact generation")
        return Command(text, body)
    if body.startswith("ENCODE "):
        words = body.split(" ")
        if (len(words) != 5 or re.fullmatch(r"[A-Z0-9_]{1,15}", words[1]) is None or
                not 1 <= len(words[2]) <= 31 or any(char in words[2] for char in '\\"') or
                re.fullmatch(r"[A-Z0-9_]{1,8}\.BIN", words[3]) is None or
                re.fullmatch(r"[A-Z0-9_]{1,8}\.WAV", words[4]) is None or at is not None):
            raise ControlError("invalid encoder, profile, filenames or artifact schedule")
        return Command(text, body)
    if body.startswith("SEED:") or body.startswith("REFERENCE:"):
        if at is not None:
            raise ControlError("AT cannot change source configuration")
        flag, value = body.split(":", 1)
        if flag == "SEED":
            _decimal(value)
        else:
            _number(value, 0, 1, positive=True)
        return Command(text, body)
    if body in ("CW ON", "CW OFF", "STATIC ON", "STATIC OFF"):
        return Command(text, body, True, at, 1)
    tokens = body.split(" ")
    bounds = {("CW", "FREQ"): (0, 24000), ("CW", "CI"): (-120, 120),
              ("STATIC", "RATE"): (0.001, 1000), ("STATIC", "PEAK"): (-120, 60)}
    if len(tokens) == 3 and tokens[0] == "CW" and tokens[2] in ("ON", "OFF"):
        _decimal(tokens[1], 3)
        return Command(text, body, True, at, 1)
    if len(tokens) == 3 and tuple(tokens[:2]) in bounds:
        low, high = bounds[tuple(tokens[:2])]
        value = _number(tokens[2], low, high, positive=tokens[1] == "FREQ")
        if tokens[1] == "FREQ" and value >= 24000:
            raise ControlError("CW frequency must be below Nyquist")
        return Command(text, body, True, at, 1)
    if len(tokens) == 4 and tokens[0] == "CW" and tokens[2] in ("FREQ", "CI"):
        _decimal(tokens[1], 3)
        low, high = bounds[("CW", tokens[2])]
        value = _number(tokens[3], low, high, positive=tokens[2] == "FREQ")
        if tokens[2] == "FREQ" and value >= 24000:
            raise ControlError("CW frequency must be below Nyquist")
        return Command(text, body, True, at, 1)
    if len(tokens) == 4 and tokens[:2] == ["FADE", "NOW"]:
        _number(tokens[2], 0, 120)
        if _decimal(tokens[3], 3600000) < 1:
            raise ControlError("fade duration must be 1..3600000 integer milliseconds")
        return Command(text, body, True, at, 1)
    if tokens[:2] == ["SWEEP", "CW"] and len(tokens) in (7, 8):
        kind_index = 2
        if len(tokens) == 8:
            _decimal(tokens[2], 3)
            kind_index = 3
        if tokens[kind_index] not in ("FREQ", "CI"):
            raise ControlError("unsupported WFG-LIVE/1 command")
        low, high = bounds[("CW", tokens[kind_index])]
        for token in tokens[kind_index + 1:kind_index + 3]:
            value = _number(token, low, high, positive=tokens[kind_index] == "FREQ")
            if tokens[kind_index] == "FREQ" and value >= 24000:
                raise ControlError("CW frequency must be below Nyquist")
        steps_token, interval_token = tokens[kind_index + 3:kind_index + 5]
    elif tokens[:2] == ["SWEEP", "FADE"] and len(tokens) == 7:
        for token in tokens[2:4]:
            _number(token, 0, 120)
        if _decimal(tokens[6], 3600000) < 1:
            raise ControlError("fade duration must be 1..3600000 integer milliseconds")
        steps_token, interval_token = tokens[4:6]
    else:
        raise ControlError("unsupported WFG-LIVE/1 command")
    steps = _decimal(steps_token, 16)
    interval = _decimal(interval_token, 3600000)
    step_frames = interval * 48
    if steps < 2 or interval < 1:
        raise ControlError("sweep needs 2..16 steps and 1..3600000 integer milliseconds between steps")
    command = Command(text, body, True, at, steps, step_frames)
    if at is not None:
        command.event_frames(at)
    return command


class Transport(Protocol):
    def write(self, data: bytes) -> int: ...
    def read(self, count: int) -> bytes: ...
    def close(self) -> None: ...


class SerialTransport:
    def __init__(self, port: str, timeout: float):
        try:
            import serial
        except ImportError as error:
            raise ControlError("physical serial control requires the explicitly installed pyserial==3.5 dependency") from error
        self.serial = serial.Serial(port=port, baudrate=115200, bytesize=8,
                                    parity="N", stopbits=1, timeout=0.05, write_timeout=timeout)

    def write(self, data):
        return self.serial.write(data)

    def read(self, count):
        return self.serial.read(count)

    def close(self):
        self.serial.close()


class Controller:
    def __init__(self, transport: Transport, expected_serial: str, *, journal: TextIO | None = None,
                 timeout=3.0, volume_dismounted=False, clock=time.monotonic):
        if not expected_serial or not expected_serial.isascii() or len(expected_serial) > 128:
            raise ControlError("an explicit expected fixture USB serial is required")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ControlError("timeout must be finite and positive")
        self.transport = transport
        self.expected_serial = expected_serial
        self.journal = journal
        self.timeout = timeout
        self.volume_dismounted = volume_dismounted
        self.clock = clock
        self.next_sequence = 1
        self.identity = None
        self.poisoned = False
        self.in_flight = False

    def _record(self, record):
        if self.journal is not None:
            self.journal.write(json.dumps(record, ensure_ascii=True, allow_nan=False, separators=(",", ":")) + "\n")
            self.journal.flush()

    def identify(self):
        response = self.execute("INFO?")
        info = response["info"]
        if (info.get("kind") != "waveform-player" or info.get("board") != "MIMXRT1170-EVK"
                or info.get("sample_rate_hz") != 48000 or info.get("usb_serial") != self.expected_serial):
            self.poisoned = True
            raise ProtocolError("INFO identity does not match the expected original RT1170 waveform player")
        self.identity = info
        self._record({"schema": "wfg-live.session/1", "record": "identity", "protocol": PROTOCOL,
                      "observed_utc": datetime.now(timezone.utc).isoformat(), "info": info})
        return info

    def _read_response(self, deadline):
        data = bytearray()
        while self.clock() < deadline:
            chunk = self.transport.read(min(256, RESPONSE_LIMIT + 1 - len(data)))
            if not isinstance(chunk, bytes):
                raise ProtocolError("transport returned a non-byte response")
            data.extend(chunk)
            if len(data) > RESPONSE_LIMIT:
                raise ProtocolError("response exceeds 4096 wire bytes")
            if b"\n" in data:
                newline = data.index(b"\n")
                if newline != len(data) - 1:
                    raise ProtocolError("unsolicited or interleaved response bytes")
                return strict_json(bytes(data[:newline]).removesuffix(b"\r"))
        raise TimeoutError("no complete response before the deadline")

    @staticmethod
    def _validate_response(response, sequence, command):
        if not isinstance(response, dict):
            raise ProtocolError("response must be a JSON object")
        if type(response.get("seq")) is not int or response["seq"] != sequence or response.get("protocol") != PROTOCOL:
            raise ProtocolError("response sequence or protocol mismatch")
        if type(response.get("ok")) is not bool:
            raise ProtocolError("response ok must be boolean")
        state = response.get("state")
        if not (_integer(state, UINT32_MAX) or isinstance(state, str) and re.fullmatch(r"[A-Z_]{1,32}", state)):
            raise ProtocolError("invalid response state")
        run_id = response.get("run_id")
        if run_id is not None and (not isinstance(run_id, str) or not re.fullmatch(r"[0-9a-f]{32}", run_id)):
            raise ProtocolError("invalid response run_id")
        field = QUERIES.get(command.body, "result") if response["ok"] else "error"
        if set(response) != {"seq", "protocol", "ok", "state", "run_id", field} or not isinstance(response[field], dict):
            raise ProtocolError("response does not match its command envelope")
        if not response["ok"]:
            error = response["error"]
            if set(error) != {"code", "message"} or not isinstance(error["code"], str) or not re.fullmatch(r"[A-Z_]{1,40}", error["code"]):
                raise ProtocolError("invalid error response")
            if not isinstance(error["message"], str) or not error["message"].isascii() or len(error["message"]) > 160:
                raise ProtocolError("invalid diagnostic message")
        elif command.live:
            result = response["result"]
            if (result.get("accepted") is not True or not _integer(result.get("apply_frame"))
                    or not _integer(result.get("events"), 16) or result["events"] != command.events):
                raise ProtocolError("live command lacks its acknowledged apply frame and event count")
            if command.at_frame is not None and result["apply_frame"] != command.at_frame:
                raise ProtocolError("device changed an explicitly scheduled apply frame")
            command.event_frames(result["apply_frame"])
        return response

    def execute(self, text: str):
        command = parse_command(text)
        if self.poisoned:
            raise UncertainCommand("connection is untrusted; reconnect and inspect state instead of retrying")
        if self.in_flight:
            raise ControlError("only one request may be outstanding")
        if command.mutates and self.identity is None:
            raise ControlError("verify INFO identity before sending mutations")
        if command.body == "MEDIA LOCAL" and not self.volume_dismounted:
            raise ControlError("MEDIA LOCAL requires explicit confirmation that the host volume was flushed and dismounted")
        if self.next_sequence > UINT32_MAX:
            raise ControlError("sequence exhausted; start a new explicit connection")
        sequence = self.next_sequence
        self.next_sequence += 1
        wire = f"{sequence} {text}\n".encode("ascii")
        self.in_flight = True
        record = {"schema": "wfg-live.session/1", "record": "command", "seq": sequence,
                  "command": text, "sent_utc": datetime.now(timezone.utc).isoformat()}
        try:
            deadline = self.clock() + self.timeout
            written = 0
            while written < len(wire):
                if self.clock() >= deadline:
                    raise TimeoutError("request write timed out")
                count = self.transport.write(wire[written:])
                if not _integer(count, len(wire) - written) or count == 0:
                    raise ProtocolError("transport did not accept the complete request")
                written += count
            response = self._validate_response(self._read_response(deadline), sequence, command)
            record["response"] = response
            self._record(record)
        except Exception as error:
            self.poisoned = True
            record["uncertain"] = True
            record["error"] = str(error)
            self._record(record)
            raise UncertainCommand(f"sequence {sequence} has no trustworthy acknowledgment: {error}") from error
        finally:
            self.in_flight = False
        if not response["ok"]:
            raise CommandRejected(response)
        return response


def export_replay(records: list[dict]):
    identity = None
    source_commands = []
    controls = []
    run_id = None
    saw_play = False
    terminal = None
    previous_sequence = 0
    live_seed = None
    live_reference_rms = None
    for record in records:
        if not isinstance(record, dict) or record.get("schema") != "wfg-live.session/1":
            raise ControlError("unsupported session record")
        if record.get("record") == "identity":
            if identity is not None or not isinstance(record.get("info"), dict) or record.get("protocol") != PROTOCOL:
                raise ControlError("export exactly one connection and one playback at a time")
            identity = record["info"]
            continue
        if record.get("record") != "command":
            raise ControlError("unknown session record")
        if record.get("uncertain") or "response" not in record:
            raise ControlError("an uncertain command prevents deterministic replay export")
        command = parse_command(record["command"])
        response = record["response"]
        sequence = record.get("seq")
        if not _integer(sequence, UINT32_MAX) or sequence <= previous_sequence:
            raise ControlError("recorded command sequences must strictly increase")
        previous_sequence = sequence
        Controller._validate_response(response, sequence, command)
        if not response["ok"]:
            continue  # Explicit rejection has no effect; retain it in the source journal.
        if command.body.startswith("PLAY"):
            if saw_play:
                raise ControlError("export exactly one playback at a time")
            saw_play = True
            run_id = response["run_id"]
            live_seed = response["result"].get("live_seed")
            live_reference_rms = response["result"].get("live_reference_rms")
        elif command.live:
            if saw_play and response["run_id"] != run_id:
                raise ControlError("live acknowledgment belongs to a different playback")
            ack = response.get("result", {})
            if ack.get("accepted") is not True or not _integer(ack.get("apply_frame")) or ack.get("events") != command.events:
                raise ControlError("live event has no complete acknowledged frame")
            command.event_frames(ack["apply_frame"])
            controls.append({"command": command.body, "apply_frame": ack["apply_frame"], "events": command.events})
        elif command.body.startswith(("LOAD:", "SEED:", "REFERENCE:")):
            if saw_play:
                raise ControlError("source configuration changed after playback; select a single run journal")
            source_commands.append(command.body)
            if command.body.startswith("LOAD:"):
                controls.clear()  # LOAD discards the preceding selection's preloaded events.
        elif command.body in ("STATUS?", "COUNTERS?") and saw_play:
            if response["run_id"] != run_id:
                raise ControlError("snapshot belongs to a different playback")
            terminal = response[QUERIES[command.body]]
            live_seed = terminal.get("live_seed", live_seed)
            live_reference_rms = terminal.get("live_reference_rms", live_reference_rms)
    if identity is None or not saw_play or not any(command.startswith("LOAD:") for command in source_commands):
        raise ControlError("replay requires verified identity, an explicit source selection, and one acknowledged PLAY")
    if sum(item["events"] for item in controls) > CAPTURE_LIMIT:
        raise ControlError("replay exceeds the 128-event capture bound")
    if not _integer(live_seed) or type(live_reference_rms) not in (int, float) or not math.isfinite(live_reference_rms) or live_reference_rms <= 0:
        raise ControlError("record STATUS with the effective live_seed and live_reference_rms before exporting")
    plan = {"schema": "wfg-live.replay/1", "protocol": PROTOCOL, "identity": identity,
            "recorded_run_id": run_id, "source_commands": source_commands, "controls": controls,
            "live_seed": live_seed, "live_reference_rms": live_reference_rms,
            "last_snapshot": terminal,
            "scope": "engineering output-frame replay; retain and verify the selected source and scenario files separately"}
    if terminal is not None and _integer(terminal.get("live_frame")):
        plan["live_frames"] = terminal["live_frame"]
    return plan


def replay(controller: Controller, plan: dict, *, sleep=time.sleep):
    if not isinstance(plan, dict) or plan.get("schema") != "wfg-live.replay/1" or plan.get("protocol") != PROTOCOL:
        raise ControlError("unsupported replay plan")
    if controller.identity is None:
        raise ControlError("verify INFO before replay")
    if not isinstance(plan.get("identity"), dict):
        raise ControlError("replay identity must be an object")
    for field in ("usb_serial", "build_id"):
        expected = plan.get("identity", {}).get(field)
        if expected is None or controller.identity.get(field) != expected:
            raise ControlError(f"replay identity differs or lacks {field}")
    source_commands = plan.get("source_commands")
    controls = plan.get("controls")
    if not isinstance(source_commands, list) or not isinstance(controls, list):
        raise ControlError("replay source commands and controls must be arrays")
    prepared = []
    for text in source_commands:
        command = parse_command(text)
        if not command.body.startswith(("LOAD:", "SEED:", "REFERENCE:")) or command.at_frame is not None:
            raise ControlError("invalid replay source configuration")
    if not any(text.startswith("LOAD:") for text in source_commands):
        raise ControlError("replay must select its source explicitly")
    for item in controls:
        if not isinstance(item, dict) or set(item) != {"command", "apply_frame", "events"}:
            raise ControlError("invalid replay event")
        command = parse_command(item["command"])
        if not command.live or command.at_frame is not None or item["events"] != command.events or not _integer(item["apply_frame"]):
            raise ControlError("invalid replay command or acknowledged frame")
        scheduled = f"AT:{item['apply_frame']} {command.body}"
        parse_command(scheduled)
        prepared.append((scheduled, command.event_frames(item["apply_frame"])))
    if sum(len(frames) for _, frames in prepared) > CAPTURE_LIMIT:
        raise ControlError("replay exceeds the 128-event capture bound")
    # Complete all artifact validation before the first source mutation.
    for command in source_commands:
        controller.execute(command)
    pending = []
    index = 0
    while index < len(prepared) and len(pending) + len(prepared[index][1]) <= PENDING_LIMIT:
        command, frames = prepared[index]
        controller.execute(command)
        pending.extend(frames)
        index += 1
    run_id = uuid.uuid4().hex
    controller.execute(f"PLAY:{run_id}")
    last_frame = None
    progress_deadline = controller.clock() + max(controller.timeout, 10.0)
    while index < len(prepared):
        status = controller.execute("STATUS?")
        if status["run_id"] != run_id:
            raise ControlError("playback generation changed while scheduling replay")
        frame = status["status"].get("live_frame")
        if not _integer(frame):
            raise ProtocolError("STATUS lacks the live output-frame cursor")
        if status["state"] in (7, 255, "DONE", "ABORTED", "FAULT"):
            raise ControlError("playback ended before the remaining replay events could be scheduled")
        if frame != last_frame:
            last_frame = frame
            progress_deadline = controller.clock() + max(controller.timeout, 10.0)
        elif controller.clock() >= progress_deadline:
            raise ControlError("replay output cursor stopped advancing while its pending queue was full")
        pending = [event for event in pending if event >= frame]
        command, frames = prepared[index]
        if frames[0] < frame:
            raise ControlError("replay missed an acknowledged apply frame; stop and retain this failed run")
        if len(pending) + len(frames) <= PENDING_LIMIT:
            controller.execute(command)
            pending.extend(frames)
            index += 1
        else:
            sleep(0.01)
    return run_id


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="explicit serial endpoint; never a remembered COM default")
    parser.add_argument("--expected-serial", help="expected fixture USB serial, checked against INFO")
    parser.add_argument("--record", type=Path, help="new caller-owned JSONL journal (existing files are not overwritten)")
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--host-volume-dismounted", action="store_true",
                        help="operator assertion that host writes were flushed and its volume dismounted")
    actions = parser.add_subparsers(dest="action", required=True)
    commands = actions.add_parser("command", help="send one or more quoted commands on one connection")
    commands.add_argument("commands", nargs="+")
    actions.add_parser("interactive", help="one command at a time; EOF/QUIT closes without stopping playback")
    export = actions.add_parser("export", help="offline: export one recorded playback to an acknowledged-frame replay plan")
    export.add_argument("journal", type=Path)
    export.add_argument("output", type=Path)
    replay_parser = actions.add_parser("replay", help="replay the acknowledged schedule with a fresh run ID")
    replay_parser.add_argument("plan", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.action == "export":
            records = [strict_json(line) for line in args.journal.read_text(encoding="utf-8").splitlines() if line]
            plan = export_replay(records)
            with args.output.open("x", encoding="utf-8", newline="\n") as output:
                output.write(json.dumps(plan, indent=2, allow_nan=False) + "\n")
            return 0
        if not args.port or not args.expected_serial or args.record is None:
            raise ControlError("physical control requires --port, --expected-serial and a new --record file")
        if args.action == "command":
            for command in args.commands:
                parse_command(command)
        plan = strict_json(args.plan.read_bytes()) if args.action == "replay" else None
        with args.record.open("x", encoding="utf-8", newline="\n") as journal:
            transport = SerialTransport(args.port, args.timeout)
            try:
                controller = Controller(transport, args.expected_serial, journal=journal, timeout=args.timeout,
                                        volume_dismounted=args.host_volume_dismounted)
                controller.identify()
                if args.action == "command":
                    for command in args.commands:
                        print(json.dumps(controller.execute(command), allow_nan=False))
                elif args.action == "replay":
                    print(json.dumps({"run_id": replay(controller, plan), "schedule_submitted": True}))
                else:
                    while True:
                        try:
                            command = input("wfg-live> ")
                        except EOFError:
                            break
                        if command == "QUIT":
                            break
                        try:
                            print(json.dumps(controller.execute(command), allow_nan=False))
                        except CommandRejected as error:
                            print(str(error), file=sys.stderr)
                # Retain the actual live seed/reference and the latest produced
                # output position; do not infer these from command arrival times.
                if not controller.poisoned:
                    controller.execute("STATUS?")
            finally:
                transport.close()
        return 0
    except (ControlError, OSError, ValueError) as error:
        print(f"live control: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
