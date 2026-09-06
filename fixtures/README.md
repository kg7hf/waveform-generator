# Phase 1 fixture recipes

These JSON documents are preparation recipes. No WAV, SD package, capture, or
qualification result is implied by their presence. They validate against
[fixture-recipe.schema.json](../schema/fixture-recipe.schema.json), not the
[frozen manifest schema](../schema/waveform-manifest.schema.json).

| Recipe | Preparation | Intended first use |
| --- | --- | --- |
| [tone-10s.json](tone-10s.json) | Repeat the supplied 48 signed PCM16 codes 10,000 times into one WAV. | 10-second 1-kHz direct tone and separate HFSimulator clean pass-through calibration. |
| [clean-300L-short.json](clean-300L-short.json) | Supply the specified 256-byte payload to an explicitly selected external 300L transmitter. | Short complete analog capture/decode and repeat/re-arm checks. |
| [clean-300L-1h.json](clean-300L-1h.json) | Supply the specified 135,000-byte payload to an explicitly selected external 300L transmitter. | One continuous clean transmission with 3600 seconds of nominal payload dwell. |

All files use 48,000 Hz, one channel, signed little-endian PCM16. The player reads
each file once. The tone's periodic sample content is intentional; firmware
file looping is not part of the recipe. Its peak code is 4096, approximately
-18.06 dBFS against the PCM16 full-scale magnitude 32768. The frozen integer
period avoids dependence on a platform's sine function or rounding convention.

For the two M110 recipes, concatenate SHA-256 digests of:

    ASCII(domain_ascii) || 0x00 || uint64_little_endian(counter)

Start the counter at zero and increment it once per 32-byte digest. Keep the
first payload.bytes bytes. The recipe gives the SHA-256 of those expected
payload bytes. This is an engineering pattern, not a claim to implement the
governed 2047-bit test pattern. Octets retain their generated order; the declared
M110 transmitter serializes bits least-significant-bit first within each octet.

The short payload hash is
6fe022b1ba676e9a3bdc9fe38b86923a3bcfcccda080ea88326f5bd6d0e399d4.
The one-hour payload hash is
2de0370aa20d399e51570c75a9a79cecb4774ed8f25566bde48f6f11ce28a1f7.
These identify recipe payloads only; they are not source-WAV hashes.

The external producer invocation contract is:

    <explicit-transmitter-executable> 300 long <output.wav> --file <payload.bin>

Record the exact executable hash and arguments. The generator neither builds
this executable nor searches a parent repository for it. The supplied producer
must provide retained transmission-plan evidence sufficient to establish actual
sample count, preamble/body/EOM coverage, padding, and trailing silence. If its
output does not establish those facts, preparation remains incomplete pending
an external producer enhancement or an independently retained plan calculation.

For 135,000 octets, 135000 * 8 / 300 = 3600 seconds of nominal payload dwell.
The WAV is longer once preamble, EOM/flush, frame rounding, and declared trailing
silence are included. Do not truncate the WAV to 172,800,000 samples or extend a
short transmission with silence to claim one hour. Short-recipe nominal payload
dwell is the exact fraction 2048/300 seconds; its JSON decimal is informational.

Create a finalized package manifest only after producing and inspecting the
actual WAV. Record its complete file hash, PCM-region hash/offset/count, WGM
hash, actual expected payload, producer identity, and source interval evidence.
Preserve the recipe alongside that manifest. Finalized manifests carry no fake
hash placeholders. Validate exact clean digital decoding and resource use
before scheduling the analog run.

The initial codec value 70 is only a calibration candidate. Recipe files do not
assert that it is safe or qualified for a particular load/input setting. Freeze
the measured codec/capture configuration and audio/drift acceptance criteria
before the intermediate and one-hour gates. See the local
[implementation contract](../docs/phase1-implementation.md).
