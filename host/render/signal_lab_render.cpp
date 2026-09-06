// Host WAV-rendering backend for the portable signal_lab engine.
//
//   signal_lab_render --scenario case.json --source clean.wav --output impaired.wav
//                     [--sidecar impaired.json] [--reference-rms R] [--target-scenario PLAY.SCN]
//                     [--block 4096]
//
// The same Engine object and block size the RT1170 player uses render the
// file, so the printed output digest is the value the player must report.

#include "common/wav.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

signal_lab::Engine engine;

struct Options
{
    std::string scenario;
    std::string source;
    std::string output;
    std::string sidecar;
    std::string target_scenario;
    double reference_rms{};
    bool has_reference_rms{};
    std::size_t block{signal_lab::engine_block_frames};
};

bool read_file(const std::string& path, std::vector<unsigned char>& bytes)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0)
    {
        std::fclose(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    const bool ok = size == 0 || std::fread(bytes.data(), 1U, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return ok;
}

bool reader_callback(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t bytes) noexcept
{
    const auto* data = static_cast<const std::vector<unsigned char>*>(context);
    if (offset + bytes > data->size())
    {
        return false;
    }
    std::memcpy(destination, data->data() + offset, bytes);
    return true;
}

bool write_wav(const std::string& path, const std::vector<std::int16_t>& pcm)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }
    const auto data_size = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    const std::uint32_t riff_size = 36U + data_size;
    unsigned char header[44]{};
    auto put32 = [&](std::size_t at, std::uint32_t value) {
        header[at] = static_cast<unsigned char>(value & 0xFFU);
        header[at + 1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
        header[at + 2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
        header[at + 3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
    };
    auto put16 = [&](std::size_t at, std::uint16_t value) {
        header[at] = static_cast<unsigned char>(value & 0xFFU);
        header[at + 1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    };
    std::memcpy(header, "RIFF", 4);
    put32(4, riff_size);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    put32(16, 16U);
    put16(20, 1U);
    put16(22, 1U);
    put32(24, 48000U);
    put32(28, 96000U);
    put16(32, 2U);
    put16(34, 16U);
    std::memcpy(header + 36, "data", 4);
    put32(40, data_size);
    bool ok = std::fwrite(header, 1U, sizeof(header), file) == sizeof(header);
    for (const std::int16_t sample : pcm)
    {
        const auto bits = static_cast<std::uint16_t>(sample);
        const unsigned char two[2] = {static_cast<unsigned char>(bits & 0xFFU), static_cast<unsigned char>(bits >> 8U)};
        ok = ok && std::fwrite(two, 1U, 2U, file) == 2U;
    }
    return std::fclose(file) == 0 && ok;
}

double dbfs(double amplitude)
{
    return amplitude > 0.0 ? 20.0 * std::log10(amplitude) : -999.0;
}

std::string hex64(std::uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

bool parse_arguments(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string flag = argv[index];
        const char* value = index + 1 < argc ? argv[index + 1] : nullptr;
        if (flag == "--scenario" && value != nullptr)
        {
            options.scenario = value;
            ++index;
        }
        else if (flag == "--source" && value != nullptr)
        {
            options.source = value;
            ++index;
        }
        else if (flag == "--output" && value != nullptr)
        {
            options.output = value;
            ++index;
        }
        else if (flag == "--sidecar" && value != nullptr)
        {
            options.sidecar = value;
            ++index;
        }
        else if (flag == "--target-scenario" && value != nullptr)
        {
            options.target_scenario = value;
            ++index;
        }
        else if (flag == "--reference-rms" && value != nullptr)
        {
            options.reference_rms = std::strtod(value, nullptr);
            options.has_reference_rms = true;
            ++index;
        }
        else if (flag == "--block" && value != nullptr)
        {
            options.block = static_cast<std::size_t>(std::strtoul(value, nullptr, 10));
            ++index;
        }
        else
        {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", flag.c_str());
            return false;
        }
    }
    if (options.scenario.empty() || options.source.empty() || options.output.empty())
    {
        std::fprintf(stderr, "usage: signal_lab_render --scenario case.json --source clean.wav --output impaired.wav"
                             " [--sidecar out.json] [--reference-rms R] [--target-scenario PLAY.SCN] [--block N]\n");
        return false;
    }
    if (options.block == 0U || options.block > signal_lab::engine_block_frames)
    {
        std::fprintf(stderr, "--block must be 1..%zu\n", signal_lab::engine_block_frames);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_arguments(argc, argv, options))
    {
        return 2;
    }

    std::vector<unsigned char> scenario_text;
    if (!read_file(options.scenario, scenario_text))
    {
        std::fprintf(stderr, "cannot read scenario %s\n", options.scenario.c_str());
        return 2;
    }
    signal_lab::Scenario scenario;
    const char* error = nullptr;
    if (!signal_lab::parse_scenario(reinterpret_cast<const char*>(scenario_text.data()), scenario_text.size(), scenario, &error))
    {
        std::fprintf(stderr, "scenario error: %s\n", error != nullptr ? error : "unknown");
        return 2;
    }

    std::vector<unsigned char> wav_bytes;
    if (!read_file(options.source, wav_bytes))
    {
        std::fprintf(stderr, "cannot read source %s\n", options.source.c_str());
        return 2;
    }
    const waveform_generator::WavReader reader{&wav_bytes, static_cast<std::uint32_t>(wav_bytes.size()), &reader_callback};
    waveform_generator::WavPayload payload{};
    if (waveform_generator::validate_pcm16_mono_48k_wav(reader, payload) != waveform_generator::WavValidationError::none)
    {
        std::fprintf(stderr, "source must be a 48 kHz mono PCM16 WAV with one fmt and one data chunk\n");
        return 2;
    }
    const std::size_t total_frames = payload.bytes / 2U;
    std::vector<std::int16_t> source(total_frames);
    for (std::size_t index = 0U; index < total_frames; ++index)
    {
        const unsigned char* at = wav_bytes.data() + payload.offset + 2U * index;
        source[index] = static_cast<std::int16_t>(static_cast<std::uint16_t>(at[0] | (at[1] << 8U)));
    }

    double reference_rms = 0.0;
    if (options.has_reference_rms)
    {
        reference_rms = options.reference_rms;
    }
    else if (scenario.has_reference_rms)
    {
        reference_rms = scenario.reference_rms;
    }
    else
    {
        std::size_t start = 0U;
        std::size_t end = total_frames;
        if (scenario.reference_auto)
        {
            while (start < end && source[start] == 0)
            {
                ++start;
            }
            while (end > start && source[end - 1U] == 0)
            {
                --end;
            }
        }
        else
        {
            start = static_cast<std::size_t>(signal_lab::det::seconds_to_frames(scenario.reference_start_seconds, signal_lab::sample_rate_hz));
            const auto length = static_cast<std::size_t>(signal_lab::det::seconds_to_frames(scenario.reference_duration_seconds, signal_lab::sample_rate_hz));
            end = start + length;
            if (end > total_frames)
            {
                std::fprintf(stderr, "reference interval extends beyond the source\n");
                return 2;
            }
        }
        if (end <= start)
        {
            std::fprintf(stderr, "source is silent; cannot measure a reference level\n");
            return 2;
        }
        double energy = 0.0;
        for (std::size_t index = start; index < end; ++index)
        {
            const double x = static_cast<double>(source[index]) / 32768.0;
            energy += x * x;
        }
        reference_rms = std::sqrt(energy / static_cast<double>(end - start));
    }

    if (!engine.configure(scenario, reference_rms, total_frames, &error))
    {
        std::fprintf(stderr, "engine configuration failed: %s\n", error != nullptr ? error : "unknown");
        return 2;
    }

    std::vector<std::int16_t> output;
    output.reserve(total_frames + 4096U);
    std::int16_t block[signal_lab::engine_capacity_frames];
    for (std::size_t cursor = 0U; cursor < total_frames;)
    {
        const std::size_t frames = (total_frames - cursor) < options.block ? (total_frames - cursor) : options.block;
        const std::size_t produced = engine.process(source.data() + cursor, frames, block, signal_lab::engine_capacity_frames);
        output.insert(output.end(), block, block + produced);
        cursor += frames;
    }
    if (!write_wav(options.output, output))
    {
        std::fprintf(stderr, "cannot write %s\n", options.output.c_str());
        return 2;
    }

    const signal_lab::EngineStats& stats = engine.stats();
    const double output_rms = stats.frames_out != 0U ? std::sqrt(stats.output_energy / static_cast<double>(stats.frames_out)) : 0.0;

    if (!options.target_scenario.empty())
    {
        // Same document with the measured reference level injected for streaming targets.
        std::string text(reinterpret_cast<const char*>(scenario_text.data()), scenario_text.size());
        const std::size_t brace = text.find('{');
        if (brace == std::string::npos)
        {
            std::fprintf(stderr, "scenario text has no object to annotate\n");
            return 2;
        }
        char injected[96];
        std::snprintf(injected, sizeof(injected), "\"reference_rms\": %.17g, ", reference_rms);
        const std::size_t existing = text.find("\"reference_rms\"");
        if (existing == std::string::npos)
        {
            text.insert(brace + 1U, injected);
        }
        std::FILE* file = std::fopen(options.target_scenario.c_str(), "wb");
        if (file == nullptr || std::fwrite(text.data(), 1U, text.size(), file) != text.size() || std::fclose(file) != 0)
        {
            std::fprintf(stderr, "cannot write %s\n", options.target_scenario.c_str());
            return 2;
        }
    }

    if (!options.sidecar.empty())
    {
        std::FILE* file = std::fopen(options.sidecar.c_str(), "wb");
        if (file == nullptr)
        {
            std::fprintf(stderr, "cannot write %s\n", options.sidecar.c_str());
            return 2;
        }
        std::fprintf(file, "{\n \"schema\": \"signal-lab.render/1\",\n \"generator\": \"signal-lab-cpp/0.1.0\",\n");
        std::fprintf(file, " \"test_id\": \"%s\",\n \"scenario_path\": \"%s\",\n \"source_path\": \"%s\",\n \"output_path\": \"%s\",\n", scenario.test_id, options.scenario.c_str(),
                     options.source.c_str(), options.output.c_str());
        std::fprintf(file, " \"seed\": %llu,\n \"source_gain_db\": %.17g,\n \"reference_rms\": %.17g,\n \"reference_rms_dbfs\": %.6f,\n \"scaled_reference_rms_dbfs\": %.6f,\n",
                     static_cast<unsigned long long>(scenario.seed), scenario.source_gain_db, reference_rms, dbfs(reference_rms), dbfs(engine.scaled_reference_rms()));
        std::fprintf(file, " \"block_frames\": %zu,\n \"frames_in\": %llu,\n \"frames_out\": %llu,\n \"clipped_samples\": %llu,\n \"peak_dbfs\": %.6f,\n \"rms_dbfs\": %.6f,\n",
                     options.block, static_cast<unsigned long long>(stats.frames_in), static_cast<unsigned long long>(stats.frames_out),
                     static_cast<unsigned long long>(stats.clipped_samples), dbfs(stats.peak), dbfs(output_rms));
        std::fprintf(file, " \"output_digest\": \"%s\",\n \"source_digest\": \"%s\",\n \"arena_bytes_used\": %zu,\n \"stages\": [", hex64(stats.output_digest).c_str(),
                     hex64(stats.source_digest).c_str(), stats.arena_bytes_used);
        for (std::uint32_t index = 0U; index < scenario.stage_count; ++index)
        {
            const signal_lab::Impairment* stage = engine.stage(index);
            const signal_lab::StageStats& s = stage->stats();
            const double component_rms = s.component_frames != 0U ? std::sqrt(s.component_energy / static_cast<double>(s.component_frames)) : 0.0;
            std::fprintf(file, "%s\n  {\"order\": %u, \"type\": \"%s\", \"events_scheduled\": %u, \"events_applied\": %u, \"events_dropped\": %u,"
                               " \"component_peak_dbfs\": %.6f, \"component_rms_dbfs\": %.6f, \"component_frames\": %llu}",
                         index == 0U ? "" : ",", index, signal_lab::stage_type_name(stage->type()), s.events_scheduled, s.events_applied, s.events_dropped,
                         dbfs(s.component_peak), dbfs(component_rms), static_cast<unsigned long long>(s.component_frames));
        }
        std::fprintf(file, "\n ]\n}\n");
        std::fclose(file);
    }

    std::printf("output=%s frames_in=%llu frames_out=%llu clipped=%llu peak_dbfs=%.2f digest=%s source_digest=%s stages=%u events=%u/%u/%u arena=%zu\n",
                options.output.c_str(), static_cast<unsigned long long>(stats.frames_in), static_cast<unsigned long long>(stats.frames_out),
                static_cast<unsigned long long>(stats.clipped_samples), dbfs(stats.peak), hex64(stats.output_digest).c_str(), hex64(stats.source_digest).c_str(),
                stats.stage_count, stats.events_scheduled, stats.events_applied, stats.events_dropped, stats.arena_bytes_used);
    return 0;
}
