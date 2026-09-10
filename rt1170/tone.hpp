#pragma once

#include "common/status.hpp"

namespace waveform_generator
{

// Nominal 1 kHz deterministic PCM24 checkpoint. This is not the finite WAV
// player or a qualified source: P1.4 must verify physical format and timing.
[[nodiscard]] m110::Status start_tone_checkpoint() noexcept;

} // namespace waveform_generator
