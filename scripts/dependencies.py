#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Check local dependency files and prepare vendor patches offline."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


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


def check_dependency(root: Path, dep: dict) -> None:
    """Require local inputs. Git manages submodule revisions; no duplicate SHA lock."""
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
    parser.add_argument('--prepare', type=Path, help='Overlay output directory inside root/build')
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.prepare.resolve() if args.prepare else None
    if output is not None and not output.is_relative_to(root / 'build'):
        parser.error('--prepare must be inside the project build directory')
    deps = json.loads((root / 'dependencies.json').read_text())['dependencies']
    for dep in deps:
        check_dependency(root, dep)
    if output:
        for dep in deps:
            if dep.get('patch'):
                prepare(root, dep, output)
    print(f'Checked {len(deps)} local dependencies' + ('; prepared vendor patches.' if output else '.'))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f'Dependency setup failed: {error}', file=sys.stderr)
        sys.exit(1)
