# Finite prepared-stream coordination

## 50,000-foot view

`common/finite_playback.*` is the portable ownership and accounting foundation
for producing a finite waveform into storage while playing an earlier part.
It separates persistent prepared lead from the small RAM playback queue.
It neither opens files nor accesses an audio device. The RT1170 PREPARE/PLAY
path remains sequential; the experimental PREPAREPLAY integration below uses
the coordinator to render into storage while playing committed PCM.

The coordinator is compiled for host and ARM. A host simulation exercises
delayed production, storage visibility and playback using a virtual clock.
Passing that simulation establishes accounting and failure behavior, not SD
throughput or an RT1170 playback deadline.

## How to use it

One owner configures the known finite output length, conservative measured
production rate, safety margin, RAM queue capacity and maximum operation size.
The rate must represent end-to-end production under simultaneous playback and
storage contention. No existing isolated board measurement establishes it.

The owner requests bounded actions, performs the associated work, and returns
the exact action ticket with its checked status and frame count. Actions cover
production, writing, a storage visibility barrier, reading into the playback
queue, and finalization. Only one action may be outstanding. Report actual
audio consumption separately, including while a storage action is pending.

`next_action()` supplies a conservative default policy: before start, accumulate
the disk lead and fill the playback queue; while running, refill the queue from
committed storage first when there is room for a useful batch, then confirm a
previous write's visibility, write the staged block, or produce the next block.
Each request reserves just one bounded action. A busy result means an action is
already pending, the initial queue is ready for explicit `start()`, or the
producer is waiting for playback to consume queued frames. It is not a polling
loop or a guarantee that the selected operation fits the hardware time budget.
The explicit `begin_*` methods remain available for policy tests and adapters.

Production uses one bounded staging block. A successful write alone does not
make its frames readable: the visibility barrier must also succeed. The
storage adapter must define and implement that barrier for its actual writer
and reader handles. Do not substitute the existence of a file or an attempted
flush for confirmed visibility.

Start requires both the prepared lead and a full initial RAM queue (or all
remaining frames for a short artifact). Cancellation preserves a pending
ticket until the owner settles it. The owner still owns all file handles,
buffers and cleanup; the coordinator never claims those resources are closed.
After a terminal result and settled tickets, the owner calls `release()` only
after stopping transport and completing external cleanup. A new configuration
is refused until this explicit acknowledgment; tickets are never reused across
configurations of the same object.

Unknown output length, including an unplanned sample-slip result, must use
complete preparation first. A growing temporary WAV must not be handed to
the normal LOAD/PLAY reader. The adapter uses a private bounded stream reader,
then header finalization, verification and completion publication.

## Scientist and maintainer view

The ordered cursors are:

```text
consumed <= read_frames <= committed <= written <= produced <= total
```

`produced - written` is bounded staging memory. `read_frames - consumed` is
bounded playback RAM. `committed - read_frames` is unread persistent storage
and can be much larger than either RAM buffer. Reading an uncommitted frame,
overfilling the queue, acknowledging a different action, or restarting an
active job is refused. A physical operation failure or playback shortage
latches a terminal failure; no silence or shifted timeline is synthesized.

For output length `N`, playback rate `F = 48000`, conservative contended
production rate `R`, and margin `M`, the initial persistent lead is:

```text
P = min(N, ceil(N * max(0, F - R) / F) + M)
```

Integer arithmetic must avoid overflowing the intermediate multiplication or
margin addition. The formula is a planning lower bound: a correct average rate
does not bound individual I/O delays. Queue sizing and `M` must account for
those delays, and a missed deadline remains a failure even when the formula
was satisfied.
The coordinator's logical counts can span uint64; the storage adapter must
separately enforce its WAV/container size limit before accepting a job.

Toy example: `N=480000`, `R=30000` and `M=4800` require `184800` prepared
frames (3.85 seconds of output) before start. A 4096-frame RAM queue is still
possible because the lead is on storage. These numbers are illustrative,
not measured board parameters.

Terminal success additionally requires successful artifact finalization.
Playback completion alone never proves that the retained artifact was
finalized, verified or published. No public artifact should become reusable
until its storage owner has completed the existing publication contract.

## 5th-grader view

Think of someone writing a long story while another person reads it aloud.
Finished pages can wait in a large pile, but the reader holds only a few pages
at a time. The reader cannot read a page whose ink is still wet. If the writer
falls too far behind, we record that the reading stopped; we do not invent
words to hide the interruption.

## Next integration gate

`rt1170/prepared_stream.*` now implements the storage boundary. It borrows one
fresh read/write FatFs handle, tracks exact writes, syncs before advancing the
committed watermark, and restores the append cursor after a prefix read. It
refuses writes over committed PCM while allowing final RIFF header patching.
Read scratch is word-aligned and borrowed; the output byte offset determines
the scratch prefix required to preserve alignment of direct sector transfers.
The owner must retain the file and scratch until cleanup is complete.

`PrepareJob` uses that stream for ordinary preparation and exposes owner-only
`commit_prefix()` and `read_prefix()` during rendering. Source read/decode,
DSP/pack and append now take separate `step()` calls. The playback read scratch
is separate from pending packed output, so an intervening prefix read cannot
destroy a staged block. I/O failures enter existing retryable cleanup, while
range refusals leave the job active. Reads do not publish a completed WAV.

Host tests interleave the actual engine, writer and prefix reader, preserving
the final PCM and manifest contract. Another test drives the scheduler through
the real stream class with fake FatFs. Neither models card latency or DMA.
The normal and read-only firmware builds still need physical regression;
splitting a render into three steps can add two one-tick owner waits per block
under the existing player loop. No throughput improvement is claimed.

## Experimental player integration

The writable player advertises `prepare-play-experimental` and accepts:

```text
PREPAREPLAY:OUT.WAV:SRC.WAV:CASE.SCN:KEEP|TEMP:RATE:MARGIN[:SEED]
```

Names, retention and seed follow PREPARE. RATE is a positive uint32 in frames/s;
MARGIN is a uint32 in frames, including zero. Both must use canonical unsigned
decimal spelling. Neither has a default. RATE must be a conservative measured
end-to-end rate with SD writes, syncs and concurrent playback reads, not an
isolated engine benchmark. This interface does not establish those measurements.

`host/live_control.py prepare-play` takes the usual `--source`, `--scenario`,
`--output`, `--retention` and optional `--seed`, plus required `--rate` and
`--margin-frames`. The ACK means admission only. Monitor `PREPAREPLAY?` for
overlap progress, `PREPARE?` for the artifact lifecycle and `STATUS?` for
playback errors, FIFO and transport counts.
A completed artifact alone is not evidence of successful external audio capture.

`PreparedPlayback` schedules managed PrepareJob production, appends, checked
visibility barriers and prefix reads. The logical queue prefill is 24,000 frames
(or the entire shorter file); each action handles at most 2,048 frames. The
existing FIFO receives the stored PCM24 samples directly. The impairment engine
runs once while rendering, never again on the playback samples. Finalization,
verification and manifest-last publication wait for the finite output to drain.
Both retention modes keep successful WAV/JSN pairs, as with ordinary PREPARE.

STOP cancels unfinished work and releases ownership only after cleanup; a failed
cleanup retains ownership for a later STOP retry. Other mutating commands are
BUSY. Pending CUT schedules and live controls refuse admission rather than being
silently ignored. Read-only images refuse the command. Any sample-slip stage is
rejected before output creation; use ordinary PREPARE followed by PLAY instead.

The command is experimental until physical testing establishes conservative
contended production rates, worst I/O stalls and suitable margins. Production
rate auto-selection, playlist timing integration, physical
overlap acceptance and higher sample rates remain separate work. The completed
PREPARE-then-PLAY path remains the tester's current-main workflow.

### Coherent progress query

`PREPAREPLAY?` is an unscheduled read-only WFG-LIVE/1 query. The host command
`prepare-play-status` requires the `prepare-play-experimental` capability and
prints the reply containing a `prepare_play` object. It performs no file I/O and does not
advance preparation. The player publishes all fields together after each
bounded owner step; a query sees one publication, including the 64-bit counts.

| Fields | Meaning |
| --- | --- |
| `operation_id` | Boot-local uint64 admission counter. Zero means no PREPAREPLAY has been admitted; each successful admission advances it once. It never wraps. |
| `active` | The coordinator still owns an admitted operation, including cleanup. |
| `state` | Scheduler state: 0 unconfigured, 1 ready, 2 running, 3 cancelling, 4 completed, 5 failed, 6 cancelled. |
| `rate`, `margin_frames` | The admitted explicit production rate in frames/s and lead margin in frames; uint32. They are inputs, not measured telemetry. |
| `total_frames`, `required_lead_frames` | Finite output length and computed persistent lead; uint64. |
| `produced_frames`, `written_frames`, `committed_frames` | Cumulative generated PCM, appended PCM, and PCM made readable by a successful visibility barrier; uint64. |
| `queued_frames`, `consumed_frames` | Cumulative PCM read into the output queue and taken from it; uint64. Their difference is pending queued PCM, not a transport-delivery count. |
| `started`, `artifact_finalized`, `action_pending`, `owner_released` | Independent scheduler lifecycle facts, encoded as JSON booleans. |
| `status`, `reason` | Coordinator outcome: StatusCode 0 through 8 and a static message bounded to 128 characters before JSON escaping. |
| `timing` | Clock frequencies and per-phase elapsed-cycle accounting, described below. This does not change the admitted rate or lead. |

The ordered watermarks are `consumed <= queued <= committed <= written <=
produced <= total`, with `required_lead <= total`. During initial setup,
`active` may be true while `state` is unconfigured and counts are zero. An
early preparation failure can retain state 0 with a nonzero `status`; do not
infer success from state alone. Before any admission, every count and boolean
is zero/false, status is 0 and reason is `ok`. Refused admissions and ordinary
PREPARE operations leave the last admitted PREPAREPLAY snapshot intact.
The operation identifier remains attached through completion, cancellation,
failure and cleanup. Rejected requests do not advance it. Exhausting the uint64
counter refuses further admission before claiming preparation resources.
When setup fails before scheduler configuration, its flags remain false even
after cleanup; `active=false` reports that the coordinator has released the job.

Read `active`, `owner_released`, `status` and artifact lifecycle together when
deciding whether a run has finished cleanup. FIFO consumption does not prove
USB delivery or external capture: correlate `STATUS?`, `USB?` and the captured
PCM independently. The host rejects malformed snapshots, wrong value types,
out-of-range integers and inconsistent watermarks before returning them.

### Overlap phase timing

The coherent `timing` object contains `cycle_hz`, `tick_hz` and a `phases`
object with eight fixed members. Each phase reports uint64 `calls`,
`total_cycles` and `rejected`, uint32 `max_cycles`, and boolean `saturated`.
Counts reset only on a new admitted operation and remain available after
completion or failure. A refused request or ordinary PREPARE retains them.

| Phase | Timed coordinator work |
| --- | --- |
| `setup` | Preparation job setup steps before the scheduler is configured |
| `source_read` | Source read/decode half of a managed production step |
| `render` | DSP and PCM packing half of that step |
| `append` | Append generated PCM to the growing WAV |
| `commit` | Commit a prefix for subsequent playback reads, including its visibility barrier |
| `read` | Read committed PCM into the playback staging buffer |
| `finalize` | Finish and advance artifact finalization after output drain |
| `cleanup` | Advance cancellation or failure cleanup |

The measurements bracket job calls, including failed calls. They include time
blocked in I/O, interrupts and task preemption. They exclude intervals between
calls, queue copying, physical output and admission outside the coordinator.
Consequently they are elapsed operation costs, not CPU utilization or an
end-to-end production-rate measurement. Zero-cycle calls can be valid at the
counter's resolution. `total_cycles / cycle_hz` describes only accepted timed
intervals; it must not be used to automatically select a safe rate or lead.

A 32-bit cycle counter wraps after about 4.31 seconds at 996 MHz. The
accumulator checks the elapsed RTOS ticks with a conservative two-tick sampling
allowance before accepting a modulo cycle delta. Intervals that might span a
full cycle-counter period, zero clock frequencies, or endpoint frequencies
that differ from the admitted frequency increment `rejected` without adding
cycles. `calls` includes rejected intervals. Aggregate counters saturate instead
of wrapping and set `saturated`; either rejection or saturation makes the
aggregate incomplete for performance analysis.

This guard assumes the RTOS ticks and cycle counter continue running and an
individual call lasts less than a full 32-bit RTOS tick period (about 49 days
at 1 kHz). Debugger halts, stopped clocks or clock changes away and back between
samples are outside that contract. Host tests validate arithmetic, phase
attribution, serialization and lifecycle retention; they establish no board
timing or contended SD throughput. Later physical tests must correlate these
counters with wall time, frame counts, underruns and captured output.

### Saved timing report

`host/summarize_prepare_play.py` reads a saved `wfg-live.session/1` controller
journal using ordinary local file I/O. It requires the expected USB serial,
firmware commit and nonzero operation identifier:

```text
python host/summarize_prepare_play.py saved-session.jsonl --expected-serial SERIAL --firmware-commit COMMIT --operation-id 1
```

Replace the placeholders with the retained firmware identity: `COMMIT` is
40 lowercase hexadecimal digits and `SHA256` is 64. Preserve the original
journal beside the report; the report includes its SHA-256 and the observed
build identity, including the dirty-build marker. This command opens no serial
port and performs no board operation. Input is streamed with limits of 64 KiB
per line, 100,000 records and 256 MiB in total.

Use one controller connection and one selected operation per journal. The
report checks the recorded INFO response against the identity record, validates
protocol responses, and rejects uncertain transactions, mixed operation IDs,
regressing counters or frame watermarks, changed clocks and a return to active
after a terminal snapshot. Initial idle snapshots may precede the selected
operation. Sequence numbers must increase; observation timestamps are not used
as a performance clock.

Each phase uses the **last cumulative snapshot**, without summing repeated
polls. Accepted cycle totals, mean call cost and maximum call cost are converted
to seconds only when the clock and aggregate permit it. Rejected intervals
leave timing partial; saturated counters cannot provide trustworthy derived
costs. These values retain the elapsed-call interpretation above, including
blocking and preemption. They do not establish CPU load, end-to-end throughput,
a safe production rate or a lead margin.

The CLI writes `wfg-live.prepare-play-summary/2` JSON to stdout, including the
observed identity, final snapshot, outcome, timing completeness and per-phase
raw counters with derived durations. Exit 0 requires a successful terminal operation
with complete timing and no recorded command rejection; exit 1 reports valid
but active, incomplete, failed, rejected or saturated measurements. Command
rejections are counted separately and do not change the timing-quality flag.
Exit 2 means the journal or requested
identity cannot be validated. A single terminal snapshot can be summarized,
but does not prove the whole operation was observed. The boot-local identifier
does not bind an admission ACK or prove that a reset could not recreate the
same identity and counters. Physical output and artifact acceptance still need
their independent evidence.

### Bounded host wait

For a script that needs the current or last admitted operation's outcome, use
`host/live_control.py` with the normal explicit port, serial identity and new
journal path, followed by:

```text
prepare-play-status --wait-timeout 600 --interval 1
```

Both durations are finite positive seconds. Without `--wait-timeout`, this is
still one read-only status query. With it, the host records every PREPAREPLAY?
reply and keeps polling while `active` is true, including failed or cancelled
operations that still own cleanup. A completed artifact or terminal scheduler
state alone does not end the wait. The host sends no admission, retry or STOP.

When the operation becomes inactive, the final reply is printed. Exit code 0
requires successful scheduler completion, playback started, artifact finalized,
owner released, no pending action, status 0 and every cumulative watermark at
the total frame count. Idle, cancellation, early setup failure or any other
incomplete outcome returns 4. Invalid options, timeout or an untrustworthy
reply return 2 through the normal CLI error path. Timeout leaves the board's
operation untouched; retained replies show whether cleanup was still active.
The poll wait budget and each CDC transaction's `--timeout` are separate; a
transaction already in progress may take up to that transport timeout.

The wait pins the `operation_id` from its first reply. If any later poll
reports another identifier, including an already completed operation, it
fails with exit code 2 instead of attributing that operation's result to the
original one. The rejection sends no STOP or other mutation.

The first reply still describes the current or last admitted operation; this
does not bind the wait to an earlier admission ACK. An already completed
operation can return immediately. Keep one controlling session/operator when
associating an admission with its first query. The counter resets on firmware
boot and is not a persistent or globally unique experiment identifier; do not
use it to correlate work across resets. Use matching host and experimental
firmware versions: the host requires the new snapshot field and refuses an
older schema. The command's normal final
STATUS? query is still journaled, but physical output acceptance also requires
USB?/codec evidence and the independent capture. No rate is measured or margin
selected by this wait command.
