# Watterson multipath fading stage for signal-lab: design

This note preserves the mathematical model, numeric choices, implementation constraints, and unresolved acceptance limits for the Watterson stage. The current code is described in the [module guide](../modules/signal-lab-watterson.md); the [validation reference](watterson-validation.md) gives the host procedure.

Current boundaries:

- The sample-clock stage in Appendix A is deferred. Low-latency block mode and latency stamps are also deferred; the stage's algorithmic delay is `k + D + L` and is covered by a component test.
- The Watterson components are held by value in public headers under `signal-lab/include/signal_lab/`. `WattersonStage` receives its channel through `StageResources`; a missing channel is rejected during preparation.
- The host renderer supports the static channel and bounded WAV streaming. Firmware resource wiring and offline ARM build checks do not establish physical behavior or runtime margin.
- Host statistical comparisons use `signal-lab/validation/run_watterson_validation.py` and its dedicated `wfg_watterson_render` driver. Pathsim is an independent statistical reference, not an implementation template.

Design inputs include the [HF channel simulator survey](hf-channel-simulator-survey.md) and MIL-STD-188-110D with Change 1, Appendix E. The [numeric helper](watterson-stage-numbers.py) uses NumPy only; run `python -B docs/design/watterson-stage-numbers.py`. For optional lane-analyzer figures, use `--lane-probes 'C:/reference-data/hfchannel/probes'`; that reference data is external and not included here.
## 1. Decisions at a glance

| Topic | Design choice | Detail |
|---|---|---|
| Signal model | Real 48 kHz input/output; a type III Kaiser Hilbert FIR creates the analytic signal. Output is the real part of the sum of delayed complex paths. Algorithmic delay is D = 215 frames (4.479 ms). | 3 |
| Paths | Up to four fixed-capacity paths; delays 0â€“20 ms; relative powers normalized to one; Rayleigh or fixed fading; shifts up to +/-75 Hz; Rician combines fixed and Rayleigh components. | 4 |
| Tap processes | Deterministic complex Gaussian taps use PCG32, a Doppler FIR and linear interpolation. The unresolved truncation interpretation is summarized in section 5.5 and section 13. | 5 |
| Normalization | Correct interpolation variance analytically; configure-time scale is independent of signal content. | 6 |
| AWGN | Use a calibrated Kaiser noise band, unit-variance Gaussian excitation, and N0-referenced SNR. ENBW is 3233.4 Hz. | 7 |
| Sample clock | A separate sample-clock stage is deferred; Appendix A preserves its proposed model. | A |
| Determinism | Output depends on seed, stage, path, and absolute frame; block partition does not change the output. | 8 |
| Interface | Fixed channel state and a typed stage adapter are supplied through `StageResources`; scenario units and ranges are validated. | 9 |
| Verification | Host unit tests cover component contracts; optional external statistical comparisons use pathsim as an independent reference. | 10 |
| Budget | CPU and memory values in section 11 are estimates until measured on target under the intended workload. | 11 |

## 2. Requirements trace

| Requirement | Design coverage | Verification source |
|---|---|---|
| E.4.1 / E.4.2: fading channel and tapped-delay-line model | Sections 3â€“6 | Host unit tests and statistical validation |
| E.4.4: algorithmic and stream latency | Section 3.4 | Algorithmic delay is covered by component tests; stream latency needs target measurement |
| E.5.1 / E.5.3: bandwidth, input reference, independent noise | Sections 3.5 and 7 | Host validation with declared probes and analyzers |
| E.5.2: nearest-sample delay | Section 4.2 | Delay arithmetic tests and optional external comparisons |
| E.5.4: Doppler spectrum, tap power, interpolation | Sections 5 and 6 | Component tests and optional external statistical comparison |
| E.5.5: N0 x 3000 Hz SNR reference | Section 7 | Host SNR validation |
| E.6.2: analytic bandwidth and Hilbert cost | Section 3.3 | Filter design checks |
| E.7.1â€“E.7.4: formal statistical acceptance | Section 10 | Optional external-data battery; see [validation method](watterson-validation.md) |
| D.6.2: static multipath examples | Sections 4.3 and 4.6 | Host scenarios and optional external comparison |
## 3. Signal model

### 3.1 Equations

The input x(n) is the engine's normalised float stream (+/-1.0 is PCM full scale), at
fs = 48 kHz. Let D = (N_H - 1) / 2 be the Hilbert FIR's centre delay.

```text
analytic signal   xa(n) = x(n - D) + j xh(n)
Hilbert branch    xh(n) = sum over odd m = 1, 3, ..., D of  h_H[m] (x(n - D - m) - x(n - D + m))
                  h_H[m] = 2 / (pi m) * w_K[m]             (type III, antisymmetric, w_K the Kaiser window)
path gain         g_i(n) = a_i * h_i(n) * r_i(n)           h_i: interpolated Gaussian tap (Rayleigh) or 1 (fixed)
                  r_i(n) = exp(j (2 pi f_i n / fs + phi_i))  f_i: shift, phi_i: phase (fixed paths)
output            y(n) = Re{ sum_i g_i(n) xa(n - L_i) }
                       = sum_i [ Re g_i(n) * x(n - D - L_i)  -  Im g_i(n) * xh(n - L_i) ]
```

This is E.4.2's complex tapped delay line, with the complex x made by a Hilbert transform and the
real part taken as the output. For x(n) = cos(w n), xa(n) = exp(j w n): only the positive
frequency survives. With g = exp(j phi) the output is cos(w n + phi), and a shift f moves the
tone to f_tone + f. Tests pin these signs.

### 3.2 Why a Hilbert FIR at the native rate

| Option | Verdict |
|---|---|
| **Hilbert FIR at 48 kHz (chosen)** | E.4.2 allows it. No resampling, so a channel chain's 48k -> 8k -> 48k loss of -0.59 dB at 3.2-3.3 kHz and its 8 kHz Nyquist limit are gone. It is wideband up to fs/2 - 250 Hz, which serves WBHF and lets the rate be a parameter. A fixed path is the exact identity (3.6). Cost: 108 multiplies per frame. Its 4.48 ms is the stage's whole share of the E.4.4 budget (3.4). |
| Complex baseband: mix down, low-pass and decimate, run the TDL at a low rate, interpolate, mix up | Rejected. The design needs delays exact to one 48 kHz sample, so the TDL must run at 48 kHz, and the low rate saves no CPU. It also needs two resampling filters, each comparable to the Hilbert, to keep 300-3300 Hz flat and images below -70 dBc. It ties the stage to one band centre and width, and it adds about the same latency. |
| Overlapped-FFT Hilbert (E.6.2's alternative) | Rejected. The latency is at least one FFT block, the arithmetic depends on block alignment, and it fits the low-latency block mode (blocks of 96 to 448 frames, 3.4) badly. |

### 3.3 Hilbert FIR design (computed)

| Item | Value | Source or reason |
|---|---|---|
| Form | Type III: odd length N_H = 4k + 3, antisymmetric, even-offset taps zero, end taps non-zero | linear phase with an integer delay |
| Design band edge | 250 Hz (and fs/2 - 250 Hz) | the 3 kHz band's 300 Hz edge less a 50 Hz guard (E.6.2) |
| Mirror-image target | at least 70 dB below the wanted component | criterion 4's -70 dBc spur level |
| Kaiser design attenuation | A = 72 dB, beta = 0.1102 (A - 8.7) = 6.976 | Kaiser (1974) |
| Length | (A - 7.95) / (2.285 dw) + 1 with dw = 2 pi (2 x 250 Hz) / fs, rounded up to 4k + 3: N_H = 431 | Kaiser (1974); the half-band prototype's transition is twice the band edge |
| Delay | D = 215 frames = 4.479 ms | (N_H - 1) / 2 |
| Work | 216 non-zero taps, 108 multiplies per frame (antisymmetric pairs) | |
| Magnitude error, f >= 300 Hz | 4.1e-4 (0.0036 dB) | computed |
| Mirror image, f >= 300 / 250 / 400 Hz | -73.8 / -71.6 / -76.1 dBc | computed |

The mirror image is the residual negative-frequency part of xa, about (|H| - 1) / 2 in amplitude. A
shift moves it to the opposite side of the carrier. Without a shift it falls inside the Doppler
lobe and does no harm. The coefficients are computed at configure time with det_math (sin and an
I0 series, section 9.2) into the channel object, so host and board hold the same values.

### 3.4 Group-delay compensation and latency

The in-phase branch x(n - D) is read from the centre tap of the same history buffer that the
Hilbert FIR reads, so I and Q are aligned exactly, with no separate compensating delay line. Every
path refers to this delayed analytic signal, so the delay-0 ("direct") path comes out D frames
late, and the relative delays between paths are exact.

- The stage latency is the constant D = 215 frames (4.479 ms). It is independent of block size
  and of the paths, and the stage reports it (`latency`). pathsim's latency is 174 frames at
  48 kHz (reference calibration A4). The remaining E.4.4 budget belongs to the complete player and interface path.
- At the edges of the stream, the first D output frames are the channel's response to silence (the
  history starts at zero), and the last D input frames are still inside the stage when the stream
  ends. The engine keeps frames_out = frames_in, as pathsim does.
- A later stage's window is counted in input frames, as today. The Watterson stage moves the signal
  in time by D, but AWGN, CW and impulse stages after it place their own components at the frame
  they name.
- The reference's delay analyzer measures tap delays relative to the first tap, so D does not enter the
  delay criterion. Where the analyzer falls back to `--offset` (as pathsim's 5 ms case did), the
  offset is 215.

**Stream latency is a separate target measurement.** The stage contributes its algorithmic delay D; a player path also includes block fill, processing, buffering, and interface delay. Measured end-to-end latency must include block fill, processing, buffering, and interface delay. Host tests do not establish this target result.
### 3.5 Analytic band

The Hilbert transform is accurate from 250 Hz to fs/2 - 250 Hz. A faded or shifted component
outside that range keeps an unsuppressed mirror, so input content below 250 Hz lies outside the
model (E.6.2). This covers the 3 kHz waveforms (300-3300 Hz), and WBHF up to 15 kHz at 48 kHz
(E.5.1, survey summary item 6).

### 3.6 The fixed path is the identity

One fixed path at 0 ms, 0 dB and 0 degrees gives g = 1 + 0j exactly. Its normalised power is
p = w / w = 1 and its interpolation correction is 1. det::cos(0) = 1 and det::sin(0) = 0, and the
rotator is skipped when the shift and the phase are both zero. So y(n) = 1 * x(n - D) - 0 * xh(n)
= x(n - D), bit for bit. E.7.1's response is then flat by construction. tests check this, and it is
the strongest check of the plumbing.

### 3.7 The sample rate is a parameter

The engine runs at `signal_lab::sample_rate_hz` (48 kHz). Every size is a constexpr function of a
`SampleRate` constant:

- the delay-ring capacity, bit_ceil(fs x 20 ms + analytic_chunk_frames) (4.2);
- the Hilbert length, which is proportional to fs (E.6.2): about 861 taps at 96 kHz;
- the Doppler FIR size, which does not depend on fs (5.2).

`static_assert`s check each one for the configured rate. Another rate is a compile-time change, as
the survey recommends (gap item G-B3), and never a runtime one.

## 4. Paths

### 4.1 Count

`watterson_max_paths = 4` is fixed at compile time. Its sources: 110D D.6.2 Table D-LXV (at most 4
paths), MIL-STD-188-110D Table D-LXV (at least 4), lane amendment 1, and survey item G-B2. Lane criterion 7 asks for at least 3.
Validation refuses more than 4 paths. Nothing grows at run time.

### 4.2 Delay

- **Rounding.** L_i = round_half_even(delay_ms x fs / 1000) frames, rounded to the nearest sample
  (E.5.2). Every delay in the 110 tests and in F.1487 is a whole number of 48 kHz frames: 0.5 ms is
  24 frames, 2 ms is 96, 9 ms is 432. The stage reports the realised delay in frames.
- **Range.** 0 to 20 ms, which is 960 frames. The task text asked for 7 ms. The binding inputs ask
  for more: D.6.2 Table D-LXV needs 9 ms (MIL-STD-188-110D Table D-LXV), and the survey asks for 20 ms of design
  headroom (gap item G-A1; F.1487 Annex 2 sweeps 0-20 ms). The design uses 20 ms.
- **Storage.** A ring of analytic samples (complex float) with capacity 1024 frames, which is
  8 KiB, indexed with a mask. The ring holds samples after the Hilbert, so its length covers only
  the path delays. The Hilbert's D sits in the Hilbert's input buffer, not in the ring.
- **Order of work, and why the ring is big enough.** The stage works in chunks of
  `analytic_chunk_frames` = 64 frames. For each chunk it (1) appends the 64 input frames to the
  Hilbert's linear input buffer (430 frames of history plus the chunk), (2) computes xh for the 64
  frames and writes xa(n) ... xa(n + 63) into the ring, and only then (3) runs each path over the
  chunk, reading xa(n + k - L_i), and adds the terms into the output in path order. Step 2
  overwrites the ring slots of frames n - 1024 ... n - 961. The oldest frame step 3 reads is
  n - 960 (k = 0, L = 960), so nothing still needed is overwritten. The rule is

  ```text
  analytic_ring_frames >= max_delay_frames + analytic_chunk_frames      1024 >= 960 + 64 at 48 kHz
  ```

  and a `static_assert` states it. 64 is the largest chunk that keeps the 1024-frame ring; a
  256-frame chunk would need a 2048-frame ring (+8 KiB). Frame-by-frame processing is the special
  case of a 1-frame chunk. a component test runs a 960-frame path at every chunk phase.
- **Independence.** Each path reads its own lag. Delays need not be ordered, and two paths may share
  a delay (4.7).

### 4.3 Power

Each path has a relative `power_db` in [-60, +60] dB, held as a `units::Gain` (9.1). Section 6
normalises the set to a total of 1.
Equal powers are the default and the D.6.2 rule. D.6.2's exception, Table D-LXV's 48 kHz WID 12 (a
second path 3 dB down), is written as power_db -3.

### 4.4 Fading kind

- `rayleigh` (the default): the spread d_i is in [0.1, 10] Hz, measured as 2 sigma of a Gaussian
  Doppler PSD with sigma = d/2 (E.5.4). The upper end is E.7.3's 10 Hz test. The range is
  criterion 7.
- `fixed`: a path that does not fade, the specular or ground-wave tap of F.1487 section 3. Table
  XVI's "1 fixed" row is one fixed path, and the D.6.2 static channels are 2 to 4 fixed paths.

### 4.5 Shift

Any path can take `shift_hz` in [-75, +75] Hz. The sources are 110B C.6.3 and 110D D.6.4 (+/-75 Hz),
F.1487's per-component shift and pathsim's `--offset`. separate global frequency offset's global offset and sweep are
separate stages (gap items G-A4 and G-A5). A fixed path with a shift is also F.1487's specular tap with
an offset.

- **Phase.** An integer increment inc_i = round_half_even(f_i x 2^32 / fs). The phase at frame n is
  (n x inc_i) mod 2^32, exact for any 64-bit n. The step is fs / 2^32 = 11.2 uHz, and the realised
  shift (within 5.6 uHz) is reported.
- **Rotator.** r_i is held in double precision. At every absolute frame with n mod 1024 = 0, it is
  set exactly from the integer phase with det::cos and det::sin. In between, it is multiplied by
  the step exp(j 2 pi inc / 2^32), which is computed once. The relative error after 1023 steps is
  below 1e-12 (-240 dB), so the resync leaves no measurable spur.
- **Zero shift.** A path with zero shift and zero phase skips the rotator. This is a configure-time
  property, so the output does not depend on it.

### 4.6 Phase of fixed paths

D.6.2 does not specify a relative phase for static paths. This design uses:

- zero relative phase by default;
- a per-path `phase_deg` on fixed paths;
- a phase sweep, which is a small `shift_hz` on a fixed path. For example, 0.01 Hz turns the
  relative phase once every 100 s.



### 4.7 Rician paths

A Rician path is a fixed path and a Rayleigh path at the same delay. Its K-factor is their power
ratio, and the specular part may carry its own shift (F.1487 section 3). No separate kind is needed,
and validation allows equal delays. The reference's "fixed-path option for the direct path" is a fixed
path at 0 ms, with Rayleigh paths beside it.

### 4.8 Coverage examples

| Channel | Paths (delay ms, kind, spread Hz, power dB) | Source |
|---|---|---|
| AWGN through the stage ("1 fixed") | (0, fixed) | Table XVI, E.7.2 |
| CCIR / ITU-R Poor | (0, rayleigh, 1, 0), (2, rayleigh, 1, 0) | D.6.1, Table XVI, F.1487 |
| Table XVI stress rows | 2 ms at 0.5, 1 and 5 Hz; 5 ms at 1 and 5 Hz (two equal Rayleigh paths) | MIL-STD-188-110D Table D-LXV (first battery) |
| E.7.3 | single 1 Hz; dual 2 ms 1 Hz; single 10 Hz; dual 2 ms 10 Hz | E.7.3 |
| D.6.2, 3 kHz WID 10 | (0, fixed), (2.0, fixed), (4.5, fixed) | Table D-LXV |
| D.6.2, 6 kHz WID 10 | (0, fixed), (1.5, fixed), (3.0, fixed), (5.0, fixed) | Table D-LXV (4 paths) |
| D.6.2, 3 kHz WID 2 | (0, fixed), (3.0, fixed), (9.0, fixed) | Table D-LXV (9 ms) |
| D.6.2, 48 kHz WID 12 | (0, fixed, -, 0), (1.5, fixed, -, -3) | Table D-LXV exception |
| Rician direct | (0, fixed, -, 0), (0, rayleigh, 1, -10), (2, rayleigh, 1, -10) | F.1487 section 3 |
| Shift hold | (0, rayleigh, 1, 0, shift +75) | C.6.3, D.6.4 |

## 5. Tap processes

### 5.1 Generator chain

The chain for each Rayleigh path i:

```text
PCG32 stream (seed, family 9, 4 s + i)  ->  complex Gaussian w[m]          one complex draw per node
w[m]  ->  Gaussian FIR c_k at the node rate R_i                            h_i[m] = sum_k c_k w[m - k]
h_i[m]  ->  linear interpolation to 48 kHz (M_i frames per node)           h_i(n)
h_i(n) * a_i * r_i(n)  =  g_i(n)
```

The Gaussians come from Box-Muller (Box and Muller, 1958), through one primitive that both the
tap processes and the redesigned AWGN (7.2) use:

```cpp
// det_gaussian.hpp. A pure function of two PCG32 draws, so tests can inject any draw.
struct GaussianPair { double first{}; double second{}; };   // independent, each N(0, 1)
[[nodiscard]] GaussianPair gaussian_pair(std::uint32_t radius_draw, std::uint32_t angle_draw) noexcept;
// u1 = (as_double(radius_draw) + 1.0) * 0x1p-32    in [2^-32, 1]: widened BEFORE the + 1
// u2 = as_double(angle_draw >> 8U) * 0x1p-24       in [0, 1), the same 24 bits as uniform()
// r  = sqrt(-2 ln u1) <= 6.660;  first = r cos(2 pi u2), second = r sin(2 pi u2)
```

- **The widening is part of the specification.** `next()` returns `std::uint32_t`, so
  `next() + 1` written literally wraps to 0 for 0xFFFFFFFF; u1 would be 0, `det::log(0)` returns
  -1e300, and the radius would be 1e150. The draw comes once in 2^32: for the AWGN's 24,000 u1
  draws per second that is once per 49.7 h on average, 9.6 % of 5 h rows (computed). `as_double`
  is the exact widening in `convert.hpp` (9.1), and the sum is then exact in double. a component test injects
  0xFFFFFFFF and 0.
- **Variance per component is 1.** Each real output has unit variance. The tap process takes
  w = (first + j second) x `inverse_sqrt2`, so E|w|^2 = 1 (radius sqrt(-ln u1) <= 4.710); the noise
  stage uses the pair unscaled (7.2).
- exactly two PCG32 draws per pair, taken in two separate statements (radius first);
- det::log, det::sin, det::cos and IEEE sqrt.

Why Box-Muller:

- **Fixed draw count.** Each pair takes exactly two draws, so the loop is bounded (criterion 8).
  pathsim's Marsaglia polar method rejects and redraws.
- **Exact Gaussian.** The existing `Pcg32::gaussian()` (Irwin-Hall, the sum of 12 uniforms) has
  an excess kurtosis of -0.1. Through an 89-tap FIR that would show in the envelope statistics.
  The u1 floor of 2^-32 bounds |x| at 6.66 sigma, where a true Gaussian has a two-sided tail of
  2.7e-11: far below anything a battery can see.
- **Cost.** At the tap rate it is negligible: at most 640 pairs per second per path.

The Gaussian helper is part of the Watterson implementation; the generator uses its existing random stream interface.

### 5.2 Doppler filter: E.5.4 in its prose form

```text
c_k  proportional to  exp(-pi^2 d^2 (k / R)^2) = exp(-pi^2 (k / K)^2),   |k| <= L,   K = R / d
L = ceil(tau R),  tau = sqrt(ln 100) / (pi d) = 0.6831 / d        outermost tap <= 1 % of peak (E.5.4)
N = 2 L + 1;  sum c_k^2 = 1                                        unit power at the node rate
```

- The prose-derived filter has 45 taps at K = 32 and 89 at K = 64. This design matches: 89 at
  K = 64, with the end tap at 0.94 % of peak.
- **The literal-formula trap is avoided.** The printed time-domain formula, read literally as
  exp(-pi^2 d^2 t^2 / 2), gives sigma = 0.354 d. The prose form gives sigma = d/2. Computed for the
  realised FIR, including the interpolator's droop:
  - 2 sigma by a fit to the top 30 dB of the lobe: 1.0006 d;
  - 2 sigma by the second moment over +/-4 sigma: 0.9992 d.
- a component test fails if sigma is not d/2, and it carries the trap form as a negative control.
- The coefficients are computed at configure time with det::exp, then stored as float in the
  channel object, symmetric, as 46 half-coefficients.

### 5.3 Tap rate

| Quantity | Rule | Value at 48 kHz |
|---|---|---|
| Minimum rate multiple | K_min = 64 | survey design input; E.5.4 requires at least 32 |
| Frames per node | M_i = floor((fs / K_min) / d_i), with fs / K_min = 750 exact | 7500 at 0.1 Hz ... 75 at 10 Hz |
| Node rate | R_i = fs / M_i | 6.4 Hz ... 640 Hz |
| Realised multiple | K_i = R_i / d_i | in [64, 64.86) over 0.1-10 Hz |
| FIR length | N_i = 2 ceil(0.6831 K_i) + 1 | 89 or 91; compile-time capacity 91 (`static_assert`) |

Node m sits at the absolute frame m x M_i. The reference's own numpy reference, refchan, uses the same
integer-grid rule at 32x. A d given as a double makes M a deterministic IEEE quotient, the same on
every target. K is at least 64 to within one ulp, and a component test allows exactly that.

### 5.4 Interpolator: linear, which is a polyphase interpolator with M phases

```text
h_i(n) = h_i[m] + (j / M_i) (h_i[m + 1] - h_i[m]),     m = floor(n / M_i),  j = n - m M_i
```

- **Exact phase.** mu = j / M is exact rational: j and M are integers, and nothing accumulates.
- **Images.** The triangle prototype's response, sinc^2(f / R), has a double zero at every k R. At
  K = 64 the Gaussian lobe has sigma = R / 128, so each image of the lobe sits deep in a notch.
- **Look-ahead.** Node m + 1 is computed one node ahead. The tap process is random, so the
  look-ahead does not matter.

Computed. The table gives the 48 kHz PSD of the node process through the triangle, against the
lobe's power within +/-4 sigma:

| K | Images at R / 2R / 3R (dBc) | All images (dBc) | Largest truncation sidelobe | Table E-I -20 / -30 dB points | 2 sigma fit |
|---|---|---|---|---|---|
| 32, the E.5.4 minimum | -67.5 / -79.6 / -86.6 | -64.1 | -59.4 dB re lobe peak at 3.32 d; -65.1 dBc in +/-0.25 d | -0.12 / +0.25 dB | 0.9996 d |
| **64, this design** | **-79.6 / -91.6 / -98.6** | **-76.2** | **-58.3 dB at 3.36 d; -64.0 dBc** | **-0.09 / +0.34 dB** | **1.0006 d** |
| 64.85, the worst K in range | -79.8 / -91.8 / -98.9 | -76.4 | -59.1 dB at 3.33 d; -64.7 dBc | -0.08 / +0.33 dB | 1.0008 d |

Measured simulators, for comparison:

| Simulator | Images | Source |
|---|---|---|
| IONOS (sample-and-hold) | -42 dBc | lane |
| refchan (linear at 32x) | -67 dBc; matches the K = 32 row | reference calibration case |
| pathsim (x5 stages from 12.8/64/320 Hz) | images -60 to -65 dBc; truncation sidelobes -66.7/-67.7 dBc | reference calibration A3 and appE E.7.4 |

The survey's -64 / -76 dBc for 32x / 64x is confirmed. Table E-I allows +/-1.5 dB at -20 dB and
+/-2.0 dB at -30 dB, so the designed shape uses less than a fifth of that.

These figures are a property of the coefficients. An implementation defect would show at the same
frequencies (a node update a frame late, a hold-and-step at node boundaries, an off-by-one in m or
j), and for d above about 2.1 Hz the first image (R = 64 d, 135 Hz at 2.1 Hz) lies outside what
the reference's `analyze_doppler` can see: it low-passes at 150 Hz (-6 dB) before decimating to 500 Hz
and scans spurs only within +/-135 Hz. So the implemented interpolator is checked twice on its own
output: a component test compares the stage's 48 kHz gain with the reference interpolation of its own nodes,
bit for bit in float order, at M = 75, 150 and 7500; and the host validation measures the images k R, k = 1 to 3, on
the 48 kHz render directly (see the [validation method](watterson-validation.md)).

**A multistage interpolator was considered.** A x5 (or x15) polyphase Kaiser stage before the
linear one would put the images below -100 dBc, at about 1.5x the node-rate cost. It is not
adopted, because:

- linear interpolation at K >= 64 is already 16-20 dB inside -60 dBc;
- it is 12-16 dB below E.5.4's own truncation sidelobes, which no interpolator can remove (5.5);
- a second stage adds a second state machine and draw-to-output latency for no measurable gain.

It stays the upgrade path if future requirements call for images below -80 dBc.

### 5.5 The sidelobes belong to E.5.4

Rectangular truncation at 1 % sets the sidelobes. Whether they meet criterion 2 ("everything
outside the main lobe below -60 dBc") depends on how the criterion is read, and the two readings
give opposite verdicts:

| Truncation | Taps (K = 64 / largest K) | Largest sidelobe, per spur (+/-0.25 d) | Lane `analyze_doppler`, largest spur | Integrated beyond +/-3 d (6 sigma) |
|---|---|---|---|---|
| ideal Gaussian | - | - | - | -87.0 dBc |
| **1 % (this design)** | 89 / 91 | -64.0 dBc at 3.36 d; -64.7 at the largest K | -64.5 to -64.8 dBc at 4.02-4.10 d | **-53.6 dBc; -54.3 at the largest K** |
| 0.1 % | 109 / 111 | -87.1 dBc at 3.92 d | -79.5 to -80.2 dBc | -74.7 dBc; -75.3 at the largest K |

(Computed, except the analyzer column: *measured (reference analyzer)*, `analyze_baseband` on the
designed process for d = 0.5, 1 and 2 Hz, 600-1200 s, no noise.)

- **Per spur** (the analyzer's metric: 5 Welch bins around a peak against the lobe's power), the
  1 % design passes with 4.5-4.8 dB to spare. The 3.36 d sidelobe falls inside the analyzer's
  exclusion of +/-(6 s + 3 df), so the analyzer reports the next one, at about 4.04 d.
- **As total power** outside the lobe, the 1 % design fails by about 6 dB on every Rayleigh row, and
  so would pathsim and refchan, by the same mechanism. Only a lower truncation level meets it.
- Truncating at 0.1 % (tau = 0.8366 / d) also satisfies E.5.4's "<= 1 % of peak at +/-tau". Over
  the whole spread range it needs a capacity of **111 taps** (2 ceil(0.8366 x 64.853) + 1, for d
  just above 750/76 Hz), not 109. The cost is 360 B per path (10 more half-coefficients and 20 more
  complex history pairs), 1,440 B for 4 paths, and 10 more symmetric pairs per node update. The
  spread does not move measurably: the reference analyzer read 2 sigma = 0.9995-1.0067 d at 1 % and
  1.0000-1.0019 d at 0.1 %, both within its own scatter.

The criterion lists "1 % truncation" as a design target and "-60 dBc outside the main lobe" as a
criterion; under the total-power reading they conflict. The design keeps 1 % and sizes everything
for either (the 20 KiB budget holds 111 taps). The applicable criterion interpretation remains unresolved.

### 5.6 Interpolation variance

Linear interpolation of a correlated process loses a little variance in the middle of each
interval. With lag-1 correlation rho1 = sum c_k c_(k+1) = 0.998793 at K = 64, the loss is
-0.0026 dB at mu = 1/2 and -0.0017 dB on average (1 - v = 4.02e-4). Section 6 corrects it exactly.
The periodic part is inside the image figures above. The correction is far below anything a
statistical test can resolve (the standard error of mean |g|^2 over 2^22 nodes at 1 Hz is
2.9e-3), so a a deterministic component test checks it.

### 5.7 Start-up and draw accounting

configure() fills each FIR history with N fresh draws and computes nodes 0 and 1. The taps are
stationary from frame 0, with no warm-up (pathsim pre-rolls 4 s). Each Rayleigh path draws N + 1
complex samples at configure time, then exactly one per node. Fixed paths draw nothing.

### 5.8 Cost of the tap processes (estimate)

- **Per node update:** about 800 cycles. Box-Muller is about 400 (det::log, det::sin, det::cos,
  sqrt in double). The FIR is 46 symmetric pairs, twice over (real and imaginary).
- **Per output frame:** at M = 75 (10 Hz) about 11 cycles per path; at 1 Hz about 1.
- **Memory per path:** 46 half-coefficients (184 B), a mirrored complex history of 2 x 91 (1,456 B)
  and about 128 B of state.

## 6. Normalisation

```text
p_i = 10^(P_i / 10) / sum_j 10^(P_j / 10)                          sum p_i = 1   (E.5.4: k sets the total power gain to 1.0)
Rayleigh:  a_i = sqrt(p_i / v_i),
           v_i = (1 / M_i) sum_{j=0}^{M_i - 1} [ (1 - mu_j)^2 + mu_j^2 + 2 mu_j (1 - mu_j) rho1_i ],   mu_j = j / M_i
               = 1 - (1 - rho1_i) (M_i^2 - 1) / (3 M_i^2)                                     (closed form)
Fixed:     a_i = sqrt(p_i)
```

- **v_i** is the exact mean power of the interpolated tap for a unit-energy c. The stage uses the
  closed form (no loop over M_i, which reaches 7500); it equals the brute-force average to 1e-16
  (computed). rho1_i = sum c_k c_(k+1) / sum c_k^2 is computed in double from the stored float
  coefficients, so v_i describes exactly the filter that runs. a_i is folded into c, so no extra
  multiply runs per frame.
- **The scale is fixed.** It is computed once, in double, at configure time, and depends only on
  the parameters. There is no level tracker. pathsim pins its output at -18.27 dBFS from a running
  RMS. The stage keeps the scenario's level.
- **Output power.** For Re{g xa}, E[y^2] = 1/2 E|g|^2 E|xa|^2 = p P_x. So E[y^2] = (sum p_i) P_x
  = P_x for independent paths: the channel keeps the mean power.
- **Fixed paths.** Their cross terms, 2 a_i a_j R_x(L_i - L_j) cos(...), vanish once the delay
  difference exceeds the signal's correlation time. The static-multipath notches that remain are
  the intended physics.
- **SNR.** E.5.5 refers SNR to the input. The AWGN stage's reference (StageContext.reference_rms,
  the scaled source RMS) is therefore unchanged by the channel. The delivered output SNR under
  fading is snr_db + 10 log10(realised tap power), the decomposition used for pathsim calibration
  (calib A2).

## 7. Noise

### 7.1 How the stage relates to AWGN

The Watterson stage adds no noise. AWGN stays a separate stage after it, and validation enforces
that order, since E.4.1 adds the noise after the channel. Each draws from its own stream: AWGN from
family 1, Watterson from family 9 (E.5.3). AWGN measures its reference from the input power
(E.5.5).

### 7.2 AWGN design rationale

The design targets a flat 300â€“3300 Hz band and defines the requested SNR against N0 Ã— 3000 Hz. Its calculated equivalent noise bandwidth is 3233.4 Hz; the details and scaling are below.

This calibrated design uses a consistent noise definition across host and target renders. It intentionally changes the legacy AWGN output and has no compatibility switch.

1. **Noise-band kernel.** Kaiser window, A = 60 dB (beta = 5.653), cutoffs 150 and 3450 Hz (300 Hz
   transitions outside the band), N = 583, unit gain at 1800 Hz. Computed:
   - flat to -0.009 / +0.010 dB over 300-3300 Hz;
   - ENBW 3233.4 Hz;
   - |H|^2 at DC -56 dB, at 100 Hz -13 dB, at 150 Hz -6 dB, at 250 Hz -0.4 dB;
   - at or below -59.6 dB from 3.6 kHz up;
   - 292 multiplies per frame, against 257 today.

   The kernel is computed with det_math at configure time.
2. **Scale referred to N0.** The excitation e(n) is white with unit variance per real sample
   (item 3). sigma_w = R 10^(-SNR/20) sqrt(fs / (2 x 3000 Hz)). The one-sided in-band density is
   N0 = 2 sigma_w^2 / fs, so N0 x 3000 Hz = R^2 10^(-SNR/10) exactly. The total noise is
   N0 x 3233.4 Hz, 0.325 dB above N0 x 3000. The band integral over 300-3300 Hz is
   N0 x 3000 x (1 +/- 0.002). The sidecar reports the ENBW.
3. **Box-Muller excitation.** `gaussian_pair()` from 5.1, unscaled: `first` goes to frame 2k and
   `second` to frame 2k + 1, each with unit variance (radius sqrt(-2 ln u1)). The tap processes
   scale the pair by 1/sqrt(2) (5.1); that scaled form, used here, would put the delivered SNR
   3.01 dB above the label (computed), which the calibrated design avoids.
   Pairs align to absolute even frames, and the odd half is carried across a block boundary. A component test
   checks the pair formula on injected draws and the unit variance on 2^22 samples.
4. **Unchanged.** The chunked FIR with retained history, family 1, and the window semantics. The
   impulse stage's ring noise keeps the existing `BandFir`, because it is not a calibrated noise and
   no criterion applies to it (separate noise behavior).
5. **Typed implementation.** AWGN uses typed parameters and bounded storage. The noise stage is separate from the impulse stage's ring noise, which is not calibrated AWGN. Comparisons with external analyzers require those tools to follow the same calibrated kernel and SNR convention.
### 7.3 SNR convention

| Measure | Value |
|---|---|
| N0 x 3000 Hz (primary, E.5.5) | snr_db exactly; offset 0 |
| Band integral over 300-3300 Hz | within 0.01 dB of snr_db |
| Total noise over the full band | 0.325 dB lower SNR (ENBW 3233.4 Hz) |
| pathsim, for comparison | label + 0.42 dB (ENBW 3304.5 Hz, calib A1) |

E.5.5 refers SNR to the "average power of input x", which is the scenario's reference. `"auto"`
trims leading and trailing exact zeros. Probes and modem cards use an explicit interval or a
`reference_rms`. This avoids the reference's 1 s pad trap (calib A1).

## 8. Determinism

- **Inputs to a realisation.** The scenario seed (64 bits), the stage index s, the path index i and
  the absolute frame n. Nothing else: no clock, no address, no uninitialised memory.
- **Streams.**
  - Path i of stage s draws from `Pcg32(seed, 9, 4 s + i)`. The families in use are: 1 AWGN,
    2/4/5 impulse, 7 CW, 8 fade, 10 the sample clock's walk (appendix A), 0x4C53000x live.
  - Fixed paths draw nothing.
  - Adding, removing or changing one path never changes another path tap sequence (covered by component tests).
  - The noise and fading streams are independent (E.5.3).
- **Draw order.** Each statement takes at most one draw, radius before angle. This is pathsim's
  lesson: x86 and Arm GCC evaluated a draw pair in opposite orders ([pathsim.md](../modules/pathsim.md),
  "Draw order").
- **Block independence.** Every state change is keyed to the absolute frame:
  - node updates at n mod M_i = 0;
  - rotator resyncs at n mod 1024 = 0;
  - the Hilbert and the ring are per-sample FIR states.

  The order of summation for each frame is fixed: paths 0 to 3, each term
  (Re g x) - (Im g xh), all in float. Internal chunks of 64 frames (4.2: the ring is written for the
  whole chunk before any path reads it) change no arithmetic. The stage checks that
  `first_input_frame` equals its next frame, which is the engine's cursor contract, and latches a
  fault if it does not (9.5). The output is therefore identical for any partition into blocks of 1
  to 2048 frames (component test), including block sizes from 1 to 2048 frames.
- **Host and board.** Both use:
  - IEEE binary32 and binary64 with `-ffp-contract=off -fno-fast-math` (signal-lab's options;
    `/fp:strict` under MSVC);
  - det_math for exp, log, sin and cos; IEEE sqrt; integer phase;
  - no libm, no FMA and no reassociation;
  - the FPSCR defaults: round to nearest and FZ = 0. Values never come near the subnormal range.

  The design target is a bit-exact host = board, stronger than the reference's statistical criterion.
  It is verified by the digest of the same render on both targets; this remains a separate cross-target check.
- **Board-vs-board exactness across a reset.** configure() rewrites the whole channel object, so no
  state survives, and garbage left in NOLOAD OCRAM cannot leak into the output. a component test poisons the
  object before configure() to show this.
- **64-bit frame counter** (gap item G-A8). The node index n / M is held in 64 bits, and the phase
  (n x inc) mod 2^32 is exact for any n.

## 9. Types, constants, schema and API

### 9.1 Strong unit types

Neither signal-lab nor common has unit types. channel-sim has one-field aggregates of its own
([units.hpp](../../channel-sim/core/units.hpp)), in a separate project that signal-lab must not
depend on. One-field aggregates with a public value are also not enough: one `Decibels` for both a
path level and an SNR lets `power_db + snr_db` compile and mean nothing. The
stage uses separate unit types in `signal-lab/include/signal_lab/units/`,
namespace `signal_lab::units`:

- every value is private; a type is built with a unit tag (`Frequency{300.0, Hz}`,
  `Duration{2.0, ms}`, `Ratio{18.0, dB}`) and read with `in(unit)`; `from_hz()`, `from_db()` and
  the like name the canonical raw form; `operator<=>` is defaulted;
- the algebra lives in the types, so an expression that means nothing does not compile;
- storage is double for these configuration values; linear conversions go
  through det_math.

| Header | Types | Algebra (anything not listed does not compile) | Used for |
|---|---|---|---|
| `units/decibel.hpp` | `Decibel<Tag>`: `Level` (absolute, dBFS), `Gain` (relative), `Ratio` (dimensionless) | Level +/- Gain -> Level; Level - Level -> Ratio; Level +/- Ratio -> Level; Gain +/- Gain -> Gain; Ratio +/- Ratio -> Ratio; -Gain, -Ratio. Level + Level, -Level and Gain +/- Ratio are not defined, so a path level plus an SNR cannot be expressed | `Gain`: path level, source gain; `Ratio`: SNR, the Kaiser attenuations, the reported K-factor; `Level`: the reference and reported levels |
| `units/frequency.hpp` | `BasicFrequency<Kind>`: `Frequency` (a point), `FrequencyDelta` (a signed difference) | Frequency - Frequency -> Delta; Frequency +/- Delta -> Frequency; Delta +/- Delta -> Delta; Delta x or / scalar -> Delta; Delta / Delta -> double; -Delta. Frequency + Frequency, scaling a Frequency and comparing across kinds are not defined | `Frequency`: band edges (300, 3300 Hz), the 1800 Hz reference, the 250 Hz Hilbert edge, tones; `FrequencyDelta`: spread (2 sigma), shift, the SNR bandwidth (3300 Hz - 300 Hz = 3000 Hz), ENBW, node rate R |
| `units/duration.hpp` | `Duration` | Duration +/- Duration; x or / scalar; Duration / Duration -> double | path delay; latency as time |
| `units/sampling.hpp` | `SampleRate`, `SampleCount`, `SampleIndex` | Index - Index -> Count; Index +/- Count -> Index; Count +/- Count -> Count; Index + Index not defined | the configured rate; realised delays, M_i, latency in frames; the absolute 64-bit frame |
| `units/angle.hpp` (same shape) | `Angle` (degrees, radians) | Angle +/- Angle; -Angle | fixed-path phase |

The conversions are one function each:

```cpp
[[nodiscard]] SampleCount to_frames(Duration delay, SampleRate rate) noexcept;      // E.5.2: nearest frame, ties to even
[[nodiscard]] Duration to_duration(SampleCount frames, SampleRate rate) noexcept;
[[nodiscard]] double power_ratio(Gain gain) noexcept;                                 // 10^(dB/10) through det::exp; Ratio has the same pair
[[nodiscard]] double amplitude_ratio(Gain gain) noexcept;                             // 10^(dB/20): field form, a helper rather than a type
[[nodiscard]] PhaseStep phase_step(FrequencyDelta shift, SampleRate rate) noexcept;   // round(f 2^32 / fs), two's complement, private uint32
```

a compile-time check holds the negative checks at compile time, through a concept (a requires-expression outside a
template would make an ill-formed expression a hard error):

```cpp
template <class A, class B> concept addable = requires(A a, B b) { a + b; };
static_assert(!addable<Level, Level> && !addable<Gain, Ratio> && !addable<Frequency, Frequency>);
static_assert(!std::is_constructible_v<FrequencyDelta, double>);   // no construction without a unit
```

The parameter structs name each field (`spread`, `shift`). Functions take whole structs, never
loose Hz arguments that could be swapped. Numeric conversions (integer to double, double to float)
go only through a new `signal_lab/convert.hpp`, the pattern of channel-sim's
[convert.hpp](../../channel-sim/core/convert.hpp). The stage has no other cast. The unit headers
and convert.hpp could move to `common/` later; that path belongs to the playlist workflow today.

### 9.2 Named constants and their sources

| Constant | Value | Source |
|---|---|---|
| `watterson_max_paths` | 4 | 110D D.6.2 Table D-LXV (at most 4); MIL-STD-188-110D Table D-LXV; survey recommendation |
| `watterson_max_delay` | `Duration{20.0, ms}` | survey G-A1; F.1487 Annex 2 (0-20 ms); at least D.6.2's 9 ms |
| `watterson_min_spread`, `watterson_max_spread` | `FrequencyDelta{0.1, Hz}`, `FrequencyDelta{10.0, Hz}` | criteria 2 and 7; E.7.3 (10 Hz) |
| `watterson_max_shift` | `FrequencyDelta{75.0, Hz}` | 110B C.6.3, 110D D.6.4 |
| `watterson_max_relative_level` | `Gain{60.0, dB}` | design range for relative path levels |
| `doppler_sigma_per_spread` | 0.5 | E.5.4 prose (sigma_f = d/2); the prose-derived sigma = d/2 requirement |
| `doppler_truncation_fraction` | 0.01 | E.5.4 (tap gain at +/-tau at most 1 % of peak); 0.001 if the integrated-power interpretation governs |
| `tap_rate_multiple_min` | 64 | survey design input (E.5.4 requires at least 32) |
| `doppler_max_taps` | 91 (111 at 0.1 %) | 2 ceil(tau d x 64 x 76 / 75) + 1, the largest K at the 10 Hz maximum (M = 75); a `static_assert` derives it from the constants above |
| `inverse_sqrt2` | 0.70710678118654752 | E\|w\|^2 = 1 for the tap excitation from a unit-variance pair (5.1) |
| `analytic_chunk_frames` | `SampleCount{64}` | the largest chunk the 1024-frame ring allows (4.2) |
| `analytic_ring_frames` | `SampleCount{1024}` | bit_ceil(max delay 960 + chunk 64); `static_assert` (4.2) |
| `rotator_resync_frames` | 1024 | rotator error budget (4.5) |
| `hilbert_band_edge` | `Frequency{250.0, Hz}` | the 300 Hz band edge less a 50 Hz guard; E.6.2 |
| `hilbert_attenuation` | `Ratio{72.0, dB}` | at least 70 dBc mirror image (criterion 4) |
| `kaiser_beta_slope`, `kaiser_beta_offset_db` | 0.1102, 8.7 | Kaiser (1974), A > 50 dB (the formula takes A as a number of dB) |
| `kaiser_length_offset_db`, `kaiser_length_slope` | 7.95, 2.285 | Kaiser (1974) |
| `kaiser_series_terms` | 32 | I0 series; for beta <= 10 the next term is below 1e-29 relative |
| `watterson_rng_family` | 9 | a PCG32 family no other stage draws from (in use: 1, 2, 4, 5, 7, 8, 10 for the sample clock (appendix A) and the live 0x4C53000x) |
| `noise_band_low`, `noise_band_high` | `Frequency{300.0, Hz}`, `Frequency{3300.0, Hz}` | the reference's SNR band; 110D 5.3.2.5 (3 kHz) |
| `snr_reference_bandwidth` | `noise_band_high - noise_band_low`, a `FrequencyDelta` of 3000 Hz | the reference's N0 x 3000 Hz convention; E.5.5 |
| `noise_band_transition`, `noise_band_attenuation` | `FrequencyDelta{300.0, Hz}`, `Ratio{60.0, dB}` | design (7.2) |
| `noise_band_reference` | `Frequency{1800.0, Hz}` | band centre, E.7.1's reference frequency |
| `max_awgn_stages` | 2 | representative effect chains use one AWGN stage; one spare (7.3, placement estimate) |
| `stage_frame_limit_bytes` | 256 | the stack bound for every function of the new stages (9.9) |

Tests also name their tolerances at the definition: E.7 (0.5, 0.25 and 0.5 dB), Table E-I (1.5 and
2.0 dB), Rayleigh mean/RMS sqrt(pi)/2, and the KS critical values.

### 9.3 Scenario schema

The new stage is one item of `impairments`. Unknown members in the stage or in a path are errors
(the existing stages keep ignoring theirs), so a typo such as `shift` for `shift_hz` cannot silently
become a default.

| Member | Type, unit | Range | Default | Notes |
|---|---|---|---|---|
| `type` | `"watterson"` | | required | at most one per scenario, and it must be the first stage |
| `paths` | array of path objects | 1 to 4 entries | required | |
| path `delay_ms` | number, ms | [0, 20] | 0 | rounded to the nearest frame (E.5.2), ties to even; reported in frames |
| path `power_db` | number, dB, relative | [-60, 60] | 0 | the set is normalised to a total of 1 (E.5.4) |
| path `fading` | `"rayleigh"` or `"fixed"` | | `"rayleigh"` | |
| path `spread_hz` | number, Hz, 2 sigma | [0.1, 10] | required for `rayleigh` | not allowed on `fixed` |
| path `shift_hz` | number, Hz | [-75, 75] | 0 | any path |
| path `phase_deg` | number, degrees | [-360, 360] | 0 | `fixed` paths only |

Validation errors are static messages, from both the parser and `validate_scenario` for
programmatic scenarios:

- `watterson: at most one stage per scenario`
- `watterson: must be the first stage (noise and interference follow the channel, E.4.1)`
- `watterson.paths: required, 1 to 4 path objects`
- `watterson: unknown member`; `watterson.paths[]: unknown member`
- `watterson.paths[].delay_ms: number in [0, 20]`
- `watterson.paths[].power_db: number in [-60, 60]`
- `watterson.paths[].fading: "rayleigh" or "fixed"`
- `watterson.paths[].spread_hz: required for a rayleigh path, number in [0.1, 10]`
- `watterson.paths[].spread_hz: not allowed on a fixed path`
- `watterson.paths[].shift_hz: number in [-75, 75]`
- `watterson.paths[].phase_deg: number in [-360, 360], fixed paths only`
- `watterson.paths[]: value is not finite` (programmatic scenarios)
- at configure time: `watterson: needs a WattersonChannel (StageResources.watterson)`

**Parsing and validation.** The scenario schema and refusal messages are maintained with the implementation; consult `signal-lab/include/signal_lab/scenario.hpp`, the stage parser, and the [module guide](../modules/signal-lab-watterson.md) for current details. The examples below illustrate the design contract:
CCIR Poor at a delivered 18 dB, for example:

```json
{
  "schema": "signal-lab.scenario/1", "test_id": "WAT-POOR-18", "seed": 1,
  "reference": "auto", "source_gain_db": -12, "clip_policy": "reject",
  "impairments": [
    {"type": "watterson", "paths": [
      {"delay_ms": 0.0, "spread_hz": 1.0},
      {"delay_ms": 2.0, "spread_hz": 1.0}]},
    {"type": "awgn", "snr_db": 18.0}
  ]
}
```

D.6.2, 3 kHz WID 10 at 50 dB: `"paths": [{"fading": "fixed"}, {"delay_ms": 2.0, "fading": "fixed"},
{"delay_ms": 4.5, "fading": "fixed"}]`, followed by AWGN at 50 dB.

### 9.4 C++ API sketch

```cpp
// signal-lab/include/signal_lab/watterson_params.hpp (small: scenario.hpp includes it)
namespace signal_lab
{
enum class PathFading : std::uint8_t { rayleigh, fixed };

struct WattersonPath
{
    units::Duration delay{};                 // rounded to the nearest frame (E.5.2)
    units::Gain level{};                     // relative; the set is normalised to a total of 1 (E.5.4)
    PathFading fading{PathFading::rayleigh};
    units::FrequencyDelta spread{};          // two-sigma Doppler spread, rayleigh only
    units::FrequencyDelta shift{};
    units::Angle phase{};                    // fixed only
};

struct WattersonParams
{
    std::array<WattersonPath, watterson_max_paths> paths{};
    std::uint32_t path_count{};
};

// signal-lab/include/signal_lab/watterson.hpp
[[nodiscard]] m110::Status validate_watterson(const WattersonParams& params) noexcept;

struct WattersonPathReport
{
    units::SampleCount delay{};
    units::SampleCount frames_per_node{};    // M_i (0 for fixed paths)
    units::FrequencyDelta node_rate{};       // R_i
    double rate_multiple{};                  // K_i = R_i / d_i, a pure ratio (FrequencyDelta / FrequencyDelta)
    std::uint32_t doppler_taps{};            // N_i, a count of coefficients
    units::Gain gain{};                      // 20 log10 a_i
    units::FrequencyDelta realised_shift{};
};

struct WattersonReport
{
    std::array<WattersonPathReport, watterson_max_paths> paths{};
    std::uint32_t path_count{};
    std::uint32_t hilbert_taps{};
    units::SampleCount latency{};            // D
    std::uint64_t parameter_digest{};        // FNV-1a over algorithm version, rate, seed, stage and realised parameters
};

struct TapRecord                             // channel truth (gap item G-C10): one record per node per path
{
    units::SampleIndex node_frame{};
    std::uint32_t path{};
    float real{};
    float imag{};
};

class TapObserver                      // host diagnostics; never on the board's audio path
{
public:
    virtual void on_tap(const TapRecord& record) noexcept = 0;
protected:
    TapObserver() = default;
    ~TapObserver() = default;          // C.35: protected, non-virtual
    // copy and move: protected and defaulted, as Impairment's are (C.67)
};

// Bulk state, about 17.5 KiB, trivially copyable, no virtual functions. Static
// storage that the platform places (OCRAM2 .ocram.watterson, NOLOAD) and the
// engine borrows. configure() writes every member before process() reads it, so
// the object never depends on startup initialisation.
class WattersonChannel
{
public:
    [[nodiscard]] m110::Status configure(const WattersonParams& params, std::uint64_t seed,
                                         std::uint32_t stage_index, TapObserver* observer) noexcept;
    [[nodiscard]] m110::Status process(std::span<float> block, units::SampleIndex first_frame) noexcept;  // in place
    [[nodiscard]] const WattersonReport& report() const noexcept { return mReport; }

private:
    AnalyticFilter mAnalytic{};                      // Hilbert coefficients and its 430 + 64 frame input buffer
    AnalyticRing mRing{};                            // 1024 complex floats (4.2)
    std::array<PathState, watterson_max_paths> mPaths{};
    std::uint32_t mPathCount{};
    units::SampleIndex mNextFrame{};
    TapObserver* mObserver{};
    WattersonReport mReport{};
};
static_assert(std::is_trivially_copyable_v<WattersonChannel>);
static_assert(sizeof(WattersonChannel) <= watterson_channel_budget_bytes);   // 20 KiB, room for the 111-tap option
static_assert(analytic_ring_frames.value() >= max_delay_frames.value() + analytic_chunk_frames.value());   // 4.2

struct StageResources
{
    WattersonChannel* watterson{};   // borrowed; outlives the engine's use of it
    TapObserver* tap_observer{};     // borrowed; host only
};

// The Impairment adapter, built by the engine in its typed slot (9.5).
class WattersonStage final : public Impairment
{
public:
    WattersonStage(const WattersonParams& params, WattersonChannel& channel, TapObserver* observer) noexcept;
    [[nodiscard]] StageType type() const noexcept override;
    // prepare() calls mChannel->configure(*mParams, context.seed, context.index, mObserver).
    [[nodiscard]] bool prepare(const StageContext& context, const char** error) noexcept override;
    [[nodiscard]] std::size_t process(float* block, std::size_t frames, std::uint64_t first_input_frame,
                                      std::size_t capacity) noexcept override;   // legacy boundary -> std::span
    [[nodiscard]] const StageStats& stats() const noexcept override;
private:
    const WattersonParams* mParams{};
    WattersonChannel* mChannel{};
    TapObserver* mObserver{};
    StageStats mStats{};
};
} // namespace signal_lab
```

The accessor `report()` is defined in the class. The repository's
[clang-format profile](../../.clang-format) owns the layout of these interfaces.

The internal components live in `signal-lab/src/`, each with its own header and suite:

- `include/signal_lab/units/*.hpp`, `include/signal_lab/convert.hpp`: the unit types of 9.1;
- `kaiser.*`: beta, length and the I0 series;
- `analytic_filter.*`: the Hilbert FIR and the analytic ring;
- `doppler_filter.*`: the E.5.4 design, the tap rate and node generation;
- `watterson.cpp`: the channel, the rotator, normalisation, validation, the digest and the adapter;
- `watterson_scenario.hpp` and the parse and validate functions in `watterson.cpp` (9.3);
- `det_gaussian.*`: `gaussian_pair()` on two `det::Pcg32` draws (5.1);
- [Watterson module guide](../modules/signal-lab-watterson.md), which documents the current source layout and runtime behavior.

The components use std::array storage and std::span views for indexed access, and contain no
pointer arithmetic, C arrays, `reinterpret_cast` or `void*`. Their loops are bounded by
compile-time capacities or by the block length.

### 9.5 Storage, lifetime and the engine

The channel bulk state uses fixed-capacity storage supplied by the caller. On RT1170, the channel object is placed in OCRAM2; the typed stage adapter and engine state remain owned by the engine. The adapter receives its channel through `StageResources`, and a scenario that lacks the required resource is rejected during preparation. Watterson and redesigned AWGN stages use typed slots rather than the legacy impairment arena. These boundaries avoid heap allocation and make ownership explicit. The module guide and linker checks describe the current placement contract.
### 9.6 Reporting and identity

- **StageStats.** `events_scheduled` and `events_applied` are 1. The component energy, peak and
  frame count are those of the stage's output.
- **WattersonReport and the sidecar.** The report carries the realised parameters (delays in
  frames, M, R, K, N, gains, realised shifts, Hilbert taps, latency) and `parameter_digest`. The
  digest covers an algorithm version constant, so a change of arithmetic also changes the identity.
  The engine sidecar copies both.
- **Scenario identity.** The realised parameter digest can be associated with playlist items and firmware identity by the player integration.

### 9.7 Channel truth (gap item G-C10)

When an observer is attached, the channel reports every node of every Rayleigh path as a
`TapRecord`. It reports fixed paths once, at configure time. The report gives M, a_i, inc_i, phi_i
and the delays, so the 48 kHz path gains can be rebuilt exactly with 5.4 and 4.5. That is
pathsim's `--truth-out` at the node rate rather than per sample: about 1.15 M records per path for
5 h at 1 Hz.

The host renderer writes them as `signal-lab.truth/1`: a JSON header with the realised parameters,
then little-endian records. The board passes no observer and pays nothing. A bounded ring buffer on
the board can come later.

### 9.8 Embedded implementation constraints

The stages use fixed capacities, bounded loops, deterministic math, and no dynamic allocation. Keep channel state, adapter lifetime, and DMA ownership explicit. Host and offline cross-build checks can verify types, placement, and static stack bounds; they do not measure target runtime margin. Current coding and test gates live in the [testing reference](../testing.md) and module guide.
## 10. Verification

Unit tests cover filter design, path and delay arithmetic, deterministic random streams, interpolation, normalization, scenario refusals, block partition invariance, and channel truth. Run the repository's current host test suite rather than relying on historical test IDs from the original implementation plan.

The optional statistical battery and pathsim comparison are described in [watterson-validation.md](watterson-validation.md). That comparison requires external reference data. Board acceptance separately measures host-to-board parity, reset determinism, end-to-end latency, sustained playback, and analog behavior where applicable. Neither host statistics nor offline cross-builds imply those physical results.
## 11. Budget on the board

### 11.1 CPU

These are original RT1170 planning estimates for a Cortex-M7 at 996 MHz,
not measurements or an RT1064 budget. Recompute for the actual board clock
and measure the combined workload. The estimate assumes:

- single-precision add and multiply at one per cycle, with a 3-cycle add latency, so a
  single-accumulator FIR loop costs about 3 cycles per symmetric pair;
- double-precision operations at 2 to 3 cycles;
- det::log, det::sin and det::cos at 100 to 150 cycles each;
- OCRAM2 reached through the 32 KiB D-cache.

The bench measurement (`engine_max_block_cycles`) replaces them. A 2048-frame block lasts 42.7 ms,
which is 42.5 M cycles.

| Part | Cycles per output frame |
|---|---|
| Hilbert (108 pairs) | about 330 |
| Each path: interpolation, rotator (double), tap times rotator, ring read and term | about 30 |
| Node updates for each path at 10 Hz (M = 75) / at 1 Hz | about 11 / about 1 |
| Loop and chunk overhead (64-frame chunks) | about 20 |
| **Watterson, 4 shifted Rayleigh paths at 10 Hz** | **about 520: 25 M cycles/s, 2.5 %; 1.07 M cycles (1.07 ms) per 2048-frame block** |
| Watterson, CCIR Poor (2 paths, 1 Hz, no shift) | about 400: 1.9 % |
| Legacy AWGN (257 pairs, 12 PCG32 draws per frame) | about 890: 4.3 % |
| AWGN redesigned (292 pairs, Box-Muller per 2 frames) | about 1,110: 5.3 % |
| Sample-clock stage (2 x 16 MACs and a blend per output frame, appendix A) | about 110: 0.5 % |
| Channel chain (4-path Watterson plus redesigned AWGN) | about 1,630: 7.9 %. Bench gate: 10 % or less (8.4 % with the sample clock) |
| Per-block fixed cost at small blocks (3.4) | about 700 cycles per block: 1.4 % of the frame cost at B = 96 (target timing check measures it) |

### 11.2 Memory (48 kHz, 4 paths)

| Object | Bytes | Where |
|---|---|---|
| Analytic ring, 1024 complex floats (960 + 64, 4.2) | 8,192 | OCRAM2 `.ocram.watterson` (NOLOAD) |
| Hilbert input buffer, 430 history + 64 chunk floats | 1,976 | same |
| Hilbert half-coefficients, 108 floats | 432 | same |
| Paths, 4 x (184 coefficients + 1,456 mirrored history + about 128 state) | 7,072 | same |
| Report, digest, control | about 256 | same |
| **WattersonChannel** | **17,928 (about 17.5 KiB)** (`static_assert` at most 20 KiB) | OCRAM2 |
| WattersonChannel with the 0.1 % truncation option: paths 4 x (224 + 1,776 + 128) | 19,368 (about 18.9 KiB), inside the same assert | OCRAM2 |
| WattersonStage in the engine's typed slot | about 72 | engine object (DTCM today) |
| WattersonParams in the scenario | about 200 | scenario (DTCM today; read only at configure) |
| Configure-time scratch | none: coefficients are computed in place, with a few doubles on the stack (9.9) | |
| Code | about 6 to 8 KiB (estimate) | flash |
| Redesigned AWGN (7.2) | 2 typed slots of about 2.5 KiB, a 1.2 KiB shared kernel; the arena shrinks by 4 KiB | engine object (DTCM today): about +2.2 KiB net |
| Sample-clock stage (appendix A) | a shared 65 x 16 float phase table (4,160 B, computed once) and a typed slot of about 160 B (16-frame history, accumulator, walk state, stats) | table: OCRAM2 by a target integration linker rule; slot: engine object |
| 96 kHz build, for scale | ring 16 KiB (2048 frames: 1920 + a 128-frame chunk), Hilbert about 861 taps: channel about 28 KiB, with a rate-scaled budget | OCRAM2 |

Only the CPU reads and writes the channel object, never DMA, so a cacheable placement is right, as
it is for the output ring. Its working set, about 17.5 KiB, fits the 32 KiB D-cache. The new DTCM
is the adapter slot, the scenario slot and the AWGN slots, about 2.5 KiB together. The firmware
target integration may place `Scenario`, which is touched only at configure time, and the engine object to
OCRAM2 (placement estimate).

## 12. Implementation and verification

The feature is implemented. The current source and [module guide](../modules/signal-lab-watterson.md) are authoritative for file layout and runtime behavior. Repository unit tests cover component behavior; the [validation reference](watterson-validation.md) describes host statistical checks and optional comparisons using external reference data.
## 13. Open questions and limits

- **Doppler truncation interpretation remains open.** The selected 1% truncation satisfies the per-spur reading of the stated limit; integrated power outside the main lobe exceeds -60 dBc. The validation reference records both readings. Do not claim unconditional criterion 2 acceptance until the governing interpretation is resolved.
- **Target resource estimates require measurement.** CPU, memory placement, stack use, and long-run stability must be checked on the target. Host statistics and offline ARM builds do not establish runtime capacity.
- **Physical channel acceptance remains separate.** Host agreement with pathsim does not establish board-to-board reset determinism, analog performance, receiver performance, or modem conformance.
- **Sample-clock behavior is future work.** Appendix A preserves its proposed model and test ideas, but it is not part of current acceptance; discrete `sample_slip` remains the implemented time-step model.
## 14. References

Repository:

- [HF channel simulator survey](hf-channel-simulator-survey.md): sections 2.1.1, 4.2,
  4.3 and 4.4; G-A1, G-A2, G-A3, G-A6, G-A7, G-A8, G-B2, G-B4, G-B6 and G-C10.
- [Coding standard](../coding-standard.md) and [.clang-tidy](../../.clang-tidy).
- [Signal Lab engine](../modules/signal-lab-engine.md),
  [build and dependencies](../modules/build-and-dependencies.md) (Catch2) and
  [pathsim and the channel-sim chain](../modules/pathsim.md).
- [scenario.hpp](../../signal-lab/include/signal_lab/scenario.hpp),
  [impairment.hpp](../../signal-lab/include/signal_lab/impairment.hpp),
  [mixer.hpp](../../signal-lab/include/signal_lab/mixer.hpp),
  [det_math.hpp](../../signal-lab/include/signal_lab/det_math.hpp) and
  [channel_spec.hpp](../../channel-sim/core/channel_spec.hpp).

Standards and literature:

- MIL-STD-188-110D w/Change 1, Appendix E (E.4.1, E.4.2, E.4.4, E.5.1 to E.5.5, E.6.2, E.7.1 to
  E.7.4, Table E-I) and Appendix D (D.6.1, D.6.2, Table D-LXV, D.6.4); MIL-STD-188-110B C.6.3 and
  C.6.4.
- ITU-R F.1487 (2000): section 3, Annex 2 and Annex 3.
- W. N. Furman and J. W. Nieto, "Understanding HF Channel Simulator Requirements in Order to
  Reduce HF Modem Performance Measurement Variability", HF01, 2001 (through the survey).
- J. F. Kaiser, "Nonrecursive digital filter design using the I0-sinh window function", Proc. IEEE
  ISCAS, 1974.
- G. E. P. Box and M. E. Muller, "A note on the generation of random normal deviates", Ann. Math.
  Statist. 29(2), 1958.
- M. E. O'Neill, "PCG: A family of simple fast space-efficient statistically good algorithms for
  random number generation", 2014.
- H. B. Mann and D. R. Whitney, "On a test of whether one of two random variables is
  stochastically larger than the other", Ann. Math. Statist. 18(1), 1947.
- P. D. Welch, "The use of fast Fourier transform for the estimation of power spectra", IEEE Trans.
  Audio Electroacoust. 15(2), 1967.

## Appendix A. Proposed sample-clock stage

**Deferred proposal.** Sample-clock offset is not implemented or part of current acceptance. The existing discrete time-step model is `sample_slip`. This appendix preserves a possible later design, not a current feature contract.

### A.1 Motivation

A sample-clock stage models the difference between transmitter and receiver sample clocks, separate from carrier-frequency offset. Keep board clock measurement distinct from a simulated receiver-clock offset. The proposed design uses a fixed-length polyphase interpolator, an integer phase accumulator, and an optional bounded random walk.
### A.2 Model and arithmetic

The stage resamples the stream as a receiver whose clock runs at (1 + e) times the nominal rate:
output frame m is the input at position t_m = m / (1 + e) input frames, so N input frames give
about N (1 + e) output frames, and a tone at f is heard at f / (1 + e). A positive `offset_ppm` is
a fast receiver clock.

- **Accumulator.** 32.32 fixed point. `step` = round(2^32 / (1 + e)) as `std::uint64_t`, about
  2^32. The position is a 64-bit input `SampleIndex` and a 32-bit fraction phi. Each output frame
  adds `step` to (index, phi) with carry. Integer arithmetic only, so the realised ratio is exactly
  2^32 / step, the resolution is 2.33e-4 ppm, and any block partition gives the same output.
- **Interpolator.** A Kaiser-windowed sinc fractional delay: 16 taps, 64 phases plus one, beta 10,
  cut-off 0.45 fs, each phase scaled to unit gain at DC. The phase is the top 6 bits of phi; the
  next 26 bits blend phases p and p + 1 linearly. Worst error over all fractional positions
  (computed): **-92.5 dB to 3.3 kHz**, but only -39.8 dB to 15 kHz, so a WBHF build needs the
  24-tap, 128-phase table (-93.8 dB to 3.3 kHz, -89.3 dB to 15 kHz, 12.4 KiB), a constexpr choice
  of the later rate and band work (survey recommendations). The cut-off is below fs/2, so even at mu = 0 the
  filter is not a unit impulse: at 0 ppm the stage is an 8-frame delay to within that same error,
  not bit for bit. There is no bypass for 0 ppm; a scenario without the stage is the identity.
- **Table.** 65 x 16 floats (4,160 B), computed once with det::sin and the Kaiser I0 series of
  section 3.3, shared by every play.
- **Random walk** (optional). Every `clock_walk_interval` = 48,000 output frames (1 s, keyed to
  absolute output frames) e moves by `walk_ppm` x a unit Gaussian from `gaussian_pair()` on PCG32
  family 10, clamped to +/-`walk_bound_ppm`, and `step` is recomputed. With `walk_ppm` 0 the offset
  is static.
- **Latency.** 8 frames (3.4 counts it).
- **Frames.** At +1000 ppm a 2048-frame block gives at most 2052 frames, inside the engine's
  capacity of 2304 (`engine_slack_frames` = 256). The engine already handles a frame count that
  changes, for sample slips.
- **Order.** A receiver's clock samples signal and noise together, so a future sample-clock stage must follow the noise and be last. Because `sample_slip` must also be last, a future scenario schema should reject scenarios that combine the two.

### A.3 Proposed schema

`{"type": "sample_clock", "offset_ppm": 100.0}` or, with a walk,
`{"type": "sample_clock", "offset_ppm": 0.0, "walk_ppm": 0.5, "walk_bound_ppm": 20.0}`.

| Member | Type, unit | Range | Default |
|---|---|---|---|
| `offset_ppm` | number, ppm | [-1000, 1000] | required |
| `walk_ppm` | number, ppm per 1 s step | [0, 10] | 0 |
| `walk_bound_ppm` | number, ppm | [0, 1000]; at least \|offset_ppm\|; above 0 when `walk_ppm` is | 0 |

Messages: `sample_clock: at most one stage, and it must be the last`, `sample_clock: cannot be
combined with sample_slip`, one range message per member, `sample_clock: unknown member`.
A future implementation would need matching parsing and validation rules.

### A.4 Proposed code, units and constants

- `SampleClockStage` with the local `mCamel` private-member convention in a typed engine slot
  `std::optional<SampleClockStage>`, about 160 B (16-frame history, accumulator, walk state,
  stats). The shared table goes to OCRAM2 by a target integration linker rule.
- A new unit type `units::PartsPerMillion` in `units/clock.hpp`, the same shape as 9.1 (private
  value, `PartsPerMillion{100.0, ppm}`, `in(ppm)`, `in(ratio)`), so an offset is never a bare
  double.
- Constants, each cited: `clock_max_offset` = `PartsPerMillion{1000.0, ppm}` (twice E.5.1's
  500 ppm); `clock_max_walk_step` = `PartsPerMillion{10.0, ppm}` (design); `clock_walk_interval`
  = `SampleCount{48000}` (1 s); `clock_interpolator_taps` = 16, `clock_interpolator_phases` = 64,
  `clock_kaiser_beta` = 10, `clock_cutoff_fraction` = 0.45 (A.2, computed); `clock_rng_family` =
  a newly reserved family ID. Family 10 is already used by Markov CW;
  do not reuse it for this proposed stage.
- Cost: about 110 cycles per output frame (two 16-tap dot products, a blend and the accumulator),
  0.5 % (11.1).

### A.5 Proposed tests

| ID | Test |
|---|---|
| Proposed check | `step` and the realised offset for 0, +/-1, +/-10, +/-100, +/-500 and +/-1000 ppm: known-answer steps, realised offset within 2.4e-4 ppm |
| Proposed check | Interpolator design: worst error at most -90 dB to 3.3 kHz over 4096 fractional positions; every phase sums to 1; known-answer coefficients for phases 0, 32 and 64 |
| Proposed check | A 1 kHz tone at +100 ppm matches the analytic resampled tone within -85 dBc over 10 s; at 0 ppm a 300-3300 Hz multitone comes out delayed by 8 frames within -90 dB |
| Proposed check | Frame accounting: after N input frames the stage has emitted exactly the count the accumulator predicts; a 2048-frame block at +1000 ppm gives at most 2052 frames |
| Proposed check | Partition invariance with a changing output count (blocks 1, 7, 96, 448, 2048 and a seeded random partition), and bit-for-bit equality with a frame-by-frame reference model |
| Proposed check | The walk: a known-answer offset sequence for seed 1; never beyond the bound; step standard deviation within 5 s.e. over 1e5 steps of the walk function alone (no audio); every schema message reachable; refused next to sample_slip |

### A.6 Possible statistical checks

- The delay probe through `sample_clock` at -500, -100, +100 and +500 ppm, 300 s each:
  `analyze_delay`'s clock offset equals the realised offset within 5 x its reported s.e.
- tone1800 through the same: the tone frequency (`analyze_level`, or the driver's FFT) equals
  1800 / (1 + e) within 1 mHz (at +500 ppm the shift is 0.9 Hz).
- A 1 h row with `walk_ppm` 1 and `walk_bound_ppm` 50: `analyze_delay`'s drift fit follows the
  offset track that the stage reports per step.
- With the wideband noise-spur method at 60 dB SNR: no line in 300-3300 Hz other than the tone above -70 dBc.
- The engine's output frame count equals the predicted count, and partition hashes hold.
