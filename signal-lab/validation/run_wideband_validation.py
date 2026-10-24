# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Host-only, resumable noise-spur evidence for the 6/9/12 kHz AWGN kernels.

The default campaign is three 5-hour mono PCM24 renders. Shorter durations are
useful for smoke checks, but are always reported as inconclusive. Renders and
analysis stay sequential and stream from disk; the campaign never retains the
whole signal in memory.

  python -B signal-lab/validation/run_wideband_validation.py --work <new-dir>
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import math
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

# Cap common BLAS/OpenMP pools before NumPy is imported by wv_analysis. The
# campaign also runs only one renderer/analyzer at a time.
for _thread_variable in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
    os.environ[_thread_variable] = "2"
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

import wv_analysis as wa  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
FS_HZ = 48_000
SCHEMA = "signal-lab.wideband-validation/1"
DEFAULT_DURATION_SECONDS = 5 * 60 * 60
BANDS_HZ = (6_000, 9_000, 12_000)
REFERENCE_DBFS = -12.0
REFERENCE_RMS = 10.0 ** (REFERENCE_DBFS / 20.0)
SNR_DB = 10.0
SPUR_LIMIT_DBC = -70.0
CONTROL_DBC = -68.0
FOLD_PERIODS = (2048,)
FOLD_DERIVED = {1024: 2048, 256: 1024, 64: 256}
FOLD_OUTPUT_PERIODS = (2048, 1024, 256, 64)
START_MARGIN_FRAMES = FS_HZ
END_MARGIN_FRAMES = FS_HZ
BELOW_NORMAL = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
GATE_HOLD_EXIT = 3
DEFAULT_HOLD_MARKERS = (
    REPO / "build/main/bringup/AB_HOLD",
)


@dataclass(frozen=True)
class Config:
    duration_seconds: float = float(DEFAULT_DURATION_SECONDS)
    bands_hz: tuple[int, ...] = BANDS_HZ
    seed: int = 1
    snr_db: float = SNR_DB
    reference_rms: float = REFERENCE_RMS
    spur_limit_dbc: float = SPUR_LIMIT_DBC
    control_dbc: float = CONTROL_DBC
    line_segment_seconds: float = 100.0
    deadline_utc: str | None = None

    @property
    def frames(self) -> int:
        return int(round(self.duration_seconds * FS_HZ))

    def validate(self) -> None:
        if not math.isfinite(self.duration_seconds) or self.duration_seconds <= 0:
            raise ValueError("duration_seconds must be finite and positive")
        exact_frames = self.duration_seconds * FS_HZ
        if abs(exact_frames - round(exact_frames)) > 1e-7:
            raise ValueError("duration_seconds must resolve to a whole 48 kHz frame")
        min_seconds = (START_MARGIN_FRAMES + END_MARGIN_FRAMES + max(FOLD_OUTPUT_PERIODS)) / FS_HZ
        if self.duration_seconds <= min_seconds:
            raise ValueError(f"duration_seconds must exceed {min_seconds:.9f} s for trimmed harmonic folds")
        if self.bands_hz != BANDS_HZ:
            raise ValueError(f"bands_hz must be exactly {BANDS_HZ}")
        if not isinstance(self.seed, int) or isinstance(self.seed, bool) or self.seed < 0:
            raise ValueError("seed must be a non-negative integer")
        if not math.isfinite(self.snr_db) or self.snr_db != SNR_DB:
            raise ValueError(f"snr_db is fixed by this evidence contract at {SNR_DB:g} dB")
        if not math.isfinite(self.reference_rms) or self.reference_rms <= 0:
            raise ValueError("reference_rms must be finite and positive")
        if not math.isclose(self.reference_rms, REFERENCE_RMS, rel_tol=0.0, abs_tol=1e-15):
            raise ValueError(f"reference_rms must represent {REFERENCE_DBFS:g} dBFS")
        if not math.isfinite(self.spur_limit_dbc) or self.spur_limit_dbc != SPUR_LIMIT_DBC:
            raise ValueError(f"spur_limit_dbc is fixed by this evidence contract at {SPUR_LIMIT_DBC:g} dBc")
        if not math.isfinite(self.control_dbc) or self.control_dbc != CONTROL_DBC:
            raise ValueError(f"control_dbc is fixed by this evidence contract at {CONTROL_DBC:g} dBc")
        if not math.isfinite(self.line_segment_seconds) or self.line_segment_seconds <= 0.0:
            raise ValueError("line_segment_seconds must be finite and positive")
        if self.deadline_utc is not None:
            parse_deadline(self.deadline_utc)

    def as_json(self) -> dict[str, Any]:
        return {
            "duration_seconds": self.duration_seconds,
            "frames": self.frames,
            "bands_hz": list(self.bands_hz),
            "seed": self.seed,
            "snr_db": self.snr_db,
            "source_gain_db": 0.0,
            "reference_dbfs": REFERENCE_DBFS,
            "reference_rms": self.reference_rms,
            "spur_limit_dbc": self.spur_limit_dbc,
            "positive_control_dbc": self.control_dbc,
            "line_segment_seconds": self.line_segment_seconds,
            "deadline_utc": self.deadline_utc,
            "fold_periods": list(FOLD_OUTPUT_PERIODS),
            "fold_injected_control_dbc": self.control_dbc,
            "analysis_margins_frames": [START_MARGIN_FRAMES, END_MARGIN_FRAMES],
            "acceptance_duration_seconds": DEFAULT_DURATION_SECONDS,
        }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def parse_deadline(value: str) -> dt.datetime:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ValueError("deadline_utc must be an ISO-8601 UTC timestamp ending in Z")
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise ValueError("deadline_utc must be an ISO-8601 UTC timestamp ending in Z") from error
    if parsed.tzinfo != dt.timezone.utc:
        raise ValueError("deadline_utc must be UTC")
    return parsed


def write_json_atomic(path: Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    if temporary.exists():
        raise FileExistsError(f"refusing to replace leftover temporary file: {temporary}")
    with temporary.open("xb") as stream:
        stream.write(canonical_json(value))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def write_bytes_exclusive_or_equal(path: Path, content: bytes) -> None:
    if path.exists():
        if path.read_bytes() != content:
            raise FileExistsError(f"refusing to overwrite non-identical artifact: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())


def scenario_for(bandwidth_hz: int, seed: int) -> dict[str, Any]:
    return {
        "schema": "signal-lab.scenario/1",
        "test_id": f"wideband-noise-{bandwidth_hz}hz",
        "seed": seed,
        "source_gain_db": 0.0,
        "clip_policy": "reject",
        "noise_bandwidth_hz": bandwidth_hz,
        "impairments": [{"type": "awgn", "snr_db": SNR_DB}],
    }


def evaluate_row(line_search: dict[str, Any], folds: dict[str, Any], *, duration_seconds: float,
                 limit_dbc: float = SPUR_LIMIT_DBC, control_dbc: float = CONTROL_DBC) -> dict[str, Any]:
    """Apply the evidence gates without turning an inadequate measurement floor into a pass/fail."""
    errors: list[str] = []
    floor_reasons: list[str] = []
    smoke_reasons: list[str] = []
    control = folds.get("positive_control")

    line_floor = line_search.get("floor_dbc")
    if not isinstance(line_floor, (int, float)) or not math.isfinite(float(line_floor)):
        floor_reasons.append("line-search floor is unavailable")
    elif float(line_floor) >= limit_dbc:
        floor_reasons.append(f"line-search floor {float(line_floor):.3f} dBc does not clear {limit_dbc:g} dBc")

    detected_lines = line_search.get("detected")
    if not isinstance(detected_lines, int) or isinstance(detected_lines, bool) or detected_lines < 0:
        floor_reasons.append("line-search detection count is unavailable")
    line_worst = line_search.get("worst_line_dbc")
    if line_worst is None and detected_lines not in (0, None):
        floor_reasons.append("line-search detected lines but omitted their measured level")
    elif line_worst is not None and (not isinstance(line_worst, (int, float)) or isinstance(line_worst, bool) or
                                     not math.isfinite(float(line_worst))):
        floor_reasons.append("line-search worst-line level is malformed")
    if isinstance(line_worst, (int, float)) and math.isfinite(float(line_worst)) and float(line_worst) > limit_dbc:
        errors.append(f"line-search spur {float(line_worst):.3f} dBc exceeds {limit_dbc:g} dBc")

    fold_rows = folds.get("folds")
    if not isinstance(fold_rows, list) or {row.get("period") for row in fold_rows if isinstance(row, dict)} != set(FOLD_OUTPUT_PERIODS):
        floor_reasons.append("harmonic-fold result does not contain all required periods")
        fold_rows = fold_rows if isinstance(fold_rows, list) else []
    for row in fold_rows:
        if not isinstance(row, dict):
            floor_reasons.append("malformed harmonic-fold result")
            continue
        floor = row.get("threshold_dbc")
        worst = row.get("worst_dbc")
        period = row.get("period", "?")
        if not isinstance(floor, (int, float)) or not math.isfinite(float(floor)):
            floor_reasons.append(f"period {period} detection floor is unavailable")
        elif float(floor) >= limit_dbc:
            floor_reasons.append(f"period {period} detection floor {float(floor):.3f} dBc does not clear {limit_dbc:g} dBc")
        if not isinstance(worst, (int, float)) or isinstance(worst, bool) or not math.isfinite(float(worst)):
            floor_reasons.append(f"period {period} worst-harmonic level is unavailable")
        else:
            above = row.get("above_threshold")
            if not isinstance(above, int) or isinstance(above, bool) or above < 0:
                floor_reasons.append(f"period {period} above-threshold count is unavailable")
            elif float(worst) > limit_dbc:
                statistically_detected = (
                    above > 0 and isinstance(floor, (int, float)) and not isinstance(floor, bool) and
                    math.isfinite(float(floor)) and float(worst) > float(floor)
                )
                if statistically_detected:
                    errors.append(f"period {period} harmonic {float(worst):.3f} dBc exceeds {limit_dbc:g} dBc")
                elif not any(reason.startswith(f"period {period} detection floor") for reason in floor_reasons):
                    floor_reasons.append(f"period {period} raw maximum exceeds limit without a statistically detected line")

    control_result = evaluate_positive_control(control, fold_rows, control_dbc=control_dbc)
    errors.extend(control_result["errors"])
    floor_reasons.extend(control_result["inconclusive_reasons"])
    control_ok = control_result["status"] == "pass"

    full_duration = duration_seconds >= DEFAULT_DURATION_SECONDS
    if not full_duration:
        smoke_reasons.append("short render is smoke-only and cannot satisfy the 5-hour acceptance row")
    if errors:
        status = "fail"
    elif floor_reasons or smoke_reasons:
        status = "inconclusive"
    else:
        status = "pass"
    return {
        "status": status,
        "acceptance_eligible": full_duration and status == "pass",
        "control_pass": control_ok,
        "control_status": control_result["status"],
        "control_period": control_result["period"],
        "control_detection_floor_dbc": control_result["floor_dbc"],
        "floor_pass": not floor_reasons,
        "errors": errors,
        "inconclusive_reasons": floor_reasons + smoke_reasons,
        "limit_dbc": limit_dbc,
        "line_search_floor_dbc": line_floor,
        "line_search_worst_dbc": line_worst,
        "fold_floor_dbc": {str(row.get("period")): row.get("threshold_dbc") for row in fold_rows if isinstance(row, dict)},
        "fold_worst_dbc": {str(row.get("period")): row.get("worst_dbc") for row in fold_rows if isinstance(row, dict)},
    }


def evaluate_positive_control(control: Any, fold_rows: list[Any], *, control_dbc: float) -> dict[str, Any]:
    """Classify control evidence against the threshold for the control's own fold period."""
    invalid = {"status": "fail", "period": None, "floor_dbc": None,
               "errors": ["positive control is missing or malformed"], "inconclusive_reasons": []}
    if not isinstance(control, dict):
        return invalid
    period = control.get("period")
    injected = control.get("injected_dbc")
    measured = control.get("measured_dbc")
    detected = control.get("detected")
    if (not isinstance(period, int) or isinstance(period, bool) or period <= 0 or
            not isinstance(injected, (int, float)) or isinstance(injected, bool) or not math.isfinite(float(injected)) or
            not math.isclose(float(injected), control_dbc, abs_tol=1e-9) or
            not isinstance(measured, (int, float)) or isinstance(measured, bool) or not math.isfinite(float(measured)) or
            not isinstance(detected, bool)):
        return invalid
    matching = [row for row in fold_rows if isinstance(row, dict) and row.get("period") == period]
    if len(matching) != 1:
        return {**invalid, "errors": [f"positive control period {period} has no unique matching harmonic-fold result"]}
    passed = detected and abs(float(measured) - control_dbc) <= 1.0
    if passed:
        threshold = matching[0].get("threshold_dbc")
        floor = (float(threshold) if isinstance(threshold, (int, float)) and not isinstance(threshold, bool)
                 and math.isfinite(float(threshold)) else None)
        return {"status": "pass", "period": period, "floor_dbc": floor,
                "errors": [], "inconclusive_reasons": []}
    floor = matching[0].get("threshold_dbc")
    if (not isinstance(floor, (int, float)) or isinstance(floor, bool) or not math.isfinite(float(floor))):
        return {"status": "inconclusive", "period": period, "floor_dbc": floor,
                "errors": [], "inconclusive_reasons": [f"period {period} positive-control detection floor is unavailable"]}
    # The control must be resolvable across its accepted +/-1 dB measurement range.
    # A threshold at or above -69 dBc cannot support a failed verdict for the -68 dBc control.
    if float(floor) >= control_dbc - 1.0:
        return {"status": "inconclusive", "period": period, "floor_dbc": float(floor),
                "errors": [], "inconclusive_reasons": [
                    f"period {period} positive-control floor {float(floor):.3f} dBc cannot resolve the {control_dbc:g} dBc control"
                ]}
    return {"status": "fail", "period": period, "floor_dbc": float(floor),
            "errors": [f"{control_dbc:g} dBc harmonic-fold positive control was missed or measured outside 1 dB"],
            "inconclusive_reasons": []}


def analysis_call(analysis: Any, wav: Path, frames: int, bandwidth_hz: int, *,
                  segment_seconds: float = 100.0, gate: Callable[[], None]) -> dict[str, Any]:
    start = START_MARGIN_FRAMES
    count = frames - START_MARGIN_FRAMES - END_MARGIN_FRAMES
    band_hz = (300.0, 300.0 + bandwidth_hz)
    # For smoke renders, bound the Welch segment by available evidence. The
    # effective value is recorded so a short run cannot masquerade as 100 s.
    segment_seconds = min(segment_seconds, count / FS_HZ)
    gate()
    lines = analysis.line_search(wav, start, count, REFERENCE_RMS ** 2, [], [], segment_seconds, band_hz=band_hz)
    gate()
    folds = analysis.fold_harmonics(wav, start, count, REFERENCE_RMS ** 2, FOLD_PERIODS, FOLD_DERIVED,
                                    inject_dbc=CONTROL_DBC, band_hz=band_hz)
    verdict = evaluate_row(lines, folds, duration_seconds=frames / FS_HZ)
    return {
        "band_hz": list(band_hz),
        "analysis_interval_frames": [start, start + count],
        "line_search_segment_seconds": segment_seconds,
        "moment_frame_count": folds.get("frames_folded"),
        "moment_duration_seconds": (folds.get("frames_folded") / FS_HZ
                                     if isinstance(folds.get("frames_folded"), int) else None),
        "moment_field_context": "Historical *_5h field names are retained; values cover only the frames_folded interval reported here.",
        "line_search": lines,
        "fold_harmonics": folds,
        "verdict": verdict,
    }


def default_gate(hold_markers: tuple[Path, ...] = DEFAULT_HOLD_MARKERS) -> None:
    """Check the shared-PC hold directly before each heavy process or analyzer call."""
    held = [str(path) for path in hold_markers if path.exists()]
    if held:
        raise RuntimeError("AB_HOLD is present; refusing render/analysis: " + ", ".join(held))


def campaign_gate(hold_markers: tuple[Path, ...], deadline_utc: str | None, *,
                  now_utc: Callable[[], dt.datetime] = lambda: dt.datetime.now(dt.timezone.utc),
                  hold_check: Callable[[tuple[Path, ...]], None] = default_gate) -> Callable[[], None]:
    """Check AB_HOLD and UTC cutoff at each call boundary; active work is not interrupted."""
    deadline = parse_deadline(deadline_utc) if deadline_utc is not None else None

    def check() -> None:
        hold_check(hold_markers)
        if deadline is not None and now_utc() >= deadline:
            raise RuntimeError(f"deadline_utc {deadline_utc} reached; refusing to start the next render or analysis step")

    return check


def expected_rows(config: Config) -> list[dict[str, Any]]:
    rows = []
    for bandwidth_hz in config.bands_hz:
        scenario = scenario_for(bandwidth_hz, config.seed)
        rows.append({"bandwidth_hz": bandwidth_hz, "scenario": scenario,
                     "scenario_sha256": hashlib.sha256(canonical_json(scenario)).hexdigest()})
    return rows


def repo_head_context() -> dict[str, Any]:
    try:
        result = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True,
                                timeout=5, creationflags=BELOW_NORMAL)
    except (OSError, subprocess.TimeoutExpired):
        return {"value": None, "provenance": "git rev-parse HEAD unavailable"}
    value = result.stdout.strip()
    if result.returncode == 0 and value and all(character in "0123456789abcdefABCDEF" for character in value):
        return {"value": value, "provenance": "git rev-parse HEAD; context only, does not identify the renderer build"}
    return {"value": None, "provenance": f"git rev-parse HEAD unavailable (exit {result.returncode})"}


def build_identity(config: Config, renderer: Path, *, renderer_sha256: str,
                   runner_path: Path = Path(__file__).resolve(), analysis_path: Path = Path(wa.__file__).resolve()) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "config": config.as_json(),
        "scenarios": [{"bandwidth_hz": row["bandwidth_hz"], "sha256": row["scenario_sha256"]}
                      for row in expected_rows(config)],
        "renderer": {"path": str(renderer.resolve()), "binary_sha256": renderer_sha256},
        "repo_head_context": repo_head_context(),
        "analysis": {"path": str(analysis_path.resolve()), "sha256": sha256_file(analysis_path)},
        "runner": {"path": str(runner_path.resolve()), "sha256": sha256_file(runner_path)},
    }


def validate_resume(root: Path, identity: dict[str, Any], plans: list[dict[str, Any]]) -> dict[str, Any]:
    manifest_path = root / "manifest.json"
    if not manifest_path.is_file():
        raise FileExistsError("--resume requires an existing manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("identity") != identity:
        raise ValueError("resume identity differs (scenario/config, renderer, analysis or runner); refusing reuse")
    expected_ids = {f"noise-{plan['bandwidth_hz'] // 1000}k" for plan in plans}
    if set(manifest.get("planned_row_ids", [])) != expected_ids:
        raise ValueError("resume planned rows differ; refusing reuse")
    completed = manifest.get("rows", {})
    if not isinstance(completed, dict) or not set(completed).issubset(expected_ids):
        raise ValueError("resume manifest has unknown or malformed rows")
    for row_id, row in completed.items():
        for key in ("scenario", "wav", "sidecar", "analysis"):
            artifact = row.get("artifacts", {}).get(key)
            if not isinstance(artifact, dict):
                raise ValueError(f"resume row {row_id} lacks artifact identity for {key}")
            relative = Path(artifact["path"])
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"resume artifact path escapes campaign: {relative}")
            path = (root / relative).resolve()
            if not path.is_relative_to(root):
                raise ValueError(f"resume artifact path escapes campaign: {relative}")
            if not path.is_file() or sha256_file(path) != artifact.get("sha256"):
                raise ValueError(f"resume artifact changed or missing: {path}")
    return manifest


def check_unowned_files(root: Path, plans: list[dict[str, Any]], completed: dict[str, Any]) -> None:
    known: set[Path] = {root / "manifest.json", root / "report.json", root / "summary.md"}
    for plan in plans:
        row_id = f"noise-{plan['bandwidth_hz'] // 1000}k"
        known.update({root / "scenarios" / f"{row_id}.json", root / "renders" / f"{row_id}.wav",
                      root / "renders" / f"{row_id}.render.json", root / "rows" / f"{row_id}.json"})
    for path in root.rglob("*"):
        if path.is_file() and path not in known:
            raise FileExistsError(f"refusing unrecognized existing artifact: {path}")
        if path.is_file() and path.name.endswith(".tmp"):
            raise FileExistsError(f"refusing leftover temporary artifact: {path}")
    for plan in plans:
        row_id = f"noise-{plan['bandwidth_hz'] // 1000}k"
        if row_id in completed:
            continue
        for path in (root / "renders" / f"{row_id}.wav", root / "renders" / f"{row_id}.render.json",
                     root / "rows" / f"{row_id}.json"):
            if path.exists():
                raise FileExistsError(f"refusing unrecorded partial artifact: {path}")


def render_process(renderer: Path, scenario_path: Path, wav: Path, sidecar: Path, config: Config,
                   *, gate: Callable[[], None]) -> dict[str, Any]:
    gate()
    command = [str(renderer), "--scenario", str(scenario_path), "--silence-seconds", f"{config.duration_seconds:.12g}",
               "--reference-rms", repr(config.reference_rms), "--output", str(wav), "--sidecar", str(sidecar)]
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, creationflags=BELOW_NORMAL)
    if result.returncode != 0:
        raise RuntimeError(f"renderer failed ({result.returncode}): {result.stdout}{result.stderr}")
    return {"argv": command, "wall_seconds": round(time.monotonic() - started, 3),
            "stdout": result.stdout.strip()[-2000:], "stderr": result.stderr.strip()[-2000:]}


def validate_sidecar(sidecar: dict[str, Any], config: Config, bandwidth_hz: int) -> None:
    if sidecar.get("schema") != "signal-lab.watterson-render/1" or sidecar.get("tool") != "wfg_watterson_render":
        raise ValueError("renderer sidecar identity is unexpected")
    if sidecar.get("tool_version") != "1":
        raise ValueError("renderer tool_version is not the expected version 1")
    if sidecar.get("frames_in") != config.frames or sidecar.get("frames_out") != config.frames:
        raise ValueError("renderer frame count differs from requested duration")
    if sidecar.get("clipped_samples") != 0:
        raise ValueError("renderer clipped samples under clip_policy=reject")
    if sidecar.get("noise_bandwidth_hz") != bandwidth_hz or sidecar.get("noise_band_low_hz") != 300.0 or \
            sidecar.get("noise_band_high_hz") != 300.0 + bandwidth_hz:
        raise ValueError("renderer sidecar noise band differs from scenario")
    reference_rms = sidecar.get("reference_rms")
    if (not isinstance(reference_rms, (int, float)) or isinstance(reference_rms, bool) or
            not math.isfinite(float(reference_rms)) or
            not math.isclose(float(reference_rms), config.reference_rms, rel_tol=0.0, abs_tol=1e-14)):
        raise ValueError("renderer sidecar reference RMS differs from contract")
    source_gain_db = sidecar.get("source_gain_db")
    if (not isinstance(source_gain_db, (int, float)) or isinstance(source_gain_db, bool) or
            not math.isfinite(float(source_gain_db)) or
            not math.isclose(float(source_gain_db), 0.0, rel_tol=0.0, abs_tol=1e-12)):
        raise ValueError("renderer sidecar source gain differs from contract")
    if not isinstance(sidecar.get("source_digest"), str) or not sidecar["source_digest"]:
        raise ValueError("renderer sidecar has no silence-source digest")
    if not isinstance(sidecar.get("output_digest"), str) or not sidecar["output_digest"]:
        raise ValueError("renderer sidecar has no engine-output digest")


def summary_text(report: dict[str, Any]) -> str:
    lines = ["# Wideband noise-spur validation", "", f"Campaign status: **{report['status'].upper()}**", "",
             f"Duration: {report['identity']['config']['duration_seconds']:g} s; acceptance eligible: "
             f"{report['acceptance_eligible']}", "", "| Bandwidth | Status | Line floor (dBc) | Worst line (dBc) |", "|---:|---|---:|---:|"]
    for row in report["rows"]:
        verdict = row["analysis"]["verdict"]
        lines.append(f"| {row['bandwidth_hz']} Hz | {verdict['status']} | {verdict['line_search_floor_dbc']} | "
                     f"{verdict['line_search_worst_dbc']} |")
        interval = row["analysis"].get("analysis_interval_frames", [None, None])
        analyzed_frames = max(0, interval[1] - interval[0]) if len(interval) == 2 and all(isinstance(x, int) for x in interval) else 0
        moment_frames = row["analysis"].get("moment_frame_count")
        moment_text = (f"{moment_frames} frames ({moment_frames / FS_HZ:g} s)" if isinstance(moment_frames, int)
                       else "an unavailable frame count")
        lines.append(f"  - Analyzed {analyzed_frames} frames ({analyzed_frames / FS_HZ:g} s); fold moments cover "
                     f"{moment_text}. Historical `*_5h` fields "
                     "retain their names but do not imply that the full five-hour render was analyzed.")
        for reason in verdict["errors"] + verdict["inconclusive_reasons"]:
            lines.append(f"  - {reason}")
    lines += ["", "All levels are dBc relative to the -12 dBFS input reference. Analyses trim one second from each "
              "end of each render. AB_HOLD and --deadline-utc are checked at render/analysis call boundaries; "
              "they do not interrupt a renderer or analysis already in flight. Short renders are smoke checks, "
              "never acceptance evidence.", ""]
    return "\n".join(lines)


def run_campaign(root: Path, renderer: Path, config: Config, *, resume: bool = False,
                 render_fn: Callable[..., dict[str, Any]] | None = None, analysis: Any = wa,
                 gate: Callable[[], None] = default_gate, identity_override: dict[str, Any] | None = None) -> dict[str, Any]:
    config.validate()
    root = root.resolve()
    renderer = renderer.resolve()
    root.mkdir(parents=True, exist_ok=True)
    plans = expected_rows(config)
    if identity_override is None:
        if render_fn is not None:
            renderer_sha256 = "injected-test-renderer"
        else:
            if not renderer.is_file():
                raise FileNotFoundError(f"missing renderer: {renderer}")
            renderer_sha256 = sha256_file(renderer)
        identity = build_identity(config, renderer, renderer_sha256=renderer_sha256)
    else:
        identity = identity_override
    manifest_path = root / "manifest.json"
    if resume:
        manifest = validate_resume(root, identity, plans)
    else:
        if any(root.iterdir()):
            raise FileExistsError(f"work directory is non-empty; pass --resume only for exact verified reuse: {root}")
        manifest = {"schema": SCHEMA, "identity": identity,
                    "planned_row_ids": [f"noise-{p['bandwidth_hz'] // 1000}k" for p in plans], "rows": {}}
        write_json_atomic(manifest_path, manifest)
    check_unowned_files(root, plans, manifest["rows"])

    by_id = {row_id: row for row_id, row in manifest["rows"].items()}
    for plan in plans:
        row_id = f"noise-{plan['bandwidth_hz'] // 1000}k"
        if row_id in by_id:
            continue
        gate()
        scenario_path = root / "scenarios" / f"{row_id}.json"
        scenario_path.parent.mkdir(parents=True, exist_ok=True)
        if scenario_path.exists():
            if sha256_file(scenario_path) != plan["scenario_sha256"]:
                raise FileExistsError(f"existing scenario differs; refusing overwrite: {scenario_path}")
        else:
            with scenario_path.open("xb") as stream:
                stream.write(canonical_json(plan["scenario"]))
        # render_one creates the scenario with exclusive-create; it owns an already-verified one only
        # when resuming a partial run, so use a small wrapper to retain no-clobber semantics.
        # New campaigns write the scenario before invoking the renderer; partial resumes reuse bytes.
        row = render_one_existing(root, plan, config, renderer, render_fn=render_fn, analysis=analysis,
                                  gate=gate, scenario_already_written=True)
        by_id[row_id] = row
        manifest["rows"][row_id] = row
        write_json_atomic(manifest_path, manifest)

    rows = [by_id[f"noise-{plan['bandwidth_hz'] // 1000}k"] for plan in plans]
    statuses = [row["analysis"]["verdict"]["status"] for row in rows]
    status = "fail" if "fail" in statuses else "inconclusive" if "inconclusive" in statuses else "pass"
    report = {"schema": SCHEMA, "identity": identity, "status": status,
              "acceptance_eligible": config.duration_seconds >= DEFAULT_DURATION_SECONDS and status == "pass",
              "rows": rows, "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    summary_path = root / "summary.md"
    report_path = root / "report.json"
    if report_path.exists():
        existing_report = json.loads(report_path.read_text(encoding="utf-8"))
        for key in ("schema", "identity", "status", "acceptance_eligible", "rows"):
            if existing_report.get(key) != report.get(key):
                raise ValueError(f"existing report differs from verified campaign at {key}; refusing replacement")
        report_bytes = report_path.read_bytes()
        expected_summary = summary_text(existing_report).encode("utf-8")
        if summary_path.exists() and summary_path.read_bytes() != expected_summary:
            raise ValueError("existing summary differs from verified report; refusing replacement")
        if "reports" in manifest:
            for name, path, content in (("report", report_path, report_bytes), ("summary", summary_path, expected_summary)):
                record = manifest["reports"].get(name, {})
                if record.get("sha256") != hashlib.sha256(content).hexdigest() or not path.is_file():
                    raise ValueError(f"existing {name} hash differs from manifest; refusing reuse")
        write_bytes_exclusive_or_equal(summary_path, expected_summary)
        manifest["reports"] = {
            "report": {"path": "report.json", "sha256": hashlib.sha256(report_bytes).hexdigest()},
            "summary": {"path": "summary.md", "sha256": hashlib.sha256(expected_summary).hexdigest()},
        }
        write_json_atomic(manifest_path, manifest)
        return existing_report

    report_bytes = canonical_json(report)
    write_bytes_exclusive_or_equal(report_path, report_bytes)
    summary_bytes = summary_text(report).encode("utf-8")
    write_bytes_exclusive_or_equal(summary_path, summary_bytes)
    manifest["reports"] = {
        "report": {"path": "report.json", "sha256": hashlib.sha256(report_bytes).hexdigest()},
        "summary": {"path": "summary.md", "sha256": hashlib.sha256(summary_bytes).hexdigest()},
    }
    write_json_atomic(manifest_path, manifest)
    return report


def render_one_existing(root: Path, plan: dict[str, Any], config: Config, renderer: Path, *,
                        render_fn: Callable[..., dict[str, Any]] | None, analysis: Any,
                        gate: Callable[[], None], scenario_already_written: bool) -> dict[str, Any]:
    """Render using the already-created, hash-checked scenario and preserve no-overwrite semantics."""
    bandwidth_hz = int(plan["bandwidth_hz"])
    row_id = f"noise-{bandwidth_hz // 1000}k"
    scenario_path = root / "scenarios" / f"{row_id}.json"
    wav = root / "renders" / f"{row_id}.wav"
    sidecar_path = root / "renders" / f"{row_id}.render.json"
    analysis_path = root / "rows" / f"{row_id}.json"
    if not scenario_already_written or sha256_file(scenario_path) != plan["scenario_sha256"]:
        raise RuntimeError("internal scenario preflight mismatch")
    for path in (wav, sidecar_path, analysis_path):
        if path.exists():
            raise FileExistsError(f"refusing to overwrite existing output: {path}")
    wav.parent.mkdir(parents=True, exist_ok=True)
    analysis_path.parent.mkdir(parents=True, exist_ok=True)
    if render_fn is None:
        render_record = render_process(renderer, scenario_path, wav, sidecar_path, config, gate=gate)
    else:
        gate()
        render_record = render_fn(scenario_path, wav, sidecar_path, config)
    if not wav.is_file() or not sidecar_path.is_file():
        raise RuntimeError(f"renderer did not produce both WAV and sidecar for {row_id}")
    sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
    validate_sidecar(sidecar, config, bandwidth_hz)
    if wa.frame_count(wav) != config.frames:
        raise ValueError(f"WAV frame count differs from renderer sidecar for {row_id}")
    analysis_record = analysis_call(analysis, wav, config.frames, bandwidth_hz,
                                    segment_seconds=config.line_segment_seconds, gate=gate)
    row = {
        "row_id": row_id,
        "bandwidth_hz": bandwidth_hz,
        "scenario": plan["scenario"],
        "scenario_sha256": plan["scenario_sha256"],
        "source": {"kind": "digital_silence", "frames": config.frames, "source_digest": sidecar["source_digest"]},
        "renderer": {"tool": sidecar["tool"], "tool_version": sidecar["tool_version"],
                     "algorithm_version": sidecar.get("algorithm_version"), "output_digest": sidecar["output_digest"],
                     "wall_seconds": render_record.get("wall_seconds")},
        "render": {"stdout": render_record.get("stdout", ""), "stderr": render_record.get("stderr", "")},
        "analysis": analysis_record,
        "artifacts": {
            "scenario": {"path": str(scenario_path.relative_to(root)), "sha256": sha256_file(scenario_path)},
            "wav": {"path": str(wav.relative_to(root)), "sha256": sha256_file(wav), "bytes": wav.stat().st_size},
            "sidecar": {"path": str(sidecar_path.relative_to(root)), "sha256": sha256_file(sidecar_path)},
            # A report cannot contain its own hash; the enclosing manifest records it after writing.
            "analysis": {"path": str(analysis_path.relative_to(root)), "sha256": None},
        },
    }
    # The per-row report hash is recorded in the campaign manifest, not recursively in itself.
    write_json_atomic(analysis_path, row)
    row["artifacts"]["analysis"]["sha256"] = sha256_file(analysis_path)
    return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build/main/host-c", help="host build containing wfg_watterson_render")
    parser.add_argument("--renderer", type=Path, help="explicit renderer path (default: <build>/signal-lab/wfg_watterson_render[.exe])")
    parser.add_argument("--work", type=Path, help="campaign directory (default: <build>/work/wideband-validation)")
    parser.add_argument("--duration-seconds", type=float, default=float(DEFAULT_DURATION_SECONDS), help="render duration; below 18000 seconds is smoke-only")
    parser.add_argument("--line-segment-seconds", type=float, default=100.0,
                        help="Welch segment duration for arbitrary in-band lines (default: 100 s; shortened for smoke renders)")
    parser.add_argument("--deadline-utc", help="optional ISO-8601 UTC cutoff ending in Z; checked at render/analysis boundaries")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--resume", action="store_true", help="reuse only exact, hash-verified rows in this campaign directory")
    parser.add_argument("--hold-marker", type=Path, action="append", default=None,
                        help="additional AB_HOLD marker checked before each render/analysis (repeatable)")
    args = parser.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(newline="\n")
    if os.name == "nt":
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)
    config = Config(duration_seconds=args.duration_seconds, seed=args.seed,
                    line_segment_seconds=args.line_segment_seconds, deadline_utc=args.deadline_utc)
    try:
        config.validate()
        build = args.build.resolve()
        exe = ".exe" if os.name == "nt" else ""
        renderer = (args.renderer or build / "signal-lab" / f"wfg_watterson_render{exe}").resolve()
        root = (args.work or build / "work" / "wideband-validation").resolve()
        markers = tuple(DEFAULT_HOLD_MARKERS) + tuple(args.hold_marker or ())
        report = run_campaign(root, renderer, config, resume=args.resume,
                              gate=campaign_gate(markers, config.deadline_utc))
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"wideband-validation: {error}", file=sys.stderr)
        return 2
    print(f"wideband validation: {report['status']} ({root})")
    return 0 if report["status"] == "pass" else 1 if report["status"] == "fail" else 2


if __name__ == "__main__":
    raise SystemExit(main())
