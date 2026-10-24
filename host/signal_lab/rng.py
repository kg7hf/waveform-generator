# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Deterministic random-number generators (pcm_stress-compatible seeding)."""

import numpy as np

UINT32_MAX = (1 << 32) - 1
UINT64_MASK = (1 << 64) - 1
STREAM_ID_DOMAIN = 0x5746475354524D31
STREAM_ID_ALGORITHM_VERSION = 1
_SPLITMIX_GAMMA = 0x9E3779B97F4A7C15
_SPLITMIX_MUL1 = 0xBF58476D1CE4E5B9
_SPLITMIX_MUL2 = 0x94D049BB133111EB


def validate_stream_id(value):
    """Return a valid explicit stream ID, rejecting bools and fractional values."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("stream_id must be an integer in [1, 2^32-1]")
    if not 1 <= value <= UINT32_MAX:
        raise ValueError("stream_id must be an integer in [1, 2^32-1]")
    numeric = float(value)
    if not np.isfinite(numeric) or not numeric.is_integer() or not 1 <= numeric <= UINT32_MAX:
        raise ValueError("stream_id must be an integer in [1, 2^32-1]")
    return int(numeric)


def splitmix64_next(state):
    """One fixed SplitMix64 output step, masked to unsigned 64-bit arithmetic."""
    value = (int(state) + _SPLITMIX_GAMMA) & UINT64_MASK
    value = ((value ^ (value >> 30)) * _SPLITMIX_MUL1) & UINT64_MASK
    value = ((value ^ (value >> 27)) * _SPLITMIX_MUL2) & UINT64_MASK
    return (value ^ (value >> 31)) & UINT64_MASK


def effective_stream_seed(master_seed, stream_id):
    """Resolve an opt-in stable stage seed without changing legacy PCG64 keys."""
    stream_id = validate_stream_id(stream_id)
    state = (int(master_seed) ^ STREAM_ID_DOMAIN ^ stream_id) & UINT64_MASK
    return splitmix64_next(state)

# Family ids.  1-4 mirror tools/pcm_stress.py so overlapping models reproduce
# its streams; 5+ are signal_lab additions. Families 10, 11, and 12 are reserved
# for the Markov CW gate, random FSK symbols, and CW Doppler fading, respectively,
# so they do not share phase or noise draw sequences.
FAMILY_BACKGROUND_NOISE = 1
FAMILY_IMPULSE_NOISE = 2
FAMILY_ELECTRICAL_NOISE = 3
FAMILY_IMPULSE_SCHEDULE = 4
FAMILY_IMPULSE_PHASE = 5
FAMILY_SLIP_SCHEDULE = 6
FAMILY_TONE_PHASE = 7
FAMILY_FADE_SCHEDULE = 8
FAMILY_MARKOV_GATE = 10
FAMILY_FSK_SYMBOL = 11
FAMILY_CW_DOPPLER = 12


def generator(seed, family, index=0):
    """PCG64 generator keyed by (seed, family, index); identical to pcm_stress."""
    return np.random.Generator(np.random.PCG64(np.random.SeedSequence([int(seed), int(family), int(index)])))
