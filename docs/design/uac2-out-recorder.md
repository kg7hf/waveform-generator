# UAC2 OUT recorder design (proposed)

**Status: proposal for technical review; no firmware implementation or board
acceptance is implied.**
The decisions below describe the proposed implementation. The
clock-source and packet-normalization details, file-owner integration, and
memory placement are explicit review gates before code starts.

## Proposed contract

Add a USB Audio Class 2 OUT data *consumer* alongside the existing UAC2
capture stream. A PC sends 48 kHz, stereo, 24-bit PCM in 32-bit
little-endian subslots to the device. The device records PCM24 to `2:/WG` on
the SD card. Feature-unit mute and volume remain observable controls and are
never applied to either samples or recorded data. Recording is exclusive with
playback, playlist activity, USB-host media ownership, and another FatFs
transaction. The target recording contract is exactly 48 output frames per
USB millisecond, with absent or short packets represented by exact zero
samples and counted. That gap-timing behavior is blocked on proving a reliable
OUT completion interval tag (see below). No USB interrupt or callback calls
FatFs.

This is a digital capture claim only. It does not prove host mixer behavior,
RF output, modem decoding, or analog fidelity.

## Interface and clock

Keep the current UAC2 topology and append a separate OUT streaming path. The
existing function uses interface 3 for control and interface 4 for capture,
with endpoint `0x84`; interface 5 and endpoint `0x04` are the proposed recorder
assignments. Add a USB-streaming Input Terminal, its own Feature Unit, and a
non-speaker Output Terminal, all using the shared clock source 4; preserve the
existing capture entities and IDs. The exact terminal type and new entity IDs
need descriptor review. Preserve alternate 0 with no endpoint and add a
single alternate 1 for Type I PCM, stereo, 24-bit samples in 4-byte subslots,
48 kHz. Keep mute/volume state read/write and report it, but do not scale or
mute recorded values.

**Clock proposal:** declare the OUT endpoint synchronous to the same internal
48 kHz clock source that is synchronized to USB SOF. Do not add explicit
feedback or rate conversion. This matches the existing IN function's
one-48-frame-per-millisecond time base and is intended to preserve gaps in the
recording timeline rather than shift later samples. Use the same
1 ms interval at FS (`bInterval=1`) and HS (`bInterval=4`, eight
125-microsecond microframes). The proposed endpoint is isochronous,
synchronous, data; its 392-byte capacity allows 49 stereo subslot frames even
though the recorder's logical interval is 48 frames. Confirm this capacity
and Windows driver behavior in descriptor review and enumeration tests.

This clock choice does **not** assert that the RT1170 crystal and PC SOF are
frequency-identical. Add independent observability: at each USB SOF interrupt,
snapshot a free-running, crystal-derived board counter that does not derive
from the USB clock. Capture the counter at the DCD/interrupt boundary and
carry that snapshot through the event queue; sampling it later in a TinyUSB
task would measure queue latency. Over a reported window of `N` SOF intervals,
report counter delta,
nominal counter frequency, and signed SOF-versus-crystal ppm:
`(N * counter_hz / counter_delta - 1) * 1,000,000` (define sign in code and
tests). Use the RT1170 clock tree to select and document the actual independent
counter before implementation; do not use a counter clocked from USB SOF or
the same synthesized clock being measured. Keep the raw window and deltas in
`USB?`/`INFO` so a ppm result is auditable. If the board has no suitable
independent counter in the selected image, stop and revise the design rather
than publish a fabricated ppm value. Compare this measurement with the
codec-route clock evidence separately; the codec is not locked to USB. This
USB SOF choice does not resolve the separate USB/codec clock domains (the
WM8960 is the I2S clock master). A shared-clock/clock-domain-crossing or other
explicit bridge design remains a separate phase-9 requirement; do not imply
that the OUT recorder makes USB and codec samples share a clock.

The official [USB Audio Class 2 specification, release 2.0 with errata and
ECNs through 2026-09-14](https://www.usb.org/sites/default/files/Audio2_with_Errata_and_ECN_through_Sep_14_2026.pdf)
defines synchronous isochronous clock locking to USB frames (section 3.11.2),
the terminal/alternate-setting requirements (sections 3.16.2 and 4.10), and
feedback use for adaptive sources or asynchronous sinks (section 5.10.4).
Descriptor behavior must be checked against that specification and the
TinyUSB version pinned by this repository.

## Interval and sample semantics

**Blocking observability issue:** the current TinyUSB `dcd_event_t` completion
event carries endpoint, result and length, but no completion frame. The current
`m110_usb_frame_number()` samples the controller's *current* FRINDEX; reading
it in an `audio_device`/TinyUSB callback can date a queued completion in a
later frame. Callback-time SOF ordering alone therefore cannot support the
exact missing-interval claim below. Do not implement or describe interval
recovery as exact until this is resolved.

First inspect the pinned DCD IRQ path and design a DCD-layer transport for the
OUT completion's service-interval identity (and SOF counter snapshot) that
survives TinyUSB queue delay. This may require a narrowly scoped extension to
the TinyUSB DCD event payload/queue; that dependency needs separate ownership
and review before implementation. Capturing FRINDEX at the DCD transfer-complete
interrupt and carrying it in an extended event is a candidate, not an assumed
solution: prove that interrupt latency and simultaneous SOF/completion
ordering cannot shift the tag, or identify a hardware event timestamp that
can. Extend/wrap event numbering beyond the 11-bit frame number without
confusing full-speed frames and high-speed microframes. Add delayed-ISR,
delayed-callback, same-boundary ordering, and frame-wrap tests. If the DCD or
hardware cannot provide a reliable interval tag, return to design review and
remove exact packet-absence timing claims; do not infer the completion's frame
from callback time.

Once that mechanism is demonstrated, use its SOF boundaries and completion
tags to associate packets with intervals. Preserve event order rather than
assuming callback order. Arm only after the file header has opened; acknowledge
`RECORD` when ready to begin at a complete interval. Frame index zero is the
first frame written after that arm point and advances by 48 for every interval,
including a missing one.

For each armed interval, publish exactly one 384-byte stereo block to the
single-producer/single-consumer ring:

- 48 complete frames: store the received bytes unchanged.
- 1 through 47 complete frames: store those frames, zero-fill the tail, and
  increment the short-packet counter.
- no packet (including a UAC2 null packet): store 48 frames of exact zero and
  increment the missed-interval counter.
- more than 48 frames, a non-frame-aligned byte count, or a duplicate packet
  for one interval: reject that interval as malformed, store 48 zero frames,
  and increment a malformed counter. Do not trim silently or shift later
  frames. Although the endpoint capacity is 49 frames, capacity is not the
  recording-time authority.

After interval tagging is proven, the accounting task must also materialize an
empty interval if no completion arrives before the following SOF. A valid
48-frame all-zero packet
is valid audio silence, not a missing packet. Consequently `rec_missed_intervals`
measures absent intervals; `rec_short_packets` and `rec_malformed_packets`
describe the other normalization cases. A host pause can only be identified
when the USB transfer is absent/short/malformed; an explicitly transmitted
48-frame silence block is indistinguishable from intentional digital silence.
Document this limit in host tooling and in the status field descriptions.

Reset recorder frame index and per-record counters for every successful start.
Track the UAC endpoint's opens, packets, decoded frames, short/missed/malformed
intervals and most recent host mute/volume since boot as separate stream-level
counters. Distinguish “not recording” from “recording valid zeros.”

## Buffering, file owner, and WAV format

The OUT callback copies into an OCRAM1 `NOLOAD` SPSC ring and publishes a
sequence number with lock-free 32-bit indices and a memory barrier. It never
allocates, blocks, logs, or touches a file. Reserve 1024 interval slots of 384
PCM bytes each (393,216 bytes, 1.024 seconds at 48 kHz), plus compact per-slot
flags and indices. Align slots/cache lines and prove placement, bounds and
startup behavior from the linker map. Explicitly initialize ring indices,
flags and counters before enabling USB reception: `NOLOAD` memory is not
initialized at reset. OCRAM1 is nominally at `0x20240000` with 512 KiB in the
RT1170 memory map, but the current linker script does not map it. Add a
dedicated `MEMORY` region and aligned `NOLOAD` output/input section, then add
post-link checks for its address, size and bounds. Review MPU/cache attributes
for the new region; the ring is CPU-copied, not a DMA endpoint buffer, so do
not claim that it needs noncacheable MPU treatment. It must not consume DTCM or
steal space from the existing OCRAM2 playback/engine buffers.

The player task drains complete interval blocks and is the only file writer.
Drain in at least 8 KiB batches. The task owns the record file from create
through close and patches RIFF/data sizes only after all writes finish. Reuse
the existing player-task-only TX artifact service (`rt1170/tx_artifact.hpp`
and its `rt1170/player.cpp` call sites); do not add another worker or a new
filesystem arbiter. Coordinate through that service so generation cannot
overlap a recording. A busy generation request must cause a deterministic
`BUSY` response; no concurrent mount, unmount, read, or write is allowed. The
card must be Local and ready for the complete recording lifetime. `MEDIA HOST`,
playback, playlist start, and new generation transactions are refused until
close.

Accept `RECORD:NAME.WAV` only for a validated 8.3 basename under `2:/WG/`,
when the card is Local/ready, playback and playlist are idle, and the name does
not already exist. Never overwrite. Return the exact refusal reason. Accept
`RECORD STOP` idempotently; a frame limit may be supported but does not replace
STOP. Use stereo PCM24 WAV by default. The WAV data chunk itself provides frame
count; add a same-basename `.REC` sidecar created exclusively with the WAV to
hold per-channel CRC-32, first/last file frame index, missing/short/malformed
counts, maximum SD-write latency, ring high-water, and crystal/SOF measurement
window (`uac_sof_window_intervals`, `uac_sof_counter_delta`,
`uac_sof_counter_hz`, and signed `uac_sof_ppm`). CRC-32 uses zlib/IEEE CRC over each channel's 3-byte little-endian
PCM24 samples in frame order. If either file cannot be created, do not arm.

Stereo PCM24 at 48 kHz is 288,000 bytes/s and exceeds classic RIFF's 32-bit
data-size limit after about 4.14 hours. For the requested five-hour run,
provide an explicit mono-left recording option (144,000 bytes/s, below the
limit) or stop before the RIFF limit with a clear `SIZE_LIMIT` error. RF64,
automatic file splitting, and changing sample rate are outside this proposal;
review them separately if five-hour stereo is required.

## Status, errors, and resource limits

Expose a bounded `REC?` report with a compile-time serialized-size limit of
4096 bytes. Include `rec_state`, `rec_error`, `rec_frames`, `rec_crc32_l`,
`rec_crc32_r`, `rec_first_frame_index`, `rec_last_frame_index`,
`rec_missed_intervals`, `rec_short_packets`, `rec_malformed_packets`,
`rec_overruns`, `rec_max_write_ms`, `rec_ring_high_water`, `uac_out_open`,
`uac_out_packets`, `uac_out_frames`, `uac_out_missed_intervals`,
`uac_out_short_packets`, `uac_sof_window_intervals`,
`uac_sof_counter_delta`, `uac_sof_counter_hz`, signed `uac_sof_ppm`, and host
mute/volume. `uac_out_frames` counts received complete frames before padding;
`rec_frames` counts normalized frames actually written. Define whether each
counter is per-record or since boot in the protocol docs and test ordering/size.

If the ring is full, atomically latch `OVERRUN`, stop accepting data for that
recording, and let the file owner drain already-published blocks and finalize
the valid prefix. Never overwrite unread audio. A write failure, card removal,
ownership loss, or RIFF size limit latches a stable error and initiates close;
patch sizes and sidecar only when the written prefix is known. Preserve an
incomplete file if safe finalization fails, and report it as incomplete rather
than presenting it as a successful recording. Do not attempt FatFs recovery
from an ISR.

## Software verification and acceptance gates

Before implementation, review: endpoint/terminal topology; 392-byte
`wMaxPacketSize` against the pinned host/USB stack; a proven DCD completion
interval-tag mechanism; actual crystal-counter source; one-owner FatFs
arbitration; OCRAM1 addresses/capacity and MPU attributes; WAV/sidecar naming;
and 5-hour output format. Keep proposed choices above labeled as pending until
those reviews resolve them. Phase 9 must separately resolve the USB/codec
clock-domain bridge.

Host tests should cover descriptor lengths and FS/HS parity, alt 0/1, endpoint
address/attributes, DCD event tagging under delayed IRQ/callback and
same-boundary SOF/OUT ordering, extended frame-number wrap, one interval per SOF,
0/short/48/49-frame and malformed packets, exact zero fill, explicit zero
audio vs absent packets, wraparound and SPSC ordering, ring-full stop/drain,
record arming at the next interval, filename/precondition/arbitration failures,
WAV/sidecar create/close/patch failure, sample count, channel CRC, frame
indices, counter-to-ppm sign/window, JSON size/order, and repeated start/stop.
Use fake clock, USB events, and a fake filesystem; host tests do not prove
interrupt timing or physical SD throughput. Update `host/live_control.py` for
`RECORD`/`RECORD STOP`/`REC?`; add an exclusive-mode 24-in-32 stereo source to
`host/play_wav.py` with explicit endpoint selection for the bit-exact test.

Cross-build checks should validate descriptors, static buffer/size assertions,
OCRAM1 `NOLOAD` placement and exact map occupancy, no added DTCM, task stack
limits, warning/tidy gates, and the 4096-byte report bound. No board execution
is authorized by this design task.

Physical acceptance remains pending: enumerate on Windows without changing
the system default playback device; confirm the OUT format and host mute/volume
reporting; play a known exclusive-mode PCM24 source and verify exact samples,
constant onset offset, zeroed missing intervals, sample count and CRC; show
that Windows shared-mode volume does not change recorded samples (or document
the host mix semantics); measure recorder SOF/crystal ppm and compare with the
separate codec route; run at least five hours in the selected mono/stereo
format with no overrun, write fault, assertion or DTCM regression; and verify
card removal/host-media handoff failures are clean. These gates require the
authorized RT1170 board, card, PC endpoint and bench setup. A host test,
cross-build, map check, or descriptor inspection cannot satisfy them.
