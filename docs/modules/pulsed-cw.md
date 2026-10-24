# Periodically pulsed CW impairment

The signal-lab CW impairment supports optional periodic gating. Omit both
pulse fields to preserve the existing continuous-CW behavior and resolved
metadata.

```json
{
  "type": "cw",
  "frequency_hz": 1000,
  "ci_db": 12,
  "phase_degrees": 0,
  "pulse_period_seconds": 0.1,
  "pulse_duty_cycle": 0.25,
  "ramp_seconds": 0.002
}
```

`pulse_period_seconds` and `pulse_duty_cycle` must appear together. The period
must be finite, positive, and no greater than 1e9 seconds. Duty cycle is a
finite fraction greater than zero and at most one. Both the period and its
on-time (`period * duty`) must map to positive whole 48 kHz frame counts within
1e-6 frame. The renderer refuses values that would otherwise be rounded to a
different pulse timing. The realized duty cycle is the integer on-frame count
divided by the integer period-frame count.

The first pulse begins at the CW window start, after `start_seconds`; the
duration window still determines when the stage ends. Carrier phase uses the
sample offset from that window start and advances through off intervals, so
each pulse resumes the same free-running sinusoid instead of restarting its
phase. The window uses the same raised-cosine amplitude rule as continuous CW.
Each pulse's on-window gets its own leading and trailing shoulders. The ramp
length is clamped to at most half that pulse's on-time; a final pulse cut short
by the CW duration is clamped using its truncated length. A zero ramp leaves
the pulse edges rectangular. At 100% duty, each period still has its configured
shoulders; set `ramp_seconds` to zero for an ungated continuous tone.

`ci_db` retains the existing convention: it is the ratio of reference RMS to
the sinusoid's RMS while the carrier is on and unramped. Periodic gating and
the shoulders reduce total time-averaged interference power below that
on-state value. Resolved stage metadata includes `pulse_period_frames`,
`pulse_on_frames`, and `pulse_realized_duty_cycle` only when pulsing is enabled.

The Python implementation uses the same NumPy/float64 sinusoid and amplitude
convention as continuous CW. Its timing and raised-cosine math are independently
implemented from the C++ engine; numerical sample-for-sample parity is not a
contract. Python tests cover frame-alignment refusal, window-relative phase,
ramps, truncated pulses, chunk partitioning, on-state C/I, and unchanged legacy
metadata when the optional fields are omitted.

The C++ stage, both C++ renderers and the player accept the same timing pair.
The [example scenario](../../fixtures/scenarios/pulsed-cw.json) can be used in
a PLAYLIST item. This adds no stage slot and consumes no additional random
draws. The INFO capability is `pulsed-cw`; the existing live CW overlay remains
a separate control path.

Host validation includes literal sample vectors, exact pulse masks and ramps,
a phase check beyond 2^32 frames, on-state C/I within 0.1 dB, malformed timing
refusals, and identical PCM24 across block sizes 1, 127 and 2048. Both renderers
agree, target-scenario exports replay, and all four mocked player variants
exercise pulsed/continuous reconfiguration with Watterson, offset, noise and
CUT. Four retained legacy-CW cases have identical PCM24 and engine digests
against the preceding renderer binary. These are software checks; physical
host/board parity, reset repeatability and decoder characterization remain.
