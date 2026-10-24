#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Compose two verified mono renderer WAVs into an unscaled stereo PCM24 WAV.

This is an offline artifact-preparation tool. It does not access audio devices.
The project convention maps the left channel to the channel-0 upper sideband and
the right channel to the channel-1 lower sideband; that mapping is not a device
or RF acceptance claim.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import BinaryIO, Any


SAMPLE_RATE_HZ = 48_000
PCM24_BYTES = 3
BLOCK_FRAMES = 8192
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
UINT32_MAX = 0xFFFFFFFF
UINT64_MAX = 0xFFFFFFFFFFFFFFFF
CRC32_FORMAT = "zlib-crc32-packed-pcm24"


class ComposeError(ValueError):
    """Input, output, or verification data failed the tool's contract."""


def _uint(value: Any, maximum: int = UINT64_MAX) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= maximum


def _sha_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _read_exact(stream: BinaryIO, count: int, what: str) -> bytes:
    data = stream.read(count)
    if len(data) != count:
        raise ComposeError(f"truncated {what}")
    return data


def _wav_header(stream: BinaryIO, path: Path, channels: int, allowed_bits: tuple[int, ...]) -> tuple[int, int, int]:
    """Read and validate the renderer's canonical 44-byte PCM RIFF layout."""
    header = _read_exact(stream, 44, f"WAV header: {path}")
    (riff, riff_size, wave, fmt, fmt_size, encoding, actual_channels, rate,
     byte_rate, align, bits, data_tag, data_size) = struct.unpack("<4sI4s4sIHHIIHH4sI", header)
    if riff != b"RIFF" or wave != b"WAVE" or fmt != b"fmt " or data_tag != b"data":
        raise ComposeError(f"{path}: expected canonical 44-byte RIFF/WAVE PCM header")
    if fmt_size != 16 or encoding != 1 or actual_channels != channels or rate != SAMPLE_RATE_HZ:
        raise ComposeError(f"{path}: expected {channels}-channel 48 kHz integer PCM")
    if bits not in allowed_bits:
        raise ComposeError(f"{path}: unsupported PCM bit depth {bits}")
    bytes_per_sample = bits // 8
    expected_align = channels * bytes_per_sample
    if align != expected_align or byte_rate != SAMPLE_RATE_HZ * expected_align:
        raise ComposeError(f"{path}: inconsistent PCM block alignment or byte rate")
    pad_size = data_size & 1
    if riff_size != 36 + data_size + pad_size:
        raise ComposeError(f"{path}: RIFF size does not match data chunk")
    try:
        actual_size = path.stat().st_size
    except OSError as error:
        raise ComposeError(f"cannot stat {path}: {error}") from error
    if actual_size != 44 + data_size + pad_size:
        raise ComposeError(f"{path}: file length does not match canonical data chunk")
    if data_size % expected_align:
        raise ComposeError(f"{path}: data chunk ends inside a PCM frame")
    if pad_size:
        stream.seek(44 + data_size)
        if _read_exact(stream, 1, f"RIFF padding byte: {path}") != b"\x00":
            raise ComposeError(f"{path}: canonical RIFF pad byte must be zero")
        stream.seek(44)
    return data_size // expected_align, bits, data_size


def _read_sidecar(path: Path) -> tuple[dict[str, Any], bytes]:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ComposeError(f"{path}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    def finite_float(token: str) -> float:
        value = float(token)
        if not math.isfinite(value):
            raise ComposeError(f"{path}: non-finite JSON number {token!r}")
        return value

    def invalid_constant(token: str) -> None:
        raise ComposeError(f"{path}: non-standard JSON constant {token!r}")

    try:
        raw = path.read_bytes()
        value = json.loads(raw, object_pairs_hook=unique_object, parse_float=finite_float, parse_constant=invalid_constant)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ComposeError(f"cannot read valid JSON sidecar {path}: {error}") from error
    if not isinstance(value, dict):
        raise ComposeError(f"{path}: renderer sidecar root must be an object")
    required = ("schema", "generator", "output_encoding", "seed", "frames_out", "scenario_path",
                "source_path", "output_digest", "source_digest", "stages")
    missing = [key for key in required if key not in value]
    if missing:
        raise ComposeError(f"{path}: renderer sidecar is missing {', '.join(missing)}")
    if value["schema"] != "signal-lab.render/1" or not isinstance(value["generator"], str) or not value["generator"].startswith("signal-lab-cpp/"):
        raise ComposeError(f"{path}: unsupported C++ renderer sidecar schema or generator")
    if value["output_encoding"] != "pcm_s24le":
        raise ComposeError(f"{path}: renderer output_encoding must be pcm_s24le")
    if not _uint(value["seed"]) or not _uint(value["frames_out"]) or value["frames_out"] == 0:
        raise ComposeError(f"{path}: seed and nonzero frames_out must be uint64 integers")
    if not all(isinstance(value[key], str) for key in ("scenario_path", "source_path")):
        raise ComposeError(f"{path}: scenario_path and source_path must be strings")
    for key in ("output_digest", "source_digest"):
        digest = value[key]
        if not isinstance(digest, str) or len(digest) != 16 or any(c not in "0123456789abcdefABCDEF" for c in digest):
            raise ComposeError(f"{path}: {key} must contain 16 hexadecimal digits")
    if not isinstance(value["stages"], list):
        raise ComposeError(f"{path}: stages must be an array")
    return value, raw


def _fnv_update(current: int, data: bytes) -> int:
    for byte in data:
        current = ((current ^ byte) * FNV_PRIME) & UINT64_MAX
    return current


def _input_pcm(path: Path, sidecar: dict[str, Any]) -> tuple[int, int]:
    with path.open("rb") as stream:
        frames, bits, data_size = _wav_header(stream, path, 1, (24,))
        if frames == 0 or frames != sidecar["frames_out"]:
            raise ComposeError(f"{path}: nonzero WAV frame count must match sidecar frames_out")
        digest = FNV_OFFSET
        remaining = data_size
        while remaining:
            block = _read_exact(stream, min(remaining, BLOCK_FRAMES * PCM24_BYTES), f"PCM data: {path}")
            digest = _fnv_update(digest, block)
            remaining -= len(block)
        if f"{digest:016x}" != sidecar["output_digest"].lower():
            raise ComposeError(f"{path}: PCM24 data does not match sidecar output_digest")
    return frames, data_size


def _distinct_paths(inputs: list[Path], outputs: list[Path]) -> None:
    normalized_inputs = [path.resolve(strict=True) for path in inputs]
    normalized_outputs = [path.resolve(strict=False) for path in outputs]
    if len(set(normalized_outputs)) != len(normalized_outputs):
        raise ComposeError("output WAV and sidecar paths must be different")
    if any(output in normalized_inputs for output in normalized_outputs):
        raise ComposeError("an output path aliases an input artifact")
    for path in outputs:
        if path.exists():
            raise ComposeError(f"refusing to overwrite existing output: {path}")
        if not path.parent.is_dir():
            raise ComposeError(f"output directory does not exist: {path.parent}")


def _scenario_record(path: Path) -> dict[str, str]:
    resolved = path.resolve(strict=True)
    if not resolved.is_file():
        raise ComposeError(f"scenario is not a regular file: {path}")
    return {
        "path": str(resolved),
        "sha256": _sha_file(resolved),
        "binding": "current_file_hash_unverified_against_render",
    }


def _output_wav_header(frames: int) -> bytes:
    data_size = frames * 2 * PCM24_BYTES
    if data_size > UINT32_MAX - 36:
        raise ComposeError("stereo output exceeds RIFF's 32-bit size limit")
    block_align = 2 * PCM24_BYTES
    return struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + data_size, b"WAVE", b"fmt ", 16,
                       1, 2, SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * block_align, block_align, 24, b"data", data_size)


def _write_sidecar_exclusive(path: Path, serialized: str) -> None:
    """Create one JSON sidecar exclusively and remove it if writing fails."""
    created = False
    try:
        with path.open("x", encoding="utf-8", newline="\n") as sidecar:
            created = True
            sidecar.write(serialized)
            sidecar.flush()
            os.fsync(sidecar.fileno())
    except BaseException:
        if created:
            try:
                path.unlink()
            except OSError:
                pass
        raise


def compose(left_wav: str | os.PathLike[str], left_sidecar: str | os.PathLike[str], left_scenario: str | os.PathLike[str],
            right_wav: str | os.PathLike[str], right_sidecar: str | os.PathLike[str], right_scenario: str | os.PathLike[str],
            output_wav: str | os.PathLike[str], output_sidecar: str | os.PathLike[str]) -> dict[str, Any]:
    """Verify and interleave two mono PCM24 renderer artifacts without scaling."""
    inputs = [Path(p) for p in (left_wav, left_sidecar, left_scenario, right_wav, right_sidecar, right_scenario)]
    destinations = [Path(output_wav), Path(output_sidecar)]
    for path in inputs:
        if not path.is_file():
            raise ComposeError(f"input is not a regular file: {path}")
    _distinct_paths(inputs, destinations)

    left_meta, left_meta_raw = _read_sidecar(inputs[1])
    right_meta, right_meta_raw = _read_sidecar(inputs[4])
    left_frames, left_size = _input_pcm(inputs[0], left_meta)
    right_frames, right_size = _input_pcm(inputs[3], right_meta)
    if left_frames != right_frames:
        raise ComposeError(f"channel frame counts differ: left={left_frames}, right={right_frames}")
    left_file_sha = _sha_file(inputs[0])
    left_pcm_sha = _sha_pcm(inputs[0], left_size)
    right_file_sha = _sha_file(inputs[3])
    right_pcm_sha = _sha_pcm(inputs[3], right_size)
    header = _output_wav_header(left_frames)
    left_scenario_record = _scenario_record(inputs[2])
    right_scenario_record = _scenario_record(inputs[5])

    created: list[Path] = []
    output_data_sha = hashlib.sha256()
    left_fnv = FNV_OFFSET
    right_fnv = FNV_OFFSET
    left_crc = 0
    right_crc = 0
    try:
        with destinations[0].open("xb") as out:
            created.append(destinations[0])
            out.write(header)
            with inputs[0].open("rb") as left, inputs[3].open("rb") as right:
                left.seek(44)
                right.seek(44)
                remaining = left_frames
                while remaining:
                    count = min(remaining, BLOCK_FRAMES)
                    left_data = _read_exact(left, count * PCM24_BYTES, f"left PCM: {inputs[0]}")
                    right_data = _read_exact(right, count * PCM24_BYTES, f"right PCM: {inputs[3]}")
                    left_fnv = _fnv_update(left_fnv, left_data)
                    right_fnv = _fnv_update(right_fnv, right_data)
                    interleaved = bytearray(count * 2 * PCM24_BYTES)
                    interleaved[0::6] = left_data[0::3]
                    interleaved[1::6] = left_data[1::3]
                    interleaved[2::6] = left_data[2::3]
                    interleaved[3::6] = right_data[0::3]
                    interleaved[4::6] = right_data[1::3]
                    interleaved[5::6] = right_data[2::3]
                    out.write(interleaved)
                    output_data_sha.update(interleaved)
                    left_crc = zlib.crc32(left_data, left_crc)
                    right_crc = zlib.crc32(right_data, right_crc)
                    remaining -= count
            if f"{left_fnv:016x}" != left_meta["output_digest"].lower() or \
                    f"{right_fnv:016x}" != right_meta["output_digest"].lower():
                raise ComposeError("an input PCM stream changed after preflight or does not match its sidecar digest")
            out.flush()
            os.fsync(out.fileno())

        if _sha_file(inputs[0]) != left_file_sha or _sha_pcm(inputs[0], left_size) != left_pcm_sha or \
                _sha_file(inputs[3]) != right_file_sha or _sha_pcm(inputs[3], right_size) != right_pcm_sha:
            raise ComposeError("an input WAV changed while composing")

        metadata: dict[str, Any] = {
            "schema": "waveform-generator.stereo-compose/1",
            "generator": "waveform-generator-compose-stereo/1",
            "sample_rate_hz": SAMPLE_RATE_HZ,
            "channels": 2,
            "bits_per_sample": 24,
            "frames": left_frames,
            "channel_order": {"left": "channel0_upper", "right": "channel1_lower"},
            "left": {
                "scenario": left_scenario_record,
                "render_sidecar_sha256": hashlib.sha256(left_meta_raw).hexdigest(),
                "wav_path": str(inputs[0].resolve()),
                "wav_file_sha256": left_file_sha,
                "wav_pcm_sha256": left_pcm_sha,
                "render": left_meta,
            },
            "right": {
                "scenario": right_scenario_record,
                "render_sidecar_sha256": hashlib.sha256(right_meta_raw).hexdigest(),
                "wav_path": str(inputs[3].resolve()),
                "wav_file_sha256": right_file_sha,
                "wav_pcm_sha256": right_pcm_sha,
                "render": right_meta,
            },
            "output": {
                "data_sha256": output_data_sha.hexdigest(),
                "file_sha256": _sha_file(destinations[0]),
                "crc32_left": f"{left_crc & UINT32_MAX:08x}",
                "crc32_right": f"{right_crc & UINT32_MAX:08x}",
                "crc32_format": CRC32_FORMAT,
            },
        }
        serialized = json.dumps(metadata, indent=2, sort_keys=True, allow_nan=False) + "\n"
        _write_sidecar_exclusive(destinations[1], serialized)
        created.append(destinations[1])
        return metadata
    except BaseException:
        for path in reversed(created):
            try:
                path.unlink()
            except OSError:
                pass
        raise


def _sha_pcm(path: Path, size: int) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        stream.seek(44)
        remaining = size
        while remaining:
            block = _read_exact(stream, min(remaining, 1024 * 1024), f"PCM data: {path}")
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def verify(expected_wav: str | os.PathLike[str], capture_wav: str | os.PathLike[str]) -> dict[str, Any]:
    """Compare exact stereo frames; allow only PCM24 identity or top-16 truncation."""
    expected_path = Path(expected_wav)
    capture_path = Path(capture_wav)
    if expected_path.resolve(strict=True) == capture_path.resolve(strict=True):
        raise ComposeError("expected and capture WAV paths must be different")
    with expected_path.open("rb") as expected, capture_path.open("rb") as capture:
        expected_frames, _, expected_size = _wav_header(expected, expected_path, 2, (24,))
        capture_frames, capture_bits, _ = _wav_header(capture, capture_path, 2, (16, 24))
        if expected_frames == 0 or expected_frames != capture_frames:
            raise ComposeError(f"frame counts differ or are zero: expected={expected_frames}, capture={capture_frames}")
        if capture_bits not in (16, 24):
            raise ComposeError("capture must be PCM16 or PCM24")
        frames_read = 0
        expected_crc_left = 0
        expected_crc_right = 0
        capture_crc_left = 0
        capture_crc_right = 0
        while frames_read < expected_frames:
            count = min(BLOCK_FRAMES, expected_frames - frames_read)
            expected_data = _read_exact(expected, count * 6, "expected stereo PCM")
            capture_bpf = capture_bits // 8 * 2
            capture_data = _read_exact(capture, count * capture_bpf, "capture stereo PCM")
            for index in range(count):
                exp_at = index * 6
                cap_at = index * capture_bpf
                if capture_bits == 24:
                    if capture_data[cap_at:cap_at + 6] != expected_data[exp_at:exp_at + 6]:
                        raise ComposeError(f"PCM mismatch at frame {frames_read + index}")
                    capture_left = capture_data[cap_at:cap_at + 3]
                    capture_right = capture_data[cap_at + 3:cap_at + 6]
                else:
                    if capture_data[cap_at:cap_at + 2] != expected_data[exp_at + 1:exp_at + 3] or \
                            capture_data[cap_at + 2:cap_at + 4] != expected_data[exp_at + 4:exp_at + 6]:
                        raise ComposeError(f"PCM16 top-16 mismatch at frame {frames_read + index}")
                    capture_left = capture_data[cap_at:cap_at + 2]
                    capture_right = capture_data[cap_at + 2:cap_at + 4]
                expected_crc_left = zlib.crc32(expected_data[exp_at:exp_at + 3], expected_crc_left)
                expected_crc_right = zlib.crc32(expected_data[exp_at + 3:exp_at + 6], expected_crc_right)
                capture_crc_left = zlib.crc32(capture_left, capture_crc_left)
                capture_crc_right = zlib.crc32(capture_right, capture_crc_right)
            frames_read += count
        if expected_size != expected_frames * 6:
            raise ComposeError("expected WAV data size is inconsistent")
    return {
        "ok": True,
        "frames": expected_frames,
        "capture_encoding": "pcm_s24le" if capture_bits == 24 else "pcm_s16le_top16",
        "crc32_left": f"{expected_crc_left & UINT32_MAX:08x}",
        "crc32_right": f"{expected_crc_right & UINT32_MAX:08x}",
        "crc32_format": CRC32_FORMAT,
        "capture_native_crc32_left": f"{capture_crc_left & UINT32_MAX:08x}",
        "capture_native_crc32_right": f"{capture_crc_right & UINT32_MAX:08x}",
        "capture_crc32_format": "zlib-crc32-packed-pcm24" if capture_bits == 24 else "zlib-crc32-packed-pcm16",
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("compose", help="verify and interleave two mono PCM24 renders")
    for side in ("left", "right"):
        create.add_argument(f"--{side}-wav", required=True)
        create.add_argument(f"--{side}-sidecar", required=True)
        create.add_argument(f"--{side}-scenario", required=True)
    create.add_argument("--output-wav", required=True)
    create.add_argument("--output-sidecar", required=True)
    check = commands.add_parser("verify", help="compare a stereo capture with expected PCM24")
    check.add_argument("--expected-wav", required=True)
    check.add_argument("--capture-wav", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "compose":
            result = compose(args.left_wav, args.left_sidecar, args.left_scenario,
                             args.right_wav, args.right_sidecar, args.right_scenario,
                             args.output_wav, args.output_sidecar)
        else:
            result = verify(args.expected_wav, args.capture_wav)
    except (ComposeError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
