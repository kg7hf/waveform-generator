// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

// Bounded JSON reader
// Guide: docs/modules/signal-lab-engine.md
// Ownership: Input is borrowed; tokens and text occupy fixed document storage.
// Contract: Reject storage/depth exhaustion and non-finite numeric results.

#include "signal_lab/json.hpp"

#include <cmath>

namespace signal_lab::json
{
namespace
{

constexpr std::uint32_t none = 0xFFFFFFFFU;
constexpr std::uint32_t max_depth = 32U;

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

constexpr double exact_powers_of_ten[] =
{
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

} // namespace

bool Value::equals(const char* literal) const noexcept
{
    if (type != Type::string || literal == nullptr)
    {
        return false;
    }

    std::uint32_t index = 0U;

    while (index < text_length)
    {
        if (literal[index] == '\0' || literal[index] != text[index])
        {
            return false;
        }

        ++index;
    }

    return literal[index] == '\0';
}

bool Document::parse(const char* text, std::size_t length) noexcept
{
    count_ = 0U;
    depth_ = 0U;
    cursor_ = text;
    end_ = text + length;
    error_ = nullptr;

    if (text == nullptr)
    {
        return fail("null text");
    }

    std::uint32_t root_index = none;

    if (!parse_value(root_index))
    {
        return false;
    }

    skip_whitespace();

    if (cursor_ != end_)
    {
        return fail("trailing characters after the document");
    }

    return true;
}

const Value* Document::root() const noexcept
{
    return (count_ == 0U || error_ != nullptr) ? nullptr : &nodes_[0];
}

const Value* Document::child(std::uint32_t index) const noexcept
{
    return index < count_ ? &nodes_[index] : nullptr;
}

const Value* Document::find(const Value& object, const char* key) const noexcept
{
    if (object.type != Type::object || key == nullptr)
    {
        return nullptr;
    }

    std::uint32_t index = object.first_child;

    while (index != none)
    {
        const Value& node = nodes_[index];
        std::uint32_t position = 0U;
        bool match = true;

        while (position < node.key_length)
        {
            if (key[position] == '\0' || key[position] != node.key[position])
            {
                match = false;
                break;
            }

            ++position;
        }

        if (match && key[position] == '\0')
        {
            return &node;
        }

        index = node.next_sibling;
    }

    return nullptr;
}

const Value* Document::at(const Value& array, std::uint32_t wanted) const noexcept
{
    if (array.type != Type::array)
    {
        return nullptr;
    }

    std::uint32_t index = array.first_child;
    std::uint32_t position = 0U;

    while (index != none)
    {
        if (position == wanted)
        {
            return &nodes_[index];
        }

        index = nodes_[index].next_sibling;
        ++position;
    }

    return nullptr;
}

double Document::number_or(const Value& object, const char* key, double fallback) const noexcept
{
    const Value* node = find(object, key);
    return (node != nullptr && node->type == Type::number) ? node->number : fallback;
}

bool Document::has(const Value& object, const char* key) const noexcept
{
    return find(object, key) != nullptr;
}

bool Document::fail(const char* message) noexcept
{
    if (error_ == nullptr)
    {
        error_ = message;
    }

    return false;
}

bool Document::allocate(std::uint32_t& index) noexcept
{
    if (count_ >= document_capacity)
    {
        return fail("document has too many values");
    }

    index = count_;
    nodes_[index] = Value{};
    ++count_;
    return true;
}

void Document::skip_whitespace() noexcept
{
    while (cursor_ != end_ && (*cursor_ == ' ' || *cursor_ == '\t' || *cursor_ == '\n' || *cursor_ == '\r'))
    {
        ++cursor_;
    }
}

bool Document::parse_string(const char*& text, std::uint32_t& length) noexcept
{
    if (cursor_ == end_ || *cursor_ != '"')
    {
        return fail("expected a string");
    }

    ++cursor_;
    text = cursor_;

    while (cursor_ != end_ && *cursor_ != '"')
    {
        if (*cursor_ == '\\')
        {
            ++cursor_;

            if (cursor_ == end_)
            {
                break;
            }
        }

        ++cursor_;
    }

    if (cursor_ == end_)
    {
        return fail("unterminated string");
    }

    length = static_cast<std::uint32_t>(cursor_ - text);
    ++cursor_;
    return true;
}

bool Document::parse_number(double& value) noexcept
{
    bool negative = false;

    if (cursor_ != end_ && *cursor_ == '-')
    {
        negative = true;
        ++cursor_;
    }

    if (cursor_ == end_ || !is_digit(*cursor_))
    {
        return fail("expected a digit");
    }

    std::uint64_t mantissa = 0U;
    int mantissa_digits = 0;
    int exponent10 = 0;

    while (cursor_ != end_ && is_digit(*cursor_))
    {
        if (mantissa_digits < 19)
        {
            mantissa = mantissa * 10U + static_cast<std::uint64_t>(*cursor_ - '0');

            if (mantissa != 0U)
            {
                ++mantissa_digits;
            }
        }
        else
        {
            ++exponent10;
        }

        ++cursor_;
    }

    if (cursor_ != end_ && *cursor_ == '.')
    {
        ++cursor_;

        if (cursor_ == end_ || !is_digit(*cursor_))
        {
            return fail("expected a fraction digit");
        }

        while (cursor_ != end_ && is_digit(*cursor_))
        {
            if (mantissa_digits < 19)
            {
                mantissa = mantissa * 10U + static_cast<std::uint64_t>(*cursor_ - '0');

                if (mantissa != 0U)
                {
                    ++mantissa_digits;
                }

                --exponent10;
            }

            ++cursor_;
        }
    }

    if (cursor_ != end_ && (*cursor_ == 'e' || *cursor_ == 'E'))
    {
        ++cursor_;
        bool exponent_negative = false;

        if (cursor_ != end_ && (*cursor_ == '+' || *cursor_ == '-'))
        {
            exponent_negative = *cursor_ == '-';
            ++cursor_;
        }

        if (cursor_ == end_ || !is_digit(*cursor_))
        {
            return fail("expected an exponent digit");
        }

        int exponent = 0;

        while (cursor_ != end_ && is_digit(*cursor_))
        {
            if (exponent < 10000)
            {
                exponent = exponent * 10 + (*cursor_ - '0');
            }

            ++cursor_;
        }

        exponent10 += exponent_negative ? -exponent : exponent;
    }

    // Single rounding when both factors are exact doubles (mantissa < 2^53, |exp| <= 22).
    double result = static_cast<double>(mantissa);

    if (exponent10 > 0)
    {
        while (exponent10 > 22)
        {
            result *= 1e22;
            exponent10 -= 22;
        }

        result *= exact_powers_of_ten[exponent10];
    }
    else if (exponent10 < 0)
    {
        int remaining = -exponent10;

        while (remaining > 22)
        {
            result /= 1e22;
            remaining -= 22;
        }

        result /= exact_powers_of_ten[remaining];
    }

    if (!std::isfinite(result))
    {
        return fail("JSON number is outside the finite range");
    }

    value = negative ? -result : result;
    return true;
}

bool Document::parse_value(std::uint32_t& out_index) noexcept
{
    skip_whitespace();

    if (cursor_ == end_)
    {
        return fail("unexpected end of document");
    }

    if (!allocate(out_index))
    {
        return false;
    }

    Value& node = nodes_[out_index];
    const char c = *cursor_;

    if (c == '{')
    {
        node.type = Type::object;
        return parse_object(out_index);
    }

    if (c == '[')
    {
        node.type = Type::array;
        return parse_array(out_index);
    }

    if (c == '"')
    {
        node.type = Type::string;
        return parse_string(node.text, node.text_length);
    }

    if (c == 't' && end_ - cursor_ >= 4 && cursor_[1] == 'r' && cursor_[2] == 'u' && cursor_[3] == 'e')
    {
        node.type = Type::boolean;
        node.boolean = true;
        cursor_ += 4;
        return true;
    }

    if (c == 'f' && end_ - cursor_ >= 5 && cursor_[1] == 'a' && cursor_[2] == 'l' && cursor_[3] == 's' && cursor_[4] == 'e')
    {
        node.type = Type::boolean;
        node.boolean = false;
        cursor_ += 5;
        return true;
    }

    if (c == 'n' && end_ - cursor_ >= 4 && cursor_[1] == 'u' && cursor_[2] == 'l' && cursor_[3] == 'l')
    {
        node.type = Type::null;
        cursor_ += 4;
        return true;
    }

    if (c == '-' || is_digit(c))
    {
        node.type = Type::number;
        return parse_number(node.number);
    }

    return fail("unexpected character");
}

bool Document::parse_object(std::uint32_t index) noexcept
{
    if (++depth_ > max_depth)
    {
        return fail("document nests too deeply");
    }

    ++cursor_; // '{'
    std::uint32_t last_child = none;
    skip_whitespace();

    if (cursor_ != end_ && *cursor_ == '}')
    {
        ++cursor_;
        --depth_;
        return true;
    }

    for (;;)
    {
        skip_whitespace();
        const char* key = nullptr;
        std::uint32_t key_length = 0U;

        if (!parse_string(key, key_length))
        {
            return false;
        }

        skip_whitespace();

        if (cursor_ == end_ || *cursor_ != ':')
        {
            return fail("expected ':' after an object key");
        }

        ++cursor_;
        std::uint32_t child_index = none;

        if (!parse_value(child_index))
        {
            return false;
        }

        nodes_[child_index].key = key;
        nodes_[child_index].key_length = key_length;

        if (last_child == none)
        {
            nodes_[index].first_child = child_index;
        }
        else
        {
            nodes_[last_child].next_sibling = child_index;
        }

        last_child = child_index;
        nodes_[index].child_count++;
        skip_whitespace();

        if (cursor_ == end_)
        {
            return fail("unterminated object");
        }

        if (*cursor_ == ',')
        {
            ++cursor_;
            continue;
        }

        if (*cursor_ == '}')
        {
            ++cursor_;
            --depth_;
            return true;
        }

        return fail("expected ',' or '}' in an object");
    }
}

bool Document::parse_array(std::uint32_t index) noexcept
{
    if (++depth_ > max_depth)
    {
        return fail("document nests too deeply");
    }

    ++cursor_; // '['
    std::uint32_t last_child = none;
    skip_whitespace();

    if (cursor_ != end_ && *cursor_ == ']')
    {
        ++cursor_;
        --depth_;
        return true;
    }

    for (;;)
    {
        std::uint32_t child_index = none;

        if (!parse_value(child_index))
        {
            return false;
        }

        if (last_child == none)
        {
            nodes_[index].first_child = child_index;
        }
        else
        {
            nodes_[last_child].next_sibling = child_index;
        }

        last_child = child_index;
        nodes_[index].child_count++;
        skip_whitespace();

        if (cursor_ == end_)
        {
            return fail("unterminated array");
        }

        if (*cursor_ == ',')
        {
            ++cursor_;
            continue;
        }

        if (*cursor_ == ']')
        {
            ++cursor_;
            --depth_;
            return true;
        }

        return fail("expected ',' or ']' in an array");
    }
}

} // namespace signal_lab::json
