"""Deterministic random-number generators (pcm_stress-compatible seeding)."""

import numpy as np

# Family ids.  1-4 mirror tools/pcm_stress.py so overlapping models reproduce
# its streams; 5+ are signal_lab additions.
FAMILY_BACKGROUND_NOISE = 1
FAMILY_IMPULSE_NOISE = 2
FAMILY_ELECTRICAL_NOISE = 3
FAMILY_IMPULSE_SCHEDULE = 4
FAMILY_IMPULSE_PHASE = 5
FAMILY_SLIP_SCHEDULE = 6
FAMILY_TONE_PHASE = 7
FAMILY_FADE_SCHEDULE = 8


def generator(seed, family, index=0):
    """PCG64 generator keyed by (seed, family, index); identical to pcm_stress."""
    return np.random.Generator(np.random.PCG64(np.random.SeedSequence([int(seed), int(family), int(index)])))
