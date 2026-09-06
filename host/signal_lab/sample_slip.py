"""Sample-stream corruption: deleted and duplicated sample runs (USB/buffer slips).

Events are placed in *input* frame coordinates.  "delete" removes the
length_samples frames starting at the event frame.  "duplicate" re-emits the
last length_samples frames that were delivered immediately before the event
frame (what a stalled buffer replays), so the output grows by that amount.
Placements: single (at_seconds), spaced (first_seconds, period_seconds,
count), clustered (first_seconds, spacing_seconds, count) or an explicit
events list.  Sample slips must be the last stage of a scenario because they
shift the timeline seen by later stages.
"""

import numpy as np

from . import FS
from .stream import Impairment, seconds_to_frames

KINDS = ("delete", "duplicate")


class SampleSlip(Impairment):
    """params: events [{at_seconds, kind, length_samples}] | (kind, length_samples, placement, at_seconds |
    first_seconds + period_seconds/spacing_seconds + count)."""

    type_name = "sample_slip"
    changes_length = True

    def prepare(self, reference_rms, total_frames, seed, index):
        super().prepare(reference_rms, total_frames, seed, index)
        p = self.params
        events = []
        if "events" in p:
            for item in p["events"]:
                events.append((seconds_to_frames(item["at_seconds"], "sample_slip.at_seconds"), item["kind"], int(item["length_samples"])))
            self.placement = "explicit"
        else:
            kind, length = p["kind"], int(p["length_samples"])
            self.placement = p.get("placement", "single")
            if self.placement == "single":
                starts = [seconds_to_frames(p["at_seconds"], "sample_slip.at_seconds")]
            elif self.placement == "spaced":
                first = seconds_to_frames(p["first_seconds"], "sample_slip.first_seconds")
                period = seconds_to_frames(p["period_seconds"], "sample_slip.period_seconds", True)
                starts = [first + k * period for k in range(int(p["count"]))]
            elif self.placement == "clustered":
                first = seconds_to_frames(p["first_seconds"], "sample_slip.first_seconds")
                spacing = seconds_to_frames(p["spacing_seconds"], "sample_slip.spacing_seconds", True)
                starts = [first + k * spacing for k in range(int(p["count"]))]
            else:
                raise ValueError("sample_slip.placement must be single, spaced, clustered")
            events = [(s, kind, length) for s in starts]
        for frame, kind, length in events:
            if kind not in KINDS:
                raise ValueError("sample_slip kind must be delete or duplicate")
            if length <= 0 or length > FS:
                raise ValueError("sample_slip length_samples must be in 1..48000")
            if kind == "duplicate" and frame < length:
                raise ValueError("sample_slip duplicate needs length_samples frames of history before it")
        self.planned = sorted(events)
        self.applied = []
        self.dropped = []
        self.cursor = 0
        self.delete_remaining = 0
        self.max_length = max([e[2] for e in events] + [1])
        self.history = np.zeros(0)
        self.input_frames = 0
        self.output_frames = 0

    def process(self, block, first_frame):
        count = len(block)
        parts = []
        i = 0
        if self.delete_remaining:
            skip = min(self.delete_remaining, count)
            i += skip
            self.delete_remaining -= skip
        while self.cursor < len(self.planned) and self.planned[self.cursor][0] < first_frame + count:
            frame, kind, length = self.planned[self.cursor]
            if frame < first_frame + i:
                self.dropped.append({"frame": frame, "kind": kind, "length_samples": length, "reason": "inside a deleted run"})
                self.cursor += 1
                continue
            parts.append(block[i:frame - first_frame])
            i = frame - first_frame
            if kind == "duplicate":
                emitted = np.concatenate([self.history] + parts) if parts else self.history
                if len(emitted) < length:
                    self.dropped.append({"frame": frame, "kind": kind, "length_samples": length, "reason": "insufficient history"})
                else:
                    parts.append(emitted[-length:].copy())
                    self.applied.append({"frame": frame, "kind": kind, "length_samples": length})
            else:
                skip = min(length, count - i)
                i += skip
                self.delete_remaining = length - skip
                self.applied.append({"frame": frame, "kind": kind, "length_samples": length})
            self.cursor += 1
            if self.delete_remaining:
                break
        parts.append(block[i:])
        out = np.concatenate(parts) if len(parts) > 1 else parts[0]
        self.history = np.concatenate((self.history, out))[-self.max_length:]
        self.input_frames += count
        self.output_frames += len(out)
        return out

    def resolved(self):
        return {"type": self.type_name, "placement": self.placement, "planned_events": len(self.planned),
                "convention": "events in input-frame coordinates; delete removes [frame, frame+length); duplicate re-emits the last length emitted frames before frame"}

    def stats(self):
        return {"input_frames": self.input_frames, "output_frames": self.output_frames,
                "net_shift_samples": self.output_frames - self.input_frames,
                "applied_events": len(self.applied), "dropped_events": len(self.dropped)}

    def events(self):
        return [dict(e, at_seconds=e["frame"] / FS) for e in self.applied] + [dict(e, at_seconds=e["frame"] / FS) for e in self.dropped]
