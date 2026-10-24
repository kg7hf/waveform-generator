#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Draw-order check (gate 1b): which part of a complex pair pathsim's first random draw reaches.

rev-6 pathsim draws each complex pair as v.set(rsample(), rsample()) (Path.cpp, Rayleigh::sample;
NoiseGen.cpp, add_band_limited_noise). C++ leaves the order of the two calls unspecified, and GCC
fixes it per target when it lowers the call to GIMPLE: x86-64 evaluates the second argument first,
Arm the first. The lane's pathsim.exe (x86-64) therefore puts the first draw into the imaginary
part. patches/pathsim-wfg-embedded.patch writes that order out for every compiler.

This compiles Path.cpp and NoiseGen.cpp of both build-tree copies (pathsim-src: rev-6;
pathsim-chain-src: rev-6 + the patch) with the chain's flags and the RNG shim, with the host
compiler and with arm-none-eabi-g++ for the RT1170 (the firmware's Cortex-M7 flags), dumps GIMPLE
(-fdump-tree-gimple), and reads at each draw site which argument of cmplx::set receives the first
draw. Pass: every site of the chain copy, on both compilers, and of rev-6 on the host compiler,
puts the first draw into the imaginary part. rev-6 on Arm is reported as well (the swap the patch
removes). Outputs go under <work>/draw-order only. Without an Arm compiler the check is SKIP, which
is not a pass: the exit status is 1 unless --allow-skip accepts it (as run_gates.py's).

  python channel-sim/gates/check_draw_order.py --build build/main/pathsim-host [--arm-gxx <arm-none-eabi-g++>] [--allow-skip]

Guide: docs/modules/pathsim.md.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse  # noqa: E402
import json  # noqa: E402
import os  # noqa: E402
from pathlib import Path  # noqa: E402
import re  # noqa: E402
import shutil  # noqa: E402
import subprocess  # noqa: E402
from typing import Callable  # noqa: E402

BELOW_NORMAL = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)
CHANNEL_SIM = Path(__file__).resolve().parents[1]
SHIM = CHANNEL_SIM / 'rng' / 'pathsim_rng_shim.hpp'
FILES = ('Path.cpp', 'NoiseGen.cpp')
TREES = (('rev-6', 'pathsim-src'), ('chain', 'pathsim-chain-src'))
# pathsim_lib's flags (channel-sim/CMakeLists.txt) and the firmware's target flags (cmake/rt1170_flags.cmake).
CHAIN_FLAGS = ('-O3', '-DNDEBUG', '-std=gnu++17', '-fno-exceptions', '-fno-rtti', '-ffp-contract=off')
CORTEX_M7_FLAGS = ('-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16')

DRAW = re.compile(r'^\s*(\S+) = .*::operator\(\) \(&rsample\);\s*$')
SET = re.compile(r'cmplx::set \(([^,]+), ([^,]+), ([^)]+)\);')

Run = Callable[[list[str], Path], None]

PASS, FAIL, SKIP = 'PASS', 'FAIL', 'SKIP'


def verdict(passed: bool, skipped: bool) -> str:
    """A check's verdict: SKIP when it did not run, whatever its partial results say."""
    return SKIP if skipped else PASS if passed else FAIL


def exit_status(verdicts: list[str], allow_skip: bool) -> int:
    """0 when every verdict is PASS, or SKIP and skips are allowed (--allow-skip); else 1, and 1 for no verdict
    at all (nothing ran, so nothing passed). Both gate scripts use it."""
    return 0 if verdicts and all(v == PASS or (v == SKIP and allow_skip) for v in verdicts) else 1


def find_arm_gxx(explicit: Path | None) -> Path | None:
    """--arm-gxx, then $ARMGCC_DIR (as the firmware build), PATH, the MCUXpresso tools folder."""
    exe = '.exe' if os.name == 'nt' else ''
    candidates: list[Path] = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get('ARMGCC_DIR'):
        candidates.append(Path(os.environ['ARMGCC_DIR']) / 'bin' / f'arm-none-eabi-g++{exe}')
    on_path = shutil.which('arm-none-eabi-g++')
    if on_path:
        candidates.append(Path(on_path))
    candidates += sorted((Path.home() / '.mcuxpressotools').glob(f'arm-gnu-toolchain-*/bin/arm-none-eabi-g++{exe}'), reverse=True)
    return next((c for c in candidates if c.is_file()), None)


def draw_sites(gimple: str) -> list[dict]:
    """Every cmplx::set call whose two value arguments are the last two draws: which part got the first."""
    sites, draws = [], []
    for number, line in enumerate(gimple.splitlines(), 1):
        draw = DRAW.match(line)
        if draw:
            draws.append(draw.group(1))
            continue
        call = SET.search(line)
        if call and len(draws) >= 2:
            real, imaginary = call.group(2).strip(), call.group(3).strip()
            first, second = draws[-2], draws[-1]
            if {real, imaginary} == {first, second}:
                sites.append({'gimple_line': number, 'call': call.group(0), 'first_draw_to': 'imaginary' if imaginary == first else 'real'})
            draws.clear()
    return sites


def default_run(command: list[str], cwd: Path) -> None:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, creationflags=BELOW_NORMAL)
    if result.returncode != 0:
        raise RuntimeError(f'command failed (rc={result.returncode}): {" ".join(command)}\n{result.stdout}{result.stderr}')


def check(build: Path, work: Path, host_gxx: Path, arm_gxx: Path | None, run: Run = default_run) -> dict:
    compilers = [('host', host_gxx, ())]
    if arm_gxx:
        compilers.append(('cortex-m7', arm_gxx, CORTEX_M7_FLAGS))
    results = []
    for label, gxx, target_flags in compilers:
        for tree_label, tree in TREES:
            for name in FILES:
                out = work / 'draw-order' / label / tree_label / Path(name).stem
                out.mkdir(parents=True, exist_ok=True)
                for stale in out.glob('*'):
                    stale.unlink()
                source = build / tree / name
                command = [str(gxx), *CHAIN_FLAGS, *target_flags, '-include', str(SHIM), '-I', str(CHANNEL_SIM),
                           '-I', str(build / tree), '-fdump-tree-gimple', '-c', str(source), '-o', str(out / 'draw.o')]
                run(command, out)
                dumps = sorted(out.glob('*.gimple'))
                if len(dumps) != 1:
                    raise RuntimeError(f'expected one GIMPLE dump in {out}, found {len(dumps)}')
                sites = draw_sites(dumps[0].read_text(encoding='utf-8', errors='replace'))
                results.append({'compiler': label, 'gxx': str(gxx), 'tree': tree_label, 'file': name, 'argv': command,
                                'sites': sites, 'lane_order': len(sites) == 1 and sites[0]['first_draw_to'] == 'imaginary'})
    required = [r for r in results if r['tree'] == 'chain' or r['compiler'] == 'host']
    record = {
        'gate': 1, 'part': 'b', 'case': 'draw-order',
        'purpose': 'first draw of each pair -> imaginary part (the lane order) at every pathsim draw site, host and Cortex-M7',
        'arm_compiler': str(arm_gxx) if arm_gxx else None,
        'results': results,
        'rev6_cortex_m7_swapped': [r['file'] for r in results if r['compiler'] == 'cortex-m7' and r['tree'] == 'rev-6' and not r['lane_order']],
        'skipped': arm_gxx is None,
    }
    record['pass'] = not record['skipped'] and all(r['lane_order'] for r in required)
    record['verdict'] = verdict(record['pass'], record['skipped'])
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, required=True, help='channel-sim build directory (pathsim-src, pathsim-chain-src)')
    parser.add_argument('--work', type=Path, help='work directory (default <build>/work)')
    parser.add_argument('--arm-gxx', type=Path, help='arm-none-eabi-g++ (default: $ARMGCC_DIR, PATH, ~/.mcuxpressotools)')
    parser.add_argument('--allow-skip', action='store_true', help='exit 0 when the check is skipped (no Arm compiler)')
    args = parser.parse_args()
    build = args.build.resolve()
    recipe = json.loads((build / 'pathsim_chain.recipe.json').read_text(encoding='utf-8'))
    record = check(build, (args.work or build / 'work').resolve(), Path(recipe['compiler']), find_arm_gxx(args.arm_gxx))
    for result in record['results']:
        order = ', '.join(site['first_draw_to'] for site in result['sites']) or 'no draw site found'
        print(f"{result['compiler']:9} {result['tree']:5} {result['file']:12} first draw -> {order}")
    print('draw order:', f"{SKIP} (no arm-none-eabi-g++)" if record['skipped'] else record['verdict'])
    return exit_status([record['verdict']], args.allow_skip)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f'draw-order check failed to run: {error}', file=sys.stderr)
        sys.exit(2)
