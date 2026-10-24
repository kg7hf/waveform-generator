# PC virtual-audio delivery proposal

**Status: on hold.** The existing Python WAV playback utility remains available. This note preserves the architecture questions for a future PC delivery backend; it does not describe an implemented project-owned virtual audio device.

## Intended workflow

A user selects a WAV or optional encoder, applies a scenario, and sends prepared or generated samples to a selectable recording endpoint used by another application. A maintained delivery adapter should be evaluated before deciding whether the project needs to own a virtual audio driver. Do not assume an existing virtual driver is reliable without identifying and reproducing its observed failure.

Keep waveform generation, encoder plugins, and scenario semantics independent of the delivery backend. Start with prepared-file playback; validate live generation and effect changes as a separate increment. A custom driver is optional and should be limited to endpoint and sample transport if eventually needed.

## Control and data boundaries

Reuse the WFG-LIVE/1 command model where its contracts fit: sequence-numbered newline-delimited requests, bounded responses, one outstanding request, explicit run identity, and no automatic retry of an uncertain mutation. TCP is for local control, not a network audio stream. The audio samples stay on a local buffered audio path.

A PC profile needs its own identity and capabilities. Do not impersonate a board or weaken USB board identity checks. Endpoint selection, readiness, PC diagnostics, partial socket reads/writes, ownership, disconnect, and reconnect semantics need explicit contracts. Commands should pass through a bounded owner queue; audio delivery must not wait for parsing, network IO, logging, or status clients.

## Acceptance before deployment

- Prepare effects before baseline playback and prefill bounded buffers. Keep file IO, allocation, logging, network waits, and controller locks out of the delivery worker.
- Coordinate receiver readiness and drain the expected tail before completing capture. Generator admission is not receiver readiness, and submitted frames do not prove received frames.
- Record intentional slips, zero intervals, and transport stalls separately from unexpected starvation or receiver discontinuities. An unexplained discontinuity fails a clean-path result.
- Record negotiated formats, channel mapping, conversion, volume and Windows processing settings, driver version, latency, and buffer behavior. A PCM24 source or device label alone does not prove bit-exact delivery.
- Test known samples, tones, start/stop/restart, receiver attach/detach, device loss, sustained playback, and representative host load. Never silently fall back to another endpoint.
- Run applicable rows on at least two Windows PC configurations with the same vectors and receiver version first. Compare PC delivery with RT1170 USB and codec paths separately; analog paths use measured level, delay, distortion, and receiver behavior rather than byte equality.

A successful run on one PC does not qualify other hosts or drivers. Keep RT1170 build and regression coverage as PC support grows. PC-only installation must not require a board, firmware tools, or private encoder modules.
