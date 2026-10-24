# Host Capture and Analog Analysis

## 1. Scope

This guide covers `waveform_capture`, its WinMM and raw WASAPI backends, the
bounded callback-to-writer queue, controlled session wrapper, mono float WAV
format, and `analyze_capture.py` integrity/correlation report. The same
recorder captures either board's USB sound card. Select its exact current
endpoint name; digital samples can be checked bit for bit.
The [Windows bench](../windows-usb-bench.md) owns the dual-board operator workflow.

## 2. 50,000-foot view

The capture module records what physically arrives at a selected PC audio input.
The backend converts captured audio into a bounded sample queue. WinMM's native
callback only records completed headers and signals an adapter-owned worker;
that worker converts samples, calls the application callback, and requeues
headers. A separate writer drains the sample queue and finalizes the WAV.

```text
audio endpoint -> backend worker / application callback -> bounded sample queue -> writer thread
-> finalized float WAV -> integrity/correlation analysis
```

Capture is a physical-path evidence source. It does not control or reconfigure an
external modem application.

## 3. How to use it

Enumerate devices immediately before a run:

```powershell
build\host-release\host\capture\waveform_capture.exe `
  --backend wasapi-raw --list-devices --json
```

Capture with an exact current endpoint name (the run folder must exist; the
first line makes it):

```powershell
New-Item -ItemType Directory -Force build\runs | Out-Null
.\host\capture-session.ps1 `
  -Output build\runs\capture.wav `
  -DeviceName "REPLACE_WITH_EXACT_INPUT_DEVICE_NAME" `
  -StopFile build\runs\stop.flag `
  -JsonlPath build\runs\capture.jsonl `
  -Backend wasapi-raw -Channel left -MaxSeconds 30
```

Analyze integrity:

```powershell
python host/analyze_capture.py build\runs\capture.wav `
  --output-json build\runs\capture.analysis.json
```

Add `--source clean.wav` for bounded correlation against a known mono PCM16 or PCM24
48 kHz source. Choose `left`, `right`, or `average` from actual cable routing;
do not assume the two input channels are equivalent.

### Capturing the board's USB sound card

Both player families expose a USB recording endpoint. Use the
[Windows bench procedure](../windows-usb-bench.md) for identity-bound staging,
capture and playback, and the [USB capture check](../usb-sound-card-bringup.md#7-play-and-check-bit-exact)
for comparison. Open and settle every capture client before `PLAY` and leave
them open until playback finishes. `PLAY` returns `NO_STREAM` while no client
records from the selected board. Verify actual USB bit depth with `USB?`;
a float capture file does not establish a 24-bit USB stream.

## 4. Scientist and maintainer view

### Backend contract

WinMM requests 48 kHz stereo PCM16 and converts the selected channel to mono
float. Raw WASAPI opens the named active endpoint in shared raw mode, accepts its
actual 48 kHz PCM16 or IEEE-float mono/stereo mix format, and does not silently
resample. Backend identity and conversion policy are emitted in `READY`.

### Real-time queue

One block holds 480 mono samples, or 10 ms. The single-producer/single-consumer
queue holds 512 blocks, or 5.12 seconds. Atomic acquire/release ordering publishes
a complete block before the writer sees it. Queue overflow is a visible failure,
not a request to drop old audio.

WinMM also has a bounded 16-header completion ring before the sample queue.
Its native callback uses atomics and `SetEvent`; the worker performs conversion,
application delivery, and `waveInAddBuffer` outside that callback. Overlapping
native callbacks, ring overflow, signal failure, and sample-queue refusal end
the capture with a reported error. Shutdown resets the driver, waits for native
callbacks, drains and joins the worker, then releases prepared headers.

### Output and measurements

Output is mono IEEE float32 at 48 kHz. Finalization patches RIFF and data sizes.
`READY` identifies endpoint and format; `FINAL` reports delivered/written samples,
tail behavior, queue state, peaks, RMS, clipping, non-finite samples, and stop
reason.

The analyzer checks file structure, hashes, finite values, clipping, DC, RMS,
peak, and long exact-zero/dropout/constant runs. Optional correlation reports a
bounded best offset and similarity. Correlation is not decode and does not prove
modem framing or BER.

### Bit-exact expectations for the USB sound card

`analyze_capture.py --source` reports bounded, decimated correlation; it is not
an exact comparison. Use `host/make_playlist.py compare` for every sample and
channel of a PCM24 expected stream. The [testing procedure](../testing.md#physical-usb-playback-and-cw)
owns the commands, alignment requirements and per-run acceptance sequence.

Decode signed PCM24 at scale `2^23`, not 8388607. Float32 can represent each
such normalized sample exactly. Verify the active USB alternate and bit depth
as well as the capture-file format. Check board and recorder errors, frame
count, the complete active stream and surrounding silence. For live effects,
compare with the acknowledged C++ replay, not the unmodified source.

USB volume requests are not applied by firmware, but Windows gain or processing
can still alter the captured samples. Sample equality is the check; a slider
setting or clean board-side CRC alone is insufficient.

### CUT capture comparisons

Use `host/cut_schedule.py predict` to construct the expected post-engine
PCM24 stream, and `host/make_playlist.py compare` to compare the capture.
The [CLI reference](command-line-tools.md) owns their arguments; the
[time-step guide](time-step-faults.md) owns CUT versus source-slip semantics.
No separately supplied capture verifier is required for this comparison.

Check the entire expected stream, not just the original WAV: ZERO inserts
exact frames, DROP removes frames, and an erasure emits zero instead of the
removed content. A static scenario is rendered before applying CUT. For a
deliberately stopped run, compare only its reported sent prefix and label it
partial; it cannot satisfy a complete-play acceptance check.

Also check per-play frame counts, CRCs, CUT counters and USB stream format.
Board CRCs describe PCM24 before a 16-bit alternate truncates it. A ZERO is
intentional content, not a USB scheduling gap; missed intervals remain errors
even during a ZERO. Align using the expected stream's onset or an explicitly
known capture start, including any leading ZERO frames.

### 16-bit captures of the USB sound card

The endpoint also offers 16-bit stereo (alternate 2) and, on an image built
with `WFG_UAC_MONO_ALT`, 16-bit mono (alternate 3); the
[protocols guide](protocols-media-and-artifacts.md#capture-formats) lists
them and says how they are selected. `USB?` names the format in use
(`uac_stream_alt`, `uac_stream_bits`, `uac_stream_channels`) and the one a
play went out in (`uac_play_stream_bits`, `uac_play_stream_channels`); read
it with the play's STATUS, because a recorder cannot tell: Windows' shared
engine converts the device format to its mix format either way.

What to expect in 16 bits:

1. Each 16-bit sample is the card's 24-bit sample shifted right by 8, an
   arithmetic shift with no rounding and no dither: `capture16 == card24 >> 8`
   exactly, which the lane verifier checks with `--expect-truncation-bits 8`.
   In a float capture (raw WASAPI's float32 mix), each value times 32768 is
   that 16-bit integer; times 2^23 it is the card's sample with its low 8 bits
   cleared toward minus infinity.
2. In mono (one channel) the sample is the card's left channel.
3. Everything before and after the file is exactly zero, as in 24 bits. A
   card whose first samples lie from 1 to 255 shows its first nonzero 16-bit
   sample later than its first nonzero 24-bit one (they truncate to 0, while
   -1 to -256 truncate to -1); align with that in mind.
4. `uac_play_crc32_l` and `_r` stay `zlib.crc32` of the WAV's data chunk:
   the board computes them over the 24-bit source before the conversion. They
   do not describe the 16-bit capture; the shifted comparison in 1 does.

Agents and the lane never change Windows audio settings, so the endpoint's
default format (Sound settings, Advanced) is the owner's call. A 16-bit
capture that leaves the default alone is a WASAPI exclusive-mode capture that
asks for 48 kHz 16-bit PCM itself: `usbaudio2` then selects alternate 2 (two
channels) or, on a mono image, alternate 3 (one channel) for that client, and
returns to alternate 0 when it closes. `waveform_capture` opens WASAPI in
shared mode only (an exclusive backend is a follow-up), so use another
exclusive client, for example python-sounddevice:

```python
import sounddevice as sd

device = next(index for index, info in enumerate(sd.query_devices())
              if info["name"].startswith("Line (RT1170 Player)")
              and sd.query_hostapis(info["hostapi"])["name"] == "Windows WASAPI"
              and info["max_input_channels"] > 0)
seconds = 30
capture = sd.rec(seconds * 48000, samplerate=48000, channels=2, dtype="int16", device=device,
                 extra_settings=sd.WasapiSettings(exclusive=True))
# Other shell: "OUTPUT USB", "LOAD:NAME.WAV", "PLAY" once USB? reads uac_stream_alt 2.
sd.wait()
```

Use `channels=1` on a mono image for alternate 3. Exclusive mode needs the
endpoint to itself: it fails while any shared-mode client records from "Line
(RT1170 Player)", and shared clients cannot open the endpoint while it runs,
so it does not mix with the lane's four-client runs. "Allow applications to
take exclusive control of this device" (Advanced tab) must stay checked, as
it is by default. Start `PLAY` only after the capture has opened the stream
(`PLAY` answers `NO_STREAM` before), and stop the capture only after the play
is done: closing the stream mid-play is error 19.

### Failure and debugging

Retain stderr and JSONL. Distinguish endpoint-open failure, unsupported mix format,
queue overflow, disk write failure, non-finite input, host disconnect, duration
limit, and explicit stop. A WAV without a trustworthy `FINAL` may have a valid
looking header but incomplete provenance.

## 5. 5th-grader view

The callback is a person catching balls from a machine. It puts each small basket
of balls on a conveyor belt and immediately turns back for the next basket. A
different person writes the basket contents into a notebook. If the notebook
writer is too slow and the conveyor fills up, the experiment fails instead of
quietly throwing balls away.

## 6. Toy worked example

Suppose two stereo frames arrive:

```text
frame 0: left=0.25, right=-0.25
frame 1: left=0.50, right=0.00
```

Channel policies produce:

```text
left:    [0.25, 0.50]
right:   [-0.25, 0.00]
average: [0.00, 0.25]
```

If the cable is connected only to the left input, `average` halves the wanted
level. That is why the selected policy belongs in the retained `READY` record.

From the USB sound card, a captured value of `-2.384185791015625e-07` times
2^23 is exactly -2: the board sent the WAV sample stored as `FE FF FF`. Both
channels carry it, so `left`, `right` and `average` all give the same value.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Queue, conversion, measurements | `host/capture/capture_core.hpp` |
| CLI and writer lifecycle | `host/capture/capture_main.cpp` |
| Backends | `host/capture/winmm_audio.*`, `host/capture/wasapi_audio.*` |
| Controlled stop wrapper | `host/capture-session.ps1` |
| Offline analysis | `host/analyze_capture.py` |

Backend or format changes need device-list evidence, conversion unit tests,
queue/finalization tests, explicit no-resampling behavior, a bounded physical
capture, and updated examples with exact endpoint identity.

## 8. Maintainer walkthrough

Start at `host/capture/capture_main.cpp` for explicit backend/device selection and
controlled-stop behavior. Read the selected backend and the capture core queue
together: the WinMM native callback hands completed headers to its worker, while
the backend application callback enqueues bounded sample blocks. The writer
thread owns file writes and final RIFF lengths. Keep native handles owned by
cleanup scopes and report setup/write/queue failures in the final record.

`analyze_capture.py` reads the finalized artifact and produces integrity/level
metrics. Correlation and expected-source analysis have explicit input-format
requirements; a float capture is not automatically a valid target playback source.
`wfg-capture-core` exercises queue/writer behavior and `wfg-analyze-capture` checks
analysis fixtures. Neither test opens a physical input. Device enumeration,
physical capture, cable routing and decoder scoring are separate checks.

## Analyzer details

The analyzer streams a complete mono 48 kHz float32 capture. It checks RIFF and
chunk lengths, finite values, sample counts, whole-file and PCM checksums. Default
long-run detection is 4,800 samples (100 ms); the dropout threshold is `1e-6`.
Clipping is counted at `abs(sample) >= 0.999`. Exact-zero, near-zero and constant
runs can be intentional, so interpret them against the source and physical route.

With `--source`, one-second windows near the source's beginning and end are
correlated within bounded capture regions, decimated every 100 samples. The
report includes anchor confidence, lag and a two-anchor clock estimate in ppm.
Noise, repeated patterns and silence can make the estimate ambiguous. The
capture is not resampled to hide lag. Analyzer success means the file was
analyzed; modem decoding and analog performance require their own checks.
