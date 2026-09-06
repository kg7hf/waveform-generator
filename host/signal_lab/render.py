"""Scenario -> impaired WAV + JSON sidecar (the Phase 1 host renderer)."""

import hashlib
import json
import math
import os
import platform
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from . import BLOCK, FS, SIDECAR_SCHEMA, VERSION
from .scenario import ScenarioError, build_pipeline, scenario_sha256, validate
from .stream import amplitude_to_db, seconds_to_frames
from .wav_io import Pcm16Writer, pcm_region_sha256, quantize_pcm16, read_float, require_canonical, sha256_file, wav_info


class RenderError(RuntimeError):
    pass


def package_identity():
    """Hash of every signal_lab module so a sidecar pins the generator bytes."""
    root = Path(__file__).resolve().parent
    digest = hashlib.sha256()
    files = sorted(p for p in root.glob("*.py"))
    for path in files:
        digest.update(path.name.encode("utf-8") + b"\0" + path.read_bytes() + b"\0")
    return {"name": VERSION, "package_sha256": digest.hexdigest(), "files": [p.name for p in files],
            "python": platform.python_version(), "numpy": np.__version__}


def artifact(path):
    path = Path(path)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def wav_artifact(path):
    info = wav_info(path)
    art = artifact(path)
    art.update({"samples": info["samples"], "duration_seconds": info["samples"] / FS, "encoding": info["encoding"],
                "sample_rate_hz": info["sample_rate_hz"], "channels": info["channels"],
                "pcm": {"data_offset_bytes": info["data_offset_bytes"], "data_bytes": info["data_bytes"],
                        "sha256": pcm_region_sha256(path, info)}})
    return art


def load_source_sidecar(source_path):
    """A reference WAV carries its own sidecar (<name>.json); pass its identity through."""
    sidecar = Path(source_path).with_suffix(".json")
    if not sidecar.exists():
        return None
    try:
        with open(sidecar, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return None
    keep = {key: data[key] for key in ("schema", "test_id", "mode", "rate_bps", "interleave", "payload", "framing", "wav", "generator") if key in data}
    keep["sidecar"] = artifact(sidecar)
    return keep


def resolve_reference(samples, reference):
    if reference == "auto":
        nonzero = np.flatnonzero(samples)
        if len(nonzero) == 0:
            raise RenderError("source is silent; cannot measure a reference level")
        start, end = int(nonzero[0]), int(nonzero[-1]) + 1
    else:
        start = seconds_to_frames(reference["start_seconds"], "reference.start_seconds")
        end = start + seconds_to_frames(reference["duration_seconds"], "reference.duration_seconds", True)
        if end > len(samples):
            raise RenderError("reference interval extends beyond the source")
    segment = samples[start:end]
    rms = math.sqrt(float(np.dot(segment, segment)) / len(segment))
    if rms <= 0:
        raise RenderError("reference interval has zero RMS")
    return start, end, rms


def render(scenario, output_wav, sidecar_path=None, base_dir=None, overwrite=False, block=BLOCK, created_utc=None):
    """Render one scenario.  Returns the sidecar dict (also written to disk)."""
    scenario = validate(scenario)
    base = Path(base_dir) if base_dir else Path.cwd()
    source_path = Path(scenario["source"]["path"])
    if not source_path.is_absolute():
        source_path = base / source_path
    output_wav = Path(output_wav)
    sidecar_path = Path(sidecar_path) if sidecar_path else output_wav.with_suffix(".json")
    if not overwrite and (output_wav.exists() or sidecar_path.exists()):
        raise RenderError("refusing to overwrite %s" % output_wav)
    output_wav.parent.mkdir(parents=True, exist_ok=True)

    samples, info = read_float(source_path)
    require_canonical(info)
    total = len(samples)
    ref_start, ref_end, reference_rms = resolve_reference(samples, scenario["reference"])
    gain = 10.0 ** (scenario["source_gain_db"] / 20.0)
    scaled_reference = reference_rms * gain
    pipeline = build_pipeline(scenario)
    pipeline.prepare(scaled_reference, total, scenario["seed"])

    fd, temp_name = tempfile.mkstemp(prefix="." + output_wav.name + ".", suffix=".partial", dir=output_wav.parent)
    os.close(fd)
    temp = Path(temp_name)
    clipped = 0
    peak = 0.0
    energy = 0.0
    frames = 0
    first_clip = None
    try:
        with Pcm16Writer(temp) as writer:
            for out in pipeline.run(samples * gain, block=block):
                pcm, block_clipped = quantize_pcm16(out)
                if block_clipped and scenario["clip_policy"] == "reject":
                    scaled = out * 32768.0
                    index = int(np.flatnonzero((scaled < -32768.0) | (scaled > 32767.0))[0])
                    raise RenderError("clipping at output frame %d and clip_policy=reject" % (frames + index))
                if block_clipped and first_clip is None:
                    scaled = out * 32768.0
                    first_clip = frames + int(np.flatnonzero((scaled < -32768.0) | (scaled > 32767.0))[0])
                clipped += block_clipped
                peak = max(peak, float(np.max(np.abs(out))) if len(out) else 0.0)
                energy += float(np.dot(out, out))
                frames += len(out)
                writer.write(pcm)
        os.replace(temp, output_wav)
    except BaseException:
        temp.unlink(missing_ok=True)
        raise

    stages = []
    for index, stage in enumerate(pipeline.stages):
        stages.append({"order": index, "configured": scenario["impairments"][index], "resolved": stage.resolved(),
                       "measured": stage.stats(), "events": stage.events()})
    rms = math.sqrt(energy / frames) if frames else 0.0
    sidecar = {
        "schema": SIDECAR_SCHEMA,
        "test_id": scenario["test_id"],
        "description": scenario.get("description", ""),
        "tags": scenario.get("tags", {}),
        "created_utc": created_utc or datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "generator": package_identity(),
        "scenario": scenario,
        "scenario_sha256": scenario_sha256(scenario),
        "seed": scenario["seed"],
        "source": wav_artifact(source_path),
        "source_reference": load_source_sidecar(source_path),
        "reference": {"start_frame": ref_start, "end_frame": ref_end, "rms": reference_rms, "rms_dbfs": amplitude_to_db(reference_rms),
                      "scaled_rms_dbfs": amplitude_to_db(scaled_reference)},
        "source_gain_db": scenario["source_gain_db"],
        "clip_policy": scenario["clip_policy"],
        "impairment_order": [item["type"] for item in scenario["impairments"]],
        "impairments": stages,
        "output": dict(wav_artifact(output_wav), clipped_samples=clipped, first_clipped_frame=first_clip,
                       peak_dbfs=amplitude_to_db(peak), rms_dbfs=amplitude_to_db(rms), input_samples=total,
                       sample_rate_hz=FS, pcm_format="pcm_s16le"),
        "conventions": {
            "order": "source * source_gain -> impairments in list order -> PCM16 round-half-even, saturate",
            "levels": "all impairment levels are relative to the reference-interval RMS of the *scaled* source",
            "reference": "auto = first..last non-zero source sample; otherwise the declared interval",
            "rng": "NumPy PCG64 SeedSequence([seed, family, stage_index]); see each stage's resolved.rng_family",
            "determinism": "same scenario + source + signal_lab package + Python/NumPy -> identical bytes (hash-bound)",
        },
    }
    with open(sidecar_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(sidecar, handle, indent=1, sort_keys=True, allow_nan=False)
        handle.write("\n")
    return sidecar


def main(argv=None):
    import argparse
    parser = argparse.ArgumentParser(description="Render one signal_lab scenario to an impaired WAV plus JSON sidecar.")
    parser.add_argument("--scenario", required=True, type=Path, help="scenario JSON")
    parser.add_argument("--output", required=True, type=Path, help="output .wav (sidecar = same name .json)")
    parser.add_argument("--base-dir", type=Path, default=None, help="directory relative source paths resolve against (default: scenario file's directory)")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args(argv)
    with open(args.scenario, "r", encoding="utf-8") as handle:
        scenario = json.load(handle)
    base = args.base_dir or args.scenario.resolve().parent
    try:
        sidecar = render(scenario, args.output, base_dir=base, overwrite=args.overwrite)
    except (ScenarioError, RenderError, ValueError, OSError) as error:
        print("signal_lab render: %s" % error, file=sys.stderr)
        return 2
    print(json.dumps({"output": sidecar["output"]["path"], "sha256": sidecar["output"]["sha256"],
                      "samples": sidecar["output"]["samples"], "clipped_samples": sidecar["output"]["clipped_samples"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
