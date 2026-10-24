#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
"""Compare a verified RT1170 WAV/JSN pair with an offline C++ retained render.

This is a read-only comparison of ordinary local files. It does not render,
write artifacts, access devices, or compare the different engine FNV digest
contracts. A valid input mismatch is uncomparable; differing output hashes are
a comparable mismatch.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any

if __package__:
    from . import prepared_artifacts, retain_waveform
else:
    import prepared_artifacts
    import retain_waveform


SCHEMA = "waveform-generator.prepared-comparison/2"
MAX_REFERENCE_JSON_BYTES = 1024 * 1024
_HEX40 = re.compile(r"^[0-9a-f]{40}$")


class ComparisonError(ValueError):
    """A prepared artifact or reference render is invalid or not comparable."""


def _directory(path: str | Path, label: str) -> Path:
    try:
        root = prepared_artifacts._safe_path(Path(path))
        if not root.is_dir():
            raise ComparisonError(f"{label} directory is not a directory: {root}")
        entries = prepared_artifacts._directory_entries(root)
    except ComparisonError:
        raise
    except (OSError, TypeError, ValueError) as error:
        raise ComparisonError(f"cannot safely inspect {label} directory: {error}") from error
    if any(entry.name.casefold() == "incomplete.json" for entry in entries):
        raise ComparisonError(f"{label} directory contains INCOMPLETE.json")
    return root


def _bound_reference_metadata(root: Path) -> None:
    """Require known retained JSON inputs to be regular, safe and at most one MiB."""
    for name in ("manifest.json", "renderer.json", "scenario.json"):
        path = prepared_artifacts._safe_path(root / name)
        if path.parent != root or not path.is_file():
            raise ComparisonError(f"reference {name} is missing or not a regular file")
        if path.stat().st_size > MAX_REFERENCE_JSON_BYTES:
            raise ComparisonError(f"reference {name} exceeds the one MiB metadata limit")


def _compare(board_directory: str | Path, name: str, reference_directory: str | Path, *,
             firmware_commit: str) -> dict[str, Any]:
    board_root = _directory(board_directory, "board artifact")
    reference_root = _directory(reference_directory, "reference artifact")
    if not isinstance(firmware_commit, str) or not _HEX40.fullmatch(firmware_commit):
        raise ComparisonError("expected firmware commit must be 40 lowercase hex digits")

    board_result = prepared_artifacts.verify(board_root, name)
    _bound_reference_metadata(reference_root)
    reference_result = retain_waveform.verify(reference_root)
    board_manifest = board_result["manifest"]
    reference_manifest = reference_result["manifest"]
    reference_identity = reference_manifest["request_identity"]

    if board_manifest["firmware_commit"] != firmware_commit:
        raise ComparisonError("board firmware commit differs from the expected identity")

    board_secondary = board_manifest["secondary"]
    reference_secondary = reference_identity["secondary_sources"]
    if board_secondary:
        if len(reference_secondary) != 1 or board_manifest["secondary_sha256"] != reference_secondary[0]["sha256"]:
            raise ComparisonError("board and reference secondary-source presence or hash differs")
    elif reference_secondary:
        raise ComparisonError("board and reference secondary-source presence differs")

    for label, board_value, reference_value in (
            ("source SHA-256", board_manifest["source_sha256"], reference_identity["source_sha256"]),
            ("scenario SHA-256", board_manifest["scenario_sha256"], reference_identity["scenario_sha256"]),
            ("seed", board_manifest["seed"], reference_identity["seed"])):
        if board_value != reference_value:
            raise ComparisonError(f"board and reference {label} differ")

    checks = {
        "frames": board_result["frames"] == reference_result["frames"],
        "wav_sha256": board_result["wav_sha256"] == reference_result["wav_sha256"],
        "pcm_sha256": board_result["pcm_sha256"] == reference_result["pcm_sha256"],
    }
    board_identity = {
        "seed": board_manifest["seed"],
        "source_sha256": board_manifest["source_sha256"],
        "scenario_sha256": board_manifest["scenario_sha256"],
        "secondary": {"present": bool(board_secondary), "sha256": board_manifest["secondary_sha256"]},
        "firmware_commit": board_manifest["firmware_commit"],
        "engine_algorithm_version": board_manifest["engine_algorithm_version"],
        "frames": board_result["frames"],
        "wav_sha256": board_result["wav_sha256"],
        "pcm_sha256": board_result["pcm_sha256"],
    }
    reference_secondary_identity = [{"role": item["role"], "sha256": item["sha256"]}
                                    for item in reference_secondary]
    reference_output_identity = {
        "seed": reference_identity["seed"],
        "source_sha256": reference_identity["source_sha256"],
        "scenario_sha256": reference_identity["scenario_sha256"],
        "secondary_sources": reference_secondary_identity,
        "renderer_sha256": reference_identity["renderer_sha256"],
        "frames": reference_result["frames"],
        "wav_sha256": reference_result["wav_sha256"],
        "pcm_sha256": reference_result["pcm_sha256"],
    }
    assurance = ("Exact stored WAV/PCM identity only; manifests are not authenticated provenance and this is not "
                 "PathSim statistical acceptance.")
    return {"schema": SCHEMA, "comparable": True, "match": all(checks.values()), "checks": checks,
            "assurance": assurance,
            "board": board_identity, "reference": reference_output_identity}


def compare(board_directory: str | Path, name: str, reference_directory: str | Path, *,
            firmware_commit: str) -> dict[str, Any]:
    """Compare verified identity and PCM output without rendering or modifying either artifact.

    Detailed view: reuse each format's own verifier, require matching input hashes/seed and the
    caller's exact board provenance, then compare frame count, whole-WAV SHA-256 and PCM SHA-256.
    A bad/unbound input raises ComparisonError; valid inputs with changed output return match=false.
    5th-grade view: check both reports, then check whether both files contain the same sound.
    """
    try:
        return _compare(board_directory, name, reference_directory, firmware_commit=firmware_commit)
    except ComparisonError:
        raise
    except (prepared_artifacts.PreparedArtifactError, retain_waveform.RetainError, OSError, TypeError,
            ValueError, RecursionError) as error:
        raise ComparisonError(str(error)) from error


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board_directory", type=Path)
    parser.add_argument("name", help="board artifact basename or .JSN filename")
    parser.add_argument("reference_directory", type=Path)
    parser.add_argument("--firmware-commit", required=True, help="expected 40-digit lowercase board firmware commit")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = compare(args.board_directory, args.name, args.reference_directory,
                         firmware_commit=args.firmware_commit)
    except ComparisonError as error:
        print(f"prepared comparison: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
    return 0 if result["match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
