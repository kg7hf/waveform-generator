# Second-source overlay

The normal host renderer and shared RT1170/RT1064 SD player can add one bounded WAV excerpt as a second signal. The stage runs after Watterson and the optional global frequency shift, then before noise and the remaining impairment stages. It adds the excerpt; it does not replace or alter the primary source channel.

```json
{
  "reference_rms": 0.2,
  "impairments": [
    {
      "type": "second_source",
      "source_path": "interferer.wav",
      "si_db": 6,
      "start_frame": 0,
      "source_start_frame": 12000,
      "duration_frames": 24000,
      "frequency_offset_hz": 25
    }
  ]
}
```

`source_path` is resolved relative to the scenario file and must name a mono 48 kHz PCM24 or legacy PCM16 WAV accepted by the host renderer. Use plain valid UTF-8 JSON path characters; escaped path spellings and backslashes are refused by the fixed-capacity scenario parser. The normalized absolute path must also fit in the scenario's 255-byte path field and avoid characters that require JSON escaping, so an otherwise-valid relative path can be refused if its resolved parent makes it too long or needs escaping. The WAV adapter currently uses the renderer's existing narrow-path file interface, so Windows runs should use ASCII filesystem paths. `si_db` is the desired-signal to interferer power ratio. The renderer measures the RMS of the exact selected excerpt, including zero-valued samples, and scales that excerpt to `reference_rms * 10^(source_gain_db/20) * 10^(-si_db/20)`, so S/I follows the desired channel's applied source gain. A silent, empty, truncated, or out-of-range excerpt is refused before any requested output artifact is published. `duration_frames` may be omitted to select the rest of the source; the selected excerpt must be nonempty.

The destination window starts at `start_frame`. `source_start_frame` chooses the first source sample. A nonzero `frequency_offset_hz` is limited to +/-75 Hz. Offset and optional fading share one analytic filter, whose 215-frame latency is relative to the overlay window; with neither enabled the source has no filter delay. The source excerpt is not consumed outside that window, and the renderer does not flush a shifted tail beyond the primary stream length.

The renderer sidecar records the normalized source path, full source frame count, selected excerpt start/count and RMS, desired S/I ratio, offset, latency, and a `pcm_digest`. That digest uses the renderer's existing FNV-1a 64-bit digest over canonical packed PCM24 for the selected excerpt; it is a compact identity check, not a cryptographic file hash. The exported target scenario rewrites the second-source path to its resolved absolute path so it continues to identify the same file from another working directory.

The source is streamed once for the RMS/digest prepass and again when the engine asks for active blocks. As with the primary `WavSource`, callers must keep the WAV file unchanged for the duration of a render; a later read failure is reported and prevents artifact publication, while a same-length rewrite cannot be detected as a locked snapshot. The exported absolute path supports host replay. For SD playback, replace it with the on-card basename as described below. The validation renderer explicitly refuses scenarios that require a second-source provider rather than silently omitting the stage.

The S/I target is defined from the primary input reference after `source_gain_db`, and the selected excerpt's RMS before its added-channel scaling. A short filtered excerpt or a faded desired primary can produce a measured finite-render S/I different from that input-reference command even when the scaling is correct. Host integration checks exercise sample addition and measured S/I, both offset signs, block partition equality, selected-window metadata, relative-path export, replay of the exported scenario, and failure without artifact publication. Physical channel, SD-card execution and receiver behavior require separate board acceptance.

## Independent Rayleigh fading

Add `"fading": {"spread_hz": 1.0}` to the second-source object to fade that
source independently. The object accepts exactly one finite `spread_hz` from
0.1 through 10 Hz, using the existing Watterson Gaussian Doppler process.
The [faded example](../../fixtures/scenarios/second-source-fading.json) combines
this with a stable `stream_id` and a +25 Hz offset.

The stage owns one `WattersonPathState` with unit ensemble mean power, zero
path delay and no additional path frequency shift. It uses PCG32 family 13,
distinct from the primary Watterson and faded-CW streams. An explicit
[`stream_id`](random-stream-identity.md) keeps its random component stable when
other stages are inserted. Its local time starts at zero at the beginning of
the selected excerpt, independently of source and destination file offsets;
it advances on all active excerpt samples, including exact zeros. It does not
advance before or after the overlay window. Fixed state and a 64-sample gain
workspace bound processing storage; no gain array grows with file length.

For the analytic source `z = I + jQ`, offset angle `theta`, and complex fading
gain `h`, the added signal is `scale * Re(h * z * exp(j*theta))`. This changes
phase as well as amplitude. Offset and fading use the same Hilbert/NCO path,
so their combined filter delay is 215 frames. The unfaded zero-offset path
continues to bypass that filter and draws no random values.

Calibration still uses the raw selected WAV excerpt RMS. Unit ensemble fading
power preserves the commanded S/I in expectation; the stage never rescales a
finite faded record to force that record's power to the command. The source
sidecar adds the spread, unit mean power, family 13, fading algorithm version 1
and `det-pcg32` implementation only when fading is enabled. The raw source
identity remains independent of the fade; retain the scenario/seed alongside
it to identify the processed condition.

## Markov ON/OFF gating

An optional `markov_on_off` object supplies finite `mean_on_seconds` and
`mean_off_seconds`, each from one sample period through 1,000,000 seconds.
Unknown, duplicate or missing members are refused. The
[gated example](../../fixtures/scenarios/second-source-markov.json) combines
it with an independent fade and offset.

The reusable [Markov gate](markov-cw.md) starts ON at the first destination
window sample and draws geometric integer dwells using family 10. It advances
once per active sample, after that sample observes the current state; samples
before and after the window do not advance it. Explicit `stream_id` values
stabilize its key independently of stage position. Its family is separate from
the secondary fading stream, so enabling a gate does not change fade draws.

The gate multiplies the secondary contribution **after** the offset/fading
filter. Thus OFF samples add exactly zero at their scenario input-frame
positions, including when the analytic filter is enabled. Its 215-frame signal
latency is unchanged; the gate itself is not delayed. The WAV source, shifter
and fading process continue through OFF intervals, so the next ON interval
resumes the continuously evolving filtered source. There is no gate ramp or
filter-tail leakage into OFF samples. Subsequent scenario stages still process
the combined signal normally; a final sample slip changes its emitted indices.

S/I continues to set the ungated component's scale, with unit ensemble fading
power when enabled. OFF samples lower the whole-window interference power;
neither gate duty cycle nor a finite fading realization is renormalized. Stage
component energy includes the gated values and its frame count includes the
entire active window. The optional sidecar object records both dwell means,
family 10, algorithm version 1, initial ON and the `destination-window` timeline.
Retain the scenario and seed with the raw source identity for exact replay.

## RT1170 SD provider

The player accepts a single ASCII WAV basename in `source_path`, resolved under `2:/WG/`. Long ASCII filenames up to 255 bytes are supported by the configured FatFs long-filename mode. Separators, drive prefixes, traversal, control/non-ASCII bytes, FAT-forbidden characters and trailing dots/spaces are refused. The extension is `.WAV`, case-insensitively. The target requires mono 48 kHz PCM24; the host's legacy PCM16 support does not change that target format.

The player task owns a separate file handle and a fixed packed read buffer in OCRAM2. Each measurement or engine read transfers at most 2,048 frames. The prepass reuses existing float scratch and includes every sample in the selected excerpt, including zeros. Neither an entire WAV nor a file object is placed on the task stack. The primary and secondary files stay open through playback, with serialized FatFs access in the player task. Audio callbacks do not read files. The engine is detached before the provider closes, including on failures and playlist transitions.

Playlist admission checks the second WAV's header, format and excerpt bounds without scanning the whole excerpt or claiming a measured identity. Other stages still undergo engine configuration checks. After PLAYLIST is acknowledged, each actual item reopens and measures its source; STOP is checked between bounded prepass reads. This keeps long calibration out of the synchronous command reply. A silent source or a calibration/configuration failure at item start produces an impairment error and follows the playlist's stop/CONTINUE policy before any samples are output. Plain PLAY retains the established scenario-refusal behavior: it reports the refusal and plays clean. A secondary read failure during processing stops either kind of play.

`ENGINE?` and retained playlist results carry `second_source_*` fields for the measured algorithm version, full source frame count, selected interval, raw RMS, PCM24 digest and analytic-filter latency. The identity is copied only after successful configuration, survives completion, and cannot be overwritten by preflight. A new clean or refused play has a zero identity. No file digest is revalidated during output; source stability relies on the player's exclusive media ownership. The existing primary SD counters describe primary reads; the producer and engine cycle measurements include secondary reads performed in the processing path.

Mocked FatFs/player tests establish software behavior, not sustained SD throughput, DMA correctness or zero underruns on the board. The one-hour two-source acceptance, host/target PCM comparison, runtime stack/deadline measurements and decoder characterization remain outstanding.
