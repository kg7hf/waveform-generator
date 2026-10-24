# Windows USB bench: RT1170 or RT1064

Use the same native Windows capture tool and WFG-LIVE/1 controller with either
board. The PC receives samples over USB; SSH can launch local commands and
retrieve results. Linux CI containers continue to provide software checks.
Physical USB capture runs on Windows, independently of those containers.

## Software and fixture selection

Install Python 3.10 or newer and the control requirements in a virtual
environment. Build `host-release`, or deploy the matching source and native
`waveform_capture.exe` / `signal_lab_render.exe` binaries with their runtime
DLLs in `bin/`. No compiler is needed for a runtime deployment. The launcher
uses `bin/` first, then the normal `build/host-release` capture location.

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r host/requirements-control.txt -r host/requirements-render.txt
.\.venv\Scripts\python.exe host/usb_bench.py devices
```

Create one machine-local JSON profile per physical board, outside source control:

```json
{
  "board": "MIMXRT1064-EVK",
  "serial": "REPLACE_WITH_USB_SERIAL",
  "capture_name": null
}
```

Replace the placeholder with the actual alphanumeric USB serial. The other
supported board name is `MIMXRT1170-EVK`. Keep `capture_name` null until the board
is connected and its physical recording endpoint has been associated with
that serial. Then use its exact name, such as `Line (RT1064 Player)`; Windows
may include a number. Names and COM numbers can differ between PCs.

The launcher discovers only player CDC ports with the selected serial, then
checks the board model and serial through INFO before operating. It refuses
missing or duplicate ports. It also refuses unset, missing or duplicate audio
names. **An exact audio name is an operator association, not proof of USB serial
mapping.** If two identical fixtures have indistinguishable audio names, give
their recording endpoints distinct names containing RT1170 or RT1064 and
update their profiles. Never use a default device or Remote Audio.

## Connect and check

Retain the programmed firmware and SD card. Connect the target USB data port
and the board's existing power/debug arrangement. The RT1064 uses J9 for
target USB and J41 for debug. Both accept Micro-B plugs; USB-C adapters can
reuse existing USB-A-to-Micro-B data cables.

From the deployed source root:

```powershell
$python = Join-Path $PWD '.venv\Scripts\python.exe'
$fixture = Join-Path $PWD 'config\rt1064-profile.json'
$runs = Join-Path $PWD 'build\usb-runs'
& $python host/usb_bench.py --profile $fixture --runs-root $runs check
```

Choose a machine-local fixture profile and a results directory that is not
under version control. Each operation prints its unique run directory and saves
the selected fixture, protocol journal, stdout and stderr there. Existing
runs are retained. A missing board returns nonzero without attempting another
port. The same commands work with an RT1170 profile.

The owner selects **2 channel, 24 bit, 48000 Hz** in the recording endpoint's
Windows Properties / Advanced tab, then closes Sound settings. When capture
is running, `USB?` must report alternate 1, 24 bits and two channels. A float32
WASAPI mix format alone does not establish USB precision. This setup does not
change default devices, audio format, power settings, firmware or card layout.

## Stage and play

Generate the repository's standard ten-second, 1 kHz PCM24 tone, then stage
that exact file as `TEST01.WAV`. Use a new vector directory for each run:

```powershell
$vectors = Join-Path $PWD 'build\usb-bench-vector'
New-Item -ItemType Directory -Path $vectors
$tone = Join-Path $vectors 'clean.wav'
& $python host/stage_wav.py make-tone --output $tone
if ($LASTEXITCODE -ne 0) { throw 'Tone generation failed.' }
```

Before operating hardware, ensure no other test or application owns the fixture;
this launcher does not reserve hardware or coordinate other test systems.

```powershell
& $python host/usb_bench.py --profile $fixture --runs-root $runs control 'MEDIA HOST'
& $python host/usb_bench.py --profile $fixture --runs-root $runs stage $tone --name TEST01.WAV
```

The card must be host-owned and mounted before `stage`; allow Windows to mount
it after `MEDIA HOST`. Use an unused upper-case 8.3 filename. The staging helper
verifies the copy, flushes and reads it back, locks/dismounts/ejects the medium,
then sends `MEDIA LOCAL` only after successful staging. Failure stops the
handoff. There is no formatting, overwrite, UAC or reset fallback. Windows access depends on local permissions and device policy.

Start a bounded capture in one terminal or SSH session:

```powershell
& $python host/usb_bench.py --profile $fixture --runs-root $runs capture --seconds 30
```

It records the profile's exact physical endpoint to `capture.wav`, using the
left channel by default (`--channel right` is also supported). Wait for the
`READY` record in its `capture.stdout.log`, then query `USB?` twice, at least
three seconds apart. Before playback, confirm `uac_stream_alt` is 1,
`uac_stream_bits` is 24, `uac_stream_channels` is 2, and
`uac_missed_intervals` has not changed between queries. This confirms the USB
format and that the stream has settled after the capture client opened.

In a second terminal:

```powershell
& $python host/usb_bench.py --profile $fixture --runs-root $runs control 'USB?'
# Check its journal for 24-bit stereo before starting playback.
& $python host/usb_bench.py --profile $fixture --runs-root $runs control 'OUTPUT USB' 'LOAD:TEST01.WAV' 'PLAY'
```

Capture stops after the requested wall-clock duration (1..3600 seconds). The
launcher allows 30 additional seconds for native cleanup, then fails on timeout;
an interrupted capture may be incomplete. It does not stop board playback, so
choose a capture longer than the source and use `control 'STOP'` if necessary.
The native recorder reports its intentional duration limit with exit code 1;
the launcher treats that as a completed operation only when the READY and FINAL
records match, all delivered samples were written, and a nonempty WAV exists.

For a second capture of the standard ten-second tone with scheduled CW:

```powershell
& $python host/usb_bench.py --profile $fixture --runs-root $runs control `
  'OUTPUT USB' 'LOAD:TEST01.WAV' 'SEED:42' 'REFERENCE:0.088388343346' `
  'CW FREQ 900' 'CW CI 6' 'AT:96000 CW ON' 'AT:336000 CW OFF' 'PLAY'
```

That reference RMS belongs to the standard tone only. See the
[RT1064 short-test procedure](modules/rt1064-board.md#reproduce-the-short-wav-and-cw-test)
and [USB capture checks](usb-sound-card-bringup.md#7-play-and-check-bit-exact).
Require complete sample agreement for the clean source, the expected CW timing
and level, and clean board/capture counters. Launcher completion means the
requested operation completed; it does not declare physical acceptance.

## Maintainer notes

`host/usb_bench.py` provides discovery, profile selection and process/log handling.
It delegates protocol grammar and identity validation to `live_control.py`,
card handling to `media_eject.py`, and audio to the native recorder. There is
no new network protocol, audio worker or firmware dependency. Regression tests
in `tests/test_usb_bench.py` exercise fixture isolation, refusal of ambiguous
audio and ports, and failure before an unsafe media handoff.

For current software and physical acceptance boundaries, see the
[testing guide](testing.md). The procedure above records the evidence needed
for a bounded run; launcher completion alone does not establish acceptance.
