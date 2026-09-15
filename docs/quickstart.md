# Quick start: a WAV with interference

The default build needs no encoder plugin. It accepts mono **48 kHz PCM24** WAVs,
adds effects, and plays them through the existing PC helper or RT1170 file player.
Run these commands from the repository root in PowerShell.

## Build

Install Git, Python 3.10+, CMake 3.27+ and the C++23 MinGW toolchain, then:

```powershell
.\scripts\init-dependencies.ps1
cmake --preset host-release -DWFG_ENABLE_ENCODER_PLUGINS=OFF
cmake --build --preset host-release --parallel
```

## Generate a demo or supply your own WAV

```powershell
# Creates a ten-second tone, a CW mixture and a fade/noise/CW mixture:
.\scripts\demo-waveform.ps1 -OutputDirectory build/quickstart

# Alternative: use an existing compatible WAV and a different output directory:
.\scripts\demo-waveform.ps1 -SourceWav 'C:\Audio\source.wav' `
  -OutputDirectory build/my-audio
```

The output directory must be new. `clean.wav`, `cw.wav` and `field.wav` are PCM24;
the mixed files have JSON reports. Edit copies of the recipes under `examples/`
to change CW frequency/strength, noise, fades or impulses. The
[how-to guide](how-to-use.md) includes the renderer command and downloaded-WAV demo.

## Listen on the PC

```powershell
python -m pip install -r host/requirements-playback.txt
python host/play_wav.py --list-devices
python host/play_wav.py build/quickstart/cw.wav `
  --device "Headphones (Realtek(R) Audio)" --gain-db -12
```

Use the exact device name printed on your PC. This uses Python/PortAudio to play
a rendered file; native Windows live output and a graphical effects studio remain
future work. The RT1170 already supports live effects over CDC.

## Use RT1170

Build and load the `rt1170-player-release` firmware. Stage a compatible WAV with
`host/stage_wav.py`, flush/dismount the verified Windows card volume, and hand
ownership to firmware before `LOAD:PLAY.WAV` and `PLAY`. The
[README board workflow](../README.md#5-play-through-rt1170-and-add-live-interference)
has complete commands and live CW examples. Playing a pre-mixed WAV does not
require an encoder module. Remove an old on-card scenario if it should not add
another effects pass.

## Add your own waveform producer

Start with the [module tutorial](custom-encoder-tutorial.md). It covers the
working `TONE` example, separate repositories/submodules, build flags, custom
profiles, and adding or extending CDC commands. The
[plugin reference](encoder-plugins.md) defines the interface and resource bounds.
