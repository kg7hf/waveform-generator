# Global frequency offset and Doppler sweep

The portable C++ engine implements whole-signal frequency shift. It runs in
the host renderers and the shared RT1170/RT1064 player code. Physical board acceptance remains
outstanding. This is separate from each Watterson path's own `shift_hz`.

## Scenarios

One `frequency_shift` stage is allowed. It must be first, or immediately after
Watterson when a Watterson stage is present. AWGN, interference and other
impairments follow it; sample slips remain last.

```json
{"type": "frequency_shift", "mode": "offset", "offset_hz": 75}
```

Offset mode requires `offset_hz`, a finite value in [-150, 150]. The additional
range provides headroom around the +/-75 Hz rows of C.6.3 and D.6.4.

```json
{"type": "frequency_shift", "mode": "sweep", "amplitude_hz": 75,
 "slope_hz_per_second": 3.5, "start_frequency_hz": -75,
 "direction": "ascending"}
```

The sweep fields above are defaults. Amplitude must be positive and at most
150 Hz. Slope must be positive, finite and representable by the cycle-rate
word. Start frequency must lie within the amplitude bounds. Direction is
`ascending` or `descending`; at an endpoint the trajectory turns inward.
When amplitude changes and the starting frequency is omitted, the start is
the negative amplitude. Unknown, duplicate and mode-inapplicable fields are
refused. Offset mode does not also apply a sweep.

The defaults describe C.6.4/D.6.5: a triangle from -75 to +75 Hz at 3.5 Hz/s,
with a nominal period of 300/3.5 seconds. A complete example is:

```json
{
  "schema": "signal-lab.scenario/1", "seed": 7,
  "reference_rms": 0.25, "source_gain_db": -12,
  "clip_policy": "reject",
  "impairments": [
    {"type": "watterson", "paths": [
      {"spread_hz": 1}, {"delay_ms": 2, "spread_hz": 1}
    ]},
    {"type": "frequency_shift", "mode": "sweep"},
    {"type": "awgn", "snr_db": 24}
  ]
}
```

Use the C++ `signal_lab_render` for ordinary rendering and card `.SCN`
generation. A playlist's `scn=` selects the same document. The Python reference
renderer refuses this stage with a message directing the user to the C++ tool.
The raw scenario identity includes its parameters. The ordinary renderer's
sidecar has a `frequency_shift` object with algorithm version, parameters and
added filter delay. Firmware INFO advertises `frequency-shift` and its algorithm
version. Existing Watterson identity/latency fields continue to describe the
Watterson stage alone.

The validation renderer also streams [frequency truth](../design/frequency-truth-export.md)
with `--frequency-truth-out`. Its absolute frame coordinates and latency metadata
are independent of caller block size and of Watterson's separate tap export.

## Signal and time conventions

The stage reuses `AnalyticFilter`, including its coefficients and accumulation
order, to form `x_a(n) = x(n-215) + j*x_h(n)`. It emits
`Re{x_a(n) * exp(j*phi(n))}`. Positive offset raises a positive-frequency tone.
The oscillator starts with zero phase on preparation. Its unsigned 64-bit phase
word advances once per sample and wraps modulo one turn; it is never reset at
a triangle turn or a caller block boundary.

The triangle cycle is also represented in Q0.64. Its absolute-index phase is
`start + frame * cycle_step` modulo one turn, with the cycle rate rounded once.
No half-period is rounded to an integer number of frames: the nominal period
at 48 kHz is 4,114,285.714... frames. The carrier integrates adjacent frequency
samples with the trapezoid rule. The resulting numerical contract is a sampled
triangle with a quantized cycle rate, not an exact real-arithmetic oscillator
over every possible uint64 duration. Tests compare its truth with an independent
continuous formula beyond frame 2^32 and compare exact output across partitions.

The second Hilbert pass adds **215 frames (4.479 ms at 48 kHz)** after Watterson's
own 215-frame delay and any path delays. Even an explicitly configured zero
offset retains that delay. Removing the stage removes its filter and delay.
The shared filter's validated input band still applies. In a cascade, content
already moved outside that band by an upstream Watterson path needs separate
characterization; this stage does not broaden the filter's passband.

The engine remains a 48 kHz engine. The frequency-law helper accepts a typed
sample rate, and filter geometry derives from the compiled stream rate; this
does not implement selectable USB sample rates. Reference RMS is the same
input reference used by AWGN, and the shifter precedes AWGN so added noise is
not shifted or used to redefine SNR.

## Ownership and validation

The engine owns one typed optional stage, including its Hilbert history and one
64-frame analytic buffer. It borrows the scenario parameters for the engine's
lifetime, as the other stages do. Configure and process allocate no heap memory.
The RT1170 player constructs its engine once in aligned OCRAM2 storage before
creating the owner task. NOLOAD bytes are initialized explicitly, including
after a failed task-creation attempt; a retry keeps the live engine. The map gate
requires this engine section in OCRAM2 and limits it to 64 KiB (55,360 bytes in
the current ARM release build). Host tests poison the storage and exercise a
failed start, successful retry and repeated-start refusal.
Preparing the stage clears its filter, phase, cursor and statistics. A bad
configuration or discontinuous process call returns a Status; the engine latches
a process failure.

Host coverage includes strict schema/placement checks, both shift signs,
spectral image/spur measurements, known phase values, fractional-period truth,
turn crossings, exact block partitions, Watterson/AWGN composition, allocation
checks and actual player code exercised through USB/codec mocks with CUT.
These are software tests. Target timing, captures, reset repeatability, deadline
and underrun counts, and the prescribed long board rows remain required.
