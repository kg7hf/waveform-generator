// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/sample_stream.hpp"

namespace signal_lab
{

namespace
{

constexpr m110::Status pipeline_refused{.code = m110::StatusCode::invalid_argument, .message = "run_pipeline needs a configured engine and both blocks"};

} // namespace

m110::Status run_pipeline(SampleSource& source, Engine& engine, SampleSink& sink, std::int16_t* input_block, std::int16_t* output_block) noexcept
{
    if (!engine.status().is_ok())
    {
        return engine.status();
    }

    if (!engine.configured() || input_block == nullptr || output_block == nullptr)
    {
        return pipeline_refused;
    }

    for (;;)
    {
        const m110::Result<std::size_t> frames = source.read(input_block, engine_block_frames);

        if (!frames.has_value())
        {
            return frames.error();
        }

        if (frames.value() == 0U)
        {
            return m110::Status::success();
        }

        const m110::Result<std::size_t> produced = engine.process(input_block, frames.value(), output_block, engine_capacity_frames);

        if (!produced.has_value())
        {
            return produced.error();
        }

        if (produced.value() == 0U)
        {
            continue;
        }

        if (const m110::Status written = sink.write(output_block, produced.value()); !written.is_ok())
        {
            return written;
        }
    }
}

} // namespace signal_lab
