#pragma once
#include "native-m110/source.hpp"
#include "native-m110/sha256.hpp"
#include "waveform-source/wav_generator.hpp"
namespace native_m110
{
inline constexpr const char* generator_version = "wfg-native-m110/0.2.0";
using ByteSink = waveform_source::ByteSink;
using JobState = waveform_source::JobState;
using ArtifactHashes = waveform_source::ArtifactHashes;
class WavGenerator final : public waveform_source::WavGenerator
{
public:
    using waveform_source::WavGenerator::begin;
    [[nodiscard]] m110::Status begin(Source& source, m110::BodyMode mode, native_m110::ByteSource& payload,
                                    std::size_t payload_bytes, ByteSink& sink, std::uint32_t tail = 48000U) noexcept;
};
}
