# Binary FSK carried by CW

The CW impairment accepts optional binary FSK parameters:

```json
{
  "type": "cw",
  "frequency_hz": 1000,
  "ci_db": 12,
  "phase_degrees": 30,
  "ramp_seconds": 0.002,
  "fsk": {
    "baud": 1000,
    "shift_hz": 200,
    "pattern": "alternating"
  }
}
```

FSK requires `frequency_hz` as the center frequency and cannot use
`frequency_sweep`. Its nested object accepts exactly `baud`, `shift_hz`, and
`pattern`. The mark frequency is `center + shift_hz / 2`; the space frequency
is `center - shift_hz / 2`. Thus `shift_hz` is the full mark-to-space
separation. Both frequencies must be from 1 through 23,999 Hz and must remain
distinct after conversion to the C++ oscillator's Q0.64 phase increments.

Baud must be finite, positive, and no greater than 48,000. The symbol period
must resolve to a positive whole number of 48 kHz frames within 1e-6 frame and
must not exceed 48 trillion frames. Fractional baud rates are accepted when
they meet that exact frame-grid requirement (37.5 baud has a 1,280-frame
symbol); an unaligned rate such as 110 baud is refused rather than rounded.
The required `pattern` is either `alternating` or `random`. Alternating starts
with mark. Random chooses mark when its uniform draw is at least 0.5 and space
otherwise.

The first symbol and carrier phase are relative to the start of the CW window.
Carrier phase and symbol timing continue through periodic or Markov gate-off
intervals; the next active sample resumes the phase and symbol that would have
occurred without gating. Ramps apply at gate ON-window edges, not at symbol
boundaries. A zero ramp keeps symbol transitions phase-continuous. Periodic and
Markov gates are supported, but remain mutually exclusive with each other.

`ci_db` sets the conditional tone RMS relative to the reference RMS while the
carrier is on and unramped. Each symbol uses that same single-tone amplitude;
the mark and space tones do not each receive a full simultaneous power
allocation. Gating and ramps reduce time-averaged emitted power.

Random patterns use dedicated family 11, keyed by the run seed and CW stage
index: deterministic PCG32 in C++, NumPy PCG64 in Python. One draw is consumed on entering each random symbol,
including the first. Alternating patterns do not draw randomness, while their
resolved metadata still identifies family 11. The embedded renderer uses its
own random generator, so Python and C++ transition schedules are distribution
compatible but not bit-identical. Adding the FSK option does not consume other
random families; inserting another stage can change later stages' index-based
streams. An explicit [`stream_id`](random-stream-identity.md) instead selects
a stable derived seed and RNG index zero, preserving this stage's symbols
when other stages are inserted or reordered.

Resolved FSK metadata reports the requested baud and full shift, realized mark
and space frequencies, integer frames per symbol, pattern, algorithm version
and RNG family. Python additionally names its RNG
implementation. Host tests establish software behavior only. Board timing,
C++ host/target parity and decoder performance remain pending physical and
receiver characterization. Python and C++ random schedules intentionally differ.
