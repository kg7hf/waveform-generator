#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Cyclomatic complexity ratchet over the owned C and C++ sources (lizard, CCN 25).

The long-term target is to gate every function at CCN 25. Some functions are already
above it (live_protocol::parse is the largest), so the gate is a ratchet
against scripts/complexity-baseline.json, which lists every function above the
limit with its CCN:

- a function above the limit that the baseline does not list fails (a new one,
  or one that grew past the limit);
- a listed function whose CCN grew fails; staying equal or falling passes;
- a listed function that lizard no longer measures at all fails: lizard may
  have lost the part of its file after a construct it misreads, while the
  file keeps enough functions for the rule below (#49: one C array with an
  initialiser late in rt1170/player.cpp hid run_player). A function removed
  or renamed is dropped with --update, so its removal shows in the
  baseline's diff;
- the average CCN over all measured functions must stay below 6;
- a measured file must yield about one function per 100 lines of code (at
  least NLOC // 100 functions, NLOC as lizard counts it), unless the baseline
  lists it under "sparse_files" with why (a header of declarations only, a
  data table). Fewer means lizard has lost track of the file: a construct it
  misreads hides every function after it, and the gate above cannot see
  them (#49, rt1170/player.cpp);
- a measured file may not yield fewer functions than the baseline records for
  it under "file_functions": a loss smaller than the rule above allows (one C
  array with an initialiser late in rt1170/player.cpp hid 47 of its 164
  functions) shows here, even where no listed function is among the lost. A
  recorded file that is still in the tree (on disk, or in the commit's tree
  with --rev) but missing from lizard's file table fails too: every function
  in it would lose its floor unseen. A file that gained functions, a file not
  yet recorded and a recorded file gone from the tree are reported, and
  --update records them (a file it no longer measures is dropped).

A function is identified by its file and lizard's qualified name, so a moved or
renamed function counts as new: edit the baseline in the same commit and say
why. --update rewrites the baseline from the current sources, and only when the
check passes (apart from listed functions no longer measured, which it drops,
and files below their recorded count, whose count it resets), so it can lower
or drop entries but never admit a new one. Removing or merging functions
lowers a file's count: run --update in the same commit, and the baseline's diff
shows it.

lizard's counts depend on its version, so the baseline records the version it
was made with and the check refuses another (install it with
`pip install lizard==<version>`, as the host CI does). The sources are the work
tree's by default; --rev measures a commit's tree instead (via git archive),
which leaves uncommitted edits out, against that commit's own baseline (git
show), so each commit is checked as it was made; --baseline names another
file. --update rewrites the work tree's baseline and takes no --rev. Tests and
vendored code are not measured.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

MEASURED_PATHS = ('channel-sim/cli', 'channel-sim/core', 'channel-sim/rng', 'common', 'examples', 'host/capture',
                  'host/render', 'rt1170', 'rt1064', 'signal-lab/include', 'signal-lab/src', 'waveform-source')
CCN_LIMIT = 25
AVERAGE_CCN_LIMIT = 6.0
# A file needs at least one function per this many lines of code (NLOC).
NLOC_PER_FUNCTION = 100
# The columns of lizard's per-file table, as the pinned version prints them.
FILE_TABLE_COLUMNS = ['NLOC', 'Avg.NLOC', 'AvgCCN', 'Avg.token', 'function_cnt', 'file']
BASELINE = Path(__file__).resolve().with_name('complexity-baseline.json')
# The baseline's path in a commit's tree, which --rev reads.
BASELINE_IN_TREE = 'scripts/complexity-baseline.json'


@dataclass(frozen=True)
class Function:
    file: str
    name: str
    line: int
    ccn: int

    @property
    def key(self) -> tuple[str, str]:
        return self.file, self.name


@dataclass(frozen=True)
class FileCount:
    file: str
    nloc: int
    functions: int


@dataclass(frozen=True)
class Measurement:
    functions: list[Function]
    files: list[FileCount]


def lizard_version() -> str:
    result = subprocess.run([sys.executable, '-m', 'lizard', '--version'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('lizard is not installed for this Python: ' + (result.stderr.strip() or 'no output'))
    return result.stdout.strip()


def run_lizard(source_root: Path, arguments: list[str]) -> str:
    # -i -1: exit 0 whatever the warnings, so a non-zero exit is a real failure.
    result = subprocess.run([sys.executable, '-m', 'lizard', '-l', 'cpp', '-i', '-1', *arguments],
                            cwd=source_root, capture_output=True, text=True, encoding='utf-8',
                            env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
    if result.returncode:
        raise RuntimeError(f'lizard failed: {result.stderr.strip()}')
    return result.stdout


def file_counts(report: str) -> list[FileCount]:
    """The per-file table of lizard's default report: NLOC, Avg.NLOC, AvgCCN, Avg.token, function_cnt, file."""
    lines = report.splitlines()
    header = next((index for index, line in enumerate(lines) if line.split()[:1] == ['NLOC'] and 'function_cnt' in line), None)
    if header is None:
        raise RuntimeError("lizard's report has no file table")
    if lines[header].split() != FILE_TABLE_COLUMNS:
        raise RuntimeError(f"lizard's file table has the columns {lines[header].split()}, not {FILE_TABLE_COLUMNS}")
    files = []
    for line in lines[header + 1:]:
        if line.startswith('='):
            break
        fields = line.split(None, 5)
        if len(fields) == 6 and fields[0].isdigit() and fields[4].isdigit():
            files.append(FileCount(file=fields[5].strip().replace('\\', '/'), nloc=int(fields[0]), functions=int(fields[4])))
    return files


def measure(source_root: Path) -> Measurement:
    """Every function lizard finds below the measured paths that exist, and each file's lines of code and functions."""
    paths = [path for path in MEASURED_PATHS if (source_root / path).is_dir()]
    if not paths:
        raise RuntimeError(f'none of the measured paths exists below {source_root}')
    functions = []
    # Columns: NLOC, CCN, token, PARAM, length, location, file, function name, long name, start, end.
    for row in csv.reader(io.StringIO(run_lizard(source_root, ['--csv', *paths]))):
        functions.append(Function(file=row[6].replace('\\', '/'), name=row[7], line=int(row[9]), ccn=int(row[1])))
    if not functions:
        raise RuntimeError('lizard found no functions')
    files = file_counts(run_lizard(source_root, paths))
    if not files:
        raise RuntimeError('lizard reported no file')
    return Measurement(functions=functions, files=files)


def measure_revision(root: Path, rev: str) -> Measurement:
    listed = subprocess.run(['git', '-C', str(root), 'ls-tree', '--name-only', rev, '--', *MEASURED_PATHS],
                            capture_output=True, text=True)
    if listed.returncode or not listed.stdout.splitlines():
        raise RuntimeError(f'git ls-tree found no measured path in {rev}: {listed.stderr.strip()}')
    archive = subprocess.run(['git', '-C', str(root), 'archive', '--format=tar', rev, '--', *listed.stdout.splitlines()],
                             capture_output=True)
    if archive.returncode:
        raise RuntimeError(f'git archive failed: {archive.stderr.decode(errors="replace").strip()}')
    with tempfile.TemporaryDirectory(prefix='wfg-complexity-') as temporary:
        with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
            tar.extractall(temporary, filter='data')
        return measure(Path(temporary))


@dataclass(frozen=True)
class Baseline:
    lizard: str
    functions: dict[tuple[str, str], int]
    sparse_files: dict[str, str]  # file: why it has fewer functions than its lines of code suggest
    file_functions: dict[str, int]  # file: the functions lizard measured in it when the baseline was written


def parse_baseline(text: str, name: str) -> Baseline:
    data = json.loads(text)
    if data['ccn_limit'] != CCN_LIMIT:
        raise RuntimeError(f"{name} was made for CCN {data['ccn_limit']}, this check gates at {CCN_LIMIT}")
    sparse = data.get('sparse_files', {})
    if not isinstance(sparse, dict) or not all(isinstance(why, str) and why for why in sparse.values()):
        raise RuntimeError(f'{name}: sparse_files maps each file to why it is sparse')
    counts = data.get('file_functions', {})
    if not isinstance(counts, dict) or not all(type(count) is int and count >= 0 for count in counts.values()):
        raise RuntimeError(f'{name}: file_functions maps each file to the number of functions measured in it')
    return Baseline(lizard=data['lizard'], functions={(entry['file'], entry['function']): entry['ccn'] for entry in data['functions']},
                    sparse_files=sparse, file_functions=counts)


def read_baseline(path: Path) -> Baseline:
    return parse_baseline(path.read_text(encoding='utf-8'), path.name)


def revision_baseline(root: Path, rev: str) -> Baseline:
    """The baseline in `rev`'s own tree."""
    shown = subprocess.run(['git', '-C', str(root), 'show', f'{rev}:{BASELINE_IN_TREE}'], capture_output=True, text=True,
                           encoding='utf-8')
    if shown.returncode:
        raise RuntimeError(f'{rev} has no {BASELINE_IN_TREE}: {shown.stderr.strip()}')
    return parse_baseline(shown.stdout, f'{rev}:{BASELINE_IN_TREE}')


def sparse(files: list[FileCount], listed: dict[str, str]) -> tuple[list[str], list[str]]:
    """Files with fewer than one function per NLOC_PER_FUNCTION lines of code that the baseline does not list,
    and listed files that no longer need to be."""
    failures, improved = [], []
    for count in sorted(files, key=lambda count: count.file):
        expected = count.nloc // NLOC_PER_FUNCTION
        if count.functions >= expected:
            if count.file in listed:
                improved.append(f'{count.file}: listed in sparse_files, but its {count.functions} functions in {count.nloc} '
                                f'lines of code are enough')
            continue
        if count.file not in listed:
            failures.append(f'{count.file}: {count.functions} function(s) in {count.nloc} lines of code, fewer than one per '
                            f'{NLOC_PER_FUNCTION}: lizard has lost track of this file; restructure what it misreads, or '
                            f'list the file under sparse_files with why')
    measured = {count.file for count in files}
    for file in sorted(set(listed) - measured):
        improved.append(f'{file}: listed in sparse_files, but not measured (or gone)')
    return failures, improved


def present_files(root: Path, rev: str | None, files: set[str]) -> set[str]:
    """Which of `files` are still in the tree: in the commit's tree with `rev`, on disk otherwise. A tree git
    cannot list is an error, never an empty tree: that would make a recorded file lizard did not measure look
    gone, and pass."""
    if not files:
        return set()
    if rev:
        listed = subprocess.run(['git', '-C', str(root), 'ls-tree', '-r', '-z', '--name-only', rev, '--'],
                                capture_output=True, text=True, encoding='utf-8')
        if listed.returncode:
            raise RuntimeError(f'git ls-tree of {rev} failed: {listed.stderr.strip()}')
        return files & set(listed.stdout.split('\0'))
    return {file for file in files if (root / file).is_file()}


def file_floors(files: list[FileCount], recorded: dict[str, int],
                present: set[str] | frozenset[str] = frozenset()) -> tuple[list[str], list[str]]:
    """Files that yield fewer functions than the baseline records for them, or that lizard did not measure
    although they are still in the tree (`present`), and the counts --update would record: a file that
    gained functions, one not yet recorded, and a recorded file gone from the tree."""
    failures, changed = [], []
    for count in sorted(files, key=lambda count: count.file):
        floor = recorded.get(count.file)
        if floor is None:
            changed.append(f'{count.file}: {count.functions} functions, not yet in file_functions')
        elif count.functions < floor:
            failures.append(f'{count.file}: {count.functions} functions, fewer than the {floor} in file_functions: '
                            f'lizard may have lost part of this file; if functions were removed or merged, run --update')
        elif count.functions > floor:
            changed.append(f'{count.file}: {count.functions} functions, file_functions {floor}')
    measured = {count.file for count in files}
    for file in sorted(set(recorded) - measured):
        if file in present:
            failures.append(f'{file}: in file_functions and still in the tree, but lizard did not measure it: every '
                            f'function in it has lost its floor; if it is no longer meant to be measured, run --update')
        else:
            changed.append(f'{file}: in file_functions, but gone from the tree')
    return failures, changed


def over_limit(functions: list[Function]) -> dict[tuple[str, str], Function]:
    """The functions above the limit; of two with one key (overloads), the more complex."""
    worst: dict[tuple[str, str], Function] = {}
    for function in functions:
        if function.ccn > CCN_LIMIT and (function.key not in worst or function.ccn > worst[function.key].ccn):
            worst[function.key] = function
    return worst


def compare(current: dict[tuple[str, str], Function], baseline: dict[tuple[str, str], int],
            measured: set[tuple[str, str]]) -> tuple[list[str], list[str], list[str]]:
    """Failures, baseline entries that can be lowered or dropped, and baseline entries that name no measured
    function (`measured`: the key of every function lizard found)."""
    failures, improved, unmeasured = [], [], []
    for key, function in sorted(current.items()):
        where = f'{function.file}:{function.line}: {function.name}'
        if key not in baseline:
            failures.append(f'{where}: CCN {function.ccn}, above {CCN_LIMIT} and not in the baseline')
        elif function.ccn > baseline[key]:
            failures.append(f'{where}: CCN {function.ccn}, grew from {baseline[key]} (a function above '
                            f'{CCN_LIMIT} may not grow)')
        elif function.ccn < baseline[key]:
            improved.append(f'{where}: CCN {function.ccn}, baseline {baseline[key]}')
    for key, ccn in sorted(baseline.items()):
        if key in current:
            continue
        if key in measured:
            improved.append(f'{key[0]}: {key[1]}: no longer above {CCN_LIMIT}, baseline {ccn}')
        else:
            unmeasured.append(f'{key[0]}: {key[1]}: in the baseline at CCN {ccn}, but lizard no longer measures it: '
                              f'run --update if you removed or refactored it; otherwise lizard has lost track of '
                              f'part of the file')
    return failures, improved, unmeasured


def write_baseline(path: Path, version: str, current: dict[tuple[str, str], Function], sparse_files: dict[str, str],
                   files: list[FileCount]) -> None:
    # One entry per line, so a change to the baseline reads as a one-line diff.
    entries = [json.dumps({'file': function.file, 'function': function.name, 'ccn': function.ccn})
               for _, function in sorted(current.items())]
    listed = ','.join(f'\n    {entry}' for entry in entries)
    sparse_lines = ','.join(f'\n    {json.dumps(file)}: {json.dumps(why)}' for file, why in sorted(sparse_files.items()))
    count_lines = ','.join(f'\n    {json.dumps(count.file)}: {count.functions}' for count in sorted(files, key=lambda count: count.file))
    text = ('{\n'
            f'  "lizard": {json.dumps(version)},\n'
            f'  "ccn_limit": {CCN_LIMIT},\n'
            f'  "sparse_files": {{{sparse_lines}\n  }},\n'
            f'  "file_functions": {{{count_lines}\n  }},\n'
            f'  "functions": [{listed}\n  ]\n'
            '}\n')
    path.write_bytes(text.encode('utf-8'))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--rev', help='Measure this commit\'s tree (for example HEAD) instead of the work tree')
    parser.add_argument('--baseline', type=Path,
                        help=f'The baseline to read (default: {BASELINE.name} beside this script, or with --rev '
                             f'{BASELINE_IN_TREE} in that commit\'s tree)')
    parser.add_argument('--update', action='store_true',
                        help='Rewrite the baseline from the measured sources; only when the check passes, or to '
                             'create a missing baseline')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.update and args.rev:
        print('--update rewrites the work tree\'s baseline and takes no --rev', file=sys.stderr)
        return 2
    baseline_path = args.baseline or BASELINE
    from_revision = bool(args.rev) and args.baseline is None
    baseline_name = f'{args.rev}:{BASELINE_IN_TREE}' if from_revision else baseline_path.name
    try:
        version = lizard_version()
        measurement = measure_revision(root, args.rev) if args.rev else measure(root)
        functions = measurement.functions
        current = over_limit(functions)
        if args.update and not baseline_path.exists():
            write_baseline(baseline_path, version, current, {}, measurement.files)
            print(f'Wrote {baseline_path.name}: {len(current)} functions above CCN {CCN_LIMIT} (lizard {version}).')
            return 0
        recorded = revision_baseline(root, args.rev) if from_revision else read_baseline(baseline_path)
        unmeasured_files = set(recorded.file_functions) - {count.file for count in measurement.files}
        present = present_files(root, args.rev, unmeasured_files)
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f'Complexity check failed to run: {error}', file=sys.stderr)
        return 2
    if recorded.lizard != version:
        print(f'{baseline_name} was made with lizard {recorded.lizard}, this is lizard {version}; '
              f'install lizard=={recorded.lizard}', file=sys.stderr)
        return 2
    baseline = recorded.functions
    failures, improved, unmeasured = compare(current, baseline, {function.key for function in functions})
    sparse_failures, sparse_improved = sparse(measurement.files, recorded.sparse_files)
    failures += sparse_failures
    improved += sparse_improved
    below_floor, counts_changed = file_floors(measurement.files, recorded.file_functions, present)
    if not args.update:
        failures += unmeasured + below_floor
    average = sum(function.ccn for function in functions) / len(functions)
    if average >= AVERAGE_CCN_LIMIT:
        failures.append(f'average CCN {average:.2f} over {len(functions)} functions is not below {AVERAGE_CCN_LIMIT}')
    for line in failures:
        print('FAIL ' + line)
    if args.update:
        for line in unmeasured:
            print('drop ' + line)
        for line in below_floor:
            print('reset ' + line)
    for line in improved:
        print('lower ' + line)
    for line in counts_changed:
        print('count ' + line)
    where = f'tree of {args.rev}' if args.rev else 'work tree'
    print(f'Complexity ({where}, lizard {version}): {len(functions)} functions in {len(measurement.files)} files, '
          f'average CCN {average:.2f}; {len(current)} above {CCN_LIMIT}, baseline {len(baseline)}; '
          f'{len(recorded.sparse_files)} sparse files listed; {len(recorded.file_functions)} file counts recorded; '
          f'{len(failures)} failures.')
    if failures:
        return 1
    if args.update:
        write_baseline(baseline_path, version, current, recorded.sparse_files, measurement.files)
        print(f'Rewrote {baseline_path.name}: {len(current)} functions above CCN {CCN_LIMIT}, '
              f'{len(measurement.files)} file counts.')
    elif improved or counts_changed:
        print(f'Run with --update to bring {baseline_name} to the measured values.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
