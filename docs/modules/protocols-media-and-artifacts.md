# Protocols, Media Ownership, and Artifacts

## 1. Scope

This guide covers the common command grammars, CDC response framing, DD008
payload transfer, USB MSC exposure, exclusive SD ownership, and WAV artifact
generation on RT1170.

## 2. 50,000-foot view

The composite player exposes two kinds of access to one board:

- CDC carries bounded commands, acknowledgments, status, and DD008 upload data.
- MSC lets Windows access the microSD card as a removable volume.

Windows MSC and firmware FatFs must never own the card at the same time. The
protocol is therefore partly an ownership state machine, not just a list of
commands.

## 3. How to use it

Use the normal player PID `0x4012` for host staging and local generation. Use the
read-only player PID `0x4013` when the card must remain write-protected. Verify
USB serial in both cases.

Safe ownership sequence:

```text
MEDIA HOST -> Windows mounts volume -> stage files -> flush/dismount volume
-> MEDIA LOCAL -> LOAD/GENERATE/PLAY -> STOP/completion -> MEDIA HOST
```

Example status query:

```powershell
python host/live_control.py --port COM42 --expected-serial SERIAL `
  --record status.jsonl command "INFO?" "STATUS?"
```

Example target generation from an SD payload:

```powershell
python host/tx_upload.py --port COM42 --expected-serial SERIAL `
  --sd-input INPUT.BIN --output TEST001.WAV --mode 600L `
  --record generation.jsonl
```

Example USB payload upload:

```powershell
python host/tx_upload.py --port COM42 --expected-serial SERIAL `
  --payload message.bin --output TEST002.WAV --mode 1200S `
  --record upload.jsonl
```

## 4. Scientist and maintainer view

### Wire contracts

`WFG-LIVE/1` uses newline-delimited, sequence-numbered requests and JSON responses.
The maximum request is 192 bytes and the response buffer is 4096 bytes. Responses
must remain complete JSON under partial USB writes. A disconnect discards a partial
line and resets the connection-local sequence state.

DD008 uses a separate binary COBS-framed session on the same CDC endpoint. A new
connection selects one protocol; clients must close and reopen rather than switch
mid-record. Payload and control channels remain bounded and checksum-protected.

### Media ownership

`MEDIA HOST` allows MSC access only when playback, upload, and generation permit
it. `MEDIA LOCAL` means the operator has already flushed and dismounted the host
volume. The command cannot flush Windows caches; the host utility requires an
explicit assertion to prevent the command from pretending it did.

The target media state, `msc_ready`, write-protection flag, eject count, ownership
handoffs, read/write counters, and errors are part of the retained status record.

### Artifact lifecycle

Target generation states are idle, receiving payload, creating WAV, complete, and
failed/aborted. The player task advances generation in bounded blocks so it does
not monopolize the owner loop. A completed artifact records payload bytes, frame
count, output filename, WAV SHA-256, encoder identity/profile, and error state.

The read-only image excludes the local artifact writer and rejects MSC writes
before an SD write routine can run.

### Failure behavior

Do not automatically retry uncertain mutations. Do not treat an uploader exit
code as proof of retained bytes; inspect terminal artifact status and hashes. Do
not infer media safety from a drive letter disappearing without confirming the
board's ownership response.

## 5. 5th-grader view

The SD card is one notebook shared by Windows and the board. Only one may hold the
pencil. `MEDIA HOST` hands the pencil to Windows. After Windows closes the book
and safely ejects it, `MEDIA LOCAL` hands the pencil to the board.

CDC is the intercom used to ask for the pencil or request a waveform. MSC is the
desk where Windows can see the notebook's files.

## 6. Toy worked example

Start in host state:

| Step | Owner | Allowed action | Unsafe action |
|---|---|---|---|
| 1 | Host | Copy `PLAY.WAV` | Firmware opens FatFs file |
| 2 | Host, flushing | Dismount the volume | Pull ownership while a copy is pending |
| 3 | Local | `LOAD`, `GENERATE`, `PLAY` | Windows writes the volume |
| 4 | Local, stopped | Close files and audio | Request host during generation |
| 5 | Host | Mount and inspect hashes | Assume the same historical drive letter |

If a command times out between steps 2 and 3, the owner is unknown. Reconnect and
query status; do not repeat `MEDIA LOCAL` merely because no acknowledgment arrived.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Live text protocol | `common/live_protocol.*` |
| Artifact request protocol | `common/tx_protocol.*` |
| DD008 framing/session | `common/dd008/` |
| USB descriptors and CDC service | `rt1170/usb/`, `rt1170/usb_checkpoint.*` |
| Media block ownership | `rt1170/platform/waveform_msc.*`, `waveform_sd.*` |
| Artifact owner | `rt1170/tx_artifact.*`, `rt1170/tx_usb.*` |
| Host clients | `host/live_control.py`, `host/tx_upload.py`, `host/media-control.ps1` |

Protocol changes need parser tests, fragmented-write tests, disconnect tests,
capacity proofs, host validation, firmware response checks, and backward-compatible
behavior or an explicit version change.
