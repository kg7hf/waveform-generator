// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

// Minimal fixed-capacity JSON reader for scenario documents.
//
// No allocation: values live in a static node table and strings point into the
// caller's text buffer (escape sequences are skipped, not decoded, which is all
// the scenario vocabulary needs). Numbers are converted with a single rounding
// so "7.0", "0.25" and "-12" parse to exactly the doubles Python produces.

#include <cstddef>
#include <cstdint>

namespace signal_lab::json
{

enum class Type : std::uint8_t
{
    null,
    boolean,
    number,
    string,
    array,
    object,
};

struct Value
{
    Type type{Type::null};
    bool boolean{};
    double number{};
    const char* text{};
    std::uint32_t text_length{};
    const char* key{};
    std::uint32_t key_length{};
    std::uint32_t first_child{0xFFFFFFFFU};
    std::uint32_t next_sibling{0xFFFFFFFFU};
    std::uint32_t child_count{};

    [[nodiscard]] bool equals(const char* literal) const noexcept;
};

// A five-stage corpus scenario uses about 90 nodes; 256 keeps the static reader small (about 14 KiB).
inline constexpr std::uint32_t document_capacity = 256U;

class Document
{
public:
    // Parse text[0, length). Returns false and sets error() on failure.
    bool parse(const char* text, std::size_t length) noexcept;

    [[nodiscard]] const Value* root() const noexcept;
    [[nodiscard]] const char* error() const noexcept
    {
        return error_;
    }
    [[nodiscard]] std::uint32_t node_count() const noexcept
    {
        return count_;
    }

    // Object member lookup (nullptr when absent); array element by index.
    [[nodiscard]] const Value* find(const Value& object, const char* key) const noexcept;
    [[nodiscard]] const Value* at(const Value& array, std::uint32_t index) const noexcept;
    [[nodiscard]] const Value* child(std::uint32_t index) const noexcept;

    // Convenience accessors with defaults.
    [[nodiscard]] double number_or(const Value& object, const char* key, double fallback) const noexcept;
    [[nodiscard]] bool has(const Value& object, const char* key) const noexcept;

private:
    bool parse_value(std::uint32_t& out_index) noexcept;
    bool parse_object(std::uint32_t index) noexcept;
    bool parse_array(std::uint32_t index) noexcept;
    bool parse_string(const char*& text, std::uint32_t& length) noexcept;
    bool parse_number(double& value) noexcept;
    bool allocate(std::uint32_t& index) noexcept;
    void skip_whitespace() noexcept;
    bool fail(const char* message) noexcept;

    Value nodes_[document_capacity] {};
    std::uint32_t count_{};
    const char* cursor_{};
    const char* end_{};
    const char* error_{"not parsed"};
    std::uint32_t depth_{};
};

} // namespace signal_lab::json
