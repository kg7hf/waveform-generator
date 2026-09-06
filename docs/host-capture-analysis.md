# Host capture analysis

`host/analyze_capture.py` is a post-capture engineering check for the
generator's streamed WAV. It is intentionally independent of the live capture
process and uses only the Python standard library, so a copied
`tools/waveform-generator` directory remains sufficient to run it.

The capture input must be a complete RIFF/WAVE file containing one mono,
48,000 Hz, IEEE float32 `data` chunk. The analyzer checks the RIFF length,
chunk bounds, format fields, integral frame count, whole-file SHA-256, PCM
data SHA-256, and the streamed sample count. It computes finite, nonfinite,
clipped (`abs(sample) >= 0.999`), peak, RMS, and DC-mean measurements without
loading the audio into memory.

The run checks use explicit defaults of 4,800 samples (100 ms at 48 kHz) for
long-run detection and `1e-6` linear amplitude for a dropout. It reports the
number and total sample count of exact-zero, near-zero dropout, and constant
sample runs at least that long, together with each maximum run length. A
silence or constant run can be intentional; these fields are diagnostics and
require interpretation using the route and source artifact.

Example:

```powershell
python host/analyze_capture.py artifacts/runs/<run>/capture.wav `
  --output-json artifacts/runs/<run>/audio-metrics.json
```

An optional `--source` accepts a mono PCM16, 48 kHz WAV. The analyzer reads a
one-second source window at the beginning and end, searches bounded capture
regions using normalized zero-mean correlation decimated every 100 samples,
and reports the two anchor locations, confidence, lag, and a two-anchor
relative clock estimate in ppm. This is a bounded diagnostic estimate rather
than an oscillator calibration: analog filtering or noise, repeated waveform
features, silence, and short or drifted captures may make it unavailable or
ambiguous. The capture is never resampled to conceal the measured lag.

The JSON contains `engineering_test_only=true` and
`formal_conformance=false`. A successful analyzer exit means that the artifact
was structurally and numerically analyzed; it does not establish tone fidelity,
modem decode success, M110 conformance, or a clean baseline.
