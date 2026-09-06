# Phase 1 implementation and qualification contract

Contract revision: P1.0 / 1, 5 September 2026. This contract freezes interfaces,
recipes, and acceptance procedures. It does not claim a working WAV player,
capture harness, generated M110 vector, or qualification result. The adjacent
P1.1 build checkpoint compiles a stopped scaffold and an optional resident tone;
neither implements SD/WAV playback. Later changes to a frozen contract require a
reviewed revision and compatible schema version.

Execution order was revised on 5 September 2026 without changing the final
frozen interfaces. P1.2 now exposes the original-EVK microSD card through a
temporary composite CDC-plus-MSC device, uses small CDC media-mode commands to
hand exclusive ownership to firmware, plays one fixed `/WG/PLAY.WAV`, and stops;
a small host tool performs the minimum WAV/WGM staging check. P1.3 adds the SDRAM
ring and long-duration refill/EOF behavior, P1.4 replaces the temporary commands
with WFG/1 and full ARM validation, P1.5 hardens sustained playback and re-arm,
and P1.6 adds host capture/orchestration. Full schema/provenance/M110-vector
packaging and preflight formerly planned first move to P1.7. P1.8 runs the
qualification ladder and still requires the one-hour direct clean gate before
impaired campaigns.

The temporary P1.2 CDC control accepts exactly `MEDIA HOST`, `MEDIA LOCAL`,
`PLAY`, and `STATUS`, each followed by LF; CRLF is also accepted. It returns one
bounded, newline-terminated `OK ...` or `ERR ...` line. `MEDIA HOST` exposes the
microSD LUN only while FatFs is unmounted and playback is idle. When media is
uninitialized or faulted, that same command is also the explicit retry trigger:
it deinitializes any partial SD host state, starts one new initialization
attempt, and returns `OK MEDIA HOST INIT`; a later `STATUS` reports the outcome.
An already host-ready card returns `OK MEDIA HOST`. Before `MEDIA LOCAL`, the
host must flush and dismount the Windows volume; the command then makes MSC
report no medium and grants firmware exclusive access. `PLAY` is valid only in
local mode. `STATUS` is read-only. SCSI eject alone does not change ownership.
These commands are an engineering bridge and are replaced by the frozen WFG/1
state machine in P1.4.

The host helper requires `-HostVolumeDismounted` with `-Action Local`. This is
an explicit operator assertion after the Windows volume has been flushed and
dismounted; the CDC command cannot perform or verify that host operation. Raw
CDC clients remain responsible for the same ordering.

The temporary numeric `STATUS` mappings are:

| Field | Values |
| --- | --- |
| `media_state` | `0` uninitialized, `1` initializing, `2` host/MSC, `3` local/FatFs, `255` fault |
| `media_error` | `0` none, `1` no card, `2` host init, `3` card init, `4` geometry, `5` disk init, `6` task start |
| `msc_read_only` | `1` for the PID `0x4013` inspection image; `0` for the PID `0x4012` host-staging image |
| `player_state` | `0` stopped, `1` waiting, `2` mounting, `3` opening, `4` buffering, `5` playing, `6` draining, `7` done, `255` fault |
| `player_error` | `0` none; `1` host init; `2` card detect; `3` mount; `4` open; `5` header I/O; `6` invalid RIFF; `7` missing fmt; `8` missing data; `9` unsupported format; `10` seek; `11` SD read; `12` codec config; `13` codec level; `14` codec start; `15` underrun |

`STATUS` also exposes every P1.2 extent/read counter plus `audio_running`,
processed-block/backlog timing, and the codec/eDMA overrun, RX-error, and
TX-error counters. A `done` result requires `pcm_drained=1`, `audio_running=0`,
matching total/enqueued/submitted file frames, and zero media, SD-read,
underrun, and audio error counters.

## P1.2 Progress Record

Status on 5 September 2026: implementation and offline validation are complete,
and a replacement card now initializes, mounts read-only as exFAT in firmware,
and enumerates as a write-protected USB mass-storage disk. Live USB-loaded-card
playback is not complete because the card has no `/WG/PLAY.WAV` and the
read-only inspection run intentionally performed no staging. The RT1170 player
Release ELF has been flashed through exact probe
`0244000009b2982200000000000000000000000097969905`; the exact ELF hash is a
build artifact because the embedded build identity includes this document and
therefore changes when the progress record changes.

Offline validation after the CDC-plus-MSC integration built the player Debug and
Release images and reran the strict standalone build-evidence checks for both.
Both player audits reported `status: pass`, `translation_units: 69`,
`cmake_inputs: 98`, `dependency_headers: 3174`, `missing_coverage: []`, and
`violations: []`. Host Debug and Release contract tests passed 4/4, the
standalone verifier self-test passed 10 tests, and all RT1170 scaffold, tone,
player, and SD-only diagnostic presets built with the known linker warning that
the ELF has a RWX LOAD segment.

Subsequent P2 response/retry hardening preserved the four-command P1.2 protocol
and reran host Debug/Release contract tests plus RT1170 player Debug/Release
builds and standalone audits. The refreshed player audits reported
`status: pass`, `translation_units: 69`, `cmake_inputs: 99`,
`dependency_headers: 3176`, `missing_coverage: []`, and `violations: []`.
The hardened Release image was then programmed to the exact probe and exercised
over CDC on `COM8`. From the initial `media_state=255`, `media_error=1` state,
`MEDIA HOST` returned `OK MEDIA HOST INIT`; the next `STATUS` showed
`init_attempts` advance from 1 to 2 and return to the same no-card fault. MSC
read/write/error, ownership-handoff, player, and audio counters all remained
zero. This is live evidence for bounded retry and safe no-medium behavior. It
does not add ready-media or playback acceptance evidence beyond the live state
recorded below.

The fixed-file WAV parser now uses the same shared bounded RIFF/WAVE validator
in firmware and host tests. It requires `RIFF` size plus eight bytes to equal the
actual file length, rejects duplicate `fmt ` and `data` chunks, walks through
the RIFF end after `data` so trailing chunk structure is still validated, and
keeps all non-PCM header/fmt bytes within the 64 KiB parser bound. Host C++
tests cover valid trailing chunks plus malformed size, duplicate chunk, trailing
chunk overrun, and over-bound header cases. Player status distinguishes
`file_frames_submitted` from the post-EOF drain result using `pcm_drained`,
`eof_silence_frames`, and a separate `ring_min_pre_eof_frames` low-water mark so
normal final drain does not erase the pre-EOF SD margin.

An independent SD-only diagnostic image was added to isolate uSDHC/card/clock
behavior from TinyUSB, MSC, player, and audio transport. Its power-control mode
has no FatFs path. Its identify mode adds only read-only FatFs mount and `f_stat`
operations; neither mode has USB MSC, an SD write, erase, file write, or format
path. The SD-only build also carries bounded UART diagnostics around the USDHC
nonblocking transfer wait and caps the ACMD41 retry loop so live failures return
to the bring-up task instead of looking like an unbounded firmware stop. Those
diagnostics are compile-time gated to the `sdcard` image.

The restored live player enumerates the composite USB device as
`VID_CAFE&PID_4012`, with CDC on `COM8` and an MSC interface at `MI_02`.
`STATUS` reports `media_state=255`, `media_error=1`, `card_ready=0`,
`msc_ready=0`, `block_count=0`, `block_size=0`, `init_attempts=1`,
`host_init_status=0`, `host_detect_status=0`,
`card_init_status=4294967295`, `disk_init_status=4294967295`,
`test_ready_calls=44`, `not_ready_responses=44`, and zero MSC read/write/error
activity. Raw diagnostics report `cd_gpio3_level=1`,
`cd_cm7_gpio3_level=1`, and `cd_inserted=0`. The player therefore keeps USB
responsive and avoids `SD_CardInit`/FatFs while the DAT3 host-detect result is
removed.

The SD-only diagnostic image resolved the DAPLink VCOM by
`VID_0D28&PID_0204&MI_01` on `COM12`, then printed progress through
`SD_HostInit` success. The original NXP-helper clock path and the board's
existing 24 MHz `BOARD_BootClockRUN` USDHC1 root both reached
`SDPROBE before direct SD_CardInit` without card geometry. With bounded host
diagnostics enabled, the 24 MHz run proved that command completion events are
arriving rather than being lost in the IRQ/event path: `CMD0` completed,
`CMD8` returned `response0=0x1aa`, ACMD41 returned `response0=0xff8000`
without the power-up-busy bit, `CMD2` failed, and `SD_CardInit` returned
`1805` (`AllSendCidFailed`). Disabling host-controlled signal-voltage switching
for the SD-only image produced the same trace.

A follow-up SD-only diagnostic removed DAT3 host-detect side effects entirely by
using an application/GPIO always-present detect path, kept 3.3 V-only signaling,
and verified the full ACMD41 retry budget. With the 24 MHz USDHC1 root and
400 ms power-off/power-on delays, ACMD41 ran 1000 attempts in about 8199 ms and
never observed the power-up-busy bit. A more conservative variant using a
100 kHz identification clock and 1000 ms power-off/power-on delays also ran 1000
attempts, took about 9177 ms, ended with ACMD41 `response0=0xffffff`, then
failed `CMD2` and returned `1805`. These runs also show that the imported
`fsl_sd.c` ACMD41 helper returns its last successful host-transfer status after
exhausting the readiness loop, which lets card init proceed to `CMD2` even when
the card never reached ready.

A final read-only socket power-control diagnostic then initialized the host,
left the card in idle, and sent `CMD8` with socket power enabled. The card
responded with `status=0`, `response0=0x000001aa`. The diagnostic drove
`SD_PWREN_B` off, waited about one second, and sent `CMD8` again without
re-enabling power. That off-state command returned `status=1`,
`response0=0x00000000`, and the diagnostic reported
`power_control_result card_unresponsive_while_off`. This supports that the
observed board path is actually removing socket power for this test; it does not
by itself prove card fault, socket fault, or standards conformance. The board was
then restored to the player Release image and reset.

A replacement card superseded the earlier no-ready-media result. With physical
card detect still bypassed, `SD_CardInit` completed after six ACMD41 attempts in
about 284 ms. Read-only sector inspection reported 499,744,768 logical blocks of
512 bytes (255,869,321,216 bytes), an MBR signature, partition type `0x07` at LBA
65,536, and an `EXFAT   ` volume boot sector. Read-only FatFs mount returned
`FR_OK`; `f_stat` reported no `/WG`, `/WG/PLAY.WAV`, or `/WG/PLAY.WGM`. The
identify ELF contained `SD_ReadBlocks`, `f_mount`, and `f_stat`, with no linked
SD write, erase, file-write, or MSC symbol.

The dedicated `rt1170-player-readonly-*` image uses USB PID `0x4013`, reports
the LUN as not writable through TinyUSB, returns SCSI DATA PROTECT from its
WRITE10 callback, and does not link `SD_WriteBlocks`, `SD_EraseBlocks`,
`disk_write`, or `sd_disk_write`. The normal PID `0x4012` player retains its host
staging path as a separate build. In the live read-only run, CDC enumerated on
`COM9`, Windows identified disk 3 as `WFG RT1170 SD READ`, and Windows reported
`IsReadOnly=True`, MBR, one healthy exFAT volume, and the same byte capacity.
Read-only directory inspection confirmed that `/WG` and both fixed playback
files are absent. Firmware `STATUS` reported `media_state=2`, `media_error=0`,
`card_ready=1`, `msc_ready=1`, `msc_read_only=1`, all four initialization status
values successful, 398 successful MSC block reads, and zero write calls, written
blocks, read errors, write errors, invalid requests, player requests, or audio
activity. `host_detect_status=1` is the configured always-present result;
`cd_inserted=0` is the known unusable raw detect sample and is not physical-card
evidence.

This completes read-only identification and USB exposure of the replacement
card without formatting, erasing, or writing it. It is engineering bring-up
evidence only. Host staging, `MEDIA LOCAL`, `PLAY`, audible WAV playback,
EOF-stop verification, and missing/malformed-file live failure tests remain
unobserved. P1.2 remains in progress until a staged tone is played and heard,
its final counters are independently checked, and no MSC, SD, underrun, or audio
errors are observed.

## Scope and ownership

The fixture is a standalone original-MIMXRT1170-EVK CM7 transparent WAV player:

    frozen WAV -> microSD -> SDRAM ring -> SAI1/eDMA -> WM8960
      -> optional HFSimulator -> USB PnP capture -> external PC M110 decoder

The generator project root owns all required firmware, capture, control, build,
test, documentation, and materialized vendor source. No build or local unit test
may need M110 parent sources, a parent build, parent Git metadata, an installed
MCUX SDK checkout, or an external modem executable. Installed compiler/runtime,
OS, CMake, Python, and programmer tools are explicit workstation exceptions.
Generator Python dependencies must be locked and installed locally. No hidden
source lookup, junction, symlink, alternate Git object store, parent Python
import, or configure-time source download is allowed.

The editor boundary is the same as the build boundary. Open this directory as
the VS Code root, either directly or through `waveform-generator.code-workspace`.
The checked-in workspace and `.vscode/settings.json` set CMake's source directory
to `${workspaceFolder}`; the parent M110 folder is not an editor or configure
root for generator work.

One-time imports record donor path/revision, actual source hash and dirty state,
license/notices, local destination, transitive helpers, and local adaptations in
[source-imports.json](source-imports.json). Donor paths in this ledger are
provenance, not build inputs. Dependencies are resolved by the local lock file
and materialized under third_party during an explicit setup step. Vendor code
remains unmodified; local configuration/adapters live outside vendor trees.

Local task WFG-P1.0 establishes these contracts. Parent program traceability is
M110 WBS 8.7, with WBS 5.2 supplying donor RT1170 codec work and 8.2 owning the
broader endurance concerns. This fixture does not complete WBS 8.6, which
concerns production RT1170 modem RX and lifecycle. This reference is informative;
no parent document is a runtime input.

The PC remains the M110 receiver. Phase 1's selected route captures one continuous
analog record and then invokes a separately supplied application-chain decoder
offline. That scope does not qualify live GUI/TCP/audio scheduling. Source and
receiver may share M110 algorithms; clean recovery does not establish independent
interoperability or correctness of their common waveform implementation.

| Arm | Source | Physical channel | Eligibility |
| --- | --- | --- | --- |
| A | Clean frozen WAV | Direct to selected USB PnP input | Phase 1 baseline. |
| B | Clean frozen WAV | HFSimulator verified clean pass-through | Separate clean route qualification. |
| C | PathSim-rendered frozen WAV | Direct | Later software-channel reference; Arm A must pass first. |
| D | Clean frozen WAV | HFSimulator impaired | Later independent hardware comparison; Arm B must pass first. |

The Phase 1 run-summary schema accepts A/B clean attempts only. Supporting C/D
acceptance needs an explicit later contract/schema revision. Packaging a
PathSim-rendered source is supported by the manifest schema, without authorizing
an impaired campaign. No GPL PathSim code is imported into firmware or reused
as a utility. Cascaded PathSim plus HFSimulator impairment is outside WFG-P1.0;
it requires a separately declared study. Tone, mock capture, short decode, and
self-loopback remain engineering checks, not formal conformance dispositions.

## Document and schema contracts

- [Fixture recipes](../fixtures/README.md) describe reproducible preparation and
  validate against [fixture-recipe.schema.json](../schema/fixture-recipe.schema.json).
- [waveform-manifest.schema.json](../schema/waveform-manifest.schema.json)
  describes actual retained source packages. A recipe is not a finalized manifest.
- [run-summary.schema.json](../schema/run-summary.schema.json) describes a clean
  Phase 1 attempt, including incomplete/invalid outcomes.

Use JSON Schema Draft 2020-12 with format checking enabled, finite JSON numbers,
no duplicate object keys, UTF-8, and no NaN/Infinity. Schema validation checks
shape; the semantic/file checks below remain mandatory. All schema references
are internal and must resolve without fetching a remote schema. A path in an
artifact reference identifies runtime data relative to its enclosing package or
run, unless it is explicitly absolute. Resolve and verify it; never execute a
path from an untrusted manifest or use it as a source/include fallback.

Artifact records contain path, actual bytes, and lowercase SHA-256. Missing data
is null in an incomplete run, not a fabricated empty file or all-zero hash.
Finalized source manifests require real nonempty artifacts. The schema's
engineering_only value is intentional; a separate governed procedure owns any
formal conformance claim.

## Frozen WAV and WGM contract

Accept exactly RIFF/WAVE format tag 1, 48,000 Hz, one channel, signed little-endian
PCM16, block alignment 2, byte rate 96,000. Reject RF64, float, stereo, other
rates, compressed/extensible formats, empty PCM, and unsupported headers. Walk
chunks with checked lengths and odd-byte padding. Require exactly one fmt and
one data chunk. Skip bounded unknown chunks; reject duplicate critical chunks,
truncation, arithmetic overflow, odd PCM length, and inconsistent RIFF/file bounds.
No resampling, normalization, looping, interpolation, or crossfade occurs in the
firmware. Hash bytes exactly as stored.

For one FAT32 card, select uppercase 8.3 files under /WG/. The basename contains
1-8 characters from A-Z, 0-9, underscore, followed by .WAV. No spaces,
separators, traversal, or other extensions are accepted. LOAD:C300S.WAV binds
/WG/C300S.WAV and /WG/C300S.WGM. P1.2 provisions the card through a temporary
USB mass-storage LUN while firmware FatFs is unmounted. After the host flushes
and dismounts the volume, CDC transfers exclusive ownership to firmware and the
card is mounted read-only. No WAV upload protocol is carried over CDC.

WGM version 1 is at most 512 bytes, exactly the following eight ordered ASCII
lines, including a final LF. CR, BOM, blank/duplicate/unknown fields, or trailing
content are invalid. Decimal values are canonical: zero is 0; other values have
no sign or leading zero. Hexadecimal hashes are exactly 64 lowercase characters.

    WGM1
    wav_bytes=<decimal>
    wav_sha256=<hex>
    data_offset=<decimal>
    data_bytes=<decimal>
    pcm_sha256=<hex>
    samples=<decimal>
    level_percent=<0..100>

The JSON manifest binds the WGM file hash. ARM verifies WGM/header/lengths,
whole-WAV hash, PCM hash, and applied level. It then rewinds to PCM start and
initializes a separate playback-input hash. Hash every byte published into the
playback ring exactly once, including ARM prefill and later refill reads. The
validation scan must not contribute to this second hash. These hashes identify
digital file input, not DAC behavior or measured analog voltage.

Required semantic checks include:

1. WGM fields, WAV header, and manifest agree exactly. data_bytes = samples * 2;
   data_offset + data_bytes is within the actual RIFF/file. RIFF size plus eight
   equals the actual file length. Validate bounds before addition/multiplication.
2. Sidecar basename and card filename match. The package contains no unresolved
   or mutable artifact reference when an attempt is armed.
3. Logical headphone level maps to the recorded raw value; digital gain is one.
   The applied setting is known and matches WGM/manifest at ARM.
4. For M110, expected_bits = payload_bytes * 8 = payload artifact length * 8.
   Verify the retained payload against its recipe when one is declared.
5. All source intervals are zero-based half-open WAV sample intervals, within N.
   Leading guard, preamble, body, and trailing guard occur in order and cover
   the declared file. body includes payload and final EOM/flush transmission;
   eom_flush is a documented subinterval of body, potentially covering complete
   interleaver blocks that carry those bits. Do not invent a sample-exact EOM
   position from a decoded text log. interval_evidence identifies the producer's
   retained transmission plan and its exact rounding/guard conventions.
6. payload_dwell_seconds excludes preamble, guards, and EOM/flush-only duration.
   Establish it from payload bits/rate and the retained transmission plan. The
   hour recipe requires at least 3600 seconds of continuous payload dwell, not
   one hour of file length or a sum of separate bursts.
7. An external PathSim package binds its clean source, generation manifest,
   actual simulator binary/arguments, and rendered WAV. It is a separate source
   arm, not a firmware capability or an independent hardware observation.

## Runtime ownership and failure behavior

Start with a 1 MiB PCM16 SDRAM ring: 524,288 samples, about 10.92 seconds. Use
32 KiB sequential read requests reduced at EOF. Refill below half occupancy
toward full. ARM prefills the ring or the complete shorter file. An intentionally
smaller final request is normal; a return shorter than that request is a fault.
Record actual capacities, thresholds, request sizes, and configured timeouts.

Three permanent owners serve storage/validation, audio, and CDC/control. Audio
has highest task priority. Stacks, queues, buffers, and descriptors are bounded
and allocated before playback. No filesystem work, allocation, I2C write,
hashing, formatting, or logging runs in the audio ISR/hook. Every in-flight SD
operation has a timeout; STOP is observed between bounded operations. Tag read
completion with its arm generation and discard stale completions after cancel.

SD bounce buffers/descriptors and SAI buffers satisfy their actual DMA/cache
requirements. The CPU-owned SDRAM ring is separate from DMA storage. Verify real
SDRAM linker placement, initialization, MPU/cache attributes, and cold boot.
Validate signed PCM16-to-wire packing against codec/SAI word width before using
it; preserve unity amplitude without a float round-trip.

Use TX ownership/completion and primed finite descriptors. Associate each buffer
with generation, source start, and valid sample count. A missed refill must not
silently replay a circular stale buffer. Observe FIFO/eDMA faults and deadlines;
one callback is not proof of one consumed descriptor when interrupts coalesce.
After the last file sample, count final-block silence separately, drain hardware,
then complete. Submission/file consumption alone is not EOF completion.

On underrun, read error, or audio fault, latch the first cause and affected
position, suppress further valid-data output, and silence/stop at the earliest
verified safe boundary. If exact position is unavailable, report the uncertainty
interval. Never invent a precise sample number or recover within the same run
and mark it valid. An operator STOP is ABORTED. Deliberate failure tests are
expected to produce invalid/aborted playback attempts; their separate test-case
verdict establishes whether failure handling worked.

## WFG/1 CDC protocol

Use one ordered USB CDC byte stream, no DD-008 framing and no waveform data.
115200/8-N-1 is the host convention; baud does not define USB playback timing.
Discover by USB identity/serial and verify INFO identifies waveform-player
before any mutating command. A historical COM number is not identity.

Each request is one canonical positive decimal uint32 sequence, one ASCII space,
then one command, then LF; CRLF is also accepted. No other whitespace or lowercase
command spelling is accepted. Maximum length is 192 wire bytes including LF
(and CR when present). Discard an overlong line through LF without executing it.
Sequences strictly increase within a CDC connection; do not reuse/retry a
sequence. Start a new connection before uint32 exhaustion. Reject a duplicate
or decreasing sequence with BAD_REQUEST; no command executes. A syntactically
valid sequence is consumed even when its command is rejected. An unparseable
sequence returns seq 0, which never acknowledges a valid request.

Only one host request is outstanding. Firmware retains one bounded pending
request and one response of at most 4096 UTF-8 bytes including LF. Stop draining
CDC input when reply storage is occupied; allow the USB stack's bounded endpoint
backpressure rather than dropping responses or blocking audio. Retain partial
USB writes and never interleave records. Unsolicited output is prohibited on
this endpoint; the host polls STATUS once per second.

Every response is a JSON object with exactly the envelope fields seq, protocol,
ok, state, run_id, plus either one command-specific data field or error.
protocol is WFG/1; state is the current state; run_id is null before the first
ARM and thereafter the most recently accepted run ID until the next ARM.
Successful queries use info, status, or counters. Successful mutations use
result. Errors use an object error with code and message; message is diagnostic
ASCII of at most 160 characters. Invalid commands do not change playback state.

| Command | Successful result and transition |
| --- | --- |
| INFO? | info object; allowed in all states. |
| LOAD:<name>.WAV | result = {selected_file: name}; IDLE/LOADED/DONE/ABORTED or safely stopped FAULT -> LOADED. Selection is not validation. |
| LEVEL:<percent> | result = {percent, raw, applied_known: true}; same stopped states as LOAD. A selected file becomes LOADED; with no selection remain IDLE. Complete both channel writes before acknowledging. |
| ARM:<run_id> | 32 lowercase hex digits, unique per attempt. LOADED only -> ARMING; immediate result = {accepted: true}. Validation/prefill later changes state to ARMED or FAULT. |
| PLAY | ARMED only -> PLAYING; result = {accepted: true}. Start that primed generation once. Repeated PLAY is BAD_STATE, not restart. |
| STOP | result = {stop_pending: boolean}. ARMING/ARMED/PLAYING/DRAINING -> STOPPING -> ABORTED; repeated STOPPING remains pending. IDLE/LOADED are unchanged; terminal states retain their result. Preserve a pre-existing FAULT. |
| STATUS? | status object; allowed in all states. |
| COUNTERS? | counters snapshot; available after the first accepted ARM, including after terminal completion. Before that return BAD_STATE. |

All states are IDLE, LOADED, ARMING, ARMED, PLAYING, DRAINING, STOPPING, DONE,
ABORTED, FAULT. Armed configuration is immutable. To re-arm, STOP if necessary,
save the terminal result, LOAD again (or explicitly reapply LEVEL for the selected
file), then ARM a new ID. Bare ARM and repeated ARM while ARMING/ARMED are invalid.
LOAD/LEVEL may change current selection/state after a terminal run, but the
COUNTERS snapshot remains that previous run's immutable result until ARM.
Its snapshot_state can therefore differ from the response envelope state.

Errors are BAD_REQUEST, LINE_TOO_LONG, BAD_STATE, BUSY, BAD_FILE, BAD_WAV,
BAD_MANIFEST, HASH_MISMATCH, LEVEL_MISMATCH, SD_ERROR, AUDIO_ERROR. An asynchronous
fault appears in STATUS/COUNTERS, not an unsolicited response. The first_fault
object has code from UNDERRUN, SD_SHORT_READ, SD_READ_ERROR, SD_TIMEOUT,
CODEC_ERROR, SAI_FIFO_ERROR, EDMA_ERROR, AUDIO_DEADLINE, HASH_MISMATCH,
BAD_FILE, BAD_WAV, BAD_MANIFEST, LEVEL_MISMATCH; message is bounded as above.
Add fault codes only through an explicit contract revision.

INFO fields are kind (waveform-player), firmware_version, firmware_sha256,
source_manifest_sha256, board (MIMXRT1170-EVK), core (CM7), uid,
usb_serial, sample_rate_hz (48000), audio_clock_configuration, wire_format,
codec_level (percent/raw/applied_known/register_readback_available),
ring_capacity_samples, audio_block_frames, storage (state/card_identity), and
capabilities (array of protocol features). Identity strings are at most 128 ASCII
characters each; hash fields are 64 lowercase hex. Unknown observed values are
null. INFO configuration describes nominal/setup clocks, not measured analog
clock accuracy. The serialized response must fit the hard response limit;
truncate neither a field nor a JSON record to make it fit.

STATUS fields are selected_file, active_run_id (null when no generation is
active), validation (bytes_scanned/bytes_total/complete), source_sample_position,
tx_submitted_file_samples, tx_completed_file_samples, ring_fill_samples,
valid, first_fault, and completion_reason. Unknown positions/totals are null.
valid is null while a run is incomplete, true only for source-valid DONE, false
for FAULT/ABORTED. It is fixture validity, not the host's capture/BER disposition.
completion_reason is null until terminal, then EOF, STOP, or the first fault code.

After an uncertain mutation response, query state/run ID; never blindly resend
PLAY or ARM. Disconnect discards partial protocol records and resets connection
sequence tracking, but playback continues and its counters remain in RAM.
Reconnect reads INFO and the final snapshot. Reboot, wrong UID/run ID, or missing
final evidence invalidates the attempt. No automatic replay or host keepalive
is provided. Record disconnect/control errors without applying audio backpressure.

LEVEL sets both headphone channels. At zero raw is zero (mute). For p from one
through 100, raw = 48 + floor(((p - 1) * 79 + 49) / 99). At 70, raw is 103
(0x67). This is not calibrated volts. Digital WAV gain remains one; record DAC
volume separately. There is no claim of independent codec-register readback.
A failed write latches CODEC_ERROR and applied_known false. A later ARM needs
successful explicit reapplication or codec reinitialization; cached percentage
alone cannot clear the fault.

## Exact run counter snapshot

Counters are nonnegative uint64 JSON integers, not float approximations. A host
must parse them without losing integer precision. Samples are zero-based mono
WAV sample positions, not stereo slots. Snapshot coherently on the 32-bit MCU
using bounded ownership/publication. Before any ARM priming, initialize the new
generation's counters. Unknown/not-measured values are null with an explicit
measurement availability entry; zero is a measurement.

| Fields | Definition |
| --- | --- |
| run_id, snapshot_state, firmware_identity, level | Attempt/build/applied-configuration identity. |
| expected_samples | Validated N from manifest/header; null until known. |
| audio_requested_samples, primed_file_samples | Requested frame slots including pre-PLAY priming and non-file silence; the second field identifies primed valid source samples. |
| file_samples_delivered | File samples removed from ring into audio buffers, including priming. |
| tx_submitted_file_samples | Samples credited only when hardware starts their generation after PLAY. |
| tx_completed_file_samples | Samples consumed by hardware; state completion precision and final FIFO drain. |
| startup_silence_samples, tail_silence_samples, fault_silence_samples | Non-file output slots, not part of N. |
| ring_fill_samples, ring_capacity_samples, ring_min_samples, ring_max_samples | Occupancy and PLAY-to-terminal extrema. Natural EOF drain to zero is not starvation. |
| ring_min_before_eof_samples | Minimum refill headroom while unread source remains, excluding terminal drain. |
| underruns, first_underrun_sample | Count and first missing position; index null without an underrun or exact observation. |
| sd_validation_read_calls, sd_validation_bytes_read, sd_validation_max_read_us | Header/hash validation scan only. |
| sd_read_calls, sd_bytes_read, sd_short_reads, sd_read_errors, sd_max_read_us | Playback-input reads, including ARM prefill and later refill, excluding validation scan. |
| codec_errors, sai_fifo_errors, edma_errors, audio_deadline_misses | Distinct setup/runtime faults; setup faults cannot disappear at PLAY. |
| audio_max_service_us, audio_max_wake_latency_us, audio_max_backlog | Callback work, completion-to-service latency, and unserviced ownership backlog; actual block period accompanies them. |
| first_fault, first_fault_sample, first_fault_sample_upper_bound, position_exact | First cause and exact position or explicitly bounded uncertain interval. |
| elapsed_us, first_tx_tick, last_tx_tick, clock_source, clock_hz | Monotonic device timing. Extend wrapping hardware counters safely; one hour cannot use one subtraction of a 32-bit DWT value. |
| usb_disconnects, control_errors, control_queue_high_water | Control-path health, independent of audio. |
| storage_stack_min_free, audio_stack_min_free, control_stack_min_free | Minimum free stack bytes, with availability and configured capacities. |
| validation_wav_sha256, validation_pcm_sha256, playback_pcm_sha256, sidecar_sha256 | Identity values; partial hashes have complete=false and are never substituted for complete hashes. |
| hashes_complete, hardware_drain_complete, completion_reason, valid | Terminal proof and fixture-only validity. |
| measurements | Map from field name to {available, unit, source}; no unsupported diagnostic is reported as a measured zero. |

Represent each hash as {hex, complete}, where hex is null before any valid digest
exists. firmware_identity contains version, binary hash, and source-manifest
hash; level contains percent/raw/applied_known and DAC volume configuration.
DONE requires full hardware drain, complete matching hashes, zero source
error/deadline counters, and N = delivered = submitted = completed. Hashing must
complete before DONE, including all prefetched bytes. Preserve the final snapshot
until the next ARM and save it before issuing that command. USB-only interruptions
do not automatically invalidate source playback if its final proof is retained.

## Host capture contract

The local waveform_capture owns a single explicit WinMM input. Use the imported
capture mechanism, bounded callback-to-writer queue, and a separate disk writer.
Do not place disk I/O or formatting in the callback. No allocation grows with
duration. WinMM is opened with the requested native PCM16 stereo, 48 kHz format;
each stereo frame is converted to one float32 sample for the streamed WAV and
metrics using an explicit `left`, `right`, or `average` channel policy. The
default policy is `left`, which preserves a physical mic-in channel when the
two endpoint channels have opposite polarity. One hour is about
691.2 MB before pre/post-roll; check free space and RIFF limits before opening.

The optional `wasapi-raw` backend resolves the exact active endpoint by unique
friendly name, retains its stable endpoint ID, sets
`AUDCLNT_STREAMOPTIONS_RAW`, and uses an event-driven `IAudioCaptureClient`.
It reports and accepts only the endpoint's actual 48 kHz PCM16 or IEEE-float
mono/stereo mix format; it does not silently resample. It records data
discontinuity, silent-packet, and device/QPC-position counters.

The planned CLI is:

    waveform_capture --backend winmm --list-devices --json
    waveform_capture --output <capture.wav> --device-index <index>
      --sample-rate 48000 --channel left --stop-stdin --max-seconds <safety-deadline>
    waveform_capture --backend wasapi-raw --output <capture.wav>
      --device-name <exact-friendly-name> --sample-rate 48000
      --channel left --stop-stdin --max-seconds <safety-deadline>

Enumeration returns JSON device records with index, backend, native_name,
endpoint_id, pnp_instance_id, max_input_channels, requested_input_format, and
conversion, and default_channel. Unknown identity fields are null; support is established by the
open-time WinMM format query, rather than claimed for every device. The harness
resolves exactly one intended physical input, verifies the selected open-time
identity, and retains native names/index, sample format, capture level,
enhancement/AGC state, and ownership. First-match prefixes, default endpoints,
and first non-numbered USB PnP heuristics are not valid selection. If an
endpoint cannot be uniquely established, do not run.

The recorder emits JSON Lines on stdout and diagnostic text on stderr; retain
both. First READY is emitted only after the output/header and writer are ready,
audio start succeeds, and the first capture callback is observed. Its fields
are record=READY, capture_id, endpoint, format, monotonic_ns, samples_delivered,
samples_written, queue_capacity_samples. No PLAY occurs before READY plus the
requested pre-roll. Capture IDs and run IDs are explicitly bound by the host.

STOP followed by LF on stdin requests controlled stop. EOF on stdin also stops
and records HOST_DISCONNECTED; it is not a successful qualification finish.
Stop device delivery, drain queued captured samples, finalize the WAV with the
actual written sample count, and emit one FINAL. A deadline expires with
DURATION_LIMIT and invalidates incomplete source coverage. Error exits must
still attempt to finalize and retain the partial WAV; a failed finalization is
recorded, not hidden. No fault recovery or automatic recorder restart joins
separate sessions into one capture.

FINAL fields are record=FINAL, capture_id, completion_reason, endpoint, format,
samples_delivered, samples_written, queue_overruns, write_errors, dropped_blocks,
out_of_order, partial_buffers, reset_buffers, reset_partial_buffers,
tail_samples, maximum_ready_batch, maximum_queue_samples, clipped_samples,
first_callback_ns, last_callback_ns, stopped_ns,
driver_discontinuity_detection, data_discontinuity_packets,
startup_discontinuity_packets, silent_packets, position_packets,
position_errors, first_device_position, first_qpc_position,
last_device_position, last_qpc_position, and measurements. Timing uses a named
monotonic clock. Error counters not instrumented are null, with availability false. Save
driver diagnostics rather than silently treating callback scheduling gaps as
ADC loss. Distinguish normal final partial delivery from unexpected interior
partial buffers in the retained diagnostics. An unexplained interior partial
buffer invalidates the run. The first WASAPI startup discontinuity is retained
separately and does not invalidate the run; later discontinuities invalidate it.
Clipping counts samples with absolute float magnitude
at least 0.999 before any capture-side scaling; retain non-finite sample counts
and invalidate if nonzero.

Exit zero means controlled STOP with complete file finalization and no recorder
integrity fault; one means recording fault/limit/disconnect, two means argument
or setup rejection. A zero exit code alone is not an audio or modem pass.

## External M110 producer and decoder contracts

M110 generation/decoding remain separately delivered executables. The host
adapter receives explicit executable and artifact paths; it does not compile,
download, discover, or import M110. Record executable/source identities and
actual argv. Generic local tone/PN and mocked adapter tests need no M110 install.

The first producer/payload recipes are specified in fixtures/README.md. Record
actual WAV/transmission-plan counts and preflight the full-duration frozen
input. Current producer/decoder implementations retain whole-waveform or
whole-burst workspaces; measure time/peak memory on the full vector. Any required
length/memory changes belong to the external producer/DUT project and require
re-pinning its binary. Build success or a larger command-line cap is not proof
that a full-hour decoder invocation can finish.

The decoder adapter contract m110_app_decode_text_v1 invokes:

    <explicit-decoder> <capture.wav> --engine siso --expect-file <expected.bin>
      --maximum-payload-octets <manifest-payload-bytes> --maximum-clipped 0

No --single-burst option is used: inspect the complete capture and account for
extra transmissions. Freeze all arguments; do not enable forced mode, supervised
reference decoding, or oracle-assisted equalization. Expected payload is used
for post-decode comparison only. This command is an external tool interface,
not a generator source dependency.

Retain stdout/stderr and exit status. Recognize only complete declared record
lines, bound parsing by the run's expected payload/capture sizes, and reject
truncated, contradictory, duplicate, or malformed required output. Per decoded
burst require a declared mode, selected equalizer engine, exactly one primary
payload_hex record, eom=found/not-found with eom_bit_errors, and exact_payload
with expected_bytes/received_bytes/bit_errors/compared_bits/missing_bytes.
The terminal summary contains decoded_bursts, exact_payloads,
required_exact_payloads, clipped_samples, maximum_clipped_samples, audio_quality.
Diagnostic stream or turbo probe payloads are not primary payload records.

Decode even-length hexadecimal primary payload records into decoded.bin in
burst order; retain burst boundaries in ber.json. Do not use payload_ascii.
Recompute counts and bit differences from retained bytes, then reconcile them
with decoder records and the terminal summary. Missing records or count
contradictions are invalid evidence. A valid complete zero-burst result is an RX
failure, not BER zero. If the selected executable lacks this output contract,
declare a separate external DUT-tool prerequisite; do not import its source.

For a single expected transmission, compare aligned bytes from the beginning,
with no substring search, leading-byte removal, cyclic PN alignment, or best
shift. Report compared_bits = 8 * min(expected_bytes, decoded_bytes), bit errors
as XOR-popcount in that region, missing_bits and extra_bits separately. BER is
null for zero coverage and otherwise bit_errors/compared_bits. Passing requires
expected_bits = decoded_bits = compared_bits > 0, zero bit/missing/extra errors,
exactly one declared-mode transmission, exact EOM, and a zero decoder exit.
A decoder's own common-prefix BER or minimum-exact count alone cannot pass.

The live PC host's current 65,536-octet cap cannot carry the 135,000-octet 300L
hour. That live route is outside this offline gate; do not relabel offline work
as live scheduling acceptance or substitute a different mode/many short bursts.

## Levels, clocks, and acceptance lock

Record source PCM amplitude, unity digital gain, both headphone/raw settings,
DAC configuration, analog load/channel/cabling, HFSimulator knobs, and capture
level/enhancement/AGC state. Calibration candidates are not qualified settings.
No clipping is allowed; also establish useful noise-floor and level headroom.
Any hidden/unknown configuration that prevents a repeatable calibrated run is
an unresolved gate, not a default value.

Use a retained PN/timing signal with identifiable features across the record.
Fit captured feature position y against source position x: y = a + b*x.
relative_ppm = (b - 1)*1,000,000. Positive means more captured sample positions
per source-sample interval. Record source/capture intervals, fit method,
correlation confidence, residual samples, and abrupt lag-jump indicators.
Pure tone ambiguity or low-correlation windows cannot supply an exact drift
bound. Do not resample the capture to hide measured drift. Independent clocks
mean source and captured interval sample counts need not be equal. Relative
clock agreement is not absolute oscillator calibration.

P1.0 freezes the method, not an invented ppm limit. After calibration, retain a
calibration JSON artifact with schema_version=acceptance-lock/1, locked_utc,
route, source/capture/firmware identities, applied levels, no_clipping=true,
max_abs_relative_ppm, max_correlation_residual_samples,
minimum_correlation_confidence, maximum_abrupt_lag_jump_samples, minimum_peak,
maximum_peak, minimum_rms, and rationale/evidence artifact references. All limits
are finite and justified from calibration, selected RX tolerance, and bench needs.
Lock this before the intermediate/hour runs, not after seeing their results.
The run-summary copies max_abs_relative_ppm and max_correlation_residual_samples
from that lock; verify the copies agree.

## Artifact and disposition contract

Each attempt uses a new artifacts/runs/<UTC>-<arm>-<run-id>/ directory. Retain:

    manifest.json / source-location.json / source.WGM / expected.bin
    fixture.jsonl / fixture-final.json / host.jsonl / environment.json
    capture.wav / capture.json / decoder.log / decoded.bin / ber.json
    audio-metrics.json / summary.json / hashes.json
    hf-transcript.jsonl when the HFSimulator route is used

The source WAV remains in a durable hash-addressed package with a resolvable
location. environment.json binds local firmware/capture binary hashes, source
manifest, optional Git/dirty-source facts, compiler/flags, dependencies,
external producer/decoder hashes/argv, device identities, card identity, route,
settings, and calibration. Build outputs are disposable; artifacts are not.

Hash finalized evidence only. Finalize capture/decoder/metrics, then summary,
then hashes.json. The inventory includes summary.json but excludes its own hash;
summary holds the inventory path only, avoiding a circular hash dependency.
Each inventory item records path, bytes, and SHA-256. Verify referenced durable
external sources/binaries separately and bind their hashes. Preserve failed,
aborted, and invalid attempts without overwriting them.

Dispositions are pass, fail, invalid, operator-aborted. Pass requires all
applicable integrity/acceptance checks. Fail means a valid test measured an RX
acceptance failure. Invalid means source/capture/configuration or essential
evidence is faulty/unavailable; operator-aborted means intentionally incomplete.
An observed integrity fault takes precedence over operator abort, which takes
precedence over any incomplete RX result. Preserve all reasons. Null evidence
and not_observed checks are valid representations of an invalid attempt.

The run-summary schema supplies structural pass prerequisites. Its semantic
validator must additionally verify all file hashes/references, same run/UID,
source sample counter equality, measured diagnostic availability, full-stream
bit arithmetic, mode/transmission/EOM agreement, calibration locked before PLAY,
and every numeric acceptance comparison. An unknown required check cannot pass.
For Arm B, a retained HF transcript may contain explicit manual-setting records
when command automation is unavailable; it must establish the clean state and
knob settings. Missing clean-state evidence makes that route invalid.

## Ordered qualification and completion

1. Reproduce a local imported tone checkpoint, then play the frozen 10-second
   tone from SD directly to the unique USB PnP input. Verify actual pitch/rate,
   channel, useful level, clipping, distortion/noise, EOF/drain, and source ledger.
2. Repeat the tone through independently documented HFSimulator clean
   pass-through. Keep Arm B separate. Unresolved serial flooding can block B
   automation without blocking direct Arm A work.
3. Run several minutes of PN/multitone with unambiguous timing features. Measure
   clocks, capture continuity indicators, SD latency/headroom, noise, and gain;
   freeze the acceptance lock before subsequent gated duration work.
4. Exercise storage/audio delays, read error/card removal, STOP/partial final
   block, cold boot, repeated arm/play, and slow/disconnected CDC. Verify latched
   failures, generation cancellation, bounded silence/stop, and retained results.
5. Replay the frozen short 300L transmission directly; capture before PLAY,
   decode afterward, and require exact full payload/count/mode/EOM. Repeat to
   expose re-arm errors. Separately repeat short clean Arm B.
6. Run approximately ten continuous clean minutes in Arm A using the final
   capture/harness/polling/acceptance lock. Verify sustained writer/source health,
   measured drift, and complete offline decode before an hour.
7. Run the frozen 300L hour: one preamble, at least 3600 seconds of continuous
   payload dwell, one EOM, no file loop. Start capture, wait for READY and two
   seconds pre-roll, PLAY, poll once per second, wait for source-valid DONE,
   retain two seconds post-roll, finalize, decode, compare, and seal. Require
   all source/capture/level/drift/evidence checks and BER zero with full coverage.

At 48 kHz, 3600 seconds alone is 172,800,000 samples/345,600,000 PCM16 bytes.
The hour recipe's full WAV includes extra framing/tail samples; actual validated
N owns accounting. Do not count silence as payload dwell.

Arm A's hour must pass before impaired work. Arm C may then proceed under its
later contract; Arm D also requires a full-duration clean Arm B baseline. Keep
all routes, builds, vectors, levels, and resulting claims distinct.

P1.0 acceptance is a consistent portable contract/schema/recipe package, reviewed
import/dependency provenance, and explicit unresolved calibration/hardware
prerequisites. The full Phase 1 exit additionally requires implemented local
firmware/harness, focused failure tests, independent build/extraction evidence,
and a retained passing one-hour direct run. None is implied by P1.0 alone.

## Independent build gate for the next stages

Resolve every source/header/linker/CMake/generated-input/script path and audit
compile dependencies as well as target declarations. Every project/vendor input
must be within the generator; standard explicit toolchain/OS facilities are the
only source exceptions. Audit Python imports and subprocess paths as well.

Copy only the generator plus its materialized dependencies into an unrelated
empty directory, without parent Git metadata or existing build outputs. This is
an extraction check, not a Git worktree. Clear parent SDK/repository environment
variables; configure fresh caches and build RT1170 Debug/Release plus host
contract targets offline for P1.1. Run local format/protocol/host tests without
boards or M110. Once P1.6 implements host capture, the final repeated Phase 1
gate also builds and tests that executable from the extracted root.
Verify build identity from the source manifest without Git. Then test the
separately configured M110 integration with explicit executables/artifacts.
Retain source inventories, resolved paths, dependency hashes, compiler flags,
maps, and extraction logs. Repeat this gate after the final implementation.
The extracted directory must also open as the only VS Code workspace folder and
retain the same `${workspaceFolder}` CMake binding.

P1.0 evidence alone establishes document/schema/recipe portability. P1.1 build
and extraction evidence is recorded separately, and neither can satisfy the
physical board, capture, decoder, or one-hour gates.
