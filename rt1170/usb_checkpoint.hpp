#pragma once

#include "common/status.hpp"

namespace waveform_generator
{

// P1.1 owns only USB initialization and stack servicing. No WFG/1 command
// parser, WAV transfer, or modem/DTE service is present in this checkpoint.
[[nodiscard]] m110::Status start_usb_checkpoint() noexcept;

} // namespace waveform_generator
