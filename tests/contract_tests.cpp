#include "common/level.hpp"
#include "common/wav.hpp"
#include "rt1170/status_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

using waveform_generator::WavPayload;
using waveform_generator::WavReader;
using waveform_generator::WavValidationError;

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void patch_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

void append_fourcc(std::vector<std::uint8_t>& bytes, char a, char b, char c,
                   char d)
{
    bytes.push_back(static_cast<std::uint8_t>(a));
    bytes.push_back(static_cast<std::uint8_t>(b));
    bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.push_back(static_cast<std::uint8_t>(d));
}

void append_chunk_header(std::vector<std::uint8_t>& bytes, char a, char b,
                         char c, char d, std::uint32_t size)
{
    append_fourcc(bytes, a, b, c, d);
    append_u32(bytes, size);
}

void append_fmt_chunk(std::vector<std::uint8_t>& bytes)
{
    append_chunk_header(bytes, 'f', 'm', 't', ' ', 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, 1U);
    append_u32(bytes, 48000U);
    append_u32(bytes, 96000U);
    append_u16(bytes, 2U);
    append_u16(bytes, 16U);
}

void append_data_chunk(std::vector<std::uint8_t>& bytes, std::uint32_t samples)
{
    append_chunk_header(bytes, 'd', 'a', 't', 'a', samples * 2U);
    for (std::uint32_t index = 0U; index < samples; ++index)
    {
        append_u16(bytes, static_cast<std::uint16_t>(index));
    }
}

std::vector<std::uint8_t> make_riff()
{
    std::vector<std::uint8_t> bytes;
    append_fourcc(bytes, 'R', 'I', 'F', 'F');
    append_u32(bytes, 0U);
    append_fourcc(bytes, 'W', 'A', 'V', 'E');
    append_fmt_chunk(bytes);
    append_data_chunk(bytes, 4U);
    patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
    return bytes;
}

bool vector_read(void* context, std::uint32_t offset, std::uint8_t* destination,
                 std::uint32_t count) noexcept
{
    const auto* bytes = static_cast<const std::vector<std::uint8_t>*>(context);
    if (offset > bytes->size() || count > bytes->size() - offset)
    {
        return false;
    }
    std::memcpy(destination, bytes->data() + offset, count);
    return true;
}

WavValidationError validate(std::vector<std::uint8_t>& bytes,
                            WavPayload& payload)
{
    const WavReader reader{
        .context = &bytes,
        .file_size = static_cast<std::uint32_t>(bytes.size()),
        .read = &vector_read,
    };
    return waveform_generator::validate_pcm16_mono_48k_wav(reader, payload);
}

int run_wav_tests()
{
    {
        auto bytes = make_riff();
        append_chunk_header(bytes, 'J', 'U', 'N', 'K', 2U);
        bytes.push_back(1U);
        bytes.push_back(2U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};
        if (validate(bytes, payload) != WavValidationError::none ||
            payload.offset != 44U || payload.bytes != 8U)
        {
            return 10;
        }
    }
    {
        auto bytes = make_riff();
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 7U));
        WavPayload payload{};
        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 11;
        }
    }
    {
        auto bytes = make_riff();
        append_fmt_chunk(bytes);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};
        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 12;
        }
    }
    {
        auto bytes = make_riff();
        append_data_chunk(bytes, 1U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};
        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 13;
        }
    }
    {
        auto bytes = make_riff();
        append_chunk_header(bytes, 'J', 'U', 'N', 'K', 4U);
        bytes.push_back(0U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};
        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 14;
        }
    }
    {
        std::vector<std::uint8_t> bytes;
        append_fourcc(bytes, 'R', 'I', 'F', 'F');
        append_u32(bytes, 0U);
        append_fourcc(bytes, 'W', 'A', 'V', 'E');
        append_fmt_chunk(bytes);
        append_chunk_header(bytes, 'J', 'U', 'N', 'K', 65500U);
        bytes.resize(bytes.size() + 65500U);
        append_data_chunk(bytes, 1U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};
        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 15;
        }
    }

    return 0;
}

} // namespace

int main()
{
    using waveform_generator::headphone_percent_to_raw;

    static_assert(waveform_generator::status_response_worst_case_bytes() <=
                  waveform_generator::cdc_response_capacity);
    static_assert(waveform_generator::status_response_worst_case_bytes() == 1465U);

    constexpr std::array<std::uint8_t, 6> inputs{0U, 1U, 2U, 50U, 70U, 100U};
    constexpr std::array<std::uint8_t, 6> expected{0x00U, 0x30U, 0x31U,
                                                  0x57U, 0x67U, 0x7fU};

    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        if (headphone_percent_to_raw(inputs[index]) != expected[index])
        {
            return 1;
        }
    }

    std::uint8_t previous = headphone_percent_to_raw(1U);
    for (std::uint8_t percent = 2U; percent <= 100U; ++percent)
    {
        const auto current = headphone_percent_to_raw(percent);
        if (current < previous || current < 0x30U || current > 0x7fU)
        {
            return 2;
        }
        previous = current;
    }

    return run_wav_tests();
}
