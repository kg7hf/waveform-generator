"""Block-streaming pipeline: SampleSource -> [Impairment...] -> SampleSink.

Each impairment sees float64 blocks plus the absolute index of the block's
first *input* frame.  A stage may change the block length (sample slips);
downstream stages then see the shifted timeline, which is why transport
corruption is applied last in every scenario.
"""

import math

import numpy as np

from . import BLOCK, FS


def db_to_amplitude(db):
    return 10.0 ** (float(db) / 20.0)


def amplitude_to_db(amplitude):
    return 20.0 * math.log10(amplitude) if amplitude > 0 else None


def seconds_to_frames(seconds, name="value", positive=False):
    seconds = float(seconds)
    if not math.isfinite(seconds) or seconds < 0:
        raise ValueError(name + " must be a finite non-negative number of seconds")
    frames = int(round(seconds * FS))
    if positive and frames <= 0:
        raise ValueError(name + " must span at least one sample")
    return frames


def number(value, name, low=None, high=None):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(name + " must be a number")
    value = float(value)
    if not math.isfinite(value) or (low is not None and value < low) or (high is not None and value > high):
        raise ValueError(name + " is out of range or non-finite")
    return value


class Impairment:
    """Base class.  Subclasses override prepare() and process()."""

    type_name = "impairment"
    changes_length = False

    def __init__(self, params):
        self.params = dict(params)
        self.reference_rms = None
        self.total_frames = None
        self.seed = None
        self.index = 0
        self.instance_index = 0
        self.energy = 0.0
        self.peak = 0.0
        self.frames_seen = 0

    def prepare(self, reference_rms, total_frames, seed, index):
        """Bind the run-time context (reference RMS, input length, seed) before streaming."""
        self.reference_rms = float(reference_rms)
        self.total_frames = int(total_frames)
        self.seed = int(seed)
        self.index = int(index)

    def process(self, block, first_frame):
        raise NotImplementedError

    def account(self, component):
        """Accumulate the RMS/peak of an additive component."""
        if len(component):
            self.energy += float(np.dot(component, component))
            self.peak = max(self.peak, float(np.max(np.abs(component))))
            self.frames_seen += len(component)

    def resolved(self):
        """Fully-resolved parameters (defaults filled, frames computed) for the sidecar."""
        return dict(self.params)

    def stats(self):
        rms = math.sqrt(self.energy / self.frames_seen) if self.frames_seen else 0.0
        return {"component_rms_dbfs": amplitude_to_db(rms),
                "component_peak_dbfs": amplitude_to_db(self.peak),
                "component_rms_rel_reference_db": (amplitude_to_db(rms / self.reference_rms)
                                                   if rms > 0 and self.reference_rms else None)}

    def events(self):
        return []


class Pipeline:
    def __init__(self, stages):
        self.stages = list(stages)

    def prepare(self, reference_rms, total_frames, seed):
        instances = {}
        for index, stage in enumerate(self.stages):
            # AWGN keeps its first legacy stream at zero regardless of stage
            # order, while repeated instances need independent noise streams.
            stage.instance_index = instances.get(stage.type_name, 0)
            instances[stage.type_name] = stage.instance_index + 1
            stage.prepare(reference_rms, total_frames, seed, index)

    def run(self, samples, block=BLOCK):
        """Yield processed float64 blocks for the whole input array (input-frame aligned)."""
        total = len(samples)
        cursor = 0
        while cursor < total:
            count = min(block, total - cursor)
            data = np.array(samples[cursor:cursor + count], dtype=np.float64, copy=True)
            for stage in self.stages:
                data = stage.process(data, cursor)
            yield data
            cursor += count
