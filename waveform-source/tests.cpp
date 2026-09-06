// This test target links only the generic writer, without any M110 adapter.
#include "waveform-source/wav_generator.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int failures{};
void check(bool value, const char* label)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}
class Bytes final : public waveform_source::ByteSource
{
public:
    std::size_t read(std::uint8_t* output, std::size_t count) noexcept override
    {
        constexpr std::array<std::uint8_t,3U> payload{'a','b','c'};
        count = std::min(count, payload.size() - cursor);
        std::copy_n(payload.data() + cursor, count, output);
        cursor += count;
        return count;
    }
    std::size_t cursor{};
};
class DifferentEncoder final : public waveform_source::Encoder
{
public:
    waveform_source::Status configure(std::string_view profile, waveform_source::ByteSource& payload, std::size_t bytes) noexcept override
    {
        cursor = 0U;
        std::uint8_t input[3]{};
        good = profile == "test:steps" && bytes == 3U && payload.read(input, 3U) == 3U;
        return status();
    }
    waveform_source::Status status() const noexcept override
    {
        return good ? waveform_source::Status::success() : waveform_source::Status{m110::StatusCode::invalid_configuration, "test encoder inactive"};
    }
    void stop() noexcept override { good = false; }
    std::uint64_t total_frames() const noexcept override { return values.size(); }
    std::size_t read(std::int16_t* output, std::size_t capacity) noexcept override
    {
        const auto count = std::min({capacity, values.size() - cursor, std::size_t{2U}});
        std::copy_n(values.data() + cursor, count, output);
        cursor += count;
        return count;
    }
    std::array<std::int16_t,5U> values{-32768,-1,0,1,32767};
    std::size_t cursor{};
    bool good{};
};
class Sink final : public waveform_source::ByteSink
{
public:
    bool write(const std::uint8_t* input, std::size_t size) noexcept override
    {
        bytes.insert(bytes.end(), input, input + size);
        return true;
    }
    std::vector<std::uint8_t> bytes;
};
}
int main()
{
    Bytes payload;
    DifferentEncoder encoder;
    Sink sink;
    waveform_source::WavGenerator writer;
    check(writer.begin(encoder, "test:steps", payload, 3U, sink, 3U).is_ok(), "generic configure");
    check(sink.bytes.size() == 44U && writer.total_frames() == 8U, "generic known WAV geometry");
    while (writer.state() == waveform_source::JobState::running) { (void)writer.step(3U); }
    check(writer.state() == waveform_source::JobState::complete, "different encoder completes without native adapter");
    constexpr std::array<std::uint8_t,16U> expected{0U,128U,255U,255U,0U,0U,1U,0U,255U,127U,0U,0U,0U,0U,0U,0U};
    check(sink.bytes.size() == 60U && std::equal(expected.begin(), expected.end(), sink.bytes.begin()+44), "generic writer preserves encoder PCM and appends only artifact padding");
    check(std::strcmp(writer.hashes().payload, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "generic payload hash includes configure-time reads");
    std::printf("generic waveform source tests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
