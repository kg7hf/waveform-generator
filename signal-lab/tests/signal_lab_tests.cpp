// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

// Host unit tests for the portable signal_lab engine (no framework; exit code = failures).

#include "signal_lab/det_math.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/sample_stream.hpp"
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

    for (const char* count :
{"0", "-1", "1.5", "true", "4294967296", "1e300", "1e999"
})
    {
        const std::string fade = std::string("{\"impairments\":[{\"type\":\"fade\",\"depth_db\":18,\"duration_ms\":250,\"period_seconds\":1,\"count\":") + count + "}]}";
        check(!signal_lab::parse_scenario(fade.c_str(), fade.size(), scenario, &error), "fade rejects invalid count before conversion");
    }
}

void test_engine_determinism()
{
    const auto source = synthetic_signal();
    const double reference = rms_of(source);
    const auto a = render(engine_a, mix005, source, 2048U, reference);
    const auto b = render(engine_b, mix005, source, 2048U, reference);
    check(!a.empty() && a == b, "same scenario renders identically");
    check(engine_a.stats().output_digest == engine_b.stats().output_digest, "digests match");
    const auto c = render(engine_b, mix005, source, 1000U, reference);
    check(a == c, "block size does not change the output");
    check(a.size() == source.size() - 10U, "two 5-sample deletions shorten the stream");
    std::string other = mix005;
    other.replace(other.find("\"seed\": 234"), 11U, "\"seed\": 235");
    const auto d = render(engine_b, other, source, 2048U, reference);
    check(d != a, "seed changes the output");
    check(engine_a.stats().arena_bytes_used > 0U && engine_a.stats().stage_count == 5U, "arena in use");
}

void test_levels()
{
    const auto source = synthetic_signal();
    const double reference = rms_of(source);
    const std::string cw = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"cw\", \"frequency_hz\": 1800, \"ci_db\": 6, \"ramp_seconds\": 0}]}";
    const auto with_cw = render(engine_a, cw, source, 2048U, reference);
    double energy = 0.0;

    for (std::size_t n = 0U; n < source.size(); ++n)
    {
        const double d = static_cast<double>(with_cw[n] - source[n]) / 32768.0;
        energy += d * d;
    }

    const double tone_rms = std::sqrt(energy / static_cast<double>(source.size()));
    check_near(20.0 * std::log10(reference / tone_rms), 6.0, 0.05, "cw C/I");
    const std::string awgn = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"awgn\", \"snr_db\": 10}]}";
    const auto with_noise = render(engine_a, awgn, source, 2048U, reference);
    energy = 0.0;

    for (std::size_t n = 0U; n < source.size(); ++n)
    {
        const double d = static_cast<double>(with_noise[n] - source[n]) / 32768.0;
        energy += d * d;
    }

    check_near(20.0 * std::log10(reference / std::sqrt(energy / static_cast<double>(source.size()))), 10.0, 0.3, "awgn SNR");
    const std::string fade = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"fade\", \"depth_db\": 18, \"duration_ms\": 250, \"starts_seconds\": [1.0]}]}";
    const auto faded = render(engine_a, fade, source, 2048U, reference);
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
    const auto scaled = render(engine_b, passthrough, source, 2048U, reference);
    const std::string impulse =
        "{\"source_gain_db\": -12, \"impairments\": [{\"type\": \"impulse\", \"period_seconds\": 1.0, \"first_seconds\": 0.5, \"peak_db\": 12, \"decay_ms\": 5, \"ring_hz\": 0, \"phase_degrees\": 0}]}";
    const auto crashed = render(engine_a, impulse, source, 2048U, reference);
    const double first = static_cast<double>(crashed[24000] - scaled[24000]) / 32768.0;
    check_near(first, engine_a.scaled_reference_rms() * signal_lab::det::db_to_amplitude(12.0), 2.0 / 32768.0, "impulse peak amplitude");
    check(engine_a.stage(0)->stats().events_applied == 3U, "three periodic crashes");
    const std::string ring = "{\"source_gain_db\": -12, \"impairments\": [{\"type\": \"impulse\", \"rate_per_sec\": 5, \"peak_db\": 30, \"decay_ms\": 35, \"ring\": \"noise\"}]}";
    const auto noisy = render(engine_a, ring, source, 2048U, reference);
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
    auto out = render(engine_a, del, ramp, 2048U, 0.3);
    check(out.size() == ramp.size() - 3U && out[4799] == 4799 && out[4800] == 4803, "delete removes [4800, 4803)");
    const std::string dup = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.1, \"kind\": \"duplicate\", \"length_samples\": 3}]}]}";
    out = render(engine_a, dup, ramp, 2048U, 0.3);
    check(out.size() == ramp.size() + 3U && out[4800] == 4797 && out[4802] == 4799 && out[4803] == 4800, "duplicate re-emits the last three");
    // Boundary-straddling delete with a tiny block size (delete carries across blocks).
    const std::string del48 = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.0999, \"kind\": \"delete\", \"length_samples\": 48}]}]}";
    out = render(engine_a, del48, ramp, 100U, 0.3);
    const std::size_t at = static_cast<std::size_t>(signal_lab::det::seconds_to_frames(0.0999, 48000U));
    check(out.size() == ramp.size() - 48U && out[at] == static_cast<std::int16_t>(at + 48U), "48-sample delete across blocks");
    const auto reference_out = render(engine_b, del48, ramp, 2048U, 0.3);
    check(out == reference_out, "slip output independent of block size");
}

void test_slip_validation_and_capacity()
{
    signal_lab::Scenario scenario;
    const char* error = nullptr;

    for (const char* length :
{"0", "-1", "1.5", "1024.5", "1025", "48000", "4294967297", "1e300", "1e999"
})
    {
        const std::string compact = std::string("{\"impairments\":[{\"type\":\"sample_slip\",\"kind\":\"delete\",\"at_seconds\":0,\"length_samples\":") + length + "}]}";
        check(!signal_lab::parse_scenario(compact.c_str(), compact.size(), scenario, &error), "compact slip rejects invalid length before conversion");
        const std::string explicit_event = std::string("{\"impairments\":[{\"type\":\"sample_slip\",\"events\":[{\"kind\":\"delete\",\"at_seconds\":0,\"length_samples\":") + length + "}]}]}";
        check(!signal_lab::parse_scenario(explicit_event.c_str(), explicit_event.size(), scenario, &error), "explicit slip rejects invalid length before conversion");
    }

    for (const char* count :
{"0", "-1", "1.5", "true", "32.5", "33", "4294967297", "1e300", "1e999"
})
    {
        const std::string slip =
            std::string("{\"impairments\":[{\"type\":\"sample_slip\",\"kind\":\"delete\",\"placement\":\"spaced\",\"first_seconds\":0,\"period_seconds\":0.001,\"length_samples\":1,\"count\":") + count + "}]}";
        check(!signal_lab::parse_scenario(slip.c_str(), slip.size(), scenario, &error), "sample slip rejects invalid count before conversion");
    }
    std::vector<std::int16_t> source(8192U);

    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        source[index] = static_cast<std::int16_t>(10000U + index);
    }

    const auto configure = [&](const std::string & text)
    {
        check(signal_lab::parse_scenario(text.c_str(), text.size(), scenario, &error), "slip regression scenario parses");
        return engine_a.configure(scenario, 0.3, source.size(), &error);
    };
    const std::string no_history =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","events":[{"kind":"delete","at_seconds":0,"length_samples":499},{"kind":"duplicate","at_seconds":0.010416666666666666,"length_samples":400}]}]})";
    check(!configure(no_history) && std::strstr(error, "previously emitted") != nullptr, "deletion cannot supply duplicate history");
    const std::string too_large = R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","at_seconds":0.01,"length_samples":400}]})";
    check(!configure(too_large) && std::strstr(error, "256") != nullptr, "single duplicate exceeding output slack is rejected before rendering");
    const std::string too_dense =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","placement":"clustered","first_seconds":0.01,"spacing_seconds":0.001,"count":3,"length_samples":128}]})";
    check(!configure(too_dense) && std::strstr(error, "256") != nullptr, "clustered duplicate expansion is checked as a whole");
    const std::string maximum_delete = R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"delete","at_seconds":0.01,"length_samples":1024}]})";
    const auto deleted = render(engine_a, maximum_delete, source, 2048U, 0.3);
    check(deleted.size() == source.size() - signal_lab::max_slip_length && deleted[480] == source[1504], "1024-sample deletion remains supported");
    check(deleted == render(engine_b, maximum_delete, source, 100U, 0.3), "maximum deletion is independent of block size");
    const std::string within_slack =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","placement":"clustered","first_seconds":0.01,"spacing_seconds":0.001,"count":2,"length_samples":128}]})";
    const auto expanded = render(engine_a, within_slack, source, 2048U, 0.3);
    check(expanded.size() == source.size() + signal_lab::engine_slack_frames && expanded.back() == source.back(), "all source frames survive maximum clustered expansion");
    check(expanded == render(engine_b, within_slack, source, 100U, 0.3), "maximum clustered expansion is independent of block size");
    const std::string separated =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","placement":"spaced","first_seconds":0.01,"period_seconds":0.042666666666666665,"count":2,"length_samples":256}]})";
    const auto separate_output = render(engine_a, separated, source, 2048U, 0.3);
    check(separate_output.size() == source.size() + 512U && separate_output == render(engine_b, separated, source, 100U, 0.3), "duplicates exactly one maximum block apart are supported");
    const std::string dropped_duplicate =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","events":[{"kind":"delete","at_seconds":0,"length_samples":1024},{"kind":"duplicate","at_seconds":0.01,"length_samples":400}]}]})";
    const auto dropped = render(engine_a, dropped_duplicate, source, 2048U, 0.3);
    check(dropped.size() == source.size() - 1024U && engine_a.stats().events_dropped == 1U, "duplicate inside deleted input does not consume expansion or history");
    check(dropped == render(engine_b, dropped_duplicate, source, 100U, 0.3), "dropped duplicate is independent of block size");
    // Exercise stage defenses when the caller violates continuity or provides too
    // little expansion space: source samples must still survive without bad reads.
    alignas(signal_lab::impairment_alignment) unsigned char storage[8192] {};
    float scratch[signal_lab::engine_capacity_frames] {};
    float samples[signal_lab::engine_capacity_frames] {};

    for (std::size_t index = 0U; index < 480U; ++index)
    {
        samples[index] = static_cast<float>(index);
    }

    signal_lab::SampleSlipParams params;
    params.event_count = 1U;
    params.events[0] = {0.01, true, 128U};
    signal_lab::Impairment* stage = signal_lab::construct_sample_slip(params, storage, sizeof(storage));
    check(stage != nullptr && stage->prepare({0.3, 8192U, 0U, 0U, scratch}, &error), "standalone slip stage prepares");
    check(stage->process(samples, 200U, 480U, signal_lab::engine_capacity_frames) == 200U &&
          stage->stats().events_dropped == 1U && samples[199] == 199.0f,
          "missing runtime history drops the duplicate even with sufficient output space");
    check(stage->prepare({0.3, 8192U, 0U, 0U, scratch}, &error), "standalone slip stage resets history");
    check(stage->process(samples, 480U, 0U, signal_lab::engine_capacity_frames) == 480U,
          "standalone slip stage establishes enough duplicate history");
    check(stage->process(samples, 200U, 480U, 200U) == 200U && stage->stats().events_dropped == 1U && samples[199] == 199.0f,
          "insufficient expansion space preserves source frames even with valid history");
}

void test_fade_event_boundaries()
{
    const std::vector<std::int16_t> source(8192U, 10000);
    const std::string spaced = R"({"source_gain_db":0,"impairments":[{"type":"fade","depth_db":20,"duration_ms":1,"shape":"rectangular","period_seconds":0.002,"count":32}]})";
    const auto output = render(engine_a, spaced, source, 2048U, 0.3);
    check(engine_a.stats().events_applied == 32U && engine_a.stats().events_dropped == 0U, "nonoverlapping fades do not compete for active slots");

    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        const bool faded = index < 32U * 96U && index % 96U < 48U;
        check(output[index] == (faded ? 1000 : 10000), "every scheduled rectangular fade has the expected samples");
    }

    check(output == render(engine_b, spaced, source, 100U, 0.3), "nonoverlapping fades are independent of block size");
    // Eight fades end exactly when the next eight start; the ninth simultaneous
    // fade in each group is the only event dropped at either block size.
    const std::string overlap =
        R"({"source_gain_db":0,"impairments":[{"type":"fade","depth_db":1,"duration_ms":1,"shape":"rectangular","starts_seconds":[0,0,0,0,0,0,0,0,0,0.001,0.001,0.001,0.001,0.001,0.001,0.001,0.001,0.001]}]})";
    const auto overlapping = render(engine_a, overlap, source, 2048U, 0.3);
    check(engine_a.stats().events_applied == 16U && engine_a.stats().events_dropped == 2U, "fade capacity counts true simultaneous events and reuses finished slots");
    check(overlapping == render(engine_b, overlap, source, 17U, 0.3) && engine_b.stats().events_applied == 16U && engine_b.stats().events_dropped == 2U,
          "overlap admission and retirement are independent of block size");
}

void test_processing_failures_and_emitted_levels()
{
    signal_lab::Scenario scenario;
    scenario.source_gain_db = 6.0;
    scenario.saturate = true;
    const char* error = nullptr;
    const std::int16_t input[] = {0, 1, -1, 123, -123, 32767, -32768};
    std::int16_t output[signal_lab::engine_capacity_frames] {};
    check(engine_a.configure(scenario, 0.3, 7U, &error), "saturating level scenario configures");
    check(engine_a.process(input, 7U, output, signal_lab::engine_capacity_frames) == 7U && engine_a.process_error() == signal_lab::ProcessError::none,
          "saturation still emits every frame");
    double emitted_energy = 0.0;

    for (std::size_t index = 0U; index < 7U; ++index)
    {
        const double sample = static_cast<double>(output[index]) / 32768.0;
        emitted_energy += sample * sample;
    }

    check(output[5] == 32767 && output[6] == -32768 && engine_a.stats().clipped_samples == 2U, "saturation clamps both PCM rails");
    check(engine_a.stats().peak == 1.0f && engine_a.stats().output_energy == emitted_energy, "level statistics measure quantized emitted PCM after saturation");
    scenario.saturate = false;
    const std::int16_t good[] = {1, -1, 123, -123};
    const std::int16_t bad[] = {32767, 10, -32768, 20};
    check(engine_a.configure(scenario, 0.3, 8U, &error), "rejecting level scenario configures");
    check(engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames) == 4U, "unclipped prefix is accepted");
    const signal_lab::EngineStats prefix = engine_a.stats();
    check(engine_a.process(bad, 4U, output, signal_lab::engine_capacity_frames) == 0U && engine_a.process_error() == signal_lab::ProcessError::clipping,
          "clip policy rejects the entire offending block");
    const signal_lab::EngineStats rejected = engine_a.stats();
    signal_lab::det::StreamDigest consumed;
    consumed.update(good, 4U);
    consumed.update(bad, 4U);
    check(rejected.frames_in == 8U && rejected.source_digest == consumed.value(), "rejected block is counted as consumed source");
    check(rejected.frames_out == prefix.frames_out && rejected.output_digest == prefix.output_digest && rejected.peak == prefix.peak &&
          rejected.output_energy == prefix.output_energy,
          "rejected block commits no output frames digest or levels");
    check(rejected.clipped_samples == 2U && rejected.first_clipped_frame == 4U, "clipping diagnostic identifies first rejected output frame");
    check(engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames) == 0U && engine_a.stats().frames_in == rejected.frames_in,
          "processing failure remains latched without consuming further input");
    check(engine_a.configure(scenario, 0.3, 4U, &error) && engine_a.process_error() == signal_lab::ProcessError::none &&
          engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames) == 4U,
          "configure clears a previous processing failure");
    engine_a.configure_passthrough();
    check(engine_a.process(good, 4U, output, 4U + signal_lab::engine_slack_frames - 1U) == 0U &&
          engine_a.process_error() == signal_lab::ProcessError::invalid_buffer && engine_a.stats().frames_in == 0U,
          "insufficient output slack fails before consuming input");
    engine_a.configure_passthrough();
    check(engine_a.process_error() == signal_lab::ProcessError::none && engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames) == 4U &&
          output[0] == good[0] && output[3] == good[3],
          "passthrough configuration clears failures and preserves samples");
}

void test_pipeline_error_propagation()
{
    class Source final : public signal_lab::SampleSource
    {
    public:
        const std::int16_t* samples{};
        std::size_t length{};
        std::size_t cursor{};
        [[nodiscard]] std::size_t read(std::int16_t* frames, std::size_t capacity) noexcept override
        {
            std::size_t count = length - cursor;
            count = count < 4U ? count : 4U;
            count = count < capacity ? count : capacity;

            for (std::size_t index = 0U; index < count; ++index)
            {
                frames[index] = samples[cursor++];
            }

            return count;
        }
        [[nodiscard]] std::uint64_t total_frames() const noexcept override
        {
            return length;
        }
    } source;
    class Sink final : public signal_lab::SampleSink
    {
    public:
        std::size_t frames{};
        std::size_t calls{};
        [[nodiscard]] bool write(const std::int16_t*, std::size_t count) noexcept override
        {
            frames += count;
            ++calls;
            return true;
        }
    } sink;
    const std::int16_t samples[] = {1, -1, 123, -123, 32767, 10, -32768, 20};
    source.samples = samples;
    source.length = 8U;
    signal_lab::Scenario scenario;
    scenario.source_gain_db = 6.0;
    scenario.saturate = false;
    const char* error = nullptr;
    std::int16_t input_block[signal_lab::engine_block_frames] {};
    std::int16_t output_block[signal_lab::engine_capacity_frames] {};
    check(engine_a.configure(scenario, 0.3, source.total_frames(), &error), "pipeline rejecting scenario configures");
    check(!signal_lab::run_pipeline(source, engine_a, sink, input_block, output_block) && sink.frames == 4U && sink.calls == 1U &&
          engine_a.process_error() == signal_lab::ProcessError::clipping,
          "pipeline propagates clipping and does not deliver the rejected block");
    source.cursor = 0U;
    source.length = 4U;
    sink.frames = sink.calls = 0U;
    scenario.source_gain_db = 0.0;
    scenario.stage_count = 1U;
    scenario.stages[0].type = signal_lab::StageType::sample_slip;
    scenario.stages[0].slip.event_count = 1U;
    scenario.stages[0].slip.events[0] = {0.0, false, 4U};
    check(engine_a.configure(scenario, 0.3, source.total_frames(), &error), "pipeline deletion scenario configures");
    check(signal_lab::run_pipeline(source, engine_a, sink, input_block, output_block) && sink.frames == 0U && sink.calls == 0U &&
          engine_a.stats().frames_in == 4U && engine_a.process_error() == signal_lab::ProcessError::none,
          "pipeline accepts a successful zero-output deletion block");
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
    test_slip_validation_and_capacity();
    test_fade_event_boundaries();
    test_processing_failures_and_emitted_levels();
    test_pipeline_error_propagation();

    if (failures == 0)
    {
        std::printf("signal_lab tests: PASS\n");
        return 0;
    }

    std::printf("signal_lab tests: %d FAILURES\n", failures);
    return 1;
}
