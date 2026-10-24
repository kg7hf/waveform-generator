// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The CUT injector's portable logic (common/cut_schedule.hpp) on the host:
// the command's spelling and limits (16 entries of 32-bit offsets in one
// command), the schedule rules (sorted, non-overlapping, lengths 1 to 4800),
// the stage's steps over the engine's output for DROP and ZERO at its start,
// middle and end, at a block's edge and across blocks, mixed, adjacent, an
// erasure, lengths 1 and 4800, the most entries, entries past the output's
// end and an empty output, with the output handed over in blocks of every
// size (the player's engine blocks, blocks a sample slip lengthens, empties
// or shortens, a few frames, one frame) and steps of every limit, and the
// log's tally of every interesting prefix of the cut stream, all against a
// plain reference that applies a schedule to the output. Without cuts every
// step sends a whole engine block, as the player sent it before the injector.
// Only a schedule validate() accepted reaches the stage: the lists it
// refuses, and the stage's end, in a bounded number of steps, over random
// valid schedules and at the edges of the 32-bit offsets. A step limit is one
// frame at least, the log refuses a 17th cut, the stage refuses output past
// 2^32 - 1 frames, and the entries past the output's end count once the
// whole stream went out.
// With --predict <pcm24 data> <CUT command> [<sent frames>] it prints, as one
// JSON line, what the stage and the log make of an engine output (raw mono
// PCM24, as a WAV's data chunk holds it: a clean card, or a scenario's host
// render) handed over in engine blocks as the player hands it, for the Python
// predictor's cross-check (tests/test_cut_schedule.py).

#include "common/crc32.hpp"
#include "common/cut_schedule.hpp"
#include "common/pcm24.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
namespace cut = waveform_generator::cut;
namespace audio = waveform_generator::audio;

class Checks
{
public:
    void expect(bool condition, std::string_view what)
    {
        if (!condition)
        {
            ++failures_;
            std::fputs("FAIL: ", stdout);
            std::fputs(std::string{what}.c_str(), stdout);
            std::fputs("\n", stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

// An engine output the tests cut: a card played clean, or a scenario's.
using Output = std::vector<audio::Pcm24Sample>;

// The outputs the tests play: long enough for 4800-frame cuts at the start,
// middle and end, and samples counting up from 1, so an inserted zero never
// looks like an output frame and every output frame tells where it came from.
constexpr std::uint32_t output_length = 20000U;
constexpr std::uint32_t middle = output_length / 2U;
constexpr std::uint32_t last_frame = output_length - 1U;
// The player's blocks: the engine's output of one card block, and the most a
// block of sample slips puts out, which is also the player's step limit
// (player.cpp output_step_frames; signal_lab::engine_block_frames and
// engine_capacity_frames). The cut stream does not depend on either, which
// the tests below show over every block size and limit.
constexpr std::uint32_t player_block = 2048U;
constexpr std::uint32_t player_step = 2304U;
// Blocks and limits that split the stage's steps more finely than any cut.
constexpr std::uint32_t odd_block = 7U;
constexpr std::uint32_t frame_block = 1U;
constexpr auto limits = std::to_array<std::uint32_t>({player_step, odd_block, frame_block});

constexpr std::uint32_t short_length = 100U;
constexpr std::uint32_t erasure_length = 480U;
constexpr std::uint32_t entry_spacing = 1000U;

// How a test hands the output to the stage: blocks of these sizes in turn
// (the last one cut to the output's end).
struct BlockPattern
{
    std::string_view name;
    std::vector<std::uint32_t> sizes;
};

[[nodiscard]] std::vector<BlockPattern> block_patterns()
{
    return {
        {.name = "engine blocks", .sizes = {player_block}},
        {.name = "blocks a sample slip lengthens, empties and shortens", .sizes = {player_block, player_step, 0U, player_block - 1U}},
        {.name = "blocks of 7", .sizes = {odd_block}},
        {.name = "blocks of 1", .sizes = {frame_block}},
    };
}

[[nodiscard]] Output make_output(std::uint32_t frames)
{
    Output output(frames);
    std::ranges::iota(output, audio::Pcm24Sample{1});
    return output;
}

// A count the tests hold in a container, as the cut module counts frames.
[[nodiscard]] std::uint32_t frames_of(std::size_t count) noexcept
{
    return static_cast<std::uint32_t>(count);
}

// A test's limit as the stage takes it. Every limit a test gives is a frame
// at least; value() of a refused one would trap and end the run.
[[nodiscard]] cut::StepLimit limit_of(std::uint32_t frames) noexcept
{
    return cut::StepLimit::of(frames).value();
}

[[nodiscard]] constexpr cut::CutEntry drop(std::uint32_t offset, std::uint32_t length) noexcept
{
    return {.offset = cut::OutputFrames{.value = offset}, .length = length, .kind = cut::CutKind::drop};
}

[[nodiscard]] constexpr cut::CutEntry zero(std::uint32_t offset, std::uint32_t length) noexcept
{
    return {.offset = cut::OutputFrames{.value = offset}, .length = length, .kind = cut::CutKind::zero};
}

// The test's entries as a list; one that does not fit a CutList (more than 16
// entries) fails the run rather than lose the entries past the 16th.
[[nodiscard]] cut::CutList list_of(Checks& checks, std::span<const cut::CutEntry> entries)
{
    cut::CutList list{};

    for (const auto& entry : entries)
    {
        checks.expect(list.push(entry).is_ok(), "the test's schedule fits a CUT list");
    }

    return list;
}

// The test's entries as the schedule validate() makes of them; entries that
// break a rule fail the run, and the stage gets the empty schedule.
[[nodiscard]] cut::ValidSchedule valid_of(Checks& checks, std::span<const cut::CutEntry> entries)
{
    const auto schedule = cut::validate(list_of(checks, entries));
    checks.expect(schedule.has_value(), "the test's schedule is valid");
    return schedule.has_value() ? schedule.value() : cut::ValidSchedule{};
}

// What a play of `output` with `entries` sends, built the plain way: the
// output's frames up to each entry in turn, then the entry's zeros, or the
// output again past its dropped frames (clipped at the output's end).
struct CutStream
{
    Output frames;
    std::vector<cut::AppliedCut> applied;
    std::uint32_t rejected{};
};

[[nodiscard]] CutStream reference_stream(std::span<const audio::Pcm24Sample> output, std::span<const cut::CutEntry> entries)
{
    CutStream stream;
    std::size_t at = 0U;

    for (const auto& entry : entries)
    {
        const std::size_t offset = entry.offset.value;

        if (offset >= output.size())
        {
            ++stream.rejected;
            continue;
        }

        const auto kept = output.subspan(at, offset - at);
        stream.frames.insert(stream.frames.end(), kept.begin(), kept.end());
        at = offset;
        const cut::SentFrames position{.value = frames_of(stream.frames.size())};

        if (entry.kind == cut::CutKind::zero)
        {
            stream.frames.insert(stream.frames.end(), entry.length, audio::Pcm24Sample{});
            stream.applied.push_back({.position = position, .frames = entry.length, .kind = cut::CutKind::zero});
            continue;
        }

        const auto discarded = std::min<std::size_t>(entry.length, output.size() - at);
        at += discarded;
        stream.applied.push_back({.position = position, .frames = frames_of(discarded), .kind = cut::CutKind::drop});
    }

    const auto rest = output.subspan(at);
    stream.frames.insert(stream.frames.end(), rest.begin(), rest.end());
    return stream;
}

// The tally of the first `sent` frames by the documented rule: a cut counts
// once a frame at or after its place went out, or, at the stream's end, once
// the whole stream went out; a ZERO counts the zeros sent of it; the entries
// past the output's end count once the whole stream went out.
[[nodiscard]] cut::CutTally reference_tally(const CutStream& stream, std::uint32_t sent)
{
    const auto end = frames_of(stream.frames.size());
    cut::CutTally tally{.rejected = sent == end ? stream.rejected : 0U, .sent = cut::SentFrames{.value = sent}};

    for (const auto& applied : stream.applied)
    {
        const auto position = applied.position.value;

        if (sent <= position && !(sent == end && position == end))
        {
            continue;
        }

        ++tally.applied;
        tally.first = tally.first.value_or(applied.position);

        if (applied.kind == cut::CutKind::drop)
        {
            tally.drop_frames.value += applied.frames;
        }
        else
        {
            tally.zero_frames.value += std::min(applied.frames, sent - position);
        }
    }

    tally.output_frames.value = sent - tally.zero_frames.value + tally.drop_frames.value;
    return tally;
}

[[nodiscard]] bool same_tally(const cut::CutTally& got, const cut::CutTally& want) noexcept
{
    return got.applied == want.applied && got.drop_frames == want.drop_frames && got.zero_frames == want.zero_frames && got.first == want.first && got.rejected == want.rejected &&
           got.sent == want.sent && got.output_frames == want.output_frames;
}

// The blocks `pattern` hands over for an output of `frames` frames, the
// empty ones included: what the stage asks for before the output's end.
[[nodiscard]] std::uint64_t blocks_for(std::uint64_t frames, std::span<const std::uint32_t> pattern) noexcept
{
    std::uint64_t blocks = 0U;

    for (std::uint64_t offered = 0U; offered < frames; ++blocks)
    {
        offered += std::span{pattern}[blocks % pattern.size()];
    }

    return blocks;
}

// The most steps the stage may take over `frames` frames handed over in
// `pattern`'s blocks, with steps of at most `limit`: a pull for each block and
// one at the end; sends that each end at the limit, a block's end or an
// entry; each ZERO's zeros steps; discards that each end at a block's end or
// the DROP's; the end.
[[nodiscard]] std::uint64_t most_steps(std::span<const cut::CutEntry> entries, std::uint64_t frames, std::span<const std::uint32_t> pattern, std::uint64_t limit) noexcept
{
    const auto whole_steps = [limit](std::uint64_t count) { return (count + limit - 1U) / limit; };

    const auto blocks = blocks_for(frames, pattern);
    std::uint64_t steps = 3U * blocks + whole_steps(frames) + 2U * entries.size() + 2U;

    for (const auto& entry : entries)
    {
        steps += entry.kind == cut::CutKind::zero ? whole_steps(entry.length) : 0U;
    }

    return steps;
}

// The stage over `output` handed over in `pattern`'s blocks with steps of at
// most `limit`, as the player runs it: each block offered when the stage asks
// for one, each step taken (its cut logged) before its frames are sent, the
// output's end once every frame is offered.
struct StageRun
{
    Output frames;
    std::vector<cut::CutStep> steps;
    cut::CutLog log;
    std::uint32_t unlogged{};       // cuts the log refused
    std::uint32_t refused_blocks{}; // blocks the stage refused
    std::uint32_t misplaced{};      // steps whose block_position() is not where they begin in the block on hand
    bool ended{};
    std::size_t offered_blocks{}; // blocks handed over so far, empty ones included
    std::size_t block_start{};    // where the last block handed over begins in the output
    std::size_t block_end{};      // and where it ends
};

// Hands the stage `output`'s next block, which it asked for, or ends the
// output once every frame of it is offered.
void hand_over(cut::CutStage& stage, StageRun& run, std::span<const audio::Pcm24Sample> output, std::span<const std::uint32_t> pattern)
{
    if (run.block_end == output.size())
    {
        const auto end = stage.finish(cut::SentFrames{.value = frames_of(run.frames.size())});

        if (end.clipped.has_value() && !run.log.record(*end.clipped).is_ok())
        {
            ++run.unlogged;
        }

        run.log.reject(end.past_end);
        return;
    }

    const auto size = std::min<std::size_t>(std::span{pattern}[run.offered_blocks % pattern.size()], output.size() - run.block_end);
    ++run.offered_blocks;
    run.refused_blocks += stage.offer(cut::OutputFrames{.value = frames_of(size)}).is_ok() ? 0U : 1U;
    run.block_start = run.block_end;
    run.block_end += size;
}

// Carries out a send, zeros or discard step on the run's stream.
void carry_out(cut::CutStage& stage, StageRun& run, std::span<const audio::Pcm24Sample> output, const cut::CutStep& step)
{
    const auto at = stage.position().value;
    run.misplaced += stage.block_position() == at - run.block_start ? 0U : 1U;
    const auto applied = stage.take(step, cut::SentFrames{.value = frames_of(run.frames.size())});

    if (applied.has_value() && !run.log.record(*applied).is_ok())
    {
        ++run.unlogged;
    }

    if (step.kind == cut::StepKind::send)
    {
        const auto frames = output.subspan(at, step.frames);
        run.frames.insert(run.frames.end(), frames.begin(), frames.end());
    }
    else if (step.kind == cut::StepKind::zeros)
    {
        run.frames.insert(run.frames.end(), step.frames, audio::Pcm24Sample{});
    }
}

[[nodiscard]] StageRun run_stage(std::span<const audio::Pcm24Sample> output, const cut::ValidSchedule& schedule, std::span<const std::uint32_t> pattern, std::uint32_t limit)
{
    StageRun run;
    cut::CutStage stage;
    stage.latch(schedule);
    const auto step_limit = most_steps(schedule.entries(), output.size(), pattern, limit);
    const auto step_most = limit_of(limit);

    while (run.steps.size() < step_limit)
    {
        const auto step = stage.next(step_most);
        run.steps.push_back(step);

        if (step.kind == cut::StepKind::end)
        {
            run.log.end(cut::SentFrames{.value = frames_of(run.frames.size())});
            run.ended = true;
            return run;
        }

        if (step.kind == cut::StepKind::pull)
        {
            hand_over(stage, run, output, pattern);
            continue;
        }

        carry_out(stage, run, output, step);
    }

    return run;
}

// The prefixes worth tallying: none, one frame, the whole stream and its last
// frame left out, and around each cut's place (before it, at it, past it and
// halfway through a ZERO's zeros).
[[nodiscard]] std::vector<std::uint32_t> interesting_prefixes(const CutStream& stream)
{
    const auto end = frames_of(stream.frames.size());
    std::vector<std::uint32_t> prefixes{0U, 1U, end - std::min(end, 1U), end};

    for (const auto& applied : stream.applied)
    {
        const auto position = applied.position.value;

        for (const std::uint32_t prefix : {position - std::min(position, 1U), position, position + 1U, position + applied.frames / 2U})
        {
            prefixes.push_back(std::min(prefix, end));
        }
    }

    return prefixes;
}

struct StageCase
{
    std::string_view name;
    std::vector<cut::CutEntry> entries;
};

// Sixteen entries, alternately ZERO and DROP, one every entry_spacing frames,
// with lengths 1, 2, 3 and so on.
[[nodiscard]] std::vector<cut::CutEntry> most_entries()
{
    std::vector<cut::CutEntry> entries;

    for (std::uint32_t index = 0U; index < cut::max_entries; ++index)
    {
        const auto offset = index * entry_spacing;
        entries.push_back(index % 2U == 0U ? zero(offset, index + 1U) : drop(offset, index + 1U));
    }

    return entries;
}

[[nodiscard]] std::vector<StageCase> stage_cases()
{
    return {
        {.name = "no cuts", .entries = {}},
        {.name = "DROP at the start", .entries = {drop(0U, short_length)}},
        {.name = "DROP in the middle", .entries = {drop(middle, short_length)}},
        {.name = "DROP clipped at the end", .entries = {drop(output_length - short_length / 2U, short_length)}},
        {.name = "DROP of the last frames", .entries = {drop(output_length - short_length, short_length)}},
        {.name = "ZERO at the start", .entries = {zero(0U, short_length)}},
        {.name = "ZERO in the middle", .entries = {zero(middle, short_length)}},
        {.name = "ZERO before the last frame", .entries = {zero(last_frame, short_length)}},
        {.name = "an erasure: ZERO then DROP at one offset", .entries = {zero(middle, erasure_length), drop(middle, erasure_length)}},
        {.name = "adjacent DROP, ZERO and DROPs",
         .entries = {drop(entry_spacing, short_length), zero(entry_spacing + short_length, short_length), drop(entry_spacing + short_length, short_length),
                     drop(entry_spacing + 2U * short_length, 1U)}},
        {.name = "mixed lengths 1 and 4800",
         .entries = {zero(0U, 1U), drop(1U, 1U), drop(short_length, cut::max_length), zero(short_length + cut::max_length, cut::max_length), drop(middle, 1U), zero(last_frame, 1U)}},
        {.name = "ZEROs of a step and more", .entries = {zero(player_block, player_step), zero(middle, player_step + 1U)}},
        {.name = "a ZERO and an erasure at an engine block's edge", .entries = {zero(player_block, 1U), zero(2U * player_block, short_length), drop(2U * player_block, player_block)}},
        {.name = "a DROP across three engine blocks", .entries = {drop(player_block - short_length, cut::max_length)}},
        {.name = "the most entries", .entries = most_entries()},
        {.name = "entries past the end", .entries = {zero(last_frame - short_length, short_length), zero(output_length, 1U), drop(output_length + short_length, 1U)}},
        {.name = "a DROP clipped at the end, then an entry past it", .entries = {drop(output_length - short_length / 2U, short_length), zero(output_length + short_length, 1U)}},
    };
}

// The stage's run sends the reference cut stream and logs its cuts, no send
// or zeros step exceeds the limit and no discard the largest block, and the
// tally of every interesting prefix is the reference's.
void check_run(Checks& checks, const StageRun& run, const CutStream& expected, std::span<const std::uint32_t> pattern, std::uint32_t limit, const std::string& name)
{
    const auto largest_block = std::ranges::max(pattern);
    checks.expect(run.ended && run.frames == expected.frames && run.unlogged == 0U && run.refused_blocks == 0U && run.misplaced == 0U,
                  name + ": the stage sends the reference cut stream, each step where block_position() says");
    checks.expect(std::ranges::equal(run.log.applied(), expected.applied), name + ": the log holds each cut where it takes effect");
    const bool within =
        std::ranges::all_of(run.steps, [limit, largest_block](const cut::CutStep& step) { return step.kind == cut::StepKind::discard ? step.frames <= largest_block : step.frames <= limit; });

    checks.expect(within, name + ": no send or zeros step exceeds the limit, no discard the block on hand");

    for (const std::uint32_t sent : interesting_prefixes(expected))
    {
        checks.expect(same_tally(run.log.tally(cut::SentFrames{.value = sent}), reference_tally(expected, sent)), name + ": the tally of the prefix of " + std::to_string(sent) + " frames");
    }
}

void check_stage_case(Checks& checks, const StageCase& item, std::span<const audio::Pcm24Sample> output)
{
    const auto schedule = cut::validate(list_of(checks, item.entries));
    const auto expected = reference_stream(output, item.entries);
    const std::string name{item.name};
    checks.expect(schedule.has_value(), name + ": the schedule is valid");

    if (!schedule.has_value())
    {
        return;
    }

    for (const auto& pattern : block_patterns())
    {
        for (const std::uint32_t limit : limits)
        {
            const auto run = run_stage(output, schedule.value(), pattern.sizes, limit);
            check_run(checks, run, expected, pattern.sizes, limit, name + " in " + std::string{pattern.name} + ", steps of " + std::to_string(limit));
        }
    }

    const auto whole = reference_tally(expected, frames_of(expected.frames.size()));
    checks.expect(whole.output_frames.value == output.size() && whole.applied == expected.applied.size(), name + ": the whole stream covers the whole output, every applicable cut applied");
}

void test_stage(Checks& checks)
{
    const auto output = make_output(output_length);

    for (const auto& item : stage_cases())
    {
        check_stage_case(checks, item, output);
    }

    const auto past = reference_stream(output, stage_cases().back().entries);
    checks.expect(past.rejected == 1U && past.applied.size() == 1U && past.applied.front().frames == short_length / 2U, "a DROP clipped at the end applies; the entry after it is past the end");
    const Output empty{};
    const auto none = run_stage(empty, valid_of(checks, std::to_array({zero(0U, 1U), drop(0U, 1U)})), std::to_array({player_block}), player_step);
    checks.expect(none.ended && none.frames.empty() && none.steps.size() == 2U && none.log.tally(cut::SentFrames{}).rejected == 2U,
                  "an empty output: every entry past its end, and the stage ends at its first block");
}

// Without cuts every step sends a whole engine block, then the output's rest,
// as the player sent its file before the injector; the erasure sends its
// zeros a step at a time and discards its DROP block by block.
void test_steps(Checks& checks)
{
    constexpr std::uint32_t frames = 3U * player_block + short_length;
    const auto output = make_output(frames);
    const auto blocks = std::to_array({player_block});
    const cut::CutStep pull{.kind = cut::StepKind::pull};
    const cut::CutStep end{.kind = cut::StepKind::end};
    const cut::CutStep whole_block{.kind = cut::StepKind::send, .frames = player_block};
    const auto plain = run_stage(output, cut::ValidSchedule{}, blocks, player_step);
    const std::vector<cut::CutStep> whole_blocks{pull, whole_block, pull, whole_block, pull, whole_block, pull, {.kind = cut::StepKind::send, .frames = short_length}, pull, end};
    checks.expect(plain.steps == whole_blocks && plain.frames == output, "without cuts: three whole blocks, then the rest");
    const auto erasure = run_stage(output, valid_of(checks, std::to_array({zero(short_length, cut::max_length), drop(short_length, cut::max_length)})), blocks, player_step);
    // The DROP's frames: the first block's rest, a whole block, then the rest of its length.
    constexpr std::uint32_t first_part = player_block - short_length;
    constexpr std::uint32_t last_part = cut::max_length - first_part - player_block;
    const std::vector<cut::CutStep> steps{pull,
                                          {.kind = cut::StepKind::send, .frames = short_length},
                                          {.kind = cut::StepKind::zeros, .frames = player_step},
                                          {.kind = cut::StepKind::zeros, .frames = player_step},
                                          {.kind = cut::StepKind::zeros, .frames = cut::max_length - 2U * player_step},
                                          {.kind = cut::StepKind::discard, .frames = first_part},
                                          pull,
                                          {.kind = cut::StepKind::discard, .frames = player_block},
                                          pull,
                                          {.kind = cut::StepKind::discard, .frames = last_part},
                                          {.kind = cut::StepKind::send, .frames = player_block - last_part},
                                          pull,
                                          {.kind = cut::StepKind::send, .frames = short_length},
                                          pull,
                                          end};
    const auto kinds = [](const StageRun& run)
    {
        std::vector<cut::CutKind> begun;
        begun.reserve(run.log.applied().size());

        for (const auto& applied : run.log.applied())
        {
            begun.push_back(applied.kind);
        }

        return begun;
    };

    checks.expect(erasure.steps == steps, "an erasure: sends up to it, the zeros in steps, the DROP block by block, then the rest");
    checks.expect(kinds(erasure) == std::vector<cut::CutKind>{cut::CutKind::zero, cut::CutKind::drop} && erasure.frames.size() == output.size(),
                  "the erasure logs its ZERO, then its DROP, once each, and keeps the stream's length");
}

// The CUT command's spelling: canonical decimals, the two kinds in lower
// case, commas between entries and nothing else. The widest whole schedule
// parses; a 17th entry does not.
void test_command(Checks& checks)
{
    const auto cleared = cut::parse_command("CUT");
    checks.expect(cleared.has_value() && cleared.value().empty(), "CUT alone clears");
    constexpr std::uint32_t one_second = 48000U;
    constexpr std::uint32_t two_seconds = 96000U;
    const auto two = cut::parse_command("CUT:48000:4800:drop,96000:1:zero");
    const auto expected = std::to_array({drop(one_second, cut::max_length), zero(two_seconds, 1U)});
    checks.expect(two.has_value() && std::ranges::equal(two.value().entries(), expected), "two entries in their order");
    std::string widest{cut::entries_prefix};
    std::string seventeen{cut::entries_prefix};
    std::vector<cut::CutEntry> widest_entries;

    for (std::size_t index = 0U; index < cut::max_entries; ++index)
    {
        widest += (index == 0U ? "" : ",") + std::string{"4294967295:4800:drop"};
        seventeen += "1:1:zero,";
        widest_entries.push_back(drop(std::numeric_limits<std::uint32_t>::max(), cut::max_length));
    }

    seventeen += "1:1:zero";
    const auto parsed = cut::parse_command(widest);
    checks.expect(parsed.has_value() && std::ranges::equal(parsed.value().entries(), widest_entries), "16 entries of 32-bit offsets parse");
    checks.expect(!cut::parse_command(seventeen).has_value(), "a 17th entry is refused");

    for (const std::string_view bad : {"CUT:",          "CUT:,",         "CUT:1:1:drop,", "CUT:,1:1:drop", "CUT:1:1:drop,,2:1:drop", "CUT:01:1:drop",         "CUT:1:01:drop",
                                       "CUT:1:1:Drop",  "CUT:1:1:DROP",  "CUT:1:1:zeros", "CUT:1:1",       "CUT:1:1:drop:2",         "CUT:1::drop",           "CUT::1:drop",
                                       "CUT:-1:1:drop", "CUT:+1:1:drop", "CUT:1 :1:drop", "CUT: 1:1:drop", "CUT:4294967296:1:drop",  "CUT:1:4294967296:drop", "CUT 1:1:drop",
                                       "CUTS",          "cut",           "CUT:1:1:drop "})
    {
        checks.expect(!cut::parse_command(bad).has_value(), "refused: " + std::string{bad});
    }

    checks.expect(cut::is_command("CUT") && cut::is_command("CUT:") && !cut::is_command("CUTS") && !cut::is_command("PLAY"), "what counts as a CUT command");
    const auto lengths = cut::parse_command("CUT:1:0:drop,2:4801:zero");
    checks.expect(lengths.has_value() && lengths.value().size() == 2U, "lengths outside 1 to 4800 parse, for validate() to refuse");
}

// The rules a schedule must keep.
void test_rules(Checks& checks)
{
    const auto valid = [&checks](std::initializer_list<cut::CutEntry> entries) { return cut::validate(list_of(checks, std::span{entries.begin(), entries.size()})).status(); };

    const auto refused = [&valid](std::initializer_list<cut::CutEntry> entries, const m110::Status& status) { return std::string_view{valid(entries).message} == status.message; };

    // An entry at `at`, and a DROP there of `length` frames, which ends at `end`.
    constexpr std::uint32_t at = short_length;
    constexpr std::uint32_t length = odd_block;
    constexpr std::uint32_t end = at + length;
    checks.expect(valid({}).is_ok() && valid({drop(0U, 1U)}).is_ok() && valid({zero(0U, cut::max_length)}).is_ok(), "empty, length 1 and length 4800");
    checks.expect(refused({drop(0U, 0U)}, cut::length_out_of_range) && refused({zero(0U, cut::max_length + 1U)}, cut::length_out_of_range), "lengths 0 and 4801 are CUT_RANGE");
    checks.expect(refused({zero(at, 1U), zero(at - 1U, 1U)}, cut::entries_unsorted) && refused({drop(at, 1U), drop(at - 1U, 1U)}, cut::entries_unsorted), "a later offset first is CUT_UNSORTED");
    checks.expect(refused({drop(at, length), zero(end - 1U, 1U)}, cut::entries_overlap) && refused({drop(at, length), drop(end - 1U, 1U)}, cut::entries_overlap),
                  "an entry inside a DROP is CUT_OVERLAP");
    checks.expect(refused({drop(at, length), zero(at, 1U)}, cut::entries_overlap), "a ZERO before a frame its DROP removed is CUT_OVERLAP");
    checks.expect(refused({zero(at, length), zero(at, 1U)}, cut::entries_overlap), "two ZEROs at one offset are CUT_OVERLAP");
    checks.expect(valid({zero(at, length), drop(at, length)}).is_ok() && valid({drop(at, length), zero(end, length)}).is_ok() && valid({drop(at, length), drop(end, length)}).is_ok() &&
                      valid({zero(at, length), zero(at + 1U, length)}).is_ok(),
                  "an erasure and adjacent entries are valid");
    // The drop balance: DROPs of max_length one after another, a ZERO among them taking nothing back.
    constexpr std::uint32_t most = cut::max_length;
    checks.expect(valid({drop(at, most), drop(at + most, most), drop(at + 2U * most, most)}).is_ok() &&
                      refused({drop(at, most), drop(at + most, most), drop(at + 2U * most, most), drop(at + 3U * most, 1U)}, cut::drop_run_too_long) &&
                      refused({drop(at, most), drop(at + most, most), drop(at + 2U * most, most), zero(at + 3U * most, most), drop(at + 3U * most, 1U)}, cut::drop_run_too_long),
                  "three adjacent DROPs of 4800 frames keep the drop balance; one more DROP frame is CUT_DROP_RUN, a ZERO between them or not");
    checks.expect(valid({drop(at, most), drop(at + most, most), drop(at + 2U * most, most), drop(at + 3U * most + 1U, 1U)}).is_ok() &&
                      valid({drop(at, most), drop(at + most, most), drop(at + 2U * most, most), drop(at + 4U * most, most)}).is_ok(),
                  "the output frames between DROPs take the balance back, frame for frame");
    checks.expect(valid({drop(std::numeric_limits<std::uint32_t>::max(), cut::max_length), zero(std::numeric_limits<std::uint32_t>::max(), 1U)}).message == cut::entries_overlap.message,
                  "a DROP's end past 32 bits never wraps");
    const auto list = list_of(checks, most_entries());
    checks.expect(list.size() == cut::max_entries && cut::validate(list).has_value(), "16 entries are valid");
    cut::CutList full = list;
    checks.expect(std::string_view{full.push(zero(output_length, 1U)).message} == cut::too_many_entries.message && full.size() == cut::max_entries,
                  "a full list refuses a 17th entry and keeps its 16");
}

// The stage latches only what validate() made: a list cannot reach it, the
// player's pending list or a playlist item unjudged.
static_assert(!std::is_invocable_v<decltype(&cut::CutStage::latch), cut::CutStage&, const cut::CutList&>, "the stage latches no unjudged list");

// The lists that, latched unjudged, once ran the reader past its file's end
// without ever ending (QA-001): validate() refuses each and makes no schedule.
void test_refused_lists(Checks& checks)
{
    constexpr std::uint32_t early = 100U;
    constexpr std::uint32_t inside = 120U;
    constexpr std::uint32_t late = 500U;
    constexpr std::uint32_t length = 5U;
    constexpr std::uint32_t long_drop = 50U;
    struct Refused
    {
        std::string_view name;
        std::vector<cut::CutEntry> entries;
        std::string_view code;
    };

    const std::vector<Refused> refused{
        {.name = "a later offset first", .entries = {zero(late, length), zero(early, length)}, .code = cut::entries_unsorted.message},
        {.name = "a ZERO inside a DROP", .entries = {drop(early, long_drop), zero(inside, length)}, .code = cut::entries_overlap.message},
        {.name = "a DROP inside a DROP", .entries = {drop(early, long_drop), drop(inside, long_drop)}, .code = cut::entries_overlap.message},
        {.name = "a ZERO of 2^32 - 1 frames", .entries = {zero(early, std::numeric_limits<std::uint32_t>::max())}, .code = cut::length_out_of_range.message},
    };

    for (const auto& item : refused)
    {
        const auto schedule = cut::validate(list_of(checks, item.entries));
        checks.expect(!schedule.has_value() && std::string_view{schedule.status().message} == item.code, std::string{item.name} + " is " + std::string{item.code} + " and makes no schedule");
    }
}

// Random lists for the property test, the same on every run: up to 16
// entries, each at or after the place the one before it ends, of lengths 1,
// 4800 or between, some past the output's end. Now and then one breaks a rule
// (an offset behind the last entry's end, a length of 0 or 4801, a ZERO at the
// offset of the ZERO before it), so validate() refuses some lists.
class ListSource
{
public:
    // A number below `bound`, which is not 0.
    [[nodiscard]] std::uint32_t below(std::uint32_t bound) { return std::uniform_int_distribution<std::uint32_t>{0U, bound - 1U}(engine_); }

    [[nodiscard]] bool one_in(std::uint32_t odds) { return below(odds) == 0U; }

    [[nodiscard]] std::vector<cut::CutEntry> entries(std::uint32_t frames)
    {
        const std::uint32_t count = below(cut::max_entries + 1U);
        // Room between entries, so a list spans the output and runs past it now and then.
        const std::uint32_t spread = frames / cut::max_entries * 2U + 2U;
        std::uint32_t from = below(spread);
        std::vector<cut::CutEntry> entries;

        for (std::uint32_t index = 0U; index < count; ++index)
        {
            const bool is_zero = one_in(2U);
            const std::uint32_t length = next_length();
            std::uint32_t offset = from + (one_in(4U) ? 0U : below(spread));

            if (one_in(rule_odds))
            {
                offset -= std::min(offset, 1U + below(short_length));
            }

            entries.push_back(is_zero ? zero(offset, length) : drop(offset, length));
            from = is_zero ? offset + below(2U) : offset + length;
        }

        return entries;
    }

    // A block pattern: one of the tests' own, or blocks of one random size.
    [[nodiscard]] std::vector<std::uint32_t> pattern()
    {
        constexpr std::uint32_t largest_block = 3000U;
        const auto patterns = block_patterns();
        const auto choice = below(frames_of(patterns.size()) + 1U);
        return choice < patterns.size() ? patterns.at(choice).sizes : std::vector<std::uint32_t>{1U + below(largest_block)};
    }

    // A step limit: the player's, a few frames, one frame, or anything up to largest_limit.
    [[nodiscard]] std::uint32_t limit()
    {
        constexpr std::uint32_t largest_limit = 3000U;
        const auto choices = std::to_array<std::uint32_t>({player_step, odd_block, frame_block, 1U + below(largest_limit)});
        return choices.at(below(frames_of(choices.size())));
    }

private:
    static constexpr std::uint32_t list_seed = 20261008U;
    static constexpr std::uint32_t rule_odds = 40U;
    // Of every length_kinds lengths, one is 0, one 4801, edge_lengths are 1
    // and as many 4800, and the rest anything from 1 to 4800.
    static constexpr std::uint32_t length_kinds = 2U * rule_odds;
    static constexpr std::uint32_t edge_lengths = 8U;

    [[nodiscard]] std::uint32_t next_length()
    {
        const std::uint32_t kind = below(length_kinds);

        if (kind < 2U)
        {
            return kind == 0U ? cut::min_length - 1U : cut::max_length + 1U;
        }

        if (kind < 2U + 2U * edge_lengths)
        {
            return kind % 2U == 0U ? cut::min_length : cut::max_length;
        }

        return cut::min_length + below(cut::max_length);
    }

    std::mt19937 engine_{list_seed};
};

// For every random list validate() accepts, the stage ends within
// most_steps(), sends the reference cut stream and logs the reference's cuts,
// over random block patterns and limits. Both verdicts come up often enough
// for that to mean something.
void test_valid_schedules_end(Checks& checks)
{
    constexpr std::uint32_t lists = 1500U;
    // At least a quarter of the lists valid, and at least a tenth refused.
    constexpr std::uint32_t fewest_valid = lists / 4U;
    constexpr std::uint32_t most_valid = lists - lists / 10U;
    ListSource source;
    std::uint32_t accepted = 0U;

    for (std::uint32_t index = 0U; index < lists; ++index)
    {
        const auto output = make_output(source.below(output_length + 1U));
        const auto entries = source.entries(frames_of(output.size()));
        const auto schedule = cut::validate(list_of(checks, entries));

        if (!schedule.has_value())
        {
            continue;
        }

        ++accepted;
        const auto pattern = source.pattern();
        const auto limit = source.limit();
        const auto run = run_stage(output, schedule.value(), pattern, limit);
        const auto expected = reference_stream(output, entries);
        checks.expect(run.ended && run.frames == expected.frames && std::ranges::equal(run.log.applied(), expected.applied) && run.unlogged == 0U &&
                          run.log.tally(cut::SentFrames{.value = frames_of(expected.frames.size())}).rejected == expected.rejected,
                      "random list " + std::to_string(index) + " (" + std::to_string(entries.size()) + " entries, " + std::to_string(output.size()) + " frames, steps of " + std::to_string(limit) +
                          "): the stage ends and sends the reference cut stream");
    }

    checks.expect(accepted >= fewest_valid && accepted <= most_valid,
                  "the random lists are valid often and invalid now and then: " + std::to_string(accepted) + " of " + std::to_string(lists) + " valid");
}

// What the stage alone did with an output.
struct StageEnd
{
    bool ended{};
    std::uint64_t steps{};
    cut::OutputFrames position{};
    std::uint32_t applied{};
    std::uint32_t past_end{};
};

// The stage alone over an output of `frames` frames handed over in blocks of
// `block` with steps of at most `limit`, for at most `step_limit` steps.
[[nodiscard]] StageEnd run_to_end(const cut::ValidSchedule& schedule, cut::OutputFrames frames, std::uint32_t block, std::uint32_t limit, std::uint64_t step_limit)
{
    cut::CutStage stage;
    stage.latch(schedule);
    StageEnd end{};
    const auto most = limit_of(limit);
    std::uint32_t offered = 0U;

    while (end.steps < step_limit)
    {
        const auto step = stage.next(most);
        ++end.steps;

        if (step.kind == cut::StepKind::end)
        {
            end.ended = true;
            break;
        }

        if (step.kind == cut::StepKind::pull && offered == frames.value)
        {
            const auto output_end = stage.finish(cut::SentFrames{});
            end.applied += output_end.clipped.has_value() ? 1U : 0U;
            end.past_end = output_end.past_end;
        }
        else if (step.kind == cut::StepKind::pull)
        {
            const auto size = std::min(block, frames.value - offered);
            offered += stage.offer(cut::OutputFrames{.value = size}).is_ok() ? size : 0U;
        }
        else
        {
            end.applied += stage.take(step, cut::SentFrames{}).has_value() ? 1U : 0U;
        }
    }

    end.position = stage.position();
    return end;
}

// At the edges of the 32-bit offsets, with no samples behind them: an output
// of 2^32 - 1 frames whose last frames a DROP removes, an erasure at its last
// frame, sixteen DROPs that end at its end (each after 4800 output frames, as
// the drop balance asks of so many) and three adjacent ones that do, an entry
// at the largest offset;
// an empty output; a DROP longer than its output; a ZERO in one-frame steps.
// Each ends within most_steps(), every output frame sent or discarded and
// every entry applied or past the end.
void test_edge_schedules_end(Checks& checks)
{
    constexpr std::uint32_t top = std::numeric_limits<std::uint32_t>::max();
    std::vector<cut::CutEntry> packed;
    packed.reserve(cut::max_entries);

    for (std::uint32_t index = 0U; index < cut::max_entries; ++index)
    {
        packed.push_back(drop(top - (cut::max_entries - index) * 2U * cut::max_length + cut::max_length, cut::max_length));
    }

    const auto adjacent =
        std::vector<cut::CutEntry>{drop(top - 3U * cut::max_length, cut::max_length), drop(top - 2U * cut::max_length, cut::max_length), drop(top - cut::max_length, cut::max_length)};
    struct EdgeCase
    {
        std::string_view name;
        std::vector<cut::CutEntry> entries;
        std::uint32_t frames;
        std::uint32_t block;
        std::uint32_t limit;
    };

    const std::vector<EdgeCase> cases{
        {.name = "a DROP of the last 4800 frames of 2^32 - 1", .entries = {drop(top - cut::max_length, cut::max_length)}, .frames = top, .block = top, .limit = top},
        {.name = "an erasure at the last frame of 2^32 - 1", .entries = {zero(top - 1U, cut::max_length), drop(top - 1U, cut::max_length)}, .frames = top, .block = top, .limit = top},
        {.name = "sixteen DROPs that end at the end of 2^32 - 1 frames", .entries = packed, .frames = top, .block = top, .limit = top},
        {.name = "three adjacent DROPs that end at the end of 2^32 - 1 frames", .entries = adjacent, .frames = top, .block = top, .limit = top},
        {.name = "an entry at the largest offset", .entries = {zero(top, cut::max_length)}, .frames = top, .block = top, .limit = top},
        {.name = "an empty output", .entries = {zero(0U, 1U), drop(0U, cut::max_length)}, .frames = 0U, .block = 1U, .limit = 1U},
        {.name = "a DROP longer than its output", .entries = {drop(0U, cut::max_length)}, .frames = 1U, .block = 1U, .limit = 1U},
        {.name = "a ZERO in one-frame steps", .entries = {zero(0U, cut::max_length)}, .frames = 1U, .block = 1U, .limit = 1U},
    };

    for (const auto& item : cases)
    {
        const auto pattern = std::to_array({item.block});
        const auto limit = most_steps(item.entries, item.frames, pattern, item.limit);
        const auto end = run_to_end(valid_of(checks, item.entries), cut::OutputFrames{.value = item.frames}, item.block, item.limit, limit);
        checks.expect(end.ended && end.position.value == item.frames && end.applied + end.past_end == item.entries.size(),
                      std::string{item.name} + ": the stage ends within " + std::to_string(limit) + " steps, every frame and entry accounted for");
    }
}

// A step limit is one frame at least: of() refuses 0, and nothing else makes
// one. With the smallest limit every send and zeros step is one frame, and a
// DROP's discard still takes the frames on hand: on a 7-frame output in one
// block, a ZERO of 3 at frame 2 and a DROP of 2 at frame 4.
void test_step_limit(Checks& checks)
{
    const auto none = cut::StepLimit::of(0U);
    checks.expect(!none.has_value() && std::string_view{none.status().message} == cut::no_step_frames.message, "a limit of 0 frames is CUT_STEP_ZERO");
    constexpr std::uint32_t widest = std::numeric_limits<std::uint32_t>::max();
    const auto smallest = cut::StepLimit::of(frame_block);
    const auto largest = cut::StepLimit::of(widest);
    checks.expect(smallest.has_value() && smallest.value().frames() == 1U && largest.has_value() && largest.value().frames() == widest, "limits of 1 and 2^32 - 1 frames");
    constexpr std::uint32_t output_frames = 7U;
    constexpr std::uint32_t zero_at = 2U;
    constexpr std::uint32_t zero_length = 3U;
    constexpr std::uint32_t drop_at = 4U;
    constexpr std::uint32_t drop_length = 2U;
    const auto output = make_output(output_frames);
    const auto run = run_stage(output, valid_of(checks, std::to_array({zero(zero_at, zero_length), drop(drop_at, drop_length)})), std::to_array({odd_block}), frame_block);
    const cut::CutStep pull{.kind = cut::StepKind::pull};
    const cut::CutStep send_one{.kind = cut::StepKind::send, .frames = 1U};
    const cut::CutStep zero_one{.kind = cut::StepKind::zeros, .frames = 1U};
    const std::vector<cut::CutStep> one_frame{
        pull, send_one, send_one, zero_one, zero_one, zero_one, send_one, send_one, {.kind = cut::StepKind::discard, .frames = drop_length}, send_one, pull, {.kind = cut::StepKind::end}};
    checks.expect(run.steps == one_frame && run.log.applied().size() == 2U, "the smallest limit: one-frame sends and zeros, one discard for the DROP, each cut once");
}

// The log holds max_entries cuts, the most a schedule applies; a 17th is
// refused with CUT_LOG_FULL and changes nothing, the tally included.
void test_log_full(Checks& checks)
{
    cut::CutLog log;
    bool recorded = true;

    for (std::uint32_t index = 0U; index < cut::max_entries; ++index)
    {
        recorded = log.record({.position = cut::SentFrames{.value = index}, .frames = 1U, .kind = cut::CutKind::zero}).is_ok() && recorded;
    }

    const cut::SentFrames all{.value = output_length};
    checks.expect(recorded && log.applied().size() == cut::max_entries && log.tally(all).applied == cut::max_entries, "the log holds 16 cuts");
    const auto refused = log.record({.position = cut::SentFrames{.value = cut::max_entries}, .frames = 1U, .kind = cut::CutKind::drop});
    checks.expect(refused.code == cut::log_full.code && std::string_view{refused.message} == cut::log_full.message && log.applied().size() == cut::max_entries &&
                      log.applied().back().position.value == cut::max_entries - 1U && log.tally(all).applied == cut::max_entries && log.tally(all).drop_frames == cut::OutputFrames{},
                  "a 17th cut is CUT_LOG_FULL and changes nothing");
}

// The stage's places are 32 bits: a block that would take the output past
// 2^32 - 1 frames is CUT_OUTPUT_LONG and changes nothing; one that reaches it
// exactly is taken.
void test_output_too_long(Checks& checks)
{
    constexpr std::uint32_t top = std::numeric_limits<std::uint32_t>::max();
    cut::CutStage stage;
    stage.latch(cut::ValidSchedule{});
    const auto whole = limit_of(top);
    const bool first = stage.offer(cut::OutputFrames{.value = top - 1U}).is_ok();
    const auto sent = stage.next(whole);
    const auto logged = stage.take(sent, cut::SentFrames{});
    const auto refused = stage.offer(cut::OutputFrames{.value = 2U});
    const auto after = stage.next(whole);
    checks.expect(first && sent == cut::CutStep{.kind = cut::StepKind::send, .frames = top - 1U} && !logged.has_value(), "without a schedule the send step takes the whole block and logs no cut");
    checks.expect(std::string_view{refused.message} == cut::output_too_long.message && after.kind == cut::StepKind::pull && stage.position().value == top - 1U,
                  "a block past 2^32 - 1 output frames is CUT_OUTPUT_LONG and changes nothing");
    checks.expect(stage.offer(cut::OutputFrames{.value = 1U}).is_ok() && stage.next(whole) == cut::CutStep{.kind = cut::StepKind::send, .frames = 1U}, "a block that ends at 2^32 - 1 frames is taken");
}

// An entry past the output's end counts in the tally once the whole stream
// went out, and in no prefix before: only then is the end within the frames
// sent, so the counters of a play cut short never depend on how far ahead of
// the stream the producer ran.
void test_rejected_at_the_end(Checks& checks)
{
    const auto output = make_output(output_length);
    const auto entries = std::to_array({zero(middle, short_length), zero(output_length, 1U)});
    const auto run = run_stage(output, valid_of(checks, entries), std::to_array({player_block}), player_step);
    const auto end = frames_of(run.frames.size());
    checks.expect(run.ended && run.log.tally(cut::SentFrames{.value = end}).rejected == 1U && run.log.tally(cut::SentFrames{.value = end - 1U}).rejected == 0U &&
                      run.log.tally(cut::SentFrames{}).rejected == 0U,
                  "an entry past the end counts once the whole stream went out, and in no shorter prefix");
}

// A DROP at offset == the output length begins where the output ends: the
// end finds it not under way (its offset is not before the position), so it
// is rejected, never clipped to an applied cut of no frames (264347f).
void test_drop_at_the_end(Checks& checks)
{
    const auto output = make_output(output_length);
    const auto entries = std::to_array({drop(output_length, short_length)});
    const auto run = run_stage(output, valid_of(checks, entries), std::to_array({player_block}), player_step);
    const auto end = frames_of(run.frames.size());
    const auto tally = run.log.tally(cut::SentFrames{.value = end});
    checks.expect(run.ended && end == output_length && tally.rejected == 1U && tally.applied == 0U && tally.drop_frames == cut::OutputFrames{} && run.log.applied().empty() && run.unlogged == 0U,
                  "a DROP at offset == the output length is rejected 1, applied 0, and discards nothing");
}

// The predictor: the stage and the log over an engine output's raw PCM24 data.
[[nodiscard]] std::optional<Output> read_output(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};

    if (!file)
    {
        return std::nullopt;
    }

    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    const std::span<const std::uint8_t> data{bytes};
    Output output;
    output.reserve(data.size() / audio::pcm24_bytes_per_sample);

    for (std::size_t at = 0U; at + audio::pcm24_bytes_per_sample <= data.size(); at += audio::pcm24_bytes_per_sample)
    {
        output.push_back(audio::read_pcm24_le(data.subspan(at, audio::pcm24_bytes_per_sample).data()));
    }

    return output;
}

[[nodiscard]] std::uint32_t crc_of(std::span<const audio::Pcm24Sample> samples)
{
    waveform_generator::Crc32 crc;
    std::array<std::uint8_t, audio::pcm24_bytes_per_sample> bytes{};

    for (const auto sample : samples)
    {
        audio::write_pcm24_le(bytes.data(), sample);
        crc.update(bytes);
    }

    return crc.value();
}

void print_member(std::string_view name, const std::string& value, bool first)
{
    std::fputs(first ? R"({")" : R"(,")", stdout);
    std::fputs(std::string{name}.c_str(), stdout);
    std::fputs(R"(":)", stdout);
    std::fputs(value.c_str(), stdout);
}

int predict(std::span<char* const> args)
{
    constexpr std::size_t with_sent = 3U;
    const auto output = read_output(args.front());
    const auto entries = cut::parse_command(args[1]);
    const auto schedule = entries.has_value() ? cut::validate(entries.value()) : m110::Result<cut::ValidSchedule>{entries.status()};

    if (!output.has_value() || !schedule.has_value())
    {
        std::fputs("cut predictor: unreadable output or invalid schedule\n", stderr);
        return 2;
    }

    const auto run = run_stage(output.value(), schedule.value(), std::to_array({player_block}), player_step);
    const auto end = frames_of(run.frames.size());
    const auto sent = args.size() >= with_sent ? cut::parse_uint32(args[2]) : m110::Result<std::uint32_t>{end};

    if (!sent.has_value() || sent.value() > end)
    {
        std::fputs("cut predictor: the sent frames are not a prefix of the cut stream\n", stderr);
        return 2;
    }

    const auto tally = run.log.tally(cut::SentFrames{.value = sent.value()});
    const auto crc = std::to_string(crc_of(std::span{run.frames}.first(sent.value())));
    print_member("uac_play_sent_frames", std::to_string(tally.sent.value), true);
    print_member("uac_play_crc32_l", crc, false);
    print_member("uac_play_crc32_r", crc, false);
    print_member("uac_play_file_frames", std::to_string(tally.output_frames.value), false);
    print_member("uac_play_cut_count", std::to_string(tally.applied), false);
    print_member("uac_play_cut_drop_frames", std::to_string(tally.drop_frames.value), false);
    print_member("uac_play_cut_zero_frames", std::to_string(tally.zero_frames.value), false);
    print_member("cut_first_position", tally.first.has_value() ? std::to_string(tally.first->value) : std::string{"null"}, false);
    print_member("uac_play_cut_rejected", std::to_string(tally.rejected), false);
    print_member("stream_frames", std::to_string(end), false);
    std::fputs("}\n", stdout);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    constexpr std::string_view predict_flag{"--predict"};
    constexpr std::size_t predict_args = 4U;

    if (args.size() >= predict_args && std::string_view{args[1]} == predict_flag)
    {
        return predict(args.subspan(2U));
    }

    Checks checks;
    test_command(checks);
    test_rules(checks);
    test_refused_lists(checks);
    test_steps(checks);
    test_stage(checks);
    test_valid_schedules_end(checks);
    test_edge_schedules_end(checks);
    test_step_limit(checks);
    test_log_full(checks);
    test_output_too_long(checks);
    test_rejected_at_the_end(checks);
    test_drop_at_the_end(checks);

    if (checks.failures() == 0)
    {
        std::puts("cut schedule tests: PASS");
        return 0;
    }

    std::fputs("cut schedule tests: FAIL\n", stdout);
    return 1;
}
