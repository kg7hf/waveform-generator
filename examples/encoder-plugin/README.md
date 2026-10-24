# Example waveform encoder

This optional module registers `TONE`, a bounded calibration-tone producer.
It is excluded from production builds unless explicitly selected. The public
test suite exercises it independently of any separately supplied encoder.

See [Encoder plugins](../../docs/encoder-plugins.md) for complete build commands,
the wire command, profile rules, workspace ownership, and an example calculation.
Copy this module into your own repository and replace the adapter implementation
and descriptor to add another waveform family.

The [custom module tutorial](../../docs/custom-encoder-tutorial.md) shows how to
copy this adapter, define a profile and extend its CDC control workflow.
