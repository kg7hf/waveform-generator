# PC WAV playback

## Scope and 50,000-foot view

`host/play_wav.py` streams an existing mono 48 kHz PCM24 or legacy PCM16 file to
one explicitly selected Windows WASAPI output through sounddevice/PortAudio.
Any WASAPI output device that accepts 48 kHz stereo playback can be selected,
including built-in sound cards, USB audio interfaces, speakers and headsets.
There is no vendor-specific device requirement. This helper has no live interference engine; render the mixture
first.

```text
WAV -> validate format -> normalize -> playback attenuation -> duplicate L/R
    -> blocking WASAPI shared-mode stream -> selected headphones/speakers
```

## How to use it

```powershell
python -m pip install -r host/requirements-playback.txt
python host/play_wav.py --list-devices
python host/play_wav.py build/demo/clean-cw.wav --device "REPLACE_WITH_EXACT_OUTPUT_DEVICE_NAME" --gain-db -12
```

The friendly name must match exactly one current WASAPI stereo-capable output.
Device indices are diagnostic only. Missing/ambiguous names fail without default
fallback. Ctrl+C stops playback. `PLAYING` identifies the file, output and gain;
`DONE` reports submitted frames and underflows. An underflow produces a nonzero
exit status. Normal completion drains queued output before closing.

## Scientist and maintainer view

### Functions and ownership

| Entry point | Contract |
|---|---|
| `outputs()` | Enumerates current host APIs/devices and filters WASAPI outputs |
| `main()` argument validation | Requires file and exact device; finite nonpositive playback gain |
| WAV context | Owns file handle and validates mono/48 kHz/PCM16-or-24 before opening stream |
| Output stream context | Owns the selected device; writes at most 4096 frames per iteration |
| PCM conversion | Sign-extends packed 24-bit samples then divides by 8388608; legacy16 divides by 32768 |
| Completion | Checks declared/read frame count; reports underflows and closes resources |

The mono waveform is duplicated to both output channels, not interleaved with
different content. Attenuation scales the complete mixture, preserving its C/I.
The helper allocates NumPy arrays and performs blocking writes. It is an offline
convenience path, not the embedded callback model. Windows processing and volume
can alter analog output, so record the endpoint settings for physical evidence.

### Failure diagnosis

| Symptom | Inspect |
|---|---|
| No exact match | Current enumeration, unplugged headset, ambiguous duplicate endpoint names |
| Unsupported file | Channels, rate, sample width and compression; convert explicitly |
| Device rejects 48 kHz stereo float | Actual endpoint capabilities/settings |
| Nonzero underflows | Host scheduling/load; the emitted stream may have gaps |
| Frame mismatch | Truncated file; do not claim a completed vector |

Device enumeration is read-only. Software tests for PCM packing do not establish
that a physical endpoint was opened or heard. Hardware playback is a separate
operation with its own retained device identity and observations.

## 5th-grader view and worked example

The helper is a tape player with a named headphone socket. It plays the recording
you already made and sends the same sound to both ears. A PCM24 sample of
`2097152` represents `0.25`. At -12 dB playback gain it becomes about `0.0628` on
each channel. The whistle and desired signal both get quieter by the same amount.

## Change checklist

Keep device selection explicit, check file geometry before opening audio,
preserve context cleanup, and report incomplete/underflowed playback. If adding
live rendering, design a separate preallocated producer/output contract and
test deadline behavior; do not put file parsing or NumPy allocation in an audio
callback. The [how-to guide](../how-to-use.md) owns the end-to-end playback recipes.
