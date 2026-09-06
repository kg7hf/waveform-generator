# Phase 1 report: deterministic M110B test-waveform and impairment corpus

Date: 2026-09-06. Scope: Phase 1 of the phased test-waveform and
dynamic-interference prompt (`docs/Prompt — Phased M110B Test Waveform and
Dynamic Interference Generator.md` in this directory). Everything here is engineering evidence for receiver development; nothing in it
is a formal MIL-STD-188-110B conformance result.

## 1. Existing repository architecture (what was found and reused)

**This generator (`tools/waveform-generator`).** A standalone nested project.
Its RT1170 firmware is a transparent one-shot player: it validates exactly one
container (RIFF/WAVE, format tag 1, 48 000 Hz, mono, PCM16, one `fmt ` and one
`data` chunk, RIFF size + 8 equal to the file size), reads `2:/WG/PLAY.WAV`
through read-only FatFs in 8 KiB chunks from the priority-4 `wfg_player` task
into a static 32 768-frame int16 ring (16 384-frame prefill), and the WM8960
driver's priority-7 `m110_audio` task pulls 128-frame blocks through a single
`CodecBlockHook` that duplicates each mono sample into a 32-bit stereo I2S slot
for SAI1/eDMA ping-pong (two 512-frame non-cacheable buffers). Control is a
temporary four-command LF-terminated CDC protocol (`STATUS`, `MEDIA HOST`,
`MEDIA LOCAL`, `PLAY`); the frozen WFG/1 JSON protocol is documented but not
implemented. The project contains no impairment, mixing, noise or channel code;
its only synthesized source is a 48-sample 1 kHz tone table in a separate
firmware image. Host Python is `stage_wav.py` (card staging, WAV validation),
`analyze_capture.py` (capture metrics) and the WinMM/WASAPI `waveform_capture`
recorder. The Phase 3 insertion point for a portable impairment engine is
`rt1170/player.cpp`, between `read_payload_chunk()` (fills an 8 KiB buffer, i.e.
4096 mono frames) and `push_samples()` (copies into the ring), in the
priority-4 player task where about 340 ms of ring headroom exists; the
priority-7 audio hook forbids DSP, allocation and I/O.

**M110 transmitter (reused as-is).** `m110_tx_to_pcm <rate> <short|long> <out.wav> --file <payload>`
(parent repo, `tools/tx_to_pcm.cpp`) is the authoritative waveform source. It
writes 48 kHz mono PCM16 with no leading silence, the preamble at sample 0 and
exactly 1.000 s of trailing silence. Measured on every reference: RMS
−8.40 dBFS, peak −0.6 to −1.3 dBFS. Framing (from `core/transmitter.cpp`):
`bits = 8·payload + 32 (EOM) + 144 (flush)`, `blocks = ceil(bits / block_bits)`,
LONG = 4.8 s preamble + 4.8 s blocks, SHORT = 0.6 s preamble + 0.6 s blocks.
The reference sidecars predict the sample count from this rule and every one
of the twelve references matched exactly.

**Decoders (reused as-is).** `m110_app_decode` (the application's streaming
receiver plus the whole-burst turbo engine, `--engine siso`; `--engine adaptive`
is the DFE/Viterbi path) prints a line-oriented text contract:
`burst=… mode=<rate>/<long|short>`, `eom=found|not-found eom_bit_errors=N`,
`exact_payload=yes|no expected_bytes= received_bytes= bit_errors= compared_bits= missing_bytes=`,
`stream_release=…`, `stream_summary … loss_of_lock= discontinuities= frontend_clipped=`,
and `summary decoded_bursts= exact_payloads= clipped_samples= audio_quality=`.
Scoring runs it with the production receiver caps (`--maximum-payload-octets 1200000`,
the same value `app/modem_host.cpp` uses) so multi-minute messages are never
truncated by the 4096-octet default.

**pathsim campaign tooling (conventions reused, binary not used).** The PN-11
payload generator (`pn11_payload`: ITU-T O.153 x¹¹+x⁹+1, seed 0x7FF, MSB-first
packing, tiled) is copied verbatim and unit-tested byte-identical against
`tools/pathsim-campaign.py`; the corner table (`CONDITIONS`/`SHORT_CONDITIONS`)
is cited in the corpus spec and cross-checked by a test. pathsim itself is
8 kHz-only, GPL, and only needed for Watterson fading, which is out of Phase 1
scope.

**pcm_stress (algorithms and conventions adopted, file untouched).** The owner's
`tools/pcm_stress.py` already renders band-limited AWGN, raised-cosine fades,
Hann-gated noise packets and CW tones relative to one reference-interval RMS.
Its 513-tap Blackman-windowed-sinc 300–3300 Hz kernel, its SNR/level
definitions, its raised-cosine fade shape, its tone ramp/phase model, its PCM16
quantization and its PCG64 `SeedSequence([seed, family, index])` seeding were
adopted into `signal_lab` (import ledger entry `m110-025`), and a unit test
proves the shared models reproduce pcm_stress bit-exactly. pcm_stress could not
be used directly because its schema is closed, its pipeline is strictly
frame-preserving (no sample slips), it has no exponential-decay crash model, and
the handoff marks it owner-parallel work.

**tests/impairments.cpp** holds complex-baseband C++ primitives (AWGN, NCO,
impulse, linear resampler) compiled only into the unit tests; they are a useful
reference for Phase 2 but operate on IQ samples, not PCM.

## 2. What was built

```
host/signal_lab/            waveform-agnostic impairment engine (Phase 1 Python form)
  stream.py                 Impairment base class + Pipeline (4096-frame blocks, absolute input-frame indexing)
  awgn.py cw.py impulse.py fade.py sample_slip.py
  scenario.py               portable scenario document, validation, canonical hash
  render.py                 scenario -> WAV + JSON sidecar
  wav_io.py rng.py
host/m110_reference.py      PN-11 payload sizing + transmitter invocation + reference sidecar
host/m110_score.py          decoder invocation, text-contract parser, alignment diagnostics, CSV/JSON summaries
host/corpus_phase1.py       refs -> validate -> render -> score -> summary -> verify
host/render_scenario.py     single-scenario CLI
corpus/phase1-corpus.json   the declarative corpus (modes, cases, seeds, corner table)
tests/test_signal_lab.py    16 unit tests (registered as ctest wfg-signal-lab)
test-vectors/simulated-interference/<MODE>/{reference,cw,impulse,fade,sample-slip,noise,mixed}/
                            + payload/ + manifests/
```

The engine interface is the one the prompt asks for: `PCM in → impairment
transform → PCM out`. Each stage is a `process(block, first_input_frame)`
method with state prepared once from `(reference_rms, total_frames, seed,
stage_index)`; phases, schedules and RNG streams are keyed by absolute input
frame so results do not depend on block size. That is the shape the Phase 2
C++ engine and the Phase 3 player task (4096-frame chunks) need.

### 2.1 Conventions

| Item | Convention |
|---|---|
| Container | 48 kHz mono PCM16, round-half-even, saturate (same as pcm_stress and the transmitter) |
| Reference level | RMS over the source's first..last non-zero sample (`"auto"`), or a declared interval; every level is relative to it |
| Source gain | `source_gain_db` applied before impairments (−12 dB for every corpus case, see §6.2); levels stay relative to the scaled reference |
| C/I | `ci_db = 20·log10(reference RMS / tone RMS)` |
| SNR | `snr_db = 20·log10(reference RMS / noise RMS)`, noise confined to 300–3300 Hz (the 3 kHz-band convention of Table 10.1 as realised by pcm_stress; not pathsim-qualified) |
| Crash peak | `peak_db` = envelope peak relative to reference RMS |
| Fade | multiplicative on the desired signal only; depths add in dB when fades overlap |
| Order | fade (propagation) → awgn → cw → impulse (receiver) → sample_slip (transport); `sample_slip` must be last |
| Seeds | PCG64 `SeedSequence([seed, family, stage_index])`; families 1 noise, 2 impulse noise ringing, 4 impulse schedule, 5 impulse phase, 6 slip, 7 tone phase, 8 fade schedule |
| Clipping | `clip_policy: saturate` counts clipped samples into the sidecar; `reject` aborts |

### 2.2 Sidecar contents (every WAV)

`test_id`, `seed`, generator identity (package SHA-256 of all `signal_lab`
modules, Python and NumPy versions), the full scenario and its canonical
SHA-256, source artifact (path, bytes, file SHA-256, PCM-region SHA-256,
samples), the source reference's identity (mode, rate, interleave, payload
artifact and hash, framing), the measured reference level, `source_gain_db`,
`clip_policy`, `impairment_order`, and per stage the configured parameters, the
fully resolved parameters, measured component RMS/peak and the event list
(crash times and phases, fade intervals, slip positions). Output: file and
PCM hashes, samples, duration, clipped samples, peak and RMS. Reference
sidecars additionally carry the producer identity and `argv`, the PN-11
definition, and the framing intervals. Score files (`*.<engine>.score.json`)
hold the decoder identity and `argv`, exit code, wall-clock time, the prompt's
§19 fields, and alignment diagnostics.

## 3. Reference signals

All twelve decode with `m110_app_decode --engine siso` as
`exact_payload=yes`, `bit_errors=0`, `eom=found`, one burst, correct mode and
interleaver. The payload for a given rate is identical for L and S; every
payload is a prefix of one master PN-11 stream
(`payload/m110_reference_payload_pn11_master.bin`, 65 536 bytes, SHA-256
`5f6a64818894c8078b6f2858ab51fdc5f09c90f4764165a780a1726990f4a9cc`).

| Mode | File | Payload bytes | Body blocks | Duration s | WAV SHA-256 (prefix) | Payload SHA-256 (prefix) |
|---|---|---:|---:|---:|---|---|
| 300L | m110_300L_reference_perfect.wav | 4170 | 24 | 121.0 | f46163a7edddce67 | 5f5933a68f746097 |
| 300S | m110_300S_reference_perfect.wav | 4170 | 187 | 113.8 | acb433d6ed851161 | 5f5933a68f746097 |
| 600L | m110_600L_reference_perfect.wav | 8363 | 24 | 121.0 | 9d45feabbe425beb | dd9386a2f50defe0 |
| 600S | m110_600S_reference_perfect.wav | 8363 | 187 | 113.8 | ba8b272545035a3f | dd9386a2f50defe0 |
| 1200L | m110_1200L_reference_perfect.wav | 16748 | 24 | 121.0 | deef6055458887c5 | 15ea57436384510a |
| 1200S | m110_1200S_reference_perfect.wav | 16748 | 187 | 113.8 | 152c160b9179cf20 | 15ea57436384510a |
| 300L | m110_300L_reference_perfect_5min.wav | 10920 | 61 | 298.6 | 30a125976f439ccb | b8690b5846a8bf7f |
| 300S | m110_300S_reference_perfect_5min.wav | 10920 | 487 | 293.8 | 9565e1d41cf957c9 | b8690b5846a8bf7f |
| 600L | m110_600L_reference_perfect_5min.wav | 21863 | 61 | 298.6 | fffe2f8aa10d6007 | d9efc64c6502cc5e |
| 600S | m110_600S_reference_perfect_5min.wav | 21863 | 487 | 293.8 | 43728f049f0fe913 | d9efc64c6502cc5e |
| 1200L | m110_1200L_reference_perfect_5min.wav | 43748 | 61 | 298.6 | 8775acc93399d324 | c69ea129ccb05588 |
| 1200S | m110_1200S_reference_perfect_5min.wav | 43748 | 487 | 293.8 | 25c1a68334a77a6e | c69ea129ccb05588 |

Full hashes are in `manifests/phase1-corpus-manifest.json` and each sidecar.
Level: RMS −8.40 dBFS, peak −1.1 dBFS, zero clipped samples, preamble
0–4.8 s (LONG) or 0–0.6 s (SHORT).

## 4. Impairment implementation

- **CW (`cw`)**: `A·sin(2πf·u/Fs + φ)` with `A = √2·ref·10^(−ci/20)`, 10 ms raised-cosine shoulders, optional random phase. Corpus: 1800 Hz at C/I +6/+3 dB and 2100 Hz at +3 dB on every mode; 1800 Hz at 0 dB, 600 Hz and 3000 Hz at +10 dB and 1500 Hz at +3 dB on 600L.
- **Static crashes (`impulse`)**: the prompt's model `A·e^(−t/τ)·cos(2πf_r t + φ)`, event length 8τ, `f_r` = 1700 Hz, phase per event from the seed, Poisson schedule (exponential inter-arrivals, as pcm_stress) or periodic; an optional broadband variant replaces the cosine with band-limited Gaussian ringing. Corpus profiles exactly as specified: light (0.2/s, +12 dB, 5 ms), moderate (1/s, +20 dB, 20 ms), severe (5/s, +30 dB, 35 ms). At the fixed −12 dB source gain the +20 dB peaks touch full scale occasionally and the +30 dB peaks saturate hard, which is what an ADC does; clipped counts are in every sidecar.
- **Fades (`fade`)**: raised-cosine (attack = recovery = duration/4, hold = duration/2) or rectangular; explicit, periodic or Poisson schedules; overlapping fades multiply. Corpus: 12 dB/250 ms, 18 dB/250 ms, 24 dB/500 ms every 10 s from t = 10 s on every mode; 18 dB/100 ms and rectangular 24 dB/250 ms on 600L.
- **Sample slips (`sample_slip`)**: deletion of a run or re-emission of the last delivered run, positions in input-frame coordinates, single/spaced/clustered placements. Corpus: 5-sample deletions and duplications every 20 s (5 events), 48-sample clustered deletions (5 at 0.5 s spacing) on every mode; single 1-sample deletion, single 48-sample duplication and clustered 1-sample duplications on 600L.
- **AWGN (`awgn`)**: band-limited Gaussian at `corner + offset` using the corner table (300/600: 7 dB, 1200: 11 dB; S rows reuse the L corner). Corpus: corner, −2, −4 on every mode; −8, −12 and the gain-control ladder (−18/−24/−28 dB source gain at corner) on 600L.
- **Mixed**: MIX-001…005 exactly as specified, on all six modes.

## 5. Corpus statistics

| Item | Count |
|---|---:|
| WAV files | 147 (12 references + 135 impaired), 1.7 GB, all 48 kHz mono PCM16 |
| Per mode | 22 each (300L, 300S, 600S, 1200L, 1200S); 600L 37 (priority-mode extras) |
| Per family (impaired) | cw 22, impulse 19, fade 20, sample-slip 21, noise 23, mixed 30 |
| Per severity | light 29, moderate 55, severe 45, extreme 3, control 3 |
| Scoring | every file × 2 engines (`siso` turbo, `adaptive` DFE/Viterbi), production caps |
| Decode wall-clock | siso mean 11.0 s per file (max 87.5 s, 1200S MIX-003); adaptive mean 0.5 s |
| Determinism | every scenario re-rendered by `--stage verify` and compared by PCM SHA-256: verify: 135 scenarios re-rendered, 0 mismatches |

Per-file results: `manifests/phase1-scores-siso.csv`, `manifests/phase1-scores-adaptive.csv`
(one row per file with the §19 fields), `manifests/phase1-summary.json` (mode × family
matrix per engine), and `<wav>.<engine>.score.json` beside each WAV.

## 6. Receiver findings

Complete-message recovery, `siso / adaptive` per cell. ✓ = complete message
(exact payload, exact EOM); ✗N = N payload bit errors with the message
otherwise delivered; ✗m = delivered part error-free but the message was
truncated (loss of lock / missing EOM); NA = no acquisition (no burst).
Column 600L includes the extra priority-mode cases (– elsewhere).

| Case | 300L | 300S | 600L | 600S | 1200L | 1200S |
|---|---|---|---|---|---|---|
| cw_1800_ci+03 | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✗424 / ✓ | ✗493 / ✓ |
| cw_1800_ci+06 | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| cw_2100_ci+03 | ✓ / ✓ | NA / NA | ✓ / ✓ | NA / NA | ✓ / ✓ | NA / NA |
| fade_12db_250ms | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| fade_18db_250ms | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| fade_24db_500ms | ✓ / ✓ | ✓ / ✗m | ✓ / ✓ | ✓ / ✗31 | ✓ / ✓ | ✓ / ✗343 |
| impulse_light | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| impulse_moderate | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✗12 / ✓ | ✓ / ✓ | ✗401 / ✓ |
| impulse_severe | ✗12413 / ✓ | ✗9860 / ✗m | ✗33890 / ✓ | ✗26007 / ✗m | ✗64333 / ✗20 | ✗67081 / ✗m |
| mix_MIX001 | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| mix_MIX002 | ✓ / ✓ | NA / NA | ✗102 / ✓ | NA / NA | ✗727 / ✓ | ✗4112 / ✓ |
| mix_MIX003 | ✓ / ✗m | NA / NA | ✓ / ✗m | NA / NA | ✗3978 / ✗m | ✗5876 / ✗m |
| mix_MIX004 | ✓ / ✓ | ✓ / ✓ | ✗2 / ✓ | ✗74 / ✓ | ✓ / ✓ | ✗365 / ✓ |
| mix_MIX005 | ✗11 / ✗m | NA / NA | ✗1178 / ✗m | ✗4311 / ✗m | ✗19068 / ✗m | NA / NA |
| noise_corner-02 | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| noise_corner-04 | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| noise_corner | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| reference_perfect | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| reference_perfect_5min | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| slip_del005_spaced | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| slip_del048_clustered | ✗10926 / ✗m | ✗10594 / ✗m | ✗22565 / ✗m | ✗21541 / ✗m | ✗45564 / ✗m | ✗43038 / ✗m |
| slip_dup005_spaced | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ | ✓ / ✓ |
| cw_0600_ci+10 | – | – | ✓ / ✓ | – | – | – |
| cw_1500_ci+03 | – | – | ✓ / ✓ | – | – | – |
| cw_1800_ci+00 | – | – | NA / NA | – | – | – |
| cw_3000_ci+10 | – | – | ✓ / ✓ | – | – | – |
| fade_18db_100ms | – | – | ✓ / ✓ | – | – | – |
| fade_24db_250ms_rect | – | – | ✓ / ✗m | – | – | – |
| impulse_moderate_noise | – | – | ✓ / ✓ | – | – | – |
| noise_corner-08 | – | – | ✗202 / ✗1772 | – | – | – |
| noise_corner-12 | – | – | ✗27454 / ✗m | – | – | – |
| noise_corner_gain-18 | – | – | ✓ / ✓ | – | – | – |
| noise_corner_gain-24 | – | – | ✗33334 / ✓ | – | – | – |
| noise_corner_gain-28 | – | – | ✗34368 / ✓ | – | – | – |
| slip_del001_single | – | – | ✓ / ✓ | – | – | – |
| slip_dup001_clustered | – | – | ✓ / ✓ | – | – | – |
| slip_dup048_single | – | – | ✓ / ✓ | – | – | – |

Totals: siso 105/147 complete, adaptive 113/147, at least one engine 119/147.
Twenty files are decoded by exactly one engine (§6.8).

### 6.1 Easy cases (both engines, every mode)

All references (2 and 5 min); CW at 1800 Hz C/I +6 dB; 600/1500/3000 Hz tones at
+10/+3 dB (600L); AWGN at corner, corner −2 and corner −4 dB (the AWGN-only margin is
at least 4 dB below the fading corner for every mode, as the conformance campaign
predicted); 12 and 18 dB fades of 100–250 ms every 10 s; light static (0.2/s, +12 dB);
MIX-001; 1- and 5-sample deletions and duplications, and a single 48-sample duplication.

### 6.2 Absolute input-level sensitivity of the turbo engine (new, actionable)

With no impairment the 600L reference decodes at every source gain from −12 to
−40 dB with both engines. With band noise at the corner SNR the turbo engine passes
at −12 and −18 dB and collapses at −24 and −28 dB (33 334 and 34 368 errors of
66 904 bits, i.e. random, EOM not found), while the adaptive engine still decodes
both files exactly. The relative SNR is identical in all four files; only the absolute
level changes. The turbo seed line shows the estimated `noise_variance` scaling with
level (0.0175 at −18 dB, 0.0037 at −24 dB) toward the fixed floor seen on clean
files (0.0010), so the engine's noise model is apparently in absolute input units
rather than relative to the equalized symbol scale. This reproduces the unexplained
−24 dB control failure in the owner's pcm_stress pilot and is the first defect the
receiver effort should fix: real receivers deliver widely varying absolute levels.
Until then every corpus comparison must hold the level fixed (all cases use −12 dB).

### 6.3 CW and static sensitivity: the turbo engine is the weaker one

- 1800 Hz at C/I +3 dB: both 1200 modes fail on siso (424 and 493 bit errors) and pass
  on adaptive; 300 and 600 pass on both. At 0 dB C/I (600L) nothing acquires.
- Moderate static (1/s, +20 dB, 20 ms, saturating): siso fails on 600S and 1200S,
  adaptive passes on all six. Severe static (5/s, +30 dB, 35 ms, about 10 % of samples
  saturated): siso fails everywhere; adaptive still decodes 300L and 600L completely
  and 1200L with 20 errors. The broadband-ringing variant (600L) passes on both.
- MIX-002 (corner −2, CW +6, moderate static): siso fails on 600L/1200L/1200S; adaptive
  passes every mode that acquires. MIX-004 (corner, moderate static, 5-sample slips):
  siso fails on 600L (2 errors), 600S (74) and 1200S (365); adaptive passes all six.

The pattern is consistent with the turbo engine's Gaussian noise model being hurt by
non-Gaussian interference (tones, saturated crashes) that the DFE/Viterbi path
tolerates. Impulse blanking / heavy-tailed likelihoods and a CW notch in front of the
turbo engine are therefore the receiver ideas this corpus can now measure directly.

### 6.4 Acquisition failures: SHORT interleaver under CW plus noise

A 2100 Hz tone at C/I +3 dB defeats acquisition on every SHORT mode (300S, 600S,
1200S, both engines) while every LONG mode acquires and decodes it; the same tone
level at 1800 Hz acquires on all six. MIX-002, MIX-003 and MIX-005 (1800 Hz CW plus
noise) do not acquire on 300S and 600S either. The 0.6 s (3-segment) preamble gives
the detector too little to reject a strong tone; the 4.8 s LONG preamble does not
have this problem. A related diagnostic: on clean SHORT references the burst header
reports `correlation≈0.58, frequency_offset≈−21.7 Hz` (LONG: 0.9999, 0.07 Hz) even
though the decode is exact, so the SHORT-preamble frequency estimate is already
coarse before any impairment.

### 6.5 Fade / recovery: the adaptive engine loses lock, the turbo engine does not

Every fade case (12/18/24 dB, 100–500 ms, raised-cosine or rectangular, every 10 s)
decodes completely with the turbo engine. The adaptive engine fails 24 dB/500 ms on
all three SHORT modes and the rectangular 24 dB/250 ms on 600L by truncating the
message (0 errors in the delivered part, then loss of lock), and MIX-003 (fade + CW
+ noise) on every LONG mode the same way. Whole-burst re-estimation across fades is
the turbo engine's clear strength.

### 6.6 Sample-discontinuity failures

Single-sample and 5-sample deletions/duplications, spaced or clustered, are absorbed
by symbol-timing tracking on both engines (zero errors). Five clustered 48-sample
deletions (240 samples = 12 symbols lost within 2 s at t = 40 s) are catastrophic
for both engines on every mode: the turbo engine's payload is correct up to the
first event (600L: 2551 correct bytes = 40.7 s) and random after it, the adaptive
engine releases the burst. A single 48-sample duplication (600L) is decoded
exactly, so the damage is the cumulative timing jump, not one slip. The alignment
diagnostic finds no byte shift that re-aligns the tail, i.e. the receiver does not
re-synchronise after a multi-symbol slip within a message.

### 6.7 Noise cliff (600L)

corner −8 dB (−1 dB SNR): siso 202 errors in 66 904 bits with EOM found; adaptive
1772 errors plus a truncated tail. corner −12 dB (−5 dB SNR): both fail. The
band-limited AWGN cliff of the turbo engine on 600L therefore lies between −1 and
−5 dB SNR in this convention, about 8–12 dB below the 7 dB fading corner.

### 6.8 LONG versus SHORT, and engine complementarity

LONG modes: siso 62/81, adaptive 67/81 complete. SHORT modes: siso 43/66, adaptive
46/66. SHORT loses on acquisition (§6.4), on moderate static with the turbo engine,
and on deep fades with the adaptive engine; 300L is the most robust mode overall
(19/22 on both engines, and MIX-005 misses by only 11 bits on siso).
The two engines disagree on 20 files: adaptive wins 13 (CW +3 dB on 1200,
moderate/severe static, MIX-002, MIX-004, the low-level controls), siso wins 7
(deep fades, MIX-003). This is exactly the "uncorrelated recovery" the interference
specification asks for: a late selection or fusion between the two engines would
raise complete-message recovery from 105 or 113 to 119 of 147 on this corpus.

## 7. Phase 2 recommendation

Promote all five Phase 1 impairment models into the portable C++ engine; each one
produced receiver-discriminating cases:

| Model | Promote? | Phase 2 / real-time notes |
|---|---|---|
| CW tone | yes | trivial: per-sample phase accumulator keyed by absolute sample index (the tone.cpp pattern) |
| Static crash (tone ringing + noise ringing) | yes | precompute each event's 8τ waveform when scheduled (at most 19 200 samples at 50 ms); Poisson schedule from the seeded RNG |
| Fade (raised-cosine, rectangular; periodic/explicit/Poisson) | yes | gain table per block; overlapping fades multiply |
| Sample slip (delete / duplicate) | yes | 48-sample history buffer; output length varies per block, so the player must tolerate variable push sizes |
| Band-limited AWGN | yes, with a change | the 513-tap FIR via 8192-point FFT is host-only; on the RT1170 use a direct FIR (about 25 MMAC/s at 48 kHz) or a shorter kernel and record the kernel hash in the scenario so host and target can be compared |

Keep: the scenario JSON as the single portable description; the reference-RMS level
convention; seeding by `(seed, family, stage_index)` with one RNG family per stream.
Change for C++: replace NumPy's PCG64 with a small fixed-width generator implemented
identically on host and target (PCG32 or xoshiro128**), and specify float32 math with
a defined summation order so the host renderer and the RT1170 produce the same
samples to within one LSB rather than promising bit-identity across NumPy versions.
Do not promote: pcm_stress's Hann-gated packet model (covered by the noise-ringing
crash), burst/adjacent-channel interference and sample-rate error (not exercised in
Phase 1; add them in Phase 2 only after the five models are in C++).

## 8. Caveats and open items

- "corner" is a label: the values are the Table 10.1 fading-channel SNRs applied as
  band-limited AWGN in the pcm_stress convention (not the pathsim-qualified +0.40 dB
  delivered SNR); AWGN-only results say nothing about conformance.
- One seed per case. Boundaries above are single-realisation engineering screens; the
  specification's 10-seed confirmation is Phase 2 work (the corpus spec takes any seed base).
- The SHORT-preamble frequency-offset artifact (§6.4) needs a look in the acquisition
  code before it is blamed for the CW failures.
- Untested in Phase 1: single 48-sample deletion and clustered 48-sample
  duplications (to separate cluster from direction), CW frequency sweeps, phase
  rotation through fades, sample-rate error, adjacent-channel signals.
- `signal_lab` needs NumPy; the generator's earlier host tools are standard-library
  only. Phase 2 removes the dependency by moving the engine to C++.
- Tracked corpus outputs (payloads, manifests) enter this project's source manifest
  and hence the firmware build identity; move them to `artifacts/` or exclude
  `test-vectors/` in `CMakeLists.txt` if that is unwanted.
- Nothing in this report is committed; the WAVs are regenerated bit-exactly from
  `corpus/phase1-corpus.json` by `host/corpus_phase1.py`.
