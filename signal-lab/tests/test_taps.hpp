// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// A TapObserver that keeps every record, and the helpers the Watterson suites use to
// run a channel over a signal in blocks. Host tests only: they may allocate.

#include "signal_lab/watterson.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace signal_lab_test
{

class TapRecorder final : public signal_lab::TapObserver
{
public:
    TapRecorder() = default;
    ~TapRecorder() = default;
    TapRecorder(const TapRecorder&) = delete;
    TapRecorder& operator=(const TapRecorder&) = delete;
    TapRecorder(TapRecorder&&) = delete;
    TapRecorder& operator=(TapRecorder&&) = delete;

    void on_tap(const signal_lab::TapRecord& record) noexcept override { mRecords.push_back(record); }

    // The records of one path, in the order the channel reported them.
    [[nodiscard]] std::vector<signal_lab::TapRecord> path(std::uint32_t index) const
    {
        std::vector<signal_lab::TapRecord> selected;

        for (const signal_lab::TapRecord& record : mRecords)
        {
            if (record.path == index)
            {
                selected.push_back(record);
            }
        }

        return selected;
    }

    [[nodiscard]] std::size_t size() const noexcept { return mRecords.size(); }

private:
    std::vector<signal_lab::TapRecord> mRecords;
};

// The channel object is ~18 KiB: tests keep it off the stack, as the board keeps it static.
[[nodiscard]] inline std::unique_ptr<signal_lab::WattersonChannel> make_channel()
{
    return std::make_unique<signal_lab::WattersonChannel>();
}

// Runs input through a configured channel in blocks of `block` frames and returns the output.
[[nodiscard]] inline std::vector<float> run(signal_lab::WattersonChannel& channel, std::span<const float> input, std::size_t block, bool& ok)
{
    std::vector<float> output(input.begin(), input.end());
    ok = true;

    for (std::size_t offset = 0U; offset < output.size(); offset += block)
    {
        const std::size_t count = std::min(block, output.size() - offset);
        const signal_lab::units::SampleIndex first{offset};
        ok = ok && channel.process(std::span{output}.subspan(offset, count), first).is_ok();
    }

    return output;
}

// Runs input in the given sequence of block sizes, repeated until the input ends.
[[nodiscard]] inline std::vector<float> run_partition(signal_lab::WattersonChannel& channel, std::span<const float> input, std::span<const std::size_t> blocks, bool& ok)
{
    std::vector<float> output(input.begin(), input.end());
    ok = true;
    std::size_t offset = 0U;

    for (std::size_t index = 0U; offset < output.size(); ++index)
    {
        const std::size_t count = std::min(blocks[index % blocks.size()], output.size() - offset);
        const signal_lab::units::SampleIndex first{offset};
        ok = ok && channel.process(std::span{output}.subspan(offset, count), first).is_ok();
        offset += count;
    }

    return output;
}

} // namespace signal_lab_test
