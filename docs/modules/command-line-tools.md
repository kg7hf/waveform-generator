# Command-line Tools Reference

## 1. Scope

This is the operator-facing reference for the project-owned Python, PowerShell,
and host executable entry points. It records the current options and safe command
sequences. Run each Python tool with `--help` for the parser's live wording.

## 2. 50,000-foot view

The tools form four workflows:

| Workflow | Tools |
|---|---|
| Make clean audio | `stage_wav.py make-tone`, optional plugin tools |
| Add impairments | `render_scenario.py`, `signal_lab_render.exe` |
| Operate RT1170 or RT1064 | `usb_bench.py`, `media-control.ps1`, `live_control.py`, `media_eject.py` (card staging without Explorer's Eject), `tx_upload.py`, `stage_wav.py`, `make_playlist.py`, `cut_schedule.py`; `usb_storm.py` currently checks only RT1170 |
| Capture and inspect audio (analog input, or the board's USB sound card) | `waveform_capture.exe`, `capture-session.ps1`, `analyze_capture.py` |

## 3. How to use it

### `host/usb_bench.py`

Native Windows launcher for either RT1170 or RT1064. Use `devices` for discovery;
`check`, `control`, `stage` and `capture` require a machine-local `--profile`.
`--runs-root` selects the parent of fresh per-operation logs. The profile holds
the expected board, USB serial and an explicitly associated capture name.
Its complete operator syntax is:

```text
[--profile PROFILE.json] [--runs-root DIR] devices|check
[--profile PROFILE.json] [--runs-root DIR] control [--host-volume-dismounted] "COMMAND"...
[--profile PROFILE.json] [--runs-root DIR] stage WAV --name NEWNAME.WAV
[--profile PROFILE.json] [--runs-root DIR] capture [--seconds 1..3600] [--channel left|right|average]
```

`devices` needs no profile. The other actions do; `stage` expects an already
host-owned card, verifies and ejects the new file, then returns ownership to
the firmware. `capture` records the profile's exact Windows input endpoint.
See [Windows USB bench setup](../windows-usb-bench.md) for profiles, commands,
media handoff and the distinction between capture completion and acceptance.

### `host/render_scenario.py`

```text
--scenario PATH   Required scenario JSON
--output PATH     Required output WAV; sidecar uses the same stem
--base-dir DIR    Resolve relative source paths here; default is scenario directory
--overwrite       Permit replacing output artifacts
```

### `signal_lab_render.exe`

```text
--scenario PATH          Static scenario; optional only with --live-events
--source PATH            Required mono PCM24 48 kHz source WAV; PCM16 import is legacy-only
--output PATH            Required output WAV
--sidecar PATH           Optional explicit result sidecar
--target-scenario PATH   Write scenario with effective reference_rms for either board
--reference-rms VALUE    Finite positive external reference override
--block N                Processing block, 1 through 2048
--live-events PATH       Apply a wfg-live.replay/1 plan
--seed N                 Replace the scenario's seed (canonical decimal uint64), as a PLAYLIST item's seed= does
```

`--seed` renders a [PLAYLIST](../live-control-protocol.md#playlist) item with
`scn=` and `seed=` from its unchanged scenario file, so the file's bytes (the
block's `scn_crc32`) stay those the board checked. It needs `--scenario` and
refuses `--target-scenario`, whose document would keep its own seed.

### `host/compose_stereo_wav.py`

For independent left/right channel artifacts, use
[`host/compose_stereo_wav.py`](stereo-artifacts.md). Its `compose` command
checks two C++ renderer WAV/sidecar pairs and writes stereo PCM24 with channel
hashes and CRCs. Its `verify` command compares aligned stereo files exactly,
including the PCM16 top-bit capture case. Both commands are offline.

```text
compose --left-wav L.WAV --left-sidecar L.json --left-scenario L.json
        --right-wav R.WAV --right-sidecar R.json --right-scenario R.json
        --output-wav STEREO.WAV --output-sidecar STEREO.json
verify --expected-wav STEREO.WAV --capture-wav CAPTURE.WAV
```

### `host/summarize_prepare_play.py`

```text
positional JOURNAL                Saved wfg-live.session/1 JSONL
--expected-serial SERIAL           Required fixture identity
--firmware-commit COMMIT            Required 40-digit lowercase hexadecimal commit
--operation-id ID                  Required nonzero boot-local uint64 identifier
```

This offline reader reports the last cumulative PREPAREPLAY timing snapshot
and hashes the original journal. Exit 0 means a successful terminal operation
with complete timing, 1 means a valid partial or unsuccessful result, and 2
means invalid or unbound input. It opens no board connection. See
[saved timing reports](finite-playback.md#saved-timing-report) for identity,
counter and performance-interpretation limits.

### `host/stage_wav.py`

```text
stage_wav.py inspect WAV
stage_wav.py make-tone --output PATH [--recipe PATH] [--force]
stage_wav.py stage WAV --card-root ROOT [--level-percent N] [--force]
```

`stage` validates the fixed mono packed-PCM24 48 kHz contract and writes the fixed
`/WG/PLAY.WAV` bring-up path plus its retained metadata.

### `host/generate_fixture.py` and `host/prepare_demo_wav.py`

```text
generate_fixture.py RECIPE.json OUTPUT.wav
prepare_demo_wav.py SOURCE.wav OUTPUT.wav [--seconds N]
```

The first turns a local fixture recipe into a PCM24 WAV. The second is the
demonstration converter used by `demo-download.ps1`: it averages stereo,
interpolates to 48 kHz, adjusts duration, leaves headroom and records the
conversion. It is not a calibrated resampler. Both operate on local files.

### `host/retain_waveform.py` and prepared-artifact readers

```text
retain_waveform.py prepare --renderer EXE --source WAV --scenario JSON
                           --output-dir DIR [--seed N]
                           [--retention temporary|keep|archive] [--reuse]
retain_waveform.py verify DIR
prepared_artifacts.py list DIR
prepared_artifacts.py verify DIR NAME
prepared_artifacts.py archive DIR NAME NEW_DIR
prepared_artifacts.py reuse DIR NAME --firmware-commit COMMIT --seed N
                            --source WAV --scenario SCN [--secondary WAV]
compare_prepared.py BOARD_DIR NAME REFERENCE_DIR --firmware-commit COMMIT
```

`retain_waveform.py` renders and verifies a local reference. The
`prepared_artifacts.py` commands inspect or copy already retrieved board
WAV/JSN pairs; they do not reach into a board. `compare_prepared.py` compares
one verified pair with a verified local reference. See
[prepared artifacts](../prepared-artifacts.md) for identity and failure rules.

### `host/live_control.py`

Global hardware options:

```text
--port COMN
--expected-serial SERIAL
--expected-board MIMXRT1170-EVK|MIMXRT1064-EVK  (default RT1170)
--record NEW.jsonl
--timeout SECONDS
--host-volume-dismounted
```

Subcommands:

```text
command "COMMAND"...    Send one or more validated commands
files                   List every regular file in 2:/WG/ as JSON, following FILES? pages
prepare --source SRC.WAV --scenario CASE.SCN --output NEW.WAV
        [--retention keep|temporary] [--seed N] [--wait-timeout S]
                        Render a new impaired WAV on a writable card; optionally wait for final status
prepare-status [--wait-timeout S] [--interval S]
                        Query PREPARE?, or wait for a terminal phase
prepare-play --source SRC.WAV --scenario CASE.SCN --output NEW.WAV
        --rate N --margin-frames N [--retention keep|temporary] [--seed N]
                        Experimental finite render/play admission; rate is caller-supplied
prepare-play-status [--wait-timeout S] [--interval S]
                        Query experimental finite playback, or wait for inactive cleanup with a checked outcome
cut set OFFSET:N:drop|zero,...
                        Set the CUT schedule the next PLAY takes (CUT:...)
cut clear               No cuts for the next PLAY (CUT)
playlist start NAME [--continue]
                        Start the card's playlist 2:/WG/NAME (PLAYLIST:NAME[:CONTINUE])
playlist status         The progress and the oldest result blocks (PLAYLIST?)
playlist ack SEQ        Free the result blocks up to SEQ (PLAYLIST ACK:SEQ)
playlist drain --blocks FILE.jsonl [--until-done] [--interval S] [--max-polls N] [--reconnect-timeout S]
                        Save every result block as a JSON line, acknowledging each once saved, on one session
interactive             Read commands until EOF or QUIT
export JOURNAL OUTPUT   Create wfg-live.replay/1 offline
replay PLAN             Replay an exported plan with a fresh run ID
```

Important commands include `INFO?`, `STATUS?`, `COUNTERS?`, `USB?` (USB sound-card
diagnostics and miss attribution), `MEDIA HOST`,
`MEDIA LOCAL`, `OUTPUT CODEC`, `OUTPUT USB` (where `PLAY` sends the file),
`LOAD:NAME.WAV`, `CUT:OFFSET:N:drop|zero,...` and `CUT` (the next play's
[cut schedule](../live-control-protocol.md#cut-injector)), `PLAYLIST:NAME`,
`PLAYLIST?` and `PLAYLIST ACK:SEQ` ([PLAYLIST](../live-control-protocol.md#playlist)), `PLAY`, `STOP`,
`SEED:N`, `REFERENCE:R`, four-slot
CW controls, static controls, immediate fades, and stepped sweeps. See
[Live Control and Replay](live-control-and-replay.md) for semantics and the
[protocol reference](../live-control-protocol.md) for every reply and field.

The tool first sends `INFO?` and refuses a board whose `kind`, `board`,
`sample_rate_hz` or `usb_serial` differ; after the last command it sends a
final `STATUS?` for the journal (it is not printed; add `STATUS?` to the
commands to see it). `command` checks every listed command before it opens
the port and stops at the first one the board rejects (exit code 2).

`files` additionally requires INFO's `files` capability. It follows each
`next_offset` until JSON `null`, checks exact page fields, uint64 sizes,
offsets, stable totals and duplicate names, then prints one JSON object with
`total` and `entries`. Keep the card Local and the player idle for the whole
listing. The helper neither changes media ownership nor retries failed pages.
It uses stdlib and pyserial; its pagination tests use fake transports and open
no ports. See the [wire contract](../live-control-protocol.md#card-listing).

`prepare-play-status` requires `prepare-play-experimental`. With
`--wait-timeout`, it records progress until the current or last operation is
inactive, returning 0 only for a drained and finalized successful run with
released ownership. An idle, cancelled or failed outcome returns 4; timeout
and protocol/transport errors return 2. It never sends STOP or starts a job.
See [bounded host wait](finite-playback.md#bounded-host-wait) for attribution,
cleanup and physical-delivery limits. This experimental command is separate
from the merged-main PREPARE/PLAY tester workflow.

`prepare` requires the writable image's `prepare` capability. Its output
name must be new. The ACK admits an asynchronous render; `--wait-timeout`
polls `PREPARE?` to a terminal phase. `prepare-status` is read-only and can
resume observation in a fresh session. `keep` and `temporary` are labels;
neither automatically deletes the result. The host must not infer a complete
published artifact merely because an output WAV appears on the card. The
experimental `prepare-play` additionally requires
`prepare-play-experimental`; `--rate` and `--margin-frames` are explicit
caller-supplied planning inputs, not measurements by that invocation. See
[board-side preparation](../live-control-protocol.md#board-side-preparation)
and [finite playback](finite-playback.md) for state and ownership rules.

`interactive` prints a rejection by the board and goes on, but a line the
tool itself refuses ends the session with exit code 2 and without the final
`STATUS?`: bad syntax, a `PLAY:` run ID that is not 32 lowercase hex digits
(`[guid]::NewGuid().ToString('N')`, not the dashed default), a file name
that is not uppercase 8.3 with letters, digits or underscore in the stem.
`MEDIA LOCAL` needs `--host-volume-dismounted`; without it the tool refuses
the line in the same way. Each invocation needs a new `--record` file. An
export of a journal that used `OUTPUT` (or
whose INFO named a route) replays on that route. On firmware with
`cut-injection` the plan always ends its source commands with the `CUT`
its `PLAY` took: the journal's last accepted `CUT`, or `CUT` alone when it
sent none, so a replay never inherits a list pending on the board.

`cut set` and `cut clear` send one `CUT` command on a connection, like
`command`. The tool checks a schedule as the board does before it opens the
port: at most 16 entries, N 1 to 4800, sorted and non-overlapping (a ZERO
then a DROP at one offset is an erasure), its DROPs within the drop balance
of 14400 frames, and refuses it with the board's code (`CUT_UNSORTED`,
`CUT_OVERLAP`, `CUT_RANGE`, `CUT_DROP_RUN`, or `BAD_REQUEST` for a bad
spelling). The request limit is 384 bytes, enough for 16 entries with 32-bit
offsets:

```powershell
python host/live_control.py --port COM42 --expected-serial SERIAL --record cut.jsonl `
  cut set 48000:480:zero,48000:480:drop,144000:480:zero,144000:480:drop
```

The list stays pending on the board until a `PLAY` takes it, but each
invocation writes its own journal, and `export` sees only its journal's
commands. For a run that is to be exported or replayed, send the `CUT`
on the `PLAY`'s connection instead:

```powershell
python host/live_control.py --port COM42 --expected-serial SERIAL --record play.jsonl `
  command "CUT:48000:480:zero,48000:480:drop" "OUTPUT USB" "LOAD:CARD.WAV" "PLAY"
```

`playlist start`, `status` and `ack` send one PLAYLIST command on a
connection, like `command`; the tool checks the name (an upper-case 8.3 name)
before it opens the port. The board checks every item against the card
before it answers `playlist start`, so that one reply may take a few seconds
on a long playlist: the tool waits up to 30 s for it (or `--timeout`, if
longer). A refusal prints the board's code with the line it names, for
example `PLAYLIST_WAV_INVALID line 4: unsupported_format` (exit code 2).

`playlist drain` collects the result blocks while the PC records: it polls
`PLAYLIST?`, appends each block it has not saved yet to the `--blocks` file
as a JSON line (`{"schema":"wfg-live.playlist-block/1","playlist":NAME,"block":{...}}`,
NAME the playlist `PLAYLIST?` reports when the block's `run` is its run, and
null for a block an earlier playlist left unacknowledged in the ring: its own
`run` says which playlist it was),
flushes it, then sends `PLAYLIST ACK` up to the newest block it saved, and
polls again at once while blocks remain. With `--until-done` it keeps
polling every `--interval` seconds (default 1) while the playlist runs,
pauses or waits a gap, and returns once it has ended and every block is
saved; without it, the first poll with nothing to save ends it. It prints
how many blocks it wrote and acknowledged, the playlist's final state, and
for each block it saved its `seq`, `item`, `label`, `result`, `end_cause`,
`player_error`, `uac_play_sent_frames`, `loops`, `att_tenths_db`,
`dur_frames` and `engine_deadline_misses` (those the block has: an image
before `att=` and `dur=` reports no `end_cause`), so a looped item that an
anomaly ended shows its cause at once. A block whose `end_cause`,
`att_tenths_db`, `dur_frames`, `loops` or `engine_deadline_misses` is not
spelt as the board spells it is refused as a protocol error.
A block is always saved before it is acknowledged, so a drain cut short
loses none: run it again with the same `--blocks` file (and, as every call,
a new `--record`), and a block it saved but did not acknowledge is not saved
twice. A full ring pauses the playlist on the board until a drain frees it.
When the next item is preparing, an ACK can receive an explicit `BUSY`
refusal. The drain retries that definite refusal at most eight times, waiting
the configured poll interval between attempts. Persistent BUSY or another
refusal ends the command with the saved blocks intact. A missing or uncertain
reply follows the reconnect and ring-reconciliation path below instead of
this immediate retry path.

The drain keeps one CDC session open for its whole run, however long: it
never opens the port again to poll. Opening a session costs the board a burst
of EP0 control requests (the port open, its line coding and DTR), and storms
of those were what tripped TinyUSB's EP0 assertions (#32); polling on an open
session sends only bulk data. Blocks are fetched back to back while the board
holds any; a poll with nothing to save is followed by `--interval` seconds,
so a 5 h row with the default 1 s makes about 18 000 polls on one session.

If the link is lost (the port vanishes or fails, or a reply does not come),
the drain closes the port and opens a new session, at most one attempt per
`--interval`, for up to `--reconnect-timeout` seconds (default 60; 0 ends the
drain at once). The new session numbers its requests on, so the journal's
sequences keep increasing, and checks the board's identity again. Its first
`PLAYLIST?` reply decides what happened, by the RTOS `tick` that reply
carries (a board reset restarts it; otherwise it has run on by the time the
PC saw pass, within 2 s and 1 %), by the run and block numbers (they only
grow between resets) and by the state (only a reset makes it `idle` again):

- **No reset** (USB re-enumerated, a cable or hub glitch): the drain goes on.
  A block it saved but whose `PLAYLIST ACK` was lost is shown again and not
  saved twice; nothing is lost.
- **A board reset**: the board's ring and playlist are gone. The drain ends
  with `ended` `board_reset` and exit code 3. Its `links` entry names the
  rules that saw it, the last reply before and the first after, `lost_seqs`
  (the first and last number of the blocks the board had made by that last
  reply but the drain never saw, or null) and `playlist_running` (whether
  the playlist was still running). `lost_seqs` is a lower bound: blocks the
  board made after the drain's last reply were lost too and no reply named
  them, so with `playlist_running` true re-run every item after the last
  one `lost_seqs` names (or after the newest saved block), not only those it
  names.
- **No session within the timeout**: `ended` `link_lost`, exit code 3.

A malformed reply is never treated as a lost link: it ends the drain with a
protocol error (exit code 2), as before. The journal records each `link`
event (`lost`, `restored` with `board_reset` and `rules`). The printed
result also gives `ended` (`done`, `aborted`, `error` or `idle` as the
playlist ended, `max_polls`, `empty_poll` without `--until-done`,
`board_reset` or `link_lost`), `polls`, `sessions` and `links`. An image
from before the `tick` member is judged by the run, block numbers and
state alone.
`export` refuses a journal with an accepted `PLAYLIST`: replay its playlist
file instead.

```powershell
python host/live_control.py --port COM42 --expected-serial SERIAL --record pl-start.jsonl playlist start BATTERY.PL
python host/live_control.py --port COM42 --expected-serial SERIAL --record pl-drain-01.jsonl `
  playlist drain --blocks battery-blocks.jsonl --until-done
```

### `host/make_playlist.py`

Writes and checks PLAYLIST files by the board's own rules, predicts each
item's result block from its card, and cuts a battery's capture into one
piece per item, so the capture can be checked item by item. Standard library
only:

```text
write --items ITEMS.json --output NAME.PL [--comment TEXT] [--force]
check NAME.PL [--cards DIR] [--expect EXPECT.json] [--expected-wavs DIR] [--render signal_lab_render] [--no-usb]
predict NAME.PL --cards DIR --item N [--sent-frames S] [--first-frame-index I] [--output-wav W.WAV] [--render signal_lab_render] [--no-usb]
split --capture CAP.f32.wav --blocks BLOCKS.jsonl --expect EXPECT.json --cards DIR
      --usb-after END.jsonl [--usb-before BEGIN.jsonl] [--run N] --output DIR
```

- `write` reads a JSON array of items, each an object with `file`, `gap`
  (seconds: a number, or its spelling such as `"5"` or `"0.25"`), `route`
  (`USB` or `CODEC`) and optionally `label`, `scn`, `seed`, `cut` (the
  entries as CUT spells them after `CUT:`), `att` (dB: a number such as
  `-20` or `-6.5`, or its spelling) and `dur` (seconds, as `gap`), checks
  it as the board will, and
  writes the file (ASCII, LF endings, an optional comment line first). The
  output's name must be an 8.3 name; an existing file is replaced only with
  `--force`.
- `check` parses a playlist file with the board's rules and prints its
  items as JSON, or the board's refusal code and line (exit code 2). With
  `--cards` (the folder staged as `\WG\`) it also runs the board's card
  checks (each WAV mono 48 kHz packed PCM24, each `scn=` present) and
  prints each item's prediction:

  - `uac_play_sent_frames`, `uac_play_file_frames` and both CRCs (zlib
    CRC-32 over each channel's 3-byte little-endian samples of the frames
    sent, through `host/cut_schedule.py` for items with `cut=`, which act
    on the engine's output: the card for an item without `scn=`; a codec
    item's are 0 as its block's are), the cut counters and
    `cut_first_position`;
  - `att_tenths_db`, the item's `att=`. The frames sent are attenuated
    exactly as the board attenuates them: each sample times the board's
    Q1.31 gain (`host/attenuation.py`, the table the firmware's
    `common/attenuation_gains.hpp` is written from), rounded half to even,
    so the CRCs of an attenuated item are predicted too;
  - `dur_frames` and `input_frames` (the frames the item feeds the engine:
    its card's, or its duration's), and for an item with `dur=` its looped
    stream: the card again from its frame 0 after its last, its cuts at
    that stream's absolute frames, then its attenuation, so its CRCs and
    frames sent are predicted as a card's are; `loops` (whole passes of the
    card the frames sent cover), `end_cause` `end` and
    `engine_deadline_misses` 0 for the whole play; `seam_count` and
    `seam_positions`, where each seam's frame 0 lands in the stream sent
    (null for a seam a DROP removed; at most 10000 listed,
    `seam_positions_complete` says whether all are); and `stream_crc32`,
    the CRC of the frames sent on either route (a codec item's block has
    none);
  - `scn_crc32` (zlib CRC-32 of the `scn=` file in `--cards`, 0 without
    one) and `engine_state` (1 with `scn=`, 0 without), as the block reports
    them. An item without `scn=` plays its card clean, whatever `<FILE>.SCN`
    the folder holds;
  - where each USB item starts: `scheduled` (the block's `scheduled_index`
    is set), `start_late` (the block's, null when it cannot be said),
    `run_start_item` and `start_offset_frames` (the item's
    `uac_play_first_frame_index` less that of `run_start_item`). A USB item
    after a USB item that sends frames is scheduled `48 * ceil(sent / 48) +
    gap` frames after it, and the predictions take a gap of at least 1 s
    (`on_time_gap_ms`) to be enough to prepare it. An item after a gap of 0
    always starts late (`start_late` 1), and one after a shorter gap than
    1 s may (null); such an item, one that starts when it is ready (the
    first, one after a codec item or after a USB item that sends nothing)
    and one after an item of unknown length each begin a run, offset 0.

  An item with `scn=` gets no CRC (`samples_predicted` false), and no length
  either when its scenario has a `sample_slip` stage (nor cut counters: the
  end of its engine output decides whether an entry near it applies, is
  clipped or is rejected), unless `--render` names the host renderer: an item with `scn=` is then rendered from its
  card with its `seed=` (`signal_lab_render --seed`, the board's engine,
  which renders bit-exact what the board plays), its `cut=` and `att=`
  applied to the render (as the board cuts and attenuates the engine's
  output), and predicted as a clean card is. A looped item with `scn=` is
  rendered from its looped card, written to a scratch WAV of `dur_frames`
  frames first, so the render sees one input across the seams as the
  board's engine does; for a long row that WAV and the render take room
  (a 60 min row: about 518 MB each, and signal_lab_render holds its input
  and output in memory). A rendered item's prediction also has
  `engine_status`, the STATUS engine fields the board reports right after
  it (frames in and out, clipped samples, output and source digests as
  `_hi`/`_lo` halves, events applied and dropped), from the renderer's
  summary line; null for any other item. A WAV's sizes are 32 bits, so it
  holds at most 1431655752 frames (about 8.28 h): a longer clean item is
  predicted without its expected WAV, and a longer `scn=` item is refused
  with `--render` (the renderer reads and writes WAVs); check it without.
  `--expect` writes the predictions to a new JSON file
  (`wfg-playlist.expectations/2`), `--expected-wavs` each predicted item's
  expected stream as a WAV (`<index>-<label>.WAV`), the reference its slice
  of the capture must equal. `--no-usb` checks for an image without the
  sound card.
- `predict` gives one item's prediction (`--item`, its index in the file
  from 0, the block's `item`): for its whole stream, or with
  `--sent-frames` (the block's `uac_play_sent_frames` of an item that ended
  part way: STOP or an anomaly) for that prefix of it, since the frames an
  item sends are always an exact prefix of its stream. The CRCs, the cut
  counters, `uac_play_file_frames`, `loops` and the seams (`seam_count`,
  `seam_positions`: those whose frame 0 was sent, or whose DROP took
  effect) are then the prefix's, and `end_cause` is null (a prefix ends at
  whatever ended it). With
  `--first-frame-index` (the block's `uac_play_first_frame_index`) it adds
  `uac_play_last_frame_index` and `uac_play_cut_first_index`; `--output-wav`
  writes the frames sent as a new WAV.
- `split` cuts the recorder's capture of a whole battery into one piece per
  USB item that sent frames, by the blocks `live_control.py playlist drain`
  saved (`--run`, or the newest run of the expectations' playlist). The
  capture's first sound is the first such item's first non-zero frame (its
  stream must be predicted: a clean card, or a rendered scenario item), which
  places every stream index in the capture, so each block's
  `uac_play_first_frame_index` names its item's place. Each piece is a WAV
  in the capture's own format (`.f32.wav` for a `.f32.wav` capture) holding
  the item with the silence around it: all the lead-in before the first,
  half of each gap between, all the last gap after the last, at most 2 s a
  side. Beside it `<piece>.status.json` holds the block, `output_route` 1,
  `live_events` 0 (a playlist refuses the live controls) and the stream's
  `uac_stream_opens`, `usb_stack_asserts` and `uac_short_packets` over the
  battery: those of `USB?` in `--usb-after`, less those in `--usb-before`
  when given (both `live_control.py` journals). A stream opened again
  between the two is refused: its indices restarted. `split.json` lists
  every item: its piece, whether the piece's onset is where the block says
  (`onset_ok`) and silent around the item (`silent_around`), and the
  `verify_capture.py` arguments for a clean item (`--card`, `--capture`,
  `--status`, and `--cuts` with `--usb` for one with `cut=`). A scenario,
  attenuated or looped item has none: verify_capture 0.2.0 checks clean
  cards played once at full level (`engine_state` 0). Its entry has
  `compare` arguments instead when its expected WAV exists (`check
  --expected-wavs`, with `--render` for `scn=`): the piece, its lead-in as
  `--capture-start`, `--sent-frames` for an item that ended part way and
  `--stream-bits 16` for a 16-bit stream. The exit code is 1 when a
  piece's check failed, 2 for a refusal; every output is a new file.
- `compare --expected EXPECTED.WAV --capture PIECE [--sent-frames S]
  [--stream-bits 16|24] [--capture-start F]` checks a capture, or a piece
  `split` cut from one, against an expected stream (a mono PCM24 WAV from
  `--expected-wavs`, `predict --output-wav` or `cut_schedule.py predict
  --output-wav`). It reads the capture's own format: PCM of 16, 24 or 32
  bits (24 in 32, as the board's stream), 32-bit float, or the extensible
  form of either. It places the stream at the capture's first non-zero
  frame less the expected stream's (or at `--capture-start`), compares the
  first `--sent-frames` frames (all by default) on every channel exactly:
  a float sample times 2^23, a 16-bit sample shifted up 8 bits, and with
  `--stream-bits 16` each expected sample with its low 8 bits cleared (the
  16-bit stream is the 24-bit one >> 8). The capture must be exact silence
  after them. It prints the first mismatch (frame, channel, expected, got)
  and exits 0 when all match, 1 when not, 2 for a refusal (an unreadable
  format, or a silent expected stream without `--capture-start`).

`wfg-playlist-host` checks the parser against the firmware's own
(`wfg_playlist_tests --check`) file by file, refusals and line numbers
included, the start offsets against the firmware's
`next_start_index()` (`wfg_playlist_tests --next-start`), and the
attenuation table and arithmetic against the firmware's entry by entry and
sample by sample (`--gain-table`, `--attenuate`).

### `host/attenuation.py`

The attenuation of a PLAYLIST item (`att=`) as the board computes it: the
401 Q1.31 gains round_half_even(2^31 * 10^(-n/200)) for n tenths of a dB,
evaluated from that closed form in decimal arithmetic at 60 digits, and
`apply()`, a PCM24 sample times a gain rounded half to even. It wrote the
firmware's table once, and checks it:

```text
python host/attenuation.py write common/attenuation_gains.hpp
python host/attenuation.py check common/attenuation_gains.hpp
```

`check` exits 1 when the header is not the table the closed form gives.
`host/make_playlist.py` imports it; standard library only.

### `host/cut_schedule.py`

Makes and checks CUT schedules on the host and predicts what the board
reports for them; the board never draws random numbers. The board applies a
schedule to the scenario engine's output, after the channel: the card itself
for a clean play, so offsets count card frames there. Standard library
only:

```text
generate --seed S (--card CARD.WAV | --file-frames F) --count N
         [--min-length 1] [--max-length 4800] [--mix drop|zero|mixed | --zero-fraction P]
         [--min-spacing FRAMES] [--start FRAME] [--json]
render OFFSET:N:drop|zero,...
predict --card CARD.WAV --cut "CUT:..." [--sent-frames N] [--first-frame-index I] [--output-wav EXPECTED.WAV]
        [--scenario X.SCN --render signal_lab_render [--seed N] [--render-wav RENDER.WAV]]
```

- `generate` draws a schedule from `random.Random(seed)`: `count` entries,
  each a ZERO with probability `--zero-fraction` (`--mix` 0, 1 or 0.5),
  lengths uniform between the bounds, all inside the card with at least
  `--min-spacing` card frames between one entry's end and the next offset
  (and a ZERO after a ZERO at least one frame later, as the board refuses
  two ZEROs at one offset). Every DROP lies wholly inside the card. It prints the `CUT` command (with
  `--json`, the seed, the card's frame count, the entries and the command).
  The same arguments give the same schedule.
- `render` checks entries by the board's rules and prints their `CUT`
  command.
- `predict` applies the schedule to the engine's output exactly as the
  board's cut stage does (entries past the end ignored, and counted once the
  whole stream is sent; a DROP clipped at the end) and prints the values the
  play reports: `uac_play_sent_frames`,
  `uac_play_crc32_l`/`_r` (zlib CRC-32 of the frames sent as 3-byte
  little-endian samples), `uac_play_file_frames`, `uac_play_cut_count`,
  `uac_play_cut_drop_frames`, `uac_play_cut_zero_frames`,
  `uac_play_cut_rejected`, and `cut_first_position` (the first cut's place
  in the stream). With `--first-frame-index` (the play's
  `uac_play_first_frame_index`) it adds `uac_play_cut_first_index` and
  `uac_play_last_frame_index`. With `--sent-frames` (a play that ended part
  way: its `uac_play_sent_frames`) the values are that prefix's.
  `--output-wav` writes the frames sent as a new mono PCM24 WAV: the
  reference a capture must equal. The engine's output is the card, or with
  `--scenario` the card's host render through that scenario
  (`--render` names `host/render/signal_lab_render`; `--seed` replaces the
  scenario's seed, as a PLAYLIST item's `seed=` does; `--render-wav` keeps
  the render). `card_frames` is the card's frame count and `output_frames`
  the engine output's; with `--scenario`, `engine_status` holds the STATUS
  engine fields of the play (frames, clipped samples, the output and
  source digests as `_hi`/`_lo`, events applied and dropped) from the
  renderer's summary. `host/make_playlist.py compare` checks a capture
  against the `--output-wav` stream.

`wfg-cut-host` checks the predictor against the firmware's own cut stage
and log (`wfg_cut_schedule_tests --predict`) on seeded schedules and every
interesting prefix, on cards and on scenario renders; with CMake
`WFG_FSRAND_WAV` naming image A's card it also reproduces image A's CUT
rows.

### `host/usb_storm.py`

USB control stress: storms of CDC sessions while a capture client opens
and closes the stream, with each burst's change of the USB counters. Each
session opens the port, checks the fixture's identity (`INFO?`), reads
`USB?` and closes again, so a burst of them is a burst of EP0 control
requests (port open, line coding, DTR). The capture command after `--`
starts every `--capture-every` sessions, one at a time. This stress helper
currently checks the default RT1170 identity; it has no RT1064 board selector. Standard library and
pyserial (through `live_control.py`).

```powershell
python host/usb_storm.py --port COM8 --expected-serial SERIAL --record-dir storm `
  --bursts 12 --sessions 84 --capture-every 12 -- build/host-release/host/capture/waveform_capture.exe `
  --backend wasapi-raw --device-name "REPLACE_WITH_EXACT_INPUT_DEVICE_NAME" `
  --max-seconds 2 --output storm/cap.wav
```

The example reuses `storm/cap.wav` as disposable churn output; it is not a
retained continuity capture. Use a fresh `storm` directory for each run.

A session reads `USB?` and `STATUS?` before the first burst and after each
one (`--pause` seconds after it, default 2). The printed JSON gives per
burst, and in total, the change of `usb_stack_asserts`, `usb_assert_count`,
`usb_ep0_reordered`, `usb_ep0_unordered` (null on an image without the EP0
order), `uac_stream_opens`, `uac_short_packets`, `uac_missed_intervals` and
`uac_play_missed_intervals`, with any session that failed. Each burst's
requests go to `<record-dir>/storm-NN.jsonl` (new files; `storm-00` is the
baseline). Exit 0 when neither assertion count rose and every session
succeeded, 1 otherwise, 2 when it could not run. It opens no audio device
itself; the capture command does, and it must be a client that opens the
stream and exits (an exclusive capture of a few seconds).

### `host/media_eject.py`

Card staging without Explorer's Eject, which can remove the board's whole
composite device (#33; [why](protocols-media-and-artifacts.md#card-ownership)).
It finds the board's drive by its disk's SCSI identity (`WFG` `RT1170 SD
LOADER`, or `RT1170 SD READ` on the read-only image) and refuses any other
drive, or a guess between two. Standard library and ctypes; `cycle` also
needs pyserial. Windows only.

```text
find                         The PC's drives, the board's marked (no elevation)
eject [--drive X]            Lock and dismount the volume, allow medium removal,
                             eject the medium (START STOP UNIT, LoEj=1)
stage FILE [--name N.WAV] [--drive X] [--eject]
                             Copy FILE into \WG\, flush, read it back past the
                             Windows cache (FILE_FLAG_NO_BUFFERING), compare
                             SHA-256; --eject ejects the medium after
cycle --port P --expected-serial S --record-dir D --file F [--cycles N]
                             The staging cycle N times (default 20), with its checks
```

Run `eject` and `stage --eject` from ordinary PowerShell first.
The helper reports the actual process token separately from the volume's
`access` rights; opening a read-write handle does not prove elevation.
Device/filesystem permissions or managed Windows policy can still deny access.

Use `--expected-serial SERIAL` on `stage` and `eject` to select storage by the
fixture's USB serial. If `--drive` is also supplied, both must match. `cycle`
binds its required serial to CDC **and** storage and accepts
`--expected-board MIMXRT1064-EVK` (default: RT1170). This matters when both
boards are connected and advertise the same storage product string.

Exit codes: 0 done; 1 a check failed; 2 refused selection/name/overwrite;
3 Windows denied both volume-open access modes (check policy/permissions;
elevation may be needed on that system); 4 the lock did not succeed within
20 attempts (an open file or policy can cause error 5); 5 a later media
request failed. No failed lock/dismount/eject authorizes `MEDIA LOCAL`.
The tool never launches UAC or uses a reset as a substitute for dismounting.

See the [Windows bench](../windows-usb-bench.md) for the staging sequence.

`cycle` repeats the media handoff check. Each cycle: a session sends `USB?` and
`MEDIA HOST`; the tool waits up to 30 s for the drive's volume; `stage
--eject`; a second session sends `STATUS?` (expects `msc_ejected` 1 and
`msc_host_hold` 0), `MEDIA LOCAL` (the dismount happened, so it asserts
`--host-volume-dismounted` itself), `STATUS?` (expects `media_state` 3) and
`USB?` (expects no new `usb_stack_asserts`); then the COM port must still be
listed (`serial.tools.list_ports`). If the board does not confirm eject with
no host hold, the cycle stops without sending `MEDIA LOCAL`. Each cycle's requests go to
`<record-dir>/cycle-NN.jsonl`. The JSON result gives per cycle its seconds,
write and read MB/s, the access the eject used, actual process elevation and each check; the run stops
at the first cycle that cannot finish. Exit 0 when every cycle passed.

### USB sound card session

The [Windows bench](../windows-usb-bench.md) owns the complete RT1170/RT1064
discovery, serial-bound staging and capture sequence. The
[USB bring-up runbook](../usb-sound-card-bringup.md) adds manual diagnostics.
Use [testing](../testing.md#physical-usb-playback-and-cw) for continuity
acceptance; a completed capture command alone is insufficient.

### Custom waveform generation over CDC

`live_control.py ... command "ENCODE ID PROFILE INPUT.BIN OUTPUT.WAV"` requests
file generation from a plugin advertised by `INFO?`. The default build has none.
Profiles are module-defined; the public `TONE` example uses a frequency in Hz.
See [the module/CDC tutorial](../custom-encoder-tutorial.md) for wire limits,
custom profiles and parser extension points.

### `host/tx_upload.py`

Requires an enabled plugin that explicitly supports the legacy rate/interleave
protocol. The default build returns `ENCODERS_DISABLED`. For other plugins use
the generic `ENCODE ID PROFILE INPUT.BIN OUTPUT.WAV` live command.

```text
--port COMN
--expected-serial SERIAL
--output NAME.WAV
--mode 75S|75L|150S|150L|300S|300L|600S|600L|1200S|1200L|2400S|2400L|4800U
--payload PATH | --sd-input NAME.BIN
--record NEW.jsonl
--reset
--timeout SECONDS
--generation-timeout SECONDS
```

Use `--reset` only to explicitly abandon a known partial upload/generation. It is
not a generic retry switch.

### `waveform_capture.exe`

```text
--backend winmm|wasapi-raw
--list-devices [--json]
--output PATH
--device-index N | --device-name EXACT
--sample-rate 48000
--channel left|right|average
--max-seconds N
--stop-stdin
```

`--backend` defaults to `winmm`. `--output` and `--max-seconds` are required.
`wasapi-raw` selects the endpoint only by its exact `--device-name`, for
example `"Line (RT1170 Player)"` for the board's USB sound card. The exit code
is 0 only for a capture ended by `STOP` on standard input; the `FINAL` record
gives the reason otherwise.

### `host/capture-session.ps1`

```text
-Output PATH
-DeviceName EXACT
-StopFile PATH
-JsonlPath PATH
-Backend wasapi-raw|winmm
-Channel left|right|average
-MaxSeconds N
-Recorder PATH
```

### `host/analyze_capture.py`

```text
positional CAPTURE
--output-json PATH
--source PATH
--zero-run-samples N
--constant-run-samples N
--dropout-level VALUE
--correlation-window-seconds VALUE
--correlation-search-seconds VALUE
```

### `host/media-control.ps1`

```text
-Port COMN
-Action Status|Host|Local|Play
-TimeoutMilliseconds N
-HostVolumeDismounted
```

The switch is valid only with `-Action Local` and is an operator assertion, not a
filesystem operation.

## 4. Scientist and maintainer view

CLI parsers are part of the experimental contract. Required paths must be explicit;
hardware identity must be checked; existing evidence files must not be silently
overwritten; finite numeric values must be validated before narrowing; and an
uncertain mutation must not be automatically retried.

A critical error, a defect in the tool (a failed `Result` taken as a value), ends
`signal_lab_render`, `wfg_watterson_render`, `waveform_capture` and
`pathsim_chain` with one line on stderr,
`<tool>: critical error: <code> (<number>): <message>; call site 0x<address>`,
and the tool's failure status: 2 for the two renderers, 1 for the capture and
for `pathsim_chain`. Each tool's `main()` installs that handler
(`common/host_critical_error.hpp`) before anything else.

Exit success means the tool completed its own contract. It does not elevate the
evidence layer. For example, `signal_lab_render` success establishes an output
artifact, `live_control` success establishes an acknowledgment, and capture success
establishes a finalized recording. None alone establishes correct modem decode.

When changing options, update parser tests, this reference, root examples, replay
or sidecar schemas, and any scripts that invoke the old form. Preserve a compatibility
alias only when its semantics remain unambiguous.

## 5. 5th-grader view

Command-line options are labels on laboratory switches. `--source` says which
recording enters the machine. `--output` says where the new recording goes.
`--seed` chooses the repeatable random pattern. `--expected-serial` checks that you
are turning knobs on the intended board instead of another USB device.

The program should stop when a required label is missing rather than guessing.

## 6. Toy worked example

This small offline experiment has an explicit input, recipe, output, and report:

```powershell
python host/stage_wav.py make-tone --output clean.wav

build\host-release\host\render\signal_lab_render.exe `
  --scenario cw-case.json --source clean.wav --output impaired.wav `
  --sidecar impaired.json --block 256

python host/stage_wav.py inspect impaired.wav
```

Changing `--block 256` to another legal block size should not change the impaired
PCM digest. Changing the scenario seed is expected to change random impairments.

## 7. Common operating mistakes

| Mistake | Correct behavior |
|---|---|
| Reusing a historical COM port | Enumerate and match VID/PID/serial |
| Sending `MEDIA LOCAL` while Windows still mounts the volume | Flush and dismount first, then assert the handoff; the board answers `HOST_LOCKED` or `HOST_WRITING` while Windows still holds the volume |
| Expecting the ejected drive to come back by itself | It stays absent (`msc_ejected` 1) until the next `MEDIA HOST` |
| Explorer's Eject or Safely Remove on the board's drive | It can remove the whole composite device (COM port and endpoint); use `host/media_eject.py` |
| `PLAY` on the USB route before any program records from "Line (RT1170 Player)" | Start the capture first; the board answers `NO_STREAM` |
| Starting or stopping a capture client during a USB play | Open every client and let the stream settle before `PLAY`; Windows pauses the stream while it reconfigures |
| Asserting `uac_missed_intervals` per play | Assert `uac_play_missed_intervals`; the stream count also holds idle Windows pauses |
| Reusing an old journal filename | Create a new retained journal |
| Retrying a timed-out mutation | Reconnect and inspect state |
| Treating `--overwrite` as harmless | Use only when intentionally replacing derived artifacts |
| Calling a deterministic seed a statistical campaign | Run and identify the required realization set |

## 8. Demonstration scripts

[How to use](../how-to-use.md) documents `scripts/demo-waveform.ps1` and
`scripts/demo-download.ps1`. Both render clean/CW/mixed PCM24 artifacts and take
`-Play none|clean|cw|field`, an explicit `-Device`, and a new `-OutputDirectory`.
The first accepts optional `-SourceWav`; the second downloads a sample
and records its explicit conversion before invoking the first.

`host/play_wav.py WAV --device EXACT --gain-db -12` plays a retained result;
`--list-devices` enumerates outputs without playback. `host/prepare_demo_wav.py
SOURCE OUTPUT --seconds 10` performs the documented demonstration conversion.
These are host conveniences, not embedded audio callbacks or qualified signal
conditioning tools.
