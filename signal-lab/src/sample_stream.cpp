#include "signal_lab/sample_stream.hpp"

namespace signal_lab
{

bool run_pipeline(SampleSource& source, Engine& engine, SampleSink& sink, std::int16_t* input_block, std::int16_t* output_block) noexcept
{
    if (!engine.configured() || input_block == nullptr || output_block == nullptr)
    {
        return false;
    }
    for (;;)
    {
        const std::size_t frames = source.read(input_block, engine_block_frames);
        if (frames == 0U)
        {
            return true;
        }
        const std::size_t produced = engine.process(input_block, frames, output_block, engine_capacity_frames);
        if (produced != 0U && !sink.write(output_block, produced))
        {
            return false;
        }
    }
}

} // namespace signal_lab
