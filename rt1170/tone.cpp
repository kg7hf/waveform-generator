// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "tone.hpp"
#include "platform/wm8960_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace waveform_generator
{

// One exact 48-frame period, quantized to a PCM24 peak of 1048576. The PCM table
// and phase advance are independent of callback size and scheduling jitter.
constexpr std::array<std::int32_t, 48U> tone_period
{
    0, 136867, 271391, 401273, 524288, 638333, 741455, 831891,
    908093, 968758, 1012847, 1039605, 1048576, 1039605, 1012847, 968758,
    908093, 831891, 741455, 638333, 524288, 401273, 271391, 136867,
    0, -136867, -271391, -401273, -524288, -638333, -741455, -831891,
    -908093, -968758, -1012847, -1039605, -1048576, -1039605, -1012847, -968758,
    -908093, -831891, -741455, -638333, -524288, -401273, -271391, -136867,
};
std::size_t tone_phase{};

void tone_hook(void*, const std::uint32_t*, std::uint32_t* playback,
               std::size_t frames) noexcept
{
    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        const auto word = static_cast<std::uint32_t>(tone_period[tone_phase]) << 8U;
        playback[2U * frame] = word;
        playback[2U * frame + 1U] = word;
        ++tone_phase;

        if (tone_phase == tone_period.size())
        {
            tone_phase = 0U;
        }
    }
}

m110::Status start_tone() noexcept
{
    auto& codec = m110::imxrt1170::wm8960_codec();
    m110::imxrt1170::CodecConfig configuration;
    configuration.frames_per_block = 128U;
    const auto configured = codec.configure(configuration, &tone_hook, nullptr);

    if (!configured.is_ok())
    {
        return configured;
    }

    const auto level = codec.set_transmit_level_percent(70U);

    if (!level)
    {
        return level.status();
    }

    return codec.start();
}

} // namespace waveform_generator
