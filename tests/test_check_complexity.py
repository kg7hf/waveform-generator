# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/check-complexity.py's ratchet on canned lizard reports; lizard itself is not run.

- file_counts() reads lizard 1.24.0's per-file table, and refuses a table whose
  columns are not that version's (a reordered table would be read wrongly).
- sparse(): a file with fewer than NLOC // 100 functions fails unless the
  baseline's sparse_files lists it; a listed file that has enough, or is not
  measured, is reported for lowering.
- compare(): a function above CCN 25 that the baseline does not list, or that
  grew, fails; one that fell is reported; a listed function lizard no longer
  measures at all fails (#49: one C array with an initialiser later in
  rt1170/player.cpp hid run_player while the file kept enough functions).
- file_floors(): a file that yields fewer functions than the baseline's
  file_functions records fails, also when every listed function is still
  measured and NLOC // 100 is met (#49's partial loss: 47 of player.cpp's 164
  functions hidden after run_player); so does a recorded file lizard did not
  measure at all while it is still in the tree (present_files(): on disk, or
  in the commit's tree with --rev; a commit whose tree git cannot list is an
  error, and main() exits 2 saying so, never an empty tree); a file that
  gained functions, one not yet recorded and one gone from the tree are
  reported.
- main(): --update keeps sparse_files, lowers and drops entries (a function no
  longer measured included), resets a file's count that fell, records every
  measured file's count, and refuses a new function above the limit; a
  malformed file_functions is refused; --rev reads the commit's own baseline
  (not the work tree's), unless --baseline names one, and takes no --update.
"""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'check-complexity.py'
_spec = importlib.util.spec_from_file_location('check_complexity', SCRIPT)
check_complexity = importlib.util.module_from_spec(_spec)
# Its dataclasses look their module up while the class is made.
sys.modules[_spec.name] = check_complexity
_spec.loader.exec_module(check_complexity)

LIZARD = '1.24.0'
PLAYER = 'rt1170/player.cpp'
SCENARIO = 'signal-lab/include/signal_lab/scenario.hpp'
# A recorded file lizard's table leaves out.
LOST = 'common/lost.hpp'

# The fixture repository ignores the user's and the system's Git configuration
# and carries its own identity.
FIXTURE_GIT_SETTINGS = {
    'GIT_CONFIG_GLOBAL': os.devnull,
    'GIT_CONFIG_NOSYSTEM': '1',
    'GIT_AUTHOR_NAME': 'wfg-test',
    'GIT_AUTHOR_EMAIL': 'wfg-test@example.invalid',
    'GIT_COMMITTER_NAME': 'wfg-test',
    'GIT_COMMITTER_EMAIL': 'wfg-test@example.invalid',
}
GIT_REPOSITORY_VARIABLES = ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY')

# lizard 1.24.0's default report on Windows, cut to three files.
REPORT = '''================================================
  NLOC    CCN   token  PARAM  length  location
------------------------------------------------
       5      2     19      1       5 f@1-5@common\\a.cpp
       4      1      8      0       4 g@6-9@common\\a.cpp
3 file analyzed.
==============================================================
NLOC    Avg.NLOC  AvgCCN  Avg.token  function_cnt    file
--------------------------------------------------------------
   2479      13.9     3.5       96.0       164     rt1170\\player.cpp
    250       0.0     0.0        0.0         0     signal-lab\\include\\signal_lab\\scenario.hpp
      9       4.5     1.5       13.5         2     common\\a.cpp

===============================================================================================================
No thresholds exceeded (cyclomatic_complexity > 15 or length > 1000 or nloc > 1000000 or parameter_count > 100)
==========================================================================================
Total nloc   Avg.NLOC  AvgCCN  Avg.token   Fun Cnt  Warning cnt   Fun Rt   nloc Rt
------------------------------------------------------------------------------------------
      2738       4.5     1.5       13.5      166            0      0.00    0.00
'''


def function(name, ccn, file=PLAYER, line=1):
    return check_complexity.Function(file=file, name=name, line=line, ccn=ccn)


def file_count(file, nloc, functions):
    return check_complexity.FileCount(file=file, nloc=nloc, functions=functions)


class FileTableTests(unittest.TestCase):
    def test_the_file_table_is_read_with_forward_slashes(self):
        self.assertEqual(check_complexity.file_counts(REPORT),
                         [file_count(PLAYER, 2479, 164), file_count(SCENARIO, 250, 0), file_count('common/a.cpp', 9, 2)])

    def test_a_report_without_the_table_or_with_other_columns_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'no file table'):
            check_complexity.file_counts(REPORT.split('3 file analyzed.')[0])
        reordered = REPORT.replace('NLOC    Avg.NLOC  AvgCCN  Avg.token  function_cnt    file',
                                   'NLOC    function_cnt  Avg.NLOC  AvgCCN  Avg.token    file')
        with self.assertRaisesRegex(RuntimeError, 'columns'):
            check_complexity.file_counts(reordered)


class SparseFileTests(unittest.TestCase):
    def test_a_file_lizard_lost_fails_unless_listed(self):
        # 2479 lines of code need 24 functions.
        for functions, fails in ((1, True), (23, True), (24, False), (164, False)):
            with self.subTest(functions=functions):
                failures, improved = check_complexity.sparse([file_count(PLAYER, 2479, functions)], {})
                self.assertEqual(len(failures), 1 if fails else 0, failures)
                self.assertEqual(improved, [])
        failures, _ = check_complexity.sparse([file_count(PLAYER, 2479, 1)], {})
        self.assertIn('lizard has lost track of this file', failures[0])
        failures, improved = check_complexity.sparse([file_count(SCENARIO, 250, 0)], {SCENARIO: 'declarations only'})
        self.assertEqual((failures, improved), ([], []))

    def test_a_listed_file_that_no_longer_needs_it_is_reported(self):
        listed = {SCENARIO: 'declarations only', 'gone.cpp': 'a data table'}
        failures, improved = check_complexity.sparse([file_count(SCENARIO, 250, 3)], listed)
        self.assertEqual(failures, [])
        self.assertEqual(len(improved), 2)
        self.assertIn('are enough', improved[0])
        self.assertIn('gone.cpp: listed in sparse_files, but not measured', improved[1])


class FileFloorTests(unittest.TestCase):
    def test_a_file_below_its_recorded_count_fails(self):
        for functions, fails, reported in ((117, True, False), (163, True, False), (164, False, False), (170, False, True)):
            with self.subTest(functions=functions):
                failures, changed = check_complexity.file_floors([file_count(PLAYER, 2479, functions)], {PLAYER: 164})
                self.assertEqual(len(failures), 1 if fails else 0, failures)
                self.assertEqual(len(changed), 1 if reported else 0, changed)
        failures, _ = check_complexity.file_floors([file_count(PLAYER, 2479, 117)], {PLAYER: 164})
        self.assertIn(f'{PLAYER}: 117 functions, fewer than the 164 in file_functions', failures[0])
        self.assertIn('run --update', failures[0])
        _, changed = check_complexity.file_floors([file_count(PLAYER, 2479, 170)], {PLAYER: 164})
        self.assertIn(f'{PLAYER}: 170 functions, file_functions 164', changed[0])

    def test_a_sparse_file_keeps_its_count_of_none(self):
        self.assertEqual(check_complexity.file_floors([file_count(SCENARIO, 250, 0)], {SCENARIO: 0}), ([], []))

    def test_unrecorded_and_vanished_files_are_reported_not_failed(self):
        failures, changed = check_complexity.file_floors([file_count('common/a.cpp', 9, 2)], {'gone.cpp': 4})
        self.assertEqual(failures, [])
        self.assertEqual(changed, ['common/a.cpp: 2 functions, not yet in file_functions',
                                   'gone.cpp: in file_functions, but gone from the tree'])

    def test_a_recorded_file_still_in_the_tree_but_not_measured_fails(self):
        # lizard skipped or lost the whole file: every function in it would lose its floor unseen.
        recorded = {'common/a.cpp': 2, LOST: 4, 'gone.cpp': 3}
        failures, changed = check_complexity.file_floors([file_count('common/a.cpp', 9, 2)], recorded, {LOST})
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(f'{LOST}: in file_functions and still in the tree, but lizard did not measure it', failures[0])
        self.assertIn('run --update', failures[0])
        self.assertEqual(changed, ['gone.cpp: in file_functions, but gone from the tree'])


class PresentFileTests(unittest.TestCase):
    """present_files(): a recorded file is in the tree when it is on disk, or with --rev in the commit's tree."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)

    def write(self, path):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b'int f();\n')

    def git(self, *args):
        environment = {key: value for key, value in os.environ.items() if key not in GIT_REPOSITORY_VARIABLES}
        environment.update(FIXTURE_GIT_SETTINGS)
        subprocess.run(['git', '-C', str(self.root), *args], check=True, capture_output=True, env=environment)

    def test_the_work_tree_counts_the_files_on_disk(self):
        self.write(LOST)
        self.assertEqual(check_complexity.present_files(self.root, None, {LOST, 'common/gone.hpp'}), {LOST})
        self.assertEqual(check_complexity.present_files(self.root, None, set()), set())

    def test_a_revision_counts_the_files_of_its_tree(self):
        self.git('init', '-q')
        self.write(LOST)
        self.git('add', LOST)
        self.git('commit', '-q', '-m', 'one file')
        (self.root / LOST).unlink()
        self.write('common/new.hpp')
        self.assertEqual(check_complexity.present_files(self.root, 'HEAD', {LOST, 'common/new.hpp'}), {LOST})
        self.assertEqual(check_complexity.present_files(self.root, None, {LOST, 'common/new.hpp'}), {'common/new.hpp'})

    def test_a_revision_git_cannot_list_is_an_error_not_an_empty_tree(self):
        # QA on 5062eb8 (a surviving mutant): read as an empty tree, a failed listing would make a recorded file
        # lizard did not measure look gone, and the check would pass.
        self.git('init', '-q')
        self.write(LOST)
        self.git('add', LOST)
        self.git('commit', '-q', '-m', 'one file')
        with self.assertRaisesRegex(RuntimeError, r'^git ls-tree of no-such-revision failed: .+'):
            check_complexity.present_files(self.root, 'no-such-revision', {LOST})
        with self.assertRaisesRegex(RuntimeError, r'^git ls-tree found no measured path in no-such-revision: .+'):
            check_complexity.measure_revision(self.root, 'no-such-revision')


class CompareTests(unittest.TestCase):
    BASELINE = {(PLAYER, 'run_player'): 27, (PLAYER, 'parse'): 40, (PLAYER, 'gone'): 30, (PLAYER, 'tamed'): 26}

    def compared(self, functions):
        current = check_complexity.over_limit(functions)
        return check_complexity.compare(current, self.BASELINE, {measured.key for measured in functions})

    def test_new_and_grown_functions_fail_and_fallen_ones_are_reported(self):
        failures, improved, unmeasured = self.compared(
            [function('run_player', 27), function('parse', 41), function('tamed', 12), function('fresh', 26),
             function('gone', 29)])
        # Each list is sorted by file and function name.
        self.assertEqual(len(failures), 2, failures)
        self.assertIn('fresh: CCN 26, above 25 and not in the baseline', failures[0])
        self.assertIn('parse: CCN 41, grew from 40', failures[1])
        self.assertEqual(len(improved), 2, improved)
        self.assertIn('gone: CCN 29, baseline 30', improved[0])
        self.assertIn('tamed: no longer above 25, baseline 26', improved[1])
        self.assertEqual(unmeasured, [])

    def test_a_listed_function_lizard_no_longer_measures_is_named(self):
        # #49's partial loss: run_player and the functions after it are not measured.
        failures, improved, unmeasured = self.compared([function('parse', 40), function('tamed', 3), function('gone', 30)])
        self.assertEqual(failures, [])
        self.assertEqual(len(improved), 1)
        self.assertEqual(len(unmeasured), 1, unmeasured)
        self.assertIn(f'{PLAYER}: run_player: in the baseline at CCN 27, but lizard no longer measures it', unmeasured[0])
        self.assertIn('run --update if you removed or refactored it', unmeasured[0])


class MainTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.baseline = Path(self.folder.name) / 'complexity-baseline.json'
        self.write_baseline()

    def tearDown(self):
        self.folder.cleanup()

    def write_baseline(self, **extra):
        entries = [{'file': PLAYER, 'function': 'run_player', 'ccn': 27}, {'file': PLAYER, 'function': 'parse', 'ccn': 40}]
        self.baseline.write_text(json.dumps({'lizard': LIZARD, 'ccn_limit': 25, 'sparse_files': {SCENARIO: 'declarations only'},
                                             **extra, 'functions': entries}), encoding='utf-8')

    def run_main(self, functions, *extra):
        measurement = check_complexity.Measurement(
            functions=functions, files=[file_count(PLAYER, 2479, len(functions)), file_count(SCENARIO, 250, 0)])
        argv = ['check-complexity.py', '--root', self.folder.name, '--baseline', str(self.baseline), *extra]
        with mock.patch.object(check_complexity, 'lizard_version', return_value=LIZARD), \
                mock.patch.object(check_complexity, 'measure', return_value=measurement), \
                mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()) as printed:
            status = check_complexity.main()
        return status, printed.getvalue()

    def recorded(self):
        return json.loads(self.baseline.read_text(encoding='utf-8'))

    def few(self, count):
        return [function(f'small{index}', 2) for index in range(count)]

    def test_a_listed_function_no_longer_measured_fails_the_check(self):
        status, printed = self.run_main([function('parse', 40), *self.few(30)])
        self.assertEqual(status, 1, printed)
        self.assertIn('FAIL rt1170/player.cpp: run_player: in the baseline at CCN 27, but lizard no longer measures it', printed)

    def test_update_keeps_sparse_files_and_drops_and_lowers_entries(self):
        before = self.baseline.read_bytes()
        status, printed = self.run_main([function('parse', 38), *self.few(30)], '--update')
        self.assertEqual(status, 0, printed)
        self.assertIn('drop rt1170/player.cpp: run_player', printed)
        recorded = self.recorded()
        self.assertEqual(recorded['sparse_files'], {SCENARIO: 'declarations only'})
        self.assertEqual(recorded['functions'], [{'file': PLAYER, 'function': 'parse', 'ccn': 38}])
        self.assertEqual(recorded['lizard'], LIZARD)
        self.assertNotEqual(self.baseline.read_bytes(), before)

    def test_update_never_admits_a_new_function(self):
        before = self.baseline.read_bytes()
        status, printed = self.run_main([function('run_player', 27), function('parse', 40), function('fresh', 26), *self.few(30)],
                                        '--update')
        self.assertEqual(status, 1, printed)
        self.assertEqual(self.baseline.read_bytes(), before, 'a failing check leaves the baseline as it was')

    def test_the_measured_tree_passes(self):
        status, printed = self.run_main([function('run_player', 27), function('parse', 40), *self.few(30)])
        self.assertEqual(status, 0, printed)
        self.assertIn('0 failures', printed)

    def test_a_partial_loss_that_keeps_every_listed_function_fails(self):
        # #49's probe with the hiding array after run_player: 164 functions fall to 117, run_player and
        # parse are still measured, and 117 is still above NLOC // 100 = 24.
        self.write_baseline(file_functions={PLAYER: 164, SCENARIO: 0})
        lost = [function('run_player', 27), function('parse', 40), *self.few(115)]
        status, printed = self.run_main(lost)
        self.assertEqual(status, 1, printed)
        self.assertIn(f'FAIL {PLAYER}: 117 functions, fewer than the 164 in file_functions', printed)
        status, printed = self.run_main([function('run_player', 27), function('parse', 40), *self.few(162)])
        self.assertEqual(status, 0, printed)

    def test_update_resets_a_count_that_fell_and_keeps_sparse_files(self):
        self.write_baseline(file_functions={PLAYER: 164, SCENARIO: 0})
        status, printed = self.run_main([function('run_player', 27), function('parse', 40), *self.few(115)], '--update')
        self.assertEqual(status, 0, printed)
        self.assertIn(f'reset {PLAYER}: 117 functions, fewer than the 164', printed)
        recorded = self.recorded()
        self.assertEqual(recorded['file_functions'], {PLAYER: 117, SCENARIO: 0})
        self.assertEqual(recorded['sparse_files'], {SCENARIO: 'declarations only'})
        status, printed = self.run_main([function('run_player', 27), function('parse', 40), *self.few(115)])
        self.assertEqual(status, 0, printed)
        self.assertNotIn('count ', printed, 'the counts are recorded')

    def test_update_records_the_count_of_every_measured_file(self):
        status, printed = self.run_main([function('run_player', 27), function('parse', 40), *self.few(30)])
        self.assertEqual(status, 0, printed)
        self.assertIn(f'count {PLAYER}: 32 functions, not yet in file_functions', printed)
        self.assertIn('Run with --update', printed)
        status, printed = self.run_main([function('run_player', 27), function('parse', 40), *self.few(30)], '--update')
        self.assertEqual(status, 0, printed)
        self.assertEqual(self.recorded()['file_functions'], {PLAYER: 32, SCENARIO: 0})

    def test_a_recorded_file_lizard_did_not_measure_fails_while_it_is_in_the_tree(self):
        counts = {PLAYER: 32, SCENARIO: 0, LOST: 4}
        measured = [function('run_player', 27), function('parse', 40), *self.few(30)]
        lost = Path(self.folder.name) / LOST
        lost.parent.mkdir(parents=True)
        lost.write_bytes(b'int f();\n')
        self.write_baseline(file_functions=counts)
        status, printed = self.run_main(measured)
        self.assertEqual(status, 1, printed)
        self.assertIn(f'FAIL {LOST}: in file_functions and still in the tree, but lizard did not measure it', printed)
        status, printed = self.run_main(measured, '--update')
        self.assertEqual(status, 0, printed)
        self.assertIn(f'reset {LOST}: in file_functions and still in the tree', printed)
        self.assertEqual(self.recorded()['file_functions'], {PLAYER: 32, SCENARIO: 0}, '--update drops it')
        # Gone from the tree, it is reported and passes.
        lost.unlink()
        self.write_baseline(file_functions=counts)
        status, printed = self.run_main(measured)
        self.assertEqual(status, 0, printed)
        self.assertIn(f'count {LOST}: in file_functions, but gone from the tree', printed)

    def test_a_revision_is_measured_against_its_own_baseline(self):
        # The fixture commits a baseline with parse at CCN 40 and fresh at 26; the work tree's (self.baseline,
        # passed by no option here) would admit parse at 45 and not list fresh.
        repository = Path(self.folder.name) / 'repository'
        (repository / 'scripts').mkdir(parents=True)
        committed = {'lizard': LIZARD, 'ccn_limit': 25, 'sparse_files': {SCENARIO: 'declarations only'},
                     'functions': [{'file': PLAYER, 'function': 'parse', 'ccn': 40},
                                   {'file': PLAYER, 'function': 'fresh', 'ccn': 26}]}
        tree_baseline = repository / 'scripts' / 'complexity-baseline.json'
        tree_baseline.write_text(json.dumps(committed), encoding='utf-8')
        environment = {key: value for key, value in os.environ.items() if key not in GIT_REPOSITORY_VARIABLES}
        environment.update(FIXTURE_GIT_SETTINGS)
        for command in (('init', '-q'), ('add', '-A'), ('commit', '-q', '-m', 'baseline')):
            subprocess.run(['git', '-C', str(repository), *command], check=True, capture_output=True, env=environment)
        tree_baseline.write_text(json.dumps(dict(committed, functions=[{'file': PLAYER, 'function': 'parse', 'ccn': 45}])),
                                 encoding='utf-8')

        def run_revision(functions, *extra):
            measurement = check_complexity.Measurement(
                functions=functions, files=[file_count(PLAYER, 2479, len(functions)), file_count(SCENARIO, 250, 0)])
            argv = ['check-complexity.py', '--root', str(repository), '--rev', 'HEAD', *extra]
            with mock.patch.object(check_complexity, 'lizard_version', return_value=LIZARD), \
                    mock.patch.object(check_complexity, 'measure_revision', return_value=measurement), \
                    mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()) as printed, \
                    contextlib.redirect_stderr(io.StringIO()) as errors:
                status = check_complexity.main()
            return status, printed.getvalue() + errors.getvalue()

        # parse grew from the commit's 40: a failure, which the work tree's 45 would hide.
        status, printed = run_revision([function('parse', 42), function('fresh', 26), *self.few(30)])
        self.assertEqual(status, 1, printed)
        self.assertIn('parse: CCN 42, grew from 40', printed)
        # fresh is in the commit's baseline: no failure, which the work tree's would report.
        status, printed = run_revision([function('parse', 40), function('fresh', 26), *self.few(30)])
        self.assertEqual(status, 0, printed)
        # --baseline still names the file to read.
        status, printed = run_revision([function('parse', 42), *self.few(30)], '--baseline', str(tree_baseline))
        self.assertEqual(status, 0, printed)
        # A revision without the file cannot be checked, and --update writes no baseline from a revision.
        self.assertEqual(run_revision(self.few(30), '--update')[0], 2)
        tree_baseline.unlink()
        subprocess.run(['git', '-C', str(repository), 'rm', '-q', '--cached', 'scripts/complexity-baseline.json'], check=True,
                       capture_output=True, env=environment)
        subprocess.run(['git', '-C', str(repository), 'commit', '-q', '-m', 'no baseline'], check=True, capture_output=True,
                       env=environment)
        status, printed = run_revision(self.few(30))
        self.assertEqual(status, 2, printed)
        self.assertIn('HEAD has no scripts/complexity-baseline.json', printed)

    def test_a_revision_whose_files_git_cannot_list_cannot_be_checked(self):
        # The folder is no repository, so git ls-tree fails: the check says why and exits 2, rather than read the
        # recorded file lizard did not measure as gone from the tree and pass.
        self.write_baseline(file_functions={PLAYER: 32, SCENARIO: 0, LOST: 4})
        measurement = check_complexity.Measurement(
            functions=[function('run_player', 27), function('parse', 40), *self.few(30)],
            files=[file_count(PLAYER, 2479, 32), file_count(SCENARIO, 250, 0)])
        argv = ['check-complexity.py', '--root', self.folder.name, '--rev', 'HEAD', '--baseline', str(self.baseline)]
        with mock.patch.object(check_complexity, 'lizard_version', return_value=LIZARD), \
                mock.patch.object(check_complexity, 'measure_revision', return_value=measurement), \
                mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()) as printed, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            status = check_complexity.main()
        self.assertEqual(status, 2, printed.getvalue() + errors.getvalue())
        self.assertIn('Complexity check failed to run: git ls-tree of HEAD failed: ', errors.getvalue())
        self.assertNotIn('failures', printed.getvalue())

    def test_a_malformed_file_functions_is_refused(self):
        for counts in ({PLAYER: -1}, {PLAYER: '164'}, {PLAYER: 16.4}, {PLAYER: True}, [PLAYER]):
            with self.subTest(counts=counts):
                self.write_baseline(file_functions=counts)
                with contextlib.redirect_stderr(io.StringIO()) as errors:
                    status, _ = self.run_main([function('run_player', 27), function('parse', 40), *self.few(30)])
                self.assertEqual(status, 2)
                self.assertIn('file_functions maps each file', errors.getvalue())


if __name__ == '__main__':
    unittest.main()
