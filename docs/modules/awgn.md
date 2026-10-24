# Additive white Gaussian noise

## Scope and 50,000-foot view

The AWGN stage generates Gaussian noise, filters it into the selected band,
and scales its spectral density relative to the input reference RMS.
The engine owns stage ordering and resources; this guide owns the noise
model and implementation. Use [wideband noise](wideband-noise.md) for scenario
examples and [testing](../testing.md) for repeatable checks.

## Model and use

The C++ AWGN stage (`signal-lab/include/signal_lab/awgn.hpp`; design
[7.3](../design/watterson-stage.md)) follows the lane's SNR convention, N0 x
BW in 300 to `300 + BW` Hz (MIL-STD-188-110D E.5.5 refers SNR to the input power).
The scenario-wide `noise_bandwidth_hz` accepts 3000 (the unchanged default),
6000, 9000 or 12000. See [wideband noise](wideband-noise.md) for examples,
metadata and acceptance boundaries. The default 3 kHz design is:

```text
noise(n) = sigma_w (h * e)(n),   sigma_w = R 10^(-SNR/20) sqrt(fs / (2 x 3000 Hz))
e(n)     unit-variance Box-Muller pairs: first to frame 2k, second to frame 2k + 1 (E.5.3)
h        Kaiser band-pass, A = 60 dB, cutoffs 150 and 3450 Hz, 583 taps, unit gain at 1800 Hz
```

A pair's second value serves only its own frame, once. Behind a sample slip the
engine still passes each block its input cursor, so a block can start on an odd
frame whose value the previous block never drew (a drop) or already used (a
duplicate); that frame then draws a fresh pair and takes its second value, so
no excitation value repeats. Contiguous blocks draw exactly as before (the
deterministic output digests did not change). The reference level and sigma_w are typed
(`units::Amplitude`), the frame positions and the window are `units::SampleIndex`,
and the window `[start, start + duration)` gates the output only, never the
draws, so the noise inside a window is the unwindowed noise sample for sample
(`awgn_tests.cpp` checks both at seven block sizes, for known and live streams).

| Measure | Value |
|---|---|
| N0 x 3000 Hz (lane primary, E.5.5) | the declared SNR by construction; validate with the statistical tests |
| Flatness over 300-3300 Hz | -0.009 / +0.010 dB |
| ENBW | 3233.4 Hz: the total noise is 0.325 dB above N0 x 3000, so a total-power SNR reads label - 0.325 dB |
| Excitation | exact Gaussian per sample (Box-Muller, the radius draw widened before the + 1); PCG32 family 1 |

Each selected kernel is designed once with det_math into a shared four-kernel
bank (4,704 B). On the player it lies in OCRAM2
(`.ocram.noise_band_kernel`, NOLOAD), out of DTCM. A one-byte ready mask in
zero-initialised `.bss` replaces the former design flag; startup never clears
the NOLOAD bank, so each bit is set only after that kernel is fully designed.
Configuration is single-threaded. Each stage keeps its 582-sample FIR history
and its pair carry in a typed engine slot
(at most two AWGN stages per scenario) and uses the engine's per-call scratch
for the excitation, so no cast and no placement new remain in the noise path.
The Python peer
`host/signal_lab/awgn.py` follows it: the same 583-tap Kaiser band (designed in
float64 with NumPy's Kaiser window, unit gain at 1800 Hz) and the same sigma_w,
over NumPy PCG64 excitation (`SeedSequence([seed, 1, index])`), applied by
overlap-save FFT. The two were never sample identical (see the PCM24 section
in the engine guide), so they are compared statistically: `tests/test_signal_lab_render.py`
renders one seeded scenario (87 s of noise at 10 dB against a reference RMS of
0.125) through both and holds each to N0 x 3000 Hz within 0.05 dB of the label
and of the other, 500 Hz sub-bands of 300-3300 Hz within 0.1 dB, the ENBW within
1 % of 3233.4 Hz, and the stopband below -60 dB at 3.7-6 kHz and -80 dB above.
The Python peer accepts an explicit 3000 Hz setting but refuses the wider
bands and directs the caller to `signal_lab_render`. The impulse stage's ring
noise keeps the legacy 513-tap Blackman band in both
engines (`band_fir.hpp`; `filter_kernel()` and `bandlimit()` in `awgn.py`).


## Maintainer walkthrough

- `noise_band_kernel(bandwidth)`: designs the kernel on first use (configure time):
  pass 1 sums the windowed ideal band-pass's response at 1800 Hz, pass 2 stores
  each tap divided by it, and the ENBW is (fs / 2) sum h^2 of the stored floats.
- `band_tap(index, beta)`: 2 f_hi/fs sinc(2 f_hi m/fs) - 2 f_lo/fs sinc(2 f_lo m/fs)
  with f_lo = 150 Hz, f_hi = 3450 Hz, times the Kaiser window at m / 291.
- `awgn_noise_sigma(R, snr)`: R 10^(-SNR/20) sqrt(fs / (2 x 3000 Hz)), an
  `Amplitude` from an `Amplitude` (a power or a dBFS number does not compile).
- `AwgnStage::prepare`: checks the SNR (+/-120 dB) and the scratch, resolves the
  window, sets sigma_w, reseeds the generator (family 1, the stage index) and
  fills the history with 291 pairs, so frame 0 starts a pair.
- `AwgnStage::excitation(frame)`: an even frame draws a pair, returns `first`
  and keeps `second` for frame + 1; an odd frame returns the kept `second` if it
  is that frame's and unused, and otherwise draws a fresh pair and returns its
  `second`. A block may start on an odd frame; behind a sample slip its value
  was never drawn (a drop) or already used (a duplicate), and no value repeats.
- `AwgnStage::add_chunk`: history, then the chunk's excitation, in the scratch;
  the symmetric FIR for each frame (pairs from the end taps in, then the
  centre); inside the window the noise times sigma_w is added to the block;
  the last 582 excitation samples become the history.
- `AwgnStage::process`: the legacy pointer and count, viewed once as a span,
  in chunks of at most 1722 frames (the scratch's 2304 less the history).

## Worked example: default 3 kHz band

R = 0.125 (about -18.06 dBFS), SNR = 10 dB:

```text
sigma_w = 0.125 x 10^(-10/20) x sqrt(48000 / 6000) = 0.125 x 0.316228 x 2.828427 = 0.111803
N0      = 2 sigma_w^2 / fs = 5.208e-7 per Hz        N0 x 3000 Hz = 0.0015625 = R^2 / 10   (10 dB, exactly)
total   = N0 x 3233.4 Hz = 0.0016841                 RMS 0.041037, a total-power SNR of 9.675 dB
```


## Plain-language view

The stage makes random hiss, keeps the pitches in the chosen radio band,
and sets its strength relative to the desired signal. Changing the band
changes how much total noise is present; the SNR label uses the declared band.

## Maintenance boundaries

The walkthrough uses the default 3 kHz kernel. The C++ kernel bank also
supports 6, 9 and 12 kHz; the Python reference supports 3 kHz only. Preserve
FIR summation order and frame-keyed Gaussian pair consumption when changing
the implementation. The impulse stage uses its separate legacy `BandFir`;
it is not a calibrated AWGN stage.
