// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "card_listing.hpp"

#include <cstddef>
#include <cstdint>

#include "ff.h"

namespace waveform_generator::card_listing
{
namespace
{

constexpr char directory_path[] = "2:/WG";
// FatFs exposes AM_DIR publicly; the FAT volume-label bit is internal to ff.c.
constexpr std::uint8_t volume_label_attribute = 0x08U;
static_assert(FF_CODE_PAGE == 437 && FF_USE_LFN != 0 && FF_LFN_UNICODE == 0, "directory aliases use the configured CP437 FatFs API");

constexpr std::uint16_t cp437_high_table[] = {
    0x00C7U, 0x00FCU, 0x00E9U, 0x00E2U, 0x00E4U, 0x00E0U, 0x00E5U, 0x00E7U, 0x00EAU, 0x00EBU, 0x00E8U, 0x00EFU, 0x00EEU, 0x00ECU, 0x00C4U, 0x00C5U, 0x00C9U, 0x00E6U, 0x00C6U,
    0x00F4U, 0x00F6U, 0x00F2U, 0x00FBU, 0x00F9U, 0x00FFU, 0x00D6U, 0x00DCU, 0x00A2U, 0x00A3U, 0x00A5U, 0x20A7U, 0x0192U, 0x00E1U, 0x00EDU, 0x00F3U, 0x00FAU, 0x00F1U, 0x00D1U,
    0x00AAU, 0x00BAU, 0x00BFU, 0x2310U, 0x00ACU, 0x00BDU, 0x00BCU, 0x00A1U, 0x00ABU, 0x00BBU, 0x2591U, 0x2592U, 0x2593U, 0x2502U, 0x2524U, 0x2561U, 0x2562U, 0x2556U, 0x2555U,
    0x2563U, 0x2551U, 0x2557U, 0x255DU, 0x255CU, 0x255BU, 0x2510U, 0x2514U, 0x2534U, 0x252CU, 0x251CU, 0x2500U, 0x253CU, 0x255EU, 0x255FU, 0x255AU, 0x2554U, 0x2569U, 0x2566U,
    0x2560U, 0x2550U, 0x256CU, 0x2567U, 0x2568U, 0x2564U, 0x2565U, 0x2559U, 0x2558U, 0x2552U, 0x2553U, 0x256BU, 0x256AU, 0x2518U, 0x250CU, 0x2588U, 0x2584U, 0x258CU, 0x2590U,
    0x2580U, 0x03B1U, 0x00DFU, 0x0393U, 0x03C0U, 0x03A3U, 0x03C3U, 0x00B5U, 0x03C4U, 0x03A6U, 0x0398U, 0x03A9U, 0x03B4U, 0x221EU, 0x03C6U, 0x03B5U, 0x2229U, 0x2261U, 0x00B1U,
    0x2265U, 0x2264U, 0x2320U, 0x2321U, 0x00F7U, 0x2248U, 0x00B0U, 0x2219U, 0x00B7U, 0x221AU, 0x207FU, 0x00B2U, 0x25A0U, 0x00A0U,
};

void clear_page(Page& page) noexcept
{
    page.offset = 0U;
    page.total = 0U;
    page.count = 0U;

    for (Entry& entry : page.entries)
    {
        for (char& value : entry.name)
        {
            value = '\0';
        }

        entry.size_bytes = 0U;
    }
}

[[nodiscard]] std::uint16_t cp437_codepoint(unsigned char byte) noexcept
{
    if (byte < 0x80U)
    {
        return byte;
    }

    return cp437_high_table[static_cast<std::size_t>(byte - 0x80U)];
}

[[nodiscard]] m110::Status append_utf8(std::uint16_t codepoint, char* output, std::size_t& output_length) noexcept
{
    const std::size_t available = static_cast<std::size_t>(name_capacity) - 1U - output_length;

    if (codepoint <= 0x7FU)
    {
        if (available < 1U)
        {
            return invalid_name;
        }

        output[output_length++] = static_cast<char>(codepoint);
        return m110::Status::success();
    }

    if (codepoint <= 0x7FFU)
    {
        if (available < 2U)
        {
            return invalid_name;
        }

        output[output_length++] = static_cast<char>(0xC0U | (codepoint >> 6U));
        output[output_length++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        return m110::Status::success();
    }

    if (available < 3U)
    {
        return invalid_name;
    }

    output[output_length++] = static_cast<char>(0xE0U | (codepoint >> 12U));
    output[output_length++] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    output[output_length++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    return m110::Status::success();
}

[[nodiscard]] m110::Status convert_name(const FILINFO& information, char (&name)[name_capacity]) noexcept
{
    name[0] = '\0';
    const bool has_short_alias = information.altname[0] != '\0';
    const char* source = has_short_alias ? information.altname : information.fname;
    const std::size_t source_capacity = has_short_alias ? sizeof(information.altname) : sizeof(information.fname);
    std::size_t source_length = 0U;

    while (source_length < source_capacity && source[source_length] != '\0')
    {
        ++source_length;
    }

    // FAT returns an SFN alias in altname when one exists. exFAT has no SFN
    // alias, so FatFs leaves altname empty and fname is the actual LFN. Bound
    // both cases by the serialized UTF-8 storage, while keeping SFNs to 8.3.
    if (source_length == 0U || source_length == source_capacity || (has_short_alias && source_length > 12U) || (!has_short_alias && source_length > name_capacity - 1U))
    {
        return invalid_name;
    }

    // FatFs reports an exFAT name that cannot be represented in the configured
    // OEM code page as a lone '?'. Such names cannot be used as a path here.
    if ((source_length == 1U && (source[0] == '.' || source[0] == '?')) || (source_length == 2U && source[0] == '.' && source[1] == '.'))
    {
        return invalid_name;
    }

    std::size_t output_length = 0U;

    for (std::size_t index = 0U; index < source_length; ++index)
    {
        const unsigned char byte = static_cast<unsigned char>(source[index]);

        if (byte < 0x20U || byte == 0x7FU || byte == '/' || byte == '\\' || byte == ':')
        {
            return invalid_name;
        }

        if (const m110::Status appended = append_utf8(cp437_codepoint(byte), name, output_length); !appended.is_ok())
        {
            return appended;
        }
    }

    name[output_length] = '\0';
    return m110::Status::success();
}

} // namespace

[[nodiscard]] m110::Status read_page(std::uint64_t offset, Page& page) noexcept
{
    clear_page(page);
    DIR directory{};

    if (f_opendir(&directory, directory_path) != FR_OK)
    {
        return open_failed;
    }

    std::uint64_t total = 0U;
    m110::Status failure = m110::Status::success();
    FILINFO information{};

    for (;;)
    {
        information = {};

        if (f_readdir(&directory, &information) != FR_OK)
        {
            failure = read_failed;
            break;
        }

        if (information.fname[0] == '\0')
        {
            break;
        }

        if ((information.fattrib & (AM_DIR | volume_label_attribute)) != 0U)
        {
            continue;
        }

        if (total == UINT64_MAX)
        {
            failure = invalid_offset;
            break;
        }

        char converted_name[name_capacity]{};

        if (const m110::Status converted = convert_name(information, converted_name); !converted.is_ok())
        {
            failure = converted;
            break;
        }

        if (total >= offset && page.count < page_capacity)
        {
            Entry& entry = page.entries[page.count];

            for (std::size_t index = 0U; index < name_capacity; ++index)
            {
                entry.name[index] = converted_name[index];
            }

            entry.size_bytes = static_cast<std::uint64_t>(information.fsize);
            ++page.count;
        }

        ++total;
    }

    const FRESULT close_result = f_closedir(&directory);

    if (!failure.is_ok())
    {
        clear_page(page);
        return failure;
    }

    if (close_result != FR_OK)
    {
        clear_page(page);
        return close_failed;
    }

    if (offset > total)
    {
        clear_page(page);
        return invalid_offset;
    }

    page.offset = offset;
    page.total = total;
    return m110::Status::success();
}

} // namespace waveform_generator::card_listing
