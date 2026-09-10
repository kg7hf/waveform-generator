// Host WAV-rendering backend for the portable signal_lab engine.
//
//   signal_lab_render --scenario case.json --source clean.wav --output impaired.wav
//                     [--sidecar impaired.json] [--reference-rms R] [--target-scenario PLAY.SCN]
//                     [--block 2048] [--live-events replay.json]
//
// The same Engine object and block size the RT1170 player uses render the
// file, so the printed output digest is the value the player must report.

#include "common/wav.hpp"
#include "common/pcm24.hpp"
#include "live_replay.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

signal_lab::Engine engine;
signal_lab::LiveController live;

struct Options
{
    std::string scenario;
    std::string source;
    std::string output;
    std::string sidecar;
    std::string target_scenario;
    std::string live_events;
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

bool write_wav(const std::string& path, const std::vector<float>& pcm)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }
    const auto data_size = static_cast<std::uint32_t>(pcm.size() * 3U);
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
    put32(28, 144000U);
    put16(32, 3U);
    put16(34, 24U);
    std::memcpy(header + 36, "data", 4);
    put32(40, data_size);
    bool ok = std::fwrite(header, 1U, sizeof(header), file) == sizeof(header);
    for (const float sample : pcm)
    {
        unsigned char packed[3]{};
        waveform_generator::audio::write_pcm24_le(
            packed, signal_lab::det::quantize_pcm24(sample));
        ok = ok && std::fwrite(packed, 1U, sizeof(packed), file) == sizeof(packed);
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

std::string json_string(const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string encoded{"\""};
    for (const unsigned char byte : value)
    {
        if (byte == '\"' || byte == '\\')
        {
            encoded += '\\';
            encoded += static_cast<char>(byte);
        }
        else if (byte < 0x20U)
        {
            encoded += "\\u00";
            encoded += hex[byte >> 4U];
            encoded += hex[byte & 0x0FU];
        }
        else
        {
            encoded += static_cast<char>(byte);
        }
    }
    encoded += '\"';
    return encoded;
}

// Member names point into the original JSON, so compare their decoded ASCII
// spelling without mistaking a nested member or string value for a root key.
bool key_equals(const signal_lab::json::Value& member, const char* expected)
{
    std::size_t position = 0U;
    for (std::size_t index = 0U; index < member.key_length; ++index)
    {
        unsigned int byte = static_cast<unsigned char>(member.key[index]);
        if (byte == '\\')
        {
            if (++index >= member.key_length)
            {
                return false;
            }
            byte = static_cast<unsigned char>(member.key[index]);
            if (byte == 'u')
            {
                if (index + 4U >= member.key_length)
                {
                    return false;
                }
                byte = 0U;
                for (unsigned int digit = 0U; digit < 4U; ++digit)
                {
                    const char c = member.key[++index];
                    const unsigned int value = c >= '0' && c <= '9' ? static_cast<unsigned int>(c - '0') :
                                               c >= 'a' && c <= 'f' ? static_cast<unsigned int>(c - 'a') + 10U :
                                               c >= 'A' && c <= 'F' ? static_cast<unsigned int>(c - 'A') + 10U : 16U;
                    if (value == 16U)
                    {
                        return false;
                    }
                    byte = (byte << 4U) | value;
                }
            }
            else if (byte == 'b')
            {
                byte = '\b';
            }
            else if (byte == 'f')
            {
                byte = '\f';
            }
            else if (byte == 'n')
            {
                byte = '\n';
            }
            else if (byte == 'r')
            {
                byte = '\r';
            }
            else if (byte == 't')
            {
                byte = '\t';
            }
        }
        if (expected[position] == '\0' || byte != static_cast<unsigned char>(expected[position++]))
        {
            return false;
        }
    }
    return expected[position] == '\0';
}

bool json_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string target_scenario(const signal_lab::json::Document& document,
                            const std::vector<unsigned char>& original, double reference_rms)
{
    char level[64];
    std::snprintf(level, sizeof(level), "{\"reference_rms\": %.17g", reference_rms);
    std::string result{level};
    const char* end = reinterpret_cast<const char*>(original.data()) + original.size();
    while (json_whitespace(end[-1]))
    {
        --end;
    }
    --end; // Root object's closing brace; the document has already been parsed.
    for (const auto* member = document.child(document.root()->first_child); member != nullptr;
         member = document.child(member->next_sibling))
    {
        if (key_equals(*member, "reference_rms"))
        {
            continue;
        }
        const auto* next = document.child(member->next_sibling);
        const char* member_end = next != nullptr ? next->key - 1 : end;
        while (json_whitespace(member_end[-1]))
        {
            --member_end;
        }
        if (next != nullptr && member_end[-1] == ',')
        {
            --member_end;
        }
        result += ", ";
        // Keep every other member's original JSON, including its numeric and
        // escaped-string spelling. Only the root reference level is replaced.
        result.append(member->key - 1, static_cast<std::size_t>(member_end - (member->key - 1)));
    }
    result += "}\n";
    return result;
}

std::string test_id_json(const signal_lab::json::Document& document)
{
    for (const auto* member = document.child(document.root()->first_child); member != nullptr;
         member = document.child(member->next_sibling))
    {
        if (key_equals(*member, "test_id") && member->type == signal_lab::json::Type::string)
        {
            // This is already an encoded JSON string; retain its semantic value
            // instead of escaping the portable parser's raw string a second time.
            return std::string(member->text - 1, member->text_length + 2U);
        }
    }
    return json_string("");
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
        else if (flag == "--live-events" && value != nullptr)
        {
            options.live_events = value;
            ++index;
        }
        else if (flag == "--reference-rms" && value != nullptr)
        {
            char* end = nullptr;
            options.reference_rms = std::strtod(value, &end);
            if (end == value || *end != '\0' || !std::isfinite(options.reference_rms) || options.reference_rms <= 0.0)
            {
                std::fprintf(stderr, "--reference-rms must be a finite positive number\n");
                return false;
            }
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
    if ((options.scenario.empty() && options.live_events.empty()) || options.source.empty() || options.output.empty())
    {
        std::fprintf(stderr, "usage: signal_lab_render --scenario case.json --source clean.wav --output impaired.wav"
                             " [--sidecar out.json] [--reference-rms R] [--target-scenario PLAY.SCN] [--block N] [--live-events replay.json]\n"
                             "With --live-events, omit --scenario for unity-gain clean-source replay.\n");
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

    waveform_generator::LiveReplay replay;
    const bool with_live = !options.live_events.empty();
    if (with_live)
    {
        std::string replay_error;
        if (!waveform_generator::read_live_replay(options.live_events, replay, replay_error))
        {
            std::fprintf(stderr, "live replay error: %s\n", replay_error.c_str());
            return 2;
        }
    }
    std::vector<unsigned char> scenario_text;
    if (options.scenario.empty())
    {
        const std::string unity = "{\"source_gain_db\":0,\"impairments\":[]}";
        scenario_text.assign(unity.begin(), unity.end());
    }
    else if (!read_file(options.scenario, scenario_text))
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
    signal_lab::json::Document scenario_document;
    if (!scenario_document.parse(reinterpret_cast<const char*>(scenario_text.data()), scenario_text.size()))
    {
        std::fprintf(stderr, "scenario error: %s\n", scenario_document.error());
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
    bool source_is_pcm24 = waveform_generator::validate_pcm24_mono_48k_wav(reader, payload) ==
                           waveform_generator::WavValidationError::none;
    if (!source_is_pcm24)
    {
        payload = {};
        if (waveform_generator::validate_pcm16_mono_48k_wav(reader, payload) !=
            waveform_generator::WavValidationError::none)
        {
            std::fprintf(stderr, "source must be a 48 kHz mono PCM24 or legacy PCM16 WAV with one fmt and one data chunk\n");
            return 2;
        }
    }
    const std::size_t source_bytes_per_frame = source_is_pcm24 ? 3U : 2U;
    const std::size_t total_frames = payload.bytes / source_bytes_per_frame;
    std::vector<float> source(total_frames);
    for (std::size_t index = 0U; index < total_frames; ++index)
    {
        const unsigned char* at = wav_bytes.data() + payload.offset + source_bytes_per_frame * index;
        if (source_is_pcm24)
        {
            source[index] = waveform_generator::audio::pcm24_to_float(
                waveform_generator::audio::read_pcm24_le(at));
        }
        else
        {
            const auto value = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(at[0] | (at[1] << 8U)));
            source[index] = static_cast<float>(value) / 32768.0F;
        }
    }

    double reference_rms = 0.0;
    if (options.has_reference_rms)
    {
        reference_rms = options.reference_rms;
    }
    else if (options.scenario.empty() && with_live)
    {
        reference_rms = replay.reference_rms;
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
            while (start < end && source[start] == 0.0F)
            {
                ++start;
            }
            while (end > start && source[end - 1U] == 0.0F)
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
            const double x = static_cast<double>(source[index]);
            energy += x * x;
        }
        reference_rms = std::sqrt(energy / static_cast<double>(end - start));
    }

    if (!engine.configure(scenario, reference_rms, total_frames, &error))
    {
        std::fprintf(stderr, "engine configuration failed: %s\n", error != nullptr ? error : "unknown");
        return 2;
    }
    if (with_live && !live.reset(replay.seed, replay.reference_rms, &error))
    {
        std::fprintf(stderr, "live configuration failed: %s\n", error);
        return 2;
    }

    std::vector<float> output;
    output.reserve(total_frames + 4096U);
    float block[signal_lab::engine_capacity_frames];
    std::size_t next_live_event = 0U;
    const auto queue_live_events = [&]() {
        while (next_live_event < replay.events.size() && live.pending_count() < signal_lab::live_pending_capacity)
        {
            const auto result = live.enqueue(replay.events[next_live_event]);
            if (result != signal_lab::ControlResult::accepted)
            {
                std::fprintf(stderr, "live replay enqueue failed: %s\n", signal_lab::control_result_name(result));
                return false;
            }
            ++next_live_event;
        }
        return true;
    };
    for (std::size_t cursor = 0U; cursor < total_frames;)
    {
        if (with_live && replay.has_frames && live.frame() == replay.frames)
        {
            break;
        }
        const std::size_t frames = (total_frames - cursor) < options.block ? (total_frames - cursor) : options.block;
        const std::size_t produced = engine.process(source.data() + cursor, frames, block, signal_lab::engine_capacity_frames);
        if (engine.process_error() != signal_lab::ProcessError::none)
        {
            if (engine.process_error() == signal_lab::ProcessError::clipping)
            {
                std::fprintf(stderr, "render rejected clipping at output frame %llu\n",
                             static_cast<unsigned long long>(engine.stats().first_clipped_frame));
            }
            else
            {
                std::fprintf(stderr, "engine processing failed\n");
            }
            return 2;
        }
        std::size_t delivered = produced;
        if (with_live)
        {
            if (replay.has_frames && replay.frames - live.frame() < delivered)
            {
                delivered = static_cast<std::size_t>(replay.frames - live.frame());
            }
            std::size_t done = 0U;
            while (done < delivered)
            {
                if (!queue_live_events()) return 2;
                std::size_t count = delivered - done;
                // At most 64 events are pending at a time. Stop before the first
                // event not yet queued, then admit it after earlier slots retire.
                if (next_live_event < replay.events.size() && replay.events[next_live_event].frame - live.frame() < count)
                {
                    count = static_cast<std::size_t>(replay.events[next_live_event].frame - live.frame());
                }
                if (count == 0U || !live.process(block + done, count, &error))
                {
                    std::fprintf(stderr, "live replay processing failed: %s\n", count == 0U ? "pending event capacity" : error);
                    return 2;
                }
                done += count;
            }
        }
        output.insert(output.end(), block, block + delivered);
        cursor += frames;
    }
    if (with_live && replay.has_frames && live.frame() != replay.frames)
    {
        std::fprintf(stderr, "source ends before recorded live_frames\n");
        return 2;
    }
    if (with_live && (!queue_live_events() || next_live_event != replay.events.size()))
    {
        std::fprintf(stderr, "live replay leaves more pending controls than the target can retain\n");
        return 2;
    }
    if (!write_wav(options.output, output))
    {
        std::fprintf(stderr, "cannot write %s\n", options.output.c_str());
        return 2;
    }

    const signal_lab::EngineStats& stats = engine.stats();
    const auto final_digest = with_live ? live.stats().output_digest : stats.output_digest;
    const auto final_clipped = with_live ? live.stats().clipped_samples : stats.clipped_samples;
    const auto final_peak = with_live ? live.stats().peak : stats.peak;
    const double output_energy = with_live ? live.stats().output_energy : stats.output_energy;
    const double output_rms = !output.empty() ? std::sqrt(output_energy / static_cast<double>(output.size())) : 0.0;

    if (!options.target_scenario.empty())
    {
        const std::string text = target_scenario(scenario_document, scenario_text, reference_rms);
        std::FILE* file = std::fopen(options.target_scenario.c_str(), "wb");
        if (file == nullptr)
        {
            std::fprintf(stderr, "cannot write %s\n", options.target_scenario.c_str());
            return 2;
        }
        const bool written = std::fwrite(text.data(), 1U, text.size(), file) == text.size();
        if (std::fclose(file) != 0 || !written)
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
        std::fprintf(file, "{\n \"schema\": \"signal-lab.render/1\",\n \"generator\": \"signal-lab-cpp/0.2.0\",\n \"output_encoding\": \"pcm_s24le\",\n");
        std::fprintf(file, " \"test_id\": %s,\n \"scenario_path\": %s,\n \"source_path\": %s,\n \"output_path\": %s,\n", test_id_json(scenario_document).c_str(),
                     json_string(options.scenario).c_str(), json_string(options.source).c_str(), json_string(options.output).c_str());
        std::fprintf(file, " \"seed\": %llu,\n \"source_gain_db\": %.17g,\n \"reference_rms\": %.17g,\n \"reference_rms_dbfs\": %.6f,\n \"scaled_reference_rms_dbfs\": %.6f,\n",
                     static_cast<unsigned long long>(scenario.seed), scenario.source_gain_db, reference_rms, dbfs(reference_rms), dbfs(engine.scaled_reference_rms()));
        std::fprintf(file, " \"block_frames\": %zu,\n \"frames_in\": %llu,\n \"frames_out\": %llu,\n \"clipped_samples\": %llu,\n \"peak_dbfs\": %.6f,\n \"rms_dbfs\": %.6f,\n",
                     options.block, static_cast<unsigned long long>(stats.frames_in), static_cast<unsigned long long>(output.size()),
                     static_cast<unsigned long long>(final_clipped), dbfs(final_peak), dbfs(output_rms));
        if (with_live)
        {
            std::fprintf(file, " \"live_replay\": {\"path\": %s, \"seed\": %llu, \"reference_rms\": %.17g, \"controls_accepted\": %u, \"controls_applied\": %u,"
                               " \"pending_controls\": %zu, \"static_events_started\": %llu, \"static_events_dropped\": %llu, \"engine_output_digest\": \"%s\","
                               " \"engine_frames_out\": %llu, \"engine_clipped_samples\": %llu},\n",
                         json_string(options.live_events).c_str(), static_cast<unsigned long long>(replay.seed), replay.reference_rms,
                         live.stats().controls_accepted, live.stats().controls_applied, live.pending_count(),
                         static_cast<unsigned long long>(live.stats().static_events_started), static_cast<unsigned long long>(live.stats().static_events_dropped),
                         hex64(stats.output_digest).c_str(), static_cast<unsigned long long>(stats.frames_out), static_cast<unsigned long long>(stats.clipped_samples));
        }
        std::fprintf(file, " \"output_digest\": \"%s\",\n \"source_digest\": \"%s\",\n \"arena_bytes_used\": %zu,\n \"stages\": [", hex64(final_digest).c_str(),
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
        const bool written = std::ferror(file) == 0;
        if (std::fclose(file) != 0 || !written)
        {
            std::fprintf(stderr, "cannot write %s\n", options.sidecar.c_str());
            return 2;
        }
    }

    std::printf("output=%s frames_in=%llu frames_out=%llu clipped=%llu peak_dbfs=%.2f digest=%s source_digest=%s stages=%u events=%u/%u/%u arena=%zu\n",
                options.output.c_str(), static_cast<unsigned long long>(stats.frames_in), static_cast<unsigned long long>(output.size()),
                static_cast<unsigned long long>(final_clipped), dbfs(final_peak), hex64(final_digest).c_str(), hex64(stats.source_digest).c_str(),
                stats.stage_count, stats.events_scheduled, stats.events_applied, stats.events_dropped, stats.arena_bytes_used);
    return 0;
}
