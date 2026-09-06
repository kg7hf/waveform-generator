#!/usr/bin/env python3
"""Render perfect deterministic M110 reference WAVs with the project transmitter.

The existing encoder (m110_tx_to_pcm) stays the sole authority for the M110
waveform; this script only sizes the PN-11 payload for a target duration,
invokes the encoder, verifies the WAV, measures it and writes a JSON sidecar
that pins the payload, framing, hashes and producer identity.

    python host/m110_reference.py --tx <m110_tx_to_pcm.exe> --out-dir test-vectors/simulated-interference \
        --modes 300L,300S,600L,600S,1200L,1200S --seconds 120
"""

import argparse
import hashlib
import json
import math
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from signal_lab import FS, VERSION  # noqa: E402
from signal_lab.stream import amplitude_to_db  # noqa: E402
from signal_lab.wav_io import pcm_region_sha256, read_float, require_canonical, sha256_file, wav_info  # noqa: E402

REFERENCE_SCHEMA = "signal-lab.reference/1"
MASTER_PAYLOAD_BYTES = 65536
# core/transmitter.cpp: unpadded_bits = payload*8 + body_eom_bits (32) + body_flush_bits (144); blocks = ceil(unpadded / information_bits).
EOM_BITS = 32
FLUSH_BITS = 144
TRAILING_SILENCE_SECONDS = 1.0
# core/body_waveform.cpp: LONG = 24 preamble segments (4.8 s) + 4.8 s blocks; SHORT = 3 segments (0.6 s) + 0.6 s blocks (verified by render).
FRAMING = {"long": {"preamble_seconds": 4.8, "block_seconds": 4.8}, "short": {"preamble_seconds": 0.6, "block_seconds": 0.6}}
RATES = (300, 600, 1200, 150, 75, 2400)
MODE_RE = re.compile(r"^(\d+)([LS])$")


def pn11_payload(length):
    """ITU-T O.153 2^11-1 sequence, x^11 + x^9 + 1, seed 0x7FF, packed MSB-first, tiled.

    Byte-identical to tools/pathsim-campaign.py pn11_payload (the JITC 2047 test-pattern convention).
    """
    reg = 0x7FF
    out = bytearray()
    byte = 0
    filled = 0
    while len(out) < length:
        bit = reg & 1
        feedback = ((reg >> 10) ^ (reg >> 8)) & 1
        reg = ((reg << 1) | feedback) & 0x7FF
        byte = ((byte << 1) | bit) & 0xFF
        filled += 1
        if filled == 8:
            out.append(byte)
            byte = 0
            filled = 0
    return bytes(out[:length])


def parse_mode(mode):
    match = MODE_RE.match(mode.strip().upper())
    if not match or int(match.group(1)) not in RATES:
        raise ValueError("mode must look like 600L or 300S")
    return int(match.group(1)), "long" if match.group(2) == "L" else "short"


def payload_bytes_for_duration(rate, interleave, target_seconds):
    """Size the payload so the whole WAV (preamble + blocks + 1 s silence) lands near target_seconds."""
    framing = FRAMING[interleave]
    data_seconds = target_seconds - framing["preamble_seconds"] - TRAILING_SILENCE_SECONDS - framing["block_seconds"] / 2
    bits = int(math.floor(data_seconds * rate)) - EOM_BITS - FLUSH_BITS
    return max(1, bits // 8)


def predicted_samples(rate, interleave, payload_bytes):
    framing = FRAMING[interleave]
    block_bits = framing["block_seconds"] * rate
    blocks = int(math.ceil((payload_bytes * 8 + EOM_BITS + FLUSH_BITS) / block_bits))
    preamble = int(round(framing["preamble_seconds"] * FS))
    block = int(round(framing["block_seconds"] * FS))
    return preamble, blocks, preamble + blocks * block + int(TRAILING_SILENCE_SECONDS * FS)


def artifact(path):
    path = Path(path)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def write_payloads(payload_dir, sizes):
    payload_dir.mkdir(parents=True, exist_ok=True)
    master = pn11_payload(MASTER_PAYLOAD_BYTES)
    master_path = payload_dir / "m110_reference_payload_pn11_master.bin"
    if not master_path.exists() or master_path.read_bytes() != master:
        master_path.write_bytes(master)
    paths = {}
    for size in sorted(set(sizes)):
        if size > MASTER_PAYLOAD_BYTES:
            raise ValueError("payload of %d bytes exceeds the master PN-11 stream" % size)
        path = payload_dir / ("m110_reference_payload_pn11_%d.bin" % size)
        data = master[:size]
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
        paths[size] = path
    return master_path, paths


def generate(tx, mode, out_wav, payload_path, payload_bytes, target_seconds, master_path, created_utc=None, overwrite=False):
    rate, interleave = parse_mode(mode)
    out_wav = Path(out_wav)
    sidecar_path = out_wav.with_suffix(".json")
    if not overwrite and (out_wav.exists() or sidecar_path.exists()):
        raise FileExistsError("refusing to overwrite %s" % out_wav)
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    argv = [str(tx), str(rate), interleave, str(out_wav), "--file", str(payload_path)]
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("transmitter failed (%d): %s" % (result.returncode, (result.stdout + result.stderr)[:400]))
    log = result.stdout + result.stderr
    match = re.search(r"wrote (\d+) waveform samples at (\d+) Hz \((\d+) body blocks\)", log)
    if not match:
        raise RuntimeError("could not parse the transmitter report: " + log[:200])
    waveform_samples, rate_hz, body_blocks = int(match.group(1)), int(match.group(2)), int(match.group(3))
    samples, info = read_float(out_wav)
    require_canonical(info)
    nonzero = np.flatnonzero(samples)
    active = samples[: waveform_samples]
    rms = math.sqrt(float(np.dot(active, active)) / len(active))
    preamble, predicted_blocks, predicted_total = predicted_samples(rate, interleave, payload_bytes)
    block = int(round(FRAMING[interleave]["block_seconds"] * FS))
    sidecar = {
        "schema": REFERENCE_SCHEMA,
        "test_id": out_wav.stem,
        "kind": "reference_perfect",
        "created_utc": created_utc or datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "generator": {"name": VERSION, "script": artifact(Path(__file__).resolve())},
        "mode": mode.upper(),
        "rate_bps": rate,
        "interleave": interleave,
        "target_seconds": target_seconds,
        "payload": dict(artifact(payload_path), payload_bytes=payload_bytes, pattern="pn11",
                        pn11={"polynomial": "x^11 + x^9 + 1", "seed": "0x7FF", "packing": "msb_first", "tiled": True,
                              "convention": "tools/pathsim-campaign.py pn11_payload (JITC 2047 test pattern)"},
                        master=artifact(master_path), bit_order="lsb_first_within_octet",
                        payload_sha256=hashlib.sha256(Path(payload_path).read_bytes()).hexdigest()),
        "producer": {"method": "external_m110", "tool_identity": artifact(tx), "argv": argv, "report": log.strip()},
        "wav": dict(artifact(out_wav), samples=info["samples"], duration_seconds=info["samples"] / FS,
                    sample_rate_hz=info["sample_rate_hz"], channels=info["channels"], encoding=info["encoding"],
                    pcm={"data_offset_bytes": info["data_offset_bytes"], "data_bytes": info["data_bytes"], "sha256": pcm_region_sha256(out_wav, info)}),
        "framing": {
            "waveform_samples": waveform_samples, "body_blocks": body_blocks, "trailing_silence_samples": info["samples"] - waveform_samples,
            "preamble": {"start_sample": 0, "end_sample_exclusive": preamble, "seconds": preamble / FS},
            "body": {"start_sample": preamble, "end_sample_exclusive": waveform_samples, "seconds": (waveform_samples - preamble) / FS},
            "block_samples": block, "block_seconds": block / FS,
            "predicted": {"blocks": predicted_blocks, "total_samples": predicted_total},
            "prediction_matches": predicted_total == info["samples"],
            "payload_dwell_seconds": payload_bytes * 8 / rate,
            "first_nonzero_sample": int(nonzero[0]) if len(nonzero) else None, "last_nonzero_sample": int(nonzero[-1]) if len(nonzero) else None,
        },
        "level": {"active_rms_dbfs": amplitude_to_db(rms), "peak_dbfs": amplitude_to_db(float(np.max(np.abs(samples)))), "clipped_samples": int(np.count_nonzero(np.abs(samples) >= 32767 / 32768))},
        "impairments": [],
        "conventions": {"format": "48 kHz mono PCM16, 1 s trailing silence appended by the transmitter",
                        "framing": "measured rule: preamble + ceil((8*payload_bytes + 32 EOM bits)/block_bits) blocks (300S rounds up one extra block)"},
    }
    with open(sidecar_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(sidecar, handle, indent=1, sort_keys=True, allow_nan=False)
        handle.write("\n")
    return sidecar


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tx", required=True, type=Path, help="explicit m110_tx_to_pcm executable")
    parser.add_argument("--out-dir", required=True, type=Path, help="corpus root (mode/reference/ and payload/ are created below it)")
    parser.add_argument("--modes", default="300L,300S,600L,600S,1200L,1200S")
    parser.add_argument("--seconds", type=float, default=120.0, help="target total WAV duration")
    parser.add_argument("--tag", default="", help="file-name suffix, e.g. _5min")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args(argv)
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    sizes = {}
    for mode in modes:
        rate, interleave = parse_mode(mode)
        # Size from the LONG framing so L and S of one rate share the same payload bytes.
        sizes[mode] = payload_bytes_for_duration(rate, "long", args.seconds)
    master_path, payload_paths = write_payloads(args.out_dir / "payload", sizes.values())
    results = []
    for mode in modes:
        out_wav = args.out_dir / mode.upper() / "reference" / ("m110_%s_reference_perfect%s.wav" % (mode.upper(), args.tag))
        try:
            sidecar = generate(args.tx, mode, out_wav, payload_paths[sizes[mode]], sizes[mode], args.seconds, master_path, overwrite=args.overwrite)
        except (RuntimeError, ValueError, OSError) as error:
            print("m110_reference: %s: %s" % (mode, error), file=sys.stderr)
            return 2
        results.append({"mode": mode, "wav": str(out_wav), "payload_bytes": sizes[mode], "duration_seconds": sidecar["wav"]["duration_seconds"],
                        "sha256": sidecar["wav"]["sha256"], "prediction_matches": sidecar["framing"]["prediction_matches"]})
        print(json.dumps(results[-1], sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
