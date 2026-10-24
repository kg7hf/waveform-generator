# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Bounded-state binary FSK oscillator for CW impairment stages."""

import math

import numpy as np

from . import FS

FSK_ALGORITHM_VERSION = 1


class FskOscillator:
    """Generate phase-continuous binary FSK symbols from a contiguous span."""

    def __init__(self, mark_hz, space_hz, frames_per_symbol, pattern, window_frames, rng=None):
        self.mark_hz = float(mark_hz)
        self.space_hz = float(space_hz)
        self.frames_per_symbol = int(frames_per_symbol)
        self.pattern = pattern
        self.window_frames = int(window_frames)
        self.rng = rng
        self.cursor = 0
        self.started = False
        self.symbol_index = 0
        self.symbol_start = 0
        self.symbol_end = 0
        self.frequency_hz = 0.0
        self.phase_cycles = 0.0

    def _enter_symbol(self):
        if self.pattern == "alternating":
            is_mark = self.symbol_index % 2 == 0
        else:
            # A draw is consumed for every entered symbol, including symbol 0.
            is_mark = float(self.rng.random()) >= 0.5
        self.frequency_hz = self.mark_hz if is_mark else self.space_hz
        self.symbol_end = min(self.window_frames, self.symbol_start + self.frames_per_symbol)

    def render(self, first_relative_frame, count, phase_degrees):
        """Return phase radians for the next contiguous frame span.

        Validation precedes symbol or RNG state mutation. Empty spans do not
        initialize the first symbol or consume its random choice.
        """
        if first_relative_frame < 0 or count < 0:
            raise ValueError("FSK span must have non-negative frame bounds")
        if first_relative_frame + count > self.window_frames:
            raise ValueError("FSK span must remain inside the CW window")
        if count == 0:
            return np.zeros(0, dtype=np.float64)
        if first_relative_frame != self.cursor:
            raise ValueError("FSK processing must be contiguous from the window start")

        phase = np.empty(count, dtype=np.float64)
        cursor = first_relative_frame
        end_frame = first_relative_frame + count
        while cursor < end_frame:
            if not self.started:
                self.started = True
                self.symbol_index = 0
                self.symbol_start = 0
                self._enter_symbol()
            elif cursor >= self.symbol_end:
                elapsed_frames = self.symbol_end - self.symbol_start
                self.phase_cycles = math.remainder(
                    self.phase_cycles + elapsed_frames * self.frequency_hz / FS, 1.0)
                self.symbol_start = self.symbol_end
                self.symbol_index += 1
                self._enter_symbol()

            span_end = min(end_frame, self.symbol_end)
            output_start = cursor - first_relative_frame
            output_end = span_end - first_relative_frame
            offsets = np.arange(cursor, span_end, dtype=np.float64) - self.symbol_start
            cycles = self.phase_cycles + offsets * (self.frequency_hz / FS)
            phase[output_start:output_end] = 2.0 * np.pi * np.remainder(cycles, 1.0)
            cursor = span_end

        self.cursor = end_frame
        return phase + math.radians(phase_degrees)
