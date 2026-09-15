# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""signal_lab: waveform-agnostic PCM impairment engine (Python reference).

The engine operates on 48 kHz mono sample streams and never inspects the
protocol that produced them.  Every impairment is a block-streaming transform
(Impairment.process(block, first_frame)) so the same structure ports to the
portable C++ engine and the RT1170 real-time player.

Level conventions:
all interference/noise levels are relative to one fixed clean-input reference
interval RMS, noise is band-limited 300-3300 Hz with the same L2-normalised
Blackman-windowed-sinc FIR, and randomness is NumPy PCG64 seeded by
SeedSequence([seed, family, index]).
"""

VERSION = "signal-lab-py/0.2.0"
SCENARIO_SCHEMA = "signal-lab.scenario/1"
SIDECAR_SCHEMA = "signal-lab.sidecar/1"
FS = 48000
BLOCK = 4096
