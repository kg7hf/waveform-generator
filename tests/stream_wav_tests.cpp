// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Exercise the offline adapter's lifecycle, including failed replacement and
// reads before validation. The single test owns its file in CTest's build directory.
#include "host/render/stream_wav.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>

namespace
{

namespace render = waveform_generator::render;

class TestFile
{
public:
    TestFile() = default;
    TestFile(const TestFile&) = delete;
    TestFile& operator=(const TestFile&) = delete;
    TestFile(TestFile&&) = delete;
    TestFile& operator=(TestFile&&) = delete;

    ~TestFile()
    {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(mPath, error));
    }

    [[nodiscard]] const std::string& path() const noexcept { return mPath; }

private:
    std::string mPath{"wfg-stream-wav-lifecycle.wav"};
};

void require_unready(render::WavSource& source)
{
    REQUIRE(source.frames() == 0U);
    std::array<float, 1U> samples{1.0F};
    REQUIRE(source.read(0U, samples).code == waveform_generator::wav_read_unready.code);
    REQUIRE(samples.front() == 1.0F);
    REQUIRE_FALSE(source.read(0U, {}).is_ok());
    REQUIRE(source.reference(signal_lab::Scenario{}).status().code == waveform_generator::wav_read_unready.code);
}

TEST_CASE("Offline WAV source enforces validated file ownership and bounded reads")
{
    TestFile file;
    constexpr std::array<float, 4U> expected{0.0F, 0.5F, -0.5F, 0.0F};
    render::WavSpool spool;
    REQUIRE_FALSE(spool.append(expected).is_ok());
    REQUIRE(spool.open().is_ok());
    REQUIRE_FALSE(spool.open().is_ok());
    REQUIRE(spool.append(expected).is_ok());
    REQUIRE(spool.publish(file.path()).is_ok());
    render::WavSource source;
    require_unready(source);
    source.close();
    require_unready(source);
    REQUIRE(source.open(file.path()).is_ok());
    REQUIRE(source.frames() == expected.size());
    SECTION("exact reads, end boundary and overflow leave the destination intact on refusal")
    {
        std::array<float, expected.size()> samples{};
        REQUIRE(source.read(0U, samples).is_ok());
        REQUIRE(samples == expected);
        REQUIRE(source.read(source.frames(), {}).is_ok());
        REQUIRE_FALSE(source.read(source.frames(), samples).is_ok());
        REQUIRE_FALSE(source.read(std::numeric_limits<std::uint64_t>::max(), samples).is_ok());
        REQUIRE(samples == expected);
        std::array<float, signal_lab::engine_block_frames + 1U> oversized{};
        REQUIRE_FALSE(source.read(0U, oversized).is_ok());
    }

    SECTION("a successful replacement starts a fresh reader")
    {
        REQUIRE(source.open(file.path()).is_ok());
        std::array<float, expected.size()> samples{};
        REQUIRE(source.read(0U, samples).is_ok());
        REQUIRE(samples == expected);
    }

    SECTION("an inaccessible replacement clears the old source and permits recovery")
    {
        REQUIRE_FALSE(source.open("").is_ok());
        require_unready(source);
        REQUIRE(source.open(file.path()).is_ok());
        REQUIRE(source.frames() == expected.size());
    }

    SECTION("malformed input releases the file and permits replacement")
    {
        source.close();
        {
            std::ofstream invalid{file.path(), std::ios::binary | std::ios::trunc};
            invalid << "not a WAV";
            invalid.close();
            REQUIRE_FALSE(invalid.fail());
        }

        REQUIRE_FALSE(source.open(file.path()).is_ok());
        require_unready(source);
        REQUIRE(spool.open().is_ok());
        REQUIRE(spool.append(expected).is_ok());
        REQUIRE(spool.publish(file.path()).is_ok());
        REQUIRE(source.open(file.path()).is_ok());
    }

    source.close();
    source.close();
    require_unready(source);
}

} // namespace
