#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Prepare and verify immutable offline signal-lab waveform artifacts.

The tool invokes the portable C++ renderer only; it has no audio or device
interfaces. A manifest is marked complete only after the rendered PCM has
been read back and checked against the renderer sidecar. Keep all inputs
immutable during preparation; before/after hashes detect ordinary changes but
cannot detect a transient edit that is restored while rendering.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from typing import Any, BinaryIO


SCHEMA = "waveform-generator.retained-render/1"
RENDER_SCHEMA = "signal-lab.render/1"
SAMPLE_RATE_HZ = 48_000
PCM24_WIDTH = 3
BLOCK_FRAMES = 2048
BLOCK_BYTES = BLOCK_FRAMES * PCM24_WIDTH
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
UINT64_MAX = 0xFFFFFFFFFFFFFFFF
_HEX64 = re.compile(r"^[0-9a-f]{16}$")
_HEX256 = re.compile(r"^[0-9a-f]{64}$")


class RetainError(ValueError):
    """A render input or retained artifact violates the offline contract."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise RetainError(f"duplicate JSON key {key!r}")
        value[key] = item
    return value


def _bad_constant(value: str) -> None:
    raise RetainError(f"non-finite JSON value {value}")


def _read_json(path: Path) -> tuple[Any, bytes]:
    def finite_float(token: str) -> float:
        value = float(token)
        if not math.isfinite(value):
            raise RetainError(f"non-finite JSON number {token!r}")
        return value

    try:
        raw = path.read_bytes()
        value = json.loads(raw, object_pairs_hook=_unique_object, parse_float=finite_float, parse_constant=_bad_constant)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RetainError(f"cannot read JSON {path}: {error}") from error
    return value, raw


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError as error:
        raise RetainError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _fnv_update(value: int, data: bytes) -> int:
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & UINT64_MAX
    return value


def _wav_payload(path: Path) -> tuple[int, str, str, int]:
    """Validate canonical mono PCM24 RIFF and stream full-file/PCM hashes."""
    full_hash = hashlib.sha256()
    pcm_hash = hashlib.sha256()
    fnv = FNV_OFFSET
    try:
        with path.open("rb") as stream:
            header = stream.read(44)
            if len(header) != 44:
                raise RetainError(f"{path}: truncated WAV header")
            (riff, riff_size, wave, fmt, fmt_size, encoding, channels, rate,
             byte_rate, align, bits, data_tag, data_size) = struct.unpack("<4sI4s4sIHHIIHH4sI", header)
            if riff != b"RIFF" or wave != b"WAVE" or fmt != b"fmt " or data_tag != b"data":
                raise RetainError(f"{path}: noncanonical RIFF/WAVE structure")
            if (fmt_size, encoding, channels, rate, byte_rate, align, bits) != (
                    16, 1, 1, SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * PCM24_WIDTH, PCM24_WIDTH, 24):
                raise RetainError(f"{path}: expected mono 48 kHz PCM24")
            if data_size % PCM24_WIDTH:
                raise RetainError(f"{path}: PCM24 payload ends inside a sample")
            frames = data_size // PCM24_WIDTH
            pad = data_size & 1
            if riff_size != 36 + data_size + pad or path.stat().st_size != 44 + data_size + pad:
                raise RetainError(f"{path}: RIFF size or file length is inconsistent")
            full_hash.update(header)
            remaining = data_size
            while remaining:
                block = stream.read(min(remaining, BLOCK_BYTES))
                if not block:
                    raise RetainError(f"{path}: truncated PCM payload")
                remaining -= len(block)
                full_hash.update(block)
                pcm_hash.update(block)
                fnv = _fnv_update(fnv, block)
            if pad:
                padding = stream.read(1)
                if padding != b"\x00":
                    raise RetainError(f"{path}: odd-length RIFF padding must be zero")
                full_hash.update(padding)
            if stream.read(1):
                raise RetainError(f"{path}: trailing data after RIFF")
    except OSError as error:
        raise RetainError(f"cannot read WAV {path}: {error}") from error
    return frames, full_hash.hexdigest(), pcm_hash.hexdigest(), fnv


def _source_paths(scenario: dict[str, Any], scenario_path: Path) -> list[tuple[str, Path, str]]:
    paths: list[tuple[str, Path, str]] = []
    stages = scenario.get("impairments", [])
    if not isinstance(stages, list):
        raise RetainError("scenario impairments must be an array")
    for index, stage in enumerate(stages):
        if isinstance(stage, dict) and stage.get("type") == "second_source":
            configured = stage.get("source_path")
            if not isinstance(configured, str) or not configured:
                raise RetainError(f"second_source stage {index} has no source_path")
            source = Path(configured)
            resolved = source if source.is_absolute() else scenario_path.parent / source
            try:
                resolved = resolved.resolve(strict=True)
            except OSError as error:
                raise RetainError(f"cannot resolve second_source input {configured!r}: {error}") from error
            if not resolved.is_file():
                raise RetainError(f"second_source input is not a file: {resolved}")
            paths.append((f"second_source_{index}", resolved, configured))
    return paths


def _uint64(value: Any) -> bool:
    return type(value) is int and 0 <= value <= UINT64_MAX


def _scenario_seed(value: Any) -> int:
    """Match the renderer's finite, integral JSON-number seed contract."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise RetainError("scenario seed must be a non-negative integer")
    try:
        numeric = float(value)
    except OverflowError as error:
        raise RetainError("scenario seed is outside uint64") from error
    if not math.isfinite(numeric) or numeric < 0 or numeric >= 2**64 or not numeric.is_integer():
        raise RetainError("scenario seed must be a non-negative uint64 integer")
    return int(numeric)


def _sidecar(path: Path) -> tuple[dict[str, Any], bytes]:
    value, raw = _read_json(path)
    if not isinstance(value, dict) or value.get("schema") != RENDER_SCHEMA:
        raise RetainError("renderer sidecar has an unsupported schema")
    if value.get("output_encoding") != "pcm_s24le":
        raise RetainError("renderer sidecar output_encoding is not pcm_s24le")
    if not _uint64(value.get("seed")) or not _uint64(value.get("frames_out")):
        raise RetainError("renderer sidecar seed/frames_out is invalid")
    digest = value.get("output_digest")
    if not isinstance(digest, str) or not _HEX64.fullmatch(digest.lower()):
        raise RetainError("renderer sidecar output_digest must be 16 hex digits")
    for field in ("scenario_path", "source_path"):
        if not isinstance(value.get(field), str):
            raise RetainError(f"renderer sidecar {field} must be a path string")
    if value.get("block_frames") != BLOCK_FRAMES:
        raise RetainError("renderer sidecar block_frames does not match requested block size")
    return value, raw


def _canonical_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def _write_durable(path: Path, data: bytes, *, exclusive: bool = True) -> None:
    mode = "xb" if exclusive else "wb"
    try:
        with path.open(mode) as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as error:
        raise RetainError(f"cannot write {path}: {error}") from error


def _sync_file(path: Path) -> None:
    try:
        with path.open("rb+") as stream:
            os.fsync(stream.fileno())
    except OSError as error:
        raise RetainError(f"cannot flush {path}: {error}") from error


def _copy_file_exclusive(source: Path, destination: Path) -> None:
    digest = hashlib.sha256()
    try:
        with source.open("rb") as inp, destination.open("xb") as out:
            while block := inp.read(1024 * 1024):
                digest.update(block)
                out.write(block)
            out.flush()
            os.fsync(out.fileno())
    except OSError as error:
        raise RetainError(f"cannot publish {source}: {error}") from error
    if digest.hexdigest() != _hash_file(source):
        raise RetainError(f"staging file changed during publish: {source}")


def _copy_tree_exclusive(source: Path, destination: Path, *, skip: frozenset[str] = frozenset()) -> None:
    """Copy generated staging files without replacing any concurrently created path."""
    for child in sorted(source.iterdir(), key=lambda item: item.name):
        if child.name in skip:
            continue
        target = destination / child.name
        if child.is_symlink():
            raise RetainError(f"refusing symlink in staging output: {child}")
        if child.is_dir():
            target.mkdir()
            _copy_tree_exclusive(child, target)
        else:
            _copy_file_exclusive(child, target)


def _remove_private_stage(stage: Path, output_path: Path) -> None:
    """Remove only the staging directory created for this reserved output."""
    expected_parent = output_path.parent.resolve(strict=True)
    resolved_stage = stage.resolve(strict=True)
    prefix = f".{output_path.name}.incomplete-"
    if (stage.is_symlink() or not resolved_stage.is_dir()
            or resolved_stage.parent != expected_parent
            or not resolved_stage.name.startswith(prefix)):
        raise RetainError("refusing to remove staging path outside its private output scope")
    shutil.rmtree(resolved_stage)


def _artifact_file(directory: Path, relative: str) -> Path:
    root = directory.resolve(strict=True)
    lexical = directory / relative
    cursor = directory
    for part in Path(relative).parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise RetainError(f"artifact reference traverses a symlink: {relative}")
    candidate = lexical.resolve(strict=True)
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise RetainError(f"artifact reference escapes its directory: {relative}") from error
    if not candidate.is_file():
        raise RetainError(f"artifact reference is not a file: {relative}")
    return candidate


def _identity(scenario_raw: bytes, scenario: dict[str, Any], source: Path, secondary: list[tuple[str, Path, str]],
              renderer: Path, seed: int, retention: str) -> dict[str, Any]:
    return {
        "scenario_sha256": hashlib.sha256(scenario_raw).hexdigest(),
        "source_sha256": _hash_file(source),
        "secondary_sources": [{"role": role, "configured_path": configured, "path": str(path), "sha256": _hash_file(path)}
                              for role, path, configured in secondary],
        "renderer_sha256": _hash_file(renderer),
        "seed": seed,
        "retention": retention,
        "block_frames": BLOCK_FRAMES,
        "sample_rate_hz": SAMPLE_RATE_HZ,
        "output_format": "mono_pcm24",
    }


def _safe_archive_copy(source: Path, destination: Path, expected_hash: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    try:
        with source.open("rb") as inp, destination.open("xb") as out:
            while block := inp.read(1024 * 1024):
                digest.update(block)
                out.write(block)
            out.flush()
            os.fsync(out.fileno())
    except OSError as error:
        raise RetainError(f"cannot archive {source}: {error}") from error
    if digest.hexdigest() != expected_hash:
        raise RetainError(f"input changed while archiving: {source}")


def _verify_directory(directory: Path, *, allow_incomplete: bool = False) -> dict[str, Any]:
    if not directory.is_dir():
        raise RetainError(f"artifact directory does not exist: {directory}")
    if (directory / "INCOMPLETE.json").exists() and not allow_incomplete:
        raise RetainError("artifact directory still has its incomplete-work marker")
    manifest_path = _artifact_file(directory, "manifest.json")
    manifest, _ = _read_json(manifest_path)
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA or manifest.get("complete") is not True:
        raise RetainError("artifact manifest is absent, incomplete, or unsupported")
    for field, filename in (("scenario_sha256", "scenario.json"), ("renderer_report_sha256", "renderer.json")):
        expected = manifest.get(field)
        if not isinstance(expected, str) or not _HEX256.fullmatch(expected):
            raise RetainError(f"manifest field {field} is invalid")
        if _hash_file(_artifact_file(directory, filename)) != expected:
            raise RetainError(f"retained {filename} hash does not match manifest")
    for field, filename in (("renderer_stdout_sha256", "renderer.stdout.txt"),
                            ("renderer_stderr_sha256", "renderer.stderr.txt")):
        expected = manifest.get(field)
        if not isinstance(expected, str) or not _HEX256.fullmatch(expected) or _hash_file(_artifact_file(directory, filename)) != expected:
            raise RetainError(f"retained {filename} hash does not match manifest")
    output = _artifact_file(directory, "output.wav")
    frames, wav_hash, pcm_hash, fnv = _wav_payload(output)
    if (not _uint64(manifest.get("frames")) or frames != manifest["frames"] or
            not isinstance(manifest.get("wav_sha256"), str) or not _HEX256.fullmatch(manifest["wav_sha256"]) or
            wav_hash != manifest["wav_sha256"] or not isinstance(manifest.get("pcm_sha256"), str) or
            not _HEX256.fullmatch(manifest["pcm_sha256"]) or pcm_hash != manifest["pcm_sha256"]):
        raise RetainError("retained output WAV does not match manifest")
    sidecar, sidecar_raw = _sidecar(_artifact_file(directory, "renderer.json"))
    if sidecar["frames_out"] != frames or f"{fnv:016x}" != sidecar["output_digest"].lower():
        raise RetainError("retained output WAV does not match renderer report")
    if not isinstance(manifest.get("output_digest"), str) or not _HEX64.fullmatch(manifest["output_digest"]) or manifest["output_digest"] != f"{fnv:016x}":
        raise RetainError("manifest output digest does not match WAV")
    if manifest.get("renderer_report_sha256") != hashlib.sha256(sidecar_raw).hexdigest():
        raise RetainError("manifest renderer report hash is inconsistent")
    generation = manifest.get("generation")
    identity = manifest.get("request_identity")
    if not isinstance(generation, dict) or not isinstance(identity, dict):
        raise RetainError("manifest generation or request identity is missing")
    expected_identity_keys = {"scenario_sha256", "source_sha256", "secondary_sources", "renderer_sha256", "seed",
                              "retention", "block_frames", "sample_rate_hz", "output_format"}
    if set(identity) != expected_identity_keys:
        raise RetainError("manifest request identity has an unsupported shape")
    for field in ("scenario_sha256", "source_sha256", "renderer_sha256"):
        if not isinstance(identity.get(field), str) or not _HEX256.fullmatch(identity[field]):
            raise RetainError(f"manifest request identity {field} is invalid")
    renderer_identity = manifest.get("renderer")
    source_identity = manifest.get("source")
    if (identity["scenario_sha256"] != manifest.get("scenario_sha256") or
            not isinstance(renderer_identity, dict) or identity["renderer_sha256"] != renderer_identity.get("sha256") or
            not isinstance(source_identity, dict) or identity["source_sha256"] != source_identity.get("sha256")):
        raise RetainError("manifest request identity hash links are inconsistent")
    if (set(renderer_identity) != {"path_at_render", "sha256"} or
            not isinstance(renderer_identity["path_at_render"], str) or
            set(source_identity) != {"path_at_render", "sha256", "bytes"} or
            not isinstance(source_identity["path_at_render"], str) or not _uint64(source_identity["bytes"])):
        raise RetainError("manifest renderer or source provenance is malformed")
    duration = manifest.get("render_duration_seconds")
    if isinstance(duration, bool) or not isinstance(duration, (int, float)) or not math.isfinite(duration) or duration < 0:
        raise RetainError("manifest render duration is invalid")
    command = manifest.get("renderer_command")
    if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
        raise RetainError("manifest renderer command is invalid")
    block_at = command.index("--block") if "--block" in command else -1
    scenario_at = command.index("--scenario") if "--scenario" in command else -1
    source_at = command.index("--source") if "--source" in command else -1
    scenario_record_for_command = manifest.get("scenario_input")
    scenario_path_at_render = (scenario_record_for_command.get("path_at_render")
                               if isinstance(scenario_record_for_command, dict) else None)
    if (command[0] != renderer_identity["path_at_render"] or block_at < 0 or block_at + 1 >= len(command) or
            scenario_at < 0 or scenario_at + 1 >= len(command) or source_at < 0 or source_at + 1 >= len(command) or
            command[block_at + 1] != str(BLOCK_FRAMES) or
            command[scenario_at + 1] != scenario_path_at_render or
            command[source_at + 1] != source_identity["path_at_render"]):
        raise RetainError("manifest renderer command does not match the generation contract")
    if (identity.get("block_frames"), identity.get("sample_rate_hz"), identity.get("output_format")) != (
            BLOCK_FRAMES, SAMPLE_RATE_HZ, "mono_pcm24"):
        raise RetainError("manifest request generation contract is inconsistent")
    if set(generation) != {"block_frames", "sample_rate_hz", "format", "seed"} or (
            generation.get("block_frames"), generation.get("sample_rate_hz"), generation.get("format")) != (
                BLOCK_FRAMES, SAMPLE_RATE_HZ, "mono_pcm24"):
        raise RetainError("manifest generation options are inconsistent")
    if (not _uint64(generation.get("seed")) or not _uint64(identity.get("seed")) or
            generation["seed"] != sidecar["seed"] or identity["seed"] != sidecar["seed"]):
        raise RetainError("manifest seed does not match renderer report")
    if manifest.get("retention") not in ("temporary", "keep", "archive") or manifest.get("retention") != identity.get("retention"):
        raise RetainError("manifest retention does not match request identity")
    secondary = manifest.get("secondary_sources")
    if secondary != identity.get("secondary_sources") or not isinstance(secondary, list):
        raise RetainError("manifest secondary-source identities are inconsistent")
    for item in secondary:
        if (not isinstance(item, dict) or set(item) != {"role", "configured_path", "path", "sha256"} or
                not isinstance(item["role"], str) or not isinstance(item["configured_path"], str) or
                not isinstance(item["path"], str) or
                not isinstance(item["sha256"], str) or not _HEX256.fullmatch(item["sha256"])):
            raise RetainError("manifest secondary-source identity is malformed")
        if not re.fullmatch(r"second_source_[0-9]+", item["role"]):
            raise RetainError("manifest secondary-source role is invalid")
    scenario, _ = _read_json(_artifact_file(directory, "scenario.json"))
    if scenario != manifest.get("scenario"):
        raise RetainError("retained scenario does not match manifest parsed scenario")
    scenario_input = manifest.get("scenario_input")
    if (not isinstance(scenario_input, dict) or set(scenario_input) != {"path_at_render", "sha256", "bytes"} or
            not isinstance(scenario_input["path_at_render"], str) or not _uint64(scenario_input["bytes"]) or
            scenario_input["sha256"] != identity["scenario_sha256"]):
        raise RetainError("manifest scenario input provenance is malformed")
    if scenario_input["bytes"] != _artifact_file(directory, "scenario.json").stat().st_size:
        raise RetainError("retained scenario size does not match its provenance")
    if sidecar["scenario_path"] != scenario_input["path_at_render"] or sidecar["source_path"] != source_identity["path_at_render"]:
        raise RetainError("renderer report input paths do not match manifest provenance")
    scenario_stages = scenario.get("impairments", []) if isinstance(scenario, dict) else []
    expected_secondary = [(f"second_source_{index}", stage.get("source_path"))
                          for index, stage in enumerate(scenario_stages)
                          if isinstance(stage, dict) and stage.get("type") == "second_source"]
    if [(item["role"], item["configured_path"]) for item in secondary] != expected_secondary:
        raise RetainError("manifest secondary sources do not match retained scenario")
    archived = manifest.get("archived_inputs")
    if not isinstance(archived, list):
        raise RetainError("manifest archived_inputs must be an array")
    expected_archive = {"inputs/primary.wav": identity["source_sha256"],
                        "inputs/scenario.json": identity["scenario_sha256"]}
    if manifest["retention"] == "archive":
        expected_archive.update({f"inputs/{item['role']}.wav": item["sha256"] for item in secondary})
    elif archived:
        raise RetainError("non-archive artifact must not claim archived inputs")
    if manifest["retention"] == "archive":
        actual_archive: dict[str, str] = {}
        root = directory.resolve(strict=True)
        for item in archived:
            relative = item.get("path") if isinstance(item, dict) else None
            digest = item.get("sha256") if isinstance(item, dict) else None
            relative_path = Path(relative) if isinstance(relative, str) else Path(".")
            if (not isinstance(relative, str) or relative_path.is_absolute() or relative_path.drive or
                    ".." in relative_path.parts or "\\" in relative or ":" in relative or
                    not isinstance(digest, str) or not _HEX256.fullmatch(digest)):
                raise RetainError("archive input reference is unsafe or malformed")
            resolved = _artifact_file(directory, relative)
            try:
                resolved.relative_to(root)
            except ValueError as error:
                raise RetainError("archive input path escapes artifact directory") from error
            if relative in actual_archive or not resolved.is_file() or _hash_file(resolved) != digest:
                raise RetainError(f"archived input hash does not match: {relative}")
            actual_archive[relative] = digest
        if actual_archive != expected_archive:
            raise RetainError("archive does not contain the exact required source and scenario copies")
    return {"ok": True, "directory": str(directory.resolve()), "frames": frames,
            "wav_sha256": wav_hash, "pcm_sha256": pcm_hash, "output_digest": f"{fnv:016x}",
            "retention": manifest.get("retention"), "manifest": manifest}


def verify(directory: str | os.PathLike[str]) -> dict[str, Any]:
    """Verify retained manifest, scenario/report, output WAV and archive copies."""
    return _verify_directory(Path(directory).resolve())


def prepare(renderer: str | os.PathLike[str], source: str | os.PathLike[str], scenario: str | os.PathLike[str],
            output_dir: str | os.PathLike[str], *, seed: int | None = None, retention: str = "keep",
            reuse: bool = False) -> dict[str, Any]:
    """Render and publish a new complete artifact directory without overwriting."""
    if retention not in ("temporary", "keep", "archive"):
        raise RetainError("retention must be temporary, keep, or archive")
    if seed is not None and not _uint64(seed):
        raise RetainError("seed must be a uint64 integer")
    renderer_path = Path(renderer).resolve(strict=True)
    source_path = Path(source).resolve(strict=True)
    scenario_path = Path(scenario).resolve(strict=True)
    output_path = Path(output_dir).resolve(strict=False)
    for path, description in ((renderer_path, "renderer"), (source_path, "source"), (scenario_path, "scenario")):
        if not path.is_file():
            raise RetainError(f"{description} is not a regular file: {path}")
    scenario_value, scenario_raw = _read_json(scenario_path)
    if not isinstance(scenario_value, dict):
        raise RetainError("scenario root must be a JSON object")
    secondary = _source_paths(scenario_value, scenario_path)
    actual_seed = seed if seed is not None else _scenario_seed(scenario_value.get("seed", 0))
    request = _identity(scenario_raw, scenario_value, source_path, secondary, renderer_path, actual_seed, retention)
    if reuse:
        result = _verify_directory(output_path)
        manifest = result["manifest"]
        if manifest.get("request_identity") != request:
            raise RetainError("existing artifact request identity differs; refusing reuse")
        return result
    if output_path.exists():
        raise RetainError(f"refusing to overwrite existing output directory: {output_path}")
    if output_path in (source_path, scenario_path, renderer_path) or any(output_path == path for _, path, _ in secondary):
        raise RetainError("output directory aliases an input")
    if output_path == Path(output_path.anchor):
        raise RetainError("output directory cannot be a filesystem root")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{output_path.name}.incomplete-", dir=output_path.parent))
    _write_durable(stage / "INCOMPLETE.json", _canonical_json({"schema": SCHEMA, "complete": False}))
    try:
        saved_scenario = stage / "scenario.json"
        _write_durable(saved_scenario, scenario_raw)
        output_wav = stage / "output.wav"
        sidecar = stage / "renderer.json"
        stdout_path = stage / "renderer.stdout.txt"
        stderr_path = stage / "renderer.stderr.txt"
        command = [str(renderer_path), "--scenario", str(scenario_path), "--source", str(source_path),
                   "--output", str(output_wav), "--sidecar", str(sidecar), "--block", str(BLOCK_FRAMES)]
        if seed is not None:
            command.extend(("--seed", str(seed)))
        render_started = time.monotonic()
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            completed = subprocess.run(command, stdout=stdout, stderr=stderr, check=False, shell=False)
        render_duration = time.monotonic() - render_started
        if completed.returncode != 0:
            raise RetainError(f"renderer exited with {completed.returncode}; incomplete work retained at {stage}")
        for generated in (output_wav, sidecar, stdout_path, stderr_path):
            _sync_file(generated)
        report, report_raw = _sidecar(sidecar)
        frames, wav_hash, pcm_hash, fnv = _wav_payload(output_wav)
        if report["frames_out"] != frames or report["seed"] != actual_seed:
            raise RetainError("renderer sidecar frame count or seed does not match request")
        if report["scenario_path"] != str(scenario_path) or report["source_path"] != str(source_path):
            raise RetainError("renderer sidecar paths do not match requested inputs")
        if f"{fnv:016x}" != report["output_digest"].lower():
            raise RetainError("rendered PCM digest does not match renderer sidecar")
        # Detect input replacement/modification during the renderer invocation.
        scenario_after_raw = scenario_path.read_bytes()
        scenario_after, _ = _read_json(scenario_path)
        if not isinstance(scenario_after, dict):
            raise RetainError("scenario changed to a non-object while rendering")
        secondary_after = _source_paths(scenario_after, scenario_path)
        after_request = _identity(scenario_after_raw, scenario_after, source_path, secondary_after,
                                  renderer_path, actual_seed, retention)
        if request != after_request:
            raise RetainError("an input changed while rendering; incomplete work retained")
        archived: list[dict[str, str]] = []
        if retention == "archive":
            copies = [("inputs/primary.wav", source_path, request["source_sha256"])]
            copies.extend((f"inputs/{role}.wav", path, item["sha256"])
                          for (role, path, _configured), item in zip(secondary, request["secondary_sources"]))
            copies.append(("inputs/scenario.json", scenario_path, request["scenario_sha256"]))
            for relative, original, digest in copies:
                _safe_archive_copy(original, stage / relative, digest)
                archived.append({"path": relative, "sha256": digest})
        manifest = {
            "schema": SCHEMA,
            "complete": True,
            "retention": retention,
            "request_identity": request,
            "renderer": {"path_at_render": str(renderer_path), "sha256": request["renderer_sha256"]},
            "renderer_command": command,
            "render_duration_seconds": render_duration,
            "scenario": scenario_value,
            "scenario_sha256": request["scenario_sha256"],
            "scenario_input": {"path_at_render": str(scenario_path), "sha256": request["scenario_sha256"],
                               "bytes": len(scenario_raw)},
            "source": {"path_at_render": str(source_path), "sha256": request["source_sha256"],
                       "bytes": source_path.stat().st_size},
            "secondary_sources": request["secondary_sources"],
            "archived_inputs": archived,
            "generation": {"block_frames": BLOCK_FRAMES, "sample_rate_hz": SAMPLE_RATE_HZ,
                           "format": "mono_pcm24", "seed": actual_seed},
            "renderer_report_sha256": hashlib.sha256(report_raw).hexdigest(),
            "renderer_stdout_sha256": _hash_file(stdout_path),
            "renderer_stderr_sha256": _hash_file(stderr_path),
            "frames": frames,
            "wav_sha256": wav_hash,
            "pcm_sha256": pcm_hash,
            "output_digest": f"{fnv:016x}",
        }
        _write_durable(stage / "manifest.json", _canonical_json(manifest))
        _verify_directory(stage, allow_incomplete=True)
        # Reserve the destination with mkdir(exist_ok=False), which is an
        # atomic no-overwrite operation. The marker makes partial publication
        # fail closed if copying or verification fails.
        try:
            output_path.mkdir()
        except OSError as error:
            raise RetainError(f"cannot reserve output directory (private work retained at {stage}): {error}") from error
        try:
            _write_durable(output_path / "INCOMPLETE.json", _canonical_json({"schema": SCHEMA, "complete": False}))
        except BaseException:
            try:
                output_path.rmdir()
            except OSError:
                pass
            raise
        _copy_tree_exclusive(stage, output_path, skip=frozenset({"INCOMPLETE.json", "manifest.json"}))
        _copy_file_exclusive(stage / "manifest.json", output_path / "manifest.json")
        _verify_directory(output_path, allow_incomplete=True)
        (output_path / "INCOMPLETE.json").unlink()
        _remove_private_stage(stage, output_path)
        return _verify_directory(output_path)
    except BaseException:
        # The uniquely named staging directory is deliberately retained for
        # diagnosis, and is never eligible for --reuse.
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    command = actions.add_parser("prepare", help="render and retain an offline impaired WAV")
    command.add_argument("--renderer", required=True, type=Path)
    command.add_argument("--source", required=True, type=Path)
    command.add_argument("--scenario", required=True, type=Path)
    command.add_argument("--output-dir", required=True, type=Path)
    command.add_argument("--seed", type=int)
    command.add_argument("--retention", choices=("temporary", "keep", "archive"), default="keep")
    command.add_argument("--reuse", action="store_true")
    check = actions.add_parser("verify", help="verify a previously retained render")
    check.add_argument("directory", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = prepare(args.renderer, args.source, args.scenario, args.output_dir, seed=args.seed,
                         retention=args.retention, reuse=args.reuse) if args.action == "prepare" else verify(args.directory)
        print(json.dumps({key: value for key, value in result.items() if key != "manifest"}, sort_keys=True))
        return 0
    except (RetainError, OSError) as error:
        print(f"retain_waveform: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
