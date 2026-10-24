#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""PLAYLIST files for the RT1170 player: write, check, predict each item, and
cut a battery's capture into one piece per item.

The player's PLAYLIST batch mode (docs/live-control-protocol.md, "PLAYLIST")
plays a playlist file from 2:/WG on the card, one item per line:

  <FILE.WAV> <gap_seconds> <USB|CODEC> [label=<id>] [scn=<NAME.SCN>] [seed=<u32>] [cut=<offset:N:drop|zero,...>] [att=<dB>] [dur=<seconds>]

This tool writes such a file from a JSON list of items, and checks a file by
the board's own rules: a refusal names the board's code and line. With the
cards at hand it also checks each item's WAV (mono 48 kHz packed PCM24) and
predicts what the item's result block will report:

- uac_play_sent_frames, and on the USB route uac_play_file_frames and both
  CRCs: zlib CRC-32 over each channel's 3-byte little-endian samples of the
  frames sent (L = R for the lane's mono cards), the engine's output after
  its cuts (host/cut_schedule.py) and its attenuation (att=, exact integer
  gains from host/attenuation.py, the board's table): the card itself for
  an item without scn= or att=. A codec item's block has no USB play: its
  uac_play_file_frames and CRCs are 0.
- its attenuation: att_tenths_db, in tenths of a dB below full level.
- its duration: dur_frames, and for an item with dur= its looped stream:
  the card again from its frame 0 after its last frame until the duration,
  its cuts at that stream's absolute frames, so the CRCs, frames sent,
  loops (whole passes of the card the frames sent cover) and each seam's
  place in the stream sent (seam_positions, null for a seam a DROP took)
  are predicted; end_cause "end" and engine_deadline_misses 0 for a whole
  play.
- its cuts: uac_play_cut_count, _drop_frames, _zero_frames, _rejected, and
  cut_first_position, the frames from the item's first frame to its first
  cut (the block's uac_play_cut_first_index is uac_play_first_frame_index
  plus that on the USB route, and that alone on the codec route).
- its scenario: scn_crc32, zlib CRC-32 of the scn= file's bytes as the
  cards' folder holds it (0 without scn=), and engine_state, 1 with scn= and
  0 without. An item plays the scenario its scn= names, or none: its WAV's
  own <FILE>.SCN plays no part.
- where a USB item starts: scheduled and start_late (the block's
  scheduled_index set, and its start_late), and start_offset_frames from the
  first frame of run_start_item (see start_predictions()). A scheduled item
  is prepared during the gap before it; the predictions take a gap of at
  least ON_TIME_GAP_MS (1 s) to be long enough for that. After a gap of 0 a
  scheduled item always starts late; after a shorter gap than 1 s it may.

An item with scn= plays its scenario, and its cuts act on the scenario's
output, after the channel. With --render (host signal_lab_render, the
board's engine on the host, which renders bit-exact what the board plays),
an item with scn= is rendered from its card with its seed (a looped item
from its looped card, written to a scratch WAV first: the engine sees one
input across the seams), and its cut= and att= applied to the render: its
samples, length, CRCs and cut counters are then predicted as a clean
card's are, and its expected stream is written with --expected-wavs.
Without --render its samples are not predicted (no CRC), and its length
only when the scenario, read from the cards' folder, has no sample_slip
stage.

A rendered scn= item's prediction also holds engine_status: the STATUS
engine fields (frames in and out, clipped samples, output and source
digests, scenario events) the board reports right after it, as the last
play, from the renderer's summary. Without --render its cut counters are
null too when its scenario slips samples, since the end of its engine
output is not known.

An expected stream is written as a WAV only while it fits one (32-bit
sizes: at most 1431655752 frames, about 8.28 h): a longer clean item is
still predicted, without a WAV, and a longer scn= item cannot be rendered
(signal_lab_render reads and writes WAVs), so check it without --render.

predict gives one item's values, for its whole stream or, with
--sent-frames (the block's uac_play_sent_frames of an item that ended part
way), for that prefix of it: the frames an item sends are always an exact
prefix of its stream, and its seams are those within the prefix.
stream_crc32 is the CRC-32 of the frames sent on either route (a codec
item's block reports no CRC).

compare checks a capture, or a piece split cut from one, against an
expected stream (a mono PCM24 WAV from --expected-wavs, predict
--output-wav or cut_schedule.py predict --output-wav), sample for sample on
every channel: it places the stream by its first non-zero frame (or
--capture-start), reads the capture's format (PCM 16, 24 or 32 bits, or
32-bit float, at the 24-bit scale), compares the first --sent-frames
frames (the whole stream by default) exactly, with --stream-bits 16 for a
16-bit stream (each sample the 24-bit one >> 8), and checks that the
capture is exact silence after them.

split cuts the capture of a whole battery (the lane recorder's, of "Line
(RT1170 Player)") into one WAV piece per USB item that sent frames, by the
result blocks live_control.py playlist drain saved. The capture's first
sound is the first such item's first non-zero frame, which places every
stream index in the capture; each piece holds its item with the silence
around it, in the capture's own format. Beside each piece a status JSON
holds the block, its route as output_route (1), live_events 0 (a playlist
refuses the live controls) and the stream's uac_stream_opens and, over the
battery, usb_stack_asserts and uac_short_packets (USB? after it, less USB?
before it when given): verify_capture --status reads it, and split.json
names each piece's verify_capture arguments, or for a scenario,
attenuated or looped item (which verify_capture 0.2.0 does not check) its
compare arguments.

  python host/make_playlist.py write --items battery.json --output BATTERY.PL
  python host/make_playlist.py check BATTERY.PL [--cards DIR] [--expect EXPECT.json] [--expected-wavs DIR] [--render signal_lab_render]
  python host/make_playlist.py predict BATTERY.PL --cards DIR --item N [--sent-frames S] [--first-frame-index I] [--output-wav W.WAV]
                                       [--render signal_lab_render]
  python host/make_playlist.py split --capture CAP.f32.wav --blocks BLOCKS.jsonl --expect EXPECT.json --cards DIR
                                     --usb-after END.jsonl [--usb-before BEGIN.jsonl] [--run N] --output DIR
  python host/make_playlist.py compare --expected EXPECTED.WAV --capture PIECE.f32.wav [--sent-frames S] [--stream-bits 16|24]
                                       [--capture-start F]

The items file is a JSON array of objects with "file", "gap" (seconds, a
number or a string such as "5" or "0.25"), "route" ("USB" or "CODEC") and
optionally "label", "scn", "seed", "cut" (the entries, as CUT spells them
after "CUT:"), "att" (dB: a number such as -20 or -6.5, or its spelling)
and "dur" (seconds, as "gap"). Standard library only.
"""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass, field
from decimal import Decimal, InvalidOperation
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from typing import BinaryIO, Iterable, Iterator
import zlib

import attenuation
import cut_schedule
import stage_wav


MAX_ITEMS = 256
MAX_LINE_CHARS = 511
MAX_FILE_BYTES = 262144
LABEL_MAX_CHARS = 24
FRAMES_PER_MILLISECOND = 48
CHUNK_FRAMES = 48
MAX_GAP_MILLISECONDS = 3600 * 1000
MIN_DURATION_MILLISECONDS = 1000
MAX_DURATION_MILLISECONDS = 86400 * 1000
# The gap the predictions take as long enough to prepare a scheduled USB item
# (mount, open, the WAV and scenario checks, the prefill) before its index.
ON_TIME_GAP_MS = 1000
UINT32_MAX = (1 << 32) - 1
ROUTES = {"USB": "usb", "CODEC": "codec"}
OPTION_KEYS = ("label", "scn", "seed", "cut", "att", "dur")
SCHEMA = "wfg-playlist.expectations/2"
SPLIT_SCHEMA = "wfg-playlist.split/1"
# live_control.py: a block playlist drain saved, and its journal's records.
BLOCK_SCHEMA = "wfg-live.playlist-block/1"
SESSION_SCHEMA = "wfg-live.session/1"
# USB? stream counters split reads, all since boot.
STREAM_COUNTERS = ("uac_stream_opens", "uac_short_packets", "usb_stack_asserts")
SAMPLE_RATE_HZ = 48_000
# The most silence a piece keeps on each side of its item.
MAX_MARGIN_FRAMES = 2 * SAMPLE_RATE_HZ
COPY_FRAMES = 1 << 16
# The seams of a looped item listed in its prediction; the count is always given.
MAX_SEAMS_LISTED = 10000
PCM24_BYTES = 3
_NAME_STEM = r"[A-Z0-9_]{1,8}"
_WAV = re.compile(_NAME_STEM + r"\.WAV")
_SCN = re.compile(_NAME_STEM + r"\.SCN")
_PLAYLIST_NAME = re.compile(_NAME_STEM + r"(?:\.[A-Z0-9_]{1,3})?")
_LABEL = re.compile(r"[A-Za-z0-9_.\-]{1,%d}" % LABEL_MAX_CHARS)
_DECIMAL = re.compile(r"0|[1-9][0-9]*")
_GAP = re.compile(r"(0|[1-9][0-9]{0,3})(?:\.([0-9]{1,3}))?")
_DURATION = re.compile(r"(0|[1-9][0-9]{0,4})(?:\.([0-9]{1,3}))?")
_SEPARATORS = re.compile(r"[ \t]+")


class PlaylistError(ValueError):
    """A playlist the board refuses: its WFG-LIVE/1 code and the line it names (0: the file as a whole)."""

    def __init__(self, code: str, line: int, message: str):
        super().__init__(f"{code} line {line}: {message}" if line else f"{code}: {message}")
        self.code = code
        self.line = line


@dataclass(frozen=True)
class Item:
    file: str
    gap_frames: int
    route: str                       # "usb" or "codec"
    line: int
    label: str | None = None
    scenario: str | None = None
    seed: int | None = None
    cuts: tuple[cut_schedule.CutEntry, ...] = field(default_factory=tuple)
    attenuation_tenths: int = 0      # att=, tenths of a dB below full level
    duration_frames: int | None = None  # dur=: the card loops until the engine has this many frames

    def spelling(self) -> str:
        """The item's line, as write renders it."""
        words = [self.file, format_gap(self.gap_frames), "USB" if self.route == "usb" else "CODEC"]
        if self.label is not None:
            words.append(f"label={self.label}")
        if self.scenario is not None:
            words.append(f"scn={self.scenario}")
        if self.seed is not None:
            words.append(f"seed={self.seed}")
        if self.cuts:
            words.append("cut=" + ",".join(entry.spelling() for entry in self.cuts))
        if self.attenuation_tenths:
            words.append(f"att={attenuation.spelling(self.attenuation_tenths)}")
        if self.duration_frames is not None:
            words.append(f"dur={format_gap(self.duration_frames)}")
        return " ".join(words)


def is_playlist_name(name: str) -> bool:
    """The playlist's own name: an upper-case 8.3 name whose extension may be left out."""
    return isinstance(name, str) and _PLAYLIST_NAME.fullmatch(name) is not None


def parse_gap(text: str, line: int = 0) -> int:
    """<seconds>[.<up to 3 decimals>], 0 to 3600 s, canonical: the gap in 48 kHz frames."""
    match = _GAP.fullmatch(text)
    if match is None:
        raise PlaylistError("PLAYLIST_GAP", line, f"a gap is 0 to 3600 seconds with up to 3 decimals, not {text!r}")
    milliseconds = int(match.group(1)) * 1000 + int((match.group(2) or "").ljust(3, "0"))
    if milliseconds > MAX_GAP_MILLISECONDS:
        raise PlaylistError("PLAYLIST_GAP", line, f"{text} s is longer than 3600 s")
    return milliseconds * FRAMES_PER_MILLISECOND


def parse_duration(text: str, line: int = 0) -> int:
    """dur=<seconds>: spelt as a gap, 1 to 86400 s: the item's looped input in 48 kHz frames."""
    match = _DURATION.fullmatch(text)
    milliseconds = int(match.group(1)) * 1000 + int((match.group(2) or "").ljust(3, "0")) if match is not None else None
    if milliseconds is None or not MIN_DURATION_MILLISECONDS <= milliseconds <= MAX_DURATION_MILLISECONDS:
        raise PlaylistError("PLAYLIST_DUR", line, f"a duration is 1 to 86400 seconds with up to 3 decimals, not {text!r}")
    return milliseconds * FRAMES_PER_MILLISECOND


def format_gap(frames: int) -> str:
    """A gap's canonical spelling: whole seconds, or the decimals it needs."""
    milliseconds = frames // FRAMES_PER_MILLISECOND
    seconds, rest = divmod(milliseconds, 1000)
    return str(seconds) if rest == 0 else f"{seconds}.{rest:03d}".rstrip("0")


def gap_from_json(value: object, code: str = "PLAYLIST_GAP") -> str:
    """A JSON gap or duration (a number of seconds, or its spelling) as the file spells it; `code` names a refusal."""
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise PlaylistError(code, 0, f"a number of seconds, not {value!r}")
    if isinstance(value, str):
        return value
    try:
        milliseconds = Decimal(str(value)) * 1000
    except InvalidOperation as error:
        raise PlaylistError(code, 0, f"not a number of seconds: {value!r}") from error
    if milliseconds != milliseconds.to_integral_value() or milliseconds < 0:
        raise PlaylistError(code, 0, f"whole milliseconds, not {value!r} s")
    return format_gap(int(milliseconds) * FRAMES_PER_MILLISECOND)


def attenuation_from_json(value: object) -> str:
    """A JSON att (a number of dB such as -20 or -6.5, or its spelling) as the file spells it; the line's parse judges it."""
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise PlaylistError("PLAYLIST_ATT", 0, f"att is a number of dB, not {value!r}")
    return value if isinstance(value, str) else str(Decimal(str(value)))


def _tokens(text: str) -> list[str]:
    """A line's tokens, up to a token that starts a comment."""
    tokens = []
    for token in _SEPARATORS.split(text.strip(" \t")):
        if not token or token.startswith("#"):
            break
        tokens.append(token)
    return tokens


def _parse_cuts(value: str, line: int) -> tuple[cut_schedule.CutEntry, ...]:
    try:
        entries = cut_schedule.parse_entries(value)
    except cut_schedule.CutError as error:
        raise PlaylistError("PLAYLIST_CUT", line, f"cut= is spelt as CUT's entries: {error}") from error
    try:
        return tuple(cut_schedule.validate(entries))
    except cut_schedule.CutError as error:
        raise PlaylistError(error.code, line, str(error)) from error


def _parse_option(token: str, line: int, options: dict[str, object]) -> None:
    key, separator, value = token.partition("=")
    if not separator or key not in OPTION_KEYS:
        raise PlaylistError("PLAYLIST_SYNTAX", line, f"unknown option {token!r}")
    if key in options:
        raise PlaylistError("PLAYLIST_SYNTAX", line, f"{key}= given twice")
    if key == "label":
        if _LABEL.fullmatch(value) is None:
            raise PlaylistError("PLAYLIST_LABEL", line, f"a label is 1 to {LABEL_MAX_CHARS} of A-Z a-z 0-9 _ - ., not {value!r}")
        options[key] = value
    elif key == "scn":
        if _SCN.fullmatch(value) is None:
            raise PlaylistError("PLAYLIST_SCN", line, f"scn= names an upper-case 8.3 .SCN file, not {value!r}")
        options[key] = value
    elif key == "seed":
        if _DECIMAL.fullmatch(value) is None or int(value) > UINT32_MAX:
            raise PlaylistError("PLAYLIST_SEED", line, f"a seed is a canonical 32-bit decimal, not {value!r}")
        options[key] = int(value)
    elif key == "att":
        try:
            options[key] = attenuation.parse(value)
        except attenuation.AttenuationError as error:
            raise PlaylistError("PLAYLIST_ATT", line, str(error)) from error
    elif key == "dur":
        options[key] = parse_duration(value, line)
    else:
        options[key] = _parse_cuts(value, line)


def parse_line(text: str, line: int, *, usb_offered: bool = True) -> Item | None:
    """One line without its LF (and a CR before it): an item, or None for a blank or comment line."""
    if any(not (32 <= ord(character) <= 126 or character == "\t") for character in text):
        raise PlaylistError("PLAYLIST_SYNTAX", line, "only printable ASCII and tabs")
    tokens = _tokens(text)
    if not tokens:
        return None
    if len(tokens) < 3:
        raise PlaylistError("PLAYLIST_SYNTAX", line, "an item is <FILE.WAV> <gap_seconds> <USB|CODEC> [options]")
    file, gap, route = tokens[:3]
    if _WAV.fullmatch(file) is None:
        raise PlaylistError("PLAYLIST_FILE", line, f"an item's file is an upper-case 8.3 .WAV name, not {file!r}")
    frames = parse_gap(gap, line)
    if route not in ROUTES:
        raise PlaylistError("PLAYLIST_ROUTE", line, f"the route is USB or CODEC, not {route!r}")
    if route == "USB" and not usb_offered:
        raise PlaylistError("PLAYLIST_ROUTE_UNAVAILABLE", line, "this image has no USB sound card")
    options: dict[str, object] = {}
    for token in tokens[3:]:
        _parse_option(token, line, options)
    if "seed" in options and "scn" not in options:
        raise PlaylistError("PLAYLIST_SEED_NO_SCN", line, "seed= is the scenario's seed: it needs scn=")
    return Item(file=file, gap_frames=frames, route=ROUTES[route], line=line, label=options.get("label"),
                scenario=options.get("scn"), seed=options.get("seed"), cuts=options.get("cut", ()),
                attenuation_tenths=options.get("att", 0), duration_frames=options.get("dur"))


def parse_bytes(data: bytes, *, usb_offered: bool = True) -> list[Item]:
    """A whole playlist file by the board's rules: its items, or the board's first refusal."""
    if len(data) > MAX_FILE_BYTES:
        raise PlaylistError("PLAYLIST_TOO_LARGE", 0, f"a playlist file is at most {MAX_FILE_BYTES} bytes")
    items: list[Item] = []
    labels: set[str] = set()
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()  # the file ends with an LF
    for number, raw in enumerate(lines, 1):
        if len(raw) > MAX_LINE_CHARS:
            raise PlaylistError("PLAYLIST_LINE_TOO_LONG", number, f"a line is at most {MAX_LINE_CHARS} characters before its LF")
        text = raw[:-1] if raw.endswith(b"\r") else raw
        item = parse_line(text.decode("latin-1"), number, usb_offered=usb_offered)
        if item is None:
            continue
        if len(items) == MAX_ITEMS:
            raise PlaylistError("PLAYLIST_TOO_MANY", number, f"at most {MAX_ITEMS} items")
        if item.label is not None and item.label in labels:
            raise PlaylistError("PLAYLIST_LABEL_DUPLICATE", number, f"label {item.label} is another item's")
        if item.label is not None:
            labels.add(item.label)
        items.append(item)
    if not items:
        raise PlaylistError("PLAYLIST_EMPTY", 0, "no item")
    return items


def items_from_json(entries: object) -> list[str]:
    """The lines of a JSON list of items, each checked as the board checks its line."""
    if not isinstance(entries, list) or not entries:
        raise PlaylistError("PLAYLIST_EMPTY", 0, "the items are a non-empty JSON array")
    lines = []
    for number, entry in enumerate(entries, 1):
        if not isinstance(entry, dict) or not {"file", "gap", "route"} <= set(entry) or not set(entry) <= {"file", "gap", "route", *OPTION_KEYS}:
            raise PlaylistError("PLAYLIST_SYNTAX", number, f"an item has file, gap and route, and only {', '.join(OPTION_KEYS)} besides")
        words = [str(entry["file"]), gap_from_json(entry["gap"]), str(entry["route"])]
        for key in OPTION_KEYS:
            if entry.get(key) is not None:
                value = attenuation_from_json(entry[key]) if key == "att" else gap_from_json(entry[key], "PLAYLIST_DUR") if key == "dur" else entry[key]
                words.append(f"{key}={value}")
        line = " ".join(words)
        parse_line(line, number)
        lines.append(line)
    return lines


def render(lines: Iterable[str], comment: str | None = None) -> bytes:
    """A playlist file: an optional comment line, then one item per line, LF endings."""
    head = [f"# {comment}"] if comment else []
    return ("\n".join(head + list(lines)) + "\n").encode("ascii")


def _scenario_keeps_length(path: Path) -> bool:
    """Whether a scenario file leaves the stream's length alone: no sample_slip stage."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    stages = document.get("impairments", []) if isinstance(document, dict) else []
    return isinstance(stages, list) and all(not isinstance(stage, dict) or stage.get("type") != "sample_slip" for stage in stages)


def render_item(render: Path, cards: Path, item: Item, output: Path, source: Path | None = None) -> dict[str, int]:
    """The item's stream as the board plays it, rendered on the host: its card (or `source`, its looped card) through its scn= with its seed= (a new file).

    Returns the STATUS engine fields of that play (cut_schedule.engine_status()).
    """
    if output.exists():
        raise FileExistsError(f"{output} exists: an expected stream is a new file")
    command = [str(render), "--scenario", str(cards / str(item.scenario)), "--source", str(source or cards / item.file), "--output", str(output)]
    if item.seed is not None:
        command += ["--seed", str(item.seed)]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise PlaylistError("PLAYLIST_SCN_INVALID", item.line, f"{item.scenario} does not render with {item.file}: {completed.stderr.strip()}")
    return cut_schedule.engine_status(completed.stdout)


def stream_facts(path: Path) -> tuple[int, int]:
    """A mono 48 kHz PCM24 WAV's frames and zlib CRC-32 of its samples."""
    offset, frames = cut_schedule.card_data(path)
    crc = 0
    with path.open("rb") as stream:
        stream.seek(offset)
        remaining = frames * PCM24_BYTES
        while remaining:
            data = stream.read(min(remaining, COPY_FRAMES * PCM24_BYTES))
            if not data:
                raise stage_wav.WavError(f"{path}: truncated PCM data")
            crc = zlib.crc32(data, crc)
            remaining -= len(data)
    return frames, crc


def attenuated(data: bytes, gain: int) -> bytes:
    """Little-endian PCM24 samples, each attenuated by a Q1.31 gain (host/attenuation.py); the bytes themselves at 0 dB."""
    if gain == attenuation.UNITY:
        return data
    output = bytearray(len(data))
    for at in range(0, len(data) - len(data) % PCM24_BYTES, PCM24_BYTES):
        sample = int.from_bytes(data[at:at + PCM24_BYTES], "little", signed=True)
        output[at:at + PCM24_BYTES] = (attenuation.apply(sample, gain) & 0xFFFFFF).to_bytes(PCM24_BYTES, "little")
    return bytes(output)


@dataclass(frozen=True)
class EngineOutput:
    """An item's engine output: `frames` frames of a mono PCM24 WAV's data, the WAV's first `period` frames repeated.

    A clean looped item (dur=) repeats its card's frames, frame 0 of each pass
    right after the last of the pass before; a card played once, or a render,
    has `period` equal to `frames`.
    """

    path: Path
    data_offset: int
    period: int
    frames: int

    def runs(self, first: int, count: int) -> Iterator[tuple[int, int]]:
        """(WAV frame, frames) runs covering output frames first .. first + count - 1, split at each pass's end."""
        done = 0
        while done < count:
            at = (first + done) % self.period
            run = min(count - done, self.period - at, COPY_FRAMES)
            yield at, run
            done += run


class OutputReader:
    """An engine output's frames, attenuated: a looped card is held in memory, attenuated once; a render is read as needed."""

    def __init__(self, output: EngineOutput, gain: int, stream: BinaryIO):
        self.output = output
        self.gain = gain
        self.stream = stream
        self.held: bytes | None = None
        if output.period < output.frames:
            stream.seek(output.data_offset)
            self.held = attenuated(stream.read(output.period * PCM24_BYTES), gain)

    def read(self, first: int, count: int) -> Iterator[bytes]:
        for at, run in self.output.runs(first, count):
            if self.held is not None:
                yield self.held[at * PCM24_BYTES:(at + run) * PCM24_BYTES]
                continue
            self.stream.seek(self.output.data_offset + at * PCM24_BYTES)
            data = self.stream.read(run * PCM24_BYTES)
            if len(data) != run * PCM24_BYTES:
                raise stage_wav.WavError(f"{self.output.path}: truncated PCM data")
            yield attenuated(data, self.gain)


def stream_chunks(output: EngineOutput, stream: cut_schedule.CutStream, gain: int, sent: int, wav: BinaryIO) -> Iterator[bytes]:
    """The first `sent` frames the item sends: its engine output with its cuts (zeros for a ZERO), each sample attenuated."""
    reader = OutputReader(output, gain, wav)
    remaining = sent
    for segment in stream.segments:
        frames = min(segment.frames, remaining)
        remaining -= frames
        if segment.card_frame is None:
            for done in range(0, frames, COPY_FRAMES):
                yield bytes(min(COPY_FRAMES, frames - done) * PCM24_BYTES)
        else:
            yield from reader.read(segment.card_frame, frames)
        if remaining == 0:
            return


def seam_positions(stream: cut_schedule.CutStream, period: int, frames: int, sent: int | None = None) -> list[int | None]:
    """Where each seam of a looped item lands in the stream it sends: the frames sent before frame 0 of each pass after the first.

    None for a seam a DROP removed. The seams are those of the engine's input,
    which are its output's unless the item's scenario slips samples. With
    `sent`, only the seams within the first `sent` frames: one whose frame 0
    was sent, or one whose DROP took effect (a frame after the DROP's place
    was sent, or the whole stream for a DROP at its end).
    """
    seams = list(range(period, frames, period))
    positions: list[int | None] = [None] * len(seams)
    places = [stream.frames] * len(seams)
    position = 0
    next_seam = 0
    for segment in stream.segments:
        if segment.card_frame is not None:
            while next_seam < len(seams) and seams[next_seam] < segment.card_frame:
                places[next_seam] = position  # a DROP removed it: its place is where the stream resumes
                next_seam += 1
            while next_seam < len(seams) and seams[next_seam] < segment.card_frame + segment.frames:
                positions[next_seam] = places[next_seam] = position + seams[next_seam] - segment.card_frame
                next_seam += 1
        position += segment.frames
    if sent is None:
        return positions
    whole = sent == stream.frames
    return [seam for seam, place in zip(positions, places) if (place < sent if seam is not None else place < sent or whole)]


def predict_stream(output: EngineOutput, entries: list[cut_schedule.CutEntry], gain: int, *, sent: int | None = None,
                   output_wav: Path | None = None) -> dict[str, object]:
    """cut_schedule.predict() of an engine output (a card, looped or not, or a render), each frame sent attenuated by `gain`.

    The gain is memoryless and leaves zero at zero, so attenuating the cut
    stream is the cut of the attenuated output: the CRCs of any prefix follow.
    """
    stream = cut_schedule.apply(entries, output.frames)
    result = cut_schedule.tally(stream, sent)
    sent_frames = int(result["uac_play_sent_frames"])
    crc = 0
    with Path(output.path).open("rb") as wav, cut_schedule.open_output(output_wav, sent_frames) as expected:
        for data in stream_chunks(output, stream, gain, sent_frames, wav):
            crc = zlib.crc32(data, crc)
            if expected is not None:
                expected.write(data)
        if expected is not None and sent_frames * PCM24_BYTES % 2:
            expected.write(b"\0")  # the RIFF pad byte after an odd-length data chunk
    result["uac_play_crc32_l"] = result["uac_play_crc32_r"] = crc
    result["output_frames"] = output.frames
    result["stream_frames"] = stream.frames
    result["cut_stream"] = stream
    return result


def write_looped(card: Path, frames: int, output: Path) -> None:
    """A new mono PCM24 WAV of `frames` frames of the card looped: the engine input of a looped item."""
    data_offset, period = cut_schedule.card_data(card)
    source = EngineOutput(card, data_offset, period, frames)
    with card.open("rb") as wav, cut_schedule.open_output(output, frames) as looped:
        reader = OutputReader(source, attenuation.UNITY, wav)
        for data in reader.read(0, frames):
            looped.write(data)
        if frames * PCM24_BYTES % 2:
            looped.write(b"\0")


def item_input_frames(item: Item, card_frames: int) -> int:
    """The frames the item feeds the engine: its card once, or its looped card for dur= (the board's play_input_frames())."""
    return item.duration_frames if item.duration_frames is not None and card_frames else card_frames


def predict_item(index: int, item: Item, cards: Path, expected_wavs: Path | None = None, render: Path | None = None,
                 scratch: Path | None = None, sent: int | None = None, output_wav: Path | None = None) -> dict[str, object]:
    """What the item's result block will report, from its card (looped for dur=, with its cuts and its attenuation, or its rendered scenario).

    With `sent`, the values are those of the first `sent` frames the item
    sends: a play that ended part way (STOP, an anomaly) sends an exact prefix.
    """
    card = cards / item.file
    data_offset, frames = cut_schedule.card_data(card)
    length = item_input_frames(item, frames)
    entries = list(item.cuts)
    gain = attenuation.table()[item.attenuation_tenths]
    usb = item.route == "usb"
    result: dict[str, object] = {"item": index, "line": item.line, "label": item.label, "file": item.file, "route": item.route,
                                 "gap_frames": item.gap_frames, "scn": item.scenario, "seed": item.seed,
                                 "cut": cut_schedule.render_command(entries) if entries else None, "card_frames": frames,
                                 "scn_crc32": zlib.crc32((cards / item.scenario).read_bytes()) if item.scenario is not None else 0,
                                 "engine_state": 1 if item.scenario is not None else 0, "att_tenths_db": item.attenuation_tenths,
                                 "dur_frames": item.duration_frames, "input_frames": length}
    output = output_wav
    if output is None and expected_wavs is not None:
        output = expected_wavs / f"{index:03d}-{(item.label or Path(item.file).stem)}.WAV"
    crc: int | None = None
    engine: dict[str, int] | None = None
    if item.scenario is None:
        stream_frames = cut_schedule.apply(entries, length).frames if sent is None else sent
        if output_wav is None and stream_frames > cut_schedule.MAX_WAV_FRAMES:
            output = None  # too long for a WAV: the CRCs are still predicted
        tally = predict_stream(EngineOutput(card, data_offset, frames, length), entries, gain, sent=sent, output_wav=output)
        crc = int(tally["uac_play_crc32_l"])
    elif render is not None and scratch is not None:
        # The engine's output is the render of the item's input, its card or
        # its looped card; the cuts and the attenuation act on it (after the
        # channel). Without either the render is the expected stream itself.
        if length > cut_schedule.MAX_WAV_FRAMES:
            raise ValueError(f"line {item.line}: {item.file} for {length} frames does not fit a WAV (at most {cut_schedule.MAX_WAV_FRAMES} frames, "
                             f"about {cut_schedule.MAX_WAV_FRAMES / SAMPLE_RATE_HZ / 3600:.2f} h), which signal_lab_render needs for its input: "
                             "check this item without --render")
        source = None if length == frames else scratch / f"{index:03d}-INPUT.WAV"
        if source is not None:
            write_looped(card, length, source)
        as_rendered = not entries and gain == attenuation.UNITY and sent is None
        rendered = output if output is not None and as_rendered else scratch / f"{index:03d}.WAV"
        engine = render_item(render, cards, item, rendered, source)
        render_offset, render_frames = cut_schedule.card_data(rendered)
        tally = predict_stream(EngineOutput(rendered, render_offset, render_frames, render_frames), entries, gain, sent=sent,
                               output_wav=None if as_rendered else output)
        crc = int(tally["uac_play_crc32_l"])
    else:
        stream = cut_schedule.apply(entries, length)
        tally = {**cut_schedule.tally(stream, sent), "cut_stream": stream}
        output = None
    samples = crc is not None
    result["samples_predicted"] = samples
    result["expected_wav"] = str(output) if output is not None else None
    keeps_length = samples or _scenario_keeps_length(cards / str(item.scenario))
    result["length_predicted"] = keeps_length
    result["uac_play_sent_frames"] = tally["uac_play_sent_frames"] if keeps_length else None
    result["uac_play_file_frames"] = (tally["uac_play_file_frames"] if keeps_length else None) if usb else 0
    result["uac_play_crc32_l"] = result["uac_play_crc32_r"] = (crc if usb else 0) if samples else None
    result["stream_crc32"] = crc
    # Without the output's length (a sample_slip scenario, not rendered) the
    # entries near its end may apply, clip or count as rejected either way.
    for name in ("uac_play_cut_count", "uac_play_cut_drop_frames", "uac_play_cut_zero_frames", "uac_play_cut_rejected", "cut_first_position"):
        result[name] = tally[name] if keeps_length else None
    # A whole play ends at its end; a prefix may end at any cause.
    result["loops"] = int(tally["uac_play_file_frames"]) // frames if keeps_length else None
    result["end_cause"] = "end" if sent is None else None
    result["engine_deadline_misses"] = 0 if sent is None else None
    result["engine_status"] = engine if sent is None else None
    looped = item.duration_frames is not None and keeps_length
    seams = seam_positions(tally["cut_stream"], frames, length, sent) if looped else []
    result["seam_count"] = len(seams)
    result["seam_positions"] = seams[:MAX_SEAMS_LISTED]
    result["seam_positions_complete"] = len(seams) <= MAX_SEAMS_LISTED
    return result


def whole_chunk_frames(frames: int) -> int:
    """Frames rounded up to whole 48-frame packets."""
    return -(-frames // CHUNK_FRAMES) * CHUNK_FRAMES


def start_predictions(items: list[Item], predictions: list[dict[str, object]]) -> list[dict[str, object]]:
    """Where each USB item starts, as the board places it (docs/live-control-protocol.md, PLAYLIST gaps).

    The board schedules a USB item that follows a USB item which sent frames:
    it is to start at the item before's first frame + its frames sent
    rounded up to whole 48-frame packets + its gap (the block's
    scheduled_index). Any other USB item (the first, one after a codec item,
    one after a USB item that sent nothing) starts when it is ready
    (scheduled_index null). For each USB item:

    - scheduled: whether its block's scheduled_index is set; None when the
      frames the item before sends are not predicted.
    - start_late: the block's start_late. 0 for an item that is not
      scheduled, or that follows a gap of at least ON_TIME_GAP_MS; 1 for a
      scheduled item after a gap of 0 (its index is the end of the item
      before's last packet, already sent once the item is prepared); None
      when it cannot be said (a gap shorter than ON_TIME_GAP_MS, or a gap of
      0 after an item whose frames sent are not predicted).
    - run_start_item, start_offset_frames: the item that begins its run of
      exactly placed items, and the frames from that item's
      uac_play_first_frame_index to this one's. An item that starts when it
      is ready, may start late, or follows an item whose frames sent are not
      predicted begins a run (offset 0); every other item starts exactly
      whole packets of the item before, then its gap, after the item before.

    A codec item has None for all four.
    """
    starts: list[dict[str, object]] = []
    for index, item in enumerate(items):
        if item.route != "usb":
            starts.append({"scheduled": None, "start_late": None, "run_start_item": None, "start_offset_frames": None})
            continue
        before = items[index - 1] if index else None
        sent = predictions[index - 1]["uac_play_sent_frames"] if before is not None and before.route == "usb" else 0
        scheduled = None if sent is None else int(sent) != 0
        gap_ms = before.gap_frames // FRAMES_PER_MILLISECOND if before is not None else 0
        if scheduled is False or gap_ms >= ON_TIME_GAP_MS:
            late: int | None = 0
        elif scheduled is True and gap_ms == 0:
            late = 1
        else:
            late = None
        if scheduled is True and gap_ms >= ON_TIME_GAP_MS and before is not None:
            previous = starts[index - 1]
            run = {"run_start_item": previous["run_start_item"],
                   "start_offset_frames": int(previous["start_offset_frames"]) + whole_chunk_frames(int(sent)) + before.gap_frames}
        else:
            run = {"run_start_item": index, "start_offset_frames": 0}
        starts.append({"scheduled": scheduled, "start_late": late, **run})
    return starts


def expectations(name: str, items: list[Item], cards: Path, expected_wavs: Path | None = None, render: Path | None = None) -> dict[str, object]:
    """Every item's prediction, with where it starts."""
    if expected_wavs is not None:
        expected_wavs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        predictions = [predict_item(index, item, cards, expected_wavs, render, Path(scratch)) for index, item in enumerate(items)]
    for prediction, start in zip(predictions, start_predictions(items, predictions)):
        prediction.update(start)
    return {"schema": SCHEMA, "playlist": name, "on_time_gap_ms": ON_TIME_GAP_MS, "items": predictions}


def predict_prefix(items: list[Item], index: int, cards: Path, *, sent: int | None = None, first_frame_index: int | None = None,
                   output_wav: Path | None = None, render: Path | None = None) -> dict[str, object]:
    """One item's prediction (predict_item()), for the first `sent` frames it sends when given, with its stream indices from `first_frame_index`.

    An item that ended part way, at STOP or an anomaly, sent an exact prefix
    of its stream: its CRCs, cut counters and loops are that prefix's.
    """
    if not 0 <= index < len(items):
        raise ValueError(f"the playlist has items 0 to {len(items) - 1}, not {index}")
    item = items[index]
    _check_cards([item], cards)
    with tempfile.TemporaryDirectory() as scratch:
        result = predict_item(index, item, cards, render=render, scratch=Path(scratch), sent=sent, output_wav=output_wav)
    if first_frame_index is not None and result["uac_play_sent_frames"] is not None:
        sent_frames = int(result["uac_play_sent_frames"])
        first = result["cut_first_position"]
        result["uac_play_cut_first_index"] = 0 if first is None else (first_frame_index + int(first)) & UINT32_MAX
        result["uac_play_last_frame_index"] = (first_frame_index + sent_frames - 1) & UINT32_MAX if sent_frames else 0
    return result


def _check_cards(items: list[Item], cards: Path) -> None:
    """The board's card checks: each WAV opens and is mono 48 kHz PCM24; each scn= exists."""
    for item in items:
        path = cards / item.file
        if not path.is_file():
            raise PlaylistError("PLAYLIST_WAV_MISSING", item.line, f"{item.file} is not in {cards}")
        try:
            stage_wav.inspect_wav(path)
        except stage_wav.WavError as error:
            raise PlaylistError("PLAYLIST_WAV_INVALID", item.line, f"{item.file}: {error}") from error
        if item.scenario is not None and not (cards / item.scenario).is_file():
            raise PlaylistError("PLAYLIST_SCN_MISSING", item.line, f"{item.scenario} is not in {cards}")


def summary(name: str, items: list[Item]) -> dict[str, object]:
    return {"playlist": name, "items": [{"line": item.line, "file": item.file, "gap_frames": item.gap_frames, "route": item.route,
                                         "label": item.label, "scn": item.scenario, "seed": item.seed,
                                         "cut": ",".join(entry.spelling() for entry in item.cuts) or None,
                                         "att_tenths": item.attenuation_tenths, "dur_frames": item.duration_frames} for item in items]}


@dataclass(frozen=True)
class WavLayout:
    """A capture WAV's format chunk and where its frames are: any PCM or float format."""

    fmt: bytes          # the fmt chunk's payload, as the capture holds it
    data_offset: int
    frames: int
    block_align: int
    sample_rate: int


def wav_layout(path: Path) -> WavLayout:
    """A capture WAV by a RIFF walk: its fmt chunk and its data chunk (a data size the recorder never wrote counts to the file's end)."""
    size = path.stat().st_size
    with path.open("rb") as stream:
        head = stream.read(12)
        if len(head) != 12 or head[:4] != b"RIFF" or head[8:] != b"WAVE":
            raise stage_wav.WavError(f"{path}: not a RIFF/WAVE file")
        position, fmt = 12, None
        while position + 8 <= size:
            stream.seek(position)
            chunk_id, chunk_bytes = struct.unpack("<4sI", stream.read(8))
            if chunk_id == b"fmt ":
                fmt = stream.read(chunk_bytes)
                if len(fmt) != chunk_bytes or chunk_bytes < 16:
                    raise stage_wav.WavError(f"{path}: a short fmt chunk")
            elif chunk_id == b"data":
                if fmt is None:
                    raise stage_wav.WavError(f"{path}: its data chunk comes before its fmt chunk")
                available = size - position - 8
                data_bytes = available if chunk_bytes in (0, UINT32_MAX) or chunk_bytes > available else chunk_bytes
                _tag, _channels, rate, _byte_rate, align, _bits = struct.unpack("<HHIIHH", fmt[:16])
                if align == 0:
                    raise stage_wav.WavError(f"{path}: a block align of 0")
                return WavLayout(fmt=fmt, data_offset=position + 8, frames=data_bytes // align, block_align=align, sample_rate=rate)
            position += 8 + chunk_bytes + (chunk_bytes & 1)
    raise stage_wav.WavError(f"{path}: no data chunk")


def first_sound(path: Path, layout: WavLayout, start: int, end: int) -> int | None:
    """The first frame from `start` up to `end` that is not exact silence (a byte other than 0 in any channel), or None."""
    with path.open("rb") as stream:
        frame = start
        while frame < end:
            count = min(end - frame, COPY_FRAMES)
            stream.seek(layout.data_offset + frame * layout.block_align)
            data = stream.read(count * layout.block_align)
            sound = len(data) - len(data.lstrip(b"\0"))
            if sound < len(data):
                return frame + sound // layout.block_align
            frame += count
    return None


def write_piece(capture: Path, layout: WavLayout, start: int, end: int, output: Path) -> None:
    """Frames `start` up to `end` of the capture as a new WAV of its own format."""
    data_bytes = (end - start) * layout.block_align
    fmt = layout.fmt + b"\0" * (len(layout.fmt) % 2)
    header = (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + data_bytes + data_bytes % 2) + b"WAVE" +
              b"fmt " + struct.pack("<I", len(layout.fmt)) + fmt + b"data" + struct.pack("<I", data_bytes))
    with output.open("xb") as piece, capture.open("rb") as source:
        piece.write(header)
        source.seek(layout.data_offset + start * layout.block_align)
        remaining = data_bytes
        while remaining:
            data = source.read(min(remaining, COPY_FRAMES * layout.block_align))
            if not data:
                raise stage_wav.WavError(f"{capture}: ends inside its data chunk")
            piece.write(data)
            remaining -= len(data)
        piece.write(b"\0" * (data_bytes % 2))


# WAVE format tags (mmreg.h, ksmedia.h) and the extensible format's subformat GUIDs' first two bytes.
WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT, WAVE_FORMAT_EXTENSIBLE = 0x0001, 0x0003, 0xFFFE
PCM24_SCALE = 1 << 23


def capture_format(layout: WavLayout) -> tuple[str, int, int]:
    """A capture's sample kind ("pcm" or "float"), bits per sample and channels, from its fmt chunk."""
    tag, channels, _rate, _byte_rate, align, bits = struct.unpack("<HHIIHH", layout.fmt[:16])
    if tag == WAVE_FORMAT_EXTENSIBLE:
        if len(layout.fmt) < 26:
            raise stage_wav.WavError("an extensible fmt chunk without its subformat")
        tag = struct.unpack("<H", layout.fmt[24:26])[0]
    kind = {WAVE_FORMAT_PCM: "pcm", WAVE_FORMAT_IEEE_FLOAT: "float"}.get(tag)
    if kind is None or channels == 0 or align != channels * bits // 8 or (kind, bits) not in (("pcm", 16), ("pcm", 24), ("pcm", 32), ("float", 32)):
        raise stage_wav.WavError(f"format {tag:#06x}, {bits} bits, {channels} channels: compare reads PCM of 16, 24 or 32 bits and 32-bit float")
    return kind, bits, channels


def capture_samples(data: bytes, kind: str, bits: int) -> list[int | None]:
    """Each sample of a capture's frames at the 24-bit scale: PCM16 << 8, PCM24 as it is, PCM32 >> 8 (24 in 32), a float times 2^23;
    None for a float that is no 24-bit sample (not an integer there, or out of range)."""
    if kind == "float":
        values = struct.unpack(f"<{len(data) // 4}f", data)
        scaled = [value * PCM24_SCALE for value in values]
        return [int(value) if math.isfinite(value) and value == int(value) and -PCM24_SCALE <= value < PCM24_SCALE else None for value in scaled]
    width = bits // 8
    samples = [int.from_bytes(data[at:at + width], "little", signed=True) for at in range(0, len(data), width)]
    if bits == 16:
        return [sample << 8 for sample in samples]
    if bits == 32:
        return [sample >> 8 for sample in samples]
    return samples


def as_captured(data: bytes, kind: str, bits: int, channels: int, stream_bits: int) -> bytes:
    """Mono PCM24 samples as a capture in that format holds them, the same sample on every channel: PCM16 its top 16 bits, PCM24 as it is,
    PCM32 << 8, a float the sample over 2^23; with stream_bits 16, each sample with its low 8 bits cleared first (the 16-bit stream's >> 8)."""
    frames = len(data) // PCM24_BYTES
    low, middle, high = data[0::3], data[1::3], data[2::3]
    if stream_bits == 16:
        low = bytes(frames)
    if kind == "float":
        widened = bytearray(4 * frames)  # each sample << 8, as a little-endian int32
        widened[1::4], widened[2::4], widened[3::4] = low, middle, high
        words = array("i")
        words.frombytes(bytes(widened))
        if sys.byteorder != "little":
            words.byteswap()
        floats = array("f", map((1.0 / (1 << 31)).__mul__, words))  # (sample << 8) / 2^31 = sample / 2^23, exact in float32
        if sys.byteorder != "little":
            floats.byteswap()
        packed = floats.tobytes()
        lanes = [packed[byte::4] for byte in range(4)]
    elif bits == 32:
        lanes = [bytes(frames), low, middle, high]
    elif bits == 24:
        lanes = [low, middle, high]
    else:
        lanes = [middle, high]
    width = len(lanes)
    out = bytearray(width * channels * frames)
    for channel in range(channels):
        for byte, lane in enumerate(lanes):
            out[channel * width + byte::width * channels] = lane
    return bytes(out)


def first_mismatch(data: bytes, captured: bytes, kind: str, bits: int, channels: int, stream_bits: int) -> dict[str, int] | None:
    """The first sample of a capture's frames that differs from the expected stream's (`data`, mono PCM24), sample by sample; None when
    every one is equal (a float capture may hold -0.0 for a zero)."""
    got = capture_samples(captured, kind, bits)
    shift = 24 - stream_bits
    for frame in range(len(data) // PCM24_BYTES):
        sample = int.from_bytes(data[frame * PCM24_BYTES:(frame + 1) * PCM24_BYTES], "little", signed=True)
        want = (sample >> shift) << shift
        for channel in range(channels):
            value = got[frame * channels + channel]
            if value != (want >> 8 << 8 if bits == 16 else want):
                return {"frame": frame, "channel": channel, "expected": want, "got": value}
    return None


def first_nonzero_frame(path: Path) -> int | None:
    """The first frame of a mono PCM24 WAV that is not 0, or None."""
    offset, frames = cut_schedule.card_data(path)
    return first_sound(path, WavLayout(fmt=b"", data_offset=offset, frames=frames, block_align=PCM24_BYTES, sample_rate=SAMPLE_RATE_HZ), 0, frames)


def compare_capture(expected: Path, capture: Path, *, sent: int | None = None, stream_bits: int = 24, capture_start: int | None = None) -> dict[str, object]:
    """A capture (or a split piece) against an expected stream, sample for sample on every channel (see the module's text)."""
    if stream_bits not in (16, 24):
        raise ValueError("--stream-bits is 16 or 24")
    _offset, stream_frames = cut_schedule.card_data(expected)
    frames = stream_frames if sent is None else sent
    if not 0 <= frames <= stream_frames:
        raise ValueError(f"--sent-frames must be 0..{stream_frames}, the expected stream's frames")
    layout = wav_layout(capture)
    kind, bits, channels = capture_format(layout)
    if layout.sample_rate != SAMPLE_RATE_HZ:
        raise ValueError(f"{capture} is at {layout.sample_rate} Hz, not 48 kHz")
    if capture_start is None:
        onset = first_nonzero_frame(expected)
        heard = first_sound(capture, layout, 0, layout.frames)
        if onset is None or onset >= frames:
            raise ValueError("the frames to compare are silent: give --capture-start, the capture frame where the stream begins")
        if heard is None:
            raise ValueError(f"{capture} is silent")
        capture_start = heard - onset
    result: dict[str, object] = {"expected": str(expected), "capture": str(capture), "format": {"kind": kind, "bits": bits, "channels": channels},
                                 "stream_bits": stream_bits, "capture_start_frame": capture_start, "frames_compared": frames,
                                 "first_mismatch": None, "silent_after": None, "ok": False}
    if capture_start < 0 or capture_start + frames > layout.frames:
        result["why"] = "the capture does not hold the stream at that place"
        return result
    expected_offset, _ = cut_schedule.card_data(expected)
    done = 0
    with capture.open("rb") as source, expected.open("rb") as stream:
        source.seek(layout.data_offset + capture_start * layout.block_align)
        stream.seek(expected_offset)
        while done < frames and result["first_mismatch"] is None:
            count = min(frames - done, COPY_FRAMES)
            data = stream.read(count * PCM24_BYTES)
            captured = source.read(count * layout.block_align)
            if len(data) != count * PCM24_BYTES or len(captured) != count * layout.block_align:
                raise stage_wav.WavError("a WAV ends inside its data chunk")
            if as_captured(data, kind, bits, channels, stream_bits) != captured:
                mismatch = first_mismatch(data, captured, kind, bits, channels, stream_bits)
                if mismatch is not None:
                    result["first_mismatch"] = {**mismatch, "frame": done + mismatch["frame"]}
            done += count
    result["silent_after"] = first_sound(capture, layout, capture_start + frames, layout.frames) is None
    result["ok"] = result["first_mismatch"] is None and bool(result["silent_after"])
    return result


def usb_snapshot(path: Path) -> dict[str, int]:
    """The stream counters of the last USB? a live_control.py journal recorded an answer to."""
    found = None
    for line in path.read_text(encoding="utf-8").splitlines():
        record = json.loads(line) if line else None
        if not isinstance(record, dict) or record.get("schema") != SESSION_SCHEMA or record.get("command") != "USB?":
            continue
        response = record.get("response")
        if isinstance(response, dict) and response.get("ok") is True and isinstance(response.get("usb"), dict):
            found = response["usb"]
    if found is None:
        raise ValueError(f"{path} holds no answered USB?")
    if not all(isinstance(found.get(name), int) for name in STREAM_COUNTERS):
        raise ValueError(f"{path}: its USB? lacks {', '.join(STREAM_COUNTERS)}")
    return {name: int(found[name]) for name in STREAM_COUNTERS}


# The block members split relies on, and their types.
_BLOCK_FIELDS = {"run": int, "item": int, "file": str, "route": str, "uac_play_sent_frames": int, "uac_play_first_frame_index": int,
                 "uac_play_stream_bits": int}


def saved_blocks(path: Path, playlist: str, run: int | None = None) -> tuple[int, dict[int, dict]]:
    """The result blocks of one run of `playlist` a drain saved, by item: that run (the newest saved, unless given) and its blocks."""
    blocks = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        record = json.loads(line)
        if not isinstance(record, dict) or record.get("schema") != BLOCK_SCHEMA or not isinstance(record.get("block"), dict):
            raise ValueError(f"{path} holds a line that is no saved result block")
        block = record["block"]
        if not all(isinstance(block.get(name), kind) for name, kind in _BLOCK_FIELDS.items()):
            raise ValueError(f"{path}: block {block.get('seq')} lacks {', '.join(_BLOCK_FIELDS)}")
        if record.get("playlist") == playlist:
            blocks.append(block)
    runs = sorted({block["run"] for block in blocks})
    if not runs:
        raise ValueError(f"{path} holds no block of {playlist}")
    chosen = runs[-1] if run is None else run
    by_item: dict[int, dict] = {}
    for block in blocks:
        if block["run"] == chosen:
            if block["item"] in by_item:
                raise ValueError(f"{path} holds item {block['item']} of run {chosen} twice")
            by_item[block["item"]] = block
    if not by_item:
        raise ValueError(f"{path} holds no block of run {chosen} of {playlist}")
    return chosen, by_item


def predicted_onset(expect: dict, cards: Path) -> int | None:
    """The first frame of the item's predicted stream that is not 0: from its expected WAV, or its card (looped for dur=) after its cuts and its attenuation; None unpredicted."""
    if not expect.get("samples_predicted"):
        return None
    wav = Path(str(expect["expected_wav"])) if expect.get("expected_wav") else None
    if wav is not None and wav.is_file():
        offset, frames = cut_schedule.card_data(wav)
        output = EngineOutput(wav, offset, frames, frames)
        stream = cut_schedule.apply([], frames)
        gain = attenuation.UNITY
    elif expect.get("scn") is None:
        path = cards / str(expect["file"])
        offset, frames = cut_schedule.card_data(path)
        length = int(expect.get("input_frames") or frames)
        output = EngineOutput(path, offset, frames, length)
        stream = cut_schedule.apply(cut_schedule.parse_command(str(expect["cut"])) if expect.get("cut") else [], length)
        # An attenuated card may begin with samples the gain takes to zero.
        gain = attenuation.table()[int(expect.get("att_tenths_db") or 0)]
    else:
        return None
    done = 0
    with output.path.open("rb") as source:
        for data in stream_chunks(output, stream, gain, stream.frames, source):
            sound = len(data) - len(data.lstrip(b"\0"))
            if sound < len(data):
                return done + sound // PCM24_BYTES
            done += len(data) // PCM24_BYTES
    return None


def compare_arguments(expect: dict, block: dict, piece: Path, pre_frames: int) -> list[str] | None:
    """The make_playlist.py compare arguments that check a split piece against the item's expected WAV: the frames the block sent, at the piece's own
    place for them (its lead-in), in the stream's format; None without an expected WAV."""
    wav = expect.get("expected_wav")
    if not wav or not Path(str(wav)).is_file():
        return None
    arguments = ["compare", "--expected", str(wav), "--capture", str(piece), "--capture-start", str(pre_frames)]
    if int(block["uac_play_sent_frames"]) != cut_schedule.card_data(Path(str(wav)))[1]:
        arguments += ["--sent-frames", str(block["uac_play_sent_frames"])]
    if block["uac_play_stream_bits"] == 16:
        arguments += ["--stream-bits", "16"]
    return arguments


def _piece_entry(expect: dict, block: dict | None) -> dict[str, object]:
    return {"item": expect["item"], "label": expect["label"], "file": expect["file"], "route": expect["route"],
            "seq": block.get("seq") if block is not None else None, "piece": None, "status": None, "verify_capture": None, "compare": None}


def split_capture(capture: Path, blocks_path: Path, expect_path: Path, cards: Path, output: Path, usb_after: Path,
                  usb_before: Path | None = None, run: int | None = None) -> dict[str, object]:
    """One piece of the capture per USB item that sent frames, a status for each, and split.json naming them (see the module's text)."""
    expected = json.loads(expect_path.read_text(encoding="utf-8"))
    if not isinstance(expected, dict) or expected.get("schema") != SCHEMA or not isinstance(expected.get("items"), list):
        raise ValueError(f"{expect_path} is no {SCHEMA} file (make_playlist.py check --expect)")
    playlist = str(expected["playlist"])
    chosen, blocks = saved_blocks(blocks_path, playlist, run)
    items = expected["items"]
    for expect in items:
        block = blocks.get(expect["item"])
        if block is not None and (block["file"], block["route"], block.get("label")) != (expect["file"], expect["route"], expect["label"]):
            raise ValueError(f"the block of item {expect['item']} names {block['file']}, the expectations {expect['file']}: not one playlist")
    after = usb_snapshot(usb_after)
    before = usb_snapshot(usb_before) if usb_before is not None else None
    if before is not None and before["uac_stream_opens"] != after["uac_stream_opens"]:
        raise ValueError("the capture stream opened again during the battery: its frame indices restarted, so its items cannot be placed in one capture")
    stream = {"uac_stream_opens": after["uac_stream_opens"],
              **{name: after[name] - (before[name] if before is not None else 0) for name in ("uac_short_packets", "usb_stack_asserts")}}
    layout = wav_layout(capture)
    if layout.sample_rate != SAMPLE_RATE_HZ:
        raise ValueError(f"{capture} is at {layout.sample_rate} Hz: the stream's indices count 48 kHz frames")
    sounding = [expect for expect in items if expect["route"] == "usb" and expect["item"] in blocks and blocks[expect["item"]]["uac_play_sent_frames"] > 0]
    if not sounding:
        raise ValueError("no USB item sent a frame")
    anchor = sounding[0]
    anchor_onset = predicted_onset(anchor, cards)
    if anchor_onset is None:
        raise ValueError(f"item {anchor['item']} ({anchor['file']}), the first that sent frames, has no predicted stream to place the capture by: "
                         "check with --render, or begin the battery with a clean card")
    heard = first_sound(capture, layout, 0, layout.frames)
    if heard is None or heard == 0:
        raise ValueError(f"{capture} is silent" if heard is None else f"{capture} begins with sound: start the recorder before PLAYLIST")
    origin = int(blocks[anchor["item"]]["uac_play_first_frame_index"]) + anchor_onset - heard  # the stream index of capture frame 0
    spans = []
    for expect in sounding:
        block = blocks[expect["item"]]
        start = int(block["uac_play_first_frame_index"]) - origin
        spans.append((start, start + int(block["uac_play_sent_frames"])))
        if len(spans) > 1 and spans[-1][0] < spans[-2][1]:
            raise ValueError(f"item {expect['item']} begins before the item before it ends: the blocks are not one stream's")
    output.mkdir(parents=True, exist_ok=True)
    suffix = ".f32.wav" if capture.name.lower().endswith(".f32.wav") else ".wav"
    pieces: dict[int, dict[str, object]] = {}
    for position, expect in enumerate(sounding):
        block = blocks[expect["item"]]
        start, end = spans[position]
        entry = _piece_entry(expect, block)
        pieces[expect["item"]] = entry
        if start < 0 or end > layout.frames:
            entry.update(ok=False, why="the capture does not hold the whole item")
            continue
        space_before = start - (spans[position - 1][1] if position else 0)
        space_after = (spans[position + 1][0] if position + 1 < len(spans) else layout.frames) - end
        pre = min(space_before if position == 0 else space_before // 2, MAX_MARGIN_FRAMES)
        post = min(space_after if position + 1 == len(spans) else space_after // 2, MAX_MARGIN_FRAMES)
        name = f"{expect['item']:03d}-{expect['label'] or Path(str(expect['file'])).stem}"
        piece = output / (name + suffix)
        status_path = output / (name + ".status.json")
        write_piece(capture, layout, start - pre, end + post, piece)
        status = {**block, "output_route": 1, "live_events": 0, **stream}
        with status_path.open("x", encoding="utf-8", newline="\n") as status_file:
            status_file.write(json.dumps(status, indent=1) + "\n")
        onset = predicted_onset(expect, cards)
        heard = first_sound(capture, layout, start, end)
        silent = first_sound(capture, layout, start - pre, start) is None and first_sound(capture, layout, end, end + post) is None
        onset_ok = None if onset is None else heard == start + onset
        arguments = None
        plain = not expect.get("att_tenths_db") and expect.get("dur_frames") is None
        if expect.get("samples_predicted") and expect.get("scn") is None and plain:
            arguments = ["--card", str(cards / str(expect["file"])), "--capture", str(piece), "--status", str(status_path)]
            if block["uac_play_stream_bits"] == 16:
                arguments += ["--expect-truncation-bits", "8"]
            if expect.get("cut"):
                arguments += ["--cuts", str(expect["cut"]), "--usb", str(status_path)]
        entry.update(piece=str(piece), status=str(status_path), capture_start_frame=start, frames=end - start, pre_frames=pre, post_frames=post,
                     onset_ok=onset_ok, silent_around=silent, verify_capture=arguments, compare=None, ok=silent and onset_ok is not False)
        if arguments is None:
            entry["compare"] = compare_arguments(expect, block, piece, pre)
            entry["why"] = ("an attenuated or looped item: verify_capture 0.2.0 checks the card played once at full level" if not plain else
                            "a scenario item: verify_capture 0.2.0 checks clean cards (engine_state 0)")
            entry["why"] += ("; make_playlist.py compare with these arguments checks the piece against its expected_wav" if entry["compare"] is not None else
                             "; its stream has no expected_wav (check --expected-wavs, with --render for a scenario item)")
    entries = []
    for expect in items:
        if expect["item"] in pieces:
            entries.append(pieces[expect["item"]])
            continue
        block = blocks.get(expect["item"])
        entry = _piece_entry(expect, block)
        entry["why"] = ("no block" if block is None else "a codec item: not in the USB capture" if expect["route"] != "usb" else "it sent no frame")
        entries.append(entry)
    manifest = {"schema": SPLIT_SCHEMA, "capture": str(capture), "playlist": playlist, "run": chosen, "anchor_item": anchor["item"],
                "origin_stream_index": origin, "stream": stream, "stream_counted_from": "USB? before" if before is not None else "boot",
                "items": entries, "ok": all(entry.get("ok", True) for entry in entries)}
    with (output / "split.json").open("x", encoding="utf-8", newline="\n") as manifest_file:
        manifest_file.write(json.dumps(manifest, indent=2) + "\n")
    return manifest


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    actions = parser.add_subparsers(dest="action", required=True)
    write = actions.add_parser("write", help="a playlist file from a JSON list of items, checked by the board's rules")
    write.add_argument("--items", type=Path, required=True, help="a JSON array of items")
    write.add_argument("--output", type=Path, required=True, help="the new playlist file; its name is the one PLAYLIST takes")
    write.add_argument("--comment", help="a comment line at the top")
    write.add_argument("--force", action="store_true", help="replace an existing output file")
    check = actions.add_parser("check", help="check a playlist file, and predict each item from its card")
    check.add_argument("playlist", type=Path)
    check.add_argument("--cards", type=Path, help="the folder holding the items' WAVs and scenarios (the card's 2:/WG)")
    check.add_argument("--expect", type=Path, help="a new JSON file of every item's predicted block values (needs --cards)")
    check.add_argument("--expected-wavs", type=Path, help="a folder for each predicted item's expected stream as a WAV (needs --cards)")
    check.add_argument("--render", type=Path, help="signal_lab_render: predict each scn= item from its rendered scenario, its cut= applied to the render (needs --cards)")
    check.add_argument("--no-usb", action="store_true", help="check for an image without the USB sound card")
    forecast = actions.add_parser("predict", help="one item's block values, for its whole stream or the first --sent-frames frames of it")
    forecast.add_argument("playlist", type=Path)
    forecast.add_argument("--cards", type=Path, required=True, help="the folder holding the items' WAVs and scenarios (the card's 2:/WG)")
    forecast.add_argument("--item", type=int, required=True, help="the item's index in the file, from 0 (the block's item)")
    forecast.add_argument("--sent-frames", type=int, help="uac_play_sent_frames of an item that ended part way: the values of that prefix")
    forecast.add_argument("--first-frame-index", type=int, help="uac_play_first_frame_index, for the stream indices")
    forecast.add_argument("--output-wav", type=Path, help="a new WAV of the frames sent")
    forecast.add_argument("--render", type=Path, help="signal_lab_render, for an item with scn=")
    forecast.add_argument("--no-usb", action="store_true", help="check for an image without the USB sound card")
    split = actions.add_parser("split", help="cut a battery's capture into one piece per USB item, each with a status verify_capture reads")
    split.add_argument("--capture", type=Path, required=True, help="the recorder's capture of the whole battery (its .f32.wav)")
    split.add_argument("--blocks", type=Path, required=True, help="the result blocks live_control.py playlist drain saved")
    split.add_argument("--expect", type=Path, required=True, help="the predictions check --expect wrote for the playlist")
    split.add_argument("--cards", type=Path, required=True, help="the folder holding the items' WAVs (the card's 2:/WG)")
    split.add_argument("--usb-after", type=Path, required=True, help="a live_control.py journal of USB? read after the battery")
    split.add_argument("--usb-before", type=Path, help="one of USB? read before PLAYLIST: the stream counters then count the battery alone")
    split.add_argument("--run", type=int, help="the playlist run (PLAYLIST? run) to cut; the newest the blocks hold by default")
    split.add_argument("--output", type=Path, required=True, help="a folder for the pieces, their statuses and split.json (new files)")
    compare = actions.add_parser("compare", help="a capture or a split piece against an expected stream, sample for sample on every channel")
    compare.add_argument("--expected", type=Path, required=True, help="the expected stream: a mono PCM24 WAV (--expected-wavs, predict --output-wav)")
    compare.add_argument("--capture", type=Path, required=True, help="the capture, or a piece split cut from it, in its own format")
    compare.add_argument("--sent-frames", type=int, help="uac_play_sent_frames of an item that ended part way: compare that prefix")
    compare.add_argument("--stream-bits", type=int, choices=(16, 24), default=24, help="the stream's bits (the block's uac_play_stream_bits)")
    compare.add_argument("--capture-start", type=int, help="the capture frame where the stream begins (default: placed by its first sound)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.action == "write":
            if not is_playlist_name(args.output.name):
                raise PlaylistError("PLAYLIST_NAME", 0, f"PLAYLIST takes an upper-case 8.3 name, not {args.output.name!r}")
            data = render(items_from_json(json.loads(args.items.read_text(encoding="utf-8"))), args.comment)
            items = parse_bytes(data)
            with args.output.open("wb" if args.force else "xb") as output:
                output.write(data)
            print(json.dumps(summary(args.output.name, items)))
            return 0
        if args.action == "split":
            manifest = split_capture(args.capture, args.blocks, args.expect, args.cards, args.output, args.usb_after, args.usb_before, args.run)
            print(json.dumps(manifest))
            return 0 if manifest["ok"] else 1
        if args.action == "compare":
            result = compare_capture(args.expected, args.capture, sent=args.sent_frames, stream_bits=args.stream_bits, capture_start=args.capture_start)
            print(json.dumps(result))
            return 0 if result["ok"] else 1
        if args.action == "predict":
            items = parse_bytes(args.playlist.read_bytes(), usb_offered=not args.no_usb)
            print(json.dumps(predict_prefix(items, args.item, args.cards, sent=args.sent_frames, first_frame_index=args.first_frame_index,
                                            output_wav=args.output_wav, render=args.render)))
            return 0
        if (args.expect or args.expected_wavs or args.render) and args.cards is None:
            raise ValueError("--expect, --expected-wavs and --render need --cards")
        items = parse_bytes(args.playlist.read_bytes(), usb_offered=not args.no_usb)
        if args.cards is None:
            print(json.dumps(summary(args.playlist.name, items)))
            return 0
        _check_cards(items, args.cards)
        predicted = expectations(args.playlist.name, items, args.cards, args.expected_wavs, args.render)
        if args.expect is not None:
            with args.expect.open("x", encoding="utf-8", newline="\n") as output:
                output.write(json.dumps(predicted, indent=2) + "\n")
        print(json.dumps(predicted))
        return 0
    except (PlaylistError, cut_schedule.CutError, stage_wav.WavError, ValueError, OSError) as error:
        print(f"make playlist: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
