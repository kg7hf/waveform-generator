# Host Capture and Analog Analysis

## 1. Scope

This guide covers `waveform_capture`, its WinMM and raw WASAPI backends, the
bounded callback-to-writer queue, controlled session wrapper, mono float WAV
format, and `analyze_capture.py` integrity/correlation report.

## 2. 50,000-foot view

The capture module records what physically arrives at a selected PC audio input.
The audio callback only converts and queues samples. A separate writer drains the
queue and finalizes the WAV, keeping disk I/O outside the real-time callback.

```text
audio endpoint -> callback conversion -> bounded queue -> writer thread
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

Capture with an exact current endpoint name:

```powershell
.\host\capture-session.ps1 `
  -Output build\runs\capture.wav `
  -DeviceName "Microphone (Realtek(R) Audio)" `
  -StopFile build\runs\stop.flag `
  -JsonlPath build\runs\capture.jsonl `
  -Backend wasapi-raw -Channel left -MaxSeconds 30
```

Analyze integrity:

```powershell
python host/analyze_capture.py build\runs\capture.wav `
  --output-json build\runs\capture.analysis.json
```

Add `--source clean.wav` for bounded correlation against a known mono PCM16
48 kHz source. Choose `left`, `right`, or `average` from actual cable routing;
do not assume the two input channels are equivalent.

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

### Output and measurements

Output is mono IEEE float32 at 48 kHz. Finalization patches RIFF and data sizes.
`READY` identifies endpoint and format; `FINAL` reports delivered/written samples,
tail behavior, queue state, peaks, RMS, clipping, non-finite samples, and stop
reason.

The analyzer checks file structure, hashes, finite values, clipping, DC, RMS,
peak, and long exact-zero/dropout/constant runs. Optional correlation reports a
bounded best offset and similarity. Correlation is not decode and does not prove
modem framing or BER.

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
