# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Bounded one-path Gaussian Doppler fading state for CW impairments."""

import math

import numpy as np

from . import FS

CW_DOPPLER_ALGORITHM_VERSION = 1
DOPPLER_RATE_MULTIPLE_MIN = 64.0
DOPPLER_TRUNCATION_DECADES = 2.0
DOPPLER_TRUNCATION_LOG = DOPPLER_TRUNCATION_DECADES * math.log(10.0)


class RayleighDoppler:
    """One Rayleigh complex gain stream with Watterson's FIR and interpolation.

    State is one finite Gaussian-filter history and two adjacent complex nodes;
    samples are generated only for requested, contiguous CW-window spans.
    """

    def __init__(self, spread_hz, rng, window_frames):
        self.spread_hz = float(spread_hz)
        self.rng = rng
        self.window_frames = int(window_frames)
        scale_hz = FS / DOPPLER_RATE_MULTIPLE_MIN
        self.frames_per_node = max(int(math.floor(scale_hz / self.spread_hz)), 1)
        self.node_rate_hz = FS / self.frames_per_node
        self.rate_multiple = self.node_rate_hz / self.spread_hz
        self.half_taps = int(math.ceil(
            self.rate_multiple * math.sqrt(DOPPLER_TRUNCATION_LOG) / math.pi))
        offsets = np.arange(-self.half_taps, self.half_taps + 1, dtype=np.float64)
        raw = np.exp(-np.square(math.pi * offsets / self.rate_multiple))
        self.coefficients = np.asarray(raw / math.sqrt(float(np.dot(raw, raw))), dtype=np.float32)
        self.coefficient64 = self.coefficients.astype(np.float64)
        self.coefficient_energy = float(np.dot(self.coefficient64, self.coefficient64))
        self.lag_one_correlation = float(np.dot(self.coefficient64[:-1], self.coefficient64[1:]) /
                                         self.coefficient_energy)
        frames = float(self.frames_per_node)
        self.interpolation_power = 1.0 - (1.0 - self.lag_one_correlation) * (frames * frames - 1.0) / (3.0 * frames * frames)
        self.node_scale = 1.0 / math.sqrt(self.coefficient_energy * self.interpolation_power)

        self.history = np.zeros(len(self.coefficients), dtype=np.complex128)
        self.cursor = 0
        self.started = False
        self.node_start = 0
        self.node_end = 0
        self.current_node = np.complex64(0.0j)
        self.next_node = np.complex64(0.0j)

    def _draw_complex_normal(self):
        pair = self.rng.standard_normal(2) / math.sqrt(2.0)
        return np.complex64(complex(float(pair[0]), float(pair[1])))

    def _filtered_history(self):
        real = float(np.dot(self.coefficient64, self.history.real))
        imag = float(np.dot(self.coefficient64, self.history.imag))
        return np.complex64(complex(real * self.node_scale, imag * self.node_scale))

    def _filter_node(self):
        self.history[:-1] = self.history[1:]
        self.history[-1] = self._draw_complex_normal()
        return self._filtered_history()

    def _initialize(self):
        for index in range(len(self.coefficients)):
            self.history[index] = self._draw_complex_normal()
        self.current_node = self._filtered_history()
        self.next_node = self._filter_node()
        self.node_start = 0
        self.node_end = min(self.window_frames, self.frames_per_node)
        self.started = True

    def render(self, first_relative_frame, count):
        """Return the complex fading gain for the next contiguous window span."""
        if first_relative_frame < 0 or count < 0:
            raise ValueError("CW fading span must have non-negative frame bounds")
        if first_relative_frame + count > self.window_frames:
            raise ValueError("CW fading span must remain inside the CW window")
        if count == 0:
            return np.zeros(0, dtype=np.complex64)
        if first_relative_frame != self.cursor:
            raise ValueError("CW fading spans must be contiguous from the window start")

        output = np.empty(count, dtype=np.complex64)
        cursor = first_relative_frame
        end_frame = first_relative_frame + count
        while cursor < end_frame:
            if not self.started:
                self._initialize()
            elif cursor >= self.node_end:
                self.current_node = self.next_node
                self.next_node = self._filter_node()
                self.node_start = self.node_end
                self.node_end = min(self.window_frames, self.node_start + self.frames_per_node)

            span_end = min(end_frame, self.node_end)
            output_start = cursor - first_relative_frame
            output_end = span_end - first_relative_frame
            offsets = np.arange(cursor - self.node_start, span_end - self.node_start,
                                dtype=np.float32)
            fraction = offsets / np.float32(self.frames_per_node)
            output[output_start:output_end] = self.current_node + fraction * (self.next_node - self.current_node)
            cursor = span_end

        self.cursor = end_frame
        return output
