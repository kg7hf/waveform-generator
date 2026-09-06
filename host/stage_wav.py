#!/usr/bin/env python3
"""Prepare one known-good WAV for the P1.2 microSD playback checkpoint.

This is deliberately a small staging utility, not the Phase 1 qualification
packager.  It accepts only the one transparent firmware format and copies the
validated bytes to the fixed bring-up path ``/WG/PLAY.WAV`` on an explicitly
selected card root.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
from typing import BinaryIO


BLOCK_ALIGN = 2
BYTE_RATE = 96_000
CHANNELS = 1
FIXED_CARD_NAME = "PLAY.WAV"
FIXED_SIDECAR_NAME = "PLAY.WGM"
SAMPLE_RATE_HZ = 48_000
SAMPLE_WIDTH_BITS = 16
STAGE_DIRECTORY = "WG"


class WavError(ValueError):
    """The input cannot be replayed by the P1.2 firmware."""


def _read_exact(stream: BinaryIO, count: int, context: str) -> bytes:
    value = stream.read(count)
    if len(value) != count:
        raise WavError(f"truncated {context}")
    return value


def _sha256_region(stream: BinaryIO, offset: int, length: int) -> str:
    digest = hashlib.sha256()
    stream.seek(offset)
    remaining = length
    while remaining:
        block = stream.read(min(64 * 1024, remaining))
        if not block:
            raise WavError("truncated PCM data while hashing")
        digest.update(block)
        remaining -= len(block)
    return digest.hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def make_wgm(facts: dict[str, object], level_percent: int) -> bytes:
    if not 0 <= level_percent <= 100:
        raise WavError("level percent must be between 0 and 100")
    text = (
        "WGM1\n"
        f"wav_bytes={facts['file_bytes']}\n"
        f"wav_sha256={facts['file_sha256']}\n"
        f"data_offset={facts['data_offset']}\n"
        f"data_bytes={facts['data_bytes']}\n"
        f"pcm_sha256={facts['pcm_sha256']}\n"
        f"samples={facts['samples']}\n"
        f"level_percent={level_percent}\n"
    )
    encoded = text.encode("ascii")
    if len(encoded) > 512:
        raise WavError("WGM sidecar exceeds the frozen 512-byte limit")
    return encoded


def inspect_wav(path: Path) -> dict[str, object]:
    """Validate *path* and return the exact facts needed for card bring-up."""

    path = path.resolve(strict=True)
    if not path.is_file():
        raise WavError(f"not a regular file: {path}")

    file_bytes = path.stat().st_size
    if file_bytes < 44:
        raise WavError("file is too short to be a PCM WAV")

    fmt_fields: tuple[int, int, int, int, int, int] | None = None
    data_offset: int | None = None
    data_bytes: int | None = None

    with path.open("rb") as stream:
        riff_id, riff_bytes, wave_id = struct.unpack("<4sI4s", _read_exact(stream, 12, "RIFF header"))
        if riff_id != b"RIFF" or wave_id != b"WAVE":
            raise WavError("expected little-endian RIFF/WAVE")
        if riff_bytes != file_bytes - 8:
            raise WavError("RIFF length does not match the file length")

        position = 12
        while position < file_bytes:
            if file_bytes - position < 8:
                raise WavError("truncated chunk header")
            stream.seek(position)
            chunk_id, chunk_bytes = struct.unpack("<4sI", _read_exact(stream, 8, "chunk header"))
            payload_offset = position + 8
            payload_end = payload_offset + chunk_bytes
            padded_end = payload_end + (chunk_bytes & 1)
            if payload_end < payload_offset or padded_end > file_bytes:
                raise WavError("chunk length exceeds the file")

            if chunk_id == b"fmt ":
                if fmt_fields is not None:
                    raise WavError("duplicate fmt chunk")
                if chunk_bytes < 16:
                    raise WavError("fmt chunk is shorter than PCM format data")
                stream.seek(payload_offset)
                fmt_fields = struct.unpack("<HHIIHH", _read_exact(stream, 16, "fmt chunk"))
            elif chunk_id == b"data":
                if data_offset is not None:
                    raise WavError("duplicate data chunk")
                data_offset = payload_offset
                data_bytes = chunk_bytes

            position = padded_end

        if position != file_bytes:
            raise WavError("chunk walk did not end at the file boundary")
        if fmt_fields is None:
            raise WavError("missing fmt chunk")
        if data_offset is None or data_bytes is None:
            raise WavError("missing data chunk")

        format_tag, channels, sample_rate, byte_rate, block_align, sample_bits = fmt_fields
        expected = (1, CHANNELS, SAMPLE_RATE_HZ, BYTE_RATE, BLOCK_ALIGN, SAMPLE_WIDTH_BITS)
        if fmt_fields != expected:
            raise WavError(
                "unsupported format: expected PCM16 mono 48000 Hz "
                f"(got tag={format_tag}, channels={channels}, rate={sample_rate}, "
                f"byte_rate={byte_rate}, align={block_align}, bits={sample_bits})"
            )
        if data_bytes == 0:
            raise WavError("PCM data is empty")
        if data_bytes % BLOCK_ALIGN:
            raise WavError("PCM data length is not a whole sample count")

        pcm_sha256 = _sha256_region(stream, data_offset, data_bytes)

    whole_sha256 = _sha256_file(path)
    samples = data_bytes // BLOCK_ALIGN
    return {
        "schema_version": "wfg-stage-wav/1",
        "source": str(path),
        "file_bytes": file_bytes,
        "file_sha256": whole_sha256,
        "data_offset": data_offset,
        "data_bytes": data_bytes,
        "pcm_sha256": pcm_sha256,
        "samples": samples,
        "duration_seconds": samples / SAMPLE_RATE_HZ,
        "format": {
            "encoding": "pcm_s16le",
            "sample_rate_hz": SAMPLE_RATE_HZ,
            "channels": CHANNELS,
            "bits_per_sample": SAMPLE_WIDTH_BITS,
        },
    }


def _write_tone_from_recipe(recipe_path: Path, output_path: Path, force: bool) -> dict[str, object]:
    recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
    preparation = recipe.get("preparation", {})
    period = preparation.get("pcm16_period")
    repetitions = preparation.get("period_repetitions")
    declared_samples = preparation.get("samples")
    if (
        recipe.get("fixture_id") != "tone-10s"
        or not isinstance(period, list)
        or not period
        or not isinstance(repetitions, int)
        or repetitions <= 0
        or declared_samples != len(period) * repetitions
        or any(not isinstance(sample, int) or sample < -32768 or sample > 32767 for sample in period)
    ):
        raise WavError("tone recipe does not contain the expected bounded integer period")

    if output_path.exists() and not force:
        raise WavError(f"output already exists (use --force): {output_path}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + ".tmp")
    data_bytes = declared_samples * BLOCK_ALIGN
    riff_bytes = 36 + data_bytes
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        riff_bytes,
        b"WAVE",
        b"fmt ",
        16,
        1,
        CHANNELS,
        SAMPLE_RATE_HZ,
        BYTE_RATE,
        BLOCK_ALIGN,
        SAMPLE_WIDTH_BITS,
        b"data",
        data_bytes,
    )
    packed_period = struct.pack(f"<{len(period)}h", *period)
    try:
        with temporary.open("wb") as stream:
            stream.write(header)
            for _ in range(repetitions):
                stream.write(packed_period)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(output_path)
    finally:
        temporary.unlink(missing_ok=True)
    return inspect_wav(output_path)


def stage_wav(
    source: Path, card_root: Path, force: bool, level_percent: int = 70
) -> dict[str, object]:
    """Validate and copy *source* to the fixed P1.2 card pathname."""

    source_facts = inspect_wav(source)
    card_root = card_root.resolve(strict=True)
    if not card_root.is_dir():
        raise WavError(f"card root is not a directory: {card_root}")
    stage_directory = card_root / STAGE_DIRECTORY
    stage_directory.mkdir(exist_ok=True)
    destination = stage_directory / FIXED_CARD_NAME
    sidecar_destination = stage_directory / FIXED_SIDECAR_NAME
    existing = [path for path in (destination, sidecar_destination) if path.exists()]
    if existing and not force:
        raise WavError(f"destination already exists (use --force): {existing[0]}")

    temporary = stage_directory / (FIXED_CARD_NAME + ".TMP")
    try:
        with source.resolve(strict=True).open("rb") as input_stream, temporary.open("wb") as output_stream:
            shutil.copyfileobj(input_stream, output_stream, length=64 * 1024)
            output_stream.flush()
            os.fsync(output_stream.fileno())
        copied_facts = inspect_wav(temporary)
        if copied_facts["file_sha256"] != source_facts["file_sha256"]:
            raise WavError("copied file hash does not match the source")
        if destination.exists():
            destination.unlink()
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)

    final_facts = inspect_wav(destination)
    if final_facts["file_sha256"] != source_facts["file_sha256"]:
        raise WavError("final card file hash does not match the source")
    sidecar = make_wgm(final_facts, level_percent)
    sidecar_temporary = stage_directory / (FIXED_SIDECAR_NAME + ".TMP")
    try:
        with sidecar_temporary.open("wb") as stream:
            stream.write(sidecar)
            stream.flush()
            os.fsync(stream.fileno())
        if sidecar_destination.exists():
            sidecar_destination.unlink()
        sidecar_temporary.replace(sidecar_destination)
    finally:
        sidecar_temporary.unlink(missing_ok=True)
    retained_sidecar = sidecar_destination.read_bytes()
    if retained_sidecar != sidecar:
        raise WavError("final card WGM bytes do not match the generated sidecar")
    return {
        "schema_version": "wfg-stage-card/1",
        "source": source_facts,
        "destination": str(destination),
        "destination_file_sha256": final_facts["file_sha256"],
        "sidecar": str(sidecar_destination),
        "sidecar_sha256": hashlib.sha256(retained_sidecar).hexdigest(),
        "level_percent": level_percent,
        "verified": True,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="validate and describe a WAV")
    inspect_parser.add_argument("wav", type=Path)

    tone_parser = subparsers.add_parser("make-tone", help="generate the frozen ten-second tone WAV")
    tone_parser.add_argument("--output", required=True, type=Path)
    tone_parser.add_argument(
        "--recipe",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "fixtures" / "tone-10s.json",
    )
    tone_parser.add_argument("--force", action="store_true")

    stage_parser = subparsers.add_parser("stage", help="copy a validated WAV to /WG/PLAY.WAV")
    stage_parser.add_argument("wav", type=Path)
    stage_parser.add_argument("--card-root", required=True, type=Path)
    stage_parser.add_argument("--level-percent", type=int, default=70)
    stage_parser.add_argument("--force", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "inspect":
            result = inspect_wav(args.wav)
        elif args.command == "make-tone":
            result = _write_tone_from_recipe(args.recipe, args.output, args.force)
        else:
            result = stage_wav(args.wav, args.card_root, args.force, args.level_percent)
    except (OSError, WavError, json.JSONDecodeError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True), file=sys.stderr)
        return 2
    print(json.dumps({"ok": True, **result}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
