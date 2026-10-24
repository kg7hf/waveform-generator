# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Criterion 7's coverage judgement (criterion7_close): the matrix is judged on the full battery only,
and main() gives it the run's own criteria and --quick.

  python -B -m unittest discover -s signal-lab/validation -p "test_*.py" -v
"""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_watterson_validation as rv  # noqa: E402

P = rv.P
ALL = list(rv.FULL_BATTERY)
SUBSET = ['6', '7']
SUBSET_REASON = 'criteria 1, 2, 3, 4, 5 not run; --quick'


class CoverageJudgementTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)

    def write(self, test_id: str, paths: tuple, snr_db: float | None = None) -> None:
        rv.write_json(self.work / 'scenarios' / f'{test_id}.json', rv.scenario(test_id, paths, snr_db))

    def subset_scenarios(self) -> None:
        """What `--criteria 6,7` renders for criterion 7: B9's shifted and phased paths, B10's Table XVI rows."""
        for shift in (75.0, -75.0):
            self.write(f'c7-fixed-shift{shift:+g}Hz', (P(shift_hz=shift),))
        self.write('c7-rayleigh-1Hz-shift+75Hz', (P(spread_hz=1.0, shift_hz=75.0),))
        self.write('c7-fixed-phase90', (P(phase_deg=90.0),))
        for delay_ms, spread in rv.TABLE_XVI:
            self.write(f'c7-b10-{delay_ms:g}ms-{spread:g}Hz', (P(spread_hz=spread), P(delay_ms, spread)), rv.B10_SNR_LABEL_DB)

    def rest_of_the_battery(self) -> None:
        """What the other criteria add that the matrix needs: spreads of 0.1 and 10 Hz (criterion 2) and
        D.6.2's fixed channels at 50 dB (criterion 5), which here also bring the 4 paths and 9 ms that
        criterion 2's delay rows bring in a full run."""
        self.write('c2-doppler-0.1Hz', (P(spread_hz=0.1),))
        self.write('c2-doppler-10Hz', (P(spread_hz=10.0),))
        for name, delays_ms in rv.D62_CHANNELS:
            self.write(f'c5-D62-{name}', tuple(P(delay_ms=d) for d in delays_ms), rv.D62_SNR_DB)

    @staticmethod
    def block(rows_pass: bool = True, b10_pass: bool = True) -> dict:
        return {'criterion': 7, 'title': 'coverage', 'rows': [{'pass': rows_pass}], 'b10': [{'pass': b10_pass}]}

    @staticmethod
    def summary(block: dict, quick: bool) -> str:
        return rv.summary_markdown({'started_utc': 'start', 'finished_utc': 'end', 'git_head': 'head', 'quick': quick,
                                    'binaries': {'driver': {'sha256': 'd' * 64}, 'pathsim_chain': {'sha256': 'p' * 64}},
                                    'criteria': {'7': block}})

    def test_what_a_run_judges(self):
        self.assertIsNone(rv.coverage_not_judged(['1', '2', '3', '4', '6', '7', '5'], False))  # the default --criteria
        self.assertEqual(rv.coverage_not_judged(ALL, True), '--quick')
        self.assertEqual(rv.coverage_not_judged(['7'], False), 'criteria 1, 2, 3, 4, 5, 6 not run')
        self.assertEqual(rv.coverage_not_judged(SUBSET, True), SUBSET_REASON)

    def test_subset_run_reports_coverage_not_judged(self):
        """`--criteria 6,7 --quick`: the matrix misses the lane minimums and D.6.2 by construction; it is
        reported, not judged, and criterion 7 is its rows' verdict."""
        self.subset_scenarios()
        block = rv.criterion7_close(self.block(), self.work, SUBSET, True)
        matrix = block['matrix']
        self.assertFalse(matrix['pass'])
        self.assertEqual((matrix['path_counts'], len(matrix['first_battery']['d62_50db_missing'])), ([1, 2], 3))
        self.assertEqual(matrix['first_battery']['table_xvi_missing'], [])
        self.assertFalse(matrix['judged'])
        self.assertEqual(matrix['not_judged'], f'{SUBSET_REASON}: the matrix is judged on the full battery only (every criterion, without --quick)')
        self.assertEqual((block['pass'], block['verdict']), (True, f'PASS (coverage not judged: {SUBSET_REASON})'))
        summary = self.summary(block, True)
        self.assertIn(f'- criterion 7: PASS (coverage not judged: {SUBSET_REASON}) (coverage)', summary)
        self.assertIn(f'Coverage matrix: not judged ({SUBSET_REASON}: the matrix is judged on the full battery only', summary)
        for failing in (self.block(rows_pass=False), self.block(b10_pass=False)):  # its rows and B10's still judge it
            failing = rv.criterion7_close(failing, self.work, SUBSET, True)
            self.assertEqual((failing['pass'], failing['verdict']), (False, f'FAIL (coverage not judged: {SUBSET_REASON})'))

    def test_full_battery_judges_coverage(self):
        self.subset_scenarios()
        block = rv.criterion7_close(self.block(), self.work, ALL, False)
        self.assertTrue(block['matrix']['judged'])
        self.assertFalse(block['pass'])
        self.assertNotIn('verdict', block)
        self.assertNotIn('not_judged', block['matrix'])
        self.assertIn('- criterion 7: FAIL (coverage)', self.summary(block, False))
        self.assertIn('Coverage matrix: FAIL.', self.summary(block, False))
        self.rest_of_the_battery()
        block = rv.criterion7_close(self.block(), self.work, ALL, False)
        self.assertEqual((block['matrix']['pass'], block['pass']), (True, True))
        self.assertIn('Coverage matrix: PASS.', self.summary(block, False))
        self.assertFalse(rv.criterion7_close(self.block(rows_pass=False), self.work, ALL, False)['pass'])


class MainPassesQuickTests(unittest.TestCase):
    """main() on `--criteria 6,7`, with and without --quick: the run's own --quick reaches criterion7_close
    (QA on 9c9fc85). The battery is stubbed (no simulator, render or probe runs); criterion7_close and the
    results it writes are the real ones."""

    LANE_TOOLS = ('analyze_snr.py', 'analyze_doppler.py', 'analyze_delay.py', 'common.py', 'gen_probes.py', 'align.py')

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        exe = '.exe' if rv.os.name == 'nt' else ''
        self.build, self.pathsim, self.lane, self.work = root / 'build', root / 'pathsim', root / 'lane', root / 'work'
        for stand_in in (self.build / 'signal-lab' / f'wfg_watterson_render{exe}', self.pathsim / f'pathsim_chain{exe}',
                         *(self.lane / 'hfchannel/probes' / name for name in self.LANE_TOOLS)):
            stand_in.parent.mkdir(parents=True, exist_ok=True)
            stand_in.write_bytes(b'stand-in')

    def run_main(self, *flags: str) -> tuple[int, mock.MagicMock, dict]:
        argv = ['run_watterson_validation.py', '--build', str(self.build), '--pathsim-build', str(self.pathsim), '--lane-root', str(self.lane),
                '--work', str(self.work), '--criteria', '6,7', *flags]
        passing = [{'pass': True}]
        runner = mock.MagicMock(log=[], gate_waits=0)
        with mock.patch.object(rv, 'criterion7_close', wraps=rv.criterion7_close) as close, \
                mock.patch.object(rv, 'Runner', return_value=runner), mock.patch.object(rv, 'Probes'), \
                mock.patch.object(rv, 'determinism_rows', return_value=passing), mock.patch.object(rv, 'coverage_rows', return_value=passing), \
                mock.patch.object(rv, 'b10_rows', return_value=passing), mock.patch.object(rv, 'below_normal_self'), \
                mock.patch.object(rv, 'toolchain_bin', return_value=None), mock.patch.object(rv, 'git_head', return_value='head'), \
                mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
            code = rv.main()
        return code, close, rv.read_json(self.work / 'results.json')

    def test_quick_reaches_criterion7_close(self):
        for flags, quick, reason in ((('--quick',), True, SUBSET_REASON), ((), False, 'criteria 1, 2, 3, 4, 5 not run')):
            with self.subTest(quick=quick):
                code, close, results = self.run_main(*flags)
                close.assert_called_once()
                self.assertEqual(close.call_args.args[1:], (self.work.resolve(), SUBSET, quick))
                self.assertEqual(code, 0)
                self.assertIs(results['quick'], quick)
                self.assertEqual(results['criteria']['7']['verdict'], f'PASS (coverage not judged: {reason})')
                self.assertFalse(results['criteria']['7']['matrix']['judged'])


if __name__ == '__main__':
    unittest.main()
