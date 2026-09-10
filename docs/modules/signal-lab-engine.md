# Signal Lab Static Impairment Engine

## 1. Scope

This guide covers the Python reference implementation in `host/signal_lab/` and
the allocation-free C++23 implementation in `signal-lab/`. Both consume mono
48 kHz PCM and apply an ordered scenario containing AWGN, CW, impulse, fade, and
sample-slip stages.

It does not cover live controls, waveform encoding, codec transport, or decoder
scoring. Those are separate modules.

## 2. 50,000-foot view

Signal Lab is a conveyor belt for samples. A scenario fixes the source gain,
reference interval or RMS, clip policy, seed, and an ordered list of impairment
stages. Every source sample moves through those stages in document order.

```text
normalized float -> source gain -> stage 0 -> stage 1 -> ... -> clip -> PCM24 boundary
```

The Python path is convenient for corpus creation and inspection. The C++ path
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

Stage order is observable. A fade before noise attenuates the desired signal but
not later noise. A fade after noise attenuates both. Sample slips must be last
because they change stream length and the meaning of later input-frame windows.

## 4. Scientist and maintainer view

### Numerical contract

The production C++ path receives normalized float. A packed PCM24 source sample
`q` becomes `x = q / 8388608`; the legacy PCM16 adapter uses `q / 32768`. Source gain is
`g = 10^(source_gain_db/20)`. The scaled desired reference is
`R = reference_rms * g`.

| Stage | Main convention |
|---|---|
| AWGN | Noise RMS is `R * 10^(-SNR/20)` after band limiting |
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
| Placement arena | 32 KiB |
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
| Deterministic math and RNG | `signal-lab/src/det_math.cpp`, `host/signal_lab/rng.py` |

When adding a stage, update both parsers, both implementations, storage sizing,
sidecar reporting, schemas, module documentation, and cross-block tests. State
explicitly whether the stage is a channel effect, external interferer, or receiver
fault model.
