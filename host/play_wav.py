#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Play a mono 48 kHz PCM16/PCM24 WAV on an explicitly named WASAPI output.

Convenience playback using sounddevice/PortAudio and Windows shared mode.
Render interference with signal_lab_render first. This is not a bit-exact
endpoint measurement or the planned live Waveform Studio runtime.
"""

import argparse
import json
import math
import sys
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd


def outputs():
    apis = sd.query_hostapis()
    return [(index, device) for index, device in enumerate(sd.query_devices())
            if apis[device['hostapi']]['name'] == 'Windows WASAPI'
            and device['max_output_channels'] >= 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav', nargs='?', type=Path)
    parser.add_argument('--list-devices', action='store_true')
    parser.add_argument('--device', help='Exact WASAPI output name; no default fallback')
    parser.add_argument('--gain-db', type=float, default=-12.0,
                        help='Playback attenuation in dB (default: -12; must be <= 0)')
    args = parser.parse_args()
    devices = outputs()
    if args.list_devices:
        for index, device in devices:
            print(json.dumps({'index': index, 'name': device['name'],
                              'backend': 'Windows WASAPI'}))
        return 0
    if args.wav is None or not args.device:
        parser.error('provide a WAV and --device, or use --list-devices')
    if not math.isfinite(args.gain_db) or args.gain_db > 0:
        parser.error('--gain-db must be finite and <= 0')
    matches = [(index, device) for index, device in devices
               if device['name'] == args.device]
    if len(matches) != 1:
        parser.error(f'expected one exact output match, found {len(matches)}; use --list-devices')
    index, device = matches[0]
    gain = 10 ** (args.gain_db / 20)
    frames = 0
    underflows = 0
    with wave.open(str(args.wav), 'rb') as reader:
        width = reader.getsampwidth()
        if (reader.getnchannels() != 1 or reader.getframerate() != 48000
                or width not in (2, 3) or reader.getcomptype() != 'NONE'):
            parser.error('input must be mono 48 kHz PCM16 or packed PCM24')
        expected = reader.getnframes()
        sd.check_output_settings(device=index, channels=2, dtype='float32', samplerate=48000)
        with sd.OutputStream(device=index, channels=2, dtype='float32',
                             samplerate=48000, latency='high') as stream:
            print(json.dumps({'event': 'PLAYING', 'file': str(args.wav.resolve()),
                              'device': device['name'], 'backend': 'Windows WASAPI shared',
                              'gain_db': args.gain_db, 'frames': expected}), flush=True)
            while raw := reader.readframes(4096):
                if width == 2:
                    mono = np.frombuffer(raw, dtype='<i2').astype(np.float32) / 32768
                else:
                    packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
                    values = packed[:, 0] | (packed[:, 1] << 8) | (packed[:, 2] << 16)
                    values = (values ^ 0x800000) - 0x800000
                    mono = values.astype(np.float32) / 8388608
                stereo = np.repeat((mono * gain)[:, None], 2, axis=1)
                underflows += int(stream.write(stereo))
                frames += len(mono)
            # Normal stream context exit drains queued audio before closing.
        if frames != expected:
            raise ValueError(f'truncated WAV: expected {expected} frames, read {frames}')
    print(json.dumps({'event': 'DONE', 'frames': frames, 'underflows': underflows}), flush=True)
    return 1 if underflows else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print('Playback stopped.', file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, wave.Error, sd.PortAudioError) as error:
        print(f'Playback failed: {error}', file=sys.stderr)
        sys.exit(1)
