// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Single-handle FatFs model for the prepared stream's writer/read interleave.
// The fake exposes only bytes made visible by a successful f_sync.

#include "common/finite_playback.hpp"
#include "common/pcm24_artifact.hpp"
#include "rt1170/prepared_stream.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

namespace
{

using waveform_generator::Rt1170PreparedStream;
namespace artifact = waveform_generator::artifact;
namespace playback = waveform_generator::finite_playback;

struct FakeFatFile
{
    std::vector<std::uint8_t> bytes{};
    std::size_t synced_bytes{};
    unsigned int writes{};
    unsigned int reads{};
    unsigned int seeks{};
    unsigned int syncs{};
    unsigned int fail_write_call{};
    unsigned int short_write_call{};
    unsigned int fail_read_call{};
    unsigned int short_read_call{};
    unsigned int fail_seek_call{};
    unsigned int fail_sync_call{};
};

void check(const bool condition, const char* message)
{
    INFO(message);
    CHECK(condition);
}

FakeFatFile& state(FIL* file)
{
    return *static_cast<FakeFatFile*>(file->context);
}

std::array<std::uint8_t, 3U> sample_bytes(const std::uint64_t frame) noexcept
{
    const std::uint32_t sample = static_cast<std::uint32_t>((frame * 7919U + 0x54321U) & 0x00ffffffU);
    return {static_cast<std::uint8_t>(sample), static_cast<std::uint8_t>(sample >> 8U), static_cast<std::uint8_t>(sample >> 16U)};
}

std::vector<std::uint8_t> packed_frames(const std::uint64_t first, const std::uint32_t count)
{
    std::vector<std::uint8_t> packed;
    packed.reserve(static_cast<std::size_t>(count) * 3U);

    for (std::uint32_t index = 0U; index < count; ++index)
    {
        const auto sample = sample_bytes(first + index);
        packed.insert(packed.end(), sample.begin(), sample.end());
    }

    return packed;
}

void put_u16(std::vector<std::uint8_t>& output, const std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_u32(std::vector<std::uint8_t>& output, const std::uint32_t value)
{
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::vector<std::uint8_t> expected_wav(const std::span<const std::uint8_t> pcm)
{
    const auto data_size = static_cast<std::uint32_t>(pcm.size());
    const auto pad = data_size & 1U;
    std::vector<std::uint8_t> result;
    result.reserve(44U + pcm.size() + pad);
    result.insert(result.end(), {'R', 'I', 'F', 'F'});
    put_u32(result, 36U + data_size + pad);
    result.insert(result.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(result, 16U);
    put_u16(result, 1U);
    put_u16(result, 1U);
    put_u32(result, 48000U);
    put_u32(result, 144000U);
    put_u16(result, 3U);
    put_u16(result, 24U);
    result.insert(result.end(), {'d', 'a', 't', 'a'});
    put_u32(result, data_size);
    result.insert(result.end(), pcm.begin(), pcm.end());

    if (pad != 0U)
    {
        result.push_back(0U);
    }

    return result;
}

FIL make_file(FakeFatFile& backing)
{
    FIL file{};
    file.size = 0U;
    file.position = 0U;
    file.opened = 1;
    file.context = &backing;
    file.mode = static_cast<unsigned char>(FA_READ | FA_WRITE);
    return file;
}

void test_writer_interleaves_reads_and_rewrites_header()
{
    FakeFatFile backing{};
    FIL file = make_file(backing);
    alignas(4) std::array<std::uint8_t, 128U> scratch{};
    Rt1170PreparedStream stream;
    check(stream.bind(&file, scratch).is_ok(), "bind prepared stream");
    artifact::Pcm24ArtifactWriter writer;
    check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin PCM24 writer");
    const auto first = packed_frames(0U, 7U);
    check(writer.append_frames(first).is_ok(), "append first producer block");
    std::array<std::uint8_t, 6U> before_commit{};
    before_commit.fill(0xA5U);
    const auto reads_before = backing.reads;
    check(!stream.read_frames(0U, before_commit).is_ok(), "uncommitted bytes are unavailable");
    check(backing.reads == reads_before, "uncommitted read performs no FatFs read");
    check(std::ranges::all_of(before_commit, [](const std::uint8_t byte) { return byte == 0xA5U; }),

          "uncommitted read leaves caller output untouched");
    check(stream.commit_frames(7U).is_ok(), "sync first committed block");
    const auto syncs_after_commit = backing.syncs;
    check(stream.commit_frames(7U).is_ok() && backing.syncs == syncs_after_commit + 1U, "equal watermark performs a fresh visibility sync");
    const auto committed_bytes = backing.bytes;
    check(stream.seek(44U).is_ok(), "seek to the start of committed PCM");
    const std::array<std::uint8_t, 3U> replacement{0U, 0U, 0U};
    check(!stream.write_exact(replacement).is_ok(), "refuse writes that overlap committed payload");
    check(backing.bytes == committed_bytes, "refused committed overwrite leaves file unchanged");
    check(stream.seek(44U + first.size()).is_ok(), "restore append position after overwrite refusal");
    std::array<std::uint8_t, 12U> crossing{};
    check(stream.read_frames(2U, crossing).is_ok(), "read nonzero offset from committed payload");
    const auto expected_first = packed_frames(2U, 4U);
    check(std::ranges::equal(crossing, expected_first), "read returns exact PCM24 frame bytes");
    check(file.position == 44U + first.size(), "read restores the writer cursor");
    const auto second = packed_frames(7U, 10U);
    check(writer.append_frames(second).is_ok(), "append after a playback read");
    check(file.position == 44U + first.size() + second.size(), "append resumes at the previous write end");
    check(stream.commit_frames(17U).is_ok(), "sync expanded committed watermark");
    std::array<std::uint8_t, 30U> straddling{};
    check(stream.read_frames(6U, straddling).is_ok(), "read across an earlier block boundary");
    const auto expected_straddling = packed_frames(6U, 10U);
    check(std::ranges::equal(straddling, expected_straddling), "cross-boundary read stays byte exact");
    check(file.position == 44U + first.size() + second.size(), "second read restores the write cursor");
    artifact::Pcm24ArtifactSummary summary{};
    check(writer.finish(summary).is_ok(), "finish and rewrite canonical WAV header");
    check(stream.commit_frames(17U).is_ok(), "sync finalized WAV header and odd payload pad");
    const auto pcm = packed_frames(0U, 17U);
    const auto expected = expected_wav(pcm);
    check(backing.bytes == expected, "final single-handle file equals expected canonical WAV");
    check(summary.frames == 17U && summary.data_bytes == pcm.size() && summary.file_bytes == expected.size(), "writer summary reports final frame and byte counts");
    check(backing.synced_bytes == expected.size(), "final sync makes the complete artifact visible");
    stream.unbind();
}

void test_bind_requires_fresh_empty_file()
{
    FakeFatFile backing{};
    FIL file = make_file(backing);
    const std::array<std::uint8_t, 1U> byte{0x42U};
    backing.bytes.assign(byte.begin(), byte.end());
    file.size = 1U;
    file.position = 1U;
    alignas(4) std::array<std::uint8_t, 64U> scratch{};
    Rt1170PreparedStream stream;
    check(!stream.bind(&file, scratch).is_ok(), "reject a nonempty/nonzero-cursor file binding");
    check(stream.status().is_ok(), "invalid initial binding does not bind or poison the stream");
}

void test_bounds_and_misaligned_reads_do_not_reach_fatfs()
{
    FakeFatFile backing{};
    FIL file = make_file(backing);
    alignas(4) std::array<std::uint8_t, 12U> scratch{};
    Rt1170PreparedStream bad_alignment;
    check(!bad_alignment.bind(&file, std::span<std::uint8_t>{scratch}.subspan(1U)).is_ok(), "reject unaligned read scratch");
    Rt1170PreparedStream stream;
    check(stream.bind(&file, scratch).is_ok(), "bind range test stream");
    artifact::Pcm24ArtifactWriter writer;
    check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin range test writer");
    const auto pcm = packed_frames(0U, 8U);
    check(writer.append_frames(pcm).is_ok(), "append range test payload");
    check(stream.commit_frames(8U).is_ok(), "commit range test payload");
    const auto reads_before = backing.reads;
    std::array<std::uint8_t, 4U> misaligned{};
    check(!stream.read_frames(0U, misaligned).is_ok(), "reject byte count not divisible by three");
    std::array<std::uint8_t, 3U> beyond{};
    check(!stream.read_frames(8U, beyond).is_ok(), "reject read at the committed end");
    check(!stream.read_frames(std::numeric_limits<std::uint64_t>::max(), beyond).is_ok(), "reject overflowing frame offset");
    std::array<std::uint8_t, 15U> too_large{};
    check(stream.read_frames(0U, too_large).code == m110::StatusCode::buffer_too_small, "reject read larger than bounded scratch");
    check(backing.reads == reads_before, "invalid and oversized reads do not invoke FatFs");
    check(stream.status().is_ok(), "range refusals do not poison a valid stream");
    stream.unbind();
}

void test_sync_write_and_read_failures_latch()
{
    {
        FakeFatFile backing{};
        FIL file = make_file(backing);
        alignas(4) std::array<std::uint8_t, 64U> scratch{};
        Rt1170PreparedStream stream;
        check(stream.bind(&file, scratch).is_ok(), "bind short-size stream");
        artifact::Pcm24ArtifactWriter writer;
        check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin short-size writer");
        const auto bytes = packed_frames(0U, 4U);
        check(writer.append_frames(bytes).is_ok(), "append before short-size check");
        file.size = static_cast<FSIZE_t>(44U + bytes.size() - 1U);
        check(!stream.commit_frames(4U).is_ok(), "short f_size is rejected");
        check(stream.committed_frames() == 0U && !stream.status().is_ok(), "short f_size latches without advancing the watermark");
        stream.unbind();
    }

    {
        FakeFatFile backing{};
        FIL file = make_file(backing);
        alignas(4) std::array<std::uint8_t, 64U> scratch{};
        Rt1170PreparedStream stream;
        check(stream.bind(&file, scratch).is_ok(), "bind sync-failure stream");
        artifact::Pcm24ArtifactWriter writer;
        check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin sync-failure writer");
        const auto first = packed_frames(0U, 4U);
        check(writer.append_frames(first).is_ok(), "append before sync failure");
        check(stream.commit_frames(4U).is_ok(), "commit initial frames");
        const auto next = packed_frames(4U, 3U);
        check(writer.append_frames(next).is_ok(), "append frames before failed sync");
        backing.fail_sync_call = backing.syncs + 1U;
        check(!stream.commit_frames(7U).is_ok(), "sync failure is returned");
        check(stream.committed_frames() == 4U, "failed sync does not advance committed watermark");
        check(!stream.status().is_ok(), "sync failure latches stream status");
        stream.unbind();
    }

    for (const bool short_write : {false, true})
    {
        FakeFatFile backing{};
        FIL file = make_file(backing);
        alignas(4) std::array<std::uint8_t, 64U> scratch{};
        Rt1170PreparedStream stream;
        check(stream.bind(&file, scratch).is_ok(), "bind write-failure stream");
        artifact::Pcm24ArtifactWriter writer;
        check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin write-failure writer");

        if (short_write)
        {
            backing.short_write_call = backing.writes + 1U;
        }
        else
        {
            backing.fail_write_call = backing.writes + 1U;
        }

        const auto bytes = packed_frames(0U, 2U);
        check(!writer.append_frames(bytes).is_ok(), short_write ? "short write is rejected" : "write error is returned");
        check(!stream.status().is_ok(), "write failure latches stream status");
        stream.unbind();
    }

    for (const bool short_read : {false, true})
    {
        FakeFatFile backing{};
        FIL file = make_file(backing);
        alignas(4) std::array<std::uint8_t, 64U> scratch{};
        Rt1170PreparedStream stream;
        check(stream.bind(&file, scratch).is_ok(), "bind read-failure stream");
        artifact::Pcm24ArtifactWriter writer;
        check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin read-failure writer");
        const auto bytes = packed_frames(0U, 8U);
        check(writer.append_frames(bytes).is_ok(), "append read-failure fixture");
        check(stream.commit_frames(8U).is_ok(), "commit read-failure fixture");

        if (short_read)
        {
            backing.short_read_call = backing.reads + 1U;
        }
        else
        {
            backing.fail_read_call = backing.reads + 1U;
        }

        std::array<std::uint8_t, 12U> output{};
        check(!stream.read_frames(0U, output).is_ok(), short_read ? "short read is rejected" : "read error is returned");
        check(!stream.status().is_ok(), "read failure latches stream status");
        stream.unbind();
    }

    {
        FakeFatFile backing{};
        FIL file = make_file(backing);
        alignas(4) std::array<std::uint8_t, 64U> scratch{};
        Rt1170PreparedStream stream;
        check(stream.bind(&file, scratch).is_ok(), "bind short-sync stream");
        artifact::Pcm24ArtifactWriter writer;
        check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin short-sync writer");
        const auto bytes = packed_frames(0U, 3U);
        check(writer.append_frames(bytes).is_ok(), "append short-sync fixture");
        check(stream.commit_frames(3U).is_ok(), "commit cursor-restore failure fixture");
        backing.fail_seek_call = backing.seeks + 2U;
        std::array<std::uint8_t, 6U> output{};
        check(!stream.read_frames(0U, output).is_ok(), "cursor restore failure is returned");
        check(!stream.status().is_ok(), "cursor restore failure latches stream status");
        stream.unbind();
    }
}

void test_scheduler_plays_bytes_read_from_committed_single_handle()
{
    constexpr std::uint64_t total_frames = 256U;
    constexpr std::uint32_t queue_capacity = 32U;
    constexpr std::uint32_t max_block = 16U;
    playback::Scheduler scheduler;
    check(scheduler.configure({.total_frames = total_frames, .measured_rate_frames_per_second = 24000U, .margin_frames = 16U, .queue_capacity_frames = queue_capacity, .max_block_frames = max_block})
              .is_ok(),
          "configure finite scheduler");
    FakeFatFile backing{};
    FIL file = make_file(backing);
    alignas(4) std::array<std::uint8_t, 128U> scratch{};
    Rt1170PreparedStream stream;
    check(stream.bind(&file, scratch).is_ok(), "bind scheduler stream");
    artifact::Pcm24ArtifactWriter writer;
    check(writer.begin(stream, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok(), "begin scheduler writer");
    std::vector<std::uint8_t> staging{};
    std::vector<std::uint8_t> queue{};
    std::vector<std::uint8_t> played{};
    std::uint64_t staging_first{};
    bool started{};
    bool finalized{};

    for (std::uint32_t iteration = 0U; iteration < 10000U; ++iteration)
    {
        const auto selected = scheduler.next_action();

        if (!selected.has_value())
        {
            check(selected.error().code == m110::StatusCode::busy, "scheduler only waits when no action is ready");

            if (!started)
            {
                check(scheduler.start().is_ok(), "scheduler starts after committed lead and queue prefill");
                started = true;
                continue;
            }

            const auto available = static_cast<std::uint32_t>(queue.size() / 3U);

            if (available == 0U)
            {
                break;
            }

            const auto count = std::min<std::uint32_t>({available, max_block, 7U});
            check(scheduler.consume(count).is_ok(), "advance finite playback clock");
            const auto bytes = static_cast<std::size_t>(count) * 3U;
            played.insert(played.end(), queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(bytes));
            queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(bytes));
            continue;
        }

        const auto action = selected.value();
        m110::Status operation = m110::Status::success();
        std::uint32_t completed_frames{};

        switch (action.kind)
        {
        case playback::ActionKind::produce:
            staging = packed_frames(action.first_frame, action.frames);
            staging_first = action.first_frame;
            completed_frames = action.frames;
            break;

        case playback::ActionKind::write:
            check(staging_first == action.first_frame, "write action uses matching staged frame offset");
            operation = writer.append_frames(std::span<const std::uint8_t>{staging}.first(static_cast<std::size_t>(action.frames) * 3U));

            if (operation.is_ok())
            {
                const auto used = static_cast<std::size_t>(action.frames) * 3U;
                staging.erase(staging.begin(), staging.begin() + static_cast<std::ptrdiff_t>(used));
                staging_first += action.frames;
                completed_frames = action.frames;
            }

            break;

        case playback::ActionKind::visibility_barrier:
            operation = stream.commit_frames(writer.frames());
            break;

        case playback::ActionKind::read:
        {
            std::vector<std::uint8_t> read_buffer(static_cast<std::size_t>(action.frames) * 3U);
            operation = stream.read_frames(action.first_frame, read_buffer);

            if (operation.is_ok())
            {
                queue.insert(queue.end(), read_buffer.begin(), read_buffer.end());
                completed_frames = action.frames;
            }

            break;
        }

        case playback::ActionKind::finalize:
        {
            artifact::Pcm24ArtifactSummary summary{};
            operation = writer.finish(summary);

            if (operation.is_ok())
            {
                operation = stream.commit_frames(total_frames);
            }

            finalized = operation.is_ok();
            break;
        }
        }

        const auto completion = scheduler.complete(action, operation, completed_frames);
        check(completion.is_ok() || scheduler.snapshot().state == playback::State::failed, "scheduler accepts exact stream action completion");

        if (scheduler.snapshot().state == playback::State::completed)
        {
            break;
        }
    }

    const auto snapshot = scheduler.snapshot();
    check(started, "scheduler reached audible playback start");
    check(finalized && snapshot.state == playback::State::completed && snapshot.artifact_finalized, "scheduler finalized the same streamed artifact");
    const auto expected_pcm = packed_frames(0U, static_cast<std::uint32_t>(total_frames));
    check(played == expected_pcm, "scheduler playback bytes match the generated source pattern");
    check(backing.bytes == expected_wav(expected_pcm), "single-handle file contains exact final WAV bytes");
    check(stream.committed_frames() == total_frames, "scheduler leaves all output frames committed");
    stream.unbind();
}

} // namespace

extern "C" FRESULT f_write(FIL* file, const void* data, const UINT requested, UINT* actual)
{
    auto& backing = state(file);
    ++backing.writes;

    if (backing.writes == backing.fail_write_call)
    {
        *actual = 0U;
        return FR_DISK_ERR;
    }

    UINT count = requested;

    if (backing.writes == backing.short_write_call && count > 0U)
    {
        --count;
    }

    if (file->position > backing.bytes.size())
    {
        return FR_DISK_ERR;
    }

    const auto end = static_cast<std::size_t>(file->position) + count;

    if (end > backing.bytes.size())
    {
        backing.bytes.resize(end);
    }

    std::copy_n(static_cast<const std::uint8_t*>(data), count, backing.bytes.begin() + static_cast<std::ptrdiff_t>(file->position));
    file->position += count;
    file->size = static_cast<FSIZE_t>(backing.bytes.size());
    *actual = count;
    return FR_OK;
}

extern "C" FRESULT f_read(FIL* file, void* data, const UINT requested, UINT* actual)
{
    auto& backing = state(file);
    ++backing.reads;

    if (backing.reads == backing.fail_read_call)
    {
        *actual = 0U;
        return FR_DISK_ERR;
    }

    UINT count = requested;

    if (backing.reads == backing.short_read_call && count > 0U)
    {
        --count;
    }

    const auto end = static_cast<std::size_t>(file->position) + count;

    if (end > backing.synced_bytes || end > backing.bytes.size())
    {
        return FR_DISK_ERR;
    }

    std::copy_n(backing.bytes.begin() + static_cast<std::ptrdiff_t>(file->position), count, static_cast<std::uint8_t*>(data));
    file->position += count;
    *actual = count;
    return FR_OK;
}

extern "C" FRESULT f_lseek(FIL* file, const FSIZE_t offset)
{
    auto& backing = state(file);
    ++backing.seeks;

    if (backing.seeks == backing.fail_seek_call || offset > backing.bytes.size())
    {
        return FR_DISK_ERR;
    }

    file->position = offset;
    return FR_OK;
}

extern "C" FRESULT f_sync(FIL* file)
{
    auto& backing = state(file);
    ++backing.syncs;

    if (backing.syncs == backing.fail_sync_call)
    {
        return FR_DISK_ERR;
    }

    backing.synced_bytes = backing.bytes.size();
    return FR_OK;
}

TEST_CASE("Prepared stream owner preserves committed file visibility")
{
    test_writer_interleaves_reads_and_rewrites_header();
    test_bind_requires_fresh_empty_file();
    test_bounds_and_misaligned_reads_do_not_reach_fatfs();
    test_sync_write_and_read_failures_latch();
    test_scheduler_plays_bytes_read_from_committed_single_handle();
}
