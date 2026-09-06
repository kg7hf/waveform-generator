#include "host/capture/capture_core.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

int main()
{
    waveform_capture::BoundedSampleQueue<2U> queue;
    const std::array<float, 3> first{1.0F, -0.5F, 0.0F};
    const std::array<float, 2> second{0.25F, -0.25F};
    if (!queue.push(first) || !queue.push(second) || queue.push(first) || queue.size() != 2U)
    {
        return 1;
    }

    waveform_capture::SampleBlock block{};
    if (!queue.pop(block) || block.count != first.size() || block.samples[0] != first[0] ||
        !queue.pop(block) || block.count != second.size() || queue.pop(block))
    {
        return 2;
    }

    waveform_capture::CaptureMeasurements measurements;
    const std::array<float, 5> samples{0.5F, -1.0F, 0.25F, NAN, 0.0F};
    measurements.observe(samples);
    if (measurements.samples != 5U || measurements.finite_samples != 4U ||
        measurements.nonfinite_samples != 1U || measurements.clipped_samples != 1U ||
        measurements.peak != 1.0F || std::abs(measurements.rms() - std::sqrt(1.3125 / 4.0)) > 1.0e-6)
    {
        return 3;
    }

    const std::array<std::int16_t, 6> stereo{-32768, -32768, 32767, 32767, -32768, 32767};
    std::array<float, 3> mono{};
    waveform_capture::downmix_pcm16_stereo_to_float(stereo, mono, waveform_capture::ChannelPolicy::average);
    if (mono[0] != -1.0F || std::abs(mono[1] - (32767.0F / 32768.0F)) > 1.0e-7F ||
        std::abs(mono[2] + (1.0F / 65536.0F)) > 1.0e-7F)
    {
        return 4;
    }
    const std::array<std::int16_t, 4> opposite_polarity{8192, -8192, -16384, -8192};
    std::array<float, 2> selected{};
    waveform_capture::downmix_pcm16_stereo_to_float(opposite_polarity, selected, waveform_capture::ChannelPolicy::left);
    if (std::abs(selected[0] - 0.25F) > 1.0e-7F || std::abs(selected[1] + 0.5F) > 1.0e-7F)
    {
        return 5;
    }
    waveform_capture::downmix_pcm16_stereo_to_float(opposite_polarity, selected, waveform_capture::ChannelPolicy::right);
    if (std::abs(selected[0] + 0.25F) > 1.0e-7F || std::abs(selected[1] + 0.25F) > 1.0e-7F)
    {
        return 6;
    }
    waveform_capture::downmix_pcm16_stereo_to_float(opposite_polarity, selected, waveform_capture::ChannelPolicy::average);
    if (std::abs(selected[0]) > 1.0e-7F || std::abs(selected[1] + 0.375F) > 1.0e-7F)
    {
        return 7;
    }
    if (waveform_capture::select_channels_to_mono(0.25F, -0.25F, waveform_capture::ChannelPolicy::left) != 0.25F ||
        waveform_capture::select_channels_to_mono(0.25F, -0.25F, waveform_capture::ChannelPolicy::right) != -0.25F ||
        waveform_capture::select_channels_to_mono(0.25F, -0.25F, waveform_capture::ChannelPolicy::average) != 0.0F)
    {
        return 8;
    }
    if (waveform_capture::pcm16_stereo_frame_count(7U, 480U) != 1U ||
        waveform_capture::pcm16_stereo_frame_count(9600U, 480U) != 480U)
    {
        return 9;
    }
    return 0;
}
