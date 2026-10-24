#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check local dependency files and prepare vendor patches offline.

Every entry of dependencies.json names a submodule and the files the build
needs from it. An entry's "scope" says which build reads it: "firmware" (the
default, for entries that name none) or "host-test" (Catch2, which only host
test builds compile). The firmware configure, the only caller of --prepare,
checks the firmware inputs alone, so a checkout without a host-test dependency
still builds firmware; init and a run without --prepare check every entry.
Git records each submodule revision; this inventory checks only the local files
the build needs and does not duplicate a commit pin.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

SCOPES = ('firmware', 'host-test')


def git(root: Path, *args: str) -> str:
    result = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr.strip() or f'git {args[0]} failed')
    return result.stdout.strip()


def local_path(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    if path == root or not path.is_relative_to(root):
        raise ValueError(f'dependency path escapes its root: {relative}')
    return path


def check_file(root: Path, item: str) -> None:
    path = local_path(root, item)
    if not path.is_file():
        raise ValueError(f'missing dependency file: {path}')


def scope(dep: dict) -> str:
    """The build that reads the dependency: "firmware" unless the entry says otherwise."""
    value = dep.get('scope', 'firmware')
    if value not in SCOPES:
        raise ValueError(f"{dep['name']}: unknown scope {value!r}; expected one of {', '.join(SCOPES)}")
    return value


def check_dependency(root: Path, dep: dict) -> None:
    """Require the local inputs without adding a second revision lock."""
    scope(dep)
    destination = local_path(root, dep['destination'])
    for item in dep['files']:
        check_file(destination, item)
    if patch := dep.get('patch'):
        check_file(root, patch)
def prepare(root: Path, dep: dict, output: Path) -> None:
    """Copy only declared inputs, then apply the retained patch without touching Git."""
    destination = local_path(output, dep['name'])
    for item in dep['files']:
        target = local_path(destination, item)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(local_path(root / dep['destination'], item), target)
    patch = dep['patch']
    git(root, 'apply', '--no-index', '--whitespace=nowarn',
        '--directory=' + destination.relative_to(root).as_posix(),
        str(local_path(root, patch)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--prepare', type=Path,
                        help='Firmware configure: check only the firmware inputs and write their patched '
                             'vendor overlay to this directory inside root/build')
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.prepare.resolve() if args.prepare else None
    if output is not None and not output.is_relative_to(root / 'build'):
        parser.error('--prepare must be inside the project build directory')
    deps = json.loads((root / 'dependencies.json').read_text())['dependencies']
    selected = [dep for dep in deps if output is None or scope(dep) == 'firmware']
    for dep in selected:
        check_dependency(root, dep)
    if output:
        for dep in selected:
            if dep.get('patch'):
                prepare(root, dep, output)
    print(f'Checked {len(selected)} local dependencies' + ('; prepared vendor patches.' if output else '.'))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f'Dependency setup failed: {error}', file=sys.stderr)
        sys.exit(1)
