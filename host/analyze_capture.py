#!/usr/bin/env python3
"""Stream and qualify a waveform-generator analog capture.

This is an engineering artifact check.  It does not establish modem
conformance.  The implementation deliberately uses only the Python standard
library so it remains usable from an extracted waveform-generator checkout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable


CHUNK_BYTES = 1024 * 1024
DEFAULT_RATE = 48_000
DEFAULT_RUN_THRESHOLD = 4_800  # 100 ms at 48 kHz
DEFAULT_DROPOUT_LEVEL = 1.0e-6


class WavError(ValueError):
    """The file is not a complete supported RIFF/WAV."""


@dataclass(frozen=True)
class WavInfo:
    path: Path
    file_size_bytes: int
    riff_size: int
    fmt_tag: int
    channels: int
    sample_rate_hz: int
    byte_rate: int
    block_align: int
    bits_per_sample: int
    data_offset: int
    data_bytes: int

    @property
    def samples(self) -> int:
        if self.data_bytes % self.block_align:
            raise WavError("data chunk is not an integral number of frames")
        return self.data_bytes // self.block_align


def _read_exact(stream: BinaryIO, count: int) -> bytes:
    value = stream.read(count)
    if len(value) != count:
        raise WavError("truncated RIFF/WAV structure")
    return value


def inspect_wav(path: Path) -> WavInfo:
    size = path.stat().st_size
    if size < 12:
        raise WavError("file is shorter than a RIFF header")
    with path.open("rb") as stream:
        if _read_exact(stream, 4) != b"RIFF":
            raise WavError("file is not RIFF")
        riff_size = struct.unpack("<I", _read_exact(stream, 4))[0]
        if _read_exact(stream, 4) != b"WAVE":
            raise WavError("RIFF form is not WAVE")
        riff_end = 8 + riff_size
        if riff_end != size:
            raise WavError(f"RIFF length {riff_end} does not equal file size {size}")

        fmt: tuple[int, int, int, int, int] | None = None
        data_offset: int | None = None
        data_bytes: int | None = None
        while stream.tell() < riff_end:
            if riff_end - stream.tell() < 8:
                raise WavError("truncated RIFF chunk header")
            chunk_id = _read_exact(stream, 4)
            chunk_size = struct.unpack("<I", _read_exact(stream, 4))[0]
            payload = stream.tell()
            chunk_end = payload + chunk_size
            if chunk_end > riff_end:
                raise WavError(f"chunk {chunk_id!r} extends past RIFF length")
            if chunk_id == b"fmt ":
                if fmt is not None:
                    raise WavError("multiple fmt chunks")
                if chunk_size < 16:
                    raise WavError("fmt chunk is shorter than WAVEFORMATEX")
                raw = _read_exact(stream, 16)
                fmt = struct.unpack("<HHIIHH", raw)
            elif chunk_id == b"data":
                if data_offset is not None:
                    raise WavError("multiple data chunks are not supported")
                data_offset = payload
                data_bytes = chunk_size
            stream.seek(chunk_end + (chunk_size & 1))

        if fmt is None or data_offset is None or data_bytes is None:
            raise WavError("WAV needs one fmt chunk and one data chunk")
        if fmt[3] != fmt[2] * fmt[4]:
            raise WavError("fmt byte rate is inconsistent")
        if fmt[5] % 8 or fmt[4] != fmt[1] * (fmt[5] // 8):
            raise WavError("fmt block alignment is inconsistent")
        info = WavInfo(path, size, riff_size, *fmt, data_offset, data_bytes)
        _ = info.samples
        return info


def _require_capture_format(info: WavInfo) -> None:
    if (info.fmt_tag, info.channels, info.sample_rate_hz,
            info.bits_per_sample, info.block_align) != (3, 1, DEFAULT_RATE, 32, 4):
        raise WavError("capture must be mono IEEE_FLOAT32 48 kHz")


def _require_source_format(info: WavInfo) -> None:
    common = (info.fmt_tag, info.channels, info.sample_rate_hz)
    sample_format = (info.bits_per_sample, info.block_align)
    if common != (1, 1, DEFAULT_RATE) or sample_format not in ((16, 2), (24, 3)):
        raise WavError("source must be mono PCM16 or packed PCM24 at 48 kHz")


def _iter_capture_samples(info: WavInfo, chunk_bytes: int = CHUNK_BYTES) -> Iterable[float]:
    with info.path.open("rb") as stream:
        stream.seek(info.data_offset)
        remaining = info.data_bytes
        while remaining:
            count = min(remaining, chunk_bytes)
            count -= count % 4
            raw = _read_exact(stream, count)
            yield from (value[0] for value in struct.iter_unpack("<f", raw))
            remaining -= count


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while raw := stream.read(CHUNK_BYTES):
            digest.update(raw)
    return digest.hexdigest()


class _Run:
    def __init__(self, threshold: int, predicate) -> None:
        self.threshold = threshold
        self.predicate = predicate
        self.current = 0
        self.runs = 0
        self.samples = 0
        self.maximum = 0

    def observe(self, value: float) -> None:
        if self.predicate(value):
            self.current += 1
            return
        self.finish()

    def finish(self) -> None:
        if self.current >= self.threshold:
            self.runs += 1
            self.samples += self.current
            self.maximum = max(self.maximum, self.current)
        self.current = 0


def analyze_capture(path: Path, zero_threshold: int = DEFAULT_RUN_THRESHOLD,
                    constant_threshold: int = DEFAULT_RUN_THRESHOLD,
                    dropout_threshold: float = DEFAULT_DROPOUT_LEVEL) -> dict:
    info = inspect_wav(path)
    _require_capture_format(info)
    zero = _Run(zero_threshold, lambda value: value == 0.0)
    dropout = _Run(dropout_threshold and zero_threshold,
                   lambda value: math.isfinite(value) and abs(value) <= dropout_threshold)
    # Constant runs are tracked against the immediately preceding sample.  The
    # first sample cannot start a run until it has a predecessor.
    constant = _Run(constant_threshold, lambda _value: False)
    finite = nonfinite = clipped = 0
    peak = 0.0
    sum_squares = 0.0
    sum_values = 0.0
    pcm_digest = hashlib.sha256()
    previous: float | None = None
    with info.path.open("rb") as stream:
        stream.seek(info.data_offset)
        remaining = info.data_bytes
        while remaining:
            count = min(remaining, CHUNK_BYTES)
            count -= count % 4
            raw = _read_exact(stream, count)
            pcm_digest.update(raw)
            for (value,) in struct.iter_unpack("<f", raw):
                if math.isfinite(value):
                    finite += 1
                    magnitude = abs(value)
                    peak = max(peak, magnitude)
                    sum_squares += float(value) * float(value)
                    sum_values += value
                    if magnitude >= 0.999:
                        clipped += 1
                else:
                    nonfinite += 1
                zero.observe(value)
                dropout.observe(value)
                if previous is None:
                    constant.current = 1
                elif value == previous:
                    constant.current += 1
                else:
                    constant.finish()
                    constant.current = 1
                previous = value
            remaining -= count
    zero.finish()
    dropout.finish()
    constant.finish()
    samples = info.samples
    if samples != finite + nonfinite:
        raise WavError("sample count does not match streamed sample count")
    return {
        "schema_version": "wfg-capture-analysis/1",
        "engineering_test_only": True,
        "formal_conformance": False,
        "file": {
            "path": str(path.resolve()),
            "size_bytes": info.file_size_bytes,
            "sha256": _sha256_file(path),
            "riff_size": info.riff_size,
            "data_offset": info.data_offset,
            "data_bytes": info.data_bytes,
            "pcm_sha256": pcm_digest.hexdigest(),
            "sample_count": samples,
            "duration_seconds": samples / DEFAULT_RATE,
        },
        "format": {
            "encoding": "IEEE_FLOAT32",
            "channels": 1,
            "sample_rate_hz": DEFAULT_RATE,
            "bits_per_sample": 32,
            "block_align": 4,
        },
        "metrics": {
            "finite_samples": finite,
            "nonfinite_samples": nonfinite,
            "clipped_samples": clipped,
            "peak_linear": peak,
            "peak_dbfs": 20.0 * math.log10(peak) if peak else None,
            "rms_linear": math.sqrt(sum_squares / finite) if finite else None,
            "rms_dbfs": (20.0 * math.log10(math.sqrt(sum_squares / finite))
                         if finite and sum_squares else None),
            "dc_mean": sum_values / finite if finite else None,
        },
        "run_thresholds": {
            "exact_zero_samples": 0.0,
            "dropout_level_linear": dropout_threshold,
            "zero_run_samples": zero_threshold,
            "constant_run_samples": constant_threshold,
        },
        "runs": {
            "exact_zero_runs": zero.runs,
            "exact_zero_samples_in_long_runs": zero.samples,
            "maximum_exact_zero_run_samples": zero.maximum,
            "dropout_runs": dropout.runs,
            "dropout_samples_in_long_runs": dropout.samples,
            "maximum_dropout_run_samples": dropout.maximum,
            "constant_runs": constant.runs,
            "constant_samples_in_long_runs": constant.samples,
            "maximum_constant_run_samples": constant.maximum,
        },
    }


def _read_window(info: WavInfo, start: int, count: int) -> list[float]:
    start = max(0, min(start, info.samples))
    count = max(0, min(count, info.samples - start))
    if count == 0:
        return []
    width = info.block_align
    with info.path.open("rb") as stream:
        stream.seek(info.data_offset + start * width)
        raw = _read_exact(stream, count * width)
    if (info.fmt_tag, info.bits_per_sample, width) == (1, 16, 2):
        return [sample / 32768.0 for (sample,) in struct.iter_unpack("<h", raw)]
    if (info.fmt_tag, info.bits_per_sample, width) == (1, 24, 3):
        return [int.from_bytes(raw[index:index + 3], "little", signed=True) / 8388608.0
                for index in range(0, len(raw), 3)]
    if (info.fmt_tag, info.bits_per_sample, width) == (3, 32, 4):
        return [sample for (sample,) in struct.iter_unpack("<f", raw)]
    raise WavError("unsupported correlation window format")


def _decimate(values: list[float], step: int) -> list[float]:
    return values[::step]


def _correlation(left: list[float], right: list[float]) -> float | None:
    count = min(len(left), len(right))
    if count < 16:
        return None
    left = left[:count]
    right = right[:count]
    left_mean = sum(left) / count
    right_mean = sum(right) / count
    a = [value - left_mean for value in left]
    b = [value - right_mean for value in right]
    norm = math.sqrt(sum(value * value for value in a) * sum(value * value for value in b))
    if norm == 0.0:
        return None
    return sum(x * y for x, y in zip(a, b)) / norm


def estimate_correlation(capture: WavInfo, source: WavInfo,
                         window_seconds: float = 1.0,
                         search_seconds: float = 15.0) -> dict:
    _require_capture_format(capture)
    _require_source_format(source)
    window = max(16, int(window_seconds * DEFAULT_RATE))
    step = max(1, min(100, window // 16))  # at least 16 correlation points.
    source_start = _decimate(_read_window(source, 0, window), step)
    if not source_start:
        return {"available": False, "reason": "source has no correlation window"}

    def search(anchor: list[float], center: int, radius: int) -> tuple[int, float] | None:
        first = max(0, center - radius)
        last = min(capture.samples - window, center + radius)
        best: tuple[int, float] | None = None
        positions = list(range(first, last + 1, step))
        if last >= first and (not positions or positions[-1] != last):
            positions.append(last)
        for position in positions:
            candidate = _decimate(_read_window(capture, position, window), step)
            score = _correlation(anchor, candidate)
            if score is not None and (best is None or score > best[1]):
                best = (position, score)
        if best is not None:
            fine_first = max(first, best[0] - step)
            fine_last = min(last, best[0] + step)
            for position in range(fine_first, fine_last + 1):
                candidate = _decimate(_read_window(capture, position, window), step)
                score = _correlation(anchor, candidate)
                if score is not None and score > best[1]:
                    best = (position, score)
        return best

    start = search(source_start, 0, int(search_seconds * DEFAULT_RATE))
    if start is None:
        return {"available": False, "reason": "no nonconstant start correlation window"}

    source_end_position = max(0, source.samples - window)
    source_end = _decimate(_read_window(source, source_end_position, window), step)
    predicted = start[0] + source_end_position
    end = search(source_end, predicted, int(search_seconds * DEFAULT_RATE))
    result = {
        "available": end is not None,
        "method": "normalized zero-mean correlation of 1-second windows, decimated every 100 samples; start searched from capture sample 0 and end searched around the start-plus-source-length prediction",
        "limitations": [
            "This is a bounded diagnostic estimate, not an oscillator calibration or conformance measurement.",
            "Analog filtering, impairments, silence, repeated patterns, and insufficient correlation can make the estimate unavailable or ambiguous.",
            "The reported slope uses two window positions and is not a fit over the complete recording.",
        ],
        "source_samples": source.samples,
        "capture_samples": capture.samples,
        "window_samples": window,
        "decimation": step,
        "start": {"source_sample": 0, "capture_sample": start[0], "confidence": start[1]},
    }
    if end is None:
        result["reason"] = "no nonconstant end correlation window near predicted position"
        return result
    denominator = float(max(1, source_end_position))
    slope = (end[0] - start[0]) / denominator
    result["end"] = {"source_sample": source_end_position, "capture_sample": end[0], "confidence": end[1]}
    result["relative_ppm"] = (slope - 1.0) * 1_000_000.0
    result["lag_samples"] = end[0] - start[0] - source_end_position
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="mono float32 48 kHz capture WAV")
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--source", type=Path,
                        help="optional mono PCM16 or packed PCM24 48 kHz source WAV")
    parser.add_argument("--zero-run-samples", type=int, default=DEFAULT_RUN_THRESHOLD)
    parser.add_argument("--constant-run-samples", type=int, default=DEFAULT_RUN_THRESHOLD)
    parser.add_argument("--dropout-level", type=float, default=DEFAULT_DROPOUT_LEVEL)
    parser.add_argument("--correlation-window-seconds", type=float, default=1.0)
    parser.add_argument("--correlation-search-seconds", type=float, default=15.0)
    args = parser.parse_args()
    if args.zero_run_samples < 1 or args.constant_run_samples < 1 or args.dropout_level < 0:
        parser.error("run thresholds must be positive and dropout level must be nonnegative")
    try:
        result = analyze_capture(args.capture, args.zero_run_samples,
                                  args.constant_run_samples, args.dropout_level)
        if args.source:
            result["source_correlation"] = estimate_correlation(
                inspect_wav(args.capture), inspect_wav(args.source),
                args.correlation_window_seconds, args.correlation_search_seconds)
    except (OSError, WavError, ValueError) as error:
        print(f"analyze_capture: error: {error}")
        return 1
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
