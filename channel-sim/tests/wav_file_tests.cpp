// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The host CLI's WAV reader on well-formed and malformed files (guide:
// docs/modules/pathsim.md). Each case writes a small file into the working
// directory (ctest: the build directory), opens it with WavFileSource, closes it and
// removes the file.
//
//  - an odd-sized unknown chunk is skipped with its pad byte;
//  - a chunk whose 32-bit size is 0xFFFFFFFF is refused (its padded size must not
//    wrap to 0 and be misparsed);
//  - a chunk or data size beyond the end of the file is refused;
//  - a truncated header is refused.

#include "cli/wav_file.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace
{
namespace cs = channel_sim;
using Bytes = std::vector<char>;

constexpr std::uint32_t test_rate_hz = 8000U;
constexpr std::uint16_t pcm_format = 1U;
constexpr std::uint16_t mono = 1U;
constexpr std::uint16_t pcm16_bits = 16U;
constexpr std::uint16_t pcm16_frame_bytes = 2U;
constexpr std::uint32_t fmt_bytes = 16U;
constexpr std::uint32_t odd_chunk_bytes = 3U;
constexpr std::uint32_t largest_chunk = 0xFFFFFFFFU;
constexpr std::uint32_t riff_size_placeholder = 0U; // the reader does not use the RIFF size
constexpr std::array<std::int16_t, 2U> samples{1234, -4321};
constexpr std::uint32_t data_bytes = 4U;
static_assert(data_bytes == samples.size() * pcm16_frame_bytes, "the data chunk holds the samples");
constexpr unsigned byte_bits = 8U;
constexpr std::uint32_t byte_mask = 0xFFU;
constexpr const char* scratch_file = "channel_sim_wav_test.wav";

class Checks
{
public:
    void expect(bool condition, const char* what) noexcept
    {
        if (!condition)
        {
            ++failures_;
            std::fputs("FAIL: ", stdout);
            std::fputs(what, stdout);
            std::fputs("\n", stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

void put_tag(Bytes& bytes, std::string_view tag)
{
    bytes.insert(bytes.end(), tag.begin(), tag.end());
}

void put_le32(Bytes& bytes, std::uint32_t value)
{
    for (unsigned shift = 0U; shift < 4U * byte_bits; shift += byte_bits)
    {
        bytes.push_back(static_cast<char>((value >> shift) & byte_mask));
    }
}

void put_le16(Bytes& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<char>(value & byte_mask));
    bytes.push_back(static_cast<char>((value >> byte_bits) & byte_mask));
}

[[nodiscard]] Bytes riff_header()
{
    Bytes bytes;
    put_tag(bytes, "RIFF");
    put_le32(bytes, riff_size_placeholder);
    put_tag(bytes, "WAVE");
    return bytes;
}

void put_fmt(Bytes& bytes)
{
    put_tag(bytes, "fmt ");
    put_le32(bytes, fmt_bytes);
    put_le16(bytes, pcm_format);
    put_le16(bytes, mono);
    put_le32(bytes, test_rate_hz);
    put_le32(bytes, test_rate_hz * pcm16_frame_bytes);
    put_le16(bytes, pcm16_frame_bytes);
    put_le16(bytes, pcm16_bits);
}

void put_data(Bytes& bytes, std::uint32_t declared_bytes)
{
    put_tag(bytes, "data");
    put_le32(bytes, declared_bytes);

    for (const std::int16_t sample : samples)
    {
        put_le16(bytes, static_cast<std::uint16_t>(sample));
    }
}

// Writes the bytes and opens them with a fresh WavFileSource. The source is closed
// before the file is removed (Windows removes no open file), and the removal is checked.
[[nodiscard]] m110::Status open_bytes(Checks& checks, const Bytes& bytes, std::vector<cs::Pcm16>* read_back = nullptr)
{
    {
        std::ofstream file{scratch_file, std::ios::binary | std::ios::trunc};
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    m110::Status status = m110::Status::success();
    {
        const auto source = std::make_unique<cs::cli::WavFileSource>();
        status = source->open(scratch_file);

        if (status.is_ok() && read_back != nullptr)
        {
            std::array<cs::Pcm16, samples.size() + 1U> buffer{};
            const m110::Result<std::size_t> got = source->read(buffer);
            status = got.status();

            if (got)
            {
                read_back->assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got.value()));
            }
        }
    }

    checks.expect(std::remove(scratch_file) == 0, "the scratch file is removed once its reader is closed");
    return status;
}

[[nodiscard]] bool refused_with(const m110::Status& status, std::string_view message) noexcept
{
    return !status.is_ok() && std::string_view{status.message} == message;
}

void test_wav_reader(Checks& checks)
{
    Bytes odd = riff_header();
    put_fmt(odd);
    put_tag(odd, "LIST");
    put_le32(odd, odd_chunk_bytes);
    odd.insert(odd.end(), {'a', 'b', 'c', '\0'}); // three bytes and the pad byte
    put_data(odd, data_bytes);
    std::vector<cs::Pcm16> read_back;
    checks.expect(open_bytes(checks, odd, &read_back).is_ok() && read_back == std::vector<cs::Pcm16>(samples.begin(), samples.end()), "an odd-sized chunk is skipped with its pad byte");
    Bytes huge = riff_header();
    put_fmt(huge);
    put_tag(huge, "LIST");
    put_le32(huge, largest_chunk);
    put_data(huge, data_bytes);
    checks.expect(refused_with(open_bytes(checks, huge), "input WAV has a chunk that runs past the end of the file"), "a chunk of size 0xFFFFFFFF is refused, not wrapped to 0");
    Bytes huge_fmt = riff_header();
    put_tag(huge_fmt, "fmt ");
    put_le32(huge_fmt, largest_chunk);
    checks.expect(refused_with(open_bytes(checks, huge_fmt), "input WAV has a chunk that runs past the end of the file"), "a fmt chunk beyond the end of the file is refused");
    Bytes long_data = riff_header();
    put_fmt(long_data);
    put_data(long_data, largest_chunk);
    checks.expect(refused_with(open_bytes(checks, long_data), "input WAV is shorter than its data chunk"), "a data chunk beyond the end of the file is refused when opening");
    Bytes no_data = riff_header();
    put_fmt(no_data);
    checks.expect(refused_with(open_bytes(checks, no_data), "input WAV has no data chunk"), "a file without data is refused");
    Bytes truncated;
    put_tag(truncated, "RIFF");
    checks.expect(refused_with(open_bytes(checks, truncated), "input is not a RIFF/WAVE file"), "a truncated header is refused");
}
} // namespace

int main()
{
    Checks checks;
    test_wav_reader(checks);

    if (checks.failures() != 0)
    {
        std::fputs("wav_file tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("wav_file tests: PASS\n", stdout);
    return 0;
}
