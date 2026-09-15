# reusable payload-to-WAV artifact generation

The artifact generator creates a retained clean waveform before playback. Both supported entry
paths finish with a WAV on the SD card, an exact binary payload, and a JSON
manifest. The test system can retrieve those files through MSC after local media
ownership has been released, and retain them with its playback/control journal.

```text
PC payload -> local encoder -> WAV + BIN + JSON -> SD staging
                                                    |
USB payload -> SD BIN -> board encoder -> SD WAV + JSON
                                                    |
                    saved SD WAV -> scenario/live impairments -> audio output
```

Encoding is an incremental file-generation utility. It does not send native
encoder PCM directly into the player FIFO. The same saved WAV can be replayed
cleanly, impaired with another scenario, or retrieved for an independent decode.
An existing external waveform generator can also supply the saved WAV.

## PC artifact creation

The default build consumes existing WAVs and can create a calibration tone with
`python host/stage_wav.py make-tone --output build/TONE.WAV`. No encoder is needed.
An optional module may also supply host tools; those commands and supported
profiles belong in that module's documentation. See [Encoder plugins](encoder-plugins.md).

Copy a compatible WAV to `/WG` while the host owns the SD medium. After the media
handoff to `MEDIA LOCAL`, use `LOAD:NAME.WAV` and `PLAY` through the
[live controller](live-control-protocol.md). Encoding does not start playback.

## Board artifact creation

With a writable image and an enabled plugin, the board supports a payload already staged as `/WG/PAYLOAD.BIN` and a binary
payload uploaded over the existing DD008 framing. All filesystem operations and
encoder steps execute in the player task. Generation is refused while playback
is busy; the host cannot own the SD medium during upload or generation.

With the payload already on SD, the generic WFG-LIVE/1 command is:

```text
ENCODE TONE 1000 PAYLOAD.BIN TEST003.WAV
```

This example requires the public `TONE` plugin. The default build reports an
empty encoder list and returns `ENCODERS_DISABLED` for generation.
`GENERATE:600:long:PAYLOAD.BIN:TEST003.WAV` is a legacy spelling requiring an
adapter that explicitly opts into the rate/interleave protocol.
Poll the file-generation status until complete before loading the WAV. A command
acknowledgement means the request was accepted, not that the artifact is finished.

For USB payload upload with a legacy-compatible adapter, first complete the media handoff and close the live
controller connection. The DD008 utility verifies the explicitly selected USB
port's serial and fixture identity before changing configuration:

```powershell
python host/tx_upload.py --port <actual-port> --expected-serial <actual-serial> --payload message.bin --output TEST004.WAV --mode 600L --record artifacts/upload004.jsonl
python host/tx_upload.py --port <actual-port> --expected-serial <actual-serial> --sd-input PAYLOAD.BIN --output TEST005.WAV --mode 1200S --record artifacts/upload005.jsonl
```

Install the explicit dependency in `host/requirements-control.txt` for serial
operation. DD008's control channel carries the existing `CMD:DATA RATE:` and
`CMD:SENDBUFFER` spellings; its data channel carries at most 256 payload bytes per
acknowledged chunk. `SENDBUFFER` starts artifact creation. The upload bound is
1,048,576 bytes, and the resulting waveform must also fit canonical RIFF's size
limit. The utility waits for completed counters and a digest. It does not retry
an uncertain mutation automatically. Its `4800U` token selects the adapter's
`4800:zero` profile.

For an upload, the board retains `TEST004.BIN`. For an existing SD payload it
retains and references that original BIN. The board refuses existing output WAV,
JSON or temporary names. A successful job syncs and closes the WAV and JSON,
renames the WAV, then publishes the JSON completion marker. Failed jobs do not
publish a complete manifest; partial `.TMP`/`.JMP` files may remain for inspection.
`CMD:RESET MDM` aborts a pending job without deleting retained artifacts; WFG-LIVE/1
`STOP` can also abort an active generation job. The read-only inspection image
excludes the artifact writer and rejects creation requests. Reconnect
to switch between DD008 and WFG-LIVE/1, then explicitly LOAD/PLAY the result.

After generation/playback have released their files, hand the medium to the host
and retrieve the referenced payload, WAV, JSON and host journal. The reports include optional diagnostic hashes.

## Encoder and artifact interfaces

`waveform-source/encoder.hpp` defines a sequential `ByteSource` and an `Encoder`
implementing the existing `signal_lab::SampleSource` PCM interface. An encoder
accepts an opaque profile, a payload byte source and its declared byte count. It
must provide the exact waveform frame count and produce canonical 48 kHz mono
PCM24. It also reports errors and supports stopping. The generic writer owns the
artifact's default 48,000-frame trailing silence.

`waveform-source/wav_generator.*` writes a known-length 44-byte RIFF header and
then at most 2,048 frames per `step()`. It hashes payload, complete WAV and PCM
region incrementally. Digests become available only after successful completion.
No whole-message payload or waveform buffer is required. Its byte sink abstracts
host files and FatFs equally; the owner handles publication and failed files.

Enabled modules supply the descriptor list behind `waveform-source/encoder_registry.cpp`.
Each registers an ID, version, workspace requirement and factory. The firmware
reserves aligned 128 KiB encoder storage only when generation is compiled in.
The SD utility, WAV writer, effects engine and player need no modulation-specific
branches. The default catalog is empty; the public example registers `TONE`.
See [Encoder plugins](encoder-plugins.md) for module structure and build options.

## Artifact identity and validation

Board manifests identify the retained payload and its SHA-256, the WAV SHA-256,
PCM-region SHA-256, encoding settings, generator version and build label. Paths are relative to the sidecar directory. The board's `waveform-artifact/1`
manifest records the generic encoder ID, opaque profile, total frames, trailing
silence and `complete: true`. Module-specific host tools document their own
additional metadata and schemas.

`host/m110_score.py` accepts uppercase or lowercase JSON sidecars, resolves
relative payload paths and translates a generic board `M110B` profile into its
M110 mode for scoring. The waveform-agnostic Python renderer preserves the
encoder/profile identity in an impaired sidecar's `source_reference`; only the
M110 scorer interprets that profile. A different encoder ID is not treated as
M110. Decoder execution remains an explicitly supplied external tool.

## Maintenance checks

Run plugin, waveform-source and artifact-owner tests after changes.
Both writable and read-only firmware profiles should build. Physical upload,
SD access and analog playback are separate runtime checks.
