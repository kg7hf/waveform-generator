#include "native-m110/wav_generator.hpp"
#include <cstdio>
namespace native_m110
{
m110::Status WavGenerator::begin(Source& source, m110::BodyMode mode, native_m110::ByteSource& payload,
                                 std::size_t payload_bytes, ByteSink& sink, std::uint32_t tail) noexcept
{
    char profile[32]{};
    const char* interleave = mode.interleave == m110::BodyInterleave::long_block ? "long" :
                             mode.interleave == m110::BodyInterleave::short_block ? "short" : "zero";
    std::snprintf(profile, sizeof(profile), "%u:%s", static_cast<unsigned>(mode.data_rate), interleave);
    return waveform_source::WavGenerator::begin(source, profile, payload, payload_bytes, sink, tail);
}
}
