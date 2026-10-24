# Stereo artifacts

## 50,000-foot view

`host/compose_stereo_wav.py` prepares offline stereo artifacts from two
independently rendered mono channels. It interleaves their PCM24 bytes without
changing gain, duration, sample rate or timing. A combined sidecar retains each
render's seed, scenario identity, output digest and channel-specific CRC.

The SD player and its secondary-source provider still require mono WAV input.
Independent firmware engines, a stereo FIFO, channel routing, playlist identity,
mono-alternate refusal and physical acceptance remain separate work.

## How to use it

Render two channels with `signal_lab_render`, selecting each scenario and seed
independently. Both outputs must be nonempty, equally long, canonical PCM24 mono
WAVs at 48 kHz, with C++ renderer sidecars (`signal-lab.render/1`). An odd data
length requires the renderer's zero RIFF pad byte. Then compose them:

```powershell
python host/compose_stereo_wav.py compose `
  --left-wav left.wav --left-sidecar left.json --left-scenario left-scenario.json `
  --right-wav right.wav --right-sidecar right.json --right-scenario right-scenario.json `
  --output-wav stereo.wav --output-sidecar stereo.json
```

Output paths must be new and their directories must already exist. The tool
rejects existing outputs and input aliases; failure removes only artifacts it
created. It refuses unequal channel lengths, including unequal lengths caused
by sample-slip scenarios. It never trims, pads, resamples or aligns channels.

For an already aligned, separately retained stereo capture file:

```powershell
python host/compose_stereo_wav.py verify `
  --expected-wav stereo.wav --capture-wav capture.wav
```

Verification accepts exact PCM24 or exact top-16-bit truncation to PCM16.
The capture must use the canonical 44-byte integer-PCM WAV header and contain
exactly the expected frames. Other RIFF layouts, including extensible WAVs,
need a separate conversion that preserves the PCM bytes and provenance.
This command opens files only; it does not select or open an audio endpoint.
It supplies no marker search, gain compensation, clock correction or tolerance.

## Scientist and maintainer view

Each frame is six bytes: three little-endian signed PCM24 bytes from the left
render followed by three from the right. The project convention labels these
channel 0 / upper and channel 1 / lower. That naming does not establish RF
sideband mapping or a modem's channel contract.

Before composition, both frame counts and FNV-1a64 digests are checked against
the renderer sidecars. FNV covers packed PCM24 data only; RIFF padding is excluded.
Composition rechecks those digests and input hashes, uses bounded 8,192-frame
blocks and refuses stereo output beyond RIFF's 32-bit size limit. The JSON
reader rejects duplicate keys and nonfinite numbers.

`waveform-generator.stereo-compose/1` retains the original renderer metadata,
sidecar SHA-256, current scenario SHA-256, source WAV SHA-256 and PCM SHA-256,
plus output file/PCM SHA-256 and separate channel CRC32 values. The renderer's
older sidecar does not bind the original scenario bytes with a hash. Therefore
the scenario record explicitly says `current_file_hash_unverified_against_render`;
it is not evidence that those bytes produced the earlier render.

The CRC convention matches the existing USB statistics: ordinary zlib CRC32
over each channel's packed three-byte samples, in frame order. `verify` returns
these full-PCM24 expected CRCs even for PCM16 capture. It separately reports
capture-native CRCs and their encoding; a PCM16 CRC cannot establish the missing
low eight bits. Byte-exact equality establishes artifact identity, not that
independent seeds produce independent stochastic processes.

`common/wav.hpp` also offers a distinct
`validate_pcm24_stereo_48k_wav(reader, payload)` entry point. It shares the
bounded header parser with the mono validators, sets `bytes_per_frame` to six,
and does not read sample payloads. Existing mono APIs still reject stereo.
The parser checks RIFF bounds, chunk padding, duplicate format/data chunks,
PCM format, rate, byte rate, block alignment and whole frames. The composer
deliberately accepts the narrower canonical renderer layout.

## 5th-grader view

Imagine two numbered stacks of cards. Take card 1 from the left stack, then
card 1 from the right, and repeat. Nothing written on either card changes.
If one stack is shorter, stop with an error instead of inventing extra cards.
The checksums let someone confirm later that the cards stayed in the same order.

## Toy worked example

Left samples `[1, -1]` and right samples `[2, -2]` become the two frames
`01 00 00 | 02 00 00` and `ff ff ff | fe ff ff`. Sending the same mono render
as both inputs produces identical channels by construction; choosing different
scenarios or seeds preserves their distinct rendered samples instead.
