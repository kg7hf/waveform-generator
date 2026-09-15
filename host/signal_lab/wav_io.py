# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""48 kHz mono PCM24 artifacts, explicit legacy PCM16 input and float inspection.

The PCM24 quantizer rejects non-finite values before narrowing and packs the
signed low 24 bits little-endian. Hashes exclude any RIFF data-chunk padding.
"""

import hashlib
import struct
import wave

import numpy as np

from . import FS


class WavError(ValueError):
    pass


def sha256_file(path, chunk=1 << 20):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for data in iter(lambda: handle.read(chunk), b""):
            digest.update(data)
    return digest.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def wav_info(path):
    """Parse the RIFF chunk list; return the format and the raw data-chunk location."""
    with open(path, "rb") as handle:
        head = handle.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise WavError("%s: not a RIFF/WAVE file" % path)
        info = {"path": str(path)}
        offset = 12
        fmt = None
        data = None
        while True:
            handle.seek(offset)
            header = handle.read(8)
            if len(header) < 8:
                break
            tag, size = header[:4], struct.unpack("<I", header[4:8])[0]
            body = offset + 8
            if tag == b"fmt ":
                payload = handle.read(min(size, 40))
                if len(payload) < 16:
                    raise WavError("%s: truncated fmt chunk" % path)
                format_tag, channels, rate, byte_rate, block_align, bits = struct.unpack("<HHIIHH", payload[:16])
                fmt = {"format_tag": format_tag, "channels": channels, "sample_rate_hz": rate, "byte_rate": byte_rate,
                       "block_align": block_align, "bits_per_sample": bits}
                if format_tag == 0xFFFE and len(payload) >= 26:
                    fmt["format_tag"] = struct.unpack("<H", payload[24:26])[0]
            elif tag == b"data":
                data = {"data_offset_bytes": body, "data_bytes": size}
                break
            offset = body + size + (size & 1)
        if fmt is None or data is None:
            raise WavError("%s: missing fmt or data chunk" % path)
        info.update(fmt)
        info.update(data)
        if (fmt["block_align"] != fmt["channels"] * ((fmt["bits_per_sample"] + 7) // 8)
                or fmt["block_align"] == 0 or data["data_bytes"] % fmt["block_align"]
                or fmt["byte_rate"] != fmt["sample_rate_hz"] * fmt["block_align"]):
            raise WavError("%s: inconsistent sample geometry" % path)
        info["samples"] = data["data_bytes"] // max(1, fmt["block_align"])
        if fmt["format_tag"] == 1 and fmt["bits_per_sample"] == 16:
            info["encoding"] = "pcm_s16le"
        elif fmt["format_tag"] == 1 and fmt["bits_per_sample"] == 24:
            info["encoding"] = "pcm_s24le"
        elif fmt["format_tag"] == 3 and fmt["bits_per_sample"] == 32:
            info["encoding"] = "pcm_f32le"
        else:
            info["encoding"] = "unsupported"
        return info


def require_canonical(info, *, allow_legacy_pcm16=False):
    """Require 48 kHz mono PCM24; callers must explicitly allow legacy input."""
    encodings = ("pcm_s24le", "pcm_s16le") if allow_legacy_pcm16 else ("pcm_s24le",)
    if info["channels"] != 1 or info["sample_rate_hz"] != FS or info["encoding"] not in encodings:
        raise WavError("%s: expected 48 kHz mono PCM24, found %d ch %d Hz %s" %
                       (info["path"], info["channels"], info["sample_rate_hz"], info["encoding"]))


def pcm_region_sha256(path, info=None):
    info = info or wav_info(path)
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        handle.seek(info["data_offset_bytes"])
        remaining = info["data_bytes"]
        while remaining:
            data = handle.read(min(1 << 20, remaining))
            if not data:
                raise WavError("%s: data chunk truncated" % path)
            digest.update(data)
            remaining -= len(data)
    return digest.hexdigest()


def read_float(path, *, allow_float=False):
    """Read PCM24/legacy PCM16 as float64; floating capture input is opt-in."""
    info = wav_info(path)
    if info["channels"] != 1 or info["sample_rate_hz"] != FS:
        raise WavError("%s: expected 48 kHz mono" % path)
    with open(path, "rb") as handle:
        handle.seek(info["data_offset_bytes"])
        raw = handle.read(info["data_bytes"])
    if len(raw) != info["data_bytes"]:
        raise WavError("%s: data chunk truncated" % path)
    if info["encoding"] == "pcm_s16le":
        samples = np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<i2").astype(np.float64) / 32768.0
    elif info["encoding"] == "pcm_s24le":
        packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        values = packed[:, 0] | (packed[:, 1] << 8) | (packed[:, 2] << 16)
        values = (values ^ 0x800000) - 0x800000
        samples = values.astype(np.float64) / 8388608.0
    elif info["encoding"] == "pcm_f32le" and allow_float:
        samples = np.frombuffer(raw[: len(raw) // 4 * 4], dtype="<f4").astype(np.float64)
    else:
        raise WavError("%s: unsupported encoding %s" % (path, info["encoding"]))
    if not np.all(np.isfinite(samples)):
        raise WavError("%s: non-finite sample" % path)
    return samples, info


def quantize_pcm24(samples):
    """Return signed int32 values and clipping count, with half-even PCM24 rounding."""
    values = np.asarray(samples, dtype=np.float64)
    if not np.all(np.isfinite(values)):
        raise WavError("non-finite sample cannot be quantized")
    clipped = int(np.count_nonzero((values < -1.0) | (values > 8388607 / 8388608)))
    # Clamp before multiplication so even finite float64 maxima cannot overflow.
    pcm = np.rint(np.clip(values, -1.0, 8388607 / 8388608) * 8388608.0).astype("<i4")
    return pcm, clipped


class Pcm24Writer:
    """Streaming packed PCM24 writer. Close patches lengths and pads odd data bytes."""

    def __init__(self, path):
        self.writer = open(path, "wb")
        self.frames = 0
        try:
            self.writer.write(struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36, b"WAVE",
                                          b"fmt ", 16, 1, 1, FS, FS * 3, 3, 24, b"data", 0))
        except OSError:
            self.writer.close()
            raise

    def write(self, pcm):
        values = np.asarray(pcm)
        if values.ndim != 1 or values.dtype.kind not in "iu" or np.any(values < -8388608) or np.any(values > 8388607):
            raise WavError("PCM24 writer requires signed 24-bit integer sample values")
        if (self.frames + len(values)) * 3 > 0xFFFFFFFF - 37:
            raise WavError("PCM24 output exceeds RIFF length capacity")
        packed = np.ascontiguousarray(values, dtype="<i4").view(np.uint8).reshape(-1, 4)[:, :3]
        self.writer.write(packed.tobytes())
        self.frames += len(values)

    def close(self):
        if self.writer.closed:
            return
        try:
            data_bytes = self.frames * 3
            if data_bytes & 1:
                self.writer.write(b"\0")
            self.writer.seek(4)
            self.writer.write(struct.pack("<I", 36 + data_bytes + (data_bytes & 1)))
            self.writer.seek(40)
            self.writer.write(struct.pack("<I", data_bytes))
        finally:
            self.writer.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def write_pcm24(path, samples):
    pcm, clipped = quantize_pcm24(samples)
    with Pcm24Writer(path) as writer:
        writer.write(pcm)
    return clipped


def quantize_pcm16(samples):
    """Round-half-even of 32768*x saturated to int16 (pcm_stress convention); returns (pcm, clipped_count)."""
    scaled = np.asarray(samples, dtype=np.float64) * 32768.0
    clipped = int(np.count_nonzero((scaled < -32768.0) | (scaled > 32767.0)))
    pcm = np.rint(np.clip(scaled, -32768.0, 32767.0)).astype("<i2")
    return pcm, clipped


class Pcm16Writer:
    """Streaming PCM16 writer; the wave module patches the RIFF sizes on close."""

    def __init__(self, path):
        self.path = str(path)
        self.writer = wave.open(self.path, "wb")
        self.writer.setnchannels(1)
        self.writer.setsampwidth(2)
        self.writer.setframerate(FS)
        self.frames = 0

    def write(self, pcm):
        self.writer.writeframesraw(np.ascontiguousarray(pcm, dtype="<i2").tobytes())
        self.frames += len(pcm)

    def close(self):
        self.writer.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def write_pcm16(path, samples):
    pcm, clipped = quantize_pcm16(samples)
    with Pcm16Writer(path) as writer:
        writer.write(pcm)
    return clipped
