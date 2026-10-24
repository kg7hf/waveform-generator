# Markov-gated CW impairment

CW scenarios may add a stochastic on/off gate with `markov_on_off`:

```json
{
  "type": "cw",
  "frequency_hz": 12000,
  "ci_db": 12,
  "phase_degrees": 45,
  "ramp_seconds": 0.002,
  "markov_on_off": {
    "mean_on_seconds": 0.25,
    "mean_off_seconds": 0.75
  }
}
```

The object accepts exactly `mean_on_seconds` and `mean_off_seconds`. Each must
be finite, numeric, and between one 48 kHz frame and 1,000,000 seconds. Means
may be fractional frame counts. Markov gating can accompany fixed-frequency or
swept CW, but it cannot be combined with periodic pulse fields.

The gate begins ON at the start of the CW window. It alternates ON and OFF
integer-frame dwells. For a state with mean `m` frames, the discrete geometric
model uses `p = 1/m` and draws a dwell as
`floor(log(1-u) / log(1-p)) + 1`, where `u` is one uniform random value in
`[0,1)`. A one-frame mean still consumes one draw and yields a one-frame dwell.
The carrier or sweep phase continues to advance while the gate is OFF. Each ON
dwell receives leading and trailing raised-cosine shoulders using
`ramp_seconds`, clamped to half the dwell; a final dwell cut off by the CW
window uses its visible truncated length. A zero ramp leaves rectangular
edges. The interference ratio `ci_db` describes the unramped ON-state tone
RMS relative to the configured reference RMS; gating and ramps lower its
time-averaged power.

The C++ renderer uses its deterministic PCG32 generator with family ID 10.
Its uniform values have 24 significant bits; probabilities are quantized and
the largest draw has a finite tail of about 16.64 times the configured mean
for large means. This is the version-1 contract. No transition table or heap
allocation is used on the C++ sample path. The Python renderer uses a dedicated
NumPy PCG64 stream keyed by the existing run seed and CW stage index, also with
family ID 10. Each implementation consumes one uniform draw
when entering each dwell and retains only the current state, boundary and RNG
state. Python uses NumPy PCG64 while the embedded renderer uses its own RNG
implementation, so matching distributions are expected, not bit-identical
transition schedules. Enabling the gate does not consume the CW random-phase
stream or other family streams. Without an explicit `stream_id`, random streams
retain their legacy stage-index key; insertion can change later streams. The
optional [stable identity](random-stream-identity.md) derives a stage seed from
its ID and uses RNG index zero, preserving its schedule when another stage is
inserted.

For a Markov-enabled CW stage, processing must begin at absolute frame zero and
continue in contiguous blocks. Empty blocks do not advance the gate. Resolved
metadata records the means in seconds and frames, initial state, distribution,
RNG family and algorithm version. Python additionally names its RNG implementation.
C++ scheduled/applied event counts record ON dwells actually encountered; they
do not predict the count in a future stochastic interval. The Python implementation
is a reference model; host software tests do not establish C++ bit parity,
board behavior, or decoder performance. Those physical and decoder checks
remain pending.

## Repeatable checks

`tests/test_cw_markov_render.py` measures transitions from rendered PCM24,
checks geometric dwell statistics and on-state C/I, and exercises block
sizes 1/127/2048. Catch2 covers literal dwell vectors, ramps, stream-order
errors, reconfiguration and allocation-free processing. Python checks its
own transition statistics and partition/reset behavior. Run the
[host tests](../testing.md#host-tests) after changing the gate; generated
measurements belong in the ignored run directory.

Physical board timing, reset repeatability and decoder C/I ladders require
separate acceptance; these software checks do not establish them.
