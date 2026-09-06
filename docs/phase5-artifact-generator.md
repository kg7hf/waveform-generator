# Phase 5: reusable payload-to-WAV artifact generation

Phase 5 creates a retained clean waveform before playback. Both supported entry
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

Build the standalone host target:

```powershell
cmake --preset host-release
cmake --build --preset host-release --target wfg_m110_tx --parallel
build/host-release/host/tx/wfg_m110_tx.exe 600 long build/TEST001.WAV --file message.bin
build/host-release/host/tx/wfg_m110_tx.exe 1200 short build/TEST002.WAV "exact message text"
```

The positional rate/interleave/output/text-or-file interface follows the donor
`tx_to_pcm` tool. Supported rates are 75, 150, 300, 600, 1200 and 2400 with `short`
or `long`, plus 4800 with `zero`. Text arguments are joined with one ASCII space;
use `--file` for an exact byte sequence. File input must be nonempty. The tool
supports raw `.pcm` output for offline consumers; board playback uses `.WAV`.

For `TEST001.WAV`, the utility retains `TEST001.BIN` and `TEST001.JSON` beside it.
The BIN contains the exact payload, including for text input. An input already
named `TEST001.BIN` is reused without truncation. A payload file that aliases the
WAV or JSON output is rejected. Host outputs use temporary files and publish the
manifest last; existing output artifacts may be replaced. Use unique names when
preserving earlier runs. Choose an uppercase stem of one through eight letters,
digits or underscores for files that will be selected by the board protocol.
The `.JSON` sidecar uses the enabled FatFs long-filename support.

Copy the WAV, BIN and JSON together to `/WG` while the host owns the SD medium.
After the existing media handoff to `MEDIA LOCAL`, use `LOAD:TEST001.WAV` and
`PLAY` through the [live controller](phase4-live-control.md). Generation itself
does not initiate playback.

## Board artifact creation

The board supports a payload already staged as `/WG/PAYLOAD.BIN` and a binary
payload uploaded over the existing DD008 framing. All filesystem operations and
encoder steps execute in the player task. Generation is refused while playback
is busy; the host cannot own the SD medium during upload or generation.

With the payload already on SD, the generic WFG-LIVE/1 command is:

```text
ENCODE M110B 600:long PAYLOAD.BIN TEST003.WAV
```

`GENERATE:600:long:PAYLOAD.BIN:TEST003.WAV` is the M110 convenience spelling.
Poll the file-generation status until complete before loading the WAV. A command
acknowledgement means the request was accepted, not that the artifact is finished.

For USB payload upload, first complete the media handoff and close the live
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
and retrieve the referenced payload, WAV, JSON and host journal. Verify their
recorded hashes against the retrieved bytes. SD retrieval and analog playback of
this new path remain hardware acceptance work.

## Encoder and artifact interfaces

`waveform-source/encoder.hpp` defines a sequential `ByteSource` and an `Encoder`
implementing the existing `signal_lab::SampleSource` PCM interface. An encoder
accepts an opaque profile, a payload byte source and its declared byte count. It
must provide the exact waveform frame count and produce canonical 48 kHz mono
PCM16. It also reports errors and supports stopping. The generic writer owns the
artifact's default 48,000-frame trailing silence.

`waveform-source/wav_generator.*` writes a known-length 44-byte RIFF header and
then at most 2,048 frames per `step()`. It hashes payload, complete WAV and PCM
region incrementally. Digests become available only after successful completion.
No whole-message payload or waveform buffer is required. Its byte sink abstracts
host files and FatFs equally; the owner handles publication and failed files.

`native-m110/` implements the M110B adapter. It retains one information/interleaver
block, one encoded segment and a small audio window, with continuous framing/FEC
state across blocks. Host `sizeof(Source)` is 97,232 bytes; host writer workspace
is 8,624 bytes. These sizes are independent of payload duration. The firmware
composition reserves an aligned 128 KiB encoder workspace in OCRAM and constructs
the adapter explicitly there. Embedded sizes are checked by the build.

`rt1170/encoder_catalog.cpp` is the composition point for encoder IDs, versions
and factories. Another encoder implements this interface and registers its own
ID, profile syntax and workspace factory. The generic SD utility, WAV writer,
impairment engine and player need no modulation-specific branches. The only
registered encoder is currently `M110B`; M110D/JT8 implementations are future work.
The generic writer test uses a separate fake encoder and links without native
M110 code.

## Artifact identity and validation

Both manifests identify the retained payload and its SHA-256, the WAV SHA-256,
PCM-region SHA-256, encoding settings, generator version and build source-manifest
SHA-256. Paths are relative to the sidecar directory. The schemas differ:

| Producer | Schema | Additional metadata |
| --- | --- | --- |
| PC utility | `wfg.native-m110-artifact/1` | M110 mode, rate/interleave, framing counts, file sizes, PCM byte offset, build ID and donor revision. |
| Board utility | `waveform-artifact/1` | Generic encoder ID and opaque profile, total frames, trailing silence and `complete: true`. |

`host/m110_score.py` accepts uppercase or lowercase JSON sidecars, resolves
relative payload paths and translates a generic board `M110B` profile into its
M110 mode for scoring. The waveform-agnostic Python renderer preserves the
encoder/profile identity in an impaired sidecar's `source_reference`; only the
M110 scorer interprets that profile. A different encoder ID is not treated as
M110. Decoder execution remains an explicitly supplied external tool.

The retained TX donor closure is recorded in `source-imports.json`, entries
`m110-027` through `m110-045`; DD008 imports are `m110-046` through `m110-050`.
These entries pin donor revision `b20ae7dfab068937f438a10bd37c73429f54e591`, selected
file state, donor hashes and local hashes. Builds use only the local copies. The
donor floating-point modulation math is unchanged, so host/target libm differences
still require a measured artifact comparison before claiming byte identity.

Host validation covers all 13 supported mode combinations against the locally
retained donor whole-message oracle, multi-block framing, arbitrary read sizes,
short payload/sink failures, SHA-256 vectors and artifact publication. The native
utility also regenerated the existing 600L PN-11 reference with 5,808,000 total
frames exactly: WAV SHA-256
`9d45feabbe425bebeb7174ebd0586a6ef21348ce02fb0acc888fa896d6d44567`.
The local comparison is retained in
`build/native-m110/saved-reference-comparison.json`.

```powershell
ctest --test-dir build/host-release -R 'wfg-(native-m110|waveform-source)' --output-on-failure
python scripts/verify-standalone.py --root . --check-provenance
cmake --build --preset rt1170-player-release
```

Host tests and an RT1170 link establish software and build evidence. They do not
establish that the new USB upload, on-board encoder, SD publication or analog
playback path has been exercised on hardware, nor formal waveform conformance.
