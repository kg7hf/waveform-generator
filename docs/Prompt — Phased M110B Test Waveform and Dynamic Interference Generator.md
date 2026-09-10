# Task: Phased M110B Test-Waveform and Dynamic Interference Generator

We want to develop a phased test-signal capability for the M110 modem project.

The immediate goal is **not** to build the entire dynamic RT1170 interference simulator at once.

The work shall deliberately proceed in phases:

1. **Phase 1 — quickly produce useful deterministic WAV test files**
2. **Phase 2 — isolate the interference algorithms into a portable reusable library**
3. **Phase 3 — integrate the same impairment engine into the existing RT1170 waveform generator**
4. **Phase 4 — add live dynamic control of interference while the RT1170 is playing a WAV**
5. **Future — optionally generate M110 or other waveforms natively, without making the interference system waveform-specific**

The existing RT1170 waveform generator is located at:

```text
M110-modem\tools\waveform-generator
```

Inspect this implementation before proposing or writing new architecture.

The existing M110 transmitter/encoder and decoder shall remain the authoritative source of the M110 waveform.

Do not independently reimplement M110 modulation unless there is no reusable transmitter path in the repository.

---

# 1. Read Existing Project Material First

Before making changes, inspect the repository and locate/read:

```text
simulated-interference-specifications.md
M110-mode-test-thresholds.md
M110-the-sky-is-the-limit.md
M110-beyond-conformance-improvement-opportunities.md
```

Also inspect:

```text
M110-modem\tools\waveform-generator
```

and identify:

- how WAV files are currently read;
- how PCM buffers are represented;
- how the RT1170 feeds its DAC/audio codec;
- current sample rate and sample format;
- buffering/DMA architecture;
- FreeRTOS task structure;
- whether waveform playback is mono/stereo;
- current file format restrictions;
- existing USB CDC command/control support;
- existing transmitter/waveform-generation utilities;
- `pathsim`;
- `pcm_stress`;
- existing WAV/test-vector creation utilities.

Reuse existing functionality where practical.

Do not duplicate working channel or interference code merely to create a new tool.

---

# 2. Overall Architectural Principle

The **interference engine must be waveform-agnostic**.

It shall operate on PCM/sample streams.

Conceptually:

```text
any clean waveform
       |
       v
PCM sample stream
       |
       v
interference / impairment engine
       |
       v
modified PCM sample stream
```

The source may eventually be:

```text
M110 WAV
FT8 WAV
voice WAV
PSK waveform
another modem waveform
native M110 generator
future native FT8 generator
```

The impairment engine must not require knowledge of the protocol that generated the PCM.

This is important.

We want to be able to test arbitrary waveforms later by simply loading a clean WAV file.

---

# 3. Phase 1 — Immediate Deliverable: Short Deterministic Test WAVs

## 3.1 Phase-1 Objective

Produce a useful initial test corpus quickly.

Do **not** attempt to build the complete dynamic RT1170 impairment generator in Phase 1.

Generate clean and impaired WAV files for these modes:

```text
300L
300S

600L
600S

1200L
1200S
```

Do not include 150, 75, or 2400 in this first implementation unless doing so is nearly free after the framework exists.

The immediate priority is these six configurations.

---

# 4. Phase-1 Reference WAVs

Generate one **perfect deterministic reference waveform** for each mode:

```text
m110_300L_reference_perfect.wav
m110_300S_reference_perfect.wav

m110_600L_reference_perfect.wav
m110_600S_reference_perfect.wav

m110_1200L_reference_perfect.wav
m110_1200S_reference_perfect.wav
```

Each waveform shall contain:

- valid acquisition/preamble;
- correct M110 mode;
- correct LONG or SHORT interleaver;
- valid payload/data body;
- valid EOM;
- no channel fading;
- no intentional noise;
- no CW interference;
- no sample-clock distortion;
- no sample drops;
- no clipping.

Use the project's normal:

```text
48,000 samples/sec
```

and existing PCM/WAV convention.

Do not invent a new sample format if existing tooling already has one.

---

# 5. Test Signal Duration

The test signals should be long enough to expose intermittent receiver behavior, but short enough to manage easily.

Generate reference/test signals between approximately:

```text
1 minute and 5 minutes
```

Recommended initial duration:

```text
~2 minutes
```

unless existing framing/transmitter tools make another duration more natural.

At least one longer test per mode should eventually be approximately:

```text
5 minutes
```

Do not create one-hour files for Phase 1.

The objective is fast iterative development.

---

# 6. Deterministic Payload

Prefer the existing project PN-11 payload convention if available.

The payload should:

- be deterministic;
- allow exact BER comparison;
- contain enough data to span many blocks;
- end cleanly with valid EOM.

Prefer using the **same logical payload pattern** for all modes.

Save the expected payload separately.

Example:

```text
test-vectors/reference/m110_reference_payload.bin
```

---

# 7. Validate Every Perfect WAV

Before adding any impairment:

Run each perfect reference through the existing decoder.

Require:

```text
acquisition       PASS
mode detection    correct
interleaver       correct
payload           correct
bit_errors        0
BER               0
EOM                PASS
```

If any clean reference does not decode perfectly, fix that problem before generating impaired derivatives.

---

# 8. Phase-1 Interference Scope

Phase 1 should deliberately implement only a manageable subset of the larger `simulated-interference-specifications.md`.

Start with these five impairment families:

1. **CW interference**
2. **static crashes / impulsive noise**
3. **controlled fades**
4. **sample deletion / duplication**
5. **AWGN / lower-SNR background**

Additionally create a small number of **mixed-interference files** combining them.

Do not attempt every possible interference permutation in Phase 1.

---

# 9. CW Interference

Implement a sinusoidal interferer:

```text
desired signal + CW tone
```

Required initial frequencies:

```text
600 Hz
1500 Hz
1800 Hz
2100 Hz
3000 Hz
```

The 1800-Hz case is particularly important.

Initial C/I levels:

```text
+10 dB
+6 dB
+3 dB
0 dB
```

For Phase 1, do not create the full Cartesian product.

Select representative cases.

Example:

```text
m110_600L_cw_1800_ci+06.wav
m110_600L_cw_1800_ci+03.wav
m110_600L_cw_2100_ci+03.wav
```

---

# 10. Static Crashes / Impulse Noise

Implement at least one useful simulated static-crash model.

Prefer:

```text
high amplitude impulse
      +
exponential decay
      +
optional ringing
```

Initial profiles:

## Light static

```text
rate      = 0.2 events/sec
peak      = +12 dB
decay     = 5 ms
```

## Moderate static

```text
rate      = 1 event/sec
peak      = +20 dB
decay     = 20 ms
```

## Severe static

```text
rate      = 5 events/sec
peak      = +30 dB
decay     = 20–50 ms
```

Use deterministic seeds.

---

# 11. Controlled Fade Tests

Implement deterministic amplitude fades.

Initial depths:

```text
12 dB
18 dB
24 dB
```

Initial durations:

```text
100 ms
250 ms
500 ms
```

At least support:

```text
raised-cosine fade
```

and optionally:

```text
rectangular fade
```

Create several cases with repeated fades throughout the 1–5 minute recording.

---

# 12. Sample Deletion / Duplication

Create tests representing audio/USB sample-stream corruption.

Initial deletion lengths:

```text
1 sample
5 samples
48 samples
```

Initial duplication lengths:

```text
1 sample
5 samples
48 samples
```

At 48 kHz:

```text
48 samples = 1 ms
```

Place events deterministically.

Include:

- one event;
- several widely spaced events;
- several clustered events.

---

# 13. AWGN / Low-SNR Tests

Reuse the existing project channel/noise implementation if appropriate.

Do not duplicate `pathsim` functionality unnecessarily.

Generate several useful relative-SNR conditions for each mode.

Prefer relative specification:

```text
corner
corner -2 dB
corner -4 dB
```

using the project's authoritative mode-corner table.

Do not hard-code guessed corner SNR values.

---

# 14. Mixed Phase-1 Test Files

Create a small set of deliberately useful mixed cases.

At minimum:

## MIX-001 — mild field noise

```text
corner SNR
CW @ 2100 Hz, C/I +10 dB
light static
```

## MIX-002 — moderate

```text
corner -2 dB
CW @ 1800 Hz, C/I +6 dB
moderate static
```

## MIX-003 — fade + interference

```text
corner -2 dB
18-dB fade / 250 ms
CW @ 1800 Hz, C/I +3 dB
```

## MIX-004 — digital transport damage

```text
corner
moderate static
5-sample deletion events
```

## MIX-005 — severe composite

```text
corner -4 dB
24-dB fade / 250 ms
CW @ 1800 Hz, C/I +3 dB
moderate/static crashes
5-sample deletion
```

Apply these to all six modes where practical.

---

# 15. Phase-1 Corpus Size

Keep the first corpus deliberately modest.

Target roughly:

```text
6 perfect reference WAVs

plus approximately
10–20 impaired WAVs per mode
```

Total:

```text
~70–125 WAV files
```

This is enough to prove the framework without creating an unmanageable test archive.

---

# 16. Phase-1 Directory Structure

Use something like:

```text
test-vectors/
└── simulated-interference/
    ├── payload/
    ├── manifests/
    │
    ├── 300L/
    │   ├── reference/
    │   ├── cw/
    │   ├── impulse/
    │   ├── fade/
    │   ├── sample-slip/
    │   ├── noise/
    │   └── mixed/
    │
    ├── 300S/
    ├── 600L/
    ├── 600S/
    ├── 1200L/
    └── 1200S/
```

Use the same subdirectory organization for every mode.

---

# 17. Phase-1 Impairment Tool

For Phase 1, it is acceptable to implement the impairment/mixing tool using:

- Python; or
- host-side C++.

Choose whichever produces reliable results fastest.

The priority is **proof of concept and useful test vectors**.

However, keep the impairment algorithms modular so they can be ported/reimplemented cleanly in the portable C++ engine in Phase 2.

Do not tightly couple the logic to Python scripting conventions.

Conceptual interface:

```text
PCM input
   |
   v
impairment transform
   |
   v
PCM output
```

---

# 18. Phase-1 Metadata

Every generated WAV must record:

```text
test ID
mode
rate
interleaver
duration
sample rate
PCM format
source perfect WAV
source WAV hash
payload hash
seed
impairments applied
impairment order
all impairment parameters
generator version
```

Prefer JSON sidecars.

Example:

```text
m110_600L_mix_MIX003_seed0042.wav
m110_600L_mix_MIX003_seed0042.json
```

---

# 19. Automatic Decoder Scoring

Run every generated file through the existing decoder automatically.

Record:

```text
mode_detected
interleaver_detected
acquired
loss_of_lock
bit_errors
BER
payload_bytes
complete_message
EOM_detected
decode_time
```

Generate a summary CSV/JSON.

Phase 1 is intended to establish **receiver boundaries**, so impaired files are not expected to pass universally.

---

# 20. Phase-1 Acceptance Criteria

Phase 1 is complete when:

1. Six clean perfect WAVs exist.
2. Each clean WAV is 1–5 minutes long.
3. All clean WAVs decode BER 0.
4. All clean WAVs produce valid EOM.
5. CW impairment works.
6. static-crash impairment works.
7. fade impairment works.
8. sample deletion/duplication works.
9. noise/SNR impairment works.
10. mixed impairments work.
11. all random effects are deterministic by seed.
12. approximately 70–125 useful WAVs exist.
13. every WAV has metadata.
14. every WAV is automatically scored.
15. the corpus can be regenerated exactly.

**Stop here and review results before beginning Phase 2.**

---

# 21. Phase 2 — Portable Interference Engine

Once Phase 1 proves which impairment models are useful, implement or migrate the interference algorithms into a portable C++23 library.

The purpose is to share identical impairment behavior between:

```text
Windows WAV renderer
Linux WAV renderer
RT1170 live waveform player
```

Suggested structure:

```text
signal-lab/
├── include/
│   └── signal_lab/
│       ├── sample_stream.hpp
│       ├── impairment.hpp
│       ├── scenario.hpp
│       └── mixer.hpp
│
├── src/
│   ├── awgn.cpp
│   ├── cw.cpp
│   ├── impulse.cpp
│   ├── fade.cpp
│   ├── sample_slip.cpp
│   ├── resampler.cpp
│   └── mixer.cpp
│
└── tests/
```

The library must remain waveform-agnostic.

It processes PCM/sample streams, not M110 protocol objects.

---

# 22. Phase 2 Source Abstraction

Introduce a simple source abstraction:

```text
SampleSource
    |
    +-- WAV file
    +-- future generated waveform
```

The interference system should not care which source produced the samples.

Conceptually:

```text
SampleSource
     |
     v
ImpairmentPipeline
     |
     v
SampleSink
```

Possible sinks:

```text
WAV writer
RT1170 DAC
future FPGA
```

---

# 23. Phase 2 Scenario Configuration

Create a portable scenario description.

Example:

```yaml
source:
  type: wav
  file: m110_600L_reference_perfect.wav

seed: 60042

impairments:
  - type: cw
    frequency_hz: 1800
    ci_db: 3

  - type: impulse
    rate_per_sec: 1
    peak_db: 20
    decay_ms: 20

  - type: fade
    depth_db: 18
    duration_ms: 250
```

The same scenario definition should eventually be usable for:

```text
offline WAV rendering
```

and:

```text
RT1170 real-time playback
```

---

# 24. Phase 3 — Integrate Into Existing RT1170 Waveform Generator

The existing RT1170 implementation is:

```text
M110-modem\tools\waveform-generator
```

Do not replace it.

Extend it.

Current conceptual behavior:

```text
SD / filesystem
      |
      v
clean WAV
      |
      v
buffer
      |
      v
codec / DAC
```

Phase 3 should become:

```text
SD / filesystem
      |
      v
clean WAV
      |
      v
buffer
      |
      v
portable impairment engine
      |
      v
codec / DAC
```

The clean WAV remains unchanged on storage.

Interference is mixed dynamically during playback.

---

# 25. Phase-3 First Dynamic RT1170 Features

Do not implement everything at once.

First support:

```text
CW
static crashes
fade
AWGN
```

Then add:

```text
sample slips
clock-rate distortion
adjacent-channel signal
```

only after the simpler path is stable.

---

# 26. RT1170 Determinism

The RT1170 implementation must preserve deterministic scenarios.

Given:

```text
same WAV
same scenario
same seed
```

the produced sample sequence should be reproducible to the degree practical.

This is important because an OTA/hardware failure must be replayable offline.

---

# 27. Phase 4 — Dynamic Runtime Control

Once RT1170 dynamic mixing is stable, expose scenario controls through USB CDC.

Example conceptual commands:

```text
LOAD 600L_reference.wav

CW ON
CW FREQ 1800
CW CI 3

STATIC ON
STATIC RATE 1
STATIC PEAK 20

FADE NOW 18 250

PLAY
```

Or use structured JSON/CBOR commands.

The exact protocol should fit the existing waveform-generator control architecture.

---

# 28. Live Sweeps

Phase 4 should support dynamic sweeps.

Examples:

## CW frequency sweep

```text
300 Hz → 3400 Hz
```

while the same clean waveform plays continuously.

## C/I sweep

```text
+20
+10
+6
+3
0
-3 dB
```

## Fade-depth sweep

```text
6
12
18
24
30 dB
```

This turns the RT1170 generator into an interactive receiver-characterization instrument.

---

# 29. Phase 5 — Optional Native Waveform Sources

Only after WAV playback plus dynamic interference works reliably should native waveform generation be considered.

Possible future source types:

```text
WAV source
M110 encoder source
FT8 encoder source
other digital waveform
synthetic tone/noise
```

Architecture:

```text
                  SampleSource
                       |
       +---------------+---------------+
       |               |               |
      WAV            M110 TX        future FT8
       |               |               |
       +---------------+---------------+
                       |
                       v
               impairment engine
                       |
                       v
                      DAC
```

M110 knowledge belongs in the `M110 TX source`.

It must **not** leak into the impairment engine.

This preserves the ability to test FT8 or any other waveform later simply by loading a clean WAV, even if no native FT8 encoder exists.

---

# 30. Explicit Non-Goals

Do not:

- rewrite the existing M110 transmitter;
- rewrite the current RT1170 waveform player unnecessarily;
- implement every impairment in Phase 1;
- build native M110 generation on the RT1170 in Phase 1;
- build native FT8 generation;
- implement FPGA processing as part of this task;
- generate thousands of WAVs before validating the first 70–125;
- change the production decoder to accommodate the generated tests.

---

# 31. Deliverables by Phase

## Phase 1

Deliver:

- six perfect reference WAVs;
- approximately 70–125 impaired WAVs;
- impairment generator;
- metadata;
- automatic decoder results;
- short report of useful/failing cases.

## Phase 2

Deliver:

- portable C++23 impairment library;
- common scenario description;
- host WAV-rendering backend;
- unit tests.

## Phase 3

Deliver:

- RT1170 dynamic impairment integration in:

```text
M110-modem\tools\waveform-generator
```

- clean WAV playback through interference pipeline;
- initial CW/static/fade/AWGN real-time generation.

## Phase 4

Deliver:

- USB CDC dynamic control;
- interactive impairment changes;
- live sweeps;
- reproducible scenarios.

## Phase 5

Optional:

- native M110 sample source;
- future additional waveform generators.

---

# 32. Required Final Phase-1 Report

Before proceeding to Phase 2, produce a Markdown report containing:

## Existing repository architecture

Explain what was found in:

```text
M110-modem\tools\waveform-generator
```

and what existing transmitter/channel tools were reused.

## Reference signals

List:

```text
300L
300S
600L
600S
1200L
1200S
```

including:

- duration;
- hashes;
- clean decoder results.

## Impairment implementation

Describe each implemented Phase-1 impairment.

## Corpus statistics

Count WAVs by:

- mode;
- impairment;
- severity.

## Receiver findings

Identify:

- easy cases;
- interesting failures;
- acquisition failures;
- fade/recovery failures;
- sample-discontinuity failures;
- CW/static sensitivity;
- LONG-vs-SHORT differences.

## Phase-2 recommendation

Based on measured results, identify which impairment algorithms are worth promoting into the portable real-time C++ engine.

Do not automatically promote every Phase-1 experiment.

---

# 33. Governing Development Principle

The immediate goal is modest:

> **Produce a small but valuable set of reproducible interference WAV files now.**

The longer-term goal is more capable:

> **Turn the existing RT1170 waveform generator into a protocol-independent real-time impairment and interference generator that can dynamically modify any clean PCM waveform during playback.**

The phased architecture must allow us to achieve the first objective quickly without creating throw-away work that blocks the second.

---

# 34. Phase 6 - Expanded Channel and Interference Simulation

Phase 6 extends the completed waveform-agnostic engine into a composable RF/audio
impairment instrument. The implementation shall distinguish three kinds of
transformation rather than treating every effect as generic noise:

1. `channel` effects modify the wanted waveform, including deep fading,
   multipath, Doppler, and frequency-selective fading.
2. `interferer` sources generate independent additive waveforms, including CW,
   keyed CW, AM carriers, impulsive emitters, pulsed radars, and jammers.
3. `receiver_fault` effects model front-end or transport behavior, including
   saturation, clipping, quantization, clock error, and sample slips.

The default physical ordering is:

```text
wanted source
    -> propagation/channel effects
    -> additive interferer summation
    -> receiver front-end limiting
    -> sampling/transport faults
    -> PCM16 output
```

Scenario ordering remains explicit where an experiment intentionally needs a
different placement. The sidecar shall record the resolved order and level
conventions.

## 34.1 Phase 6A - Contract and correctness hardening

Before adding new emitters:

- make Python and portable C++ parsers reject fractional, Boolean, non-finite,
  zero, negative, and overflowing integer parameters identically;
- use the portable 1024-sample limit for every shared `sample_slip` scenario;
- retain the 32-event portable limit for explicit/sample-slip schedules;
- add regressions for compact and explicit sample-slip forms and periodic fade
  counts;
- preserve deterministic output across processing block sizes; and
- keep all RT1170 processing bounded and allocation-free.

Phase 6A is the first implementation increment of this roadmap.

## 34.2 Phase 6B - Composable tone and oscillator sources

The existing ordered float pipeline already provides wide intermediate
summation and one final PCM quantization boundary. Extend it rather than adding
a modem-specific mixer.

Deliver:

- multiple simultaneous CW sources with independent frequency, C/I, phase,
  start, stop, and drift;
- keyed-CW envelopes with configurable words per minute, message spacing, and
  shaped rise/fall edges;
- stable per-source identity so adding an unrelated stage does not change an
  existing source's random phase or RNG stream;
- a bounded live-control oscillator bank; and
- worked recipes for single-tone, multi-tone, comb, and swept-tone jamming.

Repeated `cw` stages may be used for fixed multi-CW scenarios immediately, but
that capability does not by itself provide keyed Morse or a live oscillator
bank.

## 34.3 Phase 6C - Generalized impulses and burst emitters

Implement reusable scheduling and pulse-shaping primitives:

- periodic pulse trains;
- periodic trains with bounded jitter;
- bounded-random inter-arrival times;
- clustered events and bursts;
- configurable pulse width, polarity, rise/fall, decay, ringing, and spectrum;
- deterministic amplitude distributions; and
- finite event capacity with explicit dropped-event accounting.

Build named recipes from those primitives instead of hard-coding unique DSP
classes for `ignition`, `lightning`, or `woodpecker`:

- ignition noise: RPM/cylinder-related repetition, jitter, polarity variation,
  and broadband impulse shaping;
- lightning/static crashes: bounded or clustered arrivals, heavy-tailed levels,
  and selectable crash templates;
- pulsed-radar/woodpecker-style interference: carrier frequency, PRF, pulse
  width, burst length, chirp/sweep, and jitter.

The current Poisson `STATIC RATE` control remains an average-rate model. It
must not be described as a bounded lightning scheduler.

## 34.4 Phase 6D - AM and intentional interference families

Add a reusable AM source with carrier frequency, modulation index, audio source,
bandwidth filter, level convention, drift, and start/stop window. Add composable
intentional-interference recipes for:

- spot CW and keyed CW;
- multi-tone and comb interference;
- swept/chirped carriers;
- barrage and band-limited noise;
- pulsed and burst interference; and
- combinations of the above.

Reactive or waveform-following interference is a later subphase because it
requires bounded signal analysis and control feedback, not just sample
generation.

## 34.5 Phase 6E - Deep and frequency-selective fading

Keep scalar fades for deterministic outages, then add a real channel model:

- configurable deep-fade/outage envelopes;
- Rayleigh and Rician amplitude processes;
- delayed paths with independent gain, phase, and Doppler;
- frequency-selective fading over the occupied audio passband; and
- optional Watterson-style profiles with every coefficient and seed retained.

A scalar amplitude reduction shall not be reported as multipath or
frequency-selective fading.

# 35. Phase-6 Scenario and Level Contract

Every source shall use an explicit level convention. Supported conventions
shall distinguish at least:

```text
dBFS
interferer RMS relative to wanted-signal reference RMS
carrier-to-interference ratio
impulse envelope peak relative to wanted-signal reference RMS
wanted-signal peak relative to wanted-signal RMS
```

The scenario and sidecar shall record resolved frequencies, sample-indexed
event positions, source IDs, seeds, phases, envelopes, clipping policy, and
event-drop counters. Schedulers shall use sample positions, not wall-clock
callbacks. One source's random stream shall not change when another source is
inserted or removed.

# 36. Phase-6 Verification and Hardware Evidence

Each new primitive requires:

- host Python and portable C++ contract tests;
- deterministic vectors at multiple block sizes;
- phase and envelope continuity at block boundaries;
- exact event-boundary and overflow tests;
- defined headroom, summation, saturation, and clipping behavior;
- long-run bounded-memory and event-capacity tests;
- RT1170 Release build and static-memory accounting;
- host/RT1170 PCM or digest comparison for deterministic cases; and
- receiver-side payload, EOM, BER, acquisition, and recovery measurements.

The powered RT1170 and hard-patched codec/PC audio loop may be used for analog
engineering tests after host and firmware gates pass. Before programming or
claiming results, bind the run to the exact original MIMXRT1170-EVK identity,
firmware hash, scenario hash, Realtek endpoint identity, sample format, gain
settings, and chronological playback/capture logs.

Host tests, cross-builds, waveform hashes, analog loopback, and receiver decode
results are separate evidence classes. None alone establishes formal
MIL-STD-188-110 conformance.
