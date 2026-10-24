# RT1064-EVK player and codec port

## Scope and current status

The MIMXRT1064-EVK is a second physical waveform fixture alongside the RT1170.
It builds the same SD player, effects engine, WFG-LIVE/1 controls, USB audio
capture device and WM8960 codec transport. Windows virtual audio is on hold.
The normal build consumes user-supplied mono 48 kHz PCM24 WAV files. Optional
[encoder plugins](../encoder-plugins.md) remain a build-time extension.

The writable player, read-only player and codec tone images have build presets.
USB playback and live CW are supported. Recheck continuity on the firmware,
board and PC used for each campaign; historical bench results do not qualify
a new combination. See [testing](../testing.md#physical-usb-playback-and-cw).

Check actual USB bit depth with `USB?`; a float32 shared-mode format alone does
not establish it. Stress under host load, all-effects DSP capacity, right-channel
capture integrity and analog codec operation remain unverified. No modem
conformance claim is made. The removable SD card is the supported storage path;
no eMMC driver or storage claim is included.

## 50,000-foot view

```text
SD WAV -> shared player/effects -> shared output FIFO -> USB UAC2 -> PC capture
                                                   -> SAI1/eDMA -> WM8960
USB CDC -> WFG-LIVE/1 -> shared player owner task
```

Both boards use the same protocol and processing code. The RT1064 selects its
own startup, flash layout, pin assignments, clock roots and unique chip ID.
Its USB product is `RT1064 Waveform Player` (with `RO` for read-only); the
audio function is `RT1064 Player`. INFO reports `MIMXRT1064-EVK`.
A board's serial, CDC port and capture endpoint must be associated explicitly.
A PC default audio device is not a fixture selection.

## How to build and use it

Initialize the existing dependency submodules as described in the README.
The NXP SDK submodule already supplies RT1064 device and board support.

```powershell
cmake --preset rt1064-player-release
cmake --build --preset rt1064-player-release --parallel

# Initial inspection without filesystem writes:
cmake --preset rt1064-player-readonly-release
cmake --build --preset rt1064-player-readonly-release --parallel
```

The ELF is in `build/<preset>/waveform_generator.elf`. Debug and tone variants
also exist (`rt1064-player-debug`, `rt1064-tone-release`, and their counterparts).
Select **MIMXRT1064:EVK-MIMXRT1064** and the actual debug probe serial in
LinkServer. Do not reuse the RT1170 image or probe selection. Verify board
power before programming. The debugger's mounted drive is not the SD card.
Inspect the programming log and verify the image before use. During bring-up,
one ELF load reported success but failed read-back. Erasing and loading a
single binary with execution held, then verifying against the ELF, passed;
do not treat the loader exit code alone as verification.

Connect the target **J9 USB OTG1** device connector as well as the debug cable.
After enumeration, use the fixture's actual CDC port and USB serial below;
`COMxx` and `FIXTURE_SERIAL` are placeholders, not remembered defaults.

```powershell
python host/live_control.py --port COMxx --expected-serial FIXTURE_SERIAL `
  --expected-board MIMXRT1064-EVK --record build/rt1064-info.jsonl `
  command 'INFO?' 'STATUS?'
```

For the writable player, stage `/WG/PLAY.WAV` using the existing
[staging workflow](../how-to-use.md). Flush and dismount the host SD volume,
then transfer ownership to the local player. Start a capture client on the
RT1064 audio endpoint before USB playback. In a new journal:

```powershell
python host/live_control.py --port COMxx --expected-serial FIXTURE_SERIAL `
  --expected-board MIMXRT1064-EVK --host-volume-dismounted `
  --record build/rt1064-play.jsonl command `
  'MEDIA LOCAL' 'LOAD:PLAY.WAV' 'OUTPUT USB' 'CW FREQ 900' 'CW ON' 'PLAY'
```

Use the exact CW grammar in [the live protocol guide](../live-control-protocol.md)
for levels and scheduled events. `OUTPUT CODEC` selects the analog route while
stopped. Keep analog levels conservative and verify wiring before connecting
a modem. The codec uses 24 valid sample bits in 32-bit stereo I2S slots at
48 kHz. The first USB capture alternate carries 24 valid bits in 32-bit stereo slots at
48 kHz;
the mono source feeds the shared channel mapping. Codec input is configured
for line-level left INPUT3 with the right input closed, as in the shared
transport; an electret microphone bias path is not enabled.

Before a 24-bit capture, the owner selects **2 channel, 24 bit, 48000 Hz**
in the Windows Recording endpoint's Properties / Advanced tab, then closes
Sound settings. During capture, `USB?` must report `uac_stream_alt: 1`,
`uac_stream_bits: 24` and `uac_stream_channels: 2`. A float32 WASAPI mix format
alone does not prove the USB stream has 24-bit precision. See the shared
[capture-format workflow](protocols-media-and-artifacts.md#capture-formats).

The shared storage inquiry string currently still says `RT1170 SD READ`
(or `RT1170 SD LOADER`), which keeps the existing media helper compatible.
Identify the physical fixture through its USB serial and CDC INFO board name,
not that storage product string, particularly with both boards connected.

A read-only image cannot stage files or write prepared artifacts. Supply a
card already containing the vector and follow its advertised capabilities.
Use `--expected-board MIMXRT1064-EVK` with the optional DD008 upload helper or
PREPAREPLAY journal summarizer as well.

## Moving the fixture to another Windows PC

Connect power/debug through J41 and target data through **J9 USB OTG1**.
Both accept Micro-B plugs. A USB-C-only PC can use USB-C-to-Micro-B data
cables or USB-C-to-USB-A adapters with the existing data cables. Retain the
programmed image and SD card; moving PCs does not require reflashing or formatting.

Run the host controller and capture client natively on the destination PC.
SSH can launch them and retrieve results; the USB samples stay on that PC.
Keep the hardware bench separate from Linux build/CI containers. Discover the
new COM port and physical recording endpoint and match them to the board's
serial. Do not carry over the original PC's COM number, drive letter, endpoint
ID or recording format. With Remote Desktop, select the physical RT1064
recording device explicitly rather than Remote Audio.

The owner selects 24-bit stereo at 48 kHz on the new PC. Confirm the negotiated
USB format while capture is open, then repeat the short clean and live-CW
checks below. Software installation and endpoint enumeration alone do not
establish playback continuity. Codec testing is a separate later step.

## Unattended staging without elevation

Use the serial-bound [Windows bench procedure](../windows-usb-bench.md).
Start with ordinary PowerShell. The helper reports process elevation separately
from volume access rights and refuses failed lock/dismount/eject operations.
Permissions can differ between PCs; a failed handoff does not authorize
`MEDIA LOCAL`. No reset or forced filesystem ownership is part of staging.

## Reproduce the short WAV and CW test

Generate the standard ten-second 1 kHz source on the PC:

```powershell
python host/stage_wav.py make-tone --output build/PLAY.WAV
```

Use a new output path if that file already exists. Stage it as `/WG/PLAY.WAV`
with the writable image and the [Windows bench staging procedure](../windows-usb-bench.md).
The procedure verifies the copy, dismounts/ejects only the storage medium and
then sends `MEDIA LOCAL`. It does not format cards. Open the selected RT1064
capture endpoint, let it settle, and verify 24-bit stereo with `USB?`.

In a WFG-LIVE/1 interactive session, clean playback is:

```text
OUTPUT USB
LOAD:PLAY.WAV
PLAY
STATUS?
USB?
```

Wait for `player_state: 7` (done), then use a fresh capture for the CW run:

```text
LOAD:PLAY.WAV
SEED:42
REFERENCE:0.088388343346
CW FREQ 900
CW CI 6
AT:96000 CW ON
AT:336000 CW OFF
PLAY
STATUS?
USB?
```

The reference RMS above belongs to the standard tone only; use the measured
source RMS for another file. `LOAD` clears the previous live controls. CW is
scheduled in output samples, so the on/off positions do not depend on CDC
arrival timing. Both captures should contain ten seconds of source audio;
the second has CW only from 2 to 7 seconds. See the shared
[bit-exact capture check](../usb-sound-card-bringup.md#7-play-and-check-bit-exact)
for clean-source comparison and per-play counters. For the CW capture,
compare against an independently rendered mixture from the acknowledged live
journal, as described in [testing](../testing.md#physical-usb-playback-and-cw);
the unmodified source alone is not the expected CW output. Close the capture
only after playback has finished.

## Maintainer view

- `rt1064/board_pins.c`: LPUART1 debug console, status LED and USDHC1 SD pins.
- `rt1064/codec_board.hpp`: SAI1 pins, LPI2C1 and audio PLL setup. The codec
  is clock master. PLL 786.432 MHz / 4 / 16 supplies 12.288 MHz MCLK;
  codec-control I2C uses a 10 MHz source.
- `cmake/rt1064_config.cmake`: SDK components and device selection.
- `cmake/MIMXRT1064xxxxx_flexspi_nor_wfg.ld`: 4 MiB in-package flash at
  0x70000000; 128 KiB ITCM; 128 KiB DTCM reserved for uncached DMA objects;
  256 KiB ordinary OCRAM; 480 KiB large player state; final 32 KiB OCRAM
  reserved as the MPU noncacheable MSC window. No external SDRAM is required.
  `.ncache` is NOLOAD: startup clears it, and the ELF must not ask a flash
  programmer to load zero-filled bytes at the DTCM address.
- `rt1170/CMakeLists.txt`: shared firmware source list selected by WFG_TARGET.
  The directory name is historical; it does not imply RT1170 register access.
- `rt1170/platform/device_identity.h`: board strings and fuse-word mapping.
- `rt1170/platform/waveform_sd.c`: RT1064 active-high SD power on GPIO1_19,
  active-low detect on GPIO2_28 and the 198 MHz USDHC1 root. The card remains
  at 3.3 V with a 50 MHz bus ceiling.
- `rt1170/usb/tusb_config.h`: RT1064 TinyUSB endpoint/controller objects in
  uncached DTCM. Codec transfer descriptors are also uncached: a handle
  contains a DMA-fetched TCD ring, not just CPU bookkeeping.

The player, effects and codec task keep their bounded queues and static
storage. ISR paths must not allocate, access the filesystem or wait on host
control. Automatic linker placement and stack checks run after the player
link. Host tests exercise shared behavior; cross-builds check target code;
physical captures establish timing and continuity. These are separate checks.

## Explain it simply

The SD card holds a recording. The board reads ahead into a bucket of samples.
The USB connection or codec empties that bucket at a steady rate. Effects
change the samples before they reach the bucket. The second board follows the
same recipe, with different pins and memory addresses.

## Small test and acceptance sequence

1. Confirm power, probe identity, boot and a responsive task loop through debug.
2. Read the existing card with the read-only image; preserve its files.
3. Connect target USB; verify INFO identity, MSC ownership and PCM24 capture.
4. Play a short known tone, then a supplied WAV; compare captured samples and
   receiver continuity with the source and board counters.
5. Add CW, test stop/restart, then sustained playback under host load.
6. Test codec output and input separately with a known wired signal and levels.
7. Swap each board between PCs using the same vectors and scenarios.

A USB bus bandwidth calculation alone does not establish live DSP headroom.
Measure expensive effects on this 600 MHz target and use prepared files when
necessary. A successful RT1170 run does not accept an RT1064 run.

## Board references

[NXP RT1064-EVK](https://www.nxp.com/design/design-center/development-boards-and-designs/MIMXRT1064-EVK)
and the SDK's `boards/evkmimxrt1064` supply the board configuration. Pin and
clock choices follow NXP's [SAI example](https://github.com/nxp-mcuxpresso/mcuxsdk-examples/tree/main/_boards/evkmimxrt1064/driver_examples/sai/edma_transfer)
and [SD example](https://github.com/nxp-mcuxpresso/mcuxsdk-examples/tree/main/_boards/evkmimxrt1064/sdmmc_examples/sdcard_fatfs).
