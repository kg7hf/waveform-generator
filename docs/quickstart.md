# Quick start: render and play a WAV

The default host build can make a calibration tone, add interference and play
the result on an explicitly selected Windows output. No encoder plugin or board
is needed for this first run. Run commands from the repository root in
PowerShell.

## Build

Install Git, Python 3.10 or newer, CMake 3.27 or newer, and the C++23 MinGW
toolchain. Then:

```powershell
.\scripts\init-dependencies.ps1
cmake --preset host-release
cmake --build --preset host-release --parallel
```

## Generate the example WAVs

```powershell
.\scripts\demo-waveform.ps1 -OutputDirectory build/quickstart
```

The directory must be new. It contains a clean ten-second tone, a CW mixture,
and a fade/noise/CW mixture; the mixed WAVs have JSON reports. To use a source
WAV instead, see [How to use](how-to-use.md).

## Listen on Windows

```powershell
python -m pip install -r host/requirements-playback.txt
python host/play_wav.py --list-devices
python host/play_wav.py build/quickstart/cw.wav `
  --device "REPLACE_WITH_EXACT_OUTPUT_DEVICE_NAME" --gain-db -12
```

Use the exact output name printed on your PC. The helper uses Python/PortAudio
to play the rendered file. Native Windows live output and a graphical effects
studio are future work.

## Continue

- For a downloaded source WAV, recipe edits, and more examples, see
  [How to use](how-to-use.md).
- For either board's serial-bound card staging and USB playback, see the
  [Windows USB bench guide](windows-usb-bench.md). Board-specific setup is in
  the [RT1170 guide](modules/rt1170-player-and-audio.md) and
  [RT1064 guide](modules/rt1064-board.md). [Testing](testing.md) summarizes
  acceptance boundaries and retained evidence.
- To add an encoder, see the [plugin reference](encoder-plugins.md) and
  [module tutorial](custom-encoder-tutorial.md).
