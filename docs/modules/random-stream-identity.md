# Stable random-stream identity

Signal-lab scenarios may assign an optional positive 32-bit `stream_id` to an impairment. IDs must be unique within the scenario, including IDs attached to stages that currently draw no random values. This reserves identity consistently if a stage later gains a stochastic component. Booleans, fractional values, zero, negative values, and values above `2^32-1` are invalid.

An identified stage derives its effective seed from the scenario seed and ID
using SplitMix64-next version 1. Each arithmetic operation below uses unsigned
64-bit values, with addition and multiplication wrapping modulo `2^64`:

```text
x = master_seed XOR 0x5746475354524D31 XOR stream_id
x = x + 0x9E3779B97F4A7C15
x = (x XOR (x >> 30)) * 0xBF58476D1CE4E5B9
x = (x XOR (x >> 27)) * 0x94D049BB133111EB
effective_seed = x XOR (x >> 31)
```

The stage uses effective RNG index zero. Existing RNG family and event/path
substream calculations remain in place. For a fixed master seed, distinct IDs
produce distinct effective seeds; this is not a collision guarantee across
different master seeds, RNG families or legacy streams. Seed 17 and ID 101
produce effective seed `d06b33f8173bcb28`.

Stages without `stream_id` keep the existing seed and stage-index behavior exactly. The Python AWGN stage also retains its historical per-AWGN instance index when no ID is provided, preserving `pcm_stress` compatibility. A stage’s array order remains separately recorded as `order`; its random identity does not change placement or processing order.

Stable streams are opt-in. Explicitly identified stages retain their random realizations when other stages are inserted or reordered. Untagged stages remain order-dependent, so a scenario only gains full insertion stability when every stochastic stage has an ID. Inserting an impairment still changes the resulting combined signal. The guarantee concerns each identified stage’s own random component, not the final mixed waveform. Python uses NumPy PCG64, so this identity contract does not make Python random samples bit-identical to the embedded PCG32 implementation.

For example, `{"type":"awgn","stream_id":102,"snr_db":24}` selects a stable
AWGN realization. The [combined fixture](../../fixtures/scenarios/stable-streams.json)
assigns independent IDs to Watterson, AWGN and a faded, Markov-gated random FSK
interferer. The firmware advertises `stream-id` in `INFO` capabilities. Both C++
renderers and the Python renderer add a stage-level `random_key` object only
when an ID is present:

```json
{"mode":"stream-id-v1","stream_id":101,"effective_seed_hex":"d06b33f8173bcb28","index":0,"algorithm_version":1}
```

Host tests pin independent seed vectors and compare actual stochastic output,
including Watterson after its filter latency, across changed stage ordinals.
Both C++ renderers test inserted inactive stages, partition sizes 1/127/2048,
scenario replay, invalid IDs and unchanged Watterson parameter digests. All
four player mocks cover identified Watterson/noise/interference with post-engine
CUTs. These checks do not establish physical board parity or reset acceptance.
