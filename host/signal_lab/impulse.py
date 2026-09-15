# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Static crashes / impulsive noise.

Each event is i(t) = A * exp(-t / tau) * ring(t), t in [0, truncate_tau * tau):
  ring = cos(2 pi f_r t + phi)               ("tone" ringing, the specification model)
  ring = band-limited unit-variance Gaussian ("noise" ringing, a broadband crash)
A = reference_rms * 10^(peak_db / 20), i.e. peak_db is the envelope peak
relative to the clean-signal reference RMS.  Events are scheduled either as a
Poisson process (rate_per_sec, exponential inter-arrivals in sample units,
like pcm_stress) or periodically (period_seconds).  Phase per event is drawn
from rng family 5 unless fixed.
"""

import math

import numpy as np

from . import FS
from .awgn import bandlimit
from .rng import FAMILY_IMPULSE_NOISE, FAMILY_IMPULSE_PHASE, FAMILY_IMPULSE_SCHEDULE, generator
from .stream import Impairment, number, seconds_to_frames

MAX_EVENTS = 100000


class Impulse(Impairment):
    """params: peak_db, decay_ms, rate_per_sec | period_seconds, ring ('tone'|'noise', default 'tone'),
    ring_hz (1700), phase_degrees ('random' | number), truncate_tau (8), start_seconds (0),
    duration_seconds (None = to end), first_seconds (periodic only; default = start)."""

    type_name = "impulse"

    def prepare(self, reference_rms, total_frames, seed, index):
        super().prepare(reference_rms, total_frames, seed, index)
        p = self.params
        self.peak_db = number(p["peak_db"], "impulse.peak_db", -60, 120)
        self.decay_ms = number(p["decay_ms"], "impulse.decay_ms", 0.01, 5000)
        self.ring = p.get("ring", "tone")
        if self.ring not in ("tone", "noise"):
            raise ValueError("impulse.ring must be 'tone' or 'noise'")
        self.ring_hz = number(p.get("ring_hz", 1700.0), "impulse.ring_hz", 0, FS / 2 - 1)
        self.truncate_tau = number(p.get("truncate_tau", 8.0), "impulse.truncate_tau", 1, 40)
        self.start = seconds_to_frames(p.get("start_seconds", 0.0), "impulse.start_seconds")
        duration = p.get("duration_seconds")
        self.end = total_frames if duration is None else min(total_frames, self.start + seconds_to_frames(duration, "impulse.duration_seconds", True))
        self.tau = self.decay_ms * 1e-3 * FS
        self.length = max(2, int(round(self.truncate_tau * self.tau)))
        self.amplitude = self.reference_rms * 10.0 ** (self.peak_db / 20.0)
        phase = p.get("phase_degrees", "random")
        self.fixed_phase = None if phase == "random" else number(phase, "impulse.phase_degrees", -360, 360)
        if "rate_per_sec" in p and "period_seconds" in p:
            raise ValueError("impulse: give rate_per_sec or period_seconds, not both")
        if "rate_per_sec" in p:
            self.rate = number(p["rate_per_sec"], "impulse.rate_per_sec", 1e-6, 10000)
            self.period = None
            rng = generator(seed, FAMILY_IMPULSE_SCHEDULE, index)
            starts = []
            position = float(self.start)
            while True:
                position += float(rng.exponential(FS / self.rate))
                frame = int(round(position))
                if frame >= self.end:
                    break
                starts.append(frame)
                if len(starts) > MAX_EVENTS:
                    raise ValueError("impulse: too many events")
        else:
            self.rate = None
            self.period = seconds_to_frames(p["period_seconds"], "impulse.period_seconds", True)
            first = seconds_to_frames(p.get("first_seconds", p.get("start_seconds", 0.0)), "impulse.first_seconds")
            starts = list(range(max(first, self.start), self.end, self.period))
            if len(starts) > MAX_EVENTS:
                raise ValueError("impulse: too many events")
        phase_rng = generator(seed, FAMILY_IMPULSE_PHASE, index)
        self.starts = starts
        self.phases = [self.fixed_phase if self.fixed_phase is not None else float(phase_rng.uniform(0.0, 360.0)) for _ in starts]
        self.cursor = 0
        self.cache = {}

    def waveform(self, event):
        if event in self.cache:
            return self.cache[event]
        t = np.arange(self.length, dtype=np.float64)
        envelope = self.amplitude * np.exp(-t / self.tau)
        phi = math.radians(self.phases[event])
        if self.ring == "tone":
            ring = np.cos(2 * np.pi * self.ring_hz * t / FS + phi) if self.ring_hz > 0 else np.ones(self.length)
        else:
            rng = generator(self.seed, FAMILY_IMPULSE_NOISE, self.index * 65536 + event)
            ring = bandlimit(rng.standard_normal(self.length))
        wave = envelope * ring
        self.cache = {event: wave}
        return wave

    def process(self, block, first_frame):
        count = len(block)
        last = first_frame + count
        while self.cursor < len(self.starts) and self.starts[self.cursor] + self.length <= first_frame:
            self.cursor += 1
        added = np.zeros(count)
        for event in range(self.cursor, len(self.starts)):
            start = self.starts[event]
            if start >= last:
                break
            wave = self.waveform(event)
            left, right = max(first_frame, start), min(last, start + self.length)
            if right > left:
                added[left - first_frame:right - first_frame] += wave[left - start:right - start]
        self.account(added)
        return block + added

    def resolved(self):
        return {"type": self.type_name, "peak_db": self.peak_db, "decay_ms": self.decay_ms, "ring": self.ring,
                "ring_hz": self.ring_hz if self.ring == "tone" else None, "truncate_tau": self.truncate_tau,
                "rate_per_sec": self.rate, "period_frames": self.period, "start_frame": self.start, "end_frame": self.end,
                "event_frames": self.length, "event_count": len(self.starts),
                "peak_amplitude": self.amplitude, "peak_dbfs": 20 * math.log10(self.amplitude) if self.amplitude > 0 else None,
                "rng_families": {"schedule": FAMILY_IMPULSE_SCHEDULE, "phase": FAMILY_IMPULSE_PHASE, "noise_ring": FAMILY_IMPULSE_NOISE},
                "convention": "i(t) = A exp(-t/tau) ring(t); A = reference RMS * 10^(peak_db/20); Poisson schedule uses exponential inter-arrivals from the interval start"}

    def events(self):
        return [{"index": index, "start_frame": start, "start_seconds": start / FS, "phase_degrees": phase}
                for index, (start, phase) in enumerate(zip(self.starts, self.phases))]
