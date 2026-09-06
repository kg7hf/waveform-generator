#pragma once

#include "signal_lab/live_control.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace waveform_generator
{

struct LiveReplay
{
    std::uint64_t seed{};
    double reference_rms{};
    bool has_frames{};
    std::uint64_t frames{};
    // Stable output-frame order; controls sharing a frame retain recorded order.
    std::vector<signal_lab::ControlEvent> events;
};

// Read an exported wfg-live.replay/1 plan. Metadata and controls are checked
// before rendering; uint64 seed/frame tokens are parsed without a double roundtrip.
bool read_live_replay(const std::string& path, LiveReplay& replay, std::string& error);

} // namespace waveform_generator
