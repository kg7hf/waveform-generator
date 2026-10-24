#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check, by the AST, that no owned code reads a C status as a truth value (#51).

An owned C function that can fail returns an enum wfg_status
(rt1170/platform/wfg_status.h), whose success, kWFG_StatusOk, is 0. Read as a
truth value, a status inverts silently: if (!f()) takes every success for a
failure and every failure for a success, and the compiler says nothing (the
enumeration has a zero enumerator, so clang-tidy's
bugprone-non-zero-enum-to-bool-conversion skips it). check-error-style.py
finds such a call by its text; this check reads what the compiler reads, so
it also finds the forms the text cannot see: a cast ((bool)f(),
static_cast<bool>(f())), a macro's expansion (assert(f())), extra
parentheses (if ((f()))), a for's condition, a stored status (enum wfg_status
s = f(); if (s)), a member, a typedef, a template, and a status explicitly
cast to an integer and then directly read as truth.

clang-query runs these matchers over each owned translation unit of a
compile database. Each binds its own name, which the finding gives as its
form. "status" is an expression whose type is enum wfg_status (wfg_status in
C++), compared by its canonical type, so a typedef or a using alias, C's
elaborated type (enum wfg_status, which enumType() does not match) and a
const status all count; each matcher looks through parentheses and implicit
conversions to it:

- bool-conversion: the status converted to bool implicitly (the matcher of
  #51). In C++ every truth read is one: the condition of an if, a while, a
  for, a do and ?:, an operand of !, && and ||, a bool initialised,
  assigned, passed or returned. In C it is a _Bool (bool) initialised,
  assigned, passed or returned;
- bool-cast: the status cast to bool, _Bool in C: (bool)s, (_Bool)s, bool(s),
  static_cast<bool>(s);
- if-condition, while-condition, do-condition, for-condition and
  conditional-condition: the status as the condition of an if, a while, a
  do, a for or ?:. In C a condition is no conversion to bool, so only these
  find it there;
- not-operand and logical-operand: the status as the operand of !, or of &&
  or ||, which C reads without a conversion to bool too;
- bitwise-operand: the status as an operand of &, |, ^, &=, |= or ^=, each
  operand on its own. A status is no set of bits, so this is the old bool
  returns' accumulation (ok &= f(), ok = ok & f()), which with kWFG_StatusOk
  0 is false on success; C and C++ promote the status to int there, so no
  matcher above sees it;
- boolean-comparison: a C++ status compared with the bool literals true or
  false. C spells these as integer macros, indistinguishable from an intentional
  comparison with 0 or 1 in the AST, so the matcher does not guess in C;
- conditional-branch: each status arm of a conditional expression used
  directly as an if, while, do or for condition. C may promote the result to
  int, hiding the status from the ordinary condition matcher;
- conditional-not-operand and conditional-logical-operand: each status arm
  when the conditional result is directly used as !, && or || operand;
- conditional-bool-conversion: each status arm when the promoted conditional
  result is implicitly converted to bool/_Bool, including assignment, argument
  and return contexts;
- integer-cast-truth: a status explicitly cast to an integer and directly
  read as truth in a condition, logical operand or bool conversion;
- integer-cast-conditional-branch: an integer-cast status arm when its
  conditional result is directly read as truth;
- status-seen: a hidden sentinel requiring a function declaration that returns
  or accepts the anchored enum. It is excluded from findings and makes a
  semantic type-anchor typo fail closed.

So in C++ a read is found by bool-conversion and by the matcher of its form
(a bit operation by bitwise-operand alone), in C by the matcher of its form.
A status compared first (s !=
kWFG_StatusOk), read through status_of() (status_of(f()).is_ok()), passed
on, stored or switched on is no truth read. An explicit integer cast followed
directly by a truth read is found, but arbitrary values carried through a
separate integer variable are not tracked. GNU's ?: without a middle operand
is not matched: owned code builds with -Wpedantic -Werror, which refuses it.

Every owned unit of the database is checked: each source under the owned
top-level directories (check-style.py's) that the database compiles, with
every compile command it has for it (clang-query parses a source once per
command, so each variant of its macros is read). A status can be read in any
unit that sees wfg_status.h, and choosing the units by their text would
bring back the text gate's blind spots; a unit that never sees the
enumeration costs one parse and matches nothing. NDEBUG is undefined
(--extra-arg=-UNDEBUG), so an assert() in a release build is read as well,
and -Wno-error keeps a clang warning that the build's -Werror would make an
error a warning: the unit still parses whole, and warnings are clang-tidy's
to report. A place found through several units (a header) is reported once.

The check fails on a match, and it fails, as unable to check, when
clang-query exits with a failure (a matcher it cannot parse ends its run
there), prints an error (a unit with an error still parses to an AST and
matches, so its exit status says nothing), runs fewer matchers than it was
given, or counts a match it gives no location for, and when no owned unit
was checked (no database, an empty one, or none of its units owned).

--build-dir names the build whose compile_commands.json is read, by default
build/host-release, which `cmake --preset host-release` writes. The host
database covers the owned C sources the host tests compile, the C++ that
calls them there, and player.cpp and usb_service.cpp through the tests that
include them, under the host's macros. The firmware-only units (main.cpp,
player.cpp, usb_service.cpp, tx_usb.cpp, os/task.cpp, the platform's board
sources, usb_audio_tinyusb.c, and another image's sdcard_probe.cpp and
tone.cpp), and the firmware's macros (the WFG_PLAYER_IMAGE branches), are in
a firmware database only, which a hand run reads (docs/coding-standard.md):
--build-dir names that build, and each --extra-arg (repeatable) is passed to
clang-query after the unit's own flags, as the Arm GNU toolchain's include
directories are. A firmware database carries the post-link stack check's GCC
flags (scripts/check-stack-usage.py): -fcallgraph-info=su, which clang
rejects as an unknown argument, and -fstack-usage, which only asks for a .su
file; and its compiler is named for its target (arm-none-eabi-g++), which
clang-tidy reads the target from and clang-query does not. So a database
whose compile commands carry either flag, in a command string or an
arguments list, or name a compiler so, is read through a copy in the
temporary directory without the flags and with --target=<the name's target>
(unless the command gives one); the database itself is left as it is, and
the output says what the copy changed in how many compile commands.

--clang-query names the executable (default: clang-query-19, else
clang-query, on PATH: the versioned name first, since an unversioned clang
tool can be an older LLVM, as ubuntu-24.04's clang-tidy is 18). --jobs runs
that many clang-query processes at once. Exit status 0: none found; 1:
findings, one per line as path:line:column: code <- the forms; 2: the check
could not run or could not check a unit.
"""

from __future__ import annotations

import argparse
from collections.abc import Iterable, Sequence
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import NamedTuple

_STYLE = Path(__file__).resolve().with_name('check-style.py')
_spec = importlib.util.spec_from_file_location('check_style', _STYLE)
check_style = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_style)

UNIT_EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx')
DEFAULT_BUILD_DIR = 'build/host-release'
CLANG_QUERY_NAMES = ('clang-query-19', 'clang-query')
TRUTH = 'a C status read as a truth value'
TRUTH_WHY = ('kWFG_StatusOk is 0, so this reads a success as a failure; compare with kWFG_StatusOk, or read it in C++ '
             'through status_of()')


class Matcher(NamedTuple):
    bind: str  # the name its match binds, given as the finding's form
    query: str  # the clang-query matcher, which names the status as "status"


# An expression of the C status's type, by its canonical type (a typedef, C's elaborated type and const included).
STATUS_TYPE = 'hasCanonicalType(hasDeclaration(enumDecl(hasName("wfg_status"))))'
STATUS = f'expr(hasType({STATUS_TYPE}))'


def _status(bind: str) -> str:
    """The status, through parentheses and implicit conversions, bound as `bind`."""
    return f'ignoringParenImpCasts(status.bind("{bind}"))'


def _condition(statement: str, bind: str) -> Matcher:
    return Matcher(bind, f'{statement}(hasCondition({_status(bind)}))')


def _conditional_branch(bind: str) -> str:
    return ('conditionalOperator('
            f'eachOf(hasTrueExpression({_status(bind)}), hasFalseExpression({_status(bind)})))')


def _conditional_statement(statement: str, bind: str) -> str:
    return f'{statement}(hasCondition(ignoringParenImpCasts({_conditional_branch(bind)})))'


def _integer_cast_status(bind: str) -> str:
    return f'explicitCastExpr(hasDestinationType(isInteger()), hasSourceExpression({_status(bind)}))'


def _conditional_integer_cast_branch(bind: str) -> str:
    cast = f'ignoringParenImpCasts({_integer_cast_status(bind)})'
    return f'conditionalOperator(eachOf(hasTrueExpression({cast}), hasFalseExpression({cast})))'


def _truth_contexts(expression: str) -> str:
    value = f'ignoringParenImpCasts({expression})'
    conditions = ', '.join(f'{statement}(hasCondition({value}))'
                           for statement in ('ifStmt', 'whileStmt', 'doStmt', 'forStmt', 'conditionalOperator'))
    return ('stmt(anyOf(' + conditions
            + f', unaryOperator(hasOperatorName("!"), hasUnaryOperand({value}))'
            + ', binaryOperator(hasAnyOperatorName("&&", "||"), '
            + f'eachOf(hasLHS({value}), hasRHS({value})))'
            + ', implicitCastExpr(hasImplicitDestinationType(booleanType()), hasSourceExpression('
            + f'{value}))))')


MATCHERS = (
    # Reuse the type anchor itself as a semantic sentinel: a misspelled status anchor
    # must fail closed even when every clang-query string remains syntactically valid.
    Matcher('status-seen',
            f'functionDecl(anyOf(returns({STATUS_TYPE}), hasAnyParameter(hasType({STATUS_TYPE}))))'
            '.bind("status-seen")'),
    Matcher('bool-conversion', 'implicitCastExpr(hasImplicitDestinationType(booleanType()), '
                               f'hasSourceExpression({_status("bool-conversion")}))'),
    Matcher('bool-cast',
            f'explicitCastExpr(hasDestinationType(booleanType()), hasSourceExpression({_status("bool-cast")}))'),
    _condition('ifStmt', 'if-condition'),
    _condition('whileStmt', 'while-condition'),
    _condition('doStmt', 'do-condition'),
    _condition('forStmt', 'for-condition'),
    _condition('conditionalOperator', 'conditional-condition'),
    Matcher('not-operand', f'unaryOperator(hasOperatorName("!"), hasUnaryOperand({_status("not-operand")}))'),
    Matcher('logical-operand',
            'binaryOperator(hasAnyOperatorName("&&", "||"), '
            f'eachOf(hasLHS({_status("logical-operand")}), hasRHS({_status("logical-operand")})))'),
    Matcher('boolean-comparison',
            'binaryOperator(hasAnyOperatorName("==", "!="), '
            f'eachOf(allOf(hasLHS({_status("boolean-comparison")}), '
            'hasRHS(ignoringParenImpCasts(cxxBoolLiteral()))), '
            f'allOf(hasRHS({_status("boolean-comparison")}), '
            'hasLHS(ignoringParenImpCasts(cxxBoolLiteral())))))'),
    Matcher('conditional-branch',
            'stmt(anyOf(' + ', '.join(_conditional_statement(statement, 'conditional-branch')
                                      for statement in ('ifStmt', 'whileStmt', 'doStmt', 'forStmt')) + '))'),
    Matcher('conditional-not-operand',
            'unaryOperator(hasOperatorName("!"), hasUnaryOperand(ignoringParenImpCasts('
            f'{_conditional_branch("conditional-not-operand")})))'),
    Matcher('conditional-logical-operand',
            'binaryOperator(hasAnyOperatorName("&&", "||"), '
            f'eachOf(hasLHS(ignoringParenImpCasts({_conditional_branch("conditional-logical-operand")})), '
            f'hasRHS(ignoringParenImpCasts({_conditional_branch("conditional-logical-operand")}))))'),
    Matcher('conditional-bool-conversion',
            'implicitCastExpr(hasImplicitDestinationType(booleanType()), hasSourceExpression('
            f'ignoringParenImpCasts({_conditional_branch("conditional-bool-conversion")})))'),
    Matcher('integer-cast-truth', _truth_contexts(_integer_cast_status('integer-cast-truth'))),
    Matcher('integer-cast-conditional-branch',
            _truth_contexts(_conditional_integer_cast_branch('integer-cast-conditional-branch'))),
    # A status is never a set of bits, so any bit operation on one is the old bool accumulation (ok &= f()).
    Matcher('bitwise-operand',
            'binaryOperator(hasAnyOperatorName("&", "|", "^", "&=", "|=", "^="), '
            f'eachOf(hasLHS({_status("bitwise-operand")}), hasRHS({_status("bitwise-operand")})))'),
)

# A match's location as clang-query prints it (set output diag), and the count each matcher ends with.
LOCATED = re.compile(r'^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): note: "(?P<bind>[^"]+)" binds here\s*$', re.M)
COUNT = re.compile(r'^(?P<count>\d+) match(?:es)?\.\s*$', re.M)
# An error diagnostic: a compiler's (path:line:column: error:, fatal error: too) or a driver's (error: ...). A source
# excerpt below a diagnostic starts with a space, so a line of code that holds the words is none.
ERROR = re.compile(r'^(?:[^\s].*?:\d+(?::\d+)?: )?(?:fatal )?error: ')
# How much of clang-query's output a failure quotes.
QUOTED_LINES = 8
# After each unit's own flags: assert() expands in a release build too, and a warning the build's -Werror makes an
# error (a clang warning GCC does not give) stays a warning, since the unit parses whole.
EXTRA_ARGUMENTS = ('-UNDEBUG', '-Wno-error')
# The post-link stack check's GCC flags, which every firmware compile command carries (see above): one argument, and
# in a command string one word after a space.
GCC_ONLY_FLAG = re.compile(r'-fcallgraph-info(?:=\S*)?|-fstack-usage')
GCC_ONLY_WORD = re.compile(rf'\s+({GCC_ONLY_FLAG.pattern})(?=\s|$)')
# A compiler named for its target, as the Arm GNU toolchain's arm-none-eabi-g++ is (".exe" dropped first), and a
# command that gives its target already. A command string's compiler is its first word, quoted or not.
TARGETED_COMPILER = re.compile(r'(?P<target>[A-Za-z0-9_.]+(?:-[A-Za-z0-9_.]+)+)'
                               r'-(?:gcc|g\+\+|cc|c\+\+|clang|clang\+\+)(?:-[0-9.]+)?')
TARGET_ARGUMENT = re.compile(r'--target(?:=.*)?|-target')
COMMAND_COMPILER = re.compile(r'\s*(?:"(?P<quoted>[^"]*)"|(?P<plain>\S+))')
COMMAND_TARGET = re.compile(r'\s(?:--target(?:=|\s)|-target\s)')


class Unit(NamedTuple):
    """An owned source of the database, and the directory of its first compile command (which a relative path in
    clang-query's output is relative to)."""
    path: str
    directory: str


class Match(NamedTuple):
    path: str  # repository-relative with "/", or as clang-query gave it outside the repository
    line: int
    column: int
    bind: str


class UnitResult(NamedTuple):
    matches: list[Match]
    failures: list[str]


class Finding(NamedTuple):
    path: str
    line: int
    column: int
    binds: tuple[str, ...]


class Report(NamedTuple):
    findings: list[Finding]
    failures: list[str]
    units: int  # the owned units checked
    commands: int  # their compile commands
    notes: str = ''  # what the copy of the database read changed, or '' when the database was read as it is


class PreparedEntry(NamedTuple):
    entry: dict  # the compile command as clang-query reads it
    dropped: tuple[str, ...]  # the GCC-only flags taken out of it
    target: str  # the target added to it, or ''


def commands(matchers: Sequence[Matcher]) -> list[str]:
    """clang-query's commands: each match gives its own bind names only, at their location."""
    return ['set bind-root false', 'set output diag', f'let status {STATUS}',
            *(f'match {matcher.query}' for matcher in matchers)]


def display_path(root: Path, path: Path) -> str:
    """The path relative to the repository with "/", or the absolute path outside it."""
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def is_owned_unit(root: Path, path: Path) -> bool:
    try:
        relative = path.relative_to(root)
    except ValueError:
        return False
    return (len(relative.parts) > 1 and relative.parts[0] in check_style.OWNED_SOURCE_DIRECTORIES
            and path.suffix.lower() in UNIT_EXTENSIONS)


def owned_units(root: Path, entries: Iterable[dict]) -> tuple[list[Unit], int]:
    """The database's owned sources, once each in path order, and the number of compile commands they have."""
    units: dict[str, Unit] = {}
    count = 0
    for entry in entries:
        directory = Path(entry['directory'])
        path = Path(os.path.normpath(directory / entry['file']))
        if is_owned_unit(root, path):
            count += 1
            units.setdefault(os.path.normcase(str(path)), Unit(str(path), str(directory)))
    return sorted(units.values()), count


def quoted(text: str) -> str:
    """The text's last lines on one line: clang-query ends with what stopped it."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    shown = '; '.join(lines[-QUOTED_LINES:])
    return (f'({len(lines) - QUOTED_LINES} lines before) ... ' if len(lines) > QUOTED_LINES else '') + shown


def read_unit(root: Path, unit: Unit, returncode: int, stdout: str, stderr: str, matcher_count: int) -> UnitResult:
    """One unit's matches, and why it was not checked if it was not: clang-query failed, printed an error, ran fewer
    matchers than it was given, or counted a match without a location."""
    name = display_path(root, Path(unit.path))
    failures = []
    if returncode != 0:
        output = '; '.join(text for text in (quoted(stdout), quoted(stderr)) if text)
        failures.append(f'{name}: clang-query exited with {returncode}: {output}')
    errors = [line for line in stderr.splitlines() if ERROR.match(line)]
    if errors:
        more = f' ... ({len(errors) - QUOTED_LINES} more errors)' if len(errors) > QUOTED_LINES else ''
        failures.append(f'{name}: clang-query could not parse it: {"; ".join(errors[:QUOTED_LINES])}{more}')
    matches = [Match(display_path(root, Path(os.path.normpath(Path(unit.directory) / match['path']))),
                     int(match['line']), int(match['column']), match['bind']) for match in LOCATED.finditer(stdout)]
    counts = [int(match['count']) for match in COUNT.finditer(stdout)]
    if not failures and len(counts) != matcher_count:
        failures.append(f'{name}: clang-query ran {len(counts)} of {matcher_count} matchers: {quoted(stdout)}')
    elif not failures and sum(counts) != len(matches):
        failures.append(f'{name}: clang-query counted {sum(counts)} matches, {sum(counts) - len(matches)} of them '
                        'without a location')
    return UnitResult(matches, failures)


def compiler_target(compiler: str) -> str:
    """The target a compiler's name gives (arm-none-eabi for .../arm-none-eabi-g++.exe), or ''."""
    name = re.split(r'[\\/]', compiler)[-1]
    if name.lower().endswith('.exe'):
        name = name[:-len('.exe')]
    named = TARGETED_COMPILER.fullmatch(name)
    return named['target'] if named else ''


def prepared_entry(entry: dict) -> PreparedEntry:
    """A compile command without the GCC-only flags and with the target its compiler's name gives, in either form a
    database writes it: an "arguments" list or a "command" string (CMake's)."""
    prepared = dict(entry)
    dropped: list[str] = []
    target = ''
    if isinstance(entry.get('arguments'), list):
        arguments = [str(argument) for argument in entry['arguments']]
        dropped += [argument for argument in arguments if GCC_ONLY_FLAG.fullmatch(argument)]
        arguments = [argument for argument in arguments if not GCC_ONLY_FLAG.fullmatch(argument)]
        if arguments and not any(TARGET_ARGUMENT.fullmatch(argument) for argument in arguments):
            target = compiler_target(arguments[0])
            if target:
                arguments.insert(1, f'--target={target}')
        prepared['arguments'] = arguments
    if isinstance(entry.get('command'), str):
        command = entry['command']
        dropped += [flag.group(1) for flag in GCC_ONLY_WORD.finditer(command)]
        command = GCC_ONLY_WORD.sub('', command)
        compiler = COMMAND_COMPILER.match(command)
        if not target and compiler and not COMMAND_TARGET.search(command):
            target = compiler_target(compiler['quoted'] if compiler['quoted'] is not None else compiler['plain'])
            if target:
                command = f'{command[:compiler.end()]} --target={target}{command[compiler.end():]}'
        prepared['command'] = command
    return PreparedEntry(prepared, tuple(dropped), target)


def prepared_database(entries: Sequence[dict]) -> tuple[list[dict], str]:
    """The compile commands clang-query reads, and what they changed ('' when nothing: the database is read as it is)."""
    prepared = [prepared_entry(entry) for entry in entries]
    dropped = sorted({flag for item in prepared for flag in item.dropped})
    filtered = sum(1 for item in prepared if item.dropped)
    targets = sorted({item.target for item in prepared if item.target})
    targeted = sum(1 for item in prepared if item.target)
    notes = []
    if filtered:
        notes.append(f'without the GCC-only flags {", ".join(dropped)} ({filtered} compile commands)')
    if targeted:
        named = ', '.join(f'--target={target}' for target in targets)
        notes.append(f"with {named} from the compiler's name ({targeted} compile commands)")
    return [item.entry for item in prepared], '; '.join(notes)


def run_unit(root: Path, build: Path, clang_query: str, unit: Unit, matchers: Sequence[Matcher],
             extra_arguments: Sequence[str] = ()) -> UnitResult:
    arguments = [clang_query, '-p', str(build),
                 *(f'--extra-arg={argument}' for argument in (*EXTRA_ARGUMENTS, *extra_arguments))]
    for command in commands(matchers):
        arguments += ['-c', command]
    completed = subprocess.run([*arguments, unit.path], stdin=subprocess.DEVNULL, capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
    return read_unit(root, unit, completed.returncode, completed.stdout, completed.stderr, len(matchers))


def collect(results: Iterable[UnitResult]) -> list[Finding]:
    """The matches of all units, one finding per place with every form found there, in path and line order."""
    places: dict[tuple[str, int, int], set[str]] = {}
    for result in results:
        for match in result.matches:
            if match.bind == 'status-seen':
                continue
            places.setdefault((match.path, match.line, match.column), set()).add(match.bind)
    return [Finding(path, line, column, tuple(sorted(binds))) for (path, line, column), binds in sorted(places.items())]


def check(root: Path, build: Path, clang_query: str, jobs: int, matchers: Sequence[Matcher] = MATCHERS,
          extra_arguments: Sequence[str] = ()) -> Report:
    database = build / 'compile_commands.json'
    try:
        entries = json.loads(database.read_bytes().decode('utf-8'))
    except (OSError, ValueError) as error:
        return Report([], [f'no compile database to read: {database}: {error}'], 0, 0)
    if not isinstance(entries, list):
        return Report([], [f'{database} is no compile database (a JSON list of compile commands)'], 0, 0)
    units, count = owned_units(root, entries)
    if not units:
        return Report([], [f'no owned translation unit in {database}: nothing was checked'], 0, 0)
    prepared, notes = prepared_database(entries)
    with tempfile.TemporaryDirectory(prefix='wfg-c-status-truth-') as temporary:
        read = build
        if notes:
            read = Path(temporary)
            (read / 'compile_commands.json').write_bytes(json.dumps(prepared, indent=2).encode('utf-8'))
        with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
            results = list(pool.map(lambda unit: run_unit(root, read, clang_query, unit, matchers, extra_arguments), units))
    if not any(result.failures for result in results) and any(matcher.bind == 'status-seen' for matcher in matchers) and not any(
            match.bind == 'status-seen' for result in results for match in result.matches):
        return Report([], ['no checked translation unit exposes a function that returns or accepts enum wfg_status; '
                           'status matchers could not be verified'],
                      len(units), count, notes)
    return Report(collect(results), [failure for result in results for failure in result.failures], len(units), count,
                  notes)


def source_line(root: Path, path: str, line: int) -> str:
    try:
        lines = (root / path).read_bytes().decode('utf-8', errors='replace').split('\n')
    except OSError:
        return ''
    return lines[line - 1].strip() if 0 < line <= len(lines) else ''


def clang_query_version(clang_query: str) -> str:
    completed = subprocess.run([clang_query, '--version'], stdin=subprocess.DEVNULL, capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
    found = re.search(r'version (\S+)', completed.stdout)
    return found.group(1) if found else 'of unknown version'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build-dir', type=Path, default=Path(DEFAULT_BUILD_DIR),
                        help=f'The build whose compile_commands.json is read (default {DEFAULT_BUILD_DIR}, below --root)')
    parser.add_argument('--clang-query',
                        help=f'The clang-query executable (default: {" or ".join(CLANG_QUERY_NAMES)} on PATH)')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1, help='clang-query processes at once')
    parser.add_argument('--extra-arg', action='append', default=[], dest='extra_arguments', metavar='ARGUMENT',
                        help="Passed to clang-query as an --extra-arg, after the unit's own flags (repeatable; write "
                             '--extra-arg=<value> for a value that starts with -)')
    args = parser.parse_args()
    root = args.root.resolve()
    build = (root / args.build_dir).resolve()
    clang_query = args.clang_query or next((found for found in map(shutil.which, CLANG_QUERY_NAMES) if found), None)
    if not clang_query or not shutil.which(clang_query):
        wanted = args.clang_query or ' or '.join(CLANG_QUERY_NAMES)
        print(f'C status truth check failed to run: no clang-query ({wanted})', file=sys.stderr)
        return 2
    try:
        version = clang_query_version(clang_query)
        report = check(root, build, clang_query, args.jobs, extra_arguments=args.extra_arguments)
    except OSError as error:
        print(f'C status truth check failed to run: {error}', file=sys.stderr)
        return 2
    for finding in report.findings:
        print(f'{finding.path}:{finding.line}:{finding.column}: {source_line(root, finding.path, finding.line)}  '
              f'<- {TRUTH} ({", ".join(finding.binds)}): {TRUTH_WHY}')
    for failure in report.failures:
        print(f'not checked: {failure}')
    if report.notes:
        print(f'C status truth read {display_path(root, build / "compile_commands.json")} through a copy {report.notes}.')
    print(f'C status truth (clang-query {version}, {display_path(root, build)}): {len(report.findings)} C statuses read '
          f'as a truth value in {report.units} owned translation units ({report.commands} compile commands), '
          f'{len(report.failures)} not checked.')
    if report.failures:
        return 2
    return 1 if report.findings else 0


if __name__ == '__main__':
    sys.exit(main())
