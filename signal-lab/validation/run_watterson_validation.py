#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Host statistical validation of signal-lab's Watterson stage against pathsim (design 10.2).

Guide: docs/design/watterson-validation.md (the report, with every number this runner writes).
Design: docs/design/watterson-stage.md, section 10 (B1 to B10); the lane's stage acceptance 1 to 7 in
the owner decision of 2026-10-08.

For each lane criterion the runner renders the lane's probe signals through the stage
(signal-lab/validation/watterson_render.cpp, built as wfg_watterson_render in build/main/host-c) and,
where pathsim has the parameters, through pathsim (channel-sim's pathsim_chain, which gates 0 to 3
prove equal to the lane's pathsim.exe 7746121 and run_pathsim.py chain), then measures both with the
lane's analyzers (hfchannel/probes, read only) and with wv_analysis.py for what those do not cover:

  1  delivered SNR on a ladder (B1, E.7.2) and under fading (B2, E.7.3, 2 h each)
  2  Doppler two-sigma, shape and images per spread (B3, E.7.4 3 h), per path from the channel truth,
     and tap delays (B4)
  3  Rayleigh envelope, path powers and correlation, Rician K (B5)
  4  noise: flatness, ENBW, Gaussianity, folds of 5 h for block combs, lines beside a tone (B6), and
     beside a shifted tone, where the output depends on the quadrature path and the rotator (B6d)
  6  determinism: repeated renders, block sizes, another seed (B8)
  7  coverage, shifts to +/-75 Hz, phase (B9), and the first battery of lane amendment 1 (B10): the
     JITC Table XVI two-path rows through B2 to B4, and D.6.2's fixed 3- and 4-path channels at 50 dB
  5  decoder equivalence: renders the inputs (8 rungs x 4 seeds x 2 simulators, the D.6.2 rows of
     B10, a manifest, the lane's decode commands in c5/decode_c5.ps1); when every render is decoded,
     scores them (design 10.4, with the lane's Q11 answer). The threshold set (--c5-threshold) adds,
     per ladder, 3 SNR points 2 dB apart at 12 seeds around BER 1e-3, from which the scorer reads
     the equivalent-SNR offset (lane call 3). Criterion 5's verdict in results.json combines c5's
     score with the threshold set's where c5t/ exists (c5_status).

Read only on the lane tree; writes only under --work (default build/main/host-c/work/watterson-validation):
rows/<id>.json per row, results.json, summary.md, and c5t/score.json once the threshold set is decoded;
with --c5-threshold, only c5t/. Every subprocess
runs at below-normal priority, and build/main/bringup/ab_gate.ps1 (the shared PC's A/B hold) is run
before each one; on HOLD the runner waits and asks again. No Python bytecode is written (lane analyzers included). Each row records the
sha256 of the runner and of wv_analysis.py that produced it, and the git head.

  python -B signal-lab/validation/run_watterson_validation.py [--criteria 1,2,3,4,6,7,5] [--quick] [--resume]
  python -B signal-lab/validation/run_watterson_validation.py --criteria 5 --resume --c5-extend auto
  python -B signal-lab/validation/run_watterson_validation.py --c5-threshold [--resume]
  python -B signal-lab/validation/run_watterson_validation.py --score-c5 <work>/c5/manifest.json
  python -B signal-lab/validation/run_watterson_validation.py --score-c5 <work>/c5t/manifest.json

--quick shortens the long rows (E.7.3 to 600 s, no E.7.4, B6b 1 h) for a smoke run; the report's
numbers come from a full run. --resume reuses rows/<id>.json records from an earlier run.
--c5-extend renders seeds 1 to 12 for the named rungs (or, with "auto", for every rung whose decodes
show a bit error) and adds their decodes to decode_c5.ps1, which skips the renders already decoded.
--c5-threshold renders the threshold set (default C5_THRESHOLD_SET, or a set such as
"C24L05-MPP:8,10,12;C06L05-MPP:0,2,4"), seeds 1 to 12, both simulators, into c5t/ with its own
manifest and decode_c5.ps1; a c5 render with byte-identical inputs, made by today's binary, is
referenced, not rendered again.
Exit status 0: every criterion run passed (criterion 5: scored and passed, or not yet decoded).
Criterion 7's coverage matrix reads every scenario of the work directory, the parameters of all the
criteria together, so it is judged only on the full battery (every criterion, without --quick); a
subset or --quick run reports it as not judged, and criterion 7's verdict there is its rows' and B10's.
--score-c5 exits 0 when it passes (every ladder's offset within its bound, no rung rejects), 1 when a
ladder or rung fails, 2 when a decode is missing and 3 when it is inconclusive (a ladder's 1e-3
crossing lies outside its points, or the ladder lacks 3 points 2 dB apart).
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True  # no __pycache__ beside these scripts or the lane's

import argparse  # noqa: E402
import copy  # noqa: E402
import ctypes  # noqa: E402
import dataclasses  # noqa: E402
import datetime  # noqa: E402
import hashlib  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
import os  # noqa: E402
import platform  # noqa: E402
import subprocess  # noqa: E402
import time  # noqa: E402
from pathlib import Path  # noqa: E402

import numpy as np  # noqa: E402

import wv_analysis as wa  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
BELOW_NORMAL = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)
GATE_HOLD_EXIT = 3                          # ab_gate.ps1: HOLD, run it again
FS = 48000
LATENCY_FRAMES = 215                        # the stage's D (design 3.4; watterson report latency)
PATHSIM_LATENCY_FRAMES = 174                # pathsim's latency at 48 kHz in fading modes (calib A4)
STAGE_GAIN_DB = -12.0                       # design 10.2: -12 dBFS probes leave the stage at -24 dBFS
PATHSIM_PIN_DBFS = 20.0 * math.log10(0.122)  # pathsim pins its signal at 0.122 RMS (calib A5): -18.27 dBFS
PATHSIM_COMMAND_OFFSET_DB = -0.40           # the lane's governed offset: commanded = target - 0.40 (calib A1)
PATHSIM_LABEL_OFFSET_DB = 0.42              # pathsim delivers label + 0.42 dB by N0 x 3000 (calib A1)
SNR_TOLERANCE_DB = 0.1                      # lane criterion 1 (E.5.5)
SE_FACTOR = 5.0                             # design 10.2: a 0.1 dB verdict needs 0.1 >= 5 s.e.
E72_TOLERANCE_DB = 0.25                     # E.7.2
E73_TOLERANCE_DB = 0.5                      # E.7.3
SPREAD_TOLERANCE = 0.10                     # lane criterion 2: two sigma within 10 %
IMAGE_LIMIT_DBC = -60.0                     # lane criterion 2: images below -60 dBc
DELAY_TOLERANCE_FRAMES = 0.05               # design B4: LS refinement within 0.05 frame
POWER_TOLERANCE_DB = 0.5                    # design B4/B5: relative powers within +/-0.5 dB
RESPONSE_TOLERANCE_DB = 0.5                 # E.7.1
E_I_TOLERANCE_DB = {'-20': 1.5, '-30': 2.0}  # 110D Table E-I via E.7.4
MEAN_OVER_RMS_TOLERANCE = 0.01              # design B5
LCR_TOLERANCE = 0.15                        # design B5
FLATNESS_TOLERANCE_DB = 0.2                 # lane criterion 4
SPUR_LIMIT_DBC = -70.0                      # lane criterion 4
ENBW_DESIGN_HZ = 3233.4                     # design 7.3; awgn.hpp NoiseBandKernel::enbw
ENBW_TOLERANCE_HZ = 10.0                    # design B6a
GAUSS_Z_LIMIT = 4.0                         # design U31/B6a: within 4 standard errors
CORRELATION_SE_LIMIT = 4.0                  # design U16
FREQUENCY_TOLERANCE_HZ = 1e-3               # design B9: within 1 mHz
CENTRE_TOLERANCE_HZ = 0.05                  # B9's lobe centre: about 3 s.e. of a 600 s, 1 Hz centroid
PHASE_TOLERANCE_DEG = 0.01
MIRROR_LIMIT_DBC = -70.0                    # design B9 / U6
K_TOLERANCE_DB = 0.5                        # design U19
BLOCKS = (1, 7, 63, 64, 65, 96, 255, 256, 448, 1000, 2047, 2048)  # design U21 / B8
ENGINE_BLOCK_FRAMES = 2048                  # impairment.hpp engine_block_frames: the render's block grid
# B6b folds the AWGN-only render. Its only frame grid is the engine block: the AWGN stage's 1722-frame
# chunks restart in every block (awgn.cpp), so their edges sit at 2048 k and 2048 k + 1722 and repeat
# every 2048 frames, inside the 2048 fold. 1024, 256 and 64 are folds of the 2048 average (its
# harmonics' subsets); they test the AWGN path only, since no Watterson stage is in that render.
FOLD_PERIODS = (ENGINE_BLOCK_FRAMES,)
FOLD_DERIVED = {1024: 2048, 256: 2048, 64: 2048}
# The Watterson stage's own frame grids (B6d): the engine block, the rotator's exact resync every 1024
# frames (watterson.hpp rotator_resync_frames), 256, and the 64-frame chunk of the analytic filter
# (analytic_filter.hpp analytic_chunk_frames); a defect keyed to one shows as tone + k fs / P.
WATTERSON_GRID_FRAMES = {'engine block': 2048, 'rotator resync': 1024, '256-frame divisor': 256, 'Watterson chunk': 64}
IONOS_COMB_HZ = 344.53                      # the lane's IONOS comb, k x 344.53 Hz (negative-control list)
# The formal E.7 durations (lane amendment 2) are of the analysed interval. The lane analyzers trim
# 0.25 s from each end of a probe body (analyze_snr --margin, analyze_doppler), so the probes are 10 s
# longer than each minimum and the analysed duration is checked against it.
E72_MIN_S, E73_MIN_S, E74_MIN_S = 1200.0, 7200.0, 10800.0
E72_PROBE, E73_PROBE, E74_PROBE = '1210s', '7210s', '10810s'
C5_SEEDS = (1, 2, 3, 4)
C5_EXTENDED_SEEDS = tuple(range(1, 13))     # design 10.4: a threshold rung goes to 12 seeds per simulator
OFFSET_SEEDS = 8                            # independent noise realisations for the constant-offset estimate
# B10 (lane amendment 1, first battery): JITC Table XVI's two equal Rayleigh paths, (delay ms, spread Hz),
# and D.6.2's fixed equal-power channels (Table D-LXV) at 50 dB, BER at most 1e-5.
TABLE_XVI = ((2.0, 0.5), (2.0, 1.0), (2.0, 5.0), (5.0, 1.0), (5.0, 5.0))
D62_SNR_DB = 50.0
D62_BER_LIMIT = 1e-5
D62_CARD = ('2400L', 'C24L05')
D62_CHANNELS = (('3k-wid2', (0.0, 3.0, 9.0)), ('3k-wid10', (0.0, 2.0, 4.5)), ('3k-wid12', (0.0, 1.5)),
                ('6k-wid10-4path', (0.0, 1.5, 3.0, 5.0)))
B10_SNR_LABEL_DB = 20.0                     # as B2: the decomposition delivered = label + realised tap power
B10_TRUTH_SECONDS = 1800.0
# The tap processes depend on (seed, stage, path) only, not on the delay, so each Table XVI row has its
# own seed (11, 12, ...): five independent realisations rather than three repeated ones.
B10_FIRST_SEED = 11
C5_RUNGS = (  # design 10.4: the P21 phase D ladder (mode, card, geometry, delivered target dB)
    ('2400L', 'C24L05', 'MPP', 18.0), ('2400L', 'C24L05', 'MPP', 14.0), ('2400L', 'C24L05', 'MPP', 10.0),
    ('2400L', 'C24L05', 'AWGN', 10.0), ('1200L', 'C12L05', 'MPP', 12.0), ('1200L', 'C12L05', 'MPP', 8.0),
    ('600L', 'C06L05', 'MPP', 7.0), ('600L', 'C06L05', 'MPP', 4.0))
C5_ALPHA = 0.05 / len(C5_RUNGS)             # design 10.4 and Q11: 0.05 / 8 per rung
C5_VERDICT_SEEDS = 12                       # Q11: the rank-sum is a verdict only at >= 12 seeds per simulator
C5_CARD_MODES = {card: mode for mode, card, _, _ in C5_RUNGS}
# Criterion 5's threshold set (lane request 2026-10-09, kg7hf/waveform-generator#21): per ladder 3 SNR
# points 2 dB apart around BER 1e-3, seeds 1 to 12, both simulators; the card, channel and level rules
# of the c5 rungs, only the SNR changes. <card>-<MPP|AWGN>:<delivered target dB>,...; ladders by ';'.
C5_THRESHOLD_SET = 'C24L05-MPP:8,10,12;C24L05-AWGN:6,8,10;C12L05-MPP:4,6,8;C06L05-MPP:0,2,4'
OFFSET_BER = 1e-3                           # lane call 3: the equivalent-SNR offset at BER 1e-3
OFFSET_BOUND_DB = {'AWGN': 0.5, 'fading': 1.0}  # lane call 3: AWGN rows 0.5 dB, fading rows 1.0 dB
OFFSET_STEP_DB = 2.0                        # lane call 3: >= 3 SNR points 2 dB apart ...
OFFSET_MIN_POINTS = 3
OFFSET_MIN_SEEDS = 4                        # ... each pooling >= 4 seeds
C5_VERDICTS = {'pass': 'PASS', 'fail': 'FAIL', 'inconclusive': 'INCONCLUSIVE', 'missing': 'PENDING (decodes missing)'}
C5_TOOLS = {'stage': 'driver', 'pathsim': 'pathsim_chain'}  # each simulator's binary, by its name in a manifest's 'binaries'
FULL_BATTERY = ('1', '2', '3', '4', '5', '6', '7')  # the criteria of the full battery, the only run that judges coverage


# ================================================================================================ plumbing

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 22), b''):
            digest.update(block)
    return digest.hexdigest()


def plain(value):
    """JSON for numpy scalars and paths (np.bool_ is not a bool; a Path is its text)."""
    if isinstance(value, np.bool_):
        return bool(value)
    if isinstance(value, np.integer):
        return int(value)
    if isinstance(value, np.floating):
        return float(value)
    return str(value)


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(value, indent=1, default=plain) + '\n').encode('utf-8'))


def read_json(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def below_normal_self() -> None:
    """The runner's own analyses run at below-normal priority too (Windows; elsewhere a no-op)."""
    if os.name == 'nt':
        kernel = ctypes.windll.kernel32
        kernel.SetPriorityClass(kernel.GetCurrentProcess(), 0x00004000)  # BELOW_NORMAL_PRIORITY_CLASS


def toolchain_bin(build: Path) -> Path | None:
    cache = build / 'CMakeCache.txt'
    for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('CMAKE_CXX_COMPILER:'):
            return Path(line.split('=', 1)[1]).parent
    return None


class Runner:
    """Process control: the A/B gate before each command, below-normal priority, the toolchain's
    runtime first on PATH (Git Bash's PATH otherwise shadows libstdc++), a command log."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.work: Path = args.work
        self.env = dict(os.environ)
        self.env['PYTHONDONTWRITEBYTECODE'] = '1'
        if args.toolchain_bin:
            self.env['PATH'] = str(args.toolchain_bin) + os.pathsep + self.env.get('PATH', '')
        self.log: list[dict] = []
        self.gate_waits = 0
        # The code that measures a row, recorded in the row itself (a resumed pass reuses rows).
        self.identity = {'tool_sha256': sha256(Path(__file__).resolve()), 'analysis_sha256': sha256(Path(wa.__file__).resolve()),
                         'git_head': git_head()}

    def gate(self) -> None:
        script = self.args.gate_script
        if script is None or not script.is_file():
            return
        while True:
            result = subprocess.run(['pwsh', '-NoProfile', '-File', str(script)], capture_output=True, text=True)
            first = (result.stdout.strip().splitlines() or [''])[0]
            if result.returncode == 0:
                return
            if result.returncode == GATE_HOLD_EXIT:
                self.gate_waits += 1
                print(f'gate: {first}; asking again', flush=True)
                continue
            raise RuntimeError(f'ab_gate.ps1 failed ({result.returncode}): {result.stdout}{result.stderr}')

    def run(self, command: list) -> tuple[float, str]:
        command = [str(part) for part in command]
        self.gate()
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True, env=self.env, creationflags=BELOW_NORMAL)
        seconds = round(time.monotonic() - started, 3)
        self.log.append({'argv': command, 'returncode': result.returncode, 'wall_seconds': seconds,
                         'stdout': result.stdout.strip()[-1500:], 'stderr': result.stderr.strip()[-1500:]})
        if result.returncode != 0:
            raise RuntimeError(f'command failed (rc={result.returncode}): {" ".join(command)}\n{result.stdout}{result.stderr}')
        return seconds, result.stdout

    def inside(self, *parts: str) -> Path:
        path = (self.work.joinpath(*parts)).resolve()
        if not path.is_relative_to(self.work.resolve()):
            raise ValueError(f'refusing to write outside the work directory: {path}')
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    def discard(self, path: Path) -> None:
        if not self.args.keep_outputs and path.is_file() and path.stat().st_size > (64 << 20):
            path.unlink()


# ================================================================================================ probes

@dataclasses.dataclass
class Probe:
    name: str
    path: Path
    sidecar: dict
    shifted: Path | None = None  # half-sample shifted PCM16 copy for pathsim's 8 kHz level seed (calib, probe prep)

    @property
    def kind(self) -> str:
        return self.sidecar['probe']

    @property
    def body_start(self) -> int:
        return int(self.sidecar['body']['start_sample'])

    @property
    def body_frames(self) -> int:
        return int(self.sidecar['body']['samples'])

    @property
    def tone_hz(self) -> float:
        return float(self.sidecar['params']['tone_hz'])

    def reference(self) -> dict:
        return {'start_seconds': self.body_start / FS, 'duration_seconds': self.body_frames / FS}

    def analysis_window(self, latency: int, margin_s: float = 0.5) -> tuple[int, int]:
        margin = int(margin_s * FS)
        return self.body_start + latency + margin, self.body_frames - 2 * margin


class Probes:
    """The lane's calib probes (read only, used in place: no tool writes beside them) and the ones the
    lane's gen_probes.py writes into <work>/probes; shift_long.py (calib appE) makes the shifted copies."""

    def __init__(self, runner: Runner) -> None:
        self.runner = runner
        self.lane_calib = runner.args.lane_root / 'hfchannel/calib/probes'
        self.made = runner.inside('probes', 'x').parent
        self.cache: dict[str, Probe] = {}

    def get(self, name: str) -> Probe:
        if name in self.cache:
            return self.cache[name]
        lane = self.lane_calib / f'probe_{name}.wav'
        path = lane if lane.is_file() else self.made / f'probe_{name}.wav'
        if not path.is_file():
            self.generate(name)
        sidecar = read_json(path.with_suffix('.json'))
        shifted = path.with_name(f'probe_{name}_hs.wav')
        probe = Probe(name, path, sidecar, shifted if shifted.is_file() else None)
        self.cache[name] = probe
        return probe

    def generate(self, name: str) -> None:
        kind, duration = name.rsplit('_', 1)
        seconds = duration.rstrip('s')
        script = self.runner.args.lane_root / 'hfchannel/probes/gen_probes.py'
        command = [sys.executable, '-B', script, '--out', self.made, '--duration', seconds, '--suffix', f'_{duration}']
        if kind.startswith('tone'):
            command += ['--only', 'tone', '--tone-hz', kind[4:]]
        elif kind == 'silence':
            command += ['--only', 'silence', '--silence-duration', seconds]
        else:
            command += ['--only', kind]
        self.runner.run(command)

    def shifted(self, name: str) -> Path:
        """The probe's half-sample shifted copy (pathsim's tone rows; calib 'Probe prep')."""
        probe = self.get(name)
        if probe.shifted is not None:
            return probe.shifted
        target = self.made / f'probe_{name}_hs.wav'
        if not target.is_file():
            script = self.runner.args.lane_root / 'hfchannel/calib/appE/shift_long.py'
            self.runner.run([sys.executable, '-B', script, '--in', probe.path, '--out', target])
        probe.shifted = target
        return target


# ================================================================================================ rendering

@dataclasses.dataclass(frozen=True)
class P:
    """One path of a scenario (docs/modules/signal-lab-watterson.md, "Scenario schema")."""
    delay_ms: float = 0.0
    spread_hz: float | None = None  # None: a fixed path
    power_db: float = 0.0
    shift_hz: float = 0.0
    phase_deg: float = 0.0

    def json(self) -> dict:
        item: dict = {'delay_ms': self.delay_ms}
        if self.spread_hz is None:
            item['fading'] = 'fixed'
            if self.phase_deg:
                item['phase_deg'] = self.phase_deg
        else:
            item['spread_hz'] = self.spread_hz
        if self.power_db:
            item['power_db'] = self.power_db
        if self.shift_hz:
            item['shift_hz'] = self.shift_hz
        return item


def scenario(test_id: str, paths: tuple[P, ...], snr_db: float | None, *, seed: int = 1, gain_db: float = STAGE_GAIN_DB,
             reference: dict | str | None = 'auto') -> dict:
    impairments = []
    if paths:
        impairments.append({'type': 'watterson', 'paths': [p.json() for p in paths]})
    if snr_db is not None:
        impairments.append({'type': 'awgn', 'snr_db': snr_db})
    doc = {'schema': 'signal-lab.scenario/1', 'test_id': test_id, 'seed': seed, 'source_gain_db': gain_db,
           'clip_policy': 'reject', 'impairments': impairments}
    if reference is not None:
        doc['reference'] = reference
    return doc


def pathsim_options(paths: tuple[P, ...], snr_db: float | None) -> list[str] | None:
    """pathsim_chain's options for the same channel (design 10.3), or None where pathsim lacks it:
    more than 3 paths, a first path not at 0 ms, unequal powers, a fixed path's phase."""
    if len(paths) > 3 or any(p.power_db or p.phase_deg for p in paths) or (paths and paths[0].delay_ms != 0.0):
        return None
    options: list[str] = []
    if not paths:
        options.append('--direct')
    for index, p in enumerate(paths):
        suffix = '' if index == 0 else str(index + 1)
        if index:
            options += [f'--delay{suffix}', f'{p.delay_ms:g}']
        options += [f'--spread{suffix}', f'{(p.spread_hz or 0.0):g}']
        if p.shift_hz:
            options += [f'--offset{suffix}', f'{p.shift_hz:g}']
    if snr_db is not None:
        options += ['--snr', f'{snr_db:.2f}']
    return options


def render_stage(runner: Runner, row_id: str, doc: dict, *, source: Path | None = None, silence_s: float | None = None,
                 reference_rms: float | None = None, truth: bool = False, block: int | None = None,
                 seed: int | None = None, folder: str = 'out', scenario_dir: tuple[str, ...] = ('scenarios',)) -> dict:
    scenario_path = runner.inside(*scenario_dir, f'{row_id}.json')
    write_json(scenario_path, doc)
    output = runner.inside(folder, f'{row_id}.stage.wav')
    sidecar = output.with_suffix('.json')
    command = [runner.args.driver, '--scenario', scenario_path, '--output', output, '--sidecar', sidecar]
    command += ['--source', source] if source is not None else ['--silence-seconds', f'{silence_s:g}']
    if reference_rms is not None:
        command += ['--reference-rms', repr(float(reference_rms))]
    if truth:
        command += ['--truth-out', output.with_suffix('.taps')]
    if block is not None:
        command += ['--block', block]
    if seed is not None:
        command += ['--seed', seed]
    seconds, stdout = runner.run(command)
    info = read_json(sidecar)
    return {'simulator': 'stage', 'argv': [str(c) for c in command], 'seconds': seconds, 'wav': output, 'sidecar': info,
            'truth': output.with_suffix('.taps') if truth else None, 'scenario': doc, 'scenario_sha256': sha256(scenario_path),
            'digest': info['output_digest'], 'stdout': stdout.strip()}


def render_pathsim(runner: Runner, row_id: str, options: list[str], source: Path, seed: int = 1, folder: str = 'out') -> dict:
    output = runner.inside(folder, f'{row_id}.pathsim.wav')
    command = [runner.args.chain, *options, '--seed', str(seed), source, output]
    seconds, _ = runner.run(command)
    return {'simulator': 'pathsim', 'argv': [str(c) for c in command], 'seconds': seconds, 'wav': output, 'options': options}


def lane_analyzer(runner: Runner, kind: str, wav: Path, ref: Path, label: str, extra: list | None = None,
                  offset: int | None = None, analysis_dir: tuple[str, ...] = ('analysis',)) -> dict:
    """One of the lane's analyzers on a render. Where a static comb hides the lead marker (calib A4/B4:
    pathsim's 5 ms static case needed it too), the probe start is passed instead with --offset: the
    simulator's latency plus the earliest path's delay (design 3.4)."""
    script = runner.args.lane_root / 'hfchannel/probes' / f'analyze_{kind}.py'
    out = runner.inside(*analysis_dir, f'{label}.{kind}.json')
    command = [sys.executable, '-B', script, '--in', wav, '--ref', ref, '--out', out, *(extra or [])]
    try:
        seconds, _ = runner.run(command)
        fallback = False
    except RuntimeError as error:
        if offset is None or 'marker not found' not in str(error):
            raise
        command += ['--offset', str(offset)]
        seconds, _ = runner.run(command)
        fallback = True
    result = read_json(out)
    result['_argv'] = [str(c) for c in command]
    result['_seconds'] = seconds
    result['_offset_fallback'] = fallback
    return result


def finish(runner: Runner, render: dict) -> dict:
    """The render's record: hashes kept, large outputs deleted unless --keep-outputs."""
    record = {k: (str(v) if isinstance(v, Path) else v) for k, v in render.items() if k not in ('sidecar',)}
    wav = render['wav']
    record['sha256'] = sha256(wav)
    record['bytes'] = wav.stat().st_size
    if render.get('sidecar'):
        side = render['sidecar']
        record['report'] = {k: side.get(k) for k in ('output_digest', 'parameter_digest', 'frames_out', 'clipped_samples', 'peak',
                                                     'reference_rms', 'scaled_reference_rms', 'noise_band_enbw_hz', 'block_frames', 'seed')}
        record['watterson'] = side.get('watterson')
    runner.discard(wav)
    return record


def row_cached(runner: Runner, row_id: str, rows_dir: tuple[str, ...] = ('rows',)) -> dict | None:
    path = runner.work.joinpath(*rows_dir, f'{row_id}.json')
    return read_json(path) if runner.args.resume and path.is_file() else None


def row_store(runner: Runner, row_id: str, record: dict, rows_dir: tuple[str, ...] = ('rows',)) -> dict:
    record['id'] = row_id
    record['measured_by'] = dict(runner.identity) | {'utc': utc_now()}
    if record.get('pass') is not None:
        record['pass'] = bool(record['pass'])
    write_json(runner.inside(*rows_dir, f'{row_id}.json'), record)
    print(f"row {row_id}: {'PASS' if record.get('pass') else 'FAIL' if record.get('pass') is False else 'info'}", flush=True)
    return record


# ================================================================================================ criterion 1

def c1_snr_ladder(runner: Runner, probes: Probes) -> dict:
    """B1: AWGN only and one fixed path + AWGN, labels 0-30 dB, tone1000 and gapnoise, 1200 s each;
    E.7.2 two-capture at 10 and 30 dB. pathsim at the lane's command (target - 0.40) on the same probes."""
    labels = (0.0, 5.0, 10.0, 18.0, 20.0, 30.0)
    duration = '120s' if runner.args.quick else '1200s'
    rows = []
    for geometry, paths in (('awgn', ()), ('fixed', (P(),))):
        for probe_name in (f'tone1000_{duration}', f'gapnoise_{duration}'):
            probe = probes.get(probe_name)
            for label in labels:
                row_id = f'c1-{geometry}-{probe.kind}-{label:g}dB'
                cached = row_cached(runner, row_id)
                if cached:
                    rows.append(cached)
                    continue
                ours = render_stage(runner, row_id, scenario(row_id, paths, label, reference=probe.reference()), source=probe.path)
                a = lane_analyzer(runner, 'snr', ours['wav'], probe.path, f'{row_id}.stage')
                options = pathsim_options(paths, label + PATHSIM_COMMAND_OFFSET_DB)
                ps_input = probes.shifted(probe_name) if probe.kind == 'tone' else probe.path
                theirs = render_pathsim(runner, row_id, options, ps_input)
                b = lane_analyzer(runner, 'snr', theirs['wav'], probe.path, f'{row_id}.pathsim')
                se = a.get('snr_standard_error_db') or 0.0
                judged = SNR_TOLERANCE_DB >= SE_FACTOR * se
                delta = a['snr_db'] - label
                record = {'criterion': 1, 'geometry': geometry, 'probe': probe.name, 'label_db': label,
                          'stage': {'snr_db': a['snr_db'], 'se_db': se, 'delta_db': delta, 'band_integral_db': a.get('snr_db_band_integral'),
                                    'noise_bandwidth_hz': a.get('noise_bandwidth_hz'), 'warnings': a.get('warnings'), 'render': finish(runner, ours)},
                          'pathsim': {'commanded_db': label + PATHSIM_COMMAND_OFFSET_DB, 'snr_db': b['snr_db'],
                                      'se_db': b.get('snr_standard_error_db'), 'delta_vs_target_db': b['snr_db'] - label,
                                      'delivered_minus_commanded_db': b['snr_db'] - label - PATHSIM_COMMAND_OFFSET_DB,
                                      'band_integral_db': b.get('snr_db_band_integral'), 'render': finish(runner, theirs)},
                          'judged': judged, 'pass': (abs(delta) <= SNR_TOLERANCE_DB) if judged else None}
                rows.append(row_store(runner, row_id, record))
    rows += c1_e72(runner, probes, '120s' if runner.args.quick else E72_PROBE)
    seeds = c1_offset_seeds(runner, probes, duration)
    ladder = [r for r in rows if 'label_db' in r and r.get('kind') != 'e72']
    deltas = np.array([r['stage']['delta_db'] for r in ladder])
    ses = np.array([r['stage']['se_db'] for r in ladder])
    weights = 1.0 / np.maximum(ses, 1e-6) ** 2
    mean = float(np.sum(weights * deltas) / np.sum(weights))
    mean_se = float(1.0 / math.sqrt(np.sum(weights)))
    chi2 = float(np.sum(weights * (deltas - mean) ** 2))
    judged = [r for r in ladder if r['judged']]
    e72 = [r for r in rows if r.get('kind') == 'e72']
    passed = bool(judged) and all(r['pass'] for r in judged) and len(judged) == len(ladder) and all(r['pass'] for r in e72)
    passed = passed and seeds['pass']
    return {'criterion': 1, 'title': 'delivered SNR (ladder, E.7.2)', 'rows': rows, 'pass': passed, 'offset_seeds': seeds,
            'offset': {'weighted_mean_db': mean, 'weighted_mean_se_db': mean_se, 'chi2': chi2, 'dof': len(ladder) - 1},
            'enbw_hz': ladder[0]['stage']['render']['report']['noise_band_enbw_hz'] if ladder else None,
            'max_abs_delta_db': float(np.max(np.abs(deltas))) if len(deltas) else None,
            'rows_judged': len(judged), 'rows_total': len(ladder)}


def c1_offset_seeds(runner: Runner, probes: Probes, duration: str) -> dict:
    """The ladder's rows share one noise realisation per geometry and probe (the AWGN draws depend on
    the seed, the stage index and the frame, and the label only scales them), so their errors are
    correlated. The constant offset is therefore measured over independent realisations: AWGN only at
    10 dB, seeds 1 to 8, both probes, on both simulators (pathsim at the lane's command)."""
    rows = []
    for probe_name in (f'tone1000_{duration}', f'gapnoise_{duration}'):
        probe = probes.get(probe_name)
        for seed in range(1, OFFSET_SEEDS + 1):
            row_id = f'c1-seed{seed}-awgn-{probe.kind}-10dB'
            cached = row_cached(runner, row_id)
            if cached:
                rows.append(cached)
                continue
            ours = render_stage(runner, row_id, scenario(row_id, (), 10.0, seed=seed, reference=probe.reference()), source=probe.path)
            a = lane_analyzer(runner, 'snr', ours['wav'], probe.path, f'{row_id}.stage')
            ps_input = probes.shifted(probe_name) if probe.kind == 'tone' else probe.path
            theirs = render_pathsim(runner, row_id, pathsim_options((), 10.0 + PATHSIM_COMMAND_OFFSET_DB), ps_input, seed=seed)
            b = lane_analyzer(runner, 'snr', theirs['wav'], probe.path, f'{row_id}.pathsim')
            record = {'criterion': 1, 'kind': 'offset-seed', 'probe': probe.name, 'seed': seed, 'label_db': 10.0,
                      'stage': {'snr_db': a['snr_db'], 'se_db': a.get('snr_standard_error_db'), 'delta_db': a['snr_db'] - 10.0,
                                'render': finish(runner, ours)},
                      'pathsim': {'snr_db': b['snr_db'], 'se_db': b.get('snr_standard_error_db'), 'delta_vs_target_db': b['snr_db'] - 10.0,
                                  'render': finish(runner, theirs)},
                      'pass': None}
            rows.append(row_store(runner, row_id, record))
    summary = {}
    for kind in ('tone', 'gapnoise'):
        for simulator, key in (('stage', 'delta_db'), ('pathsim', 'delta_vs_target_db')):
            values = np.array([r[simulator][key] for r in rows if r['probe'].startswith(kind)])
            mean = float(np.mean(values))
            se = float(np.std(values, ddof=1) / math.sqrt(len(values)))
            summary[f'{simulator}-{kind}'] = {'mean_db': mean, 'se_db': se, 'sd_db': float(np.std(values, ddof=1)), 'n': int(len(values)),
                                              'min_db': float(np.min(values)), 'max_db': float(np.max(values))}
    # The stage's convention has no offset (awgn.hpp): each probe's mean offset within 3 s.e. of 0 and
    # every realisation within the lane's 0.1 dB.
    stage_ok = all(abs(v['mean_db']) <= max(3 * v['se_db'], 0.01) for k, v in summary.items() if k.startswith('stage')) and \
        all(abs(r['stage']['delta_db']) <= SNR_TOLERANCE_DB for r in rows)
    return {'rows': rows, 'summary': summary, 'pass': stage_ok}


def analysed_seconds(result: dict) -> float | None:
    """The duration a lane analyzer averaged over: its interval after the 0.25 s margins, or, where it
    reports both (analyze_doppler's baseband is a little shorter), the shorter of the two."""
    values = [v for v in ((result.get('interval') or {}).get('duration_s'), result.get('duration_s')) if v is not None]
    return min(values) if values else None


def c1_e72(runner: Runner, probes: Probes, duration: str) -> list[dict]:
    """E.7.2: tone1000 and silence through one fixed path + AWGN, two captures of at least 20 min of
    analysed interval each (the probes are 1210 s, so the analysers' margins leave more than 1200 s)."""
    rows = []
    tone = probes.get(f'tone1000_{duration}')
    silence = probes.get(f'silence_{duration}')
    minimum = 0.0 if runner.args.quick else E72_MIN_S
    for label in (10.0, 30.0):
        row_id = f'c1-e72-{label:g}dB'
        cached = row_cached(runner, row_id)
        if cached:
            rows.append(cached)
            continue
        paths = (P(),)
        ours_tone = render_stage(runner, f'{row_id}-tone', scenario(row_id, paths, label, reference=tone.reference()), source=tone.path)
        reference = ours_tone['sidecar']['reference_rms']
        ours_noise = render_stage(runner, f'{row_id}-noise', scenario(row_id, paths, label, reference=None), source=silence.path,
                                  reference_rms=reference)
        a = lane_analyzer(runner, 'snr', ours_tone['wav'], tone.path, f'{row_id}.stage',
                          ['--noise-in', ours_noise['wav'], '--noise-ref', silence.path])
        options = pathsim_options(paths, label + PATHSIM_COMMAND_OFFSET_DB)
        ps_tone = render_pathsim(runner, f'{row_id}-tone', options, probes.shifted(tone.name))
        ps_noise = render_pathsim(runner, f'{row_id}-noise', options, silence.path)
        b = lane_analyzer(runner, 'snr', ps_tone['wav'], tone.path, f'{row_id}.pathsim',
                          ['--noise-in', ps_noise['wav'], '--noise-ref', silence.path])
        e72 = a['e72']['snr_db_band_integral']
        tone_seconds = analysed_seconds(a)
        noise_seconds = analysed_seconds(a['e72'].get('noise_interval') or {})
        record = {'criterion': 1, 'kind': 'e72', 'label_db': label, 'probe': tone.name, 'minimum_analysed_s': minimum,
                  'stage': {'e72_band_db': e72, 'e72_density_db': a['e72'].get('snr_db_density_3000'),
                            'noise_equiv_width_hz': a['e72'].get('noise_capture_equiv_width_hz'),
                            'tone_method_db': a['snr_db'], 'delta_db': e72 - label,
                            'analysed_s': {'tone': tone_seconds, 'noise': noise_seconds},
                            'renders': [finish(runner, ours_tone), finish(runner, ours_noise)]},
                  'pathsim': {'e72_band_db': b['e72']['snr_db_band_integral'], 'e72_density_db': b['e72'].get('snr_db_density_3000'),
                              'noise_equiv_width_hz': b['e72'].get('noise_capture_equiv_width_hz'),
                              'delta_vs_target_db': b['e72']['snr_db_band_integral'] - label,
                              'renders': [finish(runner, ps_tone), finish(runner, ps_noise)]},
                  'pass': abs(e72 - label) <= E72_TOLERANCE_DB and min(tone_seconds or 0.0, noise_seconds or 0.0) >= minimum}
        rows.append(row_store(runner, row_id, record))
    return rows


def c1_fading(runner: Runner, probes: Probes) -> dict:
    """B2 / E.7.3: single and dual 2 ms paths at 1 and 10 Hz, 20 dB, gapnoise 2 h each. The realised
    tap power comes from the same render without AWGN (the channel's realisation is a function of
    the seed and the frame only, so it is y - w exactly): S(channel only) - S(input) in the band."""
    probe = probes.get('gapnoise_600s' if runner.args.quick else f'gapnoise_{E73_PROBE}')
    minimum = 0.0 if runner.args.quick else E73_MIN_S
    clean = lane_analyzer(runner, 'snr', probe.path, probe.path, f'c1-fading-input-{probe.name}')
    geometries = (('single_1Hz', (P(spread_hz=1.0),), 'e73_single_1Hz_2h_snr.json'),
                  ('dual_2ms_1Hz', (P(spread_hz=1.0), P(delay_ms=2.0, spread_hz=1.0)), 'e73_dual_2ms_1Hz_2h_snr.json'),
                  ('single_10Hz', (P(spread_hz=10.0),), 'e73_single_10Hz_2h_snr.json'),
                  ('dual_2ms_10Hz', (P(spread_hz=10.0), P(delay_ms=2.0, spread_hz=10.0)), 'e73_dual_2ms_10Hz_2h_snr.json'))
    rows = []
    label = 20.0
    for name, paths, baseline in geometries:
        row_id = f'c1-e73-{name}-{probe.name}'
        cached = row_cached(runner, row_id)
        if cached:
            rows.append(cached)
            continue
        noisy = render_stage(runner, row_id, scenario(row_id, paths, label, reference=probe.reference()), source=probe.path)
        a = lane_analyzer(runner, 'snr', noisy['wav'], probe.path, f'{row_id}.stage')
        noisy_record = finish(runner, noisy)
        channel = render_stage(runner, f'{row_id}-channel', scenario(row_id, paths, None, reference=probe.reference()), source=probe.path)
        c = lane_analyzer(runner, 'snr', channel['wav'], probe.path, f'{row_id}-channel.stage')
        channel_record = finish(runner, channel)
        tap_power = c['S_dbfs'] - (clean['S_dbfs'] + STAGE_GAIN_DB)
        delivered = a['snr_db']
        lane_result = runner.args.lane_root / 'hfchannel/calib/appE/results' / baseline
        stored = read_json(lane_result) if lane_result.is_file() else None
        seconds = analysed_seconds(a)
        record = {'criterion': 1, 'kind': 'e73', 'geometry': name, 'probe': probe.name, 'label_db': label, 'minimum_analysed_s': minimum,
                  'stage': {'snr_db': delivered, 'se_db': a.get('snr_standard_error_db'), 'tap_power_db': tap_power,
                            'delta_db': delivered - label, 'residual_db': delivered - label - tap_power, 'analysed_s': seconds,
                            'warnings': a.get('warnings'), 'renders': [noisy_record, channel_record]},
                  'pathsim_baseline': {'source': str(lane_result), 'command_label_db': label,
                                       'snr_db': stored['snr_db'] if stored else None,
                                       'se_db': stored.get('snr_standard_error_db') if stored else None,
                                       'delivered_minus_label_db': stored['snr_db'] - label if stored else None,
                                       'expected_db': label + PATHSIM_LABEL_OFFSET_DB},
                  'pass': abs(delivered - label) <= E73_TOLERANCE_DB and abs(delivered - label - tap_power) <= SNR_TOLERANCE_DB
                  and (seconds or 0.0) >= minimum}
        rows.append(row_store(runner, row_id, record))
    return {'criterion': 1, 'title': 'delivered SNR under fading (E.7.3)', 'rows': rows, 'pass': all(r['pass'] for r in rows)}


# ================================================================================================ criterion 2

def frozen_tap_render(runner: Runner, probe: Probe, floors: dict) -> dict:
    """The measurement floor of the image check: the same probe through one fixed path (no tap process,
    no AWGN), measured with the same windows. Its image windows hold only the probe's own spectrum,
    the window's leakage and the PCM quantisation. Rendered once per probe and kept until doppler_rows
    ends (finish() would delete it)."""
    if probe.name not in floors:
        row_id = f'c2-image-floor-{probe.name}'
        floors[probe.name] = render_stage(runner, row_id, scenario(row_id, (P(),), None, reference=probe.reference()), source=probe.path)
    return floors[probe.name]


def doppler_rows(runner: Runner, probes: Probes) -> list[dict]:
    """B3: one Rayleigh path, d = 0.1 ... 10 Hz, tone1700 without AWGN; analyze_doppler on both
    simulators; the direct image check on the stage's 48 kHz output, beside the same check on a
    frozen-tap render of the probe (the floor the image levels must clear)."""
    spreads = (0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0)
    rows = []
    floors: dict[str, dict] = {}
    for spread in spreads:
        duration = '1800s' if spread <= 1.0 and not runner.args.quick else '600s'
        probe = probes.get(f'tone1700_{duration}')
        row_id = f'c2-doppler-{spread:g}Hz-{duration}'
        cached = row_cached(runner, row_id)
        if cached:
            rows.append(cached)
            continue
        paths = (P(spread_hz=spread),)
        ours = render_stage(runner, row_id, scenario(row_id, paths, None, reference=probe.reference()), source=probe.path)
        a = lane_analyzer(runner, 'doppler', ours['wav'], probe.path, f'{row_id}.stage', ['--label-spread', f'{spread:g}'])
        node_rate = ours['sidecar']['watterson']['paths'][0]['node_rate_hz']
        frozen = frozen_tap_render(runner, probe, floors)
        runner.gate()
        start, count = probe.analysis_window(LATENCY_FRAMES)
        images = wa.image_check(ours['wav'], start, count, probe.tone_hz, spread, node_rate)
        floor = wa.image_check(frozen['wav'], start, count, probe.tone_hz, spread, node_rate)
        images['floor'] = {'render': frozen['wav'].name, 'worst_image_window_dbc': floor['worst_image_dbc'],
                           'images': floor['images'], 'sidelobe_continuum_dbc': floor['sidelobe_continuum_dbc']}
        images['clearance_db'] = images['worst_image_dbc'] - floor['worst_image_dbc']
        theirs = render_pathsim(runner, row_id, pathsim_options(paths, None), probes.shifted(probe.name))
        b = lane_analyzer(runner, 'doppler', theirs['wav'], probe.path, f'{row_id}.pathsim', ['--label-spread', f'{spread:g}'])
        record = {'criterion': 2, 'kind': 'doppler', 'spread_hz': spread, 'probe': probe.name, 'node_rate_hz': node_rate,
                  'stage': doppler_summary(a, spread) | {'images': images, 'render': finish(runner, ours)},
                  'pathsim': doppler_summary(b, spread) | {'render': finish(runner, theirs)}}
        s = record['stage']
        record['pass'] = (abs(s['relative_error']) <= SPREAD_TOLERANCE and s['shape_class'].startswith('gaussian-like')
                          and (s['worst_spur_dbc'] is None or s['worst_spur_dbc'] <= IMAGE_LIMIT_DBC)
                          and images['worst_image_dbc'] <= IMAGE_LIMIT_DBC)
        rows.append(row_store(runner, row_id, record))
    for name, frozen in floors.items():
        finish(runner, frozen)
    return rows


def doppler_summary(result: dict, spread: float) -> dict:
    spurs = result.get('spurs') or []
    e1 = result.get('table_e1') or {}
    return {'spread_2sigma_hz': result['spread_2sigma_hz'], 'relative_error': result['spread_2sigma_hz'] / spread - 1.0,
            'full_fit_hz': result.get('spread_2sigma_full_fit_hz'), 'moment_hz': result.get('spread_2sigma_moment_hz'),
            'method': result.get('spread_method'), 'shape_class': result.get('shape_class', ''),
            'worst_spur_dbc': max((sp['level_dbc'] for sp in spurs if sp.get('level_dbc') is not None), default=None),
            'spurs': spurs[:4], 'table_e1': e1.get('points'), 'centre_hz': result.get('tone_centre_hz'),
            'envelope': result.get('envelope'), 'warnings': result.get('warnings'),
            'duration_s': result.get('duration_s')}


def e74_row(runner: Runner, probes: Probes) -> dict | None:
    """E.7.4: one 1 Hz path over at least 3 h of analysed interval (tone1700_10810s), Table E-I by
    analyze_doppler's Kaiser Welch."""
    if runner.args.quick:
        return None
    probe = probes.get(f'tone1700_{E74_PROBE}')
    row_id = f'c2-e74-1Hz-{E74_PROBE}'
    cached = row_cached(runner, row_id)
    if cached:
        return cached
    paths = (P(spread_hz=1.0),)
    ours = render_stage(runner, row_id, scenario(row_id, paths, None, reference=probe.reference()), source=probe.path)
    a = lane_analyzer(runner, 'doppler', ours['wav'], probe.path, f'{row_id}.stage', ['--label-spread', '1'])
    stored_path = runner.args.lane_root / 'hfchannel/calib/appE/results/e74_single_1Hz_3h_doppler.json'
    stored = read_json(stored_path) if stored_path.is_file() else None
    s = doppler_summary(a, 1.0)
    deviations = []
    for point in s['table_e1'] or []:
        for side in ('low', 'high'):
            deviations.append({'level_db': point['level_db'], 'side': side, 'deviation_db': point[f'deviation_db_{side}'],
                               'tolerance_db': E_I_TOLERANCE_DB[f"{point['level_db']:g}"]})
    seconds = analysed_seconds(a)
    record = {'criterion': 2, 'kind': 'e74', 'probe': probe.name, 'minimum_analysed_s': E74_MIN_S,
              'stage': s | {'e_i': deviations, 'analysed_s': seconds, 'render': finish(runner, ours)},
              'pathsim_baseline': {'source': str(stored_path), **(doppler_summary(stored, 1.0) if stored else {})},
              'pass': bool(deviations) and all(abs(d['deviation_db']) <= d['tolerance_db'] for d in deviations)
              and abs(s['relative_error']) <= SPREAD_TOLERANCE and (seconds or 0.0) >= E74_MIN_S}
    return row_store(runner, row_id, record)


def lane_doppler_module(runner: Runner):
    """The lane's analyze_doppler, imported read only (its analyze_baseband is the per-path analyzer)."""
    probes_dir = str(runner.args.lane_root / 'hfchannel/probes')
    if probes_dir not in sys.path:
        sys.path.insert(0, probes_dir)
    import analyze_doppler  # noqa: PLC0415
    return analyze_doppler


def truth_rows(runner: Runner) -> list[dict]:
    """Channel truth (survey C10) of a 4-path Rayleigh channel, 3600 s of nodes: per-path Doppler through
    the lane's analyze_baseband on the rebuilt gain at 500 Hz, node moments, KS, mean power against the
    setting, and the correlation of every pair (criteria 2 'per path' and 3)."""
    duration = 900.0 if runner.args.quick else 3600.0
    paths = (P(0.0, 0.5, 0.0), P(2.0, 1.0, -3.0), P(5.0, 2.0, -6.0), P(7.0, 5.0, -10.0))
    row_id = f'c3-truth-4path-{duration:g}s'
    cached = row_cached(runner, row_id)
    if cached:
        return [cached]
    return [row_store(runner, row_id, truth_record(runner, row_id, paths, duration, 3))]


def truth_record(runner: Runner, row_id: str, paths: tuple[P, ...], duration: float, criterion: int, seed: int = 1) -> dict:
    """The truth statistics of one Rayleigh channel (every path fading), from duration seconds of nodes."""
    render = render_stage(runner, row_id, scenario(row_id, paths, None, seed=seed, reference=None), silence_s=duration, reference_rms=1.0, truth=True)
    runner.gate()
    truth = wa.read_truth(render['truth'])
    reports = render['sidecar']['watterson']['paths']
    doppler = lane_doppler_module(runner)
    fs_bb = 500.0
    at = np.arange(0, int((duration - 1.0) * FS), FS // int(fs_bb), dtype=np.int64)
    gains = {}
    per_path = []
    weights = [10 ** (p.power_db / 10) for p in paths]
    for index, (spec, report) in enumerate(zip(paths, reports)):
        frames, nodes = truth[index]
        gain = wa.rebuild_gain(frames, nodes, report, at)
        gains[index] = gain
        result, warnings = doppler.analyze_baseband(gain, fs_bb, label_spread=spec.spread_hz)
        stats = wa.node_statistics(nodes, report['amplitude'], spec.spread_hz, report['node_rate_hz'])
        expected = weights[index] / sum(weights)
        realised = float(np.mean(np.abs(gain) ** 2))
        power_se = stats['mean_power_se']
        per_path.append({'path': index, 'delay_ms': spec.delay_ms, 'spread_hz': spec.spread_hz, 'power_db_set': spec.power_db,
                         'expected_power': expected, 'realised_power': realised,
                         'power_error_db': 10 * math.log10(realised / expected), 'power_z': (realised / expected - 1.0) / power_se,
                         'spread_2sigma_hz': result['spread_2sigma_hz'], 'relative_error': result['spread_2sigma_hz'] / spec.spread_hz - 1.0,
                         'shape_class': result['shape_class'], 'spurs': result['spurs'][:3],
                         'worst_spur_dbc': max((s['level_dbc'] for s in result['spurs'] if s.get('level_dbc') is not None), default=None),
                         'table_e1': result['table_e1']['points'], 'nodes': stats, 'warnings': warnings,
                         'normalisation': {'amplitude': report['amplitude'], 'interpolation_power': report['interpolation_power'],
                                           'a2v': report['amplitude'] ** 2 * report['interpolation_power']}})
    pairs = []
    for i in range(len(paths)):
        for j in range(i + 1, len(paths)):
            rho = wa.correlation(gains[i], gains[j])
            se = wa.correlation_se(paths[i].spread_hz, paths[j].spread_hz, duration)
            pairs.append({'paths': [i, j], 'rho': rho, 'se': se, 'bound': CORRELATION_SE_LIMIT * se, 'pass': rho <= CORRELATION_SE_LIMIT * se})
    normalisation = sum(p['normalisation']['a2v'] for p in per_path)
    record = {'criterion': criterion, 'kind': 'truth', 'seed': seed, 'duration_s': duration, 'paths': per_path, 'pairs': pairs, 'sum_a2v': normalisation,
              'render': finish(runner, render) | {'truth_bytes': render['truth'].stat().st_size}}
    record['pass_per_path_doppler'] = all(abs(p['relative_error']) <= SPREAD_TOLERANCE and p['shape_class'].startswith('gaussian-like')
                                          for p in per_path)
    record['pass_moments'] = all(p['nodes']['ks_distance'] < p['nodes']['ks_critical_95']
                                 and abs(p['nodes']['mean_over_rms'] - wa.RAYLEIGH_MEAN_OVER_RMS) <= max(MEAN_OVER_RMS_TOLERANCE, 3 * p['nodes']['mean_over_rms_se'])
                                 and abs(p['nodes']['fourth_moment_ratio'] - 2.0) <= 5 * p['nodes']['fourth_moment_ratio_se']
                                 for p in per_path)
    record['pass_powers'] = all(abs(p['power_z']) <= SE_FACTOR and abs(p['power_error_db']) <= POWER_TOLERANCE_DB for p in per_path) \
        and abs(normalisation - 1.0) <= 1e-9
    record['pass_correlation'] = all(p['pass'] for p in pairs)
    record['pass'] = record['pass_per_path_doppler'] and record['pass_moments'] and record['pass_powers'] and record['pass_correlation']
    render['truth'].unlink()
    return record


def delay_specs(quick: bool) -> list[tuple[str, tuple[P, ...], str]]:
    specs: list[tuple[str, tuple[P, ...], str]] = [('fixed0', (P(),), 'delay_60s')]
    for delay in (0.5, 1.0, 2.0, 5.0, 7.0):
        specs.append((f'static2-{delay:g}ms', (P(), P(delay_ms=delay)), 'delay_60s'))
    specs += [('d62-3k-wid2', (P(), P(3.0), P(9.0)), 'delay_60s'),
              ('d62-3k-wid10', (P(), P(2.0), P(4.5)), 'delay_60s'),
              ('d62-6k-wid10-4path', (P(), P(1.5), P(3.0), P(5.0)), 'delay_60s'),
              ('d62-48k-wid12-3dB', (P(), P(1.5, power_db=-3.0)), 'delay_60s'),
              ('unordered-7-0.5-2ms', (P(7.0), P(0.5), P(2.0)), 'delay_60s'),
              ('static2-20ms', (P(), P(20.0)), 'delay_60s'),
              ('fading-2ms-1Hz', (P(spread_hz=1.0), P(2.0, 1.0)), 'delay_300s' if quick else 'delay_1200s'),
              ('fading-4path-powers', (P(0.0, 1.0), P(2.0, 1.0, -3.0), P(5.0, 1.0, -6.0), P(7.0, 1.0, -10.0)),
               'delay_300s' if quick else 'delay_1200s')]
    return specs


def delay_rows(runner: Runner, probes: Probes) -> list[dict]:
    """B4: taps exact, relative powers, latency 215, E.7.1 or the analytic comb; pathsim where it has the paths."""
    rows = []
    for name, paths, probe_name in delay_specs(runner.args.quick):
        row_id = f'c2-delay-{name}'
        cached = row_cached(runner, row_id)
        rows.append(cached if cached else row_store(runner, row_id, delay_record(runner, probes, row_id, name, paths, probe_name, 2)))
    return rows


def delay_record(runner: Runner, probes: Probes, row_id: str, name: str, paths: tuple[P, ...], probe_name: str, criterion: int,
                 seed: int = 1) -> dict:
    """One B4 row: the delay probe through the channel and AWGN at 40 dB, analyze_delay on both simulators."""
    probe = probes.get(probe_name)
    ours = render_stage(runner, row_id, scenario(row_id, paths, 40.0, seed=seed, reference=probe.reference()), source=probe.path)
    reports = ours['sidecar']['watterson']['paths']
    first = min(int(r['delay_frames']) for r in reports)
    a = lane_analyzer(runner, 'delay', ours['wav'], probe.path, f'{row_id}.stage', offset=LATENCY_FRAMES + first)
    frame = int(a.get('frame_samples', 4096))
    bins = np.arange(frame // 2 + 1)
    band_bins = bins[(bins * FS / frame >= 300) & (bins * FS / frame <= 3300)]
    record = {'criterion': criterion, 'kind': 'delay', 'name': name, 'probe': probe.name, 'seed': seed,
              'paths': [dataclasses.asdict(p) for p in paths],
              'stage': delay_summary(a, paths, reports, band_bins, LATENCY_FRAMES) | {'render': finish(runner, ours),
                                                                                     'offset_fallback': a['_offset_fallback']}}
    options = pathsim_options(paths, 40.0)
    if options is not None:
        theirs = render_pathsim(runner, row_id, options, probe.path, seed=seed)
        b = lane_analyzer(runner, 'delay', theirs['wav'], probe.path, f'{row_id}.pathsim', offset=PATHSIM_LATENCY_FRAMES)
        record['pathsim'] = delay_summary(b, paths, None, band_bins, None) | {'render': finish(runner, theirs),
                                                                             'offset_fallback': b['_offset_fallback']}
    record['pass'] = record['stage']['pass']
    return record


def delay_summary(result: dict, paths: tuple[P, ...], reports: list[dict] | None, band_bins: np.ndarray, latency: int | None) -> dict:
    """Taps against the realised delays (frames, relative to the earliest) and the normalised powers."""
    delays = [int(r['delay_frames']) for r in reports] if reports else [int(round(p.delay_ms * FS / 1000.0)) for p in paths]
    weights = [10 ** (p.power_db / 10) for p in paths]
    powers = [w / sum(weights) for w in weights]
    order = sorted(range(len(paths)), key=lambda i: delays[i])
    expected_delays = [delays[i] - delays[order[0]] for i in order]
    expected_powers = [powers[i] for i in order]
    taps = sorted(result.get('taps', []), key=lambda t: t['delay_samples'])
    measured_delays = [t['delay_samples'] for t in taps]
    measured_powers = [t['power_rel_total'] for t in taps]
    same = len(taps) == len(paths)
    delay_errors = [m - e for m, e in zip(measured_delays, expected_delays)] if same else []
    power_errors = [10 * math.log10(m / e) for m, e in zip(measured_powers, expected_powers)] if same else []
    fixed = all(p.spread_hz is None for p in paths)
    amplitudes = [math.sqrt(p) for p in powers]
    comb = wa.static_comb([delays[i] for i in range(len(paths))], amplitudes, [p.phase_deg for p in paths], band_bins) if fixed else None
    response = result.get('frequency_response') or {}
    measured_rows = {tuple(r['band_hz']): r['db_rel_1800'] for r in response.get('rows', [])}
    if comb is not None:
        response_errors = [measured_rows[tuple(r['band_hz'])] - r['db_rel_1800'] for r in comb['rows'] if tuple(r['band_hz']) in measured_rows]
        point_errors = {k: response['e71_points_db'][k] - v for k, v in comb['e71_points_db'].items()}
    else:
        response_errors = []
        point_errors = {k: v for k, v in (response.get('e71_points_db') or {}).items()}
    expected_latency = latency + delays[order[0]] if latency is not None else None
    measured_latency = (result.get('latency') or {}).get('first_path_samples')
    out = {'n_taps': len(taps), 'expected_delays': expected_delays, 'measured_delays': measured_delays, 'delay_errors': delay_errors,
           'expected_powers': expected_powers, 'measured_powers': measured_powers, 'power_errors_db': power_errors,
           'latency_frames': measured_latency, 'expected_latency_frames': expected_latency,
           'e71_points_error_db': point_errors, 'response_rows_max_error_db': max((abs(e) for e in response_errors), default=None),
           'comb_expected': comb is not None and len(paths) > 1, 'pslr_db': result.get('pslr_db'), 'warnings': result.get('warnings')}
    checks = [same, all(abs(e) <= DELAY_TOLERANCE_FRAMES for e in delay_errors), all(abs(e) <= POWER_TOLERANCE_DB for e in power_errors),
              all(abs(e) <= RESPONSE_TOLERANCE_DB for e in point_errors.values()),
              all(abs(e) <= RESPONSE_TOLERANCE_DB for e in response_errors)]
    if expected_latency is not None:
        checks.append(measured_latency is not None and abs(measured_latency - expected_latency) <= DELAY_TOLERANCE_FRAMES)
    out['pass'] = all(checks)
    return out


# ================================================================================================ criterion 3

def rician_rows(runner: Runner, probes: Probes) -> list[dict]:
    """B5's Rician row: a fixed 0 dB path and a 1 Hz Rayleigh path at -6 dB at the same delay
    (K = 6 dB): K from the moments of the tone's complex envelope on the 48 kHz output (600 s), and
    from the channel truth over 3600 s, with batch-means standard errors."""
    rows = []
    paths = (P(), P(spread_hz=1.0, power_db=-6.0))
    k_set = 6.0
    probe = probes.get('tone1700_600s')
    row_id = 'c3-rician-6dB'
    cached = row_cached(runner, row_id)
    if cached:
        return [cached]
    ours = render_stage(runner, row_id, scenario(row_id, paths, None, reference=probe.reference()), source=probe.path)
    lane_doppler_module(runner)
    common = sys.modules['common']
    start, count = probe.analysis_window(LATENCY_FRAMES)
    runner.gate()
    x = wa.read_frames(ours['wav'], start, count)
    z, _ = common.ddc(x, FS, probe.tone_hz, 96, 150.0)
    z = z[100:-100]
    audio = wa.rician_k(z)
    audio['batches'] = [wa.rician_k(b)['k_db'] for b in np.array_split(z, 10)]
    audio['k_db_se'] = float(np.std(audio['batches'], ddof=1) / math.sqrt(10))
    duration = 900.0 if runner.args.quick else 3600.0
    t = render_stage(runner, f'{row_id}-truth', scenario(row_id, paths, None, reference=None), silence_s=duration, reference_rms=1.0, truth=True)
    truth = wa.read_truth(t['truth'])
    reports = t['sidecar']['watterson']['paths']
    at = np.arange(0, int((duration - 1.0) * FS), 96, dtype=np.int64)
    gain = sum(wa.rebuild_gain(*truth[i], reports[i], at) for i in range(2))
    k_truth = wa.rician_k(gain)
    k_truth['batches'] = [wa.rician_k(b)['k_db'] for b in np.array_split(gain, 10)]
    k_truth['k_db_se'] = float(np.std(k_truth['batches'], ddof=1) / math.sqrt(10))
    t['truth'].unlink()
    record = {'criterion': 3, 'kind': 'rician', 'k_set_db': k_set, 'audio_600s': audio, 'truth': k_truth | {'duration_s': duration},
              'renders': [finish(runner, ours), finish(runner, t)]}
    record['pass'] = abs(k_truth['k_db'] - k_set) <= max(K_TOLERANCE_DB, 3 * k_truth['k_db_se']) and \
        abs(audio['k_db'] - k_set) <= max(K_TOLERANCE_DB, 3 * audio['k_db_se'])
    rows.append(row_store(runner, row_id, record))
    return rows


def envelope_verdict(env: dict | None, spread_hz: float) -> dict:
    """B5 on analyze_doppler's envelope block: KS below its 95 % point; mean/RMS within 0.01 of 0.886
    (or 3 s.e. where 0.01 is below 3 s.e. at the lowest spreads); LCR ratio within 0.15 of 1 (or 3
    Poisson s.e. of the crossing count)."""
    if not env:
        return {'pass': False}
    n_eff = env['independent_samples_approx']
    mor_tol = max(MEAN_OVER_RMS_TOLERANCE, 3 * 0.2614 / math.sqrt(n_eff))
    lcr = []
    for row in env['level_crossings']:
        tol = max(LCR_TOLERANCE, 3.0 / math.sqrt(max(row['crossings'], 1)))
        lcr.append({'level_db': row['level_db_rel_rms'], 'ratio': row['lcr_ratio'], 'tolerance': tol, 'pass': abs(row['lcr_ratio'] - 1.0) <= tol})
    out = {'ks': env['ks_distance_rayleigh'], 'ks_critical': env['ks_critical_95_approx'], 'mean_over_rms': env['mean_over_rms'],
           'mean_over_rms_tolerance': mor_tol, 'lcr': lcr,
           'deep_fade': env['deep_fade_probability'], 'independent_samples': n_eff, 'spread_hz': spread_hz}
    out['pass'] = env['ks_distance_rayleigh'] < env['ks_critical_95_approx'] and \
        abs(env['mean_over_rms'] - wa.RAYLEIGH_MEAN_OVER_RMS) <= mor_tol and all(r['pass'] for r in lcr)
    return out


# ================================================================================================ criterion 4

def noise_rows(runner: Runner, probes: Probes) -> list[dict]:
    rows = []
    reference = 10 ** (-12.0 / 20.0)
    duration = '120s' if runner.args.quick else '1200s'
    silence = probes.get(f'silence_{duration}')
    # B6a: noise only, 10 dB against a -12 dBFS reference, unity source gain.
    row_id = f'c4-b6a-{duration}'
    cached = row_cached(runner, row_id)
    if cached:
        rows.append(cached)
    else:
        doc = scenario(row_id, (), 10.0, gain_db=0.0, reference=None)
        ours = render_stage(runner, row_id, doc, source=silence.path, reference_rms=reference)
        # The silence probe carries the lead and trail markers: analyse inside its body only.
        start, count = silence.analysis_window(0)
        runner.gate()
        stats = wa.noise_stats(ours['wav'], start, count, reference)
        comb = sorted({k * step for step in (23.4375, 46.875, 187.5, 750.0, IONOS_COMB_HZ) for k in range(1, 200)})
        lines = wa.line_search(ours['wav'], start, count, reference ** 2, [], comb)
        theirs = render_pathsim(runner, row_id, ['--direct', '--snr', f'{10.0 + PATHSIM_COMMAND_OFFSET_DB:.2f}'], silence.path)
        pin = 10 ** (PATHSIM_PIN_DBFS / 20.0)
        runner.gate()
        pstats = wa.noise_stats(theirs['wav'], start + PATHSIM_LATENCY_FRAMES, count, pin)
        record = {'criterion': 4, 'kind': 'b6a', 'stage': {'stats': stats, 'lines': lines, 'render': finish(runner, ours)},
                  'pathsim': {'stats': pstats, 'reference': 'pathsim pin 0.122 RMS', 'render': finish(runner, theirs)}}
        record['pass'] = b6a_pass(stats)
        record['pathsim_pass_same_rules'] = b6a_pass(pstats)
        rows.append(row_store(runner, row_id, record))
    rows.append(b6b_row(runner, reference))
    rows.append(b6c_row(runner, probes, duration))
    rows += b6d_rows(runner, probes, duration)
    return rows


def b6a_pass(stats: dict) -> bool:
    """Flatness in 100 Hz bands always; in 10 Hz bands where 0.2 dB is at least 5 s.e. of a band (1200 s
    gives 0.029 dB); kurtosis and the 3 and 4 sigma tails within 4 s.e.; ENBW; N0 x 3000."""
    tails_ok = all(abs(t['z']) <= GAUSS_Z_LIMIT for t in stats['tails'] if t['z'] is not None and t['k_sigma'] < 5.0)
    fine_judged = FLATNESS_TOLERANCE_DB >= SE_FACTOR * stats['flatness_10hz_standard_error_db']
    fine_ok = stats['flatness_10hz_max_abs_db'] <= FLATNESS_TOLERANCE_DB or not fine_judged
    return (stats['flatness_100hz_max_abs_db'] <= FLATNESS_TOLERANCE_DB and fine_ok
            and abs(stats['excess_kurtosis_z']) <= GAUSS_Z_LIMIT and tails_ok
            and abs(stats['enbw_measured_hz'] - ENBW_DESIGN_HZ) <= ENBW_TOLERANCE_HZ
            and abs(stats['delivered_snr_db_n0x3000'] - 10.0) <= SNR_TOLERANCE_DB)


def b6b_row(runner: Runner, reference: float) -> dict:
    """B6b: 5 h of AWGN on silence (no Watterson stage), folded at the engine block (2048), the render's
    only frame grid (the AWGN chunk edges repeat every 2048 frames), and at its divisors 1024, 256 and
    64; every in-band harmonic in dBc re the reference, and a -68 dBc positive control. The Watterson
    stage's grids are B6d's, on renders that pass through it."""
    seconds = 3600.0 if runner.args.quick else 18000.0
    row_id = f'c4-b6b-{seconds:g}s'
    cached = row_cached(runner, row_id)
    if cached:
        return cached
    doc = scenario(row_id, (), 10.0, gain_db=0.0, reference=None)
    ours = render_stage(runner, row_id, doc, silence_s=seconds, reference_rms=reference)
    runner.gate()
    folds = wa.fold_harmonics(ours['wav'], FS, int((seconds - 2.0) * FS), reference ** 2, FOLD_PERIODS, FOLD_DERIVED, inject_dbc=-68.0)
    record = {'criterion': 4, 'kind': 'b6b', 'seconds': seconds, 'stage': {'folds': folds, 'render': finish(runner, ours)}}
    record['pass'] = all(f['worst_dbc'] <= SPUR_LIMIT_DBC and f['threshold_dbc'] < SPUR_LIMIT_DBC for f in folds['folds']) \
        and folds['positive_control']['detected']
    return row_store(runner, row_id, record)


def b6c_row(runner: Runner, probes: Probes, duration: str) -> dict:
    """B6c: tone1000 through one fixed path + AWGN at 60 dB; lines in 300-3300 Hz other than the tone,
    against the same search on the clean probe (lines of the source itself) and pathsim."""
    probe = probes.get(f'tone1000_{duration}')
    row_id = f'c4-b6c-{duration}'
    cached = row_cached(runner, row_id)
    if cached:
        return cached
    tone = probe.tone_hz
    comb = sorted({tone + sign * k * step for step in (23.4375, 46.875, 187.5, 750.0) for k in range(1, 140) for sign in (-1, 1)}
                  | {k * IONOS_COMB_HZ for k in range(1, 10)})
    exclude = [(tone - 1.0, tone + 1.0)]
    ours = render_stage(runner, row_id, scenario(row_id, (P(),), 60.0, reference=probe.reference()), source=probe.path)
    start, count = probe.analysis_window(LATENCY_FRAMES)
    runner.gate()
    f, p, _ = wa.welch_hann(ours['wav'], start, count, FS)
    tone_power = wa.band_power(f, p, tone, 3.0)
    lines = wa.line_search(ours['wav'], start, count, tone_power, exclude, comb)
    theirs = render_pathsim(runner, row_id, ['--spread', '0', '--snr', f'{60.0 + PATHSIM_COMMAND_OFFSET_DB:.2f}'], probes.shifted(probe.name))
    runner.gate()
    pstart = probe.body_start + PATHSIM_LATENCY_FRAMES + FS // 2
    f, p, _ = wa.welch_hann(theirs['wav'], pstart, count, FS)
    plines = wa.line_search(theirs['wav'], pstart, count, wa.band_power(f, p, tone, 3.0), exclude, comb)
    record = {'criterion': 4, 'kind': 'b6c', 'probe': probe.name,
              'stage': {'tone_power': tone_power, 'lines': lines, 'render': finish(runner, ours)},
              'pathsim': {'lines': plines, 'render': finish(runner, theirs)}}
    worst = lines['worst_line_dbc']
    record['pass'] = (worst is None or worst <= SPUR_LIMIT_DBC) and lines['floor_dbc'] < SPUR_LIMIT_DBC and \
        (lines['worst_explicit_dbc'] is None or lines['worst_explicit_dbc'] <= SPUR_LIMIT_DBC)
    return row_store(runner, row_id, record)


def b6d_rows(runner: Runner, probes: Probes, duration: str) -> list[dict]:
    """B6d: lines beside a tone whose output depends on the Hilbert quadrature path and the rotator:
    tone1000 through a fixed path shifted +75 Hz, the same at -75 Hz, and a 1 Hz Rayleigh path shifted
    +75 Hz, each with AWGN at 60 dB, 1200 s, 0.1 Hz bins. The explicit candidates are the shifted tone
    +/- k fs / P for the stage's frame grids (P = 2048, 1024, 256, 64: 23.4375, 46.875, 187.5 and
    750 Hz) and the IONOS comb, so a defect keyed to the rotator's resync or to the 64-frame chunk shows
    as its comb. A fixed path's mirror (tone - shift) is a detection at its own level (B9: -89.5 dBc).
    The fading row excludes its own lobe and continuum (shifted tone +/- R / 2); its images at k R are
    detections at their B3 level (-79.6 dBc). Pass: as B6c, nothing above -70 dBc."""
    probe = probes.get(f'tone1000_{duration}')
    cases = (('fixed+75Hz', (P(shift_hz=75.0),)), ('fixed-75Hz', (P(shift_hz=-75.0),)),
             ('rayleigh-1Hz+75Hz', (P(spread_hz=1.0, shift_hz=75.0),)))
    rows = []
    for name, paths in cases:
        row_id = f'c4-b6d-{name}-{duration}'
        cached = row_cached(runner, row_id)
        if cached:
            rows.append(cached)
            continue
        ours = render_stage(runner, row_id, scenario(row_id, paths, 60.0, reference=probe.reference()), source=probe.path)
        report = ours['sidecar']['watterson']['paths'][0]
        shifted = probe.tone_hz + report['realised_shift_hz']
        node_rate = report['node_rate_hz']
        # The tone's own band: a fixed path's line (+/-1 Hz), or a fading path's lobe and continuum (+/-R / 2).
        half = node_rate / 2.0 if node_rate else 1.0
        exclude = [(shifted - half, shifted + half)]
        comb = sorted({shifted + sign * k * FS / frames for frames in WATTERSON_GRID_FRAMES.values() for k in range(1, 140) for sign in (-1, 1)}
                      | {k * IONOS_COMB_HZ for k in range(1, 10)})
        start, count = probe.analysis_window(LATENCY_FRAMES)
        runner.gate()
        f, p, _ = wa.welch_hann(ours['wav'], start, count, FS)
        tone_power = wa.band_power(f, p, shifted, half + 2.0)
        lines = wa.line_search(ours['wav'], start, count, tone_power, exclude, comb)
        record = {'criterion': 4, 'kind': 'b6d', 'case': name, 'probe': probe.name, 'shifted_tone_hz': shifted, 'excluded_hz': exclude[0],
                  'grids_frames': WATTERSON_GRID_FRAMES, 'stage': {'tone_power': tone_power, 'lines': lines, 'render': finish(runner, ours)}}
        worst = lines['worst_line_dbc']
        record['pass'] = (worst is None or worst <= SPUR_LIMIT_DBC) and lines['floor_dbc'] < SPUR_LIMIT_DBC and \
            (lines['worst_explicit_dbc'] is None or lines['worst_explicit_dbc'] <= SPUR_LIMIT_DBC)
        rows.append(row_store(runner, row_id, record))
    return rows


# ================================================================================================ criterion 6

def determinism_rows(runner: Runner, probes: Probes) -> list[dict]:
    """B8: the same render twice (separate processes), every block size of U21, another seed; sha256 of
    the PCM24 files and the engine's output digest. pathsim: the same seed twice and another seed."""
    cases = (('ccir-poor-18dB', (P(spread_hz=1.0), P(2.0, 1.0)), 18.0, 'gapnoise_120s'),
             ('4path-all-30dB', (P(0.0, 0.1, 0.0, 12.5), P(9.0, 10.0, -3.0), P(20.0, None, -6.0, 0.0, 30.0), P(0.5, None, 0.0, -75.0)),
              30.0, 'tone1700_120s'))
    rows = []
    for name, paths, snr, probe_name in cases:
        probe = probes.get(probe_name)
        row_id = f'c6-{name}'
        cached = row_cached(runner, row_id)
        if cached:
            rows.append(cached)
            continue
        doc = scenario(row_id, paths, snr, reference=probe.reference())
        runs = []
        for index, block in enumerate((2048,) + BLOCKS):
            render = render_stage(runner, f'{row_id}-b{block}-r{index}', doc, source=probe.path, block=block, folder='c6')
            runs.append({'block': block, 'seed': 1, 'sha256': sha256(render['wav']), 'digest': render['digest'],
                         'parameter_digest': render['sidecar']['watterson']['parameter_digest'], 'seconds': render['seconds'],
                         'wav': render['wav']})
        other = render_stage(runner, f'{row_id}-seed2', doc, source=probe.path, seed=2, folder='c6')
        x = wa.read_frames(runs[0]['wav'], 0, wa.frame_count(runs[0]['wav']))
        y = wa.read_frames(other['wav'], 0, wa.frame_count(other['wav']))
        differing = int(np.count_nonzero(x != y))
        reference = runs[0]
        identical = all(r['sha256'] == reference['sha256'] and r['digest'] == reference['digest'] for r in runs)
        same_parameters = all(r['parameter_digest'] == reference['parameter_digest'] for r in runs)
        other_digest = other['sidecar']['watterson']['parameter_digest']
        record = {'criterion': 6, 'case': name, 'probe': probe.name,
                  'runs': [{k: v for k, v in r.items() if k != 'wav'} for r in runs],
                  'other_seed': {'seed': 2, 'sha256': sha256(other['wav']), 'digest': other['digest'], 'parameter_digest': other_digest,
                                 'differing_samples': differing, 'frames': int(len(x))},
                  'identical': identical, 'parameter_digest_same': same_parameters}
        if pathsim_options(paths, snr) is not None:
            ps = []
            for seed in (1, 1, 2):
                theirs = render_pathsim(runner, f'{row_id}-pathsim-s{seed}-{len(ps)}', pathsim_options(paths, snr + PATHSIM_COMMAND_OFFSET_DB),
                                        probe.path, seed=seed, folder='c6')
                ps.append({'seed': seed, 'sha256': sha256(theirs['wav'])})
            record['pathsim'] = {'runs': ps, 'same_seed_identical': ps[0]['sha256'] == ps[1]['sha256'], 'other_seed_differs': ps[0]['sha256'] != ps[2]['sha256']}
        record['pass'] = identical and same_parameters and differing > 0 and other_digest != reference['parameter_digest']
        for r in runs:
            r['wav'].unlink()
        other['wav'].unlink()
        rows.append(row_store(runner, row_id, record))
    rows.append(engine_equivalence_row(runner, probes))
    gates = runner.args.pathsim_build / 'gates.json'
    if gates.is_file():
        report = read_json(gates)
        rows.append({'criterion': 6, 'kind': 'pathsim-gate3', 'source': str(gates), 'started_utc': report.get('started_utc'),
                     'gate3': report.get('gates', {}).get('gate3'), 'pass': None})
    return rows


def engine_equivalence_row(runner: Runner, probes: Probes) -> dict:
    """The driver is the engine: on a scenario without a Watterson stage (which signal_lab_render cannot
    configure yet, design WP7), the driver and the project's signal_lab_render write the same bytes."""
    probe = probes.get('gapnoise_120s')
    row_id = 'c6-driver-equals-signal_lab_render'
    cached = row_cached(runner, row_id)
    if cached:
        return cached
    doc = scenario(row_id, (), 18.0, reference=probe.reference())
    ours = render_stage(runner, row_id, doc, source=probe.path, folder='c6')
    if not runner.args.renderer.is_file():
        return row_store(runner, row_id, {'criterion': 6, 'kind': 'engine-equivalence', 'renderer': str(runner.args.renderer),
                                          'note': 'signal_lab_render not built', 'pass': None})
    theirs = runner.inside('c6', f'{row_id}.signal_lab_render.wav')
    runner.run([runner.args.renderer, '--scenario', runner.work / 'scenarios' / f'{row_id}.json', '--source', probe.path, '--output', theirs,
                '--block', '2048'])
    record = {'criterion': 6, 'kind': 'engine-equivalence', 'probe': probe.name, 'driver_sha256': sha256(ours['wav']),
              'signal_lab_render_sha256': sha256(theirs), 'renderer': str(runner.args.renderer),
              'renderer_sha256': sha256(runner.args.renderer)}
    record['pass'] = record['driver_sha256'] == record['signal_lab_render_sha256']
    ours['wav'].unlink()
    theirs.unlink()
    return row_store(runner, row_id, record)


# ================================================================================================ criterion 7

def coverage_rows(runner: Runner, probes: Probes) -> list[dict]:
    """B9: fixed paths at +/-75 Hz (frequency within 1 mHz, mirror <= -70 dBc), a Rayleigh path at
    +75 Hz (lobe centre and spread), a fixed path's 90 degree phase; pathsim's offset beside them."""
    probe = probes.get('tone1700_600s')
    rows = []
    for shift in (75.0, -75.0):
        row_id = f'c7-fixed-shift{shift:+g}Hz'
        cached = row_cached(runner, row_id)
        if cached:
            rows.append(cached)
            continue
        paths = (P(shift_hz=shift),)
        ours = render_stage(runner, row_id, scenario(row_id, paths, None, reference=probe.reference()), source=probe.path)
        realised = ours['sidecar']['watterson']['paths'][0]['realised_shift_hz']
        start, count = probe.analysis_window(LATENCY_FRAMES)
        runner.gate()
        measured = wa.tone_frequency(ours['wav'], start, count, probe.tone_hz + shift, probe.tone_hz - shift)
        theirs = render_pathsim(runner, row_id, pathsim_options(paths, None), probes.shifted(probe.name))
        runner.gate()
        pmeasured = wa.tone_frequency(theirs['wav'], probe.body_start + PATHSIM_LATENCY_FRAMES + FS // 2, count, probe.tone_hz + shift,
                                      probe.tone_hz - shift)
        expected = probe.tone_hz + realised
        record = {'criterion': 7, 'kind': 'shift', 'shift_hz': shift, 'realised_shift_hz': realised, 'expected_hz': expected,
                  'stage': measured | {'error_hz': measured['measured_hz'] - expected, 'render': finish(runner, ours)},
                  'pathsim': pmeasured | {'error_hz': pmeasured['measured_hz'] - probe.tone_hz - shift, 'render': finish(runner, theirs)}}
        record['pass'] = abs(record['stage']['error_hz']) <= FREQUENCY_TOLERANCE_HZ and measured['mirror_dbc'] <= MIRROR_LIMIT_DBC
        rows.append(row_store(runner, row_id, record))
    row_id = 'c7-rayleigh-1Hz-shift+75Hz'
    cached = row_cached(runner, row_id)
    if cached:
        rows.append(cached)
    else:
        paths = (P(spread_hz=1.0, shift_hz=75.0),)
        ours = render_stage(runner, row_id, scenario(row_id, paths, None, reference=probe.reference()), source=probe.path)
        a = lane_analyzer(runner, 'doppler', ours['wav'], probe.path, f'{row_id}.stage', ['--label-spread', '1', '--tone-hz', '1775'])
        theirs = render_pathsim(runner, row_id, pathsim_options(paths, None), probes.shifted(probe.name))
        b = lane_analyzer(runner, 'doppler', theirs['wav'], probe.path, f'{row_id}.pathsim', ['--label-spread', '1', '--tone-hz', '1775'])
        s = doppler_summary(a, 1.0)
        record = {'criterion': 7, 'kind': 'rayleigh-shift', 'shift_hz': 75.0, 'stage': s | {'render': finish(runner, ours)},
                  'pathsim': doppler_summary(b, 1.0) | {'render': finish(runner, theirs)}}
        record['pass'] = abs(s['centre_hz'] - 1775.0) <= CENTRE_TOLERANCE_HZ and abs(s['relative_error']) <= SPREAD_TOLERANCE \
            and s['shape_class'].startswith('gaussian-like')
        rows.append(row_store(runner, row_id, record))
    row_id = 'c7-fixed-phase90'
    cached = row_cached(runner, row_id)
    if cached:
        rows.append(cached)
    else:
        short = probes.get('tone1700_120s')
        paths = (P(phase_deg=90.0),)
        ours = render_stage(runner, row_id, scenario(row_id, paths, None, gain_db=0.0, reference=short.reference()), source=short.path)
        start, count = short.analysis_window(0)
        out = wa.tone_phase(ours['wav'], start + LATENCY_FRAMES, count, short.tone_hz)
        inp = wa.tone_phase(short.path, start, count, short.tone_hz)
        # y(n) = Re{e^(j phi) xa(n - D)} = A cos(w (n - D) + theta + phi); tone_phase refers both to frame 0,
        # so out / inp = e^(j (phi - w D)), and phi = angle(out / inp) + w D.
        measured = math.degrees(np.angle(out / inp * np.exp(2j * np.pi * short.tone_hz * LATENCY_FRAMES / FS)))
        error = (measured - 90.0 + 180.0) % 360.0 - 180.0
        record = {'criterion': 7, 'kind': 'phase', 'phase_set_deg': 90.0, 'measured_deg': measured, 'error_deg': error,
                  'amplitude_ratio': float(abs(out / inp)), 'render': finish(runner, ours),
                  'pass': abs(error) <= PHASE_TOLERANCE_DEG and abs(abs(out / inp) - 1.0) <= 1e-3}
        rows.append(row_store(runner, row_id, record))
    return rows


def b10_rows(runner: Runner, probes: Probes) -> list[dict]:
    """B10, the first battery of lane amendment 1 (JITC Table 10.1 / Table XVI): two equal Rayleigh
    paths at 2 ms (0.5, 1 and 5 Hz) and at 5 ms (1 and 5 Hz), each through
      B2   delivered SNR at a label of 20 dB on gapnoise 1200 s: delivered = label + the realised tap
           power within 0.1 dB (the lane's decomposition), and within 0.5 dB of the label (E.7.3's
           rule, reported; E.7.3 itself is 1 and 10 Hz at 2 h, criterion 1);
      B3/5 the channel truth over 1800 s: each path's two sigma within 10 % and Gaussian, Rayleigh
           moments, powers as set, the two paths uncorrelated;
      B4   the delay probe 1200 s at 40 dB: taps at their delays, equal powers, latency 215;
    with pathsim beside each. "1 fixed" is B1's fixed-path rows; D.6.2's fixed 3- and 4-path channels
    at 50 dB are decoded with criterion 5 (c5_prepare)."""
    quick = runner.args.quick
    snr_probe = probes.get('gapnoise_120s' if quick else 'gapnoise_1200s')
    delay_probe = 'delay_300s' if quick else 'delay_1200s'
    truth_seconds = 300.0 if quick else B10_TRUTH_SECONDS
    clean = None
    rows = []
    for index, (delay_ms, spread) in enumerate(TABLE_XVI):
        name = f'{delay_ms:g}ms-{spread:g}Hz'
        seed = B10_FIRST_SEED + index
        paths = (P(spread_hz=spread), P(delay_ms, spread))
        row_id = f'c7-b10-snr-{name}-{snr_probe.name}'
        cached = row_cached(runner, row_id)
        if cached:
            rows.append(cached)
        else:
            clean = clean or lane_analyzer(runner, 'snr', snr_probe.path, snr_probe.path, f'c7-b10-input-{snr_probe.name}')
            rows.append(row_store(runner, row_id, b10_snr_record(runner, row_id, name, paths, snr_probe, clean, seed)))
        row_id = f'c7-b10-delay-{name}'
        cached = row_cached(runner, row_id)
        rows.append(cached if cached else row_store(runner, row_id, delay_record(runner, probes, row_id, f'b10-{name}', paths, delay_probe, 7, seed)))
        row_id = f'c7-b10-truth-{name}-{truth_seconds:g}s'
        cached = row_cached(runner, row_id)
        rows.append(cached if cached else
                    row_store(runner, row_id, truth_record(runner, row_id, paths, truth_seconds, 7, seed) | {'battery': 'B10', 'name': name}))
    return rows


def b10_snr_record(runner: Runner, row_id: str, name: str, paths: tuple[P, ...], probe: Probe, clean: dict, seed: int) -> dict:
    label = B10_SNR_LABEL_DB
    noisy = render_stage(runner, row_id, scenario(row_id, paths, label, seed=seed, reference=probe.reference()), source=probe.path)
    a = lane_analyzer(runner, 'snr', noisy['wav'], probe.path, f'{row_id}.stage')
    noisy_record = finish(runner, noisy)
    channel = render_stage(runner, f'{row_id}-channel', scenario(row_id, paths, None, seed=seed, reference=probe.reference()), source=probe.path)
    c = lane_analyzer(runner, 'snr', channel['wav'], probe.path, f'{row_id}-channel.stage')
    channel_record = finish(runner, channel)
    theirs = render_pathsim(runner, row_id, pathsim_options(paths, label + PATHSIM_COMMAND_OFFSET_DB), probe.path, seed=seed)
    b = lane_analyzer(runner, 'snr', theirs['wav'], probe.path, f'{row_id}.pathsim')
    tap_power = c['S_dbfs'] - (clean['S_dbfs'] + STAGE_GAIN_DB)
    delivered = a['snr_db']
    record = {'criterion': 7, 'battery': 'B10', 'kind': 'b10-snr', 'name': name, 'probe': probe.name, 'label_db': label, 'seed': seed,
              'paths': [dataclasses.asdict(p) for p in paths],
              'stage': {'snr_db': delivered, 'se_db': a.get('snr_standard_error_db'), 'tap_power_db': tap_power, 'delta_db': delivered - label,
                        'residual_db': delivered - label - tap_power, 'analysed_s': analysed_seconds(a), 'renders': [noisy_record, channel_record]},
              'pathsim': {'commanded_db': label + PATHSIM_COMMAND_OFFSET_DB, 'snr_db': b['snr_db'], 'se_db': b.get('snr_standard_error_db'),
                          'delta_vs_target_db': b['snr_db'] - label, 'render': finish(runner, theirs)}}
    record['within_e73_rule'] = abs(delivered - label) <= E73_TOLERANCE_DB
    record['pass'] = abs(delivered - label - tap_power) <= SNR_TOLERANCE_DB
    return record


def q1_reading(rows: list[dict]) -> dict:
    """Open question Q1 (design 5.5): "everything outside the main lobe below -60 dBc" read per spur
    (the analyzer's spurs and the k R images, each below -60 dBc) or as the total power beyond +/-3 d
    (to R / 2). Both readings, so the verdict does not hide the one that fails."""
    doppler = [r for r in rows if r.get('kind') == 'doppler']
    totals = [r['stage']['images']['out_of_lobe_beyond_3d_dbc'] for r in doppler if r['stage']['images'].get('out_of_lobe_beyond_3d_dbc') is not None]
    spurs = [r['stage']['worst_spur_dbc'] for r in doppler if r['stage']['worst_spur_dbc'] is not None]
    images = [r['stage']['images']['worst_image_dbc'] for r in doppler]
    worst_total = max(totals, default=None)
    return {'open': True, 'per_spur': {'worst_spur_dbc': max(spurs, default=None), 'worst_image_dbc': max(images, default=None),
                                       'pass': all(v <= IMAGE_LIMIT_DBC for v in spurs + images)},
            'total_power_beyond_3d': {'worst_dbc': worst_total, 'best_dbc': min(totals, default=None),
                                      'pass': worst_total is not None and worst_total <= IMAGE_LIMIT_DBC}}


def criterion2_verdict(block: dict) -> str:
    """The criterion 2 verdict with Q1 stated beside it whenever the two readings differ."""
    q1 = block['q1']
    if not block['pass']:
        return 'FAIL'
    total = q1['total_power_beyond_3d']
    if total['pass']:
        return 'PASS (both Q1 readings)'
    return (f"PASS (per-spur reading; Q1 open: the total-power reading fails, {total['best_dbc']:.1f} to "
            f"{total['worst_dbc']:.1f} dBc beyond +/-3 d against -60 dBc)")


def coverage_matrix(results: dict) -> dict:
    """Which parameters the battery reached, from the scenarios it ran, and the first battery (B10):
    each Table XVI two-path geometry and each D.6.2 fixed equal-power channel at 50 dB."""
    paths_seen, delays, spreads, shifts, kinds, phases, powers = set(), set(), set(), set(), set(), set(), set()
    table_xvi, d62 = set(), set()
    for scenario_file in (Path(results['work']) / 'scenarios').glob('*.json'):
        doc = read_json(scenario_file)
        snr = next((s['snr_db'] for s in doc['impairments'] if s['type'] == 'awgn'), None)
        for stage in doc['impairments']:
            if stage['type'] == 'watterson':
                paths_seen.add(len(stage['paths']))
                for path in stage['paths']:
                    delays.add(path.get('delay_ms', 0.0))
                    kinds.add(path.get('fading', 'rayleigh'))
                    if 'spread_hz' in path:
                        spreads.add(path['spread_hz'])
                    shifts.add(path.get('shift_hz', 0.0))
                    phases.add(path.get('phase_deg', 0.0))
                    powers.add(path.get('power_db', 0.0))
                p = stage['paths']
                equal = all(not q.get('power_db') and not q.get('shift_hz') for q in p)
                if equal and len(p) == 2 and all('spread_hz' in q for q in p) and p[0].get('delay_ms', 0.0) == 0.0 \
                        and p[0]['spread_hz'] == p[1]['spread_hz']:
                    table_xvi.add((p[1].get('delay_ms', 0.0), p[0]['spread_hz']))
                if equal and len(p) >= 3 and all(q.get('fading') == 'fixed' and not q.get('phase_deg') for q in p) and snr == D62_SNR_DB:
                    d62.add(tuple(q.get('delay_ms', 0.0) for q in p))
    table_xvi_missing = [g for g in TABLE_XVI if g not in table_xvi]
    d62_wanted = [delays_ms for _, delays_ms in D62_CHANNELS if len(delays_ms) >= 3]
    d62_missing = [g for g in d62_wanted if g not in d62]
    lane_pass = (max(paths_seen, default=0) >= 4 and max(delays, default=0) >= 9.0 and min(spreads, default=1) <= 0.1
                 and max(spreads, default=0) >= 10.0 and max(shifts, default=0) >= 75.0 and min(shifts, default=0) <= -75.0)
    return {'path_counts': sorted(paths_seen), 'delays_ms': sorted(delays), 'spreads_hz': sorted(spreads), 'shifts_hz': sorted(shifts),
            'fading': sorted(kinds), 'phases_deg': sorted(phases), 'powers_db': sorted(powers),
            'lane_minimums': {'paths': 4, 'delay_ms': 9.0, 'spread_hz': [0.1, 10.0], 'shift_hz': 75.0},
            'first_battery': {'table_xvi_rendered': sorted(table_xvi), 'table_xvi_missing': table_xvi_missing,
                              'd62_50db_rendered': sorted(d62), 'd62_50db_missing': d62_missing},
            'pass': lane_pass and not table_xvi_missing and not d62_missing}


def coverage_not_judged(selected: list[str], quick: bool) -> str | None:
    """Why this run does not judge criterion 7's coverage matrix, or None where it does. The matrix
    reads every scenario in <work>/scenarios, the parameters of all the criteria together (criterion
    2's 4-path and 20 ms rows, criterion 5's D.6.2 channels, B10), so only the full battery can meet
    it: every criterion, without --quick (whose criterion 5 renders no D.6.2 channel). On any other
    run it would fail by construction, or pass on scenarios an earlier run left in the folder."""
    reasons = []
    absent = [c for c in FULL_BATTERY if c not in selected]
    if absent:
        reasons.append(f"criteria {', '.join(absent)} not run")
    if quick:
        reasons.append('--quick')
    return '; '.join(reasons) or None


def criterion7_close(block: dict, work: Path, selected: list[str], quick: bool) -> dict:
    """Criterion 7 after the other criteria (criterion 5 renders D.6.2's channels): the coverage matrix
    of <work>/scenarios, judged where coverage_not_judged allows, and the verdict of its rows and B10's
    with it. Where the matrix is not judged it is reported with the reason, and the verdict says so."""
    matrix = coverage_matrix({'work': str(work)})
    reason = coverage_not_judged(selected, quick)
    rows_pass = all(r['pass'] for r in block['rows']) and all(r['pass'] for r in block['b10'])
    block['matrix'] = matrix | {'judged': reason is None}
    if reason is None:
        block['pass'] = matrix['pass'] and rows_pass
        return block
    block['matrix']['not_judged'] = f'{reason}: the matrix is judged on the full battery only (every criterion, without --quick)'
    block['pass'] = rows_pass
    block['verdict'] = f'{verdict(rows_pass)} (coverage not judged: {reason})'
    return block


# ================================================================================================ criterion 5

def lane_card_source(lane: Path, card: str, geometry: str, target: float, seed: int, ours_sha256: str) -> dict | None:
    """The lane's P20/P21 phase D card of this rung and seed, where one exists (seeds 1 and 2): its pathsim
    source WAV (before the card's level cut) was made by run_pathsim.py with this recipe, so its sha256
    must equal this render's (gate 2's equality of pathsim_chain and the lane chain, again)."""
    name = f"D{card[1:3]}{'M' if geometry == 'MPP' else 'A'}{int(target):02d}S{seed}"
    gen = lane / 'hfchannel/methodology/stimuli-usb' / f'{name}.gen.json'
    if not gen.is_file():
        return None
    recipe = read_json(gen)
    return {'card': name, 'gen_json': str(gen), 'source_sha256': recipe.get('source_sha256'),
            'equal': recipe.get('source_sha256') == ours_sha256}


def ps_quote(text: str) -> str:
    """A PowerShell single-quoted literal."""
    return "'" + text.replace("'", "''") + "'"


def trimmed_rms(path: Path) -> float:
    """The RMS between the first and last non-zero sample: the driver's "auto" reference (E.5.5)."""
    frames = wa.frame_count(path)
    energy, first, last, n = 0.0, None, None, 0
    for s in range(0, frames, 1 << 22):
        x = wa.read_frames(path, s, min(1 << 22, frames - s))
        nz = np.flatnonzero(x)
        if len(nz):
            first = s + int(nz[0]) if first is None else first
            last = s + int(nz[-1])
        energy += float(np.sum(x * x))
        n += len(x)
    return math.sqrt(energy / (last - first + 1))


def c5_extended_rungs(runner: Runner) -> set[str]:
    """The rungs --c5-extend names: explicit rung names, or "auto" for every rung of the existing
    manifest whose decodes show a bit error (design 10.4's threshold rungs)."""
    names = {n.strip() for n in (runner.args.c5_extend or '').split(',') if n.strip()}
    if 'auto' not in names:
        return names
    names.discard('auto')
    manifest = runner.work / 'c5' / 'manifest.json'
    if manifest.is_file():
        for record in read_json(manifest)['renders']:
            if record.get('kind') == 'd62':
                continue
            trials = decode_trials(record)
            if trials is not None and trials['errors'] > 0:
                names.add(record['rung'])
    return names


def decode_trials(record: dict) -> dict | None:
    """A render's decode, from decode_existing.py's trials.jsonl (its last trial): bit errors, with
    missing bytes counted as half their bits in error (coin flips), and the bits compared."""
    trials = Path(record['decode']['out_dir']) / 'trials.jsonl'
    if not trials.is_file():
        return None
    last = [json.loads(line) for line in trials.read_text(encoding='utf-8').splitlines() if line.strip()][-1]
    bits = 8 * record['payload']['bytes']
    errors = last['bit_errors'] + 4 * last.get('missing_bytes', 0)
    return {'errors': errors, 'bits': bits, 'ber': errors / bits, 'exact': bool(last.get('exact')), 'missing_bytes': last.get('missing_bytes', 0)}


def full_scale_samples(path: Path, x: np.ndarray) -> int:
    """Samples of a PCM render at its largest code (+/-(2^(bits-1) - 1), or the lowest): where a
    saturating writer clamped (pathsim's AudioFile writer, the stage under clip_policy saturate)."""
    bits = wa.wav_layout(path)[2]
    if bits not in (16, 24):
        return 0
    full = ((1 << (bits - 1)) - 1) / (1 << (bits - 1))
    return int(np.count_nonzero(np.abs(x) >= full))


def c5_render(runner: Runner, probes: Probes, *, label: str, rung: str, kind: str, mode: str, card: str, geometry: str, target: float, seed: int,
              simulator: str, paths: tuple[P, ...], pathsim_args: list[str], card_level: dict, stim: Path, folder: str = 'c5') -> dict:
    """One decoder-equivalence render (stage or pathsim), its companion SNR and its decode command,
    under <work>/<folder>: for c5 the scenarios and analyses go to the work directory's own folders
    (as before), for any other folder into that folder. Where the stage's clip_policy reject refuses
    a render (a sample beyond full scale), it is made again with saturate, as pathsim's writer clamps,
    and the recipe says so; every record counts its output's full-scale samples. The record carries the
    sha256 of the binary that made the render (hashed before and after it; a change between stops)."""
    lane = runner.args.lane_root
    binary = Path(runner.args.driver if simulator == 'stage' else runner.args.chain)
    binary_sha256 = sha256(binary)
    decoder = lane / 'snapshots/wave3-final/tools/m110_app_decode.exe'
    decode_script = lane / 'scripts/decode_existing.py'
    companion = probes.get('gapnoise_305s')
    clean = stim / 'src' / f'{card}-clean-48k.wav'
    payload = stim / f'{card}.BIN'
    timeline = read_json(stim / f'{card}.timeline.json')
    gain = PATHSIM_PIN_DBFS - 20 * math.log10(card_level['trimmed_rms'])
    sub = () if folder == 'c5' else (folder,)
    scenario_dir, analysis_dir = (*sub, 'scenarios'), (*sub, 'analysis')
    if simulator == 'stage':
        doc = scenario(label, paths, target, seed=seed, gain_db=gain, reference='auto')
        fallback = None
        try:
            render = render_stage(runner, label, doc, source=clean, folder=folder, scenario_dir=scenario_dir)
        except RuntimeError as error:
            if 'clip_policy reject' not in str(error):
                raise
            doc = doc | {'clip_policy': 'saturate'}
            render = render_stage(runner, label, doc, source=clean, folder=folder, scenario_dir=scenario_dir)
            fallback = {'refused': 'clip_policy reject: render rejected (clipping)', 'clip_policy': 'saturate',
                        'clipped_samples': render['sidecar'].get('clipped_samples')}
        comp = render_stage(runner, f'{label}-companion', scenario(label, paths, target, seed=seed, reference=companion.reference()),
                            source=companion.path, folder=folder, scenario_dir=scenario_dir)
        channel = render['sidecar']['watterson']
        recipe = {'scenario': doc, 'scenario_path': str(runner.work.joinpath(*scenario_dir, f'{label}.json')),
                  'scenario_sha256': render['scenario_sha256'], 'output_digest': render['digest'],
                  'parameter_digest': channel.get('parameter_digest') if isinstance(channel, dict) else None}
        if fallback is not None:
            recipe['clip_fallback'] = fallback
    else:
        options = pathsim_args + ['--snr', f'{target + PATHSIM_COMMAND_OFFSET_DB:.2f}']
        render = render_pathsim(runner, label, options, clean, seed=seed, folder=folder)
        comp = render_pathsim(runner, f'{label}-companion', options, companion.path, seed=seed, folder=folder)
        recipe = {'pathsim_chain_argv': render['argv'], 'commanded_snr_db': target + PATHSIM_COMMAND_OFFSET_DB,
                  'recipe': 'run_pathsim.py chain (48k -> 8k at -12 dB -> pathsim 7746121 -> 48k), as pathsim_chain'}
        if kind == 'ladder':
            recipe['lane_card_source'] = lane_card_source(lane, card, geometry, target, seed, sha256(render['wav']))
    if sha256(binary) != binary_sha256:
        raise RuntimeError(f'{binary} changed during the render of {label}; the render was not stored')
    a = lane_analyzer(runner, 'snr', comp['wav'], companion.path, f'{label}-companion', analysis_dir=analysis_dir)
    wav = render['wav']
    runner.gate()
    x = wa.read_frames(wav, 0, wa.frame_count(wav))
    decode_dir = runner.work / folder / 'decode' / label
    argv = [sys.executable, '-B', str(decode_script), '--wav', str(wav), '--payload', str(payload), '--mode', mode,
            '--out', str(decode_dir), '--decoder', str(decoder), '--seed', str(seed), '--peak-memory', '--',
            '--engine', 'siso', '--maximum-payload-octets', str(int(timeline['payload_bytes']) + 4096),
            '--loss-of-lock-frames', '1000000']
    record = {'criterion': 5, 'kind': kind, 'label': label, 'rung': rung, 'mode': mode, 'card': card, 'geometry': geometry,
              'target_db': target, 'seed': seed, 'simulator': simulator,
              'input': {'path': str(clean), 'sha256': card_level['sha256'], 'trimmed_rms': card_level['trimmed_rms']},
              'payload': {'path': str(payload), 'sha256': sha256(payload), 'bytes': int(timeline['payload_bytes'])},
              'recipe': recipe,
              'output': {'path': str(wav), 'sha256': sha256(wav), 'frames': int(len(x)),
                         'rms_dbfs': 20 * math.log10(float(np.sqrt(np.mean(x * x)))),
                         'peak_dbfs': 20 * math.log10(float(np.max(np.abs(x)))), 'full_scale_samples': full_scale_samples(wav, x)},
              'companion': {'probe': companion.name, 'snr_db': a['snr_db'], 'se_db': a.get('snr_standard_error_db'),
                            'sha256': sha256(comp['wav'])},
              'decode': {'argv': argv, 'out_dir': str(decode_dir)}, 'binary_sha256': binary_sha256, 'pass': None}
    comp['wav'].unlink()
    return record


def c5_rung_name(card: str, geometry: str, target: float) -> str:
    return f'{card}-{geometry}-{target:g}dB'


def c5_ladder_jobs(rungs, seeds_of) -> list[dict]:
    """The render jobs of ladder rungs (mode, card, geometry, target): CCIR Poor (two equal 1 Hz
    Rayleigh paths, 0 and 2 ms) or AWGN only, each seed of seeds_of(rung) through both simulators."""
    jobs = []
    for mode, card, geometry, target in rungs:
        rung = c5_rung_name(card, geometry, target)
        paths = (P(spread_hz=1.0), P(2.0, 1.0)) if geometry == 'MPP' else ()
        pathsim_args = ['--spread', '1', '--delay2', '2', '--spread2', '1'] if geometry == 'MPP' else ['--direct']
        for seed in seeds_of(rung):
            for simulator in ('stage', 'pathsim'):
                jobs.append(dict(label=f'c5-{rung}-s{seed}-{simulator}', rung=rung, kind='ladder', mode=mode, card=card, geometry=geometry,
                                 target=target, seed=seed, simulator=simulator, paths=paths, pathsim_args=pathsim_args))
    return jobs


def parse_threshold_set(text: str) -> list[tuple[str, str, str, float]]:
    """--c5-threshold: '<card>-<MPP|AWGN>:<dB>,<dB>,...' per ladder, ladders separated by ';', into
    rungs (mode, card, geometry, target dB), each once, in the order given."""
    rungs: list[tuple[str, str, str, float]] = []
    for part in (p.strip() for p in text.split(';')):
        if not part:
            continue
        name, _, values = part.partition(':')
        card, _, geometry = name.strip().partition('-')
        try:
            targets = [float(v) for v in values.split(',') if v.strip()]
        except ValueError:
            targets = []
        if card not in C5_CARD_MODES or geometry not in ('MPP', 'AWGN') or not targets:
            raise ValueError(f'--c5-threshold: "{part}" is not <card>-<MPP|AWGN>:<dB>,<dB>,... (cards {", ".join(C5_CARD_MODES)})')
        for target in targets:
            rung = (C5_CARD_MODES[card], card, geometry, target)
            if rung not in rungs:
                rungs.append(rung)
    if not rungs:
        raise ValueError('--c5-threshold: no rung given')
    return rungs


def c5_reusable(runner: Runner, job: dict, record: dict | None, card_level: dict, stim: Path) -> dict | None:
    """The c5 render of this job, where its inputs are byte-identical: the same card (sha256) and, for
    the stage, the same scenario document (seed, gain, paths, SNR, reference, test id), for pathsim the
    same pathsim_chain argv up to the output path; and its output still on disk with the sha256 the c5
    manifest records (hashed again here). The binary that made it is checked by c5_reuse
    (c5_binary_evidence)."""
    if record is None or record.get('kind') != 'ladder' or record['input']['sha256'] != card_level['sha256']:
        return None
    clean = stim / 'src' / f"{job['card']}-clean-48k.wav"
    if job['simulator'] == 'stage':
        gain = PATHSIM_PIN_DBFS - 20 * math.log10(card_level['trimmed_rms'])
        doc = scenario(job['label'], job['paths'], job['target'], seed=job['seed'], gain_db=gain, reference='auto')
        if record['recipe'].get('scenario') != doc:
            return None
    else:
        options = job['pathsim_args'] + ['--snr', f"{job['target'] + PATHSIM_COMMAND_OFFSET_DB:.2f}"]
        argv = record['recipe'].get('pathsim_chain_argv') or []
        if argv[:1] != [str(runner.args.chain)] or argv[1:-1] != [*options, '--seed', str(job['seed']), str(clean)]:
            return None
    wav = Path(record['output']['path'])
    if not wav.is_file():
        return None
    runner.gate()
    if sha256(wav) != record['output']['sha256']:
        return None
    return copy.deepcopy(record)


def c5_binary_evidence(record: dict, binary: dict) -> str | None:
    """Why the c5 render `record` was made by `binary` ({path, sha256}, hashed now), or None where that
    is not shown. A record that carries the binary's sha256 (c5_render) must equal it. A c5 record
    made before renders carried it (3b4aeb1 and earlier) qualifies when the binary file is older than
    the record (its mtime before the record's measured_by time): the file hashed now is the file
    that ran. results.json is no evidence: a --resume pass rewrites it with the binaries of that
    pass, which renders nothing."""
    if record.get('binary_sha256') is not None:
        return 'binary sha256 recorded with the render and equal' if record['binary_sha256'] == binary['sha256'] else None
    stored = (record.get('measured_by') or {}).get('utc')
    if stored is None:
        return None
    mtime = datetime.datetime.fromtimestamp(Path(binary['path']).stat().st_mtime, datetime.timezone.utc)
    if mtime >= datetime.datetime.fromisoformat(stored):
        return None
    return (f"no binary sha256 in the record; the binary (sha256 {binary['sha256'][:16]}, mtime "
            f"{mtime.replace(microsecond=0).isoformat()}) is older than the record ({stored})")


def binary_unchanged(binary: dict, name: str) -> None:
    """Stops the threshold set where a tool's binary changed since the run began (another build in the
    checkout): no ladder mixes two builds of a simulator."""
    if sha256(Path(binary['path'])) != binary['sha256']:
        raise RuntimeError(f"c5t: {name} changed during the run ({binary['path']}); the render in progress was not stored")


def c5_reuse(runner: Runner, job: dict, candidate: dict | None, binary: dict, card_level: dict, stim: Path, source: Path) -> dict | None:
    """The reuse decision of one threshold job: the row that references `candidate` (the record of the
    job's label in the c5 manifest `source`, or None) instead of rendering the job again, or None where
    the job is rendered. The candidate qualifies when `binary` ({path, sha256} of the job's simulator,
    hashed when the run began) made it (c5_binary_evidence) and its inputs and output are byte-identical
    (c5_reusable). The row keeps what was checked and the binary it was checked against."""
    if candidate is None:
        return None
    evidence = c5_binary_evidence(candidate, binary)
    record = c5_reusable(runner, job, candidate, card_level, stim) if evidence is not None else None
    if record is None:
        return None
    record['reused_from'] = {'manifest': str(source), 'label': job['label'], 'output_sha256': record['output']['sha256'],
                             'binary_sha256': binary['sha256'],
                             'checked': f'card sha256 and scenario (stage) or pathsim_chain argv (pathsim) equal; {evidence}; '
                                        'output sha256 hashed again and equal',
                             'reused_by': dict(runner.identity) | {'utc': utc_now()}}
    return record


def c5_made_by(record: dict, binary: dict) -> str | None:
    """The sha256 of the binary that made a threshold row, `binary` being today's of the row's
    simulator: a rendered row's own; for a referenced c5 render, the one checked at reuse, or (a row
    referenced by 3b4aeb1's runner, which kept none) today's where c5_binary_evidence holds; None
    where it is not known."""
    reuse = record.get('reused_from')
    if not reuse:
        return record.get('binary_sha256')
    if reuse.get('binary_sha256') is not None:
        return reuse['binary_sha256']
    return binary['sha256'] if c5_binary_evidence(record, binary) is not None else None


def c5_other_builds(renders: list[dict], binaries: dict[str, dict]) -> list[str]:
    """The mixed-binary guard of the threshold set: the sha256 prefixes ('None' where unknown) of the
    binaries other than today's (`binaries`, by C5_TOOLS name) that made rows of `renders`, sorted;
    empty where today's build of each simulator made every row."""
    others = set()
    for record in renders:
        binary = binaries[C5_TOOLS[record['simulator']]]
        made = c5_made_by(record, binary)
        if made != binary['sha256']:
            others.add(str(made)[:16])
    return sorted(others)


def c5_threshold_prepare(runner: Runner, probes: Probes) -> dict:
    """Criterion 5's threshold set (--c5-threshold; lane call 3 and Q11): the given rungs at seeds 1 to
    12 through both simulators, with the c5 rungs' card, channel and level rules, into <work>/c5t:
    renders, scenarios, companion analyses, rows, the manifest (the c5 schema) and decode_c5.ps1.
    A c5 render with byte-identical inputs (c5_reusable), made by today's binary (c5_binary_evidence),
    is referenced by path and sha256 with its decode, not rendered again (c5_reuse); a run whose rows
    another build made stops (c5_other_builds)."""
    folder = 'c5t'
    lane = runner.args.lane_root
    stim = lane / 'hfchannel/conformance/stimuli'
    rungs = parse_threshold_set(runner.args.c5_threshold)
    rows_dir = (folder, 'rows')
    source_path = (runner.args.c5_reuse or runner.work / 'c5' / 'manifest.json').resolve()
    source = {r['label']: r for r in read_json(source_path)['renders']} if source_path.is_file() else {}
    binaries = {name: {'path': str(path), 'sha256': sha256(path)} for name, path in (('driver', runner.args.driver), ('pathsim_chain', runner.args.chain))}
    card_levels: dict[str, dict] = {}

    def level(card: str) -> dict:
        if card not in card_levels:
            clean = stim / 'src' / f'{card}-clean-48k.wav'
            runner.gate()
            card_levels[card] = {'trimmed_rms': trimmed_rms(clean), 'sha256': sha256(clean)}
        return card_levels[card]

    renders, reused, rendered = [], [], []
    for job in c5_ladder_jobs(rungs, lambda rung: C5_EXTENDED_SEEDS):
        tool = C5_TOOLS[job['simulator']]
        cached = row_cached(runner, job['label'], rows_dir)
        if cached:
            renders.append(cached)
            (reused if cached.get('reused_from') else rendered).append(job['label'])
            continue
        record = c5_reuse(runner, job, source.get(job['label']), binaries[tool], level(job['card']), stim, source_path)
        if record is not None:
            write_json(runner.inside(*rows_dir, f"{job['label']}.json"), record)
            print(f"row {job['label']}: reused from c5", flush=True)
            renders.append(record)
            reused.append(job['label'])
            continue
        binary_unchanged(binaries[tool], tool)
        record = c5_render(runner, probes, **job, card_level=level(job['card']), stim=stim, folder=folder)
        binary_unchanged(binaries[tool], tool)
        renders.append(row_store(runner, job['label'], record, rows_dir))
        rendered.append(job['label'])
    mixed = c5_other_builds(renders, binaries)
    if mixed:
        raise RuntimeError(f'c5t: rows made by other or unknown binaries ({", ".join(mixed)}): render them again without --resume')
    ladders = {}
    for mode, card, geometry, target in rungs:
        ladders.setdefault((mode, card, geometry), []).append(target)
    manifest = {'schema': 'watterson-validation.c5-inputs/2', 'created_utc': utc_now(),
                'rule': 'lane call 3 and Q11 (kg7hf/waveform-generator#21, 2026-10-09); docs/design/watterson-validation.md 9.1',
                'set': 'threshold', 'threshold_set': runner.args.c5_threshold,
                'rungs': [{'mode': m, 'card': c, 'geometry': g, 'target_db': t} for m, c, g, t in rungs], 'seeds': list(C5_EXTENDED_SEEDS),
                'extended_rungs': [], 'extended_seeds': list(C5_EXTENDED_SEEDS),
                'ladders': [{'mode': m, 'card': c, 'geometry': g, 'targets_db': sorted(t),
                             'offset_bound_db': OFFSET_BOUND_DB['AWGN' if g == 'AWGN' else 'fading']} for (m, c, g), t in ladders.items()],
                'offset_rule': {'target_ber': OFFSET_BER, 'bound_db': OFFSET_BOUND_DB, 'min_points': OFFSET_MIN_POINTS,
                                'step_db': OFFSET_STEP_DB, 'min_seeds': OFFSET_MIN_SEEDS},
                'd62': None,
                'level': {'stage': f'source gain per card so the scaled reference is {PATHSIM_PIN_DBFS:.2f} dBFS (pathsim pin); reference "auto"',
                          'pathsim': 'pinned at 0.122 RMS (calib A5)', 'knee_gain': 'not needed: both deliver the signal at -18.27 dBFS',
                          'clip': 'clip_policy reject as in c5; where reject refuses a stage render (a sample beyond full scale) it is '
                                  'made with saturate, as pathsim\'s writer clamps (recipe.clip_fallback); output.full_scale_samples '
                                  'counts the clamped samples of either simulator'},
                'cards': card_levels, 'binaries': binaries,
                'reused': {'from': str(source_path), 'renders': len(reused), 'labels': reused},
                'renders': renders,
                'decode_command_template': ('python -B <lane>/scripts/decode_existing.py --wav <render.wav> --payload <card>.BIN --mode <mode> '
                                            '--out <decode>/<label> --decoder <lane>/snapshots/wave3-final/tools/m110_app_decode.exe '
                                            '--seed <seed> --peak-memory -- --engine siso --maximum-payload-octets <payload_bytes + 4096> '
                                            '--loss-of-lock-frames 1000000'),
                'decode_dirs': {'rendered here': str(runner.work / folder / 'decode'), 'reused': str(runner.work / 'c5' / 'decode')},
                'score_command': f'python -B {Path(__file__).resolve()} --score-c5 {runner.work / folder / "manifest.json"}'}
    write_json(runner.inside(folder, 'manifest.json'), manifest)
    write_decode_script(runner, renders, folder)
    decoded = sum(1 for r in renders if decode_trials(r) is not None)
    return {'manifest': str(runner.work / folder / 'manifest.json'), 'decode_script': str(runner.work / folder / 'decode_c5.ps1'),
            'renders': len(renders), 'reused': len(reused), 'rendered': len(rendered), 'decoded': decoded,
            'full_scale': {r['label']: r['output'].get('full_scale_samples') for r in renders if r['output'].get('full_scale_samples')},
            'clip_fallback': [r['label'] for r in renders if r['recipe'].get('clip_fallback')]}


def c5_prepare(runner: Runner, probes: Probes) -> dict:
    """Criterion 5 (design 10.4): render the 8 rungs x 4 seeds through the stage and through pathsim
    (the lane's recipe), 12 seeds on the rungs --c5-extend names, and B10's D.6.2 fixed channels at
    50 dB (one seed: a static channel; the 4-path one has no pathsim twin); measure each render's
    delivered SNR on a companion gapnoise probe through the identical channel and seed; write the
    manifest and the gated decode script; score the decodes when every render has one."""
    lane = runner.args.lane_root
    stim = lane / 'hfchannel/conformance/stimuli'
    renders = []
    card_levels = {}
    extended = c5_extended_rungs(runner)
    rungs, seeds = (C5_RUNGS[:1], C5_SEEDS[:1]) if runner.args.quick else (C5_RUNGS, C5_SEEDS)

    def level(card: str) -> dict:
        if card not in card_levels:
            clean = stim / 'src' / f'{card}-clean-48k.wav'
            card_levels[card] = {'trimmed_rms': trimmed_rms(clean), 'sha256': sha256(clean)}
        return card_levels[card]

    jobs = c5_ladder_jobs(rungs, lambda rung: C5_EXTENDED_SEEDS if rung in extended else seeds)
    if not runner.args.quick:
        mode, card = D62_CARD
        for name, delays_ms in D62_CHANNELS:
            paths = tuple(P(delay_ms=d) for d in delays_ms)
            rung = f'D62-{card}-{name}-{D62_SNR_DB:g}dB'
            options = pathsim_options(paths, None)
            for simulator in ('stage', 'pathsim') if options is not None else ('stage',):
                jobs.append(dict(label=f'c5-{rung}-s1-{simulator}', rung=rung, kind='d62', mode=mode, card=card, geometry=name,
                                 target=D62_SNR_DB, seed=1, simulator=simulator, paths=paths, pathsim_args=options or []))
    for job in jobs:
        cached = row_cached(runner, job['label'])
        if cached:
            renders.append(cached)
            continue
        record = c5_render(runner, probes, **job, card_level=level(job['card']), stim=stim)
        renders.append(row_store(runner, job['label'], record))
    manifest = {'schema': 'watterson-validation.c5-inputs/2', 'created_utc': utc_now(), 'rule': 'docs/design/watterson-stage.md 10.4',
                'rungs': [{'mode': m, 'card': c, 'geometry': g, 'target_db': t} for m, c, g, t in rungs], 'seeds': list(seeds),
                'extended_rungs': sorted(extended), 'extended_seeds': list(C5_EXTENDED_SEEDS),
                'd62': {'card': D62_CARD[1], 'mode': D62_CARD[0], 'snr_db': D62_SNR_DB, 'ber_limit': D62_BER_LIMIT,
                        'channels': [{'name': n, 'delays_ms': list(d)} for n, d in D62_CHANNELS]},
                'level': {'stage': f'source gain per card so the scaled reference is {PATHSIM_PIN_DBFS:.2f} dBFS (pathsim pin); reference "auto"',
                          'pathsim': 'pinned at 0.122 RMS (calib A5)', 'knee_gain': 'not needed: both deliver the signal at -18.27 dBFS'},
                'cards': card_levels, 'renders': renders,
                'decode_command_template': ('python -B <lane>/scripts/decode_existing.py --wav <render.wav> --payload <card>.BIN --mode <mode> '
                                            '--out <decode>/<label> --decoder <lane>/snapshots/wave3-final/tools/m110_app_decode.exe '
                                            '--seed <seed> --peak-memory -- --engine siso --maximum-payload-octets <payload_bytes + 4096> '
                                            '--loss-of-lock-frames 1000000'),
                'score_command': f'python -B {Path(__file__).resolve()} --score-c5 {runner.work / "c5" / "manifest.json"}'}
    write_json(runner.inside('c5', 'manifest.json'), manifest)
    write_decode_script(runner, renders)
    stage = [r for r in renders if r['simulator'] == 'stage']
    pathsim = [r for r in renders if r['simulator'] == 'pathsim']
    decoded = all(decode_trials(r) is not None for r in renders)
    score = score_manifest(manifest) if decoded else None
    if score is not None:
        write_json(runner.inside('c5', 'score.json'), score)
    threshold = c5_threshold_score(runner)
    status = c5_status(score, threshold)
    if not decoded:
        title = 'decoder equivalence: inputs prepared, decodes pending'
    elif threshold is not None and threshold['score']['status'] == 'missing':
        title = 'decoder equivalence: the threshold set\'s decodes pending'
    else:
        title = 'decoder equivalence'
    return {'criterion': 5, 'title': title,
            'renders': len(renders), 'stage_renders': len(stage), 'pathsim_renders': len(pathsim), 'manifest': str(runner.work / 'c5' / 'manifest.json'),
            'decode_script': str(runner.work / 'c5' / 'decode_c5.ps1'), 'rows': renders, 'decoded': decoded, 'score': score,
            'threshold': threshold, 'status': status, 'verdict': C5_VERDICTS[status],
            'pass': None if status in ('missing', 'inconclusive') else status == 'pass'}


def c5_threshold_score(runner: Runner) -> dict | None:
    """The threshold set (<work>/c5t, --c5-threshold) where its manifest exists: its renders, how many
    are decoded and its score, written to c5t/score.json once every render is decoded (as c5's)."""
    path = runner.work / 'c5t' / 'manifest.json'
    if not path.is_file():
        return None
    manifest = read_json(path)
    score = score_manifest(manifest)
    if not score['missing']:
        write_json(runner.inside('c5t', 'score.json'), score)
    return {'manifest': str(path), 'decode_script': str(path.with_name('decode_c5.ps1')), 'renders': len(manifest['renders']),
            'decoded': len(manifest['renders']) - len(score['missing']), 'score': score}


def c5_status(score: dict | None, threshold: dict | None) -> str:
    """Criterion 5's status from c5's score (None until every c5 render is decoded) and the threshold
    set's (None where there is none). Without a threshold set, c5's status. With one, the threshold
    set judges the offset, which c5's ladders (rungs 3 or 4 dB apart) cannot: c5's n/a ladders are
    left out, and c5 still fails on a rejected rung, a D.6.2 disagreement or a ladder offset beyond
    its bound."""
    if score is None:
        return 'missing'
    if threshold is None:
        return score['status']
    theirs = threshold['score']['status']
    if theirs == 'missing':
        return 'missing'
    if theirs == 'fail' or 'fail' in [item['status'] for item in score['rungs'] + score['ladders'] + score['d62']]:
        return 'fail'
    return theirs


def write_decode_script(runner: Runner, renders: list[dict], folder: str = 'c5') -> None:
    """<folder>/decode_c5.ps1: one decode_existing.py per render not yet decoded, sequential, each after
    the shared PC's A/B gate (asked again on HOLD) and at below-normal priority; then the scorer."""
    gate = runner.args.gate_script
    script = ['# Criterion 5 decodes (design 10.4), for a quiet PC. Generated by run_watterson_validation.py.',
              '# Each render not yet decoded (no trials.jsonl) is decoded after build/main/bringup/ab_gate.ps1 says OPEN',
              '# (HOLD: asked again), one at a time, at below-normal priority. Then the scorer.',
              '$ErrorActionPreference = "Stop"',
              f'$gate = {ps_quote(str(gate))}',
              'function Wait-Gate {',
              '    do { pwsh -NoProfile -File $gate | Out-Null; $code = $LASTEXITCODE } while ($code -eq 3)',
              '    if ($code -ne 0) { throw "ab_gate.ps1 failed ($code)" }',
              '}']
    for record in renders:
        argv = record['decode']['argv']
        arguments = ','.join(ps_quote(a) for a in argv[1:])
        trials = ps_quote(str(Path(record['decode']['out_dir']) / 'trials.jsonl'))
        script.append(f"if (-not (Test-Path {trials})) {{ Wait-Gate; $p = Start-Process -FilePath {ps_quote(argv[0])} -ArgumentList @({arguments}) "
                      "-NoNewWindow -PassThru; $p.PriorityClass = 'BelowNormal'; $p.WaitForExit() }")
    script.append(f"python -B {ps_quote(str(Path(__file__).resolve()))} --score-c5 {ps_quote(str(runner.work / folder / 'manifest.json'))}")
    runner.inside(folder, 'decode_c5.ps1').write_bytes(('\n'.join(script) + '\n').encode('utf-8'))


def score_manifest(manifest: dict) -> dict:
    """Criterion 5 on the decodes: the lane's call 3 and its Q11 answer (kg7hf/waveform-generator#21,
    2026-10-09), with the lane's literal reading beside it.
      - per rung and simulator, the pooled verdict: exact (Poisson 95 % bound 3 / bits) or the pooled
        BER, missing bytes counted as half their bits in error;
      - per ladder (mode, card, geometry), the equivalent-SNR offset at BER 1e-3 (wv_analysis.snr_offset):
        log10(pooled BER) interpolated linearly in SNR between the two points that bracket 1e-3, for
        each simulator; offset = SNR_stage - SNR_pathsim; pass at |offset| <= 0.5 dB (AWGN) or 1.0 dB
        (fading). n/a, neither pass nor fail, where the crossing lies outside a simulator's points
        (naming the rung to add) or the ladder lacks 3 points 2 dB apart with 4 seeds each;
      - per rung, Q11's exact two-sided rank-sum of the per-seed BERs at 0.05 / 8: a verdict only with
        12 seeds per simulator (equal counts), informational below that, and rungs where both
        simulators are error-free on every seed excluded;
      - the lane's literal rule, reported: verdicts agree (both exact or both not), and how many of the
        stage's per-seed BERs lie inside pathsim's per-seed min-max (and whether the stage's pooled BER does).
    D.6.2 rows: each render's BER against 1e-5, and whether the two simulators' verdicts agree.
    The status: missing (a decode), fail (a ladder's offset beyond its bound, a rung rejected, a D.6.2
    disagreement), inconclusive (a ladder's offset n/a), else pass."""
    per: dict[str, dict[str, list]] = {}
    meta: dict[str, tuple] = {}
    d62: dict[str, dict[str, dict]] = {}
    missing, missing_rungs = [], set()
    for record in manifest['renders']:
        trials = decode_trials(record)
        if record.get('kind') != 'd62':
            meta.setdefault(record['rung'], (record['mode'], record['card'], record['geometry'], float(record['target_db'])))
            per.setdefault(record['rung'], {}).setdefault(record['simulator'], [])
        if trials is None:
            missing.append(record['label'])
            missing_rungs.add(record['rung'])
            continue
        if record.get('kind') == 'd62':
            d62.setdefault(record['rung'], {})[record['simulator']] = trials | {'seed': record['seed']}
            continue
        per[record['rung']][record['simulator']].append(trials | {'seed': record['seed']})
    rungs = []
    for rung, sims in per.items():
        stage, pathsim = sims.get('stage', []), sims.get('pathsim', [])
        a, b = [r['ber'] for r in stage], [r['ber'] for r in pathsim]
        verdicts = {}
        for sim, rows in (('stage', stage), ('pathsim', pathsim)):
            errors, bits = sum(r['errors'] for r in rows), sum(r['bits'] for r in rows)
            verdicts[sim] = {'errors': errors, 'bits': bits, 'seeds': len(rows), 'pooled_ber': errors / bits if bits else None,
                             'exact': errors == 0,
                             'text': '-' if not bits else ('exact (u95 %.1e)' % (3.0 / bits)) if errors == 0 else ('BER %.2e' % (errors / bits))}
        seeds = min(len(stage), len(pathsim))
        p = None
        if rung in missing_rungs:
            status, reason = 'missing', 'a decode is missing'
        elif not (any(a) or any(b)):
            status, reason = 'excluded', 'both simulators error-free on every seed (Q11: excluded)'
        elif len(a) != len(b):
            status, reason = 'informational', f'unequal seed counts ({len(a)} and {len(b)}): no rank-sum verdict'
        else:
            p = wa.rank_sum_exact(a, b)
            if seeds < C5_VERDICT_SEEDS:
                status = 'informational'
                reason = f'rank-sum p = {p:.4g} at {seeds} seeds per simulator: informational (a verdict needs {C5_VERDICT_SEEDS})'
            else:
                status = 'pass' if p >= C5_ALPHA else 'fail'
                reason = f'rank-sum p = {p:.4g} {">=" if p >= C5_ALPHA else "<"} {C5_ALPHA:.5f}'
        low, high = (min(b), max(b)) if b else (None, None)
        inside = sum(1 for x in a if low is not None and low <= x <= high)
        lane = {'verdicts_agree': verdicts['stage']['exact'] == verdicts['pathsim']['exact'],
                'stage_seeds_inside_pathsim_range': inside, 'stage_seeds': len(a), 'pathsim_range': [low, high],
                'stage_pooled_inside_pathsim_range': low is not None and verdicts['stage']['pooled_ber'] is not None
                and low <= verdicts['stage']['pooled_ber'] <= high}
        rungs.append({'rung': rung, 'target_db': meta[rung][3], 'seeds_per_simulator': seeds, 'stage': verdicts['stage'],
                      'pathsim': verdicts['pathsim'], 'per_seed': {'stage': stage, 'pathsim': pathsim}, 'rank_sum_p': p,
                      'status': status, 'reason': reason, 'lane_literal': lane})
    ladders = ladder_offsets(rungs, meta)
    d62_rows = []
    for rung, sims in d62.items():
        row = {'rung': rung, 'ber_limit': D62_BER_LIMIT}
        for sim, trials in sims.items():
            row[sim] = trials | {'meets_d62': trials['ber'] <= D62_BER_LIMIT}
        row['verdicts_agree'] = ('pathsim' not in sims) or (row['stage']['meets_d62'] == row['pathsim']['meets_d62'])
        row['status'] = 'pass' if row['verdicts_agree'] else 'fail'
        d62_rows.append(row)
    statuses = [r['status'] for r in rungs + ladders + d62_rows]
    if missing:
        status = 'missing'
    elif 'fail' in statuses:
        status = 'fail'
    elif not ladders or any(item['status'] != 'pass' for item in ladders):
        status = 'inconclusive'
    else:
        status = 'pass'
    return {'schema': 'watterson-validation.c5-score/2', 'scored_utc': utc_now(),
            'rule': 'lane call 3 and Q11, kg7hf/waveform-generator#21 (2026-10-09); docs/design/watterson-validation.md 9',
            'alpha_per_rung': C5_ALPHA, 'verdict_seeds': C5_VERDICT_SEEDS,
            'offset_rule': {'target_ber': OFFSET_BER, 'bound_db': OFFSET_BOUND_DB, 'min_points': OFFSET_MIN_POINTS,
                            'step_db': OFFSET_STEP_DB, 'min_seeds': OFFSET_MIN_SEEDS},
            'status': status, 'missing': missing, 'ladders': ladders, 'rungs': rungs, 'd62': d62_rows}


def ladder_offsets(rungs: list[dict], meta: dict[str, tuple]) -> list[dict]:
    """The equivalent-SNR offset of each ladder (rungs of one mode, card and geometry), from the rungs'
    pooled errors and bits (score_manifest). A ladder qualifies with >= 3 SNR points, consecutive points
    2 dB apart, each with >= 4 seeds per simulator; otherwise it is n/a, with the crossings beside it."""
    groups: dict[tuple, list[dict]] = {}
    for rung in rungs:
        groups.setdefault(meta[rung['rung']][:3], []).append(rung)
    out = []
    for (mode, card, geometry), rows in groups.items():
        rows = sorted(rows, key=lambda r: r['target_db'])
        targets = [r['target_db'] for r in rows]
        bound = OFFSET_BOUND_DB['AWGN' if geometry == 'AWGN' else 'fading']
        item = {'ladder': f'{mode} {geometry} ({card})', 'mode': mode, 'card': card, 'geometry': geometry, 'targets_db': targets,
                'bound_db': bound, 'offset_db': None, 'crossings': None, 'add_rungs_db': [],
                'points': [{'snr_db': r['target_db'], 'rung': r['rung'], 'seeds': r['seeds_per_simulator'],
                            'stage': {k: r['stage'][k] for k in ('errors', 'bits', 'pooled_ber')},
                            'pathsim': {k: r['pathsim'][k] for k in ('errors', 'bits', 'pooled_ber')}} for r in rows]}
        if any(r['status'] == 'missing' for r in rows):
            out.append(item | {'status': 'missing', 'reason': 'a decode is missing'})
            continue
        seeds = min(r['seeds_per_simulator'] for r in rows)
        spaced = all(abs((b - a) - OFFSET_STEP_DB) < 1e-9 for a, b in zip(targets, targets[1:]))
        result = wa.snr_offset([(r['target_db'], r['stage']['errors'], r['stage']['bits']) for r in rows],
                               [(r['target_db'], r['pathsim']['errors'], r['pathsim']['bits']) for r in rows],
                               bound, OFFSET_BER, OFFSET_STEP_DB)
        item |= {'crossings': result['crossings'], 'add_rungs_db': result['add_rungs_db']}
        if len(targets) < OFFSET_MIN_POINTS or not spaced or seeds < OFFSET_MIN_SEEDS:
            need = (f"n/a: the curve needs >= {OFFSET_MIN_POINTS} SNR points {OFFSET_STEP_DB:g} dB apart, each pooling >= "
                    f"{OFFSET_MIN_SEEDS} seeds (has {', '.join(f'{t:g}' for t in targets)} dB, {seeds} seeds at the fewest)")
            beside = result['reason'][len('n/a: '):] if result['status'] == 'n/a' else f"read anyway: {result['reason']}"
            out.append(item | {'status': 'n/a', 'reason': f'{need}; {beside}', 'offset_db_unqualified': result['offset_db']})
            continue
        out.append(item | {'status': result['status'], 'reason': result['reason'], 'offset_db': result['offset_db']})
    return out


def crossing_text(crossing: dict | None) -> str:
    if crossing is None:
        return '-'
    if crossing['snr_db'] is None:
        snrs = [p['snr_db'] for p in crossing['points']]
        return {'below': f'below {min(snrs):g}', 'above': f'above {max(snrs):g}'}.get(crossing['outside'], 'no fall')
    low, high = crossing['bracket']
    return f"{crossing['snr_db']:.2f} ({low:g}-{high:g}){'*' if crossing['from_zero_error_point'] else ''}"


def zero_error_notes(score: dict) -> list[str]:
    """The footnote of a crossing marked *: a zero-error point placed at 1/bits gives a point estimate,
    not a bound; with the point at its 95 % bound 3/bits the crossing is later (snr_db_u95)."""
    notes = []
    for item in score['ladders']:
        for name, crossing in (item['crossings'] or {}).items():
            if crossing and crossing.get('from_zero_error_point'):
                later = crossing.get('snr_db_u95')
                notes.append(f"  {item['ladder']} {name}: {crossing['snr_db']:.2f} dB; at 3/bits "
                             + (f"{later:.2f} dB ({later - crossing['snr_db']:.2f} dB later)" if later is not None else 'not bracketed'))
    if notes:
        notes.insert(0, '* uses a zero-error point placed at 1/bits: a point estimate, not a bound (the BER there may be up to its '
                        'Poisson 95 % bound 3/bits):')
    return notes


def score_table(score: dict) -> list[str]:
    """The printed form of a score: the ladders' offsets, the rungs' rank-sum verdicts, D.6.2 rows."""
    lines = []
    if score['ladders']:
        lines += ['Equivalent-SNR offset at BER 1e-3 (lane call 3): stage minus pathsim, from log10(pooled BER) against SNR',
                  f"{'Ladder':<20} {'Points dB':<12} {'Stage 1e-3 dB':<16} {'pathsim 1e-3 dB':<16} {'Offset dB':>9}  {'Bound':>6}  Verdict"]
        for item in score['ladders']:
            crossings = item['crossings'] or {}
            offset = '-' if item['offset_db'] is None else f"{item['offset_db']:+.2f}"
            lines.append(f"{item['ladder']:<20} {', '.join(f'{t:g}' for t in item['targets_db']):<12} "
                         f"{crossing_text(crossings.get('stage')):<16} {crossing_text(crossings.get('pathsim')):<16} {offset:>9}  "
                         f"{item['bound_db']:>6.1f}  {item['status'].upper()}: {item['reason'].removeprefix('n/a: ')}")
        lines += zero_error_notes(score)
        lines.append('')
    lines += [f"Rank-sum per rung (Q11: exact two-sided, alpha 0.05/{len(C5_RUNGS)} = {C5_ALPHA:.5f}, a verdict at >= {C5_VERDICT_SEEDS} "
              'seeds per simulator)',
              f"{'Rung':<20} {'Seeds':>5}  {'Stage':<20} {'pathsim':<20} {'p':>8}  {'Lane literal':<14} Verdict"]
    for r in score['rungs']:
        lane = r['lane_literal']
        p = '-' if r['rank_sum_p'] is None else f"{r['rank_sum_p']:.4f}"
        literal = f"{'agree' if lane['verdicts_agree'] else 'differ'}, {lane['stage_seeds_inside_pathsim_range']}/{lane['stage_seeds']}"
        lines.append(f"{r['rung']:<20} {r['seeds_per_simulator']:>5}  {r['stage']['text']:<20} {r['pathsim']['text']:<20} {p:>8}  "
                     f"{literal:<14} {r['status'].upper()}: {r['reason']}")
    for r in score['d62']:
        sims = ', '.join(f"{sim} BER {r[sim]['ber']:.2e} ({'meets' if r[sim]['meets_d62'] else 'misses'} 1e-5)" for sim in ('stage', 'pathsim') if sim in r)
        lines.append(f"{r['rung']}: {sims} -> {r['status'].upper()}")
    return lines


def score_c5(manifest_path: Path) -> int:
    """--score-c5: score the decodes, print the ladders' offsets and the rungs' verdicts, and write
    score.json beside the manifest. Exit 0 pass, 1 a ladder or rung fails, 2 a decode is missing,
    3 inconclusive (a ladder's 1e-3 crossing outside its points, or too few points)."""
    score = score_manifest(read_json(manifest_path))
    write_json(manifest_path.with_name('score.json'), score)
    for name in score['missing'][:10]:
        print(f'missing decode: {name}')
    if len(score['missing']) > 10:
        print(f"... {len(score['missing'])} decodes missing in all (score.json lists them)")
    for line in score_table(score):
        print(line)
    print(f"criterion 5: {score['status'].upper()}")
    return {'pass': 0, 'fail': 1, 'missing': 2, 'inconclusive': 3}[score['status']]


# ================================================================================================ report

def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()


def fmt(value, digits: int = 3) -> str:
    if value is None:
        return '-'
    if isinstance(value, bool):
        return 'yes' if value else 'no'
    if isinstance(value, float):
        return f'{value:.{digits}f}'
    return str(value)


def joined(values: list, spec: str) -> str:
    return ', '.join('-' if v is None else format(v, spec) for v in values)


def verdict(value) -> str:
    return 'PASS' if value is True else 'FAIL' if value is False else 'n/a'


def summary_markdown(results: dict) -> str:
    """Tables for docs/design/watterson-validation.md, generated from results.json."""
    out = [f"# Watterson validation summary ({results['started_utc']} to {results['finished_utc']})", '',
           f"git {results['git_head']}; driver sha256 {results['binaries']['driver']['sha256'][:16]}; pathsim_chain "
           f"{results['binaries']['pathsim_chain']['sha256'][:16]}; quick={results['quick']}", '']
    for key, block in results['criteria'].items():
        out.append(f"- criterion {key}: {block.get('verdict') or verdict(block.get('pass'))} ({block.get('title', '')})")
    out.append('')
    c = results['criteria']
    if '1' in c:
        out += ['## 1. SNR ladder (N0 x 3000 Hz; stage label, pathsim commanded target - 0.40)', '',
                '| Geometry | Probe | Target dB | Stage dB | s.e. | Stage - target | pathsim dB | pathsim - target | Verdict |', '|---|---|---|---|---|---|---|---|---|']
        for r in c['1']['rows']:
            if r.get('kind') == 'e72':
                continue
            out.append(f"| {r['geometry']} | {r['probe']} | {r['label_db']:g} | {fmt(r['stage']['snr_db'])} | {fmt(r['stage']['se_db'])} | "
                       f"{r['stage']['delta_db']:+.3f} | {fmt(r['pathsim']['snr_db'])} | {r['pathsim']['delta_vs_target_db']:+.3f} | {verdict(r['pass'])} |")
        o = c['1']['offset']
        out += ['', '| Offset over 8 seeds (10 dB, AWGN only) | Mean dB | s.e. | s.d. | Min | Max |', '|---|---|---|---|---|---|']
        for key, v in c['1']['offset_seeds']['summary'].items():
            out.append(f"| {key} | {v['mean_db']:+.4f} | {v['se_db']:.4f} | {v['sd_db']:.4f} | {v['min_db']:+.3f} | {v['max_db']:+.3f} |")
        out += ['', f"Weighted mean offset {o['weighted_mean_db']:+.4f} +/- {o['weighted_mean_se_db']:.4f} dB; chi2 {o['chi2']:.1f} on {o['dof']} dof; "
                f"ENBW {fmt(c['1']['enbw_hz'], 1)} Hz.", '',
                '| E.7.2 target dB | Probe | Analysed s (tone / noise) | Stage band dB | Stage - target | Noise width Hz | pathsim band dB | pathsim - target | Verdict |',
                '|---|---|---|---|---|---|---|---|---|']
        for r in c['1']['rows']:
            if r.get('kind') == 'e72':
                seconds = r['stage'].get('analysed_s') or {}
                out.append(f"| {r['label_db']:g} | {r.get('probe', '-')} | {fmt(seconds.get('tone'), 1)} / {fmt(seconds.get('noise'), 1)} | "
                           f"{fmt(r['stage']['e72_band_db'])} | {r['stage']['delta_db']:+.3f} | {fmt(r['stage']['noise_equiv_width_hz'], 1)} | "
                           f"{fmt(r['pathsim']['e72_band_db'])} | {r['pathsim']['delta_vs_target_db']:+.3f} | {verdict(r['pass'])} |")
        out.append('')
    if '1f' in c:
        out += ['## 1. E.7.3 fading SNR (label 20 dB)', '', '| Geometry | Probe | Analysed s | Stage dB | s.e. | Tap power dB | Stage - 20 - tap | pathsim rev-6 (appE) | Verdict |',
                '|---|---|---|---|---|---|---|---|---|']
        for r in c['1f']['rows']:
            s, b = r['stage'], r['pathsim_baseline']
            out.append(f"| {r['geometry']} | {r['probe']} | {fmt(s.get('analysed_s'), 1)} | {fmt(s['snr_db'])} | {fmt(s['se_db'])} | {s['tap_power_db']:+.3f} | "
                       f"{s['residual_db']:+.3f} | {fmt(b['snr_db'])} +/- {fmt(b['se_db'])} | {verdict(r['pass'])} |")
        out.append('')
    if '2' in c:
        out += ['## 2. Doppler (one path, lane analyze_doppler; images on the 48 kHz output)', '',
                '| d Hz | Stage 2 sigma | err | shape | spur dBc | worst image dBc | frozen-tap floor dBc | continuum dBc | beyond 3d dBc (Q1) | pathsim 2 sigma | pathsim err | pathsim spur dBc | Verdict |',
                '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
        for r in c['2']['rows']:
            if r.get('kind') != 'doppler':
                continue
            s, b = r['stage'], r['pathsim']
            images = s['images']
            out.append(f"| {r['spread_hz']:g} | {fmt(s['spread_2sigma_hz'], 4)} | {100 * s['relative_error']:+.2f} % | {s['shape_class'].split(' ')[0]} | "
                       f"{fmt(s['worst_spur_dbc'], 1)} | {fmt(images['worst_image_dbc'], 1)} | {fmt((images.get('floor') or {}).get('worst_image_window_dbc'), 1)} | "
                       f"{fmt(images.get('sidelobe_continuum_dbc'), 1)} | {fmt(images['out_of_lobe_beyond_3d_dbc'], 1)} | "
                       f"{fmt(b['spread_2sigma_hz'], 4)} | {100 * b['relative_error']:+.2f} % | {fmt(b['worst_spur_dbc'], 1)} | {verdict(r['pass'])} |")
        q1 = c['2'].get('q1')
        if q1:
            out += ['', f"Q1: per spur worst {fmt(q1['per_spur']['worst_spur_dbc'], 1)} dBc (analyzer) and {fmt(q1['per_spur']['worst_image_dbc'], 1)} dBc (images), "
                    f"{verdict(q1['per_spur']['pass'])}; total power beyond +/-3 d {fmt(q1['total_power_beyond_3d']['best_dbc'], 1)} to "
                    f"{fmt(q1['total_power_beyond_3d']['worst_dbc'], 1)} dBc, {verdict(q1['total_power_beyond_3d']['pass'])}. Verdict: {c['2'].get('verdict')}."]
        for r in c['2']['rows']:
            if r.get('kind') == 'e74':
                dev = ', '.join(f"{d['level_db']:g} {d['side']} {d['deviation_db']:+.2f}" for d in r['stage']['e_i'])
                out += ['', f"E.7.4 ({r['probe']}, analysed {fmt(r['stage'].get('analysed_s'), 1)} s, 1 Hz): stage 2 sigma {fmt(r['stage']['spread_2sigma_hz'], 4)} Hz; "
                        f"Table E-I deviations {dev} dB; {verdict(r['pass'])}."]
        out += ['', '| Delay row | Taps | Delay errors (frames) | Power errors (dB) | Latency | E.7.1 / comb max error dB | pathsim delay errors | Verdict |',
                '|---|---|---|---|---|---|---|---|']
        for r in c['2']['rows']:
            if r.get('kind') != 'delay':
                continue
            s = r['stage']
            comb = max([abs(v) for v in s['e71_points_error_db'].values()] + ([s['response_rows_max_error_db']] if s['response_rows_max_error_db'] is not None else []), default=None)
            ps = r.get('pathsim')
            out.append(f"| {r['name']} | {s['n_taps']} | {joined(s['delay_errors'], '+.3f')} | {joined(s['power_errors_db'], '+.2f')} | "
                       f"{fmt(s['latency_frames'], 2)} | {fmt(comb, 2)} | {joined(ps['delay_errors'], '+.3f') if ps else '-'} | {verdict(r['pass'])} |")
        out.append('')
    if '3' in c:
        out += ['## 3. Taps', '']
        for r in c['3']['rows']:
            if r.get('kind') == 'truth':
                out += ['| Path | d Hz | set dB | power error dB | z | 2 sigma err | shape | E|h|^4/E^2 | mean/RMS | KS (crit) |', '|---|---|---|---|---|---|---|---|---|---|']
                for p in r['paths']:
                    n = p['nodes']
                    out.append(f"| {p['path']} | {p['spread_hz']:g} | {p['power_db_set']:g} | {p['power_error_db']:+.3f} | {p['power_z']:+.2f} | "
                               f"{100 * p['relative_error']:+.2f} % | {p['shape_class'].split(' ')[0]} | {n['fourth_moment_ratio']:.3f} | {n['mean_over_rms']:.4f} | "
                               f"{n['ks_distance']:.4f} ({n['ks_critical_95']:.4f}) |")
                out += ['', 'Pair correlations: ' + '; '.join(f"{q['paths']}: {q['rho']:.4f} (bound {q['bound']:.4f})" for q in r['pairs']) +
                        f"; sum a^2 v = {r['sum_a2v']:.15f}.", '']
            if r.get('kind') == 'rician':
                out.append(f"Rician (set {r['k_set_db']:g} dB): truth {r['truth']['k_db']:.3f} +/- {r['truth']['k_db_se']:.3f} dB over {r['truth']['duration_s']:g} s; "
                           f"48 kHz audio {r['audio_600s']['k_db']:.3f} +/- {r['audio_600s']['k_db_se']:.3f} dB over 600 s; {verdict(r['pass'])}.")
            if r.get('kind') == 'envelope':
                st, ps = r['stage'], r['pathsim']
                out.append(f"Envelope d = {r['spread_hz']:g} Hz: stage KS {st['ks']:.4f} ({st['ks_critical']:.4f}), mean/RMS {st['mean_over_rms']:.4f}, "
                           f"LCR {joined([x['ratio'] for x in st['lcr']], '.3f')}; pathsim KS {ps['ks']:.4f}, mean/RMS {ps['mean_over_rms']:.4f}, "
                           f"LCR {joined([x['ratio'] for x in ps['lcr']], '.3f')}; {verdict(r['pass'])}.")
        out.append('')
    if '4' in c:
        out += ['## 4. Noise', '']
        for r in c['4']['rows']:
            if r.get('kind') == 'b6a':
                for sim in ('stage', 'pathsim'):
                    s = r[sim]['stats']
                    out.append(f"B6a {sim}: flatness 100 Hz {s['flatness_100hz_max_abs_db']:.3f} dB, 10 Hz {s['flatness_10hz_max_abs_db']:.3f} dB (s.e. {s['flatness_10hz_standard_error_db']:.3f}); "
                               f"ENBW {s['enbw_measured_hz']:.1f} Hz; N0x3000 SNR {s['delivered_snr_db_n0x3000']:.3f} dB; excess kurtosis {s['excess_kurtosis']:+.5f} "
                               f"(z {s['excess_kurtosis_z']:+.2f}); tails z {joined([t['z'] for t in s['tails']], '+.2f')}.")
                ln = r['stage']['lines']
                out.append(f"B6a lines: floor {fmt(ln['floor_dbc'], 1)} dBc, detected {ln['detected']}, worst {fmt(ln['worst_line_dbc'], 1)} dBc. {verdict(r['pass'])}.")
            if r.get('kind') == 'b6b':
                labels = {ENGINE_BLOCK_FRAMES: 'engine block: the frame grid of the render'} | {q: f'2048 / {ENGINE_BLOCK_FRAMES // q} (AWGN-only render)' for q in FOLD_DERIVED}
                out += ['', '| Fold period | Grid | Repetitions | In-band harmonics | Mean dBc | Threshold dBc | Worst dBc (Hz) | Above threshold |', '|---|---|---|---|---|---|---|---|']
                for f in r['stage']['folds']['folds']:
                    out.append(f"| {f['period']} | {labels.get(f['period'], '-')} | {f['repetitions']} | {f['inband_count']} | {fmt(f['mean_harmonic_dbc'], 1)} | {fmt(f['threshold_dbc'], 1)} | "
                               f"{fmt(f['worst_dbc'], 1)} ({fmt(f['worst_frequency_hz'], 1)}) | {f['above_threshold']} |")
                pc = r['stage']['folds']['positive_control']
                out += ['', f"Positive control: {pc['injected_dbc']:g} dBc at {pc['frequency_hz']:.1f} Hz read {pc['measured_dbc']:.1f} dBc, detected {pc['detected']}. "
                        f"5 h excess kurtosis {r['stage']['folds']['excess_kurtosis_5h']:+.5f}. {verdict(r['pass'])}.", '']
            if r.get('kind') == 'b6c':
                ln = r['stage']['lines']
                out.append(f"B6c (tone + fixed path + 60 dB): floor {fmt(ln['floor_dbc'], 1)} dBc, detected {ln['detected']}, worst detected line "
                           f"{fmt(ln['worst_line_dbc'], 1)} dBc, worst comb candidate {fmt(ln['worst_explicit_dbc'], 1)} dBc; pathsim detected "
                           f"{r['pathsim']['lines']['detected']}, worst {fmt(r['pathsim']['lines']['worst_line_dbc'], 1)} dBc "
                           f"(floor {fmt(r['pathsim']['lines']['floor_dbc'], 1)}). {verdict(r['pass'])}.")
            if r.get('kind') == 'b6d':
                ln = r['stage']['lines']
                top = ', '.join(f"{x['frequency_hz']:.1f} Hz {fmt(x['level_dbc'], 1)}" for x in ln['lines'][:3])
                out.append(f"B6d {r['case']} (tone at {r['shifted_tone_hz']:.4f} Hz, excluded {r['excluded_hz'][0]:.1f}-{r['excluded_hz'][1]:.1f} Hz): "
                           f"floor {fmt(ln['floor_dbc'], 1)} dBc, detected {ln['detected']} ({top or 'none'}), worst grid candidate "
                           f"{fmt(ln['worst_explicit_dbc'], 1)} dBc. {verdict(r['pass'])}.")
        out.append('')
    if '6' in c:
        out += ['## 6. Determinism', '']
        for r in c['6']['rows']:
            if r.get('kind') == 'engine-equivalence':
                out.append(f"Driver vs signal_lab_render (AWGN 18 dB, no Watterson stage): {r.get('driver_sha256', '-')[:16]} vs "
                           f"{r.get('signal_lab_render_sha256', '-')[:16]}. {verdict(r['pass'])}.")
            if 'case' in r:
                out.append(f"{r['case']}: {len(r['runs'])} renders (blocks {', '.join(str(x['block']) for x in r['runs'])}) identical: {r['identical']} "
                           f"(sha256 {r['runs'][0]['sha256'][:16]}, digest {r['runs'][0]['digest']}); seed 2 differs in {r['other_seed']['differing_samples']} of "
                           f"{r['other_seed']['frames']} samples; pathsim {r.get('pathsim', {}).get('same_seed_identical', '-')}/"
                           f"{r.get('pathsim', {}).get('other_seed_differs', '-')}. {verdict(r['pass'])}.")
        out.append('')
    if '7' in c:
        matrix = c['7'].get('matrix') or {}
        judged = f"not judged ({matrix['not_judged']})" if matrix.get('not_judged') else verdict(matrix.get('pass'))
        out += ['## 7. Coverage', '', f'Coverage matrix: {judged}.', '', '```', json.dumps(c['7'].get('matrix'), indent=1), '```', '']
        for r in c['7']['rows']:
            if r.get('kind') == 'shift':
                out.append(f"Fixed {r['shift_hz']:+g} Hz: {r['stage']['measured_hz']:.6f} Hz (error {1e3 * r['stage']['error_hz']:+.4f} mHz), mirror {r['stage']['mirror_dbc']:.1f} dBc; "
                           f"pathsim {r['pathsim']['measured_hz']:.6f} Hz, mirror {r['pathsim']['mirror_dbc']:.1f} dBc. {verdict(r['pass'])}.")
            if r.get('kind') == 'rayleigh-shift':
                out.append(f"Rayleigh 1 Hz at +75 Hz: centre {r['stage']['centre_hz']:.4f} Hz, 2 sigma {r['stage']['spread_2sigma_hz']:.4f}; pathsim centre "
                           f"{fmt(r['pathsim']['centre_hz'], 4)}. {verdict(r['pass'])}.")
            if r.get('kind') == 'phase':
                out.append(f"Fixed path 90 degrees: measured {r['measured_deg']:.5f} (error {r['error_deg']:+.5f}), amplitude ratio {r['amplitude_ratio']:.6f}. {verdict(r['pass'])}.")
        out.append('')
        b10 = c['7'].get('b10') or []
        if b10:
            out += ['### B10, first battery (Table XVI two-path rows)', '',
                    '| Geometry | SNR: stage dB | tap power | stage - 20 - tap | pathsim dB | delay errors (frames) | power errors (dB) | latency | 2 sigma per path | KS / crit | pair rho (bound) | Verdicts |',
                    '|---|---|---|---|---|---|---|---|---|---|---|---|']
            for delay_ms, spread in TABLE_XVI:
                name = f'{delay_ms:g}ms-{spread:g}Hz'
                snr = next((r for r in b10 if r.get('kind') == 'b10-snr' and r['name'] == name), None)
                dly = next((r for r in b10 if r.get('kind') == 'delay' and r['name'] == f'b10-{name}'), None)
                tru = next((r for r in b10 if r.get('kind') == 'truth' and r.get('name') == name), None)
                if not (snr and dly and tru):
                    continue
                s, d = snr['stage'], dly['stage']
                sig = ', '.join(f"{p['spread_2sigma_hz']:.4f}" for p in tru['paths'])
                ks = ', '.join(f"{p['nodes']['ks_distance']:.4f}/{p['nodes']['ks_critical_95']:.4f}" for p in tru['paths'])
                rho = '; '.join(f"{q['rho']:.4f} ({q['bound']:.4f})" for q in tru['pairs'])
                out.append(f"| {name} | {fmt(s['snr_db'])} | {s['tap_power_db']:+.3f} | {s['residual_db']:+.3f} | {fmt(snr['pathsim']['snr_db'])} | "
                           f"{joined(d['delay_errors'], '+.3f')} | {joined(d['power_errors_db'], '+.2f')} | {fmt(d['latency_frames'], 2)} | {sig} | {ks} | {rho} | "
                           f"{verdict(snr['pass'])} / {verdict(dly['pass'])} / {verdict(tru['pass'])} |")
            out.append('')
    if '5' in c:
        out += ['## 5. Decoder equivalence', '', f"{c['5']['renders']} renders; manifest {c['5']['manifest']}; decode script {c['5']['decode_script']}; "
                f"decoded: {fmt(c['5'].get('decoded'))}.", '',
                '| Rung | Seed | Stage companion SNR dB | pathsim companion SNR dB |', '|---|---|---|---|']
        pairs = {}
        for r in c['5']['rows']:
            pairs.setdefault((r['rung'], r['seed']), {})[r['simulator']] = r['companion']['snr_db']
        for (rung, seed), v in pairs.items():
            out.append(f"| {rung} | {seed} | {fmt(v.get('stage'))} | {fmt(v.get('pathsim'))} |")
        out.append('')
        out += [f"Criterion 5: {c['5'].get('verdict') or verdict(c['5'].get('pass'))} (c5's rungs and D.6.2, with the threshold set's "
                'offsets and rungs where c5t exists).', '']
        score = c['5'].get('score')
        if score:
            out += [f"c5 score: {score['status'].upper()}.", ''] + score_markdown(score)
        threshold = c['5'].get('threshold')
        if threshold:
            out += [f"Threshold set ({threshold['manifest']}): {threshold['renders']} renders, {threshold['decoded']} decoded; "
                    f"score {threshold['score']['status'].upper()}.", '']
            if not threshold['score']['missing']:
                out += score_markdown(threshold['score'])
    return '\n'.join(out) + '\n'


def score_markdown(score: dict) -> list[str]:
    """A criterion 5 score as markdown: the ladders' offsets, the rungs' rank-sum, D.6.2 rows."""
    out = []
    if score.get('ladders'):
        out += ['| Ladder | Points dB | Stage 1e-3 dB | pathsim 1e-3 dB | Offset dB | Bound dB | Verdict |', '|---|---|---|---|---|---|---|']
        for item in score['ladders']:
            crossings = item['crossings'] or {}
            out.append(f"| {item['ladder']} | {', '.join(f'{t:g}' for t in item['targets_db'])} | {crossing_text(crossings.get('stage'))} | "
                       f"{crossing_text(crossings.get('pathsim'))} | {fmt(item['offset_db'], 2)} | {item['bound_db']:g} | "
                       f"{item['status']}: {item['reason'].removeprefix('n/a: ')} |")
        notes = zero_error_notes(score)
        out += ([''] + notes if notes else []) + ['']
    out += ['| Rung | Seeds | Stage | pathsim | rank-sum p | Status | Lane literal: verdicts, stage seeds in pathsim range |',
            '|---|---|---|---|---|---|---|']
    for r in score['rungs']:
        lane = r['lane_literal']
        out.append(f"| {r['rung']} | {r['seeds_per_simulator']} | {r['stage']['text']} | {r['pathsim']['text']} | {fmt(r['rank_sum_p'], 4)} | "
                   f"{r['status']}: {r['reason']} | {'agree' if lane['verdicts_agree'] else 'differ'}, {lane['stage_seeds_inside_pathsim_range']} of {lane['stage_seeds']} |")
    if score['d62']:
        out += ['', '| D.6.2 row (50 dB) | Stage BER | pathsim BER | Verdicts agree |', '|---|---|---|---|']
        for r in score['d62']:
            theirs = f"{r['pathsim']['ber']:.2e}" if 'pathsim' in r else 'no twin (4 paths)'
            out.append(f"| {r['rung']} | {r['stage']['ber']:.2e} | {theirs} | {fmt(r['verdicts_agree'])} |")
    out.append('')
    return out


def git_head() -> str:
    result = subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, default=REPO / 'build/main/host-c', help='host build with wfg_watterson_render')
    parser.add_argument('--pathsim-build', type=Path, default=REPO / 'build/main/pathsim-host', help='channel-sim build with pathsim_chain')
    parser.add_argument('--work', type=Path, help='work directory (default <build>/work/watterson-validation)')
    parser.add_argument('--lane-root', type=Path, help='external reference-data root (required except with --score-c5; read only)')
    parser.add_argument('--gate-script', type=Path, default=REPO / 'build/main/bringup/ab_gate.ps1', help='shared-PC A/B hold gate')
    parser.add_argument('--criteria', default='1,2,3,4,6,7,5', help='comma-separated criteria (default all; criterion 7\'s '
                        'coverage matrix is judged only when all run, without --quick)')
    parser.add_argument('--quick', action='store_true', help='shorter long rows for a smoke run')
    parser.add_argument('--resume', action='store_true', help='reuse rows/<id>.json records')
    parser.add_argument('--keep-outputs', action='store_true', help='keep renders larger than 64 MB (default: hash, then delete)')
    parser.add_argument('--score-c5', type=Path, help='score the lane decodes of a c5 manifest and exit')
    parser.add_argument('--c5-extend', default='', help='criterion 5: rungs (e.g. C24L05-MPP-10dB) rendered at seeds 1 to 12, '
                        'or "auto" for every rung whose decodes show a bit error')
    parser.add_argument('--c5-threshold', nargs='?', const=C5_THRESHOLD_SET, default=None,
                        help='render only criterion 5\'s threshold set into <work>/c5t (seeds 1 to 12, both simulators): '
                             f'<card>-<MPP|AWGN>:<dB>,... per ladder, ";" between ladders (default {C5_THRESHOLD_SET})')
    parser.add_argument('--c5-reuse', type=Path, help='with --c5-threshold: the c5 manifest whose byte-identical renders are '
                        'referenced (default <work>/c5/manifest.json)')
    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(newline='\n')  # a log redirected from the runner is LF (Windows text mode writes CRLF)
    if args.score_c5:
        return score_c5(args.score_c5)
    if args.lane_root is None:
        parser.error('--lane-root is required for validation; supply your external reference-data directory')
    if args.c5_threshold is not None:
        try:
            parse_threshold_set(args.c5_threshold)
        except ValueError as error:
            parser.error(str(error))
    below_normal_self()
    args.build = args.build.resolve()
    args.pathsim_build = args.pathsim_build.resolve()
    args.work = (args.work or args.build / 'work' / 'watterson-validation').resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    exe = '.exe' if os.name == 'nt' else ''
    args.driver = args.build / 'signal-lab' / f'wfg_watterson_render{exe}'
    args.chain = args.pathsim_build / f'pathsim_chain{exe}'
    args.renderer = args.build / 'host' / 'render' / f'signal_lab_render{exe}'
    args.toolchain_bin = toolchain_bin(args.build)
    for required in (args.driver, args.chain, args.lane_root / 'hfchannel/probes/analyze_snr.py'):
        if not required.is_file():
            sys.exit(f'missing: {required}')
    selected = [c.strip() for c in args.criteria.split(',') if c.strip()]
    runner = Runner(args)
    probes = Probes(runner)
    started = utc_now()
    if args.c5_threshold is not None:
        block = c5_threshold_prepare(runner, probes)
        run = {'schema': 'watterson-validation.c5t-run/1', 'started_utc': started, 'finished_utc': utc_now(), 'git_head': git_head(),
               'tool_sha256': runner.identity['tool_sha256'], 'analysis_sha256': runner.identity['analysis_sha256'], 'argv': sys.argv,
               'gate_waits': runner.gate_waits, 'summary': block, 'commands': runner.log}
        write_json(runner.inside('c5t', 'run.json'), run)
        print(f"c5t: {block['renders']} renders ({block['rendered']} rendered here, {block['reused']} reused from c5), "
              f"{block['decoded']} decoded; clip fallback on {len(block['clip_fallback'])}; manifest {block['manifest']}; "
              f"decode script {block['decode_script']}")
        return 0
    criteria: dict[str, dict] = {}
    if '1' in selected:
        criteria['1'] = c1_snr_ladder(runner, probes)
        criteria['1f'] = c1_fading(runner, probes)
    doppler, truth = [], []
    if '2' in selected or '3' in selected:
        doppler = doppler_rows(runner, probes)
        truth = truth_rows(runner)
    if '2' in selected:
        rows = list(doppler)
        e74 = e74_row(runner, probes)
        rows += [e74] if e74 else []
        rows += delay_rows(runner, probes)
        criteria['2'] = {'criterion': 2, 'title': 'Doppler, images, per-path spread, delays', 'rows': rows,
                         'per_path': truth[0] if truth else None,
                         'pass': all(r['pass'] for r in rows) and all(t['pass_per_path_doppler'] for t in truth)}
        criteria['2']['q1'] = q1_reading(rows)
        criteria['2']['verdict'] = criterion2_verdict(criteria['2'])
    if '3' in selected:
        rows = []
        for r in doppler:
            row = {'criterion': 3, 'kind': 'envelope', 'spread_hz': r['spread_hz'],
                   'stage': envelope_verdict(r['stage']['envelope'], r['spread_hz']),
                   'pathsim': envelope_verdict(r['pathsim']['envelope'], r['spread_hz'])}
            row['pass'] = row['stage']['pass']
            rows.append(row)
        rows += truth + rician_rows(runner, probes)
        powers = [r for r in delay_rows(runner, probes) if r['name'] == 'fading-4path-powers'] if '2' not in selected else \
            [r for r in criteria['2']['rows'] if r.get('name') == 'fading-4path-powers']
        rows += [{'criterion': 3, 'kind': 'audio-powers', 'row': p['id'], 'power_errors_db': p['stage']['power_errors_db'], 'pass': p['pass']} for p in powers]
        criteria['3'] = {'criterion': 3, 'title': 'Rayleigh envelope, powers, correlation, Rician', 'rows': rows,
                         'pass': all(r['pass'] for r in rows if r.get('pass') is not None)}
    if '4' in selected:
        rows = noise_rows(runner, probes)
        criteria['4'] = {'criterion': 4, 'title': 'noise: flatness, Gaussianity, spurs', 'rows': rows, 'pass': all(r['pass'] for r in rows)}
    if '6' in selected:
        rows = determinism_rows(runner, probes)
        criteria['6'] = {'criterion': 6, 'title': 'determinism', 'rows': rows, 'pass': all(r['pass'] for r in rows if r.get('pass') is not None)}
    if '7' in selected:
        rows = coverage_rows(runner, probes)
        b10 = b10_rows(runner, probes)
        criteria['7'] = {'criterion': 7, 'title': 'coverage (host parameters; ppm and INFO? identity not here), first battery B10', 'rows': rows,
                         'b10': b10, 'not_run': ['ppm (the sample-clock stage is deferred, lane 2026-10-08)',
                                                 'scenario hash in the playlist item and INFO? (firmware, design WP9)']}
    if '5' in selected:
        criteria['5'] = c5_prepare(runner, probes)
    if '7' in selected:
        criteria['7'] = criterion7_close(criteria['7'], args.work, selected, args.quick)  # after criterion 5 (D.6.2's renders)
    binaries = {name: {'path': str(path), 'sha256': sha256(path)} for name, path in (('driver', args.driver), ('pathsim_chain', args.chain))}
    lane_tools = {name: sha256(args.lane_root / 'hfchannel/probes' / name) for name in
                  ('analyze_snr.py', 'analyze_doppler.py', 'analyze_delay.py', 'common.py', 'gen_probes.py', 'align.py')}
    results = {'schema': 'watterson-validation.results/1', 'started_utc': started, 'finished_utc': utc_now(), 'git_head': git_head(),
               'tool': str(Path(__file__).resolve()), 'tool_sha256': sha256(Path(__file__).resolve()),
               'analysis_sha256': sha256(Path(wa.__file__).resolve()), 'argv': sys.argv, 'quick': args.quick,
               'python': sys.version.split()[0], 'numpy': np.__version__, 'platform': platform.platform(),
               'work': str(args.work), 'lane_root': str(args.lane_root), 'lane_tools_sha256': lane_tools, 'binaries': binaries,
               'gate_waits': runner.gate_waits, 'criteria': criteria, 'commands': runner.log}
    write_json(args.work / 'results.json', results)
    (args.work / 'summary.md').write_bytes(summary_markdown(results).encode('utf-8'))
    for key, block in criteria.items():
        print(f"criterion {key}: {block.get('verdict') or verdict(block.get('pass'))}")
    print(f"results {args.work / 'results.json'}; summary {args.work / 'summary.md'}")
    return 0 if all(block.get('pass') is not False for block in criteria.values()) else 1


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main())
