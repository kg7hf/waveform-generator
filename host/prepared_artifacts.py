#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Inspect locally copied RT1170 prepared WAV/JSN artifacts.

This tool reads ordinary local files only. It does not discover devices, copy
files, publish artifacts, or claim board/archive support beyond the manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
from typing import Any


SCHEMA = "waveform-generator.prepared/2"
SAMPLE_RATE_HZ = 48_000
CHANNELS = 1
BITS_PER_SAMPLE = 24
MAX_DIRECTORY_ENTRIES = 4096
MAX_MANIFEST_BYTES = 8192
MAX_SCENARIO_BYTES = 8191
MAX_WAV_BYTES = 0xFFFFFFFF + 8
BLOCK_BYTES = 64 * 1024
UINT64_MAX = 0xFFFFFFFFFFFFFFFF
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_HEX16 = re.compile(r"^[0-9a-f]{16}$")
_HEX40 = re.compile(r"^[0-9a-f]{40}$")
_NAME = re.compile(r"^[A-Z0-9_-]{1,8}\.(?:WAV|SCN|JSN)$")
_MANIFEST_KEYS = {
    "schema", "complete", "retention", "source", "source_sha256", "scenario", "scenario_sha256",
    "secondary", "secondary_sha256", "seed", "sample_rate_hz", "channels", "bits_per_sample",
    "input_frames", "output_frames", "wav_sha256", "pcm_sha256", "engine_output_fnv1a64",
    "engine_algorithm_version", "firmware_commit",
}


class PreparedArtifactError(ValueError):
    """A local prepared-artifact pair violates its bounded manifest contract."""


def _uint(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= UINT64_MAX


def _uint32(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 0xFFFFFFFF


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise PreparedArtifactError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _bad_constant(value: str) -> None:
    raise PreparedArtifactError(f"non-standard JSON constant {value}")


def _finite_float(token: str) -> float:
    value = float(token)
    if not math.isfinite(value):
        raise PreparedArtifactError(f"non-finite JSON number {token!r}")
    return value


def _safe_path(path: Path, *, strict: bool = True) -> Path:
    """Resolve a path while refusing symlinks in it or any parent component."""
    candidate = path.expanduser().absolute()
    for component in (candidate, *candidate.parents):
        is_junction = getattr(component, "is_junction", None)
        if component.is_symlink() or (is_junction is not None and is_junction()):
            raise PreparedArtifactError(f"symlink paths are not accepted: {component}")
    try:
        resolved = candidate.resolve(strict=strict)
    except OSError as error:
        raise PreparedArtifactError(f"cannot resolve {candidate}: {error}") from error
    return resolved


def _read_file_hash(path: Path, maximum: int) -> tuple[str, int]:
    digest = hashlib.sha256()
    total = 0
    try:
        with path.open("rb") as stream:
            while block := stream.read(BLOCK_BYTES):
                total += len(block)
                if total > maximum:
                    raise PreparedArtifactError(f"file exceeds bounded size: {path}")
                digest.update(block)
    except OSError as error:
        raise PreparedArtifactError(f"cannot read {path}: {error}") from error
    return digest.hexdigest(), total


def _read_bounded_bytes(path: Path, maximum: int) -> bytes:
    """Read one small metadata file without trusting its reported filesystem size."""
    try:
        with path.open("rb") as stream:
            data = stream.read(maximum + 1)
        if len(data) > maximum:
            raise PreparedArtifactError(f"file exceeds bounded size: {path}")
        return data
    except PreparedArtifactError:
        raise
    except OSError as error:
        raise PreparedArtifactError(f"cannot read {path}: {error}") from error


def _copy_file(source: Path, destination: Path, *, expected_sha256: str, expected_size: int) -> tuple[str, int]:
    """50K view: copy only the previously verified bytes into a new file.

    Detailed view: stream fixed-size blocks, enforce the expected byte count,
    sync the output, and compare SHA-256. Failures retain the partial output.
    5th-grade view: copy in small pieces and check that nothing changed.
    """
    digest = hashlib.sha256()
    total = 0
    try:
        with source.open("rb") as input_stream, destination.open("xb") as output_stream:
            while block := input_stream.read(BLOCK_BYTES):
                total += len(block)
                if total > expected_size:
                    raise PreparedArtifactError(f"source grew while archiving: {source}")
                digest.update(block)
                written = output_stream.write(block)
                if written != len(block):
                    raise PreparedArtifactError(f"short archive write: {destination}")
            output_stream.flush()
            os.fsync(output_stream.fileno())
    except PreparedArtifactError:
        raise
    except OSError as error:
        raise PreparedArtifactError(f"cannot copy {source} to {destination}: {error}") from error
    if total != expected_size or digest.hexdigest() != expected_sha256:
        raise PreparedArtifactError(f"copied bytes differ from the verified source: {source}")
    return digest.hexdigest(), total


def _write_exclusive(path: Path, data: bytes) -> None:
    """Create a new metadata file only; a failed partial file is left for inspection."""
    try:
        with path.open("xb") as stream:
            written = stream.write(data)
            if written != len(data):
                raise PreparedArtifactError(f"short archive write: {path}")
            stream.flush()
            os.fsync(stream.fileno())
    except PreparedArtifactError:
        raise
    except OSError as error:
        raise PreparedArtifactError(f"cannot write {path}: {error}") from error


def _read_manifest(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAX_MANIFEST_BYTES + 1)
        if not raw or len(raw) > MAX_MANIFEST_BYTES:
            raise PreparedArtifactError(f"manifest size is outside 1..{MAX_MANIFEST_BYTES} bytes: {path}")
        value = json.loads(raw, object_pairs_hook=_unique_object, parse_float=_finite_float,
                           parse_constant=_bad_constant)
    except PreparedArtifactError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise PreparedArtifactError(f"cannot read valid JSON manifest {path}: {error}") from error
    if not isinstance(value, dict) or set(value) != _MANIFEST_KEYS:
        raise PreparedArtifactError("prepared manifest has missing or unsupported fields")
    if value.get("schema") != SCHEMA or value.get("complete") is not True:
        raise PreparedArtifactError("manifest is not a complete waveform-generator.prepared/2 artifact")
    if value.get("retention") not in ("temporary", "keep"):
        raise PreparedArtifactError("manifest retention is unsupported")
    for field in ("source", "scenario"):
        filename = value.get(field)
        if not isinstance(filename, str) or not _NAME.fullmatch(filename) or not filename.endswith("." + ("WAV" if field == "source" else "SCN")):
            raise PreparedArtifactError(f"manifest {field} must be an upper-case 8.3 filename")
    secondary = value.get("secondary")
    secondary_sha = value.get("secondary_sha256")
    if secondary == "":
        if secondary_sha != "":
            raise PreparedArtifactError("empty secondary source must have an empty secondary hash")
    elif not isinstance(secondary, str) or not _NAME.fullmatch(secondary) or not secondary.endswith(".WAV"):
        raise PreparedArtifactError("manifest secondary must be empty or an upper-case 8.3 WAV filename")
    for field in ("source_sha256", "scenario_sha256", "wav_sha256", "pcm_sha256"):
        if not isinstance(value.get(field), str) or not _HEX64.fullmatch(value[field]):
            raise PreparedArtifactError(f"manifest {field} must be a lowercase SHA-256 digest")
    if secondary and (not isinstance(secondary_sha, str) or not _HEX64.fullmatch(secondary_sha)):
        raise PreparedArtifactError("manifest secondary_sha256 must be a lowercase SHA-256 digest")
    if not secondary and secondary_sha != "":
        raise PreparedArtifactError("manifest has a secondary hash without a secondary source")
    if not isinstance(value.get("engine_output_fnv1a64"), str) or not _HEX16.fullmatch(value["engine_output_fnv1a64"]):
        raise PreparedArtifactError("manifest engine_output_fnv1a64 must contain 16 lowercase hex digits")
    if not _uint(value.get("seed")) or not _uint(value.get("input_frames")) or value["input_frames"] == 0 or \
            not _uint(value.get("output_frames")) or value["output_frames"] == 0:
        raise PreparedArtifactError("manifest seed and nonzero frame counts must be uint64 integers")
    if not all(_uint32(value.get(field)) for field in ("sample_rate_hz", "channels", "bits_per_sample", "engine_algorithm_version")) or \
            (value["sample_rate_hz"], value["channels"], value["bits_per_sample"],
             value["engine_algorithm_version"]) != (SAMPLE_RATE_HZ, CHANNELS, BITS_PER_SAMPLE, 1):
        raise PreparedArtifactError("manifest audio or engine algorithm identity is unsupported")
    firmware_commit = value.get("firmware_commit")
    if not isinstance(firmware_commit, str) or not _HEX40.fullmatch(firmware_commit):
        raise PreparedArtifactError("manifest firmware_commit must be a 40-digit lowercase Git commit")
    return value


def _wav_summary(path: Path, expected_frames: int | None = None) -> tuple[str, str, int, int]:
    """Validate canonical RIFF PCM24 and return (whole SHA, PCM SHA, frames, bytes) in one pass."""
    try:
        with path.open("rb") as stream:
            header = stream.read(44)
            if len(header) != 44:
                raise PreparedArtifactError(f"truncated WAV header: {path}")
            (riff, riff_size, wave, fmt, fmt_size, encoding, channels, rate, byte_rate, alignment, bits,
             data_tag, data_size) = struct.unpack("<4sI4s4sIHHIIHH4sI", header)
            frames, remainder = divmod(data_size, CHANNELS * (BITS_PER_SAMPLE // 8))
            pad_size = data_size & 1
            if (riff, wave, fmt, fmt_size, encoding, channels, rate, byte_rate, alignment, bits, data_tag) != (
                    b"RIFF", b"WAVE", b"fmt ", 16, 1, CHANNELS, SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * 3, 3,
                    BITS_PER_SAMPLE, b"data"):
                raise PreparedArtifactError(f"WAV is not canonical mono PCM24 at 48 kHz: {path}")
            if remainder or (expected_frames is not None and frames != expected_frames) or \
                    riff_size != 36 + data_size + pad_size:
                raise PreparedArtifactError(f"WAV RIFF/data lengths do not match the manifest: {path}")
            file_hash = hashlib.sha256()
            file_hash.update(header)
            pcm_hash = hashlib.sha256()
            remaining = data_size
            total_size = len(header)
            while remaining:
                block = stream.read(min(remaining, BLOCK_BYTES))
                if not block:
                    raise PreparedArtifactError(f"truncated WAV PCM data: {path}")
                file_hash.update(block)
                pcm_hash.update(block)
                total_size += len(block)
                remaining -= len(block)
            if pad_size:
                pad = stream.read(1)
                if pad != b"\x00":
                    raise PreparedArtifactError(f"WAV has a nonzero RIFF pad byte: {path}")
                file_hash.update(pad)
                total_size += 1
            if stream.read(1):
                raise PreparedArtifactError(f"WAV has trailing bytes: {path}")
            if total_size != 44 + data_size + pad_size:
                raise PreparedArtifactError(f"WAV length does not match RIFF/data chunks: {path}")
    except PreparedArtifactError:
        raise
    except (OSError, struct.error) as error:
        raise PreparedArtifactError(f"cannot validate WAV {path}: {error}") from error
    return file_hash.hexdigest(), pcm_hash.hexdigest(), frames, total_size


def _artifact_paths(directory: Path, name: str) -> tuple[Path, Path, str]:
    if not isinstance(name, str) or not name or Path(name).name != name or "/" in name or "\\" in name:
        raise PreparedArtifactError("artifact name must be a basename, not a path")
    base = name[:-4] if name.upper().endswith(".JSN") else name
    if not re.fullmatch(r"[A-Z0-9_-]{1,8}", base):
        raise PreparedArtifactError("artifact name must be an upper-case 8.3 basename")
    return directory / f"{base}.JSN", directory / f"{base}.WAV", f"{base}.SCN"


def _directory_entries(root: Path) -> list[Path]:
    try:
        entries = []
        for path in root.iterdir():
            if len(entries) == MAX_DIRECTORY_ENTRIES:
                raise PreparedArtifactError(f"directory has more than {MAX_DIRECTORY_ENTRIES} entries")
            entries.append(path)
    except OSError as error:
        raise PreparedArtifactError(f"cannot list {root}: {error}") from error
    names: dict[str, str] = {}
    for path in entries:
        folded = path.name.casefold()
        if folded in names:
            raise PreparedArtifactError(f"case-colliding filenames: {names[folded]} and {path.name}")
        names[folded] = path.name
        is_junction = getattr(path, "is_junction", None)
        if path.is_symlink() or (is_junction is not None and is_junction()):
            raise PreparedArtifactError(f"symlink entry is not accepted: {path.name}")
    return entries


def _verify_pair(directory: Path, name: str, *, allow_incomplete: bool = False) -> dict[str, Any]:
    manifest_path, wav_path, output_scenario_name = _artifact_paths(directory, name)
    output_base = manifest_path.stem
    entries = _directory_entries(directory)
    by_folded_name = {path.name.casefold(): path for path in entries}
    if not allow_incomplete and "incomplete.json" in by_folded_name:
        raise PreparedArtifactError("archive directory contains INCOMPLETE.json")
    for path in (manifest_path, wav_path):
        _safe_path(path)
        if not path.is_file():
            raise PreparedArtifactError(f"prepared artifact component is missing or not a file: {path.name}")
    if output_scenario_name.casefold() in by_folded_name:
        raise PreparedArtifactError("output basename has an SCN file; it could be applied twice on playback")
    manifest = _read_manifest(manifest_path)
    if manifest["source"] == f"{output_base}.WAV" or manifest["scenario"] == f"{output_base}.SCN" or \
            manifest["secondary"] in (manifest["source"], f"{output_base}.WAV") or \
            manifest["source"] == manifest["secondary"]:
        raise PreparedArtifactError("manifest input and output names alias")
    wav_hash, pcm_hash, frames, _ = _wav_summary(wav_path, manifest["output_frames"])
    if wav_hash != manifest["wav_sha256"] or pcm_hash != manifest["pcm_sha256"]:
        raise PreparedArtifactError("prepared WAV hashes do not match the manifest")
    return {"ok": True, "name": manifest_path.stem, "manifest": manifest, "wav": str(wav_path),
            "frames": frames, "wav_sha256": wav_hash, "pcm_sha256": pcm_hash}


def list_artifacts(directory: str | os.PathLike[str]) -> dict[str, Any]:
    """Validate each local JSN/WAV pair; malformed candidates fail the whole listing."""
    root = _safe_path(Path(directory))
    if not root.is_dir():
        raise PreparedArtifactError(f"artifact directory is not a directory: {root}")
    entries = _directory_entries(root)
    if any(path.name.casefold() == "incomplete.json" for path in entries):
        raise PreparedArtifactError("archive directory contains INCOMPLETE.json")
    manifests = sorted((path for path in entries if path.suffix.upper() == ".JSN"), key=lambda path: path.name)
    if not manifests:
        return {"ok": True, "directory": str(root), "count": 0, "artifacts": []}
    artifacts = [_verify_pair(root, path.name) for path in manifests]
    return {"ok": True, "directory": str(root), "count": len(artifacts), "artifacts": artifacts}


def verify(directory: str | os.PathLike[str], name: str) -> dict[str, Any]:
    """Verify one copied board WAV/manifest pair."""
    root = _safe_path(Path(directory))
    if not root.is_dir():
        raise PreparedArtifactError(f"artifact directory is not a directory: {root}")
    return _verify_pair(root, name)


def archive(directory: str | os.PathLike[str], name: str, destination: str | os.PathLike[str]) -> dict[str, Any]:
    """50K view: preserve a verified WAV/JSN pair without changing its inputs.

    Detailed view: create an exclusive destination and incomplete marker, sync
    bounded copies, then verify against the source snapshot before publication.
    Source and destination must not be modified concurrently. Failed copies
    remain marked and require explicit operator cleanup.
    5th-grade view: leave a work-in-progress sign until both copies check out.
    """
    source_root = _safe_path(Path(directory))
    if not source_root.is_dir():
        raise PreparedArtifactError(f"artifact directory is not a directory: {source_root}")
    verified = verify(source_root, name)
    manifest_path, wav_path, _ = _artifact_paths(source_root, name)
    manifest_bytes = _read_bounded_bytes(manifest_path, MAX_MANIFEST_BYTES)
    if _read_manifest(manifest_path) != verified["manifest"]:
        raise PreparedArtifactError("source manifest changed after verification")
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()

    destination_path = _safe_path(Path(destination), strict=False)
    destination_parent = _safe_path(destination_path.parent)
    if not destination_parent.is_dir():
        raise PreparedArtifactError(f"archive parent is not a directory: {destination_parent}")
    if source_root == destination_path or source_root in destination_path.parents:
        raise PreparedArtifactError("archive destination must be outside the source directory")
    if destination_path.exists():
        raise PreparedArtifactError(f"archive destination already exists: {destination_path}")

    try:
        destination_path.mkdir()
    except OSError as error:
        raise PreparedArtifactError(f"cannot create new archive directory {destination_path}: {error}") from error
    destination_path = _safe_path(destination_path)

    marker = destination_path / "INCOMPLETE.json"
    _write_exclusive(marker, b'{"complete":false,"schema":"waveform-generator.prepared-archive-incomplete/1"}\n')

    output_manifest = destination_path / manifest_path.name
    output_wav = destination_path / wav_path.name
    expected_wav_size = 44 + verified["frames"] * 3 + ((verified["frames"] * 3) & 1)
    _copy_file(wav_path, output_wav, expected_sha256=verified["wav_sha256"], expected_size=expected_wav_size)
    _write_exclusive(output_manifest, manifest_bytes)

    copied = _verify_pair(destination_path, name, allow_incomplete=True)
    copied_manifest_bytes = _read_bounded_bytes(output_manifest, MAX_MANIFEST_BYTES)
    if (copied["manifest"] != verified["manifest"] or copied["frames"] != verified["frames"]
            or copied["wav_sha256"] != verified["wav_sha256"] or copied["pcm_sha256"] != verified["pcm_sha256"]
            or copied_manifest_bytes != manifest_bytes or hashlib.sha256(copied_manifest_bytes).hexdigest() != manifest_sha256):
        raise PreparedArtifactError("archived pair does not match the verified source snapshot")

    try:
        marker.unlink()
    except OSError as error:
        raise PreparedArtifactError(f"cannot publish archive by removing {marker}: {error}") from error
    return {"ok": True, "directory": str(destination_path), "name": copied["name"], "frames": copied["frames"],
            "manifest_sha256": manifest_sha256, "wav_sha256": copied["wav_sha256"], "pcm_sha256": copied["pcm_sha256"],
            "files": [output_wav.name, output_manifest.name]}


def _current_input(path: str | os.PathLike[str], expected_name: str, expected_hash: str, what: str) -> dict[str, Any]:
    resolved = _safe_path(Path(path))
    if not resolved.is_file():
        raise PreparedArtifactError(f"current {what} is not a regular file: {resolved}")
    if resolved.name != expected_name:
        raise PreparedArtifactError(f"current {what} basename differs from the prepared manifest")
    digest, size = _read_file_hash(resolved, MAX_WAV_BYTES if what != "scenario" else MAX_SCENARIO_BYTES)
    if digest != expected_hash:
        raise PreparedArtifactError(f"current {what} hash differs from the prepared manifest")
    return {"path": str(resolved), "sha256": digest, "bytes": size}


def reuse(directory: str | os.PathLike[str], name: str, *, firmware_commit: str,
          seed: int, source: str | os.PathLike[str],
          scenario: str | os.PathLike[str],
          secondary: str | os.PathLike[str] | None = None) -> dict[str, Any]:
    """Confirm copied output identity against explicit current inputs; does not copy or render."""
    result = verify(directory, name)
    manifest = result["manifest"]
    if not isinstance(firmware_commit, str) or firmware_commit != manifest["firmware_commit"]:
        raise PreparedArtifactError("expected firmware commit differs from the prepared manifest")
    if not _uint(seed) or seed != manifest["seed"]:
        raise PreparedArtifactError("expected seed differs from the prepared manifest")
    checked: dict[str, Any] = {}
    checked["source"] = _current_input(source, manifest["source"], manifest["source_sha256"], "source")
    checked["scenario"] = _current_input(scenario, manifest["scenario"], manifest["scenario_sha256"], "scenario")
    if secondary is not None:
        if not manifest["secondary"]:
            raise PreparedArtifactError("current secondary supplied but manifest has no secondary source")
        checked["secondary"] = _current_input(secondary, manifest["secondary"], manifest["secondary_sha256"], "secondary")
    elif manifest["secondary"]:
        raise PreparedArtifactError("current secondary WAV is required by this prepared manifest")
    return {**result, "reusable": True, "current_inputs": checked,
            "expected_firmware_commit": firmware_commit, "expected_seed": seed}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list", help="validate all prepared JSN/WAV pairs in a local directory")
    listing.add_argument("directory", type=Path)
    checking = commands.add_parser("verify", help="verify one local prepared artifact pair")
    checking.add_argument("directory", type=Path)
    checking.add_argument("name", help="output basename or its .JSN filename")
    archiving = commands.add_parser("archive", help="copy one verified WAV/JSN pair to a new local directory")
    archiving.add_argument("directory", type=Path)
    archiving.add_argument("name", help="output basename or its .JSN filename")
    archiving.add_argument("destination", type=Path, help="new local directory to create for the verified pair")
    reusing = commands.add_parser("reuse", help="verify artifact plus expected firmware and required current inputs")
    reusing.add_argument("directory", type=Path)
    reusing.add_argument("name")
    reusing.add_argument("--firmware-commit", required=True)
    reusing.add_argument("--seed", type=int, required=True)
    reusing.add_argument("--source", type=Path, required=True)
    reusing.add_argument("--scenario", type=Path, required=True)
    reusing.add_argument("--secondary", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "list":
            result = list_artifacts(args.directory)
        elif args.command == "verify":
            result = verify(args.directory, args.name)
        elif args.command == "archive":
            result = archive(args.directory, args.name, args.destination)
        else:
            result = reuse(args.directory, args.name, firmware_commit=args.firmware_commit,
                           seed=args.seed, source=args.source,
                           scenario=args.scenario, secondary=args.secondary)
    except PreparedArtifactError as error:
        print(f"prepared artifacts: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
