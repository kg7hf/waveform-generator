#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check, in a GNU ld map, that named input sections lie inside one memory region.

The firmware build runs it after every player link (rt1170/CMakeLists.txt): the
MSC chunk buffers must lie in m_ncache_ocram, the OCRAM window that MPU region
10 makes non-cacheable, and the window's linker symbols must describe that
region exactly; and the player's state kept out of DTCM (the CUT injector's
state, the command and reply mailboxes, the request line, the PLAYLIST state,
the play's watch, the EP0 order and the AWGN noise-band kernel) must lie in
m_data2 (OCRAM2). A section the
linker script does not name (a misspelt section
attribute, say) is placed by the linker's orphan rules, possibly in cacheable
memory or in DTCM, without a diagnostic; this check fails instead. Optional
per-section maximum sizes can bound fixed state such as the Watterson channel.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys

MEMORY_HEADING = 'Memory Configuration'
MAP_HEADING = 'Linker script and memory map'
# name, origin, length and attributes (the *default* row has none).
REGION_ROW = re.compile(r'^(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(?:\s+\S+)?\s*$')
# A symbol assignment: its value, then the script's expression.
ASSIGNMENT = re.compile(r'^\s+0x([0-9a-fA-F]+)\s+(\w+)\s+=')
# An input section: one space, its name, then (here or on the next line) its
# address, size and object file. ' *(' lines are patterns and ' *fill*' padding.
INPUT_SECTION = re.compile(r'^ ([^\s*]\S*)(?:\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*))?$')
PLACEMENT = re.compile(r'^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$')


@dataclass(frozen=True)
class Region:
    name: str
    origin: int
    length: int

    def holds(self, address: int, size: int) -> bool:
        return self.origin <= address and address + size <= self.origin + self.length


@dataclass(frozen=True)
class InputSection:
    name: str
    address: int
    size: int
    source: str


@dataclass(frozen=True)
class LinkerMap:
    regions: dict[str, Region]
    symbols: dict[str, int]
    sections: list[InputSection]


def parse_map(text: str) -> LinkerMap:
    """The memory regions, symbol assignments and placed input sections of a map."""
    lines = text.splitlines()
    headings = [line.strip() for line in lines]
    try:
        memory_start = headings.index(MEMORY_HEADING)
        map_start = headings.index(MAP_HEADING)
    except ValueError as error:
        raise ValueError(f'not a GNU ld map: {error}') from error

    regions: dict[str, Region] = {}
    for line in lines[memory_start + 1:map_start]:
        row = REGION_ROW.match(line)
        if row and row.group(1) != 'Name':
            regions[row.group(1)] = Region(row.group(1), int(row.group(2), 16), int(row.group(3), 16))

    symbols: dict[str, int] = {}
    sections: list[InputSection] = []
    body = lines[map_start + 1:]
    for index, line in enumerate(body):
        assignment = ASSIGNMENT.match(line)
        if assignment:
            symbols[assignment.group(2)] = int(assignment.group(1), 16)
            continue
        section = INPUT_SECTION.match(line)
        if not section:
            continue
        if section.group(2) is not None:
            address, size, source = section.group(2), section.group(3), section.group(4)
        else:
            placement = PLACEMENT.match(body[index + 1]) if index + 1 < len(body) else None
            if not placement:
                continue
            address, size, source = placement.groups()
        sections.append(InputSection(section.group(1), int(address, 16), int(size, 16), source.strip()))
    return LinkerMap(regions, symbols, sections)


def check(linker_map: LinkerMap, region_name: str, section_names: list[str],
          window_symbols: tuple[str, str] | None = None,
          maximum_sizes: dict[str, int] | None = None) -> list[str]:
    """One line per placed section; raises ValueError naming every violation."""
    region = linker_map.regions.get(region_name)
    if region is None:
        raise ValueError(f'memory region {region_name} is not in the map')

    problems: list[str] = []
    if window_symbols is not None:
        start_name, size_name = window_symbols
        start = linker_map.symbols.get(start_name)
        size = linker_map.symbols.get(size_name)
        if start != region.origin or size != region.length:
            problems.append(f'{start_name} = {_hex(start)} and {size_name} = {_hex(size)} do not describe '
                            f'{region_name} ({_hex(region.origin)}, {_hex(region.length)})')

    report: list[str] = []
    maximum_sizes = maximum_sizes or {}
    for name in section_names:
        placed = [section for section in linker_map.sections if section.name == name]
        if len(placed) != 1:
            problems.append(f'{name}: placed {len(placed)} times, expected once')
            continue
        section = placed[0]
        if section.size == 0:
            problems.append(f'{name}: empty')
        elif name in maximum_sizes and section.size > maximum_sizes[name]:
            problems.append(f'{name}: {_hex(section.size)} exceeds maximum {_hex(maximum_sizes[name])}')
        elif not region.holds(section.address, section.size):
            problems.append(f'{name}: {_hex(section.address)} + {_hex(section.size)} lies outside '
                            f'{region_name} ({_hex(region.origin)} + {_hex(region.length)})')
        else:
            report.append(f'{name}: {_hex(section.address)} + {_hex(section.size)} in {region_name} '
                          f'({section.source})')
    if problems:
        raise ValueError('; '.join(problems))
    return report


def _hex(value: int | None) -> str:
    return 'missing' if value is None else f'0x{value:08x}'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--map', type=Path, required=True, help='GNU ld map (-Wl,-Map=...)')
    parser.add_argument('--region', required=True, help='memory region every section must lie in')
    parser.add_argument('--section', action='append', required=True, dest='sections',
                        help='input section name, placed exactly once (repeat for each)')
    parser.add_argument('--max-section-size', action='append', nargs=2, metavar=('SECTION', 'BYTES'),
                        default=[], help='maximum input section size in bytes (repeat as needed)')
    parser.add_argument('--window-symbols', nargs=2, metavar=('START', 'SIZE'),
                        help='linker symbols that must equal the region origin and length')
    args = parser.parse_args()
    try:
        linker_map = parse_map(args.map.read_text(encoding='utf-8', errors='replace'))
        window = tuple(args.window_symbols) if args.window_symbols else None
        maximum_sizes: dict[str, int] = {}
        for name, raw_size in args.max_section_size:
            size = int(raw_size, 0)
            if size <= 0:
                raise ValueError(f'{name}: maximum size must be positive')
            if name in maximum_sizes:
                raise ValueError(f'{name}: maximum size specified more than once')
            maximum_sizes[name] = size
        unknown_limits = maximum_sizes.keys() - set(args.sections)
        if unknown_limits:
            raise ValueError(f'maximum size specified for unchecked section(s): {", ".join(sorted(unknown_limits))}')
        for line in check(linker_map, args.region, args.sections, window, maximum_sizes):
            print(f'Memory placement: {line}')
    except (OSError, ValueError) as error:
        print(f'Memory placement check failed ({args.map}): {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
