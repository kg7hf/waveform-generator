#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""The gate scripts' verdicts and arguments (ctest: gate_script_tests); no compiler, lane tree or build is used.

- A skipped check (gate 1b without arm-none-eabi-g++) is SKIP, never "PASS (0/0)": both
  check_draw_order.py and run_gates.py exit 1 for it unless --allow-skip is given (#46).
- A gate that ran no case is FAIL (0/0), with or without --allow-skip.
- --gates takes a non-empty choice of 1, 2 and 3; anything else (gate 0, 4, nothing) is a
  usage error, exit 2, before any gate runs.
"""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

GATES_DIRECTORY = Path(__file__).resolve().parents[1] / 'gates'
sys.path.insert(0, str(GATES_DIRECTORY))
sys.dont_write_bytecode = True

import check_draw_order as cdo  # noqa: E402

try:
    import run_gates  # noqa: E402  (numpy, as the gates themselves)
except ImportError as missing:  # pragma: no cover - a host without numpy
    # Not a skip: a host that cannot import run_gates.py cannot run the gates either, and
    # a skip would let ctest report the --gates and SKIP-verdict tests as passed.
    run_gates = None
    MISSING = str(missing)
else:
    MISSING = ''

FAKE_GIMPLE = '''  D.1 = PathSim::Rng::operator() (&rsample);
  D.2 = PathSim::Rng::operator() (&rsample);
  cmplx::set (&v, D.2, D.1);
'''


def fake_compile(command: list[str], cwd: Path) -> None:
    """Stands in for a compiler run: one GIMPLE dump whose draw site puts the first draw into the imaginary part."""
    (cwd / 'draw.cpp.006t.gimple').write_text(FAKE_GIMPLE, encoding='utf-8')


class VerdictTests(unittest.TestCase):
    def test_a_skip_is_neither_a_pass_nor_a_failure_and_fails_the_exit_without_allow_skip(self) -> None:
        self.assertEqual(cdo.verdict(True, False), cdo.PASS)
        self.assertEqual(cdo.verdict(False, False), cdo.FAIL)
        self.assertEqual(cdo.verdict(True, True), cdo.SKIP, 'a skipped check is SKIP whatever its partial results')
        self.assertEqual(cdo.exit_status([cdo.PASS, cdo.PASS], False), 0)
        self.assertEqual(cdo.exit_status([cdo.PASS, cdo.SKIP], False), 1)
        self.assertEqual(cdo.exit_status([cdo.PASS, cdo.SKIP], True), 0)
        self.assertEqual(cdo.exit_status([cdo.FAIL, cdo.SKIP], True), 1, '--allow-skip never excuses a failure')
        self.assertEqual(cdo.exit_status([], True), 1, 'nothing ran, so nothing passed')

    def test_the_check_without_an_arm_compiler_is_skip(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            skipped = cdo.check(work / 'build', work, Path('g++'), None, fake_compile)
            ran = cdo.check(work / 'build', work, Path('g++'), Path('arm-none-eabi-g++'), fake_compile)
        self.assertTrue(skipped['skipped'])
        self.assertFalse(skipped['pass'], 'host results alone are no pass')
        self.assertEqual(skipped['verdict'], cdo.SKIP)
        self.assertEqual({result['compiler'] for result in skipped['results']}, {'host'})
        self.assertEqual((ran['skipped'], ran['pass'], ran['verdict']), (False, True, cdo.PASS))

    def test_the_standalone_check_exits_1_on_a_skip_unless_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            (build / 'pathsim_chain.recipe.json').write_text(json.dumps({'compiler': 'g++'}), encoding='utf-8')
            record = {'results': [], 'skipped': True, 'pass': False, 'verdict': cdo.SKIP}
            for extra, expected in (([], 1), (['--allow-skip'], 0)):
                with self.subTest(extra=extra), mock.patch.object(cdo, 'check', return_value=record), \
                        mock.patch.object(cdo, 'find_arm_gxx', return_value=None), \
                        mock.patch.object(sys, 'argv', ['check_draw_order.py', '--build', str(build), *extra]), \
                        contextlib.redirect_stdout(io.StringIO()) as printed:
                    self.assertEqual(cdo.main(), expected)
                self.assertIn('draw order: SKIP', printed.getvalue())


class GateRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        if run_gates is None:
            self.fail(f'run_gates.py cannot be imported (it needs numpy): {MISSING}')

    def test_gates_takes_a_choice_of_1_2_and_3(self) -> None:
        self.assertEqual(run_gates.parse_gates('1,2,3'), frozenset((1, 2, 3)))
        self.assertEqual(run_gates.parse_gates('3, 1'), frozenset((1, 3)))
        for text in ('0', '4', '', '1,4', '1,,2', 'one', '1.0'):
            with self.subTest(text=text), self.assertRaises(run_gates.argparse.ArgumentTypeError):
                run_gates.parse_gates(text)

    def test_gates_naming_no_valid_gate_is_a_usage_error_before_anything_runs(self) -> None:
        for text in ('0', '4', ''):
            with self.subTest(text=text), mock.patch.object(sys, 'argv', ['run_gates.py', '--build', 'nowhere', '--gates', text]), \
                    contextlib.redirect_stderr(io.StringIO()) as printed, self.assertRaises(SystemExit) as caught:
                run_gates.main()
            self.assertEqual(caught.exception.code, 2)
            self.assertIn('--gates', printed.getvalue())

    def test_a_skipped_gate_1b_is_reported_as_skip_and_fails_the_run_unless_allowed(self) -> None:
        def gates() -> dict:
            return {'gate1': {'cases': 7, 'passed': 7}, 'gate1b': {'cases': 1, 'passed': 0, 'skipped': True},
                    'gate3': {'cases': 3, 'passed': 3}}

        settled = gates()
        self.assertEqual(run_gates.settle(settled, False), 1)
        self.assertEqual([settled[name]['verdict'] for name in ('gate1', 'gate1b', 'gate3')], ['PASS', 'SKIP', 'PASS'])
        self.assertFalse(settled['gate1b']['pass'])
        text = run_gates.verdict_text(settled['gate1b'])
        self.assertTrue(text.startswith('SKIP'), text)
        self.assertNotIn('PASS', text)
        self.assertEqual(run_gates.settle(gates(), True), 0, '--allow-skip accepts the skip')
        failed = gates()
        failed['gate3']['passed'] = 2
        self.assertEqual(run_gates.settle(failed, True), 1)
        self.assertEqual(run_gates.verdict_text(failed['gate3']), 'FAIL (2/3 cases)')
        report = {'started_utc': 'now', 'gates': settled, 'cases': []}
        self.assertIn('- gate1b: SKIP', run_gates.markdown(report))

    def test_a_gate_that_ran_no_case_fails(self) -> None:
        def gates() -> dict:
            return {'gate1': {'cases': 7, 'passed': 7}, 'gate3': {'cases': 0, 'passed': 0}}

        for allow_skip in (False, True):
            with self.subTest(allow_skip=allow_skip):
                settled = gates()
                self.assertEqual(run_gates.settle(settled, allow_skip), 1, '--allow-skip excuses a skip, not an empty gate')
                self.assertFalse(settled['gate3']['pass'])
                self.assertEqual(settled['gate3']['verdict'], 'FAIL')
                self.assertEqual(run_gates.verdict_text(settled['gate3']), 'FAIL (0/0 cases)')
                self.assertEqual(settled['gate1']['verdict'], 'PASS')


if __name__ == '__main__':
    unittest.main(verbosity=2)
