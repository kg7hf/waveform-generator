// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include <cstddef>
#include <cstdint>

namespace waveform_generator::tx_protocol
{
inline constexpr std::size_t maximum_payload = 256U;
inline constexpr std::uint32_t maximum_upload_bytes = 1024U * 1024U;

enum class Kind : std::uint8_t
{
    data, mode, send, reset, select, info, query, generate_file
};

// Trivially copyable mailbox. All filesystem operations belong to the player.
// interleave: 0 short, 1 long, 2 zero (4800U).
struct Request
{
    Kind kind{Kind::query};
    std::uint8_t data[maximum_payload] {};
    std::uint16_t size{};
    std::uint16_t rate{600U};
    std::uint8_t interleave{1U};
    char name[13] {"GEN.WAV"};
    char input_name[13] {};
    char encoder[16] {};
    char profile[32] {}; // Opaque encoder settings; empty uses an adapter that opts into legacy mode.
};

struct Reply
{
    bool ok{};
    const char* error{"none"};
    std::uint32_t payload_bytes{};
    std::uint64_t frames{};
    bool generating{};
    bool complete{};
};

[[nodiscard]] bool valid_filename(const char* text, const char* extension) noexcept;
[[nodiscard]] const char* mode_token(std::uint16_t rate, std::uint8_t interleave) noexcept;

// Production DD-006 command spellings are delimiter-free and may be fragmented
// or concatenated. Added WAV FILE, TX FILE and STATUS commands require LF.
class CommandParser
{
public:
    void reset() noexcept;
    void feed(std::uint8_t byte) noexcept;
    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return valid_;
    }
    [[nodiscard]] bool mode_query() const noexcept
    {
        return mode_query_;
    }
    [[nodiscard]] const Request& request() const noexcept
    {
        return request_;
    }
    void consume() noexcept;

private:
    void parse(bool terminated) noexcept;
    char text_[257] {};
    std::size_t size_{};
    Request request_{};
    bool ready_{};
    bool valid_{};
    bool mode_query_{};
    bool overflow_{};
};
}
