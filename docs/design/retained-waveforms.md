# Retained impaired waveforms

A finite impaired waveform can be rendered, verified, and played later as a completed PCM24 WAV. Treat preparation, artifact transfer, playback, and live output faults as separate operations with separate identities. The normal playback path consumes a finalized WAV; it must not infer that a growing file is complete.

The host tooling supports preparation, verification, and archive retention. Board PREPARE produces a completed WAV and board-specific `.JSN` record; the record is not the host archive format. Board-side discovery, cache reuse, self-contained archive creation, and automatic cleanup of cancelled temporary files are separate capabilities. The experimental finite-stream coordinator and PREPAREPLAY behavior are documented in the [finite-playback guide](../modules/finite-playback.md); host simulations do not qualify an on-board production rate or concurrent playback.

Retaining a completed render separates impairment generation from delivery. It is useful when rendering is slower than playback or when the same samples must be replayed on multiple paths. Exact generation identity and playback continuity must be checked independently.
## Artifact contract

Each completed retained artifact should have a WAV, the exact scenario used,
and a sidecar that binds the inputs, implementation, and output. At minimum,
record:

- source WAV path and full-file SHA-256, plus sample rate, channels, and frame
  count where available;
- exact scenario bytes and SHA-256, seed, source reference interval and RMS,
  source gain, impairment order, and resolved engine identity/digest;
- renderer identity: executable or package hash, version, and exact command;
- output WAV byte count and SHA-256, PCM-region SHA-256, format, frame count,
  clipping count/policy, and completion state;
- preparation time and any host/platform details needed to reproduce the
  renderer environment.

`archive` retention includes the primary source WAV, any configured secondary
source WAVs, and the exact scenario bytes. A sidecar that names a source by path
and hash is useful for verification, but it is not a replay package if the
source is later unavailable. The manifest records the renderer executable's
path and hash, but does not bundle the executable; exact rerendering still
requires obtaining that executable identity. `verify` checks the artifact and
archived inputs without needing external source paths. `reuse` separately
rechecks the current external input hashes before accepting an existing result.

Preparation always begins from the identified clean input and applies the
identified scenario once. The retained output is a new source artifact. Playing
it uses a clean scenario by default; applying the same impairment scenario
again would create a different, double-impaired waveform and must be an
explicitly identified operation. Reuse an existing result only when its input,
scenario, seed, renderer identity, format, and output hashes all match. Never
replace an artifact just because the output filename matches.

Write incomplete output under a temporary name. On successful finish, validate
the RIFF/WAV, flush and close the file, compute and record its final identity,
then publish the final name and a completion sidecar last. A failed or cancelled
job must not look complete. Temporary files are cleanup candidates, not
automatic garbage: retain them until an operator has verified the completed
artifact and explicitly authorized cleanup. Recovery must inspect the manifest
and hashes rather than infer completion from file existence.

## Preparation time and playback budget

For a finite output of `N_out` frames at sample rate `F`, the content duration
is `N_out / F`. If the measured renderer rate is `R` output frames per second,
compute time is `N_out / R`. Let `N_in` be the number of source frames read;
these can differ when the scenario contains sample slips. For mono PCM24, the
WAV data size is `3N_out` bytes; at 48 kHz that is 144,000 bytes per second,
before the 44-byte header. A conservative on-board preparation estimate is:

```text
T_prepare >= N_out / R + 3N_out / W_write + 3N_in / W_read + T_sync + T_verify
```

Here `W_write` and `W_read` are measured sustained bytes-per-second for the
actual card, filesystem, and access pattern. Include `3N_in / W_read` when the
preparation job reads its source from that card, and include another
source-read term for a secondary WAV. This planning sum assumes these
activities serialize. If a measured end-to-end renderer rate
already includes source reads, do not add that read cost a second time. This is
not a measured guarantee. Do not assume compute and card I/O overlap until a
scheduler has demonstrated that overlap without starving either side.

Check the complete output length against canonical RIFF's 32-bit size fields
before rendering. `Pcm24ArtifactWriter` refuses an artifact whose PCM data and
optional pad cannot fit. For mono PCM24, its current bound is 1,431,655,752
frames; other channel counts have a proportionally smaller frame limit.

Measure a conservative end-to-end production rate on the actual card under the intended workload before calculating a lead. Include source reads, artifact writes, hashing, verification, publication, and concurrent playback contention. An isolated processor rate or preparation-only timing is not a substitute for this measurement.

The ordinary board PREPARE workflow completes, verifies, and publishes the WAV before ordinary playback begins. The experimental finite-stream coordinator and PREPAREPLAY path are documented in the [finite-playback guide](../modules/finite-playback.md). Host simulations do not establish on-board production rate or qualify concurrent SD reads, writes, and playback.

The current signal engine and player use 48 kHz. Higher-rate support at 96, 192, or 384 kHz belongs to future multi-rate UAC2 work and requires separate implementation and validation.

## Current design boundary

The supported board baseline is sequential: finish preparation, verify and publish the WAV, then play it. The experimental PREPAREPLAY path accepts an explicitly supplied production rate and lead margin, but host simulation is not a measurement of that rate under simultaneous SD reads, writes, and playback. Do not call the path real-time or accepted for a given card until contended end-to-end production, queue watermarks, and missed-deadline behavior are measured on the target.

For an indefinite live pipeline, production must exceed the playback rate with measured headroom for card transactions and scheduler latency. A finite job may instead prepare an initial lead and continue while playback runs. Let `N_out` be the finite output length, `F` the playback rate, and `R` a conservative measured end-to-end production rate under contention. The initial lead must satisfy:

```text
P_frames >= min(N_out, ceil(N_out * max(0, F - R) / F) + margin_frames)
T_wait >= P_frames / R
```

`margin_frames` comes from measured worst-case scheduling and I/O stalls. `R` must include source reads, artifact writes, and simultaneous playback contention; an isolated DSP rate is insufficient. A finite lead can support a finite output when `R < F`, but required lead increases with duration. No isolated renderer measurement qualifies `R`.

The present board contract is 48 kHz PCM24. Higher sample rates are separate multi-rate UAC2 work; do not infer their rendering, playback, clocking, or memory feasibility from 48 kHz behavior.
## Time faults and recorded playback

An impaired WAV records the samples produced by the scenario engine. It does
not automatically include the player's later `CUT` stage. CUT edits engine
output after live controls: DROP shortens the sent stream and ZERO inserts exact
zero frames. A CUT record therefore needs its schedule and actual playback
result/counters alongside any WAV; it must not be represented as an ordinary
scenario-only render. USB missed intervals and codec pacing gaps are also
transport-time observations, distinct from CUT and from a sample-slip
impairment. See [Time-step faults](../modules/time-step-faults.md) for those
frame axes and boundaries.

## Acceptance boundaries

Host acceptance covers deterministic rendering, exact output bytes, hash-bound
identity, bounded memory use, refusal/recovery behavior, and verify-only
archives. Firmware acceptance separately covers safe FatFs ownership, bounded
read/write progress, final publication, cancellation and reset recovery, and
playback from the completed artifact. Physical acceptance must separately
measure card write/read throughput, sustained playback, USB/codec output, and
any analog path. A successful host render or a short initial board buffer does
not establish sustained on-board generation, pipelined playback, 192 kHz
support, or a one-hour acceptance run.
