# Global frequency-shift truth export

`wfg_watterson_render --frequency-truth-out frequency.csv` writes a separate,
streamed frequency-truth artifact when the scenario contains its global
`frequency_shift` stage. This works with that stage alone and with a preceding
Watterson channel. The existing `--truth-out` WFGTAPS1 binary remains unchanged
and continues to record channel path taps only.

The CSV begins with `WFGFREQ1`, followed by comment metadata and a column row:

```text
WFGFREQ1
# sample_rate_hz=48000
# stride_frames=64
# engine_input_frames=96000
# mode=sweep
# frame_coordinate=absolute_engine_input_frame_at_frequency_shift
# frequency_shift_hilbert_delay_frames=215
# watterson_hilbert_delay_frames=215
# total_hilbert_delay_frames=430
engine_input_frame,instantaneous_offset_hz
0,-75
64,-74.995333333333337
...
```

Rows are emitted at frames 0, 64, 128, ... below `engine_input_frames`. The
frequency is `instantaneous_frequency()` evaluated at that absolute engine
input-frame coordinate. It is written with round-trip decimal precision. The
frequency-shift stage's analytic filter delays the source waveform by 215
frames. A Watterson channel adds its own 215-frame Hilbert delay, so
`total_hilbert_delay_frames` is 215 without Watterson and 430 with it. The
frame column remains the engine-input coordinate at the frequency-shift stage;
the delay metadata lets an analyzer align that modulation truth with source
content. If sample slip follows the shift, the scenario remains the authority
for the subsequent variable-length mapping.

Example:

```powershell
build\main\host-c\signal-lab\wfg_watterson_render.exe `
  --scenario case.json --source clean.wav --output shifted.wav `
  --frequency-truth-out frequency.csv
```

The driver rejects a frequency-truth request when no frequency-shift stage is
present and refuses path collisions between inputs and output artifacts.
Writing, flushing and closing the CSV are checked and reported as file errors.
Its fixed-stride writer retains only one record buffer, independent of render
duration.
