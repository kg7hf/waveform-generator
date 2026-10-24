#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""waveform_capture's refusals that need no capture device, by exit status and text (#50).

Each case stops before a device is opened: a command line the tool refuses
(exit 2 with the usage text, or the line that names the rule), and an input
that no device on any machine has, which the tool looks up and does not find
(exit 1). Looking a device up only enumerates the inputs (WinMM's device
capabilities, the MMDevice endpoints' friendly names); nothing is opened, and
nothing is written to the output path. So the capture's open() refusals and
the device selection are pinned without a sound card.

Usage: test_waveform_capture.py --capture <waveform_capture>
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

CAPTURE: Path
USAGE = [
    "usage: waveform_capture [--backend winmm|wasapi-raw] --list-devices [--json]",
    "       waveform_capture [--backend winmm|wasapi-raw] --output <capture.wav>",
    "         (--device-index <n> | --device-name <exact>) --sample-rate 48000",
    "         --channel left|right|average --max-seconds <seconds> [--stop-stdin]",
]
# A name no input has: WinMM keeps 31 characters of a name, and this is unique there too.
NO_SUCH_INPUT = "wfg-test no such input 7f3a"
# An index no WinMM input has. A rule the usage enforces, if lost, would then
# reach this index and not an input that exists.
NO_SUCH_INDEX = "9999"


class RefusalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "capture.wav"

    def capture(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(CAPTURE), *arguments], cwd=self.root, capture_output=True, text=True, check=False,
                              stdin=subprocess.DEVNULL, timeout=60)

    def assert_refused(self, result: subprocess.CompletedProcess[str], status: int, lines: list[str]) -> None:
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "".join(f"{line}\n" for line in lines))
        self.assertEqual(result.stdout, "")
        self.assertFalse(self.output.exists(), "nothing is written")

    def test_a_number_from_2_to_the_64_up_gets_the_usage(self):
        # parse_u64 refuses them as no decimal below 2^64. 2^64 to 2^64 + 3
        # would wrap to 0 to 3 s if its overflow check left out the last
        # digit, and 2^64 + 1 s would then reach the device lookup.
        # --device-index goes through the same parser.
        for seconds in ("18446744073709551616", "18446744073709551617", "18446744073709551618", "18446744073709551619",
                        "99999999999999999999"):
            with self.subTest(seconds=seconds):
                result = self.capture("--output", str(self.output), "--device-index", NO_SUCH_INDEX, "--max-seconds", seconds)
                self.assert_refused(result, 2, USAGE)

    def test_a_command_line_without_its_rules_gets_the_usage(self):
        output = str(self.output)
        for arguments in ((),
                          ("--output", output, "--max-seconds", "1"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--device-name", NO_SUCH_INPUT, "--max-seconds", "1"),
                          ("--device-index", NO_SUCH_INDEX, "--max-seconds", "1"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--max-seconds", "0"),
                          ("--output", output, "--device-index", "x", "--max-seconds", "1"),
                          ("--output", output, "--device-index", "4294967296", "--max-seconds", "1"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--max-seconds", "1", "--sample-rate", "44100"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--max-seconds", "1", "--channel", "both"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--max-seconds", "1", "--backend", "asio"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--max-seconds", "1", "--bogus"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--max-seconds", "1", "--bogus", "1"),
                          ("--output", output, "--device-index", NO_SUCH_INDEX, "--max-seconds"),
                          ("--list-devices", "--output", output),
                          ("--backend", "asio", "--list-devices", "--json")):
            with self.subTest(arguments=arguments):
                self.assert_refused(self.capture(*arguments), 2, USAGE)

    def test_wasapi_raw_selects_by_name_only(self):
        result = self.capture("--backend", "wasapi-raw", "--output", str(self.output), "--device-index", NO_SUCH_INDEX, "--max-seconds", "1")
        self.assert_refused(result, 2, ["--backend wasapi-raw requires --device-name for stable endpoint selection"])

    def test_a_winmm_name_no_input_has_is_refused(self):
        result = self.capture("--output", str(self.output), "--device-name", NO_SUCH_INPUT, "--max-seconds", "1")
        self.assert_refused(result, 1, [f"requested WinMM input name was not unique: {NO_SUCH_INPUT} (matches=0)"])

    def test_a_device_index_no_winmm_input_has_is_refused(self):
        # 4294967295 is WAVE_MAPPER, which waveInOpen would take as "the default input".
        for index in (NO_SUCH_INDEX, "4294967295"):
            with self.subTest(index=index):
                result = self.capture("--output", str(self.output), "--device-index", index, "--max-seconds", "1")
                self.assert_refused(result, 1, [f"failed to open WinMM capture: no WinMM input has this --device-index: {index}"])

    def test_an_empty_wasapi_name_is_refused_by_open(self):
        result = self.capture("--backend", "wasapi-raw", "--output", str(self.output), "--device-name", "", "--max-seconds", "1")
        self.assert_refused(result, 1, ["failed to open WASAPI RAW capture: invalid WASAPI capture arguments"])

    def test_a_wasapi_name_no_endpoint_has_is_refused_by_open(self):
        result = self.capture("--backend", "wasapi-raw", "--output", str(self.output), "--device-name", NO_SUCH_INPUT,
                              "--max-seconds", "1")
        self.assert_refused(result, 1, ["failed to open WASAPI RAW capture: WASAPI endpoint name was not unique or active"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    CAPTURE = arguments.capture.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
