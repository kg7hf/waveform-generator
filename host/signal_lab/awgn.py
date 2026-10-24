# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Band-limited additive white Gaussian noise at the lane's SNR convention.

The Python peer of the C++ AWGN stage (signal-lab/include/signal_lab/awgn.hpp,
d896a99; guide docs/modules/signal-lab-engine.md, "AWGN"): N0 x 3000 Hz in
300-3300 Hz (MIL-STD-188-110D E.5.5 refers SNR to the input power).

  noise(n) = sigma_w (h * e)(n),   sigma_w = R 10^(-SNR/20) sqrt(fs / (2 x 3000 Hz))
  e(n)     unit-variance white Gaussian excitation (NumPy PCG64, family 1)
  h        Kaiser band-pass, A = 60 dB, cutoffs 150 and 3450 Hz, 583 taps,
           unit gain at 1800 Hz, flat within +/-0.01 dB over 300-3300 Hz

The one-sided density at 1800 Hz is N0 = 2 sigma_w^2 / fs, so N0 x 3000 Hz is
R^2 10^(-SNR/10): the delivered SNR by the lane's convention is the label. The
total noise is N0 x ENBW, ENBW = 3233.4 Hz, 0.325 dB above N0 x 3000 Hz.

The two engines agree statistically, not sample for sample: C++ draws Box-Muller
pairs from PCG32 and filters in float, Python draws NumPy PCG64 from
SeedSequence([seed, family, index]) and filters in float64.

filter_kernel() and bandlimit() keep the legacy band (adapted from
tools/pcm_stress.py: a 513-tap Blackman-windowed-sinc 300-3300 Hz band-pass,
L2-normalised), which the impulse stage's ring noise uses, as the C++ impulse
stage keeps signal-lab/src/band_fir.hpp.
"""

import functools
import math

import numpy as np

from . import FS
from .rng import FAMILY_BACKGROUND_NOISE, generator
from .stream import Impairment, number, seconds_to_frames

FFT_SIZE = 8192

# The legacy band (impulse ring noise; band_fir.hpp in C++).
FIR_TAPS = 513
BAND_HZ = (300.0, 3300.0)

# The lane's SNR band and its 3000 Hz reference bandwidth (110D 5.3.2.5; E.5.5).
SNR_BAND_HZ = (300.0, 3300.0)
SNR_REFERENCE_BANDWIDTH_HZ = SNR_BAND_HZ[1] - SNR_BAND_HZ[0]
# The noise band's transitions, 300 Hz wide and centred 150 Hz outside the band
# edges, its stopband, and its unit-gain frequency (E.7.1's reference), as awgn.hpp.
NOISE_BAND_TRANSITION_HZ = 300.0
NOISE_BAND_ATTENUATION_DB = 60.0
NOISE_BAND_REFERENCE_HZ = 1800.0
# Kaiser (1974), signal-lab/include/signal_lab/kaiser.hpp: beta = 0.1102 (A - 8.7)
# for A > 50 dB, and N - 1 = (A - 7.95) / (2.285 dw), dw the transition in rad/sample.
KAISER_BETA_SLOPE = 0.1102
KAISER_BETA_OFFSET_DB = 8.7
KAISER_LENGTH_OFFSET_DB = 7.95
KAISER_LENGTH_SLOPE = 2.285
# The SNR range the scenario accepts, as awgn_max_snr, and the stage limit, as
# max_awgn_stages: representative effect chains use one AWGN stage; one is spare.
MAX_SNR_DB = 120.0
MAX_AWGN_STAGES = 2


def kaiser_beta(attenuation_db):
    return KAISER_BETA_SLOPE * (attenuation_db - KAISER_BETA_OFFSET_DB)


def kaiser_length(attenuation_db, transition_hz, rate_hz):
    transition = 2.0 * math.pi * transition_hz / rate_hz
    return math.ceil((attenuation_db - KAISER_LENGTH_OFFSET_DB) / (KAISER_LENGTH_SLOPE * transition)) + 1


# Rounded up to odd, so the kernel is symmetric about one centre tap: 583 at 48 kHz.
NOISE_BAND_TAPS = kaiser_length(NOISE_BAND_ATTENUATION_DB, NOISE_BAND_TRANSITION_HZ, FS) | 1


def filter_kernel():
    """The legacy 513-tap Blackman band, L2-normalised (impulse ring noise)."""
    m = np.arange(FIR_TAPS, dtype=np.float64) - (FIR_TAPS - 1) / 2
    kernel = (2 * BAND_HZ[1] / FS * np.sinc(2 * BAND_HZ[1] / FS * m) -
              2 * BAND_HZ[0] / FS * np.sinc(2 * BAND_HZ[0] / FS * m)) * np.blackman(FIR_TAPS)
    return kernel / math.sqrt(float(np.dot(kernel, kernel)))


@functools.lru_cache(maxsize=1)
def _noise_band():
    centre = (NOISE_BAND_TAPS - 1) // 2
    offset = np.arange(NOISE_BAND_TAPS, dtype=np.float64) - centre
    low = 2.0 * (SNR_BAND_HZ[0] - NOISE_BAND_TRANSITION_HZ / 2.0) / FS
    high = 2.0 * (SNR_BAND_HZ[1] + NOISE_BAND_TRANSITION_HZ / 2.0) / FS
    ideal = high * np.sinc(high * offset) - low * np.sinc(low * offset)
    kernel = ideal * np.kaiser(NOISE_BAND_TAPS, kaiser_beta(NOISE_BAND_ATTENUATION_DB))
    # The kernel is symmetric, so its response at the reference frequency is real.
    gain = float(np.dot(kernel, np.cos(2.0 * math.pi * NOISE_BAND_REFERENCE_HZ / FS * offset)))
    kernel = kernel / gain
    kernel.flags.writeable = False
    return kernel


def noise_band_kernel():
    """The AWGN noise band h: 583 taps, unit gain at 1800 Hz (a copy)."""
    return _noise_band().copy()


def noise_band_enbw_hz():
    """(fs / 2) sum h^2 of the noise band: 3233.4 Hz."""
    kernel = _noise_band()
    return FS / 2.0 * float(np.dot(kernel, kernel))


def noise_sigma(reference_rms, snr_db):
    """sigma_w for a reference RMS and an SNR by N0 x 3000 Hz (N0 = 2 sigma_w^2 / fs)."""
    return reference_rms * 10.0 ** (-snr_db / 20.0) * math.sqrt(FS / (2.0 * SNR_REFERENCE_BANDWIDTH_HZ))


class BandNoise:
    """Stationary FIR-filtered Gaussian noise: unit-variance excitation through `kernel`."""

    def __init__(self, rng, kernel, stationary=True):
        self.rng = rng
        self.taps = len(kernel)
        if self.taps > FFT_SIZE // 2:
            raise ValueError("kernel too long for the overlap-save filter")
        self.history = rng.standard_normal(self.taps - 1) if stationary else np.zeros(self.taps - 1)
        self.spectrum = np.fft.rfft(kernel, FFT_SIZE)
        self.chunk = FFT_SIZE - (self.taps - 1)

    def next(self, count):
        return self.filter(self.rng.standard_normal(count))

    def filter(self, excitation):
        parts = [self._filter_chunk(excitation[first:first + self.chunk])
                 for first in range(0, len(excitation), self.chunk)]
        return np.concatenate(parts) if parts else np.zeros(0)

    def _filter_chunk(self, excitation):
        count = len(excitation)
        data = np.concatenate((self.history, excitation))
        filtered = np.fft.irfft(np.fft.rfft(data, FFT_SIZE) * self.spectrum, FFT_SIZE)
        self.history = data[-(self.taps - 1):].copy()
        return filtered[self.taps - 1:self.taps - 1 + count].copy()


def bandlimit(excitation):
    """One-shot legacy band-limiting of a finite excitation (zero initial history)."""
    kernel = filter_kernel()
    return np.convolve(excitation, kernel)[FIR_TAPS // 2:FIR_TAPS // 2 + len(excitation)]


class Awgn(Impairment):
    """params: snr_db (required), start_seconds (0), duration_seconds (None = to end)."""

    type_name = "awgn"

    def prepare(self, reference_rms, total_frames, seed, index):
        super().prepare(reference_rms, total_frames, seed, index)
        self.snr_db = number(self.params["snr_db"], "awgn.snr_db", -MAX_SNR_DB, MAX_SNR_DB)
        self.start = seconds_to_frames(self.params.get("start_seconds", 0.0), "awgn.start_seconds")
        duration = self.params.get("duration_seconds")
        self.end = total_frames if duration is None else min(total_frames, self.start + seconds_to_frames(duration, "awgn.duration_seconds", True))
        self.scale = noise_sigma(self.reference_rms, self.snr_db)
        rng_index = self.rng_index if self.stream_id is not None else self.instance_index
        self.noise = BandNoise(generator(self.rng_seed, FAMILY_BACKGROUND_NOISE, rng_index), _noise_band(), stationary=True)

    def process(self, block, first_frame):
        count = len(block)
        noise = self.noise.next(count) * self.scale
        if self.start > first_frame or self.end < first_frame + count:
            absolute = np.arange(first_frame, first_frame + count)
            noise[(absolute < self.start) | (absolute >= self.end)] = 0.0
        self.account(noise)
        return block + noise

    def resolved(self):
        enbw = noise_band_enbw_hz()
        reference_db = 20 * math.log10(self.reference_rms) if self.reference_rms > 0 else None
        return {"type": self.type_name, "snr_db": self.snr_db, "start_frame": self.start, "end_frame": self.end,
                "n0_x_3000_dbfs": reference_db - self.snr_db if reference_db is not None else None,
                "noise_target_rms_dbfs": (reference_db - self.snr_db + 10 * math.log10(enbw / SNR_REFERENCE_BANDWIDTH_HZ)
                                          if reference_db is not None else None),
                "band_hz": list(SNR_BAND_HZ), "fir_taps": NOISE_BAND_TAPS, "enbw_hz": enbw,
                "rng_family": FAMILY_BACKGROUND_NOISE,
                "rng_index": self.rng_index if self.stream_id is not None else self.instance_index,
                "convention": "N0 x 3000 Hz = reference RMS^2 10^(-snr_db/10) in 300-3300 Hz (110D E.5.5); "
                              "noise = sigma_w (h * e), sigma_w = reference RMS 10^(-snr_db/20) sqrt(fs / 6000 Hz), "
                              "h a 583-tap Kaiser band with unit gain at 1800 Hz; total noise N0 x ENBW"}
