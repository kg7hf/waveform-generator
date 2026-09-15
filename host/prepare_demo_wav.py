#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Explicit demo conversion: integer PCM -> mono -> 48 kHz -> repeat/trim -> PCM24.

This small linear interpolator is for audible demonstrations, not calibrated
resampling or conformance vectors. The source file and conversion report remain
separate so its changes cannot be mistaken for the downloaded original.
"""
import argparse
import json
import math
from pathlib import Path
import wave

import numpy as np

from signal_lab.wav_io import wav_info, write_pcm24


def prepare(source, output, seconds):
    source, output = Path(source), Path(output)
    report = output.with_suffix(".conversion.json")
    if output.exists() or report.exists():
        raise ValueError("refusing to replace an existing demo artifact")
    if not math.isfinite(seconds) or not 0 < seconds <= 60:
        raise ValueError("duration must be finite, greater than zero and at most 60 seconds")
    if source.stat().st_size > 8 * 1024 * 1024:
        raise ValueError("demo source must be at most 8 MiB")
    with wave.open(str(source), "rb") as reader:
        channels, width, rate, frames = reader.getnchannels(), reader.getsampwidth(), reader.getframerate(), reader.getnframes()
        if reader.getcomptype() != "NONE" or width not in (1, 2, 3, 4) or channels not in (1, 2) or not 8000 <= rate <= 48000 or not frames:
            raise ValueError("demo input must be nonempty mono/stereo integer PCM, 8â€“48 kHz, 8/16/24/32 bit")
        raw = reader.readframes(frames)
    if len(raw) != frames * channels * width:
        raise ValueError("truncated input WAV")
    if width == 1:
        samples = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128) / 128
    elif width == 3:
        packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        values = packed[:, 0] | (packed[:, 1] << 8) | (packed[:, 2] << 16)
        samples = ((values ^ 0x800000) - 0x800000).astype(np.float64) / 8388608
    else:
        samples = np.frombuffer(raw, dtype="<i%d" % width).astype(np.float64) / 2 ** (width * 8 - 1)
    mono = samples.reshape(-1, channels).mean(axis=1)
    resampled_frames = math.ceil(frames * 48000 / rate)
    positions = np.arange(resampled_frames, dtype=np.float64) * rate / 48000
    resampled = np.interp(positions, np.arange(frames), mono)
    target_frames = round(seconds * 48000)
    if target_frames == 0:
        raise ValueError("duration must span at least one output frame")
    output_samples = np.tile(resampled, math.ceil(target_frames / len(resampled)))[:target_frames]
    # Leave deliberate headroom for the subsequent interference demonstration.
    peak = float(np.max(np.abs(output_samples)))
    if peak == 0:
        raise ValueError("silent demo source")
    gain = min(1.0, 0.25 / peak)
    output.parent.mkdir(parents=True, exist_ok=True)
    clipped = write_pcm24(output, output_samples * gain)
    metadata = {"source": str(source.resolve()),
                "source_channels": channels, "source_bits": width * 8, "source_rate_hz": rate,
                "operations": ["mean of channels", "linear interpolation to 48000 Hz", "repeat/trim", "peak headroom", "PCM24 half-even quantization"],
                "output": str(output.resolve()),
                "output_format": wav_info(output), "gain": gain, "clipped_samples": clipped}
    report.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--seconds", type=float, default=10)
    args = parser.parse_args()
    try:
        print(json.dumps(prepare(args.source, args.output, args.seconds)))
    except (ValueError, OSError, wave.Error) as error:
        parser.exit(2, "demo conversion: %s\n" % error)


if __name__ == "__main__":
    main()
