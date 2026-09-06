#!/usr/bin/env python3
"""Phase 1 corpus orchestrator: references -> validate -> render impairments -> score -> summary.

    python host/corpus_phase1.py --spec corpus/phase1-corpus.json \
        --tx <m110_tx_to_pcm.exe> --decoder <m110_app_decode.exe> --jobs 4 --stage all

Stages: refs (perfect references + payloads), validate (references must decode BER 0 with EOM),
render (every mode x case scenario), score (every WAV, each engine), summary (manifest, CSV, counts),
verify (re-render every impaired scenario to a temporary file and compare hashes).
Existing outputs are skipped unless --overwrite is given, so the corpus is regenerated incrementally.
"""

import argparse
import collections
import concurrent.futures
import copy
import json
import os
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import m110_reference  # noqa: E402
import m110_score  # noqa: E402
from signal_lab import SCENARIO_SCHEMA, VERSION  # noqa: E402
from signal_lab.render import RenderError, package_identity, render  # noqa: E402
from signal_lab.scenario import ScenarioError, scenario_sha256, validate  # noqa: E402
from signal_lab.wav_io import sha256_file  # noqa: E402

CORPUS_SCHEMA = "signal-lab.corpus/1"
FAMILY_DIR = {"cw": "cw", "impulse": "impulse", "fade": "fade", "sample-slip": "sample-slip", "noise": "noise", "mixed": "mixed"}


def load_spec(path):
    with open(path, "r", encoding="utf-8") as handle:
        spec = json.load(handle)
    if spec.get("schema") != CORPUS_SCHEMA:
        raise ValueError("corpus spec schema must be " + CORPUS_SCHEMA)
    return spec


def reference_wav(root, mode, tag=""):
    return root / mode / "reference" / ("m110_%s_reference_perfect%s.wav" % (mode, tag))


def resolve_impairment(item, rate, corners):
    """Replace {"corner_offset_db": x} SNR requests with the absolute value for this rate."""
    item = copy.deepcopy(item)
    tags = {}
    if item.get("type") == "awgn" and isinstance(item.get("snr_db"), dict):
        offset = float(item["snr_db"]["corner_offset_db"])
        corner = float(corners[str(rate)])
        item["snr_db"] = corner + offset
        tags = {"corner_snr_db": corner, "corner_offset_db": offset}
    return item, tags


def build_scenario(spec, mode, mode_index, case, case_index, root):
    rate, _interleave = m110_reference.parse_mode(mode)
    seed = int(spec.get("seed_base", 0)) + 100 * mode_index + case_index
    name = case["name"]
    test_id = "m110_%s_%s" % (mode, name)
    if case.get("seeded", False):
        test_id += "_seed%04d" % seed
    defaults = spec.get("defaults", {})
    impairments, tags = [], {"mode": mode, "rate_bps": rate, "family": case["family"], "severity": case.get("severity", ""), "case": name, "case_index": case_index}
    for item in case["impairments"]:
        resolved, extra = resolve_impairment(item, rate, spec["corner_snr_db"])
        impairments.append(resolved)
        tags.update(extra)
    source = reference_wav(root, mode)
    scenario = {
        "schema": SCENARIO_SCHEMA, "test_id": test_id, "seed": seed,
        "description": case.get("description", "%s on %s" % (name, mode)),
        "tags": tags,
        "source": {"path": os.path.relpath(source, root / mode / FAMILY_DIR[case["family"]]).replace("\\", "/")},
        "reference": defaults.get("reference", "auto"),
        "source_gain_db": float(case.get("source_gain_db", defaults.get("source_gain_db", -12.0))),
        "clip_policy": case.get("clip_policy", defaults.get("clip_policy", "saturate")),
        "impairments": impairments,
    }
    output = root / mode / FAMILY_DIR[case["family"]] / (test_id + ".wav")
    return scenario, output


def plan(spec, root, only_modes=None):
    jobs = []
    for mode_index, mode in enumerate(spec["modes"]):
        if only_modes and mode not in only_modes:
            continue
        for case_index, case in enumerate(spec["cases"]):
            if case.get("modes") and mode not in case["modes"]:
                continue
            scenario, output = build_scenario(spec, mode, mode_index, case, case_index, root)
            jobs.append((scenario, output))
    return jobs


def stage_refs(spec, root, tx, overwrite, only_modes=None):
    modes = [m for m in spec["modes"] if not only_modes or m in only_modes]
    sets = [spec["reference"]] + list(spec.get("extra_references", []))
    results = []
    for entry in sets:
        seconds, tag = float(entry["seconds"]), entry.get("tag", "")
        sizes = {mode: m110_reference.payload_bytes_for_duration(m110_reference.parse_mode(mode)[0], "long", seconds) for mode in modes}
        master, payloads = m110_reference.write_payloads(root / "payload", sizes.values())
        for mode in modes:
            out = reference_wav(root, mode, tag)
            if out.exists() and out.with_suffix(".json").exists() and not overwrite:
                results.append({"mode": mode, "tag": tag, "wav": str(out), "status": "exists"})
                continue
            sidecar = m110_reference.generate(tx, mode, out, payloads[sizes[mode]], sizes[mode], seconds, master, overwrite=True)
            results.append({"mode": mode, "tag": tag, "wav": str(out), "status": "generated", "duration_seconds": sidecar["wav"]["duration_seconds"],
                            "payload_bytes": sizes[mode], "sha256": sidecar["wav"]["sha256"]})
            print(json.dumps(results[-1], sort_keys=True))
    return results


def stage_validate(spec, root, decoder, jobs, only_modes=None):
    modes = [m for m in spec["modes"] if not only_modes or m in only_modes]
    wavs = []
    for entry in [spec["reference"]] + list(spec.get("extra_references", [])):
        wavs += [reference_wav(root, mode, entry.get("tag", "")) for mode in modes]
    identity = {"path": str(decoder), "sha256": sha256_file(decoder)}
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        for score in pool.map(lambda w: m110_score.score_one(decoder, w, "siso", (), None, True, identity), wavs):
            flat = score["result"]
            ok = flat["complete_message"] and flat["bit_errors"] == 0 and flat["eom_detected"] and flat["bursts"] == 1
            print(json.dumps({"reference": score["test_id"], "complete": flat["complete_message"], "bit_errors": flat["bit_errors"],
                              "eom": flat["eom_detected"], "mode": flat["mode_detected"], "il": flat["interleaver_detected"], "ok": ok}, sort_keys=True))
            if not ok:
                failures.append(score["test_id"])
    return failures


def render_job(args):
    scenario, output, overwrite = args
    if output.exists() and output.with_suffix(".json").exists() and not overwrite:
        with open(output.with_suffix(".json"), "r", encoding="utf-8") as handle:
            existing = json.load(handle)
        if existing.get("scenario_sha256") == scenario_sha256(validate(scenario)):
            return {"test_id": scenario["test_id"], "status": "exists", "sha256": existing["output"]["sha256"]}
    started = time.perf_counter()
    sidecar = render(scenario, output, base_dir=output.parent, overwrite=True)
    return {"test_id": scenario["test_id"], "status": "rendered", "sha256": sidecar["output"]["sha256"], "samples": sidecar["output"]["samples"],
            "clipped_samples": sidecar["output"]["clipped_samples"], "seconds": round(time.perf_counter() - started, 2)}


def stage_render(jobs_list, workers, overwrite):
    results = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, workers)) as pool:
        for result in pool.map(render_job, [(scenario, output, overwrite) for scenario, output in jobs_list]):
            results.append(result)
            print(json.dumps(result, sort_keys=True))
    return results


def all_wavs(spec, root, jobs_list, only_modes=None):
    modes = [m for m in spec["modes"] if not only_modes or m in only_modes]
    wavs = []
    for entry in [spec["reference"]] + list(spec.get("extra_references", [])):
        wavs += [reference_wav(root, mode, entry.get("tag", "")) for mode in modes]
    wavs += [output for _scenario, output in jobs_list]
    return [w for w in wavs if w.exists()]


def stage_score(wavs, decoder, engines, jobs, overwrite):
    identity = {"path": str(decoder), "sha256": sha256_file(decoder)}
    scores = {engine: [] for engine in engines}
    work = []
    for engine in engines:
        for wav in wavs:
            score_path = wav.with_suffix(".%s.score.json" % engine)
            if score_path.exists() and not overwrite:
                with open(score_path, "r", encoding="utf-8") as handle:
                    existing = json.load(handle)
                if existing.get("wav_sha256") == json.load(open(wav.with_suffix(".json"), "r", encoding="utf-8")).get("output", {}).get("sha256", existing.get("wav_sha256")) \
                        and existing.get("decoder", {}).get("identity", {}).get("sha256") == identity["sha256"]:
                    scores[engine].append(existing)
                    continue
            work.append((engine, wav))
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {pool.submit(m110_score.score_one, decoder, wav, engine, (), None, True, identity): (engine, wav) for engine, wav in work}
        for future in concurrent.futures.as_completed(futures):
            engine, wav = futures[future]
            try:
                score = future.result()
            except Exception as error:  # noqa: BLE001
                print("score failed: %s (%s): %s" % (wav, engine, error), file=sys.stderr)
                continue
            scores[engine].append(score)
            flat = score["result"]
            print(json.dumps({"test_id": score["test_id"], "engine": engine, "complete": flat["complete_message"], "bit_errors": flat["bit_errors"],
                              "ber": flat["ber"], "eom": flat["eom_detected"], "mode_ok": m110_score.row_of(score)["mode_ok"], "bursts": flat["bursts"],
                              "lol": flat["loss_of_lock"], "clipped": flat["clipped_samples"], "time_s": score["decode_time_s"]}, sort_keys=True))
    for engine in engines:
        scores[engine].sort(key=lambda s: s["test_id"])
    return scores


def stage_summary(spec, root, jobs_list, scores, engines):
    manifests = root / "manifests"
    manifests.mkdir(parents=True, exist_ok=True)
    entries = []
    for wav in all_wavs(spec, root, jobs_list):
        with open(wav.with_suffix(".json"), "r", encoding="utf-8") as handle:
            side = json.load(handle)
        is_reference = side.get("kind") == "reference_perfect"
        entries.append({
            "test_id": side["test_id"], "wav": str(wav.relative_to(root)).replace("\\", "/"),
            "sha256": side["wav"]["sha256"] if is_reference else side["output"]["sha256"],
            "samples": side["wav"]["samples"] if is_reference else side["output"]["samples"],
            "duration_seconds": side["wav"]["duration_seconds"] if is_reference else side["output"]["duration_seconds"],
            "mode": side.get("mode") or side["tags"]["mode"], "family": "reference" if is_reference else side["tags"]["family"],
            "severity": "" if is_reference else side["tags"]["severity"], "seed": None if is_reference else side["seed"],
            "scenario_sha256": None if is_reference else side["scenario_sha256"],
            "impairment_order": [] if is_reference else side["impairment_order"],
            "clipped_samples": side["level"]["clipped_samples"] if is_reference else side["output"]["clipped_samples"],
            "payload_bytes": side["payload"]["payload_bytes"] if is_reference else (side.get("source_reference") or {}).get("payload", {}).get("payload_bytes"),
        })
    counts = {"by_mode": collections.Counter(e["mode"] for e in entries), "by_family": collections.Counter(e["family"] for e in entries),
              "by_severity": collections.Counter(e["severity"] or "reference" for e in entries),
              "by_mode_family": collections.Counter("%s/%s" % (e["mode"], e["family"]) for e in entries)}
    manifest = {"schema": CORPUS_SCHEMA + ".manifest", "corpus_id": spec["corpus_id"], "generator": package_identity(),
                "spec_sha256": None, "count": len(entries), "counts": {k: dict(sorted(v.items())) for k, v in counts.items()}, "files": entries}
    with open(manifests / "phase1-corpus-manifest.json", "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, indent=1, sort_keys=True, allow_nan=False)
        handle.write("\n")
    tables = {}
    for engine in engines:
        rows = m110_score.write_summary(scores[engine], manifests / ("phase1-scores-%s.csv" % engine), manifests / ("phase1-scores-%s.json" % engine))
        matrix = collections.defaultdict(lambda: {"files": 0, "complete": 0, "acquired": 0, "eom": 0, "bit_errors": 0, "compared_bits": 0, "mode_ok": 0})
        for row in rows:
            cell = matrix["%s/%s" % (row["mode"], row["family"])]
            cell["files"] += 1
            cell["complete"] += 1 if row["complete_message"] else 0
            cell["acquired"] += 1 if row["acquired"] else 0
            cell["eom"] += 1 if row["eom_detected"] else 0
            cell["mode_ok"] += 1 if row["mode_ok"] else 0
            cell["bit_errors"] += row["bit_errors"] or 0
            cell["compared_bits"] += row["compared_bits"] or 0
        tables[engine] = {"rows": len(rows), "complete": sum(1 for r in rows if r["complete_message"]),
                          "incomplete": sorted(r["test_id"] for r in rows if not r["complete_message"]),
                          "matrix": dict(sorted(matrix.items()))}
    summary = {"schema": CORPUS_SCHEMA + ".summary", "corpus_id": spec["corpus_id"], "files": len(entries), "counts": manifest["counts"], "engines": tables}
    with open(manifests / "phase1-summary.json", "w", encoding="utf-8", newline="\n") as handle:
        json.dump(summary, handle, indent=1, sort_keys=True, allow_nan=False)
        handle.write("\n")
    print("corpus files: %d" % len(entries))
    for engine in engines:
        print("engine %s: %d/%d complete messages; incomplete: %s" % (engine, tables[engine]["complete"], tables[engine]["rows"], ", ".join(tables[engine]["incomplete"]) or "none"))
    return summary


def stage_verify(jobs_list, workers):
    """Regenerate every impaired scenario into a temporary file and compare the PCM hash with the sidecar."""
    mismatches = []
    checked = 0
    for scenario, output in jobs_list:
        sidecar_path = output.with_suffix(".json")
        if not sidecar_path.exists():
            continue
        with open(sidecar_path, "r", encoding="utf-8") as handle:
            recorded = json.load(handle)
        with tempfile.TemporaryDirectory(dir=output.parent) as temp:
            fresh = render(scenario, Path(temp) / output.name, base_dir=output.parent, overwrite=True, created_utc=recorded["created_utc"])
        checked += 1
        if fresh["output"]["pcm"]["sha256"] != recorded["output"]["pcm"]["sha256"]:
            mismatches.append(scenario["test_id"])
            print("MISMATCH " + scenario["test_id"])
    print("verify: %d scenarios re-rendered, %d mismatches" % (checked, len(mismatches)))
    return mismatches


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--spec", type=Path, default=Path(__file__).resolve().parent.parent / "corpus" / "phase1-corpus.json")
    parser.add_argument("--root", type=Path, default=None, help="corpus root (default: spec.root relative to the project)")
    parser.add_argument("--tx", type=Path, default=os.environ.get("M110_TX_TO_PCM"))
    parser.add_argument("--decoder", type=Path, default=os.environ.get("M110_APP_DECODE"))
    parser.add_argument("--engines", default="siso,adaptive")
    parser.add_argument("--stage", default="all", choices=["all", "refs", "validate", "render", "score", "summary", "verify", "plan"])
    parser.add_argument("--only-modes", default="", help="comma-separated subset of modes")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args(argv)
    spec = load_spec(args.spec)
    project = Path(__file__).resolve().parent.parent
    root = (args.root or (project / spec["root"])).resolve()
    only = [m for m in args.only_modes.split(",") if m] or None
    engines = [e for e in args.engines.split(",") if e]
    jobs_list = plan(spec, root, only)
    if args.stage == "plan":
        for scenario, output in jobs_list:
            print(json.dumps({"test_id": scenario["test_id"], "output": str(output.relative_to(root)), "seed": scenario["seed"], "gain_db": scenario["source_gain_db"],
                              "impairments": [i["type"] for i in scenario["impairments"]]}, sort_keys=True))
        print("planned impaired files: %d" % len(jobs_list))
        return 0
    stages = ["refs", "validate", "render", "score", "summary"] if args.stage == "all" else [args.stage]
    if any(s in stages for s in ("refs",)) and not args.tx:
        parser.error("--tx (or M110_TX_TO_PCM) is required for the refs stage")
    if any(s in stages for s in ("validate", "score")) and not args.decoder:
        parser.error("--decoder (or M110_APP_DECODE) is required for validate/score")
    scores = None
    try:
        for stage in stages:
            print("== stage %s" % stage)
            if stage == "refs":
                stage_refs(spec, root, args.tx, args.overwrite, only)
            elif stage == "validate":
                failures = stage_validate(spec, root, args.decoder, args.jobs, only)
                if failures:
                    print("reference validation FAILED: %s" % ", ".join(failures), file=sys.stderr)
                    return 3
            elif stage == "render":
                stage_render(jobs_list, args.jobs, args.overwrite)
            elif stage == "score":
                scores = stage_score(all_wavs(spec, root, jobs_list, only), args.decoder, engines, args.jobs, args.overwrite)
            elif stage == "summary":
                if scores is None:
                    scores = {engine: [] for engine in engines}
                    for wav in all_wavs(spec, root, jobs_list, only):
                        for engine in engines:
                            path = wav.with_suffix(".%s.score.json" % engine)
                            if path.exists():
                                with open(path, "r", encoding="utf-8") as handle:
                                    scores[engine].append(json.load(handle))
                    for engine in engines:
                        scores[engine].sort(key=lambda s: s["test_id"])
                stage_summary(spec, root, jobs_list, scores, engines)
            elif stage == "verify":
                if stage_verify(jobs_list, args.jobs):
                    return 4
    except (ScenarioError, RenderError, ValueError, OSError, RuntimeError) as error:
        print("corpus_phase1: %s" % error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
