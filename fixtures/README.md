# PCM24 fixture recipes

[tone-10s.json](tone-10s.json) describes a reproducible ten-second 1 kHz tone:
48 integer samples repeated 10,000 times, mono 48 kHz packed PCM24. Its peak code
is 1,048,576 (about -18.06 dBFS). The fixed period avoids platform sine/rounding
differences. The JSON is a reusable recipe, not an old test result.

```powershell
python host/generate_fixture.py fixtures/tone-10s.json build/fixtures/TONE.WAV
```

No encoder plugin is needed. Encoded fixtures belong with the module supplying
their producer. See [Encoder plugins](../docs/encoder-plugins.md) and
[How to use](../docs/how-to-use.md) for mixing interference and playing a file.
The recipe format is [fixture-recipe.schema.json](../schema/fixture-recipe.schema.json).
