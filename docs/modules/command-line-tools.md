# Command-line Tools Reference

## 1. Scope

This is the operator-facing reference for the project-owned Python, PowerShell,
and host executable entry points. It records the current options and safe command
sequences. Run each Python tool with `--help` for the parser's live wording.

## 2. 50,000-foot view

The tools form five workflows:

| Workflow | Tools |
|---|---|
| Make clean audio | `stage_wav.py make-tone`, optional plugin tools |
| Add impairments | `render_scenario.py`, `signal_lab_render.exe` |
| Build a campaign | `corpus.py`, `m110_score.py` |
| Operate RT1170 | `media-control.ps1`, `live_control.py`, `tx_upload.py`, `stage_wav.py` |
| Capture and inspect analog audio | `waveform_capture.exe`, `capture-session.ps1`, `analyze_capture.py` |

## 3. How to use it

### `host/render_scenario.py`

```text
--scenario PATH   Required scenario JSON
--output PATH     Required output WAV; sidecar uses the same stem
--base-dir DIR    Resolve relative source paths here; default is scenario directory
--overwrite       Permit replacing output artifacts
```

### `signal_lab_render.exe`

```text
--scenario PATH          Static scenario; optional only with --live-events
--source PATH            Required mono PCM24 48 kHz source WAV; PCM16 import is legacy-only
--output PATH            Required output WAV
--sidecar PATH           Optional explicit result sidecar
--target-scenario PATH   Write scenario with effective reference_rms for RT1170
--reference-rms VALUE    Finite positive external reference override
--block N                Processing block, 1 through 2048
--live-events PATH       Apply a wfg-live.replay/1 plan
```

### `host/corpus.py`

```text
--spec PATH
--root DIR
--tx EXE
--decoder EXE
--engines LIST
--stage all|refs|validate|render|score|summary|verify|plan
--only-modes CSV
--jobs N
--overwrite
```

### `host/m110_score.py`

```text
positional WAV...        One or more WAVs
--decoder EXE            Required decoder
--engine siso|adaptive
--expect-file PATH       Override sidecar payload for one WAV
--jobs N
--extra STRING           Additional decoder arguments
--summary PATH           Write PATH.csv and PATH.json
```

### `host/stage_wav.py`

```text
stage_wav.py inspect WAV
stage_wav.py make-tone --output PATH [--recipe PATH] [--force]
stage_wav.py stage WAV --card-root ROOT [--level-percent N] [--force]
```

`stage` validates the fixed mono packed-PCM24 48 kHz contract and writes the fixed
`/WG/PLAY.WAV` bring-up path plus its retained metadata.

### `host/live_control.py`

Global hardware options:

```text
--port COMN
--expected-serial SERIAL
--record NEW.jsonl
--timeout SECONDS
--host-volume-dismounted
```

Subcommands:

```text
command "COMMAND"...    Send one or more validated commands
interactive             Read commands until EOF or QUIT
export JOURNAL OUTPUT   Create wfg-live.replay/1 offline
replay PLAN             Replay an exported plan with a fresh run ID
```

Important commands include `INFO?`, `STATUS?`, `COUNTERS?`, `MEDIA HOST`,
`MEDIA LOCAL`, `LOAD:NAME.WAV`, `PLAY`, `STOP`, `SEED:N`, `REFERENCE:R`, four-slot
CW controls, static controls, immediate fades, and stepped sweeps. See
[Live Control and Replay](live-control-and-replay.md) for semantics.

### Custom waveform generation over CDC

`live_control.py ... command "ENCODE ID PROFILE INPUT.BIN OUTPUT.WAV"` requests
file generation from a plugin advertised by `INFO?`. The default build has none.
Profiles are module-defined; the public `TONE` example uses a frequency in Hz.
See [the module/CDC tutorial](../custom-encoder-tutorial.md) for wire limits,
custom profiles and parser extension points.

### `host/tx_upload.py`

Requires an enabled plugin that explicitly supports the legacy rate/interleave
protocol. The default build returns `ENCODERS_DISABLED`. For other plugins use
the generic `ENCODE ID PROFILE INPUT.BIN OUTPUT.WAV` live command.

```text
--port COMN
--expected-serial SERIAL
--output NAME.WAV
--mode 75S|75L|150S|150L|300S|300L|600S|600L|1200S|1200L|2400S|2400L|4800U
--payload PATH | --sd-input NAME.BIN
--record NEW.jsonl
--reset
--timeout SECONDS
--generation-timeout SECONDS
```

Use `--reset` only to explicitly abandon a known partial upload/generation. It is
not a generic retry switch.

### `waveform_capture.exe`

```text
--backend winmm|wasapi-raw
--list-devices [--json]
--output PATH
--device-index N | --device-name EXACT
--sample-rate 48000
--channel left|right|average
--max-seconds N
--stop-stdin
```

### `host/capture-session.ps1`

```text
-Output PATH
-DeviceName EXACT
-StopFile PATH
-JsonlPath PATH
-Backend wasapi-raw|winmm
-Channel left|right|average
-MaxSeconds N
-Recorder PATH
```

### `host/analyze_capture.py`

```text
positional CAPTURE
--output-json PATH
--source PATH
--zero-run-samples N
--constant-run-samples N
--dropout-level VALUE
--correlation-window-seconds VALUE
--correlation-search-seconds VALUE
```

### `host/media-control.ps1`

```text
-Port COMN
-Action Status|Host|Local|Play
-TimeoutMilliseconds N
-HostVolumeDismounted
```

The switch is valid only with `-Action Local` and is an operator assertion, not a
filesystem operation.

## 4. Scientist and maintainer view

CLI parsers are part of the experimental contract. Required paths must be explicit;
hardware identity must be checked; existing evidence files must not be silently
overwritten; finite numeric values must be validated before narrowing; and an
uncertain mutation must not be automatically retried.

Exit success means the tool completed its own contract. It does not elevate the
evidence layer. For example, `signal_lab_render` success establishes an output
artifact, `live_control` success establishes an acknowledgment, and capture success
establishes a finalized recording. None alone establishes correct modem decode.

When changing options, update parser tests, this reference, root examples, replay
or sidecar schemas, and any scripts that invoke the old form. Preserve a compatibility
alias only when its semantics remain unambiguous.

## 5. 5th-grader view

Command-line options are labels on laboratory switches. `--source` says which
recording enters the machine. `--output` says where the new recording goes.
`--seed` chooses the repeatable random pattern. `--expected-serial` checks that you
are turning knobs on the intended board instead of another USB device.

The program should stop when a required label is missing rather than guessing.

## 6. Toy worked example

This small offline experiment has an explicit input, recipe, output, and report:

```powershell
python host/stage_wav.py make-tone --output clean.wav

build\host-release\host\render\signal_lab_render.exe `
  --scenario cw-case.json --source clean.wav --output impaired.wav `
  --sidecar impaired.json --block 256

python host/stage_wav.py inspect impaired.wav
```

Changing `--block 256` to another legal block size should not change the impaired
PCM digest. Changing the scenario seed is expected to change random impairments.

## 7. Common operating mistakes

| Mistake | Correct behavior |
|---|---|
| Reusing a historical COM port | Enumerate and match VID/PID/serial |
| Sending `MEDIA LOCAL` while Windows still mounts the volume | Flush and dismount first, then assert the handoff |
| Reusing an old journal filename | Create a new retained journal |
| Retrying a timed-out mutation | Reconnect and inspect state |
| Treating `--overwrite` as harmless | Use only when intentionally replacing derived artifacts |
| Calling a deterministic seed a statistical campaign | Run and identify the required realization set |

## 8. Demonstration scripts

[How to use](../how-to-use.md) documents `scripts/demo-waveform.ps1` and
`scripts/demo-download.ps1`. Both render clean/CW/mixed PCM24 artifacts and take
`-Play none|clean|cw|field`, an explicit `-Device`, and a new `-OutputDirectory`.
The first accepts optional `-SourceWav`; the second downloads a sample
and records its explicit conversion before invoking the first.

`host/play_wav.py WAV --device EXACT --gain-db -12` plays a retained result;
`--list-devices` enumerates outputs without playback. `host/prepare_demo_wav.py
SOURCE OUTPUT --seconds 10` performs the documented demonstration conversion.
These are host conveniences, not embedded audio callbacks or qualified signal
conditioning tools.
