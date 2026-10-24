# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Streaming discrete-time Markov on/off gate for deterministic CW stages."""

import math

import numpy as np

MARKOV_GATE_ALGORITHM_VERSION = 1


class MarkovOnOffGate:
    """Alternating geometric dwell gate with bounded, block-independent state.

    One uniform draw is consumed when each dwell is entered. ``window_frames``
    clips the final dwell so its ramp is applied to the visible window fragment.
    """

    def __init__(self, mean_on_frames, mean_off_frames, window_frames, rng):
        self.mean_on_frames = float(mean_on_frames)
        self.mean_off_frames = float(mean_off_frames)
        self.window_frames = int(window_frames)
        self.rng = rng
        self.cursor = 0
        self.started = False
        self.on = True
        self.run_start = 0
        self.run_end = 0

    def _dwell_frames(self, mean_frames):
        # Consume one draw even for p=1, making the draw contract independent
        # of which state happens to have a one-frame mean.
        uniform = float(self.rng.random())
        if mean_frames <= 1.0:
            return 1
        probability = 1.0 / mean_frames
        return int(math.floor(math.log1p(-uniform) / math.log1p(-probability))) + 1

    def _enter_dwell(self):
        if not self.started:
            self.started = True
            self.on = True
            self.run_start = 0
            self.run_end = min(self.window_frames, self._dwell_frames(self.mean_on_frames))
            return
        self.on = not self.on
        self.run_start = self.run_end
        if self.run_start >= self.window_frames:
            self.run_end = self.window_frames
            return
        mean_frames = self.mean_on_frames if self.on else self.mean_off_frames
        self.run_end = min(self.window_frames, self.run_start + self._dwell_frames(mean_frames))

    def render(self, first_relative_frame, count, ramp_frames):
        """Return (on-state mask, ramp envelope) for the next contiguous span."""
        if first_relative_frame < 0 or count < 0:
            raise ValueError("Markov gate span must have non-negative frame bounds")
        if first_relative_frame + count > self.window_frames:
            raise ValueError("Markov gate span must remain inside the CW window")
        if count == 0:
            return np.zeros(0, dtype=bool), np.zeros(0, dtype=np.float64)
        if first_relative_frame != self.cursor:
            raise ValueError("Markov CW processing must be contiguous from the window start")

        on_mask = np.zeros(count, dtype=bool)
        envelope = np.zeros(count, dtype=np.float64)
        end_relative_frame = first_relative_frame + count
        cursor = first_relative_frame
        while cursor < end_relative_frame:
            if not self.started or cursor >= self.run_end:
                self._enter_dwell()
            run_end = min(end_relative_frame, self.run_end)
            local_start = cursor - first_relative_frame
            local_end = run_end - first_relative_frame
            if self.on:
                on_mask[local_start:local_end] = True
                dwell_length = self.run_end - self.run_start
                ramp = min(int(ramp_frames), dwell_length // 2)
                if ramp == 0:
                    envelope[local_start:local_end] = 1.0
                else:
                    relative = np.arange(cursor, run_end, dtype=np.int64) - self.run_start
                    distance = np.minimum(relative, dwell_length - 1 - relative)
                    envelope[local_start:local_end] = np.sin(
                        np.pi / 2.0 * np.minimum(1.0, distance / ramp)) ** 2
            cursor = run_end

        self.cursor = end_relative_frame
        return on_mask, envelope
