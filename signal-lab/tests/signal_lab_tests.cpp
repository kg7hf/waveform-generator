// Host unit tests for the portable signal_lab engine (no framework; exit code = failures).

#include "signal_lab/det_math.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int printed = 0;

void report(const char* what, const char* detail)
{
    ++failures;
    if (printed < 40)
    {
        ++printed;
        std::printf("FAIL: %s%s\n", what, detail);
    }
}

void check(bool condition, const char* what)
{
    if (!condition)
    {
        report(what, "");
    }
}

void check_near(double actual, double expected, double tolerance, const char* what)
{
    if (!(std::fabs(actual - expected) <= tolerance))
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), " (actual %.15g expected %.15g)", actual, expected);
        report(what, detail);
    }
}

signal_lab::Engine engine_a;
signal_lab::Engine engine_b;

// Deterministic pseudo-signal: three tones at -8.4 dBFS RMS, 3 seconds.
std::vector<std::int16_t> synthetic_signal(std::size_t frames = 3U * 48000U)
{
    std::vector<std::int16_t> pcm(frames);
    const double amplitude = 0.3803 * std::sqrt(2.0 / 3.0);
    for (std::size_t n = 0U; n < frames; ++n)
    {
        const double t = static_cast<double>(n) / 48000.0;
        const double x = amplitude * (signal_lab::det::sin(2.0 * signal_lab::det::pi * 700.0 * t) +
                                      signal_lab::det::sin(2.0 * signal_lab::det::pi * 1800.0 * t + 1.0) +
                                      signal_lab::det::sin(2.0 * signal_lab::det::pi * 2600.0 * t + 2.0));
        pcm[n] = static_cast<std::int16_t>(signal_lab::det::round_half_even(x * 32768.0));
    }
    return pcm;
}

double rms_of(const std::vector<std::int16_t>& pcm, std::size_t from = 0U, std::size_t to = 0U)
{
    if (to == 0U)
    {
        to = pcm.size();
    }
    double energy = 0.0;
    for (std::size_t n = from; n < to; ++n)
    {
        const double x = static_cast<double>(pcm[n]) / 32768.0;
        energy += x * x;
    }
    return std::sqrt(energy / static_cast<double>(to - from));
}

std::vector<std::int16_t> render(signal_lab::Engine& engine, const std::string& scenario_text, const std::vector<std::int16_t>& source, std::size_t block, double reference_rms)
{
    signal_lab::Scenario scenario;
    const char* error = nullptr;
    if (!signal_lab::parse_scenario(scenario_text.c_str(), scenario_text.size(), scenario, &error))
    {
        std::printf("scenario parse error: %s\n", error);
        ++failures;
        return {};
    }
    if (!engine.configure(scenario, reference_rms, source.size(), &error))
    {
        std::printf("configure error: %s\n", error);
        ++failures;
        return {};
    }
    std::vector<std::int16_t> output;
    std::int16_t buffer[signal_lab::engine_capacity_frames];
    for (std::size_t cursor = 0U; cursor < source.size();)
    {
        const std::size_t frames = (source.size() - cursor) < block ? (source.size() - cursor) : block;
        const std::size_t produced = engine.process(source.data() + cursor, frames, buffer, signal_lab::engine_capacity_frames);
        output.insert(output.end(), buffer, buffer + produced);
        cursor += frames;
    }
    return output;
}

void test_det_math()
{
    for (int i = -2000; i <= 2000; ++i)
    {
        const double x = static_cast<double>(i) * 0.0173;
        check_near(signal_lab::det::sin(x), std::sin(x), 2e-14, "det::sin");
        check_near(signal_lab::det::cos(x), std::cos(x), 2e-14, "det::cos");
    }
    for (int i = -300; i <= 300; ++i)
    {
        const double x = static_cast<double>(i) * 0.37;
        check_near(signal_lab::det::exp(x) / std::exp(x), 1.0, 1e-13, "det::exp");
        const double y = std::exp(x);
        check_near(signal_lab::det::log(y), x, 1e-12 * (1.0 + std::fabs(x)), "det::log");
    }
    check_near(signal_lab::det::sin(3.4e6), std::sin(3.4e6), 1e-9, "det::sin large argument");
    check(signal_lab::det::round_half_even(2.5) == 2.0, "round_half_even 2.5");
    check(signal_lab::det::round_half_even(3.5) == 4.0, "round_half_even 3.5");
    check(signal_lab::det::round_half_even(-2.5) == -2.0, "round_half_even -2.5");
    check(signal_lab::det::round_half_even(0.4999) == 0.0, "round_half_even 0.4999");
    check(signal_lab::det::seconds_to_frames(0.25, 48000U) == 12000U, "seconds_to_frames");
    check_near(signal_lab::det::db_to_amplitude(-6.0), std::pow(10.0, -0.3), 1e-13, "db_to_amplitude");
    signal_lab::det::Pcg32 rng(42U, 1U, 0U);
    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < 200000; ++i)
    {
        const double g = rng.gaussian();
        sum += g;
        sum_sq += g * g;
    }
    check_near(sum / 200000.0, 0.0, 0.01, "gaussian mean");
    check_near(sum_sq / 200000.0, 1.0, 0.01, "gaussian variance");
    signal_lab::det::Pcg32 again(42U, 1U, 0U);
    check(again.next() == signal_lab::det::Pcg32(42U, 1U, 0U).next(), "pcg32 reproducible");
    check(signal_lab::det::Pcg32(42U, 1U, 0U).next() != signal_lab::det::Pcg32(42U, 2U, 0U).next(), "pcg32 family separation");
}

void test_json()
{
    const char* literals[] = {"0", "7", "7.0", "-12", "0.25", "3.3e3", "1e-3", "123.456", "0.1", "1800", "0.38033893537026803"};
    signal_lab::json::Document document;
    for (const char* literal : literals)
    {
        check(document.parse(literal, std::strlen(literal)), "json number parses");
        const signal_lab::json::Value* root = document.root();
        check(root != nullptr && root->type == signal_lab::json::Type::number, "json number type");
        if (root != nullptr)
        {
            check(root->number == std::strtod(literal, nullptr), literal);
        }
    }
    const char* text = "{\"a\": [1, 2, {\"b\": \"x\\\"y\"}], \"c\": true, \"d\": null, \"e\": -3.5}";
    check(document.parse(text, std::strlen(text)), "json object parses");
    const signal_lab::json::Value* root = document.root();
    check(root != nullptr && root->child_count == 4U, "json object children");
    const signal_lab::json::Value* a = document.find(*root, "a");
    check(a != nullptr && a->type == signal_lab::json::Type::array && a->child_count == 3U, "json array");
    const signal_lab::json::Value* third = document.at(*a, 2U);
    check(third != nullptr && document.find(*third, "b") != nullptr, "json nested object");
    check(document.number_or(*root, "e", 0.0) == -3.5, "json number_or");
    check(!document.parse("{\"a\": }", 7U), "json rejects bad document");
}

const std::string mix005 =
    "{\"schema\": \"signal-lab.scenario/1\", \"test_id\": \"m110_600L_mix_MIX005_seed0234\", \"seed\": 234,"
    " \"source\": {\"path\": \"../reference/x.wav\"}, \"reference\": \"auto\", \"source_gain_db\": -12.0, \"clip_policy\": \"saturate\","
    " \"impairments\": ["
    "  {\"type\": \"fade\", \"depth_db\": 24, \"duration_ms\": 250, \"shape\": \"raised_cosine\", \"first_seconds\": 1, \"period_seconds\": 1},"
    "  {\"type\": \"awgn\", \"snr_db\": 3.0},"
    "  {\"type\": \"cw\", \"frequency_hz\": 1800, \"ci_db\": 3},"
    "  {\"type\": \"impulse\", \"rate_per_sec\": 1.0, \"peak_db\": 20, \"decay_ms\": 20},"
    "  {\"type\": \"sample_slip\", \"kind\": \"delete\", \"length_samples\": 5, \"placement\": \"spaced\", \"first_seconds\": 0.5, \"period_seconds\": 1, \"count\": 2}"
    " ]}";

void test_scenario()
{
    signal_lab::Scenario scenario;
    const char* error = nullptr;
    check(signal_lab::parse_scenario(mix005.c_str(), mix005.size(), scenario, &error), "MIX-005 parses");
    check(scenario.stage_count == 5U, "five stages");
    check(scenario.seed == 234U && scenario.source_gain_db == -12.0 && scenario.saturate, "scenario scalars");
    check(std::strcmp(scenario.test_id, "m110_600L_mix_MIX005_seed0234") == 0, "test_id");
    check(scenario.stages[0].type == signal_lab::StageType::fade && scenario.stages[0].fade.depth_db == 24.0, "fade stage");
    check(scenario.stages[1].type == signal_lab::StageType::awgn && scenario.stages[1].awgn.snr_db == 3.0, "awgn stage");
    check(scenario.stages[4].type == signal_lab::StageType::sample_slip && scenario.stages[4].slip.event_count == 2U &&
              scenario.stages[4].slip.events[1].at_seconds == 1.5,
          "slip placement expands");
    const std::string bad = "{\"impairments\": [{\"type\": \"sample_slip\", \"kind\": \"delete\", \"length_samples\": 1, \"at_seconds\": 1}, {\"type\": \"cw\", \"frequency_hz\": 1800, \"ci_db\": 3}]}";
    check(!signal_lab::parse_scenario(bad.c_str(), bad.size(), scenario, &error), "slip must be last");
    const std::string with_reference = "{\"reference_rms\": 0.25, \"impairments\": []}";
    check(signal_lab::parse_scenario(with_reference.c_str(), with_reference.size(), scenario, &error) && scenario.has_reference_rms && scenario.reference_rms == 0.25, "reference_rms");
}

void test_engine_determinism()
{
    const auto source = synthetic_signal();
    const double reference = rms_of(source);
    const auto a = render(engine_a, mix005, source, 2048U,reference);
    const auto b = render(engine_b, mix005, source, 2048U,reference);
    check(!a.empty() && a == b, "same scenario renders identically");
    check(engine_a.stats().output_digest == engine_b.stats().output_digest, "digests match");
    const auto c = render(engine_b, mix005, source, 1000U, reference);
    check(a == c, "block size does not change the output");
    check(a.size() == source.size() - 10U, "two 5-sample deletions shorten the stream");
    std::string other = mix005;
    other.replace(other.find("\"seed\": 234"), 11U, "\"seed\": 235");
    const auto d = render(engine_b, other, source, 2048U,reference);
    check(d != a, "seed changes the output");
    check(engine_a.stats().arena_bytes_used > 0U && engine_a.stats().stage_count == 5U, "arena in use");
}

void test_levels()
{
    const auto source = synthetic_signal();
    const double reference = rms_of(source);
    const std::string cw = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"cw\", \"frequency_hz\": 1800, \"ci_db\": 6, \"ramp_seconds\": 0}]}";
    const auto with_cw = render(engine_a, cw, source, 2048U,reference);
    double energy = 0.0;
    for (std::size_t n = 0U; n < source.size(); ++n)
    {
        const double d = static_cast<double>(with_cw[n] - source[n]) / 32768.0;
        energy += d * d;
    }
    const double tone_rms = std::sqrt(energy / static_cast<double>(source.size()));
    check_near(20.0 * std::log10(reference / tone_rms), 6.0, 0.05, "cw C/I");

    const std::string awgn = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"awgn\", \"snr_db\": 10}]}";
    const auto with_noise = render(engine_a, awgn, source, 2048U,reference);
    energy = 0.0;
    for (std::size_t n = 0U; n < source.size(); ++n)
    {
        const double d = static_cast<double>(with_noise[n] - source[n]) / 32768.0;
        energy += d * d;
    }
    check_near(20.0 * std::log10(reference / std::sqrt(energy / static_cast<double>(source.size()))), 10.0, 0.3, "awgn SNR");

    const std::string fade = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"fade\", \"depth_db\": 18, \"duration_ms\": 250, \"starts_seconds\": [1.0]}]}";
    const auto faded = render(engine_a, fade, source, 2048U,reference);
    bool untouched = true;
    for (std::size_t n = 0U; n < 48000U; ++n)
    {
        untouched = untouched && faded[n] == source[n];
    }
    for (std::size_t n = 48000U + 12000U + 1U; n < source.size(); ++n)
    {
        untouched = untouched && faded[n] == source[n];
    }
    check(untouched, "fade leaves other regions untouched");
    const double mid = rms_of(faded, 48000U + 3000U, 48000U + 9000U) / rms_of(source, 48000U + 3000U, 48000U + 9000U);
    check_near(20.0 * std::log10(mid), -18.0, 0.1, "fade hold depth");
    check(engine_a.stage(0)->stats().events_applied == 1U, "fade event counted");

    // +12 dB above a -12 dB scaled reference stays inside full scale, so the first sample equals A exactly.
    const std::string passthrough = "{\"source_gain_db\": -12, \"impairments\": []}";
    const auto scaled = render(engine_b, passthrough, source, 2048U,reference);
    const std::string impulse = "{\"source_gain_db\": -12, \"impairments\": [{\"type\": \"impulse\", \"period_seconds\": 1.0, \"first_seconds\": 0.5, \"peak_db\": 12, \"decay_ms\": 5, \"ring_hz\": 0, \"phase_degrees\": 0}]}";
    const auto crashed = render(engine_a, impulse, source, 2048U,reference);
    const double first = static_cast<double>(crashed[24000] - scaled[24000]) / 32768.0;
    check_near(first, engine_a.scaled_reference_rms() * signal_lab::det::db_to_amplitude(12.0), 2.0 / 32768.0, "impulse peak amplitude");
    check(engine_a.stage(0)->stats().events_applied == 3U, "three periodic crashes");

    const std::string ring = "{\"source_gain_db\": -12, \"impairments\": [{\"type\": \"impulse\", \"rate_per_sec\": 5, \"peak_db\": 30, \"decay_ms\": 35, \"ring\": \"noise\"}]}";
    const auto noisy = render(engine_a, ring, source, 2048U,reference);
    check(!noisy.empty() && engine_a.stage(0)->stats().events_applied > 5U, "noise-ringing crashes scheduled");
}

void test_slips()
{
    std::vector<std::int16_t> ramp(20000U);
    for (std::size_t n = 0U; n < ramp.size(); ++n)
    {
        ramp[n] = static_cast<std::int16_t>(n % 30000U);
    }
    const std::string del = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.1, \"kind\": \"delete\", \"length_samples\": 3}]}]}";
    auto out = render(engine_a, del, ramp, 2048U,0.3);
    check(out.size() == ramp.size() - 3U && out[4799] == 4799 && out[4800] == 4803, "delete removes [4800, 4803)");
    const std::string dup = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.1, \"kind\": \"duplicate\", \"length_samples\": 3}]}]}";
    out = render(engine_a, dup, ramp, 2048U,0.3);
    check(out.size() == ramp.size() + 3U && out[4800] == 4797 && out[4802] == 4799 && out[4803] == 4800, "duplicate re-emits the last three");
    // Boundary-straddling delete with a tiny block size (delete carries across blocks).
    const std::string del48 = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.0999, \"kind\": \"delete\", \"length_samples\": 48}]}]}";
    out = render(engine_a, del48, ramp, 100U, 0.3);
    const std::size_t at = static_cast<std::size_t>(signal_lab::det::seconds_to_frames(0.0999, 48000U));
    check(out.size() == ramp.size() - 48U && out[at] == static_cast<std::int16_t>(at + 48U), "48-sample delete across blocks");
    const auto reference_out = render(engine_b, del48, ramp, 2048U,0.3);
    check(out == reference_out, "slip output independent of block size");
}

} // namespace

int main()
{
    test_det_math();
    test_json();
    test_scenario();
    test_engine_determinism();
    test_levels();
    test_slips();
    if (failures == 0)
    {
        std::printf("signal_lab tests: PASS\n");
        return 0;
    }
    std::printf("signal_lab tests: %d FAILURES\n", failures);
    return 1;
}
