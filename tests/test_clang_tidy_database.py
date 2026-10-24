# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/clang-tidy.ps1 reads a compile database that carries GCC-only flags.

The firmware databases carry the post-link stack check's GCC flags,
-fstack-usage and -fcallgraph-info=su (scripts/check-stack-usage.py). clang
rejects -fcallgraph-info=su as an unknown argument, an error in every unit,
so the script must read such a database through a copy without them, in
either form a database writes a compile command: a "command" string (CMake's)
or an "arguments" list. Each test writes a database of one owned host source
with both flags, shows that clang-tidy itself fails on it, and runs the
script over it: it must pass and say which flags it dropped. -ExtraArgs must
reach clang-tidy, as the firmware hand run's include directories do: a value
that names a header no unit has fails a unit that passes without it. Needs
pwsh and clang-tidy 19: WFG_CLANG_TIDY names it (ctest sets it), else clang-tidy-19 or
clang-tidy on PATH (the versioned name first: ubuntu-24.04's clang-tidy is
18); without them the tests are skipped.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'clang-tidy.ps1'
GCC_ONLY = ('-fstack-usage', '-fcallgraph-info=su')


def find_clang_tidy():
    named = os.environ.get('WFG_CLANG_TIDY')
    if named:
        return named
    return next((found for found in map(shutil.which, ('clang-tidy-19', 'clang-tidy')) if found), None)


CLANG_TIDY = find_clang_tidy()
PWSH = shutil.which('pwsh')


def run_script(build, *arguments):
    return subprocess.run([PWSH, '-NoProfile', '-File', str(SCRIPT), '-BuildDir', str(build), '-ClangTidyPath', CLANG_TIDY,
                           '-Jobs', '1', *arguments], capture_output=True, text=True, encoding='utf-8', errors='replace',
                          stdin=subprocess.DEVNULL)


@unittest.skipUnless(CLANG_TIDY and PWSH, 'pwsh and clang-tidy are needed (WFG_CLANG_TIDY, or clang-tidy on PATH)')
class GccOnlyFlagTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)

    def write_database(self, entry):
        (self.build / 'compile_commands.json').write_bytes(json.dumps([entry], indent=2).encode('utf-8'))

    def assert_read_without_the_flags(self, source):
        written = (self.build / 'compile_commands.json').read_bytes()
        direct = subprocess.run([CLANG_TIDY, '-p', str(self.build), '--quiet', str(ROOT / source)], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', stdin=subprocess.DEVNULL)
        self.assertIn("unknown argument: '-fcallgraph-info=su'", direct.stdout + direct.stderr)
        run = run_script(self.build)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(f'in 1 translation units ({self.build}).', run.stdout)
        self.assertIn('without the GCC-only flags -fcallgraph-info=su, -fstack-usage (1 compile commands)', run.stdout)
        self.assertEqual((self.build / 'compile_commands.json').read_bytes(), written)

    def test_a_command_string_is_read_without_them(self):
        source = 'common/status.cpp'
        command = ' '.join(('g++', '-std=c++23', f'-I{ROOT}', *GCC_ONLY, '-O3', '-c', str(ROOT / source)))
        self.write_database({'directory': str(self.build), 'command': command, 'file': str(ROOT / source)})
        self.assert_read_without_the_flags(source)

    def test_an_arguments_list_is_read_without_them(self):
        source = 'common/host_critical_error.cpp'
        arguments = ['g++', '-std=c++23', f'-I{ROOT}', *GCC_ONLY, '-O3', '-c', str(ROOT / source)]
        self.write_database({'directory': str(self.build), 'arguments': arguments, 'file': str(ROOT / source)})
        self.assert_read_without_the_flags(source)


@unittest.skipUnless(CLANG_TIDY and PWSH, 'pwsh and clang-tidy are needed (WFG_CLANG_TIDY, or clang-tidy on PATH)')
class ExtraArgumentTests(unittest.TestCase):
    def test_an_extra_argument_reaches_clang_tidy(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            source = ROOT / 'common' / 'status.cpp'
            arguments = ['g++', '-std=c++23', f'-I{ROOT}', '-O3', '-c', str(source)]
            entry = {'directory': str(build), 'arguments': arguments, 'file': str(source)}
            (build / 'compile_commands.json').write_bytes(json.dumps([entry], indent=2).encode('utf-8'))
            plain = run_script(build)
            self.assertEqual(plain.returncode, 0, plain.stdout + plain.stderr)
            extra = run_script(build, '-ExtraArgs', '-includewfg_no_such_header.h')
            self.assertNotEqual(extra.returncode, 0, extra.stdout + extra.stderr)
            self.assertIn("'wfg_no_such_header.h' file not found", extra.stdout + extra.stderr)
            self.assertIn('clang-tidy reported errors in: common/status.cpp', extra.stdout + extra.stderr)


@unittest.skipUnless(CLANG_TIDY and PWSH, 'pwsh and clang-tidy are needed (WFG_CLANG_TIDY, or clang-tidy on PATH)')
class ExternalTreeFilterTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)

    def write_database(self, entries):
        (self.build / 'compile_commands.json').write_bytes(json.dumps(entries, indent=2).encode('utf-8'))

    def test_external_translation_units_are_excluded_case_insensitively(self):
        source = ROOT / 'common' / 'status.cpp'
        command = ['g++', '-std=c++23', f'-I{ROOT}', '-c', str(source)]
        entries = [{'directory': str(self.build), 'arguments': command, 'file': str(source)}]
        external_trees = ('EXTERNAL', 'ExTeRnAlS', 'Vendor', 'VeNdOrS', '_DePs', 'TinyUSB', 'NXP', 'STM', 'hfsimulator', 'PathSim', 'cAtCh2', 'third_party', 'BUILD')
        for index, tree in enumerate(external_trees):
            external_source = ROOT / 'common' / tree / f'not_owned_{index}.cpp'
            external_command = ['g++', '-std=c++23', f'-I{ROOT}', '-c', str(external_source)]
            entries.append({'directory': str(self.build), 'arguments': external_command, 'file': str(external_source)})
        self.write_database(entries)

        run = run_script(self.build)

        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('1 translation units', run.stdout)
        self.assertNotIn('not_owned_', run.stdout + run.stderr)

    def test_owned_headers_are_reported_but_external_vendor_headers_are_filtered(self):
        source = ROOT / 'common' / 'status.cpp'
        with tempfile.TemporaryDirectory(prefix='.clang-tidy-filter-', dir=ROOT / 'common') as temporary:
            fixture = Path(temporary)
            owned_header = fixture / 'owned_probe.hpp'
            owned_header.write_text('inline int wfg_owned_probe() { return 374829; }\n', encoding='utf-8')
            excluded_trees = ('EXTERNAL', 'ExTeRnAlS', 'Vendor', 'VeNdOrS', '_DePs', 'TinyUSB', 'NXP', 'STM', 'hfsimulator', 'PathSim', 'cAtCh2', 'third_party', 'BUILD')
            forced_headers = []
            for index, tree in enumerate(excluded_trees):
                directory = fixture / tree
                directory.mkdir()
                header = directory / f'vendor_probe_{index}.hpp'
                header.write_text(f'inline int wfg_vendor_probe_{index}() {{ return {483920 + index}; }}\n', encoding='utf-8')
                forced_headers.append(header)

            arguments = ['g++', '-std=c++23', f'-I{ROOT}', '-c']
            for header in (owned_header, *forced_headers):
                arguments.extend(('-include', str(header)))
            arguments.append(str(source))
            self.write_database([{'directory': str(self.build), 'arguments': arguments, 'file': str(source)}])

            run = run_script(self.build)

        output = run.stdout + run.stderr
        self.assertEqual(run.returncode, 0, output)
        self.assertIn('owned_probe.hpp', output)
        for index in range(len(excluded_trees)):
            self.assertNotIn(f'vendor_probe_{index}.hpp', output)


if __name__ == '__main__':
    unittest.main()
