# Rayleigh faded CW impairment

CW may include an optional one-path Rayleigh Doppler process:

```json
{
  "type": "cw",
  "frequency_hz": 1000,
  "ci_db": 12,
  "phase_degrees": 30,
  "ramp_seconds": 0.01,
  "fading": {"spread_hz": 1.0}
}
```

The `fading` object accepts exactly `spread_hz`, a finite value from 0.1 to
10 Hz. It may be combined with fixed, swept, or FSK CW and either supported
periodic or Markov gating. The fading process begins at frame zero of the CW
window, advances on every frame inside that window even while the tone is gated
OFF, and does not advance before or after it. The carrier, sweep, or FSK phase
and symbol timing keep their existing window-relative behavior.

The C++ stage embeds one `WattersonPathState` and calls its existing gain
generator, with unit path power, zero delay and zero frequency offset. It
does not apply a second channel to the desired signal or require a Hilbert
filter for the analytically generated carrier. Faded scenarios process from
global frame zero in contiguous blocks, including blocks outside the CW window.

The Python reference uses the same E.5.4 Gaussian Doppler FIR shape and linear
node interpolation. At the 48 kHz stream rate it chooses
`M = floor((48,000 / 64) / spread_hz)` frames per node, designs the Gaussian
kernel with its outer tap at most 1% of the peak, and corrects for the stored
float32-rounded coefficients' energy. Complex Gaussian inputs have unit
expected magnitude-squared. The implementation applies the same interpolation
power correction used by Watterson, so the interpolated complex gain has unit
ensemble mean power. It keeps one finite tap history and two adjacent complex
nodes; it never precomputes a gain sequence for the full render duration.

If `theta` is the existing CW phase and `h = h_real + j h_imag` the complex
Rayleigh gain, the faded sample is
`A * (h_real * sin(theta) + h_imag * cos(theta))`, where
`A = sqrt(2) * reference_rms * 10^(-ci_db/20)`. The complex gain therefore
changes both envelope and phase. `ci_db` sets expected long-run tone power
through the unit-mean-power fading process while ON and unramped; the finite render's measured power
varies with its realized fade and is not renormalized. Gate ramps still apply
at ON-window edges, not at fade or carrier-cycle boundaries.

The Python stream uses NumPy PCG64 family 12, keyed by run seed and CW stage
index. Its Gaussian draws intentionally differ from the embedded deterministic
PCG32 sequence, so the fading distributions and design match while individual
realizations are not sample-identical. Enabling fading does not consume other
random families; inserting or reordering a stage can still change later
index-keyed streams unless the stage has an explicit
[`stream_id`](random-stream-identity.md), which derives a stable stage seed
and uses RNG index zero. Resolved metadata records spread, algorithm version, RNG
family and implementation, unit ensemble mean power, and the Rayleigh
distribution.

Host tests check the independently computed tap design, unit-power and
Rayleigh envelope moments, seeded repeatability, block partitions, CW-window
origin, and continuity through gate-off intervals for fixed, swept, and FSK
carriers. The PCM24 tests pin a family-12 known-answer vector and use a coherent
12 kHz carrier to recover complex-envelope observations, with a one-frame I/Q
skew. The statistical row measures 600 seconds at 10 Hz spread; envelope
observations spaced by 0.5 seconds share no FIR excitation samples. It checks
Rayleigh moments and the CDF against the unit-power model without fitting the
observed mean. These are software checks. Board parity, physical channel captures,
and decoder performance remain pending.
