# Waveform Source and Encoder Abstraction

## 1. Scope

This guide covers `waveform-source/`: the generic byte source, PCM encoder,
byte sink, streaming WAV generator, artifact hashes, and job state used by host
and target producers. It also explains the encoder catalog boundary.

## 2. 50,000-foot view

The waveform-source layer separates "how bytes become samples" from "how samples
are stored or played." An encoder reads payload bytes and emits normalized mono
float frames. A WAV generator quantizes them once into a valid 48 kHz packed
PCM24 WAV while calculating hashes.

```text
ByteSource -> registered Encoder -> normalized float -> PCM24 WavGenerator -> ByteSink
```

The impairment engine consumes the resulting samples through `SampleSource`; it
does not need to know which encoder produced them.

## 3. How to use it

An encoder implementation must support this lifecycle:

1. Construct it in caller-owned workspace.
2. Configure it with a profile, payload source, and declared payload length.
3. Read bounded normalized-float chunks until `read_float()` returns zero at the declared end.
4. Check `status()` rather than treating zero as unconditional success.
5. Call `stop()` when abandoning the operation.

The WAV generator takes a configured encoder and a seekable byte sink. The host
utility exposes the current M110B adapter:

```powershell
build\host-release\host\tx\wfg_m110_tx.exe 600 long output.wav "HELLO"
build\host-release\host\tx\wfg_m110_tx.exe 600 long output.wav --file payload.bin
```

On RT1170, `encoder_catalog.cpp` resolves an explicit encoder ID. Only `M110B`
is currently registered. Unknown IDs fail; there is no fallback to a guessed
waveform.

## 4. Scientist and maintainer view

### Interfaces and ownership

`ByteSource` owns the payload stream. `Encoder` owns bounded waveform state but
not the payload object. `ByteSink` owns destination storage. `WavGenerator`
coordinates them and tracks state. Every referenced object and workspace must
outlive the active job.

The declared payload length is part of the contract. A short intermediate read
is legal. A zero-length read before the declared end is an error because silently
shortening a message would create a plausible but false artifact.

### WAV and hash contract

Generated audio is mono signed packed PCM24 little-endian at 48 kHz. The generator knows the
encoder's total frame count, writes bounded chunks, updates PCM and complete-WAV
hashes, and publishes completion only after the sink and header are complete.

An output filename is not identity. Retain payload hash, encoder ID and version,
profile, frame count, PCM hash, WAV hash, and any target status or manifest.

### Extension boundary

A new encoder belongs behind `waveform_source::Encoder`. It must not add waveform
knowledge to the player, Signal Lab, WAV writer, or USB transport. Registration
on RT1170 must include an explicit workspace size/alignment and a version string.

Host success and target success are separate because workspace, compiler float
behavior, and sink failure paths differ.

## 5. 5th-grader view

The encoder is a music-box cylinder. You put message bytes in one end and it
turns them into a stream of numbers for a speaker. The WAV generator is the box
and label around that stream. The label says how fast to play it and includes a
fingerprint so you can tell whether any byte changed.

The player does not care which music-box cylinder was used. It only receives the
same kind of sample stream.

## 6. Toy worked example

Assume a toy encoder declares six output samples and emits at most two per read:

```text
payload: [0x41]
read 1:  [100, 200]
read 2:  [-100, -200]
read 3:  [50, 0]
read 4:  [] with status=success
```

The WAV generator writes a header declaring six mono PCM24 frames, then the 18
PCM data bytes. If read 3 returned an empty span while two samples were still
declared, the job would fail rather than publishing a four-sample WAV.

The real encoder produces much larger chunks and a standard waveform, but the
ownership and end-of-stream rules are the same.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Generic interfaces | `waveform-source/encoder.hpp` |
| Streaming WAV job | `waveform-source/wav_generator.hpp`, `waveform-source/wav_generator.cpp` |
| Hash helper | `waveform-source/sha256.hpp` |
| Current adapter | `native-m110/source.*`, `native-m110/wav_generator.*` |
| Target registration | `rt1170/encoder_catalog.*` |

When adding an encoder, add host tests for chunk boundaries and short reads,
target workspace checks, supported-profile documentation, deterministic hashes,
and an explicit statement of what external standard or source owns the waveform.
