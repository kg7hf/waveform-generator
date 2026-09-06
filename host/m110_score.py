#!/usr/bin/env python3
"""Score WAVs through the project decoder (m110_app_decode) and record the results.

For each WAV the expected payload is taken from its sidecar (a reference
sidecar's "payload" or an impaired sidecar's "source_reference.payload") unless
--expect-file is given.  The decoder runs with the production receiver caps
(maximum_payload_octets 1200000, loss-of-lock release disabled for the turbo
engine) so a multi-minute single message is never truncated.

    python host/m110_score.py --decoder <m110_app_decode.exe> --engine siso --jobs 4 <wav>...
"""

import argparse
import concurrent.futures
import csv
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from signal_lab.wav_io import sha256_file  # noqa: E402

SCORE_SCHEMA = "signal-lab.score/1"
DECODER_CONTRACT = "m110_app_decode_text_v1"
PRODUCTION_ARGS = ["--maximum-payload-octets", "1200000"]
KEY_VALUE = re.compile(r"(\w+)=(\S+)")
COLUMNS = ["test_id", "mode", "family", "severity", "engine", "acquired", "mode_detected", "interleaver_detected", "mode_ok",
           "bursts", "eom_detected", "complete_message", "bit_errors", "compared_bits", "ber", "payload_bytes", "expected_bytes",
           "missing_bytes", "undelivered_bits", "stream_release", "loss_of_lock", "discontinuities", "frontend_clipped", "clipped_samples", "frequency_offset_hz",
           "correlation", "decode_time_s", "exit_code", "wav"]


def key_values(line):
    return {key: value for key, value in KEY_VALUE.findall(line)}


def to_number(text):
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def parse_decoder_output(text):
    """Turn the decoder's text contract into a flat record (first burst + stream/summary lines)."""
    record = {"bursts": [], "capture": {}, "stream_summary": {}, "summary": {}}
    burst = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("capture="):
            record["capture"] = {k: to_number(v) for k, v in key_values(line).items()}
        elif line.startswith("burst="):
            burst = {"header": {k: to_number(v) for k, v in key_values(line).items()}}
            mode = burst["header"].get("mode")
            if isinstance(mode, str) and "/" in mode:
                rate, interleave = mode.split("/", 1)
                burst["mode_detected"], burst["interleaver_detected"] = to_number(rate), interleave
            record["bursts"].append(burst)
        elif line.startswith("stream_release=") and burst is not None:
            burst["stream"] = {k: to_number(v) for k, v in key_values(line).items()}
        elif line.startswith("training_frequency_hz=") and burst is not None:
            burst["training_frequency_hz"] = to_number(line.split("=", 1)[1])
        elif line.startswith("payload_hex=") and burst is not None:
            burst["payload_hex"] = line.split("=", 1)[1]
        elif line.startswith("eom=") and burst is not None:
            burst["eom"] = {k: to_number(v) for k, v in key_values(line).items()}
        elif line.startswith("exact_payload=") and burst is not None:
            burst["payload"] = {k: to_number(v) for k, v in key_values(line).items()}
        elif line.startswith("stream_summary"):
            record["stream_summary"] = {k: to_number(v) for k, v in key_values(line).items()}
        elif line.startswith("summary "):
            record["summary"] = {k: to_number(v) for k, v in key_values(line).items()}
    return record


def alignment_diagnostics(payload_hex, expected, search_bytes=64):
    """Localise damage: longest correct prefix, first mismatch, and whether the tail is merely shifted.

    Fixed-offset bit counting cannot tell a sample slip (alignment loss) from noise errors; this
    reports where the first mismatch is and the byte shift (within +/-search_bytes) that best
    re-aligns the remainder, with the residual mismatches under that shift.
    """
    if not payload_hex or expected is None:
        return None
    try:
        received = bytes.fromhex(payload_hex)
    except ValueError:
        return None
    compared = min(len(received), len(expected))
    first = next((i for i in range(compared) if received[i] != expected[i]), None)
    result = {"received_bytes": len(received), "correct_prefix_bytes": compared if first is None else first,
              "first_mismatch_byte": first, "mismatched_bytes": sum(1 for i in range(compared) if received[i] != expected[i])}
    if first is not None and compared - first > 32:
        tail_r = received[first:]
        best = None
        for shift in range(-search_bytes, search_bytes + 1):
            start = first + shift
            if start < 0:
                continue
            tail_e = expected[start:start + len(tail_r)]
            n = min(len(tail_e), len(tail_r))
            if n < 32:
                continue
            mism = sum(1 for i in range(n) if tail_r[i] != tail_e[i])
            if best is None or mism < best[1]:
                best = (shift, mism, n)
        if best:
            result.update({"tail_best_shift_bytes": best[0], "tail_mismatched_bytes_at_best_shift": best[1], "tail_compared_bytes": best[2],
                           "tail_realigned": best[0] != 0 and best[1] < 0.05 * best[2]})
    return result


def flatten(record, expected_bytes, expected_payload=None):
    """Prompt section 19 fields, taken from the burst with the most compared bits (usually the only one)."""
    best = None
    for burst in record["bursts"]:
        compared = burst.get("payload", {}).get("compared_bits", -1)
        if best is None or compared > best.get("payload", {}).get("compared_bits", -1):
            best = burst
    stream = record["stream_summary"]
    summary = record["summary"]
    payload = (best or {}).get("payload", {})
    eom = (best or {}).get("eom", {})
    bit_errors = payload.get("bit_errors")
    compared = payload.get("compared_bits")
    missing = payload.get("missing_bytes")
    # Bits never delivered count against the message the way a BERT would see them.
    delivered_ber = (bit_errors / compared) if isinstance(bit_errors, int) and isinstance(compared, int) and compared else None
    flat = {
        "acquired": len(record["bursts"]) > 0,
        "bursts": len(record["bursts"]),
        "mode_detected": (best or {}).get("mode_detected"),
        "interleaver_detected": (best or {}).get("interleaver_detected"),
        "eom_detected": eom.get("eom") == "found",
        "eom_bit_errors": eom.get("eom_bit_errors"),
        "complete_message": payload.get("exact_payload") == "yes" and missing == 0,
        "bit_errors": bit_errors,
        "compared_bits": compared,
        "ber": delivered_ber,
        "payload_bytes": payload.get("received_bytes"),
        "expected_bytes": payload.get("expected_bytes", expected_bytes),
        "missing_bytes": missing if best else expected_bytes,
        "undelivered_bits": 8 * (missing if isinstance(missing, int) else expected_bytes),
        "stream_release": (best or {}).get("stream", {}).get("stream_release"),
        "stream_bit_errors": (best or {}).get("stream", {}).get("stream_bit_errors"),
        "stream_exact": (best or {}).get("stream", {}).get("stream_exact"),
        "loss_of_lock": stream.get("loss_of_lock"),
        "missing_eom": stream.get("missing_eom"),
        "discontinuities": stream.get("discontinuities"),
        "frontend_clipped": stream.get("frontend_clipped"),
        "training_failures": stream.get("training_failures"),
        "clipped_samples": summary.get("clipped_samples", record["capture"].get("clipped_samples")),
        "audio_quality": summary.get("audio_quality"),
        "decoded_bursts": summary.get("decoded_bursts"),
        "exact_payloads": summary.get("exact_payloads"),
        "frequency_offset_hz": (best or {}).get("header", {}).get("frequency_offset_hz"),
        "training_frequency_hz": (best or {}).get("training_frequency_hz"),
        "correlation": (best or {}).get("header", {}).get("correlation"),
        "burst_start_seconds": (best or {}).get("header", {}).get("start"),
        "alignment": alignment_diagnostics((best or {}).get("payload_hex"), expected_payload) if best else None,
    }
    return flat


def expected_payload_from_sidecar(wav):
    sidecar = Path(wav).with_suffix(".json")
    if not sidecar.exists():
        return None, None
    with open(sidecar, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    payload = data.get("payload") or (data.get("source_reference") or {}).get("payload")
    return (payload or {}).get("path"), data


def run_decoder(decoder, wav, expect_file, engine="siso", extra_args=(), timeout=3600):
    argv = [str(decoder), str(wav), "--engine", engine, "--expect-file", str(expect_file)] + PRODUCTION_ARGS + list(extra_args)
    started = time.perf_counter()
    result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    elapsed = time.perf_counter() - started
    text = result.stdout + result.stderr
    return argv, result.returncode, elapsed, text


def score_one(decoder, wav, engine, extra_args, expect_file=None, keep_log=True, decoder_identity=None):
    wav = Path(wav)
    sidecar_payload, sidecar = expected_payload_from_sidecar(wav)
    expect = Path(expect_file) if expect_file else (Path(sidecar_payload) if sidecar_payload else None)
    if expect is None or not expect.exists():
        raise FileNotFoundError("no expected payload for %s (pass --expect-file)" % wav)
    expected_payload = expect.read_bytes()
    expected_bytes = len(expected_payload)
    argv, code, elapsed, text = run_decoder(decoder, wav, expect, engine, extra_args)
    record = parse_decoder_output(text)
    flat = flatten(record, expected_bytes, expected_payload)
    for burst in record["bursts"]:
        burst.pop("payload_hex", None)
    tags = (sidecar or {}).get("tags", {})
    score = {
        "schema": SCORE_SCHEMA,
        "test_id": (sidecar or {}).get("test_id", wav.stem),
        "wav": str(wav),
        "wav_sha256": (sidecar or {}).get("output", {}).get("sha256") or (sidecar or {}).get("wav", {}).get("sha256") or sha256_file(wav),
        "mode": (sidecar or {}).get("mode") or ((sidecar or {}).get("source_reference") or {}).get("mode") or tags.get("mode"),
        "family": tags.get("family", "reference" if (sidecar or {}).get("kind") == "reference_perfect" else None),
        "severity": tags.get("severity"),
        "engine": engine,
        "decoder": {"contract": DECODER_CONTRACT, "identity": decoder_identity or {"path": str(decoder), "sha256": sha256_file(decoder)}, "argv": argv},
        "expected_payload": {"path": str(expect), "bytes": expected_bytes},
        "exit_code": code,
        "decode_time_s": round(elapsed, 3),
        "result": flat,
        "bursts": record["bursts"],
        "stream_summary": record["stream_summary"],
        "summary": record["summary"],
        "capture": record["capture"],
    }
    if keep_log:
        log_path = wav.with_suffix(".%s.decode.log" % engine)
        log_path.write_text(text, encoding="utf-8")
        score["decoder_log"] = str(log_path)
    score_path = wav.with_suffix(".%s.score.json" % engine)
    with open(score_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(score, handle, indent=1, sort_keys=True, allow_nan=False)
        handle.write("\n")
    return score


def row_of(score):
    flat = score["result"]
    row = {"test_id": score["test_id"], "mode": score["mode"], "family": score["family"], "severity": score["severity"], "engine": score["engine"],
           "decode_time_s": score["decode_time_s"], "exit_code": score["exit_code"], "wav": score["wav"]}
    for column in COLUMNS:
        if column not in row:
            row[column] = flat.get(column)
    expected = row["mode"]
    detected = ("%s%s" % (flat.get("mode_detected"), (flat.get("interleaver_detected") or "?")[0].upper())) if flat.get("mode_detected") else None
    row["mode_ok"] = (detected == expected) if expected and detected else False
    return row


def write_summary(scores, csv_path, json_path):
    rows = [row_of(score) for score in scores]
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with open(csv_path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: ("" if row.get(key) is None else row.get(key)) for key in COLUMNS})
    with open(json_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump({"schema": SCORE_SCHEMA + ".summary", "count": len(rows), "rows": rows}, handle, indent=1, sort_keys=True, allow_nan=False)
        handle.write("\n")
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--decoder", required=True, type=Path)
    parser.add_argument("--engine", default="siso", choices=["siso", "adaptive"])
    parser.add_argument("--expect-file", type=Path, default=None, help="override the sidecar payload (single WAV)")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--extra", default="", help="extra decoder arguments, space separated")
    parser.add_argument("--summary", type=Path, default=None, help="write <summary>.csv and <summary>.json")
    parser.add_argument("wavs", nargs="+", type=Path)
    args = parser.parse_args(argv)
    extra = args.extra.split()
    identity = {"path": str(args.decoder), "sha256": sha256_file(args.decoder)}
    scores = []
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(score_one, args.decoder, wav, args.engine, extra, args.expect_file, True, identity): wav for wav in args.wavs}
        for future in concurrent.futures.as_completed(futures):
            wav = futures[future]
            try:
                score = future.result()
            except Exception as error:  # noqa: BLE001 - report and continue scoring the rest
                failures += 1
                print("m110_score: %s: %s" % (wav, error), file=sys.stderr)
                continue
            scores.append(score)
            flat = score["result"]
            print(json.dumps({"test_id": score["test_id"], "engine": args.engine, "complete": flat["complete_message"], "bit_errors": flat["bit_errors"],
                              "ber": flat["ber"], "eom": flat["eom_detected"], "mode": flat["mode_detected"], "il": flat["interleaver_detected"],
                              "bursts": flat["bursts"], "lol": flat["loss_of_lock"], "time_s": score["decode_time_s"]}, sort_keys=True))
    scores.sort(key=lambda s: s["test_id"])
    if args.summary:
        write_summary(scores, args.summary.with_suffix(".csv"), args.summary.with_suffix(".json"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
