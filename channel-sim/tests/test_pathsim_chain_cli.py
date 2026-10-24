#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""pathsim_chain's refusals, run on the built program (ctest: pathsim_chain_cli_tests).

  python channel-sim/tests/test_pathsim_chain_cli.py --chain <build>/pathsim_chain.exe

- An output or truth file that is the input (by any spelling) is refused with exit 2
  before anything is written, so the input keeps every byte (#46: `x.wav x.wav` used to
  truncate it to a 44-byte header).
- A truth file that is the output by another spelling is refused with exit 2, also when
  neither file exists yet (on Windows "RUN.WAV" names "run.wav"; the two writers used to
  share one file), and nothing is written. A distinct truth file is written.
- A file name in the ANSI code page beyond ASCII ("caf\u00e9.wav") runs (the old check
  converted it to a std::filesystem path, which terminated the program).
- A name with '*' or '?' is taken as written, not expanded by the C runtime to the files
  it matches: an output "in-4*.wav" leaves in-48k.wav alone, an input "in-8*.wav" opens
  nothing. On Windows a "\\\\?\\" spelling of the input is refused as the output and
  opens as the input (the expansion used to mangle it).
- --resample-gain-db, the 48 kHz chain's down-conversion gain, is refused with exit 2 on
  an 8 kHz input (pathsim's own job, which has no resampler), and no output is written.
  On a 48 kHz input it is taken: -20 dB reads 8 dB below the default, the campaign's
  -12 dB, which is the same as --resample-gain-db -12.
Writes only into a temporary directory.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

EXIT_OK, EXIT_FAILURE, EXIT_USAGE = 0, 1, 2
TONE_HZ = 1000.0
TONE_PEAK = 8000.0
CHANNEL = ('--direct', '--snr', '30')
# No noise: with --snr, pathsim scales its output to a fixed level, whatever the input's.
NOISELESS = ('--direct',)
CHAIN: Path | None = None


def write_tone(path: Path, rate: int, seconds: float) -> bytes:
    """A mono PCM16 tone; returns the file's bytes."""
    frames = int(rate * seconds)
    samples = [round(TONE_PEAK * math.sin(2.0 * math.pi * TONE_HZ * index / rate)) for index in range(frames)]
    with wave.open(str(path), 'wb') as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(struct.pack(f'<{frames}h', *samples))
    return path.read_bytes()


def run_chain(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(CHAIN), *arguments], capture_output=True, text=True, timeout=120)


def level_db(path: Path) -> float:
    """The mean power of a mono PCM16 WAV's samples, in dB."""
    with wave.open(str(path), 'rb') as handle:
        frames = handle.readframes(handle.getnframes())
    samples = struct.unpack(f'<{len(frames) // 2}h', frames)
    return 10.0 * math.log10(sum(sample * sample for sample in samples) / len(samples))


class ChainRefusalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        (self.root / 'sub').mkdir()
        self.narrow = self.root / 'in-8k.wav'
        self.narrow_bytes = write_tone(self.narrow, 8000, 0.5)
        self.wide = self.root / 'in-48k.wav'
        write_tone(self.wide, 48000, 0.25)

    def tearDown(self) -> None:
        self.folder.cleanup()

    def test_a_distinct_output_runs(self) -> None:
        output = self.root / 'out-8k.wav'
        result = run_chain(*CHANNEL, str(self.narrow), str(output))
        self.assertEqual(result.returncode, EXIT_OK, result.stderr)
        self.assertGreater(output.stat().st_size, 44)
        self.assertEqual(self.narrow.read_bytes(), self.narrow_bytes)

    def test_the_input_as_output_is_refused_before_it_is_truncated(self) -> None:
        spellings = [str(self.narrow), str(self.root / 'sub' / '..' / self.narrow.name)]
        if os.name == 'nt':
            spellings.append(str(self.narrow).upper())  # Windows names ignore case
        for output in spellings:
            with self.subTest(output=output):
                result = run_chain(*CHANNEL, str(self.narrow), output)
                self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
                self.assertIn('the output file is the input file', result.stderr)
                self.assertEqual(self.narrow.read_bytes(), self.narrow_bytes, 'the input keeps every byte')
        result = run_chain(*CHANNEL, '-i', str(self.narrow), '-o', str(self.narrow))
        self.assertEqual(result.returncode, EXIT_USAGE, 'the -i/-o spelling is checked as well')
        self.assertEqual(self.narrow.read_bytes(), self.narrow_bytes)

    def test_a_truth_file_that_is_the_input_or_the_output_is_refused(self) -> None:
        output = self.root / 'out-8k.wav'
        result = run_chain(*CHANNEL, '--truth-out', str(self.narrow), str(self.narrow), str(output))
        self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
        self.assertIn('the truth file is the input file', result.stderr)
        self.assertEqual(self.narrow.read_bytes(), self.narrow_bytes)
        result = run_chain(*CHANNEL, '--truth-out', str(output), str(self.narrow), str(output))
        self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
        self.assertIn('the truth file is the output file', result.stderr)
        self.assertFalse(output.exists(), 'nothing is written')

    def test_a_truth_file_that_is_the_output_by_another_spelling_is_refused(self) -> None:
        output = self.root / 'run.wav'
        spellings = [str(self.root / 'sub' / '..' / output.name)]
        if os.name == 'nt':
            spellings.append(str(self.root / output.name.upper()))  # Windows names ignore case
        before = sorted(path.name for path in self.root.iterdir())
        for truth in spellings:
            with self.subTest(truth=truth, output='not yet written'):
                result = run_chain(*CHANNEL, '--truth-out', truth, str(self.narrow), str(output))
                self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
                self.assertIn('the truth file is the output file', result.stderr)
                self.assertEqual(sorted(path.name for path in self.root.iterdir()), before, 'nothing is written')
        output.write_bytes(b'an earlier output')
        for truth in spellings:
            with self.subTest(truth=truth, output='exists'):
                result = run_chain(*CHANNEL, '--truth-out', truth, str(self.narrow), str(output))
                self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
                self.assertIn('the truth file is the output file', result.stderr)
                self.assertEqual(output.read_bytes(), b'an earlier output', 'the output is not opened')

    def test_a_distinct_truth_file_is_written(self) -> None:
        output = self.root / 'out-8k.wav'
        truth = self.root / 'truth.bin'
        result = run_chain(*CHANNEL, '--truth-out', str(truth), str(self.narrow), str(output))
        self.assertEqual(result.returncode, EXIT_OK, result.stderr)
        self.assertGreater(truth.stat().st_size, 0)
        with wave.open(str(output), 'rb') as written, wave.open(str(self.narrow), 'rb') as read:
            self.assertEqual(written.getnframes(), read.getnframes())

    @unittest.skipUnless(os.name == 'nt', 'a name in the Windows ANSI code page')
    def test_a_name_beyond_ascii_runs(self) -> None:
        source = self.root / 'caf\u00e9.wav'
        source.write_bytes(self.narrow_bytes)
        output = self.root / 'caf\u00e9-out.wav'
        result = run_chain(*CHANNEL, str(source), str(output))
        self.assertEqual(result.returncode, EXIT_OK, result.stderr)
        self.assertGreater(output.stat().st_size, 44)
        result = run_chain(*CHANNEL, str(source), str(source))
        self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
        self.assertEqual(source.read_bytes(), self.narrow_bytes)

    def test_a_name_with_a_wildcard_is_taken_as_written(self) -> None:
        # The MinGW C runtime would expand '*' and '?' to the files they match before main()
        # runs: "in-4*.wav" as the output would name in-48k.wav and overwrite it, and
        # "in-8*.wav" as the input would read in-8k.wav.
        wide_bytes = self.wide.read_bytes()
        result = run_chain(*CHANNEL, str(self.narrow), str(self.root / 'in-4*.wav'))
        self.assertEqual(self.wide.read_bytes(), wide_bytes, 'the file the name would match keeps every byte')
        if os.name == 'nt':
            # '*' cannot be in a Windows file name, so the literal name cannot be created.
            self.assertEqual(result.returncode, EXIT_FAILURE, result.stderr)
            self.assertIn('in-4*.wav', result.stderr)
        output = self.root / 'out-8k.wav'
        result = run_chain(*CHANNEL, str(self.root / 'in-8*.wav'), str(output))
        self.assertEqual(result.returncode, EXIT_FAILURE, result.stderr)
        self.assertIn('in-8*.wav', result.stderr)
        self.assertFalse(output.exists(), 'nothing is written')

    @unittest.skipUnless(os.name == 'nt', 'the Windows extended-length prefix')
    def test_an_extended_length_name_is_taken_as_written(self) -> None:
        # "\\?\" holds a '?', which the runtime's expansion took for a pattern and mangled.
        extended = '\\\\?\\' + str(self.narrow)
        result = run_chain(*CHANNEL, str(self.narrow), extended)
        self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
        self.assertIn('the output file is the input file', result.stderr)
        self.assertEqual(self.narrow.read_bytes(), self.narrow_bytes)
        output = self.root / 'out-8k.wav'
        result = run_chain(*CHANNEL, extended, str(output))
        self.assertEqual(result.returncode, EXIT_OK, result.stderr)
        expected = self.root / 'expected-8k.wav'
        self.assertEqual(run_chain(*CHANNEL, str(self.narrow), str(expected)).returncode, EXIT_OK)
        self.assertEqual(output.read_bytes(), expected.read_bytes())

    def test_the_resample_gain_is_refused_on_an_8k_input(self) -> None:
        output = self.root / 'out-8k.wav'
        for chain in ((), ('--chain', '8k'), ('--chain', 'auto')):
            with self.subTest(chain=chain):
                result = run_chain(*CHANNEL, *chain, '--resample-gain-db', '-20', str(self.narrow), str(output))
                self.assertEqual(result.returncode, EXIT_USAGE, result.stderr)
                self.assertIn('--resample-gain-db applies to the 48 kHz chain only', result.stderr)
                self.assertFalse(output.exists(), 'the output is not created')

    def test_the_resample_gain_is_taken_on_a_48k_input(self) -> None:
        outputs = {}
        for gain in ('-20', '-12', None):
            outputs[gain] = self.root / f'out-48k-{gain or "default"}.wav'
            extra = ('--resample-gain-db', gain) if gain is not None else ()
            result = run_chain(*NOISELESS, *extra, str(self.wide), str(outputs[gain]))
            self.assertEqual(result.returncode, EXIT_OK, result.stderr)
            with wave.open(str(outputs[gain]), 'rb') as handle:
                self.assertEqual(handle.getframerate(), 48000)
        # The noiseless channel passes the level through: the output moves with the gain.
        self.assertAlmostEqual(level_db(outputs[None]) - level_db(outputs['-20']), 8.0, delta=0.1)
        self.assertEqual(outputs['-12'].read_bytes(), outputs[None].read_bytes(), "the default is the campaign's -12 dB")


def main() -> int:
    global CHAIN
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--chain', type=Path, required=True, help='the built pathsim_chain program')
    args, rest = parser.parse_known_args()
    CHAIN = args.chain.resolve()
    if not CHAIN.is_file():
        print(f'missing: {CHAIN}', file=sys.stderr)
        return 2
    program = unittest.main(argv=[sys.argv[0], *rest], exit=False, verbosity=2)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
