"""RIFF/WAVE helpers for 48 kHz mono streams (PCM16 canonical, float32 accepted)."""

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
        info["samples"] = data["data_bytes"] // max(1, fmt["block_align"])
        if fmt["format_tag"] == 1 and fmt["bits_per_sample"] == 16:
            info["encoding"] = "pcm_s16le"
        elif fmt["format_tag"] == 3 and fmt["bits_per_sample"] == 32:
            info["encoding"] = "pcm_f32le"
        else:
            info["encoding"] = "unsupported"
        return info


def require_canonical(info):
    """The project's canonical container: 48 kHz, mono, PCM16 (fixtures/README.md)."""
    if info["channels"] != 1 or info["sample_rate_hz"] != FS or info["encoding"] != "pcm_s16le":
        raise WavError("%s: expected 48 kHz mono PCM16, found %d ch %d Hz %s" %
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


def read_float(path, require_pcm16=True):
    """Whole-file read as float64 in [-1, 1).  PCM16 divides by 32768 (pcm_stress convention)."""
    info = wav_info(path)
    if info["channels"] != 1 or info["sample_rate_hz"] != FS:
        raise WavError("%s: expected 48 kHz mono" % path)
    with open(path, "rb") as handle:
        handle.seek(info["data_offset_bytes"])
        raw = handle.read(info["data_bytes"])
    if info["encoding"] == "pcm_s16le":
        samples = np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<i2").astype(np.float64) / 32768.0
    elif info["encoding"] == "pcm_f32le" and not require_pcm16:
        samples = np.frombuffer(raw[: len(raw) // 4 * 4], dtype="<f4").astype(np.float64)
    else:
        raise WavError("%s: unsupported encoding %s" % (path, info["encoding"]))
    return samples, info


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
