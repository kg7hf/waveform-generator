#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Reproduce the computed numbers in docs/design/watterson-stage.md.

numpy only; nothing is read or written. Run: python -B docs/design/watterson-stage-numbers.py
Every figure the design quotes as "computed" comes from one function below:

  noise_band()   the existing AWGN kernel and the proposed flat noise-band kernel
  hilbert()      the type III Kaiser Hilbert FIR that makes the analytic signal
  doppler()      the E.5.4 Gaussian Doppler FIR, its sidelobes, and the images of
                 linear interpolation from the tap rate to 48 kHz
  truncation()   per-spur and integrated out-of-lobe power at 1 % and 0.1 % (Q1)
  interpolation_correction()  the size of v_i and why U18 is deterministic
  box_muller()   the u1 widening: radius bounds and the cost of the uint32 wrap
  latency()      the E.4.4 stream-path budget against block size (3.4)
  static_comb()  E.7.1 points of the static multipath rows as analyze_delay averages them (B4)
  noise_floors() what B1 and B6 can resolve at their render lengths
  ladder_rule()  false-fail rate of the range rule, size and power of the rank-sum rule (B7)
  ppm_resampler() the sample-clock stage's polyphase interpolator (Appendix A)

The Monte Carlo in ladder_rule() uses a fixed seed, so its figures repeat exactly.

Optional: --lane-probes <hfchannel\\probes> also runs the lane's analyze_baseband on the designed
tap process (the per-spur reading of Q1). It imports the lane module read-only; run with python -B
so no bytecode is written beside it.
"""

import argparse
import math
import sys

import numpy as np

FS = 48000.0
NFFT = 1 << 20
BAND_LOW_HZ = 300.0          # lane SNR band, N0 x 3000 Hz in 300-3300 Hz
BAND_HIGH_HZ = 3300.0
SNR_BANDWIDTH_HZ = 3000.0
TRUNCATION = 0.01            # E.5.4: tap gain at +/-tau <= 1 % of peak
TAU_D = math.sqrt(math.log(1.0 / TRUNCATION)) / math.pi   # tau * d = 0.6831


def kaiser_beta(a_db):
    """Kaiser (1974): beta for a stopband attenuation of a_db."""
    if a_db > 50.0:
        return 0.1102 * (a_db - 8.7)
    return 0.5842 * (a_db - 21.0) ** 0.4 + 0.07886 * (a_db - 21.0)


def kaiser_taps(a_db, transition_hz):
    """Kaiser (1974): N - 1 = (A - 7.95) / (2.285 dw)."""
    dw = 2.0 * math.pi * transition_hz / FS
    return int(math.ceil((a_db - 7.95) / (2.285 * dw))) + 1


def power_response(kernel):
    response = np.abs(np.fft.rfft(kernel, NFFT)) ** 2
    return np.fft.rfftfreq(NFFT, 1.0 / FS), response


def noise_band():
    print("== noise band (AWGN) ==")
    # existing: signal-lab/src/band_fir.cpp, Blackman 513 taps, cutoffs 300/3300 Hz, L2-normalised
    n = 513
    m = np.arange(n) - (n - 1) / 2.0
    ideal = 2 * BAND_HIGH_HZ / FS * np.sinc(2 * BAND_HIGH_HZ / FS * m) - 2 * BAND_LOW_HZ / FS * np.sinc(2 * BAND_LOW_HZ / FS * m)
    phase = 2 * np.pi * np.arange(n) / (n - 1)
    existing = ideal * (0.42 - 0.5 * np.cos(phase) + 0.08 * np.cos(2 * phase))
    existing /= math.sqrt(np.sum(existing ** 2))
    f, p = power_response(existing)
    interior = (f >= 500.0) & (f <= 3100.0)
    ref = float(np.mean(p[interior]))
    band = (f >= BAND_LOW_HZ) & (f <= BAND_HIGH_HZ)
    flat = (p / ref >= 10 ** (-0.2 / 10)) & (f > 100.0) & (f < 4000.0)
    enbw = float(np.sum(p) * (f[1] - f[0]) / ref)
    kurt = float(np.sum(existing ** 4) / np.sum(existing ** 2) ** 2)
    print(f"existing Blackman 513: 300 Hz {10*math.log10(p[np.argmin(abs(f-300))]/ref):+.2f} dB,"
          f" 3300 Hz {10*math.log10(p[np.argmin(abs(f-3300))]/ref):+.2f} dB;"
          f" within +/-0.2 dB only {f[flat][0]:.0f}-{f[flat][-1]:.0f} Hz; ENBW {enbw:.1f} Hz;"
          f" delivered N0x3000 SNR = label {10*math.log10(enbw/SNR_BANDWIDTH_HZ):+.3f} dB;"
          f" sum h^4/(sum h^2)^2 {kurt:.4f} -> Irwin-Hall-12 excess kurtosis {-0.1*kurt:+.4f}")
    # proposed: Kaiser, A = 60 dB, transitions of 300 Hz centred 150 Hz outside the band edges
    a_db = 60.0
    beta = kaiser_beta(a_db)
    n = kaiser_taps(a_db, 300.0)
    n += 1 - n % 2
    low, high = BAND_LOW_HZ - 150.0, BAND_HIGH_HZ + 150.0
    m = np.arange(n) - (n - 1) / 2.0
    kernel = (2 * high / FS * np.sinc(2 * high / FS * m) - 2 * low / FS * np.sinc(2 * low / FS * m)) * np.kaiser(n, beta)
    f, p = power_response(kernel)
    kernel /= math.sqrt(p[np.argmin(abs(f - 1800.0))])        # unit gain at the band centre
    f, p = power_response(kernel)
    enbw = (FS / 2.0) * float(np.sum(kernel ** 2))
    kurt = float(np.sum(kernel ** 4) / np.sum(kernel ** 2) ** 2)
    pts = ", ".join(f"{x:.0f} Hz {10*math.log10(p[np.argmin(abs(f-x))]+1e-30):.1f}" for x in (0, 50, 100, 150, 250))
    print(f"proposed Kaiser N={n} beta={beta:.3f} cutoffs {low:.0f}/{high:.0f} Hz: 300-3300 Hz"
          f" {10*math.log10(p[band].min()):+.4f}/{10*math.log10(p[band].max()):+.4f} dB; ENBW {enbw:.1f} Hz;"
          f" total noise = N0x3000 {10*math.log10(enbw/SNR_BANDWIDTH_HZ):+.3f} dB; {pts} dB;"
          f" >= 3.6 kHz max {10*math.log10(p[f>=3600].max()):.1f} dB; multiplies/sample {(n+1)//2};"
          f" Irwin-Hall would give excess kurtosis {-0.1*kurt:+.4f}")


def hilbert_kernel(n, beta):
    m = np.arange(n) - (n - 1) // 2
    ideal = np.zeros(n)
    odd = (m % 2) != 0
    ideal[odd] = 2.0 / (np.pi * m[odd])
    return ideal * np.kaiser(n, beta)


def hilbert():
    print("== Hilbert FIR (analytic signal) ==")
    a_db, edge = 72.0, 250.0
    beta = kaiser_beta(a_db)
    n = kaiser_taps(a_db, 2.0 * edge)    # the half-band prototype's transition is 2 x the band edge
    n += (3 - n % 4) % 4                  # 4k + 3: odd centre delay, non-zero end taps
    f, p = power_response(hilbert_kernel(n, beta))
    mag = np.sqrt(p)
    for low in (250.0, 300.0, 400.0):
        sel = (f >= low) & (f <= FS / 2 - low)
        err = mag[sel] - 1.0
        print(f"N={n} beta={beta:.3f} D={(n-1)//2} frames ({(n-1)/2/FS*1e3:.3f} ms): f >= {low:.0f} Hz:"
              f" max |1-|H|| {np.max(np.abs(err)):.1e} ({20*math.log10(1+np.max(np.abs(err))):.4f} dB),"
              f" mirror image {20*math.log10(np.max(np.abs(err)/np.abs(2+err))):.1f} dBc")
    print(f"non-zero taps {((n-1)//2+1)//2*2}, multiplies per sample {((n-1)//2+1)//2}")


def doppler_kernel(k_multiple):
    half = math.ceil(TAU_D * k_multiple)          # outermost tap <= 1 % of peak (lane count 45/89 at K = 32/64)
    k = np.arange(-half, half + 1)
    g = np.exp(-(math.pi * k / k_multiple) ** 2)  # exp(-pi^2 d^2 t^2), t = k / (K d): the prose form, sigma = d/2
    return g / math.sqrt(np.sum(g * g)), half


def doppler(k_multiple, interp_factor=750):
    c, half = doppler_kernel(k_multiple)
    n = 1 << 22
    sr = np.abs(np.fft.fft(c, n)) ** 2           # tap-rate PSD over one period, u = f / R
    u = np.fft.fftfreq(n)
    fd = u * k_multiple                          # in units of d
    s0 = sr * np.sinc(u) ** 4                    # baseband with the linear interpolator's droop
    peak = s0[0]
    sel = (10 * np.log10(s0 / peak + 1e-300) >= -30.0) & (np.abs(fd) < 3.0)
    coef = np.linalg.lstsq(np.vstack([np.ones(sel.sum()), fd[sel] ** 2]).T, np.log(s0[sel] / peak), rcond=None)[0]
    two_sigma = 2.0 * math.sqrt(-1.0 / (2.0 * coef[1]))
    lobe_sel = np.abs(fd) <= 2.0                 # +/- 4 sigma
    lobe = float(np.sum(s0[lobe_sel]))
    mom = 2.0 * math.sqrt(float(np.sum(s0[lobe_sel] * fd[lobe_sel] ** 2)) / lobe)

    def level(x):
        return 10 * math.log10(s0[np.argmin(np.abs(fd - x))] / peak)
    e20 = level(0.5 * math.sqrt(2 * math.log(100.0))) + 20.0
    e30 = level(0.5 * math.sqrt(2 * math.log(1000.0))) + 30.0
    far = np.flatnonzero((fd > 3.0) & (fd < k_multiple / 2 - 1))
    loc = [j for j in far if s0[j] > s0[j - 1] and s0[j] >= s0[j + 1]]
    side = sorted(loc, key=lambda j: -s0[j])[:3]
    side_txt = "; ".join(f"{fd[j]:.2f} d {10*math.log10(s0[j]/peak):.1f} dB re peak,"
                         f" {10*math.log10(np.sum(s0[np.abs(fd-fd[j]) <= 0.25])/lobe):.1f} dBc in +/-0.25 d" for j in sorted(side))
    images = []
    for k in range(1, 41):
        uk = u + k
        lam = (np.sin(math.pi * uk) / (interp_factor * np.sin(math.pi * uk / interp_factor))) ** 4
        images.append(float(np.sum((sr * lam)[lobe_sel])))
    images = np.array(images)
    rho1 = float(np.sum(c[:-1] * c[1:]))
    print(f"K={k_multiple:g}: {2*half+1} taps, end tap {100*math.exp(-(math.pi*half/k_multiple)**2):.2f} % of peak;"
          f" 2 sigma (top-30-dB fit) {two_sigma:.4f} d, (second moment, +/-4 sigma) {mom:.4f} d;"
          f" Table E-I -20/-30 dB points {e20:+.2f}/{e30:+.2f} dB")
    print(f"    truncation sidelobes: {side_txt}")
    print(f"    linear-interpolation images: R {10*math.log10(images[0]/lobe):.1f}, 2R {10*math.log10(images[1]/lobe):.1f},"
          f" 3R {10*math.log10(images[2]/lobe):.1f} dBc; all images {10*math.log10(2*np.sum(images)/lobe):.1f} dBc;"
          f" lag-1 correlation {rho1:.6f}; variance at mid-interval {10*math.log10(0.5+0.5*rho1):+.4f} dB,"
          f" mean {10*math.log10(1-(1-rho1)/3):+.4f} dB")


# ------------------------------------------------------------------------------------------
# Added with the review revision (Review notes, appendix B of the design)
# ------------------------------------------------------------------------------------------

K_MAX = 640.0 / (750.0 / 76.0)   # 64.853: the largest K, for d just above 750/76 Hz (M = 75)
D_FRAMES = 215                   # Hilbert centre delay (hilbert())


def kernel_at(k_multiple, fraction):
    """The prose-form E.5.4 FIR truncated where the tap falls to `fraction` of the peak."""
    tau_d = math.sqrt(math.log(1.0 / fraction)) / math.pi
    half = math.ceil(tau_d * k_multiple)
    k = np.arange(-half, half + 1)
    g = np.exp(-(math.pi * k / k_multiple) ** 2)
    return g / math.sqrt(np.sum(g * g)), half, tau_d


def baseband_psd(c, k_multiple, n=1 << 22):
    """PSD of the tap process in the first zone of the node rate, with the interpolator's droop."""
    u = np.fft.fftfreq(n)
    return u * k_multiple, (np.abs(np.fft.fft(c, n)) ** 2) * np.sinc(u) ** 4


def truncation():
    print("== E.5.4 truncation: per-spur and integrated power outside the lobe (Q1) ==")
    ideal = math.erfc(6.0 / math.sqrt(2.0))      # Gaussian, sigma = d/2: power beyond +/-3 d = +/-6 sigma
    print(f"ideal Gaussian beyond +/-3 d: {10*math.log10(ideal):.1f} dBc")
    for fraction in (0.01, 0.001):
        for k_multiple in (64.0, K_MAX):
            c, half, tau_d = kernel_at(k_multiple, fraction)
            fd, s0 = baseband_psd(c, k_multiple)
            total = float(np.sum(s0))
            beyond = float(np.sum(s0[np.abs(fd) > 3.0])) / total
            far = np.flatnonzero((fd > 3.0) & (fd < k_multiple / 2 - 1))
            loc = [j for j in far if s0[j] > s0[j - 1] and s0[j] >= s0[j + 1]]
            top = max(loc, key=lambda j: s0[j])
            spur = float(np.sum(s0[np.abs(fd - fd[top]) <= 0.25])) / total
            print(f"{100*fraction:g} % (tau d = {tau_d:.4f}), K = {k_multiple:.3f}: {2*half+1} taps;"
                  f" integrated beyond +/-3 d {10*math.log10(beyond):.1f} dBc;"
                  f" largest sidelobe {fd[top]:.2f} d, {10*math.log10(spur):.1f} dBc in +/-0.25 d")
    caps = {}
    for fraction in (0.01, 0.001):
        tau_d = math.sqrt(math.log(1.0 / fraction)) / math.pi
        caps[fraction] = 2 * math.ceil(tau_d * K_MAX) + 1
    per_path = ((caps[0.001] + 1) // 2 - (caps[0.01] + 1) // 2) * 4 + 2 * (caps[0.001] - caps[0.01]) * 8
    print(f"capacity over the range: 1 % {caps[0.01]} taps, 0.1 % {caps[0.001]} taps;"
          f" 0.1 % costs {per_path} B per path (half-coefficients + mirrored complex history), {4*per_path} B for 4 paths")


def interpolation_correction():
    print("== interpolation correction v_i (section 6) and U18 ==")
    for k_multiple, m in ((64.0, 7500), (64.0, 750), (64.0, 75), (K_MAX, 75)):
        c, _, _ = kernel_at(k_multiple, TRUNCATION)
        rho1 = float(np.sum(c[:-1] * c[1:]))
        mu = np.arange(m) / m
        brute = float(np.mean((1 - mu) ** 2 + mu ** 2 + 2 * mu * (1 - mu) * rho1))
        closed = 1.0 - (1.0 - rho1) * (m * m - 1) / (3.0 * m * m)
        print(f"K = {k_multiple:.3f}, M = {m}: rho1 {rho1:.6f}; v brute {brute:.9f}, closed form {closed:.9f}"
              f" (difference {abs(brute-closed):.1e}); 1 - v = {1-closed:.2e} ({10*math.log10(closed):+.5f} dB)")
    sigma = 0.5                                          # d = 1 Hz
    tau_c = 1.0 / (2.0 * math.sqrt(math.pi) * sigma)    # integral of |R_h|^2 for a Gaussian PSD
    n_eff = (2 ** 22 / 64.0) / tau_c
    need = (5.0 / 1e-3) ** 2
    hours_10hz = need * (1.0 / (2.0 * math.sqrt(math.pi) * 5.0)) / 3600.0
    print(f"old U18: 2^22 nodes at 1 Hz -> N_eff {n_eff:.3g}, s.e. of mean |g|^2 {1/math.sqrt(n_eff):.2e};"
          f" 5 s.e. at 1e-3 needs N_eff {need:.2g} = {hours_10hz:.0f} h at 10 Hz")


def box_muller():
    print("== Box-Muller: the u1 widening and the excitation variance ==")
    u1_min = 2.0 ** -32                                  # (0 + 1) x 2^-32
    print(f"u1 in [2^-32, 1]: complex radius sqrt(-ln u1) <= {math.sqrt(-math.log(u1_min)):.3f};"
          f" unit-variance real pair sqrt(-2 ln u1) <= {math.sqrt(-2.0*math.log(u1_min)):.3f}")
    print(f"wrapped uint32 sum: u1 = 0, det::log(0) = -1e300, radius {math.sqrt(1e300):.0e}")
    noise_rate = FS / 2.0                                # one u1 per pair of noise frames
    tap_rate = 4 * 640.0                                 # 4 paths at the 10 Hz node rate
    for name, rate in (("AWGN", noise_rate), ("taps, 4 paths at 10 Hz", tap_rate)):
        mean_h = 2.0 ** 32 / rate / 3600.0
        p5 = 1.0 - math.exp(-5 * 3600 * rate / 2.0 ** 32)
        print(f"{name}: {rate:.0f} u1 draws/s, wrap draw once per {mean_h:.1f} h on average, P(in a 5 h row) {100*p5:.1f} %")
    print(f"U31 (2^24 frames = 2^23 pairs): P(wrap draw) {100*2**23/2**32:.2f} %")
    print(f"complex helper E|w|^2 = 1 -> each component 1/2; used unscaled as AWGN excitation the noise is"
          f" {10*math.log10(2):.2f} dB low (delivered SNR label + {10*math.log10(2):.2f} dB)")


def latency():
    print("== E.4.4 x-to-y latency against block size (stream path) ==")
    interface = 48                                       # one 1 ms full-speed USB packet
    do_frames, limit_frames = 480, 1200                  # 10 ms DO, 25 ms limit at 48 kHz
    for block in (64, 96, 128, 192, 256, 384, 448, 480, 512, 1024, 2048):
        worst = 2 * block + D_FRAMES + interface         # accumulate + one block of output buffering
        expected = block + math.ceil(0.1 * block) + D_FRAMES + interface
        verdict = "DO" if worst <= do_frames else ("25 ms" if worst <= limit_frames else "fails")
        print(f"block {block:4d}: worst {worst:5d} frames {1e3*worst/FS:6.2f} ms ({verdict});"
              f" with 10 % CPU {expected:5d} frames {1e3*expected/FS:6.2f} ms")
    print(f"largest block, worst case: DO {(do_frames - D_FRAMES - interface)//2}, 25 ms {(limit_frames - D_FRAMES - interface)//2} frames")


def static_comb():
    print("== B4: E.7.1 points of static channels as analyze_delay averages them ==")
    frame = 4096                                         # the lane's delay-probe frame
    f = np.arange(frame // 2 + 1) * FS / frame
    band = (f >= BAND_LOW_HZ) & (f <= BAND_HIGH_HZ)
    rows = (("1 fixed, 0 ms", ((0, 0.0),)),
            ("0/2 ms", ((0, 0.0), (96, 0.0))),
            ("0/5 ms", ((0, 0.0), (240, 0.0))),
            ("3 kHz WID 2, 0/3/9 ms", ((0, 0.0), (144, 0.0), (432, 0.0))),
            ("3 kHz WID 10, 0/2/4.5 ms", ((0, 0.0), (96, 0.0), (216, 0.0))),
            ("6 kHz WID 10, 0/1.5/3/5 ms", ((0, 0.0), (72, 0.0), (144, 0.0), (240, 0.0))),
            ("48 kHz WID 12, 0/1.5 ms, -3 dB", ((0, 0.0), (72, -3.0))),
            ("0/20 ms", ((0, 0.0), (960, 0.0))))
    for name, paths in rows:
        p = np.array([10 ** (level / 10.0) for _, level in paths])
        a = np.sqrt(p / np.sum(p))
        h = sum(ai * np.exp(-2j * np.pi * f * lag / FS) for ai, (lag, _) in zip(a, paths))
        mag = np.abs(h) ** 2

        def point(fq):
            return float(np.mean(mag[band & (np.abs(f - fq) <= 50.0)]))
        ref = point(1800.0)
        lo, hi = 10 * math.log10(point(1050.0) / ref), 10 * math.log10(point(2550.0) / ref)
        verdict = "within 0.5 dB" if max(abs(lo), abs(hi)) <= 0.5 else "outside 0.5 dB"
        print(f"{name}: 1050 {lo:+.2f} dB, 2550 {hi:+.2f} dB re 1800 ({verdict});"
              f" |H|^2 over the band {10*math.log10(mag[band].min()+1e-30):.1f} to {10*math.log10(mag[band].max()):+.2f} dB")


def normal_upper_quantile(p):
    """z with P(Z > z) = p, by bisection on erfc (no scipy)."""
    lo, hi = 0.0, 40.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if 0.5 * math.erfc(mid / math.sqrt(2.0)) > p:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def chi2_upper_ratio(nu, p):
    """Wilson-Hilferty: the upper-p point of chi-square(nu) / nu."""
    z = normal_upper_quantile(p)
    return (1.0 - 2.0 / (9.0 * nu) + z * math.sqrt(2.0 / (9.0 * nu))) ** 3


def noise_floors():
    print("== B1 and B6: what the renders can resolve ==")
    for se_ref, what in ((0.043, "pathsim tone (calib A1)"), (0.017, "refchan tone, worst"),
                         (0.042, "refchan gapnoise, worst"), (0.060, "pathsim gapnoise, worst")):
        cells = []
        for seconds in (120.0, 600.0, 1200.0):
            se = se_ref * math.sqrt(119.5 / seconds)
            cells.append(f"{seconds:.0f} s: s.e. {se:.3f} dB, 0.1 dB = {0.1/se:.1f} s.e.,"
                         f" false fail {100*math.erfc(0.1/se/math.sqrt(2.0)):.2g} %")
        print(f"B1 {what}: " + "; ".join(cells))
    per_bin_p = 1e-6                                     # false alarm per bin or harmonic
    welch_c = 0.1667                                     # Hann, 50 % overlap: adjacent-segment correlation
    for seconds in (1200.0, 18000.0):
        segments = int(2 * seconds / 10.0) - 1          # 10 s Hann segments (0.1 Hz bins, ENBW 0.15 Hz)
        nu = 2.0 * segments / (1.0 + 2.0 * welch_c ** 2)
        floor = 10 * math.log10(0.15 / SNR_BANDWIDTH_HZ)
        line = floor + 10 * math.log10(chi2_upper_ratio(nu, per_bin_p) - 1.0)
        print(f"B6 Welch, {seconds/3600:.2f} h: {segments} segments, nu {nu:.0f}; bin {floor:.1f} dB re in-band noise;"
              f" line limit (1e-6 per bin) {line:.1f} dB re noise = {line-10:.1f} dBc at 10 dB SNR,"
              f" {line-60:.1f} dBc at 60 dB SNR")
        harmonic_floor = 10 * math.log10(1.0 / (SNR_BANDWIDTH_HZ * seconds))
        single = harmonic_floor + 10 * math.log10(math.log(1.0 / per_bin_p))
        cells = []
        for period in (2048, 1024, 256, 64, 2):
            spacing = FS / period
            count = sum(1 for h in range(1, period // 2 + 1) if BAND_LOW_HZ <= h * spacing <= BAND_HIGH_HZ)
            comb = harmonic_floor + 10 * math.log10(5.0 * math.sqrt(count)) if count else None
            cells.append(f"P {period}: {count} in-band harmonics" + (f", comb total {comb:.1f}" if comb is not None else ""))
        print(f"B6 synchronous fold, {seconds/3600:.2f} h: harmonic floor {harmonic_floor:.1f} dB re noise;"
              f" single line (1e-6) {single:.1f} dB re noise = {single-10:.1f} dBc at 10 dB SNR; " + "; ".join(cells))


def mann_whitney_counts(n, m):
    """Exact null distribution of U (pairs x > y) for samples of sizes n and m, as counts."""
    table = [[None] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        table[i][0] = [1]
    for j in range(m + 1):
        table[0][j] = [1]
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            counts = [0] * (i * j + 1)
            for u, v in enumerate(table[i - 1][j]):     # the largest value is an x: it beats all j y's
                counts[u + j] += v
            for u, v in enumerate(table[i][j - 1]):     # the largest value is a y
                counts[u] += v
            table[i][j] = counts
    return table[n][m]


def ladder_rule():
    print("== B7: decoder-equivalence rule ==")
    rng = np.random.default_rng(1102026)
    trials = 200000
    a = rng.standard_normal((trials, 4))                 # ours, 4 seeds (log BER, same distribution)
    b = rng.standard_normal((trials, 4))                 # pathsim, 4 seeds
    plain = np.mean((a.min(1) >= b.min(1)) & (a.max(1) <= b.max(1)))
    sd = b.std(1, ddof=1)
    widened = np.mean((a.min(1) >= b.min(1) - sd) & (a.max(1) <= b.max(1) + sd))
    la, lb = np.exp(a), np.exp(b)                        # lognormal BER, widened on the BER scale
    sdl = lb.std(1, ddof=1)
    widened_ln = np.mean((la.min(1) >= lb.min(1) - sdl) & (la.max(1) <= lb.max(1) + sdl))
    print(f"range rule, both simulators identical: plain {plain:.3f}, widened by one SD {widened:.3f}"
          f" (lognormal on the BER scale {widened_ln:.3f}); all of 5 rungs {widened**5:.3f}, all of 8 {widened**8:.3f}")
    rungs, alpha = 8, 0.05
    per_rung = alpha / rungs
    for n in (4, 8, 12, 16):
        counts = np.array(mann_whitney_counts(n, n), dtype=float)
        cdf = np.cumsum(counts) / counts.sum()
        crit = int(np.searchsorted(cdf, per_rung / 2.0, side="right")) - 1   # reject U <= crit or U >= n^2 - crit
        size = 2.0 * cdf[crit] if crit >= 0 else 0.0
        min_p = 2.0 * counts[0] / counts.sum()
        powers = []
        for delta in (1.0, 1.5, 2.0, 3.0):
            x = rng.standard_normal((20000, n)) + delta
            y = rng.standard_normal((20000, n))
            u = np.sum(x[:, :, None] > y[:, None, :], axis=(1, 2))
            rej = (u <= crit) | (u >= n * n - crit) if crit >= 0 else np.zeros(len(u), dtype=bool)
            powers.append(f"{delta:g} SD {np.mean(rej):.2f}")
        print(f"rank-sum, {n} vs {n} seeds, alpha {per_rung:.5f} per rung: smallest p {min_p:.2g}; exact size {size:.5f}"
              f" (family-wise over {rungs} rungs <= {1-(1-size)**rungs:.4f}); power at a shift of " + ", ".join(powers))


def ppm_resampler():
    print("== Appendix A: sample-clock stage interpolator ==")

    def prototype(taps, mu, beta, cutoff):
        k = np.arange(-(taps // 2 - 1), taps // 2 + 1)
        t = k - mu
        window = np.i0(beta * np.sqrt(np.clip(1.0 - (t / (taps / 2.0)) ** 2, 0.0, None))) / np.i0(beta)
        h = 2.0 * cutoff * np.sinc(2.0 * cutoff * t) * window
        return k, h / np.sum(h)                           # unit gain at DC for every phase

    for taps, phases, beta in ((16, 64, 10.0), (24, 128, 10.0), (32, 256, 10.0)):
        cutoff = 0.45                                     # x fs
        k = prototype(taps, 0.0, beta, cutoff)[0]
        table = np.array([prototype(taps, p / phases, beta, cutoff)[1] for p in range(phases + 1)])
        mus = (np.arange(2048) + 0.5) / 2048
        cells = []
        for f_max in (BAND_HIGH_HZ, 15000.0):
            f = np.linspace(0.0, f_max, 64)
            worst = 0.0
            for mu in mus:
                pos = mu * phases
                p = int(pos)
                frac = pos - p
                c = (1.0 - frac) * table[p] + frac * table[p + 1]
                resp = np.exp(2j * np.pi * np.outer(f / FS, k)) @ c
                worst = max(worst, float(np.max(np.abs(resp - np.exp(2j * np.pi * f / FS * mu)))))
            cells.append(f"to {f_max/1e3:g} kHz {20*math.log10(worst):.1f} dB")
        print(f"N {taps}, P {phases}, Kaiser beta {beta:g}, cutoff {cutoff} fs: worst error " + ", ".join(cells)
              + f"; table {(phases + 1) * taps * 4} B; {2*taps} MACs per output frame; delay {taps//2} frames")
    step = 2.0 ** -32
    print(f"32.32 phase accumulator: ratio step {step:.2e} = {1e6*step:.2e} ppm;"
          f" a 2048-frame block at +1000 ppm gives at most {math.ceil(2048*1.001)+1} frames (engine capacity 2304)")


def lane_spur_check(probes):
    """The lane's analyze_baseband on the designed tap process (per-spur reading of Q1)."""
    sys.path.insert(0, probes)
    import analyze_doppler                               # noqa: E402 (lane module, read-only)
    print("== lane analyze_baseband on the designed tap process (500 Hz grid) ==")
    rng = np.random.default_rng(20261008)
    fs_bb = 500.0
    for fraction in (0.01, 0.001):
        for d, seconds in ((0.5, 1200.0), (1.0, 600.0), (2.0, 600.0)):
            frames_per_node = math.floor(750.0 / d)
            c, half, _ = kernel_at(FS / frames_per_node / d, fraction)
            t = np.arange(int(seconds * fs_bb)) * (FS / fs_bb)
            m = np.floor(t / frames_per_node).astype(np.int64)
            mu = (t - m * frames_per_node) / frames_per_node
            count = m[-1] + 2 + 2 * half
            w = (rng.standard_normal(count) + 1j * rng.standard_normal(count)) / math.sqrt(2.0)
            h = np.convolve(w, c, mode="valid")
            z = (1.0 - mu) * h[m] + mu * h[m + 1]
            res, _ = analyze_doppler.analyze_baseband(z, fs_bb, label_spread=d)
            spurs = res["spurs"]
            top = max(spurs, key=lambda s: s["level_dbc"]) if spurs else None
            txt = f"{top['level_dbc']:.1f} dBc at {abs(top['offset_hz'])/d:.2f} d" if top else "none"
            print(f"{100*fraction:g} %, d {d:g} Hz, {seconds:.0f} s: 2 sigma {res['spread_2sigma_hz']/d:.4f} d; largest spur {txt}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lane-probes", help="the lane's hfchannel\\probes directory (optional check)")
    args = parser.parse_args()
    noise_band()
    hilbert()
    print("== Doppler FIR and tap interpolation ==")
    for k_multiple in (32.0, 64.0, 64.85):
        doppler(k_multiple)
    truncation()
    interpolation_correction()
    box_muller()
    latency()
    static_comb()
    noise_floors()
    ladder_rule()
    ppm_resampler()
    if args.lane_probes:
        lane_spur_check(args.lane_probes)


if __name__ == "__main__":
    main()
