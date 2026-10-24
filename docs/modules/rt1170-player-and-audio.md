# RT1170 Player and Audio Pipeline

The [RT1064 port](rt1064-board.md) reuses this pipeline. Board-specific setup
and acceptance limits are documented separately; results on one board do not
establish results on the other.

## 1. Scope

This guide covers the MIMXRT1170-EVK CM7 firmware owner loop, SD WAV reader,
static and live impairment processing, OCRAM output FIFO, SAI1/eDMA transport,
WM8960 codec task, the USB route (the sound card's capture stream fed from the
USB interrupt, and its per-play statistics), the PLAYLIST sequencer that
plays a card's list of items unattended, and engineering counters. The
USB device itself (identity, descriptors, UAC2 controls) belongs to
[Protocols, Media and Artifacts](protocols-media-and-artifacts.md).

The default image plays supplied WAV files with static and live effects. Optional
[encoder modules](../encoder-plugins.md) create saved WAVs before playback;
they do not change the audio pipeline.

## 2. 50,000-foot view

The RT1170 player is a producer/consumer pipeline. A lower-priority player task
reads and processes audio ahead of time. A highest-priority audio task drains
small fixed blocks at the 48 kHz hardware deadline.

```text
microSD WAV -> player task: card blocks -> scenario engine -> live controls -> cut stage (DROP discards, ZERO inserts zeros)
-> gain (a PLAYLIST item's att=) -> 32,768-frame OCRAM FIFO -+-> 128-frame audio blocks -> SAI1/eDMA -> WM8960   (OUTPUT CODEC)
                            +-> 48-frame USB chunks (USB ISR) -> UAC2 iso IN 0x84 -> PC capture   (OUTPUT USB)
```

This separation keeps SD latency and impairment cost away from the codec callback
and the USB interrupt. The player task is the single owner of source, engine,
and control mutations. `OUTPUT CODEC` / `OUTPUT USB` select the consumer; the
producer and the FIFO are the same on both routes. A `PLAYLIST` runs one such
play per item, in the same task, with a gap between them.

## 3. How to use it

The codec route uses the board's WM8960 output. Select and verify any external
capture device explicitly. USB provides the control, media, and optional digital
audio paths.

Build the writable player:

```powershell
cmake --preset rt1170-player-release
cmake --build --preset rt1170-player-release --parallel
```

Build the inspection profile when the card must not be changed:

```powershell
cmake --preset rt1170-player-readonly-release
cmake --build --preset rt1170-player-readonly-release --parallel
```

After programming, resolve the board by DAPLink serial and resolve CDC by USB
VID/PID/serial. Do not use a remembered COM number. For playback:

1. Put the card in host ownership.
2. Stage `/WG/PLAY.WAV` and optional `/WG/PLAY.SCN`.
3. Flush and dismount the Windows volume.
4. Send `MEDIA LOCAL` with the host-side dismount assertion. The board refuses
   (`HOST_LOCKED`, `HOST_WRITING`) while Windows still holds the volume.
5. Send `LOAD:PLAY.WAV`.
6. Configure controls if needed and send `PLAY`.
7. Poll `STATUS?`, then `STOP` if the run is intentionally bounded.
8. Return media to host ownership only after audio and file activity stop.

For the USB sound card the steps are the same, with three additions (the
[bring-up runbook](../usb-sound-card-bringup.md) has the commands):

- Before `PLAY`, start every PC program that records from "Line (RT1170
  Player)" and wait until `uac_missed_intervals` has not changed for 3 s
  (the lane note below). `PLAY` answers `NO_STREAM` while none records.
- Send `OUTPUT USB` while the player is stopped (after a reset the route is
  the codec again).
- After the run, read the `uac_play_*` fields: frames, CRCs, indices and
  `uac_play_missed_intervals`.

On either route a [CUT](#cut-injector) schedule sent before `PLAY` drops
frames of that play's output and puts exact zeros into it, after the
channel, for that play only. A [PLAYLIST](#playlist-sequencer)
plays a staged list of cards unattended: stage the cards and the playlist
file in `/WG/`, then `PLAYLIST:<NAME>` instead of steps 5 to 7, and read the
result blocks with `PLAYLIST?` and `PLAYLIST ACK` (or `live_control.py
playlist drain`).

### SD second-source overlay

A scenario may add one mono 48 kHz PCM24 WAV excerpt from `/WG/` as a second
source. Its `source_path` is an ASCII basename, not a host path; the player
opens it under `2:/WG/`. The player task exclusively owns the mounted card and
both file handles. Audio callbacks never read the source.

For a normal `PLAY`, the player opens the source, validates its format and
excerpt, then measures excerpt RMS and PCM24 identity in bounded chunks of at
most 2,048 frames before engine configuration. The same file handle stays open
for bounded active-block reads during the play. The measurement result is
copied into the play identity; it does not borrow the provider's state.

`PLAYLIST` admission performs structural checks of each referenced source
(format and excerpt bounds) without scanning the whole WAV. After the playlist
has been accepted, each item reopens and measures its source before output
starts. STOP is checked between bounded measurement reads. Each item releases
the engine's borrowed stage before closing the source, including refusal,
cancellation, and cleanup paths. See the [second-source module guide](second-source.md)
for the scenario fields, scaling rule, and identity details.

Host and cross-build checks do not establish sustained SD throughput, absence
of underruns, or target audio parity. Those require separate board acceptance.

## 4. Scientist and maintainer view

### Timing and buffering

The player reads 6 KiB PCM chunks, corresponding to 2048 mono packed-PCM24 frames.
The static engine can expand a chunk to at most 2304 frames. The producer fills a
32,768-frame OCRAM2 FIFO of right-aligned signed 24-bit values, normally sleeping
above 24,000 frames and waking below 14,400. A 4,800-frame critical threshold
exposes reduced safety margin. This preserves the former FIFO byte footprint.

The codec uses stereo 32-bit I2S slots even though the logical fixture stream is
mono PCM24. Each signed sample is left-aligned into the 24 valid most-significant
bits of both physical channels. The codec hook runs in the audio task, not the ISR, and must
perform no allocation, file I/O, or logging.

### Memory placement

The generator's linker script, `cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld`,
puts RAM data in three regions. The Cortex-M7 data cache never covers DTCM;
over OCRAM the SDK's `BOARD_ConfigMPU` makes it write-back (MPU regions 6 and
7), except in the window that region 10 makes non-cacheable.

| Region | Address, size | Holds | CPU cache |
|---|---|---|---|
| `m_data` (DTCM) | `0x20000000`, 256 KB | `.data`; `.ncache`, the SDK's `NonCacheable` section (the codec's SAI buffers, the SD controller's ADMA2 descriptors); `.bss` (task stacks, player and engine state, TinyUSB's other buffers); heap and main stack | never |
| `m_data2` (OCRAM2) | `0x202C0000`, 480 KB | `.ocram`: the 32,768-frame output FIFO (128 KB); `.ocram.watterson` (NOLOAD), the Watterson channel (18,208 bytes); `.ocram.secondary_source` (NOLOAD), the fixed-storage SD second-source provider; the [PLAYLIST](#playlist-sequencer) table and ring of 32 result blocks; the CDC request line (384 bytes), the player's command mailbox (384) and reply mailbox (40), the [CUT injector](#cut-injector)'s state, the play's watch and the [critical-error record](#critical-errors) (16, in every image); and the encoder workspace and WAV writer in images built with encoder plugins | write-back |
| `m_ncache_ocram` (OCRAM2's top) | `0x20338000`, 32 KB | `.ncache_ocram`: the MSC chunk buffers, the SD bounce buffer (`waveform_msc.c`) and TinyUSB's MSC endpoint buffer (16 KB each) | none (MPU region 10) |

Read the linked image's map for current totals; features and optional encoder
workspaces change occupancy. Do not reuse memory totals from another binary.
OCRAM1 (`0x20240000`, 512 KB) is not used by this layout. RT1064 has its own
linker layout; see the [port guide](rt1064-board.md).

Rules for new memory:

- New buffers go to OCRAM, not DTCM. DTCM keeps the stacks and the state the
  hot paths touch.
- A buffer that a DMA master (USB, SD, eDMA) and the CPU share lies in DTCM or
  in the non-cacheable window, so neither side needs cache maintenance. In
  cached OCRAM its owner must clean the cache before every DMA read of it and
  invalidate after every DMA write to it, after the transfer as well as
  before, because the Cortex-M7 may fill a line speculatively while the DMA
  writes. TinyUSB's ChipIdea driver cleans and invalidates only before a
  transfer, so its buffers go to DTCM or the window, never to cached OCRAM.
- A window buffer lives in an input section named `.ncache_ocram.<buffer>`.
  TinyUSB's buffers take `CFG_TUD_MEM_SECTION`, set per TinyUSB source
  (`usb/tusb_config.h`, `rt1170/CMakeLists.txt`). The window is an MPU region,
  so its size is a power of two and its address a multiple of it; the
  linker script asserts both, and that it does not overlap `m_data2`. To grow
  it, double `NCACHE_OCRAM_SIZE`: `m_data2` shrinks by as much.
- After every player link `scripts/check-memory-placement.py` reads the map
  and fails the build unless both MSC buffers lie in the window and the
  symbols `BOARD_ConfigMPU` builds region 10 from (`__NCACHE_REGION_START`,
  `__NCACHE_REGION_SIZE`) describe it exactly. A misspelt section name would
  otherwise make the linker place the buffer by its orphan rules without a
  warning. `wfg-memory-placement` tests the check against map excerpts. A
  second run checks that `.ocram.player_cuts`, `.ocram.player_command`,
  `.ocram.player_reply`, `.ocram.control_line`, `.ocram.playlist`,
  `.ocram.player_watch`, `.ocram.critical_error`, `.ocram.watterson` and
  `.ocram.secondary_source` lie in `m_data2`; `.ocram.watterson` has a
  20,480-byte maximum and `.ocram.secondary_source` has an 8,192-byte maximum.
- The player task runs on a 16 KiB stack (`player_task_stack_words`), and the
  scenario reader, FatFs, the SD driver and the engine all run on it. Every
  player source is compiled with `-fstack-usage -fcallgraph-info=su`, and after
  every link of a release image `scripts/check-stack-usage.py` reads the `.ci`
  files of the linked objects (a debug image, `-Og` with the debug console,
  is not checked). It fails the build when the deepest chain of calls from
  `player_task_entry`, plus 232 B of reserve, does not fit in
  `player_task_stack` (the map gives the size). The reserve covers the
  FPU exception frame, the registers a context switch saves, the top
  alignment and FreeRTOS's 16 B overflow pattern. The build also fails when
  owned code recurses (the JSON reader did, one level per nesting, until
  `52bd33c`), when an owned frame is not static, and when the chain reaches a
  virtual or function-pointer call that `scripts/stack-usage-rules.json` does
  not resolve, or a local function no `.ci` file describes (a constructor's
  complete-object alias is taken as the base-object one GCC describes). A
  call into newlib or libgcc counts 64 B. At `52bd33c`, the chain was
  12,224 B + 232 B of 16,384 B, through `parse_scenario`'s 9 KiB frame.
  `wfg-stack-usage` tests the check against call-graph excerpts.

  The fresh WP9 ARM release link places the 18,208-byte channel in
  `.ocram.watterson` in `m_data2`; its ELF section is NOBITS (no CONTENTS
  flag in `objdump -h`), as expected for the linker's NOLOAD `.ocram` output
  section. The map check enforces the 20,480-byte bound. The secondary WAV
  provider and its packed buffer are likewise placed in `.ocram.secondary_source`
  with an 8,192-byte post-link limit. The stack check
  reports 12,536 of 16,384 bytes. Host
  player tests poison the channel storage before configuration and compare
  Watterson playback through USB and codec routes with CUTs, then check
  playlist seed identity, clean-item clearing, a continuous loop and the
  retained engine digest. These are host and cross-link checks; no board audio
  or bench execution is implied.
- The USB task (`usb_task_entry`) runs on an 8 KiB stack (`usb_stack_words`):
  TinyUSB's device task and the class drivers' callbacks (CDC, MSC with its SD
  reads and writes, UAC2), the control line service and the DD008 session. The same
  check reads its chain after every link of a release image and fails the
  build if it does not fit in `usb_task_stack`. TinyUSB reaches the class
  drivers through function pointers, so `scripts/stack-usage-rules.json`
  lists each TinyUSB caller with the callbacks it can reach. These are the
  fields of the `usbd_class_driver_t` entries that `usbd.c` builds for
  `CFG_TUD_CDC`, `CFG_TUD_MSC` and `CFG_TUD_AUDIO` (`rt1170/usb/tusb_config.h`):
  `tud_rhport_init` calls `init`, `configuration_reset` calls `reset`,
  `tud_task_ext` (the control request handling is inlined into it) calls
  `open`, `control_xfer_cb` and `xfer_cb`, and also
  `tud_vendor_control_xfer_cb` and the one deferred function
  (`proc_async_io_done`), and `dcd_event_handler` calls the audio driver's
  `xfer_isr` and `sof`. The link wraps `dcd_event_handler`
  (`--wrap`, for the EP0 order), so the MSC driver's calls from the task
  reach `__wrap_dcd_event_handler` and the glue's
  `wfg_usb_ep0_route_event` (72 B) before TinyUSB's handler. The check
  follows the same wrap (`--wrap`, from the one list `wfg_wrapped_symbols`
  in `rt1170/CMakeLists.txt` that the link options use), and the rules
  name the glue's forward (`__real_dcd_event_handler`, TinyUSB's handler)
  and its `ENDPTCOMPLETE` poll, and `m110::critical_error()`'s handler,
  `record_critical_error()`. The DD008 session's callbacks are the four
  `rt1170/tx_usb.cpp` passes to it. `wfg-stack-usage` checks these lists
  against `usbd.c`'s table, `tusb_config.h`, the port's forward and the
  installed handler. Before this check the task was outside it. The build
  prints the chain: 2,352 B + 232 B of 8,192 B (5,608 B spare), through the
  control line's STATUS reply (`handle_request`, `queue_status`,
  `begin_json`), deeper than any TinyUSB path; the MSC completion through
  the wrapper takes 328 B from `mscd_xfer_cb`.
- `.ocram` is `NOLOAD`: startup writes none of it, so a C++ object there has
  no initial value until its owner writes it. The output FIFO is written
  before it is read; `start_player()` writes the cut state and both
  mailboxes before the player task exists, and resets the playlist state in
  place, field by field (it is too large for a temporary on the startup
  stack); the request line is written byte by byte before it is read. The
  USB task reads the cut log only once a `PLAY` has been accepted. The
  application task clears the critical-error record before anything
  else (`install_critical_error_handler()`).

### USB route

`OUTPUT USB` sends `PLAY` to the composite device's UAC2 capture stream ("Line
(RT1170 Player)", see the [protocols guide](protocols-media-and-artifacts.md))
instead of the codec. The producer and the OCRAM FIFO are the same; the
consumer is the USB ISR:

- At every IN completion (one per USB millisecond) TinyUSB has already loaded
  the next packet. The ISR commits the completed packet and tops the IN FIFO
  back up to two 48-frame chunks: from `player_consume_from_isr()` while the
  play is armed, exact zeros otherwise. A chunk's file frames lead it, as
  L = R = `sample << 8` in 4-byte subslots, the word the codec gets; the ISR
  writes the chunk in the open alternate's
  [capture format](protocols-media-and-artifacts.md#capture-formats): those
  words unchanged (24-bit stereo), their top 16 bits (`sample >> 8`, 16-bit
  stereo) or the left channel's alone (16-bit mono).
- `PLAY` needs an open stream (a PC client recording), else `NO_STREAM`.
  Accepting it records `uac_play_accept_frame_index` and restarts the play
  statistics. Every accepted `PLAY` restarts them, on either route: a codec
  `PLAY` sets every `uac_play_*` field to 0 (no USB play), so STATUS never
  shows the previous USB play's values beside a codec run.
- The player arms the stream once the FIFO holds its prefill (or the file has
  ended), in the critical section that enters `playing`. Before that, and after
  the last file frame, the stream is exact digital silence; there is no dither.
- The run is `done` only once every file frame is confirmed sent; there is no
  codec-style 384-frame drain. STOP and faults return the stream to silence;
  chunks already queued still go out and are waited for (a closed stream drops
  them), so the statistics are final when the run reports its end. The
  exceptions are a stall, where error 20 ends the wait at once, and queued
  file frames that have not fallen for more than 100 ms while IN completions
  keep coming, where error 26 ends it (#45); a drain after the file has ended
  is bounded the same way. Either way, if the chunks still queued (at most
  three, 144 frames) go out later, they still count, the gap that delayed
  them included. The statistics describe every packet TinyUSB sent, which
  is what the host received unless a completion did not confirm its packet:
  that run ends with error 26 (below).
- Errors: 19 the stream closed (alternate 0, another alternate, also straight
  from one format to another, bus reset, unplug,
  SET_CONFIGURATION) after `PLAY` was accepted and before every file frame was
  confirmed sent; a close after that drops nothing and the run is `done`.
  20 more than 100 ms between IN completions while armed. The USB ISR judges
  each gap when the completion that ends it comes, so a gap counts even when
  it ends while the producer sleeps; the player task finds a gap that has not
  ended in its loops: within the 2 ms drain poll, or at the producer's next
  wake (up to about 250 ms later while it paces the FIFO). 26 once no more
  file frames could join those queued (the file had ended, or STOP or a
  fault disarmed the play), none of them confirmed sent for more than 100 ms
  while IN completions kept coming (`QueuedFramesWatch`, judged at each
  drain poll and at each 1 ms poll of the wait after a STOP): a backstop, so
  a stream that keeps completing without sending them cannot hang the run.
  26 also when a completion reported a size other than the packet TinyUSB
  had loaded while the play had file frames queued (silence before them
  included; `UacStream::play_mismatched()`): its frames count as sent, but
  the host may not have received the packet. The play runs to its end and
  the run ends with error 26 unless an earlier fault is its error (a looped
  item ends at once), so the host's capture shows whether it arrived.
  An underrun is error 15, as on the codec route.
- `MEDIA HOST` is refused for the whole run, armed included.

The play statistics count only frames confirmed sent: each chunk written to
the FIFO keeps a record (its stream index, file frames and the CRCs after it)
until its completion, so they stay an exact prefix when a play stops or the
stream closes part way. A completion confirms the packet TinyUSB loaded for
it at the completion before, one chunk or a zero-length packet, which the
FIFO's level tells, whatever size it reports: the ChipIdea driver reports a
transfer that ended in error as its expected minus remaining bytes, and a
record kept for such a packet would never retire, fill the 8-record ring and
stop the stream (#45). A reported size other than the loaded packet's counts
in `uac_short_packets` and `uac_sent_mismatches`, and, while the play has file
frames queued, leaves the play's delivery unconfirmed: the statistics then
include a packet the host may not have, and the run ends with error 26.
Stream indices count frames since the stream opened,
the silent pre-fill included, and wrap after 24.8 h.

A `PLAY` accepted (on either route) while an earlier USB play's file chunks
are still queued, which only a stall leaves behind (a host that stopped polling
for more than 100 ms; at most three chunks), restarts the statistics with
nothing pending: those chunks count for no play. The earlier play's statistics
are replaced, and the new play's describe exactly its own frames. If the host
polls again, the leftover file frames reach the wire right after the new
`uac_play_accept_frame_index`, ahead of the new play's first file frame, so a
capture checked after a stall shows them between the accept and first indices.

Missed intervals are counted twice. `uac_missed_intervals` is the stream's
diagnostic since boot. It includes the pauses Windows makes in its polling
while the audio engine reconfigures the shared stream: as the stream starts,
whenever a capture client joins or leaves, and while the Sound settings level
meter runs (those pauses can occur while the stream is idle,
for example 4 intervals within about 3 s of opening, and the count stayed
flat during stable plays, one of them 5 minutes long). `uac_play_missed_intervals`
applies the same rule (a gap of n frames between completions misses n - 1; a
completion handled just past a frame boundary reads as a gap of 2 that the next
completion's gap of 0 takes back) to the gaps that delay the play's file: each
gap that ends while the play has file frames queued, armed or not.

- Start: no file frame is queued before the play's first completion after
  arming, so a skip that ended before arming, or was still going on when the
  play armed, stays the stream's. A late read before the arming that is taken
  back after it changes only the stream's count; the play's never drops below
  zero.
- After a disarm: the chunks a STOP, a fault or a stall leaves queued still go
  out and still count, and so do the gaps that delay them. A stall therefore
  reads the same whoever notices it first: a 101 ms gap between completions
  counts 100 whether the host polls again before the player task looks or
  after it has latched error 20. A gap counts only once a completion ends it,
  so a host that never polls again leaves error 20 and no count.
- End: after the completion that confirms the play's last file frame sent,
  gaps are the stream's alone, also before the player task notices and
  disarms. The next completion may still take back a late read the play
  counted. So `uac_play_first_miss_frame_index` never lies past
  `uac_play_last_frame_index` once the play's chunks are all sent.

As on the stream, a late read is told from a miss only by the next completion,
so at the end of the file the count can be one high when a late read meets a
real gap. The player reads the count once it is settled: `stop_output()`
waits, at most 2 ms (`miss_settle_ms`), for the completion after the gap that
last added to it (`PlayStatistics::miss_unconfirmed`), so a late read of the
last file frames is taken back before STATUS or a playlist block reads it.
A looped PLAYLIST item ends only on missed intervals that completion has
confirmed (`confirmed_missed()`).

| STATUS field | Meaning |
|---|---|
| `uac_play_file_frames` | File frames confirmed sent; with a [CUT](#cut-injector) schedule the engine output frames the frames sent cover (sent - zeros + drops): the card's frames for a whole play unless its scenario slips samples |
| `uac_play_crc32_l`, `uac_play_crc32_r` | zlib CRC-32 of each channel's 24-bit source samples in the frames confirmed sent, as 3-byte little-endian, before the conversion to the capture format: the same in every format; for a mono PCM24 file both equal `zlib.crc32(<data chunk>)` |
| `uac_play_first_frame_index`, `uac_play_last_frame_index` | Stream indices of the first and last file frame sent (0 until one is); the first leads a 48-frame chunk |
| `uac_play_first_frame_sof` | USB frame number (`FRINDEX[13:3]`) of the completion that carried the first file frame, extended to 32 bits across wraps from the stream's first packet |
| `uac_play_accept_frame_index` | Stream index when `PLAY` was accepted; first minus accept is the command-to-audio latency in frames |
| `uac_play_missed_intervals` | 1 ms intervals missed while the play had file frames queued, armed or not, each gap counted once a completion ends it (see above) |
| `uac_play_first_miss_frame_index` | Stream index of the first frame sent after the play's first missed interval (the packet whose completion ended the gap); 0 when it missed none. A late read taken back clears it with the count |
| `uac_stream_open`, `uac_stream_opens` | An alternate with a format selected now (which one is in `USB?`); selections since boot |
| `uac_stream_frames` | Frames sent since the stream opened, read as the reply is formed |
| `uac_short_packets`, `uac_missed_intervals` | Since boot: completed packets other than one whole packet of the open format, and 1 ms intervals without a completion (a stream diagnostic: includes Windows' pauses while it reconfigures the shared stream, at start-up, when clients join or leave, and while the Sound settings level meter runs). A completion handled just past a frame boundary is not a missed interval: the next completion's zero gap takes it back |
| `uac_host_volume`, `uac_host_mute` | The host's feature-unit settings, never applied; volume is the raw UAC2 value in 1/256 dB as its uint16 two's complement (0 is 0 dB) |
| `usb_stack_asserts` | TinyUSB assertion failures since boot |

The `uac_play_*` fields describe the current or last play on the USB route
since the last accepted `PLAY`; after a codec `PLAY` they are all 0.

Lane note: assert `uac_play_missed_intervals == 0` for each play (with the
file frames, CRCs and indices above). The CRCs are over the 24-bit source in
every capture format, so a bit-exact verdict also needs `USB?` after the play
to read `uac_play_stream_bits` 24 and `uac_play_stream_channels` 2; a 16-bit
or mono play reports the same `uac_play_*` values as a bit-exact one
([capture formats](protocols-media-and-artifacts.md#capture-formats)).
`uac_missed_intervals` is a stream
diagnostic that may show Windows' reconfiguration pauses from before the
play; do not assert it per play. A pause that overlaps the play's file counts
for the play, so the lane rule is: open all capture clients and let them
settle before `PLAY`. Start every program that records from the endpoint,
keep the Sound settings level meter closed, then wait until
`uac_missed_intervals` has not changed for 3 s (the bench saw every start-up
pause within about 3 s of opening) before `PLAY`, and start or stop no
client until the play is done. Log `uac_missed_intervals` before `PLAY` and
after the run, so a per-play failure can be told apart from reconfiguration
behaviour. Whether a play's missed intervals were the device's or the host's
is in `USB?` (`uac_play_unprimed_intervals`, see
[miss attribution](#miss-attribution-usb)).

### CUT injector

For the cross-layer distinction between CUT, scenario `sample_slip`, and USB
service gaps, see [Time-step faults](time-step-faults.md). This section covers
the player's CUT stage and counters.

`CUT:<offset>:<N>:<drop|zero>,...` gives the next `PLAY` a schedule of
dropped frames (DROP) and inserted zeros (ZERO); the command, its rules and
the `USB?` fields are in the
[protocol reference](../live-control-protocol.md#cut-injector). The board
only executes the list. It applies it after the channel: the scenario engine
and the live controls process every card frame, and the cut stage between
their output and the output FIFO decides what becomes of each output frame.
One implementation serves both routes and every play, clean or with a
scenario:

- **Pending, latched, logged** (`common/cut_schedule.hpp`, state in
  `player.cpp` `PlayerCuts`, OCRAM2). `CUT` validates the list in the player
  task and keeps it as the pending list. `cut::validate()` turns a list it
  accepts into a `ValidSchedule`, and that is the only type the pending
  list, a PLAYLIST item's `cut=` and the stage hold, so a list that breaks
  a rule cannot reach the stage. The `PLAY` that is accepted latches
  it into the cut stage, empties the pending list and restarts the play's
  log and route, in the critical section that resets the run's counters. The
  player task is the only writer; the USB task reads the log and the route
  under a critical section, and only once a `PLAY` has been accepted.
- **The card.** The producer reads the card in whole engine blocks (2048
  frames) from its data chunk on, in order and without a seek, whatever the
  schedule: the reader knows nothing of the cuts. After the 44-byte header
  of a canonical card every read starts word-aligned, so the SD driver keeps
  its multi-block DMA path. Each block goes through the engine and the live
  controls, and their output (up to 2304 frames with sample slips) goes to
  the stage (`CutStage::offer()`).
- **Stage steps.** The producer asks the stage for its next step, of at
  most one engine block's output (`output_step_frames`, 2304 frames, a
  `cut::StepLimit`, which is one frame at least: `StepLimit::of()` refuses
  0): send output frames up to the next cut or the block's end, send a
  ZERO's zeros (a step at a time), discard a DROP's output frames (up to the
  block's end, the rest from the next block; nothing reaches the FIFO, so a
  DROP costs engine time while the output drains, which the drop balance
  bounds, below), or ask for the next block (`pull`); once the card is read, the next `pull` ends the output
  (`CutStage::finish()`) and the step after it ends the play. Without
  entries every step sends a whole block's output, as the producer sent it
  before the injector, so a play without cuts is unchanged. The stage's
  places are 32-bit output frames; it refuses a block that would pass
  2^32 - 1 (`CUT_OUTPUT_LONG`, and the play ends with the `cut_log` fault),
  which no card reaches (a `static_assert` bounds the longest card's
  output), nor a looped item of at most 24 h unless its scenario's sample
  slips add some 147 million frames (a second `static_assert`).
- **DROP** discards engine output frames the engine and the live controls
  have already processed, clipped at the output's end: engine time, the
  scenario's events (a Watterson stage's tap clock among them) and the live
  controls' `AT:` positions run on through them, and the engine's digest
  and frame counts cover them.
- **The drop balance** (`cut::check_drop_balance()`, in `validate()`).
  Discarded frames cost the producer engine time with nothing for the
  FIFO, so the output drains through a DROP. Each DROP adds its length to
  the balance and the engine output frames before the next DROP take it
  back; `validate()` refuses a list whose balance passes
  `max_drop_balance_frames` (14400, three adjacent DROPs of 4800 frames) with
  `CUT_DROP_RUN`. A `static_assert` in `player.cpp` ties it to the FIFO:
  a DROP begins with the FIFO at its wake watermark (300 ms) less what
  earlier DROPs still owe, and at `min_producer_speed` (2, twice real time;
  image A measured 3.4 with an AWGN, CW and fade scenario) the balance plus
  the rest of the engine block that ends it drains at most 8352 of the 9600
  frames between the wake and critical levels. `wfg-player-live-owner`
  plays the longest accepted runs with a producer at 2.03 times real time:
  no deadline miss, the FIFO above its critical level.
- **ZERO** writes exact zero samples straight into the output FIFO, which
  the engine and the live controls never see: the zeros stay exact whatever
  a scenario or a live control does, and the output resumes where it left
  off. The FIFO's pacing applies to them as to the engine's frames.
- **The log.** The player logs each cut before any frame after its place
  reaches the FIFO, with that place in the post-cut stream (the FIFO's
  frames written so far): a ZERO as it begins, a DROP once its last frame
  is discarded or the output's end cuts it short. The log holds 16 cuts, one
  for each entry a schedule may have; a cut it refuses (`CUT_LOG_FULL`,
  which a valid schedule never reaches) latches the `cut_log` fault rather
  than leave the statistics short. When the output ends the log takes the
  entries that never applied, and when every frame is in the FIFO it notes
  where the stream ends. `player_cut_statistics()` tallies the log against
  the frames the route confirmed sent: the USB play's frames confirmed sent,
  or the frames the codec took (`file_frames_submitted`). A cut took effect
  once a frame at or after its place was sent (a DROP that runs to the end,
  once the whole stream was); the entries past the end count once the whole
  stream was sent. So STOP, a close or a stall leave an exact prefix.

The FIFO holds the post-cut stream, so everything downstream of it already
describes the frames sent: the USB play's records, CRCs and first and last
indices (`usb_audio.cpp` counts the FIFO's frames as the play's), and the
codec's `file_frames_submitted`; `file_frames_enqueued` counts the zeros
too, `file_frames_total` stays the card's. Only STATUS `uac_play_file_frames`
is converted back to the engine's output frames (sent - zeros + drops), so
it keeps its meaning: a whole play reports the card's frame count, as
without cuts, unless its scenario slips samples. The USB stream sees no gap:
a DROP shortens the stream and a ZERO lengthens it, so no interval is missed
and the miss counters never see a cut.

On the codec route a ZERO is real silence at the WM8960's output and a DROP
a phase jump; on the USB route both are exact in the capture. The host
predicts a play's values with `host/cut_schedule.py predict`: from the card
for a clean play, from the card's host render through its scenario for a
scenario play (`--scenario` with `--render`). `wfg-cut-host` checks it
against the firmware's stage and log on both.

### PLAYLIST sequencer

`PLAYLIST:<NAME>[:CONTINUE]` plays a card's list of items unattended; the
file format, the commands, the refusal codes and the `PLAYLIST?` fields are
in the [protocol reference](../live-control-protocol.md#playlist). The
sequencer runs in the player task, the card's single FatFs owner, around
the same run every `PLAY` starts:

- **Portable part** (`common/playlist.hpp`, constexpr and heap-free). The
  file parser takes the file's bytes in any split (the player feeds its
  6 KiB read buffer) into a fixed table of 256 items, one 511-character line
  at a time, and names the first line it refuses. Gaps are kept as exact
  48 kHz frame counts. `cut=` goes through the CUT command's own parser and
  rules (`cut::parse_entries`, `cut::validate`). The result ring, the
  progress and the timing rule (`next_start_index()`) live there too, so
  `wfg-playlist` checks them without the player, and `wfg-playlist-host`
  checks the host's parser (`host/make_playlist.py`) against this one.
- **Start** (`accept_playlist()`). With the player stopped, the card Local
  and no file generation, the task mounts the card, parses the file and
  checks each item: its WAV through `validate_open_wav()`, the check `PLAY`
  applies, without latching a fault and with reads that count in no play's
  SD counters (`ReadCounting::uncounted`), so a refusal leaves STATUS as it
  was; its scenario through `configure_scenario()`, the load `PLAY`
  performs, for that WAV's length and with the item's seed, which also
  yields the scenario file's CRC-32 for the block. It unmounts, then
  answers: the item count, or the first refusal with its line and detail
  (`ControlReply::line`, `detail`).
- **Items** (`run_playlist()`, `run_item()`, `play_item()`). For each item:
  wait for room in the result ring, plan its start, then set its route
  (`OutputRouteSelector::set_between_runs()`, between runs) and accept its
  output as `PLAY` does (`accept_output()`). A USB item without an open
  stream ends there (error 21) and selects nothing, so the player keeps the
  run before it as it ended. Otherwise `LOAD` it (`reset_selection()`),
  point `scenario_path` at its `scn=` or leave it empty, never at
  `<FILE>.SCN` (`select_item_scenario()`; `configure_scenario()` loads none
  for an empty path), hand its seed to `load_scenario()`, put its CUT list
  in the pending slot, hand its attenuation and duration to the producer
  (`PlayerPlaylist::attenuation`, `duration`), and begin it as `PLAY` does
  (`begin_run()`, the critical section `PLAY` uses, which latches the list).
  Then `run_player()` runs it exactly as a `PLAY`'s run, and the block
  records the play.
- **Attenuation** (`common/attenuation.hpp`). The producer quantizes each
  engine output sample to PCM24 and multiplies it by the item's gain from
  the 401-entry Q1.31 table (`attenuation_gains.hpp`, `const`, in flash),
  rounding the 55-bit product half to even (`attenuation::apply()`), in the
  send step (`send_output()`), before the output FIFO; a ZERO's zeros need
  none. A plain `PLAY` and an item without `att=` take 0 dB, gain 2^31,
  which leaves every sample as it is. `host/attenuation.py` writes the
  table from its closed form and `host/make_playlist.py` predicts with the
  same table; `wfg-playlist` checks the arithmetic (0 dB on every PCM24
  sample, every entry against plain division) and `wfg-playlist-host` the
  table and samples against the host's.
- **Duration** (`CardSource`, `card_source()`, `begin_next_pass()`). An
  item with `dur=` hands its duration to the play
  (`PlayerPlaylist::duration`); `play_input_frames()` makes it the play's
  input, the frames the producer reads (`CardSource::frames_left`), and
  `load_scenario()` configures the engine for that many frames, so the
  engine sees one input across the seams. The producer reads whole blocks
  of the card's current pass (`pass_left`); when a pass ends with input
  left, it seeks back to the data chunk's first frame and starts the next
  pass, so loop n + 1's frame 0 follows loop n's last frame. That seek is
  the only one in a play, and after a canonical 44-byte header every read
  still lands word-aligned for the SD driver's DMA. The cut stage, the
  gain and the output FIFO see one longer output; the cut offsets count
  its frames absolute across the passes. The PLAYLIST start checks a
  `scn=` item's scenario for its input length too (`check_item()`).
- **Anomalies** (`PlayWatch`, `looped_anomaly()`, `note_block_deadline()`).
  Every run's watch (`.ocram.player_watch`) restarts in `begin_run()`: its
  engine deadline misses, whether it is looped, the TinyUSB assertion
  count then, the successfully configured Watterson identity, and the two
  reasons `ENGINE?` reports: the engine's
  (`player_engine_reason()`) and the live controls' (`player_live_reason()`,
  kept by `fail_live_controls()` with the `live_control` fault). `note_block_deadline()` runs after each pull step's read and
  engine block: when the output already runs before the stream's end and the
  FIFO holds fewer than `ring_critical_frames`, it counts a deadline miss
  (`player_deadline_misses()`, `USB?` `engine_deadline_misses`) and, in a
  looped play, latches `deadline_miss`. `check_output()`, which the producer
  and the drain loops call, adds a looped play's anomalies to the USB sink's
  faults: missed intervals on the USB route that the next completion has
  confirmed (`missed_interval`; `usb_audio::confirmed_missed()`, so a late
  read that completion takes back ends nothing, even when the player task
  looks in between) and the assertion count above its start (`usb_assert`).
  Underruns, SD read and seek errors, a stream close and a stall end every
  play already. After the drain only the sink's faults are judged
  (`check_sink()`): once the stream is delivered, or STOP has ended the
  item, there is nothing left for an anomaly to end. The first fault
  latched ends the run as every fault does, so the frames sent stay an
  exact prefix; `record_play()` derives the block's `end_cause` from the
  outcome and the error (`end_cause()`) and its `loops` from the output
  frames the frames sent cover.
- **Gaps** (`GapWait`, `plan_start()`, `wait_gap()`). After a USB item that
  sent frames the gap ends at stream index `first_frame_index +
  48 * ceil(sent_frames / 48) + gap_frames` on that stream (its
  `StreamCounters::opens`). A USB item after it is planned at once and armed
  for that index (`StartPlan`, `PlayerPlaylist::start`, passed to
  `usb_audio::arm_play()`); `UacStream` feeds silence until the first chunk
  that starts at or after it, so the item's first frame lands exactly
  there, prepared or not in advance. A start the stream has passed, or one
  planned on an earlier stream, starts at the next chunk; one the tick clock
  shows more than 1 s past the gap (a long pause) is reported but not armed,
  since the stream index could have wrapped. Otherwise `wait_gap()` serves
  commands and sleeps at most 100 ms at a time (a command's notification
  wakes it at once) until the stream has sent up to the index, or, without
  a device clock (codec items, an item that sent nothing, a closed or
  reopened stream, or a stream whose frames sent have not moved for more
  than `usb_stall_timeout_ms`, `StreamProgress`), until the gap's length has
  passed on the tick clock since the item ended. Gaps, the 1 s slack and the
  sleeps are `usb_audio::TickSpan`s converted once with `pdMS_TO_TICKS`;
  the blocks' `start_tick` and `end_tick` are RTOS ticks, which `PLAYLIST?`
  documents as milliseconds, so `player.cpp` asserts `configTICK_RATE_HZ`
  is 1000.
- **Blocks and the ring** (`record_play()`, `record_result()`). A block is
  filled when the run has ended: `player_snapshot()` for the card, codec and
  engine counters, and the USB play statistics with `player_cut_statistics()`
  read in one critical section, as `USB?` reads them, so a block's values
  are those STATUS and `USB?` report for that play. It also retains the
  successfully configured Watterson identity and the engine output digest
  before live controls and CUT. The ring of 32 numbers
  its blocks from 1; `PLAYLIST ACK` frees them (`acknowledge_results()`).
  Before each item `wait_for_result_room()` pauses while the ring is full,
  serving commands, so an ACK resumes the playlist and no block is dropped.
- **STOP and policies.** `STOP` sets `stop_requested` for the run and the
  playlist's own flag, which every wait checks. `playlist::end_after()`
  decides after each block: aborted after STOP, error after an item error
  under the stop policy, else on to the gap.
- **The player held.** `owns_player` is set from the accepted start to the
  end, gaps included. The command dispatcher (`dispatch_command()`) answers
  `BUSY` to every run command and live control while it is set;
  `player_allows_media_host()` refuses `MEDIA HOST`; `publish_live()`
  reports the player busy, which refuses file generation.

The state is one `PlayerPlaylist` in OCRAM2 (`.ocram.playlist`, NOLOAD,
the item table and the ring). The player task writes it; it
writes the progress, the ring and `owns_player` in critical sections, and
the USB task reads them in its own (`player_playlist_snapshot()` for
`PLAYLIST?`, `player_allows_media_host()`). `start_player()` resets it in
place (`reset_playlist()`), never through a temporary of the whole state.
`PLAYLIST?` copies the progress, the ring's numbers and its two oldest
blocks in one critical section; it is formatted by `queue_playlist_reply()`
within `playlist_json_worst_case_bytes()` (`status_contract.hpp`; 3859 of
4096 bytes). Each result retains the configured Watterson and second-source
identities and the engine output digest from before live controls and CUT.
`ENGINE?` reports the same current or last play identities alongside engine state, error and reason;
its worst case is 984 bytes. INFO advertises `watterson` after
`engine-diagnostics` and also advertises `second-source` in its capability
list; it reports Watterson algorithm version 1 at the top level.

### Miss attribution (`USB?`)

The WFG-LIVE/1 query `USB?` says where the stream last missed a 1 ms service
interval and whether the device or the host caused it, for the stream and for
the current or last USB play. JSON `STATUS?` is within 7 bytes of the
4096-byte response buffer, so these fields live only in `USB?`, whose reply
has its own budget (`usb_json_worst_case_bytes()`, 1481 bytes). STATUS is
unchanged. `USB?` repeats the stream fields STATUS reports (`uac_stream_open`
.. `usb_stack_asserts`, same values) and adds:

| `USB?` field | Meaning |
|---|---|
| `uac_rejected_chunks` | Since boot: chunks the IN FIFO refused, or that found no free record (their frames were lost) |
| `uac_last_miss_frame_index` | Stream index of the first frame sent after the latest gap that still counts a missed interval (the packet whose completion ended it); 0 when the open stream has missed none. A late read taken back returns the location before it |
| `uac_last_miss_unprimed` | How many of that gap's missed intervals are counted unprimed, the device's (see below) |
| `uac_last_miss_play` | 1 when that gap delayed the file frames of the USB play current then, so it counts in that play's `uac_play_missed_intervals`, armed or not; 0 for an idle gap, one still going on as the play armed, and one after its last file frame |
| `uac_max_gap_frames` | The largest gap between IN completions, in USB frames as the ISR read them (1 while the host polls every interval; a late read reads 2). The 11-bit frame number wraps at 2048 frames; the RTOS ticks between the two completions recover a longer gap whole |
| `uac_watchdog_expiries` | Since boot, while streaming: the service watchdog's samples. One per millisecond of a host pause, and one whenever the USB ISR is held off by about 0.5 ms or more, even when no interval is missed: expiries while `uac_missed_intervals` stays flat mean USB ISR latency, not host pauses |
| `uac_unprimed_intervals` | Since boot: the part of `uac_missed_intervals` counted unprimed, the device's (see below); the rest is the host's |
| `uac_play_unprimed_intervals` | The part of `uac_play_missed_intervals` counted unprimed, over the same gaps; it restarts with every accepted `PLAY`, like the `uac_play_*` STATUS fields |
| `usb_assert_count` | TinyUSB assertion failures since boot: failed `TU_ASSERT` checks and TinyUSB's direct `TU_BREAKPOINT()` calls, some of them host requests it stalls by design. Read with the four fields below as one record (`usb_audio::StackAsserts`); equals `usb_stack_asserts` |
| `usb_first_assert_pc`, `usb_last_assert_pc` | Call sites of the first and the latest failure: inside the call to the assert hook at the failed check, one call per assert, for `arm-none-eabi-addr2line` against the exact image's ELF ([recipe](../live-control-protocol.md#tinyusb-assertions)); 0 when none |
| `usb_last_assert_frame_index`, `usb_last_assert_tick` | The stream's frames sent (`uac_stream_frames`) and the RTOS tick when the latest failed |
| `uac_stream_alt`, `uac_stream_bits`, `uac_stream_channels` | The open stream's [capture format](protocols-media-and-artifacts.md#capture-formats): alternate 1, 2 or 3, 24 or 16 bits, 2 or 1 channels; all 0 while closed |
| `uac_play_stream_bits`, `uac_play_stream_channels` | The format the current or last USB play's first file frame went out in; 0 until it has, and after a codec `PLAY` |
| `uac_sent_mismatches` | Since boot: IN completions whose reported size was not the packet TinyUSB loaded for them. Each still counts as sent (TinyUSB took the packet from the FIFO) and as a short packet; one while a play had file frames queued ends that run with error 26, since the host may not have received the packet. 0 unless the controller reports a transfer that ended in error (#45); 16 bits, it stays at 65535 once there. The last `USB?` member |

The location and the largest gap restart when the stream opens, like its frame
indices; the stream counts run since boot, like `uac_missed_intervals`; the
play's restarts with each accepted `PLAY`.

How a miss is attributed. TinyUSB primes the next packet in the USB ISR, just
before `tud_audio_tx_done_isr`, so the device is late only when that ISR is
held off (by a critical section or a higher-priority interrupt) past the
host's next poll. A **service watchdog** tells that apart from a host that did
not poll:

- It is the USB controller's general-purpose timer 0 (1 us steps; TinyUSB
  does not use it), run one-shot. Every IN completion restarts it in
  `tud_audio_tx_done_isr`, after TinyUSB primed the next packet: for 1.5 ms
  when that ISR ran before the deadline, and for 1.0 ms when it ran at or
  past it (the count had reached zero: the ISR itself was late, and the
  shorter deadline keeps the next sample ahead of the poll after next). Its
  interrupt (`USBSTS.TI0`) shares the USB IRQ.
- `USB_OTG1_IRQHandler` checks `TI0` before TinyUSB sees the interrupt,
  acknowledges it and reads `ENDPTSTAT` (`ETBR` bit 20, endpoint 0x84):
  - primed: the packet still waits, so the host left out a poll since the gap
    began or since the previous sample. The watchdog samples again 1 ms later,
    for as long as the gap lasts;
  - not primed: the completion that took the last packet is still waiting for
    its ISR (pending with the expiry), so the host's polls since then found
    nothing to send. That completion is handled next, in the same interrupt,
    and restarts the watchdog.
- `TIE0` is hidden from TinyUSB's handler while it runs. That handler
  acknowledges every status bit it finds set and enabled, so an expiry that
  came while it ran would be lost; hidden, it stays latched and raises the
  interrupt again once `TIE0` is restored.
- The next completion ends the gap and counts its missed intervals as before.
  Each primed sample proves one interval the host left out. When a sample
  found nothing primed, the gap's other missed intervals count in
  `uac_unprimed_intervals` too, and in `uac_play_unprimed_intervals` when the
  gap delayed the play's file. A 5 ms Windows pause that ends with the ISR
  held off 2 ms reads 7 missed: 5 proven the host's, 2 unprimed.
- A late read taken back from a gap (the next completion's zero gap) leaves
  an unprimed interval first. When the gap after a late read reads one frame
  but its primed sample proves a host skip, the late read's interval moves to
  that gap as the host's: the missed count stays, the unprimed count drops if
  the late read counted unprimed. `uac_unprimed_intervals` never exceeds
  `uac_missed_intervals`.

Costs: per completion one timer count read, one reload and one status
acknowledge; per USB interrupt one status read and two `USBINTR` writes; one
extra interrupt per millisecond of a host pause and per held-off ISR. The SOF
interrupt was rejected: TinyUSB 0.21's ChipIdea driver raises it every 125 us
microframe at high speed and runs its full interrupt handler and the class SOF
hooks (`dcd_int_handler`, `dcd_event_handler`) for each, well over a few
dozen cycles at 8 kHz, and `tud_sof_cb` only runs later in the USB task,
through the event queue, too late to see what was primed when the host
polled. Nothing in `third_party/` is changed.

Limits:

- The attribution assumes the host polls once per 1 ms frame, at a steady
  point in it: each primed sample stands for one poll left out.
- The unprimed count is an upper bound on the device's own intervals. A
  held-off ISR hides whether the host polled before the completion it holds,
  so a sample that found nothing primed leaves to the device every missed
  interval of its gap that no primed sample proves the host's. It can exceed
  the device's own by one per such gap: the poll that took the packet at the
  end of a host pause, or an ISR held off between 0.5 and 1 ms that still
  primed in time but read its frame late, until a later completion takes
  that interval back or moves it.
- The deadline runs from when the ISR ran, not from the host's poll. A
  device-late interval escapes the watchdog, and counts as the host's, when
  the completion before it was handled more than 0.5 ms after its poll yet
  before its own deadline, which takes consecutive completions held off in a
  row (the one before that held off too). A count of 0 is therefore strong
  evidence for the host, not proof.
- As for `uac_missed_intervals`, a late read is told from a miss only by the
  next completion, so at the end of a play's file the play's count and its
  unprimed part can be one high.
- If TinyUSB fails to prime after a completion (the endpoint transfer refused
  in `audiod_tx_xfer_isr`, so no `tud_audio_tx_done_isr`), the watchdog's next
  sample finds nothing primed and no completion pending, and it is not
  restarted. No completion follows, so the gap never ends: what shows is one
  more `uac_watchdog_expiries` and a frozen `uac_stream_frames`, while
  `uac_missed_intervals` and `uac_unprimed_intervals` stay flat; an armed play
  ends with error 20 (stall).
- The stream counts and the location cover idle gaps and Windows'
  reconfiguration pauses as well; a play is judged by its own fields.

Bench checks. The register behaviour (GPTIMER0 load in microseconds less one,
`TI0` when the count reaches zero, a one-shot count that stays at zero after
it, `ETBR` set from prime to completion on an isochronous IN endpoint) is from
the reference manual. Both paths need the bench:

- Primed: while Windows pauses the stream (start-up, a client joining or
  leaving), `uac_watchdog_expiries` should rise by about one per millisecond
  of the pause and `uac_unprimed_intervals` should stay put. If
  `uac_missed_intervals` rises in long gaps while `uac_watchdog_expiries`
  stays 0, the watchdog is not running.
- Unprimed (required before a flat unprimed count means anything): with the
  stream open and no play, halt the core from the debugger for 100 to 500 ms,
  then resume. `uac_missed_intervals` and `uac_unprimed_intervals` should
  each rise by about the halt in milliseconds, `uac_last_miss_unprimed` and
  `uac_max_gap_frames` read about the halt, and `uac_watchdog_expiries` rise
  by one. If `uac_unprimed_intervals` stays put, `ETBR` does not behave as
  assumed and neither unprimed count means anything. Keep the halt under 2 s:
  the frame number wraps at 2.048 s, and the RTOS tick that would tell the
  wraps stops with the core.

Lane note: assert `uac_play_missed_intervals == 0` for each play (STATUS).
When it is not 0, read `USB?` after the run and before the next `PLAY`.
`uac_play_unprimed_intervals` above 0 means the USB ISR was held off with a
completion pending in a gap that delayed the play's file: the device's (it may
include one host interval per such gap). `uac_play_unprimed_intervals == 0`
leaves every missed interval of the play to the host, within the limits
above, and only once the unprimed bench check has passed.
`uac_play_first_miss_frame_index` (STATUS) locates the play's first miss;
`uac_last_miss_*` locate the stream's latest, which may be an idle gap after
the play (`uac_last_miss_play` 0). Do not take deltas of
`uac_unprimed_intervals` for a play: it also counts idle gaps and Windows'
reconfiguration pauses.

### EP0 event order

TinyUSB's ChipIdea driver reports, in one interrupt, the transfer
completions it read from `ENDPTCOMPLETE` and then a SETUP it read from
`ENDPTSETUPSTAT`. A control transfer's last stage that completes between the
two reads, with the host's next SETUP right behind it, is reported after that
SETUP, and TinyUSB then takes it for the new request's data stage: the
`usbd.c` 1587/941 assertion pair of #32 for an IN- or OUT-data request, and a
lost ACK stage (a SET_CONTROL_LINE_STATE that never reaches the CDC driver)
for the transfer before it ([EP0 control
race](../live-control-protocol.md#ep0-control-race-32)).

The USB interrupt puts these events back in bus order without changing
TinyUSB:

- The image links with `--wrap=dcd_event_handler`
  (`rt1170/CMakeLists.txt`): the driver's calls reach the symbol
  `__wrap_dcd_event_handler`, `wfg_wrap_dcd_event_handler` in
  `usb/usb_tinyusb_port.c` (the linker's names are reserved in C, so the
  functions carry them as assembler names), which routes each event through
  the EP0 order (`usb/usb_ep0_order_glue.c`) to TinyUSB's own handler.
- `USB_OTG1_IRQHandler` runs TinyUSB's handler in passes
  (`run_tinyusb_passes`). A SETUP of a pass is held. An EP0 completion of a
  pass goes on at once, ahead of the held SETUPs, and so does a SOF, which
  the driver reports last in a pass, after its SETUP. While a SETUP is held
  and `ENDPTCOMPLETE` shows an EP0 completion that no pass has reported, the
  handler runs again (at most `max_extra_passes`, 2). Any other event, and
  the end of the interrupt, release the held SETUPs first, oldest first.
  Another event that releases them while `ENDPTCOMPLETE` still shows an EP0
  completion (the glue asks the port's `ep0_completion_pending()` only while
  a SETUP is held) counts in `usb_ep0_unordered`, as the pass bound and a
  full hold do.
- A completion pending when a SETUP is reported belongs to an earlier
  transfer: nothing can be primed for the SETUP until the `wfg_usb` task has
  seen it, and the task cannot run inside the interrupt. One extra pass is
  enough; the bound only keeps a controller that never stops completing from
  holding the interrupt.
- `Ep0Order::hold()` returns a `Status`: held, `not_in_interrupt`, or
  `hold_full`, which counts in `usb_ep0_unordered` (a `static_assert` keeps
  the hold at least one SETUP per pass of an interrupt, as the driver reads
  `ENDPTSETUPSTAT` once per pass). `another_pass()` takes `Ep0Pending` and
  returns `NextPass`.
- The order's state (at most 3 held SETUPs and two counters) lives in OCRAM2
  (`.ocram.usb_ep0_order`, NOLOAD), cleared by `wfg_usb_ep0_order_reset()`
  before TinyUSB starts; the post-link placement check covers it.
  `usb_ep0_reordered` and `usb_ep0_unordered` in `USB?` report its counters.
- Events from task context (the MSC driver's own completions) pass straight
  through.

`wfg-usb-ep0-order` checks the order's rules at compile time, the glue's
routing, and the race itself on the pinned TinyUSB (`usbd.c` and the CDC
driver built for the host below `tests/usb_ep0_race_stubs/usb_ep0_race.c`):
as the driver reports it, two assertions and DTR off lost; through the
order, none and DTR off delivered.

### State and ownership

Commands enter one fixed mailbox. The player task acknowledges state-changing
commands after it owns the mutation. `LOAD` resets selection-dependent schedules.
`PLAY` opens, validates, premeasures or accepts a reference, configures engines,
prefills, starts codec transport, and transitions through running to done, aborted,
or faulted. A completed/aborted/faulted run requires another `LOAD` before replay.
A `PLAYLIST` holds the player from its accepted start to its end: the task
runs its items as runs of their own, and the mailbox answers `BUSY` to every
other run command meanwhile; `STOP` and `PLAYLIST ACK` act.

### Failure and counters

Important groups are:

| Group | Counters |
|---|---|
| SD/file | read calls/bytes, short reads, read errors, maximum SD cycles |
| FIFO | fill, min before EOF, max, average, critical events, underruns |
| Engine | state/error, frames in/out, clipping, events, source/output digests, max cycles, arena |
| Audio | running, blocks, backlog, overruns, RX/TX errors, max block cycles, TX write races, SAI FIFO words at start |
| USB sound card | stream open/opens/frames, short packets, missed intervals, host volume/mute, stack asserts; per USB play: file frames, CRC per channel, first/last/accept stream index, first USB frame, missed intervals and the first miss's stream index; in `USB?` only: rejected chunks, the latest miss's stream index, unprimed part and play flag, the largest gap, watchdog samples, unprimed (device-late) intervals for the stream and for the play, the play's cuts (count, frames dropped and inserted, first index, entries past the end, frames sent; on either route) and its engine deadline misses (either route) |
| Live | frame, digest, clipping, event count, queue/capture space, effective reference |
| PLAYLIST (`PLAYLIST?`) | state, name, run, policy, items, index, completed, failed, pending blocks and their numbers; per item: outcome and player error (21 for a USB item without a stream; 23 to 25 for a looped item's anomalies), the per-play statistics above, scenario, seed and the scenario's CRC-32, gap, scheduled stream index and lateness, ticks, attenuation, duration, loops, end cause, engine deadline misses, Watterson and second-source identities, and engine output digest |

Player error numbers are fixed. Each has its row in `published_player_errors`
(`rt1170/player.hpp`), or is reserved in `reserved_player_errors`: an image
without the feature that raises an error keeps the number free. A
static_assert (`numbers_accounted_for()`) requires every number up to
`usb_unconfirmed`'s (26, the last error) to be one or the other, in order.
This image reserves none; signal-lab-phase-2, without PLAYLIST and looped
items, reserves 21 and 23 to 25.

Build success does not test physical word alignment, analog level, clock quality,
or cable routing. CDC success does not test DAC audio. Analog capture does not by
itself test modem decode.

### Critical errors

`Result::value()` on a Result that holds an error, or any other
`m110::critical_error()` call, is a defect: the caller should have checked
`has_value()`. `common/status.cpp` passes the Status and the call site to the
installed handler and then traps (`UDF`). Each image installs the
firmware's handler, `rt1170/critical_error.cpp`, first in the application task
(`rt1170/main.cpp`). The handler masks interrupts, records the failure and
returns. The trap then takes the core into the SDK's `HardFault_Handler`,
which branches to itself, so the core stops there and the record stays as
written. The record keeps the first critical error of the boot: a later call,
which only a context the mask does not hold off (NMI, a fault handler) could
make, leaves it as it is. Installing the handler clears it at each boot.

The record lies in OCRAM2 (`.ocram.critical_error`, 16 bytes; its address is
on the map's `.ocram.critical_error` line, which the post-link check prints
for the player images). The handler writes its cache line to memory, so a
debugger that halts the core reads the record from memory:

| Word | Field | Meaning |
|---|---|---|
| 0 | signature | `0x54495243` ("CRIT") once a critical error is recorded; 0 until then |
| 1 | code | the Status's `StatusCode` (the low byte) |
| 2 | message | the address of the Status's message, a string in flash |
| 3 | site | the call site: `arm-none-eabi-addr2line -e waveform_generator.elf -f -i <site>` names the function that called `critical_error()`, or that `value()` was inlined into |

STATUS has no room for the record, and once the core has trapped nothing can
answer `USB?` either. Host tests install their own handler
(`tests/critical_error_tests.cpp`), and the host tools theirs
(`common/host_critical_error.hpp`, [coding standard](../coding-standard.md#failure-gates)).

## 5. 5th-grader view

The SD card reader is a cook making sandwiches ahead of time. The audio hardware
is a hungry customer who takes one small sandwich on an exact schedule. A large
tray sits between them. If the cook pauses briefly, the customer still eats from
the tray. If the tray becomes empty, that is an underrun.

The cook also adds the requested sound effects before putting sandwiches on the
tray. The customer never opens files or calculates effects.

With `OUTPUT USB` the customer is the PC instead, collecting a bag of 48
samples every millisecond through the USB socket. A helper at the door keeps
two bags packed. Each bag gets a receipt, and the receipt is only filed once
the PC has really taken that bag, so the board's totals always match what the
PC received, even if the meal is stopped halfway.

## 6. Toy worked example

Assume one SD read produces 2048 samples. With no sample slip, the engine emits
2048 and the producer adds them to the FIFO. The audio task then consumes sixteen
128-frame blocks:

```text
producer push: +2048 frames
16 audio callbacks: 16 * -128 = -2048 frames
net FIFO change after both sides finish: 0
```

If a legal duplicate event adds 10 samples, the engine emits 2058. The extra ten
frames move every later output-frame position relative to source input, but live
events still use the final output-frame coordinate.

If SD pauses for 20 ms, the audio side needs 960 queued frames to bridge that
pause. The much larger normal fill is intentional safety margin, not permission
to block the audio task.

A playlist of two USB items with a 5 s gap after the first: if the first's
first frame went out at stream index 96336 and it sent 240001 frames (5000
packets and one frame), its last packet ends at 96336 + 240048 = 336384,
with 47 exact zeros after its last frame. The second item starts at
336384 + 240000 = 576384, exactly, as long as it was ready by then: the
player prepared it during the gap and armed it for that index. On the codec
route the same gap is 5000 ms on the tick clock after the first item's run
ended, and the second item then starts as soon as it is prepared.

On the USB route, suppose the stream opened 2 s before `PLAY`
(`uac_stream_frames` 96000 at the accept, so `uac_play_accept_frame_index` is
96000), and the player armed about 0.5 s later, just before the completion
that brought `uac_stream_frames` to 120000. At that completion TinyUSB had
already loaded the packet that starts at 120000 and the FIFO held the one at
120048, so its top-up wrote the first file chunk at 120096:

```text
uac_play_accept_frame_index = 96000
uac_play_first_frame_index  = 120096   (latency 24096 frames, about 502 ms)
a 48001-frame file:   1001 chunks, the last one 1 file frame + 47 zero frames
uac_play_last_frame_index   = 120096 + 48001 - 1 = 168096
uac_play_file_frames        = 48001
```

If STOP takes effect when 502 file chunks have been written (499 confirmed
sent, one loaded by TinyUSB, two in the FIFO), those three still go out and
count, and every chunk after them is silence: the statistics end at 502
chunks (24096 file frames), an exact prefix of the file, and the CRCs are
those of that prefix.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Owner loop and player state | `rt1170/player.hpp`, `rt1170/player.cpp` |
| SD second-source provider and lifecycle | `rt1170/secondary_wav.hpp` (`Rt1170SecondaryWav`); `rt1170/player.cpp` (measurement, borrowed-resource detach and play identity) |
| Error names, numbers and refusal codes: each `PlayerError` and `EngineError` as a Status and back, `engine_error`'s numbers, the WAV errors' player errors, the refusals a reply carries (`refuse()`) | `rt1170/error_map.hpp` (the static_asserts pin every row); `rt1170/player.hpp` (`published_player_errors`, `reserved_player_errors`, `numbers_accounted_for()`); tests `tests/error_map_tests.cpp` (`wfg-error-map`, against `docs/live-control-protocol.md`; its static_asserts run `numbers_accounted_for()` on signal-lab-phase-2's table) |
| CUT injector: schedule rules, cut stage, log | `common/cut_schedule.hpp`; `rt1170/player.cpp` (`PlayerCuts`, `produce`, `pull_block`, `carry_out`, `finish_output`, `player_cut_statistics`); `common/live_protocol.cpp` (`CUT` parsing); tests `tests/cut_schedule_tests.cpp` (`wfg-cut-schedule`, and its `--predict` mode for `wfg-cut-host`), `tests/player_live_host_tests.cpp` (plays with cuts, scenario plays against the host render, `AT:` through drops, image A's rows with `WFG_FSRAND_WAV`) |
| PLAYLIST: file parser, ring, timing rule; sequencer; `PLAYLIST?` | `common/playlist.hpp`; `rt1170/player.cpp` (`PlayerPlaylist`, `accept_playlist`, `run_playlist`, `run_item`, `play_item`, `plan_start`, `wait_gap`, `record_play`); `rt1170/usb_audio.*` (`ScheduledStart`, `arm`); `common/live_protocol.cpp` (`PLAYLIST` parsing); `rt1170/usb_service.cpp` (`queue_playlist_reply`), `rt1170/status_contract.hpp` (`playlist_json_worst_case_bytes`); tests `tests/playlist_tests.cpp` (`wfg-playlist`, and its `--check` mode for `wfg-playlist-host`), `tests/player_live_host_tests.cpp` |
| Board/runtime/task startup | `rt1170/main.cpp`, `rt1170/platform/board.*`, `runtime.*`, `rt1170/os/task.*` |
| SD and MSC block layer | `rt1170/platform/waveform_sd.*`, `waveform_msc.*` |
| Codec transport | `rt1170/platform/wm8960_codec.*` |
| USB sound-card stream and play statistics | `rt1170/usb_audio.*`, `rt1170/usb/usb_audio_tinyusb.c` |
| USB service watchdog (miss attribution) | `rt1170/usb/usb_tinyusb_port.{h,c}` (GPTIMER0, `USB_OTG1_IRQHandler`), `rt1170/usb_audio.*` (`on_watchdog`, `unprimed_part`, `dealiased_gap`) |
| USB bring-up (hardware, then TinyUSB) | `rt1170/usb/usb_tinyusb_port.{h,c}` (`m110_usb_hardware_initialize`, `m110_tinyusb_stack_initialize`, `g_m110_usb_port_status`), `rt1170/usb_service.cpp` (`start_usb_service`, `usb_task_entry`); tests `tests/usb_port_tests.cpp` (`wfg-usb-port`), and `tests/player_live_host_tests.cpp` for the service's side: `start_usb_service`'s outcomes (`wfg-player-live-owner`) and the task after TinyUSB's start fails or succeeds (`wfg-usb-task-stack-fails`, `wfg-usb-task-stack-starts`) |
| EP0 event order (#32) | `rt1170/usb_ep0_order.{hpp,cpp}`, `rt1170/usb/usb_ep0_order.h`, `rt1170/usb/usb_ep0_order_glue.{h,c}`, `rt1170/usb/usb_tinyusb_port.c` (`wfg_wrap_dcd_event_handler`, `run_tinyusb_passes`), `rt1170/CMakeLists.txt` (`--wrap`); tests `tests/usb_ep0_order_tests.cpp` (`wfg-usb-ep0-order`), `tests/test_usb_storm.py` (`wfg-usb-storm-host`) |
| Critical-section guards (task and ISR) | `rt1170/os/critical_section.hpp` |
| Critical errors: the handler and its record | `common/status.{hpp,cpp}` (`critical_error`, `CriticalSite`), `rt1170/critical_error.{hpp,cpp}`, `rt1170/main.cpp` (`install_critical_error_handler`); tests `tests/critical_error_tests.cpp` (`wfg-critical-error`, and one run per `value()` form) |
| Linker placement and its post-link check | `cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld`, `scripts/check-memory-placement.py` (`wfg-memory-placement`: `tests/test_memory_placement.py`) |
| The player and USB tasks' stacks and their post-link check | `rt1170/CMakeLists.txt` (`-fcallgraph-info=su`), `scripts/check-stack-usage.py`, `scripts/stack-usage-rules.json` (`wfg-stack-usage`: `tests/test_check_stack_usage.py`) |
| Status and `USB?` responses | `rt1170/status_contract.hpp`, `rt1170/usb_service.cpp` |
| USB route tests | `tests/usb_audio_tests.cpp` (`wfg-usb-audio`: stream, records, statistics, missed intervals, attribution, controls), `tests/player_live_host_tests.cpp` (`wfg-player-live-owner`: PLAY, routes, errors 19/20, capture formats and their `USB?` fields, STATUS order and budgets; `-mono`, built with `WFG_UAC_MONO_ALT`: a 16-bit mono play) |

Any timing-path change needs host owner-loop tests, target Release builds, memory
maps, worst-cycle counters, bounded physical playback, and explicit evidence of
which layer was actually exercised. A USB route change also needs a bit-exact
capture through the [bring-up runbook](../usb-sound-card-bringup.md): the host
tests replace TinyUSB with a software FIFO and simulate the USB ISR.

## 8. Publication and maintenance walkthrough

The producer owns SD reads, static-engine state and live control mutation.
`write_ring` writes payload words (an engine block's quantized samples, or a
ZERO's zeros) into free FIFO slots before publishing the write index
(`publish_written`); `push_block` repeats it, waiting while the FIFO is full,
until the whole block is in or the run ends. `player_audio_hook` takes an index snapshot, copies available words,
then publishes the consumed index. The sample array itself needs no per-sample
atomic operation: its ownership passes through the indices. Only the stopped
owner may reset the counters/indices for a new run.

Cross-task flags and indices use sequentially consistent atomics, with compile-time
lock-free assertions for the target types. Existing platform barriers remain at
DMA/publication boundaries. Compound telemetry, including 64-bit totals, is copied
under short FreeRTOS critical sections. Telemetry consistency is not a license to
hold a critical section across an SD operation or client audio hook.

The codec ISR publishes completed DMA work. The audio task drains it and invokes
the hook; player and codec status expose errors and worst block cycles. The EOF
path supplies bounded silence so queued samples drain before stopping transport.
A callback underrun latches a fault instead of silently pretending the file ran.

Both consumers run one body, `consume_output`, a template on the caller's
critical section: `TaskCriticalSection` for the codec hook (audio task) and
`IsrCriticalSection` (`taskENTER_CRITICAL_FROM_ISR`) for
`player_consume_from_isr` (USB ISR). Read `consume_output`, `write_ring`,
the producer (`produce` and its reader steps), the owner command/run loop and
`player_status` together. Run `wfg-player-live-owner` (and its read-only build
`wfg-player-live-owner-readonly`, mono build `wfg-player-live-owner-mono` and 24 MHz SD clock build
`wfg-player-live-owner-sd24`) and both writable/read-only
firmware builds after changing this contract. Host stubs do not model real
interrupt preemption, cache visibility or DMA clocks; physical timing and audio
checks remain necessary for hardware acceptance.
