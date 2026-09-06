#include "tone.hpp"
#include "platform/wm8960_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace waveform_generator
{

// One exact 48-frame period, quantized to a PCM16 peak of 4096. The PCM table
// and phase advance are independent of callback size and scheduling jitter.
constexpr std::array<std::int16_t, 48U> tone_period{
    0, 535, 1060, 1567, 2048, 2493, 2896, 3250,
    3547, 3784, 3956, 4061, 4096, 4061, 3956, 3784,
    3547, 3250, 2896, 2493, 2048, 1567, 1060, 535,
    0, -535, -1060, -1567, -2048, -2493, -2896, -3250,
    -3547, -3784, -3956, -4061, -4096, -4061, -3956, -3784,
    -3547, -3250, -2896, -2493, -2048, -1567, -1060, -535,
};
std::size_t tone_phase{};

void tone_hook(void*, const std::uint32_t*, std::uint32_t* playback,
               std::size_t frames) noexcept
{
    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        const auto signed_sample = static_cast<std::int32_t>(tone_period[tone_phase]);
        const auto word = static_cast<std::uint32_t>(signed_sample) << 16U;
        playback[2U * frame] = word;
        playback[2U * frame + 1U] = word;
        ++tone_phase;
        if (tone_phase == tone_period.size())
        {
            tone_phase = 0U;
        }
    }
}

m110::Status start_tone_checkpoint() noexcept
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
