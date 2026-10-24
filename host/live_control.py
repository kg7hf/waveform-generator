#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Bounded WFG-LIVE/1 engineering controller and acknowledged-frame replay.

Physical use requires pyserial, an explicit port, and the expected fixture USB serial.
No board is opened when importing this module or exporting a replay plan.

Examples (replace the port and serial with the actual device identity):
  python host/live_control.py --port COM42 --expected-serial SERIAL --record run.jsonl command 'STATUS?'
  python host/live_control.py --port COM42 --expected-serial SERIAL --record run.jsonl interactive
  python host/live_control.py export run.jsonl replay.json
  python host/live_control.py --port COM42 --expected-serial SERIAL --record replay-run.jsonl replay replay.json
  python host/live_control.py --port COM42 --expected-serial SERIAL --record tx.jsonl command 'GENERATE:600:long:INPUT.BIN:OUT.WAV'
  python host/live_control.py --port COM42 --expected-serial SERIAL --record route.jsonl command 'OUTPUT USB' 'INFO?'
  python host/live_control.py --port COM42 --expected-serial SERIAL --record usb.jsonl command 'USB?'
  python host/live_control.py --port COM42 --expected-serial SERIAL --record cut.jsonl cut set 48000:480:zero,96000:480:drop
  python host/live_control.py --port COM42 --expected-serial SERIAL --record cut.jsonl cut clear
  python host/live_control.py --port COM42 --expected-serial SERIAL --record pl-start.jsonl playlist start BATTERY.PL [--continue]
  python host/live_control.py --port COM42 --expected-serial SERIAL --record pl-status.jsonl playlist status
  python host/live_control.py --port COM42 --expected-serial SERIAL --record pl-ack.jsonl playlist ack 6
  python host/live_control.py --port COM42 --expected-serial SERIAL --record pl-drain-01.jsonl playlist drain --blocks battery-blocks.jsonl --until-done

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
import unicodedata
from typing import Protocol, TextIO
import uuid

import cut_schedule


PROTOCOL = "WFG-LIVE/1"
# Wire bytes of one request, its LF included (INFO request_capacity): room for
# a CUT of 16 entries with 32-bit offsets (common/live_protocol.hpp).
REQUEST_LIMIT = 384
RESPONSE_LIMIT = 4096
PENDING_LIMIT = 64
CAPTURE_LIMIT = 128
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
# Read-only queries and the reply member each answers with. USB? reports the USB
# sound card's stream diagnostics and miss attribution, which STATUS? has no room for;
# PLAYLIST? the running or last playlist and its oldest result blocks; ENGINE? why
# the current or last play's scenario was refused, or its engine or live controls stopped it.
QUERIES = {"INFO?": "info", "STATUS?": "status", "COUNTERS?": "counters", "USB?": "usb", "PLAYLIST?": "playlist", "ENGINE?": "engine",
           "FILES?": "files", "PREPARE?": "prepare", "PREPAREPLAY?": "prepare_play"}
FILES_CAPABILITY = "files"
PREPARE_CAPABILITY = "prepare"
PREPARE_PLAY_CAPABILITY = "prepare-play-experimental"
PREPARE_PHASES = ("idle", "starting", "hashing_source", "hashing_secondary", "calibrating", "rendering", "verifying",
                  "publishing", "complete", "cancelled", "failed", "cleanup_failed")
FILES_PAGE_CAPACITY = 24
FILES_NAME_MAX_UTF8_BYTES = 36
# The STATUS player states in which a run has ended, by wire number (rt1170/player.hpp
# published_player_states, docs/live-control-protocol.md): done, aborted (STOP) and
# fault. A reply may also spell the state by name. tests/test_live_control.py checks
# the numbers and names against the firmware's table.
PLAYER_END_STATES = {7: "DONE", 8: "ABORTED", 255: "FAULT"}
# Output routes: the WM8960 codec (the boot default) or the UAC2 capture stream.
OUTPUTS = ("OUTPUT CODEC", "OUTPUT USB")
# The INFO "output" name of each route and the command that selects it.
ROUTE_COMMANDS = {"codec": "OUTPUT CODEC", "usb": "OUTPUT USB"}
# Stopped-player configuration that a replay re-applies, in order, before PLAY.
# CUT sets the next PLAY's schedule (CUT alone clears it); LOAD keeps it.
SETUP_PREFIXES = ("LOAD:", "SEED:", "REFERENCE:", "OUTPUT ", "CUT")
# The INFO capability of firmware with the CUT command. Its pending list
# outlives LOAD, a refused PLAY and the connection, so a replay plan for such
# firmware always names the list its PLAY took.
CUT_CAPABILITY = "cut-injection"
NUMBER = r"-?[0-9]+(?:\.[0-9]+)?"
# PLAYLIST batch mode (docs/live-control-protocol.md, "PLAYLIST"): the
# playlist's 8.3 name (its extension may be left out), the policy word, and
# the acknowledgement of result blocks.
PLAYLIST_CAPABILITY = "playlist"
PLAYLIST_START = re.compile(r"PLAYLIST:[A-Z0-9_]{1,8}(?:\.[A-Z0-9_]{1,3})?(?::CONTINUE)?")
PLAYLIST_ACK = "PLAYLIST ACK:"
PLAYLIST_STATES = ("idle", "running", "gap", "paused_ring_full", "done", "aborted", "error")
PLAYLIST_ACTIVE_STATES = ("running", "gap", "paused_ring_full")
# Legacy firmware may return three blocks; newer firmware returns two to fit
# the expanded identity fields inside the same response-byte budget.
PLAYLIST_BLOCKS_PER_REPLY = 3
# Why an item ended (common/playlist.hpp EndCause), and the block members the
# drain's summary shows for each block it saved: which item, how and why it
# ended, its frames sent and loops, and its att= and dur=. A block of an image
# before att= and dur= lacks the newer members; one that has them must spell
# them as the board does.
PLAYLIST_END_CAUSES = ("end", "stop", "underrun", "missed_interval", "stream_closed", "stall", "sd_read", "usb_assert", "deadline_miss",
                       "no_stream", "fault")
PLAYLIST_BLOCK_SUMMARY = ("seq", "item", "label", "result", "end_cause", "player_error", "uac_play_sent_frames", "loops", "att_tenths_db",
                          "dur_frames", "engine_deadline_misses", "watterson_version", "watterson_digest_hi", "watterson_digest_lo",
                          "watterson_latency_frames", "watterson_paths", "engine_digest_hi", "engine_digest_lo")
PLAYLIST_WATTERSON_IDENTITY = ("watterson_version", "watterson_digest_hi", "watterson_digest_lo", "watterson_latency_frames", "watterson_paths")
PLAYLIST_SECOND_SOURCE_UINT32 = ("second_source_version", "second_source_source_frames", "second_source_excerpt_start_frame",
                                "second_source_excerpt_frames", "second_source_digest_hi", "second_source_digest_lo",
                                "second_source_latency_frames")
PLAYLIST_SECOND_SOURCE_IDENTITY = (*PLAYLIST_SECOND_SOURCE_UINT32, "second_source_reference_rms")
PLAYLIST_BLOCK_SUMMARY += PLAYLIST_SECOND_SOURCE_IDENTITY
PLAYLIST_ENGINE_DIGEST = ("engine_digest_hi", "engine_digest_lo")
PLAYLIST_MAX_ATTENUATION_TENTHS = 400
# The board checks every item against the card before it answers PLAYLIST:
# a long playlist takes longer than an ordinary request.
PLAYLIST_START_TIMEOUT_S = 30.0
PLAYLIST_BLOCK_SCHEMA = "wfg-live.playlist-block/1"
# A drain keeps one CDC session for its whole run: opening a session costs the
# board a burst of EP0 control requests (port open, line coding, DTR), polling
# on an open one costs none. Only after the link is lost (the port vanished, a
# write or read failed, no reply came) does it open another, at most one
# attempt per --interval, for at most this long.
DRAIN_RECONNECT_TIMEOUT_S = 60.0
# PLAYLIST? reports the board's RTOS tick (ms since the scheduler started),
# which a board reset restarts. Across a lost link without a reset it runs on
# by the time the host saw pass, within this margin (the replies' latency) and
# rate tolerance (the two clocks).
DRAIN_TICK_SLACK_MS = 2000
DRAIN_TICK_RATE_TOLERANCE = 0.01
# A busy player can be preparing the next playlist item when its completed
# result is first acknowledged. Retry only explicit BUSY refusals, and bound
# the wait even when a playlist is otherwise still running.
DRAIN_ACK_BUSY_RETRIES = 8
# Why a drain ended: the playlist's own end states (and idle: none ran), the
# poll limit, one empty poll without --until-done, a board reset, or a link
# that did not come back.
DRAIN_LOST_ENDS = ("board_reset", "link_lost")


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


def _check_files_page(files: object, requested_offset: int) -> dict:
    """Validate one exact FILES? page, including its offset and terminal marker."""
    if not isinstance(files, dict) or set(files) != {"offset", "total", "next_offset", "entries"}:
        raise ProtocolError("FILES? reply does not match the page schema")
    offset, total, next_offset, entries = (files[name] for name in ("offset", "total", "next_offset", "entries"))
    if not _integer(offset) or offset != requested_offset or not _integer(total) or offset > total:
        raise ProtocolError("FILES? page has an invalid or mismatched offset/total")
    if not isinstance(entries, list) or len(entries) > FILES_PAGE_CAPACITY or len(entries) > total - offset:
        raise ProtocolError("FILES? page has an invalid entry count")
    names = set()
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"name", "size_bytes"}:
            raise ProtocolError("FILES? entry does not match the entry schema")
        name = entry["name"]
        if not isinstance(name, str) or not name:
            raise ProtocolError("FILES? entry name must be nonempty text")
        try:
            encoded = name.encode("utf-8", errors="strict")
        except UnicodeEncodeError as error:
            raise ProtocolError("FILES? entry name is not valid UTF-8") from error
        if (len(encoded) > FILES_NAME_MAX_UTF8_BYTES or name in (".", "..") or
                any(unicodedata.category(character) == "Cc" or character in "/\\:" for character in name)):
            raise ProtocolError("FILES? entry name exceeds its UTF-8 limit or contains path characters")
        key = name.casefold()
        if key in names:
            raise ProtocolError("FILES? page contains duplicate names")
        names.add(key)
        if not _integer(entry["size_bytes"]):
            raise ProtocolError("FILES? entry size must be a uint64 JSON integer")
    expected_next = offset + len(entries)
    if expected_next == total:
        if next_offset is not None:
            raise ProtocolError("FILES? terminal page must have a null next_offset")
    elif not _integer(next_offset) or next_offset != expected_next or next_offset <= offset:
        raise ProtocolError("FILES? next_offset must advance by the exact page length")
    return files


def _number(token: str, minimum: float, maximum: float, *, positive=False):
    if not re.fullmatch(NUMBER, token) or len(token.removeprefix("-")) > 16:
        raise ControlError("invalid numeric command argument")
    value = float(token)
    if not math.isfinite(value) or value < minimum or value > maximum or (positive and value == 0):
        raise ControlError("numeric command argument is out of range")
    return value


def _query_command(body: str):
    """The canonical query name for a plain query or a paged FILES? query."""
    return "FILES?" if body == "FILES?" or body.startswith("FILES? ") else body


def _files_offset(body: str):
    tail = body.removeprefix("FILES?").strip()
    return 0 if not tail else _decimal(tail)


def _check_prepare_snapshot(snapshot: object) -> dict:
    fields = {"phase", "error", "source", "scenario", "output", "retention", "seed", "input_frames", "output_frames",
              "total_input_frames", "wav_sha256", "pcm_sha256"}
    if not isinstance(snapshot, dict) or set(snapshot) != fields:
        raise ProtocolError("PREPARE? reply has an invalid snapshot shape")
    if not _integer(snapshot["phase"], len(PREPARE_PHASES) - 1):
        raise ProtocolError("PREPARE? phase is not a known value")
    if not isinstance(snapshot["error"], str) or not snapshot["error"].isascii() or len(snapshot["error"]) > 160:
        raise ProtocolError("PREPARE? error is not a bounded static message")
    for field in ("source", "scenario", "output"):
        name = snapshot[field]
        if not isinstance(name, str) or (name and re.fullmatch(r"[A-Z0-9_]{1,8}\.(?:WAV|SCN)", name) is None):
            raise ProtocolError(f"PREPARE? {field} is not a canonical card name")
    if snapshot["source"] and not snapshot["source"].endswith(".WAV"):
        raise ProtocolError("PREPARE? source must be a WAV name")
    if snapshot["scenario"] and not snapshot["scenario"].endswith(".SCN"):
        raise ProtocolError("PREPARE? scenario must be an SCN name")
    if snapshot["output"] and not snapshot["output"].endswith(".WAV"):
        raise ProtocolError("PREPARE? output must be a WAV name")
    if snapshot["retention"] not in ("keep", "temporary"):
        raise ProtocolError("PREPARE? retention is invalid")
    for field in ("seed", "input_frames", "output_frames", "total_input_frames"):
        if not _integer(snapshot[field]):
            raise ProtocolError(f"PREPARE? {field} is not a uint64")
    for field in ("wav_sha256", "pcm_sha256"):
        if not isinstance(snapshot[field], str) or (snapshot[field] and re.fullmatch(r"[0-9a-f]{64}", snapshot[field]) is None):
            raise ProtocolError(f"PREPARE? {field} is not a SHA-256 hex digest")
    if snapshot["phase"] == PREPARE_PHASES.index("complete") and not all(snapshot[field] for field in ("wav_sha256", "pcm_sha256")):
        raise ProtocolError("PREPARE? complete phase requires both output hashes")
    return snapshot


PREPARE_TIMING_PHASES = ("setup", "source_read", "render", "append", "commit", "read", "finalize", "cleanup")


def _check_prepare_timing_stats(stats: object) -> None:
    if not isinstance(stats, dict) or set(stats) != {"calls", "total_cycles", "max_cycles", "rejected", "saturated"}:
        raise ProtocolError("PREPAREPLAY? timing phase has an invalid shape")
    for field in ("calls", "total_cycles", "rejected"):
        if not _integer(stats[field]):
            raise ProtocolError(f"PREPAREPLAY? timing {field} is not a uint64")
    if not _integer(stats["max_cycles"], UINT32_MAX) or type(stats["saturated"]) is not bool:
        raise ProtocolError("PREPAREPLAY? timing maximum or saturation flag is invalid")
    if stats["rejected"] > stats["calls"] or stats["max_cycles"] > stats["total_cycles"]:
        raise ProtocolError("PREPAREPLAY? timing counters are inconsistent")
    if not stats["saturated"] and stats["calls"] == stats["rejected"] and stats["total_cycles"] != 0:
        raise ProtocolError("PREPAREPLAY? timing cycles require an accepted interval")


def _check_prepare_timing(timing: object) -> None:
    if not isinstance(timing, dict) or set(timing) != {"cycle_hz", "tick_hz", "phases"}:
        raise ProtocolError("PREPAREPLAY? timing has an invalid shape")
    if not all(_integer(timing[field], UINT32_MAX) for field in ("cycle_hz", "tick_hz")):
        raise ProtocolError("PREPAREPLAY? timing clock frequencies are not uint32 values")
    phases = timing["phases"]
    if not isinstance(phases, dict) or set(phases) != set(PREPARE_TIMING_PHASES):
        raise ProtocolError("PREPAREPLAY? timing phase set is invalid")
    for stats in phases.values():
        _check_prepare_timing_stats(stats)
        if (timing["cycle_hz"] == 0 or timing["tick_hz"] == 0) and stats["calls"] != stats["rejected"]:
            raise ProtocolError("PREPAREPLAY? timing accepted an interval without valid clocks")


def _check_prepare_play_snapshot(snapshot: object) -> dict:
    fields = {"active", "state", "rate", "margin_frames", "total_frames", "required_lead_frames",
              "produced_frames", "written_frames", "committed_frames", "queued_frames", "consumed_frames",
              "started", "artifact_finalized", "action_pending", "owner_released", "operation_id", "timing", "status", "reason"}
    if not isinstance(snapshot, dict) or set(snapshot) != fields:
        raise ProtocolError("PREPAREPLAY? reply has an invalid snapshot shape")
    for field in ("active", "started", "artifact_finalized", "action_pending", "owner_released"):
        if type(snapshot[field]) is not bool:
            raise ProtocolError(f"PREPAREPLAY? {field} is not a boolean")
    if not _integer(snapshot["state"], 6):
        raise ProtocolError("PREPAREPLAY? state is outside the published range")
    for field in ("rate", "margin_frames"):
        if not _integer(snapshot[field], UINT32_MAX):
            raise ProtocolError(f"PREPAREPLAY? {field} is not a uint32")
    for field in ("total_frames", "required_lead_frames", "produced_frames", "written_frames", "committed_frames",
                  "queued_frames", "consumed_frames", "operation_id"):
        if not _integer(snapshot[field]):
            raise ProtocolError(f"PREPAREPLAY? {field} is not a uint64")
    if snapshot["operation_id"] == 0 and (snapshot["active"] or snapshot["state"] != 0):
        raise ProtocolError("PREPAREPLAY? active or non-idle snapshot requires a nonzero operation_id")
    if not _integer(snapshot["status"], 8):
        raise ProtocolError("PREPAREPLAY? status is outside the published range")
    if (not isinstance(snapshot["reason"], str) or not snapshot["reason"].isascii()
            or len(snapshot["reason"]) > 128):
        raise ProtocolError("PREPAREPLAY? reason is not a bounded static message")
    if (snapshot["consumed_frames"] > snapshot["queued_frames"]
            or snapshot["queued_frames"] > snapshot["committed_frames"]
            or snapshot["committed_frames"] > snapshot["written_frames"]
            or snapshot["written_frames"] > snapshot["produced_frames"]
            or snapshot["produced_frames"] > snapshot["total_frames"]):
        raise ProtocolError("PREPAREPLAY? frame watermarks are not monotonic")
    if snapshot["required_lead_frames"] > snapshot["total_frames"]:
        raise ProtocolError("PREPAREPLAY? required lead exceeds total frames")
    _check_prepare_timing(snapshot["timing"])
    return snapshot


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
        return _query_command(self.body) not in QUERIES

    def event_frames(self, first_frame):
        last = first_frame + (self.events - 1) * self.step_frames
        if not _integer(first_frame) or last > UINT64_MAX:
            raise ControlError("scheduled output frame overflows uint64")
        return [first_frame + index * self.step_frames for index in range(self.events)]


def parse_command(text: str) -> Command:
    if not isinstance(text, str) or not text or not text.isascii() or any(ord(c) < 32 or ord(c) > 126 for c in text):
        raise ControlError("command must be one printable ASCII line")
    if len((f"{UINT32_MAX} {text}\n").encode("ascii")) > REQUEST_LIMIT:
        raise ControlError(f"command exceeds the {REQUEST_LIMIT}-byte wire limit")
    body = text
    at = None
    if body.startswith("AT:"):
        prefix, separator, body = body.partition(" ")
        if not separator:
            raise ControlError("AT requires a live command")
        at = _decimal(prefix[3:])
    if body.startswith("FILES?"):
        if at is not None or (body != "FILES?" and not body.startswith("FILES? ")):
            raise ControlError("FILES? is a read-only query with an optional canonical offset")
        if body != "FILES?":
            _decimal(body[7:])
        return Command(text, body)
    if body.startswith("PREPARE:"):
        if at is not None:
            raise ControlError("AT cannot schedule PREPARE")
        parts = body.removeprefix("PREPARE:").split(":")
        if len(parts) not in (4, 5):
            raise ControlError("PREPARE requires output, source, scenario, retention and optional seed")
        if (re.fullmatch(r"[A-Z0-9_]{1,8}\.WAV", parts[0]) is None or
                re.fullmatch(r"[A-Z0-9_]{1,8}\.WAV", parts[1]) is None or
                re.fullmatch(r"[A-Z0-9_]{1,8}\.SCN", parts[2]) is None or parts[3] not in ("KEEP", "TEMP")):
            raise ControlError("PREPARE requires canonical 8.3 WAV/SCN names and KEEP or TEMP")
        if len(parts) == 5:
            _decimal(parts[4])
        return Command(text, body)
    if body.startswith("PREPAREPLAY:"):
        if at is not None:
            raise ControlError("AT cannot schedule PREPAREPLAY")
        parts = body.removeprefix("PREPAREPLAY:").split(":")
        if len(parts) not in (6, 7):
            raise ControlError("PREPAREPLAY requires output, source, scenario, retention, rate, margin and optional seed")
        if (re.fullmatch(r"[A-Z0-9_]{1,8}\.WAV", parts[0]) is None or
                re.fullmatch(r"[A-Z0-9_]{1,8}\.WAV", parts[1]) is None or
                re.fullmatch(r"[A-Z0-9_]{1,8}\.SCN", parts[2]) is None or parts[3] not in ("KEEP", "TEMP")):
            raise ControlError("PREPAREPLAY requires canonical 8.3 WAV/SCN names and KEEP or TEMP")
        rate = _decimal(parts[4], UINT32_MAX)
        if rate == 0:
            raise ControlError("PREPAREPLAY rate must be a positive uint32")
        _decimal(parts[5], UINT32_MAX)
        if len(parts) == 7:
            _decimal(parts[6])
        return Command(text, body)
    if body in QUERIES or body in ("PLAY", "STOP", "MEDIA HOST", "MEDIA LOCAL") or body in OUTPUTS:
        if at is not None:
            raise ControlError("AT applies only to live impairment commands")
        return Command(text, body)
    if body == cut_schedule.COMMAND or body.startswith(cut_schedule.COMMAND + ":"):
        if at is not None:
            raise ControlError("AT cannot schedule CUT")
        try:
            cut_schedule.validate(cut_schedule.parse_command(body))
        except cut_schedule.CutError as error:
            raise ControlError(f"invalid CUT schedule: {error}") from error
        return Command(text, body)
    if PLAYLIST_START.fullmatch(body) or body.startswith(PLAYLIST_ACK):
        if at is not None:
            raise ControlError("AT cannot schedule PLAYLIST")
        if body.startswith(PLAYLIST_ACK):
            _decimal(body[len(PLAYLIST_ACK):], UINT32_MAX)
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
                 timeout=3.0, volume_dismounted=False, clock=time.monotonic, expected_board="MIMXRT1170-EVK"):
        if not expected_serial or not expected_serial.isascii() or len(expected_serial) > 128:
            raise ControlError("an explicit expected fixture USB serial is required")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ControlError("timeout must be finite and positive")
        self.transport = transport
        if expected_board not in ("MIMXRT1170-EVK", "MIMXRT1064-EVK"):
            raise ControlError("unsupported expected board")
        self.expected_serial = expected_serial
        self.expected_board = expected_board
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
        if (info.get("kind") != "waveform-player" or info.get("board") != self.expected_board
                or info.get("sample_rate_hz") != 48000 or info.get("usb_serial") != self.expected_serial):
            self.poisoned = True
            raise ProtocolError("INFO identity does not match the expected waveform player board and serial")
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
        field = QUERIES.get(_query_command(command.body), "result") if response["ok"] else "error"
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
        elif response["ok"] and _query_command(command.body) == "FILES?":
            _check_files_page(response["files"], _files_offset(command.body))
        elif response["ok"] and command.body == "PREPARE?":
            _check_prepare_snapshot(response["prepare"])
        elif response["ok"] and command.body == "PREPAREPLAY?":
            _check_prepare_play_snapshot(response["prepare_play"])
        elif response["ok"] and (command.body.startswith("PREPARE:") or command.body.startswith("PREPAREPLAY:")):
            result = response["result"]
            if result.get("accepted") is not True:
                raise ProtocolError("preparation admission reply must acknowledge acceptance")
        return response

    def execute(self, text: str):
        command = parse_command(text)
        if self.poisoned:
            raise UncertainCommand("connection is untrusted; reconnect and inspect state instead of retrying")
        if self.in_flight:
            raise ControlError("only one request may be outstanding")
        if command.mutates and self.identity is None:
            raise ControlError("verify INFO identity before sending mutations")
        if command.body.startswith("PREPARE:"):
            capabilities = self.identity.get("capabilities") if isinstance(self.identity, dict) else None
            if not isinstance(capabilities, list) or PREPARE_CAPABILITY not in capabilities:
                raise ControlError("PREPARE requires the INFO prepare capability")
        if command.body.startswith("PREPAREPLAY:"):
            capabilities = self.identity.get("capabilities") if isinstance(self.identity, dict) else None
            if not isinstance(capabilities, list) or PREPARE_PLAY_CAPABILITY not in capabilities:
                raise ControlError("PREPAREPLAY requires the INFO prepare-play-experimental capability")
        if command.body == "PREPAREPLAY?":
            capabilities = self.identity.get("capabilities") if isinstance(self.identity, dict) else None
            if not isinstance(capabilities, list) or PREPARE_PLAY_CAPABILITY not in capabilities:
                raise ControlError("PREPAREPLAY? requires the INFO prepare-play-experimental capability")
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


def list_files(controller: Controller) -> dict:
    """Read the whole card listing from a FILES-capable image using strict pages."""
    identity = controller.identity
    capabilities = identity.get("capabilities") if isinstance(identity, dict) else None
    if not isinstance(capabilities, list) or FILES_CAPABILITY not in capabilities:
        raise ControlError("the identified image does not advertise the files capability")
    offset = 0
    total = None
    entries = []
    seen_names = set()
    while True:
        command = "FILES?" if offset == 0 else f"FILES? {offset}"
        page = controller.execute(command)["files"]
        _check_files_page(page, offset)
        if total is None:
            total = page["total"]
        elif page["total"] != total:
            raise ProtocolError("FILES? total changed during pagination")
        for entry in page["entries"]:
            key = entry["name"].casefold()
            if key in seen_names:
                raise ProtocolError("FILES? listing contains duplicate names across pages")
            seen_names.add(key)
            entries.append(entry)
        next_offset = page["next_offset"]
        if next_offset is None:
            break
        offset = next_offset
    return {"total": total, "entries": entries}


def _identity_route(identity: dict):
    """The OUTPUT command for the route the recorded INFO reports.

    The route is board state that outlives a CDC connection, so a journal that
    never sent OUTPUT still ran on the route INFO reported. None for firmware
    whose INFO has no "output": it has only the codec and rejects OUTPUT.
    """
    if "output" not in identity:
        return None
    command = ROUTE_COMMANDS.get(identity["output"]) if isinstance(identity["output"], str) else None
    if command is None:
        raise ControlError("recorded INFO names an unknown output route")
    return command


def _is_cut(body: str) -> bool:
    return body == cut_schedule.COMMAND or body.startswith(cut_schedule.COMMAND + ":")


def _cut_capable(identity: dict) -> bool:
    capabilities = identity.get("capabilities")
    return isinstance(capabilities, list) and CUT_CAPABILITY in capabilities


def _check_cut_snapshot(cut: str, usb: dict):
    """A USB? reply of the exported play must not count more cuts than its CUT holds.

    uac_play_cut_count and uac_play_cut_rejected count the entries the play
    took; more than the journal's last CUT has means the board held a list
    that another connection set, which the plan cannot reproduce.
    """
    if not isinstance(usb, dict):
        raise ProtocolError("USB? reply lacks its usb object")
    entries = len(cut_schedule.parse_command(cut))
    counted = [usb.get(field) for field in ("uac_play_cut_count", "uac_play_cut_rejected")]
    if all(_integer(value, UINT32_MAX) for value in counted) and sum(counted) > entries:
        raise ControlError(f"the play took {sum(counted)} cut entries but its journal's CUT holds {entries}: "
                           "the schedule was sent on another connection; send CUT, LOAD and PLAY on one connection")


def export_replay(records: list[dict]):
    identity = None
    route = None
    cut = None
    usb_snapshots = []
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
            route = _identity_route(identity)
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
        if PLAYLIST_START.fullmatch(command.body):
            # Its items ran from the card's playlist file: replay that file, not this journal.
            raise ControlError("a PLAYLIST run is not exported: start its playlist file again")
        if command.body.startswith("PLAYLIST"):
            continue  # PLAYLIST? and PLAYLIST ACK change no playback.
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
        elif command.body.startswith(SETUP_PREFIXES):
            if saw_play:
                raise ControlError("source configuration changed after playback; select a single run journal")
            if command.body in OUTPUTS:
                route = command.body  # The last accepted OUTPUT is the route PLAY ran on.
                continue
            if _is_cut(command.body):
                cut = command.body  # Each CUT replaces the list: the last accepted one is what PLAY took.
                continue
            source_commands.append(command.body)
            if command.body.startswith("LOAD:"):
                controls.clear()  # LOAD discards the preceding selection's preloaded events.
        elif command.body == "USB?" and saw_play:
            if response["run_id"] != run_id:
                raise ControlError("snapshot belongs to a different playback")
            usb_snapshots.append(response[QUERIES[command.body]])
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
    if cut is not None or _cut_capable(identity):
        # Always explicit and last, so a replayed PLAY never takes a list left
        # pending on the target board: the journal's CUT, else none ("CUT").
        cut = cut if cut is not None else cut_schedule.COMMAND
        for usb in usb_snapshots:
            _check_cut_snapshot(cut, usb)
        source_commands.append(cut)
    if route is not None:
        # Always explicit, so a replay never inherits the target board's route.
        source_commands.insert(0, route)
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
        if not command.body.startswith(SETUP_PREFIXES) or command.at_frame is not None:
            raise ControlError("invalid replay source configuration")
    if not any(text.startswith("LOAD:") for text in source_commands):
        raise ControlError("replay must select its source explicitly")
    if "output" in plan["identity"] and not any(text in OUTPUTS for text in source_commands):
        # The recorded firmware has output routes: the plan must name the one it ran on.
        raise ControlError("replay must select its output route explicitly; export the journal again")
    if _cut_capable(plan["identity"]) and not any(_is_cut(text) for text in source_commands):
        # The recorded firmware has CUT: the plan must name the list its PLAY took, even none.
        raise ControlError("replay must set its CUT schedule explicitly; export the journal again")
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
        if status["state"] in (*PLAYER_END_STATES, *PLAYER_END_STATES.values()):
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


def playlist_command(action: str, name: str | None = None, *, continue_on_error: bool = False, seq: int | None = None) -> str:
    """The PLAYLIST command for `playlist start NAME [--continue]`, `playlist status` or `playlist ack SEQ`."""
    if action == "status":
        return "PLAYLIST?"
    if action == "ack":
        if not _integer(seq, UINT32_MAX):
            raise ControlError("PLAYLIST ACK takes a result block's number, 0..4294967295")
        return f"{PLAYLIST_ACK}{seq}"
    text = f"PLAYLIST:{name}" + (":CONTINUE" if continue_on_error else "")
    if not PLAYLIST_START.fullmatch(text):
        raise ControlError("a playlist is named by an upper-case 8.3 name on the card's 2:/WG")
    return text


def prepare_command(source: str, scenario: str, output: str, *, retention: str = "keep", seed: int | None = None) -> str:
    """Build one unscheduled PREPARE admission command with canonical card names."""
    policy = {"keep": "KEEP", "temporary": "TEMP"}.get(retention)
    if policy is None:
        raise ControlError("retention must be keep or temporary")
    body = f"PREPARE:{output}:{source}:{scenario}:{policy}"
    if seed is not None:
        if not _integer(seed):
            raise ControlError("PREPARE seed must be a uint64")
        body += f":{seed}"
    return parse_command(body).body


def prepare_play_command(source: str, scenario: str, output: str, *, rate: int, margin_frames: int,
                         retention: str = "keep", seed: int | None = None) -> str:
    """Build one experimental PREPAREPLAY admission with explicit measured rate and frame margin."""
    policy = {"keep": "KEEP", "temporary": "TEMP"}.get(retention)
    if policy is None:
        raise ControlError("retention must be keep or temporary")
    if not _integer(rate, UINT32_MAX) or rate == 0:
        raise ControlError("PREPAREPLAY rate must be a positive uint32")
    if not _integer(margin_frames, UINT32_MAX):
        raise ControlError("PREPAREPLAY margin must be a uint32 frame count")
    body = f"PREPAREPLAY:{output}:{source}:{scenario}:{policy}:{rate}:{margin_frames}"
    if seed is not None:
        if not _integer(seed):
            raise ControlError("PREPAREPLAY seed must be a uint64")
        body += f":{seed}"
    return parse_command(body).body


def _canonical_uint32_argument(token: str, *, positive: bool = False) -> int:
    try:
        value = _decimal(token, UINT32_MAX)
    except ControlError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if positive and value == 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def wait_for_prepare(controller: Controller, *, timeout: float, interval: float = 1.0,
                     sleep=time.sleep, clock=time.monotonic) -> dict:
    if not math.isfinite(timeout) or timeout <= 0 or not math.isfinite(interval) or interval <= 0:
        raise ControlError("prepare wait timeout and interval must be finite and positive")
    deadline = clock() + timeout
    while True:
        response = controller.execute("PREPARE?")
        phase = PREPARE_PHASES[response["prepare"]["phase"]]
        if phase in ("complete", "cancelled", "failed", "cleanup_failed", "idle"):
            return response
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError("PREPARE did not reach a terminal phase before the wait timeout")
        sleep(min(interval, remaining))


def prepare_wait_exit_code(response: dict) -> int:
    """Return nonzero for a terminal PREPARE wait that did not complete."""
    phase = PREPARE_PHASES[response["prepare"]["phase"]]
    return 0 if phase == "complete" else 4


def wait_for_prepare_play(controller: Controller, *, timeout: float, interval: float = 1.0,
                          sleep=time.sleep, clock=time.monotonic) -> dict:
    """Wait for one boot-local finite-playback operation to release its job.

    PREPAREPLAY? reports the current or last admitted operation. Its ID is only
    monotonic within one boot, not a global or reboot-proof identity. The first
    snapshot's ID pins this wait; a change means later snapshots are incomparable.
    Detailed view: journal read-only polls until inactive, including failure
    cleanup; cap sleeps by the deadline and leave the board untouched on error.
    5th-grade view: wait until the worker puts its tools down, even after a mistake.
    """
    if not math.isfinite(timeout) or timeout <= 0 or not math.isfinite(interval) or interval <= 0:
        raise ControlError("PREPAREPLAY wait timeout and interval must be finite and positive")
    deadline = clock() + timeout
    operation_id = None
    while True:
        response = controller.execute("PREPAREPLAY?")
        snapshot = response["prepare_play"]
        if operation_id is None:
            operation_id = snapshot["operation_id"]
        elif snapshot["operation_id"] != operation_id:
            raise ProtocolError("PREPAREPLAY operation_id changed during bounded wait")
        if not snapshot["active"]:
            return response
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError("PREPAREPLAY did not become inactive before the wait timeout; no STOP was sent")
        sleep(min(interval, remaining))


def prepare_play_wait_exit_code(response: dict) -> int:
    """Return zero only for a drained, finalized, clean completion of the reported finite playback."""
    try:
        snapshot = _check_prepare_play_snapshot(response["prepare_play"])
        total = snapshot["total_frames"]
        return 0 if (
            snapshot["active"] is False
            and snapshot["state"] == 4
            and snapshot["status"] == 0
            and snapshot["started"] is True
            and snapshot["artifact_finalized"] is True
            and snapshot["owner_released"] is True
            and snapshot["action_pending"] is False
            and all(snapshot[field] == total for field in
                    ("consumed_frames", "queued_frames", "committed_frames", "written_frames", "produced_frames"))
        ) else 4
    except (KeyError, TypeError, ProtocolError):
        return 4


def _check_playlist_reply(playlist: object) -> dict:
    """A PLAYLIST? reply's playlist object: its progress and at most three blocks, oldest first."""
    if not isinstance(playlist, dict) or playlist.get("format") != "playlist":
        raise ProtocolError("PLAYLIST? reply lacks its playlist object")
    if playlist.get("state") not in PLAYLIST_STATES or not isinstance(playlist.get("name"), str):
        raise ProtocolError("PLAYLIST? reports an unknown state")
    for field in ("run", "items", "index", "completed", "failed", "pending", "oldest_seq", "next_seq"):
        if not _integer(playlist.get(field), UINT32_MAX):
            raise ProtocolError(f"PLAYLIST? field {field} is not a uint32")
    # The tick came with the drain's reset check; an image before it has none.
    if "tick" in playlist and not _integer(playlist["tick"], UINT32_MAX):
        raise ProtocolError("PLAYLIST? field tick is not a uint32")
    blocks = playlist.get("blocks")
    if not isinstance(blocks, list) or len(blocks) > min(PLAYLIST_BLOCKS_PER_REPLY, playlist["pending"]):
        raise ProtocolError("PLAYLIST? carries more blocks than it holds")
    previous = 0
    for block in blocks:
        if not isinstance(block, dict) or not _integer(block.get("seq"), UINT32_MAX) or block["seq"] <= previous:
            raise ProtocolError("PLAYLIST? blocks must be numbered in increasing order")
        previous = block["seq"]
        _check_block_members(block)
    if blocks and blocks[0]["seq"] != playlist["oldest_seq"]:
        raise ProtocolError("PLAYLIST? blocks must start with the oldest one pending")
    return playlist


def _check_block_members(block: dict) -> None:
    """A block's att=, dur=, loops, end cause and deadline misses, where it reports them, as the board spells them."""
    if "end_cause" in block and block["end_cause"] not in PLAYLIST_END_CAUSES:
        raise ProtocolError(f"PLAYLIST? block {block['seq']} reports an unknown end cause")
    if "att_tenths_db" in block and not _integer(block["att_tenths_db"], PLAYLIST_MAX_ATTENUATION_TENTHS):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} reports an attenuation outside 0 to 400 tenths of a dB")
    for name in ("loops", "engine_deadline_misses"):
        if name in block and not _integer(block[name], UINT32_MAX):
            raise ProtocolError(f"PLAYLIST? block {block['seq']} field {name} is not a uint32")
    if "dur_frames" in block and block["dur_frames"] is not None and not _integer(block["dur_frames"], UINT32_MAX):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} field dur_frames is neither null nor a uint32")
    for name in (*PLAYLIST_WATTERSON_IDENTITY, *PLAYLIST_ENGINE_DIGEST):
        if name in block and not _integer(block[name], UINT32_MAX):
            raise ProtocolError(f"PLAYLIST? block {block['seq']} field {name} is not a uint32")
    watterson_present = [name in block for name in PLAYLIST_WATTERSON_IDENTITY]
    if any(watterson_present) and not all(watterson_present):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} has an incomplete Watterson identity")
    if all(watterson_present) and block["watterson_version"] == 0 and any(block[name] != 0 for name in PLAYLIST_WATTERSON_IDENTITY[1:]):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} has Watterson values without a configured channel")
    engine_present = [name in block for name in PLAYLIST_ENGINE_DIGEST]
    if any(engine_present) and not all(engine_present):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} has an incomplete engine digest")
    _validate_second_source_identity(block)


def _validate_second_source_identity(block: dict) -> None:
    """Validate optional measured identity while accepting legacy records without it."""
    present = [name in block for name in PLAYLIST_SECOND_SOURCE_IDENTITY]
    if not any(present):
        return
    if not all(present):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} has an incomplete second-source identity")
    if any(not _integer(block[name], UINT32_MAX) for name in PLAYLIST_SECOND_SOURCE_UINT32):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} has a second-source field outside uint32")
    rms = block["second_source_reference_rms"]
    if type(rms) not in (int, float) or not 0 <= rms <= 1 or not math.isfinite(rms):
        raise ProtocolError(f"PLAYLIST? block {block['seq']} has an invalid second-source RMS")
    if block["second_source_version"] == 0:
        if any(block[name] != 0 for name in PLAYLIST_SECOND_SOURCE_IDENTITY[1:]):
            raise ProtocolError(f"PLAYLIST? block {block['seq']} has second-source values without a configured source")
        return
    first = block["second_source_excerpt_start_frame"]
    count = block["second_source_excerpt_frames"]
    if rms == 0 or count == 0 or first + count > block["second_source_source_frames"]:
        raise ProtocolError(f"PLAYLIST? block {block['seq']} has an invalid second-source excerpt")


def block_summary(block: dict) -> dict:
    """What the drain shows of a block it saved: PLAYLIST_BLOCK_SUMMARY's members the block has."""
    return {name: block[name] for name in PLAYLIST_BLOCK_SUMMARY if name in block}


def _saved_blocks(path: Path) -> set[str]:
    """The blocks a drain already saved to `path`, as canonical JSON, so a block the board still holds is never saved twice."""
    if not path.exists():
        return set()
    saved = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if line:
            record = strict_json(line)
            if not isinstance(record, dict) or record.get("schema") != PLAYLIST_BLOCK_SCHEMA or not isinstance(record.get("block"), dict):
                raise ControlError(f"{path} holds a line that is no saved result block")
            saved.add(json.dumps(record["block"], sort_keys=True, separators=(",", ":")))
    return saved


def link_failure(error: BaseException) -> bool:
    """Whether a failed request lost the link: a port or I/O error, or no reply in time. A malformed reply is not."""
    return isinstance(error, UncertainCommand) and isinstance(error.__cause__, OSError)


def board_reset(last: dict, now: dict, elapsed_s: float) -> list[str]:
    """The rules by which `now`, the first PLAYLIST? reply after a lost link, shows a board reset since `last`.

    - "tick": the RTOS tick did not run on by the host's elapsed time (a reset
      restarts it), beyond DRAIN_TICK_SLACK_MS and DRAIN_TICK_RATE_TOLERANCE;
    - "sequence": the run number or the next block number went back (both
      only grow between resets);
    - "idle": the state went back to idle, which only a reset gives.
    An image whose reply has no tick is judged by the last two. An empty list:
    the board ran on, and only the USB link went away and came back.
    """
    rules = []
    if _integer(last.get("tick"), UINT32_MAX) and _integer(now.get("tick"), UINT32_MAX):
        advance = (now["tick"] - last["tick"]) & UINT32_MAX
        expected = max(elapsed_s, 0.0) * 1000.0
        if abs(advance - expected) > DRAIN_TICK_SLACK_MS + DRAIN_TICK_RATE_TOLERANCE * expected:
            rules.append("tick")
    if (now["run"], now["next_seq"]) < (last["run"], last["next_seq"]):
        rules.append("sequence")
    if now["state"] == "idle" and last["state"] != "idle":
        rules.append("idle")
    return rules


def _progress_summary(playlist: dict) -> dict:
    return {name: playlist.get(name) for name in ("state", "run", "items", "index", "completed", "failed", "pending", "oldest_seq",
                                                  "next_seq", "tick")}


def _lost_blocks(last: dict, newest_seq: int) -> list[int] | None:
    """The block numbers the board had made by `last` that the drain never saw: [first, last], or None.

    Blocks below the oldest pending one were acknowledged, so saved; the drain
    saved every block it was shown (`newest_seq` is the newest).
    """
    acknowledged_through = last["oldest_seq"] - 1 if last["pending"] else last["next_seq"] - 1
    first = max(newest_seq, acknowledged_through) + 1
    return [first, last["next_seq"] - 1] if first <= last["next_seq"] - 1 else None


class PlaylistDrain:
    """Saves every result block the board holds to `blocks_path` (JSON lines), then acknowledges it, on one session.

    Each block is written and flushed before its PLAYLIST ACK, so a drain cut
    short never loses one; a block saved but not acknowledged is shown again
    and not saved twice (`blocks_path` is read first). Blocks are fetched
    back to back while the board has any; with `until_done`, an empty poll is
    followed by `interval` seconds of sleep while the playlist runs, until it
    has ended, every block is saved, or `max_polls` empty polls have passed.

    The drain uses one CDC session for its whole run. When the link is lost
    (a port, write or read error, or no reply), `reconnect(first_sequence)`
    opens a new identified session, at most once per `interval`, for at most
    `reconnect_timeout` seconds; without `reconnect` the drain ends there. The
    first reply on the new session says whether the board was reset
    (board_reset()): if not, the drain goes on (a block shown again is not
    saved twice); if it was, the board's ring is gone, and the drain ends and
    reports the blocks it knows were lost (`lost_seqs`: those the board had
    made by the last reply, a lower bound, since blocks made after it cannot
    be named) and whether the playlist was still running. Each saved line names the playlist the reply reports for
    the blocks of its run; a block of an earlier run, which the ring keeps
    until it is acknowledged, is saved with no name ("playlist": null).
    """

    def __init__(self, controller: Controller, blocks_path: Path, *, interval: float = 1.0, reconnect=None,
                 reconnect_timeout: float = DRAIN_RECONNECT_TIMEOUT_S, sleep=time.sleep, clock=time.monotonic):
        self.controller = controller
        self.blocks_path = blocks_path
        self.interval = interval
        self.reconnect = reconnect
        self.reconnect_timeout = reconnect_timeout
        self.sleep = sleep
        self.clock = clock
        self.identity = controller.identity
        self.saved = _saved_blocks(blocks_path)
        self.written = self.acknowledged = self.polls = self.empty_polls = 0
        self.sessions = 1
        self.shown = []
        self.links = []
        self.last = None
        self.last_at = None
        self.newest_seq = 0
        self.relinked = False

    def run(self, *, until_done: bool = False, max_polls: int | None = None) -> dict:
        with self.blocks_path.open("a", encoding="utf-8", newline="\n") as output:
            while True:
                playlist = self._exchange(lambda: _check_playlist_reply(self.controller.execute("PLAYLIST?")["playlist"]))
                if playlist is None:
                    if not self._relink():
                        return self._result("link_lost")
                    continue
                self.polls += 1
                if self.relinked and self._reset_seen(playlist):
                    return self._result("board_reset", playlist)
                self.relinked = False
                self.last, self.last_at = playlist, self.clock()
                blocks = playlist["blocks"]
                self._save(output, playlist, blocks)
                if blocks:
                    seq = blocks[-1]["seq"]
                    if not self._acknowledge(seq):
                        if not self._relink():
                            return self._result("link_lost")
                        continue
                    self.acknowledged += len(blocks)
                    continue
                self.empty_polls += 1
                if not until_done or playlist["state"] not in PLAYLIST_ACTIVE_STATES:
                    return self._result(playlist["state"] if until_done else "empty_poll", playlist)
                if max_polls is not None and self.empty_polls >= max_polls:
                    return self._result("max_polls", playlist)
                self.sleep(self.interval)

    def _acknowledge(self, seq: int) -> bool:
        """ACK a saved page; only an explicit BUSY rejection permits a bounded retry."""
        for attempt in range(DRAIN_ACK_BUSY_RETRIES + 1):
            try:
                result = self._exchange(lambda: self.controller.execute(playlist_command("ack", seq=seq)))
            except CommandRejected as error:
                if error.response["error"]["code"] != "BUSY" or attempt == DRAIN_ACK_BUSY_RETRIES:
                    raise
                self.sleep(self.interval)
                continue
            if result is not None:
                return True
            # No trustworthy reply: reconnect and re-read the ring before any
            # repeated ACK, so an uncertain command is never sent again here.
            return False
        return False

    def _exchange(self, request):
        """The request's result, or None when it lost the link; any other failure propagates."""
        try:
            return request()
        except UncertainCommand as error:
            if not link_failure(error):
                raise
            self.links.append({"lost_after_polls": self.polls, "error": str(error)})
            self.controller._record({"schema": "wfg-live.session/1", "record": "link", "event": "lost", "error": str(error),
                                     "observed_utc": datetime.now(timezone.utc).isoformat()})
            return None

    def _save(self, output: TextIO, playlist: dict, blocks: list) -> None:
        for block in blocks:
            self.newest_seq = max(self.newest_seq, block["seq"])
            key = json.dumps(block, sort_keys=True, separators=(",", ":"))
            if key not in self.saved:
                name = playlist["name"] if block.get("run") == playlist["run"] else None
                output.write(json.dumps({"schema": PLAYLIST_BLOCK_SCHEMA, "playlist": name, "block": block},
                                        ensure_ascii=True, allow_nan=False, separators=(",", ":")) + "\n")
                self.saved.add(key)
                self.written += 1
                self.shown.append(block_summary(block))
        output.flush()

    def _relink(self) -> bool:
        """Opens a new session after a lost link, at most one attempt per interval; False when none came in time."""
        try:
            self.controller.transport.close()
        except OSError:
            pass  # The port is already gone.
        if self.reconnect is None:
            return False
        deadline = self.clock() + self.reconnect_timeout
        attempts = 0
        while True:
            self.sleep(self.interval)
            attempts += 1
            try:
                controller = self.reconnect(self.controller.next_sequence)
                break
            except (OSError, UncertainCommand) as error:
                if isinstance(error, UncertainCommand) and not link_failure(error):
                    raise
                if self.clock() >= deadline:
                    self.links[-1].update({"reconnect_attempts": attempts, "restored": False})
                    return False
        if self.identity is not None and any(controller.identity.get(name) != self.identity.get(name)
                                             for name in ("usb_serial", "build_id", "fw_commit")):
            self.links[-1]["image_changed"] = True
        self.controller = controller
        self.sessions += 1
        self.relinked = True
        self.links[-1].update({"reconnect_attempts": attempts, "restored": True})
        return True

    def _reset_seen(self, playlist: dict) -> bool:
        """Whether the first reply after a relink shows a board reset; records what the reset lost."""
        event = self.links[-1]
        if self.last is None:
            event.update({"board_reset": None, "reply_after": _progress_summary(playlist)})
            return False
        rules = board_reset(self.last, playlist, self.clock() - self.last_at)
        if event.get("image_changed"):
            rules.append("image")
        event.update({"board_reset": bool(rules), "rules": rules})
        record = {"schema": "wfg-live.session/1", "record": "link", "event": "restored", "board_reset": bool(rules), "rules": rules,
                  "observed_utc": datetime.now(timezone.utc).isoformat()}
        if rules:
            event.update({"last_reply": _progress_summary(self.last), "reply_after": _progress_summary(playlist),
                          "lost_seqs": _lost_blocks(self.last, self.newest_seq),
                          "playlist_running": self.last["state"] in PLAYLIST_ACTIVE_STATES})
            record["lost_seqs"] = event["lost_seqs"]
        self.controller._record(record)
        return bool(rules)

    def _result(self, ended: str, playlist: dict | None = None) -> dict:
        reply = playlist if playlist is not None else self.last or {}
        return {"ended": ended, "written": self.written, "acknowledged": self.acknowledged, "polls": self.polls,
                "sessions": self.sessions, "links": self.links, "state": reply.get("state"), "playlist": reply.get("name"),
                "completed": reply.get("completed"), "failed": reply.get("failed"), "items": reply.get("items"), "blocks": self.shown}


def drain_playlist(controller: Controller, blocks_path: Path, *, until_done: bool = False, interval: float = 1.0,
                   max_polls: int | None = None, sleep=time.sleep, reconnect=None,
                   reconnect_timeout: float = DRAIN_RECONNECT_TIMEOUT_S, clock=time.monotonic) -> dict:
    """Drains the board's result blocks into `blocks_path` on one session (PlaylistDrain).

    The result shows each block saved (block_summary(): which item, how and
    why it ended, its frames sent and loops, its att= and dur=), why the drain
    ended (`ended`), its polls and sessions, and each lost link (`links`).
    """
    return PlaylistDrain(controller, blocks_path, interval=interval, reconnect=reconnect, reconnect_timeout=reconnect_timeout,
                         sleep=sleep, clock=clock).run(until_done=until_done, max_polls=max_polls)


def cut_command(action: str, entries: str | None = None) -> str:
    """The CUT command for `cut set ENTRIES` or `cut clear`, checked as the board checks it."""
    if action == "clear":
        return cut_schedule.COMMAND
    try:
        return cut_schedule.render_command(cut_schedule.parse_entries(entries or ""))
    except cut_schedule.CutError as error:
        raise ControlError(f"invalid CUT schedule: {error}") from error


def open_session(port: str, expected_serial: str, *, journal: TextIO | None, timeout: float, volume_dismounted: bool = False,
                 first_sequence: int = 1, expected_board: str = "MIMXRT1170-EVK") -> Controller:
    """A new CDC session on `port`, identified by INFO?; its first request is numbered `first_sequence`.

    A drain that opens another session after a lost link numbers on, so its
    journal's sequences keep increasing (the board accepts any first number on
    a new connection). The port is closed again when the identity fails.
    """
    transport = SerialTransport(port, timeout)
    try:
        controller = Controller(transport, expected_serial, journal=journal, timeout=timeout,
                                volume_dismounted=volume_dismounted, expected_board=expected_board)
        controller.next_sequence = first_sequence
        controller.identify()
    except BaseException:
        transport.close()
        raise
    return controller


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="explicit serial endpoint; never a remembered COM default")
    parser.add_argument("--expected-serial", help="expected fixture USB serial, checked against INFO")
    parser.add_argument("--expected-board", choices=("MIMXRT1170-EVK", "MIMXRT1064-EVK"),
                        default="MIMXRT1170-EVK", help="expected board model, checked against INFO")
    parser.add_argument("--record", type=Path, help="new caller-owned JSONL journal (existing files are not overwritten)")
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--host-volume-dismounted", action="store_true",
                        help="operator assertion that host writes were flushed and its volume dismounted")
    actions = parser.add_subparsers(dest="action", required=True)
    commands = actions.add_parser("command", help="send one or more quoted commands on one connection")
    commands.add_argument("commands", nargs="+")
    cut = actions.add_parser("cut", help="set or clear the CUT schedule the next PLAY takes")
    cut_actions = cut.add_subparsers(dest="cut_action", required=True)
    cut_set = cut_actions.add_parser("set", help="OFFSET:N:drop|zero,... (engine output frames, a clean card's own; at most 16 entries, N 1..4800)")
    cut_set.add_argument("entries")
    cut_actions.add_parser("clear", help="no cuts for the next PLAY")
    playlist = actions.add_parser("playlist", help="run a playlist from the card's 2:/WG and collect its result blocks")
    playlist_actions = playlist.add_subparsers(dest="playlist_action", required=True)
    playlist_start = playlist_actions.add_parser("start", help="PLAYLIST:<NAME>: check every item, then play them unattended")
    playlist_start.add_argument("name", help="the playlist file's 8.3 name in 2:/WG")
    playlist_start.add_argument("--continue", dest="continue_on_error", action="store_true", help="go on after an item error (default: stop)")
    playlist_actions.add_parser("status", help="PLAYLIST?: the progress and the oldest result blocks")
    playlist_ack = playlist_actions.add_parser("ack", help="PLAYLIST ACK:<seq>: free the result blocks up to seq")
    playlist_ack.add_argument("seq", type=int)
    playlist_drain = playlist_actions.add_parser("drain", help="save every result block as JSON lines, acknowledging each once saved")
    playlist_drain.add_argument("--blocks", type=Path, required=True, help="JSON lines file the blocks are appended to (kept across calls)")
    playlist_drain.add_argument("--until-done", action="store_true", help="keep polling while the playlist runs")
    playlist_drain.add_argument("--interval", type=float, default=1.0, help="seconds between polls with nothing to save")
    playlist_drain.add_argument("--max-polls", type=int, help="stop after this many polls with nothing to save")
    playlist_drain.add_argument("--reconnect-timeout", type=float, default=DRAIN_RECONNECT_TIMEOUT_S,
                                help="after a lost link, try a new session once per --interval for this many seconds (0: end the drain)")
    actions.add_parser("files", help="read every FILES? page from a files-capable image")
    prepare = actions.add_parser("prepare", help="calibrate and render an impaired card WAV before playing it")
    prepare.add_argument("--source", required=True, help="canonical source WAV name on 2:/WG")
    prepare.add_argument("--scenario", required=True, help="canonical scenario SCN name on 2:/WG")
    prepare.add_argument("--output", required=True, help="new canonical output WAV name on 2:/WG")
    prepare.add_argument("--retention", choices=("keep", "temporary"), default="keep")
    prepare.add_argument("--seed", type=int, help="optional uint64 seed override")
    prepare.add_argument("--wait-timeout", type=float, help="wait for a terminal PREPARE? state for this many seconds")
    prepare_play = actions.add_parser("prepare-play", help="experimental: render and start finite playback with explicit rate and lead margin")
    prepare_play.add_argument("--source", required=True, help="canonical source WAV name on 2:/WG")
    prepare_play.add_argument("--scenario", required=True, help="canonical scenario SCN name on 2:/WG")
    prepare_play.add_argument("--output", required=True, help="new canonical output WAV name on 2:/WG")
    prepare_play.add_argument("--retention", choices=("keep", "temporary"), default="keep")
    prepare_play.add_argument("--rate", type=lambda value: _canonical_uint32_argument(value, positive=True), required=True,
                              help="conservative measured end-to-end producer rate supplied by the caller; not measured by this command")
    prepare_play.add_argument("--margin-frames", type=lambda value: _canonical_uint32_argument(value), required=True,
                              help="conservative producer lead margin in output frames")
    prepare_play.add_argument("--seed", type=int, help="optional uint64 seed override")
    prepare_status = actions.add_parser("prepare-status", help="query or bounded-wait for PREPARE? status")
    prepare_status.add_argument("--wait-timeout", type=float, help="wait for a terminal phase for this many seconds")
    prepare_status.add_argument("--interval", type=float, default=1.0, help="seconds between PREPARE? polls")
    prepare_play_status = actions.add_parser("prepare-play-status", help="query the current/last read-only PREPAREPLAY? status")
    prepare_play_status.add_argument("--wait-timeout", type=float,
                                     help="poll until the current/last finite playback becomes inactive, for at most this many seconds")
    prepare_play_status.add_argument("--interval", type=float, default=1.0, help="seconds between PREPAREPLAY? polls")
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
        if args.action == "cut":
            args.action = "command"
            args.commands = [cut_command(args.cut_action, getattr(args, "entries", None))]
        if args.action == "prepare":
            args.prepare_request = prepare_command(args.source, args.scenario, args.output,
                                                   retention=args.retention, seed=args.seed)
        if args.action == "prepare-play":
            args.prepare_play_request = prepare_play_command(args.source, args.scenario, args.output,
                                                              rate=args.rate, margin_frames=args.margin_frames,
                                                              retention=args.retention, seed=args.seed)
        drain = args.action == "playlist" and args.playlist_action == "drain"
        timeout = args.timeout
        if args.action == "playlist" and not drain:
            if args.playlist_action == "start":
                timeout = max(timeout, PLAYLIST_START_TIMEOUT_S)
            args.commands = [playlist_command(args.playlist_action, getattr(args, "name", None),
                                              continue_on_error=getattr(args, "continue_on_error", False), seq=getattr(args, "seq", None))]
            args.action = "command"
        if drain and (not math.isfinite(args.interval) or args.interval <= 0 or (args.max_polls is not None and args.max_polls < 1)
                      or not math.isfinite(args.reconnect_timeout) or args.reconnect_timeout < 0):
            raise ControlError("--interval must be positive, --max-polls at least 1 and --reconnect-timeout 0 or more")
        if args.action == "prepare-play-status" and (
                (args.wait_timeout is not None and (not math.isfinite(args.wait_timeout) or args.wait_timeout <= 0))
                or not math.isfinite(args.interval) or args.interval <= 0):
            raise ControlError("--wait-timeout and --interval must be finite and positive")
        if args.action == "command":
            for command in args.commands:
                parse_command(command)
        plan = strict_json(args.plan.read_bytes()) if args.action == "replay" else None
        exit_code = 0
        with args.record.open("x", encoding="utf-8", newline="\n") as journal:
            def connect(first_sequence=1):
                return open_session(args.port, args.expected_serial, journal=journal, timeout=timeout,
                                    volume_dismounted=args.host_volume_dismounted, first_sequence=first_sequence,
                                    expected_board=args.expected_board)

            controller = connect()
            try:
                if args.action == "command":
                    for command in args.commands:
                        print(json.dumps(controller.execute(command), allow_nan=False))
                elif args.action == "files":
                    print(json.dumps(list_files(controller), ensure_ascii=False, allow_nan=False))
                elif args.action == "prepare":
                    print(json.dumps(controller.execute(args.prepare_request), allow_nan=False))
                    if args.wait_timeout is not None:
                        result = wait_for_prepare(controller, timeout=args.wait_timeout)
                        print(json.dumps(result, allow_nan=False))
                        if prepare_wait_exit_code(result) != 0:
                            print(f"live control: PREPARE ended in {PREPARE_PHASES[result['prepare']['phase']]}: "
                                  f"{result['prepare']['error']}", file=sys.stderr)
                            exit_code = 4
                elif args.action == "prepare-play":
                    # This ACK confirms only job admission. STATUS? and PREPARE? remain the telemetry sources.
                    print(json.dumps(controller.execute(args.prepare_play_request), allow_nan=False))
                elif args.action == "prepare-status":
                    if args.wait_timeout is None:
                        print(json.dumps(controller.execute("PREPARE?"), allow_nan=False))
                    else:
                        result = wait_for_prepare(controller, timeout=args.wait_timeout, interval=args.interval)
                        print(json.dumps(result, allow_nan=False))
                        if prepare_wait_exit_code(result) != 0:
                            print(f"live control: PREPARE ended in {PREPARE_PHASES[result['prepare']['phase']]}: "
                                  f"{result['prepare']['error']}", file=sys.stderr)
                            exit_code = 4
                elif args.action == "prepare-play-status":
                    if args.wait_timeout is None:
                        print(json.dumps(controller.execute("PREPAREPLAY?"), allow_nan=False))
                    else:
                        result = wait_for_prepare_play(controller, timeout=args.wait_timeout, interval=args.interval)
                        print(json.dumps(result, allow_nan=False))
                        if prepare_play_wait_exit_code(result) != 0:
                            snapshot = result["prepare_play"]
                            print(f"live control: PREPAREPLAY ended without clean completion (state={snapshot['state']}, "
                                  f"status={snapshot['status']}, reason={snapshot['reason']})", file=sys.stderr)
                            exit_code = 4
                elif drain:
                    session = PlaylistDrain(controller, args.blocks, interval=args.interval,
                                            reconnect=connect if args.reconnect_timeout > 0 else None,
                                            reconnect_timeout=args.reconnect_timeout)
                    try:
                        result = session.run(until_done=args.until_done, max_polls=args.max_polls)
                    finally:
                        controller = session.controller
                    print(json.dumps(result, allow_nan=False))
                    if result["ended"] in DRAIN_LOST_ENDS:
                        print(f"live control: drain ended with {result['ended']}: see its links", file=sys.stderr)
                        exit_code = 3
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
                controller.transport.close()
        return exit_code
    except (ControlError, OSError, ValueError) as error:
        print(f"live control: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
