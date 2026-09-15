#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Generate a PCM24 WAV from a local fixture recipe."""

import argparse
import json
from pathlib import Path

from stage_wav import _write_tone_from_recipe


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recipe', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.suffix.lower() != '.wav':
        parser.error('output must have a .wav extension')
    for path in (output, output.with_suffix('.BIN'), output.with_suffix('.JSON')):
        if path.exists():
            parser.error(f'output already exists: {path}')
    recipe = json.loads(args.recipe.read_text(encoding='utf-8'))
    if recipe.get('schema_version') != 'fixture-recipe/1' or recipe.get('format') != {
        'container': 'RIFF/WAVE', 'encoding': 'pcm_s24le', 'sample_rate_hz': 48000,
        'channels': 1, 'bits_per_sample': 24,
    }:
        parser.error('expected a PCM24 fixture-recipe/1 document')
    preparation = recipe['preparation']
    if preparation['kind'] == 'local_integer_period':
        output.parent.mkdir(parents=True, exist_ok=True)
        print(json.dumps(_write_tone_from_recipe(args.recipe, output, False), indent=2))
        return
    parser.error('unsupported recipe preparation; use the supplying plugin for encoded fixtures')


if __name__ == '__main__':
    main()
