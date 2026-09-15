# RT1170 Player and Audio Pipeline

## 1. Scope

This guide covers the MIMXRT1170-EVK CM7 firmware owner loop, SD WAV reader,
static and live impairment processing, OCRAM output FIFO, SAI1/eDMA transport,
WM8960 codec task, and engineering counters.

The default image plays supplied WAV files with static and live effects. Optional
[encoder modules](../encoder-plugins.md) create saved WAVs before playback;
they do not change the audio pipeline.

## 2. 50,000-foot view

The RT1170 player is a producer/consumer pipeline. A lower-priority player task
reads and processes audio ahead of time. A highest-priority audio task drains
small fixed blocks at the 48 kHz hardware deadline.

```text
microSD WAV -> player task -> scenario engine -> live controls
-> 32,768-frame OCRAM FIFO -> 128-frame audio blocks -> SAI1/eDMA -> WM8960
```

This separation keeps SD latency and impairment cost away from the codec callback.
The player task is the single owner of source, engine, and control mutations.

## 3. How to use it

The current bench is hard-patched between the RT1170 codec and the PC Realtek
headphone/microphone ports. Use Realtek output to feed the patched rig and
Realtek microphone capture for its return. USB remains the control/media path.

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
4. Send `MEDIA LOCAL` with the host-side dismount assertion.
5. Send `LOAD:PLAY.WAV`.
6. Configure controls if needed and send `PLAY`.
7. Poll `STATUS?`, then `STOP` if the run is intentionally bounded.
8. Return media to host ownership only after audio and file activity stop.

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

### State and ownership

Commands enter one fixed mailbox. The player task acknowledges state-changing
commands after it owns the mutation. `LOAD` resets selection-dependent schedules.
`PLAY` opens, validates, premeasures or accepts a reference, configures engines,
prefills, starts codec transport, and transitions through running to done, aborted,
or faulted. A completed/aborted/faulted run requires another `LOAD` before replay.

### Failure and counters

Important groups are:

| Group | Counters |
|---|---|
| SD/file | read calls/bytes, short reads, read errors, maximum SD cycles |
| FIFO | fill, min before EOF, max, average, critical events, underruns |
| Engine | state/error, frames in/out, clipping, events, source/output digests, max cycles, arena |
| Audio | running, blocks, backlog, overruns, RX/TX errors, max block cycles |
| Live | frame, digest, clipping, event count, queue/capture space, effective reference |

Build success does not test physical word alignment, analog level, clock quality,
or cable routing. CDC success does not test DAC audio. Analog capture does not by
itself test modem decode.

## 5. 5th-grader view

The SD card reader is a cook making sandwiches ahead of time. The audio hardware
is a hungry customer who takes one small sandwich on an exact schedule. A large
tray sits between them. If the cook pauses briefly, the customer still eats from
the tray. If the tray becomes empty, that is an underrun.

The cook also adds the requested sound effects before putting sandwiches on the
tray. The customer never opens files or calculates effects.

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

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Owner loop and player state | `rt1170/player.hpp`, `rt1170/player.cpp` |
| Board/runtime/task startup | `rt1170/main.cpp`, `rt1170/platform/board.*`, `runtime.*`, `rt1170/os/task.*` |
| SD and MSC block layer | `rt1170/platform/waveform_sd.*`, `waveform_msc.*` |
| Codec transport | `rt1170/platform/wm8960_codec.*` |
| Linker placement | `cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld` |
| Status response | `rt1170/status_contract.hpp`, `rt1170/usb_service.cpp` |

Any timing-path change needs host owner-loop tests, target Release builds, memory
maps, worst-cycle counters, bounded physical playback, and explicit evidence of
which layer was actually exercised.

## 8. Publication and maintenance walkthrough

The producer owns SD reads, static-engine state and live control mutation.
`push_samples` writes payload words into free FIFO slots before publishing the
write index. `player_audio_hook` takes an index snapshot, copies available words,
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

Read `player_audio_hook`, `push_samples`, the owner command/run loop and
`player_status` together. Run `wfg-player-live-owner` and both writable/read-only
firmware builds after changing this contract. Host stubs do not model real
interrupt preemption, cache visibility or DMA clocks; physical timing and audio
checks remain necessary for hardware acceptance.
