# How to use the waveform generator

The normal starting point is a supplied **mono, 48 kHz, packed PCM24 WAV**.
Render an impaired WAV on the PC, play that result on a chosen Windows output,
or stage the source on an RT1170/RT1064 SD card and apply a scenario and live
controls there. An encoder plugin is optional. The examples below run from the
repository root in PowerShell.

## What is implemented

| Capability | Where it runs | Entry point and details |
|---|---|---|
| Generate a clean calibration tone | PC | `host/stage_wav.py make-tone`; no encoder plugin |
| Import a supplied WAV | PC, then either board | `stage_wav.py inspect` checks the board's mono PCM24/48 kHz contract; `prepare_demo_wav.py` explicitly converts a demo source |
| Render a fixed impairment recipe | PC and either board | `signal_lab_render` and the shared C++ Signal Lab engine; ordered scenario stages and a repeatable seed |
| Play a rendered file on Windows | PC | `host/play_wav.py` selects one exact WASAPI output and duplicates mono into stereo; render effects before playback |
| Play a card file | RT1170 and RT1064 | SD player with WM8960 codec or USB audio output; the PC records the USB route as an input device |
| Change effects during a play | RT1170 and RT1064 | WFG-LIVE/1 CDC commands for four CW slots, seeded static, fades and stepped sweeps; accepted controls can be replayed offline |
| Prepare a new impaired card WAV | Writable player images | `PREPARE` renders and verifies a new file; inspect `PREPARE?` before using it |
| Schedule a test battery | RT1170 and RT1064 | On-card `PLAYLIST` with gaps, route, scenario, seed, attenuation, duration and CUT choices per item |
| Capture and compare output | Windows | `waveform_capture` (WinMM or WASAPI raw), `analyze_capture.py`, `make_playlist.py` and board counters |
| Generate WAVs from payload bytes | Optional build | User-supplied static encoder plugin; default build advertises no encoder |

The C++ scenario engine supports `cw`, `awgn`, `impulse`, `fade`,
`sample_slip`, `watterson`, `frequency_shift` and `second_source`. A CW stage
can also use FSK, pulsing, a swept frequency, Markov gating and fading; the
second source can have its own fading and gate. See the
[scenario engine](modules/signal-lab-engine.md) and its linked stage guides for
the exact JSON fields, limits, order and numerical conventions. The Python
reference renderer supports a smaller set; use the C++ renderer when matching
the board's engine. `sample_slip` changes the rendered stream, whereas the
board's live `CUT` command inserts zeros or drops output frames after the
scenario engine. Neither models an actual USB timing gap.

The RT1064 codec path is built but analog operation has not been qualified.
Native live Windows output, a graphical effects studio and a Windows virtual
sound-card driver are not implemented. The [roadmap](roadmap.md) tracks those
ideas; the [feature-status matrix](feature-status.md) separates complete,
partial/experimental and future work. The commands here use implemented file
and USB paths.

## Choose a workflow

| Use case | Start here | Result |
|---|---|---|
| Hear CW over a known source on any suitable PC output | [Demo 1](#demo-1-generate-a-waveform-add-cw-and-a-mixed-channel) | Clean, CW and mixed PCM24 WAVs plus reports |
| Start from a downloaded example | [Demo 2](#demo-2-download-a-wav-convert-it-add-effects-and-play) | Original, explicit conversion and derived vectors |
| Use a real recording or modem waveform | [Demo 1 with `-SourceWav`](#demo-1-generate-a-waveform-add-cw-and-a-mixed-channel) | Its validated source and effect variants |
| Explore noise, fading, impulses or time faults | [Change the effects](#change-the-effects) | A scenario-controlled offline render |
| Feed a second PC's decoder through USB | [Board workflow](#take-the-result-to-the-board-or-a-decoder) | Selected SD file streamed through the board's USB input endpoint |
| Add or sweep CW during a board play | [CDC workflow](#control-a-board-over-cdc) | Sample-indexed controls and a replayable journal |
| Repeat a multi-item test battery | [PLAYLIST](live-control-protocol.md#playlist) | One result block per item and an aligned capture |
| Supply a new waveform family | [Optional waveform generators](#optional-waveform-generators) | A plugin-created WAV entering the same effects/player path |

## Interfaces and command switches

On a board, three USB functions have different jobs. They are not a TCP service
or a virtual sound card installed on Windows:

| Interface | Direction and purpose | Operator entry point |
|---|---|---|
| CDC serial, `WFG-LIVE/1` | PC sends commands; board returns JSON acknowledgments, state and counters | `host/usb_bench.py control` or `host/live_control.py` |
| USB mass storage | PC stages WAV, scenario, playlist and optional payload files while it owns the card; firmware reads them after the handoff | `host/usb_bench.py stage`, `host/media_eject.py`, `MEDIA HOST`/`MEDIA LOCAL` |
| USB Audio Class 2 input on the PC | Board streams its played PCM to a recording application; select the board's exact input endpoint and 48 kHz stereo 24-bit format | `waveform_capture.exe` / `host/usb_bench.py capture` |
| Optional DD-008 upload mode on CDC | Upload payload bytes for a legacy-compatible encoder; close and reopen the port when switching modes | `host/tx_upload.py` (only with a compatible plugin) |
| WM8960 analog codec | Board output through its physical audio connector | `OUTPUT CODEC`; RT1064 analog qualification is still separate |

The [media/artifact guide](modules/protocols-media-and-artifacts.md) explains
ownership and the optional binary upload framing. The
[live protocol](live-control-protocol.md) defines CDC framing and responses;
[USB sound-card bring-up](usb-sound-card-bringup.md) explains capture format and
continuity counters. Use the exact physical endpoint on each PC, since Windows
can renumber COM ports and audio devices.

The [command-line reference](modules/command-line-tools.md) lists each
operator-facing executable/script and its switches. The most used commands are:

| Entry point | Primary switches/subcommands |
|---|---|
| `scripts/demo-waveform.ps1` | `-OutputDirectory`, `-SourceWav`, `-Play none\|clean\|cw\|field`, `-Device` |
| `scripts/demo-download.ps1` | `-OutputDirectory`, `-Play none\|clean\|cw\|field`, `-Device` |
| `host/stage_wav.py` | `inspect WAV`; `make-tone --output [--recipe] [--force]`; `stage WAV --card-root [--level-percent] [--force]` |
| `signal_lab_render.exe` | `--source`, `--scenario`, `--output`, `--sidecar`, `--target-scenario`, `--reference-rms`, `--seed`, `--block`, `--live-events` |
| `host/play_wav.py` | `--list-devices` or `WAV --device EXACT [--gain-db DB]` |
| `host/usb_bench.py` | `devices`, `check`, `control`, `stage`, `capture`, with `--profile` for fixture operations |
| `host/live_control.py` | `--port`, `--expected-serial`, `--expected-board`, `--record`; `command`, `prepare`, `prepare-status`, `playlist`, `files`, `export`, `replay` and other specialist subcommands |
| `waveform_capture.exe` | `--backend`, `--device-name`/`--device-index`, `--output`, `--sample-rate`, `--channel`, `--max-seconds`, `--stop-stdin` |

Run a Python entry point with `--help` (and a subcommand with its own
`--help`) for its live parser. The [CLI reference](modules/command-line-tools.md)
also covers CUT prediction, media ejection, playlists and
capture analysis. The [WFG-LIVE/1 reference](live-control-protocol.md) defines
every CDC request, response, error, media handoff and measurement field.

The first two demos create mono PCM24 files, mix interference, and optionally
play a chosen file through an explicitly named Windows output. They do not
flash firmware or operate a board. Any Windows WASAPI output device that
accepts 48 kHz stereo playback can be used; the tools are not tied to a
sound-card brand.

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
`REPLACE_WITH_EXACT_OUTPUT_DEVICE_NAME`; if it is absent, choose a name from your output
list. Playback gain is -12 dB. A demo without `-Play` writes files only.

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
  -Play cw -Device "REPLACE_WITH_EXACT_OUTPUT_DEVICE_NAME"
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
  -Device "REPLACE_WITH_EXACT_OUTPUT_DEVICE_NAME"
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
  -Play cw -Device "REPLACE_WITH_EXACT_OUTPUT_DEVICE_NAME"
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
  --device "REPLACE_WITH_EXACT_OUTPUT_DEVICE_NAME" --gain-db -12
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
# Use fresh directory names; the demo script refuses to overwrite its output.
.\scripts\demo-waveform.ps1 -OutputDirectory build/demo-python-source
New-Item -ItemType Directory -Path build/demo-python-render
$scenario = Get-Content examples/cw-900hz.json -Raw | ConvertFrom-Json
$scenario.source.path = 'build/demo-python-source/clean.wav'
$json = $scenario | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText('build/demo-python-render/cw.json', $json, [Text.UTF8Encoding]::new($false))
python host/render_scenario.py --scenario build/demo-python-render/cw.json `
  --base-dir . --output build/demo-python-render/python-cw.wav
```

The edited recipe points to the source WAV created above. Python and C++ share
the WAV format, but their random generators
and arithmetic can differ, so cross-implementation noise hashes need not match.
Use C++ for reproducing the shared RT1170/RT1064 firmware engine.

## Take the result to the board or a decoder

For either RT1170 or RT1064, use the [Windows USB bench guide](windows-usb-bench.md)
for fixture selection, serial-bound staging, card handoff and playback. The
[RT1170 guide](modules/rt1170-player-and-audio.md) and
[RT1064 guide](modules/rt1064-board.md) cover board details. Do not apply an old
on-card scenario to an already mixed file unless you intend a second processing
pass.

[Capture](modules/host-capture-and-analysis.md) and
[live replay](modules/live-control-and-replay.md) have separate guides.
For repeatable experiments, keep the source and recipe. [Testing](testing.md)
describes the evidence and acceptance boundaries; audible playback, exact-byte
decoding and hardware timing establish different results.

## Record an input device

Use the existing capture helper while playing the chosen source/board waveform:

```powershell
.\host\capture-session.ps1 -Output build/return.wav `
  -DeviceName "REPLACE_WITH_EXACT_INPUT_DEVICE_NAME" -Backend wasapi-raw `
  -StopFile build/return.stop -JsonlPath build/return.jsonl
# From another PowerShell window, finish and close the WAV:
New-Item -ItemType File build/return.stop
```

This records the selected input. The demo scripts do not configure board
pass-through or change firmware. Capture uses IEEE-float WAV for analysis;
playback and test-vector generation use PCM24.

## Control a board over CDC

Both boards expose a USB CDC control port using `WFG-LIVE/1`: one line per
request, `<sequence> <command> LF`, and one JSON reply with the same `seq`.
Use `host/usb_bench.py` with a machine-local fixture profile for a guided
session, or `host/live_control.py` with an explicit COM port, expected USB
serial, **board model** and a new journal path. The latter defaults to
`MIMXRT1170-EVK`, so supply `--expected-board MIMXRT1064-EVK` for an RT1064.
The controller checks `INFO?` before sending a command; a remembered COM
number or a matching audio name alone is not board identity.

| Command family | What it does |
|---|---|
| `INFO?`, `STATUS?`, `COUNTERS?`, `USB?`, `ENGINE?`, `FILES?` | Discover capabilities, progress, continuity and card contents; `FILES?` needs local card ownership and an idle player |
| `MEDIA HOST`, `MEDIA LOCAL` | Give the card to Windows for staging, then return it to firmware after a verified flush/dismount/eject |
| `OUTPUT USB` / `OUTPUT CODEC`, `LOAD:NAME.WAV`, `PLAY`, `STOP` | Select route and file, start once, and stop; the USB route needs an already open capture stream |
| `SEED:N`, `REFERENCE:R`, `CW ...`, `STATIC ...`, `FADE NOW ...`, `SWEEP ...` | Configure repeatable live interference; prefix a live command with `AT:<output_frame>` for an exact generated-frame position |
| `CUT:...`, `CUT` | Drop generated frames or insert zeros on the next play, or clear its schedule |
| `PREPARE:...`, `PREPARE?` | Render a scenario to a new WAV on a writable card and check its final result |
| `PLAYLIST:NAME`, `PLAYLIST?`, `PLAYLIST ACK:SEQ` | Run a saved battery and retain/acknowledge per-item result blocks |
| `ENCODE ID PROFILE INPUT.BIN OUTPUT.WAV` | Ask an optional installed encoder to create a source WAV; absent in the default build |

For example, once a valid `TEST01.WAV` has been staged and the capture stream
is already open and stable, a board session can schedule CW from frame 96,000
through frame 336,000 (2–7 seconds at 48 kHz):

```powershell
$fixture = 'config/rt1064-profile.json' # or a verified RT1170 profile
$runs = 'build/usb-runs'                # new logs per operation
python host/usb_bench.py --profile $fixture --runs-root $runs control `
  'OUTPUT USB' 'LOAD:TEST01.WAV' 'SEED:42' `
  'CW FREQ 900' 'CW CI 6' 'AT:96000 CW ON' 'AT:336000 CW OFF' 'PLAY'
```

The [bench guide](windows-usb-bench.md#stage-and-play) gives the full
staging/capture sequence and a known-tone reference value; do not copy its
`REFERENCE` value to an unrelated WAV. `CI` is desired-source RMS divided
by CW RMS in dB: a smaller value makes the tone stronger. To use an arbitrary
source that begins with silence, measure its intended RMS and set
`REFERENCE:R` before enabling live interference. The board otherwise
measures an initial source interval when no scenario reference is supplied.

The wire reference specifies [framing and command replies](live-control-protocol.md#engineering-protocol),
[live controls and their bounds](live-control-protocol.md#live-commands),
[card handoff](modules/protocols-media-and-artifacts.md#card-ownership), and
[PLAYLIST syntax/results](live-control-protocol.md#playlist). A successful
control acknowledgment means the command was admitted; read `STATUS?` and
`USB?` after playback to check completion and continuity. Export the saved
journal with `live_control.py export` and use `signal_lab_render --live-events`
for offline replay as described in [Live Control and Replay](modules/live-control-and-replay.md).

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
