# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Analyses of the Watterson validation battery that the lane's analyzers do not cover.

Guide: docs/design/watterson-validation.md. Called by run_watterson_validation.py; numpy only.

The lane's probes/analyze_*.py measure delivered SNR, Doppler spread, Rayleigh envelope and tap
delays. This module adds what design 10.2 asks beyond them:

- image_check        the tap-rate images k R on the 48 kHz output itself (B3: analyze_doppler cannot
                     see them above about 2.1 Hz spread), the out-of-lobe power beyond +/-3 d (Q1) and
                     the sidelobe continuum; on a frozen-tap render, the floor of the same windows
- noise_stats        flatness, ENBW, N0 x 3000 Hz, kurtosis and tails of a noise-only render (B6a)
- fold_harmonics     synchronous (period-folded) averages for combs keyed to a block period (B6b)
- line_search        lines other than a tone in 300-3300 Hz, with explicit comb candidates (B6c, and
                     B6d beside a shifted tone, for the Watterson stage's own frame grids)
- tone_frequency     frequency of a shifted tone by its phase slope, and the mirror level (B9)
- truth_*            channel truth (TapObserver records): per-path node moments, Rayleigh KS,
                     correlation between paths, per-path Doppler through the lane's analyze_baseband
- rician_k           the K-factor of a complex gain from its moments
- static_comb        the analytic |H(f)|^2 of fixed paths at analyze_delay's bins (B4)
- rank_sum_exact     the exact two-sided Mann-Whitney test of design 10.4 (criterion 5, after decoding)
- ber_crossing       where a pooled BER curve crosses 1e-3, and snr_offset, the equivalent-SNR offset
                     between two such curves (lane call 3, criterion 5's threshold set)

Every constant names its source. Levels in dBc are relative to the reference (tone or signal) power.
"""

from __future__ import annotations

import itertools
import math
from pathlib import Path

import numpy as np

FS_HZ = 48000.0
BAND_HZ = (300.0, 3300.0)                 # the lane's 3 kHz band (110D 5.3.2.5; lane criterion 1)
SNR_REFERENCE_BANDWIDTH_HZ = 3000.0       # N0 x 3000 Hz (E.5.5; lane criterion 1)
NOISE_CENTRE_HZ = (1500.0, 2100.0)        # N0 reference region around the kernel's unit-gain 1800 Hz (awgn.hpp)
FALSE_ALARM_PER_TEST = 1e-6               # design 10.2: line floors at a false alarm of 1e-6 per bin or harmonic
Z_ONE_IN_A_MILLION = 4.753424             # standard normal upper quantile at 1e-6 (one-sided)
TRUTH_MAGIC = b'WFGTAPS1'                 # signal-lab/validation/watterson_render.cpp
TRUTH_DTYPE = np.dtype([('frame', '<u8'), ('path', '<u4'), ('re', '<f4'), ('im', '<f4')])
PHASE_TURN = 4294967296.0                 # 2^32: the stage's phase accumulator (units/conversions.hpp)
RAYLEIGH_MEAN_OVER_RMS = math.sqrt(math.pi) / 2.0
KS_95 = 1.36                              # Kolmogorov 95 % point, as the lane's envelope_stats uses
CONTINUUM_FROM_R, CONTINUUM_TO_R = 0.3, 0.7  # image_check's continuum band, in units of the node rate R


def db10(value: float) -> float | None:
    return 10.0 * math.log10(value) if value > 0 else None


def two_sided_gaussian_tail(k: float) -> float:
    return math.erfc(k / math.sqrt(2.0))


def chi2_ratio_quantile(dof: float, z: float = Z_ONE_IN_A_MILLION) -> float:
    """Upper quantile of chi^2_dof / dof by Wilson-Hilferty (accurate to < 1 % for dof > 30)."""
    a = 2.0 / (9.0 * dof)
    return (1.0 - a + z * math.sqrt(a)) ** 3


def _validated_band(band_hz: tuple[float, float]) -> tuple[float, float]:
    """Return a finite, ordered analysis band inside the sampled Nyquist interval."""
    try:
        if len(band_hz) != 2:
            raise ValueError
        raw_low, raw_high = band_hz[0], band_hz[1]
    except (TypeError, ValueError, IndexError) as error:
        raise ValueError('band_hz must contain two finite frequencies') from error
    numeric = (int, float, np.integer, np.floating)
    if (isinstance(raw_low, (bool, np.bool_)) or not isinstance(raw_low, numeric) or
            isinstance(raw_high, (bool, np.bool_)) or not isinstance(raw_high, numeric)):
        raise ValueError('band_hz frequencies must be numeric')
    low, high = float(raw_low), float(raw_high)
    if not (math.isfinite(low) and math.isfinite(high) and 0.0 <= low < high <= FS_HZ / 2.0):
        raise ValueError(f'band_hz must satisfy 0 <= low < high <= {FS_HZ / 2.0:g} Hz')
    return low, high


def _multiple_comparison_summary(tests: int) -> dict:
    """Per-bin false-alarm setting and the union-bound family-wise probability."""
    return {'false_alarm_probability_per_bin': FALSE_ALARM_PER_TEST,
            'tested_bins': tests,
            'family_wise_false_alarm_upper_bound': min(1.0, tests * FALSE_ALARM_PER_TEST)}


# ---------------------------------------------------------------------------------------------- WAV input

def wav_layout(path: Path) -> tuple[int, int, int, int]:
    """(data offset, frames, bits, format tag) of a mono WAV, by walking its chunks."""
    with open(path, 'rb') as handle:
        head = handle.read(12)
        if head[:4] != b'RIFF' or head[8:12] != b'WAVE':
            raise ValueError(f'{path}: not RIFF/WAVE')
        bits = tag = channels = None
        while True:
            chunk = handle.read(8)
            if len(chunk) < 8:
                raise ValueError(f'{path}: no data chunk')
            cid, size = chunk[:4], int.from_bytes(chunk[4:], 'little')
            if cid == b'fmt ':
                body = handle.read(size + (size & 1))
                tag, channels = int.from_bytes(body[0:2], 'little'), int.from_bytes(body[2:4], 'little')
                bits = int.from_bytes(body[14:16], 'little')
                if tag == 0xFFFE and len(body) >= 26:
                    tag = int.from_bytes(body[24:26], 'little')
            elif cid == b'data':
                if channels != 1:
                    raise ValueError(f'{path}: mono expected')
                return handle.tell(), size // (bits // 8), bits, tag


def read_frames(path: Path, start: int, count: int) -> np.ndarray:
    """Frames [start, start + count) of a mono PCM16/PCM24/float32 WAV as float64 (full scale 1.0)."""
    offset, frames, bits, tag = wav_layout(path)
    start = max(0, start)
    count = max(0, min(count, frames - start))
    width = bits // 8
    raw = np.memmap(path, dtype=np.uint8, mode='r', offset=offset + start * width, shape=(count * width,))
    if bits == 16:
        return np.frombuffer(np.asarray(raw), dtype='<i2').astype(np.float64) / 32768.0
    if bits == 24:
        b = np.asarray(raw).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        return v.astype(np.float64) / 8388608.0
    if bits == 32 and tag == 3:
        return np.frombuffer(np.asarray(raw), dtype='<f4').astype(np.float64)
    raise ValueError(f'{path}: unsupported {bits}-bit format {tag}')


def frame_count(path: Path) -> int:
    return wav_layout(path)[1]


# ---------------------------------------------------------------------------------------------- spectra

def kaiser_window(n: int, beta: float) -> np.ndarray:
    return np.kaiser(n + 1, beta)[:-1]


def welch_file(path: Path, start: int, count: int, nperseg: int, beta: float = 14.0,
               overlap: float = 0.5) -> tuple[np.ndarray, np.ndarray, dict]:
    """One-sided Welch PSD (power/Hz) of frames [start, start + count), one segment in memory at a time."""
    nperseg = int(min(nperseg, count))
    step = max(1, int(round(nperseg * (1.0 - overlap))))
    w = kaiser_window(nperseg, beta)
    scale = FS_HZ * float(np.sum(w * w))
    acc = np.zeros(nperseg // 2 + 1)
    segments = 0
    for s in range(start, start + count - nperseg + 1, step):
        seg = read_frames(path, s, nperseg)
        acc += np.abs(np.fft.rfft(seg * w)) ** 2
        segments += 1
    p = acc / (segments * scale)
    p[1:] *= 2.0
    if nperseg % 2 == 0:
        p[-1] /= 2.0
    f = np.fft.rfftfreq(nperseg, 1.0 / FS_HZ)
    enbw_bins = nperseg * float(np.sum(w * w)) / float(np.sum(w)) ** 2
    return f, p, {'segments': segments, 'segment_s': nperseg / FS_HZ, 'bin_hz': FS_HZ / nperseg, 'window': f'kaiser{beta:g}',
                  'enbw_hz': enbw_bins * FS_HZ / nperseg}


def band_power(f: np.ndarray, p: np.ndarray, centre: float, half_width: float) -> float:
    df = f[1] - f[0]
    sel = np.abs(f - centre) <= half_width
    return float(np.sum(p[sel]) * df)


def image_check(path: Path, start: int, count: int, tone_hz: float, spread_hz: float, node_rate_hz: float,
                orders: int = 3) -> dict:
    """Design 10.2, B3's direct image check on the 48 kHz output: the lobe power within +/-4 sigma of the
    tone and each image power within +/-4 sigma of tone +/- k R, k = 1..orders, from a Kaiser (beta 14)
    Welch PSD with bins of at most sigma / 10. Also the power beyond +/-3 d up to R / 2 (Q1's
    integrated reading) and the sidelobe continuum: the median PSD between 0.3 R and 0.7 R from the
    tone, scaled to a window's width. On a fading render that continuum is the 1 % truncation
    sidelobes after the interpolator, at about the images' own level; it is not a measurement floor.
    The floor is this same check on a frozen-tap render (a fixed path, the same probe and windows),
    which the runner makes beside each row."""
    sigma = spread_hz / 2.0
    bin_hz = sigma / 10.0
    nperseg = int(math.ceil(FS_HZ / bin_hz))
    nperseg = min(nperseg, count // 4)
    f, p, info = welch_file(path, start, count, nperseg)
    df = info['bin_hz']
    margin = 3.0 * df  # half the Kaiser-14 main lobe in bins is about 2.3; keep the whole lobe in
    window = 4.0 * sigma + margin
    lobe = band_power(f, p, tone_hz, window)
    images = []
    for k in range(1, orders + 1):
        for sign in (-1, 1):
            centre = tone_hz + sign * k * node_rate_hz
            if 0.0 < centre < FS_HZ / 2:
                level = band_power(f, p, centre, window)
                images.append({'order': k, 'sign': sign, 'centre_hz': centre, 'level_dbc': db10(level / lobe) if level > 0 else -300.0})
    # Q1: the power outside +/-3 d (6 sigma) but within R / 2 of the tone, which excludes the images.
    near = (np.abs(f - tone_hz) > 3.0 * spread_hz + margin) & (np.abs(f - tone_hz) <= node_rate_hz / 2.0)
    outside = float(np.sum(p[near]) * df)
    # The sidelobe continuum between the lobe and the first image (0.3 R to 0.7 R from the tone), as the
    # power of one +/-4 sigma window there: the truncation sidelobes on a fading render.
    between = (np.abs(f - tone_hz) > node_rate_hz * CONTINUUM_FROM_R) & (np.abs(f - tone_hz) < node_rate_hz * CONTINUUM_TO_R)
    continuum = float(np.median(p[between]) * 2.0 * window) if np.any(between) else 0.0
    return {'psd': info, 'lobe_power': lobe, 'window_hz': window, 'node_rate_hz': node_rate_hz, 'images': images,
            'worst_image_dbc': max((i['level_dbc'] for i in images), default=None),
            'out_of_lobe_beyond_3d_dbc': db10(outside / lobe) if outside > 0 else None,
            'sidelobe_continuum_dbc': db10(continuum / lobe) if continuum > 0 else None}


# ---------------------------------------------------------------------------------------------- noise

def noise_stats(path: Path, start: int, count: int, reference_rms: float, block_s: float = 1.0) -> dict:
    """B6a on a noise-only render: Welch PSD (Hann 1 s and 10 s), flatness in 100 Hz and 10 Hz bands,
    N0 at the centre, ENBW, the delivered SNR by N0 x 3000 Hz against the reference, and Gaussianity
    (skewness, excess kurtosis, tails at 3/4/5 sigma) with block-means standard errors."""
    block = int(block_s * FS_HZ)
    nblocks = count // block
    mom = np.zeros((nblocks, 4))
    tails = np.zeros((nblocks, 3))
    for b in range(nblocks):
        x = read_frames(path, start + b * block, block)
        mom[b] = [np.mean(x), np.mean(x * x), np.mean(x ** 3), np.mean(x ** 4)]
    m1, m2, m3, m4 = (float(v) for v in np.mean(mom, axis=0))
    var = m2 - m1 * m1
    sigma = math.sqrt(var)
    for b in range(nblocks):
        x = read_frames(path, start + b * block, block) - m1
        tails[b] = [np.mean(np.abs(x) > k * sigma) for k in (3.0, 4.0, 5.0)]
    c3 = m3 - 3 * m1 * m2 + 2 * m1 ** 3
    c4 = m4 - 4 * m1 * m3 + 6 * m1 * m1 * m2 - 3 * m1 ** 4
    skew = c3 / var ** 1.5
    kurt = c4 / var ** 2 - 3.0
    block_kurt = (mom[:, 3] - 4 * mom[:, 0] * mom[:, 2] + 6 * mom[:, 0] ** 2 * mom[:, 1] - 3 * mom[:, 0] ** 4) / (mom[:, 1] - mom[:, 0] ** 2) ** 2 - 3.0
    block_skew = (mom[:, 2] - 3 * mom[:, 0] * mom[:, 1] + 2 * mom[:, 0] ** 3) / (mom[:, 1] - mom[:, 0] ** 2) ** 1.5
    kurt_se = float(np.std(block_kurt, ddof=1) / math.sqrt(nblocks))
    skew_se = float(np.std(block_skew, ddof=1) / math.sqrt(nblocks))
    tail_rows = []
    for i, k in enumerate((3.0, 4.0, 5.0)):
        measured = float(np.mean(tails[:, i]))
        se = float(np.std(tails[:, i], ddof=1) / math.sqrt(nblocks))
        expected = two_sided_gaussian_tail(k)
        tail_rows.append({'k_sigma': k, 'measured': measured, 'gaussian': expected, 'standard_error': se,
                          'z': (measured - expected) / se if se > 0 else None})

    f1, p1, info1 = welch_hann(path, start, count, int(FS_HZ * 1.0))
    centre = (f1 >= NOISE_CENTRE_HZ[0]) & (f1 <= NOISE_CENTRE_HZ[1])
    n0 = float(np.mean(p1[centre]))
    inband = (f1 >= BAND_HZ[0]) & (f1 <= BAND_HZ[1])
    band_mean = float(np.mean(p1[inband]))
    rows100 = []
    for lo in range(int(BAND_HZ[0]), int(BAND_HZ[1]), 100):
        sel = (f1 >= lo) & (f1 < lo + 100) if lo + 100 < BAND_HZ[1] else (f1 >= lo) & (f1 <= lo + 100)
        rows100.append({'band_hz': [lo, lo + 100], 'db_rel_band_mean': db10(float(np.mean(p1[sel])) / band_mean)})
    dev10 = []
    for lo in np.arange(BAND_HZ[0], BAND_HZ[1], 10.0):
        sel = (f1 >= lo) & (f1 < lo + 10.0)
        dev10.append(db10(float(np.mean(p1[sel])) / band_mean))
    dof10 = info1['dof_segments'] * 10.0
    se10_db = 10.0 / math.log(10.0) / math.sqrt(dof10)
    total = var
    return {'interval_frames': [start, start + count], 'blocks': nblocks, 'block_s': block_s,
            'rms': sigma, 'mean': m1, 'skewness': skew, 'skewness_se': skew_se,
            'excess_kurtosis': kurt, 'excess_kurtosis_se': kurt_se,
            'excess_kurtosis_z': kurt / kurt_se if kurt_se > 0 else None, 'tails': tail_rows,
            'psd_hann_1s': {'segments': info1['segments'], 'dof_segments': info1['dof_segments'], 'bin_hz': info1['bin_hz']},
            'n0_per_hz': n0, 'n0_region_hz': list(NOISE_CENTRE_HZ),
            'enbw_measured_hz': total / n0,
            'delivered_snr_db_n0x3000': db10(reference_rms ** 2 / (n0 * SNR_REFERENCE_BANDWIDTH_HZ)),
            'band_integral_snr_db': db10(reference_rms ** 2 / (band_mean * (BAND_HZ[1] - BAND_HZ[0]))),
            'flatness_100hz_rows': rows100,
            'flatness_100hz_max_abs_db': max(abs(r['db_rel_band_mean']) for r in rows100),
            'flatness_10hz_max_abs_db': max(abs(d) for d in dev10),
            'flatness_10hz_standard_error_db': se10_db}


def welch_hann(path: Path, start: int, count: int, nperseg: int, overlap: float = 0.5) -> tuple[np.ndarray, np.ndarray, dict]:
    """One-sided Hann Welch PSD, segment by segment, with the lane's overlap-corrected dof."""
    step = max(1, int(round(nperseg * (1.0 - overlap))))
    n = np.arange(nperseg)
    w = 0.5 - 0.5 * np.cos(2 * np.pi * n / nperseg)
    scale = FS_HZ * float(np.sum(w * w))
    acc = np.zeros(nperseg // 2 + 1)
    starts = list(range(start, start + count - nperseg + 1, step))
    batch = max(1, (1 << 24) // nperseg)
    for i in range(0, len(starts), batch):
        group = starts[i:i + batch]
        first, last = group[0], group[-1] + nperseg
        x = read_frames(path, first, last - first)
        for s in group:
            seg = x[s - first:s - first + nperseg]
            acc += np.abs(np.fft.rfft(seg * w)) ** 2
    p = acc / (len(starts) * scale)
    p[1:] *= 2.0
    if nperseg % 2 == 0:
        p[-1] /= 2.0
    r1 = float(np.sum(w[step:] * w[:-step]) ** 2 / np.sum(w * w) ** 2) if step < nperseg else 0.0
    dof = len(starts) / (1 + 2 * r1 * (1 - 1.0 / len(starts)))
    enbw_bins = nperseg * float(np.sum(w * w)) / float(np.sum(w)) ** 2
    return np.fft.rfftfreq(nperseg, 1.0 / FS_HZ), p, {'segments': len(starts), 'dof_segments': dof, 'bin_hz': FS_HZ / nperseg,
                                                      'enbw_hz': enbw_bins * FS_HZ / nperseg, 'segment_s': nperseg / FS_HZ}


def line_search(path: Path, start: int, count: int, reference_power: float, exclude: list[tuple[float, float]],
                explicit_hz: list[float], segment_s: float = 10.0, *, band_hz: tuple[float, float] = BAND_HZ) -> dict:
    """Lines in band_hz from a Hann Welch PSD at 1/segment_s Hz: each bin against the median of
    its 100 Hz block; a bin is a line when its ratio exceeds the chi-square point of a 1e-6 false
    alarm per bin. Line power = (bin - median) x ENBW, in dBc re reference_power. Bins inside the
    exclusions (tone +/- a few bins) are skipped. explicit_hz lists the candidate comb frequencies,
    whose largest level is reported whether or not it is a detection. The returned family-wise bound
    is the union bound over tested bins; individual noise peaks are not automatically coherent spurs."""
    low_hz, high_hz = _validated_band(band_hz)
    if not math.isfinite(segment_s) or segment_s <= 0.0:
        raise ValueError('segment_s must be finite and positive')
    nperseg = int(round(segment_s * FS_HZ))
    if count < nperseg or nperseg < 2:
        raise ValueError('count must contain at least one complete Welch segment')
    f, p, info = welch_hann(path, start, count, nperseg)
    inband = (f >= low_hz) & (f <= high_hz)
    if not np.any(inband):
        raise ValueError('band_hz contains no FFT bins at the requested segment length')
    keep = inband.copy()
    for lo, hi in exclude:
        keep &= ~((f >= lo) & (f <= hi))
    if not np.any(keep):
        raise ValueError('no FFT bins remain after applying exclusions')
    median = np.zeros_like(p)
    for lo in np.arange(low_hz, high_hz, 100.0):
        final_band = lo + 100.0 >= high_hz
        upper_selection = (f <= high_hz) if final_band else (f < lo + 100.0)
        sel = (f >= lo) & upper_selection & keep
        if np.any(sel):
            bucket = (f >= lo) & upper_selection
            median[bucket] = np.median(p[sel])
    # The median of a chi^2_k/k variable is about (1 - 2/(9k))^3 of its mean.
    dof = 2.0 * info['dof_segments']
    mean_over_median = 1.0 / (1.0 - 2.0 / (9.0 * dof)) ** 3
    mean = median * mean_over_median
    threshold = chi2_ratio_quantile(dof)
    enbw = info['enbw_hz']
    ratio = np.where(keep & (mean > 0), p / np.where(mean > 0, mean, 1.0), 0.0)
    detected = np.flatnonzero(ratio > threshold)
    lines = []
    for i in detected[np.argsort(-ratio[detected])][:20]:
        lines.append({'frequency_hz': float(f[i]), 'ratio_db': db10(float(ratio[i])),
                      'level_dbc': db10(max(float(p[i] - mean[i]), 1e-300) * enbw / reference_power)})
    floor_dbc = db10(float(np.median(mean[keep])) * (threshold - 1.0) * enbw / reference_power)
    explicit = []
    for fx in explicit_hz:
        if low_hz <= fx <= high_hz:
            i = int(round(fx / info['bin_hz']))
            j = slice(max(0, i - 1), i + 2)
            k = int(np.argmax(p[j])) + max(0, i - 1)
            if keep[k]:
                excess = float(p[k] - mean[k])
                explicit.append({'frequency_hz': fx, 'ratio_db': db10(float(ratio[k])),
                                 'level_dbc': db10(excess * enbw / reference_power) if excess > 0 else None})
    tested = int(np.count_nonzero(keep & (mean > 0)))
    return {'psd': info, 'threshold_ratio_db': db10(threshold), 'floor_dbc': floor_dbc, 'detected': len(detected), 'lines': lines,
            **_multiple_comparison_summary(tested),
            'worst_line_dbc': max((ln['level_dbc'] for ln in lines), default=None),
            'explicit': explicit, 'worst_explicit_dbc': max((e['level_dbc'] for e in explicit if e['level_dbc'] is not None), default=None)}


def fold_harmonics(path: Path, start: int, count: int, reference_power: float, periods: tuple[int, ...],
                   derived: dict[int, int], inject_dbc: float | None = None, *,
                   band_hz: tuple[float, float] = BAND_HZ) -> dict:
    """B6b: synchronous averages of the frames at each period P (chunks aligned to lcm(periods), so one
    pass serves every period); derived = {Q: P} folds P's average again at a divisor Q. For each
    fold, the one-sided line power 2 |A_m|^2 / P^2 of every in-band harmonic m fs / P, in dBc re
    reference_power, its mean (the noise floor of the fold) and the 1e-6 false-alarm threshold
    (exponential, 2 dof). inject_dbc adds a known line at one in-band harmonic of the first period to
    a copy of its fold, the positive control of the detector. band_hz is inclusive; results report
    the per-harmonic false-alarm probability and the union-bound family-wise probability."""
    band_hz = _validated_band(band_hz)
    lcm = 1
    for period in periods:
        lcm = lcm * period // math.gcd(lcm, period)
    chunk = lcm * max(1, (1 << 23) // lcm)
    usable = (count // lcm) * lcm
    if usable == 0:
        raise ValueError('count must contain at least one complete harmonic-fold period')
    acc = {period: np.zeros(period) for period in periods}
    total = 0
    sums = np.zeros(4)
    tails = np.zeros(3)
    for s in range(0, usable, chunk):
        n = min(chunk, usable - s)
        x = read_frames(path, start + s, n)
        for period in periods:
            acc[period] += x.reshape(-1, period).sum(axis=0)
        sums += [np.sum(x), np.sum(x * x), np.sum(x ** 3), np.sum(x ** 4)]
        total += n
    m1, m2, m3, m4 = sums / total
    var = m2 - m1 * m1
    for s in range(0, usable, chunk):
        n = min(chunk, usable - s)
        x = read_frames(path, start + s, n) - m1
        tails += [np.count_nonzero(np.abs(x) > k * math.sqrt(var)) for k in (3.0, 4.0, 5.0)]
    folds = {period: acc[period] / (total // period) for period in periods}
    for q, p in derived.items():
        folds[q] = folds[p].reshape(-1, q).mean(axis=0)
    results = []
    control = None
    for period, average in sorted(folds.items(), key=lambda item: -item[0]):
        row = harmonic_lines(average, period, reference_power, total // period, band_hz=band_hz)
        results.append(row)
        if inject_dbc is not None and period == periods[0]:
            m = row['inband_harmonics'][len(row['inband_harmonics']) // 2]
            amplitude = math.sqrt(2.0 * reference_power * 10.0 ** (inject_dbc / 10.0))
            k = np.arange(period)
            injected = average + amplitude * np.cos(2 * np.pi * m * k / period)
            control_row = harmonic_lines(injected, period, reference_power, total // period, band_hz=band_hz)
            control = {'period': period, 'harmonic': m, 'frequency_hz': m * FS_HZ / period, 'injected_dbc': inject_dbc,
                       'measured_dbc': control_row['levels_dbc'][control_row['inband_harmonics'].index(m)],
                       'detected': control_row['levels_dbc'][control_row['inband_harmonics'].index(m)] > control_row['threshold_dbc']}
    c4 = m4 - 4 * m1 * m3 + 6 * m1 * m1 * m2 - 3 * m1 ** 4
    return {'frames_folded': total, 'folds': [{k: v for k, v in r.items() if k not in ('levels_dbc', 'inband_harmonics')} for r in results],
            'positive_control': control,
            'excess_kurtosis_5h': c4 / var ** 2 - 3.0,
            'tails_5h': [{'k_sigma': k, 'measured': float(t / total), 'gaussian': two_sided_gaussian_tail(k)} for k, t in zip((3.0, 4.0, 5.0), tails)]}


def harmonic_lines(average: np.ndarray, period: int, reference_power: float, repetitions: int, *,
                   band_hz: tuple[float, float] = BAND_HZ) -> dict:
    band_hz = _validated_band(band_hz)
    spectrum = np.fft.rfft(average - np.mean(average))
    m = np.arange(len(spectrum))
    freq = m * FS_HZ / period
    sel = (freq >= band_hz[0]) & (freq <= band_hz[1]) & (m > 0) & (m < period / 2)
    if not np.any(sel):
        raise ValueError('band_hz contains no non-DC, non-Nyquist harmonics for this period')
    power = 2.0 * np.abs(spectrum[sel]) ** 2 / period ** 2
    levels = [db10(float(v) / reference_power) if v > 0 else -300.0 for v in power]
    mean = float(np.mean(power)) if len(power) else 0.0
    threshold = mean * math.log(1.0 / FALSE_ALARM_PER_TEST) if mean > 0 else 0.0
    worst = int(np.argmax(power)) if len(power) else None
    tested = int(np.count_nonzero(sel))
    return {'period': period, 'repetitions': repetitions, 'harmonic_spacing_hz': FS_HZ / period,
            'inband_count': tested, **_multiple_comparison_summary(tested),
            'mean_harmonic_dbc': db10(mean / reference_power) if mean > 0 else None,
            'threshold_dbc': db10(threshold / reference_power) if threshold > 0 else None,
            'worst_dbc': levels[worst] if worst is not None else None,
            'worst_frequency_hz': float(freq[sel][worst]) if worst is not None else None,
            'above_threshold': int(np.count_nonzero(power > threshold)),
            'levels_dbc': levels, 'inband_harmonics': [int(v) for v in m[sel]]}


# ---------------------------------------------------------------------------------------------- tones

def tone_frequency(path: Path, start: int, count: int, nominal_hz: float, mirror_hz: float | None) -> dict:
    """The frequency of a (shifted) tone from the slope of its phase: mix down at nominal_hz (a whole
    number of hertz, so the mixer's phase repeats every second), average 480-frame blocks (100 Hz),
    unwrap, fit a line. And the mirror level (a Kaiser Welch PSD, +/-2 Hz around each) in dBc."""
    if nominal_hz != round(nominal_hz):
        raise ValueError('nominal_hz must be a whole number of hertz')
    block = 480
    nblocks = count // block
    step = block * 8192
    z = np.zeros(nblocks, dtype=complex)
    for s in range(0, nblocks * block, step):
        e = min(nblocks * block, s + step)
        x = read_frames(path, start + s, e - s)
        k = np.arange(s, e)
        mixed = x * np.exp(-2j * np.pi * nominal_hz * ((k % 48000) / FS_HZ))
        z[s // block:e // block] = mixed.reshape(-1, block).mean(axis=1)
    phase = np.unwrap(np.angle(z))
    t = (np.arange(nblocks) * block + block / 2.0) / FS_HZ
    slope, _ = np.polyfit(t, phase, 1)
    measured = nominal_hz + slope / (2 * np.pi)
    result = {'nominal_hz': nominal_hz, 'measured_hz': measured, 'offset_from_nominal_hz': measured - nominal_hz,
              'blocks': int(nblocks)}
    if mirror_hz is not None:
        f, p, info = welch_file(path, start, count, int(FS_HZ * 1.0))
        tone = band_power(f, p, nominal_hz, 2.0)
        mirror = band_power(f, p, mirror_hz, 2.0)
        result.update({'mirror_hz': mirror_hz, 'mirror_dbc': db10(mirror / tone) if mirror > 0 else -300.0, 'psd': info})
    return result


def tone_phase(path: Path, start: int, count: int, tone_hz: float) -> complex:
    """The complex amplitude of a tone (a whole number of hertz) over frames [start, start + count),
    referred to frame 0."""
    if tone_hz != round(tone_hz):
        raise ValueError('tone_hz must be a whole number of hertz')
    acc = 0j
    step = 1 << 22
    for s in range(0, count, step):
        e = min(count, s + step)
        x = read_frames(path, start + s, e - s)
        k = np.arange(start + s, start + e)
        acc += np.sum(x * np.exp(-2j * np.pi * tone_hz * ((k % 48000) / FS_HZ)))
    return 2.0 * acc / count


# ---------------------------------------------------------------------------------------------- truth

def read_truth(path: Path) -> dict[int, tuple[np.ndarray, np.ndarray]]:
    """Per path: (node frames, complex node values) from a watterson_render --truth-out file."""
    data = np.fromfile(path, dtype=np.uint8)
    if bytes(data[:8]) != TRUTH_MAGIC:
        raise ValueError(f'{path}: not a WFGTAPS1 file')
    records = np.frombuffer(data[8:].tobytes(), dtype=TRUTH_DTYPE)
    out = {}
    for path_index in np.unique(records['path']):
        sel = records[records['path'] == path_index]
        order = np.argsort(sel['frame'], kind='stable')
        sel = sel[order]
        out[int(path_index)] = (sel['frame'].astype(np.int64), sel['re'].astype(np.float64) + 1j * sel['im'].astype(np.float64))
    return out


def rebuild_gain(frames: np.ndarray, nodes: np.ndarray, report: dict, at: np.ndarray) -> np.ndarray:
    """g_i(n) at the frames 'at', rebuilt from the nodes by design 5.4 (linear interpolation) and the
    path's rotator (design 4.5), in double precision: h(n) = h[m] + (j / M)(h[m + 1] - h[m]). The
    record frames must be the node grid (node m at frame m M; a fixed path's one record at 0)."""
    if report['fading'] == 'fixed':
        if len(frames) != 1 or frames[0] != 0:
            raise ValueError('a fixed path reports one record, at frame 0')
        gain = np.full(len(at), nodes[0])
    else:
        m_frames = int(report['frames_per_node'])
        if not np.array_equal(frames, np.arange(len(frames), dtype=np.int64) * m_frames):
            raise ValueError('the node records are not on the m M grid')
        m = at // m_frames
        j = at - m * m_frames
        if np.any(m + 1 >= len(nodes)):
            raise ValueError('frames beyond the last node')
        gain = nodes[m] + (j / m_frames) * (nodes[m + 1] - nodes[m])
    step = int(report['phase_step'])
    if step:
        phase = ((at.astype(np.uint64) * np.uint64(step)) & np.uint64(0xFFFFFFFF)).astype(np.float64)
        gain = gain * np.exp(2j * np.pi * phase / PHASE_TURN)
    return gain


def node_statistics(nodes: np.ndarray, amplitude: float, spread_hz: float, node_rate_hz: float) -> dict:
    """Rayleigh checks on one path's nodes (design U14 on a render): E|h|^2 / a^2, E|h|^4 / E^2,
    mean/RMS of |h|, KS of |h|^2 / E|h|^2 against Exp(1), each with its standard error from the
    number of independent samples N_eff = sqrt(pi) d T (the lane's envelope_stats rule)."""
    u = np.abs(nodes) ** 2 / amplitude ** 2
    duration = len(nodes) / node_rate_hz
    n_eff = max(1.0, math.sqrt(math.pi) * spread_hz * duration)
    mean = float(np.mean(u))
    r = np.sqrt(u)
    mean_over_rms = float(np.mean(r) / math.sqrt(np.mean(u)))
    ratio = float(np.mean(u * u) / mean ** 2)
    v = np.sort(u / mean)
    cdf = 1.0 - np.exp(-v)
    n = len(v)
    ks = float(max(np.max(np.arange(1, n + 1) / n - cdf), np.max(cdf - np.arange(0, n) / n)))
    return {'nodes': int(len(nodes)), 'duration_s': duration, 'independent_samples': n_eff,
            'mean_power_rel_a2': mean, 'mean_power_se': 1.0 / math.sqrt(n_eff),
            'fourth_moment_ratio': ratio, 'fourth_moment_ratio_se': math.sqrt(20.0 / n_eff),
            'mean_over_rms': mean_over_rms, 'mean_over_rms_rayleigh': RAYLEIGH_MEAN_OVER_RMS,
            'mean_over_rms_se': 0.2614 / math.sqrt(n_eff),
            'ks_distance': ks, 'ks_critical_95': KS_95 / math.sqrt(n_eff)}


def correlation(a: np.ndarray, b: np.ndarray) -> float:
    return float(abs(np.mean(a * np.conj(b))) / math.sqrt(np.mean(np.abs(a) ** 2) * np.mean(np.abs(b) ** 2)))


def correlation_se(spread_a_hz: float, spread_b_hz: float, duration_s: float) -> float:
    """Standard error of the sample correlation of two independent Gaussian-spectrum processes:
    var = (1 / T) integral R_a R_b dtau = 1 / (T sqrt(2 pi (sigma_a^2 + sigma_b^2))), sigma = d / 2."""
    sa, sb = spread_a_hz / 2.0, spread_b_hz / 2.0
    return math.sqrt(1.0 / (duration_s * math.sqrt(2.0 * math.pi * (sa * sa + sb * sb))))


def rician_k(gain: np.ndarray) -> dict:
    """K-factor from the moments of a complex gain: r = E|g|^4 / E^2|g|^2 = (K^2 + 4K + 2) / (K + 1)^2,
    so sqrt(2 - r) = K / (K + 1)."""
    u = np.abs(gain) ** 2
    r = float(np.mean(u * u) / np.mean(u) ** 2)
    s = math.sqrt(max(2.0 - r, 0.0))
    k = s / (1.0 - s) if s < 1.0 else float('inf')
    return {'fourth_moment_ratio': r, 'k_linear': k, 'k_db': db10(k) if k > 0 else None}


# ---------------------------------------------------------------------------------------------- delays

def static_comb(delays_frames: list[int], amplitudes: list[float], phases_deg: list[float], band_bins: np.ndarray,
                frame: int = 4096) -> dict:
    """The analytic mean |H(f)|^2 of fixed paths, H(f) = sum a_i e^(j phi_i) e^(-j 2 pi f L_i / fs), at
    analyze_delay's band bins (frame 4096) and with its averages: 100 Hz rows and the E.7.1 points,
    each in dB relative to the mean over 1800 +/- 50 Hz."""
    fk = band_bins * FS_HZ / frame
    h = np.zeros(len(fk), dtype=complex)
    for delay, amplitude, phase in zip(delays_frames, amplitudes, phases_deg):
        h += amplitude * np.exp(1j * math.radians(phase)) * np.exp(-2j * np.pi * fk * delay / FS_HZ)
    mag = np.abs(h) ** 2
    ref = float(np.mean(mag[np.abs(fk - 1800) <= 50]))
    rows = []
    for lo in range(300, 3300, 100):
        sel = (fk >= lo) & (fk < lo + 100)
        if np.any(sel):
            rows.append({'band_hz': [lo, lo + 100], 'db_rel_1800': db10(float(np.mean(mag[sel])) / ref)})
    points = {str(fq): db10(float(np.mean(mag[np.abs(fk - fq) <= 50])) / ref) for fq in (1050, 1800, 2550)}
    return {'rows': rows, 'e71_points_db': points}


# ---------------------------------------------------------------------------------------------- criterion 5

def rank_sum_exact(first: list[float], second: list[float]) -> float:
    """Two-sided exact Mann-Whitney p-value with mid-ranks (design 10.4): the probability, over every
    assignment of the pooled values to two groups of the given sizes, of a rank sum at least as far
    from its mean as the one observed. Complete enumeration (C(24, 12) = 2.7 M at 12 seeds)."""
    pooled = list(first) + list(second)
    order = sorted(range(len(pooled)), key=lambda i: pooled[i])
    ranks = [0.0] * len(pooled)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and pooled[order[j + 1]] == pooled[order[i]]:
            j += 1
        for k in range(i, j + 1):
            ranks[order[k]] = (i + j) / 2.0 + 1.0
        i = j + 1
    n1 = len(first)
    mean = n1 * (len(pooled) + 1) / 2.0
    observed = abs(sum(ranks[:n1]) - mean)
    extreme = total = 0
    for combo in itertools.combinations(ranks, n1):
        total += 1
        if abs(sum(combo) - mean) >= observed - 1e-9:
            extreme += 1
    return extreme / total


def ber_crossing(points: list[tuple[float, int, int]], target: float = 1e-3) -> dict:
    """Where one simulator's BER curve crosses `target` (lane call 3, kg7hf/waveform-generator#21).

    `points` are (SNR dB, bit errors, bits compared), one per SNR, each pooled over its seeds. The
    crossing lies in the first adjacent pair, in rising SNR, whose BER falls from at or above the
    target to at or below it; log10(BER) is interpolated linearly in SNR between the two, and a
    point of that pair exactly at the target is the crossing. A point at the target on a rising
    curve is not a crossing.

    A zero-error point has no BER to take the log of. It is placed at 1/bits, a convention: the
    crossing it gives is a point estimate, not a bound, since the BER there may be anywhere up to
    its Poisson 95 % bound 3/bits ('u95'). 'snr_db_u95' is the crossing with that point at 3/bits
    instead (later than 'snr_db'), reported, never used for the offset.

    Where every point lies below the target, or every one above it, the crossing is outside the
    points: 'snr_db' is None and 'outside' is 'below' (the crossing is at a lower SNR than the
    lowest point) or 'above'; 'rising' where the BER does not fall through the target."""
    rows = []
    for snr, errors, bits in sorted(points):
        rows.append({'snr_db': float(snr), 'errors': int(errors), 'bits': int(bits), 'ber': errors / bits, 'zero_error': errors == 0,
                     'value': errors / bits if errors else 1.0 / bits, 'u95': 3.0 / bits if errors == 0 else None})
    result = {'target_ber': target, 'points': rows, 'snr_db': None, 'snr_db_u95': None, 'bracket': None, 'outside': None,
              'from_zero_error_point': False}
    level = math.log10(target)

    def between(low: dict, high: dict, low_value: float, high_value: float) -> tuple[float | None, tuple[dict, ...]]:
        """The crossing within one pair and the points it uses, or None where the pair does not fall through."""
        if not low_value >= target >= high_value or low_value <= high_value:
            return None, ()
        if low_value == target:
            return low['snr_db'], (low,)
        if high_value == target:
            return high['snr_db'], (high,)
        a, b = math.log10(low_value), math.log10(high_value)
        return low['snr_db'] + (high['snr_db'] - low['snr_db']) * (a - level) / (a - b), (low, high)

    for low, high in zip(rows, rows[1:]):
        snr, used = between(low, high, low['value'], high['value'])
        if snr is None:
            continue
        zero = any(p['zero_error'] for p in used)
        out = result | {'snr_db': snr, 'bracket': [low['snr_db'], high['snr_db']], 'from_zero_error_point': zero}
        if zero:
            out['snr_db_u95'] = between(low, high, low['u95'] or low['value'], high['u95'] or high['value'])[0]
        return out
    if all(r['value'] < target for r in rows):
        result['outside'] = 'below'
    elif all(r['value'] > target for r in rows):
        result['outside'] = 'above'
    else:
        result['outside'] = 'rising'
    return result


def snr_offset(stage: list[tuple[float, int, int]], pathsim: list[tuple[float, int, int]], bound_db: float,
               target: float = 1e-3, step_db: float = 2.0) -> dict:
    """The equivalent-SNR offset at `target` (lane call 3): SNR_stage(target) - SNR_pathsim(target),
    each from ber_crossing; 'pass' when |offset| <= bound_db, else 'fail'. A crossing outside a
    simulator's points gives no verdict ('n/a') and names the rung to add: step_db below the lowest
    point, or above the highest."""
    crossings = {'stage': ber_crossing(stage, target), 'pathsim': ber_crossing(pathsim, target)}
    add = set()
    for crossing in crossings.values():
        snrs = [p['snr_db'] for p in crossing['points']]
        if crossing['outside'] == 'below':
            add.add(min(snrs) - step_db)
        elif crossing['outside'] == 'above':
            add.add(max(snrs) + step_db)
    result = {'target_ber': target, 'bound_db': bound_db, 'offset_db': None, 'add_rungs_db': sorted(add), 'crossings': crossings}
    if any(c['snr_db'] is None for c in crossings.values()):
        if add:
            reason = 'n/a: crossing outside, add a rung at ' + ' and '.join(f'{x:g} dB' for x in sorted(add))
        else:
            reason = f'n/a: the BER does not fall through {target:g} with rising SNR'
        return result | {'status': 'n/a', 'reason': reason}
    offset = crossings['stage']['snr_db'] - crossings['pathsim']['snr_db']
    passed = abs(offset) <= bound_db
    reason = f"offset {offset:+.2f} dB, {'within' if passed else 'beyond'} +/-{bound_db:g} dB"
    for name, crossing in crossings.items():
        if crossing['from_zero_error_point']:
            later = crossing['snr_db_u95']
            reason += (f"; {name}'s crossing uses a zero-error point placed at 1/bits, a point estimate"
                       + (f" (at its 95 % bound 3/bits the crossing is {later - crossing['snr_db']:.2f} dB later)" if later is not None else ''))
    return result | {'status': 'pass' if passed else 'fail', 'offset_db': offset, 'reason': reason}
