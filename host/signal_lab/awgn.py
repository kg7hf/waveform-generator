"""Band-limited additive white Gaussian noise at a signal-relative SNR.

Adapted from tools/pcm_stress.py (BandNoise / filter_kernel): a 513-tap
Blackman-windowed-sinc 300-3300 Hz band-pass, L2-normalised so unit-variance
white excitation yields unit expected output variance, applied by 8192-point
overlap-save FFT.  SNR is 20*log10(reference RMS / noise target RMS), i.e. the
noise power is counted in the ~3 kHz voice band, matching the MIL-STD /
PathSim "SNR in 3 kHz" convention used for the project's corner table.
"""

import math

import numpy as np

from . import FS
from .rng import FAMILY_BACKGROUND_NOISE, generator
from .stream import Impairment, number, seconds_to_frames

FIR_TAPS = 513
FFT_SIZE = 8192
BAND_HZ = (300.0, 3300.0)


def filter_kernel():
    m = np.arange(FIR_TAPS, dtype=np.float64) - (FIR_TAPS - 1) / 2
    kernel = (2 * BAND_HZ[1] / FS * np.sinc(2 * BAND_HZ[1] / FS * m) -
              2 * BAND_HZ[0] / FS * np.sinc(2 * BAND_HZ[0] / FS * m)) * np.blackman(FIR_TAPS)
    return kernel / math.sqrt(float(np.dot(kernel, kernel)))


class BandNoise:
    """Stationary FIR-filtered Gaussian noise; unit expected output variance."""

    def __init__(self, rng, stationary=True):
        self.rng = rng
        self.history = rng.standard_normal(FIR_TAPS - 1) if stationary else np.zeros(FIR_TAPS - 1)
        self.spectrum = np.fft.rfft(filter_kernel(), FFT_SIZE)

    def next(self, count):
        return self.filter(self.rng.standard_normal(count))

    def filter(self, excitation):
        count = len(excitation)
        if count + FIR_TAPS - 1 > FFT_SIZE:
            raise ValueError("block too long for the overlap-save filter")
        data = np.concatenate((self.history, excitation))
        filtered = np.fft.irfft(np.fft.rfft(data, FFT_SIZE) * self.spectrum, FFT_SIZE)
        self.history = data[-(FIR_TAPS - 1):].copy()
        return filtered[FIR_TAPS - 1:FIR_TAPS - 1 + count].copy()


def bandlimit(excitation):
    """One-shot band-limiting of a finite excitation (zero initial history), same kernel."""
    kernel = filter_kernel()
    return np.convolve(excitation, kernel)[FIR_TAPS // 2:FIR_TAPS // 2 + len(excitation)]


class Awgn(Impairment):
    """params: snr_db (required), start_seconds (0), duration_seconds (None = to end)."""

    type_name = "awgn"

    def prepare(self, reference_rms, total_frames, seed, index):
        super().prepare(reference_rms, total_frames, seed, index)
        self.snr_db = number(self.params["snr_db"], "awgn.snr_db", -120, 120)
        self.start = seconds_to_frames(self.params.get("start_seconds", 0.0), "awgn.start_seconds")
        duration = self.params.get("duration_seconds")
        self.end = total_frames if duration is None else min(total_frames, self.start + seconds_to_frames(duration, "awgn.duration_seconds", True))
        self.scale = self.reference_rms * 10.0 ** (-self.snr_db / 20.0)
        self.noise = BandNoise(generator(seed, FAMILY_BACKGROUND_NOISE, 0), stationary=True)

    def process(self, block, first_frame):
        count = len(block)
        noise = self.noise.next(count) * self.scale
        if self.start > first_frame or self.end < first_frame + count:
            absolute = np.arange(first_frame, first_frame + count)
            noise[(absolute < self.start) | (absolute >= self.end)] = 0.0
        self.account(noise)
        return block + noise

    def resolved(self):
        return {"type": self.type_name, "snr_db": self.snr_db, "start_frame": self.start, "end_frame": self.end,
                "noise_target_rms_dbfs": 20 * math.log10(self.scale) if self.scale > 0 else None,
                "band_hz": list(BAND_HZ), "fir_taps": FIR_TAPS, "rng_family": FAMILY_BACKGROUND_NOISE,
                "convention": "snr_db = 20 log10(reference RMS / stationary band-limited noise target RMS); noise power counted in the 300-3300 Hz band"}
