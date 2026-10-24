# Triangular swept CW impairment

The C++ and Python signal-lab CW stages can add a fixed carrier or a repeating
triangular frequency sweep. Exactly one of `frequency_hz` and
`frequency_sweep` is required. A sweep uses these three nested fields:

```json
{
  "type": "cw",
  "frequency_sweep": {
    "low_hz": 300,
    "high_hz": 3300,
    "slope_hz_per_second": 300
  },
  "ci_db": 12,
  "phase_degrees": 0,
  "ramp_seconds": 0.01
}
```

The triangle starts at `low_hz`, rises at the configured positive slope to
`high_hz`, then descends at the same slope and repeats. One full up-and-down
period is `2 * (high_hz - low_hz) / slope_hz_per_second`. Frequencies must
satisfy `1 <= low_hz < high_hz <= 23999`. The nested object accepts only the
three fields shown above; each value must be a finite number, with booleans
and numeric strings refused.

The requested cycle rate is converted to a Q0.64 phase-cycle increment per
48 kHz frame, rounded half to even. The rounded step must be positive and
below 2^53 so it is exactly representable by the deterministic phase
arithmetic. Inputs whose step falls outside that range are refused. Resolved
metadata includes the requested limits and slope, `cycle_step_q64`, the
Q0.64-realized `realized_period_seconds`, and `sweep_algorithm_version`.
Fixed-CW metadata and PCM remain unchanged; sweep-only fields are absent from
fixed-tone reports.

The CW phase is relative to `start_seconds`. The triangle and carrier phase
continue over the entire configured duration, including pulse-off periods.
Optional pulse gating applies to the amplitude after calculating the carrier;
it does not pause or restart the sweep. The existing window and per-pulse
raised-cosine ramps are unchanged. `ci_db` continues to use the source/input
reference and the nominal carrier RMS while on and unramped. Ramps and pulse
duty reduce total time-averaged interference power.

The Python reference analytically integrates the continuous triangle using
the Q0.64-realized cycle rate and an absolute frame offset from the CW window
start. This keeps rendering independent of block partitioning. The C++ engine
uses its own per-sample fixed-point phase accumulator; the Python model is
independent and does not promise bit-for-bit output parity. The live CW
overlay is a separate interface and is not extended by this scenario field.

The C++ oscillator follows the phase-5 Q0.64 phase model. It integrates each
sample interval and splits an interval at a fractional-frame turnaround, so
carrier phase does not jump when the slope changes. It does not run the
whole-signal Hilbert shifter and adds no filter latency. Its stage processing
must start at input frame zero and remain contiguous, including blocks before
the CW window begins; late starts and gaps are refused without advancing the
oscillator. Empty blocks do not advance it. Fixed-frequency CW retains its
existing random-access behavior. A fresh prepare resets the sweep.

Both C++ renderers report the resolved frequency-sweep object and can replay
the [example scenario](../../fixtures/scenarios/swept-cw.json). Target-scenario
export retains the nested sweep settings. The player's INFO capability is
`swept-cw`; its PLAYLIST scenarios use the same engine. Sweep and pulse share
one existing CW slot, with no additional random draws or heap allocation on
the processing path.

Python tests cover validation, a full 20-second 300–3300 Hz cycle at
300 Hz/second, a fractional-frame period, phase continuity through pulse-off
intervals, chunk partition identity, input-referenced C/I, and unchanged
fixed-tone behavior. Decoder characterization and physical/board parity
remain separate validation work.

The October 12, 2026 C++ host measurement renders a full 20-second
300-3300 Hz cycle at 300 Hz/s with a 12 dB input-referenced C/I. PCM24 agrees
with an independent closed-form phase integral within 0.547 LSB. A Hilbert
phase measurement over symmetric 16-frame intervals has maximum relative
frequency error 0.01205%, including the turnarounds (limit 0.1%). Measured
C/I is 12.00000001 dB (limit +/-0.1 dB). The estimator's synthetic PCM24
control has 0.01203% maximum relative error. These finite-window frequency
measurements compare interval averages, rather than claiming pointwise
frequency resolution from a single real sample.

Additional host checks cover fractional-frame turnarounds with pulsing,
exact block identity at 1/127/2048 frames through both C++ renderers,
export/replay, literal sample vectors, frequency queries beyond 2^32,
no allocation, and invalid/capacity/overflow/noncontiguous-input recovery.
All four player mocks exercise combined channel/offset/noise/pulsed-sweep
scenarios and return to fixed CW. Seven retained old/new continuous and
pulsed-CW cases have identical PCM24 and engine digests. The offline ARM
release and its memory/stack gates pass; target deadlines and physical
host/board parity still need the explicitly released board.
