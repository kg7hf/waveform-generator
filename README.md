# M110 RT1170 waveform generator

This directory is the standalone project root for the original
MIMXRT1170-EVK CM7 waveform-generator fixture. Its firmware will replay frozen
48 kHz mono PCM16 WAV files through the EVK WM8960 codec. The PC remains the
device-under-test host and owns analog capture, M110 receive decoding, BER, and
the retained run record.

Phase 1.0 freezes the local file, protocol, counter, capture, decoder, evidence,
and qualification contracts. Phase 1.1 establishes independent host checks and
RT1170 scaffold/tone cross-builds. The revised P1.2 is the smallest working
host-to-microSD-to-codec slice: expose the card through USB mass storage, use
CDC to hand ownership to firmware, play one fixed validated WAV once, then stop.
Long-duration buffering, WFG/1 control, capture orchestration, qualification
packaging, and the one-hour run follow after that vertical slice works. A build,
resident tone, or short card playback is engineering evidence only; it does not
prove audio integrity, interoperability, or formal M110 conformance.

## Standalone boundary

All source, headers, linker inputs, build scripts, tests, and materialized vendor
files used by this project live below this directory. CMake does not add or
search the parent M110 project. An installed compiler, CMake, build runner,
Python, operating-system SDK, and programmer/debugger are tools and may reside
outside this directory. Separately supplied M110 producer/decoder executables
and WAV packages are explicit runtime artifacts; they are never implicit build
dependencies.

The dependency initialization script accepts explicit clean source checkouts,
verifies their locked commits, and copies the selected source into
`third_party/`. It is a setup operation and is never called by CMake. A normal
configure or build uses only the materialized local copy and works offline.

## Current build targets

Open this directory as the VS Code root; from the parent checkout the explicit
entry point is:

```powershell
code tools/waveform-generator/waveform-generator.code-workspace
```

The checked-in workspace and `.vscode/settings.json` bind CMake Tools to
`${workspaceFolder}` and this project's presets. All commands below run from
this directory:

CMake 3.27 or newer is required so a clean, one-pass configure emits the File
API records used by the standalone source-boundary audit.

```powershell
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug

cmake --preset host-release
cmake --build --preset host-release --parallel
ctest --preset host-release

cmake --preset rt1170-debug
cmake --build --preset rt1170-debug --parallel
cmake --preset rt1170-release
cmake --build --preset rt1170-release --parallel

cmake --preset rt1170-tone-debug
cmake --build --preset rt1170-tone-debug --parallel
cmake --preset rt1170-tone-release
cmake --build --preset rt1170-tone-release --parallel

cmake --preset rt1170-player-debug
cmake --build --preset rt1170-player-debug --parallel
cmake --preset rt1170-player-release
cmake --build --preset rt1170-player-release --parallel
cmake --preset rt1170-player-readonly-release
cmake --build --preset rt1170-player-readonly-release --parallel
```

The `rt1170-*` scaffold compiles the original-EVK board, FreeRTOS, WM8960, and
TinyUSB mechanisms but leaves playback unavailable. The `rt1170-tone-*` image is
an optional resident-code tone checkpoint. The `rt1170-player-*` image is the
P1.2 composite CDC-plus-MSC microSD player. Its temporary CDC media commands hand
the card between the PC and firmware, and it reads `2:/WG/PLAY.WAV` once before
stopping. The normal player uses USB PID `0x4012` and permits host staging. The
`rt1170-player-readonly-*` inspection image uses PID `0x4013`, reports the LUN
write-protected, and rejects every WRITE10 before an SD write function can run.
It is suitable for inspecting an existing card without changing it. Both player
variants read FAT12/16/32 and exFAT through a read-only FatFs configuration. They
do not implement the full WFG/1 control state machine yet.

## Windows waveform capture

`waveform_capture.exe` records one explicit WinMM input as a streaming mono
48 kHz IEEE-float WAV. It selects the left channel by default; pass
`--channel right` or `--channel average` when the physical routing requires a
different policy. The callback only copies fixed-size blocks into a 512-block
bounded queue; a writer thread performs all disk I/O and patches the final
RIFF/data lengths after a controlled stop. READY and FINAL JSON Lines report the
selected policy and are written to stdout, with diagnostics and visible setup
failures on stderr.

For endpoints with system enhancements or APO processing, the same exact
friendly name can be selected through the event-driven raw WASAPI backend. It
uses the active endpoint's stable device ID, requests `AUDCLNT_STREAMOPTIONS_RAW`,
and accepts only the endpoint's actual 48 kHz PCM16 or IEEE-float mono/stereo
mix format; it does not silently resample.

Enumerate the current WinMM names before a run and select either the exact
native name or its numbered index:

```powershell
cmake --build --preset host-capture-release --parallel
build\host-release\host\capture\waveform_capture.exe --list-devices --json
build\host-release\host\capture\waveform_capture.exe --output capture.wav --device-name "Microphone (Realtek(R) Audio)" --sample-rate 48000 --channel left --max-seconds 4000 --stop-stdin
build\host-release\host\capture\waveform_capture.exe --backend wasapi-raw --output capture-raw.wav --device-name "Microphone (Realtek(R) Audio)" --sample-rate 48000 --channel left --max-seconds 4000 --stop-stdin
```

With `--stop-stdin`, send a line containing `STOP` for a controlled finish;
stdin EOF is recorded as `HOST_DISCONNECTED`. A duration limit, queue or write
error, non-finite sample, or interior partial WinMM buffer returns a visible
FINAL record and a nonzero exit status. Buffers returned by WinMM during reset
are drained and counted as `tail_samples`; those samples remain in the WAV.
The recorder does not perform a live capture during builds or tests.

The P1.0 standard-library checks reject duplicate/non-finite JSON, resolve every
local `$ref`, verify the recipes and their independent payload/tone arithmetic,
and check documentation links. A locked full Draft 2020-12 validation engine is
not materialized in P1.0; schema-engine validation and negative manifest/run
fixtures remain a P1.7 qualification-package acceptance item.

Use the boundary checker after a configure/build:

```powershell
$armGccRoot = Join-Path $env:USERPROFILE `
  '.mcuxpressotools\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi'
$cmakeRoot = Split-Path -Parent (Split-Path -Parent (Get-Command cmake).Source)
python scripts/verify-standalone.py --root . --build-dir build/rt1170-debug `
  --toolchain-root $armGccRoot --toolchain-root $cmakeRoot `
  --check-provenance --require-build-evidence
```

Each configure writes `build/<preset>/source-manifest.json`. The firmware build
identity includes the SHA-256 of that path-independent first-party manifest plus
the dependency-lock and import-ledger hashes; Git metadata is not required.

## P1.2 USB microSD staging

The early host utility is intentionally small. It can generate the frozen
10-second tone, reject a WAV outside the one supported format, write the frozen
WGM sidecar, copy both files to the fixed bring-up path, and verify the copied
hashes. It does not finalize qualification manifests or invoke an M110 producer
or decoder.

Run it from this project root. With the player image and card present, resolve
the fixture CDC endpoint by USB identity, set it to host mode, and wait for
Windows to assign the mass-storage volume. Replace the example COM port and
`R:\` with those explicitly identified endpoints; do not infer either from a
historical number or label.

```powershell
$fixturePort = 'COM15'
.\host\media-control.ps1 -Port $fixturePort -Action Host

python host/stage_wav.py make-tone --output build/staging/TONE10.WAV
python host/stage_wav.py inspect build/staging/TONE10.WAV
python host/stage_wav.py stage build/staging/TONE10.WAV --card-root R:\

# Flush and dismount R:\ through Windows before transferring card ownership.
.\host\media-control.ps1 -Port $fixturePort -Action Local -HostVolumeDismounted
.\host\media-control.ps1 -Port $fixturePort -Action Play
.\host\media-control.ps1 -Port $fixturePort -Action Status
```

The final two card files are `R:\WG\PLAY.WAV` and `R:\WG\PLAY.WGM`. The P1.2
host must flush and dismount `R:\` before sending `MEDIA LOCAL`, then send
`PLAY`. The helper requires `-HostVolumeDismounted` with `Local` so an accidental
command does not imply that CDC flushed Windows' filesystem cache. `STATUS`
reports media, player, SD-read, and codec/eDMA health counters. Firmware reads
the WAV once from microSD and treats the WGM as host-retained identity information;
full on-board sidecar/hash validation begins with WFG/1 ARM in P1.4. Never allow
Windows mass storage and firmware FatFs to own the card at the same time.
If `STATUS` shows the media as uninitialized or faulted after a card is inserted
or replaced, `Host` may be sent again while playback and FatFs are idle. The
firmware keeps the four-command P1.2 protocol: `OK MEDIA HOST` means the LUN is
already host-ready, while `OK MEDIA HOST INIT` means one initialization attempt
has started or is already in progress; a later `Status` reports the result.

The original EVK's physical card-detect signal is not usable on the tested board,
so the player deliberately assumes a card is present and lets `SD_CardInit`
establish whether it responds. Insert the card before reset. In this build,
`host_detect_status=1` records that configured assumption; it is not evidence
from a physical detect switch. The raw `cd_*` fields remain diagnostic only.
For a card that must not be changed, use the read-only preset and require both
Windows `IsReadOnly=True` and `STATUS msc_read_only=1`; do not run `stage_wav.py`.

The complete extraction gate copies this directory to an unrelated empty
location without Git metadata or prior build caches and repeats both host and
RT1170 Debug/Release builds with network access unnecessary. The command and
retained evidence are specified in [the local Phase 1 runbook](docs/phase1-implementation.md).

## Project records

- [Phase 1 implementation and qualification contract](docs/phase1-implementation.md)
- [First-party source import ledger](docs/source-imports.json)
- [Third-party dependency lock](dependencies.lock.json)
- [Third-party notices and licensing boundary](THIRD_PARTY_NOTICES.md)
- [Fixture recipes](fixtures/README.md)
- [Frozen JSON schemas](schema/)

Local build trees, virtual environments, and working run artifacts are ignored.
Evidence selected for a governed project record must be copied to its declared
retention location with hashes; an ignored `artifacts/` directory by itself is
not a sealed record.

PathSim remains an external preparation tool. No GPL PathSim source is imported
or linked here. A PathSim-rendered WAV is replayed directly as one test arm.
HFSimulator is a separate physical-channel arm. Combining the two impairments is
valid only for an explicitly declared cascaded-impairment experiment.
