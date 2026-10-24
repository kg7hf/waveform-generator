// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

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
    if (!value)
    {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

class Bytes final : public waveform_source::ByteSource
{
public:
    std::size_t read(std::uint8_t* output, std::size_t count) noexcept override
    {
        constexpr std::array<std::uint8_t, 3U> payload{'a', 'b', 'c'};
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

    m110::Result<std::size_t> read(std::int16_t* output, std::size_t capacity) noexcept override
    {
        const auto count = std::min({capacity, values.size() - cursor, std::size_t{2U}});
        std::copy_n(values.data() + cursor, count, output);
        cursor += count;
        return count;
    }

    std::array<std::int16_t, 5U> values{-32768, -1, 0, 1, 32767};
    std::size_t cursor{};
    bool good{};
};

class Sink final : public waveform_source::ByteSink
{
public:
    m110::Status write(const std::uint8_t* input, std::size_t size) noexcept override
    {
        bytes.insert(bytes.end(), input, input + size);
        return m110::Status::success();
    }

    std::vector<std::uint8_t> bytes;
};

// A sink whose write number `failing` (0: the header) fails with `code`, and
// every one after it.
class FailingSink final : public waveform_source::ByteSink
{
public:
    FailingSink(std::size_t failing, m110::StatusCode code) noexcept : failing_{failing}, code_{code} {}

    m110::Status write(const std::uint8_t*, std::size_t size) noexcept override
    {
        if (writes_++ >= failing_)
        {
            return {.code = code_, .message = "the test sink fails"};
        }

        bytes_ += size;
        return m110::Status::success();
    }

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    std::size_t failing_;
    m110::StatusCode code_;
    std::size_t writes_{};
    std::size_t bytes_{};
};

// The RIFF header the generator writes first, and a legacy PCM16 frame.
constexpr std::size_t wav_header_bytes = 44U;
constexpr std::size_t pcm16_frame_bytes = 2U;
// Whether `status` has `code` and the text `message`.
bool failed_with(waveform_source::Status status, m110::StatusCode code, const char* message)
{
    return status.code == code && std::strcmp(status.message, message) == 0;
}

// A sink's failure fails the job with the sink's own code and the
// generator's own message (the text the TX protocol reports, whatever the
// sink said), stops the encoder, and counts no frame it did not write. Each
// sink fails with another code, none of them the generator's io_error.
void test_sink_failures()
{
    constexpr std::size_t step_frames = 3U;
    {
        Bytes payload;
        DifferentEncoder encoder;
        FailingSink sink{0U, m110::StatusCode::buffer_too_small};
        waveform_source::WavGenerator writer;
        const auto begun = writer.begin(encoder, "test:steps", payload, 3U, sink, 3U, waveform_source::WavEncoding::pcm16_legacy);
        check(failed_with(begun, m110::StatusCode::buffer_too_small, "failed to write native WAV header") &&
                  failed_with(writer.status(), m110::StatusCode::buffer_too_small, "failed to write native WAV header") && writer.state() == waveform_source::JobState::failed && !encoder.good &&
                  sink.bytes() == 0U,
              "a failed header write fails begin() with the sink's code and the generator's message");
        check(writer.step(step_frames) == waveform_source::JobState::failed && writer.frames_written() == 0U, "a failed job takes no step");
    }

    {
        Bytes payload;
        DifferentEncoder encoder;
        FailingSink sink{1U, m110::StatusCode::unavailable};
        waveform_source::WavGenerator writer;
        check(writer.begin(encoder, "test:steps", payload, 3U, sink, 3U, waveform_source::WavEncoding::pcm16_legacy).is_ok(), "the header is written");
        check(writer.step(step_frames) == waveform_source::JobState::failed && failed_with(writer.status(), m110::StatusCode::unavailable, "failed to write native WAV PCM") &&
                  writer.frames_written() == 0U && !encoder.good && writer.hashes().wav[0] == '\0' && sink.bytes() == wav_header_bytes,
              "a failed first PCM write fails the job with the sink's code and counts no frame");
    }

    {
        Bytes payload;
        DifferentEncoder encoder;
        FailingSink sink{2U, m110::StatusCode::timeout};
        waveform_source::WavGenerator writer;
        check(writer.begin(encoder, "test:steps", payload, 3U, sink, 3U, waveform_source::WavEncoding::pcm16_legacy).is_ok(), "the header is written");
        check(writer.step(step_frames) == waveform_source::JobState::running, "the first PCM block is written");
        const auto written = writer.frames_written();
        check(writer.step(step_frames) == waveform_source::JobState::failed && failed_with(writer.status(), m110::StatusCode::timeout, "failed to write native WAV PCM") && written != 0U &&
                  writer.frames_written() == written && sink.bytes() == wav_header_bytes + pcm16_frame_bytes * written,
              "a failed later PCM write keeps the sink's code and the frames written before it");
    }
}
} // namespace

int main()
{
    Bytes payload;
    DifferentEncoder encoder;
    Sink sink;
    waveform_source::WavGenerator writer;
    check(writer.begin(encoder, "test:steps", payload, 3U, sink, 3U, waveform_source::WavEncoding::pcm16_legacy).is_ok(), "generic configure");
    check(sink.bytes.size() == 44U && writer.total_frames() == 8U, "generic known WAV geometry");

    while (writer.state() == waveform_source::JobState::running)
    {
        (void)writer.step(3U);
    }

    check(writer.state() == waveform_source::JobState::complete, "different encoder completes without native adapter");
    constexpr std::array<std::uint8_t, 16U> expected{0U, 128U, 255U, 255U, 0U, 0U, 1U, 0U, 255U, 127U, 0U, 0U, 0U, 0U, 0U, 0U};
    check(sink.bytes.size() == 60U && std::equal(expected.begin(), expected.end(), sink.bytes.begin() + 44), "generic writer preserves encoder PCM and appends only artifact padding");
    check(std::strcmp(writer.hashes().payload, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "generic payload hash includes configure-time reads");
    test_sink_failures();
    std::printf("generic waveform source tests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
