# Roadmap

The current tools generate PCM24 WAVs, render interference offline, play files
through Python/PortAudio on Windows, capture PC audio, and run an SD player and
live impairment engine on the RT1170. See [How to use](how-to-use.md) for the
supported workflows. The default build consumes supplied WAVs; optional
[encoder plugins](encoder-plugins.md) create additional waveform families.
A public example and [module/CDC tutorial](custom-encoder-tutorial.md) are available.

## Next useful improvements

- **Native Windows live output:** connect the C++ engine to an explicitly selected
  WASAPI endpoint. Report underruns, device changes and actual output format.
- **Interactive controls:** build a small front end around existing scenarios and
  live commands, with source selection, meters, headroom and visible errors.
- **Longer bench runs:** measure SD latency, FIFO margin, clock drift and recovery
  on the original RT1170 EVK and its connected analog path.
- **More example vectors:** add focused recipes that show a receiver's behavior
  under fades, noise bursts and combined interference.

These are future work, not implemented features or a delivery schedule. Keep
file generation and command-line use available independently of any future UI.
