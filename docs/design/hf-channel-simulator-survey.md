# HF channel simulator survey for signal-lab

Date: 2026-10-08. Scope: HF ionospheric channel simulators and models, commercial,
government and open source, surveyed for features that signal-lab (the deterministic
48 kHz block engine in `signal-lab/`, host and RT1170) should consider for
MIL-STD-188-110A/B/C/D testing, including the 110D Appendix D wideband waveforms (WBHF).

This is dated design background, not a current product comparison or an
implementation-status list. The engine now supports both RT1170 and RT1064;
use the [module index](../modules/README.md) and [roadmap](../roadmap.md) for
current capabilities and planned work. Preserve source-specific caveats below
when using this survey to justify a design choice.

Method: public-source research and owner-supplied reference documents. Some
figures and equation glyphs were unavailable in text extraction; affected
claims identify that limitation. Search-excerpt claims are explicitly weaker
than claims supported by the full source.

Evidence tags used throughout:

- **[D: Sn p.x]** documented in source Sn (Section 5), at the printed page given.
- **[X: Sn]** documented only in a search-engine excerpt of Sn; the page itself could not
  be fetched (HTTP 403, redirect, or size limit). Treat as weaker than [D].
- **[I]** inferred by this survey (reasoning or arithmetic), not stated by a source.
- **[NF]** not found in the public sources searched. This is not proof of absence.

---

## 1. Summary

1. **The binding simulator specification is MIL-STD-188-110D Appendix E**, unchanged from
   110C w/Change 1 wherever the 110D text could be compared [D: S1 pp.246-251; S2 pp.219-226].
   It is short and concrete. It requires: a two-path Watterson tapped delay line, with delay
   rounded to the nearest sample (E.5.2); independent white Gaussian sources, flat within
   ±1 dB to fs/2 (E.5.3); Gaussian fading filters with sigma = d/2, truncated at 1 % of
   peak, with total path power normalized to 1.0 (E.5.4). Taps are computed at 32 times the
   fading bandwidth or faster and interpolated to at least the symbol rate; interpolating to
   the sample rate is the design objective (DO) (E.5.4). SNR is set in the waveform
   bandwidth against the average input power (E.5.5). The sample rate must be at least 4 times
   the symbol rate (E.5.1). The appendix also sets: radio filters disabled (E.4.3); latency
   ≤ 25 ms (DO 10 ms) for two-way tests (E.4.4); and validation tolerances of ±0.5 dB
   response, SNR within 0.25 dB without fading and 0.5 dB with fading, and a Doppler spectrum
   mask (E.7). Its scope stops at 24 kHz, although Appendix D now reaches 48 kHz
   [D: S1 p.246].
2. **The conformance channels are few.** They are AWGN, two equal-power independent
   Rayleigh paths at 2 ms / 1 Hz ("ITU-R Poor"), the Table XVI/XX stress rows (2 ms / 5 Hz,
   5 ms / 1 Hz, 5 ms / 5 Hz), a ±75 Hz offset hold, a ±75 Hz triangle sweep at 3.5 Hz/s, and
   (110D) static 2-, 3- and 4-path multipath up to 9 ms [D: S3a; S3c]. Signal-lab's planned
   Watterson stage, frequency offset and sweep cover all of them, provided the Watterson
   stage supports **at least 4 paths, a non-fading (fixed) path mode and delays up to
   9 ms**.
3. **Commercial simulators add these beyond Watterson:** Rician/specular paths
   (Collins MDM-3001, ITS Watterson hardware). Interferers: CW, pulsed CW, swept CW and FSK
   (MDM-3001); jamming and intermodulation (Signatron S250C). CCIR/atmospheric impulsive
   noise (MDM-Q9604, HSM-2051, ITS hardware, Signatron). Simulated STANAG 4203 radio filters
   (MDM-Q9604). Multiple independent channels for ISB and diversity (MDM-Q9604 4-channel,
   HSM-2051 2-channel, S250C dual diversity, ITS dual space diversity). Two-way
   half/full-duplex wiring (IONOS, S250C). Remote scripting (Fraunhofer RTSIM, MDM-3001
   remote protocol). Product detail is sparse. Several vendors named in the brief (GE, TCI,
   Telefunken, Racal, Rohde & Schwarz, Spirent, Noisecom) have **no public HF ionospheric
   simulator product** that this survey could find.
4. **Simulator fidelity measurably changes BER results.** Furman & Nieto (Harris) report
   about 0.5 dB cross-simulator spread for 2400L and more than 2.3 dB for 9600VL. The causes
   are fading-filter shape, tap update rate and interpolation [D: S3b citing S5]. Their
   recommendations became Appendix E.5. Linear interpolation at exactly 32× gives tap images
   of about -64 dBc in total, only 4 dB inside the lane's -60 dBc criterion; 64× gives about
   -76 dBc [I: Section 4.2].
5. **Wideband realism is out of the Watterson model's validated range.** F.1487 validates
   it for 3 kHz and only possibly to 12 kHz [D: S6 p.2, p.5]. Vogler-Hoffmeyer (NTIA/ITS)
   adds intra-mode delay spread, a Lorentzian Doppler option and delay-dependent Doppler
   slope [D: S9 pp.10-16; S10 p.1]. Appendix D still binds the plain two-path model at every
   bandwidth, so these are realism features, not conformance features.
6. **48 kHz real sampling limits Appendix-E-conformant WBHF testing to 15 kHz
   bandwidth** under E.5.1's 4×-symbol-rate rule [D: S1 p.248; 110D Table D-I]. 15 kHz runs
   at 12,000 Sym/s, and 4 × 12,000 = 48,000 S/s meets "at least four times" with equality.
   18 kHz is the first to fail. 18 and 24 kHz waveforms need at least 57.6 and 76.8 kS/s.
   30/36/42/48 kHz waveforms (24,000/28,800/33,600/38,400 Bd per Table D-I) need
   96/115.2/134.4/153.6 kS/s. They are outside Appendix E's 24 kHz scope and beyond a 48 kHz
   real stream's Nyquist limit. (Corrected per QA-003, issue #7.)

**Top recommendations** (detail in Section 4.3):

1. Build the Watterson stage exactly to Appendix E.4-E.5: ≥4 paths, fixed/Rician path
   option, ≥20 ms delay, ≥64× tap rate, linear-or-better interpolation to 48 kHz. Validate
   to E.7.
2. Make the noise and SNR bandwidth selectable (3-24 kHz) per E.5.5 and D.6. The current
   AWGN band is fixed at 300-3300 Hz.
3. Frequency offset and triangle Doppler sweep (±75 Hz, 3.5 Hz/s) per 110B C.6.3/C.6.4 and
   110D D.6.4/D.6.5.
4. An Appendix E.7 validation harness on the host: frequency response, SNR without and
   with fading, and the Doppler spectrum mask.
5. Low-latency real-time mode (blocks of 1024 frames or fewer) for two-way tests under
   E.4.4. Then the interferer set (swept/pulsed CW, FSK, faded co-channel) and calibrated
   impulsive atmospheric noise.

---

## 2. Per-simulator and per-model sections

### 2.1 Defining standards and models

#### 2.1.1 MIL-STD-188-110D Appendix E (and 110C w/Change 1 Appendix E)

Source text read directly: 110D [S1] printed pp.246-251; 110C w/Change 1 [S2] printed
pp.219-226. In the 110D copy, printed pp.247 (E.4.1, E.4.2, Figure E-1), 249 (E.5.4
equations, E.5.5) and 252 (Figure E-3, Table E-I) did not text-extract; they appear to be
image pages. Every 110D page that did extract matches 110C w/Change 1 word for word, so the
110C text stands in for the three missing pages [I]. The hfmodem oracle cites the 110D
appendix at p.244 [S3a]; the EverySpec copy prints it at p.246.

**Update (QA-004, issue #8):** the three pages are readable in **MIL-STD-188-110D w/Change
1**, the current baseline. A word-for-word diff of Appendix E against the 2017 base shows
only those three pages differ (re-typeset), so the "unchanged" claim holds. The oracle's
"p.244" is Change 1's pagination. Cite 110D w/Change 1 for Appendix E.

| Clause | Requirement (paraphrased; "shall" unless noted) | Source |
|---|---|---|
| E.1.1 | Baseband simulators for modems in channels up to 24 kHz (N × 3 kHz, N = 1..8) | [D: S1 p.246] |
| E.1.2 | Appendix nonmandatory, but simulators used for compliance shall comply. Text still says "MIL-STD-188-110B" (stale) | [D: S1 p.246] |
| E.4.1 | Two-path Watterson: WGN plus two copies delayed by a fixed "delay spread". Independent fading gains with Gaussian PSD of given "fading bandwidth". Equal long-term power. Fully specified by delay spread, fading bandwidth per path and average SNR | [D: S2 p.221] |
| E.4.2 | Tap delay line; complex x, y, n, h. Only h0 and h(L-1) non-zero, L = delay/Ts. Hilbert transform may create complex x; output is real part of y | [D: S2 pp.221-222; S1 p.248] |
| E.4.3 | Radio filters, if implemented, disabled for compliance testing | [D: S1 p.248] |
| E.4.4 | Latency x to y ≤ 25 ms (DO: 10 ms) for two-way (interactive) systems | [D: S1 p.248] |
| E.5.1 | Sample rate ≥ 4 × symbol rate (examples: 9600 S/s for 3 kHz Appendix C, 76 800 S/s for 24 kHz Appendix D). Sample-rate crystal "should" drift ≤ 500 ppm | [D: S1 p.248] |
| E.5.2 | L = delay spread "divided by the sample rate" (read: sample period [I]), rounded to nearest integer | [D: S1 p.248] |
| E.5.3 | Noise source and fading-gain sources independent; PSD uniform within ±1 dB up to fs/2 | [D: S1 p.248] |
| E.5.4 | Taps = complex WGN filtered by f_j(t). Gaussian PSD with sigma_j = d_j/2. Time-domain f_j truncated at ±tau where it falls to ≤ 1 % of peak. k set so total power gain of all paths = 1.0. Taps computed at ≥ 32 × fading bandwidth. Interpolated to ≥ symbol rate (DO: to simulator sample rate) | [D: S2 pp.222-223] |
| E.5.5 | Noise power set for specified SNR in the bandwidth of the waveform under test, referenced to average power of input x. Note: may need minutes to measure for high-rate waveforms | [D: S2 p.223] |
| E.6.1 | Wideband analog interface: PC sound cards often filter 20-24 kHz. Common subcarrier, e.g. 19.2 kHz, "may be beneficial" | [D: S1 p.250] |
| E.6.2 | FIR Hilbert lower band edge rises with fs; overlapped-FFT Hilbert alternative | [D: S1 p.250] |
| E.6.3 | Output LPF/BPF slightly wider than waveform, to stop wideband noise aliasing in receiver | [D: S1 p.250] |
| E.7.1 | Frequency response within ±0.5 dB at band centre and centre ±25 % of bandwidth ("should") | [D: S1 p.251] |
| E.7.2 | SNR, single non-fading path, within 0.25 dB; tone on/off method; each average ≥ 20 min | [D: S1 p.251] |
| E.7.3 | SNR, single and dual fading (2 ms), within 0.5 dB; Doppler 1.0 and 10.0 Hz; each ≥ 2 h | [D: S1 p.251] |
| E.7.4 | Doppler spectrum: tone in, ≥ 3 h of samples, windowed FFT average, Kaiser window. Table E-I (1.0 Hz single path): ±1.5 dB at -20 dB, ±2.0 dB at -30 dB | [D: S1 p.251; S2 pp.225-226] |

The E.5.4 equations are typeset in a symbol font that did not extract (π, σ, τ and the
radical are missing). **Derive the filter from the prose (sigma = d/2, 1 % truncation,
unit total power), not from a transcription of the equation glyphs** [I].

> **Trap (QA-004, issue #8): do not transcribe the printed time-domain formula.** In the
> rendered page (110D w/Change 1 p.247), `k` is typeset as the radical index and the
> exponential sits under the radical: f_j(t) = (k-th root of) 2·e^(−π² t² d²). Read
> literally, that is exp(−π² d² t² / 2), which is √2 too wide in time. Measured by FFT, its
> PSD sigma is **0.354 d instead of d/2**, a fading bandwidth 29 % too narrow. At the ideal
> −20 dB frequency it sits at −40 dB, a 20 dB miss against Table E-I's ±1.5 dB mask. The
> FIR also grows to 63/125 taps at K = 32/64 instead of 45/89. The self-consistent form is
> the prose-derived one below (sigma = 0.5000 d exactly). A unit test must fail if the
> realised sigma is not d/2.

For reference: a
Gaussian PSD with sigma_f = d/2 corresponds to an impulse response proportional to
exp(-π² d² t²). The 1 % truncation is then tau = sqrt(ln 100)/(π d) ≈ 0.683/d seconds
[I, arithmetic]. At a tap rate R = K·d the FIR length is about 2·0.683·K + 1 taps: 45 at
K = 32 and 89 at K = 64. **The length is independent of d**, which suits compile-time
sizing [I].

#### 2.1.2 MIL-STD-188-110 family test conditions (from the hfmodem oracle)

- **110B/110C/110D serial tone, Table XX (110B) / Table XVI (110C, 110D).** Fixed-frequency
  operation, maximum interleaver. Fading rows use two independent, equal-average-power
  Rayleigh paths with two-sigma fading bandwidth. Signal and noise are measured in 3 kHz.
  Frequency hopping is allowed +2 dB. 110C/110D allow embedded-radio modems +1 dB. Rows
  span "1 fixed" (AWGN), 2 ms / 0.5 Hz, 2 ms / 1 Hz, 2 ms / 5 Hz, 5 ms / 1 Hz and
  5 ms / 5 Hz [D: S3a; S3c].
- **110A** uses the same Watterson F.520-2 method. The oracle marks its table rows as
  extraction-uncertain [D: S3e].
- **Appendix C (110B/C/D), Table C-XVII.** AWGN for ≥ 60 min. "ITU-R Poor" = 2 ms,
  1 Hz, two equal paths, ≥ 5 h. Signal power is the long-term average over data, mini-probes
  and reinserted preambles. **Doppler shift test (C.6.3):** sync held ≥ 5 min at ±75 Hz,
  9600 bps / VL, 2 ms / 1 Hz, 30 dB. **Doppler sweep (C.6.4):** ±75 Hz at 3.5 Hz/s as a
  periodic triangle with period 300/3.5 s; 1 h at 24 dB AWGN, BER ≤ 1E-5 [D: S3c].
- **110D Appendix D (D.6).** AWGN ≥ 60 min; Poor = F.1487 mid-latitude disturbed (2 ms,
  1 Hz), ≥ 5 h; Table D-LXIV SNRs; SNR "measured in the specified bandwidth". Static
  multipath (D.6.2, Table D-LXV), all paths equal power except as noted (48 kHz WID 12 is 2-path (0, 1.5 ms) with the second path 3 dB down; QA-003, issue #7), 50 dB SNR: for example 3 kHz
  WID 2 at 0/3/9 ms, 3 kHz WID 10 at 0/2/4.5 ms, 48 kHz WID 10 at 0/1.5/3.0/4.0 ms.
  Doppler shift (D.6.4) ±75 Hz in 3, 6, 9, 12, 24 and 48 kHz. Doppler sweep (D.6.5)
  ±75 Hz at 3.5 Hz/s [D: S3a]. The oracle transcription does not state the relative phases
  of the static paths; check D.6.2 before building that test [NF].
- **Run length.** F.1487 says run for the longer of 3000/Doppler spread and
  100/(BER·rate) [D: S6 p.11]. At 0.1 Hz that is 30 000 s, about 8.3 h [I].

#### 2.1.3 ITU-R F.1487 (supersedes F.520; CCIR Report 549)

[D: S6]. Annex 1 describes the stationary Gaussian-scatter (Watterson) model. It has n
taps, one per mode. Each tap gain is the sum of two magneto-ionic components, each a
zero-mean complex Gaussian process with its own attenuation, Doppler shift and Gaussian
spectrum of spread 2σ: six parameters per tap (pp.3-4). Additive noise may be "Gaussian,
atmospheric, and/or man-made" and/or interference (p.3). Specular modes and the ground
wave are added as non-fading taps; a specular tap carries the mode's frequency offset
(p.5). The model is valid for 3 kHz and "may be applicable" to 12 kHz; beyond 12 kHz it is
"problematic" (pp.2, 5). The practical simulator works at audio baseband, but transmit and
receive filters and level or gain control "should always be taken into account" (p.2).
Annex 2 gives a comparative sweep: delay 0-20 ms, spread 0.1-40 Hz, S/N -10 to 50 dB
(p.6). Annex 3 gives ten representative channels from 0.5 ms / 0.1 Hz to 7 ms / 30 Hz,
occurrence statistics and the test-length rule (pp.9-11). Older CCIR 520-1 conditions
quoted by Forrer add "flat fading" 0.2 Hz and "flat fading (extreme)" 1.0 Hz at 0 ms
[D: S20 p.35].

#### 2.1.4 Watterson model and the ITS hardware simulator (c.1970)

Watterson, Juroshek and Bensema, IEEE Trans. Commun. Technol. COM-18(6), Dec. 1970
[D: S1 p.246]; ITS simulator memo ERLTM-ITS 198 (1969) [X: S36]. Hoffmeyer & Nesenbergs
summarize the ITS hardware in Table 4 [D: S7 pp.32-38]. The OCR table is misaligned; the
pairing below follows the text on pp.35-38 and the order of the table rows [I]:

- Bandwidth 3, 6 or 12 kHz; maximum 4 paths; delay 0-10 ms in 20 µs steps (p.35).
- Dispersion: all-pass filters at 0, -100, -220 or -320 µs/kHz, meant for VLF/LF use
  (p.37).
- Specular path: 0-100 dB with phase; specular components are applied before the delay
  line (p.33).
- Scatter attenuation 0-100 dB. Frequency shift from internal steps or an external
  synthesizer, -500 to +500 Hz in 1 mHz steps.
- Spread (2σ) in 1-2-5 decades from 0.01 to 100 Hz on each path, with a finer series on one
  path.
- **Fading spectrum: "3-pole approximation of Gaussian".**
- SNR -100 to +90 dB, with Gaussian **or atmospheric** noise. Atmospheric impulsiveness
  Vd 7.2 dB in 2.7 kHz for HF; LF values are also listed.
- Signal-to-interference -100 to +90 dB; single or independent dual space diversity.
- Internal distortion below -70 dB relative to signal plus noise.

Interference bypasses the delay line but "may be subject to the time-function
multiplicative effects", meaning faded interferers [D: S7 p.33]. Only dual diversity was
implemented [D: S7 p.33].

#### 2.1.5 Furman & Nieto, "Understanding HF Channel Simulator Requirements..." (Harris, HF01 2001)

The paper is cited normatively by Appendix E.2.1 [D: S1 p.246]. Its content is summarized
in the oracle [D: S3b, citing paper pp.1-12]:

- Cross-simulator BER spread comes only from fading generation: filter shape, update rate
  and interpolation.
- Updating taps at only 2× Doppler costs about 2 dB; little difference between 37.5 and
  150 Hz updates. This supports STANAG 4285's 32× rule.
- Sample-and-hold taps produce sinc-shaped images at multiples of the update rate; linear
  interpolation per sample removes them.
- Ten-point specification: tapped delay line; total power normalized to 1; filtered
  complex WGN; Gaussian filter with sigma = d/2 truncated at 1 %; taps at ≥ 32×;
  interpolation to ≥ 2400/s (Harris used 9600/s); no radio filters or AGC; Hilbert input;
  SNR in 3 kHz averaged over minutes; AWGN; clock good enough for 0.1 % Doppler accuracy,
  i.e. ≤ 500 ppm.

#### 2.1.6 STANAG 4285 / 4415 / 4539 test conditions

The STANAG texts are not public; none was retrieved [NF]. Three facts are on record:

- STANAG 4285 contains a "32 × Doppler spread" tap-update rule, as cited by Furman
  [D: S3b].
- STANAG 4539 is the Appendix C waveform [X: S47], so its tests very likely mirror
  Table C-XVII [I].
- 110D says the optional robust 75 bps mode "shall meet STANAG 4415 performance"
  [D: S3a]. The 4415 channel conditions themselves were not found [NF].

Isode's vendor measurements of 4539/5069 use a Watterson simulator at CCIR Poor
[X: S45]. A 1998 FFI study compared 4285 and 4415 over measured high-latitude channels;
low SNR was the main cause of failure [X: S38].

#### 2.1.7 Vogler-Hoffmeyer (NTIA/ITS) wideband model and the ITS wideband simulator

- **Part II, TR-90-255** [D: S9]. Lists Watterson's limits: ≤ 12 kHz; stationarity; no
  spread-F dispersion; low ray only. Notes that a machine with "24-kHz bandwidth" claims
  had no validation basis (p.4).
  - Per-mode scattering function = delay amplitude factor T(τ) (three parameters: peak
    amplitude, total spread, carrier-delay subinterval) × correlation factor C(t) × phase
    function (pp.10-13).
  - C(t) is **Gaussian, or exponential correlation giving a "peak" (Lorentzian) Doppler
    spectrum**; the exponential case is generated by a first-order recursion (pp.13-15).
  - Phase includes Doppler shift plus a **linear delay-Doppler slope** that produces
    slanted scattering functions (pp.15-16).
  - 1-3 modes (low and high ray). Measured delay spreads from a few µs to over 2 ms,
    Doppler spreads from 0.1 to over 40 Hz (p.17).
- **Part III, TR-92-284** (title block reads 93-284) [D: S10].
  - Exponential correlation (Lorentzian) fits measurements best (p.1).
  - Four ionospheric inputs (fp, ho, thickness, attenuation), obtainable from prediction
    models, i.e. **coupling to propagation prediction** (p.2).
  - A hardware simulator was under development at ITS, coupling the transfer function to
    the Lemmon & Behm noise/interference model (p.15).
- **TR-98-348** (Sutherland) implements the transfer function in software as the input to
  the hardware simulator [X: S11]. A later "Mastrangelo et al." wideband simulator based on
  Vogler-Hoffmeyer is described as needing many parameters per mode [X: S41]. Its hardware
  specifications were not retrieved [NF].

#### 2.1.8 Noise models

- **ITU-R P.372** (current edition -18, 09/2026 [X: S37]) and its predecessor CCIR
  Report 322 give atmospheric, man-made and galactic noise levels by season and 4-hour time
  block [X: S37].
- **Amplitude distributions.** Atmospheric noise APDs are parameterized by Vd
  (rms-to-average voltage ratio in dB). Akima (1972) gives a numerical representation [X];
  Spaulding & Washburn (1985) show how to generate CCIR 322 APDs by computer [D: S33 p.39].
  This survey did not verify whether P.372-18 still carries the APD/Vd curves [NF].
  APDs depend on receiver bandwidth [X: S37 (Tarasov et al.)].
- **Middleton Class A/B/C.** Class A is narrowband impulsive noise with no front-end
  transients, impulsive index A; Class B is broadband with transients; Class C is their sum
  [X]. **Atmospheric noise is Class B** [D: S7 Sec. 4]. Giles says Class B is the one
  relevant to voice-band HF, but complicated, with six parameters [D: S33 pp.37-39].
- **Hall (1966).** y(t) = a(t)·n(t), a "generalized t" model [X: S35]. Three parameters,
  with unrealistic infinite energy for some parameter values [D: S33 p.37]. Kabanov (1987)
  fitted DSTO recordings best in Merchant & Britton (1995) [D: S33 p.37].
- **Lemmon & Behm (NTIA 91-277, 93-293).** Wideband (800 kHz) noise/interference model:
  Gaussian noise, plus about 40 sinusoids with Hall-distributed amplitudes, plus impulsive
  noise. Impulses are uniformly distributed in time segments, giving a pulse-spacing
  distribution close to Poisson [D: S33 pp.35, 40; X: S34].
- **Time statistics.** Pulse-spacing and pulse-duration distributions are Poisson,
  Poisson-Poisson or log-normal [D: S33 p.39].

### 2.2 Commercial and government hardware

#### 2.2.1 Rockwell Collins MDM-3001 HFCHANSIM (owner-supplied primary source)

Source: a three-page user note, "Rockwell MDM-3001 used as an HF Channel Simulator", by
NNN0WWL, not a Rockwell manual [D: S12]. In the extracted text the minus sign is an
undecodable glyph; it is read as a minus [I].

- Purpose: built-in HFCHANSIM mode for BER-testing another MDM-3001 with a protocol
  analyzer (p.1). MS-DMT's compliance verification used this set-up [D: S3b].
- Model: "stationary Gaussian Scatter Ionospheric propagation model as defined by
  CCIR-549"; AWGN, **Rayleigh and Ricean fading**, and interference (p.2).
- Multipath: "one or two channels of Multipath" (p.2). This reads as one or two paths
  [I]; Doppler spectrum shape not stated [NF].
- Doppler spread up to 50 Hz; delay spread up to 23 ms (p.2).
- SNR -50 to +50 dB "as measured in a 3kHz bandwidth" (p.2).
- **Frequency offset -80 to +80 Hz, sweepable at a slew rate of 3.5 Hz/s** (p.2). This is
  the Appendix C/D sweep rate.
- Input attenuation up to 50 dB (p.2).
- **Internal interferers: AWGN, CW, pulsed CW, swept CW, FSK with settable baud rate**;
  external interference can also be mixed with the input (pp.2-3).
- SNR display shows current and **long-term average SNR**, the latter "used to verify the
  SNR when fading is selected"; "NO INPUT SIGNAL" when the input is absent (p.3).
- **No STANAG 4203 radio filters, unlike the newer MDM-9604**; the author adds external
  SCAF-1 filters to approximate the 300-3300 Hz S4203 passband (p.3).
- I/O: 600 Ω balanced audio, output -30 to +5 dBm, rear J1 audio connector (pp.1-2).
- Control: 20×2 LCD with three buttons, or remote binary protocol on J2 (p.1).
- Hardware: ADSP-21062 DSP (p.2).

Not stated: Doppler shape, tap update and interpolation, seeds or repeatability, number of
independent channels [NF].

#### 2.2.2 Rockwell Collins MDM-Q9604 (embedded simulator)

The datasheet URL redirected to the Collins home page; content below is from search
excerpts of it [X: S13]:

- "Embedded 4-channel HF simulator, NATO validated for STANAG 4415."
- "presets for common ionosphere simulations, multiple configurable interferer types,
  simulations up to five multi-paths, and simulated STANAG 4203 HF radio filters."
- Noise "Gaussian and CCIR noise simulating impulsive interferers such as lightning."
- "up to four independent channel simulations" for MIL-STD-188-110B Appendix F
  (multi-channel/ISB) testing.
- Certifications: JITC 110B (with Appendices C and F) and DERA (QinetiQ) STANAG 4539
  Annex B.

#### 2.2.3 Collins HSM-2051

From the product page [D: S14]: "a two-channel, real-time, ionospheric channel simulator
that simulates the effect of an HF/WBHF channel". It "Enhanc[es] the Watterson HF channel
model" with AWGN and **CCIR Recommendation 322 noise**. The modem supports 110D Appendix D
up to 48 kHz (1.2-240 kbps). Simulator parameters are not given [NF].

#### 2.2.4 Harris / L3Harris

- No Harris-branded HF channel simulator product or test set was found [NF].
- Harris engineers Furman and Nieto wrote the simulator requirements that became Appendix E
  (2.1.5).
- Forrer (1999) refers to "professional channel simulators (manufactured by Harris Corp.
  for example)" and to unresolved result discrepancies between implementations
  [D: S20 p.34].
- A Harris paper (Weston & Koski) models HF links in OMNeT++ with a calibrated PHY
  abstraction that reproduces 110B/C error statistics, including burst-error
  distributions [X: S42]. This is a link-level simulation, not a waveform channel.

#### 2.2.5 GE, TCI, Telefunken, Racal, Rohde & Schwarz, Keysight/Agilent/HP, Elektrobit/Anite/Propsim, Spirent, Noisecom

- **GE / General Electric:** no HF channel simulator found [NF]. A 1977 Radio and
  Electronic Engineer paper describes a digital HF multipath simulator: four skywave paths
  with independent delays, Doppler shifts, Rayleigh fading and attenuation. Its builder is
  not named in the excerpt [X: S48].
- **TCI:** nothing found [NF].
- **Telefunken:** ECHOTEL ETM-1810/1820 STANAG 4285 modems, no simulator [X: S44].
- **Racal:** modems (e.g. LA1117 "Piccolo"). A UK RAE study tested a modem on the **RAE
  Cobbett Hill HF Channel Simulator**, specifications not retrieved [X: S43]. RAE later
  became part of DERA and then QinetiQ [I].
- **Rohde & Schwarz:** SMW200A fading documentation surfaced only mobile-standard
  presets and a custom-fading option; no HF/Watterson profile found [NF].
- **Keysight PROPSIM (ex Elektrobit/Anite):** spec range starts at 3 MHz and marketing
  says "RF channel emulation from HF to mmWave". No ionospheric or Watterson model is
  documented on the platform page [D: S28].
- **Agilent/HP:** no HF ionospheric product found [NF].
- **Spirent:** the SR5500 covers 400 MHz-6 GHz with cellular/WLAN models; not HF
  [X: S29].
- **Noisecom:** no HF channel simulator found [NF].

#### 2.2.6 Signatron (Watterson/CCIR software simulator, 1982-84; S250C, 1996)

- **Early simulator** (Ehrman et al., 1982) [D: S7 pp.37-39, 69-70; S20 p.36]:
  - FPS-120B array processor, baseband or RF input.
  - Three paths, each with ordinary and extraordinary (O/X) components; Rayleigh and Doppler
    fading per component.
  - Gaussian and impulse noise; 4 kHz bandwidth, later 12 kHz.
  - Modified to follow a frequency-hopping modem.
  - Used by NRL and the Army CENCOMS.
- **S250C** (DTIC manual returned 403; search excerpts only) [X: S15]:
  - Real-time "extended Watterson" model over 2-90 MHz with groundwave and skywave; terrain
    and building multipath at VHF.
  - **Up to six multipath components in each direction (half-duplex)**; dual-diversity
    frequency-channel simulation when not hopping.
  - Per path: amplitude, delay, Doppler spread, Doppler shift, Gaussian noise amplitude.
  - **Impulse noise, jamming and intermodulation** effects.
  - The manual's specification tables (1.1 and 1.2) were not retrieved [NF].

#### 2.2.7 Fraunhofer IIS DT4630 RTSIM

From the product page [D: S16]:

- Watterson per ITU-R F.1487.
- Up to 4 paths, each with delay, Doppler spread, Doppler shift and attenuation.
- Delay ≤ 20 ms; Doppler spread 0-75 Hz; shift ±100 Hz; relative attenuation 0-20 dB.
- AWGN -160 to -20 dBm/Hz (an absolute density, not an SNR).
- RF 1-30 MHz, direct-sampling 100 MS/s, simulation bandwidth 1.6 MHz, latency < 50 µs,
  10 MHz external reference.
- Control: touch screen, Win32 remote GUI, web interface, **remote scripting**.
- Not stated: interference, impulsive noise, presets [NF].

#### 2.2.8 MITRE, NOSC, NRL (1980s wideband efforts)

- **MITRE:** 1 MHz baseband simulator in development with a real-time dispersive filter,
  0 to 100 µs/MHz. Single channel only; at least four (two modes × O/X) were judged
  necessary [D: S7 p.72].
- **NOSC:** 96 kHz software-only model adding a delay-distortion filter, used to evaluate
  equalizers [D: S7 pp.69-71].
- **NRL:** DSP real-time baseband simulator report (DTIC ADA261810) not retrievable
  (403) [NF].

#### 2.2.9 QinetiQ/DERA, CRC Canada, DSTO, FFI (DAMSON and replay)

- **DAMSON** (DERA with CRC, FOA and FFI) measured auroral and sub-auroral channels
  [X: S40].
- **Angling (Leicester thesis):** new model with **Lorentzian as well as Gaussian Doppler
  and per-mode delay spread**, driven by DAMSON data and compared with a Watterson
  simulator using serial-tone and FSK modems [X: S39].
- No CRC simulator product was found [NF].
- **DSTO replay simulator** (Giles thesis, owner-supplied) [D: S33]: see 2.3.12.

#### 2.2.10 IONOS / HFSimulator (ARSFI, Winlink)

From the manual, rev 2.03 [D: S17]:

- Teensy 4.0 with Arduino and Teensy Audio library, MIT licence, Watterson-based; 1, 2 or
  4 paths (p.1).
- Two mono audio inputs and outputs on 3.5 mm jacks; optional USB serial control (p.3).
- Built-in self-test and calibration loopback, ±1 dB (pp.3-4).
- S:N -40 to +40 dB, measured in the operating bandwidth of 3000 or 6000 Hz (p.5).
- Internal level monitor and crest-factor display (p.6).
- **Bandwidth correction 10·log(3000/BW) for narrower modems** (p.7).
- One unit models a symmetric half-duplex ARQ link; two units model full duplex,
  symmetric or asymmetric (p.7).
- Spec summary (p.14):
  - Channels WGN, MPG (0.1 Hz / 0.5 ms), MPM (0.5 / 1), MPP (1 / 2), MPD (2 / 4).
  - Flat fade 0-40 dB at 0.1-20 Hz; fixed offset ±200 Hz in 10 Hz steps; slow FM ±1-200 Hz
    at 0.1-20 Hz. Offset and FM are not usable with the multipath modes (p.6).
  - **Multipath update rate 64× Doppler spread**; 3 and 6 kHz bandwidths; sample rate
    44.1 kHz.
  - Experimental busy-channel detector.
- From the oracle's code reading: 128-tap Gaussian fading LPF [D: S3b].
- No seed control documented [NF].

### 2.3 Software

#### 2.3.1 PathSim (AE4JY; reworked by OK1IAK) — the lane's statistical reference

- AE4JY page [D: S18]:
  - Three paths ("usually only two are used").
  - Hilbert band-pass input; spreading by low-pass filtered complex AWGN, "continuous from
    0.1 to 30Hz" in v1.0.
  - Per-path complex-NCO frequency offset; one path can shift by ±1000 Hz in recorder mode.
  - AWGN through "a 3KHz LP filter"; SNR set from input RMS averaged over about 1 s.
  - Unity-gain path calibration; clipping indicator; canned test conditions; 8000 Hz,
    16-bit; GPL; amateur use.
- Oracle code reading [D: S3b]:
  - Fading random process at 12.8, 64 or 320 Hz by spread tier, polyphase-upsampled to the
    8 kHz internal rate; 2048-sample blocks.
  - Presets CCIR Good/Moderate/Poor plus Direct, AWGN, Doppler-fading, Flutter,
    latitude-based and NVIS.
- Lane patch [D: S50 (`docs/modules/pathsim.md`)]: adds `--seed` (whole realization from
  `srand`), `--truth-out` and the 48 kHz resampling chain.
- Note: at d = 0.4 Hz the 12.8 Hz tier is exactly 32× and at d = 2.0 Hz the 64 Hz tier is
  32×, so the tier boundaries sit at the Appendix E minimum [I from S3b tiers].

#### 2.3.2 KC7WW (Forrer) low-cost DSP simulator (TAPR DCC 1999)

[D: S20]:

- Analog Devices SHARC EZ-KIT Lite (40 MIPS) with a 48 kHz stereo codec (p.36); typically
  9600 S/s processing (p.37).
- Delays about 0.1-10 ms (1-96 taps) (p.37); IIR filter approximating the Gaussian fading
  spectrum (p.38).
- **Gaussian noise only**, explicitly called not representative of HF (p.38).
- Notes that equal-power paths create deep, sweeping notches that some users find
  unrealistic, and warns against unequal weighting in published results (p.34).
- Derived from Ehrman et al. and Kurpiers' TMS320C26 code (p.36).

#### 2.3.3 MATLAB Communications Toolbox

- **`comm.RayleighChannel`** with `doppler(...)` spectra: Jakes, Flat, Rounded, Bell,
  Asymmetric Jakes, Restricted Jakes, Gaussian, BiGaussian [D: S21c].
  - Gaussian/BiGaussian use normalized sigma.
  - MaximumDopplerShift must be < SampleRate/10/fc.
  - Sum-of-sinusoids forces Jakes [D: S21c].
- **`stdchan`** gives ten ITU-R F.1487 presets, `iturHFLQ` through `iturHFHD` including
  `iturHFMDV`, and fd "must be 1" for correct spreads [D: S21b].
- **HF example** [D: S21a]:
  - Builds a Watterson tap from two single-path Rayleigh objects with BiGaussian spectra,
    for magneto-ionic components with offsets.
  - Sigma = spread/2.
  - `RandomStream = 'mt19937ar with seed'`.
  - No noise added in that example.
- **110B example:** "moderate" F.520-2 channel with Gaussian Doppler [D: S21d].
- A Rician object exists in the toolbox but was not fetched for this survey [I].

#### 2.3.4 GNU Radio gr-channels

- **Channel Model block** [D: S22b]: noise voltage, frequency offset, epsilon (timing
  ratio), static complex taps, seed.
- **Dynamic Channel Model** [D: S22a]:
  - **CFO and sample-rate offset as bounded Gaussian random walks** (std dev per sample,
    max bound).
  - AWGN amplitude.
  - Sum-of-sinusoids fading (N sinusoids, max Doppler fD), Rayleigh or **Rician (K)**.
  - PDP with fractional-sample delays and magnitudes; seed.
- The SoS model gives a Jakes-type spectrum, not Gaussian [I]. No HF preset [NF].

#### 2.3.5 codec2 `ch` (FreeDV)

Source and help-text excerpts [X: S23]:

- `--mpg` (0.1 Hz / 0.5 ms), `--mpp` (1.0 Hz / 2.0 ms), `--mpd` (2.0 Hz / 4.0 ms).
- **Fading from precomputed files** generated by an Octave script (`ch_fading.m`) with a
  fixed seed, so realizations are repeatable.
- `--freq` offset; **`--clip` Hilbert clipper**; **`--ssbfilt` SSB filter (default on)**.
- `--Fs` 8000 default; complex output option.

#### 2.3.6 WSJT-X simulators

`fst4sim`/`qra64sim` take a Doppler spread (Hz) and a "Watterson model delay" (ms)
[X: S24]. Implementation details were not read [NF].

#### 2.3.7 Linrad

No channel simulator found. Linrad has an internal signal generator only [X: S25] [NF].

#### 2.3.8 NTIA/ITS software (Johnson Research, 1991; error models)

- "HF Simulator: Channel and Modem Modules" (1991): narrowband HF channel plus the
  FED-STD-1045 / 188-141A ALE modem, public-domain modules [D: S26 (catalogue page)]. The
  PDF exceeded the fetch limit, so its parameters were not read [NF].
- TR-94-310 (Johnson & Peach): an **HF channel error model** reproducing ALE modem
  behaviour about 100× faster than a full simulation, i.e. link-level abstraction
  [D: S27].

#### 2.3.9 Isode MoRaSky / HF Tool

From the product page [D: S30]:

- Emulates modem, radio and sky, not the waveform channel.
- CCIR Good/Moderate/Poor/AWGN; "Intermediate Term Variation following the Walnut Street
  model".
- SNR fixed or changed at intervals from a list; configurable BER and clustered errors.
- Regular and **Markov-chain on/off interference**; modem failure patterns.
- Multi-node networks, surface wave, node movement.
- **ALE including 4G ALE**, bandwidth 3-48 kHz (STANAG 5069); GUI or CLI.
- HF Tool runs sequenced tests over a channel simulator between two real modems.

#### 2.3.10 NIST Special Database 21

ALE signal recordings degraded by noise, multipath and fading with the Watterson model:
a fixed, shareable test corpus [X: S46].

#### 2.3.11 Daniels & Peters, "A New MIMO HF Data Link..." (Kuma Signals, MILCOM 2013 preprint; owner-supplied)

[D: S32]:

- Discrete-time complex-baseband Monte Carlo. Independent complex Gaussian noise on each
  receive antenna, SNR set by noise variance at the receiver (pp.1, 5).
- Baseline 110C Appendix D: symbol rate 2.4-19.2 kBd in 2.4 kBd steps, RRC 35 %, 3-24 kHz
  occupancy (p.2).
- 58 measured 2×2 NVIS channels at 7 MHz, single tap (narrowband), with a measured
  spatial correlation matrix (pp.2, 5).
- For multipath: a **2-tap, 2 ms, equal-energy "Waterson" (ITU-R Poor) channel**,
  extended with the measured spatial correlation, simulated at **48 kHz** (p.5). The
  paper does not state the Doppler spread or tap generation for this channel [NF].
- Synchronization and channel estimation assumed perfect (p.4).
- **Power-amplifier nonlinearity: Rapp model, p = 2, with 2-5 dB backoff.** Single-antenna
  256-QAM needed 36 dB SNR at 5 dB backoff (p.6).
- Relevance: PA/limiter modelling for high-PAPR QAM WIDs; correlated diversity paths;
  48 kHz two-path at 2 ms. It offers nothing on Doppler, interpolation or noise-flatness
  criteria.

#### 2.3.12 Giles, "On the Design of HF Radio Modems" (PhD thesis, Adelaide/DSTO, 1995; owner-supplied)

Page numbers are printed pages; PDF page = printed + 4 [D: S33].

- **Watterson critique** (pp.16-22):
  - Four assumptions: Rayleigh taps, independent taps, Gaussian spectrum, discrete paths.
  - Stationarity holds only for about 10 minutes or less, and validation used 36 minutes of
    data (pp.19-20).
  - Modes from the same layer are often highly correlated, which questions independence
    (p.20).
  - **The Gaussian spectrum "does not have an apparent physical origin" and the choice
    affects results**; other work used first-order (Vogler-Hoffmeyer) or Butterworth
    spectra (p.20).
  - Discrete taps need small intra-path time spread: a 0.1 ms spread limits the simulation
    bandwidth to about 2.5 kHz, and a 2.2 ms spread to about 110 Hz (p.22).
- **Model reviews:** Vogler-Hoffmeyer (pp.22-27) and the Le Roux "pseudo-deterministic"
  model, measured paths with correlated AR random parts (pp.27-29).
- **Noise review:**
  - Narrowband interference can be up to 70 dB above other noise (pp.32-33); occupancy
    models from Gott and Perry & Abraham (pp.33-35).
  - Girault et al. (1988) simulator generating **PSK, BFSK and chirp interferers** (p.35).
  - Lemmon & Behm (40 sinusoids, Hall amplitude) (p.35).
  - **Stable CW is too easy; a swept CW was chosen** (p.36).
  - Impulsive-noise PDF models: Beckmann, Hall, Kabanov, Middleton (pp.36-38).
  - CCIR 322 APDs (p.39); man-made noise, CCIR 258-4 (p.39); PSD/PDD statistics (p.39).
- **Replay simulators** (Ch. 5):
  - Noise and interference replay: WGN; **swept CW at 300 Hz/s over 300 Hz-3.3 kHz**;
    recorded impulsive-noise samples (pp.53, 56).
  - **DSTO HF radio replay simulator:** probe-derived impulse responses, n = 256 at
    8000 S/s (32 ms window). The impulse-response rate of 31.3 Hz limits fading to
    ±15.6 Hz (p.58). Impulse responses are interpolated and filtered between updates;
    recorded noise is added; about 115 MB per hour (p.59).
  - Proposes a **standard library of 48 × 15-min recordings** with known ionospheric
    conditions (p.60).

#### 2.3.13 "A model of high frequencies (HF) channel used to design a modem..." (Scribd 638478921)

Scribd blocked the document body. The landing page shows the title "H.F Channel Model for
Modem Design", uploader "ASM AAS ASSAS" (authorship not established) [D: S31]. Its
abstract, apparently auto-generated, describes:

- a model for a **9600 bit/s modem in 3 kHz**;
- channel characterized by the time-varying weighting function g(t,τ) and transfer
  function G(f,t);
- correlation analysis yielding **coherence bandwidth Bc and coherence time Tc** for modem
  design.

No copy was found elsewhere by title [NF]. A possibly related work is Abdullah's 1986
Loughborough thesis "Data transmission at 9600 bit/sec over an HF radio link", which gives
an equivalent baseband HF model for QAM simulation [X: S49]; it was not read. No
tap-generation, interpolation, noise or validation details are available from this source.

#### 2.3.14 Other relevant work

- **Mendieta-Otero et al.:** whole-HF-band interference simulator (narrowband interference
  plus congestion model, CW-Morse example) [D: S51 (abstract)].
- **Wideband HF skywave review (arXiv, 2026):** ray-traced links up to 1 MHz, P.372-17
  noise [D: S52 (abstract)].
- **IET 2006:** implementation and comparison of a wideband HF noise and interference
  simulator [X: S53].
- **Scintillation:** no HF modem simulator modelling ionospheric scintillation as a
  separate effect was found [NF].

---

## 3. Feature matrices

Cells: values with tags as above; "-" = not applicable; "?" = not found [NF].

### 3.1 Propagation and fading

| Simulator / model | Paths | Doppler shape | Spread range | Shift / offset / sweep | Delay range, resolution | Specular / Rician | Intra-path delay spread | Bandwidth, rate | Tap update / interp. |
|---|---|---|---|---|---|---|---|---|---|
| 110D/110C App. E (requirement) | 2 [D:S1] | Gaussian, σ=d/2 [D:S2] | any; E.7 tests 1, 10 Hz | none in App E; ±75 Hz, 3.5 Hz/s in C.6/D.6 [D:S3] | round(delay/Ts) [D:S1] | no | no | ≤24 kHz; fs ≥4×Rs [D:S1] | ≥32×d; interp ≥Rs, DO fs [D:S2] |
| ITU-R F.1487 | n, 2 comps each [D:S6] | Gaussian, bi-Gaussian | 0.1-40 Hz sweep [D:S6] | per component [D:S6] | 0-20 ms sweep [D:S6] | non-fading tap [D:S6] | no (>12 kHz open) | 3 kHz; ≤12 kHz maybe | ? |
| ITS Watterson hw (c.1970) | 4 [D:S7] | 3-pole approx. Gaussian [D:S7] | 0.01-100 Hz steps [D:S7] | ±500 Hz, 1 mHz [D:S7] | 0-10 ms, 20 µs [D:S7] | yes, 0-100 dB + phase [D:S7] | dispersion all-pass [D:S7] | 3/6/12 kHz | analog |
| Signatron (1982-84) | 3 × O/X [D:S7] | ? | ? | ? | ? | ? | no | 4 then 12 kHz [D:S7] | ? |
| Signatron S250C | ≤6 per direction [X:S15] | ? | per path [X:S15] | per path [X:S15] | per path [X:S15] | groundwave [X:S15] | ? | 2-90 MHz RF [X:S15] | ? |
| Collins MDM-3001 | 1-2 [D:S12] | CCIR 549 Gaussian [D:S12] | ≤50 Hz [D:S12] | ±80 Hz, slew 3.5 Hz/s [D:S12] | ≤23 ms [D:S12] | Ricean [D:S12] | no | 3 kHz audio | ? |
| Collins MDM-Q9604 | ≤5 [X:S13] | ? | ? | ? | ? | ? | ? | 3 kHz × up to 4 ch [X:S13] | ? |
| Collins HSM-2051 | ? | "Watterson" [D:S14] | ? | ? | ? | ? | ? | HF/WBHF to 48 kHz [D:S14] | ? |
| Fraunhofer DT4630 | 4 [D:S16] | F.1487 [D:S16] | 0-75 Hz [D:S16] | ±100 Hz [D:S16] | ≤20 ms [D:S16] | ? | no | 1.6 MHz RF [D:S16] | ? |
| IONOS / HFSimulator | 1/2/4 [D:S17] | Gaussian, 128-tap [D:S3b] | 0.1-2 Hz presets [D:S17] | ±200 Hz, slow FM (WGN only) [D:S17] | 0.5/1/2/4 ms [D:S17] | ? | no | 3/6 kHz, 44.1 kHz [D:S17] | 64×d [D:S17] |
| PathSim | 3 [D:S18] | Gaussian FIR [D:S3b] | 0.1-30 Hz [D:S18] | per-path NCO [D:S18] | variable [D:S18] | offset-only path [D:S3b] | no | 8 kHz, 3 kHz noise [D:S18] | 12.8/64/320 Hz + polyphase [D:S3b] |
| KC7WW (1999) | ≥2 [I] | IIR approx. Gaussian [D:S20] | CCIR 520-1 set [D:S20] | Doppler shift [D:S20] | 0.1-10 ms [D:S20] | ? | no | 9600 S/s [D:S20] | ? |
| MATLAB | any | 8 shapes incl. Gaussian, bi-Gaussian, Asym. Jakes, Flat [D:S21c] | any | via bi-Gaussian centres [D:S21a] | any, fractional | Rician object [I] | no | any | ? |
| GNU Radio | PDP taps [D:S22a] | SoS (Jakes-type) [I] | fD | CFO random walk [D:S22a] | fractional samples [D:S22a] | Rician K [D:S22a] | no | any | SoS |
| codec2 `ch` | 2 [I] | precomputed file [X:S23] | 0.1/1/2 Hz [X:S23] | `--freq` [X:S23] | 0.5/2/4 ms [X:S23] | ? | no | 8 kHz [X:S23] | file |
| Vogler-Hoffmeyer / ITS WB | 1-3 modes [D:S9] | Gaussian or Lorentzian [D:S9] | 0.1->40 Hz measured [D:S9] | shift + delay-Doppler slope [D:S9] | per mode [D:S9] | - | yes, T(τ) [D:S9] | to ~1 MHz [D:S9] | ? |
| DSTO replay (Giles) | recorded IR [D:S33] | as recorded | ≤±15.6 Hz [D:S33] | as recorded | ≤32 ms window [D:S33] | as recorded | as recorded | 100-3800 Hz [D:S33] | 31.3 Hz IR, interpolated [D:S33] |
| **signal-lab now** | 1 (direct) | none (deterministic amplitude fades) | - | - | - | direct path | - | 48 kHz real | - |
| **signal-lab planned** | Watterson (count TBD) | Gaussian (planned) | - | offset + Doppler sweep | - | ? | - | 48 kHz; L/R ISB | ? |

### 3.2 Noise, interference, system

| Simulator / model | AWGN, SNR bandwidth | Impulsive / atmospheric | Interference | Clock / timing | AGC, filters, clip | Channels, diversity, ISB, duplex | Scripting, time variation | Seeds | I/O |
|---|---|---|---|---|---|---|---|---|---|
| App. E | WGN; SNR in waveform BW vs input power [D:S2] | no | no | ≤500 ppm crystal [D:S1] | radio filters off [D:S1] | - (two-way latency ≤25 ms) | stationary | - | real audio baseband |
| ITS Watterson hw | Gaussian, -100..+90 dB [D:S7] | atmospheric, Vd 7.2 dB/2.7 kHz [D:S7] | external, S/I -100..+90, fadable [D:S7] | ? | <-70 dB internal distortion [D:S7] | dual space diversity [D:S7] | fixed per run [D:S7] | - | analog |
| Signatron (both) | Gaussian [D:S7] | impulse [D:S7; X:S15] | jamming, intermod [X:S15] | ? | ? | dual diversity, half duplex [X:S15] | computer controlled [X:S15] | ? | baseband or RF |
| MDM-3001 | AWGN, -50..+50 dB in 3 kHz, long-term avg display [D:S12] | ? | CW, pulsed CW, swept CW, FSK (baud), external mix [D:S12] | ? | no S4203 filters [D:S12] | ? | remote binary protocol [D:S12] | ? | 600 Ω, -30..+5 dBm [D:S12] |
| MDM-Q9604 | Gaussian [X:S13] | CCIR impulsive [X:S13] | multiple types [X:S13] | ? | S4203 filters [X:S13] | 4 independent (App F) [X:S13] | presets [X:S13] | ? | ? |
| HSM-2051 | AWGN [D:S14] | CCIR Rec. 322 [D:S14] | ? | ? | ? | 2 channels [D:S14] | ? | ? | ? |
| Fraunhofer DT4630 | AWGN, dBm/Hz [D:S16] | ? | ? | 10 MHz ref [D:S16] | ? | ? | remote scripting [D:S16] | ? | RF 1-30 MHz [D:S16] |
| IONOS | WGN, -40..+40 dB in 3/6 kHz [D:S17] | ? | ? | ? | level and crest-factor monitor [D:S17] | 2 ch; half/full duplex [D:S17] | USB serial [D:S17] | ? | 3.5 mm audio [D:S17] |
| PathSim (lane) | 3 kHz LP noise; RMS over ~1 s [D:S18] | ? | ? | ? | clip indicator [D:S18] | 1 | presets | `--seed` [D:S50] | WAV, sound card |
| MATLAB | separate | ? | ? | ? | ? | MIMO objects | script | mt19937ar seed [D:S21a] | arrays |
| GNU Radio | noise amplitude [D:S22a] | ? | ? | SRO random walk, epsilon [D:S22] | ? | ? | flowgraph | seed [D:S22] | streams |
| codec2 `ch` | Eb/No-style [X:S23] | ? | ? | ? | Hilbert clipper, SSB filter [X:S23] | ? | CLI | fixed-seed files [X:S23] | raw/WAV |
| Isode MoRaSky | SNR levels, lists [D:S30] | ? | on/off, Markov [D:S30] | ? | ? | networks, duplex [D:S30] | SNR schedules, Walnut Street [D:S30] | ? | protocol level |
| Lemmon & Behm (ITS) | Gaussian | Hall-type impulsive [D:S33] | ~40 CW lines [D:S33] | - | - | - | - | - | software |
| Giles / DSTO | WGN [D:S33] | recorded impulsive [D:S33] | swept CW 300 Hz/s [D:S33] | - | - | - | recorded (non-stationary) | - | file / DSP |
| Daniels & Peters | per-RX AWGN [D:S32] | - | - | - | Rapp PA p=2 [D:S32] | 2×2 MIMO, correlated [D:S32] | - | ? | baseband |
| **signal-lab now** | AWGN 300-3300 Hz (513-tap FIR), SNR vs reference RMS [D:S50] | impulse crashes (peak, decay, ring, Poisson/periodic) [D:S50] | fixed CW (C/I, ramp, phase) [D:S50] | sample slips [D:S50] | saturate/reject at PCM24 [D:S50] | mono | scenario JSON, windows, events, live control [D:S50] | per-stage derived streams [D:S50] | WAV PCM24; RT1170 audio |
| **signal-lab planned** | - | - | - | sample-clock ppm; CUT drop/zero | - | independent L/R (ISB) | - | - | USB UAC2 |

---

## 4. Gap analysis and recommendations

### 4.1 Where signal-lab already stands relative to the field

- **Unique or uncommon in signal-lab:** sample slips, and drop/zero cuts (the CUT injector
  has landed in the player and host per commits a6ba652/2ce1964 and is planned for
  signal-lab). No surveyed simulator documents receiver-side timing faults of this kind
  [NF]. Also uncommon: bit-exact, block-size-independent determinism with per-stage seeds;
  a scenario grammar with live control; a heap-free embedded engine.
- **Comparable:** AWGN with SNR in 3 kHz (matches the 110-family convention and E.5.5 for
  3 kHz waveforms); CW interference; deterministic fades (similar in purpose to IONOS flat
  fade 0-40 dB [D: S17 p.14], but scripted rather than random).
- **Impulse crashes:** a heuristic static-crash generator. It is not calibrated to an APD
  or Vd as the ITS hardware and the MDM-Q9604/HSM-2051 "CCIR" noise are [D: S7; X: S13;
  D: S14].
- **"Mixer":** in signal-lab it is the output summation and quantization stage, not a
  second-source input [D: S50]. Co-channel modulated interference therefore needs a new
  stage [I].
- **Stage budget:** `max_stages = 8` [D: S50 scenario.hpp]. A full test chain could reach
  8 stages (Hilbert/Watterson, offset/sweep, AWGN, CW, impulse, fade, radio filter, slip)
  [I].

This survey's capacity forecast is historical. Consult the
[current engine contract](../modules/signal-lab-engine.md) for implemented stages,
fixed capacity and unsupported extensions before sizing a new chain.

### 4.2 The lane's Watterson-stage acceptance criteria against the sources

| Lane criterion | Nearest source requirement | Assessment and recommendation |
|---|---|---|
| Doppler 2σ within 10 % over 0.1-10 Hz, Gaussian shape | E.5.4 sigma = d/2, 1 % truncation [D:S2 p.223]; E.7.3 tests 1.0 and 10.0 Hz; E.7.4 mask ±1.5 dB at -20 dB, ±2.0 dB at -30 dB for 1.0 Hz [D:S1 p.251; S2 p.226] | The 10 % width criterion is a different metric from Table E-I's level mask; check both [I]. Validate the tap sequence at the tap rate, not only a tone through the channel. At R = 64·d, 8 h of fading at 0.1 Hz is only about 184 k tap samples, cheap on the host [I]. Also check the FIR design analytically (deterministic). Furman shows filter shape changes BER by up to 2.3 dB [D:S3b], and the original ITS hardware used a 3-pole approximation [D:S7]. |
| Interpolation images below -60 dBc | E.5.4: ≥32×, interpolate to ≥ symbol rate (DO: sample rate) [D:S2]; Furman: sample-and-hold images; linear interpolation removes them [D:S3b] | Computed for an ideal Gaussian PSD (σ = d/2) with linear interpolation (power response sinc⁴) [I]: K = 16 gives total images -52 dBc (worst image peak -56 dB); **K = 32 gives -64 dBc (-68 dB)**; **K = 64 gives -76 dBc (-81 dB)**. Sample-and-hold at K = 32 gives only -31 dBc. **Use K ≥ 64 with linear interpolation, or K = 32 with cubic/polyphase**, for margin. |
| Delays exact to one 48 kHz sample up to 7 ms | E.5.2: round to nearest sample [D:S1 p.248] | Consistent. All 110/F.1487 test delays are integer samples at 48 kHz (0.5 ms = 24 ... 9 ms = 432) [I]. 7 ms is too short for D.6.2 (9 ms) [D:S3a], F.1487 Annex 2 (20 ms) [D:S6 p.6], RTSIM (20 ms) [D:S16] and MDM-3001 (23 ms) [D:S12]. **Size for ≥ 20 ms (960 samples)**. |
| Rayleigh tap statistics, uncorrelated paths, Rician/fixed path option | E.4.1 independent fading gains; E.5.3 independent sources [D:S1, S2]; F.1487 §3 specular = non-fading tap with the mode's offset [D:S6 p.5]; MDM-3001 Ricean [D:S12] | Consistent. The fixed path should carry amplitude, phase and optional frequency offset (F.1487 §3). D.6.2 static multipath needs 2-4 fixed equal-power paths [D:S3a]. Path correlation (Giles p.20) is a realism option only, keep it off for conformance [I]. |
| Gaussian noise flat ±0.2 dB in 300-3300 Hz, no spurs above -70 dBc | E.5.3 white source flat ±1 dB to fs/2 [D:S1 p.248]; E.7.1 response ±0.5 dB [D:S1 p.251] | Stricter than Appendix E; fine. Also test the *pre-filter* white source flat to ±1 dB up to 24 kHz (E.5.3 applies there too), and fading-generator inputs independent of the noise stream [I]. For WBHF the flatness band must follow the waveform band (E.5.5) [I]. |
| SNR per E.5.5 within ±0.1 dB | E.5.5 SNR in waveform BW vs average input power [D:S2 p.223]; E.7.2 0.25 dB (≥ 20 min); E.7.3 0.5 dB (≥ 2 h, 2 ms, 1 and 10 Hz) [D:S1 p.251] | ±0.1 dB is attainable *by construction* (noise RMS set from the reference power). Verify that deterministically. Run the E.7.2/E.7.3 statistical measurements at the Appendix E tolerances, since measuring a fading channel to 0.1 dB needs impractically long averages [I]. Keep the E.5.4 "total path gain = 1.0" normalization so the AWGN reference (signal-lab's source reference RMS) stays the *input* power [I]. Long-term reference must include probes and preambles (110B C.6.1) [D:S3c]. |

### 4.3 Prioritized gap list

Priority A = needed for 110A/B/C conformance and Appendix E compliance. Priority B =
important for 110D WBHF (Appendix D) conformance or a likely compliance trap. Priority
C = realism and nice-to-have.

The embedded column addresses the integrity constraints: deterministic; heap-free with
static, compile-time-sized state; bounded loops; no exceptions or RTTI; strong unit types;
named constants with cited sources. The budget assumes about 1 MB free on-chip RAM.

| # | Feature | Pri. | Justification (one line) | Source | Embedded-integrity note |
|---|---|---|---|---|---|
| A1 | Watterson stage exactly per App. E.4-E.5: complex TDL, independent Gaussian taps, sigma = d/2, 1 % truncation, unit total power, real output via Hilbert | A | Binding simulator spec for every 110 BER claim | [D:S1 pp.246-249; S2 pp.221-223] | Fixed `max_paths` (≥4) and `max_delay` (≥20 ms = 960 complex floats ≈ 7.5 KiB per channel). FIR length is constant (~45 taps at K = 32, ~89 at K = 64) if R = K·d [I], so it can be compile-time sized. Named constants: `k_tap_rate_multiple` (E.5.4), `k_truncation_fraction = 0.01` (E.5.4). Unit types: `Hertz`, `Seconds`, `Samples`. |
| A2 | Tap interpolation to the 48 kHz sample rate (the DO), K ≥ 64 or a higher-order interpolator | A | Images and update rate drive cross-simulator BER spread (0.5 to >2.3 dB) | [D:S2 p.223; S3b] | Interpolate with an integer (Q32/Q64) phase accumulator; per-sample work bounded; block-size independent if the accumulator is the only state [I]. Avoid floating step accumulation (drift). |
| A3 | Non-fading (fixed) and Rician/specular path option per path (amplitude, phase, offset) | A | "1 fixed" Table XVI/XX rows; F.1487 specular and ground wave | [D:S3a; S6 p.5] | Same path array; a flag selects fixed gain. No dynamic allocation. |
| A4 | Static frequency offset ±75 Hz (planned) applied after the channel, before AWGN | A | 110B/C C.6.3 and 110D D.6.4 Doppler shift test | [D:S3a; S3c] | Complex NCO with integer phase accumulator and a fixed sin/cos table or det_math. Named constant `k_doppler_test_offset_hz = 75` (C.6.3/D.6.4). |
| A5 | Triangle Doppler sweep ±75 Hz at 3.5 Hz/s (planned), period 300/3.5 s | A | C.6.4 / D.6.5 sweep test (also MDM-3001's slew rate) | [D:S3c; S3a; S12 p.2] | Closed-form frequency per sample from absolute frame index; no event list needed; bounded and block-independent [I]. |
| A6 | SNR contract per E.5.5: noise power in the waveform bandwidth against long-term average input power; document stage order (Watterson → offset → AWGN) | A | Defines what every SNR in Table XVI/C-XVII/D-LXIV means | [D:S2 p.223; S3a; S3c] | Already reference-RMS based [D:S50]. The reference interval must be bounded; RT1170 already takes `reference_rms` from the host [D:S50]. |
| A7 | Appendix E.7 validation harness (host): response ±0.5 dB at centre and ±25 %; SNR 0.25 dB non-fading, 0.5 dB fading at 2 ms with 1 and 10 Hz; Doppler mask (Kaiser, ≥ 3 h) | A | Simulator compliance is a "should" list in E.7 and the lane criteria build on it | [D:S1 p.251; S2 pp.225-226] | Host-only tool running the same C++ engine; RT1170 runs spot checks. Long runs exceed WAV comfort: 5 h mono PCM24 is about 2.6 GB, so stream statistics instead of files [I]. |
| A8 | Long-run support for BER tests (AWGN ≥ 60 min; Poor ≥ 5 h; F.1487 3000/d rule) | A | 110B C.6.1 and 110D D.6.1 minimum durations | [D:S3c; S3a; S6 p.11] | 64-bit frame counters and phase accumulators; no unbounded buffers. Output over 4 GiB needs RF64 or streaming [I]. |
| B1 | Selectable noise/SNR bandwidth (3, 6, 9, 12, 15, 18, 21, 24 kHz) and band centre | B | E.5.5 SNR "in the bandwidth of the waveform"; D.6 "specified bandwidth"; current band fixed at 300-3300 Hz | [D:S2 p.223; S3a; S50 band_fir.hpp] | `constexpr` coefficient tables per bandwidth in flash (513 taps × 4 B ≈ 2 KiB each), or one fixed-size overlap-save FFT filter (E.6.2/E.6.3 suggest the FFT approach). Cite the design method as a named constant. |
| B2 | ≥4 paths with up to 9 ms (static multipath D.6.2) | B | Table D-LXV uses 3- and 4-path channels at 50 dB, equal power except 48 kHz WID 12 (second path 3 dB down) | [D:S3a; 110D Table D-LXV] | Covered by A1 if `max_paths ≥ 4`, with per-path power as a parameter. D.6.2 states no relative-phase convention (checked): define zero relative phase as the default, a per-path phase parameter and a phase-sweep option, and cite that the standard leaves it open. |
| B3 | Sample-rate decision for WBHF: 48 kHz real meets E.5.1 up to 15 kHz (Rs 12,000, Table D-I; with equality); 18/24 kHz need ≥ 57.6/76.8 kS/s; 30/36/42/48 kHz (24,000/28,800/33,600/38,400 Bd) need 96/115.2/134.4/153.6 kS/s, outside Appendix E and beyond a 48 kHz real Nyquist (QA-003, issue #7) | B | E.5.1 4×-symbol-rate rule; 110C Appendix D symbol rates 2.4-19.2 kBd | [D:S1 p.248; S32 p.2]; arithmetic [I] | Options: host-side complex-baseband path at a higher rate, or 96/192 kHz UAC2 if the RT1170 audio path allows [I; hardware capability not checked]. Keep the rate a compile-time template parameter rather than a runtime value [I]. |
| B4 | Low-latency real-time mode (≤ 25 ms through-delay, DO 10 ms) for two-way tests (ALE, STANAG 5066 ARQ) | B | E.4.4. One 2048-frame block alone is 42.7 ms at 48 kHz | [D:S1 p.248]; arithmetic [I] | The engine is block-size independent [D:S50], so run 256-480-frame real-time blocks. Hilbert and band FIRs add group delay (513-tap FIR ≈ 5.3 ms) and must be counted [I]. |
| B5 | Independent L/R channels (ISB, planned) with independent seeds and per-channel scenarios; also usable as the two directions of a duplex link | B | 110D Appendix F 2-ISB is the mandatory multi-channel part; MDM-Q9604 has 4 independent channels; IONOS models duplex | [D:S3d; X:S13; D:S17 p.7] | Two fixed engine instances, no shared mutable state; per-channel stream derivation already exists [D:S50]. |
| B6 | Sample-clock ppm offset (planned), static and slow drift | B | E.5.1 ≤ 500 ppm simulator clock; 110D symbol clock ±10 ppm; GNU Radio SRO random walk | [D:S1 p.248; S3d; S22a] | Fixed-length polyphase or Farrow interpolator; integer phase accumulator; bounded random-walk step with clamp (GNU Radio "max bound"). Interacts with sample slips; keep slips last [D:S50]. |
| B7 | PA nonlinearity / limiter (Rapp p, backoff) and an optional receiver AGC, off for compliance | B (C for 3 kHz) | High-PAPR QAM WIDs are PA-limited (36 dB SNR at 5 dB backoff for 256-QAM); E.4.3 and Furman say none in compliance runs | [D:S32 p.6; S1 p.248; S3b] | Rapp is closed-form (pow/sqrt in det_math); AGC is a first-order IIR with named time constants; both bounded. |
| C1 | Calibrated atmospheric impulsive noise: Middleton Class A (truncated Poisson mixture) and/or Hall; APD or Vd targets per CCIR 322 / P.372 | C | Used by ITS hardware, MDM-Q9604, HSM-2051, Signatron; HF noise is rarely Gaussian | [D:S7; X:S13; D:S14; D:S33 p.32] | Class A: mixture truncated at M (e.g. 16) terms, a weights table computed at configure time into a fixed array. Avoid large Vd lookup tables unless the coefficients are sourced (Akima 1972 not retrieved [NF]). Generate in the noise band, since APD depends on bandwidth [X:S37]. |
| C2 | Interferer set: swept CW (e.g. 300 Hz/s over 300-3300 Hz), pulsed CW, FSK with baud, faded CW, Markov on/off | C | MDM-3001 internal interferers; Giles: stable CW is too easy; ITS: faded interferers; Isode Markov | [D:S12 pp.2-3; S33 pp.36, 56; S7 p.33; S30] | Each is a bounded generator stage with fixed state; the Markov chain needs a fixed-size state table (2 states). |
| C3 | Co-channel/adjacent-channel modulated interferer (second source: PSK, BFSK, chirp, or another 110/4285 waveform) at set S/I | C | Girault simulator; S250C "jamming" | [D:S33 p.35; X:S15] | Second fixed-capacity source buffer (host) or a pre-rendered file on SD (RT1170). No dynamic graph: one fixed "second-source" slot. |
| C4 | Magneto-ionic O/X split (bi-Gaussian tap: two components with separate shifts and spreads) | C | F.1487 eq. (1)-(2); Signatron 3 paths × O/X; MATLAB bi-Gaussian | [D:S6 pp.3-4; S7 p.37; S21a] | Doubles the fading generators per path; fixed upper bound. |
| C5 | Alternative Doppler shapes: Lorentzian (exponential correlation), flat | C | Vogler-Hoffmeyer and Angling show Lorentzian fits better; Giles: shape affects results | [D:S9 pp.13-15; S10 p.1; X:S39; D:S33 p.20] | First-order IIR (exponential) is cheaper than the Gaussian FIR. Off by default; conformance stays Gaussian. |
| C6 | Wideband realism: intra-mode delay spread (sub-taps with T(τ) weights) and delay-dependent Doppler slope | C | Watterson validated only to about 12 kHz; WBHF at 24-48 kHz meets intra-path spread | [D:S6 p.5; S9 pp.4, 10-16; S33 p.22] | Fixed sub-taps per mode (e.g. 4 modes × 16 = 64 generators, about 32 KiB) [I]; CPU-bound, so validate the RT1170 budget before committing. |
| C7 | Correlated paths and diversity branches (configurable correlation coefficient) | C | Same-layer modes are often correlated; MIMO/diversity measurements | [D:S33 p.20; S32 p.2] | 2×2 Cholesky factor computed at configure time; fixed size. |
| C8 | STANAG 4203 / SSB radio filter emulation, off for compliance | C | MDM-Q9604 and MDM-9604 offer it; F.1487 says consider Tx/Rx filters | [X:S13; D:S12 p.3; S6 p.2; S1 p.248] | Fixed FIR table; reuse the band-FIR machinery. |
| C9 | Piecewise-constant parameter schedules (SNR, spread, delay) and slow "intermediate-term" variation | C | Isode SNR lists and Walnut Street; non-stationarity beyond ~10 min (Giles) | [D:S30; S33 p.19] | Bounded segment array (reuse `max_events = 32`); avoid a general scenario graph [I]. Crossfade spread changes over a fixed window to avoid discontinuities. |
| C10 | Channel-truth export (tap gains, offsets) for scoring receiver estimators | C | The lane's pathsim patch already emits `--truth-out` | [D:S50 pathsim.md] | Decimated fixed-rate records; host only, or a bounded ring buffer on RT1170. |
| C11 | Replay of measured channels (impulse-response files) and recorded noise | C | Absolute rather than relative results; DSTO replay and standard-library proposal | [D:S33 pp.56-60] | Stream impulse responses from SD (256 complex taps at 31.3/s ≈ 64 KB/s) into a double buffer (about 4 KiB) [I]; never load whole files. |
| C12 | Link-level or ALE abstraction (error models, ALE timing) | out of scope | Provided by Isode, NTIA TR-94-310 and Harris OMNeT++ at protocol level | [D:S30; S27; X:S42] | Belongs in host tooling, not the signal path. |
| C13 | Propagation-prediction coupling (ionospheric parameters to channel parameters), scintillation, frequency-hopping channels | out of scope | Vogler-Hoffmeyer uses four prediction-model inputs; FH is a separate standard (188-148) | [D:S10 p.2; S3a] | Host-side parameter generator if ever needed. |

### 4.4 Embedded-integrity notes that cut across items

- **Paths, stages and events:** all compile-time maxima. At the survey snapshot the limits were 8 stages and
  32 events [D:S50]; propose `max_paths = 4` (6 for S250C parity) [I]. Reject scenarios
  above the limits at parse time; never grow. The October 12 implementation update
  above raises only the stage-slot limit to nine.
- **Memory budget** [I]. Watterson with 4 paths, a 20 ms delay line, Hilbert FIR,
  interpolators and 2 channels: under 32 KiB. Adding C6 sub-taps: under 64 KiB. Band-FIR
  coefficient tables live in flash. Well inside about 1 MB.
- **Determinism:** derive the tap rate, interpolation phase, NCO phase and sweep from the
  absolute frame index with integer accumulators. Then any legal block size gives
  identical output, matching the existing contract [D:S50]. Fix the number and order of
  random draws per output sample (or per tap update), so that streams cannot
  desynchronize. The pathsim chain enforces a related rule: no two random draws in one
  statement, rule 7 in `docs/modules/pathsim.md` [D:S50].
- **Math:** the Gaussian filter, Rapp model and Class A need exp, sqrt, log, pow and
  sin/cos. Use det_math rather than a large-table library. Compare against Python or libm
  with tolerances, not bit-exactness (per the owner's float-tolerance rule).
- **Named constants with cited sources**, each with its clause in a comment:
  - 3 kHz measurement bandwidth (110D 5.3.2.5);
  - 32× tap rate and 1 % truncation (E.5.4);
  - 4× symbol-rate sampling and 500 ppm (E.5.1);
  - 25 ms and 10 ms latency (E.4.4);
  - ±75 Hz and 3.5 Hz/s (C.6.3/C.6.4, D.6.4/D.6.5);
  - E.7 tolerances: 0.5, 0.25 and 0.5 dB; Table E-I 1.5 and 2.0 dB;
  - F.1487 Annex 3 channel table.

### 4.5 Open items to verify

1. ~~E.5.4 equation glyphs~~ **Closed (QA-004, issue #8):** rendered in 110D w/Change 1
   p.247. The printed time-domain formula is a typesetting trap (see Section 2.1.1): build
   from the prose (sigma = d/2, exp(−π² d² t²), tau = 0.683/d).
2. ~~110D D.6.2 static multipath phase convention~~ **Closed (QA-003, issue #7):** the
   standard states none; it is the simulator's choice (see B2).
3. ~~110D Table D-I symbol rates for 30-48 kHz~~ **Closed (QA-003, issue #7):**
   24,000/28,800/33,600/38,400 Bd, so E.5.1 needs 96/115.2/134.4/153.6 kS/s (see B3).
4. STANAG 4285/4415/4539 test annexes (not public). Obtain through national channels if
   4415/4285 conformance is claimed.
5. MDM-Q9604 datasheet full text, S250C Tables 1.1/1.2, the NTIA "HF Simulator" (1991)
   manual and Hall/Akima APD coefficients. Each was blocked or not retrieved.
6. Whether ITU-R P.372-18 still carries APD and Vd curves.

---

## 5. Sources

All web sources accessed 2026-10-08 unless stated. Local owner-supplied files were read on
the same date.

| Id | Source | Location |
|---|---|---|
| S1 | MIL-STD-188-110D (29 Dec 2017), Appendix E pp.246-251, Appendix D refs | https://everyspec.com/MIL-STD/MIL-STD-0100-0299/download.php?spec=MIL-STD-188-110D.055856.pdf |
| S2 | MIL-STD-188-110C w/Change 1 (3 Jan 2012), Appendix E pp.219-226 | https://www.sigidwiki.com/images/c/c8/MIL-STD-188_110C_CHG_NOTICE-1.pdf |
| S3a | hfmodem SME oracle, `hfmodem:mil-std-188-110d:constraint:performance-requirements` (110D 5.3.2.5, Table XVI, D.6-D.6.5, E.1) | sme-oracle MCP, context mil-std-188-110d |
| S3b | hfmodem SME oracle, `hfmodem:mil-std-188-110d:lesson:hf-channel-testing` (F.1487 summary, Furman & Nieto pp.1-12, pathsim and HFSimulator code reading) | sme-oracle MCP, context mil-std-188-110d |
| S3c | hfmodem SME oracle, `hfmodem:mil-std-188-110b:constraint:performance-requirements` (Table XX, C.6.1-C.6.4) | sme-oracle MCP, context mil-std-188-110b |
| S3d | hfmodem SME oracle, `hfmodem:mil-std-188-110d:guide:edition-overview` and `...:check:implementation-checklist` | sme-oracle MCP, context mil-std-188-110d |
| S3e | hfmodem SME oracle, `hfmodem:mil-std-188-110a:constraint:performance-requirements` (skeleton) | sme-oracle MCP, context mil-std-188-110a |
| S5 | Furman, W.N., Nieto, J.W., "Understanding HF Channel Simulator Requirements in Order to Reduce HF Modem Performance Measurement Variability," HF01, Faro, 2001 (cited in E.2.1; content via S3b) | not fetched directly |
| S6 | ITU-R F.1487 (05/2000) | https://www.itu.int/dms_pubrec/itu-r/rec/f/R-REC-F.1487-0-200005-I!!PDF-E.pdf |
| S7 | Hoffmeyer, J.A., Nesenbergs, M., "Wideband HF Modeling and Simulation," NTIA Report 87-221, July 1987 | https://its.ntia.gov/publications/download/87-221_ocr.pdf |
| S9 | Vogler, L.E., Hoffmeyer, J.A., "A New Approach to HF Channel Modeling and Simulation, Part II: Stochastic Model," NTIA TR-90-255, Feb 1990 | https://its.ntia.gov/publications/download/90-255_ocr.pdf |
| S10 | Vogler & Hoffmeyer, "... Part III: Transfer Function," NTIA TR-92-284 (title block 93-284), Mar 1992 | https://www.its.ntia.gov/publications/download/92-284.pdf |
| S11 | Sutherland, D.A., "Software Implementation of a Wideband HF Channel Transfer Function," NTIA TR-98-348, Apr 1998 (catalogue page) | https://its.ntia.gov/publications/details?pub=2379 |
| S12 | NNN0WWL, "Rockwell MDM-3001 used as an HF Channel Simulator" (3 pp.), locally consulted source |
| S13 | Rockwell Collins MDM-Q9604 data sheet (search excerpts; URL redirects to collinsaerospace.com) | https://www.rockwellcollins.com/~/media/Files/Unsecure/Products/Product%20Brochures/Communcation%20and%20Networks/Communication%20Radios/MDMQ9604%20data%20sheet.aspx |
| S14 | Collins Aerospace HSM-2051 High Speed Modem product page | https://www.rtx.com/collinsaerospace/what-we-do/industries/military-and-defense/communications/ground-communications/ground-vhf-uhf-l-band-communications/hsm-2051-high-speed-modem |
| S15 | Signatron S250C HF/VHF Channel Simulator User's Manual, May 1996 (DTIC; HTTP 403, search excerpts only) | https://apps.dtic.mil/sti/pdfs/ADA333380.pdf |
| S16 | Fraunhofer IIS DT4630 RTSIM product page | https://www.iis.fraunhofer.de/en/ff/kom/leist/ergebnisse/rtsim.html |
| S17 | Muething, R. (KN6KB) et al., "IONOS HF/VHF-UHF Channel Simulator," rev 2.03, Nov 2020 | https://winlink.org/sites/default/files/downloads/teensy_ionos_hf_manual_rev_2.03.pdf |
| S18 | Wheatley, M. (AE4JY), "HF Path Simulator Project" (PathSim) | https://moetronix.com/ae4jy/pathsim.htm |
| S19 | bubnikv/pathsim (OK1IAK rework), GPL-3.0 | https://github.com/bubnikv/pathsim |
| S20 | Forrer, J. (KC7WW), "A Low-Cost HF Channel Simulator for Testing and Evaluating HF Digital Systems," TAPR DCC 1999 (proceedings pagination) | https://web.tapr.org/meetings/DCC_1999/DCC1999-HFchannelSimulator-KC7WW.pdf |
| S21a | MathWorks, "HF Ionospheric Channel Models" | https://www.mathworks.com/help/comm/ug/hf-ionospheric-channel-models.html |
| S21b | MathWorks, `stdchan` reference | https://www.mathworks.com/help/comm/ref/stdchan.html |
| S21c | MathWorks, `doppler` reference | https://www.mathworks.com/help/comm/ref/doppler.html |
| S21d | MathWorks, MIL-STD-188-110B baseband end-to-end link example | https://www.mathworks.com/help/comm/ug/defense-communications-us-mil-std-188-110b-baseband-end-to-end-link.html |
| S22a | GNU Radio, Dynamic Channel Model block definition | https://raw.githubusercontent.com/gnuradio/gnuradio/main/gr-channels/grc/channels_dynamic_channel_model.block.yml |
| S22b | GNU Radio, Channel Model block definition | https://raw.githubusercontent.com/gnuradio/gnuradio/main/gr-channels/grc/channels_channel_model.block.yml |
| S23 | codec2 `src/ch.c` (search excerpts of source and help text) | https://pgit.metala.org/github.com/drowe67/codec2/plain/src/ch.c |
| S24 | WSJT-X `fst4sim` manual page (search excerpt) | https://manpages.ubuntu.com/manpages/noble/man1/fst4sim.1.html |
| S25 | Linrad mailing list, "Internal signal generator" (search excerpt) | https://groups.google.com/g/linrad/c/y09BschXFgA |
| S26 | Johnson, E.E., "HF Simulator: Channel and Modem Modules," NTIA contractor report, July 1991 (catalogue page; PDF over fetch limit) | https://www.its.ntia.gov/publications/details?pub=3444 |
| S27 | Johnson, E.E., Peach, D.F., "High-frequency radio channel error model," NTIA TR-94-310, June 1994 | https://its.ntia.gov/publications/details?pub=2337 |
| S28 | Keysight PROPSIM platforms page | https://www.keysight.com/us/en/products/channel-emulators/propsim-platforms.html |
| S29 | Spirent SR5500 reseller data (search excerpt) | https://valuetronics.com/cdn/shop/files/Spirent_SR5500.pdf |
| S30 | Isode, HF Simulation Test Tools (MoRaSky, HF Tool) | https://isode.com/products/hf-simulation-test-tools.html |
| S31 | Scribd 638478921, "H.F Channel Model for Modem Design" (landing page only; body blocked) | https://www.scribd.com/document/638478921/a-model-of-high-frequencies-hf-channel-used-to-design-a-modem-of |
| S32 | Daniels, R.C., Peters, S.W., "A New MIMO HF Data Link: Designing for High Data Rates and Backwards Compatibility," IEEE MILCOM 2013 preprint, locally consulted reference |
| S33 | Giles, T.C., "On the Design of HF Radio Modems," PhD thesis, University of Adelaide, Dec 1995, locally consulted reference (printed page = PDF page - 4) |
| S34 | Lemmon, J.J., Behm, C.J., "Wideband HF Noise/Interference Modeling Part I: First-Order Statistics," NTIA 91-277, May 1991 (front matter); Part II NTIA TR-93-293 | https://its.ntia.gov/publications/download/TR-91-277.pdf ; https://www.its.ntia.gov/publications/details?pub=2319 |
| S35 | Hall, H.M., Stanford SEL-66-052, 1966 (DTIC AD0648650; search excerpt) | https://apps.dtic.mil/sti/pdfs/AD0648650.pdf |
| S36 | Watterson, C.C., "An ionospheric channel simulator," ERLTM-ITS 198, 1969 (catalogue) | https://its.ntia.gov/publications/details?pub=3106 |
| S37 | ITU-R P.372-18 (09/2026), and search-excerpted secondary sources on P.372/APD (incl. Tarasov et al. 2016) | https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.372-18-202609-I!!PDF-E.pdf ; https://vestnik.rsreu.ru/en/archive/2016-en/3-issue-57/359-conversion-of-amplitude-probability-distribution-of-envelope-of-atmospheric-radio-noise-from-one-bandwidth-to-another-using-generalized-empirical-model |
| S38 | Bergsvik, T., FFI 1998 HF modem evaluation (search excerpt) | https://www.ffi.no/publikasjoner/arkiv/evaluering-av-hf-modemer-og-flerfrekvenssystemer-basert-pa-kanalmalinger-pa-hoye-breddegrader |
| S39 | Angling, M.J., "Measured and simulated scattering functions and their impact on HF systems," PhD thesis, Leicester (search excerpt) | https://figshare.le.ac.uk/articles/thesis/Measured_and_simulated_scattering_functions_and_their_impact_on_high_frequency_HF_systems/10162685/1 |
| S40 | Cannon, Angling et al., "DAMSON HF channel characterisation - a review," MILCOM 2000 (catalogue) | https://research.birmingham.ac.uk/en/publications/damson-hf-channel-characterisation-a-review-2 |
| S41 | URSI GA 2005 paper G02a.0893 (search excerpt mentioning the Mastrangelo et al. wideband simulator) | https://www.ursi.org/files/CommissionWebsites/commg_ursi2005/oral/g02a_0893.pdf |
| S42 | Weston, Koski, "High Frequency Radio Network Simulation Using OMNeT++" (Harris), arXiv:1509.03176 (search excerpt) | https://arxiv.org/pdf/1509.03176 |
| S43 | RAE modem evaluation using the RAE Cobbett Hill HF Channel Simulator (DTIC ADA215692; search excerpt) | https://apps.dtic.mil/sti/pdfs/ADA215692.pdf |
| S44 | Crypto Museum, NATO STANAG equipment page (Telefunken ECHOTEL modems) | https://www.cryptomuseum.com/intel/nato/stanag |
| S45 | Isode whitepaper, S5069 and S4539 measurements over a channel simulator (search excerpt) | https://www.isode.com/whitepaper/measurements-of-s5069-and-s4539-waveforms-with-varying-interleavers-over-a-channel-simulator/ |
| S46 | NIST Special Database 21 (search excerpt) | https://www.nist.gov/node/759776 |
| S47 | Signal Identification Wiki, STANAG 4539 and STANAG 4415 pages (search excerpts) | https://sigidwiki.com/wiki/STANAG_4539 ; https://sigidwiki.com/wiki/STANAG_4415 |
| S48 | "Digital HF multipath simulator," The Radio and Electronic Engineer, 1977 (search excerpt) | https://digital-library.theiet.org/content/journals/10.1049/ree.1977.0048 |
| S49 | Abdullah, S.N., "Data transmission at 9600 bit/sec over an HF radio link," Loughborough thesis, 1986 (catalogue; not read) | https://repository.lboro.ac.uk/articles/thesis/Data_transmission_at_9600_bit_sec_over_an_HF_radio_link/9539948 |
| S50 | This repository: `docs/modules/signal-lab-engine.md`, `signal-lab/include/signal_lab/scenario.hpp`, `signal-lab/src/band_fir.hpp`, `docs/modules/pathsim.md` | local, read 2026-10-08 |
| S51 | Mendieta-Otero et al., "Interference Simulator for the Whole HF Band: Application to CW-Morse," arXiv:2402.04742 | https://arxiv.org/abs/2402.04742 |
| S52 | "Wideband HF Skywave Propagation: A Review with Link Modeling Corroborated by Propagation Data," arXiv:2609.05811 (abstract) | https://arxiv.org/abs/2609.05811 |
| S53 | "Implementation and comparison of a wideband HF noise and interference simulator," IET conference 2006 (catalogue) | https://digital-library.theiet.org/content/conferences/10.1049/cp_20060254 |

Sources that could not be retrieved:

- **DTIC PDFs** (S15, S35, S43, ADA261810) returned HTTP 403 to both WebFetch and curl.
- **MDM-Q9604 datasheet** (S13) redirected.
- **NTIA "HF Simulator" (1991) PDF** (S26) exceeded the 10 MB fetch limit.
- **Scribd document body** (S31) was blocked.
- **NATO STO MP-IST-056-17** (DSTO, Harris & Scholz, on Watterson validity in Australia)
  returned 403.
- **STANAG 4285/4415/4539 texts** are not publicly available.
