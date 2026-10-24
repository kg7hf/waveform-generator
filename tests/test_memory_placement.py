#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/check-memory-placement.py against GNU ld map excerpts.

The excerpts follow arm-none-eabi-ld 2.43's map format, as the player link
writes it (rt1170/CMakeLists.txt runs the check after every player link): the
discarded sections, the region table, then the memory map with symbol
assignments and input sections, whose address goes on the next line when the
name is long.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'check-memory-placement.py'
_spec = importlib.util.spec_from_file_location('check_memory_placement', SCRIPT)
placement = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = placement  # dataclasses resolve their module by name
_spec.loader.exec_module(placement)

REGION = 'm_ncache_ocram'
WINDOW = ('__NCACHE_REGION_START', '__NCACHE_REGION_SIZE')
BOUNCE = '.ncache_ocram.wfg_msc_bounce'
EPBUF = '.ncache_ocram.tusb_msc_epbuf'
WATTERSON = '.ocram.watterson'
BOUNCE_OBJ = 'CMakeFiles/waveform_generator.elf.dir/platform/waveform_msc.c.obj'
EPBUF_OBJ = 'CMakeFiles/waveform_generator.elf.dir/__/third_party/tinyusb/src/class/msc/msc_device.c.obj'


def header(window_start: int = 0x20338000, window_size: int = 0x8000, region: str = REGION,
           start_symbol: str = WINDOW[0]) -> str:
    """The map up to the memory map's first lines: a discarded copy of the
    bounce section (which never counts), the region table and the window's
    symbol assignments."""
    return f"""\
Archive member included to satisfy reference by file (symbol)

Discarded input sections

 {BOUNCE}
                0x00000000     0x4000 CMakeFiles/old.c.obj

Memory Configuration

Name             Origin             Length             Attributes
m_text           0x30002400         0x00fbdc00         xr
m_data           0x20000000         0x00040000         rw
m_data2          0x202c0000         0x00078000         rw
{region:<16} 0x20338000         0x00008000         rw
rpmsg_sh_mem     0x202c0000         0x00000000         rw
*default*        0x00000000         0xffffffff

Linker script and memory map

                0x{window_start:08x}                        {start_symbol} = ORIGIN (m_ncache_ocram)
                0x{window_size:08x}                        {WINDOW[1]} = LENGTH (m_ncache_ocram)
"""


def bss(epbuf_in_dtcm: bool = False) -> str:
    moved = f' {EPBUF}\n                0x20037dc0     0x4000 {EPBUF_OBJ}\n' if epbuf_in_dtcm else ''
    return f"""\
.bss            0x2001c800    0x20160 load address 0x3004c000
 *(.bss*)
{moved} .bss._mscd_itf
                0x2003bdc0       0x40 {EPBUF_OBJ}
 *fill*         0x2003c95e        0x2
"""


def ncache(bounce_address: int | None = 0x20338000, epbuf_address: int | None = 0x2033c000,
           bounce_size: int = 0x4000, bounce_name: str = BOUNCE) -> str:
    """The window's output section; None leaves that buffer out."""
    bounce = (f' {bounce_name}\n                0x{bounce_address:08x}     0x{bounce_size:x} {BOUNCE_OBJ}\n'
              if bounce_address is not None else '')
    epbuf = (f' {EPBUF}\n                0x{epbuf_address:08x}     0x4000 {EPBUF_OBJ}\n'
             if epbuf_address is not None else '')
    return f"""\
.ncache_ocram   0x20338000     0x8000
                0x20338000                        . = ALIGN (0x20)
                0x20338000                        __ncache_ocram_start__ = .
 *(.ncache_ocram)
 *(.ncache_ocram.*)
{bounce}{epbuf}                0x20340000                        __ncache_ocram_end__ = .
"""


def watterson(address: int = 0x202c0000, size: int = 18_208,
              section_name: str = WATTERSON) -> str:
    return f' {section_name}\n                0x{address:08x}     0x{size:x} CMakeFiles/waveform_generator.elf.dir/player.cpp.obj\n'


def run_check(text: str, sections: tuple[str, ...] = (BOUNCE, EPBUF)) -> list[str]:
    return placement.check(placement.parse_map(text), REGION, list(sections), WINDOW)


class MemoryPlacementTests(unittest.TestCase):
    def test_both_buffers_in_the_window(self):
        report = run_check(header() + bss() + ncache())
        self.assertEqual(report, [
            f'{BOUNCE}: 0x20338000 + 0x00004000 in {REGION} ({BOUNCE_OBJ})',
            f'{EPBUF}: 0x2033c000 + 0x00004000 in {REGION} ({EPBUF_OBJ})',
        ])

    def test_parses_regions_symbols_and_sections(self):
        parsed = placement.parse_map(header() + bss() + ncache())
        self.assertEqual(parsed.regions[REGION], placement.Region(REGION, 0x20338000, 0x8000))
        self.assertEqual(parsed.regions['rpmsg_sh_mem'].length, 0)
        self.assertNotIn('Name', parsed.regions)
        self.assertEqual(parsed.symbols, {WINDOW[0]: 0x20338000, WINDOW[1]: 0x8000,
                                          '__ncache_ocram_start__': 0x20338000,
                                          '__ncache_ocram_end__': 0x20340000})
        self.assertEqual([section.name for section in parsed.sections], ['.bss._mscd_itf', BOUNCE, EPBUF])

    def test_a_short_name_on_one_line(self):
        text = header() + ncache() + f' .ncache_ocram   0x20338000       0x40 {BOUNCE_OBJ}\n'
        self.assertEqual(run_check(text, sections=('.ncache_ocram',)),
                         [f'.ncache_ocram: 0x20338000 + 0x00000040 in {REGION} ({BOUNCE_OBJ})'])

    def test_a_buffer_in_dtcm_fails(self):
        text = header() + bss(epbuf_in_dtcm=True) + ncache(epbuf_address=None)
        with self.assertRaisesRegex(ValueError, rf'{EPBUF}: 0x20037dc0 \+ 0x00004000 lies outside {REGION}'):
            run_check(text)

    def test_a_buffer_past_the_window_end_fails(self):
        with self.assertRaisesRegex(ValueError, rf'{EPBUF}: 0x2033e000 \+ 0x00004000 lies outside'):
            run_check(header() + ncache(epbuf_address=0x2033e000))

    def test_a_buffer_under_another_name_fails(self):
        # A misspelt section attribute makes an orphan the window never holds:
        # the expected name is absent. The discarded copy never counts.
        text = header() + ncache(bounce_name='.ncache_ocram_wfg_msc_bounce')
        with self.assertRaisesRegex(ValueError, f'{BOUNCE}: placed 0 times'):
            run_check(text)

    def test_a_buffer_placed_twice_fails(self):
        text = header() + ncache() + f' {BOUNCE}\n                0x2033c000     0x10 CMakeFiles/other.c.obj\n'
        with self.assertRaisesRegex(ValueError, f'{BOUNCE}: placed 2 times'):
            run_check(text)

    def test_an_empty_buffer_fails(self):
        with self.assertRaisesRegex(ValueError, f'{BOUNCE}: empty'):
            run_check(header() + ncache(bounce_size=0))

    def test_a_window_the_symbols_do_not_describe_fails(self):
        with self.assertRaisesRegex(ValueError, f'= 0x00000000 do not describe {REGION}'):
            run_check(header(window_size=0) + ncache())
        with self.assertRaisesRegex(ValueError, f'= 0x20330000 and .* do not describe {REGION}'):
            run_check(header(window_start=0x20330000) + ncache())

    def test_missing_window_symbols_fail(self):
        with self.assertRaisesRegex(ValueError, f'{WINDOW[0]} = missing'):
            run_check(header(start_symbol='other_symbol') + ncache())

    def test_every_problem_is_named(self):
        with self.assertRaises(ValueError) as caught:
            run_check(header(window_size=0) + ncache(bounce_address=None, epbuf_address=0x20000000))
        message = str(caught.exception)
        self.assertIn('do not describe', message)
        self.assertIn(f'{BOUNCE}: placed 0 times', message)
        self.assertIn(f'{EPBUF}: 0x20000000', message)

    def test_an_unknown_region_fails(self):
        with self.assertRaisesRegex(ValueError, f'memory region {REGION} is not in the map'):
            run_check(header(region='m_other') + ncache())

    def test_text_that_is_not_a_map_fails(self):
        with self.assertRaisesRegex(ValueError, 'not a GNU ld map'):
            placement.parse_map('Memory Configuration\n')

    def test_watterson_state_fits_in_ocram2(self):
        report = placement.check(placement.parse_map(header() + watterson()), 'm_data2', [WATTERSON],
                                 maximum_sizes={WATTERSON: 20_480})
        self.assertEqual(report, [f'{WATTERSON}: 0x202c0000 + 0x00004720 in m_data2 '
                                  '(CMakeFiles/waveform_generator.elf.dir/player.cpp.obj)'])

    def test_watterson_section_is_required(self):
        with self.assertRaisesRegex(ValueError, rf'{WATTERSON}: placed 0 times'):
            placement.check(placement.parse_map(header()), 'm_data2', [WATTERSON],
                            maximum_sizes={WATTERSON: 20_480})

    def test_duplicate_watterson_section_fails(self):
        text = header() + watterson() + watterson(address=0x202d0000)
        with self.assertRaisesRegex(ValueError, rf'{WATTERSON}: placed 2 times'):
            placement.check(placement.parse_map(text), 'm_data2', [WATTERSON],
                            maximum_sizes={WATTERSON: 20_480})

    def test_misplaced_watterson_section_fails(self):
        with self.assertRaisesRegex(ValueError, rf'{WATTERSON}: 0x20000000 .* lies outside m_data2'):
            placement.check(placement.parse_map(header() + watterson(address=0x20000000)), 'm_data2', [WATTERSON],
                            maximum_sizes={WATTERSON: 20_480})

    def test_oversized_watterson_section_fails(self):
        with self.assertRaisesRegex(ValueError, rf'{WATTERSON}: 0x00005001 exceeds maximum 0x00005000'):
            placement.check(placement.parse_map(header() + watterson(size=20_481)), 'm_data2', [WATTERSON],
                            maximum_sizes={WATTERSON: 20_480})

    def test_command_line(self):
        arguments = ['--region', REGION, '--window-symbols', *WINDOW, '--section', BOUNCE, '--section', EPBUF]
        with tempfile.TemporaryDirectory(prefix='wfg-memory-placement-') as directory:
            good = Path(directory) / 'good.map'
            good.write_text(header() + bss() + ncache(), encoding='utf-8')
            bad = Path(directory) / 'bad.map'
            bad.write_text(header() + ncache(bounce_address=0x20000000), encoding='utf-8')
            passed = subprocess.run([sys.executable, str(SCRIPT), '--map', str(good), *arguments],
                                    capture_output=True, text=True, check=False)
            failed = subprocess.run([sys.executable, str(SCRIPT), '--map', str(bad), *arguments],
                                    capture_output=True, text=True, check=False)
            missing = subprocess.run([sys.executable, str(SCRIPT), '--map', str(Path(directory) / 'none.map'),
                                      *arguments], capture_output=True, text=True, check=False)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertEqual(passed.stdout.count('Memory placement: '), 2)
        self.assertEqual(failed.returncode, 1)
        self.assertIn(f'{BOUNCE}: 0x20000000', failed.stderr)
        self.assertEqual(missing.returncode, 1)
        self.assertIn('Memory placement check failed', missing.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
