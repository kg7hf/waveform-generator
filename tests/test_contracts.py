#!/usr/bin/env python3
"""Standard-library syntax, reference, recipe, and semantic contract checks."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re


def parse_finite_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError(f"non-finite JSON number {value!r}")
    return parsed


def load_json(path: Path) -> object:
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r} in {path}")
            result[key] = value
        return result

    def reject_constant(value: str):
        raise ValueError(f"non-finite JSON number {value!r} in {path}")

    def finite_float(value: str):
        try:
            return parse_finite_float(value)
        except ValueError as error:
            raise ValueError(f"{error} in {path}") from error

    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=unique_object,
                         parse_constant=reject_constant, parse_float=finite_float)


def resolve_json_pointer(document: object, pointer: str) -> object:
    if pointer == "":
        return document
    if not pointer.startswith("/"):
        raise ValueError(f"invalid JSON pointer fragment {pointer!r}")
    current = document
    for encoded in pointer[1:].split("/"):
        token = encoded.replace("~1", "/").replace("~0", "~")
        if isinstance(current, dict):
            if token not in current:
                raise ValueError(f"unresolved JSON pointer token {token!r}")
            current = current[token]
        elif isinstance(current, list):
            if not token.isdecimal() or int(token) >= len(current):
                raise ValueError(f"invalid JSON array pointer token {token!r}")
            current = current[int(token)]
        else:
            raise ValueError(f"JSON pointer descends through scalar at {token!r}")
    return current


def recipe_payload(recipe: dict) -> bytes:
    payload = recipe["preparation"]["payload"]
    domain = payload["domain_ascii"].encode("ascii") + b"\0"
    generated = bytearray()
    counter = payload["initial_counter"]
    while len(generated) < payload["bytes"]:
        generated.extend(hashlib.sha256(domain + counter.to_bytes(8, "little")).digest())
        counter += 1
    return bytes(generated[: payload["bytes"]])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)

    json_paths = sorted(
        [
            root / "dependencies.lock.json",
            root / "docs/source-imports.json",
            root / ".vscode/extensions.json",
            root / ".vscode/settings.json",
            root / "waveform-generator.code-workspace",
        ]
        + list((root / "schema").glob("*.json"))
        + list((root / "fixtures").glob("*.json"))
    )
    assert json_paths, "no contract JSON files found"
    documents = {path.relative_to(root).as_posix(): load_json(path) for path in json_paths}

    schemas = {name: value for name, value in documents.items() if name.startswith("schema/")}
    for name, schema in schemas.items():
        assert isinstance(schema, dict), name
        assert schema.get("$schema") == "https://json-schema.org/draft/2020-12/schema", name
        for ref in _refs(schema):
            resource, separator, fragment = ref.partition("#")
            if ":" in resource:
                raise AssertionError((name, "external $ref is not allowed", ref))
            if resource:
                target = (root / "schema" / resource).resolve()
                assert target.is_relative_to(root / "schema") and target.is_file(), (name, ref)
                referenced_document = load_json(target)
            else:
                referenced_document = schema
            if separator:
                resolve_json_pointer(referenced_document, fragment)

    try:
        json.loads('{"overflow":1e999}', parse_float=parse_finite_float)
    except ValueError:
        pass
    else:
        raise AssertionError("overflowing JSON float was accepted")

    workspace = documents["waveform-generator.code-workspace"]
    assert workspace["folders"] == [
        {"name": "RT1170 Waveform Generator", "path": "."}
    ]
    editor_settings = (
        documents[".vscode/settings.json"],
        workspace["settings"],
    )
    for settings in editor_settings:
        assert settings["cmake.sourceDirectory"] == "${workspaceFolder}"
        assert settings["cmake.useCMakePresets"] == "always"
    assert workspace["extensions"] == documents[".vscode/extensions.json"]

    recipes = {
        name: value for name, value in documents.items() if name.startswith("fixtures/")
    }
    assert set(recipes) == {
        "fixtures/clean-300L-1h.json",
        "fixtures/clean-300L-short.json",
        "fixtures/tone-10s.json",
    }
    for name, recipe in recipes.items():
        assert isinstance(recipe, dict), name
        assert recipe.get("schema_version") == "fixture-recipe/1", name
        assert recipe.get("status") == "recipe_not_generated", name
        fmt = recipe["format"]
        assert fmt == {
            "container": "RIFF/WAVE",
            "encoding": "pcm_s16le",
            "sample_rate_hz": 48000,
            "channels": 1,
            "bits_per_sample": 16,
        }, name
        assert not ({"wav", "pcm", "sidecar", "wav_sha256"} & recipe.keys()), name
        assert recipe["qualification"]["evidence_scope"] == "engineering_only", name
        assert recipe["level_policy"]["initial_setting_is_qualified"] is False, name

    tone = recipes["fixtures/tone-10s.json"]
    tone_prep = tone["preparation"]
    assert tone_prep["samples"] == 480000
    assert tone_prep["period_repetitions"] * len(tone_prep["pcm16_period"]) == 480000
    assert tone["format"]["sample_rate_hz"] / len(tone_prep["pcm16_period"]) == 1000
    expected_period = [round(4096 * math.sin(2 * math.pi * index / 48))
                       for index in range(48)]
    assert tone_prep["pcm16_period"] == expected_period

    hour = recipes["fixtures/clean-300L-1h.json"]
    short = recipes["fixtures/clean-300L-short.json"]
    for recipe, payload_bytes, dwell in ((short, 256, 2048 / 300),
                                         (hour, 135000, 3600)):
        preparation = recipe["preparation"]
        assert preparation["rate_bps"] == 300
        assert preparation["interleave"] == "long"
        assert preparation["transmission_count"] == 1
        assert preparation["payload"]["bytes"] == payload_bytes
        assert math.isclose(preparation["nominal_payload_dwell_seconds"], dwell,
                            rel_tol=0.0, abs_tol=1e-12)
        assert preparation["framing_policy"] == (
            "one_preamble_one_eom_no_loop_no_padding_to_fake_dwell")
        actual_payload = recipe_payload(recipe)
        assert hashlib.sha256(actual_payload).hexdigest() == preparation["payload"]["sha256"]
        assert recipe["qualification"]["minimum_payload_dwell_seconds"] == preparation["nominal_payload_dwell_seconds"]

    assert hour["qualification"]["class"] == "one_hour_m110_clean"
    assert hour["qualification"]["minimum_payload_dwell_seconds"] == 3600

    link_pattern = re.compile(r"\[[^]]+\]\((?![a-z]+:|#)([^)#]+)(?:#[^)]+)?\)")
    for markdown in sorted(list((root / "docs").glob("*.md"))
                           + list((root / "fixtures").glob("*.md"))
                           + [root / "README.md", root / "THIRD_PARTY_NOTICES.md"]):
        for target in link_pattern.findall(markdown.read_text(encoding="utf-8")):
            assert (markdown.parent / target).resolve().exists(), (markdown, target)

    return 0


def _refs(value: object):
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "$ref" and isinstance(item, str):
                yield item
            yield from _refs(item)
    elif isinstance(value, list):
        for item in value:
            yield from _refs(item)


if __name__ == "__main__":
    raise SystemExit(main())
