// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Portable scenario renderer
// Guide: docs/modules/signal-lab-engine.md
// Owns offline artifacts; publishes accepted PCM24 and measured identity/levels.

// Host WAV-rendering backend for the portable signal_lab engine.
//
//   signal_lab_render --scenario case.json --source clean.wav --output impaired.wav
//                     [--sidecar impaired.json] [--reference-rms R] [--target-scenario PLAY.SCN]
//                     [--block 2048] [--live-events replay.json] [--seed N]
//
// The same Engine object and block size the RT1170 player uses render the
// file, so the printed output digest is the value the player must report.
// --seed N replaces the scenario document's seed, as a PLAYLIST item's seed=
// does on the board, so a seeded item renders from its unchanged .SCN.

#include "common/host_critical_error.hpp"
#include "common/pcm24.hpp"
#include "common/wav.hpp"
#include "live_replay.hpp"
#include "secondary_wav.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "stream_wav.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

// The exit status of every failure: a refused command line, a file that
// cannot be read or written, an engine error, and a critical error
// (common/host_critical_error.hpp).
constexpr int exit_failure = 2;

// A test build of this tool (tests/CMakeLists.txt's
// wfg_signal_lab_render_probe, compiled with WFG_CRITICAL_ERROR_PROBE) has
// one more command line, --critical-error-probe alone, on which main() takes
// the value of a failed Result as a defect would: the test reads the handler's
// report and exit status. The tool itself is built without it.
#if defined(WFG_CRITICAL_ERROR_PROBE)
constexpr bool critical_error_probe_build = true;
#else
constexpr bool critical_error_probe_build = false;
#endif
constexpr std::string_view critical_error_probe_option = "--critical-error-probe";
constexpr m110::Status critical_error_probe_status{.code = m110::StatusCode::internal_error, .message = "the critical-error probe's failed Result"};
// What the probe writes to stdout first, without flushing it: the handler
// flushes stdout before it ends the tool, so a test reading a pipe (fully
// buffered) sees this line.
constexpr const char* critical_error_probe_output = "output written before the critical error\n";

// The probe build's extra path: the value of a failed Result. Nothing in the tool itself.
void probe_critical_error(int argc, char** argv) noexcept
{
    if constexpr (critical_error_probe_build)
    {
        const std::span<char* const> arguments{argv, argc > 0 ? static_cast<std::size_t>(argc) : 0U};

        if (arguments.size() == 2U && std::string_view{arguments[1]} == critical_error_probe_option)
        {
            static_cast<void>(std::fputs(critical_error_probe_output, stdout));
            const m110::Result<int> failed{critical_error_probe_status};
            static_cast<void>(failed.value());
        }
    }
}

signal_lab::Engine engine;
signal_lab::LiveController live;
constinit signal_lab::WattersonChannel channel{};

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
    std::optional<std::string> seed_text{}; // --seed as given
    std::optional<std::uint64_t> seed{};    // its value, which replaces the scenario's own
};

// A canonical decimal (no sign, no leading zero), as a PLAYLIST item's seed=
// is spelt: its value, or none.
[[nodiscard]] std::optional<std::uint64_t> canonical_decimal(std::string_view text) noexcept
{
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
    {
        return std::nullopt;
    }

    std::uint64_t value{};
    const char* const first = std::to_address(text.cbegin());
    const char* const last = std::to_address(text.cend());
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last ? std::optional{value} : std::nullopt;
}

// A command line this tool refuses (the reason already on stderr).
constexpr m110::Status bad_arguments{.code = m110::StatusCode::invalid_argument, .message = "bad arguments"};
// A file this tool cannot read.
constexpr m110::Status unreadable_file{.code = m110::StatusCode::io_error, .message = "the file cannot be read"};

// --seed, once every argument is read: a canonical decimal uint64, and only
// with --scenario and without --target-scenario, whose document would keep
// its own seed. Success, or bad_arguments with the reason on stderr.
[[nodiscard]] m110::Status parse_seed(Options& options)
{
    if (!options.seed_text.has_value())
    {
        return m110::Status::success();
    }

    options.seed = canonical_decimal(*options.seed_text);

    if (!options.seed.has_value())
    {
        std::fputs("--seed must be a canonical decimal uint64\n", stderr);
        return bad_arguments;
    }

    if (options.scenario.empty() || !options.target_scenario.empty())
    {
        std::fputs("--seed needs --scenario and no --target-scenario\n", stderr);
        return bad_arguments;
    }

    return m110::Status::success();
}

// The whole file: its bytes, or unreadable_file.
[[nodiscard]] m110::Result<std::vector<unsigned char>> read_file(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");

    if (file == nullptr)
    {
        return unreadable_file;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (size < 0)
    {
        std::fclose(file);
        return unreadable_file;
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    const bool ok = size == 0 || std::fread(bytes.data(), 1U, bytes.size(), file) == bytes.size();
    std::fclose(file);

    if (!ok)
    {
        return unreadable_file;
    }

    return bytes;
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

// The stream owner checks ferror after writing the complete sidecar. Diagnostics
// on stderr have no further reporting channel if stderr itself cannot be written.
void write_text(std::FILE* file, std::string_view text)
{
    static_cast<void>(std::fwrite(text.data(), 1U, text.size(), file));
}

[[nodiscard]] m110::Status write_noise_band_report(std::FILE* file, const signal_lab::Scenario& scenario)
{
    const auto selected = signal_lab::noise_band_kernel(scenario.noise_bandwidth);

    if (!selected.has_value())
    {
        return selected.status();
    }

    const double bandwidth = signal_lab::noise_band_width(scenario.noise_bandwidth).hertz();
    const double low = signal_lab::noise_band_low.hertz();
    write_text(file, std::format(" \"noise_bandwidth_hz\": {:.17g}, \"noise_band_low_hz\": {:.17g},"
                                 " \"noise_band_high_hz\": {:.17g}, \"noise_band_center_hz\": {:.17g}, \"noise_band_enbw_hz\": {:.17g},\n",
                                 bandwidth, low, low + bandwidth, low + bandwidth / 2.0, selected.value().get().enbw.hertz()));
    return m110::Status::success();
}

// Close even after a failed write; preserve a prior metadata error too.
[[nodiscard]] m110::Status finish_sidecar(std::FILE* file, m110::Status metadata)
{
    const bool written = std::ferror(file) == 0;
    const int closed = std::fclose(file);
    return written && closed == 0 ? metadata : m110::Status{m110::StatusCode::io_error, "cannot finish sidecar"};
}

// The realised channel identity, with the validation driver's field names.
void write_watterson_report(std::FILE* file, const signal_lab::WattersonReport* report)
{
    if (report == nullptr)
    {
        return;
    }

    using namespace signal_lab::units;
    write_text(file, std::format(" \"watterson\": {{\"algorithm_version\": {}, \"path_count\": {}, \"hilbert_taps\": {},"
                                 " \"latency_frames\": {}, \"parameter_digest\": \"{}\", \"channel_bytes\": {}, \"paths\": [",
                                 signal_lab::watterson_algorithm_version, report->path_count, report->hilbert_taps, report->latency.value(), hex64(report->parameter_digest), sizeof(channel)));
    std::string_view separator;

    for (const auto& path : std::span{report->paths}.first(report->path_count))
    {
        write_text(file, std::format("{}{{\"fading\": \"{}\", \"delay_frames\": {}, \"frames_per_node\": {},"
                                     " \"node_rate_hz\": {:.17g}, \"rate_multiple\": {:.17g}, \"doppler_taps\": {}, \"amplitude\": {:.17g},"
                                     " \"interpolation_power\": {:.17g}, \"gain_db\": {:.17g}, \"phase_step\": {}, \"realised_shift_hz\": {:.17g}, \"phase_deg\": {:.17g}}}",
                                     separator, path.fading == signal_lab::PathFading::rayleigh ? "rayleigh" : "fixed", path.delay.value(), path.frames_per_node.value(), path.node_rate.in(Hz),
                                     path.rate_multiple, path.doppler_taps, path.amplitude, path.interpolation_power, path.gain.in(dB), path.phase_step.word(), path.realised_shift.in(Hz),
                                     path.phase.in(degrees)));
        separator = ", ";
    }

    write_text(file, "]},\n");
}

// The global Doppler stage is identified separately from the multipath channel.
// The raw scenario digest already binds all its parameters; this makes the
// added latency and frequency law visible to artifact consumers.
void write_frequency_shift_report(std::FILE* file, const signal_lab::Scenario* scenario)
{
    if (scenario == nullptr)
    {
        return;
    }

    const auto stages = std::span{scenario->stages}.first(scenario->stage_count);
    const auto found = std::ranges::find(stages, signal_lab::StageType::frequency_shift, &signal_lab::Stage::type);

    if (found == stages.end())
    {
        return;
    }

    const auto& params = scenario->frequency_shift;
    write_text(file, std::format(" \"frequency_shift\": {{\"algorithm_version\": {}, \"mode\": \"{}\","
                                 " \"latency_frames\": {}, \"offset_hz\": {:.17g}, \"amplitude_hz\": {:.17g},"
                                 " \"slope_hz_per_second\": {:.17g}, \"start_frequency_hz\": {:.17g}, \"direction\": \"{}\"}},\n",
                                 signal_lab::frequency_shift_algorithm_version, params.mode == signal_lab::FrequencyShiftMode::offset ? "offset" : "sweep", signal_lab::hilbert_delay,
                                 params.offset.hertz(), params.amplitude.hertz(), params.slope.hertz_per_second(), params.start_frequency.hertz(),
                                 params.direction == signal_lab::TriangleDirection::ascending ? "ascending" : "descending"));
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
bool key_equals(const signal_lab::json::Value& member, std::string_view expected)
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
                    const unsigned int value = c >= '0' && c <= '9'   ? static_cast<unsigned int>(c - '0')
                                               : c >= 'a' && c <= 'f' ? static_cast<unsigned int>(c - 'a') + 10U
                                               : c >= 'A' && c <= 'F' ? static_cast<unsigned int>(c - 'A') + 10U
                                                                      : 16U;

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

        if (position == expected.size() || byte != static_cast<unsigned char>(expected[position]))
        {
            return false;
        }

        ++position;
    }

    return position == expected.size();
}

bool json_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void write_cw_pulse_report(std::FILE* file, const signal_lab::Stage& stage)
{
    if (stage.type != signal_lab::StageType::cw || stage.cw.pulse_period_frames == 0U)
    {
        return;
    }

    std::fprintf(file, ", \"pulse_period_frames\": %llu, \"pulse_on_frames\": %llu, \"pulse_realized_duty_cycle\": %.17g", static_cast<unsigned long long>(stage.cw.pulse_period_frames),
                 static_cast<unsigned long long>(stage.cw.pulse_on_frames), static_cast<double>(stage.cw.pulse_on_frames) / static_cast<double>(stage.cw.pulse_period_frames));
}

void write_cw_sweep_report(std::FILE* file, const signal_lab::Stage& stage)
{
    if (stage.type != signal_lab::StageType::cw || !stage.cw.sweep.enabled)
    {
        return;
    }

    const auto& sweep = stage.cw.sweep;
    const std::uint64_t step = signal_lab::cw_sweep_cycle_step(sweep);
    const double period = 18446744073709551616.0 / static_cast<double>(step) / signal_lab::sample_rate_hz;
    std::fprintf(file,
                 ", \"sweep_algorithm_version\": %u, \"frequency_sweep\": {\"low_hz\": %.17g, \"high_hz\": %.17g, "
                 "\"slope_hz_per_second\": %.17g, \"cycle_step_q64\": %llu, \"realized_period_seconds\": %.17g}",
                 signal_lab::cw_sweep_algorithm_version, sweep.low.in(signal_lab::units::Hz), sweep.high.in(signal_lab::units::Hz), sweep.slope.hertz_per_second(),
                 static_cast<unsigned long long>(step), period);
}

void write_cw_markov_report(std::FILE* file, const signal_lab::Stage& stage)
{
    if (stage.type != signal_lab::StageType::cw || !stage.cw.markov.enabled)
    {
        return;
    }

    const auto& gate = stage.cw.markov;
    std::fprintf(file,
                 ", \"markov_on_off\": {\"mean_on_seconds\": %.17g, \"mean_off_seconds\": %.17g, "
                 "\"mean_on_frames\": %.17g, \"mean_off_frames\": %.17g, \"initial_state\": \"on\", "
                 "\"dwell_distribution\": \"geometric\", \"rng_family\": %u, \"algorithm_version\": %u}",
                 gate.mean_on_seconds, gate.mean_off_seconds, gate.mean_on_seconds * signal_lab::sample_rate_hz, gate.mean_off_seconds * signal_lab::sample_rate_hz, signal_lab::markov_gate_rng_family,
                 signal_lab::markov_gate_algorithm_version);
}

void write_cw_fsk_report(std::FILE* file, const signal_lab::Stage& stage)
{
    if (stage.type != signal_lab::StageType::cw || !stage.cw.fsk.enabled)
    {
        return;
    }

    const auto& fsk = stage.cw.fsk;
    const double shift = fsk.shift.in(signal_lab::units::Hz);
    // Engine configuration already validated this exact symbol period.
    const auto frames = signal_lab::cw_fsk_symbol_frames(fsk);
    std::fprintf(file,
                 ", \"fsk\": {\"baud\": %.17g, \"shift_hz\": %.17g, \"mark_hz\": %.17g, \"space_hz\": %.17g, "
                 "\"frames_per_symbol\": %llu, \"pattern\": \"%s\", \"rng_family\": %u, \"algorithm_version\": %u}",
                 fsk.baud, shift, stage.cw.frequency_hz + shift / 2.0, stage.cw.frequency_hz - shift / 2.0, static_cast<unsigned long long>(frames.value()),
                 fsk.pattern == signal_lab::FskPattern::alternating ? "alternating" : "random", signal_lab::cw_fsk_rng_family, signal_lab::cw_fsk_algorithm_version);
}

void write_cw_fading_report(std::FILE* file, const signal_lab::Stage& stage)
{
    if (stage.type != signal_lab::StageType::cw || !stage.cw.fading.enabled)
    {
        return;
    }

    std::fprintf(file,
                 ", \"fading\": {\"spread_hz\": %.17g, \"mean_power\": 1, \"distribution\": \"rayleigh\", "
                 "\"rng_family\": %u, \"algorithm_version\": %u, \"rng_implementation\": \"det-pcg32\"}",
                 stage.cw.fading.spread.in(signal_lab::units::Hz), signal_lab::cw_fading_rng_family, signal_lab::cw_fading_algorithm_version);
}

void write_random_key(std::FILE* file, const signal_lab::Scenario& scenario, std::uint32_t index)
{
    const std::uint32_t stream_id = scenario.stages[index].stream_id;

    if (stream_id == 0U)
    {
        return;
    }

    const auto key = signal_lab::resolve_stage_random_key(scenario.seed, index, stream_id);
    std::fprintf(file,
                 ", \"random_key\": {\"mode\": \"stream-id-v1\", \"stream_id\": %u, "
                 "\"effective_seed_hex\": \"%s\", \"index\": 0, \"algorithm_version\": %u}",
                 stream_id, hex64(key.seed).c_str(), signal_lab::stream_identity_algorithm_version);
}

std::string target_scenario(const signal_lab::json::Document& document, const std::vector<unsigned char>& original, double reference_rms, std::string_view resolved_secondary_path = {})
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
    const signal_lab::json::Value* secondary_path = nullptr;

    if (!resolved_secondary_path.empty())
    {
        const auto* impairments = document.find(*document.root(), "impairments");

        if (impairments != nullptr && impairments->type == signal_lab::json::Type::array)
        {
            for (std::uint32_t index = 0U; index < impairments->child_count; ++index)
            {
                const auto* stage = document.at(*impairments, index);

                if (stage == nullptr || stage->type != signal_lab::json::Type::object)
                {
                    continue;
                }

                const auto* type = document.find(*stage, "type");

                if (type != nullptr && type->equals("second_source"))
                {
                    secondary_path = document.find(*stage, "source_path");
                    break;
                }
            }
        }
    }

    for (const auto* member = document.child(document.root()->first_child); member != nullptr; member = document.child(member->next_sibling))
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
        const char* const member_begin = member->key - 1;
        const auto member_size = static_cast<std::size_t>(member_end - member_begin);
        const char* const path_begin = secondary_path != nullptr ? secondary_path->text - 1 : nullptr;
        const char* const path_end = secondary_path != nullptr ? path_begin + secondary_path->text_length + 2U : nullptr;

        if (path_begin != nullptr && path_begin >= member_begin && path_end <= member_end)
        {
            result.append(member_begin, static_cast<std::size_t>(path_begin - member_begin));
            result += json_string(std::string{resolved_secondary_path});
            result.append(path_end, static_cast<std::size_t>(member_end - path_end));
        }
        else
        {
            result.append(member_begin, member_size);
        }
    }

    result += "}\n";
    return result;
}

m110::Status write_target_scenario(const std::string& path, const signal_lab::json::Document& document, const std::vector<unsigned char>& original, double reference_rms,
                                   std::string_view secondary_path)
{
    const std::string text = target_scenario(document, original, reference_rms, secondary_path);
    std::FILE* file = std::fopen(path.c_str(), "wb");

    if (file == nullptr)
    {
        return m110::Status{.code = m110::StatusCode::io_error, .message = "cannot write target scenario"};
    }

    const bool written = std::fwrite(text.data(), 1U, text.size(), file) == text.size();
    return std::fclose(file) == 0 && written ? m110::Status::success() : m110::Status{.code = m110::StatusCode::io_error, .message = "cannot write target scenario"};
}

void write_secondary_source_report(std::FILE* file, const signal_lab::Scenario& scenario, const waveform_generator::render::SecondaryWavSource& source)
{
    const auto& params = scenario.second_source;
    const auto latency = signal_lab::second_source_filter_latency_frames(params).value();
    write_text(file, std::format(" \"secondary_source\": {{\"source_path\": {}, \"source_frames\": {}, "
                                 "\"excerpt_start_frame\": {}, \"excerpt_frames\": {}, \"excerpt_reference_rms\": {:.17g}, "
                                 "\"pcm_digest\": \"{}\", \"digest_format\": \"fnv1a64-canonical-pcm24\", "
                                 "\"si_db\": {:.17g}, \"start_frame\": {}, \"frequency_offset_hz\": {:.17g}, "
                                 "\"latency_frames\": {}",
                                 json_string(source.path()), source.frames(), source.excerpt_start_frame(), source.excerpt_frames(), source.reference_rms(), hex64(source.pcm_digest()),
                                 params.si.in(signal_lab::units::dB), params.start_frame, params.frequency_offset.hertz(), latency));

    if (params.fading.enabled)
    {
        std::fprintf(file,
                     ", \"fading\": {\"spread_hz\": %.17g, \"mean_power\": 1, "
                     "\"rng_family\": %u, \"algorithm_version\": %u, \"rng_implementation\": \"det-pcg32\"}",
                     params.fading.spread.hertz(), signal_lab::second_source_fading_rng_family, signal_lab::second_source_fading_algorithm_version);
    }

    if (params.markov.enabled)
    {
        std::fprintf(file,
                     ", \"markov_on_off\": {\"mean_on_seconds\": %.17g, \"mean_off_seconds\": %.17g, "
                     "\"rng_family\": %u, \"algorithm_version\": %u, \"initial_state\": \"on\", "
                     "\"timeline\": \"destination-window\"}",
                     params.markov.mean_on_seconds, params.markov.mean_off_seconds, signal_lab::markov_gate_rng_family, signal_lab::markov_gate_algorithm_version);
    }

    write_text(file, "},\n");
}

std::string test_id_json(const signal_lab::json::Document& document)
{
    for (const auto* member = document.child(document.root()->first_child); member != nullptr; member = document.child(member->next_sibling))
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

// The Options text a flag that takes a text sets, or none for any other flag.
// --seed's is kept as given, for parse_seed() once every argument is read.
[[nodiscard]] std::string* text_option(std::string_view flag, Options& options)
{
    if (flag == "--scenario")
    {
        return &options.scenario;
    }

    if (flag == "--source")
    {
        return &options.source;
    }

    if (flag == "--output")
    {
        return &options.output;
    }

    if (flag == "--sidecar")
    {
        return &options.sidecar;
    }

    if (flag == "--target-scenario")
    {
        return &options.target_scenario;
    }

    if (flag == "--live-events")
    {
        return &options.live_events;
    }

    if (flag == "--seed")
    {
        return &options.seed_text.emplace();
    }

    return nullptr;
}

[[nodiscard]] bool scenario_has_second_source(const signal_lab::Scenario& scenario) noexcept
{
    const auto stages = std::span{scenario.stages}.first(scenario.stage_count);
    return std::ranges::find(stages, signal_lab::StageType::second_source, &signal_lab::Stage::type) != stages.end();
}

[[nodiscard]] std::string generic_utf8_path(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text)
{
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

[[nodiscard]] m110::Status prepare_secondary_source(const signal_lab::Scenario& scenario, bool present, std::string_view scenario_path, waveform_generator::render::SecondaryWavSource& source,
                                                    std::string& resolved_path)
{
    if (!present)
    {
        return m110::Status::success();
    }

    resolved_path = scenario.second_source_path;
    const std::filesystem::path configured_path = path_from_utf8(scenario.second_source_path);
    const std::filesystem::path scenario_parent = path_from_utf8(scenario_path).parent_path();
    const std::filesystem::path joined = configured_path.is_absolute() ? configured_path : scenario_parent / configured_path;
    std::error_code path_error;
    const std::filesystem::path absolute = std::filesystem::absolute(joined, path_error);

    if (path_error)
    {
        return m110::Status{.code = m110::StatusCode::io_error, .message = "cannot resolve second_source path"};
    }

    // Generic separators keep exported JSON unescaped and portable while
    // remaining accepted by the host WAV reader.
    resolved_path = generic_utf8_path(absolute.lexically_normal());
    const auto needs_json_escape = [](char value)
    {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 0x20U || value == '"' || value == '\\';
    };

    if (resolved_path.size() >= signal_lab::second_source_path_capacity || std::ranges::any_of(resolved_path, needs_json_escape))
    {
        return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "resolved second_source path cannot be exported in a scenario"};
    }

    return source.open(resolved_path, scenario.second_source);
}

[[nodiscard]] m110::Result<double> resolve_primary_reference(waveform_generator::render::WavSource& source, const signal_lab::Scenario& scenario, const Options& options, bool with_live,
                                                             double replay_reference_rms)
{
    if (options.has_reference_rms)
    {
        return options.reference_rms;
    }

    if (options.scenario.empty() && with_live)
    {
        return replay_reference_rms;
    }

    if (scenario.has_reference_rms)
    {
        return scenario.reference_rms;
    }

    return source.reference(scenario);
}

// The options the command line gives, or bad_arguments (the reason already
// on stderr).
[[nodiscard]] m110::Result<Options> parse_arguments(int argc, char** argv)
{
    Options options;

    for (int index = 1; index < argc; ++index)
    {
        const std::string flag = argv[index];
        const char* value = index + 1 < argc ? argv[index + 1] : nullptr;
        std::string* const text = value != nullptr ? text_option(flag, options) : nullptr;

        if (text != nullptr)
        {
            *text = value;
            ++index;
        }
        else if (flag == "--reference-rms" && value != nullptr)
        {
            char* end = nullptr;
            options.reference_rms = std::strtod(value, &end);

            if (end == value || *end != '\0' || !std::isfinite(options.reference_rms) || options.reference_rms <= 0.0)
            {
                std::fprintf(stderr, "--reference-rms must be a finite positive number\n");
                return bad_arguments;
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
            return bad_arguments;
        }
    }

    if ((options.scenario.empty() && options.live_events.empty()) || options.source.empty() || options.output.empty())
    {
        std::fprintf(stderr, "usage: signal_lab_render --scenario case.json --source clean.wav --output impaired.wav"
                             " [--sidecar out.json] [--reference-rms R] [--target-scenario PLAY.SCN] [--block N] [--live-events replay.json] [--seed N]\n"
                             "With --live-events, omit --scenario for unity-gain clean-source replay.\n");
        return bad_arguments;
    }

    if (options.block == 0U || options.block > signal_lab::engine_block_frames)
    {
        std::fprintf(stderr, "--block must be 1..%zu\n", signal_lab::engine_block_frames);
        return bad_arguments;
    }

    if (const m110::Status seed = parse_seed(options); !seed.is_ok())
    {
        return seed;
    }

    return options;
}

} // namespace

int main(int argc, char** argv)
{
    m110::install_host_critical_error_handler("signal_lab_render", exit_failure);
    probe_critical_error(argc, argv);
    const m110::Result<Options> arguments = parse_arguments(argc, argv);

    if (!arguments.has_value())
    {
        return 2;
    }

    const Options& options = arguments.value();
    waveform_generator::LiveReplay replay;
    const bool with_live = !options.live_events.empty();

    if (with_live)
    {
        if (const m110::Status read = waveform_generator::read_live_replay(options.live_events, replay); !read.is_ok())
        {
            std::fprintf(stderr, "live replay error: %s\n", read.message);
            return 2;
        }
    }

    std::vector<unsigned char> scenario_text;

    if (options.scenario.empty())
    {
        const std::string unity = "{\"source_gain_db\":0,\"impairments\":[]}";
        scenario_text.assign(unity.begin(), unity.end());
    }
    else if (m110::Result<std::vector<unsigned char>> read = read_file(options.scenario); read.has_value())
    {
        scenario_text = std::move(read).value();
    }
    else
    {
        std::fprintf(stderr, "cannot read scenario %s\n", options.scenario.c_str());
        return 2;
    }

    signal_lab::Scenario scenario;

    if (const m110::Status parsed = signal_lab::parse_scenario(reinterpret_cast<const char*>(scenario_text.data()), scenario_text.size(), scenario); !parsed.is_ok())
    {
        std::fprintf(stderr, "scenario error: %s\n", parsed.message);
        return 2;
    }

    // As the player's configure_scenario() applies a PLAYLIST item's seed=.
    scenario.seed = options.seed.value_or(scenario.seed);
    signal_lab::json::Document scenario_document;

    if (const m110::Status parsed = scenario_document.parse(reinterpret_cast<const char*>(scenario_text.data()), scenario_text.size()); !parsed.is_ok())
    {
        std::fprintf(stderr, "scenario error: %s\n", parsed.message);
        return 2;
    }

    waveform_generator::render::WavSource source;

    if (const m110::Status opened = source.open(options.source); !opened.is_ok())
    {
        if (opened.code == m110::StatusCode::io_error)
        {
            write_text(stderr, std::format("cannot read source {}\n", options.source));
        }
        else
        {
            write_text(stderr, std::format("{}\n", opened.message));
        }

        return 2;
    }

    const std::uint64_t total_frames = source.frames();
    const bool has_second_source = scenario_has_second_source(scenario);
    waveform_generator::render::SecondaryWavSource secondary_source;
    std::string resolved_secondary_path;

    if (const m110::Status opened = prepare_secondary_source(scenario, has_second_source, options.scenario, secondary_source, resolved_secondary_path); !opened.is_ok())
    {
        std::fprintf(stderr, "second_source: %s (%s)\n", opened.message, resolved_secondary_path.c_str());
        return 2;
    }

    const m110::Result<double> measured_reference = resolve_primary_reference(source, scenario, options, with_live, replay.reference_rms);

    if (!measured_reference.has_value())
    {
        write_text(stderr, std::format("{}\n", measured_reference.error().message));
        return 2;
    }

    const double reference_rms = measured_reference.value();

    if (const m110::Status configured = engine.configure(scenario, reference_rms, total_frames, signal_lab::StageResources{&channel, nullptr, has_second_source ? &secondary_source : nullptr});
        !configured.is_ok())
    {
        std::fprintf(stderr, "engine configuration failed: %s\n", configured.message);
        return 2;
    }

    if (const m110::Status reset = with_live ? live.reset(replay.seed, replay.reference_rms) : m110::Status::success(); !reset.is_ok())
    {
        std::fprintf(stderr, "live configuration failed: %s\n", reset.message);
        return 2;
    }

    waveform_generator::render::WavSpool output;

    if (const m110::Status opened = output.open(); !opened.is_ok())
    {
        write_text(stderr, std::format("cannot create render spool: {}\n", opened.message));
        return 2;
    }

    std::array<float, signal_lab::engine_block_frames> input{};
    float block[signal_lab::engine_capacity_frames];
    std::size_t next_live_event = 0U;
    // Queues the recorded events the controller has room for: success, or
    // the controller's refusal of the first one it refuses.
    const auto queue_live_events = [&]() -> m110::Status
    {
        while (next_live_event < replay.events.size() && live.pending_count() < signal_lab::live_pending_capacity)
        {
            if (const m110::Status queued = live.enqueue(replay.events[next_live_event]); !queued.is_ok())
            {
                return queued;
            }

            ++next_live_event;
        }

        return m110::Status::success();
    };
    const auto report_enqueue_failure = [](const m110::Status& refused)
    {
        std::fputs("live replay enqueue failed: ", stderr);
        std::fputs(refused.message, stderr);
        std::fputs("\n", stderr);
    };

    for (std::uint64_t cursor = 0U; cursor < total_frames;)
    {
        if (with_live && replay.has_frames && live.frame() == replay.frames)
        {
            break;
        }

        const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(total_frames - cursor, options.block));

        if (const m110::Status read = source.read(cursor, std::span{input}.first(frames)); !read.is_ok())
        {
            write_text(stderr, std::format("source read failed: {}\n", read.message));
            return 2;
        }

        const m110::Result<std::size_t> produced = engine.process(std::span<const float>{input}.first(frames), std::span{block});

        if (!produced.has_value())
        {
            if (signal_lab::same_failure(produced.error(), signal_lab::engine_failure::clipping))
            {
                std::fprintf(stderr, "render rejected clipping at output frame %llu\n", static_cast<unsigned long long>(engine.stats().first_clipped_frame));
            }
            else
            {
                std::fprintf(stderr, "engine processing failed: %s\n", produced.error().message);
            }

            return 2;
        }

        std::size_t delivered = produced.value();

        if (with_live)
        {
            if (replay.has_frames && replay.frames - live.frame() < delivered)
            {
                delivered = static_cast<std::size_t>(replay.frames - live.frame());
            }

            std::size_t done = 0U;

            while (done < delivered)
            {
                if (const m110::Status queued = queue_live_events(); !queued.is_ok())
                {
                    report_enqueue_failure(queued);
                    return 2;
                }

                std::size_t count = delivered - done;

                // At most 64 events are pending at a time. Stop before the first
                // event not yet queued, then admit it after earlier slots retire.
                if (next_live_event < replay.events.size() && replay.events[next_live_event].frame - live.frame() < count)
                {
                    count = static_cast<std::size_t>(replay.events[next_live_event].frame - live.frame());
                }

                const m110::Status processed = count == 0U ? m110::Status{.code = m110::StatusCode::buffer_too_small, .message = "pending event capacity"} : live.process(block + done, count);

                if (!processed.is_ok())
                {
                    std::fprintf(stderr, "live replay processing failed: %s\n", processed.message);
                    return 2;
                }

                done += count;
            }
        }

        if (const m110::Status written = output.append(std::span<const float>{block}.first(delivered)); !written.is_ok())
        {
            write_text(stderr, std::format("render spool failed: {}\n", written.message));
            return 2;
        }

        cursor += frames;
    }

    if (with_live && replay.has_frames && live.frame() != replay.frames)
    {
        std::fprintf(stderr, "source ends before recorded live_frames\n");
        return 2;
    }

    if (with_live)
    {
        // The events still unqueued: a refusal is reported as itself; with
        // none, an event left over means the controller ran out of room.
        if (const m110::Status queued = queue_live_events(); !queued.is_ok())
        {
            report_enqueue_failure(queued);
            return 2;
        }

        if (next_live_event != replay.events.size())
        {
            std::fputs("live replay leaves more pending controls than the target can retain\n", stderr);
            return 2;
        }
    }

    source.close();
    secondary_source.close();

    if (!output.publish(options.output).is_ok())
    {
        std::fprintf(stderr, "cannot write %s\n", options.output.c_str());
        return 2;
    }

    const signal_lab::EngineStats& stats = engine.stats();
    const auto final_digest = with_live ? live.stats().output_digest : stats.output_digest;
    const auto final_clipped = with_live ? live.stats().clipped_samples : stats.clipped_samples;
    const auto final_peak = with_live ? live.stats().peak : stats.peak;
    const double output_energy = with_live ? live.stats().output_energy : stats.output_energy;
    const double output_rms = output.frames() != 0U ? std::sqrt(output_energy / static_cast<double>(output.frames())) : 0.0;

    if (!options.target_scenario.empty())
    {
        if (const m110::Status written = write_target_scenario(options.target_scenario, scenario_document, scenario_text, reference_rms, resolved_secondary_path); !written.is_ok())
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
        const m110::Status band_report = write_noise_band_report(file, scenario);
        std::fprintf(file, " \"test_id\": %s,\n \"scenario_path\": %s,\n \"source_path\": %s,\n \"output_path\": %s,\n", test_id_json(scenario_document).c_str(), json_string(options.scenario).c_str(),
                     json_string(options.source).c_str(), json_string(options.output).c_str());
        std::fprintf(file, " \"seed\": %llu,\n \"source_gain_db\": %.17g,\n \"reference_rms\": %.17g,\n \"reference_rms_dbfs\": %.6f,\n \"scaled_reference_rms_dbfs\": %.6f,\n",
                     static_cast<unsigned long long>(scenario.seed), scenario.source_gain_db, reference_rms, dbfs(reference_rms), dbfs(engine.scaled_reference_rms()));
        std::fprintf(file, " \"block_frames\": %zu,\n \"frames_in\": %llu,\n \"frames_out\": %llu,\n \"clipped_samples\": %llu,\n \"peak_dbfs\": %.6f,\n \"rms_dbfs\": %.6f,\n", options.block,
                     static_cast<unsigned long long>(stats.frames_in), static_cast<unsigned long long>(output.frames()), static_cast<unsigned long long>(final_clipped), dbfs(final_peak),
                     dbfs(output_rms));

        if (with_live)
        {
            std::fprintf(file,
                         " \"live_replay\": {\"path\": %s, \"seed\": %llu, \"reference_rms\": %.17g, \"controls_accepted\": %u, \"controls_applied\": %u,"
                         " \"pending_controls\": %zu, \"static_events_started\": %llu, \"static_events_dropped\": %llu, \"engine_output_digest\": \"%s\","
                         " \"engine_frames_out\": %llu, \"engine_clipped_samples\": %llu},\n",
                         json_string(options.live_events).c_str(), static_cast<unsigned long long>(replay.seed), replay.reference_rms, live.stats().controls_accepted, live.stats().controls_applied,
                         live.pending_count(), static_cast<unsigned long long>(live.stats().static_events_started), static_cast<unsigned long long>(live.stats().static_events_dropped),
                         hex64(stats.output_digest).c_str(), static_cast<unsigned long long>(stats.frames_out), static_cast<unsigned long long>(stats.clipped_samples));
        }

        write_watterson_report(file, engine.watterson_report());
        write_frequency_shift_report(file, engine.scenario());

        if (has_second_source)
        {
            write_secondary_source_report(file, scenario, secondary_source);
        }

        std::fprintf(file, " \"output_digest\": \"%s\",\n \"source_digest\": \"%s\",\n \"arena_bytes_used\": %zu,\n \"stages\": [", hex64(final_digest).c_str(), hex64(stats.source_digest).c_str(),
                     stats.arena_bytes_used);

        for (std::uint32_t index = 0U; index < scenario.stage_count; ++index)
        {
            const signal_lab::Impairment* stage = engine.stage(index);
            const signal_lab::StageStats& s = stage->stats();
            const double component_rms = s.component_frames != 0U ? std::sqrt(s.component_energy / static_cast<double>(s.component_frames)) : 0.0;
            std::fprintf(file,
                         "%s\n  {\"order\": %u, \"type\": \"%s\", \"events_scheduled\": %u, \"events_applied\": %u, \"events_dropped\": %u,"
                         " \"component_peak_dbfs\": %.6f, \"component_rms_dbfs\": %.6f, \"component_frames\": %llu",
                         index == 0U ? "" : ",", index, signal_lab::stage_type_name(stage->type()), s.events_scheduled, s.events_applied, s.events_dropped, dbfs(s.component_peak), dbfs(component_rms),
                         static_cast<unsigned long long>(s.component_frames));
            write_cw_pulse_report(file, scenario.stages[index]);
            write_cw_sweep_report(file, scenario.stages[index]);
            write_cw_markov_report(file, scenario.stages[index]);
            write_cw_fsk_report(file, scenario.stages[index]);
            write_cw_fading_report(file, scenario.stages[index]);
            write_random_key(file, scenario, index);
            std::fputc('}', file);
        }

        std::fprintf(file, "\n ]\n}\n");

        if (const m110::Status written = finish_sidecar(file, band_report); !written.is_ok())
        {
            std::fprintf(stderr, "cannot write %s\n", options.sidecar.c_str());
            return 2;
        }
    }

    std::printf("output=%s frames_in=%llu frames_out=%llu clipped=%llu peak_dbfs=%.2f digest=%s source_digest=%s stages=%u events=%u/%u/%u arena=%zu\n", options.output.c_str(),
                static_cast<unsigned long long>(stats.frames_in), static_cast<unsigned long long>(output.frames()), static_cast<unsigned long long>(final_clipped), dbfs(final_peak),
                hex64(final_digest).c_str(), hex64(stats.source_digest).c_str(), stats.stage_count, stats.events_scheduled, stats.events_applied, stats.events_dropped, stats.arena_bytes_used);
    return 0;
}
