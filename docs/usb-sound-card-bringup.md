# USB sound card bring-up

This runbook takes a player image from the build to a verified bit-exact play
through the board's USB sound card, the Windows recording endpoint "Line
(RT1170 Player)". It needs no code reading. The device side is explained in
[Protocols, Media and Artifacts](modules/protocols-media-and-artifacts.md)
(identity, composite device, UAC2 function, card ownership) and
[RT1170 Player and Audio](modules/rt1170-player-and-audio.md#usb-route) (the
USB route and its statistics). Every command and field is listed in the
[protocol reference](live-control-protocol.md#usb-sound-card).

```text
build -> flash (LinkServer) -> identity (USB, INFO?, Windows endpoint)
-> stage the card (MEDIA HOST, copy, media-only eject, MEDIA LOCAL)
-> open the capture clients and let them settle
-> OUTPUT USB, LOAD, PLAY -> STATUS? checks and capture check -> MEDIA HOST
```

## 1. Before you start

- **Shared bench.** Flashing, resetting or replugging the board, and opening
  or closing capture clients on a PC that runs live audio tests, change what
  other users of that bench see. Do these steps only inside a board window
  agreed with the board's owner and, for a board the compliance lane uses,
  with the lane.
- **Board.** MIMXRT1170-EVK with the microSD card inserted *before* reset:
  the player initializes the card at boot and does not use card detect. The
  PC connects to the EVK's USB OTG1 port, where the player enumerates, and
  the probe to its debug port.
- **PC tools.** NXP LinkServer (the bench uses 26.6.137); the host build
  (`cmake --preset host-release`, then `cmake --build --preset host-release`)
  for `waveform_capture.exe`; and
  `python -m pip install -r host/requirements-control.txt` for
  `live_control.py`.
- **Windows.** Run every command from the repository root. Steps 2 to 5 and
  the shell commands of step 7 run in one PowerShell window, the work window.
  Step 6 opens two more, the capture window and the control window: the
  capture and the control session each hold their window until they end.

## 2. Build the image

```powershell
$preset = 'rt1170-player-release'   # 'rt1170-player-readonly-release' for PID 0x4013
cmake --preset $preset
cmake --build --preset $preset --parallel
```

The link prints the memory use of each region. The image is
`build\$preset\waveform_generator.elf`: each preset builds into its own
directory, and `$preset` keeps steps 2 and 3 on the same image. Record the
commit and exact image identity before flashing:

```powershell
git rev-parse HEAD
(Get-FileHash -Algorithm SHA256 build\$preset\waveform_generator.elf).Hash.ToLower()
```

The `rt1170-player-readonly-release` preset builds the PID `0x4013` image,
which keeps the card write-protected: Windows cannot copy a file to it, so
the card must already hold the file to play (step 5). A tree with
uncommitted changes to tracked files builds an image with `fw_dirty` 1; only
`fw_dirty` 0 means `fw_commit` names the exact source
([build identity](modules/build-and-dependencies.md#maintainer-view)).

## 3. Flash with LinkServer

```powershell
LinkServer probes                               # attached probes and their serial numbers
LinkServer devices | Select-String MIMXRT1176   # LinkServer's names for the RT1176 parts and boards
LinkServer flash --probe "REPLACE_WITH_PROBE_SERIAL" "REPLACE_WITH_DEVICE_NAME" load build\$preset\waveform_generator.elf
```

`<probe-serial>` is the serial `LinkServer probes` prints for this board's
probe (never one copied from another machine), and `<device>` is the entry for
the MIMXRT1170-EVK. The image runs in place from the EVK's FlexSPI NOR flash.
If the board does not restart into the new image, reset it with
`LinkServer probe <probe-serial> wiretimedreset 100` or the EVK's reset
button. The COM port and the drive appear some seconds after a reset (about
about 16 s in the observed setup). The first enumeration of a new `bcdDevice` also installs
the Windows drivers, which can take longer.

## 4. Verify the identity

### 4.1 USB

```powershell
python -m serial.tools.list_ports -v
```

Find the port whose hardware ID shows `VID:PID=CAFE:4012` (`CAFE:4013` for the
read-only image) and note its `SER=` value, 16 uppercase hex digits. Use that
port and serial below; never a remembered COM number or drive letter.

### 4.2 INFO and STATUS

```powershell
python host/live_control.py --port COM42 --expected-serial SERIAL `
  --record identity.jsonl command "INFO?" "USB?" "STATUS?"
```

`live_control.py` checks `kind`, `board`, `sample_rate_hz` and `usb_serial`
before anything else. It prints the reply to each listed command; the final
`STATUS?` it sends itself goes only to the `--record` journal, so the
commands here list `STATUS?` to print one. Check:

| INFO field | Expected |
|---|---|
| `usb_serial` | The serial from 4.1 |
| `usb_pid` | 16402 (`0x4012`), or 16403 (`0x4013`) together with `"read_only":true` |
| `fw_commit` | The commit you built: 40 lowercase hex digits |
| `fw_dirty` | 0 (1 modified tree, 2 unknown, 3 clean but with encoder plugins) |
| `build_id` | The build label (`WFG_BUILD_ID`, for example `wfg-0.2.0`) |
| `output` | `codec`: the route at every boot |
| `capabilities` | Includes `usb-audio` and `usb-diagnostics`; also `usb-audio-mono` on a `WFG_UAC_MONO_ALT` image only, and `sd-clock-24mhz` on a `WFG_SD_CLOCK_24MHZ` image only (absent on a 49.5 MHz image) |

In the STATUS reply, `fw_commit_hi` and `fw_commit_lo` are the first 16 hex
digits of `fw_commit` as two decimal numbers, `output_route` is 0,
`media_state` is 2 (the host owns the card; 1 while the card still
initializes after the reset, 255 when it failed, with the reason in
`media_error`), and `uac_stream_open` is 0 until a PC program records from
the endpoint.

### 4.3 Windows

- The Sound control panel, Recording tab, lists "Line (RT1170 Player)", and
  its Advanced tab's Default Format reads 2 channel, 24 bit, 48000 Hz; the
  list also offers 2 channel, 16 bit, 48000 Hz (and 1 channel, 16 bit on a
  `WFG_UAC_MONO_ALT` image). Read only: if the default is not 24-bit, the
  owner sets it back
  ([capture formats](modules/protocols-media-and-artifacts.md#capture-formats)).
- The default recording device is still the one the lane captures from. If
  Windows moved it to the board, set it back
  ([why](modules/protocols-media-and-artifacts.md#the-recording-device-on-windows)).
- `build\host-release\host\capture\waveform_capture.exe --backend wasapi-raw --list-devices --json`
  lists `"native_name":"Line (RT1170 Player)"`.

Close the Sound control panel and Sound settings afterwards. Their level
meters open the endpoint, and Windows pauses the shared stream whenever a
client joins or leaves it (step 6).

## 5. Stage the card

The player plays mono, 48 kHz, packed PCM24 WAV files (format tag 1) from
`\WG\` on the card. The name is uppercase 8.3 with only letters, digits and
underscore in the stem, for example `FSRAND.WAV` (not `FS-RAND.WAV`; see the
[naming rule](live-control-protocol.md#engineering-protocol)): `LOAD`
refuses any other name. Check a file first with
`python host/stage_wav.py inspect NAME.WAV`. For a bit-exact play, leave no
`NAME.SCN` beside it: a scenario would process the audio.

**Never use Explorer's Eject (or "Safely Remove Hardware") on the board's
drive.** In some states it can remove the whole composite USB device, including
the COM port and capture endpoint. Eject only
the medium, with `host/media_eject.py`, from an **ordinary** PowerShell first.
Non-elevated lock/dismount/eject works on the tested removable FAT32 volume;
Windows policy and device permissions can vary. Select the storage serial:


```powershell
python host/live_control.py --port COM42 --expected-serial SERIAL `
  --record host.jsonl command "MEDIA HOST"
# The drive appears (disk "WFG RT1170 SD LOADER"). Ordinary shell: copy the
# file, read it back past the Windows cache, compare SHA-256, then eject the
# medium only (lock, dismount, allow removal, eject):
python host/media_eject.py stage NAME.WAV --expected-serial SERIAL --eject
if ($LASTEXITCODE -ne 0) { throw "Staging failed; do not send MEDIA LOCAL" }
# The drive disappears and stays away; COM42 and the endpoint stay.
python host/live_control.py --port COM42 --expected-serial SERIAL `
  --record ejected.jsonl command "STATUS?"
# Expect msc_ejected 1 and msc_host_hold 0.
python host/live_control.py --port COM42 --expected-serial SERIAL `
  --record local.jsonl --host-volume-dismounted command "MEDIA LOCAL" "STATUS?"
# The STATUS reply shows media_state 3: the board owns the card.
```

`media_eject.py` finds the board's drive by its disk identity and refuses
any other drive (`python host/media_eject.py find` lists them); `eject`
alone ejects after a copy made some other way. Its JSON reports
`process_elevated` separately from the volume access mode. Exit 3 means
Windows denied both open modes; exit 4 means the lock could not be obtained.
Close files held open on the card and inspect permissions/policy. Do not
reset the board or assert `--host-volume-dismounted` to bypass a failed eject.
[Windows USB bench](windows-usb-bench.md) has the dual-board, serial-bound
automated staging path. Current acceptance boundaries are in the
[testing guide](testing.md).
[Command-line tools](modules/command-line-tools.md#hostmedia_ejectpy) has
every option, and `media_eject.py cycle` runs the whole staging cycle.

`E:` stands for the drive letter Windows gives the card this time. The copy
needs the writable image (PID `0x4012`). The read-only image (`0x4013`)
fails every write to the card, so there the card must already hold
`\WG\NAME.WAV`, staged with the writable image or in a card reader (and
inserted before the reset, step 1): use `media_eject.py eject` instead of
`stage` and do the rest as written. The eject
latches `msc_ejected`, so Windows cannot remount the volume it just ejected;
only an accepted `MEDIA HOST` (or a board reset) brings the drive back. The
eject does not hand the card over: `MEDIA LOCAL` does, and only once Windows
has let go of the volume. `--host-volume-dismounted` is your assertion that it
has; the firmware checks it too:

| `MEDIA LOCAL` reply | Meaning | Do |
|---|---|---|
| `HOST_WRITING` | A host write is between its blocks | Ask again shortly |
| `HOST_LOCKED` | Windows still prevents medium removal | Finish the eject first |
| `BUSY` | The card is in neither host nor board ownership | Read `media_state` and `media_error` |

## 6. Open the capture clients and let them settle

Rule: **open all capture clients and let them settle before `PLAY`.**
Windows may pause the shared stream while its audio engine reconfigures it,
including when capture clients join or leave. A pause that overlaps a play's
file counts in that play's `uac_play_missed_intervals`.

In the work window, make the directory the run's files go to:

```powershell
New-Item -ItemType Directory -Force run | Out-Null
```

1. Start every program that records from "Line (RT1170 Player)" for this run,
   each selecting the endpoint by name. For a capture file, open a second
   PowerShell window, the capture window, started at the repository root
   (the recorder resolves `-Output` against the folder the window started
   in, not one reached with `cd`), and run:

   ```powershell
   .\host\capture-session.ps1 -Output run\NAME-capture.wav `
     -DeviceName "Line (RT1170 Player)" -Backend wasapi-raw -Channel left `
     -StopFile run\stop.flag -JsonlPath run\NAME-capture.jsonl
   ```

   It holds the capture window until `run\stop.flag` appears (step 7). Its
   `READY` record should name the input format `IEEE_FLOAT32_STEREO_48000`.
   A `PCM16_LE_...` mix format would truncate the samples, and the capture
   could not be bit-exact.
2. Close the Sound control panel and Sound settings.
3. Open one control session for the rest of the run in a third window, the
   control window:

   ```powershell
   python host/live_control.py --port COM42 --expected-serial SERIAL `
     --record run\NAME-play.jsonl interactive
   ```

   A rejection by the board leaves the session open. A line the tool itself
   refuses ends it (exit code 2): a typing error, a run ID that is not 32
   lowercase hex digits, or a name `LOAD` does not accept. Start a new
   session then, with a new `--record` file.
4. Send `STATUS?`: `uac_stream_open` is 1. Repeat it until
   `uac_missed_intervals` has not changed for 3 s.
5. From here until the play is done, do not start or stop a capture client and
   do not open a level meter.

## 7. Play and check bit-exact

In the work window, compute the expected values from the file you staged and
make a run ID of 32 lowercase hex digits (the dashed default of
`[guid]::NewGuid()` is refused):

```powershell
python -c "import sys, wave, zlib; w = wave.open(sys.argv[1], 'rb'); d = w.readframes(w.getnframes()); print('frames', w.getnframes(), 'crc32', zlib.crc32(d))" NAME.WAV
[guid]::NewGuid().ToString('N')
```

In the control window, with your run ID in place of the example's:

```text
OUTPUT USB
LOAD:NAME.WAV
PLAY:0123456789abcdef0123456789abcdef
STATUS?
```

Repeat `STATUS?` until `player_state` is 7 (done); 8 is aborted (STOP) and 255
a fault, with the reason in `player_error`. Then check:

| STATUS field | Bit-exact play |
|---|---|
| `player_state`, `player_error` | 7, 0 |
| `engine_state`, `live_events` | 0, 0: no scenario and no live control touched the samples |
| `underruns` | 0 |
| `uac_play_file_frames` | The file's frames (also `file_frames_total`) |
| `uac_play_crc32_l`, `uac_play_crc32_r` | Both the file's `crc32` |
| `uac_play_last_frame_index` - `uac_play_first_frame_index` + 1 | The file's frames |
| `uac_play_missed_intervals` | 0 (the lane's per-play assertion) |
| `uac_short_packets`, `usb_stack_asserts` | Unchanged since before the play; if `usb_stack_asserts` rose, `USB?` names where ([TinyUSB assertions](live-control-protocol.md#tinyusb-assertions)) |

Then send `USB?` (before the next `PLAY`, which restarts its play fields):

| `USB?` field | Bit-exact play |
|---|---|
| `uac_play_stream_bits`, `uac_play_stream_channels` | 24, 2: the play went out in 24-bit stereo (alternate 1) |

This check is part of every bit-exact verdict. The CRCs are over the card's
24-bit samples in every capture format, so a play that Windows took in 16 bits
(its default format moved to "2 channel, 16 bit") or in mono reports the same
`uac_play_*` values as a bit-exact one; only `USB?` shows the truncation. With 16 bits the
capture is the card `>> 8`, not the card
([capture formats](modules/protocols-media-and-artifacts.md#capture-formats)).

Stop the capture from the work window
(`New-Item -ItemType File run\stop.flag`). In the capture window, its
`FINAL` record should show `completion_reason` `STOP`, `queue_overruns` 0,
and `data_discontinuity_packets` equal to `startup_discontinuity_packets`;
the recorder reports `CAPTURE_ERROR` otherwise.

Then compare the capture with the file. The compliance lane does this with its
own capture verifier (this repository has no copy); the checks are:

1. Every captured value times 8388608 (2^23) is an integer from -8388608 to
   8388607: a float32 holds every 24-bit sample exactly.
2. From the first nonzero sample (aligned with the file's first nonzero
   sample), the next `uac_play_file_frames` samples equal the file's samples.
3. Every sample before them and after them is exactly 0.
4. Those samples, written back as 3-byte little-endian PCM24, have the CRC-32
   `uac_play_crc32_l`.

Without the lane's verifier, run the same checks in the work window with the
Python standard library. Python's `wave` module cannot open the capture (a
float32 WAV, format tag 3), so the check reads the recorder's 44-byte header
itself and compares bit patterns: each file sample as the float32 the board's
stream becomes on Windows (the sample divided by 2^23).

```powershell
@'
import array, struct, sys, wave, zlib
data = open(sys.argv[1], "rb").read()
h = struct.unpack_from("<4sI4s4sIHHIIHH4sI", data)
assert h[3:8] == (b"fmt ", 16, 3, 1, 48000) and h[10:12] == (32, b"data"), "not a waveform_capture WAV"
cap = data[44:44 + h[12]]
assert any(cap), "the capture is silent"
src = wave.open(sys.argv[2], "rb")
assert (src.getnchannels(), src.getsampwidth(), src.getframerate()) == (1, 3, 48000), "not mono PCM24"
raw = src.readframes(src.getnframes())
wide = bytearray(len(raw) // 3 * 4)  # each sample << 8, as the board sends it
wide[1::4], wide[2::4], wide[3::4] = raw[0::3], raw[1::3], raw[2::3]
want = array.array("f", (v / 2**31 for v in array.array("i", wide))).tobytes()
first = lambda b: next(i for i, w in enumerate(array.array("I", b)) if w)
onset = first(cap) - first(want)
start, end = 4 * onset, 4 * onset + len(want)
match = onset >= 0 and cap[start:end] == want
print("frames", len(raw) // 3, "onset", onset, "match", match,
      "silent_before", not any(cap[:max(start, 0)]), "silent_after", not any(cap[end:]),
      "crc32", zlib.crc32(raw) if match else None)
'@ | python - run\NAME-capture.wav NAME.WAV
```

The capture is bit-exact when `match`, `silent_before` and `silent_after` are
all `True` (checks 1 to 3), `frames` is `uac_play_file_frames`, `crc32`
is `uac_play_crc32_l` (check 4: the matched samples are the file's, so their
PCM24 CRC is the data chunk's), and `USB?` read 24 bits and 2 channels for
the play. `onset` is the capture index of the file's
first sample. A 5-minute file takes a few seconds. The lane's verifier also
checks the onset against `uac_play_first_frame_index`.

If `uac_play_missed_intervals` is not 0, send `USB?` before the next `PLAY`.
`uac_play_unprimed_intervals` above 0 means the USB interrupt on the board was
held off (the device's part); 0 leaves the missed intervals to the host
([miss attribution](modules/rt1170-player-and-audio.md#miss-attribution-usb)).
What the PC capture shows for a missed millisecond is up to Windows.

## 8. When something fails

| Symptom | Meaning | Do |
|---|---|---|
| `PLAY` answers `NO_STREAM` | No PC program records from the endpoint (`uac_stream_open` 0) | Step 6 |
| `PLAY` answers `BAD_STATE` | No `LOAD` since the last run, the card is not Local, or a generation job runs | `LOAD` again; `MEDIA LOCAL` |
| `OUTPUT USB` answers `BUSY` | A run is active | `STOP`, then wait for state 7, 8 or 255 |
| `MEDIA HOST` answers `BUSY` | A run is active (an armed USB play included) | Wait for the end, or `STOP` |
| `player_error` 15 | Underrun: the output FIFO ran dry before the end of the file | Read the SD counters (`sd_read_errors`, `max_sd_read_cycles`) |
| `player_error` 19 | The stream closed (Windows selected alternate 0, as when the last capture client stops; a bus reset; an unplug) before every file frame was confirmed sent | Keep the clients open; `LOAD` and `PLAY` again |
| `player_error` 20 | More than 100 ms between USB packets while the play was armed: the PC stopped polling | Check the PC; the statistics still describe exactly what was sent |
| `player_error` 26 | The play's last queued chunks were not confirmed sent for more than 100 ms although the PC kept polling, or a completion during the play reported a size other than its packet (`uac_sent_mismatches` rose) | Read `USB?` (`uac_sent_mismatches`, `uac_short_packets`) and report it with the image's commit. The statistics describe what TinyUSB sent; after a mismatch, compare the capture with them (CRCs, frame count) to tell whether the controller's short report was a packet that went out or one that never did |
| `uac_play_missed_intervals` above 0 | A pause delayed the file | `USB?` (step 7), then repeat with settled clients |
| The drive does not come back | `msc_ejected` is 1 | `MEDIA HOST` |
| The COM port and the endpoint vanished after an eject | Explorer's Eject or Safely Remove removed the whole composite device | Reset the board; stage with `host/media_eject.py` (step 5) |
| `media_eject.py` exits 3 | Windows denied both volume-open modes | Check storage identity, device policy and permissions; leave the card host-owned (step 5) |
| `media_eject.py` exits 4 | A program still has a file open on the drive | Close it (Explorer windows on the drive too) and run it again |
| Windows records silence from the wrong device | The default recording device moved to the board | Step 4.3 |

## 9. Finish

`STOP` a run you want to end early. `OUTPUT CODEC` returns `PLAY` to the codec
(a reset does too). `MEDIA HOST` gives the card back to Windows and clears
the eject latch.
