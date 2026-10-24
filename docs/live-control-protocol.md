# Live impairment control and replay

The RT1170 and RT1064 players share this `WFG-LIVE/1` control protocol and
accept sample-indexed impairment changes while playing a retained WAV. The
pipeline is:

```text
SD WAV -> scenario engine -> live controls -> output FIFO -> SAI/eDMA -> WM8960   (OUTPUT CODEC)
                                                         -> USB ISR -> UAC2 capture stream   (OUTPUT USB)
```

The live controller knows normalized float samples with 48 kHz mono PCM24 at
file/output boundaries. RT1064 codec operation is built but analog validation
remains separate; the USB route is the supported capture workflow on either
board. Optional plugin encoding is a
separate file-generation utility described in [artifact generation](artifact-generation.md).
PC-created WAVs and board-created WAVs enter the same playback path.

## Engineering protocol

`WFG-LIVE/1` is the implemented control protocol described below.

A request is `<sequence> <command> LF`, with optional CR immediately before LF.
The sequence is a canonical positive uint32 decimal that strictly increases
within the connection. Even a rejected command consumes its valid sequence.
Requests have a maximum of 384 wire bytes including LF/CRLF (INFO
`request_capacity`), room for a [`CUT`](#cut-injector) of 16 entries with
32-bit offsets after a 10-digit sequence (352 bytes). An oversized record is
discarded through LF. Invalid requests execute no playback command.

Responses are JSON objects with `seq`, `protocol`, `ok`, numeric `state`,
`run_id`, and exactly one of `info`, `status`, `counters`, `usb`, `playlist`, `engine`, `files`, `prepare`, `result`, or `error`.
The response buffer is 4096 bytes, including LF. Only one request is outstanding;
partial writes are retained, and endpoint backpressure bounds queued input.
There is no automatic retry after an uncertain mutation.

The legacy `STATUS`, `MEDIA HOST`, `MEDIA LOCAL`, `PLAY`, `OUTPUT CODEC` and
`OUTPUT USB` lines remain available for diagnostic tools; a legacy player line
answers `OK <line>` or `ERR <code>`. DD-008 binary upload uses a separate connection mode;
close and reopen the CDC port to switch between DD-008 and WFG-LIVE/1.

| Command | Behavior |
| --- | --- |
| `INFO?` | Board/USB serial, build label, commit, `usb_pid`, output route, feature list and encoder IDs. `kind` and `board` never change. |
| `FILES?`, `FILES? <offset>` | Read one page of regular files in `2:/WG/`, while the card is Local and the player is idle. Canonical unsigned uint64 offset, default 0. WFG-LIVE/1 only; INFO advertises `files`. See [card listing](#card-listing). |
| `PREPARE:<output.WAV>:<source.WAV>:<scenario.SCN>:<KEEP\|TEMP>[:<seed>]` | Calibrate and render a scenario-impaired WAV on the card before playback. Requires INFO capability `prepare`; names are uppercase 8.3 names. The optional canonical uint64 seed overrides the scenario seed. Admission is asynchronous; query `PREPARE?` for progress and hashes. `STOP` cancels the active job. |
| `PREPARE?` | Read the current or last preparation snapshot in a `prepare` object: numeric phase, static error message, source/scenario/output names, retention, seed, input/output/total frame counts and WAV/PCM SHA-256. Read-only; INFO advertises `prepare`. |
| `STATUS?`, `COUNTERS?` | Engineering player, FIFO, live output, USB sound-card stream and play (`uac_*`) and file-generation snapshots. |
| `USB?` | USB sound-card diagnostics in a `usb` object (`"format":"usb-diagnostics"`, then uint32 fields): the stream fields STATUS also reports (`uac_stream_open` .. `usb_stack_asserts`, same values), then `uac_rejected_chunks` and the miss attribution `uac_last_miss_frame_index`, `uac_last_miss_unprimed`, `uac_last_miss_play`, `uac_max_gap_frames`, `uac_watchdog_expiries`, `uac_unprimed_intervals` and `uac_play_unprimed_intervals`, which tell where the stream last missed a 1 ms interval and whether the device or the host caused it, for the stream and for the current or last USB play (see [`USB?` fields](#usb-query) and [miss attribution](modules/rt1170-player-and-audio.md#miss-attribution-usb)), then where TinyUSB's assertions failed: `usb_assert_count`, `usb_first_assert_pc`, `usb_last_assert_pc`, `usb_last_assert_frame_index` and `usb_last_assert_tick` (see [TinyUSB assertions](#tinyusb-assertions)), then the capture format of the open stream (`uac_stream_alt`, `uac_stream_bits`, `uac_stream_channels`) and of the current or last USB play (`uac_play_stream_bits`, `uac_play_stream_channels`; see [capture format](#capture-format)), then the microSD card's MSC transfer maxima `msc_max_write_us`, `msc_max_read_us` and `msc_max_blocks_per_call` and its SD bus clock `sd_bus_clock_hz` (see [MSC transfers](modules/protocols-media-and-artifacts.md#msc-transfers-and-throughput)), then the current or last play's cuts `uac_play_cut_count`, `uac_play_cut_drop_frames`, `uac_play_cut_zero_frames`, `uac_play_cut_first_index`, `uac_play_cut_rejected` and its frames sent `uac_play_sent_frames` (see [CUT injector](#cut-injector)), then its engine deadline misses `engine_deadline_misses` (see [Duration](#duration)), then the USB interrupt's EP0 order `usb_ep0_reordered` and `usb_ep0_unordered` (see [EP0 control race](#ep0-control-race-32)), and last `uac_sent_mismatches`, the IN completions reported short of the packet TinyUSB loaded for them. Read-only; WFG-LIVE/1 only (no legacy line). INFO advertises it as `usb-diagnostics`. Its reply has its own budget (1481 of 4096 bytes); STATUS stays as it is. |
| `MEDIA HOST`, `MEDIA LOCAL` | Existing exclusive MSC/FatFs ownership handshake. Host ownership is refused during playback (on either route), upload or generation. Local ownership is refused while the host still holds its volume: `HOST_WRITING` between the blocks of a host write, `HOST_LOCKED` while the host prevents medium removal (dismount or eject first). STATUS reports the hold as `msc_host_hold`. |
| `LOAD:TEST.WAV` | Select `/WG/TEST.WAV` and optional `/WG/TEST.SCN`; clear the live schedule and explicit reference override. Allowed after playback has stopped. |
| `SEED:42` | Set the independent live-controller seed before queuing controls. Once a control is queued, `LOAD_REQUIRED`: it would erase the queue, so `LOAD` first. |
| `REFERENCE:0.25` | Override the raw source RMS before queuing controls; must be in `(0,1]`. Once a control is queued, `LOAD_REQUIRED`, as for `SEED`. |
| `PLAY` or `PLAY:<32 lowercase hex digits>` | Start the selected file once, including open/validation/prefill, on the selected output route. Acceptance is asynchronous; poll status for setup failure. `NO_STREAM` on the USB route without an open capture stream. Every accepted `PLAY` restarts the `uac_play_*` STATUS fields, on either route (all 0 for a codec play). On the USB route, open every capture client first and wait until `uac_missed_intervals` has not changed for 3 s: Windows pauses the stream while it reconfigures it, and a pause that overlaps the play's file counts in `uac_play_missed_intervals` (see [missed intervals](#missed-intervals-stream-and-play)). |
| `STOP` | Request stopped playback and cleanup, or abort an active file-generation job. A running [PLAYLIST](#playlist) ends with it, in its gaps too. Poll for completion of cleanup; retained files are not deleted. |
| `OUTPUT CODEC`, `OUTPUT USB` | Select the output route: the WM8960 codec (the boot default) or the USB sound card's capture stream. Naming the current route is OK; a change needs a stopped player, else `BUSY`; while a [PLAYLIST](#playlist) runs both are `BUSY`, as its items set their own routes. STATUS reports `output_route` (0 codec, 1 usb) and INFO `"output"`. On the USB route `PLAY` needs an open capture stream (a PC client recording from "Line (RT1170 Player)"), else `NO_STREAM`; see [USB route](modules/rt1170-player-and-audio.md#usb-route). |
| `CUT:<offset>:<N>:<drop\|zero>,...`, `CUT` | Set the schedule of dropped frames (DROP) and inserted zeros (ZERO) that the next `PLAY` applies to the scenario engine's output, 1 to 16 entries, or clear it with `CUT` alone. Needs a stopped player, else `BUSY`; a list that breaks a rule is refused with `CUT_RANGE`, `CUT_UNSORTED`, `CUT_OVERLAP` or `CUT_DROP_RUN` and changes nothing. WFG-LIVE/1 only. See [CUT injector](#cut-injector). |
| `PLAYLIST:<NAME>`, `PLAYLIST:<NAME>:CONTINUE` | Start the playlist file `2:/WG/<NAME>`: every line is parsed and every item checked against the card before anything plays, and a refusal names its line. Then the items play unattended, each as `LOAD`, its `CUT` list and `PLAY` would, with a gap after each, in stream frames on the USB route. Each item's result block is kept until `PLAYLIST ACK`. `PLAY`'s preconditions; the first item error ends the playlist unless `:CONTINUE`. While it runs every other run command is `BUSY`. WFG-LIVE/1 only. See [PLAYLIST](#playlist). |
| `PLAYLIST?` | The running or last playlist's progress and at most its two oldest result blocks, in a `playlist` object (see [result blocks](#result-blocks-and-playlist)). Read-only; WFG-LIVE/1 only. Its reply has its own budget (3859 of 4096 bytes with two widest blocks). Clients may continue accepting three blocks from older firmware. |
| `ENGINE?` | The current or last play's channel identity, scenario engine state/error and the reason a scenario was refused or the engine/live controls stopped it, in an `engine` object (`"format":"engine-diagnostics"`; see [`ENGINE?` query](#engine-query)). Read-only; WFG-LIVE/1 only. INFO advertises it as `engine-diagnostics`. Its reply has its own budget (984 of 4096 bytes); STATUS stays as it is. |
| `PLAYLIST ACK:<seq>` | Free the result blocks numbered `seq` and below, at any time; `events` is how many. `PLAYLIST_ACK_RANGE` for a number not given yet. |
| `PREPAREPLAY?` | Read the current or last admitted experimental finite playback snapshot in `prepare_play`. Requires `prepare-play-experimental`; no `AT:` scheduling. Reports a boot-local `operation_id`, ordered render/storage/queue watermarks, cleanup ownership and per-phase elapsed-cycle accounting; see [coherent progress query](modules/finite-playback.md#coherent-progress-query). Artifact publication and transport delivery are separate evidence. |
| `GENERATE:600:long:PAYLOAD.BIN:TEST.WAV` | Legacy adapter only: create a WAV from a payload already on SD. Poll file-generation status before LOAD. |
| `ENCODE TONE 1000 PAYLOAD.BIN TEST.WAV` | Generic encoder utility: encoder ID and opaque profile are passed to its adapter. |

Source/output names use uppercase 8.3 names, with letters, digits or underscore
in the stem. Paths and traversal are rejected. The default build registers no encoders. The optional module supplies the
catalog advertised by `INFO?`; see [Encoder plugins](encoder-plugins.md).
Other waveform families require their own implementations.

### Board-side preparation

`PREPARE` is available only in a writable-media image. It validates the named
mono PCM24, 48 kHz source and scenario, requires the scenario's explicit
`reference_rms`, calibrates any secondary source, renders the impaired result,
then checks and publishes the WAV and its identities. It does not start playback.
Use `PREPARE?` to observe the phases and final hashes; `STOP` requests
cancellation, and the final phase distinguishes cancellation, failure and
cleanup failure. The request is never scheduled with `AT:`.

The numeric phases are `0 idle`, `1 starting`, `2 hashing_source`,
`3 hashing_secondary`, `4 calibrating`, `5 rendering`, `6 verifying`,
`7 publishing`, `8 complete`, `9 cancelled`, `10 failed`, and
`11 cleanup_failed`. `error` is a bounded static diagnostic; both SHA-256
strings are empty until produced. Frame counts are unsigned 64-bit values.

The first board path is prepare fully, then reference the resulting WAV from
a playlist item with no `scn=` or `cut=` and with live effects disabled. This
selects clean playback explicitly. Ordinary `LOAD`/`PLAY` can discover a
same-stem scenario; preparation refuses an existing output companion `.SCN`,
but cannot prevent one being added later. Do not reapply the original scenario
to the prepared WAV. `KEEP` and `TEMP` are
retention labels; automatic deletion is not part of this protocol. The host
`retain_waveform.py` archive workflow remains separate; board-side archive
copies and a direct PLAY PREPARED command are not implemented here.

The WAV is written to a new `.TMP`, finalized, synced, closed and read back.
Its completion manifest is written to a new `.JMP`; the final `.WAV` is renamed
first and `.JSN` last. Failed or cancelled jobs retain partial files, and a
failure between the two renames can leave an uncommitted `.WAV`. Existing names
are never overwritten. Use a new output name or explicitly clean up the failed
job's files; a WAV alone is not proof of successful preparation. A
`cleanup_failed` job retains media ownership until a later `STOP` successfully
retries cleanup. Prepared-artifact discovery and automatic reuse validation
after a restart remain separate work.

Playback states retain the diagnostic numeric mapping: 0 stopped, 1 waiting for
command, 2 mounting, 3 opening, 4 buffering, 5 playing, 6 draining, 7 done,
8 aborted and 255 fault. A completed/aborted/faulted run requires LOAD before
another PLAY. Files and audio are closed before reuse or host ownership is allowed.
Run IDs identify attempts in RAM; the auto-generated ID is only a boot-local
counter. A new host-generated UUID is recommended for each run. Neither IDs nor
the engineering counters establish hardware-qualified completion.

### Card listing

`FILES?` executes in the player task, the sole FatFs owner. It refuses while
PLAY, PLAYLIST (including gaps), upload or generation owns the player/card.
With the card Host owned it returns `HOST_OWNED` before any FatFs call. The
query preserves the selected WAV, scenario, route, live controls and retained
playback results. No write, rename, delete or attribute operation is reachable
from the listing.

Replies carry a `files` object with fields in this order: `offset`, `total`,
`next_offset`, `entries`. Each entry has `name`, then `size_bytes`. For example:

```json
{"offset":0,"total":2,"next_offset":null,"entries":[{"name":"TEST.WAV","size_bytes":288044},{"name":"TEST.SCN","size_bytes":280}]}
```

There are at most 24 entries per page. `next_offset` is `offset + entry count`
when more entries remain, otherwise JSON `null`. All numbers are unsigned
uint64 JSON integers. Offset equal to total returns an empty final page;
offset beyond total is refused. An empty directory has total 0 and no entries.
Names are FatFs 8.3 aliases when one is available. If FatFs returns no short
alias (notably on exFAT), the listing uses its filename in the same `name`
field. Either form is converted from the configured CP437 encoding to UTF-8
and must fit in 36 UTF-8 bytes; longer or unrepresentable names fail the page
with `FILES_NAME`, never truncate. This bounded fallback is not a promise to
return arbitrary-length long filenames. Hidden, system and read-only regular
files are included; directories and volume labels are omitted. File sizes are
exact bytes.

Order is the physical FatFs directory order, deterministic while the directory
is unchanged. Each page scans the directory with fixed storage. A sequence of
pages is not an atomic snapshot across requests: keep the card Local and do
not create files between pages. The host helper checks stable totals, unique
names, offsets and forward progress. The page lives in OCRAM2, and a constexpr
worst-case escaped reply budget is checked against the 4096-byte CDC buffer.

| Listing refusal | Why |
| --- | --- |
| `BUSY` | PLAY, PLAYLIST, upload, generation or another filesystem owner is active. |
| `HOST_OWNED` | The card is not Local; no FatFs operation was attempted. |
| `FILES_MOUNT` | The filesystem could not be mounted. Cleanup is still attempted. |
| `FILES_OPEN` | The `2:/WG/` directory could not be opened. |
| `FILES_READ` | Reading the directory failed. |
| `FILES_CLOSE` | Closing the directory failed. |
| `FILES_NAME` | A returned alias or filename cannot be represented within the bounded UTF-8 name contract. |
| `FILES_RANGE` | The offset is beyond the total, or a directory count would overflow. |
| `FILES_PAGE` | The internal page is missing or inconsistent; no partial reply is sent. |
| `FILES_UNMOUNT` | Cleanup failed. The player retains card ownership; a later idle Local `FILES?` retries cleanup before a fresh scan. |

The directory is closed after every successful open, including read failures,
and unmount is attempted after every mount attempt. An unmount failure takes
precedence over a scan failure, since ownership is unresolved. No partial page
is published after an error. Physical listing parity and the under-one-second
per-100-files latency gate remain board acceptance work.

### Player errors

`player_error` (STATUS) names the first fault of a run; the state is then 255.

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | `none` | No fault |
| 1, 2 | `host_init`, `card_detect` | Defined, not raised by the current player |
| 3 | `mount` | The card is not Local, or FatFs could not mount it |
| 4 | `open` | The selected file could not be opened |
| 5 to 9 | `header_io`, `invalid_riff`, `missing_format`, `missing_data`, `unsupported_format` | The WAV header could not be read or is not mono 48 kHz packed PCM24 |
| 10 | `seek` | Seeking within the file failed |
| 11 | `sd_read` | An SD read failed or came back short |
| 12 to 14 | `codec_config`, `codec_level`, `codec_start` | The WM8960 route could not be configured or started |
| 15 | `underrun` | The output FIFO ran dry before the end of the file (either route) |
| 16 | `impairment` | The scenario engine failed while processing (`engine_error`) |
| 17 | `native_source` | Defined, not raised by the current player |
| 18 | `live_control` | The live controller rejected its reference, a preset event or a block ([`ENGINE?`](#engine-query) says why) |
| 19 | `usb_stream_closed` | USB route: the capture stream closed (alternate 0, another alternate selected even straight from one format to another, bus reset, unplug, a new configuration) after `PLAY` was accepted and before every file frame was confirmed sent. A close after that drops nothing: no error |
| 20 | `usb_stall` | USB route: more than 100 ms between IN completions while the play was armed, whether the gap has ended or not, or while the wait after `STOP` still had file frames queued (the run then ends with this error, not `aborted`). A host that stops polling cannot hang the play |
| 21 | `usb_no_stream` | A [PLAYLIST](#playlist) item on the USB route found no open capture stream at its start: it played nothing. Only a result block reports it; a plain `PLAY` answers `NO_STREAM` instead and starts nothing |
| 22 | `cut_log` | The play's cut stage refused: its log had no room for a cut it applied, or the engine's output ran past 2^32 - 1 frames. The log holds a cut for each of the 16 entries a [CUT](#cut-injector) schedule may have and the stage applies each at most once, and no card's output reaches 2^32 frames (nor a looped item's of at most 24 h, unless its scenario's sample slips add some 147 million frames), so a valid schedule never causes it |
| 23 | `missed_interval` | A looped [PLAYLIST](#playlist) item (`dur=`) missed a USB interval while it had frames queued (`uac_play_missed_intervals` above 0). Only a looped item ends on it; any other play counts it and goes on |
| 24 | `usb_assert` | `usb_stack_asserts` rose during a looped item. Only a looped item ends on it |
| 25 | `deadline_miss` | An engine deadline miss in a looped item (`USB?` `engine_deadline_misses`, [Duration](#duration)). Only a looped item ends on it; any other play counts it |
| 26 | `usb_unconfirmed` | USB route: the controller did not confirm the play's packets. Either once no more file frames could join those queued (the file had ended, or STOP or a fault disarmed the play), none of them was confirmed sent for more than 100 ms while IN completions kept coming: the drain, or the wait after STOP, ends there instead of hanging (#45), and chunks that go out later still count; a host that stops polling is error 20 first. Or a completion while the play had frames queued reported a size other than the packet TinyUSB had loaded (`uac_sent_mismatches`): the statistics count that packet sent, but the host may not have received it, so they are no proof. The play runs to its end, and the run then ends with this error unless an earlier fault is its error; the host's capture shows whether the packet arrived. A looped PLAYLIST item ends at once |

The numbers are never renumbered or reused. A firmware without the feature
that raises an error keeps that error's number free and never reports it:
the signal-lab-phase-2 player, which has no PLAYLIST and no looped items,
reserves 21 and 23 to 25. This firmware assigns every number from 0 to 26.

## USB sound card

The player images are always one composite USB device: CDC (this protocol),
MSC (the card while `MEDIA HOST`) and a USB Audio Class 2 capture function
that Windows lists as the recording endpoint "Line (RT1170 Player)": 48 kHz,
stereo, 24-bit samples in 32-bit subslots, left = right. Its identity, layout
and stream format are in [Protocols, Media and Artifacts](modules/protocols-media-and-artifacts.md#usb-identity);
the bench procedure is the [bring-up runbook](usb-sound-card-bringup.md).

### Output route

`OUTPUT CODEC` and `OUTPUT USB` (WFG-LIVE/1, or as legacy lines answering
`OK OUTPUT CODEC` / `OK OUTPUT USB` or `ERR BUSY`) choose where `PLAY` sends
the file. The route is board state: the codec at every boot, then whatever
the last accepted `OUTPUT` chose, across CDC connections. Naming the current
route is OK; a change needs a stopped player, else `BUSY`. While a
[PLAYLIST](#playlist) runs, every `OUTPUT` is `BUSY`: its items set their
own routes. STATUS reports `output_route` and INFO `"output"`.

On the USB route, `PLAY` (after the usual `BAD_STATE` and `RUN_ID_REUSED`
checks) needs an open capture stream, that is a PC program recording from
the endpoint (`uac_stream_open` 1); otherwise it answers `NO_STREAM` (legacy
`ERR NO_STREAM`) and changes nothing. The stream carries exact digital
silence until the play arms (after the usual prefill), then the file's
frames, then silence. The run is `done` only once every file frame is
confirmed sent; STOP and faults return the stream to silence. Errors 19 and
20 above are this route's own; an underrun is 15, as on the codec route.
`MEDIA HOST` stays refused for the whole run. A play's samples leave the
board unchanged only without a scenario file and live controls, which act
before the output FIFO on both routes.

### STATUS fields

The legacy `STATUS` line (`name=value` pairs) and JSON `STATUS?`/`COUNTERS?`
carry the same numeric fields in the same order; every value is a uint32 in
decimal. The fields the composite device and the build identity added:

| Field | Meaning |
| --- | --- |
| `fw_commit_hi`, `fw_commit_lo` | The first 16 hex digits of the image's Git commit as two numbers; 0 when unknown |
| `fw_dirty` | 0 clean, 1 modified tracked files, 2 unknown, 3 clean with encoder plugin sources |
| `output_route` | 0 codec, 1 usb |
| `msc_ejected` | 1 after a host eject (START STOP UNIT, LoEj=1, Start=0) until the next accepted `MEDIA HOST` or a reset: the drive stays absent |
| `msc_host_hold` | Bits: 1 the host prevents medium removal, 2 a host WRITE(10) is in progress. `MEDIA LOCAL` waits for 0 |

JSON `STATUS?` also ends its object with the strings `build_id` and
`fw_commit` (40 hex digits or `unknown`). The sound card's fields follow
`msc_host_hold`, in this order. Units: *frames* are 48 kHz stereo frames,
*stream index* counts frames sent since the capture stream opened (its silent
pre-fill included; it wraps after 24.8 h), *intervals* are 1 ms USB service
intervals. "Boot" fields count since boot; "PLAY" fields restart with every
accepted `PLAY`, on either route, and are all 0 after a codec `PLAY`.

| Field | Unit, scope | Meaning |
| --- | --- | --- |
| `uac_stream_open` | 0 or 1 | 1 while a PC program has the stream open (an alternate with a format: 1, 2, or 3 on a `WFG_UAC_MONO_ALT` image; which one is in `USB?`) |
| `uac_stream_opens` | count, boot | Times the stream was opened, a change of format included |
| `uac_stream_frames` | frames, since the stream opened | Frames in completed packets, exact when the reply is formed; 48 per packet in every format |
| `uac_short_packets` | packets, boot | Completed packets other than one whole packet of the open format (384, 192 or 96 bytes) |
| `uac_missed_intervals` | intervals, boot | Intervals without a completed packet while streaming: a stream diagnostic |
| `uac_host_volume` | 1/256 dB as uint16 two's complement | The Windows level slider as sent to the feature unit; never applied (0 is 0 dB, 65280 is -1 dB) |
| `uac_host_mute` | 0 or 1 | The Windows mute; never applied |
| `usb_stack_asserts` | count, boot | TinyUSB assertion failures; where they failed is in [`USB?`](#tinyusb-assertions) |
| `uac_play_file_frames` | frames, PLAY | File frames confirmed sent. With a [CUT](#cut-injector) schedule: the engine output frames the frames sent cover (sent, less the zeros sent, plus the frames dropped). The engine puts out one frame per card frame unless its scenario slips samples (a clean card's output is the card), so a whole play still reports the card's frame count; the frames sent are `USB?` `uac_play_sent_frames` |
| `uac_play_crc32_l`, `uac_play_crc32_r` | CRC-32, PLAY | zlib CRC-32 of each channel's 24-bit source samples in the frames confirmed sent, each as 3 bytes little-endian, taken before the conversion to the stream's format: the same in every capture format (16-bit truncation and the mono format's dropped right channel included); for a mono PCM24 file both equal `zlib.crc32(<data chunk>)`. With a CUT schedule they cover the frames sent, the inserted zeros included. They name the card's samples, not the wire's: a bit-exact verdict also needs `USB?` `uac_play_stream_bits` 24 and `uac_play_stream_channels` 2 ([capture format](#capture-format)) |
| `uac_play_first_frame_index` | stream index, PLAY | The first file frame sent (0 until one is); it leads a 48-frame packet. With a CUT schedule, the first frame sent, a ZERO's zero included |
| `uac_play_first_frame_sof` | USB frames, PLAY | The USB frame number read by the completion that confirmed the first file frame, extended to 32 bits from the stream's first packet |
| `uac_play_last_frame_index` | stream index, PLAY | The last file frame sent so far |
| `uac_play_accept_frame_index` | stream index, PLAY | `uac_stream_frames` when `PLAY` was accepted; first minus accept is the command-to-audio latency in frames |
| `uac_play_missed_intervals` | intervals, PLAY | Intervals missed in the gaps that delayed this play's file frames (the per-play assertion) |
| `uac_play_first_miss_frame_index` | stream index, PLAY | The first frame sent after the play's first missed interval; 0 when it missed none |

The play fields count only frames confirmed sent, so after STOP, a stream
close or a stall they are an exact prefix of the file: what the host
received. The rules are in [RT1170 Player and Audio](modules/rt1170-player-and-audio.md#usb-route).

### Missed intervals: stream and play

The host polls the stream once per 1 ms. A gap of n USB frames between two
completed packets missed n - 1 intervals. A completion handled just after a
frame boundary reads as a gap of 2 followed by a gap of 0, and the 0 takes
that interval back, so a count can stand one high for a millisecond. A
play's count is read once the completion after its last file frames has
come (the board waits up to 2 ms for it), so STATUS and a playlist block do
not report a late read of the last packet. A
missed interval loses no device frame: the frames after it leave later, and
what a PC capture shows for that millisecond is up to Windows.

- `uac_missed_intervals` is the stream's count since boot. Windows pauses
  polling while its audio engine reconfigures the shared stream: as it
  starts, whenever a capture client joins or leaves, and while the Sound
  settings level meter runs. Those pauses came while the
  stream was idle, and during stable plays the count stayed flat. Log it
  before `PLAY` and after the run; never assert it per play.
- `uac_play_missed_intervals` counts only the gaps that delayed the play's
  file frames, armed or not; it is the value to assert (0) for each play. A
  pause that overlaps the file counts here too, which is why every capture
  client is opened, and the stream left to settle, before `PLAY`.

### INFO fields

| Field | Meaning |
| --- | --- |
| `kind`, `board` | `waveform-player`, `MIMXRT1170-EVK`; they never change |
| `firmware_version`, `sample_rate_hz` | `0.2.0`, 48000 |
| `usb_serial` | 16 uppercase hex digits from the OCOTP fuses; also the USB serial number |
| `build_id` | The CMake build label (`WFG_BUILD_ID`, at most 48 characters) |
| `fw_commit`, `fw_dirty` | The Git commit (40 hex digits or `unknown`) and its dirty state, as in STATUS |
| `usb_pid` | The USB product ID in decimal: 16402 (`0x4012`) or 16403 (`0x4013`) |
| `output` | The output route: `codec` or `usb` |
| `capabilities` | Includes `cut-injection` (the [`CUT`](#cut-injector) command), `usb-audio`, `usb-diagnostics` and `playlist` (the [PLAYLIST](#playlist) commands) on every player image, `usb-audio-mono` on a `WFG_UAC_MONO_ALT` image (the 16-bit mono alternate 3), `sd-clock-24mhz` on a `WFG_SD_CLOCK_24MHZ` image (its SD bus runs at 24 MHz, not 49.5 MHz), then `engine-diagnostics`, `watterson`, `frequency-shift`, `wideband-noise`, `pulsed-cw`, `swept-cw`, `markov-cw`, `fsk-cw`, `faded-cw`, `stream-id` and `second-source` on every player image |
| `watterson_algorithm_version` | Numeric Watterson output algorithm version (`1`), at the INFO top level |
| `frequency_shift_algorithm_version` | Numeric global offset/sweep algorithm version (`1`); see [frequency shift](modules/frequency-shift.md) |
| `read_only` | `true`, present only on the read-only image |
| `request_capacity`, `response_capacity` | 384 and 4096 bytes (192 before the CUT injector) |

[Build and dependencies](modules/build-and-dependencies.md#maintainer-view)
defines the build identity.

### `USB?` query

JSON `STATUS?` is 4089 bytes at its widest, 7 below the response buffer, so no
field is added to STATUS any more. Further USB diagnostics live in `USB?`:

```text
{"seq":7,"protocol":"WFG-LIVE/1","ok":true,"state":1,"run_id":null,"usb":{"format":"usb-diagnostics","uac_stream_open":1,...,"uac_play_unprimed_intervals":0,"usb_assert_count":0,...,"usb_last_assert_tick":0,"uac_stream_alt":1,"uac_stream_bits":24,"uac_stream_channels":2,"uac_play_stream_bits":24,"uac_play_stream_channels":2,"msc_max_write_us":2817,"msc_max_read_us":1433,"msc_max_blocks_per_call":32,"sd_bus_clock_hz":49500000,"uac_play_cut_count":0,"uac_play_cut_drop_frames":0,"uac_play_cut_zero_frames":0,"uac_play_cut_first_index":0,"uac_play_cut_rejected":0,"uac_play_sent_frames":480000,"engine_deadline_misses":0,"usb_ep0_reordered":0,"usb_ep0_unordered":0}}
```

Its first eight fields are the stream fields above, `uac_stream_open` ..
`usb_stack_asserts`, with the values STATUS would report. Then:

| Field | Unit, scope | Meaning |
| --- | --- | --- |
| `uac_rejected_chunks` | chunks, boot | Chunks the IN FIFO refused or that found no free record: their frames were lost |
| `uac_last_miss_frame_index` | stream index, since the stream opened | The first frame sent after the latest gap that still counts a missed interval; 0 when none |
| `uac_last_miss_unprimed` | intervals | How many of that gap's missed intervals are the device's (unprimed) |
| `uac_last_miss_play` | 0 or 1 | 1 when that gap delayed the file of the USB play current then |
| `uac_max_gap_frames` | USB frames, since the stream opened | The largest gap between completed packets as the USB ISR read them: 1 while the host polls every interval, but a late ISR read reads 2 even when no poll was missed (the next zero gap takes the missed interval back, not this maximum); 0 until the stream's second completed packet |
| `uac_watchdog_expiries` | samples, boot | Service-watchdog samples while streaming: about one per millisecond of a host pause, and one whenever the USB interrupt is held off by about 0.5 ms or more |
| `uac_unprimed_intervals` | intervals, boot | The device's part of `uac_missed_intervals`; the rest is the host's |
| `uac_play_unprimed_intervals` | intervals, PLAY | The device's part of `uac_play_missed_intervals` |
| `usb_assert_count` | count, boot | TinyUSB assertion failures (failed `TU_ASSERT` checks and TinyUSB's direct `TU_BREAKPOINT()` calls), read with the four fields below as one record: the same value as `usb_stack_asserts` in the same reply |
| `usb_first_assert_pc` | code address, boot | Where the first failure happened: inside the call to the assert hook at the failed `TU_ASSERT` or the `TU_BREAKPOINT()`, in the TinyUSB function that made it; 0 when none |
| `usb_last_assert_pc` | code address, boot | Where the latest failure happened, likewise; 0 when none |
| `usb_last_assert_frame_index` | stream index | `uac_stream_frames` when the latest failure happened: frames sent since the stream last opened, or the closed stream's last count if it was closed then; 0 when none |
| `usb_last_assert_tick` | RTOS ticks (ms) | The board's tick when the latest failure happened, in milliseconds since the scheduler started at reset; 0 when none |
| `uac_stream_alt` | alternate setting | The open stream's alternate: 1 (24-bit stereo), 2 (16-bit stereo) or 3 (16-bit mono, only on a `WFG_UAC_MONO_ALT` image); 0 while closed |
| `uac_stream_bits` | bits | Valid bits per sample on the wire in the open stream: 24 or 16; 0 while closed |
| `uac_stream_channels` | channels | Channels on the wire in the open stream: 2, or 1 (the left channel); 0 while closed |
| `uac_play_stream_bits` | bits, PLAY | Bits per sample on the wire when the current or last USB play's first file frame went out; 0 until it has (and after a codec `PLAY`) |
| `uac_play_stream_channels` | channels, PLAY | Channels on the wire at that moment, likewise |
| `msc_max_write_us` | microseconds, boot | The longest SD write of one MSC chunk (up to 16 KB, 32 blocks, one CMD25), passed or failed, including the wait for the card to finish programming the previous chunk; 0 until the first, and always 0 on the read-only image |
| `msc_max_read_us` | microseconds, boot | The longest SD read of one MSC chunk (one CMD18, or CMD17 for one block), likewise |
| `msc_max_blocks_per_call` | blocks, boot | The most 512-byte blocks one chunk moved in its SD command: 32 after any host command of 16 KB or more |
| `sd_bus_clock_hz` | Hz, the ready card | The SD bus clock the card's transfers run at, as card initialisation left it: 49500000 (High Speed), 24750000 (the card stayed in Default Speed), 24000000 on a `WFG_SD_CLOCK_24MHZ` image; 0 while no card is ready |
| `uac_play_cut_count` | cuts, PLAY | The [CUT](#cut-injector) entries that took effect within the frames sent |
| `uac_play_cut_drop_frames` | frames, PLAY | Engine output frames their DROPs discarded (a DROP clipped at the output's end counts the frames it discarded) |
| `uac_play_cut_zero_frames` | frames, PLAY | Zero frames their ZEROs sent |
| `uac_play_cut_first_index` | stream index, PLAY | Where the first cut took effect: its first zero, or the first frame after its dropped frames; on the codec route, frames from the play's first. 0 while none has |
| `uac_play_cut_rejected` | entries, PLAY | Entries at or past the end of the engine's output, ignored; counted once the whole stream has been sent (0 for a play that ends part way) |
| `uac_play_sent_frames` | frames, PLAY | The play's frames sent, after its cuts: confirmed sent on the USB route (`uac_play_last_frame_index - uac_play_first_frame_index + 1`), taken by the codec on the codec route |
| `engine_deadline_misses` | blocks, PLAY | Engine deadline misses of the current or last play, on either route: blocks of the engine's output ready only once the running output had drained the FIFO below 4800 frames (see [Duration](#duration)). A looped PLAYLIST item ends at the first |
| `usb_ep0_reordered` | completions, boot | EP0 transfer completions the USB interrupt reported ahead of a SETUP they preceded on the bus, which TinyUSB would otherwise have matched against that SETUP's request (see [EP0 control race](#ep0-control-race-32)). Not an error: each one is the race of #32, put back in bus order |
| `usb_ep0_unordered` | SETUPs, boot | SETUPs the order could not keep behind such a completion: an interrupt that released one with a completion still pending after its extra passes, another event (a bus reset, a suspend) that released them while one was still pending, or a SETUP that found the hold (3) full; 0 unless the controller never stops completing |
| `uac_sent_mismatches` | completions, boot | IN completions whose reported size was not the packet TinyUSB had loaded for them (#45). Each still counts as sent, since TinyUSB took the packet from the FIFO, and as a short packet (`uac_short_packets`), so a short report never leaves the stream's records behind its chunks. But the host may not have received that packet, so one while a play had frames queued ends that play with `player_error` 26. 0 unless the controller reports a transfer that ended in error; 16 bits, it stays at 65535 once there |

A missed interval is the device's (unprimed) when the USB interrupt that
should have primed the next packet was held off past the host's poll, and
the host's when a packet stood primed and the host did not poll for it. A
service watchdog tells them apart: a one-shot timer in the USB controller
(general-purpose timer 0), restarted by every completed packet, that expires
1.5 ms later (1 ms after a completion handled at or past its deadline). Each
expiry is one `uac_watchdog_expiries` sample of the IN endpoint: a packet
still primed proves a poll the host left out, and the watchdog samples again
1 ms later for as long as it waits; nothing primed means a completion was
waiting for its held-off interrupt, which then runs and restarts the
watchdog.
The unprimed counts are an upper bound on the device's share (by at most one
interval per such gap), and 0 is strong evidence for the host, not proof.
When a play's `uac_play_missed_intervals` is not 0, read `USB?` after the run
and before the next `PLAY`. The watchdog, its limits and its bench checks are
in [miss attribution](modules/rt1170-player-and-audio.md#miss-attribution-usb).

The three `msc_*` fields describe host staging over MSC, not the sound card.
TinyUSB hands each READ(10) and WRITE(10) over in 16 KB chunks, one SD command
each, so STATUS `read_calls` and `write_calls` count chunks and `read_blocks`
and `write_blocks` count blocks. A 16 KB chunk spends about 0.67 ms on the
49.5 MHz, 4-bit SD bus (1.39 ms on an image built with `WFG_SD_CLOCK_24MHZ`,
24 MHz); a maximum far above that is the card's own latency. They
run since boot and saturate at 4294967295. `sd_bus_clock_hz` says which bus
they ran on: record it with every MB/s and `msc_max_*_us` figure, since a
card that does not support High Speed initialises (`card_init_status` 0) and
then runs at 24.75 MHz; see
[MSC transfers](modules/protocols-media-and-artifacts.md#msc-transfers-and-throughput).

### `ENGINE?` query

STATUS gives the scenario engine as numbers, `engine_state` and
`engine_error`; `ENGINE?` adds why, which STATUS has no room for, and why
the live controls stopped a play (`player_error` 18). It also reports the
identity of the Watterson channel configured for the current or last actual
play:

```text
{"seq":3,"protocol":"WFG-LIVE/1","ok":true,"state":7,"run_id":"...","engine":{"format":"engine-diagnostics","watterson_version":0,"watterson_digest_hi":0,"watterson_digest_lo":0,"watterson_latency_frames":0,"watterson_paths":0,"engine_state":2,"engine_error":5,"engine_reason":"scaled reference RMS is not finite"}}
```

That configure refusal can be reproduced with a scenario whose finite
`reference_rms` is `1e308` and whose `source_gain_db` is `30`; the engine
rejects the non-finite scaled reference and playback continues cleanly.

| Field | Meaning |
|---|---|
| `watterson_version`, `watterson_digest_hi`, `watterson_digest_lo`, `watterson_latency_frames`, `watterson_paths` | Configured channel identity: the algorithm version, high and low 32-bit halves of the parameter digest, channel latency in frames, and path count. Each is uint32. All five are zero when no Watterson stage was successfully configured for that play. The identity is latched for the actual play; playlist preflight may configure the shared engine but cannot replace the reported identity. |
| `engine_state`, `engine_error` | STATUS's values, read in the same moment as the reason. `engine_state`: 0 `none` (no scenario), 1 `active`, 2 `refused` (the play went on clean), 3 `failed` (the play stopped with `player_error` 16). `engine_error`: 0 `none`, 1 `scenario_open`, 2 `scenario_read`, 3 `scenario_parse`, 4 `missing_reference_rms`, 5 `configure_failed`, 6 `scenario_too_large`, 7 `clipping_rejected`, 8 `processing_failed` (`rt1170/error_map.hpp` pins both lists, and `tests/error_map_tests.cpp` checks this row against it) |
| `second_source_version`, `second_source_source_frames`, `second_source_excerpt_start_frame`, `second_source_excerpt_frames`, `second_source_digest_hi`, `second_source_digest_lo`, `second_source_latency_frames`, `second_source_reference_rms` | Measured second-source identity for the actual play: algorithm version, whole WAV frame count, selected source interval, high/low halves of its canonical PCM24 FNV-1a digest, offset-filter latency and raw excerpt RMS. All are uint32 except RMS (a finite number in (0,1] for an active source). All eight are zero without a successfully configured second source. Calibration includes zeros and precedes S/I gain; the identity survives completion and is not replaced by playlist preflight. |
| `engine_reason` | The message of the Status that refused the scenario or stopped the engine, a JSON string of at most 128 characters (quotes and backslashes escaped, a control character sent as `?`), or `null` while there is none; every message the engine, its stages and its parser give fits whole. With `engine_error` 3 it is the parser's (the grammar's, or a stage's rule such as `watterson: must be the first stage ...`); with 5 it is the engine's or a stage's (for example, `scaled reference RMS is not finite`); with 7 and 8 the engine's (`engine: output clipped with clip_policy reject`) or the faulting stage's; otherwise the `engine_error` name. With `player_error` 18 (`live_control`) it is why the live controls stopped the play, after a refused scenario (`engine_state` 2) too, whose `engine_error` still says what the scenario's problem was: the live controller's message (`nonfinite live sample` for a block holding a NaN or an infinity, or its refusal of a block, a reset or a preset control; of several refusals the first, which stopped the play), or `live_control: a control queued before PLAY needs a reference RMS in (0, 1] (REFERENCE, or a card that is not silent)` |

The reason is the current or last play's: every accepted `PLAY` and PLAYLIST
item restarts it. Its text is a diagnostic for the operator, not a wire
contract: match `engine_error`, not the message. The engine's own messages are
listed in [the signal-lab engine guide](modules/signal-lab-engine.md#8-function-walkthrough-and-lifetime-contract).

### Capture format

The endpoint's format is the alternate setting the PC selected: Windows' audio
engine uses the endpoint's default format (Sound settings, Advanced), an
exclusive-mode client its own (see
[capture formats](modules/protocols-media-and-artifacts.md#capture-formats)).
`uac_stream_alt`, `uac_stream_bits` and `uac_stream_channels` say which one
is open now; `uac_play_stream_bits` and `uac_play_stream_channels` say which
one the play's file went out in, latched when its first file frame was
confirmed sent. They differ only when the stream closed or changed format
after that, which ends an armed play with error 19, so for a play that ended
`done` the play's format is the one its whole file went out in. Check them
before comparing a capture with the card: 24 bits is the card exactly; 16 bits
is each sample `>> 8` (truncated, `--expect-truncation-bits 8`); one channel
is the left one. The play's CRCs (`uac_play_crc32_l`/`_r`) are over the
24-bit source in every format, so they stay `zlib.crc32(<data chunk>)` for a
mono PCM24 card.

Bit-exact verdict (lane rule). STATUS has no format field, and a 16-bit or
mono play reports the `uac_play_*` values a bit-exact 24-bit play of the same
card would: the same frames, CRCs and missed intervals. A play is bit-exact only
when, besides the STATUS checks, `USB?` read after it reports
`uac_play_stream_bits` 24 and `uac_play_stream_channels` 2. Read it before
the next `PLAY`, which restarts both. Any other value makes the play a
truncated (or mono) one, whatever its CRCs say: check its capture against the
card `>> 8`, never as exact. Windows' default format is the owner's setting
and can change between runs, so read it for every play, not once per session.

An image built with `WFG_UAC_MONO_ALT` reports `bcdDevice` `0x0103` instead
of `0x0102`, and the same `usb_pid` and commit as the
standard image of its tree. INFO tells it apart by the `usb-audio-mono`
capability, and by its `build_id` when it was built with its own label (the
documented recipe uses `wfg-0.2.0-mono`; see
[capture formats](modules/protocols-media-and-artifacts.md#capture-formats)).
Its ELF is the one in the build tree it was built in, not the preset's (see
[TinyUSB assertions](#tinyusb-assertions)).

### TinyUSB assertions

The image's assert hook counts two kinds of event. One is a failed
`TU_ASSERT`, TinyUSB's check of its own state and of what the host sent: the
function that checked calls the hook and returns early with a failure. The
other is a direct `TU_BREAKPOINT()` call, which TinyUSB makes where it meets
something it does not handle. Several of those are host requests that TinyUSB
does not support and answers with a stall, by design: an unknown recipient,
or a standard request it does not implement to the device, an endpoint or
the audio function's interface. A host request that fails a `TU_ASSERT` (a
reserved request type, an endpoint number out of range) is stalled the same
way. Others mark a controller error, and some sites carry on instead of
returning (an unknown event in `tud_task_ext`, an unexpected MSC data stage in
`mscd_xfer_cb`). So the field names say only that the hook was called; the
resolved line says which check it was and what TinyUSB did next.

The hook never halts the board, even with a probe attached. It counts each
call (`usb_stack_asserts`, `usb_assert_count`) and records the call site of
the first and of the latest, with the stream frame index and the tick of the
latest. The first usually explains the rest. One event can count more than
once: when a function that failed was called inside a caller's `TU_ASSERT`,
the caller's check fails too and calls the hook again, so the inner call site
comes first and the caller's next. Any context can call it (the `wfg_usb`
task or the USB interrupt); the record is written and read under a short
critical section, so a reply's five fields belong together.

#### EP0 control race (#32)

Window 3 (2325c43) recorded 4 assertions in 2 events while 84 back-to-back
`live_control.py` sessions ran and a capture client opened the stream:
`usbd_edpt_xfer` (`usbd.c:1587`, the endpoint-busy check, inlined via
`status_stage_xact`) inside `usbd_control_xfer_cb` (`usbd.c:941`, the status
stage after a data stage). The mechanism, in the pinned TinyUSB 0.21.0:

1. The ChipIdea driver's interrupt reads `ENDPTCOMPLETE`, reports every
   completion it found (the audio IN packet's among them, with its packet
   work and the player's refill), and only then reads `ENDPTSETUPSTAT`.
2. If a control transfer's last stage completes on the bus after the first
   read and the host's next SETUP arrives before the second (two drivers'
   requests queued back to back: usbser's line state and line coding,
   usbaudio2's GET CUR and SET_INTERFACE), the SETUP is reported in this
   pass and the completion in the next interrupt, after it.
3. TinyUSB matches an EP0 completion against the request it is processing.
   For an IN-data request (GET_LINE_CODING, a UAC2 GET) the previous
   transfer's status ZLP looks like the end of the data stage, so it queues
   the status stage; the real data completion then ends the data stage a
   second time, and that status stage finds the endpoint busy: the two
   assertions. An OUT-data request (SET_LINE_CODING, a UAC2 SET) after an
   IN-data one gives the same pair. The previous transfer's own last stage
   is lost as well: a SET_CONTROL_LINE_STATE whose ZLP is taken this way
   never reaches the CDC driver, which keeps the old DTR.

The host saw no error (the transfers complete normally on the bus), which is
why window 3 found no audio effect. Upstream TinyUSB after 0.21.0 does not
change this order (its ChipIdea driver now flushes stale EP0 primes on a
SETUP, and usbd verifies instead of asserting at line 941, which would only
hide the count).

The fix is in owned code and changes no TinyUSB source. The firmware links
with `--wrap=dcd_event_handler`, so every event the driver reports reaches
the USB interrupt's EP0 order first (`rt1170/usb_ep0_order.hpp`): a SETUP is
held until the interrupt ends, an EP0 completion reported meanwhile goes to
TinyUSB ahead of it, and while a SETUP is held and `ENDPTCOMPLETE` still
shows an EP0 completion the driver's handler runs another pass (at most 2)
to report it. A SOF goes to TinyUSB at once as well: the driver reports it
last in a pass, after the SETUP, and releasing the SETUP there would put it
back ahead of the completion the next pass reports (SOF interrupts are off
today, as no driver asks for SOF). Any other event releases the held SETUPs
first, so a bus reset still follows the SETUPs before it. A completion
pending when a SETUP is reported cannot belong to that SETUP, which nothing
has answered yet, so reporting it first is the bus order.
`usb_ep0_reordered` counts each time the race happened and was put in order;
`usb_ep0_unordered` counts an interrupt that hit the pass bound, another
event that released the SETUPs while `ENDPTCOMPLETE` still showed an EP0
completion (a bus event the driver reports at the start of an extra pass),
or a SETUP that found the hold full (expected 0).

The host test `wfg-usb-ep0-order` runs the pinned TinyUSB's `usbd.c` and CDC
driver on the host below a stand-in driver: reported as the ChipIdea driver
reports it, GET_LINE_CODING after DTR off asserts exactly twice and DTR off
is lost; through the EP0 order neither happens (and likewise for
SET_LINE_CODING after GET_LINE_CODING), also when the pass that reports the
SETUP ends with a SOF. The bench row is the storm of
`host/usb_storm.py`
([command-line tools](modules/command-line-tools.md#hostusb_stormpy)).

A `*_assert_pc` field is a 32-bit code address, in decimal: the hook's return
address with the Thumb bit cleared, less 2, which lies inside the call
instruction (a 4-byte `bl`, which starts 2 bytes lower) in the function that
called the hook (or in the function that inlined it). TinyUSB is built so
that every such call belongs to one assert (`rt1170/CMakeLists.txt`): without
sibling calls, so the call is never a jump that would leave the address of
the function's caller instead, and without tail merging or cross-jumping,
which at `-O3` let one call serve the asserts of several lines, and of several
inlined functions, so that addr2line named only one of them.

Resolve the address against the ELF of the image the board runs. The owner and
read-only image, and release and debug builds, can have different code addresses
even at the same commit. Choose the ELF by:

- the preset, from INFO's `usb_pid`: 16402 (`0x4012`) is
  `rt1170-player-release`; 16403 (`0x4013`), whose INFO also reports
  `"read_only":true`, is `rt1170-player-readonly-release`;
- release or debug, which INFO does not report: the lane runs the release
  presets, and a `-debug` preset's image needs that preset's own ELF;
- that preset's build tree (`build\<preset>`, as the
  [bring-up runbook](usb-sound-card-bringup.md#2-build-the-image) builds it).
  An image
  configured with `-DWFG_UAC_MONO_ALT=ON` (INFO lists `usb-audio-mono`;
  Windows shows `REV_0103` in its hardware IDs) is no preset's: use the ELF
  of the tree that built it.

Then check the ELF before reading the line: the instruction at `pc - 2` must
be a `bl` to `wfg_usb_count_stack_assert`. Anything else means the ELF is not
the image's, and addr2line would name an unrelated function and line. Use the
`arm-none-eabi-objdump` and `arm-none-eabi-addr2line` of the toolchain that
built the image (`cmake/arm-none-eabi.cmake`). objdump takes the address in
decimal, addr2line in hex:

```powershell
$preset = 'rt1170-player-release'   # usb_pid 16402; 'rt1170-player-readonly-release' for 16403
$elf = "build\$preset\waveform_generator.elf"
$pc = 805436184   # usb_first_assert_pc from USB?
arm-none-eabi-objdump -d --no-show-raw-insn --start-address=$($pc - 2) --stop-address=$($pc + 2) $elf
arm-none-eabi-addr2line -e $elf -f -i -C -p ('0x{0:x}' -f $pc)
```

Addresses move with every build. In one release build, 805436184
(`0x3001fb18`) is the MSC read path's assert; after objdump's header, the two
commands print:

```text
3001fb16 <proc_read10_cmd.constprop.0+0x96>:
3001fb16:       bl      3002127c <wfg_usb_count_stack_assert>
proc_read_io_data at ...\third_party\tinyusb\src\class\msc/msc_device.c:874 (discriminator 1)
 (inlined by) proc_read10_cmd at ...\third_party\tinyusb\src\class\msc/msc_device.c:867 (discriminator 1)
```

Against the ELF of another build the same number disassembled to a `cmp` and
resolved to `mscd_control_xfer_cb` at `msc_device.c:426`: a plausible answer,
and a wrong one, which the check rejects.

`-f` names the function, `-i` adds each function it was inlined into
(`(inlined by) ...` lines) and `-p` prints each on one line. The first line is
the failed `TU_ASSERT` or the `TU_BREAKPOINT()` itself; each `(inlined by)`
line is where the function above it was called. In Bash the addresses are
`$((pc - 2))` and `$(printf '0x%x' "$pc")`; in Python, `pc - 2` and
`hex(pc)`. Release ELFs carry line tables (`-g`; debug sections are never
loaded onto the board). An ELF without them still gives the function, with
`??:?` for the line.

`usb_last_assert_frame_index` places the latest failure in the stream: if the
stream has not reopened since (`uac_stream_opens` as it was then),
`uac_stream_frames` minus it is the frames sent since (48 per millisecond).
`usb_last_assert_tick` places it in time: milliseconds since the board's
scheduler started after reset.

### CUT injector

`CUT` gives the next `PLAY` a schedule of dropped frames and inserted zeros,
applied after the channel, so a capture carries reproducible stalls for
receivers that must ride them out.
For the distinction between CUT, scenario `sample_slip`, and USB service gaps
(including combined frame coordinates), see [Time-step faults](modules/time-step-faults.md).
The board only executes the list. Seeded random schedules, their `CUT`
command and the values the board will report come from
[`host/cut_schedule.py`](modules/command-line-tools.md#hostcut_schedulepy).

| Command | Meaning |
| --- | --- |
| `CUT:<offset>:<N>:<drop\|zero>,...` | The schedule the next `PLAY` takes: 1 to 16 entries separated by commas, each number a canonical unsigned decimal of at most 32 bits, the kinds in lower case |
| `CUT` | No cuts for the next `PLAY` |

- **Order.** The scenario engine and the live controls process every card
  frame; the cuts act on their output. A DROP reads, processes and discards
  its frames, so engine time, the scenario's events (a Watterson stage's tap
  clock included) and live-control `AT:` positions all run on through them.
  A ZERO inserts exact zero frames into the output, which the engine and the
  live controls never see, and the output resumes where it left off.
- **Coordinates.** `offset` counts frames of the engine's output from the
  play's first. The engine puts out one frame per card frame unless its
  scenario slips samples, and a play without a scenario puts out the card
  itself, so for a clean card an offset is a card frame (the WAV's frames).
  A DROP covers output frames `[offset, offset + N)`: the stream leaves them
  out and is N frames shorter. A ZERO sits before output frame `offset`: the
  stream carries N exact zero frames there and then the output resumes; it
  is N frames longer. N is 1 to 4800 (100 ms).
- **Rules,** checked when `CUT` arrives; a refused list changes nothing. The
  entries are sorted by offset (else `CUT_UNSORTED`) and do not overlap (else
  `CUT_OVERLAP`: an entry inside a DROP's frames, including a ZERO at that
  DROP's own offset, or two ZEROs at one offset). N outside 1 to 4800 is
  `CUT_RANGE`. At one offset a ZERO may come before a DROP: an erasure, zeros
  in place of the dropped frames. Adjacent entries are fine: a ZERO or a DROP
  where a DROP ends. A bad spelling, more than 16 entries or a number past 32
  bits is `BAD_REQUEST`; `CUT` during a run is `BUSY`. Accepted, the reply's
  `events` is the number of entries.
- **The drop balance.** A DROP's frames go through the engine like any
  others but put nothing into the output FIFO, so the board spends engine
  time on them while the output drains. Each DROP adds its length to the
  schedule's drop balance, and the engine output frames between one DROP and
  the next take it back, frame for frame, never below zero (a ZERO's zeros
  take nothing back). A list whose balance passes 14400 frames (300 ms) is
  `CUT_DROP_RUN`: three adjacent DROPs of 4800 frames are fine, a fourth
  needs 4800 output frames before it. The output FIFO's margin, 300 ms at
  its wake level against 100 ms at its critical level, covers that balance
  for a producer at least twice as fast as real time (image A: 3.4 times
  with an AWGN, CW and fade scenario, 17 times for a clean card), so a
  schedule the board accepts causes no deadline miss and no underrun. A
  scenario that brings `producer_rate_sps` below 96000 needs this budget
  revisited.
- **Latching.** An accepted `PLAY` takes the list, so the play after it has no
  cuts unless `CUT` is sent again. `LOAD` keeps the list, and so does a
  refused `PLAY` (`NO_STREAM`, `BAD_STATE`). Each [PLAYLIST](#playlist) item
  sets its own list (its `cut=`, or none) through the same latch, so a list
  set before `PLAYLIST` reaches no item.
- **The output's end.** An entry whose offset is at or past the end of the
  engine's output never applies; it counts in `uac_play_cut_rejected` once
  the whole stream has been sent. A DROP that runs past the end is clipped
  there: it discards the frames up to the end, and `uac_play_cut_drop_frames`
  counts those.
- **Routes.** The player task applies the list to the output of the engine
  and the live controls, before the output FIFO, so it acts alike on `OUTPUT
  USB` and `OUTPUT CODEC`, on every play. The card is read whole and in
  order whatever the schedule. A ZERO's zeros enter the output FIFO as exact
  zeros whatever a scenario or the live controls do. For a clean card (no
  `.SCN`, no live controls) the stream is the card with the schedule
  applied, bit for bit; for a scenario play it is the scenario's output with
  the schedule applied, which
  `host/cut_schedule.py predict --scenario` predicts from the host render.
  The details are in
  [RT1170 Player and Audio](modules/rt1170-player-and-audio.md#cut-injector).

What `USB?` reports for the current or last play (the field table is under
[`USB?` query](#usb-query)):

- A cut takes effect once the first frame at or after its place in the
  stream is sent: a ZERO's first zero, or the first frame after a DROP's gap.
  A DROP that runs to the output's end takes effect when the whole stream has
  been sent. A ZERO counts the zeros sent of it. The entries past the
  output's end count when the whole stream has been sent.
- The fields cover the frames the route confirmed sent, so after `STOP`, a
  stream close or a stall they are an exact prefix: the cuts applied so far
  and no later one. `host/cut_schedule.py predict --sent-frames` predicts
  such a prefix from `uac_play_sent_frames`.
- On the USB route the play's CRCs cover the frames sent, the inserted zeros
  included, and `uac_play_first_frame_index` and `uac_play_last_frame_index`
  span them: last - first + 1 = `uac_play_sent_frames`. STATUS
  `uac_play_file_frames` counts the engine output frames they cover (sent,
  less the zeros sent, plus the frames dropped): for a whole play the frames
  it would send without cuts, the card's frame count unless its scenario
  slips samples.
- Cuts are not missed intervals: `uac_play_missed_intervals` stays 0 on a
  clean host, whatever the schedule.
- On the codec route the `uac_play_*` STATUS fields stay 0 (no USB play).
  STATUS `file_frames_submitted` counts the frames the codec took, after the
  cuts, against `file_frames_total` (the card's); `USB?` reports the cuts,
  with `uac_play_cut_first_index` counted from the play's first frame.
- With a scenario, STATUS `engine_frames_in` is the card's frame count and
  `engine_digest_hi`/`_lo` the digest of the engine's whole output, cuts or
  not: the engine saw every frame. Only the stream and the play's CRCs carry
  the cuts.
- Without a list, after `CUT` alone, or with every entry past the end, a play
  sends and reports exactly what it did before the injector.

Example, a 10 s card (480000 frames) and two erasures of 10 ms:

```text
1 CUT:48000:480:zero,48000:480:drop,144000:480:zero,144000:480:drop
-> "result":{"accepted":true,"apply_frame":0,"events":4}
2 PLAY ... the run ends done; then USB?:
uac_play_cut_count 4, uac_play_cut_drop_frames 960, uac_play_cut_zero_frames 960,
uac_play_cut_first_index = uac_play_first_frame_index + 48000,
uac_play_cut_rejected 0, uac_play_sent_frames 480000
STATUS uac_play_file_frames 480000; uac_play_crc32_l over the card with
frames 48000..48479 and 144000..144479 replaced by zeros (the card is the
engine's output of a clean play)
```

## PLAYLIST

A playlist plays a battery of cards unattended while the PC only records:
the player reads a playlist file from the card, checks every item against
the card before anything plays, then plays the items in turn, each exactly
as `LOAD`, its `CUT` list and `PLAY` would, with a gap after each. Each
item's result block, its per-play statistics as STATUS and `USB?` report
them, stays on the board until the host acknowledges it. WFG-LIVE/1 only;
INFO lists the `playlist` capability.

| Command | Meaning |
| --- | --- |
| `PLAYLIST:<NAME>` | Start the playlist file `2:/WG/<NAME>`; the first item error ends it |
| `PLAYLIST:<NAME>:CONTINUE` | The same, going on with the next item after an item error |
| `PLAYLIST?` | The running or last playlist and its oldest result blocks (read-only) |
| `PLAYLIST ACK:<seq>` | Free the result blocks numbered `seq` and below |
| `STOP` | End the item that plays and the playlist, in a gap or a pause too |

### Playlist file

`<NAME>` is an upper-case 8.3 name in `2:/WG`, its extension optional
(`BATTERY.PL`, `LADDER`). The file is ASCII (printable characters and tabs;
no UTF-8 byte order mark), with LF or CR LF line ends, at most 511
characters per line before its LF, at most 262144 bytes and 256 items. One
item per line:

```text
<FILE.WAV> <gap_seconds> <USB|CODEC> [label=<id>] [scn=<NAME.SCN>] [seed=<u32>] [cut=<offset>:<N>:<drop|zero>,...] [att=<dB>] [dur=<seconds>]
```

- Fields are separated by spaces or tabs. A field that starts with `#`
  begins a comment that runs to the line's end; blank lines and comment
  lines are ignored.
- `FILE.WAV`: the card in `2:/WG`, an upper-case 8.3 name as `LOAD` takes.
- `gap_seconds`: 0 to 3600, in whole seconds or with up to 3 decimals,
  spelt canonically (no sign, no leading zero: `0.5`, not `.5` or `00.5`).
  It is kept as an exact count of 48 kHz frames, whole milliseconds of 48
  frames, and waited after the item, the last one included.
- `USB` or `CODEC` (upper case): the item's output route, as `OUTPUT` sets it.
- Options, in any order, each at most once:
  - `label=`: 1 to 24 of `A-Z a-z 0-9 _ - .`. Optional; unique within the
    file when given. The block carries it.
  - `scn=`: a scenario file `NAME.SCN` in `2:/WG`, which the item plays.
    It is the only way an item gets a scenario: without `scn=` an item
    plays its card clean, even when `<FILE>.SCN` exists (which `LOAD` and
    `PLAY` would load). An item plays what its line says, and its block's
    `scn` and `scn_crc32` say which scenario it played.
  - `seed=`: a canonical uint32, the scenario's seed for this item: it
    replaces the scenario document's `seed`. Needs `scn=`.
  - `cut=`: the item's [CUT](#cut-injector) schedule, spelt as the entries
    after `CUT:`, with the CUT command's grammar and rules (1 to 16 entries,
    N 1 to 4800, sorted, non-overlapping). It acts on the item's output,
    after its scenario, as a `CUT` does on a play's.
  - `att=`: the item's attenuation, 0 to -40.0 dB in steps of 0.1 dB: `0`
    (also `0.0` or `-0`), or a negative level with at most one decimal and
    no leading zero (`-20`, `-20.0`, `-6.5`, `-0.5`). A positive level, a
    second decimal or a level past -40.0 dB is refused (`PLAYLIST_ATT`): the
    board only lowers the level. Without `att=` an item plays at full level.
    See [Attenuation](#attenuation).
  - `dur=`: the item's duration, 1 to 86400 s, spelt as a gap (whole
    seconds or up to 3 decimals, canonical), kept as an exact count of
    48 kHz frames. The card loops seamlessly until the duration is reached,
    unless an anomaly ends the item first. Without `dur=` an item plays its
    card once, exactly as before. See [Duration](#duration).

`scn=` and `seed=` drive the signal-lab engine (AWGN, CW, impulses, fades,
sample slips and Watterson). Watterson items use the same scenario-file and
per-item seed syntax.

Example playlist syntax:

```text
# Six items with 5 s gaps, routed to USB
C24L05.WAV 5 USB label=C24L05
C12L05.WAV 5 USB label=C12L05
C06L05.WAV 5 USB label=C06L05
FSRAND.WAV 5 USB label=FSRAND
C06L05N.WAV 5 USB label=C06L05N
C24L10.WAV 5 USB label=C24L10
```

### Starting a playlist

`PLAYLIST:<NAME>[:CONTINUE]` has `PLAY`'s preconditions: a stopped player
(else `BUSY`), the card Local and no file generation (else `BAD_STATE`). The
player task, the card's only FatFs user, mounts the card, reads and parses
the whole file, then checks every item in order: its WAV opens and passes
the validation `PLAY` applies (mono 48 kHz packed PCM24), and its `scn=`
loads as `PLAY` loads a scenario (read, parsed, `reference_rms` present, the
engine configured for that WAV's length with the item's seed). It releases
the card, then answers. Nothing plays before every item has passed.
Accepted, the reply's `events` is the number of items:

```text
4 PLAYLIST:BATTERY.PL
-> {"seq":4,...,"result":{"accepted":true,"apply_frame":0,"events":6}}
```

A refusal changes nothing (the player, the card, `PLAYLIST?` and STATUS,
the last play's SD read counters included, stay as they were: the card
checks' reads count in no play's counters). It names the first refusal in
the file's order, the line and
what the check found in the error's message, `<code> line <n>: <detail>`
(either part only when there is one):

```text
{"seq":5,...,"ok":false,...,"error":{"code":"PLAYLIST_WAV_INVALID","message":"PLAYLIST_WAV_INVALID line 4: unsupported_format"}}
```

| Code | Line | Meaning |
| --- | --- | --- |
| `PLAYLIST_OPEN` | 0 | No such file, or the card could not be mounted (`: mount`) or read (`: read`) |
| `PLAYLIST_TOO_LARGE` | 0 | The file is larger than 262144 bytes |
| `PLAYLIST_EMPTY` | 0 | The file holds no item |
| `PLAYLIST_SYNTAX` | n | Fewer than three fields, an unknown or repeated option, or a character other than printable ASCII or a tab |
| `PLAYLIST_LINE_TOO_LONG` | n | More than 511 characters before the LF |
| `PLAYLIST_FILE` | n | Not an upper-case 8.3 `.WAV` name |
| `PLAYLIST_GAP` | n | Not 0 to 3600 s with up to 3 decimals, canonically spelt |
| `PLAYLIST_ROUTE` | n | Not `USB` or `CODEC` |
| `PLAYLIST_ROUTE_UNAVAILABLE` | n | `USB` on an image without the sound card (every player image has it) |
| `PLAYLIST_LABEL` | n | A label that is empty, longer than 24 characters, or has another character |
| `PLAYLIST_LABEL_DUPLICATE` | n | The label is an earlier item's (the later item's line) |
| `PLAYLIST_SCN` | n | Not an upper-case 8.3 `.SCN` name |
| `PLAYLIST_SEED` | n | Not a canonical uint32 |
| `PLAYLIST_SEED_NO_SCN` | n | `seed=` without `scn=` |
| `PLAYLIST_CUT` | n | `cut=` not spelt as 1 to 16 CUT entries |
| `CUT_RANGE`, `CUT_UNSORTED`, `CUT_OVERLAP`, `CUT_DROP_RUN` | n | The item's schedule breaks the CUT command's rule of that name |
| `PLAYLIST_ATT` | n | `att=` that is not 0 to -40.0 dB with at most one decimal: positive, `-40.1`, `-5.25`, `-05` |
| `PLAYLIST_DUR` | n | `dur=` that is not 1 to 86400 s with up to 3 decimals, canonically spelt: `0.999`, `86400.001`, `01`, `1.2345` |
| `PLAYLIST_TOO_MANY` | n | A 257th item |
| `PLAYLIST_WAV_MISSING` | n | The item's WAV cannot be opened |
| `PLAYLIST_WAV_INVALID` | n | `PLAY` would refuse the WAV; the detail is the [player error](#player-errors) it would latch: `header_io`, `invalid_riff`, `missing_format`, `missing_data`, `unsupported_format` |
| `PLAYLIST_SCN_MISSING` | n | The item's scenario cannot be opened |
| `PLAYLIST_SCN_INVALID` | n | `PLAY` would reject the scenario; the detail is the `engine_error` name: `scenario_open`, `scenario_read`, `scenario_parse`, `missing_reference_rms`, `configure_failed`, `scenario_too_large` |

The checks open every WAV and parse every scenario, so a long playlist takes
longer to accept than an ordinary request: `live_control.py playlist start`
waits up to 30 s for the reply.

### How items run

Each item, in the file's order:

1. Room for its result block: a full ring pauses the playlist here
   (`paused_ring_full`) until `PLAYLIST ACK` frees a block.
2. The end of the gap before it, unless it is a USB item after a USB item:
   that one is prepared at once and scheduled for a stream index (below).
3. Its route. A USB item that finds no open capture stream then plays
   nothing and selects nothing: its block says `error` with player error 21
   (`usb_no_stream`), where a plain `PLAY` answers `NO_STREAM`, and the
   player keeps the file, the scenario and the end state of the run before
   it, so a later `PLAY` needs `LOAD` as after every other item. Otherwise:
   `LOAD` of its file, its scenario (`scn=`, or none) and seed, its CUT list
   (which replaces whatever list was pending: a list set before `PLAYLIST`
   reaches no item), its attenuation and its duration, then `PLAY`, which
   takes the list, with today's per-play semantics: prefill, arming, and
   `done` only once the codec has drained or the USB stream has confirmed
   every frame sent.
4. Its result block, then its gap.

#### Attenuation

`att=` lowers the item's output after the engine, on either route, in exact
integer arithmetic, so the host predicts the stream bit for bit:

- An attenuation of n tenths of a dB is the gain 10^(-n/200), kept as a
  Q1.31 fraction: round_half_even(2^31 * 10^(-n/200)), n = 0 to 400. The
  401 gains are a constant table in flash (`common/attenuation_gains.hpp`),
  written once from that closed form by `host/attenuation.py`, which
  `host/make_playlist.py` uses too; the tests check the table against the
  closed form and against the host's entry by entry.
- Each PCM24 sample s of the engine's output becomes
  round_half_even(s * gain / 2^31): an exact 55-bit product, rounded half to
  even. 0 dB (gain 2^31) passes every sample through unchanged, zero stays
  zero, and the result stays in the PCM24 range.
- It is memoryless per sample and leaves zero at zero, so its order against
  the item's cuts changes nothing: the stream is the cut stream attenuated,
  which is the attenuated output cut. A ZERO's zeros stay exact zeros.
- The engine and its statistics (`engine_digest_hi`/`_lo`, `engine_clipped`)
  see the unattenuated signal; the play's CRCs cover the attenuated frames
  sent.

#### Duration

`dur=` makes a long row from a short card, with no limit from the WAV's
size:

- **The looped stream.** The item feeds the engine `dur_frames` frames of
  its card: frame 0 again after its last frame, pass after pass, with no
  zero and no repeated frame at a seam. The last pass ends where the
  duration does. The engine sees one continuous input: it is configured
  once, for `dur_frames` frames, and its frame clock and the scenario's
  events (a fade's period, a window, a Watterson tap clock) run on across
  the seams. The engine's output is the stream's length too unless the
  scenario slips samples (then it differs by the slips, as for a card
  played once). The short block that ends each pass changes nothing: the
  engine's stages give the same output for any split of their input into
  blocks (the signal-lab tests check this for AWGN and the Watterson stage;
  `wfg-player-live-owner` checks a looped fade, AWGN and CW item against the
  host render of its looped card, sample for sample).
- **Cuts** count frames of the engine's output of the whole looped stream,
  absolute across the passes, with one schedule per item: a cut that
  straddles a seam follows the same rule as any other, and 32-bit offsets
  cover the longest duration (24 h). The duration counts the engine's
  input, so cuts never change how long the engine runs: frames sent =
  duration - dropped + zeros (for a scenario without slips).
- **Attenuation** acts on the looped stream as on a card played once.
- **The block** reports `dur_frames`, `loops` (whole passes of the card
  that the frames sent cover: the engine output they cover, divided by the
  card's frames, rounded down), the frames sent (`uac_play_sent_frames`),
  `end_cause` and `engine_deadline_misses`.
- **Counters.** The frame counts and offsets are 32-bit and cover the
  longest duration. STATUS `sd_bytes_read` counts the bytes of every pass:
  it holds at 4294967295 once an item passes 4 GiB of reads (about 8.28 h of
  a mono PCM24 card) rather than wrap.
- **Prefix rule.** The frames sent are always an exact prefix of the
  predicted stream (looped, cut and attenuated), whatever ends the item:
  `host/make_playlist.py predict --sent-frames` predicts the CRCs, the cut
  counters and the loops of any prefix.

Anomalies end a looped item early; the first one is its `end_cause` (and
its `player_error`), and the item's `result` is `error` (`aborted` for
STOP):

| Anomaly | `end_cause` | `player_error` |
| --- | --- | --- |
| The output FIFO ran dry | `underrun` | 15 |
| A USB interval without a packet while the item had frames queued (`uac_play_missed_intervals` above 0), once the next completion has confirmed it: a late read that completion takes back is no miss | `missed_interval` | 23 |
| The capture stream closed | `stream_closed` | 19 |
| `STOP` | `stop` | 0 (`result` `aborted`). On the USB route the item first waits for the frames it had queued; a host that polls nothing for more than 100 ms in that wait ends it with error 20 instead: `result` `error`, `end_cause` `stall`, `player_error` 20 (the playlist still ends `aborted`) |
| An SD read or seek failed | `sd_read` | 11, 10 |
| `usb_stack_asserts` rose during the item | `usb_assert` | 24 |
| An engine deadline miss | `deadline_miss` | 25 |
| A completion reported a size other than its packet while the item had frames queued (`uac_sent_mismatches` rose): the host may not have received that packet | `fault` | 26 |

Every other fault ends a looped item as it ends any play (`stall` for 20,
`fault` for the rest, `player_error` naming it). An item without `dur=`
behaves as before: the missed interval, the assertion and the deadline miss
end only looped items; any play counts the missed intervals and the
deadline misses. Anomalies end an item while it runs: once its last frame
has been confirmed sent, or STOP has ended it, a missed interval or an
assertion ends nothing more and shows only in the counters (the block's
`uac_play_missed_intervals`, `USB?` `usb_stack_asserts`).

An **engine deadline miss** is a block of the engine's output that comes
too late: a pull step (the SD read of a card block and the engine block
that processes it) that ends while the output is already running (the
codec started, or the USB play armed) before the stream's end, with the
output FIFO holding fewer than `ring_critical_frames` (4800 frames,
100 ms). The producer normally keeps the FIFO between its wake and high
watermarks (300 to 500 ms), so a miss means it fell more than 200 ms
behind; an underrun follows within 100 ms if it does not recover. `USB?`
reports the current or last play's count as `engine_deadline_misses`, on
either route, restarted by every accepted `PLAY` or item.

Gaps, in device time where there is one:

- A USB item after a USB item, on the capture stream the earlier one played
  on and still open, starts exactly
  `first_frame_index + 48 * ceil(sent_frames / 48) + gap_frames` frames
  into the stream: the earlier item's first frame, its frames sent rounded
  up to whole 48-frame packets (its last packet ends with exact zeros), then
  the gap. The item is prepared during the gap and armed for that stream
  index, and the stream carries exact zeros until then, so items are an
  exact number of frames apart whatever each took to prepare. Its block
  reports the index as `scheduled_index`. An item not ready by then (a gap
  shorter than its preparation, or a pause for the ring past it) starts at
  the next packet once ready, with `start_late` 1. After a gap of 0 that is
  always so: its index is the end of the earlier item's last packet, which
  has gone out by the time the earlier item ends. The board prepares an item
  (mount, open, the WAV and scenario checks, the prefill) during the gap;
  that time is still to be measured on the bench, so give a scheduled item a
  gap of at least 1 s for an on-time start (`make_playlist.py` predicts
  `start_late` 0 only then).
- After the last item, or before a codec item, a USB item's gap ends once
  the stream has sent up to that index: the capture holds the whole gap. A
  stream that sends nothing for more than 100 ms meanwhile (`usb_stall_timeout_ms`:
  a host that stopped polling without closing the stream, as a suspended
  bus does) no longer counts the gap: it then ends on the tick clock, the
  gap's length from the item's end, so the playlist never waits for frames
  that do not come.
- After a codec item, after a USB item that sent nothing, and after the
  stream closed or reopened, the gap is counted in RTOS ticks (milliseconds)
  from the item's end, and the next item starts when it is ready
  (`scheduled_index` null).

Errors and STOP:

- `STOP` ends the item that plays (`aborted`) and the playlist, in a gap, a
  pause or a scheduled wait too; no later item runs. A USB item whose host
  stops polling while the frames queued at `STOP` wait to go out ends in
  `error` instead, with `player_error` 20 (`usb_stall`) and `end_cause`
  `stall`, as a plain `PLAY` does; the playlist still ends `aborted`.
- Under the default policy an item `error` ends the playlist in `error`
  after its block; under `:CONTINUE` the next item runs, and the playlist
  ends `done` with the item counted in `failed`.

While a playlist runs, its gaps and pauses included, it holds the player:
`LOAD`, `CUT`, `OUTPUT` (even naming the current route), `SEED`,
`REFERENCE`, `PLAY`, `PLAYLIST` and the live controls answer `BUSY`;
`MEDIA HOST` is refused, so the card stays with the board; file generation
is `BUSY`. `STOP`, `PLAYLIST ACK` and the queries act as always. STATUS? and
`USB?` per-play fields describe the current or last item. When the playlist
ends the file of the last item that played stays selected and the last
item's route stays set; no CUT list is left pending, and a `PLAY` needs
`LOAD` first.

### Result blocks and `PLAYLIST?`

The board keeps the result blocks in a ring of 32. Blocks are numbered from
1 at boot, in order, never reused, and kept until `PLAYLIST ACK:<seq>`
frees every block numbered `seq` or below (the reply's `events` is how
many; `PLAYLIST_ACK_RANGE` for a number not given yet). An earlier
playlist's blocks stay until acknowledged too. A full ring never drops a
block: the playlist pauses before its next item.

`PLAYLIST?` answers with the progress and at most the two oldest blocks:

```text
{"seq":9,"protocol":"WFG-LIVE/1","ok":true,"state":7,"run_id":"...","playlist":{"format":"playlist","state":"gap","name":"BATTERY.PL",
 "run":1,"policy":"stop","items":6,"index":2,"completed":2,"failed":0,"pending":2,"oldest_seq":1,"next_seq":3,"tick":66380,"blocks":[
 {"seq":1,"run":1,"item":0,"label":"C24L05","file":"C24L05.WAV","route":"usb","result":"done","player_error":0,"file_frames_total":240000,
  "scn":null,"seed":null,"scn_crc32":0,"engine_state":0,"engine_error":0,"uac_play_file_frames":240000,"uac_play_sent_frames":240000,
  "uac_play_crc32_l":2898761450,"uac_play_crc32_r":2898761450,"uac_play_first_frame_index":96336,"uac_play_last_frame_index":336335,
  "uac_play_first_frame_sof":4054,"uac_play_missed_intervals":0,"uac_play_first_miss_frame_index":0,"uac_play_stream_bits":24,
  "uac_play_stream_channels":2,"uac_play_cut_count":0,"uac_play_cut_drop_frames":0,"uac_play_cut_zero_frames":0,
  "uac_play_cut_first_index":0,"uac_play_cut_rejected":0,"underruns":0,"pcm_drained":1,"gap_frames":240000,"scheduled_index":null,
  "start_late":0,"start_tick":61002,"end_tick":66119,"att_tenths_db":0,"dur_frames":null,"loops":1,"end_cause":"end",
  "engine_deadline_misses":0,"watterson_version":0,"watterson_digest_hi":0,
  "watterson_digest_lo":0,"watterson_latency_frames":0,"watterson_paths":0,"engine_digest_hi":0,"engine_digest_lo":0},{"seq":2,...}]}}
```

| Progress field | Meaning |
| --- | --- |
| `state` | `idle` (none since boot), `running` (an item is prepared, waits for its stream index or plays), `gap`, `paused_ring_full`, `done`, `aborted` (STOP), `error` (an item error under the stop policy) |
| `name`, `run` | The running or last playlist's file name and its number since boot (1 for the first) |
| `policy` | `stop` or `continue` |
| `items`, `index` | Its items, and the one running or next (`items` once all have run) |
| `completed`, `failed` | Items that ended, and of those the ones that ended in an error |
| `pending`, `oldest_seq`, `next_seq` | Blocks not yet acknowledged (all playlists), the oldest one's number (0 when none) and the number the next block gets |
| `tick` | The RTOS tick (ms since the scheduler started, the clock of the blocks' `start_tick` and `end_tick`) when the reply was read. A board reset restarts it, so a drain that lost its connection tells a reset (its ring is gone) from a USB re-enumeration (the board ran on). Images before it have no `tick` |
| `blocks` | The oldest pending blocks, at most 2 on current firmware, oldest first. A host may accept 3 from earlier firmware. |

A block's members, in order:

| Block field | Meaning |
| --- | --- |
| `seq`, `run`, `item` | Its number, the playlist (`run`) and the item's index in the file, from 0 |
| `label`, `file`, `route` | The item's label (null without one), WAV and route (`usb`, `codec`) |
| `result`, `player_error` | `done`, `aborted` or `error`; an error's `player_error` (STATUS's code; 21 for a USB item without a stream), else 0 |
| `file_frames_total` | STATUS `file_frames_total`: the card's frames |
| `scn`, `seed`, `scn_crc32` | The item's `scn=` and `seed=` (null without them), and zlib CRC-32 of the scenario file's bytes as checked at the start (0 without `scn=`: the item played no scenario) |
| `engine_state`, `engine_error` | STATUS's: 0 clean, 1 a scenario active, 2 rejected (the play went on clean), 3 failed |
| `uac_play_file_frames` .. `uac_play_cut_rejected` | The item's play as STATUS and `USB?` report the current or last play when it ended, under their names: `uac_play_file_frames`, `uac_play_sent_frames`, `uac_play_crc32_l`/`_r`, `uac_play_first_frame_index`, `uac_play_last_frame_index`, `uac_play_first_frame_sof`, `uac_play_missed_intervals`, `uac_play_first_miss_frame_index`, `uac_play_stream_bits`, `uac_play_stream_channels`, `uac_play_cut_count`, `uac_play_cut_drop_frames`, `uac_play_cut_zero_frames`, `uac_play_cut_first_index`, `uac_play_cut_rejected`. A codec item's `uac_play_*` are 0 but for its frames sent and its cuts, as STATUS and `USB?` report a codec play |
| `underruns`, `pcm_drained` | STATUS's codec-route counters (also set on the USB route: `pcm_drained` 1 for a play that completed) |
| `gap_frames` | The item's gap in 48 kHz frames |
| `scheduled_index`, `start_late` | The stream index the USB item was scheduled to start at (null when it started when ready), and 1 when its first frame went out elsewhere; an item that sent no frame (STOP while it waited, a fault before it armed) never started late |
| `start_tick`, `end_tick` | RTOS ticks (ms since the scheduler started; the player asserts one tick per millisecond) when the item began, before its `LOAD`, and when its play ended |
| `att_tenths_db` | The item's `att=` in tenths of a dB below full level: 0 without it, 200 for `att=-20.0`, at most 400 |
| `dur_frames` | The item's `dur=` in 48 kHz frames; null without it |
| `loops` | Whole passes of the card the frames sent cover: the engine output they cover divided by the card's frames, rounded down (1 for a card played once, 0 for an item that ended in its first pass) |
| `end_cause` | Why the item ended: `end` (every frame: the card's, or the duration's), `stop`, `underrun`, `missed_interval`, `stream_closed`, `stall`, `sd_read`, `usb_assert`, `deadline_miss`, `no_stream` (a USB item without a stream, error 21) or `fault` (another error: `player_error` names it). See [Duration](#duration) |
| `engine_deadline_misses` | The item's engine deadline misses, as `USB?` reports them for the current or last play |
| `watterson_version`, `watterson_digest_hi`, `watterson_digest_lo`, `watterson_latency_frames`, `watterson_paths` | The configured channel identity: algorithm version, high and low 32-bit digest halves, latency in frames and path count. Each is uint32 and all five are zero when the item had no successfully configured Watterson stage. The identity is latched for the actual item play; card preflight cannot overwrite it. |
| `second_source_version`, `second_source_source_frames`, `second_source_excerpt_start_frame`, `second_source_excerpt_frames`, `second_source_digest_hi`, `second_source_digest_lo`, `second_source_latency_frames`, `second_source_reference_rms` | Measured second-source identity for the actual item play: algorithm version, whole WAV frame count, selected source interval, high/low halves of its canonical PCM24 FNV-1a digest, offset-filter latency and raw excerpt RMS. All are uint32 except RMS (a finite number in (0,1] for an active source). All eight are zero without a successfully configured second source. Calibration includes zeros and precedes S/I gain; the identity survives completion and is not replaced by playlist preflight. |
| `engine_digest_hi`, `engine_digest_lo` | High and low 32-bit halves of the engine output digest before live controls and CUT. Zero when the engine did not process output (a clean item or one that did not start). |

An item that could not start (no stream) reports every play field 0.
`PLAYLIST?` has its own budget, `playlist_json_worst_case_bytes()`: 3859 of
4096 bytes with two blocks at their widest. Current firmware returns at most
two blocks; clients may accept the three-block replies emitted by earlier
firmware. STATUS is unchanged.

### Lane recipe: the acceptance battery

The former lane recipe is maintained in the [testing guide](testing.md).
This anchor remains for existing links. The sections above define the wire
format and playlist semantics; use the [Windows USB bench guide](windows-usb-bench.md)
for serial-bound media staging and capture setup.

### Lane recipe: attenuated, looped and long rows

Acceptance commands and expected comparisons for these rows are in the
[testing guide](testing.md).

## Live commands

| Command | Meaning |
| --- | --- |
| `CW ON`, `CW OFF` | Enable/disable CW slot 0 (backward-compatible form). |
| `CW FREQ 1800`, `CW CI 3` | Set slot 0 carrier frequency in Hz and source-to-carrier RMS ratio in dB. |
| `CW 1 ON`, `CW 1 OFF` | Enable/disable a selected slot; valid slots are 0 through 3. |
| `CW 1 FREQ 2800`, `CW 1 CI 6` | Set frequency or C/I independently for a selected slot. |
| `STATIC ON`, `STATIC OFF` | Enable/disable seeded static crashes. |
| `STATIC RATE 1`, `STATIC PEAK 20` | Crash arrivals per second and peak dB relative to source RMS. |
| `FADE NOW 18 250` | One raised-cosine fade, depth 18 dB, total duration 250 ms. |
| `SWEEP CW FREQ 300 3400 16 500` | Sixteen equally spaced frequency values, dwelling 500 ms at each step. CW must be enabled separately. |
| `SWEEP CW CI 20 -3 16 1000` | Sixteen equally spaced C/I values at one-second intervals. |
| `SWEEP CW 2 FREQ 300 3400 16 500` | Apply a frequency or C/I sweep to one selected CW slot. |
| `SWEEP FADE 6 30 5 1000 250` | Five fades with depths 6, 12, 18, 24 and 30 dB, one second apart, each lasting 250 ms. |

Prefix a live command with `AT:<output_frame> ` to specify its exact application
position. Otherwise, the player task queues it at the next unproduced output
frame and acknowledges that value as `result.apply_frame`. The acknowledgement
also reports the number of events accepted. A sweep is accepted atomically.
For a nonuniform C/I sequence such as +20,+10,+6,+3,0,-3, schedule individual
`AT:` commands at the desired positions.

Positions refer to the PCM stream **after** scenario sample slips and before
the [cuts](#cut-injector) and the output FIFO: a DROP's frames count, a
ZERO's zeros do not. The FIFO may already contain roughly 0.5 to 0.8 seconds of earlier
audio, so command acknowledgement is not an analog timestamp. Late commands,
exhausted queues, or a finished producer are rejected. No file I/O, formatting
or control processing occurs in the audio callback.

A refused live control's `error.code` is `BUSY` while a playlist holds the
player; `BAD_STATE` unless the player waits after a LOAD, or a run is
buffering or playing with a reference and has neither queued its last file
frame nor been told to stop; or else the live controller's refusal:

| Live control refusal | Why |
| --- | --- |
| `not_configured` | The controller has not been reset with a reference; the player resets it at LOAD, so no reply carries this today |
| `invalid_event` | A value out of its range (CW frequency 0 or at least 24 kHz, C/I outside -120 to 120 dB, static rate outside 0.001 to 1000 per second, peak outside -120 to 60 dB, fade depth outside 0 to 120 dB), or a sweep whose last step would pass the last output frame |
| `late_event` | Its output frame has already been produced |
| `pending_full` | 64 controls already wait for their frames (a sweep needs one slot for each step) |
| `capture_full` | The controller has taken 128 events since the last LOAD (a sweep takes one for each step) |

The live signal order is source multiplied by live fade, then the four-slot live CW bank and static
added, followed by PCM24 quantization. The fixed scenario is processed first.
The independent live seed does not alter scenario RNG streams. Without live
controls, PCM samples pass through unchanged and are still hashed.

The effective live reference is the explicit raw REFERENCE times scenario
source gain, or the scenario's scaled `reference_rms`. With neither, firmware
measures the first `min(N,48000)` clean source frames and rewinds before playback.
A zero reference permits clean playback but rejects live impairments; provide
REFERENCE when the WAV starts with silence. `live_reference_rms` records the
effective scaled value. The seed and reference must be retained for replay.

The bounded controller retains 128 accepted events and has 64 pending slots.
A sweep has 2 to 16 values. CW phase runs while disabled and survives updates.
Static uses 12 concurrent 1700 Hz tone-ring crashes with 20 ms exponential decay;
OFF ends active tails. A new live fade replaces the previous fade. Rejected
controls never silently evict earlier events.

## Host operation and retained evidence

The physical serial tools need the explicitly installed dependency in
`host/requirements-control.txt`. They require a selected port and expected USB
serial, then verify fixture identity before mutation. Tests do not open hardware.
Use the existing media handoff procedure before allowing FatFs access.

```powershell
python host/live_control.py --port "REPLACE_WITH_ACTUAL_PORT" --expected-serial "REPLACE_WITH_ACTUAL_SERIAL" --record artifacts/live.jsonl interactive
```

An example interactive session is:

```text
MEDIA LOCAL
LOAD:TEST.WAV
SEED:42
CW FREQ 1800
CW CI 3
CW ON
PLAY:0123456789abcdef0123456789abcdef
STATUS?
SWEEP CW FREQ 300 3400 16 500
STATUS?
STOP
STATUS?
```

Retain a final STATUS after cleanup; the final `live_frame`, effective reference,
seed, control acknowledgements, source WAV/SCN and identities are needed to
reproduce the generated PCM. Retrieve the retained source artifacts through MSC
only after local file ownership is released. A filename alone is not source identity.

```powershell
python host/live_control.py export artifacts/live.jsonl artifacts/replay.json
build/host-release/host/render/signal_lab_render.exe --source TEST.WAV --live-events artifacts/replay.json --output artifacts/replayed.wav --sidecar artifacts/replayed.json
```

Add `--scenario TEST.SCN` when the board used that scenario. The host renderer
replays the same acknowledged frame positions, including sample slips before
the live stage. Its event reader refills the bounded pending queue; it does not
require all 128 captured events to fit simultaneously. `live_frames` can retain
a stopped producer prefix. These are generated frames, not proof of samples
that reached the analog connector before STOP.

## Validation boundary

Portable tests check control levels, phases, sweeps, random-stream isolation,
queue errors and PCM identity across block sizes. Renderer tests cover exported
plans, sample slips, exact uint64 values and invalid artifact rejection. A host
harness compiles the actual player/CDC owner code against deterministic I/O
stubs to exercise lifecycle, backpressure and disconnect handling. The CUT
injector's stage and log are checked against a plain reference over every
block size and step limit (`wfg-cut-schedule`); the player's plays with
cuts on both routes, part way and without cuts, scenario plays with cuts
against the host render (the engine's digest that of the uncut render),
and live controls whose `AT:` positions run on through dropped frames
(`wfg-player-live-owner`); the host predictor against the firmware's own
stage, on cards and on scenario renders (`wfg-cut-host`). Given image A's
card (CMake `WFG_FSRAND_WAV`), both reproduce image A's CUT rows. PLAYLIST's parser, result ring
and timing rules are checked portably (`wfg-playlist`); the host's playlist
tool against the firmware's parser file by file and its start offsets
against the firmware's `next_start_index()` (`wfg-playlist-host`, which also
checks rendered scenario items and `split` on synthetic captures); the
sequencer on the player harness (`wfg-player-live-owner`): the start's
refusals (each leaving STATUS's SD read counters as they were), the lane's
six-item battery with exact device-time gaps checked frame by frame against
the capture, STOP in an item, a gap and a scheduled wait, both error
policies and the selection an item without a stream leaves, the ring's
pause and ACK, route switching, each item's own cuts, scenario and seed (and
no `<FILE>.SCN` without `scn=`), a last gap on a stream that stops sending,
and `PLAYLIST?`'s order and budget.

The RT1170 image uses static storage for the controller and FIFO. Plugin-enabled
writable builds additionally reserve source-encoder and writer workspace. Firmware timing, stack headroom on the board, live USB under
audio load, on-card write behavior and host/target live digest parity require a
hardware testing with the current firmware.

## Custom encoder commands

`ENCODE ID PROFILE INPUT.BIN OUTPUT.WAV` passes the profile unchanged to the
selected adapter. Add module-specific parameters there when they fit the bounded
wire contract. The [custom encoder tutorial](custom-encoder-tutorial.md) documents
field limits, raw CDC framing, host validation, and how to add new commands in
both parsers and the encoder-owning task. Plugins do not currently install
arbitrary CDC verbs or alter a running encoder's configuration.
