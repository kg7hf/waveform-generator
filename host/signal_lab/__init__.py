"""signal_lab: waveform-agnostic PCM impairment engine (Phase 1, Python reference).

The engine operates on 48 kHz mono sample streams and never inspects the
protocol that produced them.  Every impairment is a block-streaming transform
(Impairment.process(block, first_frame)) so the same structure ports to the
Phase 2 portable C++ engine and the Phase 3 RT1170 real-time player.

Level conventions follow the owner's pcm_stress pilot (tools/pcm_stress.py):
all interference/noise levels are relative to one fixed clean-input reference
interval RMS, noise is band-limited 300-3300 Hz with the same L2-normalised
Blackman-windowed-sinc FIR, and randomness is NumPy PCG64 seeded by
SeedSequence([seed, family, index]).
"""

VERSION = "signal-lab-py/0.1.1"
SCENARIO_SCHEMA = "signal-lab.scenario/1"
SIDECAR_SCHEMA = "signal-lab.sidecar/1"
FS = 48000
BLOCK = 4096
