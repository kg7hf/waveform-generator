// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Bounded JSON reader
// Guide: docs/modules/signal-lab-engine.md
// Ownership: Input is borrowed; tokens and text occupy fixed document storage.
// Contract: Reject storage/depth exhaustion and non-finite numeric results.

#include "signal_lab/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace signal_lab::json
{
namespace
{

constexpr std::uint32_t none = 0xFFFFFFFFU;

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// The powers of ten a double holds exactly, 10^0 to 10^22.
constexpr std::array<double, 23U> exact_powers_of_ten{
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};
constexpr int exact_power_max = static_cast<int>(exact_powers_of_ten.size()) - 1;

// A number's digits: the mantissa keeps the first 19 significant ones
// (10^19 < 2^64); an integer digit after them raises the exponent, a fraction
// digit after them is dropped.
constexpr int mantissa_digits_max = 19;
constexpr int decimal_base = 10;
// An exponent's digits after it reaches this no longer change it.
constexpr int exponent_limit = 10000;

constexpr std::string_view blanks = " \t\n\r";
constexpr std::string_view true_literal = "true";
constexpr std::string_view false_literal = "false";
constexpr std::string_view null_literal = "null";

// The reader's refusal: invalid_argument with a static message.
[[nodiscard]] constexpr m110::Status refused(const char* message) noexcept
{
    return {.code = m110::StatusCode::invalid_argument, .message = message};
}

// The character that closes an object, or an array.
[[nodiscard]] constexpr char closer(bool object) noexcept
{
    return object ? '}' : ']';
}

// Whether `rest` starts with a digit.
[[nodiscard]] bool digit_first(std::string_view rest) noexcept
{
    return !rest.empty() && is_digit(rest.front());
}

[[nodiscard]] int digit_value(char digit) noexcept
{
    return digit - '0';
}

// A number as its decimal digits and a power of ten.
struct Decimal
{
    std::uint64_t mantissa{};
    int digits{};
    int exponent10{};
};

// Takes the digits at the front of `rest` into `decimal`, as integer digits
// or as fraction digits.
void read_digits(std::string_view& rest, Decimal& decimal, bool fraction) noexcept
{
    while (digit_first(rest))
    {
        if (decimal.digits < mantissa_digits_max)
        {
            decimal.mantissa = decimal.mantissa * std::uint64_t{decimal_base} + static_cast<std::uint64_t>(digit_value(rest.front()));

            if (decimal.mantissa != 0U)
            {
                ++decimal.digits;
            }

            if (fraction)
            {
                --decimal.exponent10;
            }
        }
        else if (!fraction)
        {
            ++decimal.exponent10;
        }

        rest.remove_prefix(1U);
    }
}

// The exponent at the front of `rest` ('e' or 'E', a sign, digits), its sign
// applied; or the refusal of an exponent without a digit.
[[nodiscard]] m110::Result<int> read_exponent(std::string_view& rest) noexcept
{
    rest.remove_prefix(1U); // 'e' or 'E'
    bool negative = false;

    if (!rest.empty() && (rest.front() == '+' || rest.front() == '-'))
    {
        negative = rest.front() == '-';
        rest.remove_prefix(1U);
    }

    if (!digit_first(rest))
    {
        return refused("expected an exponent digit");
    }

    int exponent = 0;

    while (digit_first(rest))
    {
        if (exponent < exponent_limit)
        {
            exponent = exponent * decimal_base + digit_value(rest.front());
        }

        rest.remove_prefix(1U);
    }

    return negative ? -exponent : exponent;
}

// mantissa x 10^exponent10, with a single rounding when both factors are
// exact doubles (mantissa < 2^53, |exponent10| <= 22).
[[nodiscard]] double scaled(const Decimal& decimal) noexcept
{
    const std::span<const double, exact_powers_of_ten.size()> powers{exact_powers_of_ten};
    auto result = static_cast<double>(decimal.mantissa);
    int exponent10 = decimal.exponent10;

    if (exponent10 > 0)
    {
        while (exponent10 > exact_power_max)
        {
            result *= powers.back();
            exponent10 -= exact_power_max;
        }

        result *= powers[static_cast<std::size_t>(exponent10)];
    }
    else if (exponent10 < 0)
    {
        int remaining = -exponent10;

        while (remaining > exact_power_max)
        {
            result /= powers.back();
            remaining -= exact_power_max;
        }

        result /= powers[static_cast<std::size_t>(remaining)];
    }

    return result;
}

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

m110::Status Document::parse(const char* text, std::size_t length) noexcept
{
    count_ = 0U;
    rest_ = text == nullptr ? std::string_view{} : std::string_view{text, length};
    const m110::Status parsed = parse_document();
    error_ = parsed.is_ok() ? nullptr : parsed.message;
    return parsed;
}

m110::Status Document::parse_document() noexcept
{
    if (rest_.data() == nullptr)
    {
        return refused("null text");
    }

    // Each pass begins one value. An array or object it opens is read by the
    // passes that follow, each beginning one of its members; after a whole
    // value, end_value() reads the closes it completes and the ',' before the
    // next member. The open containers are in `nesting`, not in a call chain.
    Nesting nesting{};

    for (;;)
    {
        const m110::Result<Begun> begun = begin_value(nesting);

        if (!begun.has_value())
        {
            return begun.error();
        }

        if (begun.value() == Begun::opened)
        {
            continue;
        }

        const m110::Result<Next> next = end_value(nesting);

        if (!next.has_value())
        {
            return next.error();
        }

        if (next.value() == Next::done)
        {
            break;
        }
    }

    skip_whitespace();
    return rest_.empty() ? m110::Status::success() : refused("trailing characters after the document");
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

m110::Result<std::uint32_t> Document::allocate() noexcept
{
    if (count_ >= document_capacity)
    {
        return refused("document has too many values");
    }

    const std::uint32_t index = count_;
    std::span{nodes_}[index] = Value{};
    ++count_;
    return index;
}

void Document::skip_whitespace() noexcept
{
    rest_.remove_prefix(std::min(rest_.find_first_not_of(blanks), rest_.size()));
}

m110::Result<std::string_view> Document::parse_string() noexcept
{
    if (rest_.empty() || rest_.front() != '"')
    {
        return refused("expected a string");
    }

    rest_.remove_prefix(1U);
    std::size_t length = 0U;

    // A backslash escapes the character after it, a quote too.
    while (length < rest_.size() && rest_[length] != '"')
    {
        length += rest_[length] == '\\' ? 2U : 1U;
    }

    if (length >= rest_.size())
    {
        return refused("unterminated string");
    }

    const std::string_view characters = rest_.substr(0U, length);
    rest_.remove_prefix(length + 1U);
    return characters;
}

m110::Result<double> Document::parse_number() noexcept
{
    const bool negative = !rest_.empty() && rest_.front() == '-';

    if (negative)
    {
        rest_.remove_prefix(1U);
    }

    if (!digit_first(rest_))
    {
        return refused("expected a digit");
    }

    Decimal decimal{};
    read_digits(rest_, decimal, false);

    if (!rest_.empty() && rest_.front() == '.')
    {
        rest_.remove_prefix(1U);

        if (!digit_first(rest_))
        {
            return refused("expected a fraction digit");
        }

        read_digits(rest_, decimal, true);
    }

    if (!rest_.empty() && (rest_.front() == 'e' || rest_.front() == 'E'))
    {
        const m110::Result<int> exponent = read_exponent(rest_);

        if (!exponent.has_value())
        {
            return exponent.error();
        }

        decimal.exponent10 += exponent.value();
    }

    const double result = scaled(decimal);

    if (!std::isfinite(result))
    {
        return refused("JSON number is outside the finite range");
    }

    return negative ? -result : result;
}

m110::Result<Document::Begun> Document::begin_value(Nesting& nesting) noexcept
{
    skip_whitespace();

    if (rest_.empty())
    {
        return refused("unexpected end of document");
    }

    const m110::Result<std::uint32_t> index = allocate();

    if (!index.has_value())
    {
        return index.error();
    }

    if (nesting.depth > 0U)
    {
        add_member(nesting, index.value());
    }

    if (rest_.front() == '{' || rest_.front() == '[')
    {
        return open_container(nesting, index.value());
    }

    if (const m110::Status read = read_scalar(std::span{nodes_}[index.value()]); !read.is_ok())
    {
        return read;
    }

    return Begun::whole;
}

m110::Result<Document::Begun> Document::open_container(Nesting& nesting, std::uint32_t index) noexcept
{
    if (nesting.depth == document_max_depth)
    {
        return refused("document nests too deeply");
    }

    const bool object = rest_.front() == '{';
    std::span{nodes_}[index].type = object ? Type::object : Type::array;
    rest_.remove_prefix(1U); // '{' or '['
    std::span{nesting.open}[nesting.depth] = Open{.node = index, .last_child = none};
    ++nesting.depth;
    skip_whitespace();

    if (!rest_.empty() && rest_.front() == closer(object))
    {
        rest_.remove_prefix(1U);
        --nesting.depth;
        return Begun::whole;
    }

    if (const m110::Status keyed = object ? read_key(nesting) : m110::Status::success(); !keyed.is_ok())
    {
        return keyed;
    }

    return Begun::opened;
}

m110::Result<Document::Next> Document::end_value(Nesting& nesting) noexcept
{
    while (nesting.depth > 0U)
    {
        const bool object = std::span{nodes_}[std::span{nesting.open}[nesting.depth - 1U].node].type == Type::object;
        skip_whitespace();

        if (rest_.empty())
        {
            return refused(object ? "unterminated object" : "unterminated array");
        }

        if (rest_.front() == ',')
        {
            rest_.remove_prefix(1U);

            if (const m110::Status keyed = object ? read_key(nesting) : m110::Status::success(); !keyed.is_ok())
            {
                return keyed;
            }

            return Next::member;
        }

        if (rest_.front() != closer(object))
        {
            return refused(object ? "expected ',' or '}' in an object" : "expected ',' or ']' in an array");
        }

        rest_.remove_prefix(1U);
        --nesting.depth;
    }

    return Next::done;
}

m110::Status Document::read_key(Nesting& nesting) noexcept
{
    skip_whitespace();
    const m110::Result<std::string_view> key = parse_string();

    if (!key.has_value())
    {
        return key.error();
    }

    skip_whitespace();

    if (rest_.empty() || rest_.front() != ':')
    {
        return refused("expected ':' after an object key");
    }

    rest_.remove_prefix(1U);
    nesting.key = key.value();
    return m110::Status::success();
}

m110::Status Document::read_scalar(Value& node) noexcept
{
    const char first = rest_.front();

    if (first == '"')
    {
        node.type = Type::string;
        const m110::Result<std::string_view> characters = parse_string();

        if (!characters.has_value())
        {
            return characters.error();
        }

        node.text = characters.value().data();
        node.text_length = static_cast<std::uint32_t>(characters.value().size());
        return m110::Status::success();
    }

    if (first == '-' || is_digit(first))
    {
        node.type = Type::number;
        const m110::Result<double> number = parse_number();

        if (!number.has_value())
        {
            return number.error();
        }

        node.number = number.value();
        return m110::Status::success();
    }

    return read_literal(node);
}

m110::Status Document::read_literal(Value& node) noexcept
{
    if (rest_.starts_with(true_literal) || rest_.starts_with(false_literal))
    {
        node.type = Type::boolean;
        node.boolean = rest_.starts_with(true_literal);
        rest_.remove_prefix(node.boolean ? true_literal.size() : false_literal.size());
        return m110::Status::success();
    }

    if (rest_.starts_with(null_literal))
    {
        node.type = Type::null;
        rest_.remove_prefix(null_literal.size());
        return m110::Status::success();
    }

    return refused("unexpected character");
}

void Document::add_member(Nesting& nesting, std::uint32_t child) noexcept
{
    Open& open = std::span{nesting.open}[nesting.depth - 1U];

    if (std::span{nodes_}[open.node].type == Type::object)
    {
        Value& member = std::span{nodes_}[child];
        member.key = nesting.key.data();
        member.key_length = static_cast<std::uint32_t>(nesting.key.size());
    }

    append_child(open.node, open.last_child, child);
}

void Document::append_child(std::uint32_t parent, std::uint32_t& last_child, std::uint32_t child) noexcept
{
    const std::span<Value, document_capacity> nodes{nodes_};

    if (last_child == none)
    {
        nodes[parent].first_child = child;
    }
    else
    {
        nodes[last_child].next_sibling = child;
    }

    last_child = child;
    ++nodes[parent].child_count;
}

} // namespace signal_lab::json
