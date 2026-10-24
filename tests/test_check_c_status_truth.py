# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/check-c-status-truth.py on its fixture, on units it cannot check, and on canned clang-query output (#51).

The fixture (tests/c_status_truth) is a C and a C++ source that read a C
status as a truth value in each form the check's matchers cover, every such
line marked with the matchers that must find it, and the reads that are none:
a compare with kWFG_StatusOk, status_of(...).is_ok(), a status passed on or
stored, a switch on it. Its compile database is written here, with -DNDEBUG
as a release build has, so an assert() is found only because the check
undefines NDEBUG. Each form must be found by exactly the matchers its line
names. C-only conditional-promotion matchers need hits only in the C fixture;
ordinary status-typed matchers cover corresponding C++ forms. No other line
may give a finding. The check must fail on a unit clang-query cannot parse (a
syntax error, a missing header; a warning the database's -Werror would make
an error passes), on a matcher that does not parse, on a
database with no owned unit (empty, or only units outside the owned
directories), and, on canned output, when clang-query exits non-zero, prints
an error, runs fewer matchers than it was given or reports a match without a
location, and a relative location must be read against the directory of the
unit's compile command. The tests that run clang-query need it: WFG_CLANG_QUERY names it
(ctest sets it), else clang-query-19 or clang-query on PATH; without one they
are skipped and the canned-output tests still run.
"""

import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'check-c-status-truth.py'
_spec = importlib.util.spec_from_file_location('check_c_status_truth', SCRIPT)
check_c_status_truth = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_c_status_truth)

FIXTURE = ROOT / 'tests' / 'c_status_truth'
C_FIXTURE = FIXTURE / 'truth_forms.c'
CPP_FIXTURE = FIXTURE / 'truth_forms.cpp'
# A marked line: the bind names of the matchers that must find it, or "any".
MARKER = re.compile(r'\btruth:\s*([a-z -]+?)\s*(?:\*/)?\s*$')
# The reads each fixture must hold on lines that give no finding.
NEGATIVES = {
    C_FIXTURE: ('status != kWFG_StatusOk', 'wfg_truth_fixture_probe() == kWFG_StatusOk && ready',
                'const bool ok = status == kWFG_StatusOk;', 'switch (status)', 'wfg_truth_fixture_pass(status);',
                'const enum wfg_status copy = wfg_truth_fixture_probe();', 'return copy;',
                '(int)status == 0', 'if (numeric)',
                '(ready ? status : wfg_truth_fixture_probe()) == kWFG_StatusOk'),
    CPP_FIXTURE: ('status != kWFG_StatusOk', 'wfg_truth_fixture_probe() == kWFG_StatusOk && ready',
                  '!waveform_generator::status_of(status).is_ok()', 'waveform_generator::status_of(wfg_truth_fixture_probe())',
                  'held != kWFG_StatusOk', 'switch (status)', 'wfg_truth_fixture_pass(status);', 'return copy;',
                  '(int)status == 0', 'static_cast<int>(status) != kWFG_StatusOk',
                  'if (numeric)',
                  '(ready ? status : wfg_truth_fixture_probe()) == kWFG_StatusOk'),
}


# The reads with a status on either side of one operator, each operand found at its own column.
TWO_OPERAND_READS = ((C_FIXTURE, 'status ^ wfg_truth_fixture_probe()'), (CPP_FIXTURE, 'status ^ wfg_truth_fixture_probe()'),
                     (C_FIXTURE, 'status || wfg_truth_fixture_probe()'),
                     (CPP_FIXTURE, 'status || wfg_truth_fixture_probe()'),
                     (C_FIXTURE, '(int)status || (int)wfg_truth_fixture_probe()'),
                     (CPP_FIXTURE, '(int)status || (int)wfg_truth_fixture_probe()'))


def find_clang_query():
    named = os.environ.get('WFG_CLANG_QUERY')
    if named:
        return named
    return next((found for found in map(shutil.which, check_c_status_truth.CLANG_QUERY_NAMES) if found), None)


CLANG_QUERY = find_clang_query()
needs_clang_query = unittest.skipUnless(CLANG_QUERY, 'clang-query is not installed (set WFG_CLANG_QUERY or put it on PATH)')


def repo_path(path):
    return path.relative_to(ROOT).as_posix()


def marked_lines(source):
    """Each marked line of a fixture source: its number and the bind names it expects (or {'any'})."""
    lines = source.read_bytes().decode('utf-8').split('\n')
    return {number: frozenset(match.group(1).split()) for number, line in enumerate(lines, 1)
            if (match := MARKER.search(line))}


def write_database(directory, sources, extra=()):
    """A compile database for the sources, as a release host build writes one (-DNDEBUG), in `directory`."""
    entries = []
    for source in sources:
        cpp = Path(source).suffix == '.cpp'
        arguments = ['clang++' if cpp else 'clang', '-std=c++23' if cpp else '-std=c11', '-DNDEBUG',
                     f'-I{ROOT}', f'-I{ROOT / "rt1170"}', f'-I{ROOT / "rt1170" / "platform"}', *extra, '-c', str(source)]
        entries.append({'directory': str(directory), 'file': str(source), 'arguments': arguments})
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'compile_commands.json').write_bytes(json.dumps(entries, indent=2).encode('utf-8'))


def run_script(*arguments):
    return subprocess.run([sys.executable, str(SCRIPT), *arguments], capture_output=True, text=True, encoding='utf-8',
                          errors='replace', stdin=subprocess.DEVNULL)


@needs_clang_query
class FixtureTests(unittest.TestCase):
    """The matchers on the fixture: every form found by exactly its matchers, no other line found."""

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        build = Path(cls.temporary.name)
        write_database(build, (C_FIXTURE, CPP_FIXTURE))
        cls.report = check_c_status_truth.check(ROOT, build, CLANG_QUERY, jobs=2)
        cls.found = {}
        for finding in cls.report.findings:
            cls.found.setdefault((finding.path, finding.line), set()).update(finding.binds)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def assert_forms(self, source):
        path = repo_path(source)
        marked = marked_lines(source)
        self.assertGreater(len(marked), 10, path)
        for line, binds in marked.items():
            with self.subTest(path=path, line=line):
                found = self.found.get((path, line), set())
                if binds == {'any'}:
                    self.assertTrue(found, f'{path}:{line} is not found')
                else:
                    self.assertEqual(found, set(binds), f'{path}:{line}')

    def test_both_units_are_checked(self):
        self.assertEqual(self.report.failures, [])
        self.assertEqual(self.report.units, 2)

    def test_each_c_form_is_found_by_its_matchers(self):
        self.assert_forms(C_FIXTURE)

    def test_each_cpp_form_is_found_by_its_matchers(self):
        self.assert_forms(CPP_FIXTURE)

    def test_each_matcher_finds_its_form_in_c_and_in_cpp(self):
        for source in (C_FIXTURE, CPP_FIXTURE):
            path = repo_path(source)
            binds = set().union(*(found for (where, _), found in self.found.items() if where == path))
            for matcher in check_c_status_truth.MATCHERS:
                c_only = {'conditional-not-operand', 'conditional-logical-operand'}
                if (matcher.bind == 'status-seen' or (matcher.bind == 'boolean-comparison' and source == C_FIXTURE)
                        or (matcher.bind in c_only and source == CPP_FIXTURE)):
                    continue
                with self.subTest(path=path, matcher=matcher.bind):
                    self.assertIn(matcher.bind, binds)

    def test_each_status_operand_is_found_at_its_own_column(self):
        # Two statuses in one operation are two reads, and a failure names each place.
        for source, text in TWO_OPERAND_READS:
            lines = source.read_bytes().decode('utf-8').split('\n')
            numbers = [number for number, line in enumerate(lines, 1) if text in line]
            with self.subTest(path=repo_path(source), read=text):
                self.assertEqual(len(numbers), 1, f'the fixture holds {text!r} once')
                line = lines[numbers[0] - 1]
                start = line.index(text)
                columns = {start + text.index(operand) + 1 for operand in ('status', 'wfg_truth_fixture_probe')}
                found = {finding.column for finding in self.report.findings
                         if (finding.path, finding.line) == (repo_path(source), numbers[0])}
                self.assertEqual(found, columns)

    def test_an_assert_is_found_although_the_database_defines_ndebug(self):
        for source in (C_FIXTURE, CPP_FIXTURE):
            lines = source.read_bytes().decode('utf-8').split('\n')
            line = next(number for number, text in enumerate(lines, 1) if text.lstrip().startswith('assert('))
            with self.subTest(path=repo_path(source)):
                self.assertIn((repo_path(source), line), self.found)

    def test_no_read_that_is_no_truth_value_is_found(self):
        marked = {(repo_path(source), line) for source in (C_FIXTURE, CPP_FIXTURE) for line in marked_lines(source)}
        self.assertEqual(sorted(set(self.found) - marked), [])
        for source, reads in NEGATIVES.items():
            lines = source.read_bytes().decode('utf-8').split('\n')
            for read in reads:
                with self.subTest(path=repo_path(source), read=read):
                    numbers = [number for number, text in enumerate(lines, 1) if read in text]
                    self.assertTrue(numbers, f'the fixture lost the read {read!r}')
                    for number in numbers:
                        self.assertNotIn(number, marked_lines(source))
                        self.assertNotIn((repo_path(source), number), self.found)


@needs_clang_query
class FailureTests(unittest.TestCase):
    """What the check cannot check fails it: a unit, a matcher or a database."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / 'build'

    def write_unit(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode('utf-8'))
        return target

    def check(self, **options):
        return check_c_status_truth.check(self.root, self.build, CLANG_QUERY, jobs=1, **options)

    def test_a_unit_with_a_syntax_error_fails(self):
        unit = self.write_unit('rt1170/broken.c', '#include "wfg_status.h"\n\nint broken(void)\n{\n    int value = ;\n'
                               '    return value;\n}\n')
        write_database(self.build, (unit,))
        report = self.check()
        self.assertEqual(report.findings, [])
        self.assertEqual(len(report.failures), 1, report.failures)
        self.assertIn('rt1170/broken.c', report.failures[0])
        self.assertIn('expected expression', report.failures[0])

    def test_a_unit_with_a_missing_header_fails(self):
        unit = self.write_unit('tests/missing.cpp', '#include "wfg_no_such_header.hpp"\n\nint missing()\n{\n    return 0;\n}\n')
        write_database(self.build, (unit,))
        report = self.check()
        self.assertEqual(len(report.failures), 1, report.failures)
        self.assertIn('tests/missing.cpp', report.failures[0])
        self.assertIn('wfg_no_such_header.hpp', report.failures[0])

    def test_a_warning_the_database_makes_an_error_does_not_fail_the_unit(self):
        unit = self.write_unit('rt1170/warned.c', '#include "wfg_status.h"\nint warned(enum wfg_status status)\n'
                               '{\n    int unused;\n    return status == kWFG_StatusOk;\n}\n')
        write_database(self.build, (unit,), extra=('-Wall', '-Werror'))
        report = self.check()
        self.assertEqual(report.failures, [])
        self.assertEqual(report.units, 1)

    def test_a_matcher_that_does_not_parse_fails(self):
        unit = self.write_unit('rt1170/clean.c', '#include "wfg_status.h"\n\nint clean(enum wfg_status status)\n{\n'
                               '    return status == kWFG_StatusOk;\n}\n')
        write_database(self.build, (unit,))
        self.assertEqual(self.check().failures, [])
        for index in (0, len(check_c_status_truth.MATCHERS) - 1):
            matchers = list(check_c_status_truth.MATCHERS)
            matchers[index] = matchers[index]._replace(query=matchers[index].query.replace('(', 'Typo(', 1))
            with self.subTest(matcher=matchers[index].bind):
                report = self.check(matchers=tuple(matchers))
                self.assertEqual(len(report.failures), 1, report.failures)
                self.assertIn('rt1170/clean.c', report.failures[0])
                self.assertIn('Typo', report.failures[0])

    def test_an_empty_database_fails(self):
        self.build.mkdir()
        (self.build / 'compile_commands.json').write_bytes(b'[]\n')
        report = self.check()
        self.assertEqual(report.units, 0)
        self.assertEqual(len(report.failures), 1, report.failures)
        self.assertIn('no owned translation unit', report.failures[0])

    def test_a_database_of_units_outside_the_owned_directories_fails(self):
        units = (self.write_unit('third_party/lib/vendor.c', 'int vendor(void)\n{\n    return 0;\n}\n'),
                 self.write_unit('build/generated.c', 'int generated(void)\n{\n    return 0;\n}\n'))
        write_database(self.build, units)
        report = self.check()
        self.assertEqual(report.units, 0)
        self.assertIn('no owned translation unit', ' '.join(report.failures))

    def test_a_database_whose_units_never_expose_wfg_status_fails_closed(self):
        unit = self.write_unit('rt1170/platform/independent.c', 'int independent(void) { return 0; }\n')
        write_database(self.build, (unit,))
        report = self.check()
        self.assertIn('no checked translation unit exposes a function that returns or accepts enum wfg_status',
                      ' '.join(report.failures))

    def test_a_syntactically_valid_status_type_typo_fails_closed(self):
        unit = self.write_unit('rt1170/platform/use.c', '#include "wfg_status.h"\n'
                               'int use(enum wfg_status status) { return status == kWFG_StatusOk; }\n')
        write_database(self.build, (unit,))
        sentinel = next(matcher for matcher in check_c_status_truth.MATCHERS if matcher.bind == 'status-seen')
        typo = sentinel._replace(query=sentinel.query.replace('wfg_status', 'wfg_status_typo'))
        report = self.check(matchers=(typo,))
        self.assertIn('status matchers could not be verified', ' '.join(report.failures))

    def test_the_script_exits_1_on_a_truth_read_0_on_a_clean_unit_and_2_when_it_cannot_check(self):
        unit = self.write_unit('rt1170/platform/use.c', '#include "wfg_status.h"\n\nint use(enum wfg_status status)\n{\n'
                               '    return status == kWFG_StatusOk;\n}\n')
        write_database(self.build, (unit,))
        arguments = ('--root', str(self.root), '--build-dir', str(self.build), '--clang-query', CLANG_QUERY, '--jobs', '1')
        clean = run_script(*arguments)
        self.assertEqual(clean.returncode, 0, clean.stdout + clean.stderr)
        self.assertIn('0 C statuses read as a truth value in 1 owned translation units', clean.stdout)
        unit.write_bytes(b'#include "wfg_status.h"\n\nint use(enum wfg_status status)\n{\n    return !status;\n}\n')
        found = run_script(*arguments)
        self.assertEqual(found.returncode, 1, found.stdout + found.stderr)
        self.assertIn('rt1170/platform/use.c:5:13:', found.stdout)
        self.assertIn('(not-operand)', found.stdout)
        self.assertIn('1 C statuses read as a truth value', found.stdout)
        (self.build / 'compile_commands.json').write_bytes(b'[]\n')
        empty = run_script(*arguments)
        self.assertEqual(empty.returncode, 2, empty.stdout + empty.stderr)
        self.assertIn('no owned translation unit', empty.stdout + empty.stderr)


# A unit with one truth read on line 5 (column 13 in C, 12 in C++), and the firmware's stack check's GCC flags.
C_TRUTH_UNIT = '#include "wfg_status.h"\n\nint read_c(enum wfg_status status)\n{\n    return !status;\n}\n'
CPP_TRUTH_UNIT = '#include "wfg_status.h"\n\nbool read_cpp(wfg_status status)\n{\n    return !status;\n}\n'
GCC_ONLY = ('-fstack-usage', '-fcallgraph-info=su')


@needs_clang_query
class FirmwareDatabaseTests(unittest.TestCase):
    """A firmware database: the stack check's GCC flags, the target a compiler's name gives, --extra-arg."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        self.include = f'-I{ROOT / "rt1170" / "platform"}'

    def write_unit(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode('utf-8'))
        return target

    def write_entries(self, entries):
        written = json.dumps(entries, indent=2).encode('utf-8')
        (self.build / 'compile_commands.json').write_bytes(written)
        return written

    def query_directly(self, unit):
        """clang-query itself on the database as it is, with no help from the check."""
        completed = subprocess.run([CLANG_QUERY, '-p', str(self.build), '-c', 'match enumDecl()', str(unit)], capture_output=True,
                                   text=True, encoding='utf-8', errors='replace', stdin=subprocess.DEVNULL)
        return completed.stdout + completed.stderr

    def places(self, report):
        return {(finding.path, finding.line) for finding in report.findings}

    def test_the_stack_checks_gcc_flags_are_dropped_from_a_command_string_and_an_arguments_list(self):
        c_unit = self.write_unit('rt1170/flags.c', C_TRUTH_UNIT)
        cpp_unit = self.write_unit('tests/flags.cpp', CPP_TRUTH_UNIT)
        command = ' '.join(('gcc', '-std=c11', self.include, *GCC_ONLY, '-c', str(c_unit)))
        arguments = ['g++', '-std=c++23', self.include, *GCC_ONLY, '-c', str(cpp_unit)]
        written = self.write_entries([{'directory': str(self.build), 'command': command, 'file': str(c_unit)},
                                      {'directory': str(self.build), 'arguments': arguments, 'file': str(cpp_unit)}])
        for unit in (c_unit, cpp_unit):
            with self.subTest(unit=unit.name):
                self.assertIn("unknown argument: '-fcallgraph-info=su'", self.query_directly(unit))
        report = check_c_status_truth.check(self.root, self.build, CLANG_QUERY, jobs=1)
        self.assertEqual(report.failures, [])
        self.assertEqual(self.places(report), {('rt1170/flags.c', 5), ('tests/flags.cpp', 5)})
        self.assertIn('without the GCC-only flags -fcallgraph-info=su, -fstack-usage (2 compile commands)', report.notes)
        self.assertEqual((self.build / 'compile_commands.json').read_bytes(), written)
        run = run_script('--root', str(self.root), '--build-dir', str(self.build), '--clang-query', CLANG_QUERY, '--jobs', '1')
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn('through a copy without the GCC-only flags -fcallgraph-info=su, -fstack-usage (2 compile commands)',
                      run.stdout)
        self.assertEqual((self.build / 'compile_commands.json').read_bytes(), written)

    def test_the_target_a_compilers_name_gives_is_passed_on(self):
        arm = '#ifndef __arm__\n#error "not read for the Arm target"\n#endif\n'
        named = self.write_unit('rt1170/named.c', arm + C_TRUTH_UNIT)
        given = self.write_unit('rt1170/given.c', arm + C_TRUTH_UNIT)
        given_in_command = self.write_unit('rt1170/given_in_command.c', arm + C_TRUTH_UNIT)
        # A command string's compiler is quoted when its path holds a space.
        compiler = f'"{self.root / "arm gnu" / "bin" / "arm-none-eabi-gcc.exe"}"'
        command = ' '.join((compiler, '-std=c11', self.include, '-c', str(named)))
        arguments = ['arm-none-eabi-gcc', '--target=arm-none-eabi', '-std=c11', self.include, '-c', str(given)]
        given_command = ' '.join(('arm-none-eabi-gcc', '--target=arm-none-eabi', '-std=c11', self.include, '-c',
                                  str(given_in_command)))
        self.write_entries([{'directory': str(self.build), 'command': command, 'file': str(named)},
                            {'directory': str(self.build), 'arguments': arguments, 'file': str(given)},
                            {'directory': str(self.build), 'command': given_command, 'file': str(given_in_command)}])
        self.assertIn('not read for the Arm target', self.query_directly(named))
        report = check_c_status_truth.check(self.root, self.build, CLANG_QUERY, jobs=1)
        self.assertEqual(report.failures, [])
        self.assertEqual(self.places(report), {('rt1170/named.c', 8), ('rt1170/given.c', 8), ('rt1170/given_in_command.c', 8)})
        self.assertEqual(report.notes, "with --target=arm-none-eabi from the compiler's name (1 compile commands)")

    def test_an_arguments_list_arm_compiler_gets_its_implicit_target(self):
        arm = '#ifndef __arm__\n#error "not read for the Arm target"\n#endif\n'
        unit = self.write_unit('rt1170/arguments_named.c', arm + C_TRUTH_UNIT)
        self.write_entries([{'directory': str(self.build), 'arguments':
                             ['arm-none-eabi-gcc', '-std=c11', self.include, '-c', str(unit)], 'file': str(unit)}])
        report = check_c_status_truth.check(self.root, self.build, CLANG_QUERY, jobs=1)
        self.assertEqual(report.failures, [])
        self.assertEqual(self.places(report), {('rt1170/arguments_named.c', 8)})
        self.assertEqual(report.notes, "with --target=arm-none-eabi from the compiler's name (1 compile commands)")

    def test_assembly_sources_are_not_owned_translation_units(self):
        assembly = self.write_unit('rt1170/platform/owned.s', '.text\n')
        self.assertFalse(check_c_status_truth.is_owned_unit(self.root, assembly))

    def test_an_extra_argument_reaches_clang_query(self):
        unit = self.write_unit('rt1170/extra.c', '#include "wfg_status.h"\n\nint extra(enum wfg_status status)\n{\n'
                               '#ifdef WFG_TRUTH_EXTRA\n    return !status;\n#else\n    return status == kWFG_StatusOk;\n'
                               '#endif\n}\n')
        write_database(self.build, (unit,))
        plain = check_c_status_truth.check(self.root, self.build, CLANG_QUERY, jobs=1)
        self.assertEqual((plain.failures, plain.findings, plain.notes), ([], [], ''))
        extra = check_c_status_truth.check(self.root, self.build, CLANG_QUERY, jobs=1, extra_arguments=('-DWFG_TRUTH_EXTRA',))
        self.assertEqual(extra.failures, [])
        self.assertEqual(self.places(extra), {('rt1170/extra.c', 6)})
        run = run_script('--root', str(self.root), '--build-dir', str(self.build), '--clang-query', CLANG_QUERY, '--jobs', '1',
                         '--extra-arg=-DWFG_TRUTH_EXTRA')
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn('rt1170/extra.c:6:13:', run.stdout)


def canned_output(*located, counts=None):
    """clang-query's output for one unit: a note per located match, then one count per matcher."""
    matcher_count = len(check_c_status_truth.MATCHERS)
    counts = counts if counts is not None else [len(located)] + [0] * (matcher_count - 1)
    text = ''.join(f'\nMatch #{index}:\n\n{where}: note: "{bind}" binds here\n    5 |     if (f())\n      |         ^~~\n'
                   for index, (where, bind) in enumerate(located, 1))
    return text + ''.join(f'{count} {"match" if count == 1 else "matches"}.\n' for count in counts)


class OutputTests(unittest.TestCase):
    """How one unit's clang-query output is read, on canned output."""

    def setUp(self):
        self.root = Path(tempfile.gettempdir()).resolve() / 'wfg-truth-root'
        self.unit = check_c_status_truth.Unit(str(self.root / 'rt1170' / 'a.c'), str(self.root / 'build'))
        self.matcher_count = len(check_c_status_truth.MATCHERS)

    def read(self, returncode, stdout, stderr=''):
        return check_c_status_truth.read_unit(self.root, self.unit, returncode, stdout, stderr, self.matcher_count)

    def test_a_located_match_is_read(self):
        where = f'{self.root / "rt1170" / "a.c"}:5:9'
        result = self.read(0, canned_output((where, 'if-condition')))
        self.assertEqual(result.failures, [])
        self.assertEqual(result.matches, [check_c_status_truth.Match('rt1170/a.c', 5, 9, 'if-condition')])

    def test_a_relative_location_is_read_against_the_units_compile_directory(self):
        # clang prints a relative "file" of the database relative to its entry's directory, not to the repository.
        for directory, where in ((self.root, 'rt1170/a.c:5:9'), (self.root / 'build', '../rt1170/a.c:5:9'),
                                 (self.root / 'rt1170', 'a.c:5:9')):
            with self.subTest(directory=directory, where=where):
                unit = check_c_status_truth.Unit(str(self.root / 'rt1170' / 'a.c'), str(directory))
                result = check_c_status_truth.read_unit(self.root, unit, 0, canned_output((where, 'if-condition')), '',
                                                        self.matcher_count)
                self.assertEqual(result.failures, [])
                self.assertEqual(result.matches, [check_c_status_truth.Match('rt1170/a.c', 5, 9, 'if-condition')])

    def test_a_clean_unit_gives_nothing(self):
        result = self.read(0, canned_output(), 'warning: argument unused during compilation: \'-fno-plt\'\n')
        self.assertEqual(result, check_c_status_truth.UnitResult([], []))

    def test_a_non_zero_exit_fails_the_unit(self):
        result = self.read(1, canned_output())
        self.assertEqual(len(result.failures), 1, result.failures)
        self.assertIn('exited with 1', result.failures[0])

    def test_an_error_diagnostic_fails_the_unit_and_a_warning_does_not(self):
        for stderr in (f'{self.root / "rt1170" / "a.c"}:3:5: error: use of undeclared identifier \'x\'\n',
                       f'{self.root / "rt1170" / "a.c"}:1:10: fatal error: \'b.h\' file not found\n',
                       'error: unknown argument: \'-fcallgraph-info=su\'\n'):
            with self.subTest(stderr=stderr):
                result = self.read(0, canned_output(), stderr)
                self.assertEqual(len(result.failures), 1, result.failures)
                self.assertIn('could not parse', result.failures[0])
        excerpt = f'{self.root / "rt1170" / "a.c"}:3:5: warning: unused variable\n    3 |     puts("x: error: y");\n'
        self.assertEqual(self.read(0, canned_output(), excerpt).failures, [])

    def test_fewer_counts_than_matchers_fail_the_unit(self):
        result = self.read(0, canned_output(counts=[0] * (self.matcher_count - 1)))
        self.assertEqual(len(result.failures), 1, result.failures)
        self.assertIn(f'ran {self.matcher_count - 1} of {self.matcher_count} matchers', result.failures[0])

    def test_a_match_without_a_location_fails_the_unit(self):
        result = self.read(0, canned_output(counts=[1] + [0] * (self.matcher_count - 1)))
        self.assertEqual(len(result.failures), 1, result.failures)
        self.assertIn('without a location', result.failures[0])

    def test_the_versioned_clang_query_is_looked_for_first(self):
        # ubuntu-24.04 links an unversioned clang tool to LLVM 18 beside the 19 the host job installs.
        self.assertEqual(check_c_status_truth.CLANG_QUERY_NAMES, ('clang-query-19', 'clang-query'))

    def test_one_place_found_through_two_units_is_one_finding(self):
        header = 'rt1170/platform/media.h'
        results = [check_c_status_truth.UnitResult([check_c_status_truth.Match(header, 7, 9, 'if-condition')], []),
                   check_c_status_truth.UnitResult([check_c_status_truth.Match(header, 7, 9, 'bool-conversion'),
                                                    check_c_status_truth.Match('tests/b.cpp', 3, 1, 'not-operand')], [])]
        findings = check_c_status_truth.collect(results)
        self.assertEqual([(finding.path, finding.line, finding.column, finding.binds) for finding in findings],
                         [(header, 7, 9, ('bool-conversion', 'if-condition')), ('tests/b.cpp', 3, 1, ('not-operand',))])


if __name__ == '__main__':
    unittest.main()
