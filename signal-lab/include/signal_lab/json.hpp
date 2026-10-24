// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Minimal fixed-capacity JSON reader for scenario documents.
//
// No allocation: values live in a static node table and strings point into the
// caller's text buffer (escape sequences are skipped, not decoded, which is all
// the scenario vocabulary needs). Numbers are converted with a single rounding
// so "7.0", "0.25" and "-12" parse to exactly the doubles Python produces.
//
// No recursion: the arrays and objects being read are a fixed list on the
// reader's own frame, so a nested document takes the same stack as a flat
// one (the firmware reads scenarios on the player task's 16 KiB stack).

#include "common/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

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

// A representative five-stage scenario uses about 90 nodes; 256 keeps the static reader small (about 14 KiB).
inline constexpr std::uint32_t document_capacity = 256U;
// The deepest nesting of arrays and objects a document may have; one more is
// refused ("document nests too deeply").
inline constexpr std::uint32_t document_max_depth = 32U;

class Document
{
public:
    // Parse text[0, length): success, or invalid_argument with a static message
    // that says what is wrong.
    [[nodiscard]] m110::Status parse(const char* text, std::size_t length) noexcept;

    // The document the last parse() read; nullptr when it refused the text.
    [[nodiscard]] const Value* root() const noexcept;
    [[nodiscard]] std::uint32_t node_count() const noexcept { return count_; }

    // Object member lookup (nullptr when absent); array element by index.
    [[nodiscard]] const Value* find(const Value& object, const char* key) const noexcept;
    [[nodiscard]] const Value* at(const Value& array, std::uint32_t index) const noexcept;
    [[nodiscard]] const Value* child(std::uint32_t index) const noexcept;

    // Convenience accessors with defaults.
    [[nodiscard]] double number_or(const Value& object, const char* key, double fallback) const noexcept;
    [[nodiscard]] bool has(const Value& object, const char* key) const noexcept;

private:
    // An array or object being read: its node, and its last member so far
    // (0xFFFFFFFF before the first).
    struct Open
    {
        std::uint32_t node{};
        std::uint32_t last_child{};
    };

    // Where the reader is: the arrays and objects it is inside, outermost
    // first, and the key of the object member whose value it reads next.
    // parse_document() keeps it on its own frame (about 270 B).
    struct Nesting
    {
        std::array<Open, document_max_depth> open{};
        std::uint32_t depth{};
        std::string_view key{};
    };

    // After a value's first character: the value is whole (a scalar, or an
    // empty array or object), or an array or object was opened and its first
    // member's value follows.
    enum class Begun : std::uint8_t
    {
        whole,
        opened,
    };

    // After a whole value and the closes it completes: a ',' and the next
    // member follow, or the document's root is whole.
    enum class Next : std::uint8_t
    {
        member,
        done,
    };

    // Each step reads from the front of rest_ and gives what it read, or the
    // Status of the first refusal (invalid_argument, a static message), which
    // every step above it passes on unchanged.
    [[nodiscard]] m110::Status parse_document() noexcept;
    // The value at the front: allocated, linked under the innermost open
    // container, then read whole or opened.
    [[nodiscard]] m110::Result<Begun> begin_value(Nesting& nesting) noexcept;
    [[nodiscard]] m110::Result<Begun> open_container(Nesting& nesting, std::uint32_t index) noexcept;
    // The closes after a whole value, then its ',' (and in an object the next
    // member's key), or the end of the root.
    [[nodiscard]] m110::Result<Next> end_value(Nesting& nesting) noexcept;
    // An object member's key and the ':' after it, into nesting.key.
    [[nodiscard]] m110::Status read_key(Nesting& nesting) noexcept;
    [[nodiscard]] m110::Status read_scalar(Value& node) noexcept;
    [[nodiscard]] m110::Status read_literal(Value& node) noexcept;
    // A string's characters in the caller's text, without the quotes.
    [[nodiscard]] m110::Result<std::string_view> parse_string() noexcept;
    [[nodiscard]] m110::Result<double> parse_number() noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> allocate() noexcept;
    // Links `child` as the next member of the innermost open container, with
    // nesting.key as its key in an object.
    void add_member(Nesting& nesting, std::uint32_t child) noexcept;
    // Links `child` after `last_child` (none: first) under `parent`.
    void append_child(std::uint32_t parent, std::uint32_t& last_child, std::uint32_t child) noexcept;
    void skip_whitespace() noexcept;

    Value nodes_[document_capacity]{};
    std::uint32_t count_{};
    // The text not yet read.
    std::string_view rest_{};
    // The last parse()'s refusal, nullptr when it succeeded; root() reads it.
    const char* error_{"not parsed"};
};

} // namespace signal_lab::json
