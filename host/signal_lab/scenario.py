# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Portable scenario description (shared by the Python and C++ engines).

{
  "schema": "signal-lab.scenario/1",
  "test_id": "m110_600L_mix_MIX003_seed0042",
  "seed": 60042,
  "source": {"path": "m110_600L_reference_perfect.wav"},
  "reference": {"start_seconds": 0.0, "duration_seconds": 116.2} | "auto",
  "source_gain_db": -12.0,
  "clip_policy": "saturate" | "reject",
  "impairments": [{"type": "fade", ...}, {"type": "awgn", ...}, {"type": "cw", ...}, ...]
}

Impairments are applied in list order.  The physically meaningful order is
fade (propagation) -> awgn (channel noise) -> cw / impulse (at the receiver)
-> sample_slip (digital transport), and sample_slip must be last.
"""

import hashlib
import json

from . import SCENARIO_SCHEMA
from .awgn import Awgn
from .cw import Cw
from .fade import Fade
from .impulse import Impulse
from .sample_slip import SampleSlip
from .stream import Pipeline, number

REGISTRY = {cls.type_name: cls for cls in (Awgn, Cw, Impulse, Fade, SampleSlip)}
CANONICAL_ORDER = ["fade", "awgn", "cw", "impulse", "sample_slip"]
CLIP_POLICIES = ("saturate", "reject")


class ScenarioError(ValueError):
    pass


def canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def scenario_sha256(scenario):
    return hashlib.sha256(canonical_json(scenario).encode("utf-8")).hexdigest()


def validate(scenario):
    """Return a normalised copy (defaults filled) or raise ScenarioError."""
    if not isinstance(scenario, dict):
        raise ScenarioError("scenario must be an object")
    allowed = {"schema", "test_id", "description", "seed", "source", "reference", "source_gain_db", "clip_policy", "impairments", "tags"}
    unknown = set(scenario) - allowed
    if unknown:
        raise ScenarioError("unknown scenario keys: %s" % sorted(unknown))
    if scenario.get("schema", SCENARIO_SCHEMA) != SCENARIO_SCHEMA:
        raise ScenarioError("schema must be " + SCENARIO_SCHEMA)
    out = {"schema": SCENARIO_SCHEMA}
    test_id = scenario.get("test_id")
    if not isinstance(test_id, str) or not test_id:
        raise ScenarioError("test_id must be a non-empty string")
    out["test_id"] = test_id
    if "description" in scenario:
        out["description"] = str(scenario["description"])
    if "tags" in scenario:
        out["tags"] = dict(scenario["tags"])
    seed = scenario.get("seed", 0)
    if isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed < 2 ** 63:
        raise ScenarioError("seed must be an integer in [0, 2^63)")
    out["seed"] = seed
    source = scenario.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("path"), str):
        raise ScenarioError("source.path must be a string")
    out["source"] = {"path": source["path"]}
    reference = scenario.get("reference", "auto")
    if reference != "auto":
        if not isinstance(reference, dict) or set(reference) != {"start_seconds", "duration_seconds"}:
            raise ScenarioError("reference must be 'auto' or {start_seconds, duration_seconds}")
        out["reference"] = {"start_seconds": number(reference["start_seconds"], "reference.start_seconds", 0),
                            "duration_seconds": number(reference["duration_seconds"], "reference.duration_seconds", 1e-3)}
    else:
        out["reference"] = "auto"
    out["source_gain_db"] = number(scenario.get("source_gain_db", -12.0), "source_gain_db", -120, 60)
    policy = scenario.get("clip_policy", "saturate")
    if policy not in CLIP_POLICIES:
        raise ScenarioError("clip_policy must be one of %s" % (CLIP_POLICIES,))
    out["clip_policy"] = policy
    impairments = scenario.get("impairments", [])
    if not isinstance(impairments, list):
        raise ScenarioError("impairments must be a list")
    normalised = []
    for index, item in enumerate(impairments):
        if not isinstance(item, dict) or item.get("type") not in REGISTRY:
            raise ScenarioError("impairments[%d].type must be one of %s" % (index, sorted(REGISTRY)))
        normalised.append(dict(item))
    for index, item in enumerate(normalised[:-1]):
        if item["type"] == "sample_slip":
            raise ScenarioError("sample_slip must be the last impairment (it shifts the timeline)")
    out["impairments"] = normalised
    return out


def build_pipeline(scenario):
    stages = []
    for item in scenario["impairments"]:
        params = {key: value for key, value in item.items() if key != "type"}
        stages.append(REGISTRY[item["type"]](params))
    return Pipeline(stages)
