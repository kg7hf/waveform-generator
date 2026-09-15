# How to use the waveform generator

This guide starts with two short demos. They create mono **48 kHz PCM24** files,
mix interference, and optionally play a chosen file through an explicitly named
Windows output. The current bench is hard-patched between the RT1170 codec
and the Realtek headphone/microphone ports: headphone playback feeds the rig and
the microphone input captures its return. Run them from PowerShell after the host build. They never flash
firmware or operate the RT1170.

For a shorter first run, see [Quick start](quickstart.md). To add a producer or
custom CDC controls, see the [module tutorial](custom-encoder-tutorial.md).

## Prerequisites

From the repository root:

```powershell
.\scripts\init-dependencies.ps1
cmake --preset host-release
cmake --build --preset host-release --parallel
python -m pip install -r host/requirements-playback.txt
python host/play_wav.py --list-devices
```

Use the exact current output name. The examples use
`Headphones (Realtek(R) Audio)`; if it is absent, choose a name from your output
list. Playback gain is âˆ’12 dB. A demo without `-Play` writes files only.

## Demo 1: generate a waveform, add CW and a mixed channel

[`scripts/demo-waveform.ps1`](../scripts/demo-waveform.ps1) generates a clean
ten-second 1 kHz calibration tone, requiring no encoder plugin. It then
renders two derivatives: a 900 Hz CW interferer at C/I 6 dB, and a fade + AWGN +
CW recipe with source headroom.

```powershell
# Create artifacts without playing audio:
.\scripts\demo-waveform.ps1

# Use a different output directory for another run and hear the CW mixture:
.\scripts\demo-waveform.ps1 -OutputDirectory build/demo-listen `
  -Play cw -Device "Headphones (Realtek(R) Audio)"
```

Each run requires a new output directory and produces:

| File | Meaning |
|---|---|
| `clean.wav` | Clean 48 kHz mono PCM24 source |
| `cw.wav`, `cw.json` | CW mixture and render report |
| `field.wav`, `field.json` | Mixed interference and render report |

Select `-Play clean`, `-Play cw`, or `-Play field`. Ctrl+C stops the playback.
Inspect `clipped_samples` in the reports. The recipes reject clipping so an
unacceptable level cannot silently become an accepted vector.

To use your own already-compatible WAV:

```powershell
.\scripts\demo-waveform.ps1 -SourceWav 'C:\Audio\my-mono-48k-pcm24.wav' `
  -OutputDirectory build/demo-my-audio -Play field `
  -Device "Headphones (Realtek(R) Audio)"
```

The script validates it using the same staging inspection as the target, then
copies it into the run directory. It does not resample this path.

## Demo 2: download a WAV, convert it, add effects and play

[`scripts/demo-download.ps1`](../scripts/demo-download.ps1) downloads a small
[CPython audio test sample](https://github.com/python/cpython/blob/v3.13.0/Lib/test/audiodata/pluck-pcm16.wav)
from an upstream release. There is no SHA pin or download approval step. The
upstream sample is stereo PCM16 at 11,025 Hz; this fact is recorded rather than
hidden. Its upstream [license](https://github.com/python/cpython/blob/v3.13.0/LICENSE)
and download URL are retained in `download.json`.

```powershell
.\scripts\demo-download.ps1 -OutputDirectory build/demo-pluck `
  -Play cw -Device "Headphones (Realtek(R) Audio)"
```

The script retains the downloaded original and invokes
[`host/prepare_demo_wav.py`](../host/prepare_demo_wav.py) to average channels,
linearly interpolate to 48 kHz, repeat/trim to ten seconds, leave peak headroom,
and write packed PCM24. `canonical.conversion.json` records the
original format and operations. The `mixes` subdirectory holds clean, CW and
mixed-channel WAVs and reports from Demo 1.

This conversion is an audible demonstration. Linear interpolation and repetition
change the signal; they are not a calibrated resampler, a modem reference, or a
way to recover precision missing in a PCM16 source. Use native PCM24 source
artifacts for quantitative tests.

Without network access, Demo 1 still works. A download failure or unsupported
WAV stops Demo 2 before playback. Keep the failed directory for
diagnosis and choose a new directory on retry.

## Change the effects

Edit a copy of one of the [example scenarios](../examples/cw-900hz.json), then
run the portable renderer directly:

```powershell
build\host-release\host\render\signal_lab_render.exe `
  --source build/demo-generated/clean.wav --scenario examples/impulse-100hz.json `
  --output build/demo-generated/impulse.wav --sidecar build/demo-generated/impulse.json
python host/play_wav.py build/demo-generated/impulse.wav `
  --device "Headphones (Realtek(R) Audio)" --gain-db -12
```

| Parameter | Effect |
|---|---|
| CW `frequency_hz` | Moves the continuous tone |
| CW `ci_db` | Lower values make interference stronger relative to the source reference RMS |
| AWGN `snr_db` | Lower values add stronger band-limited noise |
| Fade `depth_db`, `duration_ms`, `period_seconds` | Set attenuation, length and repetition |
| Impulse `peak_db`, `decay_ms`, `period_seconds` | Set peak, decay and regular event spacing |
| `source_gain_db` | Scales source and its reference before stages; use explicit headroom |
| `seed` | Selects a repeatable random realization within that implementation |

Use the [Python renderer](../host/render_scenario.py) when you need its reference
implementation. It now also reads/writes PCM24:

```powershell
python host/render_scenario.py --scenario examples/cw-900hz.json `
  --base-dir . --output build/demo/python-cw.wav
```

That example uses the recipe's `source.path`, `build/demo/clean.wav`, created by
the [README quick start](../README.md). To use a demo script output, change
`source.path` in a copied recipe. Python and C++ now share the WAV format; their
different random generators and arithmetic still mean that cross-implementation
noise hashes need not match. Use C++ for reproducing the RT1170 engine.

## Take the result to the board or a decoder

The [README](../README.md#5-play-through-rt1170-and-add-live-interference) explains
staging `/WG/PLAY.WAV`, exporting `PLAY.SCN`, handing card ownership to firmware,
and recording live controls. Do not apply an old on-card scenario to an already
mixed file unless you intend a second processing pass.

[Capture](modules/host-capture-and-analysis.md), [live replay](modules/live-control-and-replay.md)
and [corpus/scoring](modules/corpus-rendering-and-scoring.md) have separate guides.
For repeatable experiments, keep the source and recipe; reports and measurements
are available when useful. There is no controlled-deliverable requirement.
Audible playback is useful operational evidence; exact-byte decoding and hardware
timing require their own measurements.

## Record the hard-patched Realtek return

Use the existing capture helper while playing the chosen source/board waveform:

```powershell
.\host\capture-session.ps1 -Output build/return.wav `
  -DeviceName "Microphone (Realtek(R) Audio)" -Backend wasapi-raw `
  -StopFile build/return.stop -JsonlPath build/return.jsonl
# From another PowerShell window, finish and close the WAV:
New-Item -ItemType File build/return.stop
```

This captures the existing physical return; the demo scripts do not configure
RT1170 pass-through or change its firmware. Board file playback and PC-source
playback are separate source choices. Capture uses IEEE-float WAV for analysis;
playback/test-vector generation uses PCM24.

## Python setup and test cleanup

Install only the helpers needed for your workflow:

| Workflow | Install command |
|---|---|
| Python rendering | `python -m pip install -r host/requirements-render.txt` |
| Host test suite | `python -m pip install -r host/requirements-test.txt` |
| PC WAV playback | `python -m pip install -r host/requirements-playback.txt` |
| Physical USB control | `python -m pip install -r host/requirements-control.txt` |

After configuring and building `host-release`, run `.\scripts\test.ps1`.
Tests remove their temporary audio files and this wrapper removes CTest logs.
For the reusable tone fixture recipe, see [Fixtures](../fixtures/README.md).
Future native PC live output and UI work is summarized in the [Roadmap](roadmap.md).

## Optional waveform generators

The normal build needs only a source WAV. An optional encoder module can create
that WAV from payload bytes; playback and effects use the same path afterward.
See [Encoder plugins](encoder-plugins.md) for build options, the public `TONE`
example, and keeping a separate module as an optional submodule. Enabling a module
does not start playback or install a Windows live audio backend.
