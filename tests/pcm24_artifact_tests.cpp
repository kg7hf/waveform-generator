// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/pcm24_artifact.hpp"
#include "common/uint64_text.hpp"
#include "common/wav.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace
{

namespace artifact = waveform_generator::artifact;
inline constexpr m110::Status sink_io_error{.code = m110::StatusCode::io_error, .message = "test sink failure"};

class MemorySink final : public artifact::Pcm24ByteSink
{
public:
    [[nodiscard]] m110::Status write_exact(std::span<const std::uint8_t> bytes) noexcept override
    {
        ++writes;

        if (fail_write == writes || position > storage.size())
        {
            return sink_io_error;
        }

        if (bytes.size() > storage.size() - position)
        {
            storage.resize(position + bytes.size());
        }

        std::ranges::copy(bytes, storage.begin() + static_cast<std::ptrdiff_t>(position));
        position += bytes.size();
        return m110::Status::success();
    }

    [[nodiscard]] m110::Status seek(std::uint64_t offset) noexcept override
    {
        ++seeks;

        if (fail_seek == seeks || offset > std::numeric_limits<std::size_t>::max() || offset > storage.size())
        {
            return sink_io_error;
        }

        position = static_cast<std::size_t>(offset);
        return m110::Status::success();
    }

    std::vector<std::uint8_t> storage{};
    std::size_t position{};
    std::size_t writes{};
    std::size_t seeks{};
    std::size_t fail_write{};
    std::size_t fail_seek{};
};

void require_header(const MemorySink& sink, std::uint32_t sample_rate, std::uint32_t channels, std::uint32_t frames, std::uint32_t data_bytes)
{
    const auto& bytes = sink.storage;
    REQUIRE(bytes.size() == 44U + data_bytes + (data_bytes & 1U));
    REQUIRE(bytes[0] == 'R');
    REQUIRE(bytes[1] == 'I');
    REQUIRE(bytes[2] == 'F');
    REQUIRE(bytes[3] == 'F');
    REQUIRE(waveform_generator::wav_read_le32(bytes.data() + 4U) == 36U + data_bytes + (data_bytes & 1U));
    REQUIRE(bytes[8] == 'W');
    REQUIRE(bytes[9] == 'A');
    REQUIRE(bytes[10] == 'V');
    REQUIRE(bytes[11] == 'E');
    REQUIRE(waveform_generator::wav_read_le16(bytes.data() + 20U) == 1U);
    REQUIRE(waveform_generator::wav_read_le16(bytes.data() + 22U) == channels);
    REQUIRE(waveform_generator::wav_read_le32(bytes.data() + 24U) == sample_rate);
    REQUIRE(waveform_generator::wav_read_le32(bytes.data() + 28U) == channels * sample_rate * 3U);
    REQUIRE(waveform_generator::wav_read_le16(bytes.data() + 32U) == channels * 3U);
    REQUIRE(waveform_generator::wav_read_le16(bytes.data() + 34U) == 24U);
    REQUIRE(waveform_generator::wav_read_le32(bytes.data() + 40U) == data_bytes);
    REQUIRE(frames * channels * 3U == data_bytes);
}

TEST_CASE("PCM24 artifact writes a canonical mono WAV and hashes only payload bytes")
{
    MemorySink sink;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    constexpr std::array<std::uint8_t, 9U> pcm{0U, 0U, 0U, 0xffU, 0xffU, 0xffU, 1U, 2U, 3U};
    REQUIRE(writer.append_frames(std::span{pcm}.first(3U)).is_ok());
    REQUIRE(writer.append_frames(std::span{pcm}.subspan(3U)).is_ok());
    REQUIRE(writer.frames() == 3U);
    REQUIRE(sink.storage.size() == 44U + pcm.size());
    artifact::Pcm24ArtifactSummary summary{};
    REQUIRE(writer.finish(summary).is_ok());
    REQUIRE(writer.state() == artifact::Pcm24ArtifactState::complete);
    REQUIRE(summary.frames == 3U);
    REQUIRE(summary.data_bytes == pcm.size());
    REQUIRE(summary.file_bytes == 44U + pcm.size() + 1U);
    REQUIRE(std::string_view{summary.pcm_sha256.data()} == "d86b4868d967b3b7049c9361000a6f24674523df6dd4b29454d5316addf96fa9");
    require_header(sink, 48000U, 1U, 3U, 9U);
    REQUIRE(std::ranges::equal(std::span{sink.storage}.subspan(44U, pcm.size()), pcm));
    REQUIRE(sink.storage.back() == 0U);
    REQUIRE(sink.position == summary.file_bytes);
}

TEST_CASE("PCM24 artifact writes interleaved stereo without changing sample bytes")
{
    MemorySink sink;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(sink, {.sample_rate_hz = 44100U, .channels = 2U}).is_ok());
    constexpr std::array<std::uint8_t, 12U> pcm{0U, 0U, 0x80U, 0xffU, 0xffU, 0x7fU, 0x12U, 0x34U, 0x56U, 0xabu, 0xcdu, 0xefU};
    REQUIRE(writer.append_frames(pcm).is_ok());
    artifact::Pcm24ArtifactSummary summary{};
    REQUIRE(writer.finish(summary).is_ok());
    REQUIRE(summary.frames == 2U);
    REQUIRE(summary.data_bytes == pcm.size());
    REQUIRE(summary.file_bytes == 44U + pcm.size());
    require_header(sink, 44100U, 2U, 2U, 12U);
    REQUIRE(waveform_generator::wav_read_le32(sink.storage.data() + 24U) == 44100U);
    REQUIRE(waveform_generator::wav_read_le32(sink.storage.data() + 28U) == 264600U);
    REQUIRE(std::ranges::equal(std::span{sink.storage}.subspan(44U), pcm));
}

TEST_CASE("PCM24 artifact supports an empty final WAV")
{
    MemorySink sink;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    artifact::Pcm24ArtifactSummary summary{};
    REQUIRE(writer.finish(summary).is_ok());
    REQUIRE(summary.frames == 0U);
    REQUIRE(summary.data_bytes == 0U);
    REQUIRE(summary.file_bytes == 44U);
    require_header(sink, 48000U, 1U, 0U, 0U);
    REQUIRE(std::string_view{summary.pcm_sha256.data()} == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("PCM24 artifact refuses invalid formats, incomplete frames, and oversized append calls")
{
    SECTION("invalid format does not write")
    {
        MemorySink sink;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE_FALSE(writer.begin(sink, {.sample_rate_hz = 0U, .channels = 1U}).is_ok());
        REQUIRE(sink.storage.empty());
        REQUIRE(sink.writes == 0U);
        REQUIRE_FALSE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 3U}).is_ok());
        REQUIRE(sink.storage.empty());
    }

    SECTION("partial frame latches failure")
    {
        MemorySink sink;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        const std::array<std::uint8_t, 4U> partial_frame{};
        REQUIRE_FALSE(writer.append_frames(partial_frame).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(sink.storage.size() == artifact::pcm24_wav_header_bytes);
        artifact::Pcm24ArtifactSummary summary{};
        REQUIRE_FALSE(writer.finish(summary).is_ok());
        REQUIRE(summary.file_bytes == 0U);
    }

    SECTION("append cap is tested with a fresh active writer")
    {
        MemorySink sink;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        std::vector<std::uint8_t> too_many((artifact::pcm24_artifact_max_append_frames + 1U) * 3U);
        REQUIRE_FALSE(writer.append_frames(too_many).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(sink.storage.size() == artifact::pcm24_wav_header_bytes);
        REQUIRE(writer.frames() == 0U);
    }
}

TEST_CASE("PCM24 artifact refuses an active restart, and cancellation prevents further writes")
{
    MemorySink first_sink;
    MemorySink second_sink;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(first_sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    REQUIRE_FALSE(writer.begin(second_sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    REQUIRE(second_sink.storage.empty());
    writer.cancel();
    REQUIRE(writer.state() == artifact::Pcm24ArtifactState::cancelled);
    const std::array<std::uint8_t, 3U> frame{};
    REQUIRE_FALSE(writer.append_frames(frame).is_ok());
    artifact::Pcm24ArtifactSummary summary{};
    REQUIRE_FALSE(writer.finish(summary).is_ok());
}

TEST_CASE("PCM24 artifact latches sink failures and publishes no completion summary")
{
    MemorySink sink;
    sink.fail_write = 2U;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    constexpr std::array<std::uint8_t, 3U> frame{1U, 2U, 3U};
    const auto failed = writer.append_frames(frame);
    REQUIRE_FALSE(failed.is_ok());
    REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
    REQUIRE(writer.status().code == failed.code);
    artifact::Pcm24ArtifactSummary summary{.frames = 99U, .data_bytes = 99U, .file_bytes = 99U};
    REQUIRE_FALSE(writer.finish(summary).is_ok());
    REQUIRE(summary.frames == 99U);
    REQUIRE(summary.data_bytes == 99U);
    REQUIRE(summary.file_bytes == 99U);
}

TEST_CASE("PCM24 artifact refuses completion when header patching or final seeking fails")
{
    SECTION("initial seek fails")
    {
        MemorySink sink;
        sink.fail_seek = 1U;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE_FALSE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(sink.storage.empty());
    }

    SECTION("initial placeholder header write fails")
    {
        MemorySink sink;
        sink.fail_write = 1U;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE_FALSE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(sink.storage.empty());
    }

    SECTION("padding write fails")
    {
        MemorySink sink;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        constexpr std::array<std::uint8_t, 3U> pcm{1U, 2U, 3U};
        REQUIRE(writer.append_frames(pcm).is_ok());
        sink.fail_write = 3U;
        artifact::Pcm24ArtifactSummary summary{};
        REQUIRE_FALSE(writer.finish(summary).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(summary.frames == 0U);
    }

    SECTION("header seek fails")
    {
        MemorySink sink;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        constexpr std::array<std::uint8_t, 6U> pcm{1U, 2U, 3U, 4U, 5U, 6U};
        REQUIRE(writer.append_frames(pcm).is_ok());
        sink.fail_seek = 2U;
        artifact::Pcm24ArtifactSummary summary{.frames = 7U, .data_bytes = 7U, .file_bytes = 7U};
        REQUIRE_FALSE(writer.finish(summary).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(summary.frames == 7U);
        REQUIRE(summary.pcm_sha256[0] == '\0');
    }

    SECTION("header write fails")
    {
        MemorySink sink;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        constexpr std::array<std::uint8_t, 6U> pcm{1U, 2U, 3U, 4U, 5U, 6U};
        REQUIRE(writer.append_frames(pcm).is_ok());
        sink.fail_write = 3U;
        artifact::Pcm24ArtifactSummary summary{};
        REQUIRE_FALSE(writer.finish(summary).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(summary.frames == 0U);
    }

    SECTION("final seek fails after header patch")
    {
        MemorySink sink;
        artifact::Pcm24ArtifactWriter writer;
        REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
        constexpr std::array<std::uint8_t, 6U> pcm{1U, 2U, 3U, 4U, 5U, 6U};
        REQUIRE(writer.append_frames(pcm).is_ok());
        sink.fail_seek = 3U;
        artifact::Pcm24ArtifactSummary summary{};
        REQUIRE_FALSE(writer.finish(summary).is_ok());
        REQUIRE(writer.state() == artifact::Pcm24ArtifactState::failed);
        REQUIRE(summary.frames == 0U);
    }
}

} // namespace

TEST_CASE("Embedded integer text retains all digits and terminates", "[artifact][format]")
{
    using waveform_generator::uint64_text;
    CHECK(std::string_view{uint64_text(0U).data()} == "0");
    CHECK(std::string_view{uint64_text(4294967296ULL).data()} == "4294967296");
    CHECK(std::string_view{uint64_text(std::numeric_limits<std::uint64_t>::max()).data()} == "18446744073709551615");
    CHECK(std::string_view{uint64_text<16U, 16U>(0U).data()} == "0000000000000000");
    CHECK(std::string_view{uint64_text<16U, 16U>(0xa121f18f841c53deULL).data()} == "a121f18f841c53de");
    CHECK(std::string_view{uint64_text<16U, 16U>(std::numeric_limits<std::uint64_t>::max()).data()} == "ffffffffffffffff");
}

TEST_CASE("Embedded integer padding preserves digits and bounded termination", "[artifact][format]")
{
    using waveform_generator::uint64_text;
    constexpr auto decimal = uint64_text<10U, 20U>(42U);
    constexpr auto hexadecimal = uint64_text<16U, 4U>(0xabU);
    constexpr auto wider_than_minimum = uint64_text<10U, 2U>(12345U);
    INFO("decimal=" << decimal.data() << " hexadecimal=" << hexadecimal.data() << " wider_than_minimum=" << wider_than_minimum.data());
    CHECK(std::string_view{decimal.data()} == "00000000000000000042");
    CHECK(decimal.back() == '\0');
    CHECK(std::string_view{hexadecimal.data()} == "00ab");
    CHECK(std::ranges::all_of(std::span{hexadecimal}.subspan(4U), [](char byte) { return byte == '\0'; }));

    CHECK(std::string_view{wider_than_minimum.data()} == "12345");
    CHECK(std::ranges::all_of(std::span{wider_than_minimum}.subspan(5U), [](char byte) { return byte == '\0'; }));
}
