"""Deterministic multiplicative amplitude fades (applied to the desired signal only).

Shape "raised_cosine": raised-cosine attack and recovery around a full-depth
hold (pcm_stress faded_signal); with only duration_ms given, attack =
recovery = duration/4 and hold = duration/2.  Shape "rectangular": full depth
for the whole duration with instantaneous edges.  Overlapping fades multiply
(depths add in dB).  Schedules: explicit starts_seconds, periodic
(first_seconds + period_seconds [+ count]) or Poisson (rate_per_sec, rng
family 8).
"""

import numpy as np

from . import FS
from .rng import FAMILY_FADE_SCHEDULE, generator
from .stream import Impairment, integer, number, seconds_to_frames

MAX_EVENTS = 100000


class Fade(Impairment):
    """params: depth_db, duration_ms | (attack_ms, hold_ms, recovery_ms), shape ('raised_cosine'|'rectangular'),
    starts_seconds [..] | (first_seconds, period_seconds[, count]) | rate_per_sec, start_seconds/duration_seconds window."""

    type_name = "fade"

    def prepare(self, reference_rms, total_frames, seed, index):
        super().prepare(reference_rms, total_frames, seed, index)
        p = self.params
        self.depth_db = number(p["depth_db"], "fade.depth_db", 0, 300)
        self.shape = p.get("shape", "raised_cosine")
        if self.shape not in ("raised_cosine", "rectangular"):
            raise ValueError("fade.shape must be raised_cosine or rectangular")
        if "attack_ms" in p or "hold_ms" in p or "recovery_ms" in p:
            self.attack = seconds_to_frames(number(p.get("attack_ms", 0), "fade.attack_ms", 0) / 1e3, "fade.attack_ms")
            self.hold = seconds_to_frames(number(p.get("hold_ms", 0), "fade.hold_ms", 0) / 1e3, "fade.hold_ms")
            self.recovery = seconds_to_frames(number(p.get("recovery_ms", 0), "fade.recovery_ms", 0) / 1e3, "fade.recovery_ms")
        else:
            total = seconds_to_frames(number(p["duration_ms"], "fade.duration_ms", 0.1) / 1e3, "fade.duration_ms", True)
            if self.shape == "rectangular":
                self.attack, self.recovery, self.hold = 0, 0, total
            else:
                self.attack = self.recovery = total // 4
                self.hold = total - self.attack - self.recovery
        if self.shape == "rectangular":
            self.hold += self.attack + self.recovery
            self.attack = self.recovery = 0
        self.length = self.attack + self.hold + self.recovery
        if self.length <= 0:
            raise ValueError("fade duration must span at least one sample")
        window_start = seconds_to_frames(p.get("start_seconds", 0.0), "fade.start_seconds")
        duration = p.get("duration_seconds")
        window_end = total_frames if duration is None else min(total_frames, window_start + seconds_to_frames(duration, "fade.duration_seconds", True))
        if "starts_seconds" in p:
            starts = sorted(seconds_to_frames(s, "fade.starts_seconds[]") for s in p["starts_seconds"])
            self.schedule = "explicit"
        elif "rate_per_sec" in p:
            rate = number(p["rate_per_sec"], "fade.rate_per_sec", 1e-6, 1000)
            rng = generator(seed, FAMILY_FADE_SCHEDULE, index)
            starts, position = [], float(window_start)
            while True:
                position += float(rng.exponential(FS / rate))
                frame = int(round(position))
                if frame >= window_end:
                    break
                starts.append(frame)
                if len(starts) > MAX_EVENTS:
                    raise ValueError("fade: too many events")
            self.schedule = "poisson"
        else:
            first = seconds_to_frames(p.get("first_seconds", window_start / FS), "fade.first_seconds")
            period = seconds_to_frames(p["period_seconds"], "fade.period_seconds", True)
            starts = list(range(first, window_end, period))
            if "count" in p:
                starts = starts[:integer(p["count"], "fade.count", 1, 2 ** 32 - 1)]
            self.schedule = "periodic"
        self.starts = [s for s in starts if s < total_frames]
        if len(self.starts) > MAX_EVENTS:
            raise ValueError("fade: too many events")
        self.cursor = 0
        self.faded_frames = 0
        self.minimum_gain = 1.0

    def gain(self, first_frame, count):
        gain = np.ones(count)
        absolute = np.arange(first_frame, first_frame + count)
        while self.cursor < len(self.starts) and self.starts[self.cursor] + self.length <= first_frame:
            self.cursor += 1
        touched = np.zeros(count, dtype=bool)
        for event in range(self.cursor, len(self.starts)):
            start = self.starts[event]
            if start >= first_frame + count:
                break
            t = absolute - start
            selected = (t >= 0) & (t < self.length)
            if not np.any(selected):
                continue
            u = t[selected].astype(np.float64)
            depth = np.ones(len(u))
            if self.attack:
                attack = u < self.attack
                depth[attack] = 0.5 * (1 - np.cos(np.pi * u[attack] / self.attack))
            if self.recovery:
                recovering = u >= self.attack + self.hold
                phase = (u[recovering] - self.attack - self.hold) / self.recovery
                depth[recovering] = 0.5 * (1 + np.cos(np.pi * phase))
            gain[selected] *= 10.0 ** (-self.depth_db * depth / 20.0)
            touched |= selected
        self.faded_frames += int(np.count_nonzero(touched))
        if np.any(touched):
            self.minimum_gain = min(self.minimum_gain, float(np.min(gain[touched])))
        return gain

    def process(self, block, first_frame):
        return block * self.gain(first_frame, len(block))

    def resolved(self):
        return {"type": self.type_name, "depth_db": self.depth_db, "shape": self.shape, "schedule": self.schedule,
                "attack_frames": self.attack, "hold_frames": self.hold, "recovery_frames": self.recovery,
                "event_frames": self.length, "event_count": len(self.starts),
                "convention": "multiplicative gain 10^(-depth_db*d(t)/20) on the desired signal only; raised-cosine d(t) over attack/recovery, 1 during hold; overlapping fades multiply"}

    def stats(self):
        return {"faded_frames": self.faded_frames, "faded_seconds": self.faded_frames / FS,
                "minimum_gain_db": 20 * np.log10(self.minimum_gain) if self.minimum_gain > 0 else None}

    def events(self):
        return [{"index": i, "start_frame": s, "start_seconds": s / FS, "end_frame": s + self.length} for i, s in enumerate(self.starts)]
