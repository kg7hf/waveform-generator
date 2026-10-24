// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// common/status.hpp's Result over std::expected<T, Status>: a value or the
// Status that explains its absence, for a T without a default constructor
// and a move-only T, the accessors its callers use (has_value, operator
// bool, value, error, status), and a Result built from an ok Status.

#include "common/status.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <type_traits>
#include <utility>

namespace
{

constexpr m110::Status refused{.code = m110::StatusCode::invalid_argument, .message = "refused"};
// Values the checks store and read back: any distinct numbers do.
constexpr int first_number = 5;
constexpr int second_number = 6;
constexpr int moved_number = 7;
constexpr int source_number = 8;

// A value made only from a number: no default constructor, which the old
// Result needed for its error case.
class Token
{
public:
    explicit constexpr Token(int number) noexcept : number_{number} {}

    [[nodiscard]] constexpr int number() const noexcept { return number_; }

private:
    int number_;
};

static_assert(!std::is_default_constructible_v<Token>);

// A value that can be moved but not copied.
class MoveOnly
{
public:
    explicit constexpr MoveOnly(int number) noexcept : number_{number} {}

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    constexpr MoveOnly(MoveOnly&&) noexcept = default;
    constexpr MoveOnly& operator=(MoveOnly&&) noexcept = default;
    constexpr ~MoveOnly() = default;

    [[nodiscard]] constexpr int number() const noexcept { return number_; }

private:
    int number_;
};

[[nodiscard]] constexpr bool same_status(m110::Status left, m110::Status right) noexcept
{
    return left.code == right.code && std::string_view{left.message} == std::string_view{right.message};
}

[[nodiscard]] constexpr m110::Result<Token> token_or_refusal(int number) noexcept
{
    if (number < 0)
    {
        return refused;
    }

    return Token{number};
}

// A value: has_value, operator bool and value agree, and error and status
// are success.
static_assert(token_or_refusal(3).has_value() && static_cast<bool>(token_or_refusal(3)) && token_or_refusal(3).value().number() == 3);
static_assert(token_or_refusal(3).error().is_ok() && token_or_refusal(3).status().is_ok());
// An error: no value, and error and status are the Status it was built from.
static_assert(!token_or_refusal(-1).has_value() && !static_cast<bool>(token_or_refusal(-1)));
static_assert(same_status(token_or_refusal(-1).error(), refused) && same_status(token_or_refusal(-1).status(), refused));
// An ok Status gives no value either, and the Result says why.
static_assert(!m110::Result<int>{m110::Status::success()}.has_value());
static_assert(same_status(m110::Result<int>{m110::Status::success()}.error(), m110::ok_status_without_value));
static_assert(!m110::ok_status_without_value.is_ok());
// value() keeps the value category of the Result it is called on.
static_assert(std::is_same_v<decltype(std::declval<m110::Result<Token>&>().value()), Token&>);
static_assert(std::is_same_v<decltype(std::declval<const m110::Result<Token>&>().value()), const Token&>);
static_assert(std::is_same_v<decltype(std::declval<m110::Result<Token>>().value()), Token&&>);
// A move-only value moves out of its Result.
static_assert(m110::Result<MoveOnly>{MoveOnly{4}}.value().number() == 4);
// Trivially copy-constructible and destructible when the value is, like
// std::expected.
static_assert(std::is_trivially_copy_constructible_v<m110::Result<Token>> && std::is_trivially_destructible_v<m110::Result<Token>>);
static_assert(!std::is_copy_constructible_v<m110::Result<MoveOnly>> && std::is_move_constructible_v<m110::Result<MoveOnly>>);

} // namespace

TEST_CASE("Result containing a value exposes mutable and const access", "[status][result]")
{
    m110::Result<Token> result = token_or_refusal(first_number);
    CHECK(result.has_value());
    CHECK(static_cast<bool>(result));
    CHECK(result.value().number() == first_number);
    CHECK(result.error().is_ok());
    CHECK(result.status().is_ok());
    result.value() = Token{second_number};
    CHECK(result.value().number() == second_number);
    const m110::Result<Token>& view = result;
    CHECK(view.value().number() == second_number);
}

TEST_CASE("Result containing an error exposes the original Status", "[status][result]")
{
    const m110::Result<Token> result = token_or_refusal(-2);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(same_status(result.error(), refused));
    CHECK(same_status(result.status(), refused));
    const m110::Result<int> from_ok{m110::Status::success()};
    CHECK_FALSE(from_ok.has_value());
    CHECK(same_status(from_ok.error(), m110::ok_status_without_value));
}

TEST_CASE("Result supports move-only values", "[status][result]")
{
    m110::Result<MoveOnly> result{MoveOnly{moved_number}};
    const MoveOnly taken = std::move(result).value();
    CHECK(taken.number() == moved_number);
    m110::Result<MoveOnly> source{MoveOnly{source_number}};
    const m110::Result<MoveOnly> moved{std::move(source)};
    CHECK(moved.has_value());
    CHECK(moved.value().number() == source_number);
}
