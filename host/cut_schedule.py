#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""CUT schedules for the RT1170 player: generate, check, render, predict.

The player's CUT injector (docs/live-control-protocol.md, "CUT injector")
applies a per-play schedule to the output of the scenario engine, after the
channel: the engine processes every card frame, DROP N discards N of its
output frames (the capture is N frames shorter; engine time runs on through
them), ZERO N sends N exact zero frames before an output frame (the capture
is N frames longer). Offsets count the engine's output frames from the
play's first; a play without a scenario puts out the card itself, so for a
clean card they are its frames. The board only executes the list; random
schedules are made here, from a seed, and this module predicts what the
board reports for them, so the lane and the bench can check a capture and
the USB? values without the board.

  python host/cut_schedule.py generate --seed 7 --card CARD.WAV --count 8 --mix mixed --min-spacing 48000
  python host/cut_schedule.py render 48000:480:zero,96000:480:drop
  python host/cut_schedule.py predict --card CARD.WAV --cut CUT:48000:480:zero [--sent-frames N]
      [--first-frame-index I] [--output-wav EXPECTED.WAV]
      [--scenario X.SCN --render signal_lab_render [--seed N] [--render-wav RENDER.WAV]]

The prediction follows the firmware exactly: it applies the schedule to the
engine's output, which is the card for a clean play and, with --scenario, the
host render of the card through its scenario (host/render/signal_lab_render,
the board's engine). Entries at or past the output's end are ignored, and
counted once the whole stream has been sent; a DROP that runs past the end is
clipped there; the CRCs are zlib CRC-32 over each channel's 3-byte
little-endian samples of the frames sent (L = R for the lane's mono cards).
For a play that ended part way (STOP, a stream close, a stall), pass the
frames it sent (uac_play_sent_frames): the values are then those of that
prefix. Standard library only.
"""

from __future__ import annotations

import argparse
import contextlib
from dataclasses import dataclass
import json
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
from typing import BinaryIO, Iterator
import zlib

import stage_wav


MAX_ENTRIES = 16
MIN_LENGTH = 1
MAX_LENGTH = 4800
# The drop balance (common/cut_schedule.hpp max_drop_balance_frames): each
# DROP adds its length, the output frames between one DROP and the next take
# it back, never below zero; a DROP costs the board engine time with nothing
# for its output FIFO, whose margin covers this much.
MAX_DROP_BALANCE = 3 * MAX_LENGTH
UINT32_MAX = (1 << 32) - 1
KINDS = ("drop", "zero")
COMMAND = "CUT"
BYTES_PER_FRAME = 3
SAMPLE_RATE_HZ = 48_000
# The most frames a mono PCM24 WAV holds: its RIFF size (36 + data + pad
# byte) is 32 bits. 1431655752 frames, about 8.28 h at 48 kHz.
MAX_WAV_FRAMES = (UINT32_MAX - 37) // BYTES_PER_FRAME
# The firmware's reply codes for a schedule it refuses.
CUT_RANGE = "CUT_RANGE"
CUT_UNSORTED = "CUT_UNSORTED"
CUT_OVERLAP = "CUT_OVERLAP"
CUT_TOO_MANY = "CUT_TOO_MANY"
CUT_DROP_RUN = "CUT_DROP_RUN"
BAD_REQUEST = "BAD_REQUEST"
_DECIMAL = re.compile(r"0|[1-9][0-9]*")
_COPY_BLOCK_FRAMES = 1 << 16


class CutError(ValueError):
    """A schedule the board would refuse; code is the board's reply code."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code


class RenderError(ValueError):
    """The host renderer refused a card or a scenario."""


# The summary line signal_lab_render prints after a render.
_RENDER_SUMMARY = re.compile(r" frames_in=(?P<frames_in>\d+) frames_out=(?P<frames_out>\d+) clipped=(?P<clipped>\d+) peak_dbfs=\S+"
                             r" digest=(?P<digest>[0-9a-f]{16}) source_digest=(?P<source_digest>[0-9a-f]{16}) stages=\d+"
                             r" events=(?P<scheduled>\d+)/(?P<applied>\d+)/(?P<dropped>\d+) arena=\d+$")


def engine_status(summary: str) -> dict[str, int]:
    """The STATUS engine fields the board reports after it plays what signal_lab_render rendered, from the renderer's summary line.

    The board's engine is the renderer's, so its frames, clipped samples,
    output and source digests (each split into _hi and _lo 32-bit halves)
    and scenario events are the render's. The output digest is the engine's
    own, before any cut or attenuation.
    """
    lines = summary.strip().splitlines()
    match = _RENDER_SUMMARY.search(lines[-1]) if lines else None
    if match is None:
        raise RenderError(f"signal_lab_render printed no summary line: {summary.strip()[-200:]!r}")
    digest = int(match["digest"], 16)
    source = int(match["source_digest"], 16)
    return {"engine_frames_in": int(match["frames_in"]), "engine_frames_out": int(match["frames_out"]), "engine_clipped": int(match["clipped"]),
            "engine_digest_hi": digest >> 32, "engine_digest_lo": digest & UINT32_MAX,
            "engine_source_digest_hi": source >> 32, "engine_source_digest_lo": source & UINT32_MAX,
            "engine_events_applied": int(match["applied"]), "engine_events_dropped": int(match["dropped"])}


@dataclass(frozen=True)
class CutEntry:
    offset: int  # engine output frames from the play's first (a clean card's own frames)
    length: int  # DROP: output frames discarded; ZERO: zero frames sent
    kind: str    # "drop" or "zero"

    def covered_end(self) -> int:
        """The output frame just past those the entry covers (a ZERO covers none)."""
        return self.offset + (self.length if self.kind == "drop" else 0)

    def spelling(self) -> str:
        return f"{self.offset}:{self.length}:{self.kind}"


def _uint32(token: str) -> int:
    if not _DECIMAL.fullmatch(token) or int(token) > UINT32_MAX:
        raise CutError(BAD_REQUEST, f"not a canonical 32-bit decimal: {token!r}")
    return int(token)


def parse_entries(text: str) -> list[CutEntry]:
    """Entries spelled <offset>:<length>:<drop|zero>, separated by commas, as given."""
    if not isinstance(text, str) or not text:
        raise CutError(BAD_REQUEST, "no entries")
    entries = []
    for item in text.split(","):
        fields = item.split(":")
        if len(fields) != 3 or fields[2] not in KINDS:
            raise CutError(BAD_REQUEST, f"an entry is <offset>:<length>:<drop|zero>, not {item!r}")
        entries.append(CutEntry(_uint32(fields[0]), _uint32(fields[1]), fields[2]))
    if len(entries) > MAX_ENTRIES:
        raise CutError(BAD_REQUEST, f"at most {MAX_ENTRIES} entries in one CUT")
    return entries


def parse_command(text: str) -> list[CutEntry]:
    """The entries of a CUT command: none for "CUT", else "CUT:" and its entries."""
    if text == COMMAND:
        return []
    if not isinstance(text, str) or not text.startswith(COMMAND + ":"):
        raise CutError(BAD_REQUEST, "a CUT command is CUT or CUT:<entries>")
    return parse_entries(text[len(COMMAND) + 1:])


def drop_balance(entries: list[CutEntry]) -> int:
    """The highest drop balance sorted, non-overlapping entries reach: each DROP adds its length, the output frames before the next DROP take it back, never below zero."""
    balance = highest = 0
    previous_end = 0
    for entry in entries:
        if entry.kind != "drop":
            continue
        balance = max(0, balance - max(0, entry.offset - previous_end)) + entry.length
        highest = max(highest, balance)
        previous_end = entry.covered_end()
    return highest


def validate(entries: list[CutEntry]) -> list[CutEntry]:
    """The board's rules: at most 16 entries, lengths 1 to 4800, sorted, non-overlapping, within the drop balance.

    At one offset a ZERO may precede a DROP (an erasure: zeros in place of the
    dropped frames); two ZEROs never share an offset. The DROPs keep the drop
    balance at or below MAX_DROP_BALANCE: three adjacent DROPs of 4800 frames
    at most, or more with output frames between them. Offsets past the
    output's end are not judged here: the board ignores those entries and
    counts them.
    """
    if len(entries) > MAX_ENTRIES:
        raise CutError(CUT_TOO_MANY, f"at most {MAX_ENTRIES} entries")
    for entry in entries:
        if not MIN_LENGTH <= entry.length <= MAX_LENGTH:
            raise CutError(CUT_RANGE, f"length {entry.length} is outside {MIN_LENGTH}..{MAX_LENGTH}")
    for earlier, later in zip(entries, entries[1:]):
        if later.offset < earlier.offset:
            raise CutError(CUT_UNSORTED, f"{later.spelling()} comes after {earlier.spelling()}")
        if later.offset < earlier.covered_end() or (earlier.kind == later.kind == "zero" and later.offset == earlier.offset):
            raise CutError(CUT_OVERLAP, f"{later.spelling()} overlaps {earlier.spelling()}")
    balance = drop_balance(entries)
    if balance > MAX_DROP_BALANCE:
        raise CutError(CUT_DROP_RUN, f"the DROPs reach a drop balance of {balance} frames, more than {MAX_DROP_BALANCE}: "
                                     "space them with output frames between them")
    return entries


def render_command(entries: list[CutEntry]) -> str:
    """The CUT command for a valid schedule; "CUT" alone for none."""
    validate(entries)
    if not entries:
        return COMMAND
    return COMMAND + ":" + ",".join(entry.spelling() for entry in entries)


def generate(seed: int, *, file_frames: int, count: int, min_length: int = MIN_LENGTH, max_length: int = MAX_LENGTH,
             zero_fraction: float = 0.5, min_spacing: int = 0, start: int = 0) -> list[CutEntry]:
    """A random valid schedule, the same for the same arguments.

    `count` entries, each ZERO with probability `zero_fraction` (else DROP),
    lengths uniform in min_length..max_length, all between card frame `start`
    and the card's end with at least `min_spacing` card frames between one
    entry's end and the next one's offset; a ZERO after a ZERO is at least one
    card frame later, as the board refuses two ZEROs at one offset. Every DROP
    lies wholly inside the card, so none is clipped. A draw whose DROPs pass
    the drop balance is refused (CUT_DROP_RUN): a larger --min-spacing
    avoids that. Python's random.Random(seed).
    """
    if not 0 <= count <= MAX_ENTRIES:
        raise CutError(CUT_TOO_MANY, f"count must be 0..{MAX_ENTRIES}")
    if not MIN_LENGTH <= min_length <= max_length <= MAX_LENGTH:
        raise CutError(CUT_RANGE, f"lengths must satisfy {MIN_LENGTH} <= min <= max <= {MAX_LENGTH}")
    if not 0.0 <= zero_fraction <= 1.0 or min_spacing < 0 or start < 0:
        raise ValueError("zero_fraction must be 0..1, and min_spacing and start not negative")
    rng = random.Random(seed)
    kinds = ["zero" if rng.random() < zero_fraction else "drop" for _ in range(count)]
    lengths = [rng.randint(min_length, max_length) for _ in range(count)]
    covered = sum(length for kind, length in zip(kinds, lengths) if kind == "drop")
    # Card frames between each entry's end and the next one's offset.
    spacings = [max(min_spacing, 1) if earlier == later == "zero" else min_spacing for earlier, later in zip(kinds, kinds[1:])]
    # The last entry's offset must be a card frame; spacing separates neighbours.
    slack = file_frames - start - covered - sum(spacings) - 1
    if count and slack < 0:
        raise ValueError("the card is too short for that many cuts at that spacing")
    gaps = sorted(rng.randint(0, slack) for _ in range(count))
    entries = []
    position = start
    previous_gap = 0
    for kind, length, gap, spacing in zip(kinds, lengths, gaps, spacings + [0]):
        position += gap - previous_gap
        previous_gap = gap
        entries.append(CutEntry(position, length, kind))
        position += (length if kind == "drop" else 0) + spacing
    return validate(entries)


@dataclass(frozen=True)
class Segment:
    """A run of the cut stream: output frames from `card_frame`, or zeros when None."""

    card_frame: int | None
    frames: int


@dataclass(frozen=True)
class AppliedCut:
    position: int  # the cut stream's frames before it
    frames: int    # DROP: output frames discarded (clipped at the end); ZERO: zero frames
    kind: str


@dataclass(frozen=True)
class CutStream:
    """What a play with a schedule sends, as the board's cut stage builds it from the engine's output."""

    segments: list[Segment]
    applied: list[AppliedCut]
    rejected: int
    frames: int


def apply(entries: list[CutEntry], file_frames: int) -> CutStream:
    """The cut stream of an engine output of `file_frames` frames (a clean card's) under a valid schedule."""
    validate(entries)
    segments: list[Segment] = []
    applied: list[AppliedCut] = []
    rejected = 0
    at = 0
    sent = 0
    for entry in entries:
        if entry.offset >= file_frames:
            rejected += 1
            continue
        if entry.offset > at:
            segments.append(Segment(at, entry.offset - at))
            sent += entry.offset - at
        at = entry.offset
        if entry.kind == "zero":
            applied.append(AppliedCut(sent, entry.length, "zero"))
            segments.append(Segment(None, entry.length))
            sent += entry.length
        else:
            skipped = min(entry.length, file_frames - at)
            applied.append(AppliedCut(sent, skipped, "drop"))
            at += skipped
    if at < file_frames:
        segments.append(Segment(at, file_frames - at))
        sent += file_frames - at
    return CutStream(segments, applied, rejected, sent)


def tally(stream: CutStream, sent: int | None = None) -> dict[str, object]:
    """The cuts that took effect within the first `sent` frames (all by default).

    A cut takes effect once a frame at or after its place was sent; a DROP that
    runs to the output's end, once the whole stream was sent. A ZERO counts the
    zeros sent of it. The entries past the output's end count once the whole
    stream was sent: only then is the end within the frames sent.
    uac_play_file_frames is the output frames the sent frames cover (the
    card's frames for a clean card).
    """
    sent = stream.frames if sent is None else sent
    if not 0 <= sent <= stream.frames:
        raise ValueError(f"sent frames must be 0..{stream.frames}")
    count = drop_frames = zero_frames = 0
    first = None
    for cut in stream.applied:
        if not (sent > cut.position or (sent == stream.frames and cut.position == stream.frames)):
            continue
        count += 1
        first = cut.position if first is None else first
        if cut.kind == "drop":
            drop_frames += cut.frames
        else:
            zero_frames += min(cut.frames, sent - cut.position)
    return {"uac_play_sent_frames": sent, "uac_play_file_frames": sent - zero_frames + drop_frames,
            "uac_play_cut_count": count, "uac_play_cut_drop_frames": drop_frames,
            "uac_play_cut_zero_frames": zero_frames, "cut_first_position": first,
            "uac_play_cut_rejected": stream.rejected if sent == stream.frames else 0}


def card_data(path: Path) -> tuple[int, int]:
    """The data chunk's offset and frame count of a mono 48 kHz PCM24 card."""
    facts = stage_wav.inspect_wav(Path(path))
    return int(facts["data_offset"]), int(facts["data_bytes"]) // BYTES_PER_FRAME


def stream_bytes(stream: CutStream, card: BinaryIO, data_offset: int, sent: int) -> Iterator[bytes]:
    """The first `sent` frames of the cut stream as the card stores them, in blocks."""
    remaining = sent
    for segment in stream.segments:
        frames = min(segment.frames, remaining)
        remaining -= frames
        done = 0
        while done < frames:
            block = min(frames - done, _COPY_BLOCK_FRAMES)
            if segment.card_frame is None:
                yield bytes(block * BYTES_PER_FRAME)
            else:
                card.seek(data_offset + (segment.card_frame + done) * BYTES_PER_FRAME)
                data = card.read(block * BYTES_PER_FRAME)
                if len(data) != block * BYTES_PER_FRAME:
                    raise stage_wav.WavError("truncated PCM data")
                yield data
            done += block
        if remaining == 0:
            return


def predict(card_path: Path, entries: list[CutEntry], *, sent: int | None = None,
            first_frame_index: int | None = None, output_wav: Path | None = None) -> dict[str, object]:
    """The values USB? reports for a play of the card with the schedule.

    `card_path` is the engine's output: the card of a clean play, or the host
    render of a scenario play (predict_render()). With `first_frame_index`
    (USB? uac_play_first_frame_index), the stream indices too:
    uac_play_cut_first_index and uac_play_last_frame_index. `output_wav`
    receives the frames sent, as a mono PCM24 WAV.
    """
    data_offset, file_frames = card_data(card_path)
    stream = apply(entries, file_frames)
    result = tally(stream, sent)
    sent_frames = int(result["uac_play_sent_frames"])
    crc = 0
    with Path(card_path).open("rb") as card, open_output(output_wav, sent_frames) as output:
        for data in stream_bytes(stream, card, data_offset, sent_frames):
            crc = zlib.crc32(data, crc)
            if output is not None:
                output.write(data)
        if output is not None and sent_frames * BYTES_PER_FRAME % 2:
            output.write(b"\0")  # the RIFF pad byte after an odd-length data chunk
    result["uac_play_crc32_l"] = crc
    result["uac_play_crc32_r"] = crc
    result["card_frames"] = file_frames
    result["output_frames"] = file_frames
    result["stream_frames"] = stream.frames
    if first_frame_index is not None:
        first = result["cut_first_position"]
        result["uac_play_cut_first_index"] = 0 if first is None else (first_frame_index + int(first)) & UINT32_MAX
        result["uac_play_last_frame_index"] = (first_frame_index + sent_frames - 1) & UINT32_MAX if sent_frames else 0
    return result


def render_output(render: Path, card: Path, scenario: Path, output: Path, seed: int | None = None) -> dict[str, int]:
    """The engine's output of a scenario play, as a new mono PCM24 WAV: the card
    through the scenario, rendered by host/render/signal_lab_render (with
    `seed`, the seed a PLAYLIST item's seed= gives). Returns the STATUS engine
    fields of that play (engine_status())."""
    if Path(output).exists():
        raise FileExistsError(f"{output} exists: a render is a new file")
    command = [str(render), "--scenario", str(scenario), "--source", str(card), "--output", str(output)]
    if seed is not None:
        command += ["--seed", str(seed)]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise RenderError(f"{scenario} does not render with {card}: {completed.stderr.strip()}")
    return engine_status(completed.stdout)


def predict_render(card_path: Path, scenario: Path, render: Path, entries: list[CutEntry], *, seed: int | None = None, sent: int | None = None,
                   first_frame_index: int | None = None, output_wav: Path | None = None, render_wav: Path | None = None) -> dict[str, object]:
    """predict() for a scenario play: the schedule applied to the engine's
    output, the host render of the card through `scenario` (render_output()).

    `render_wav` keeps the render (a new file). card_frames is the card's frame
    count, output_frames the render's; engine_status holds the STATUS engine
    fields the board reports after the play (engine_status()).
    """
    with tempfile.TemporaryDirectory() as folder:
        rendered = Path(render_wav) if render_wav is not None else Path(folder) / "RENDER.WAV"
        status = render_output(render, card_path, scenario, rendered, seed)
        result = predict(rendered, entries, sent=sent, first_frame_index=first_frame_index, output_wav=output_wav)
    result["card_frames"] = card_data(card_path)[1]
    result["engine_status"] = status
    return result


@contextlib.contextmanager
def open_output(path: Path | None, frames: int) -> Iterator[BinaryIO | None]:
    """A new mono 48 kHz PCM24 WAV for `frames` frames, its header written; None for no path."""
    if path is None:
        yield None
        return
    if frames > MAX_WAV_FRAMES:
        raise ValueError(f"{path}: {frames} frames do not fit a WAV, whose sizes are 32 bits: at most {MAX_WAV_FRAMES} frames "
                         f"({MAX_WAV_FRAMES / SAMPLE_RATE_HZ / 3600:.2f} h at 48 kHz)")
    data_bytes = frames * BYTES_PER_FRAME
    with Path(path).open("xb") as output:
        output.write(struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + data_bytes + data_bytes % 2, b"WAVE", b"fmt ", 16, 1, 1,
                                 SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * BYTES_PER_FRAME, BYTES_PER_FRAME, 24, b"data", data_bytes))
        yield output


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    actions = parser.add_subparsers(dest="action", required=True)
    make = actions.add_parser("generate", help="a seeded random schedule and its CUT command")
    make.add_argument("--seed", type=int, required=True)
    size = make.add_mutually_exclusive_group(required=True)
    size.add_argument("--card", type=Path, help="the card the schedule is for")
    size.add_argument("--file-frames", type=int, help="the card's frame count")
    make.add_argument("--count", type=int, required=True, help=f"entries, 0..{MAX_ENTRIES}")
    make.add_argument("--min-length", type=int, default=MIN_LENGTH)
    make.add_argument("--max-length", type=int, default=MAX_LENGTH)
    mix = make.add_mutually_exclusive_group()
    mix.add_argument("--mix", choices=("drop", "zero", "mixed"), default="mixed")
    mix.add_argument("--zero-fraction", type=float, help="probability that an entry is a ZERO")
    make.add_argument("--min-spacing", type=int, default=0, help="card frames between one entry's end and the next")
    make.add_argument("--start", type=int, default=0, help="the first card frame an entry may sit at")
    make.add_argument("--json", action="store_true", help="print the entries and the command as JSON")
    render = actions.add_parser("render", help="check entries and print their CUT command")
    render.add_argument("entries", help="<offset>:<length>:<drop|zero>,...")
    forecast = actions.add_parser("predict", help="the USB? values a play of the card with the schedule reports")
    forecast.add_argument("--card", type=Path, required=True)
    forecast.add_argument("--cut", required=True, help="the CUT command: CUT or CUT:<entries>")
    forecast.add_argument("--sent-frames", type=int, help="uac_play_sent_frames of a play that ended part way")
    forecast.add_argument("--first-frame-index", type=int, help="uac_play_first_frame_index, for the stream indices")
    forecast.add_argument("--output-wav", type=Path, help="a new WAV of the frames sent")
    forecast.add_argument("--scenario", type=Path, help="the play's scenario (.SCN): predict on the card's host render through it")
    forecast.add_argument("--render", type=Path, help="host/render/signal_lab_render, for --scenario")
    forecast.add_argument("--seed", type=int, help="the seed that replaces the scenario's own (a PLAYLIST item's seed=), for --scenario")
    forecast.add_argument("--render-wav", type=Path, help="a new WAV of the render, for --scenario")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.action == "render":
            print(render_command(parse_entries(args.entries)))
        elif args.action == "generate":
            frames = card_data(args.card)[1] if args.card is not None else args.file_frames
            fraction = args.zero_fraction if args.zero_fraction is not None else {"drop": 0.0, "zero": 1.0, "mixed": 0.5}[args.mix]
            entries = generate(args.seed, file_frames=frames, count=args.count, min_length=args.min_length, max_length=args.max_length,
                               zero_fraction=fraction, min_spacing=args.min_spacing, start=args.start)
            command = render_command(entries)
            if args.json:
                print(json.dumps({"seed": args.seed, "file_frames": frames, "command": command,
                                  "entries": [entry.__dict__ for entry in entries]}))
            else:
                print(command)
        else:
            entries = validate(parse_command(args.cut))
            if args.scenario is None and (args.render is not None or args.seed is not None or args.render_wav is not None):
                raise ValueError("--render, --seed and --render-wav go with --scenario")
            if args.scenario is not None and args.render is None:
                raise ValueError("--scenario needs --render (host/render/signal_lab_render)")
            if args.scenario is None:
                result = predict(args.card, entries, sent=args.sent_frames, first_frame_index=args.first_frame_index, output_wav=args.output_wav)
            else:
                result = predict_render(args.card, args.scenario, args.render, entries, seed=args.seed, sent=args.sent_frames,
                                        first_frame_index=args.first_frame_index, output_wav=args.output_wav, render_wav=args.render_wav)
            print(json.dumps(result))
        return 0
    except (CutError, stage_wav.WavError, ValueError, OSError) as error:
        print(f"cut schedule: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
