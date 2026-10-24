# Protocols, Media Ownership, and Artifacts

## 1. Scope

This guide covers the implementation of CDC response framing, DD008
payload transfer, the player's composite USB device (identity, interface and
endpoint numbering, and the USB Audio Class 2 capture function that makes the
board a USB sound card), USB MSC exposure, exclusive SD ownership, and WAV
artifact generation on RT1170 and RT1064. The
[live protocol reference](../live-control-protocol.md) owns command grammar
and response schemas; this guide owns transport, descriptors and media lifetime.
RT1170 endpoint names below are examples; use the RT1064 identity and endpoint
when operating that board, as described in the [Windows bench](../windows-usb-bench.md).

The player side of the sound card (the `OUTPUT USB` route, the USB ISR that
feeds the stream, and the per-play statistics) belongs to
[RT1170 Player and Audio](rt1170-player-and-audio.md#usb-route). The bench
procedure (flashing, staging, identity and the bit-exact check) is the
[USB sound card bring-up](../usb-sound-card-bringup.md) runbook.

## 2. 50,000-foot view

The composite player exposes three functions on one board:

- CDC carries bounded commands, acknowledgments, status, and DD008 upload data.
- MSC lets Windows access the microSD card as a removable volume.
- A USB Audio Class 2 capture device ("RT1170 Player") carries a 48 kHz,
  24-bit stereo stream to the PC. Windows lists it as the recording endpoint
  "Line (RT1170 Player)".

```text
               +-- CDC, interfaces 0/1  <-> WFG-LIVE/1, legacy lines, DD008 upload
PC USB host <--+-- MSC, interface 2     <-> the microSD card, only while MEDIA HOST
               +-- UAC2, interfaces 3/4 <-- iso IN 0x84: exact silence, or the
                                            file of a PLAY on the OUTPUT USB route
```

Windows MSC and firmware FatFs must never own the card at the same time. The
protocol is therefore partly an ownership state machine, not just a list of
commands.

The read-only [`FILES?`](../live-control-protocol.md#card-listing) query lists
regular files in `2:/WG/` while the card remains Local. It uses the player
task's exclusive FatFs ownership and refuses during playback, a playlist,
upload or generation. Host ownership is refused before any FatFs call. Pages
contain up to 24 UTF-8 names and exact byte sizes in stable directory
order. Names prefer 8.3 aliases, with a filename fallback of at most 36 UTF-8
bytes when FatFs supplies no alias (notably on exFAT); no mount or directory handle survives a successful reply. An unmount
failure retains ownership until a later idle Local query completes cleanup.
Both player identities advertise the `files` capability without changing
their `kind` or `board`. Hardware parity and listing latency remain unmeasured.

The common WAV parser has separate mono and stereo PCM24 entry points. The
current player and secondary SD source continue to use the mono entry point.
[Stereo artifact preparation](stereo-artifacts.md) explains the distinct stereo
validator, offline composer and channel-specific hashes/CRCs; it does not change
the board's current mono playback admission.

## 3. How to use it

Use the normal player PID `0x4012` for host staging and file playback. Local
generation additionally requires an enabled encoder plugin. Use the
read-only player PID `0x4013` when the card must remain write-protected. Verify
USB serial in both cases.

### USB identity

| PID | Image (CMake presets) | Product string | `bcdDevice` | Functions |
|---|---|---|---|---|
| `0x4011` | scaffold (`rt1170-debug`, `rt1170-release`), tone (`rt1170-tone-*`) | `RT1170 Waveform Fixture` | `0x0100` | CDC only (interfaces 0/1) |
| `0x4012` | writable player (`rt1170-player-*`) | `RT1170 Waveform Player` | `0x0102` (`0x0103` with `WFG_UAC_MONO_ALT`) | CDC + MSC + UAC2 capture |
| `0x4013` | read-only player (`rt1170-player-readonly-*`) | `RT1170 Waveform Player RO` | `0x0102` (`0x0103` with `WFG_UAC_MONO_ALT`) | CDC + MSC (writes refused) + UAC2 capture |

Every image uses VID `0xCAFE`, TinyUSB's development VID: fine for this
hardware, while a project-owned VID/PID is a productization gate. The
manufacturer string is `Waveform Generator`, the device class is
Miscellaneous / Common / Interface Association (`0xEF`/`0x02`/`0x01`), USB
2.0 (`bcdUSB` `0x0200`), endpoint 0 takes 64 bytes, and there is one
configuration (bus-powered, 100 mA). The serial number is 16 uppercase hex
digits read from two OCOTP fuse words: the same in every image and on every
enumeration, and equal to INFO `usb_serial`. Find the board by VID, PID and
serial, never by a remembered COM number or drive letter. The SD diagnostic
images (`rt1170-sdcard-*`) do not enumerate on USB. No image uses PID
`0x4014`: a separately enumerating sound-card personality was planned and
dropped, and the sound card is part of the player's PIDs.

`bcdDevice` moved from `0x0100` to `0x0101` when the UAC2 function joined CDC
and MSC, and to `0x0102` when the streaming interface gained its 16-bit stereo
alternate: each time a new hardware ID, so Windows reads the changed
descriptor set instead of a cached one. An image built with the CMake option
`WFG_UAC_MONO_ALT` (default off; see [capture formats](#capture-formats)) also
offers a mono alternate and reports `0x0103`, so the two descriptor sets never
share an ID. Move it again with any later change to the player's
descriptors. INFO reports the PID as `usb_pid` in decimal (16402 for `0x4012`,
16403 for `0x4013`), adds `"read_only":true` on the read-only image, and lists
`usb-audio` and `usb-diagnostics` (the `USB?` query) in `capabilities`, and
`usb-audio-mono` as well on a `WFG_UAC_MONO_ALT` image, whose PID and commit
are the standard image's. Likewise `sd-clock-24mhz` on an
image built with `WFG_SD_CLOCK_24MHZ` (see
[MSC transfers](#msc-transfers-and-throughput)).

### Composite layout

Both player images are always the same composite device (constants in
`rt1170/usb/usb_composite.h`):

| Interface | Function | Endpoints (full / high speed packet size) | String |
|---|---|---|---|
| 0 | CDC ACM control; an IAD groups interfaces 0 and 1 | `0x81` interrupt IN, notifications, 16 bytes | `Waveform Fixture CDC` |
| 1 | CDC data | `0x02` bulk OUT and `0x82` bulk IN, 64 / 512 bytes | |
| 2 | MSC bulk-only transport, one LUN | `0x03` bulk OUT and `0x83` bulk IN, 64 / 512 bytes | `Waveform microSD LUN` (`Waveform microSD read-only` on `0x4013`) |
| 3 | Audio control; an IAD groups interfaces 3 and 4, `iFunction` `RT1170 Player` | none (no interrupt endpoint) | `RT1170 Player` |
| 4 | Audio streaming: alternate 0 has no endpoint; alternates 1, 2 (and 3 with `WFG_UAC_MONO_ALT`) are the stream's [capture formats](#capture-formats) | `0x84` isochronous IN, synchronous, one packet per 1 ms: 384 bytes (alternate 1), 192 (2), 96 (3) | |

The fixture images (`0x4011`) present only interfaces 0/1 with the same CDC
endpoints. The MSC LUN answers SCSI INQUIRY with vendor `WFG`, product
`RT1170 SD LOADER` (`RT1170 SD READ` on the read-only image) and revision
`0001`; Windows names the disk after them. Every RT1170 image is built for
the high-speed controller, so the device also answers the device-qualifier
and other-speed requests; a full-speed host gets the full-speed
configuration, which differs only in the bulk packet sizes and the
isochronous `bInterval`.

### The recording device on Windows

Every player image, including one that only ever plays through the codec,
therefore shows Windows a recording device, "Line (RT1170 Player)". When it
first appears, Windows may make it the default recording device. Lane clients
that open the default capture device (WinMM `WAVE_MAPPER`, the default WASAPI
endpoint) would then record the board's digital silence instead of the sound
card that carries the codec output. After flashing a player image on a PC:

1. Open the Windows Sound control panel, Recording tab, and check that the
   default recording device is still the sound card the lane captures from;
   set it back if it moved.
2. Check that every lane client selects its capture device by name, not as the
   default. Hand this note to the lane with the image.

The feature unit offers mute and volume, so Windows sends its level slider
and mute to the device as feature-unit requests; the device stores and
reports them and never applies them to a sample (see
[USB Audio Class 2 function](#usb-audio-class-2-function)). That Windows adds
no software gain of its own on top is a bench check: a play's CRCs must not
change with the slider.

### Capture formats

The endpoint offers its formats as alternate settings of streaming
interface 4, in this order (alternate 0 has no endpoint):

| Alternate | Channels | Subslot, bits | Packet (`wMaxPacketSize`) | Samples on the wire | Advanced tab (expected; Windows adds a quality label) |
|---|---|---|---|---|---|
| 1 (first) | 2, front left + right | 4 bytes, 24 bits | 384 bytes | `sample << 8`, the 24-bit sample exactly | 2 channel, 24 bit, 48000 Hz |
| 2 | 2, front left + right | 2 bytes, 16 bits | 192 bytes | `sample >> 8`: the top 16 bits, truncated | 2 channel, 16 bit, 48000 Hz |
| 3, only with `WFG_UAC_MONO_ALT` | 1, front centre (Windows' mono): the left channel | 2 bytes, 16 bits | 96 bytes | the left channel's `sample >> 8` | 1 channel, 16 bit, 48000 Hz |

Every format is 48 kHz with 48 frames in every 1 ms packet on the same
synchronous endpoint `0x84` (same `bInterval`), and exact zeros while no
play feeds the stream. The 16-bit formats are **truncations**: each 16-bit
sample is the 24-bit sample shifted right by 8 bits (arithmetic shift,
toward minus infinity), with no rounding and no dither, so a capture's
16-bit value is exactly `card24 >> 8` (the lane verifier's
`--expect-truncation-bits 8`). For example -1, -256 and -2 become -1;
-257 becomes -2; 255 becomes 0 and 256 becomes 1; the 24-bit extremes
-8388608 and 8388607 become -32768 and 32767. A 24-bit capture of alternate 1
stays the only bit-exact one.

The per-play CRCs do not depend on the format. `uac_play_crc32_l` and
`uac_play_crc32_r` are computed over the 24-bit source samples the device took
from the player for each channel, as 3-byte little-endian, before they are
converted for the wire, and over exactly the frames confirmed sent. For the
lane's mono PCM24 cards (L = R) both stay `zlib.crc32(<WAV data chunk>)` in
24-bit stereo, 16-bit stereo and 16-bit mono alike, the dropped right channel
included. A capture in a 16-bit format is checked against the card shifted
right by 8, not against these CRCs.

So STATUS alone cannot tell a truncated play from a bit-exact one: STATUS has
no room for a format field, and a 16-bit or mono play reports the same
`uac_play_*` frames, CRCs and missed intervals a 24-bit play of the card would. **Lane rule: a
bit-exact verdict needs `USB?` after the play, with `uac_play_stream_bits` 24
and `uac_play_stream_channels` 2**, besides the STATUS checks; any other
reading makes the play a truncated (or mono) one, whatever its CRCs say. The
[bring-up runbook](../usb-sound-card-bringup.md#7-play-and-check-bit-exact)
lists the check.

Which format is used. Windows' audio engine opens the endpoint in its
*default format* for every shared-mode client; an exclusive-mode client opens
the format it asks for. The inbox `usbaudio2` driver selects the alternate
whose format matches.

- Default format. No Microsoft documentation found says how Windows picks the
  initial default format of a USB Audio 2.0 endpoint with several formats
  when no INF supplies one (the in-box driver has no partner customization,
  and this device brings no INF of its own). Alternate 1 is listed first
  so that, if Windows takes the first or the widest format, 24-bit stereo
  stays the default. This is a bench check after the first flash of a
  `0x0102` image: the endpoint's Advanced tab must read "2 channel, 24 bit,
  48000 Hz". If Windows chose a 16-bit default, the owner sets it back
  (below); the lane's bit-exact 24-bit captures depend on it. Windows keeps
  the setting per endpoint; whether a new `bcdDevice` resets it is part of
  the same check, so read the Advanced tab after every flash that moves it.
- Selecting 16 bits. Only the owner changes Windows audio settings; agents and
  the lane never do. Windows Sound settings: the Recording tab of the classic
  Sound control panel (`mmsys.cpl`), "Line (RT1170 Player)", Properties,
  Advanced, Default Format: "2 channel, 16 bit, 48000 Hz" selects alternate 2;
  "1 channel, 16 bit, 48000 Hz", listed only by a `WFG_UAC_MONO_ALT` image,
  selects alternate 3; "2 channel, 24 bit, 48000 Hz" returns to alternate 1.
  Apply it with no play running: Windows closes and reopens the stream in the
  new alternate, which ends a USB play as a stream close (error 19), and every
  shared-mode client of the endpoint then gets the new format. `USB?` shows
  the result (`uac_stream_alt`, `uac_stream_bits`, `uac_stream_channels`).
- Without changing the default. A WASAPI exclusive-mode client that opens
  48 kHz 16-bit PCM (two channels, or one on a mono image) makes `usbaudio2`
  select alternate 2 (or 3) for itself while the default stays 24-bit; see
  [capturing 16 bits](host-capture-and-analysis.md#16-bit-captures-of-the-usb-sound-card).
  Exclusive mode needs the endpoint to itself: it fails while a shared-mode
  client records, and shared clients fail to open while it runs.

Any change of alternate while a USB play is armed, also one straight from one
format to another without alternate 0, ends that play as a stream close
(error 19, statistics the exact prefix sent). `USB?` reports the format the
play's first file frame went out in (`uac_play_stream_bits`,
`uac_play_stream_channels`), and the open stream's.

The mono alternate is off by default (`WFG_UAC_MONO_ALT=OFF`). The design constraints behind that choice are:

- Microsoft's [USB Audio 2.0 Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)
  page: `usbaudio2` takes Type I PCM with 1 to 4-byte subslots and 8 to 32
  bits, ignores an alternate whose format it does not support, needs
  alternate 0 without an endpoint and the others in ascending order, each
  with an isochronous data endpoint, the same `bTerminalLink` in every
  alternate, and exactly one `bmFormats` bit for Type I. It describes an
  alternate's format "in terms of bNrChannels and AS Format Type
  Descriptor", and picks among alternates of one format by the smallest
  `wMaxPacketSize`. Several bit depths on one interface are therefore
  supported, as in TinyUSB's own `uac2_headset` example (16- and 24-bit
  alternates on one interface).
- Neither that page nor any other source found says whether `usbaudio2`
  accepts an alternate whose channel count (1) differs from the stereo
  cluster its terminal carries (input terminal 1, FL | FR, through feature
  unit 2 to output terminal 3), nor could the USB Audio 2.0 rule on that be
  confirmed. Windows' rule for the initial default format among several
  ([`PKEY_AudioEngine_OEMFormat`](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/pkey-audioengine-oemformat)
  covers only an INF-supplied default) stays a bench check too.
- So the mono alternate waits for a bench result on a `WFG_UAC_MONO_ALT`
  image (below): the endpoint keeps working, the Advanced tab lists the mono
  format, and a mono capture is the left channel `>> 8`. A spec-clean
  alternative is a separate mono streaming interface with its own one-channel
  terminal and endpoint; it is a follow-up.

Build a mono image in its own tree and with its own build label:

```powershell
cmake --preset rt1170-player-release -B build\rt1170-player-mono-release `
  -DWFG_UAC_MONO_ALT=ON -DWFG_BUILD_ID=wfg-0.2.0-mono
cmake --build build\rt1170-player-mono-release --parallel
```

Its commit and `usb_pid` are the standard image's, so the
lane's INFO records tell the two apart only by the label (`build_id`
`wfg-0.2.0-mono`) and by the `usb-audio-mono` capability, which INFO lists
on every `WFG_UAC_MONO_ALT` image, whatever its label. Resolve its assert
addresses against this tree's ELF.

### Card ownership

Safe ownership sequence:

```text
MEDIA HOST -> Windows mounts volume -> stage files -> flush/dismount volume
-> MEDIA LOCAL -> LOAD/GENERATE/PLAY -> STOP/completion -> MEDIA HOST
```

Staging for the sound card uses the same sequence, with a media-only eject
as the dismount: `MEDIA HOST`, copy `NAME.WAV` to `\WG\` on the drive and
read it back, eject the medium with `host/media_eject.py` (STATUS
`msc_ejected` becomes 1 and the drive stays absent), then `MEDIA LOCAL`. The
[bring-up runbook](../usb-sound-card-bringup.md#5-stage-the-card) has the
commands. The eject does not hand the card over; only `MEDIA LOCAL` does.

**Never Explorer's Eject.** The shell's Eject verb (and "Safely Remove
Hardware") can remove the whole composite device instead of the medium, also removing CDC and audio endpoints.
Use a media-only operation to preserve those functions.
`host/media_eject.py` sends only the
storage-media requests to the board's volume, the ones Windows uses for a
card reader's medium: FSCTL_LOCK_VOLUME, FSCTL_DISMOUNT_VOLUME (Windows
writes back and closes the FAT volume), IOCTL_STORAGE_MEDIA_REMOVAL (allow:
PREVENT ALLOW MEDIUM REMOVAL, which clears `msc_host_hold` bit 1) and
IOCTL_STORAGE_EJECT_MEDIA (START STOP UNIT with LoEj=1). Run this from an
ordinary process first. A read-write volume handle does not prove elevation. Device/filesystem permissions and Windows
policy may still deny access; an open file may prevent locking. Refuse the
handoff on any failed step, rather than resetting the board or declaring an
unconfirmed dismount. Use `--expected-serial` to match storage to CDC.
Microsoft describes the distinction between removable-media and fixed-disk
access in its [removable-media permissions note](https://support.microsoft.com/en-us/servicing/os/windows/2017/01/ntfs-disk-access-permissions-for-removable-or-external-media-could-be-bypassed).
The [volume lock](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume)
flushes cached data and fails while files remain open.

### Playing through the sound card

Use the [Windows bench](../windows-usb-bench.md) for discovery, staging and
capture. The [live protocol](../live-control-protocol.md) owns `OUTPUT`,
`LOAD`, `PLAY`, `PLAYLIST` and generation command semantics. The
[CLI reference](command-line-tools.md) covers playlist checking, prediction,
draining and capture splitting. The [testing guide](../testing.md) separates
generation agreement from USB transport acceptance.

The default build has no encoder. See the [custom module tutorial](../custom-encoder-tutorial.md)
for enabling the public TONE module or extending CDC control.

## 4. Scientist and maintainer view

### Wire contracts

`WFG-LIVE/1` uses newline-delimited, sequence-numbered requests and JSON responses.
The maximum request is 384 bytes (a `CUT` of 16 entries fits one request) and
the response buffer is 4096 bytes. Responses
must remain complete JSON under partial USB writes. A disconnect discards a partial
line and resets the connection-local sequence state.

DD008 uses a separate binary COBS-framed session on the same CDC endpoint. A new
connection selects one protocol; clients must close and reopen rather than switch
mid-record. Payload and control channels remain bounded and checksum-protected.

### USB Audio Class 2 function

The [proposed UAC2 OUT recorder design](../design/uac2-out-recorder.md)
extends this topology. It remains a design proposal: the current queued OUT
completion events do not carry the service-interval identity needed to locate
missing packets exactly. The existing capture function is described below.

Topology (entity IDs as in `usb_composite.h`):

```text
clock source 4: internal, fixed 48 kHz, synchronized to SOF; clocks every entity
input terminal 1 ---------> feature unit 2 ------------> output terminal 3 ---> interface 4, alts 1-3
line connector (0x0603),    master mute and volume:      USB streaming          iso IN 0x84
2 channels, FL + FR         stored, never applied        (0x0101)
```

| Descriptor | Values |
|---|---|
| Audio control header | `bcdADC` 2.00, category I/O box (`0x08`), no interrupt endpoint |
| Clock source 4 | `bmAttributes` `0x05`: internal fixed clock, synchronized to SOF; `bmControls` `0x05`: frequency and validity read-only |
| Input terminal 1 | `wTerminalType` `0x0603` (line connector), clock 4, 2 channels, `bmChannelConfig` `0x3` (front left, front right) |
| Feature unit 2 | source 1; master channel `bmaControls` `0x0F` (mute and volume host-programmable); no per-channel controls |
| Output terminal 3 | `wTerminalType` `0x0101` (USB streaming), source 2, clock 4 |
| Streaming interface 4, alternate 0 | no endpoint, no class-specific descriptors |
| Streaming interface 4, alternate 1 | terminal link 3; Format Type I PCM, 2 channels, `bmChannelConfig` `0x3`, `bSubslotSize` 4, `bBitResolution` 24 |
| Streaming interface 4, alternate 2 | terminal link 3; Format Type I PCM, 2 channels, `bmChannelConfig` `0x3`, `bSubslotSize` 2, `bBitResolution` 16 |
| Streaming interface 4, alternate 3 (`WFG_UAC_MONO_ALT` only) | terminal link 3; Format Type I PCM, 1 channel, `bmChannelConfig` `0x4` (front centre), `bSubslotSize` 2, `bBitResolution` 16 |
| Endpoint `0x84`, in each of alternates 1-3 | `bmAttributes` `0x0D` (isochronous, synchronous, data); `wMaxPacketSize` 384, 192 or 96 (48 frames of the alternate's format); `bInterval` 4 at high speed (every 2^(4-1) = 8 microframes = 1 ms), 1 at full speed; class-specific endpoint: no controls, lock delay unused |

Alternate 1 is listed first, the format Windows has used as the default since
Phase 1; see [capture formats](#capture-formats) for the others and how they
are selected. `iFunction` and the audio
control `iInterface` name the function `RT1170 Player`. Windows lists the
endpoint as "Line (RT1170 Player)" (20 characters): the
name in parentheses is this string, not the product string `RT1170 Waveform
Player`. Keep the string stable: lane clients select the endpoint by that
name, and some truncate names longer than 31 characters.

Entity requests to the clock source and the feature unit (`UacControls`, run
by TinyUSB in the `wfg_usb` task) are answered for the master channel (0)
only; any other entity, control selector, channel or request code, and a SET
whose payload has the wrong length or value, stalls the control pipe and
changes nothing:

| Entity | Request | Answer or accepted value |
|---|---|---|
| Clock source 4 | GET CUR sampling frequency | 48000 (4 bytes) |
| Clock source 4 | GET RANGE sampling frequency | one subrange: minimum 48000, maximum 48000, resolution 0 (14 bytes) |
| Clock source 4 | GET CUR clock validity | 1 (1 byte) |
| Clock source 4 | SET CUR sampling frequency | accepted only for 48000 (exactly 4 bytes) |
| Feature unit 2 | GET or SET CUR mute | 0 or 1 (1 byte) |
| Feature unit 2 | GET or SET CUR volume | int16 in 1/256 dB, from -25600 (-100 dB) to 0 (0 dB) (2 bytes) |
| Feature unit 2 | GET RANGE volume | one subrange: -25600, 0, resolution 256 (1 dB) (8 bytes) |

The host's volume and mute start at 0 dB, unmuted, at boot. They are stored,
reported as STATUS `uac_host_volume` and `uac_host_mute`, and never applied:
there is no gain stage on the stream, so it stays bit-exact whatever the
Windows level slider says.

Stream content. A packet is 48 frames of the open alternate's format: 384,
192 or 96 bytes. The player supplies every frame as two 4-byte little-endian
words holding the 24-bit sample in bits 31..8 over a zero low byte
(`sample << 8`, the word the WM8960 gets), left equal to right. Alternate 1
sends those words unchanged; their three high bytes are exactly the sample's
PCM24 WAV bytes (`usb_audio.hpp` checks this at compile time). A 16-bit
alternate sends each word's top two bytes, the sample's top 16 bits
(`sample >> 8`, truncated), and the mono alternate the left word's alone
(`pack_frames()` in `usb_audio.hpp`). Every frame that is not a file
frame of a play on the USB route is exact digital silence (every subslot zero,
no dither): the pre-fill when the stream opens, everything before the play
arms (also while `PLAY` mounts, opens and buffers the file), the rest of the
chunk that carries the last file frame, everything after it, and everything
after STOP or a fault. A play's file frames are the WAV's samples unchanged
when no scenario (`NAME.SCN`) sits beside the WAV and no live control is
queued; the impairment engine and live controls act before the output FIFO,
on both routes. Chunks a play queued while armed (at most three) still go out
after it disarms.

### Clocking: synchronous, locked to SOF

- TinyUSB's IN flow control is off (`CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 0`):
  every packet carries exactly 48 frames, one packet per 1 ms USB frame. The
  clock source is declared synchronized to SOF and the endpoint is synchronous,
  so the stream's sample clock is the host's USB frame clock times 48. Its
  drift against the host is 0 ppm by construction. There is no feedback
  endpoint, no rate adaptation and no sample-rate conversion on the device,
  and the board's own oscillator does not pace the stream: the producer only
  has to stay ahead of the host's polls. The codec route keeps its own sample
  clock, which is not locked to USB.
- A missed service interval (a 1 ms frame in which the host did not complete
  the IN transfer) drops no device frame: the primed packet waits for the next
  poll, and every later frame leaves 1 ms later. Stream frame indices count
  frames sent, so they are not a wall clock: after n missed intervals a stream
  index trails the USB frame count by 48 n frames. The device counts these
  intervals (`uac_missed_intervals`, and per play `uac_play_missed_intervals`);
  what a PC capture shows for such a millisecond is up to Windows, which is why
  the lane keeps its PC-side sample-exact check.
- TinyUSB's IN FIFO holds four of the largest packets (1536 bytes). Each
  completion tops it up to two queued packets of the open format, and TinyUSB
  has already loaded the next one, so in every format at most three packets
  (144 frames, 3 ms) are committed ahead of the wire. TinyUSB sends at most
  the alternate's `wMaxPacketSize` per packet and drops its FIFO whenever the
  host selects an alternate; the producer writes only whole packets of the
  open format, so every packet is one whole packet.

### Media ownership

`MEDIA HOST` allows MSC access only when playback, upload, and generation permit
it. `MEDIA LOCAL` means the operator has already flushed and dismounted the host
volume. The command cannot flush Windows caches; the host utility requires an
explicit assertion to prevent the command from pretending it did.

The firmware also refuses to take the card while the host still holds its
volume, so the handoff is a lock and not only an operator promise:

| Host hold (STATUS `msc_host_hold` bit) | Set by | Released by | `MEDIA LOCAL` reply |
|---|---|---|---|
| 1: removal prevented | SCSI PREVENT ALLOW MEDIUM REMOVAL with PREVENT set, which a host sends while it uses the volume (Windows FAT is expected to while the volume is dirty, Linux while the device is open) | ALLOW MEDIUM REMOVAL (the media-only eject of `host/media_eject.py` sends it after the dismount), a new USB configuration, or an unplug | `ERR HOST_LOCKED`; WFG-LIVE/1 error `HOST_LOCKED` |
| 2: write in progress | The first block of a WRITE(10) reaching the card | That command's status (passed or failed), a Bulk-Only Mass Storage Reset of the MSC interface (the host's reset recovery after it gave up on the command), a new USB configuration, or an unplug | `ERR HOST_WRITING`; WFG-LIVE/1 error `HOST_WRITING` |

A host that gives up on a WRITE(10) between its chunks (a timeout, a
cancelled copy) never gets that command's status; it recovers with a
Bulk-Only Mass Storage Reset (BOT 3.1), then CLEAR_FEATURE(HALT) on the bulk
pipes, without a new configuration. TinyUSB's MSC driver answers the reset
without an MSC callback, so the TinyUSB glue (`usb/usb_audio_tinyusb.c`)
passes the end of every control transfer's status stage to
`WFG_MediaControlTransferDone()`, which ends the write's hold on a reset of
the MSC interface (#44). The reset leaves a removal lock in place: the host
still uses the volume.

A refused `MEDIA LOCAL` changes nothing: the card stays with the host. Retry
`HOST_WRITING` after a moment; for `HOST_LOCKED`, finish the dismount or eject
first. The check and the handoff run in the USB task, which also runs every
MSC callback, so no SCSI command can start between them, and no WRITE(10) is
ever cut between two of its blocks. A card in neither Host nor Local ownership
keeps the old replies (`ERR MEDIA LOCAL FAILED`, WFG-LIVE/1 `BUSY`), and
`MEDIA LOCAL` while the board already owns the card is OK. Windows' own use of
PREVENT ALLOW MEDIUM REMOVAL is a bench check: if a Windows build kept the lock
for the whole mount, `MEDIA LOCAL` would wait for the dismount, which the safe
sequence above already requires. A host that never sends ALLOW keeps
`msc_host_hold` at 1; unplugging and replugging the USB cable ends its session.

A host media eject (START STOP UNIT with LoEj=1, Start=0, which
`host/media_eject.py` sends through IOCTL_STORAGE_EJECT_MEDIA) latches
`msc_ejected`. While it is set, TEST UNIT READY, READ(10), WRITE(10) and
READ CAPACITY answer NOT READY, MEDIUM NOT PRESENT, so Windows does not remount
the volume it just ejected. Only an accepted `MEDIA HOST` (or a board reset)
clears it; a USB bus reset, a host LOAD or START does not, and `MEDIA LOCAL`
works normally while it is set. The read-only LUN still refuses writes as DATA
PROTECT. `MEDIA HOST` and `MEDIA LOCAL` remain the only ownership arbitration:
the card is never shared. An open audio stream on its own does not block
`MEDIA HOST`; a play through it (`OUTPUT USB`) does, like any play, because the
player reads the file from the card until the run ends.

The capture stream is exact digital silence except for the file frames of a
play on the USB route: those it queued while armed, which still go out after
STOP or a stall. Its STATUS fields (`uac_stream_*`, `uac_play_*` and the rest)
are described with the [USB route](rt1170-player-and-audio.md#usb-route) and
listed with units in the [protocol reference](../live-control-protocol.md#usb-sound-card).
Every accepted `PLAY`, on either route, restarts the `uac_play_*` fields (all
0 for a codec play). Judge each play by `uac_play_missed_intervals`, after
opening every capture client and letting the stream settle (the lane note
there). `uac_missed_intervals` is a stream diagnostic since boot: it also
counts the pauses Windows makes while its audio engine reconfigures the
shared stream (at start-up, when a capture client joins or leaves, and while
the Sound settings level meter runs). The WFG-LIVE/1 query `USB?` locates the
latest miss and tells a late device from a host that did not poll
([miss attribution](rt1170-player-and-audio.md#miss-attribution-usb)); its
fields are not in STATUS, whose JSON form is within 7 bytes of the response
limit.

The target media state, `msc_ready`, write-protection flag, eject count and
latch (`msc_ejected`), the host's hold (`msc_host_hold`), ownership handoffs,
read/write counters, and errors are part of the retained status record. The
MSC transfer maxima are in `USB?` ([MSC transfers](#msc-transfers-and-throughput)).

### MSC transfers and throughput

TinyUSB hands each SCSI READ(10) or WRITE(10) to the firmware in chunks of its
MSC endpoint buffer, `CFG_TUD_MSC_EP_BUFSIZE` in `rt1170/usb/tusb_config.h`:
**16 KB, 32 blocks of 512 bytes**. Every chunk of a command but its last is
full; the last carries what is left (a 4 KB command is one 8-block chunk).
Each chunk starts at a block boundary, at the LBA where the previous one
ended. `rt1170/platform/waveform_msc.c` moves each chunk with **one SD
command**: CMD18 (read multiple blocks) or CMD25 (write multiple blocks), each
ended by the controller's automatic CMD12, through a 16 KB bounce buffer in
the non-cacheable OCRAM window
([memory placement](rt1170-player-and-audio.md#memory-placement)). Windows'
USB storage driver typically asks for 64 KB per command during a large copy:
four 16 KB SD commands per SCSI command.

Why 16 KB, and not more. The ChipIdea device controller moves one transfer
with one descriptor (qTD): a 15-bit byte count and five 4 KB page pointers, the
first starting at the buffer's own place in its page. 16 KB therefore fits from
any buffer address; 32 KB fits neither field, and TinyUSB 0.21 queues only one
descriptor per endpoint. `waveform_msc.c` checks the size at compile time
(whole blocks, at most 16 KB).

Why a bounce buffer. The SD controller's DMA uses only its own buffer, never
TinyUSB's, which the USB controller's DMA uses. Both lie in the non-cacheable
OCRAM window (`m_ncache_ocram`, 32 KB at `0x20338000`, MPU region 10): the
bounce buffer through its section attribute in `waveform_msc.c`, TinyUSB's
through `CFG_TUD_MEM_SECTION`, which `tusb_config.h` sets for TinyUSB's
`msc_device.c` alone. The CPU never caches either, so neither DMA needs cache
maintenance, and TinyUSB's buffer is 32-byte aligned, so the SD DMA could use
it directly and save the copy. That is not done here: the placement and
alignment it would rely on are TinyUSB's configuration, not this file's,
while the bounce keeps SD DMA ownership independent of USB buffer placement.

The buffer copy consumes uncached CPU bandwidth in addition to card and USB
transfer time. `msc_max_read_us` and `msc_max_write_us` time the SD transfer
alone; measure complete copy/read-back throughput at the host separately.

The RT1170 SD bus (RT1064 clock setup is in its [port guide](rt1064-board.md)). After `SD_CardInit` the card runs in **High Speed (SDR25)**
timing, switched with CMD6, on **4 data lines at 3.3 V** signalling (the
player images disable host voltage control, so no UHS-I mode). The SDK asks
for 50 MHz and divides the SD controller's root clock (USDHC1) by the
smallest divider that does not exceed it (`USDHC_SetSdClock`).
`rt1170/platform/waveform_sd_clock.h` holds the numbers, and
`wfg-sd-clock` checks them against the SDK's divider rule:

| Build | USDHC1 root | High Speed bus | Raw, block data | One 16 KB chunk on the bus | Identification |
|---|---|---|---|---|---|
| Player images (default) | `SYS_PLL2_PFD2` / 2 = **198 MHz** | 198 / 4 = **49.5 MHz** | 24.75 MB/s, 24.3 MB/s | 0.67 ms | 103125 Hz |
| `-DWFG_SD_CLOCK_24MHZ=ON`, and the SD-only image | `OSC_RC_48M_DIV2` = 24 MHz (`BOARD_BootClockRUN`'s) | 24 MHz | 12 MB/s, 11.8 MB/s | 1.39 ms | 100 kHz |

A block is 1042 bus clocks: 1024 data clocks, a 16-bit CRC and the start and
end bits. `BOARD_BootClockRUN` runs SYS_PLL2 at 528 MHz and its PFD2 at
396 MHz and owns them; `waveform_sd.c` sets only the USDHC1 root's mux and
divider, the root NXP's own SD board configuration uses on this EVK.
Identification asks for 103125 Hz, the 198 MHz root divided by 1920: the
SDK's power-of-two prescaler would turn a 100 kHz request into 96.7 kHz,
below the specification's 100 to 400 kHz for a running clock, while from the
24 MHz root the same request still gives the 100 kHz every earlier image
identified the card at.

Which clock a board runs. An image built with `WFG_SD_CLOCK_24MHZ` has the
standard image's PID and commit, so INFO lists the
`sd-clock-24mhz` capability on it (and only on it), whatever its label; give
it its own build id as well (for example `-DWFG_BUILD_ID=wfg-0.2.0-sd24`).
`USB?` reports the clock the card actually runs at as `sd_bus_clock_hz`, as
card initialisation left it: 49500000 in High Speed from the 198 MHz root,
24000000 on a `WFG_SD_CLOCK_24MHZ` image, and 0 while no card is ready
(before or during initialisation, or after it failed). The High Speed switch
(CMD6) is the last step of `SD_CardInit`, and a card that does not support
High Speed stays in Default Speed, **24.75 MHz**, while its initialisation
still succeeds (`card_init_status` 0); only `sd_bus_clock_hz` shows it. No
data crosses the bus at 49.5 MHz before `SD_CardInit` returns (the switch's
status reads run at 24.75 MHz, and the media layer reads nothing more), so
the first 49.5 MHz transfer is the host's first MSC `READ(10)` or the
board's first FatFs mount.

Validate the chosen clock with actual data transfers: require nonzero
`read_blocks`, no new `read_errors` or `write_errors`, and matching hashes after
large write/read-back operations. Successful card initialization alone does
not exercise sustained High Speed transfers. Record `sd_bus_clock_hz` beside
throughput and `msc_max_*_us`; card programming time, host scheduling and the
buffer copy all affect end-to-end performance. An RT1170 fallback image can
use `-DWFG_SD_CLOCK_24MHZ=ON`; this option does not apply to RT1064.

What the counters mean (STATUS fields, unchanged; their unit is now the
chunk):

| Field | Counts |
|---|---|
| `read_calls`, `write_calls` | Chunks handed to the card: one SD command each (a 600 MB copy is about 38,400 write chunks) |
| `read_blocks`, `write_blocks` | 512-byte blocks of the chunks that passed, exactly |
| `read_errors`, `write_errors` | Chunks the card failed and chunks refused as invalid, one each; either fails its command, so at most one per command. A failed write chunk may have put some of its blocks on the card; `write_blocks` counts none of them. A chunk refused because the medium is not the host's (ejected, or the board's) counts in neither |
| `invalid_requests` | Requests refused before the card (a chunk longer than 16 KB, not whole blocks, off a block boundary or past the card's end, another LUN, an unsupported SCSI command) |

The read-only image refuses every WRITE(10) chunk as DATA PROTECT before it
looks at the request: each counts one `write_calls` and one `write_errors`,
never an invalid request, and reaches no card. `tests/media_msc_tests.cpp`
checks each of these rules on both LUNs.

`USB?` (not STATUS, which has no room) adds the transfer maxima since boot,
passed or failed chunks alike, 0 until the first, and the bus clock they ran
at:

| `USB?` field | Meaning |
|---|---|
| `msc_max_write_us` | The longest SD write of one chunk, in microseconds: from the SD write call to its return, so it includes waiting for the card to finish programming the previous chunk (the SDK polls CMD13 first). The read-only image never writes, so it stays 0 there |
| `msc_max_read_us` | The longest SD read of one chunk, likewise |
| `msc_max_blocks_per_call` | The most blocks one chunk moved in its SD command: 32 once the host has sent a command of 16 KB or more, the proof that the card sees multi-block transfers |
| `sd_bus_clock_hz` | The SD bus clock the card's transfers run at, in Hz, as card initialisation left it: 49500000 (High Speed), 24750000 (a card that stayed in Default Speed), 24000000 on a `WFG_SD_CLOCK_24MHZ` image; 0 while no card is ready. Not a maximum: the current card's |

The times come from the CPU cycle counter (996 MHz) while it cannot have
wrapped, and from the RTOS tick (1 ms) for a transfer of 4.3 s or more, which
only a failing card takes; they saturate at 4294967295.

A write the card refuses. The SD driver (NXP's SDMMC middleware) retries a
failed write command three times, then asks the card how many blocks it wrote
(ACMD22). One to all of the chunk's blocks count as written, and the rest go
out in another write command. A report of none, a count larger than the
chunk, or an ACMD22 answer with R1 error flags fails the chunk, and with it
the command: one `write_errors`. That rule is a local change to the SDK
(`patches/sdmmc-wfg.patch`, applied to the build tree's copy). Upstream, a
card that refuses every write but still answers ACMD22 with 0 (a worn card
gone read-only, for example) made `SD_WriteBlocks` send the same blocks again
without end. The USB task never returned: no STATUS, `USB?` or `MEDIA LOCAL`
reply, and no MSC, until a power cycle. An ACMD22 answer with R1 error flags
passed the failed write. Known limit, not changed: the driver's wait for the
card (`SD_PollingCardStatusBusy`, CMD13 polls) runs down its 600 ms only while
the card holds DAT0 low. A card that stops answering while DAT0 reads high,
such as one pulled from the socket while the host owns it, still stops the USB
task at its next MSC read or write.

Commands whose length disagrees with their blocks. TinyUSB takes a command's
block size from its CBW (the CBW's byte count divided by the CDB's block
count) and never shows the CBW to the callbacks, so `waveform_msc.c` sees only
each chunk's LBA and length. A host that sizes its commands from READ
CAPACITY's 512-byte blocks, as operating systems' storage drivers do, never
sends such a command. One that carries more bytes than its blocks (BOT cases 5
and 11, Hi > Di and Ho > Do) is carried out in full and passes: a WRITE(10) of
one block at LBA 1000 with 1024 bytes writes LBAs 1000 and 1001, and a READ(10)
likewise returns the extra blocks. With the old 512-byte chunk such a command
failed at its second block, after its first was written. The device cannot
tell the two apart without the CBW, and TinyUSB stays unpatched. Do not run the
USB-IF USBCV MSC tests, whose case tests send such commands, against a card
with staged files; use a scratch card.

### Artifact lifecycle

Target generation states are idle, receiving payload, creating WAV, complete, and
failed/aborted. The player task advances generation in bounded blocks so it does
not monopolize the owner loop. A completed artifact records payload bytes, frame
count, output filename, WAV SHA-256, encoder identity/profile, and error state.

The read-only image excludes the local artifact writer and rejects MSC writes
before an SD write routine can run.

### Failure behavior

Do not automatically retry uncertain mutations. Do not treat an uploader exit
code as proof of retained bytes; inspect terminal artifact status and hashes. Do
not infer media safety from a drive letter disappearing without confirming the
board's ownership response.

## 5. 5th-grader view

The SD card is one notebook shared by Windows and the board. Only one may hold the
pencil. `MEDIA HOST` hands the pencil to Windows. After Windows closes the book
and ejects only the book (never Explorer's Eject, which can take the whole
desk away), `MEDIA LOCAL` hands the pencil to the board. If Windows is
still writing, or still has the book open, the board says so and waits.

CDC is the intercom used to ask for the pencil or request a waveform. MSC is the
desk where Windows can see the notebook's files.

The third door is a microphone socket. The PC takes one small bag of 48 sound
samples from it every millisecond, on the PC's own clock, so the board can
never run fast or slow against the PC. The bags are empty (silence) until
someone says `PLAY` with the output set to USB; then they carry the file, then
they are empty again. The board writes down every bag the PC actually took.
The PC can ask for lighter bags: the same 48 samples with their last digits
cut off (16 bits), or, on a board built for it, only the left one of each
pair. The board's notes about the file stay the same either way.

## 6. Toy worked example

Start in host state:

| Step | Owner | Allowed action | Unsafe action |
|---|---|---|---|
| 1 | Host | Copy `PLAY.WAV` | Firmware opens FatFs file |
| 2 | Host, flushing | Dismount the volume | Pull ownership while a copy is pending (the firmware refuses: `HOST_WRITING`, `HOST_LOCKED`) |
| 3 | Local | `LOAD`, `GENERATE`, `PLAY` | Windows writes the volume |
| 4 | Local, stopped | Close files and audio | Request host during generation |
| 5 | Host | Mount and inspect hashes | Assume the same historical drive letter |

If a command times out between steps 2 and 3, the owner is unknown. Reconnect and
query status; do not repeat `MEDIA LOCAL` merely because no acknowledgment arrived.

A sound-card packet, worked through. A mono PCM24 WAV holds the sample
-2 (bytes `FE FF FF`). The subslot is `-2 << 8` = `0xFFFFFE00`, sent
little-endian as `00 FE FF FF` for left and again for right. The high three
bytes `FE FF FF` are the WAV's bytes, which is why a per-channel CRC-32 over
3-byte samples equals `zlib.crc32` of the WAV's data chunk. In alternate 2
the same sample goes out as the subslot's top two bytes, `FF FF`: -1, which is
`-2 >> 8`; a sample `0x123456` goes out as the subslot `0x12345600`, sent
`00 56 34 12` in alternate 1 (the zero pad byte first, as for -2), and as
`34 12` (`0x1234`) in alternate 2. The CRCs still take `FE FF FF`, so they are
the same in both: they name the card's samples, not the wire's, and only
`USB?` (`uac_play_stream_bits` 24) shows that the play went out bit-exact.
A 100-frame file
fills its first two chunks with 96 file frames and the third with 4 file
frames and 44 zero frames; STATUS then reports `uac_play_file_frames` 100 and
`uac_play_last_frame_index` = `uac_play_first_frame_index` + 99. A Windows
pause of 5 ms shows as one gap of 6 USB frames between two completions:
`uac_missed_intervals` rises by 5, and no device frame is lost; the frames
after the gap simply leave 5 ms later.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Live text protocol | `common/live_protocol.*` |
| Artifact request protocol | `common/tx_protocol.*` |
| DD008 framing/session | `common/dd008/` |
| USB identity, numbering and sound-card constants | `rt1170/usb/usb_composite.h` |
| USB descriptors | `rt1170/usb/usb_descriptors.c` |
| TinyUSB configuration (audio block, FIFO sizes, MSC chunk size) | `rt1170/usb/tusb_config.h` |
| UAC2 callbacks, mount/unmount, IN FIFO shim | `rt1170/usb/usb_audio_tinyusb.c`, `rt1170/usb/usb_audio_callbacks.h` |
| UAC2 stream, play statistics, entity requests | `rt1170/usb_audio.*` (owned with the [USB route](rt1170-player-and-audio.md#usb-route)) |
| USB controller, frame number, service watchdog | `rt1170/usb/usb_tinyusb_port.*` |
| CDC service, STATUS/INFO/`USB?`/`PLAYLIST?` replies | `rt1170/usb_service.*`, `rt1170/status_contract.hpp` |
| PLAYLIST files, result blocks and the sequencer | `common/playlist.hpp`, `rt1170/player.cpp` ([PLAYLIST sequencer](rt1170-player-and-audio.md#playlist-sequencer)) |
| Media block ownership, eject latch, host hold, chunked SD transfers and their maxima, the card's bus clock (`sd_bus_clock_hz`) | `rt1170/platform/waveform_msc.*`, `waveform_sd.*` |
| SD controller clock (root, bus and identification clocks; `WFG_SD_CLOCK_24MHZ` and INFO's `sd-clock-24mhz`) | `rt1170/platform/waveform_sd_clock.h`, `waveform_sd.c`, `tests/sd_clock_tests.cpp` (`wfg-sd-clock`) |
| CPU cycle counter for C code | `rt1170/platform/cycle_counter.h` (defined in `board.cpp`) |
| Artifact owner | `rt1170/tx_artifact.*`, `rt1170/tx_usb.*` |
| Host clients | `host/live_control.py`, `host/tx_upload.py`, `host/media-control.ps1`, `host/make_playlist.py` |

Protocol changes need parser tests, fragmented-write tests, disconnect tests,
capacity proofs, host validation, firmware response checks, and backward-compatible
behavior or an explicit version change.

USB device changes:

- Any change to the player's descriptors (an interface, endpoint, entity,
  format or string) moves `wfg_usb_bcd_device_player` (both values, with and
  without `WFG_UAC_MONO_ALT`), so Windows does not reuse a cached descriptor
  set, and updates `wfg-usb-descriptors-writable`, `-readonly` and `-mono`
  (`tests/usb_descriptor_tests.cpp`), which parse the device, qualifier, both
  speed configurations, the other-speed copies and the strings, and check the
  identity, numbering, UAC2 topology and each alternate's format.
- A new capture format is one more alternate in both speed configurations
  (`usb_descriptors.c`), its constants in `usb_composite.h`, one more entry of
  `stream_formats` (`usb_audio.hpp`, whose `producer_supports()` must hold for
  it) and the descriptor tests' table. Every byte count in `UacStream`
  follows the open format; frames, 48 per packet, stay the unit of the
  indices, records and missed intervals. The CRCs stay over the 24-bit source.
- `usb_composite.h` is the one source of the numbers; `tusb_config.h`
  repeats the packet and FIFO sizes as literals for TinyUSB's preprocessor.
  `usb_audio_tinyusb.c` checks those literals and TinyUSB's UAC2 codes
  against `usb_composite.h` at compile time; `usb_descriptors.c` checks the
  interface and endpoint numbering and the descriptor bitfields. Neither
  repeats the other's checks.
- Keep the `RT1170 Player` string and the line-connector terminal type: they
  make the Windows endpoint name the lane selects by.
- A new STATUS field must fit `status_json_worst_case_bytes()` (4089 of 4096
  bytes now, checked at compile time and in `wfg-contract-cpp`). New USB
  diagnostics go into `USB?` (`usb_field_names`, 1481 bytes worst case), never
  into STATUS.
- `CFG_TUD_MSC_EP_BUFSIZE` is the MSC chunk and the bounce buffer's size: keep
  it whole blocks and at most 16 KB (`waveform_msc.c` checks both at compile
  time), and update the 16 KB the MSC tests expect. Both chunk buffers fill
  the 32 KB non-cacheable OCRAM window; the player link fails if either lies
  outside it ([memory placement](rt1170-player-and-audio.md#memory-placement)).
- The MSC ownership rules (eject latch, host hold) and the chunked transfers
  (one SD command per chunk, the refusals, the counters, the `USB?` maxima and
  `sd_bus_clock_hz`) are exercised by
  `wfg-media-msc-writable` and `-readonly` (`tests/media_msc_tests.cpp`), with
  every way the card's initialisation ends (each step's failure and the media
  error it latches, a task that cannot be created, the refusals); their
  `-boot` and `-host-first` runs start the media as `main()` does and with
  MEDIA HOST first, each in a process of its own. `wfg-sd-socket`
  (`tests/sd_socket_tests.cpp`) checks the socket's card detect
  (`platform/waveform_sd.c`, built with the player's SD definitions). The
  stream, its formats and the controls by `wfg-usb-audio` and `-mono` (built
  with `WFG_UAC_MONO_ALT`), and the PLAY, route and reply
  paths by `wfg-player-live-owner`, `-readonly`, `-mono` (a 16-bit mono
  play, and INFO's `usb-audio-mono`) and `-sd24` (INFO's `sd-clock-24mhz`,
  built as a `WFG_SD_CLOCK_24MHZ` image). None of them runs
  TinyUSB or a host controller: enumeration, the Windows endpoint and the
  bit-exact capture need the [bring-up runbook](../usb-sound-card-bringup.md).

## 8. Owner and failure walkthrough

DD008 framing/session code receives bounded bytes and validates a complete frame
before delivering a typed request. `tx_usb` owns USB reception and transport
backpressure; `tx_artifact` owns the local generation job and response publication.
Their handoff flags use lock-free atomics, while multi-field mailbox updates
remain protected by the owning synchronization boundary.

A generation request first verifies local card ownership and idle state. It then
opens its payload/output, runs bounded encoder steps and publishes completion
only after final file handling. Disconnect/reset/cancellation can terminate a job;
partial files and response state must remain distinguishable from success.
Never copy an active generator to another task to implement cancellation.

If FatFs reports a file-close or unmount failure, the owner keeps the affected
handle and media ownership marked active. Playback, host handoff and new TX
starts remain busy until a later STOP, RESET or disconnect cleanup succeeds.
An artifact whose WAV and completion manifest were already committed remains
complete even if the subsequent unmount fails; RESET retries releasing the card
without deleting those files.

Review the media owner state alongside every FatFs/MSC call. A USB acknowledgment
is protocol evidence; it does not prove data reached the analog output. Exercise
fragmented messages, pending writes, malformed lengths, duplicate/reset requests,
disconnect and media-owner refusal in `wfg-tx-protocol`, `wfg-tx-usb-owner`,
`wfg-tx-artifact-owner` and the Python upload/control tests.

The sound card's control path runs in the `wfg_usb` task: TinyUSB calls
`tud_audio_get_req_entity_cb` / `tud_audio_set_req_entity_cb`, which decode the
SETUP packet into a plain `wfg_uac_request_t` for `UacControls`. A refused
request stalls the control pipe and changes nothing. Interface and
configuration changes also arrive in that task; they reach the stream, which
belongs to the USB interrupt level, inside `taskENTER_CRITICAL()`, which masks
the USB IRQ. An alternate with a format opens the stream in that format
(pre-filling two silent packets of it) only inside a configuration: a
`SET_INTERFACE` that TinyUSB queued before a bus reset or unplug is refused,
and so is an alternate the image does not offer. When the host goes from one
format straight to another, TinyUSB first leaves the open alternate (the
close callback, and its FIFO dropped), then opens the new one; the stream
closes in between, so an armed play is interrupted. Bus reset and unplug close the stream from the
USB ISR (`tud_event_hook_cb`), because TinyUSB sends no interface callback
then, and `tud_mount_cb` / `tud_umount_cb` also end the host's MSC session
(its removal lock and any WRITE(10) it had started). A reset or unplug never
clears `msc_ejected`; only an accepted `MEDIA HOST` or a board reset does.

Embedded 64-bit manifest and TX-status fields use the bounded decimal/hex
formatter in `common/uint64_text.hpp` before passing text to `snprintf`.
The firmware's newlib-nano formatter does not support long-long conversions;
this keeps subsequent string arguments correctly aligned even for full-width
counters. Host regressions cover zero, values above 32 bits, and UINT64_MAX.
