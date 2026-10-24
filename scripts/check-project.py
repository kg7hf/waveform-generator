#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check required project paths and optionally compiled source locations.

--scope checks only the dependencies.json entries of that scope (see
scripts/dependencies.py): "host-test" for a checkout that initialised only the
submodules a host build reads, as the host CI does. WFG_DEPENDENCY_SCOPE in the
environment sets the default, so a ctest run of wfg-project-layout can be
scoped too. With neither, every entry is checked.
"""

import argparse
import json
import os
from pathlib import Path
import sys

from dependencies import SCOPES, check_dependency, scope

SCOPE_VARIABLE = 'WFG_DEPENDENCY_SCOPE'


def check(root: Path, build: Path | None, plugin: Path | None = None, selected: str | None = None) -> None:
    for name in ('CMakeLists.txt', 'CMakePresets.json', '.gitmodules', 'dependencies.json'):
        if not (root / name).is_file():
            raise ValueError(f'missing project file: {name}')
    config = json.loads((root / 'dependencies.json').read_text(encoding='utf-8'))
    for dependency in config['dependencies']:
        if selected is None or scope(dependency) == selected:
            check_dependency(root, dependency)
    if build is not None:
        commands = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
        if not commands:
            raise ValueError('empty compilation database')
        for command in commands:
            source = (Path(command['directory']) / command['file']).resolve()
            if not source.is_relative_to(root) and not (plugin and source.is_relative_to(plugin)):
                raise ValueError(f'compiled source outside project: {source}')
            if not source.is_file():
                raise ValueError(f'missing compiled source: {source}')
    inputs = 'dependency inputs' if selected is None else f'{selected} dependency inputs'
    print(f'Project paths and {inputs} are available.' +
          (' Compiled source paths are inside the project or explicitly supplied plugin.' if build else ''))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build-dir', type=Path, help='Also inspect its compile_commands.json')
    parser.add_argument('--plugin-dir', type=Path, help='Permit compiled sources from this separate encoder module')
    parser.add_argument('--scope', choices=SCOPES, default=os.environ.get(SCOPE_VARIABLE) or None,
                        help=f'Check only the dependencies of this scope (default: ${SCOPE_VARIABLE}, else every entry)')
    args = parser.parse_args()
    if args.scope is not None and args.scope not in SCOPES:
        parser.error(f"{SCOPE_VARIABLE}={args.scope!r}: expected one of {', '.join(SCOPES)}")
    try:
        check(args.root.resolve(), args.build_dir.resolve() if args.build_dir else None,
              args.plugin_dir.resolve(strict=True) if args.plugin_dir else None, args.scope)
    except (OSError, ValueError, KeyError) as error:
        print(f'Project check failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
