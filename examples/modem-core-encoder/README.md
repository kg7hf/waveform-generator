# M110 encoder adapter for public modem-core

This optional source plugin supplies the `M110` encoder to waveform-generator.
It uses the transmitter from [kg7hf/modem-core](https://github.com/kg7hf/modem-core)
and the existing waveform-generator PCM24 WAV writer. `modem-core` is a
separate checkout; its source stays in that repository. CMake does not download
it. The ordinary player and the `TONE` example do not depend on it.

## Build and use

```powershell
git clone https://github.com/kg7hf/modem-core.git build/public-modem-core
cmake --preset host-release -B build/host-modem-core `
  -DWFG_ENABLE_ENCODER_PLUGINS=ON `
  -DWFG_ENCODER_PLUGIN_DIR=examples/modem-core-encoder `
  -DWFG_MODEM_CORE_DIR=build/public-modem-core
cmake --build build/host-modem-core --target wfg_modem_core_encoder_tests
ctest --test-dir build/host-modem-core -R 'wfg_modem_core_encoder_tests' --output-on-failure
```

The plugin registers `M110`. Its profile is a bit rate followed by the
interleave code: `S` for short, `L` for long, or `4800Z` for 4800 bit/s with
zero interleave. Valid rates with `S` and `L` are `75`, `150`, `300`, `600`,
`1200`, and `2400`. For example, after staging `PAYLOAD.BIN` on a writable
player's card and handing card ownership back to firmware:

```text
INFO?
ENCODE M110 2400S PAYLOAD.BIN BURST.WAV
STATUS?
LOAD:BURST.WAV
PLAY
```

Wait for `STATUS?` to report generation complete before loading the WAV. The
shared writer produces mono 48 kHz packed PCM24 with its normal tail silence.
The resulting WAV can use the same playback and CW/noise effects as any other
compatible file. No legacy `GENERATE:rate:interleave` command is registered by
this adapter.

## Implementation and verification

The adapter keeps at most two modem symbol blocks and the bounded scratch
required by `modem-core`, inside waveform-generator's 128 KiB encoder
workspace. It reads payload octets sequentially, uses `modem-core` to frame the
last bits and to encode each block, and renders the audio with its stateful
windowed modulator. It never converts through PCM16 on the production path.

The host test compares every float sample bit for bit with `modem-core`'s
whole-burst transmitter across all 13 supported modes, checks PCM24 WAV output,
and checks invalid profiles and early payload EOF. The host test is evidence
for this checkout and workload, not hardware performance or analog behavior.

Both RT1064 and RT1170 release firmware builds compile and link with this
adapter, and both post-link stack checks pass. A physical board run of the
M110 plugin has not yet been performed, so target playback timing and analog
output remain unverified. The ordinary player builds are unaffected.
