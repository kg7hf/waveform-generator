# Signal Lab Watterson Channel

## 1. Scope

This guide covers the Watterson multipath fading stage of the portable C++
engine in `signal-lab/`: the channel model, its scenario schema, its C++ API,
its determinism and resource contracts, and its tests. The algorithm design and numerical tradeoffs are in
[docs/design/watterson-stage.md](../design/watterson-stage.md). The [implementation walkthrough](#implementation-walkthrough) below includes
a worked example by hand.

The stage is a channel effect: it models the HF ionosphere of MIL-STD-188-110D
Appendix E (E.4.2 to E.5.5) and ITU-R F.1487 / F.520 between the transmitter
and the noise. The AWGN stage that follows it is described in the
[engine guide](signal-lab-engine.md#awgn). pathsim stays the statistical oracle
([pathsim.md](pathsim.md)); this stage is our own implementation, not a
translation of it.

The normal host renderer and shared player supply bounded channel storage.
Host tests establish software behavior. The full physical statistical/decoder
campaign is separate; see [testing](../testing.md) and the
[validation method](../design/watterson-validation.md).

## 2. 50,000-foot view

```text
x(n) real 48 kHz ──> Hilbert FIR (431 taps) ──> xa(n) = x(n-D) + j xh(n) ──> ring (1024 frames)
                                                                              │
              path i: read xa(n - L_i) ──> times g_i(n) = a_i h_i(n) r_i(n) ──┤  (up to 4 paths)
                                                                              │
y(n) = Re{ sum_i g_i(n) xa(n - L_i) }  <──────────────────────────────────────┘

h_i(n): Rayleigh   complex Gaussian draws ──> E.5.4 Gaussian FIR at R_i >= 64 d ──> linear interpolation to 48 kHz
        fixed      1
r_i(n): exp(j (2 pi f_i n / fs + phi_i)), a 32-bit integer phase and a double rotator
a_i:    normalisation, sum of path powers = 1 (E.5.4)
```

The input and output are the engine's normalised float stream at 48 kHz. The
stage delays everything by the Hilbert's centre delay, D = 215 frames
(4.479 ms); relative path delays are exact to one frame.

## 3. How to use it

A Watterson stage is the first item of `impairments`, at most once, usually
followed by AWGN. CCIR / ITU-R Poor at a delivered 18 dB:

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

D.6.2 static multipath, 3 kHz WID 10: `"paths": [{"fading": "fixed"},
{"delay_ms": 2.0, "fading": "fixed"}, {"delay_ms": 4.5, "fading": "fixed"}]`.
A Rician direct path is a fixed path and a Rayleigh path at the same delay:
`[{"fading": "fixed"}, {"spread_hz": 1, "power_db": -10}]` (K = 10 dB).

From C++, the engine needs the channel object, which the caller owns:

```cpp
#include "signal_lab/mixer.hpp"

// Static storage, about 18 KiB: an ordinary static on the host; OCRAM2
// (.ocram.watterson, NOLOAD) on the board.
constinit signal_lab::WattersonChannel channel{};

signal_lab::Engine engine;   // non-copyable; keep it static too
const signal_lab::StageResources resources{&channel, nullptr};
if (const m110::Status configured = engine.configure(scenario, reference_rms, total_frames, resources); !configured.is_ok()) { /* configured.message is static text */ }
const signal_lab::WattersonReport* report = engine.watterson_report();   // realised delays, M, R, K, N, gains, digest
```

The three-argument `configure()` passes no resources; a Watterson scenario then
fails with `watterson: needs a WattersonChannel (StageResources.watterson)`.
The channel can also be driven on its own, as the tests do:
`channel.configure(params, seed, stage_index, observer)` then
`channel.process(block, first_frame)` block by block.

**Status on the targets.** `signal_lab_render` supplies a static channel through
`StageResources`. Its sidecar's `watterson` object records the algorithm version,
parameter digest, latency, channel bytes and realised path parameters using the
validation driver's field names. PCM and channel reports agree with that driver
for fixed, two-path and four-path channels followed by AWGN, at block sizes 1,
127 and 2048. The Python renderer explicitly directs Watterson users to this
C++ tool; it does not implement a second channel model.

The RT1170 player supplies one static channel in `.ocram.watterson`, borrowed
through `StageResources` by both ordinary PLAY and playlist configuration.
Successful configuration initializes the channel before processing; it does not
depend on startup clearing NOLOAD memory. Host-harness tests poison the storage,
then compare fixed/four-path-plus-AWGN output on the USB and codec mocks against
the host renderer, including post-channel cuts and reconfiguration. Playlist
tests cover repeated and different seed overrides, a clean item, and a continuous
six-pass source loop. These tests do not measure real interrupt timing or output.

The fresh ARM release build places the 18,208-byte channel at `0x202d56c0` in
OCRAM2 (`m_data2`), inside the `.ocram` NOLOAD output section. The post-link gate
requires exactly one such section, correctly placed, at most 20 KiB. The ELF
section has `ALLOC` without `CONTENTS` or `LOAD`; its bytes are not a startup
initialization image. The player stack gate reports 12,536 of 16,384 bytes,
including its 232-byte reserve; the USB task reports 2,456 of 8,192 bytes.
These addresses and sizes describe this build, not future layout promises.

INFO advertises `watterson` and `watterson_algorithm_version`. ENGINE? reports
the last actual play's version, split parameter digest, latency and path count;
each playlist result retains those fields plus the engine output digest before
live controls and CUT. A clean/refused/unstarted play has zero channel identity.
Preflight configuration cannot replace the published identity of the last play.
The 4096-byte reply limit now permits two playlist results per query; the host
drain also accepts older three-result replies. See the
[protocol contract](../live-control-protocol.md#engine-query) for field names.

Board execution, capture parity, producer timing and the endurance battery
remain unverified. Low-latency block mode and timestamps remain future work.

The renderer reads PCM16/PCM24 in bounded blocks and measures a missing reference
level with a streaming pre-pass. It spools PCM24 to a temporary file, publishing
the requested WAV only after the engine and live replay accept the render.
Allow temporary disk space for the complete output in addition to the final WAV.
Audio-buffer memory is independent of duration; scenario and replay metadata are
still held in memory. RIFF sizes are checked, and odd PCM24 payloads have the
required pad byte; RF64 is not supported. A sparse five-hour source regression
checks reference reads beyond 2 GiB and a short replay, not five hours of DSP or
board operation.

## 4. Scenario schema

The stage is one item of `impairments`. Unknown members of the stage or of a
path are errors (the older stages ignore theirs), and so is a member named
twice in one object, so a typo cannot become a default silently and a scenario
means one thing to every reader.

| Member | Type, unit | Range | Default | Notes |
|---|---|---|---|---|
| `type` | `"watterson"` | | required | at most one per scenario, and it must be the first stage |
| `paths` | array of path objects | 1 to 4 entries | required | |
| path `delay_ms` | number, ms | [0, 20] | 0 | rounded to the nearest frame, ties to even (E.5.2); reported in frames |
| path `power_db` | number, dB, relative | [-60, 60] | 0 | the set is normalised to a total power of 1 (E.5.4) |
| path `fading` | `"rayleigh"` or `"fixed"` | | `"rayleigh"` | |
| path `spread_hz` | number, Hz, two-sigma | [0.1, 10] | required for `rayleigh` | not allowed on `fixed` |
| path `shift_hz` | number, Hz | [-75, 75] | 0 | any path; realised to 11.2 uHz steps |
| path `phase_deg` | number, degrees | [-360, 360] | 0 | `fixed` paths only; D.6.2 states no phase convention |

The messages are static, from the parser and from `validate_scenario` for
programmatic scenarios alike; each is the message of the `m110::Status`
(`invalid_configuration`) that `parse_scenario` or `validate_scenario` returns:

- `watterson: at most one stage per scenario`
- `watterson: must be the first stage (noise and interference follow the channel, E.4.1)`
- `watterson.paths: required, 1 to 4 path objects`
- `watterson: unknown member`; `watterson.paths[]: unknown member`
- `watterson: duplicate member`; `watterson.paths[]: duplicate member`
- `watterson.paths[].delay_ms: number in [0, 20]`
- `watterson.paths[].power_db: number in [-60, 60]`
- `watterson.paths[].fading: "rayleigh" or "fixed"`
- `watterson.paths[].spread_hz: required for a rayleigh path, number in [0.1, 10]`
- `watterson.paths[].spread_hz: not allowed on a fixed path`
- `watterson.paths[].shift_hz: number in [-75, 75]`
- `watterson.paths[].phase_deg: number in [-360, 360], fixed paths only`
- `watterson.paths[]: value is not finite` (programmatic scenarios)
- at configure time: `watterson: needs a WattersonChannel (StageResources.watterson)`

The rules live in `watterson.cpp` (`parse_watterson_stage`,
`validate_watterson_stage`, `validate_watterson`). `parse_scenario` reaches
them through its one fallback branch for stages with their own files, and
`validate_scenario` through its typed-stage helper, so neither dispatcher's
complexity grew (the CI complexity gate holds them at their baseline).

## 5. Scientist and maintainer view

### Signal model

```text
y(n) = Re{ sum_i g_i(n) xa(n - L_i) } = sum_i [ Re g_i(n) x(n - D - L_i) - Im g_i(n) xh(n - L_i) ]
```

This is E.4.2's complex tapped delay line, with the complex input made by a
Hilbert transform and the real part taken as the output. For x = cos(w n),
xa = e^(j w n); a gain e^(j phi) gives cos(w n + phi), and a shift f moves the
tone to f_tone + f. One fixed path at 0 ms, 0 dB, 0 degrees gives g = 1 exactly,
so y(n) = x(n - D) bit for bit.

### Hilbert filter

A type III Kaiser FIR: N = 431 taps (the Kaiser length for A = 72 dB and a
500 Hz half-band transition, rounded up to 4k + 3), D = 215, 108 non-zero taps
per side, h[m] = 2 / (pi m) w_K[m] for odd m. Its magnitude is within 4.2e-4 of
1 from 300 Hz to fs/2 - 300 Hz, and a cosine's mirror is below -70 dBc from
250 Hz to fs/2 - 250 Hz (E.6.2). Content below 250 Hz is outside the model.
The coefficients are designed at configure time with det_math and stored as
float; the in-phase sample is the centre tap of the same history, so I and Q
are aligned exactly.

### Delay ring

The Hilbert writes a whole 64-frame chunk of analytic samples into a ring of
1024 frames before any path reads it, so every delay up to 960 frames (20 ms)
plus one chunk is held: ring >= max delay + chunk, a `static_assert`. Frames
before the stream start read as zeros (the channel's response to silence).

### Tap processes

Each Rayleigh path has its own PCG32 stream (family 9, index 4 s + i):
Box-Muller pairs (`det_gaussian.hpp`, the uniform widened before the + 1, unit
variance per component) scaled by 1/sqrt(2) give complex draws with E|w|^2 = 1.
They pass through the E.5.4 Gaussian FIR in its prose form (not the printed
time-domain formula, the QA #7/#8 trap):

```text
c_k ~ exp(-(pi k / K)^2), |k| <= L, K = R / d;  L = ceil(K sqrt(ln(1/f)) / pi), f = 1 %;  sum c_k^2 = 1
M = floor((fs / 64) / d) whole frames per node;  R = fs / M >= 64 d;  N = 2 L + 1 = 89 or 91 taps
h(n) = h[m] + mu (h[m+1] - h[m]),  m = n / M,  mu = j / M   (linear interpolation, exact phase)
```

The PSD is Gaussian with sigma = d/2, so d is the two-sigma spread. The tap
process is stationary from frame 0: configure() draws N + 1 complex samples per
path, then each node takes exactly one. The 1 % truncation is one constant,
`doppler_truncation_decades` (two decades); the lane's question Q1 (design 5.5)
would change it to three, which only changes that constant, the capacity that
follows it and the golden digests.

### Normalisation

```text
p_i = 10^(P_i/10) / sum_j 10^(P_j/10)        Rayleigh: a_i = sqrt(p_i / v_i)      fixed: a_i = sqrt(p_i)
v_i = 1 - (1 - rho1_i) (M_i^2 - 1) / (3 M_i^2)   rho1_i: lag-one correlation of the stored coefficients
```

v_i is the exact mean power of the interpolated tap (linear interpolation
loses 4e-4 at K = 64), so sum a_i^2 v_i = 1 to rounding. The scale is fixed at
configure time; there is no level tracker. E[y^2] equals the input power for
independent paths, so the AWGN stage's reference (the scaled source RMS) is
unchanged by the channel (E.5.5 refers SNR to the input).

### Shift and phase

A shift is a 32-bit phase step, inc = round(f 2^32 / fs). The double rotator
is set exactly from the integer phase (n inc) mod 2^32 at every absolute frame
with n mod 1024 = 0 and multiplied by its step in between; a fixed path's phase
rides on the rotator when it shifts and on its constant gain when it does not.

### Determinism

The tap sequence is a function of (effective seed, RNG index, path index,
absolute frame). Without `stream_id`, the effective seed and RNG index are the
scenario seed and stage index. With an optional
[stable identity](random-stream-identity.md), they are the ID-derived seed and
zero; later stage insertion preserves the channel's parameter digest and tap
sequence. Processing order and the requirement that Watterson come first remain.
Every state change is keyed to the absolute frame (node updates at
n mod M = 0, rotator resyncs at n mod 1024 = 0), the arithmetic order is fixed
and written at the top of each source file, and det_math replaces the C
library. So any partition into blocks of 1 to 2048 frames gives the same
samples, and host and board are designed to be bit-exact (the board check is
a digest comparison, design 10.5). Adding, removing or changing one path never
changes another path's tap sequence. The stage requires contiguous blocks: a
first frame other than the previous block's end latches a fault until
configure().

### Types and units

The new code carries units in its types (`signal_lab/units/`, the design of
dimensional-analysis unit types): `Duration` for delays, `FrequencyDelta`
for spreads, shifts and node rates, `Frequency` for band edges, `Gain` for path
levels, `Ratio` for SNRs and filter attenuations, `Level` for absolute levels,
`Angle` for phases, `SampleRate`, `SampleCount` and `SampleIndex` for the
stream. Values are private and built with a unit (`Duration{2.0, ms}`); an
expression that means nothing does not compile (Level + Level, Gain + Ratio,
Frequency + Frequency, index + index). `Amplitude` is a linear RMS in
full-scale units, the form of the engine's reference level and of the AWGN
stage's sigma, so a power or a dBFS number cannot stand in for one.
`signal_lab/convert.hpp` holds every numeric cast of the new code; the test
suites add `tests/test_convert.hpp` for their signed reference-model frames and
iterator distances, and the host driver wraps its iostream, C stdio, RIFF and
`main` boundaries once each.

The conversions with a domain return `m110::Result`, never a wrapped cast:
`units::to_frames` (a finite delay >= 0 below 2^53 frames), `units::phase_step`
(a finite shift within +/-fs/2) and `design_doppler` (a spread in [0.1, 10] Hz
within the static FIR capacity). A refusal is `invalid_argument` with a static
message, the same rule on host and board; configure() propagates it, although
validated parameters never reach one.

The stream rate has one definition, `stream_rate` (`stream_rate.hpp`), and
`scenario.hpp`'s `sample_rate_hz` (the legacy stages, the scenario ranges, the
hosts' WAV headers) is defined from it, so another rate scales every stage
together. The Kaiser designs assert at compile time that their attenuation is
inside the formula's domain and the I0 series bound (`kaiser::within_design_range`).

### Bounded resources

| Item | Size | Where |
|---|---|---|
| `WattersonChannel` (ring 8,192 B, Hilbert 2,408 B, 4 paths x 1,792 B, report 408 B) | 18,208 B (host); `static_assert` <= 20 KiB | caller's static storage; OCRAM2 `.ocram.watterson` on the board (the existing `.ocram` output section takes `*(.ocram*)`) |
| `WattersonStage` adapter, its slot and `StageResources` | 64 B (Cortex-M7) | the engine object (DTCM) |
| `WattersonParams` in `Scenario` | 200 B (Cortex-M7) | the scenario (DTCM) |
| Largest stack frame of the stage (Cortex-M7, `-fstack-usage`, c-fw-check) | 160 B at configure time (`WattersonChannel::configure`, `configure_rayleigh`, which hold the conversions' `Result`s); 152 B on the audio path (`process_chunk`); every frame static and at most 256 B | the caller's task stack |
| CPU (design 11.1 estimate) | about 2.5 % of the M7 for four shifted 10 Hz paths | |

No allocation, no exceptions, no virtual call except the `Impairment`
interface itself, loops bounded by compile-time capacities or the block length.
`allocation_tests.cpp` replaces the test binary's global operator new and
delete with counting ones and finds no allocation in `WattersonChannel`'s
configure() and process() or in the engine's configure() and process() for a
Watterson + AWGN scenario (a positive control shows the counter sees one).

### Failure and debugging signals

- configure() returns an `m110::Status` with one of the static messages of
  section 4; the channel then refuses process() until a configure succeeds.
- A non-contiguous block: `watterson: first_input_frame does not follow the
  previous block`. The adapter's `process()` returns the channel's
  `m110::Status` in place of a frame count; the engine latches it as its own
  status and returns it, so `Engine::process()`'s Result and `Engine::status()`
  carry the channel's own message (`wfg_watterson_render` prints it as
  `stage fault: ...`). A process() after a refused configure() of a shared
  channel reports `watterson: process() before a successful configure()` the
  same way. The frame count is never a fault signal: `block_overrun` stays the
  engine's own check that a stage kept within the block's capacity.
- `WattersonReport` gives every realised parameter and `parameter_digest`
  (FNV-1a over the algorithm version, rate, seed, stage and realised values).
  Two renders that differ with the same digest point at the input or the
  engine, not the channel's configuration.
- A `TapObserver` sees every node of every Rayleigh path and each fixed path's
  gain: with the report, the 48 kHz path gains can be rebuilt exactly (channel
  truth, survey gap C10). Host only.

## 6. In the engine

The engine builds the stage in a typed slot (`std::optional<WattersonStage>`),
not in the placement arena, as it does the AWGN stage: no cast, no placement
new. `impairment_size(StageType::watterson)` is 0 and `construct_impairment`
returns nullptr for it. The adapter borrows the scenario's `watterson` slot,
the channel and the observer, and configures the channel in `prepare()` with
the scenario's seed and the stage's index.

The stage's latency is D = 215 frames at every block size. An impulse at input
frame k comes out at k + D + L_i (E.4.4's algorithmic x-to-y delay; the
stream path's share requires measurement on the board). A later stage's
window is still counted in input frames.

## 7. Plain-language view

Radio waves bounce off the ionosphere, and they arrive by more than one route.
Each route is a little late, and the ionosphere moves, so each route's echo
gets louder and quieter and wobbles in pitch a little. The stage copies the
signal into up to four echoes, delays each one, and turns each echo's volume
knob with a slowly wandering random hand. The seed decides how the hands
wander, so the same seed gives the same weather.

## 8. Worked example

CCIR Poor, seed 1 (two equal Rayleigh paths, 0 and 2 ms, 1 Hz): each path has
M = 750 frames per node, R = 64 Hz, K = 64, N = 89 taps, v = 0.9995976 and
a = 0.7072491 (-3.009 dB). With a 1 kHz cosine at 0.25 as input, frame 1125
is half way between nodes 1 and 2 of both paths; the detailed worked example below works its output,
y(1125) = 0.0614446, from the two nodes and the two ring reads by hand.

## 9. Code map and reading order

| Responsibility | Files |
|---|---|
| Unit types and the casts | `signal-lab/include/signal_lab/units/*.hpp`, `convert.hpp`, `src/units.cpp` |
| Box-Muller, Kaiser | `det_gaussian.hpp`/`.cpp`, `kaiser.hpp`/`.cpp` |
| Hilbert and ring | `analytic_filter.hpp`/`.cpp` |
| Tap processes | `doppler_filter.hpp`/`.cpp`, `complex_sample.hpp` |
| Parameters and ranges | `watterson_params.hpp`, `stream_rate.hpp` |
| Channel, adapter, schema | `watterson.hpp`, `src/watterson.cpp`, `src/watterson_scenario.hpp` |
| Engine slot | `mixer.hpp`/`.cpp` (`watterson_stage_`, `StageResources`) |

Read `watterson_params.hpp`, then `watterson.hpp`, then `watterson.cpp` from
`WattersonChannel::configure` down; the components after that.

## 10. Tests

The suites run in `wfg_signal_lab_unit_tests` (Catch2; `ctest --test-dir
build/main/host-c -L catch2 -j 4`), about 3 s in all. Statistical cases use
fixed seeds, and each tolerance is at least 5 standard errors of its estimate.

| Suite | Design tests | What it shows |
|---|---|---|
| `units_tests.cpp` | U1, U2, U35 | E.5.2 rounding with ties to even; shifts within 5.6 uHz; the algebra that must not compile does not; a delay or shift outside a conversion's domain is refused |
| `det_gaussian_tests.cpp` | U34 (and U31/U32 for the excitation) | the 0xFFFFFFFF draw gives radius 0; the four axes; unit variance; kurtosis and tails; white to fs/2 |
| `kaiser_tests.cpp` | U3 | I0 against reference values; 431 and 583 taps; the design range holds both designs and refuses Kaiser's other formula and a beta above 10 |
| `analytic_filter_tests.cpp` | U4 to U6 | type III structure; |H| within 4.2e-4; mirrors below -70 dBc; x(n - D) exact |
| `doppler_filter_tests.cpp` | U10 to U14, U16, U17, U18 (a) | sigma = d/2 with the printed-formula trap failing; 1 % truncation and 89/91 taps; R >= 64 d; images below -79 dBc; Rayleigh moments and KS over 2^22 nodes; uncorrelated paths; two sigma within 3 % at 0.1, 1, 10 Hz; v's closed form; a spread or rate outside the design's domain is refused |
| `watterson_tests.cpp` | U7 to U9, U13 (b, c), U15, U18 (b, c), U19 to U24 | identity path; impulses at 1000 + D + L for 8 delays and 5 block sizes; phases and shifts; the 48 kHz gain equals the node interpolation bit for bit; rendered images below -75 dBc; normalisation exact; Rician K; digests; 13 partitions equal a chunk-free reference model; path independence; golden digests; 0xFF poisoning; the contiguity fault |
| `watterson_scenario_tests.cpp` | U25 | every schema message; defaults; unknown and duplicate members |
| `watterson_engine_tests.cpp` | U26 to U28, U36 | placement rules; the missing-channel message; the typed slot and arena use; the two parsers agree; E.4.4's algorithmic delay through the engine; the engine's output equals the channel's; a channel fault is the engine's latched Status with the channel's message, never a count |
| `allocation_tests.cpp` (with `allocation_counter.cpp`) | Criterion 8 | no allocation in the channel's or the engine's configure() and process(); the counter's positive control |

The golden digests of U23 pin the draw order, the float order and the
normalisation. Change them only with a documented reason (a change of
`doppler_truncation_decades`, of the algorithm version, or of any arithmetic).

## 11. Remaining acceptance and extension limits

- The interpretation of "-60 dBc outside the lobe" as per-spur or integrated
  power remains explicit in the [design](../design/watterson-stage.md).
  Changing truncation changes coefficients and golden digests.
- Use the [validation method](../design/watterson-validation.md) for decoder
  equivalence and statistical coverage. A rendered ladder alone is not a
  completed decoder comparison.
- Physical captures, contended throughput, reset repeatability and long-run
  channel acceptance must be checked independently for each board and route.
- Low-latency blocks, timing stamps and sample-clock simulation remain future
  extensions; their proposed behavior is not part of the current API.
- Follow the [coding standard](../coding-standard.md) for formatting and gates;
  preserve floating-point order and version/digest changes when changing DSP.

## Implementation walkthrough

### `watterson.cpp`: configuration

- `validate_watterson(params)`: path count 1-4; each path finite, delay 0-20 ms,
  level within +/-60 dB, shift within +/-75 Hz; a Rayleigh path needs a spread
  of 0.1-10 Hz and no phase, a fixed path no spread and a phase within +/-360.
- `parse_watterson_stage(document, object, params)`: the stage object may hold
  only `type` and `paths`; each path member is looked up in the constexpr
  table `path_numbers` (name, field, range, message), so the unknown-member
  rule and every range are one loop. `check_presence` then applies the kind
  rules. It ends with `validate_watterson`.
- `validate_watterson_stage(scenario, index)`: the placement rules (index 0
  only; a second stage is "at most one", anything before it "must be first").
- `WattersonChannel::configure(params, seed, stage, observer)`: validates,
  designs the Hilbert (`AnalyticFilter::configure`), clears the ring, works out
  p_i from the levels, and for each path calls `configure_rayleigh` or
  `configure_fixed` with its own generator `Pcg32(seed, 9, 4 stage + i)`; the
  unused path slots are cleared. `finish_report` writes D, the Hilbert length
  and the digest. Every member is written, so a channel filled with 0xFF bytes
  configures to the same state (U24).
- `WattersonPathState::configure_rayleigh`: `design_doppler` gives M, R, K, L, N;
  `DopplerFilter::design` stores the unit-energy FIR as float; rho1 is taken
  from those stored floats and v = 1 - (1 - rho1)(M^2 - 1)/(3 M^2); a =
  sqrt(p / v); the node amplitude is a / sqrt(sum c^2) as a float. `prime`
  draws N complex samples and returns node 0; `next_node` draws one more for
  node 1. The observer gets both.
- `configure_fixed`: a = sqrt(p); without a shift the gain is a e^(j phi);
  with one the gain is a and phi rides on the rotator.
- `configure_rotator`: the phase step round(f 2^32 / fs); the rotator is on only
  when the step is not 0; its step exp(j 2 pi inc / 2^32) is computed once.

### `watterson.cpp`: processing

- `WattersonChannel::process(block, first_frame)`: refuses a block that does not
  start at the next expected frame (and stays refused until configure), then
  walks the block in chunks of at most 64 frames.
- `process_chunk`: `AnalyticFilter::process` appends the chunk to the Hilbert's
  buffer and writes xa for every frame of the chunk into the ring; then each
  path's `render` runs over the same frames, the first path writing the output
  and the others adding to it.
- `WattersonPathState::render`: for each frame, reads xa(n - L) from the ring,
  asks `next_gain` for g(n), and forms (Re g xI) - (Im g xQ).
- `next_gain`: for a Rayleigh path, mu = float(j x (1/M)) and h = h[m] + mu
  (h[m+1] - h[m]); when j reaches M, `advance_node` shifts the nodes and draws
  the next one. Then, if the path rotates, the rotator is reset exactly at
  n mod 1024 = 0 (`exact_rotator`, det::cos and det::sin of the integer phase)
  and the gain is multiplied by its float value; the rotator steps once.

### `watterson.cpp`: the engine's adapter

- `WattersonStage::prepare`: without a channel it fails with
  `watterson: needs a WattersonChannel (StageResources.watterson)`; otherwise it
  configures the channel with the scenario's seed and its own stage index.
- `WattersonStage::process`: views the legacy `float*` and count once as a span,
  processes it, and keeps the output's energy and peak for the stage stats. On a
  fault it returns the channel's `m110::Status` in place of a frame count, and
  the engine latches that Status as its own (`Engine::status()`) and returns it.

### `analytic_filter.cpp`, `doppler_filter.cpp`, `det_gaussian.cpp`

- `AnalyticFilter::configure`: h[m] = 2/(pi m) w_K(m/D) for odd m with
  beta = 0.1102 (72 - 8.7).
- `AnalyticFilter::process`: the quadrature sum over 108 pairs, in order; the
  in-phase sample is the centre tap; the last 430 inputs move to the front.
- `design_doppler`: M = floor(750 / d) (at least 1), R = fs / M, K = R / d,
  L = ceil(K sqrt(2 ln 10) / pi); a spread outside [0.1, 10] Hz, a rate below
  64 Hz or an L beyond the static capacity is refused as an `m110::Result`
  (configure() returns it), never clipped.
- `DopplerFilter::filter`: the symmetric FIR over the mirrored history window,
  pairs from the outside in, then the centre tap.
- `gaussian_pair`: u1 = (double(radius_draw) + 1) 2^-32, u2 from the top 24
  bits of the angle draw, r = sqrt(-2 ln u1).

## Detailed worked example: CCIR Poor, seed 1, by hand

Scenario: two Rayleigh paths, 0 and 2 ms, 1 Hz each, equal levels; seed 1,
stage 0. Input: a 1 kHz cosine of amplitude 0.25.

**Configuration.** Both paths: M = floor(750 / 1) = 750 frames per node,
R = 48000 / 750 = 64 Hz, K = 64, L = ceil(64 x 0.68308) = 44, N = 89 taps (the
end tap is exp(-(pi 44 / 64)^2) = 0.94 % of the centre). p = 1/2 each. From the
stored coefficients rho1 = 0.998793, so v = 1 - 0.001207 x (750^2 - 1)/(3 x 750^2)
= 0.9995976 and a = sqrt(0.5 / 0.9995976) = 0.7072491 (-3.009 dB). Path 1's
delay is round(2 x 48000 / 1000) = 96 frames.

**One node update of path 0.** Its stream is `Pcg32(1, 9, 0)`. The first two
draws are 0xE80BBEF4 (radius) and 0xE600AB9B (angle): u1 = (3893083892 + 1) x
2^-32 = 0.906429, u2 = 0.898448, r = sqrt(-2 ln u1) = 0.443266, so the pair is
(0.356051, -0.264030) and the first complex draw is w = (0.251766, -0.186698)
after the 1/sqrt(2). 89 such draws fill the history; the 89-tap FIR over them,
times the node amplitude 0.7072491 / sqrt(sum c^2), gives node 0 =
(-0.228856, +0.347880). One more draw gives node 1 = (-0.220814, +0.315487),
and the next node update, at frame 749, gives node 2 = (-0.210181, +0.283426).
Path 1 (stream index 1) gives nodes (0.355381, 0.234312), (0.322424, 0.288375)
and (0.288197, 0.341488).

**Interpolation at j = 375 of M = 750.** Frame n = 1125 is in node interval
m = 1 (nodes 1 and 2) at j = 375, so mu = 375 / 750 = 0.5 exactly:

```text
path 0: h = (-0.220814, 0.315487) + 0.5 x ((-0.210181, 0.283426) - (-0.220814, 0.315487)) = (-0.215498, 0.299456)
path 1: h = ( 0.322424, 0.288375) + 0.5 x (( 0.288197, 0.341488) - ( 0.322424, 0.288375)) = ( 0.305310, 0.314931)
```

Neither path shifts, so g = h (a is already in the nodes).

**One output frame from the two ring reads.** Path 0 reads the ring at
1125 - 0, path 1 at 1125 - 96 = 1029. Both hold xa = x(n - 215) + j xh(n):
0.25 cos(2 pi 1000 x 910 / 48000) = 0.241481 and the quadrature -0.064709
(96 frames is two whole cycles of 1 kHz, so both reads are the same here).

```text
path 0: (-0.215498)(0.241481) - (0.299456)(-0.064709) = -0.052039 + 0.019378 = -0.032661
path 1: ( 0.305310)(0.241481) - (0.314931)(-0.064709) =  0.073727 + 0.020379 =  0.094106
y(1125) = -0.032661 + 0.094106 = 0.061445          (the stage computes 0.0614446)
```
