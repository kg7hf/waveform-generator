# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

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
from .cw_doppler import CW_DOPPLER_ALGORITHM_VERSION, RayleighDoppler
from .fsk import FSK_ALGORITHM_VERSION, FskOscillator
from .markov import MARKOV_GATE_ALGORITHM_VERSION, MarkovOnOffGate
from .rng import FAMILY_CW_DOPPLER, FAMILY_FSK_SYMBOL, FAMILY_MARKOV_GATE, FAMILY_TONE_PHASE, generator
from .stream import Impairment, number, seconds_to_frames

MAX_PULSE_SECONDS = 1.0e9
FRAME_ALIGNMENT_TOLERANCE = 1.0e-6
SWEEP_PHASE_TURN = 1 << 64
SWEEP_EXACT_INTEGER_LIMIT = 1 << 53
SWEEP_ALGORITHM_VERSION = 1
MARKOV_GATE_RNG_IMPLEMENTATION = "numpy-pcg64"
FSK_RNG_IMPLEMENTATION = "numpy-pcg64"
MAX_FSK_WINDOW_FRAMES = 48_000_000_000_000
FSK_FRAME_ALIGNMENT_TOLERANCE = 1.0e-6
CW_DOPPLER_RNG_IMPLEMENTATION = "numpy-pcg64"


def _whole_pulse_frames(frames_exact, name):
    """Accept a pulse frame count only when it is already aligned to the sample grid."""
    frames = int(round(frames_exact))
    if frames_exact < 1.0 or frames <= 0 or abs(frames_exact - frames) > FRAME_ALIGNMENT_TOLERANCE:
        raise ValueError(name + " must map to a positive whole number of 48 kHz frames")
    return frames


def _pulse_frames(seconds, name):
    """Convert seconds to pulse frames only when already aligned to the sample grid."""
    return _whole_pulse_frames(seconds * FS, name)


def _sweep_cycle_step_q64(low_hz, high_hz, slope_hz_per_second):
    """Round the triangle cycle rate to the C++ contract's half-even Q0.64 step."""
    ideal = (slope_hz_per_second / (2.0 * (high_hz - low_hz) * FS)) * SWEEP_PHASE_TURN
    if not math.isfinite(ideal) or ideal < 0.5 or ideal >= SWEEP_EXACT_INTEGER_LIMIT:
        raise ValueError("cw.frequency_sweep slope has no representable Q0.64 cycle step")
    step = int(round(ideal))
    if step <= 0 or step >= SWEEP_EXACT_INTEGER_LIMIT:
        raise ValueError("cw.frequency_sweep slope has no representable Q0.64 cycle step")
    return step


class Cw(Impairment):
    """params: exactly one of frequency_hz or frequency_sweep, ci_db,
    phase_degrees (number | 'random', default 0),
    start_seconds (0), duration_seconds (None = to end), ramp_seconds (0.01),
    optional paired pulse_period_seconds and pulse_duty_cycle, or
    markov_on_off with mean_on_seconds and mean_off_seconds, or fsk with
    baud, full shift_hz, and alternating/random pattern."""

    type_name = "cw"

    def prepare(self, reference_rms, total_frames, seed, index):
        super().prepare(reference_rms, total_frames, seed, index)
        has_fixed_frequency = "frequency_hz" in self.params
        has_sweep = "frequency_sweep" in self.params
        if has_fixed_frequency == has_sweep:
            raise ValueError("cw must specify exactly one of frequency_hz or frequency_sweep")
        self.fsk_enabled = "fsk" in self.params
        if self.fsk_enabled and (not has_fixed_frequency or has_sweep):
            raise ValueError("cw.fsk requires frequency_hz as its center and cannot use frequency_sweep")
        self.frequency_sweep_enabled = has_sweep
        if has_fixed_frequency:
            self.frequency_hz = number(self.params["frequency_hz"], "cw.frequency_hz", 1, 23999)
        else:
            sweep = self.params["frequency_sweep"]
            if not isinstance(sweep, dict) or set(sweep) != {"low_hz", "high_hz", "slope_hz_per_second"}:
                raise ValueError("cw.frequency_sweep must contain exactly low_hz, high_hz, and slope_hz_per_second")
            self.sweep_low_hz = number(sweep["low_hz"], "cw.frequency_sweep.low_hz", 1, 23999)
            self.sweep_high_hz = number(sweep["high_hz"], "cw.frequency_sweep.high_hz", 1, 23999)
            if self.sweep_low_hz >= self.sweep_high_hz:
                raise ValueError("cw.frequency_sweep must satisfy low_hz < high_hz")
            self.sweep_slope_hz_per_second = number(
                sweep["slope_hz_per_second"], "cw.frequency_sweep.slope_hz_per_second", 0.0)
            if self.sweep_slope_hz_per_second <= 0.0:
                raise ValueError("cw.frequency_sweep.slope_hz_per_second must be positive")
            self.sweep_cycle_step_q64 = _sweep_cycle_step_q64(
                self.sweep_low_hz, self.sweep_high_hz, self.sweep_slope_hz_per_second)
            self.sweep_period_frames = SWEEP_PHASE_TURN / self.sweep_cycle_step_q64
            self.sweep_period_seconds = self.sweep_period_frames / FS
            self.sweep_realized_slope_hz_per_second = (
                2.0 * (self.sweep_high_hz - self.sweep_low_hz) / self.sweep_period_seconds)
        if self.fsk_enabled:
            fsk = self.params["fsk"]
            if not isinstance(fsk, dict) or set(fsk) != {"baud", "shift_hz", "pattern"}:
                raise ValueError("cw.fsk must contain exactly baud, shift_hz, and pattern")
            self.fsk_baud = number(fsk["baud"], "cw.fsk.baud", 0.0, FS)
            if self.fsk_baud <= 0.0:
                raise ValueError("cw.fsk.baud must be positive")
            frames_exact = FS / self.fsk_baud
            if not math.isfinite(frames_exact) or frames_exact > MAX_FSK_WINDOW_FRAMES:
                raise ValueError("cw.fsk.baud symbol period exceeds the frame limit")
            self.fsk_frames_per_symbol = int(round(frames_exact))
            if (self.fsk_frames_per_symbol <= 0 or
                    abs(frames_exact - self.fsk_frames_per_symbol) > FSK_FRAME_ALIGNMENT_TOLERANCE):
                raise ValueError("cw.fsk.baud must map to a whole number of 48 kHz frames")
            self.fsk_shift_hz = number(fsk["shift_hz"], "cw.fsk.shift_hz", 0.0)
            if self.fsk_shift_hz <= 0.0:
                raise ValueError("cw.fsk.shift_hz must be positive")
            self.fsk_mark_hz = self.frequency_hz + self.fsk_shift_hz / 2.0
            self.fsk_space_hz = self.frequency_hz - self.fsk_shift_hz / 2.0
            if not (self.fsk_space_hz < self.fsk_mark_hz and
                    1.0 <= self.fsk_mark_hz <= 23999.0 and 1.0 <= self.fsk_space_hz <= 23999.0):
                raise ValueError("cw.fsk mark_hz and space_hz must each be in [1, 23999]")
            if (round(self.fsk_mark_hz * SWEEP_PHASE_TURN / FS) ==
                    round(self.fsk_space_hz * SWEEP_PHASE_TURN / FS)):
                raise ValueError("cw.fsk tones must have distinct Q0.64 phase steps")
            self.fsk_pattern = fsk["pattern"]
            if not isinstance(self.fsk_pattern, str) or self.fsk_pattern not in ("alternating", "random"):
                raise ValueError("cw.fsk.pattern must be 'alternating' or 'random'")
        self.fading_enabled = "fading" in self.params
        if self.fading_enabled:
            fading = self.params["fading"]
            if not isinstance(fading, dict) or set(fading) != {"spread_hz"}:
                raise ValueError("cw.fading must contain exactly spread_hz")
            self.fading_spread_hz = number(fading["spread_hz"], "cw.fading.spread_hz", 0.1, 10.0)
        has_period = "pulse_period_seconds" in self.params
        has_duty = "pulse_duty_cycle" in self.params
        if has_period != has_duty:
            raise ValueError("cw.pulse_period_seconds and cw.pulse_duty_cycle must be specified together")
        self.markov_enabled = "markov_on_off" in self.params
        if self.markov_enabled and has_period:
            raise ValueError("cw.markov_on_off cannot be combined with periodic pulse fields")
        if self.markov_enabled:
            markov = self.params["markov_on_off"]
            if not isinstance(markov, dict) or set(markov) != {"mean_on_seconds", "mean_off_seconds"}:
                raise ValueError("cw.markov_on_off must contain exactly mean_on_seconds and mean_off_seconds")
            minimum_mean_seconds = 1.0 / FS
            self.markov_mean_on_seconds = number(
                markov["mean_on_seconds"], "cw.markov_on_off.mean_on_seconds",
                minimum_mean_seconds, 1.0e6)
            self.markov_mean_off_seconds = number(
                markov["mean_off_seconds"], "cw.markov_on_off.mean_off_seconds",
                minimum_mean_seconds, 1.0e6)
            self.markov_mean_on_frames = self.markov_mean_on_seconds * FS
            self.markov_mean_off_frames = self.markov_mean_off_seconds * FS
        self.ci_db = number(self.params["ci_db"], "cw.ci_db", -120, 120)
        self.start = seconds_to_frames(self.params.get("start_seconds", 0.0), "cw.start_seconds")
        duration = self.params.get("duration_seconds")
        self.end = total_frames if duration is None else min(total_frames, self.start + seconds_to_frames(duration, "cw.duration_seconds", True))
        self.length = max(0, self.end - self.start)
        phase = self.params.get("phase_degrees", 0.0)
        if phase == "random":
            self.phase_degrees = float(generator(self.rng_seed, FAMILY_TONE_PHASE, self.rng_index).uniform(0.0, 360.0))
        else:
            self.phase_degrees = number(phase, "cw.phase_degrees", -360, 360)
        ramp = self.params.get("ramp_seconds", 0.01)
        self.ramp = seconds_to_frames(ramp, "cw.ramp_seconds")
        if 2 * self.ramp > self.length:
            self.ramp = self.length // 2
        self.amplitude = math.sqrt(2.0) * self.reference_rms * 10.0 ** (-self.ci_db / 20.0)

        if self.fsk_enabled:
            fsk_rng = (generator(self.rng_seed, FAMILY_FSK_SYMBOL, self.rng_index)
                       if self.fsk_pattern == "random" else None)
            self._fsk_oscillator = FskOscillator(
                self.fsk_mark_hz, self.fsk_space_hz, self.fsk_frames_per_symbol,
                self.fsk_pattern, self.length, fsk_rng)
        if self.fading_enabled:
            self._rayleigh_doppler = RayleighDoppler(
                self.fading_spread_hz, generator(self.rng_seed, FAMILY_CW_DOPPLER, self.rng_index), self.length)

        self.pulse_enabled = has_period
        if self.pulse_enabled:
            period_seconds = number(self.params["pulse_period_seconds"], "cw.pulse_period_seconds",
                                    0.0, MAX_PULSE_SECONDS)
            if period_seconds <= 0.0:
                raise ValueError("cw.pulse_period_seconds must be positive")
            self.pulse_duty_cycle = number(self.params["pulse_duty_cycle"], "cw.pulse_duty_cycle", 0.0, 1.0)
            if self.pulse_duty_cycle <= 0.0:
                raise ValueError("cw.pulse_duty_cycle must be greater than zero and at most one")
            self.pulse_period_frames = _pulse_frames(period_seconds, "cw.pulse_period_seconds")
            self.pulse_on_frames = _whole_pulse_frames(
                self.pulse_period_frames * self.pulse_duty_cycle, "cw.pulse_period_seconds * cw.pulse_duty_cycle")
            if self.pulse_on_frames > self.pulse_period_frames:
                raise ValueError("cw pulse on-time must not exceed its period")
        if self.markov_enabled:
            self._markov_gate = MarkovOnOffGate(
                self.markov_mean_on_frames, self.markov_mean_off_frames, self.length,
                generator(self.rng_seed, FAMILY_MARKOV_GATE, self.rng_index))
        if self.markov_enabled or self.fsk_enabled or self.fading_enabled:
            self._stateful_expected_stream_frame = None

    def process(self, block, first_frame):
        count = len(block)
        if self.markov_enabled or self.fsk_enabled or self.fading_enabled:
            if count == 0:
                return block
            if self._stateful_expected_stream_frame is None:
                if first_frame != 0:
                    raise ValueError("Stateful CW processing must begin at frame zero")
            elif first_frame != self._stateful_expected_stream_frame:
                raise ValueError("Stateful CW processing blocks must be contiguous")
            self._stateful_expected_stream_frame = first_frame + count
        absolute = np.arange(first_frame, first_frame + count)
        t = absolute - self.start
        selected = (t >= 0) & (t < self.length)
        if not np.any(selected):
            return block
        u = t[selected].astype(np.float64)
        if self.markov_enabled:
            on, envelope = self._markov_gate.render(int(u[0]), len(u), self.ramp)
        elif self.pulse_enabled:
            pulse_phase = np.remainder(u.astype(np.int64), self.pulse_period_frames)
            on = pulse_phase < self.pulse_on_frames
            envelope = np.zeros(len(u))
            if np.any(on):
                on_u = u[on].astype(np.int64)
                on_phase = pulse_phase[on]
                pulse_start = (on_u // self.pulse_period_frames) * self.pulse_period_frames
                pulse_length = np.minimum(self.pulse_on_frames, self.length - pulse_start)
                ramp = np.minimum(self.ramp, pulse_length // 2)
                pulse_distance = np.minimum(on_phase, pulse_length - 1 - on_phase)
                ramped = ramp > 0
                pulse_envelope = np.ones(len(on_u))
                pulse_envelope[ramped] = np.sin(
                    np.pi / 2 * np.minimum(1.0, pulse_distance[ramped] / ramp[ramped])) ** 2
                envelope[on] = pulse_envelope
        else:
            # Keep the legacy unpulsed arithmetic and envelope unchanged.
            envelope = np.ones(len(u))
            if self.ramp:
                distance = np.minimum(u, self.length - 1 - u)
                envelope = np.sin(np.pi / 2 * np.minimum(1.0, distance / self.ramp)) ** 2
        if self.fsk_enabled:
            phase = self._fsk_oscillator.render(int(u[0]), len(u), self.phase_degrees)
        elif self.frequency_sweep_enabled:
            phase = self._sweep_phase(u)
        else:
            # Preserve the legacy fixed-CW arithmetic exactly.
            phase = 2 * np.pi * self.frequency_hz * u / FS + math.radians(self.phase_degrees)
        tone = np.zeros(count)
        if self.fading_enabled:
            gain = self._rayleigh_doppler.render(int(u[0]), len(u))
            tone[selected] = self.amplitude * (gain.real * np.sin(phase) + gain.imag * np.cos(phase)) * envelope
        else:
            tone[selected] = self.amplitude * np.sin(phase) * envelope
        if self.markov_enabled or self.pulse_enabled:
            self.account(tone[selected][on])
        else:
            self.account(tone[selected])
        return block + tone

    def _sweep_phase(self, relative_frames):
        """Analytically integrate the quantized-period triangle from window start."""
        completed_cycles = np.floor(relative_frames / self.sweep_period_frames)
        remainder_frames = relative_frames - completed_cycles * self.sweep_period_frames
        remainder_seconds = remainder_frames / FS
        half_period_seconds = self.sweep_period_seconds / 2.0
        leg_seconds = np.minimum(remainder_seconds, half_period_seconds)
        rising_cycles = (self.sweep_low_hz * leg_seconds +
                         0.5 * self.sweep_realized_slope_hz_per_second * leg_seconds * leg_seconds)
        falling_seconds = np.maximum(0.0, remainder_seconds - half_period_seconds)
        falling_cycles = (self.sweep_high_hz * falling_seconds -
                          0.5 * self.sweep_realized_slope_hz_per_second *
                          falling_seconds * falling_seconds)
        cycle_area = (self.sweep_low_hz + self.sweep_high_hz) * 0.5 * self.sweep_period_seconds
        phase_cycles = completed_cycles * cycle_area + rising_cycles + falling_cycles
        phase_cycles += self.phase_degrees / 360.0
        return 2.0 * np.pi * np.remainder(phase_cycles, 1.0)

    def resolved(self):
        result = {"type": self.type_name}
        if not self.frequency_sweep_enabled:
            result["frequency_hz"] = self.frequency_hz
        result.update({"ci_db": self.ci_db, "phase_degrees": self.phase_degrees,
                       "start_frame": self.start, "end_frame": self.end,
                       "ramp_frames": self.ramp, "tone_peak_amplitude": self.amplitude,
                       "tone_rms_dbfs": 20 * math.log10(self.amplitude / math.sqrt(2.0)) if self.amplitude > 0 else None,
                       "convention": "ci_db = 20 log10(reference RMS / tone RMS); tone peak = sqrt(2) * tone RMS"})
        if self.frequency_sweep_enabled:
            result["frequency_sweep"] = {
                "low_hz": self.sweep_low_hz,
                "high_hz": self.sweep_high_hz,
                "slope_hz_per_second": self.sweep_slope_hz_per_second,
                "cycle_step_q64": self.sweep_cycle_step_q64,
                "realized_period_seconds": self.sweep_period_seconds,
            }
            result["sweep_algorithm_version"] = SWEEP_ALGORITHM_VERSION
        if self.fsk_enabled:
            result["fsk"] = {
                "baud": self.fsk_baud,
                "shift_hz": self.fsk_shift_hz,
                "mark_hz": self.fsk_mark_hz,
                "space_hz": self.fsk_space_hz,
                "frames_per_symbol": self.fsk_frames_per_symbol,
                "pattern": self.fsk_pattern,
                "algorithm_version": FSK_ALGORITHM_VERSION,
                "rng_family": FAMILY_FSK_SYMBOL,
                "rng_implementation": FSK_RNG_IMPLEMENTATION,
            }
        if self.fading_enabled:
            result["fading"] = {
                "spread_hz": self.fading_spread_hz,
                "algorithm_version": CW_DOPPLER_ALGORITHM_VERSION,
                "rng_family": FAMILY_CW_DOPPLER,
                "mean_power": 1.0,
                "distribution": "rayleigh",
                "rng_implementation": CW_DOPPLER_RNG_IMPLEMENTATION,
            }
        if self.pulse_enabled:
            result.update({"pulse_period_frames": self.pulse_period_frames,
                           "pulse_on_frames": self.pulse_on_frames,
                           "pulse_realized_duty_cycle": self.pulse_on_frames / self.pulse_period_frames})
        if self.markov_enabled:
            result["markov_on_off"] = {
                "mean_on_seconds": self.markov_mean_on_seconds,
                "mean_off_seconds": self.markov_mean_off_seconds,
                "mean_on_frames": self.markov_mean_on_frames,
                "mean_off_frames": self.markov_mean_off_frames,
                "initial_state": "on",
                "dwell_distribution": "geometric",
                "rng_family": FAMILY_MARKOV_GATE,
                "algorithm_version": MARKOV_GATE_ALGORITHM_VERSION,
                "rng_implementation": MARKOV_GATE_RNG_IMPLEMENTATION,
            }
        return result
