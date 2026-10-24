# Selectable noise and SNR bandwidth

Signal Lab's AWGN stage uses a selectable noise bandwidth for the 48 kHz
signal chain. A scenario may set the integer field `noise_bandwidth_hz` to
3000, 6000, 9000, or 12000. If omitted, it defaults to 3000 Hz, preserving the
existing 300–3300 Hz band. For bandwidth `BW`, the passband is 300 to
`300 + BW` Hz and its centre is `300 + BW / 2` Hz. The scenario identity
includes the selected bandwidth.

The AWGN SNR is defined over the selected bandwidth: noise power is `N0 × BW`
and the signal power is the long-term average input power. In particular, a
50 dB setting uses the requested 50 dB ratio in the selected band, not a fixed
3 kHz noise power scaled into a wider passband. Renderer sidecars report
`noise_bandwidth_hz`, `noise_band_low_hz`, `noise_band_high_hz`,
`noise_band_center_hz`, and the realised `noise_band_enbw_hz`.

For example, add `"noise_bandwidth_hz": 9000` at the root of an existing
scenario containing `{"type":"awgn","snr_db":12}`. Both C++ renderers and
the player's scenario engine use the selection. It also survives the normal
renderer’s `--target-scenario` export for SD card use. The Python reference
renderer accepts 3000 Hz but explicitly refuses wider bands; use the C++
`signal_lab_render` executable for those scenarios.

The named design method is `kaiser-sinc`. Four shared symmetric 583-tap
kernels are designed lazily during configuration, with unit gain at each
selected band centre. The bank occupies 4,704 bytes of OCRAM2 in the player;
a post-link gate caps it at 5 KiB. The existing history and scratch buffers
are reused. The offline ARM build keeps DTCM at 169,152 bytes and the engine
at 55,360 bytes. Noise filtering adds no signal delay. The impulse stage’s
separate noise filter retains its original band. The engine still runs at
48 kHz; higher stream rates require a separate rate extension and a new scratch geometry.

## D.6.2 static multipath fixtures

The JSON scenarios in [`fixtures/scenarios/wideband`](../../fixtures/scenarios/wideband/)
record the nine equal-power fixed-path combinations in Table D-LXV for 6, 9,
and 12 kHz. Each specifies the bandwidth and a 50 dB AWGN setting. All delays
are relative to the first path and are whole samples at 48 kHz; each path has
0 dB relative power. Watterson normalizes the sum of path powers to one, so
each of N equal paths has realized power 1/N. The table lists the path delays
in milliseconds.

| Bandwidth | WID | Path delays (ms) |
| ---: | ---: | --- |
| 6 kHz | 2 | 0, 2, 9 |
| 6 kHz | 10 | 0, 1.5, 3, 5 |
| 6 kHz | 12 | 0, 1.5 |
| 9 kHz | 3 | 0, 3, 8 |
| 9 kHz | 10 | 0, 2, 4.5 |
| 9 kHz | 12 | 0, 1.5 |
| 12 kHz | 2 | 0, 2.5, 8.5 |
| 12 kHz | 10 | 0, 1.5, 3, 5 |
| 12 kHz | 12 | 0, 1.5 |

The renderer-backed profile test loads each checked-in scenario with a
generated host WAV source, then checks the sidecar's selected band and
realised Watterson path delays and powers. This verifies configuration and
metadata; a generic host tone is not the Table D-LXV modem waveform and does
not establish BER performance or board acceptance.

The separate `wfg-wideband-response` host test removes AWGN from those nine
fixtures and measures coherent tone gains at the band centre and centre
+/-25% of bandwidth. It compares the rendered magnitudes with an independent
fixed-path complex comb to a 0.5 dB limit. Three single-path 90-degree cases
exercise the Hilbert branch against unit gain; a two-equal-path 1 kHz null
has an absolute leakage limit of 0.01 (-40 dB). These are signal-path response
spot checks, not a wideband modem waveform or the full E.7 measurement battery.

`12k-wid12-unequal-path-geometry.json` separately captures the 48 kHz WID 12
two-path geometry: 0 and 1.5 ms, with the second path 3 dB down. The profile
test checks the realised 0 and 72 frame delays and path-power ratio. A separate
response test now renders a low-level, deterministic broadband PCM24 probe
through that fixture and a matching single-path reference. With normalized
gains `a0 = sqrt(1/(1+10^(-3/10)))` and `a1 = a0*10^(-3/20)`, it checks each
settled sample against `a0*s[n] + a1*s[n-72]`, using the single-path output `s`.
The three-code PCM24 tolerance covers baseline/output quantization and float
rounding. The retained host run has maximum error 0.516913 codes; deliberately
using delay 71 or omitting the second path produces errors 150500.322 and
75734.275 codes respectively (both must exceed 1000). Thus the check observes
the actual delayed contribution and gain, beyond metadata. It remains a
software path-response check, not the full 48 kHz WID 12 waveform compliance
test, the distinct 12 kHz WID 12 equal-power row or physical capture parity.

## Software validation and remaining acceptance

The kernel tests pin binary32 coefficients, band-edge responses and sigma
values for each width. They check the band-centre and quarter-band response,
strict parsing, invalid enums, default/explicit 3 kHz PCM identity, allocation
counts, and output identity across block partitions. Existing 3 kHz AWGN
regressions also run; this is not an exhaustive archived-corpus comparison.
The player harness exercises Watterson, frequency sweep and each AWGN band
on both simulated output routes with CUT.

A deterministic 32-second noise render per band checks one-sided Welch PSD
times the declared bandwidth against the input reference power. It measures
the flat interior directly, rather than correcting total variance by ENBW.
One-kilohertz subbands exclude 500 Hz at each passband edge. Acceptance requires N0 x bandwidth error within 0.10 dB and subband spread
within 0.20 dB. The paired-render SNR check covers two equal fixed paths at
0/2 ms followed by a +75 Hz shift, with a 12 dB noise setting. Its noise level
continues to refer to input RMS; differencing the pair removes the channel
output before estimating N0 from the flat band interior.

The finite-noise PSD check does not prove the -70 dBc coherent-spur criterion.
That requires a documented reference power, detector floor, and positive
control. The existing `wv_analysis.py` line/fold helpers now accept a selected
analysis band, report per-bin and family-wise false-alarm limits, and pass
synthetic -68 dBc controls at 6/9/12 kHz. They also correct the formerly missed
inclusive upper-edge bin, which can change the last median bucket's results.
The host-only runner is `signal-lab/validation/run_wideband_validation.py`:

```powershell
python -B signal-lab/validation/run_wideband_validation.py `
  --build build/host-release --work build/wideband-validation
```

It streams three five-hour mono PCM24 noise renders sequentially, with 10 dB
SNR relative to a -12 dBFS input reference. The default arbitrary-line Welch
segment is 100 seconds; harmonic folds cover periods 2048, 1024, 256 and 64.
Reports give the actual trimmed intervals, measured detection floors and a
-68 dBc injected control. Passing requires all floors below -70 dBc, no
detected over-limit spur, and a detected control within 1 dB. Short runs
(`--duration-seconds 32`) and unresolved controls are inconclusive, not
acceptance. A malformed control or a missed control with sufficient detector
sensitivity fails validation.

The manifest hashes the scenarios, renderer binary, analysis code, WAVs and
reports. `--resume` accepts only unchanged recorded artifacts; it refuses
unrecorded partial output instead of overwriting it. Git HEAD is labelled as
context, not proof of the renderer's build. The runner records the renderer
binary hash. Retain a copy of the renderer for a long
campaign if the build directory will be rebuilt concurrently.

Common math thread pools are capped at two, and Windows processes use
BelowNormal priority. The runner checks the checkout's
`build/main/bringup/AB_HOLD` marker before each render or analysis call;
`--hold-marker` adds another marker, such as a shared test-station hold file.
`--deadline-utc` stops new work at an explicit UTC cutoff. Neither guard
interrupts a call already in flight. The three default WAVs occupy about
7.8 GB in total. Use the [wideband validation method](../design/wideband-validation.md)
for detector assumptions, prerequisites and interpretation.

These response and noise checks do not substitute for the complete E.7
channel/board rows.
Physical captures, reset repeatability, receiver/BER results and board
acceptance remain outstanding.
