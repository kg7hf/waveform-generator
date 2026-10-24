// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The CUT injector: a play's schedule of dropped frames (DROP) and inserted
// zeros (ZERO), applied to the scenario engine's output, after the channel
// Guide: docs/modules/rt1170-player-and-audio.md (CUT injector)
// Ownership: plain values. In the firmware the player task owns the pending
// list, the latched schedule and its stage, and writes a play's CutLog under
// a critical section that its readers (the USB task, for USB?) also take.
// Contract: constexpr, allocation-free, no I/O. A schedule holds at most
// max_entries entries, each min_length to max_length frames long, sorted and
// non-overlapping, with its DROPs within the drop balance: validate() judges
// a CutList and makes the ValidSchedule that the stage alone takes. The engine processes every card frame; the
// stage takes its output block by block and turns it and the schedule into
// steps (send, insert zeros, discard, the next block, end), so engine time
// runs on through dropped frames. The log records each cut where it took
// effect in the stream the play sends, and tallies the cuts against the
// frames a route confirmed sent, so a play that ends part way reports an
// exact prefix of the cut stream.

#pragma once

#include "common/status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace waveform_generator::cut
{

// A place in the engine's output, counted in its frames from the play's
// first, or a number of such frames. The engine puts out one frame for each
// card frame unless its scenario slips samples, and a play without a
// scenario puts out the card itself: there an output frame is a card frame.
struct OutputFrames
{
    std::uint32_t value{};

    constexpr auto operator<=>(const OutputFrames&) const noexcept = default;
};

// A place in the stream the play sends, after its cuts, counted from that
// stream's first frame (inserted zeros count, dropped output frames do not),
// or a number of such frames.
struct SentFrames
{
    std::uint32_t value{};

    constexpr auto operator<=>(const SentFrames&) const noexcept = default;
};

enum class CutKind : std::uint8_t
{
    drop = 0U, // discard `length` output frames from `offset` on: the stream is that much shorter
    zero = 1U, // send `length` exact zero frames before output frame `offset`: the stream is that much longer
};

// One entry of a schedule, as the CUT command spells it: <offset>:<length>:<drop|zero>.
struct CutEntry
{
    OutputFrames offset{};
    std::uint32_t length{}; // frames: the output frames a DROP discards, the zero frames a ZERO sends
    CutKind kind{};

    constexpr bool operator==(const CutEntry&) const noexcept = default;
};

inline constexpr std::size_t max_entries = 16U;
inline constexpr std::uint32_t min_length = 1U;
inline constexpr std::uint32_t max_length = 4800U; // 100 ms at 48 kHz
// A DROP's frames go through the engine like any others but put nothing
// into the player's output FIFO, so the play spends engine time on them
// while its output drains. The drop balance counts that cost: each DROP
// adds its length, and the engine output frames between one DROP and the
// next take it back, never below zero (a ZERO's zeros cost no engine time
// and take nothing back). A schedule keeps it at or below this; the
// player's FIFO margin covers it (rt1170/player.cpp, min_producer_speed).
inline constexpr std::uint32_t max_drop_balance_frames = 3U * max_length; // 300 ms: three adjacent DROPs of max_length

// The reply codes of a refused schedule (WFG-LIVE/1 error codes).
inline constexpr m110::Status bad_syntax{.code = m110::StatusCode::invalid_argument, .message = "BAD_REQUEST"};
inline constexpr m110::Status too_many_entries{.code = m110::StatusCode::buffer_too_small, .message = "CUT_TOO_MANY"};
inline constexpr m110::Status length_out_of_range{.code = m110::StatusCode::invalid_argument, .message = "CUT_RANGE"};
inline constexpr m110::Status entries_unsorted{.code = m110::StatusCode::invalid_argument, .message = "CUT_UNSORTED"};
inline constexpr m110::Status entries_overlap{.code = m110::StatusCode::invalid_argument, .message = "CUT_OVERLAP"};
inline constexpr m110::Status drop_run_too_long{.code = m110::StatusCode::invalid_argument, .message = "CUT_DROP_RUN"};

// Up to max_entries entries in the order they were given, judged or not.
class CutList
{
public:
    // Appends `entry`. Fails with too_many_entries, changing nothing, when the
    // list holds max_entries.
    [[nodiscard]] constexpr m110::Status push(const CutEntry& entry) noexcept
    {
        if (count_ == entries_.size())
        {
            return too_many_entries;
        }

        std::span{entries_}[count_] = entry;
        ++count_;
        return m110::Status::success();
    }

    constexpr void clear() noexcept { count_ = 0U; }

    [[nodiscard]] constexpr std::span<const CutEntry> entries() const noexcept { return std::span{entries_}.first(count_); }

    [[nodiscard]] constexpr std::uint32_t size() const noexcept { return count_; }

    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0U; }

private:
    std::array<CutEntry, max_entries> entries_{};
    std::uint32_t count_{};
};

// The output frame just past those an entry covers: a DROP covers
// [offset, offset + length), a ZERO none (it sits before `offset`). In 64
// bits, so no offset overflows.
[[nodiscard]] constexpr std::uint64_t covered_end(const CutEntry& entry) noexcept
{
    const std::uint64_t covered = entry.kind == CutKind::drop ? entry.length : 0U;
    return std::uint64_t{entry.offset.value} + covered;
}

// Whether `later` may follow `earlier`: it starts no earlier (else unsorted)
// and no earlier than the end of what `earlier` covers (else overlap). Two
// ZEROs never share an offset (one ZERO of both lengths says the same). A
// ZERO then a DROP at one offset is an erasure, zeros in place of the dropped
// frames; a DROP then a ZERO at the DROP's offset overlaps, since the ZERO
// would sit before a frame the DROP removed.
[[nodiscard]] constexpr m110::Status check_order(const CutEntry& earlier, const CutEntry& later) noexcept
{
    if (later.offset < earlier.offset)
    {
        return entries_unsorted;
    }

    const bool zeros_at_one_offset = earlier.kind == CutKind::zero && later.kind == CutKind::zero && later.offset == earlier.offset;

    if (std::uint64_t{later.offset.value} < covered_end(earlier) || zeros_at_one_offset)
    {
        return entries_overlap;
    }

    return m110::Status::success();
}

// Whether sorted, non-overlapping `entries` keep the drop balance at or
// below max_drop_balance_frames, else drop_run_too_long. Each DROP starts at
// or after the end of the DROP before it (check_order()), so the frames
// between them are never negative. In 64 bits, so no offset overflows.
[[nodiscard]] constexpr m110::Status check_drop_balance(std::span<const CutEntry> entries) noexcept
{
    std::uint64_t balance = 0U;
    std::uint64_t previous_end = 0U;

    for (const CutEntry& entry : entries)
    {
        if (entry.kind != CutKind::drop)
        {
            continue;
        }

        const std::uint64_t between = entry.offset.value - std::min<std::uint64_t>(previous_end, entry.offset.value);
        balance = balance - std::min(balance, between) + entry.length;

        if (balance > max_drop_balance_frames)
        {
            return drop_run_too_long;
        }

        previous_end = covered_end(entry);
    }

    return m110::Status::success();
}

class ValidSchedule;

[[nodiscard]] constexpr m110::Result<ValidSchedule> validate(const CutList& list) noexcept;

// A schedule validate() accepted, the only kind the stage latches, the
// player keeps pending and a playlist item holds. The empty schedule, which
// keeps every rule, is the default; validate() alone makes one with entries,
// so a list that breaks a rule cannot reach the stage.
class ValidSchedule
{
public:
    constexpr ValidSchedule() noexcept = default;

    [[nodiscard]] constexpr std::span<const CutEntry> entries() const noexcept { return list_.entries(); }

    [[nodiscard]] constexpr std::uint32_t size() const noexcept { return list_.size(); }

    [[nodiscard]] constexpr bool empty() const noexcept { return list_.empty(); }

    // The empty schedule again.
    constexpr void clear() noexcept { list_.clear(); }

private:
    friend constexpr m110::Result<ValidSchedule> validate(const CutList& list) noexcept;

    explicit constexpr ValidSchedule(const CutList& list) noexcept : list_{list} {}

    CutList list_{};
};

static_assert(!std::is_constructible_v<ValidSchedule, const CutList&> && !std::is_convertible_v<const CutList&, ValidSchedule>, "only validate() makes a schedule from a list");

// A schedule a play can use: each entry min_length to max_length frames long
// (else length_out_of_range), sorted and non-overlapping (check_order()),
// its DROPs within the drop balance (check_drop_balance()); a CutList never
// holds more than max_entries. Offsets are not judged here: an entry at or
// past the end of the play's output is ignored when the output ends.
[[nodiscard]] constexpr m110::Result<ValidSchedule> validate(const CutList& list) noexcept
{
    const auto entries = list.entries();
    const auto out_of_range = std::ranges::find_if(entries, [](const CutEntry& entry) { return entry.length < min_length || entry.length > max_length; });

    if (out_of_range != entries.end())
    {
        return length_out_of_range;
    }

    for (std::size_t index = 1U; index < entries.size(); ++index)
    {
        const auto ordered = check_order(entries[index - 1U], entries[index]);

        if (!ordered.is_ok())
        {
            return ordered;
        }
    }

    if (const auto balanced = check_drop_balance(entries); !balanced.is_ok())
    {
        return balanced;
    }

    return ValidSchedule{list};
}

// The CUT command's spelling (WFG-LIVE/1, docs/live-control-protocol.md):
// "CUT" alone clears the pending list; "CUT:" is followed by one to
// max_entries entries <offset>:<length>:<drop|zero>, separated by commas. Each
// number is canonical unsigned decimal (no sign, no leading zero) of at most
// 32 bits.
inline constexpr std::string_view command_name{"CUT"};
inline constexpr std::string_view entries_prefix{"CUT:"};
inline constexpr char entry_separator = ',';
inline constexpr char field_separator = ':';
inline constexpr std::string_view drop_name{"drop"};
inline constexpr std::string_view zero_name{"zero"};

// Whether `text` (a request without its sequence) is a CUT command, well
// formed or not.
[[nodiscard]] constexpr bool is_command(std::string_view text) noexcept
{
    return text == command_name || text.starts_with(entries_prefix);
}

// A canonical unsigned decimal of at most 32 bits, else bad_syntax.
[[nodiscard]] constexpr m110::Result<std::uint32_t> parse_uint32(std::string_view text) noexcept
{
    constexpr std::string_view digits{"0123456789"};
    constexpr std::uint64_t radix = digits.size();

    if (text.empty() || (text.size() > 1U && text.front() == digits.front()))
    {
        return bad_syntax;
    }

    std::uint64_t value = 0U;

    for (const char character : text)
    {
        const auto digit = digits.find(character);

        if (digit == std::string_view::npos)
        {
            return bad_syntax;
        }

        value = value * radix + digit;

        if (value > std::numeric_limits<std::uint32_t>::max())
        {
            return bad_syntax;
        }
    }

    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] constexpr m110::Result<CutKind> parse_kind(std::string_view text) noexcept
{
    if (text == drop_name)
    {
        return CutKind::drop;
    }

    if (text == zero_name)
    {
        return CutKind::zero;
    }

    return bad_syntax;
}

// One entry, <offset>:<length>:<drop|zero>, else bad_syntax.
[[nodiscard]] constexpr m110::Result<CutEntry> parse_entry(std::string_view text) noexcept
{
    const auto first = text.find(field_separator);
    const auto second = first == std::string_view::npos ? first : text.find(field_separator, first + 1U);

    if (second == std::string_view::npos)
    {
        return bad_syntax;
    }

    const auto offset = parse_uint32(text.substr(0U, first));
    const auto length = parse_uint32(text.substr(first + 1U, second - first - 1U));
    const auto kind = parse_kind(text.substr(second + 1U));

    if (!offset.has_value() || !length.has_value() || !kind.has_value())
    {
        return bad_syntax;
    }

    return CutEntry{.offset = OutputFrames{.value = offset.value()}, .length = length.value(), .kind = kind.value()};
}

// One to max_entries well-formed entries separated by commas, the part of a
// CUT command after "CUT:" (and a PLAYLIST item's cut= value), in their order
// (validate() judges them); bad_syntax for anything else.
[[nodiscard]] constexpr m110::Result<CutList> parse_entries(std::string_view text) noexcept
{
    CutList list{};
    auto rest = text;

    // One more pass than max_entries, so a 17th entry is refused by push().
    for (std::size_t parsed = 0U; parsed <= max_entries; ++parsed)
    {
        const auto separator = rest.find(entry_separator);
        const auto entry = parse_entry(rest.substr(0U, separator));

        if (!entry.has_value() || !list.push(entry.value()).is_ok())
        {
            return bad_syntax;
        }

        if (separator == std::string_view::npos)
        {
            return list;
        }

        rest.remove_prefix(separator + 1U);
    }

    return bad_syntax;
}

// The entries a CUT command gives, in its order (validate() judges them): none
// for "CUT". bad_syntax for anything else that is not "CUT:" and one to
// max_entries well-formed entries.
[[nodiscard]] constexpr m110::Result<CutList> parse_command(std::string_view text) noexcept
{
    if (text == command_name)
    {
        return CutList{};
    }

    if (!text.starts_with(entries_prefix))
    {
        return bad_syntax;
    }

    return parse_entries(text.substr(entries_prefix.size()));
}

// What the play does next with the engine's output.
enum class StepKind : std::uint8_t
{
    end,     // the output has ended, and every frame of it is sent or discarded
    pull,    // every frame on hand is used: the engine's next block is needed (offer()), or the end of its output (finish())
    send,    // send the next `frames` frames on hand
    zeros,   // send `frames` exact zero frames
    discard, // discard the next `frames` frames on hand (a DROP)
};

struct CutStep
{
    StepKind kind{StepKind::end};
    std::uint32_t frames{};

    constexpr bool operator==(const CutStep&) const noexcept = default;
};

// A step of no frames would never move the stage on (StepLimit::of()).
inline constexpr m110::Status no_step_frames{.code = m110::StatusCode::invalid_argument, .message = "CUT_STEP_ZERO"};

// The most frames one send or zeros step may cover: one at least, so each
// step moves the stage on. of() alone makes one, and refuses 0.
class StepLimit
{
public:
    // A limit of `frames` frames; no_step_frames for 0.
    [[nodiscard]] static constexpr m110::Result<StepLimit> of(std::uint32_t frames) noexcept
    {
        if (frames == 0U)
        {
            return no_step_frames;
        }

        return StepLimit{frames};
    }

    [[nodiscard]] constexpr std::uint32_t frames() const noexcept { return frames_; }

private:
    explicit constexpr StepLimit(std::uint32_t frames) noexcept : frames_{frames} {}

    std::uint32_t frames_{1U};
};

static_assert(!std::is_constructible_v<StepLimit, std::uint32_t> && !std::is_default_constructible_v<StepLimit>, "only of() makes a StepLimit");
static_assert(!StepLimit::of(0U).has_value() && StepLimit::of(1U).value().frames() == 1U, "a step limit is one frame at least");

// A block that would take the engine's output past 2^32 - 1 frames, where its
// places stop fitting 32 bits (CutStage::offer()).
inline constexpr m110::Status output_too_long{.code = m110::StatusCode::buffer_too_small, .message = "CUT_OUTPUT_LONG"};

// A cut as it took effect in the stream the play sends.
struct AppliedCut
{
    SentFrames position{};  // the stream's frames before it: a ZERO's first zero, or the first frame after a DROP, is frame `position`
    std::uint32_t frames{}; // a DROP's output frames discarded (cut short at the output's end); a ZERO's zero frames (its whole length)
    CutKind kind{};

    constexpr bool operator==(const AppliedCut&) const noexcept = default;
};

// The end of the engine's output (CutStage::finish()).
struct OutputEnd
{
    std::optional<AppliedCut> clipped{}; // the DROP under way, which the end cut short
    std::uint32_t past_end{};            // entries at or past the end, which never apply
};

// Walks the engine's output with the play's latched schedule, after the
// channel: the engine processes every card frame, and the stage decides what
// becomes of each frame it puts out. The player offers each block of output
// as next() asks for it (pull), asks next() for each step, carries it out and
// reports it with take(), in that order and with nothing in between; when
// next() asks for a block and the engine has none left, the player ends the
// output with finish(). A ZERO longer than the step limit goes out in several
// zeros steps; a DROP is discarded a block on hand at a time, and the
// output's end cuts it short. Without entries every step sends the frames on
// hand, at most the step limit's worth.
class CutStage
{
public:
    // The schedule of the next play; the stage starts over, with no output.
    constexpr void latch(const ValidSchedule& schedule) noexcept
    {
        *this = CutStage{};
        schedule_ = schedule;
    }

    // The engine's next block, `frames` frames long, follows the frames
    // offered so far, and the stage stands at its first; only once next()
    // asked for it. Refused with output_too_long, changing nothing, when the
    // output would pass 2^32 - 1 frames.
    [[nodiscard]] constexpr m110::Status offer(OutputFrames frames) noexcept
    {
        if (frames.value > std::numeric_limits<std::uint32_t>::max() - offered_.value)
        {
            return output_too_long;
        }

        offered_.value += frames.value;
        block_ = frames;
        return m110::Status::success();
    }

    // The engine's output ends with the frames offered: next() asked for a
    // block and the engine has none left. `stream` frames of the play's
    // stream were sent or queued before the end. A DROP under way ends here,
    // cut short; the entries not begun lie at or past the end and never
    // apply.
    [[nodiscard]] constexpr OutputEnd finish(SentFrames stream) noexcept
    {
        OutputEnd end{};
        const auto entry = pending_entry();

        if (entry.has_value() && entry->kind == CutKind::drop && entry->offset < position_)
        {
            end.clipped = AppliedCut{.position = stream, .frames = position_.value - entry->offset.value, .kind = CutKind::drop};
            ++next_;
        }

        end.past_end = schedule_.size() - next_;
        next_ = schedule_.size();
        finished_ = true;
        return end;
    }

    // The next step; a send or zeros step is at most `limit` frames long, a
    // discard step at most the frames on hand.
    [[nodiscard]] constexpr CutStep next(StepLimit limit) const noexcept
    {
        const std::uint32_t most = limit.frames();

        if (zeros_left_ != 0U)
        {
            return {.kind = StepKind::zeros, .frames = std::min(zeros_left_, most)};
        }

        const std::uint32_t on_hand = offered_.value - position_.value;

        if (on_hand == 0U)
        {
            return {.kind = finished_ ? StepKind::end : StepKind::pull};
        }

        const auto entry = pending_entry();

        if (entry.has_value() && entry->offset <= position_)
        {
            return begin(*entry, most, on_hand);
        }

        // The schedule is sorted and its entries do not overlap, so the next
        // entry's offset is never behind the stage but for the DROP under way
        // (begin()): the frames up to it never wrap.
        const std::uint32_t until = entry.has_value() ? std::min(entry->offset.value - position_.value, on_hand) : on_hand;
        return {.kind = StepKind::send, .frames = std::min(most, until)};
    }

    // `step`, the one next() gave, was carried out, with `stream` frames of
    // the play's stream sent or queued before it. Returns the cut it logs: a
    // ZERO as it begins, before its zeros; a DROP once its frames are
    // discarded, before the frame after them. A pull or end step changes
    // nothing here (offer(), finish()).
    [[nodiscard]] constexpr std::optional<AppliedCut> take(const CutStep& step, SentFrames stream) noexcept
    {
        switch (step.kind)
        {
        case StepKind::send:
            position_.value += step.frames;
            return std::nullopt;

        case StepKind::discard:
            return take_discard(step, stream);

        case StepKind::zeros:
            return take_zeros(step, stream);

        case StepKind::pull:
        case StepKind::end:
            return std::nullopt;
        }

        return std::nullopt;
    }

    // The output frames sent or discarded so far.
    [[nodiscard]] constexpr OutputFrames position() const noexcept { return position_; }

    // Where the next send or discard step begins in the last block offered:
    // its frames sent or discarded so far.
    [[nodiscard]] constexpr std::uint32_t block_position() const noexcept { return block_.value - (offered_.value - position_.value); }

private:
    // The next entry that may still apply: not begun, or the DROP under way.
    [[nodiscard]] constexpr std::optional<CutEntry> pending_entry() const noexcept
    {
        if (next_ >= schedule_.size())
        {
            return std::nullopt;
        }

        return schedule_.entries()[next_];
    }

    // The step of `entry`: a ZERO that starts where the stage stands, which
    // has a frame on hand there, or a DROP that starts there or began
    // behind it.
    [[nodiscard]] constexpr CutStep begin(const CutEntry& entry, std::uint32_t most, std::uint32_t on_hand) const noexcept
    {
        if (entry.kind == CutKind::zero)
        {
            return {.kind = StepKind::zeros, .frames = std::min(entry.length, most)};
        }

        const std::uint32_t left = entry.length - (position_.value - entry.offset.value);
        return {.kind = StepKind::discard, .frames = std::min(left, on_hand)};
    }

    // A discard step: the DROP under way ends once its last frame goes.
    [[nodiscard]] constexpr std::optional<AppliedCut> take_discard(const CutStep& step, SentFrames stream) noexcept
    {
        position_.value += step.frames;
        const auto entry = pending_entry();

        if (!entry.has_value() || position_.value - entry->offset.value < entry->length)
        {
            return std::nullopt;
        }

        ++next_;
        return AppliedCut{.position = stream, .frames = entry->length, .kind = CutKind::drop};
    }

    // A zeros step: the next part of the ZERO under way, or the first of the
    // pending one, which it begins.
    [[nodiscard]] constexpr std::optional<AppliedCut> take_zeros(const CutStep& step, SentFrames stream) noexcept
    {
        if (zeros_left_ != 0U)
        {
            zeros_left_ -= std::min(step.frames, zeros_left_);
            return std::nullopt;
        }

        const auto entry = pending_entry();

        if (!entry.has_value())
        {
            return std::nullopt;
        }

        ++next_;
        zeros_left_ = entry->length - std::min(step.frames, entry->length);
        return AppliedCut{.position = stream, .frames = entry->length, .kind = CutKind::zero};
    }

    ValidSchedule schedule_{};
    std::uint32_t next_{};       // the schedule's next entry: not begun, or the DROP under way
    OutputFrames offered_{};     // the engine's output so far
    OutputFrames block_{};       // the last block offered
    OutputFrames position_{};    // output frames sent or discarded
    std::uint32_t zeros_left_{}; // of the ZERO under way
    bool finished_{};            // the engine's output has ended
};

// What a play's cuts did within the frames a route confirmed sent: an exact
// prefix of the cut stream, as the host predicts it from the engine's output
// (the card itself without a scenario) and the schedule.
struct CutTally
{
    std::uint32_t applied{};           // cuts that took effect
    OutputFrames drop_frames{};        // output frames those DROPs discarded
    SentFrames zero_frames{};          // zero frames sent
    std::optional<SentFrames> first{}; // where the first cut took effect; none while none has
    std::uint32_t rejected{};          // entries at or past the output's end, ignored; once the whole stream went out
    SentFrames sent{};                 // the frames confirmed sent
    OutputFrames output_frames{};      // the output frames they cover: sent - zero_frames + drop_frames
};

// A cut the log has no room for: more cuts began than a schedule holds.
inline constexpr m110::Status log_full{.code = m110::StatusCode::buffer_too_small, .message = "CUT_LOG_FULL"};

// A play's cuts, in the order they took effect in its stream, the entries
// its output's end left unapplied, and where its stream ended.
class CutLog
{
public:
    // A new play: nothing applied, nothing ignored, no end yet.
    constexpr void restart() noexcept { *this = CutLog{}; }

    // The stage applied `cut`. A play applies each entry of its schedule at
    // most once, so a log of max_entries cuts never fills; a cut past that is
    // refused with log_full and changes nothing.
    [[nodiscard]] constexpr m110::Status record(const AppliedCut& cut) noexcept
    {
        if (count_ == cuts_.size())
        {
            return log_full;
        }

        std::span{cuts_}[count_] = cut;
        ++count_;
        return m110::Status::success();
    }

    // `entries` entries lie at or past the end of the play's output.
    constexpr void reject(std::uint32_t entries) noexcept { rejected_ += entries; }

    // The engine's output is done and every frame of it sent or discarded:
    // the play's stream ends after `stream` frames.
    constexpr void end(SentFrames stream) noexcept
    {
        end_ = stream;
        ended_ = true;
    }

    [[nodiscard]] constexpr std::span<const AppliedCut> applied() const noexcept { return std::span{cuts_}.first(count_); }

    // The cuts that took effect within the first `sent` frames of the stream.
    // The entries past the output's end count once the whole stream went
    // out: only then is the output's end within the frames sent.
    [[nodiscard]] constexpr CutTally tally(SentFrames sent) const noexcept
    {
        CutTally tally{.rejected = whole_stream(sent) ? rejected_ : 0U, .sent = sent};

        for (const AppliedCut& cut : applied())
        {
            if (took_effect(cut, sent))
            {
                count(cut, tally);
            }
        }

        tally.output_frames = OutputFrames{.value = sent.value - tally.zero_frames.value + tally.drop_frames.value};
        return tally;
    }

private:
    // Whether `sent` frames are the whole stream, once it has ended.
    [[nodiscard]] constexpr bool whole_stream(SentFrames sent) const noexcept { return ended_ && sent == end_; }

    // A cut took effect once a frame at or after its place went out; a DROP
    // that runs to the output's end, after which no frame follows, once the
    // whole stream went out.
    [[nodiscard]] constexpr bool took_effect(const AppliedCut& cut, SentFrames sent) const noexcept { return sent > cut.position || (whole_stream(sent) && cut.position == end_); }

    // Adds `cut`, which took effect, to `tally`: a ZERO with the zeros sent of it.
    static constexpr void count(const AppliedCut& cut, CutTally& tally) noexcept
    {
        ++tally.applied;

        if (!tally.first.has_value())
        {
            tally.first = cut.position;
        }

        if (cut.kind == CutKind::drop)
        {
            tally.drop_frames.value += cut.frames;
            return;
        }

        tally.zero_frames.value += std::min(cut.frames, tally.sent.value - cut.position.value);
    }

    std::array<AppliedCut, max_entries> cuts_{};
    std::size_t count_{};
    std::uint32_t rejected_{};
    SentFrames end_{};
    bool ended_{};
};

// The rules on a worked example: an erasure, then a ZERO where its DROP
// ends, is valid; an entry inside a DROP, a later offset first and a length
// past max_length are not.
[[nodiscard]] consteval bool rules_hold() noexcept
{
    constexpr OutputFrames at{.value = 10U};
    constexpr std::uint32_t length = 5U;
    constexpr OutputFrames end{.value = at.value + length};
    constexpr OutputFrames inside{.value = end.value - 1U};
    constexpr OutputFrames before{.value = at.value - 1U};
    const auto erasure = std::to_array<CutEntry>(
        {{.offset = at, .length = length, .kind = CutKind::zero}, {.offset = at, .length = length, .kind = CutKind::drop}, {.offset = end, .length = 1U, .kind = CutKind::zero}});
    const auto overlap = std::to_array<CutEntry>({{.offset = at, .length = length, .kind = CutKind::drop}, {.offset = inside, .length = 1U, .kind = CutKind::zero}});
    const auto disorder = std::to_array<CutEntry>({{.offset = at, .length = 1U, .kind = CutKind::zero}, {.offset = before, .length = 1U, .kind = CutKind::zero}});
    const auto too_long = std::to_array<CutEntry>({{.offset = at, .length = max_length + 1U, .kind = CutKind::zero}});
    // validate() on the entries as one list.
    const auto verdict = [](std::span<const CutEntry> entries) consteval
    {
        CutList list{};

        for (const CutEntry& entry : entries)
        {
            if (!list.push(entry).is_ok())
            {
                return too_many_entries;
            }
        }

        return validate(list).status();
    };
    return verdict(erasure).is_ok() && verdict(overlap).message == entries_overlap.message && verdict(disorder).message == entries_unsorted.message &&
           verdict(too_long).message == length_out_of_range.message;
}

// The stage and the log on a worked example: ten output frames handed over
// in blocks of 4 and 6, a ZERO of 2 before output frame 1, a DROP of frames 3
// to 5 across the blocks' edge, and a ZERO at the output's end, which never
// applies. The stream is frame 0, two zeros, frames 1 and 2, then 6 to 9.
[[nodiscard]] consteval bool stage_holds() noexcept
{
    constexpr std::uint32_t output_length = 10U;
    constexpr std::uint32_t stream_length = 9U;
    constexpr std::uint32_t drop_position = 5U;
    constexpr auto blocks = std::to_array<OutputFrames>({{.value = 4U}, {.value = 6U}});
    constexpr auto entries = std::to_array<CutEntry>({{.offset = {.value = 1U}, .length = 2U, .kind = CutKind::zero},
                                                      {.offset = {.value = 3U}, .length = 3U, .kind = CutKind::drop},
                                                      {.offset = {.value = output_length}, .length = 1U, .kind = CutKind::zero}});
    constexpr auto steps = std::to_array<CutStep>({{.kind = StepKind::pull},
                                                   {.kind = StepKind::send, .frames = 1U},
                                                   {.kind = StepKind::zeros, .frames = 2U},
                                                   {.kind = StepKind::send, .frames = 2U},
                                                   {.kind = StepKind::discard, .frames = 1U},
                                                   {.kind = StepKind::pull},
                                                   {.kind = StepKind::discard, .frames = 2U},
                                                   {.kind = StepKind::send, .frames = 4U},
                                                   {.kind = StepKind::pull},
                                                   {.kind = StepKind::end}});
    constexpr auto applied = std::to_array<AppliedCut>({{.position = {.value = 1U}, .frames = 2U, .kind = CutKind::zero}, {.position = {.value = drop_position}, .frames = 3U, .kind = CutKind::drop}});
    CutList list{};

    for (const CutEntry& entry : entries)
    {
        if (!list.push(entry).is_ok())
        {
            return false;
        }
    }

    const auto schedule = validate(list);

    if (!schedule.has_value())
    {
        return false;
    }

    CutStage stage{};
    CutLog log{};
    stage.latch(schedule.value());
    const auto limit = StepLimit::of(blocks.front().value).value();
    SentFrames stream{};
    std::size_t offered = 0U;

    for (const CutStep& expected : steps)
    {
        const auto step = stage.next(limit);

        if (step != expected)
        {
            return false;
        }

        if (step.kind == StepKind::pull && offered < blocks.size())
        {
            const auto block = std::span{blocks}[offered];
            ++offered;

            if (!stage.offer(block).is_ok())
            {
                return false;
            }
        }
        else if (step.kind == StepKind::pull)
        {
            log.reject(stage.finish(stream).past_end);
        }
        else if (const auto cut = stage.take(step, stream); cut.has_value() && !log.record(*cut).is_ok())
        {
            return false;
        }

        stream.value += step.kind == StepKind::send || step.kind == StepKind::zeros ? step.frames : 0U;
    }

    log.end(stream);
    const auto tally = log.tally(stream);
    return std::ranges::equal(log.applied(), applied) && stream.value == stream_length && tally.applied == applied.size() && tally.rejected == 1U && tally.output_frames.value == output_length &&
           stage.position().value == output_length;
}

// The CUT command and its checks, at compile time.
static_assert(parse_command("CUT").has_value() && parse_command("CUT").value().empty(), "CUT alone clears");
static_assert(parse_command("CUT:0:1:zero,4294967295:4800:drop").has_value() &&
                  parse_command("CUT:0:1:zero,4294967295:4800:drop").value().entries()[1] ==
                      CutEntry{.offset = OutputFrames{.value = std::numeric_limits<std::uint32_t>::max()}, .length = max_length, .kind = CutKind::drop},
              "offsets and lengths are 32-bit decimals");
static_assert(!parse_command("CUT:").has_value() && !parse_command("CUT:01:1:drop").has_value() && !parse_command("CUT:1:1:drop,").has_value() && !parse_command("CUT:1:1:DROP").has_value() &&
                  !parse_command("CUT:4294967296:1:drop").has_value() && !parse_command("CUT:1:1").has_value(),
              "only the canonical spelling parses");
static_assert(parse_entries("0:1:zero,9:2:drop").has_value() && parse_entries("0:1:zero,9:2:drop").value().size() == 2U && !parse_entries("").has_value() && !parse_entries("CUT:0:1:zero").has_value(),
              "the entries alone, as a playlist item's cut= gives them, parse by the same rules");
static_assert(rules_hold(), "an erasure is valid; overlaps, disorder and lengths outside 1 to 4800 are refused");

// The drop balance on DROPs of max_length: three adjacent ones reach it, a
// fourth passes it, unless max_length output frames come before the fourth;
// a ZERO between DROPs takes nothing back.
[[nodiscard]] consteval bool drop_balance_holds() noexcept
{
    const auto drop = [](std::uint32_t offset) consteval { return CutEntry{.offset = OutputFrames{.value = offset}, .length = max_length, .kind = CutKind::drop}; };
    const auto three = std::to_array({drop(0U), drop(max_length), drop(2U * max_length)});
    const auto four = std::to_array({drop(0U), drop(max_length), drop(2U * max_length), drop(3U * max_length)});
    const auto spaced = std::to_array({drop(0U), drop(max_length), drop(2U * max_length), drop(4U * max_length)});
    const auto close = std::to_array({drop(0U), drop(max_length), drop(2U * max_length), drop(4U * max_length - 1U)});
    const auto zero_between = std::to_array(
        {drop(0U), drop(max_length), drop(2U * max_length), CutEntry{.offset = OutputFrames{.value = 3U * max_length}, .length = max_length, .kind = CutKind::zero}, drop(3U * max_length)});
    return check_drop_balance(three).is_ok() && check_drop_balance(four).message == drop_run_too_long.message && check_drop_balance(spaced).is_ok() &&
           check_drop_balance(close).message == drop_run_too_long.message && check_drop_balance(zero_between).message == drop_run_too_long.message;
}

static_assert(drop_balance_holds(), "three adjacent DROPs of 4800 frames keep the drop balance; a fourth needs 4800 output frames before it");
static_assert(stage_holds(), "the stage sends, inserts and discards on the output axis, across a block's edge, and ignores an entry at the output's end");

} // namespace waveform_generator::cut
