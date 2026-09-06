"""CW (sinusoidal) interferer at a carrier-to-interference ratio.

x'(n) = x(n) + A sin(2 pi f n / Fs + phi), A = sqrt(2) * reference_rms * 10^(-ci_db/20),
so ci_db = 20 log10(reference RMS / tone RMS).  Raised-cosine amplitude
shoulders (ramp_seconds) avoid key-click transients; phase is relative to the
tone's own start frame (identical to pcm_stress tone_signal with
level_db = -ci_db).
"""

import math

import numpy as np

from . import FS
from .rng import FAMILY_TONE_PHASE, generator
from .stream import Impairment, number, seconds_to_frames


class Cw(Impairment):
    """params: frequency_hz, ci_db, phase_degrees (number | 'random', default 0),
    start_seconds (0), duration_seconds (None = to end), ramp_seconds (0.01)."""

    type_name = "cw"

    def prepare(self, reference_rms, total_frames, seed, index):
        super().prepare(reference_rms, total_frames, seed, index)
        self.frequency_hz = number(self.params["frequency_hz"], "cw.frequency_hz", 1, FS / 2 - 1)
        self.ci_db = number(self.params["ci_db"], "cw.ci_db", -120, 120)
        self.start = seconds_to_frames(self.params.get("start_seconds", 0.0), "cw.start_seconds")
        duration = self.params.get("duration_seconds")
        self.end = total_frames if duration is None else min(total_frames, self.start + seconds_to_frames(duration, "cw.duration_seconds", True))
        self.length = max(0, self.end - self.start)
        phase = self.params.get("phase_degrees", 0.0)
        if phase == "random":
            self.phase_degrees = float(generator(seed, FAMILY_TONE_PHASE, index).uniform(0.0, 360.0))
        else:
            self.phase_degrees = number(phase, "cw.phase_degrees", -360, 360)
        ramp = self.params.get("ramp_seconds", 0.01)
        self.ramp = seconds_to_frames(ramp, "cw.ramp_seconds")
        if 2 * self.ramp > self.length:
            self.ramp = self.length // 2
        self.amplitude = math.sqrt(2.0) * self.reference_rms * 10.0 ** (-self.ci_db / 20.0)

    def process(self, block, first_frame):
        count = len(block)
        absolute = np.arange(first_frame, first_frame + count)
        t = absolute - self.start
        selected = (t >= 0) & (t < self.length)
        if not np.any(selected):
            return block
        u = t[selected].astype(np.float64)
        envelope = np.ones(len(u))
        if self.ramp:
            distance = np.minimum(u, self.length - 1 - u)
            envelope = np.sin(np.pi / 2 * np.minimum(1.0, distance / self.ramp)) ** 2
        phase = 2 * np.pi * self.frequency_hz * u / FS + math.radians(self.phase_degrees)
        tone = np.zeros(count)
        tone[selected] = self.amplitude * np.sin(phase) * envelope
        self.account(tone[selected])
        return block + tone

    def resolved(self):
        return {"type": self.type_name, "frequency_hz": self.frequency_hz, "ci_db": self.ci_db,
                "phase_degrees": self.phase_degrees, "start_frame": self.start, "end_frame": self.end,
                "ramp_frames": self.ramp, "tone_peak_amplitude": self.amplitude,
                "tone_rms_dbfs": 20 * math.log10(self.amplitude / math.sqrt(2.0)) if self.amplitude > 0 else None,
                "convention": "ci_db = 20 log10(reference RMS / tone RMS); tone peak = sqrt(2) * tone RMS"}
