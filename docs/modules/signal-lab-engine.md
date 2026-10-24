# Signal Lab Static Impairment Engine

## 1. Scope

This guide covers the Python reference implementation in `host/signal_lab/` and
the allocation-free C++23 implementation in `signal-lab/`. Both consume mono
48 kHz PCM and apply an ordered scenario containing AWGN, CW, impulse, fade, and
sample-slip stages. The C++ engine also has the Watterson multipath fading
channel and the whole-signal [frequency offset / Doppler sweep](frequency-shift.md).
The channel has its own guide: [Signal Lab Watterson Channel](signal-lab-watterson.md).
The C++ core also supports one [second-source overlay](second-source.md), with
bounded WAV adapters in the normal host renderer and shared RT1170/RT1064 SD player, and
optional independent Rayleigh fading. The Python peer does not implement
Watterson, global frequency shift or second source.

It does not cover live controls, waveform encoding, codec transport, or decoder
scoring. Those are separate modules.

The [multiple-PC testing plan](../roadmap.md#current-priorities)
uses the existing RT1170 and an [RT1064 port](rt1064-board.md), with separate
USB and codec validation for each board.
Common prepared waveforms separate delivery comparisons from engine-generation
comparisons; USB and physical codec paths provide complementary coverage.
Windows virtual-audio driver/backend work is on hold, with design notes retained
in the [virtual-audio proposal](../design/pc-virtual-audio.md).

## 2. 50,000-foot view

Signal Lab is a conveyor belt for samples. A scenario fixes the source gain,
reference interval or RMS, clip policy, seed, and an ordered list of impairment
stages. Every source sample moves through those stages in document order.

```text
normalized float -> source gain -> stage 0 -> stage 1 -> ... -> clip -> PCM24 boundary
```

The Python path is convenient for batch test-vector creation and inspection. The C++ path
is the portable implementation used by both the host renderer and RT1170. A
scenario is not tied to M110: any valid mono 48 kHz PCM source can be processed.

## 3. How to use it

A minimal scenario is:

```json
{
  "schema": "signal-lab.scenario/1",
  "test_id": "CW-NEAR-01",
  "seed": 42,
  "source": {"path": "clean.wav"},
  "reference": "auto",
  "source_gain_db": -12,
  "clip_policy": "saturate",
  "impairments": [
    {"type": "cw", "frequency_hz": 1900, "ci_db": 6, "ramp_seconds": 0.01}
  ]
}
```

Render through Python:

```powershell
python host/render_scenario.py --scenario case.json --output impaired.wav
```

Render through the portable engine:

```powershell
build\host-release\host\render\signal_lab_render.exe `
  --scenario case.json --source clean.wav --output impaired-portable.wav `
  --sidecar impaired-portable.json --target-scenario PLAY.SCN
```

Use `--reference-rms` only when the reference was measured elsewhere. The
RT1170 needs `reference_rms` in `PLAY.SCN` because it cannot scan an unbounded
source before streaming.

The C++ renderer uses bounded audio buffers and a temporary PCM24 spool instead
of retaining the full source and output in memory. Reserve disk space for both
the spool and final WAV. Processing failures preserve existing output artifacts;
RIFF output has checked sizes and odd-byte padding. It also supplies the static
channel required for [Watterson scenarios](signal-lab-watterson.md); the Python
renderer reports that this stage requires the C++ tool.

The offline [`WavSource`](../../host/render/stream_wav.hpp) owns its input file.
Opening a replacement first releases the previous source; a failed open leaves
zero frames and rejects reads and reference measurements. Closing is idempotent
and also invalidates the reader. Reads require a validated file, a range inside
its declared payload and at most one engine block. The
[`stream_wav_tests`](../../tests/stream_wav_tests.cpp) exercise these lifecycle
and bounds contracts, including recovery after malformed input.

Stage order is observable. A fade before noise attenuates the desired signal but
not later noise. A fade after noise attenuates both. Sample slips must be last
because they change stream length and the meaning of later input-frame windows.
For the difference between scenario slips, the player's post-engine CUT stage,
and actual output-route gaps, see [Time-step faults](time-step-faults.md).

The C++ scenario has nine fixed stage slots. This admits the complete supported
chain: Watterson, global frequency shift, second source, two AWGN stages, CW,
impulse, fade and a final sample slip. Optional CW modulation/gating and secondary
fading use state within their owning stage. The slot limit does not override
the per-type limits or fixed arena capacity: a different nine-stage composition
can still be refused if it requires too many typed instances or arena bytes.
The parser rejects a tenth stage. Future radio-filter support would need a
separate capacity and memory review.

An impairment may also have a unique positive 32-bit `stream_id`. With this
field, its random streams are derived from the run seed and that identity,
so inserting or reordering other stages preserves its random realization.
Without it, historical stage-index keying remains unchanged (Python AWGN
retains its per-type instance index). Stage processing order still affects
the combined signal. See [random-stream identity](random-stream-identity.md)
for the exact key algorithm, validation, metadata and compatibility boundary.

The optional `second_source` stage follows Watterson and/or the global shift,
then precedes noise and other interference. It adds one independent WAV excerpt
at a stated input-referenced S/I, optionally shifts that excerpt by +/-75 Hz,
and can apply its own unit-power Rayleigh fading process and Markov ON/OFF
gate. Offset and fading share one 215-frame analytic-filter delay. The gate
acts on the filtered contribution in destination-window time, with exact OFF
samples while source/channel state continues; it adds no latency.
The [example](../../fixtures/scenarios/second-source.json) expects an
`interferer.wav` beside the scenario; supply the desired WAV with `--source`.
Its exported scenario supports host replay. For SD playback use a PCM24 source
and replace the exported host path with its on-card WAV basename. Sustained
SD playback and output parity still require physical board acceptance.

### Time-step faults

`sample_slip` is the scenario's discrete-time fault model: `delete` consumes
input samples without output; `duplicate` emits retained delivered-output
history. Event positions are seconds on the scenario input timeline at 48 kHz.
There may be at most 32 events, each 1 through 1024 samples; duplicate
expansion is also bounded per processing window. The stage must be last because
it changes the stream length. This is not a sample-clock ppm/drift model, which
remains deferred.

On the RT1170, the card is processed through the scenario engine and live
controls before the separate CUT stage. CUT offsets therefore count frames of
the engine output after any slips. A CUT DROP still consumes the engine output
and advances its state; a CUT ZERO bypasses the engine and inserts output
frames. The [cross-layer fault note](time-step-faults.md) gives the combined
render/prediction order and distinguishes both from USB missed intervals.

## 4. Scientist and maintainer view

### Numerical contract

The production C++ path receives normalized float. A packed PCM24 source sample
`q` becomes `x = q / 8388608`; the legacy PCM16 adapter uses `q / 32768`. Source gain is
`g = 10^(source_gain_db/20)`. The scaled desired reference is
`R = reference_rms * g`.

| Stage | Main convention |
|---|---|
| Watterson | Multipath fading channel, first stage only; path powers sum to 1, so the input power and `R` are unchanged ([Watterson guide](signal-lab-watterson.md)) |
| Frequency shift | One global constant offset or triangle sweep, after Watterson and before AWGN; adds 215 frames of Hilbert delay ([schema and contract](frequency-shift.md)) |
| AWGN | `N0 x BW = R^2 10^(-SNR/10)`, with selectable 3/6/9/12 kHz bandwidth in C++; the Python peer retains 3 kHz (see [AWGN](#awgn)) |
| CW | Tone RMS is `R * 10^(-C/I/20)`; sine peak is `sqrt(2)` times that RMS |
| Impulse | Envelope peak is `R * 10^(peak_db/20)` |
| Fade | Desired signal is multiplied by a time-varying linear gain derived from depth in dB |
| Sample slip | Delete consumes source without output; duplicate emits retained delivered history |

The final float stream is quantized once. `clip_policy: saturate` clamps to
signed PCM24; `clip_policy: reject` fails the block before committing output
statistics. PCM16 overloads remain only for historical tests and imports.

### Bounded resources

| Limit | Value |
|---|---|
| C++ input block | 2048 frames |
| Expansion slack | 256 frames |
| Maximum stages | 8 |
| Placement arena (CW, impulse, fade, sample slip) | 28 KiB |
| Typed engine slots | 2 AWGN stages, 1 Watterson stage, 1 global frequency-shift stage, 1 second-source stage |
| Explicit events per list | 32 |
| Slip run length | 1 through 1024 samples |

Duplicate expansion in any 2048-input-frame processing window must fit the
256-frame slack. A schedule can satisfy the per-event 1024 limit and still be
rejected because its local expansion budget is impossible.

### Determinism and block independence

Stages receive the absolute first input frame, seed, stage index, and scratch
space. Random streams are derived by stage so adding a later random stage does
not silently replace the first stage's sequence. Algorithms must produce the
same emitted PCM and digest for every legal caller block size.

Sample-slip history is delivered-output history, not arbitrary source history.
Delete/duplicate combinations must be tested at small and maximum blocks because
they expose history and capacity errors that fixed-block tests can hide.

### AWGN

See [AWGN](awgn.md) for the noise model, normalization, resources and
implementation walkthrough, and [wideband noise](wideband-noise.md) for
scenario bandwidth selection. AWGN follows the channel and frequency shift;
its reference remains the input signal RMS.

### Periodically pulsed CW

The existing `cw` slot can now apply an exact periodic gate. Optional paired
`pulse_period_seconds` and `pulse_duty_cycle` must resolve to whole period and
on-frame counts. It preserves carrier phase through off intervals and applies
the configured raised-cosine shoulders to each pulse. Omit both fields for
the existing continuous-CW output. C/I refers to the unramped on-state RMS;
gating reduces the full-window mean power. See [pulsed CW](pulsed-cw.md) for
timing, ramps, metadata and validation. No extra stage slot is consumed.

### Swept CW

The same `cw` slot accepts `frequency_sweep` in place of `frequency_hz`.
Its strict nested object contains `low_hz`, `high_hz` and
`slope_hz_per_second`. The repeating triangle starts low and ascending at
the CW window start. A Q0.64 accumulator preserves carrier phase through
turnarounds and optional pulse-off intervals. See [swept CW](swept-cw.md)
for the grammar, frequency limits, timing and validation scope.

### Markov-gated CW

The `cw` slot also accepts `markov_on_off` with `mean_on_seconds` and
`mean_off_seconds`. It starts on and alternates geometric integer-frame
dwells from a dedicated random stream. Both means must be at least one frame
and at most 1e6 seconds. It combines with fixed or swept CW and uses no extra
stage slot; periodic pulse timing is mutually exclusive. Carrier phase keeps
advancing while off. C/I describes the unramped on-state. See
[Markov CW](markov-cw.md) for distribution, seed, window and ramp details.

### Two-tone FSK

The `cw` slot accepts `fsk` with `baud`, `shift_hz` and a `pattern` of
`alternating` or `random`. In that mode, `frequency_hz` is the center and
`shift_hz` is the full mark-space separation. Each symbol must span a whole
number of 48 kHz frames; unsupported fractional periods are refused. The
carrier is phase-continuous, and symbol timing continues during pulse or
Markov OFF intervals. FSK and a frequency sweep are mutually exclusive.
See [FSK CW](fsk-cw.md) for timing, level, seed and validation details.

The same slot accepts `fading: {"spread_hz": 1}`: a unit-mean-power complex
Rayleigh tap using the Watterson Gaussian Doppler process, with two-sigma
spread from 0.1 through 10 Hz. It fades only the generated interferer and can
combine with fixed, swept or FSK carriers and either gate. Fading continues
through OFF intervals inside the CW window. C/I specifies ensemble mean ON
power; a finite realization fluctuates around it and is not renormalized.
See [faded CW](faded-cw.md) for the normalization and seed contracts.

### Failure and debugging signals

Inspect parse errors before rendering. During rendering inspect `frames_in`,
`frames_out`, `clipped_samples`, first clipped frame, scheduled/applied/dropped
events, stage component energy, source digest, output digest, and arena use.

A matching command exit code without matching source/scenario/reference hashes
does not establish deterministic equivalence.

## 5. 5th-grader view

Imagine a row of sound-effect boxes. The first box may make the recording
quieter. The next adds hiss. The next adds a whistle. The last may cut out or
repeat a tiny piece of tape. Moving the boxes changes the sound, so the scenario
writes down their exact order.

The seed is like choosing one numbered bag of dice. Using bag 42 again makes the
same random crashes happen in the same places.

## 6. Toy worked example

Take four identical PCM samples at one-quarter full scale:

```text
input PCM24:  [2097152, 2097152, 2097152, 2097152]
input float:  [0.25, 0.25, 0.25, 0.25]
```

Add a 12 kHz CW tone at 48 kHz. Its phase advances by 90 degrees per sample. If
the requested C/I is 6 dB, its RMS is approximately `0.25 * 10^(-6/20) = 0.125`
and its peak is approximately `0.1768`.

```text
tone:         [0, +0.1768, 0, -0.1768]
mixed float:  [0.25, 0.4268, 0.25, 0.0732]
output PCM24: [2097152, 3580256, 2097152, 614046]  (illustrative rounding)
```

If a fade stage came after the CW, it would scale both columns. If the fade came
before the CW, it would scale only the source before the tone was added. That is
why document order is part of the experiment identity.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Scenario grammar and bounds | `signal-lab/include/signal_lab/scenario.hpp`, `signal-lab/src/scenario.cpp`, `host/signal_lab/scenario.py` |
| Stage interface and storage | `signal-lab/include/signal_lab/impairment.hpp` |
| Mixer, quantization, and stats | `signal-lab/include/signal_lab/mixer.hpp`, `signal-lab/src/mixer.cpp` |
| Source/sink pumping | `signal-lab/include/signal_lab/sample_stream.hpp` |
| Individual stages | `signal-lab/src/awgn.cpp`, `cw.cpp`, `impulse.cpp`, `fade.cpp`, `sample_slip.cpp` and Python peers |
| Watterson channel | `signal-lab/include/signal_lab/watterson*.hpp`, `analytic_filter.*`, `doppler_filter.*`, `signal-lab/src/watterson.cpp` ([guide](signal-lab-watterson.md)) |
| Unit types and casts | `signal-lab/include/signal_lab/units/*.hpp`, `convert.hpp` |
| Deterministic math and RNG | `signal-lab/src/det_math.cpp`, `det_gaussian.cpp`, `kaiser.cpp`, `host/signal_lab/rng.py` |
| Catch2 unit tests | `signal-lab/tests/*_tests.cpp` except `signal_lab_tests.cpp` and `live_control_tests.cpp`, in `wfg_signal_lab_unit_tests` |

When adding a stage, update both parsers, both implementations, storage sizing,
sidecar reporting, schemas, module documentation, cross-block tests, and the
stage's own Catch2 suite (see "Catch2 unit tests" below). State
explicitly whether the stage is a channel effect, external interferer, or receiver
fault model.

## 8. Function walkthrough and lifetime contract

1. `parse_scenario` borrows JSON bytes and decodes into a caller-owned `Scenario`.
   Its fixed scratch document has an atomic non-spinning guard; an overlapping
   call fails (`busy`). JSON overflow and non-integral/out-of-range seeds are
   rejected. It returns an `m110::Status`: success, `invalid_argument` for a
   document the grammar refuses (including `json::Document::parse`'s own
   refusals), or `validate_scenario`'s verdict. Every message is static text.
2. `validate_scenario` also validates programmatically built descriptors. Counts
   are checked before array traversal; numeric/time fields are finite and bounded.
   A rule it refuses is `invalid_configuration` with a message that names it.
3. `Engine::configure` borrows the scenario for the engine's lifetime, validates
   it, builds each stage (the AWGN and Watterson stages in typed slots, the others
   placement-constructed in the arena) and prepares shared scratch.
   Reconfiguration destroys prior stages. Engine copy/move is forbidden because
   pointers refer into the engine's own arena and slots. Keep the scenario
   immutable while configured. The overload with `StageResources` lends the
   Watterson channel (and a host tap observer); the three-argument overload lends
   none, and a Watterson scenario then fails with a message that says so.
   `configure` returns an `m110::Status`: `validate_scenario`'s verdict,
   `invalid_argument` for a reference RMS that is not positive and finite,
   `buffer_too_small` when the arena is full, `internal_error` when a placement
   fails, or the Status of the stage whose `Impairment::prepare` refused
   (`invalid_configuration` with the stage's message; the Watterson stage's is its
   channel's). The engine is then unconfigured and keeps no stage.
4. `Engine::process(span, span)` accepts at most 2048 input frames and requires
   256 frames of output slack. It returns an `m110::Result<std::size_t>`: the
   output frame count, or the failure. Sample slips can legitimately produce
   zero frames, which is a count, never an error. The engine's own failures are
   the `engine_failure` Statuses in `mixer.hpp`:

   | Status | Code | When |
   |---|---|---|
   | `not_configured` | `unavailable` | process() before a successful configure() |
   | `invalid_block` | `invalid_argument` | a null buffer, more than 2048 frames, or an output without the 256 frames of slack |
   | `nonfinite_input` | `invalid_argument` | a non-finite input sample (float path), found before any state changes |
   | `nonfinite_output` | `internal_error` | a stage produced a non-finite sample |
   | `clipping` | `invalid_configuration` | the output clipped and the scenario's `clip_policy` is `reject` |
   | `block_overrun` | `internal_error` | a stage lengthened a block past the capacity it was given |

   A stage's fault is the Status its `Impairment::process` returned (the Watterson
   stage's contiguity and configuration checks, with the channel's own message), so
   the cause reaches the caller. The engine latches the first failure:
   `Engine::status()` holds it, and every later process() returns it without
   touching input or stages, until `configure()` or `configure_passthrough()`.
   `same_failure()` tells one Status from another by code and message text, and
   `is_engine_failure()` tells the engine's own from a stage's. The RT1170
   player keeps the Status that refused a scenario (the parser's or
   `configure`'s) or stopped its engine, and the one with which the live
   controller (`LiveController`) refused a block, its reset or a preset, and
   reports its message through the `ENGINE?` query
   ([protocol](../live-control-protocol.md#engine-query)).
   `scripts/check-error-style.py` (in ctest and the host CI) fails an owned
   function whose parameter carries a failure out instead of a returned Status
   or Result: a `const char**`, a `const char*&` or a string written through a
   reference or pointer named for an error (`error`, `error_text`, `lastError`,
   `what`; a string also `detail`, `text`, `e` or `out`), a `char*` or
   `std::span<char>` error buffer, or a non-const `Status&` or `Status*`. It
   fails a failure's text kept in an object too: a `set_error()` that takes
   one and an `error()` that gives one back, and a function named for an
   error that returns its failure as a text, unless its one parameter is the
   failure it names (a Status, its code, an error enum: a published-name
   table). A failure an object latches is a Status it gives as one
   (`Engine::status()`). A `bool` result is held by the
   bool-return ratchet, `scripts/check-bool-returns.py`
   ([coding standard](../coding-standard.md#failure-gates)).
5. Invalid/non-finite input is rejected before input digest/state changes. A
   stage/clip rejection may already have advanced input/stage state and overwritten
   output storage: discard the buffer and reconfigure before continuing. Output
   statistics/digest describe accepted blocks only.

`tests/safety_contract_tests.cpp` exercises bounds, placement and non-finite input.
The existing engine and renderer tests cover clipping, slip expansion/history,
reference export and block independence. Factories expose a low-level placement
boundary: callers own aligned storage and valid parameters; use `Engine` as the
normal validated entry point. A factory returns a `PlacedStage`
(`m110::Result<std::reference_wrapper<Impairment>>`): the stage, or
`placement_refused` for storage that is null, misaligned or too small
(`construct_impairment`: `not_an_arena_stage` for AWGN and Watterson).

### Catch2 unit tests

New signal-lab components use Catch2 unit tests;
the existing executables keep their own harness. Every suite is a source file
in `signal-lab/tests/` added to `wfg_signal_lab_unit_tests` in
`signal-lab/CMakeLists.txt`, which `wfg_add_catch2_test`
([build and dependencies](build-and-dependencies.md#catch2-unit-tests-host-only))
links with Catch2 and the library and compiles with the library's determinism
flags. Each `TEST_CASE` is its own ctest test, `wfg_signal_lab_unit_tests: <name>`,
labelled `catch2`:

```powershell
ctest --test-dir build/main/host-c -L catch2 -j 4 --output-on-failure
```

The suites: `det_math_tests.cpp`, `units_tests.cpp`, `det_gaussian_tests.cpp`,
`kaiser_tests.cpp`, `analytic_filter_tests.cpp`, `doppler_filter_tests.cpp`,
`watterson_tests.cpp`, `watterson_scenario_tests.cpp`,
`watterson_engine_tests.cpp`, `awgn_tests.cpp` and `allocation_tests.cpp` (the
[Watterson guide](signal-lab-watterson.md#10-tests) maps them to the design's
tests). Shared helpers are `test_spectrum.hpp` (FFT, periodograms, a line fit)
`test_taps.hpp` (a recording `TapObserver`), `test_convert.hpp` (the suites'
own conversions: signed reference-model frames and iterator distances, each
written once) and `allocation_counter.hpp`/`.cpp` (the binary's counting global
operator new and delete).

`det_math_tests.cpp`, the first suite, is the pattern. A case tests one
property. Each constant is named, with its source at the definition (a header
contract, a standard, a measurement). Floating-point results are compared within
a stated tolerance (`WithinRel`, `WithinAbs`), never against one C library's last
bit; exact comparisons are kept for values the construction makes exact (powers
of two through the range reduction, rounding ties, PCM24 saturation). Catch2 is
built without exceptions, so a failed `REQUIRE` ends that case's process; use
`CHECK` unless the rest of the case depends on the condition. The suites run on
the host only; board behaviour is still checked by captures.

### Python PCM24 boundary

`host/signal_lab/wav_io.py` reads packed signed PCM24 without dropping its low
eight bits. `quantize_pcm24` checks finite values, clamps, rounds half-even, then
packs three little-endian bytes. The writer excludes odd-byte RIFF padding from
PCM hashes. `render` reports emitted PCM24 levels and hashes. Legacy PCM16 is an
explicit source import, never the new renderer output. `tests/test_pcm24.py`
checks exact signed bytes and one-LSB pass-through at different block sizes.

Python uses NumPy PCG64 and float64 operations; C++ uses its portable PCG32/math
implementation and float processing. Format compatibility does not imply identical
random samples. A C++ host/RT comparison must retain the same source, recipe,
reference and engine identity.
