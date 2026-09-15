# Tutorial: add a custom waveform generator and CDC controls

## What you will build

Start from the public `TONE` module, then replace its sample producer with your
own encoder. The module supplies a stable encoder ID, an opaque profile string,
and a bounded sample stream. The shared code supplies SD ownership, WAV creation,
playback, interference and CDC responses.

This tutorial explains the extension points implemented today. Plugins register
encoders; they do not register arbitrary CDC verbs or live audio callbacks.
Most encoder controls fit the existing profile string. Adding a new wire verb
requires coordinated firmware and host-parser changes described below.

## 1. Copy the working example

From the waveform-generator root, choose an unused local directory:

```powershell
New-Item -ItemType Directory -Force private | Out-Null
Copy-Item -LiteralPath examples/encoder-plugin -Destination private/my-generator -Recurse
cmake --preset host-release -B build/my-generator-host `
  -DWFG_ENABLE_ENCODER_PLUGINS=ON -DWFG_ENCODER_PLUGIN_DIR=private/my-generator
cmake --build build/my-generator-host --parallel
ctest --test-dir build/my-generator-host -R encoder --output-on-failure
```

This initially builds the unmodified `TONE` example. The top-level `private/`
directory is ignored by the public repository. Publish the module separately
when ready, or track it as a submodule in a personal wrapper repository. See
[Encoder plugins](encoder-plugins.md) for that directory arrangement.

The module contains three functional files:

| File | Role |
|---|---|
| `wfg-plugin.cmake` | Absolute source/include paths for both host and target |
| `registry.cpp` | Static descriptors and checked placement-construction factory |
| `tone_encoder.hpp` | Example implementation of `waveform_source::Encoder` |

An optional module `CMakeLists.txt` can add host tools and tests. Link them to the
public `wfg_encoder_plugin` library. Supply that module's own include directories
to its tools. Root project warnings, no-exception and no-RTTI settings also apply
to targets in this directory.

## 2. Define an ID and profile

Change the descriptor ID to an uppercase name such as `CUSTOM`. IDs use
`[A-Z0-9_]{1,15}` and must be unique. Update the version and factory's size and
alignment when replacing the class. The registry checks at most eight entries;
invalid registration makes the catalog unavailable.

Choose a compact, explicit grammar for the profile. For example, a hypothetical
FSK generator might define `R50:S170:F1000` to mean 50 symbols/second, 170 Hz
separation and 1000 Hz center. That example is a proposed grammar, not a bundled
FSK implementation. Its adapter must parse every field, reject duplicates,
missing/trailing data and unsupported combinations, and validate bounds before
calculating buffer sizes or frame counts.

The existing wire limits are:

| Field | Limit |
|---|---|
| Whole request | 192-byte buffer including the terminating NUL; parser input excludes LF and may end with CR |
| Encoder ID | 1..15 uppercase ASCII letters, digits or underscores |
| Profile | 1..31 printable ASCII characters, with no spaces, double quotes or backslashes |
| Input | Uppercase 8.3 `.BIN` filename, stem `[A-Z0-9_]{1,8}` |
| Output | Uppercase 8.3 `.WAV` filename, same stem rule |
| Payload | At most 1,048,576 bytes; resulting WAV must also fit RIFF length bounds |

The shared parser validates transport syntax. The module validates profile
meaning. Never treat the profile as a path, shell command, or unbounded text.

## 3. Implement the sample producer

Implement the [Encoder interface](../waveform-source/encoder.hpp):

1. `configure(profile, payload, length)` validates the complete job and declares
   its exact output length through `total_frames()`.
2. `read_float(destination, capacity)` emits finite normalized 48 kHz mono samples
   without exceeding capacity. Use bounded scratch storage and bounded work.
3. `status()` distinguishes normal completion from a short input or computation
   failure. Premature payload EOF must fail the job.
4. `stop()` cancels work and releases borrowed references. It must be safe before
   configuration and after failure.
5. Implement the legacy PCM16 read entry for interface compatibility, while
   keeping production output on the float-to-PCM24 path.

The factory constructs the encoder in caller-owned storage (maximum 128 KiB,
32-byte alignment). Check null, size and alignment before placement `new`. The
payload and workspace outlive the job. Avoid dynamic allocation and exceptions;
the caller stops and destroys the object before reusing its storage.

Generation produces a file before playback. The writer adds its one-second silent
tail and creates PCM24 samples. Do not also add that tail in the adapter's generic
`configure` entry unless your waveform deliberately requires additional silence.

## 4. Control it with the existing CDC command

Build the RT1170 player using the module:

```powershell
cmake --preset rt1170-player-release -B build/my-generator-rt1170 `
  -DWFG_ENABLE_ENCODER_PLUGINS=ON -DWFG_ENCODER_PLUGIN_DIR=private/my-generator
cmake --build build/my-generator-rt1170 --parallel
```

Load that firmware using the board's normal programming procedure. Stage a payload
while Windows owns the card, flush/dismount Windows, and complete `MEDIA LOCAL` as
described in [How to use](how-to-use.md). Resolve the current CDC port and serial.
For the unmodified example:

```powershell
python host/live_control.py --port COM42 --expected-serial OBSERVED_SERIAL `
  --record build/tone-generate.jsonl command "INFO?" `
  "ENCODE TONE 1000 PAYLOAD.BIN TONE.WAV"
```

For the hypothetical custom profile the body becomes:

```text
ENCODE CUSTOM R50:S170:F1000 PAYLOAD.BIN CUSTOM.WAV
```

The host adds monotonically increasing sequence numbers. The raw CDC request
looks like `1 ENCODE TONE 1000 PAYLOAD.BIN TONE.WAV` followed by LF. Do not send
the unnumbered body directly to CDC. `INFO?` advertises the actual registered IDs.
Unknown IDs return `UNKNOWN_ENCODER`; a build without plugins returns
`ENCODERS_DISABLED`. A read-only image returns `READ_ONLY_IMAGE`.

Generation acknowledgement means accepted, not complete. Query `STATUS?` with a
fresh journal until `tx_artifact_state` is 3 (complete), or 4 (failed/aborted).
Only then load the generated file and play it. `STOP` aborts generation; existing
outputs are preserved. Never blindly retry a timed-out file-creation command.

```text
LOAD:TONE.WAV
CW 0 FREQ 900
CW 0 CI 6
CW 0 ON
PLAY
```

These are bodies for `live_control.py command` or its interactive mode. Effects
control is independent of the source encoder. To change the generated waveform's
parameters, create another file using a different profile, then load it.

## 5. Extend controls without changing the wire protocol

Add fields to your module's profile grammar and parse them in `configure()`.
Keep the 31-character limit. A module-specific host script can translate friendly
options such as `--baud 50 --shift 170` into the profile and invoke
`host/live_control.py ... command "ENCODE CUSTOM ..."`. The existing host helper
then owns serial identity checks, framing, acknowledgement and journaling.

Neither the USB service, SD writer, nor player needs to understand those fields.
Document the profile version and defaults in the module. Retain the effective
profile in the normal artifact manifest so a file's settings can be reproduced.

## 6. Add or change actual CDC commands

For a genuinely new operation, change both parsers and the owning task together.
The implemented route is:

```text
host/live_control.py parse_command()
 -> common/live_protocol.cpp parse()
 -> rt1170/usb_service.cpp handle_control_line()
 -> submit_tx_request() mailbox
 -> rt1170/tx_artifact.cpp service_tx_artifact()
 -> Encoder::configure/read_float
```

### Add an alias for an existing generation operation

For example, a proposed `TONEGEN 1000 INPUT.BIN OUTPUT.WAV` can map to the existing
`generate_file` request with `encoder="TONE"` and `profile="1000"`. This alias
is not implemented by this project. To implement it:

1. Add the grammar to `host/live_control.py`'s `parse_command()` and
   `common/live_protocol.cpp`'s `parse()`. Apply the same filename, length and
   character checks as `ENCODE`; reject `AT:` scheduling for file generation.
2. Populate the existing request fields after validation. Bounded, zero-terminated
   copies must fit both `live_protocol::Request` and `tx_protocol::Request`.
3. Reuse `Kind::generate_file` so the current USB dispatch, ownership checks,
   artifact mailbox and error replies remain authoritative.
4. Test valid, malformed, oversized, unknown-module and plugin-disabled cases in
   the Python and C++ parser tests. Document the alias beside its canonical command.

### Add a new operation, such as querying module-specific settings

An operation that cannot be expressed as generation needs an explicit request and
response contract. Add the appropriate enum and fixed-size fields in
`common/live_protocol.hpp` and, for encoder-owner work, `common/tx_protocol.hpp`.
Route it in `usb_service.cpp`, handle it in `tx_artifact.cpp`, and extend
`tx_artifact_unavailable.cpp` to reject it consistently when disabled/read-only.
If it calls a new encoder method, define that method in the shared interface and
update every module and fake adapter in the same change.

The USB task must not read SD files, construct encoders or mutate their state
directly. It parses and submits a bounded request; the player task owns encoder
and file operations. Preserve `BUSY`, media ownership, disconnect and cancellation
semantics. Do not return pointers to temporary error strings. Keep replies within
the 4096-byte response buffer and test partial USB writes.

The current interface fixes the waveform and length at configure time. Changing
encoder parameters during a running generation job is not an implemented feature.
A new mid-job control API needs explicit frame timing, acknowledgement and length
semantics; otherwise reject it while busy and require a new job. The existing
sample-indexed CW/fade controls already change playback effects safely.

If extending the **legacy DD008 protocol**, also update `common/tx_protocol.cpp`,
`rt1170/tx_usb.cpp`, and `host/tx_upload.py`. It is a separate framed protocol on
the CDC endpoint, with fixed rate/interleave compatibility commands. Setting
`legacy_default=true` only selects a compatible adapter; it does not invent new
mode tokens or enable arbitrary custom profiles. Use `ENCODE` for new families.

## 7. Verification and documentation

Keep modulation tests with the module: profile bounds, exact sample counts,
payload short reads, output independence from block size, cancellation/reuse,
workspace rejection and PCM24 output. Test the public build with plugins disabled
as well as the module-enabled build. Add CDC grammar tests to
`tests/live_protocol_tests.cpp` and `tests/test_live_control.py`; use
`tests/tx_artifact_host_tests.cpp` and `tests/player_live_host_tests.cpp` for owner
and USB behavior. DD008 changes also need its parser/upload/USB-owner tests.

Update the module README, profile examples, public capability table where
applicable, [live protocol](live-control-protocol.md), and
[CLI reference](modules/command-line-tools.md). Build host and firmware separately;
loading, audio playback and timing on hardware require their own bench run.
