#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""The host tools' critical-error handler (common/host_critical_error.hpp, #50).

A failed Result::value() in a host tool reaches m110::critical_error(), which
calls the handler the tool's main() installed: one report line on stderr and
the tool's failure status. Two checks:

- the probe: a test build of signal_lab_render (wfg_signal_lab_render_probe,
  compiled with WFG_CRITICAL_ERROR_PROBE) takes the value of a failed Result
  on --critical-error-probe; its report line and exit status 2 are checked,
  and the line it wrote to stdout just before, unflushed, which the handler
  flushes (std::_Exit does not, and stdout into a pipe is fully buffered);
  the tool itself refuses the option as it refuses any unknown one;
- every host tool's main(): each owned non-test source that defines main(),
  the firmware's aside, installs the handler as its first statement, with
  the tool's name and the constant that holds its documented failure
  status, so a tool added later without the handler fails here.

Usage: test_host_critical_error.py --probe <wfg_signal_lab_render_probe> --tool <signal_lab_render>
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
PROBE: Path
TOOL: Path
REPORT = re.compile(r"signal_lab_render: critical error: internal_error \(8\): the critical-error probe's failed Result; "
                    r"call site 0x[1-9a-f][0-9a-f]*\n")
# What the probe writes to stdout, unflushed, before the failed Result.
PROBE_OUTPUT = "output written before the critical error\n"
# How the tool refuses the probe's option where it has no probe.
REFUSED = "unknown or incomplete argument: --critical-error-probe\n"
# Each host tool's main(): the tool's name and the constant of its failure status, with that constant's value.
HOST_TOOLS = {
    "channel-sim/cli/pathsim_chain.cpp": ("pathsim_chain", "exit_failure", 1),
    "host/capture/capture_main.cpp": ("waveform_capture", "exit_failure", 1),
    "host/render/signal_lab_render.cpp": ("signal_lab_render", "exit_failure", 2),
    "signal-lab/validation/watterson_render.cpp": ("wfg_watterson_render", "exit_usage", 2),
}
# The firmware's main() installs the firmware's handler (rt1170/critical_error.hpp).
FIRMWARE_MAIN = "rt1170/main.cpp"
MAIN = re.compile(r"^int\s+main\s*\([^)]*\)\s*\{", re.M)
TEST_SOURCE = re.compile(r"(?:^|/)tests?/|(?:^|/)(?:tests?\.\w+|\w*_tests?\.\w+|test_\w*\.\w+)$")
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def first_statement(text: str) -> str:
    """The first statement of main()'s body, comments dropped and spaces folded."""
    body = text[MAIN.search(text).end():]
    body = COMMENT.sub(" ", body)
    return " ".join(body[:body.index(";") + 1].split())


class ProbeTests(unittest.TestCase):
    def run_tool(self, executable: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            return subprocess.run([str(executable), *arguments], cwd=directory, capture_output=True, text=True, check=False,
                                  timeout=60)

    def test_a_failed_result_value_is_reported_and_ends_the_tool(self):
        result = self.run_tool(PROBE, "--critical-error-probe")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertRegex(result.stderr, "^" + REPORT.pattern + "$")
        self.assertEqual(result.stdout, PROBE_OUTPUT)

    def test_the_tool_itself_has_no_probe(self):
        result = self.run_tool(TOOL, "--critical-error-probe")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(result.stderr, REFUSED)

    def test_the_probe_build_takes_the_option_only_alone(self):
        for executable in (PROBE, TOOL):
            with self.subTest(executable=executable.name):
                result = self.run_tool(executable, "--critical-error-probe", "--block", "1")
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(result.stderr, REFUSED)


class InstalledTests(unittest.TestCase):
    def owned_mains(self) -> list[str]:
        listed = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--", "*.cpp"], capture_output=True, text=True, check=True)
        return sorted(path for path in listed.stdout.splitlines()
                      if not TEST_SOURCE.search(path) and not path.startswith("third_party/")
                      and MAIN.search((ROOT / path).read_text(encoding="utf-8")))

    def test_every_host_tool_is_listed(self):
        self.assertEqual(self.owned_mains(), sorted([*HOST_TOOLS, FIRMWARE_MAIN]))

    def test_each_main_installs_the_handler_first(self):
        for path, (tool, constant, value) in HOST_TOOLS.items():
            with self.subTest(path=path):
                text = (ROOT / path).read_text(encoding="utf-8")
                self.assertEqual(first_statement(text), f'm110::install_host_critical_error_handler("{tool}", {constant});')
                self.assertRegex(text, rf"\bconstexpr int {constant} = {value};")

    def test_the_firmware_installs_its_own(self):
        self.assertNotIn("install_host_critical_error_handler", (ROOT / FIRMWARE_MAIN).read_text(encoding="utf-8"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--tool", type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    PROBE = arguments.probe.resolve(strict=True)
    TOOL = arguments.tool.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
