#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""wfg_watterson_render takes its file names as written (#46).

The MinGW C runtime expands '*' and '?' in the command line before main()
runs unless the tool links CRT_noglob.o (cmake/wfg_noglob.cmake): an output
"out-4*.wav" then names out-48k.wav and overwrites it, and a source
"src-*.wav" reads src-1.wav. Here neither happens, and the files the names
would match keep every byte. tests/test_signal_lab_render.py checks the same
for signal_lab_render.

Usage: test_watterson_render_arguments.py --renderer <wfg_watterson_render>
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

RENDERER: Path
SCENARIO = {"reference_rms": 0.1, "impairments": []}


class ArgumentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.scenario = self.root / "case.json"
        self.scenario.write_bytes(json.dumps(SCENARIO).encode("utf-8"))

    def render(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(RENDERER), "--scenario", str(self.scenario), *arguments], cwd=self.root,
                              capture_output=True, text=True, check=False)

    def test_an_output_name_with_a_wildcard_is_taken_as_written(self):
        existing = self.root / "out-48k.wav"
        existing.write_bytes(b"existing output")
        wildcard = self.root / "out-4*.wav"
        result = self.render("--silence-seconds", "0.1", "--output", str(wildcard))
        self.assertEqual(existing.read_bytes(), b"existing output", "the file the name would match keeps every byte")
        if os.name == "nt":
            # '*' cannot be in a Windows file name, so the literal name cannot be written.
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("cannot write " + str(wildcard), result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(wildcard.exists())

    def test_a_source_name_with_a_wildcard_is_taken_as_written(self):
        source = self.root / "src-1.wav"
        rendered = self.render("--silence-seconds", "0.1", "--output", str(source))
        self.assertEqual(rendered.returncode, 0, rendered.stdout + rendered.stderr)
        before = source.read_bytes()
        output = self.root / "from wildcard.wav"
        result = self.render("--source", str(self.root / "src-*.wav"), "--output", str(output))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("cannot read a RIFF/WAVE source", result.stderr)
        self.assertFalse(output.exists(), "nothing is written")
        self.assertEqual(source.read_bytes(), before)
        # The same source by its own name renders.
        plain = self.render("--source", str(source), "--output", str(output))
        self.assertEqual(plain.returncode, 0, plain.stdout + plain.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    RENDERER = arguments.renderer.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
