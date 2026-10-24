// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// In-memory FatFs checks for the incremental board preparation owner. They
// verify the portable writer/engine handoff and recoverable file ownership;
// they do not model SD timing or power-loss behavior.

#include "rt1170/prepare_job.hpp"

#include "common/pcm24.hpp"
#include "ff.h"
#include "rt1170/secondary_wav.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/watterson.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace waveform_generator;

struct File
{
    std::vector<std::uint8_t> bytes;
};

std::map<std::string, File> files;
bool mounted{};
std::size_t open_files{};
std::string fail_read_path;
std::string fail_write_path;
std::string short_write_path;
std::string fail_sync_path;
std::string fail_close_path;
std::string fail_rename_target;
std::string short_read_path;
std::string fail_seek_path;
std::string fail_stat_path;
bool fail_mount_once{};
bool fail_unmount_once{};
bool corrupt_readback_pad{};
bool corrupt_readback_pcm{};
unsigned int mount_calls{};
unsigned int read_calls{};
unsigned int write_calls{};
unsigned int sync_calls{};
int failures{};

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void put_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void set_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        bytes[offset + shift / 8U] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::vector<std::uint8_t> make_wav(const std::vector<audio::Pcm24Sample>& samples)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(45U + samples.size() * audio::pcm24_bytes_per_sample);

    for (const char value : std::string_view{"RIFF"})
    {
        bytes.push_back(static_cast<std::uint8_t>(value));
    }

    put_u32(bytes, 0U);

    for (const char value : std::string_view{"WAVEfmt "})
    {
        bytes.push_back(static_cast<std::uint8_t>(value));
    }

    put_u32(bytes, 16U);
    put_u16(bytes, 1U);
    put_u16(bytes, 1U);
    put_u32(bytes, 48000U);
    put_u32(bytes, 144000U);
    put_u16(bytes, 3U);
    put_u16(bytes, 24U);

    for (const char value : std::string_view{"data"})
    {
        bytes.push_back(static_cast<std::uint8_t>(value));
    }

    put_u32(bytes, static_cast<std::uint32_t>(samples.size() * audio::pcm24_bytes_per_sample));

    for (const auto sample : samples)
    {
        const std::size_t position = bytes.size();
        bytes.resize(position + audio::pcm24_bytes_per_sample);
        audio::write_pcm24_le(bytes.data() + position, sample);
    }

    if ((samples.size() * audio::pcm24_bytes_per_sample & 1U) != 0U)
    {
        bytes.push_back(0U);
    }

    set_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
    return bytes;
}

std::string full_path(std::string_view basename)
{
    return std::string{"2:/WG/"} + std::string{basename};
}

void put_file(std::string_view basename, std::vector<std::uint8_t> bytes)
{
    files[full_path(basename)] = {.bytes = std::move(bytes)};
}

const std::vector<std::uint8_t>& file_bytes(std::string_view basename)
{
    return files.at(full_path(basename)).bytes;
}

std::vector<audio::Pcm24Sample> samples(std::size_t count)
{
    std::vector<audio::Pcm24Sample> result(count);

    for (std::size_t index = 0U; index < count; ++index)
    {
        const std::int32_t centered = static_cast<std::int32_t>((index * 1937U) % 22001U) - 11000;
        result[index] = centered * 180;
    }

    return result;
}

std::array<float, signal_lab::engine_block_frames> input{};
std::array<float, signal_lab::engine_capacity_frames> output{};
std::array<char, 4096U> scenario_text{};
alignas(32) std::array<std::uint8_t, 6912U> packed{};
alignas(32) std::array<std::uint8_t, 6147U> stream_read_scratch{};
signal_lab::Engine engine;
signal_lab::WattersonChannel watterson;
Rt1170SecondaryWav secondary;
signal_lab::Scenario scenario;

std::vector<audio::Pcm24Sample> render_expected(const std::vector<audio::Pcm24Sample>& source, std::string_view json, signal_lab::SecondarySource* secondary_source = nullptr)
{
    std::vector<audio::Pcm24Sample> rendered;
    signal_lab::Scenario expected_scenario{};
    check(signal_lab::parse_scenario(json.data(), json.size(), expected_scenario).is_ok(), "oracle scenario parses");
    check(engine.configure(expected_scenario, expected_scenario.reference_rms, source.size(), signal_lab::StageResources{&watterson, nullptr, secondary_source}).is_ok(),
          "oracle engine configures from the same scenario");
    std::size_t cursor{};

    while (cursor < source.size())
    {
        const std::size_t count = std::min<std::size_t>(signal_lab::engine_block_frames, source.size() - cursor);

        for (std::size_t index = 0U; index < count; ++index)
        {
            input[index] = audio::pcm24_to_float(source[cursor + index]);
        }

        const auto produced = engine.process(std::span<const float>{input.data(), count}, output);
        check(produced.has_value(), "oracle engine processes one source block");

        if (produced.has_value())
        {
            for (std::size_t index = 0U; index < produced.value(); ++index)
            {
                rendered.push_back(signal_lab::det::quantize_pcm24(output[index]));
            }
        }

        cursor += count;
    }

    return rendered;
}

PrepareJob::Resources resources()
{
    return {.engine = &engine,
            .watterson = &watterson,
            .secondary = &secondary,
            .scenario = &scenario,
            .input = input,
            .output = output,
            .scenario_text = scenario_text,
            .packed = packed,
            .stream_read_scratch = stream_read_scratch};
}

prepare::Request request_for(std::string_view output_name = "OUT.WAV")
{
    prepare::Request request{};
    std::copy_n("INPUT.WAV", 9U, request.source.data());
    std::copy_n("INPUT.SCN", 9U, request.scenario.data());
    std::copy(output_name.begin(), output_name.end(), request.output.begin());
    request.retention = prepare::Retention::keep;
    return request;
}

bool pump(PrepareJob& job)
{
    std::size_t steps{};

    while (job.busy() && steps < 10000U)
    {
        static_cast<void>(job.step());
        ++steps;
    }

    check(steps < 10000U, "prepare reaches a terminal snapshot in bounded steps");
    return !job.busy();
}

bool step_until_phase(PrepareJob& job, prepare::Phase phase)
{
    std::size_t steps{};

    while (job.busy() && job.snapshot().phase != phase && steps < 10000U)
    {
        static_cast<void>(job.step());
        ++steps;
    }

    return job.busy() && job.snapshot().phase == phase;
}

void reset()
{
    files.clear();
    mounted = false;
    open_files = 0U;
    fail_read_path.clear();
    fail_write_path.clear();
    short_write_path.clear();
    fail_sync_path.clear();
    fail_close_path.clear();
    fail_rename_target.clear();
    short_read_path.clear();
    fail_seek_path.clear();
    fail_stat_path.clear();
    fail_mount_once = false;
    fail_unmount_once = false;
    corrupt_readback_pad = false;
    corrupt_readback_pcm = false;
    mount_calls = read_calls = write_calls = sync_calls = 0U;
    engine.configure_passthrough();
}

void test_exact_and_odd_pcm()
{
    reset();
    const auto source = samples(9U);
    put_file("INPUT.WAV", make_wav(source));
    const std::string scenario_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    const auto expected = render_expected(source, scenario_json);
    PrepareJob job;
    auto request = request_for();
    request.seed_override = true;
    request.seed = std::numeric_limits<std::uint64_t>::max();
    check(job.begin(request, resources()).is_ok(), "job admission validates filenames and fixed resources only");
    check(mount_calls == 0U && read_calls == 0U, "begin performs no FatFs work");
    check(pump(job), "small artifact completes");
    const auto status = job.snapshot();
    check(status.phase == prepare::Phase::complete && status.status.is_ok(), "complete snapshot is published after cleanup");
    check(status.total_input_frames == source.size() && status.output_frames == source.size(), "complete snapshot records source and actual output frames");
    check(!mounted && open_files == 0U, "completed job closes every handle and unmounts");
    check(files.contains(full_path("OUT.WAV")) && files.contains(full_path("OUT.JSN")), "WAV is published with its completion manifest");
    check(!files.contains(full_path("OUT.TMP")) && !files.contains(full_path("OUT.JMP")), "temporary names are renamed after verification");

    if (!files.contains(full_path("OUT.WAV")))
    {
        return;
    }

    const auto& wav = file_bytes("OUT.WAV");
    check(wav.size() == 44U + source.size() * 3U + 1U && wav.back() == 0U, "odd PCM payload has a canonical zero RIFF pad");

    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        check(audio::read_pcm24_le(wav.data() + 44U + index * 3U) == expected[index], "prepared PCM matches independent engine block oracle");
    }

    const auto& manifest = file_bytes("OUT.JSN");
    const std::string text{manifest.begin(), manifest.end()};
    check(text.find("\"complete\":true") != std::string::npos && text.find(status.pcm_sha256.data()) != std::string::npos, "manifest binds the verified PCM digest and marks completion");
    check(text.find("\"seed\":18446744073709551615,") != std::string::npos && text.find("\"input_frames\":9,\"output_frames\":9,") != std::string::npos,
          "manifest preserves full-width unsigned values without long-long printf conversions");
    const std::string slip_json =
        R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[{"type":"sample_slip","events":[{"kind":"delete","length_samples":1,"at_seconds":0}]}]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(slip_json.begin(), slip_json.end()));
    const auto slip_expected = render_expected(source, slip_json);
    PrepareJob slipped;
    check(slipped.begin(request_for("SLIP.WAV"), resources()).is_ok(), "variable-length request admitted");
    check(pump(slipped), "sample-deletion scenario completes");
    const auto slip_status = slipped.snapshot();
    check(scenario.stage_count == 1U && scenario.stages[0].type == signal_lab::StageType::sample_slip, "sample-slip JSON is accepted as an actual engine stage");
    check(slip_status.phase == prepare::Phase::complete && slip_status.output_frames == slip_expected.size(), "manifest frame count follows the engine's actual sample-slip output");

    if (files.contains(full_path("SLIP.WAV")))
    {
        const auto& slip_wav = file_bytes("SLIP.WAV");
        check(slip_wav.size() == 44U + slip_expected.size() * 3U + ((slip_expected.size() * 3U) & 1U), "variable output length is reflected in RIFF geometry");

        for (std::size_t index = 0U; index < slip_expected.size(); ++index)
        {
            check(audio::read_pcm24_le(slip_wav.data() + 44U + index * 3U) == slip_expected[index], "sample-slip PCM matches the independent engine oracle");
        }
    }

    const std::string insert_json =
        R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[{"type":"sample_slip","events":[{"kind":"duplicate","length_samples":1,"at_seconds":0.00004166666666666667}]}]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(insert_json.begin(), insert_json.end()));
    const auto insert_expected = render_expected(source, insert_json);
    PrepareJob inserted;
    check(inserted.begin(request_for("INSERT.WAV"), resources()).is_ok(), "sample insertion request admitted");
    check(pump(inserted), "sample insertion completes");
    const auto insert_status = inserted.snapshot();
    check(insert_status.phase == prepare::Phase::complete && insert_status.output_frames == source.size() + 1U && insert_expected.size() == source.size() + 1U,
          "sample insertion publishes the actual expanded output length");

    if (files.contains(full_path("INSERT.WAV")))
    {
        const auto& wav = file_bytes("INSERT.WAV");

        for (std::size_t index = 0U; index < insert_expected.size(); ++index)
        {
            check(audio::read_pcm24_le(wav.data() + 44U + index * 3U) == insert_expected[index], "sample-insertion PCM matches the engine oracle");
        }
    }

    const std::string empty_json =
        R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[{"type":"sample_slip","events":[{"kind":"delete","length_samples":9,"at_seconds":0}]}]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(empty_json.begin(), empty_json.end()));
    const auto empty_expected = render_expected(source, empty_json);
    PrepareJob empty;
    check(empty.begin(request_for("EMPTY.WAV"), resources()).is_ok(), "zero-output deletion request admitted");
    check(pump(empty), "zero-output render completes and verifies");
    const auto empty_status = empty.snapshot();
    check(empty_status.phase == prepare::Phase::complete && empty_status.output_frames == 0U && empty_expected.empty(),
          "complete marker records a valid empty output after deleting the entire source");

    if (files.contains(full_path("EMPTY.WAV")))
    {
        const auto& wav = file_bytes("EMPTY.WAV");
        check(wav.size() == 44U && wav[40U] == 0U && wav[41U] == 0U && wav[42U] == 0U && wav[43U] == 0U, "zero-frame output has a canonical 44-byte WAV header");
    }
}

void test_existing_output_and_cancel_cleanup()
{
    reset();
    const auto source = samples(13U);
    put_file("INPUT.WAV", make_wav(source));
    const std::string scenario_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    const std::vector<std::uint8_t> prior{0x12U, 0x34U};
    put_file("OUT.WAV", prior);
    PrepareJob rejected;
    check(rejected.begin(request_for(), resources()).is_ok(), "existing-name check occurs as bounded job work");
    check(pump(rejected), "existing output refusal cleans up its opened inputs");
    check(rejected.snapshot().phase == prepare::Phase::failed && file_bytes("OUT.WAV") == prior, "existing final name is refused without overwrite");
    files.erase(full_path("OUT.WAV"));
    PrepareJob cancelled;
    check(cancelled.begin(request_for("CANCEL.WAV"), resources()).is_ok(), "second request admitted after terminal refusal");
    static_cast<void>(cancelled.step()); // mount
    static_cast<void>(cancelled.step()); // open/validate primary
    check(cancelled.cancel().is_ok(), "STOP requests deferred cleanup");
    check(pump(cancelled), "cancelled job releases its open primary and media claim");
    check(cancelled.snapshot().phase == prepare::Phase::cancelled && !mounted && open_files == 0U, "cancelled terminal state is reported only after cleanup");
    check(!files.contains(full_path("CANCEL.WAV")) && !files.contains(full_path("CANCEL.JSN")), "cancellation publishes neither WAV nor completion marker");
}

void test_cleanup_failure_is_retryable()
{
    reset();
    const auto source = samples(5U);
    put_file("INPUT.WAV", make_wav(source));
    const std::string scenario_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"stages":[]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    fail_close_path = full_path("INPUT.WAV");
    PrepareJob job;
    check(job.begin(request_for("RETRY.WAV"), resources()).is_ok(), "retry case admitted");
    static_cast<void>(job.step());
    static_cast<void>(job.step());
    static_cast<void>(job.cancel());
    static_cast<void>(job.step());
    check(job.busy() && job.snapshot().phase == prepare::Phase::cleanup_failed, "failed close keeps the job busy and exposes cleanup_failed rather than a terminal result");
    fail_close_path.clear();
    check(job.cancel().is_ok(), "STOP/retry wakes cleanup after a transient close failure");
    check(pump(job), "cleanup retry finishes after the injected close error clears");
    check(!mounted && open_files == 0U && job.snapshot().phase == prepare::Phase::failed, "retry releases ownership while preserving the cleanup failure result");
}

void test_stop_during_render_and_secondary_calibration()
{
    reset();
    const auto primary = samples(4096U);
    put_file("INPUT.WAV", make_wav(primary));
    const std::string plain_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(plain_json.begin(), plain_json.end()));
    PrepareJob rendering;
    check(rendering.begin(request_for("STOPR.WAV"), resources()).is_ok(), "render STOP case admitted");
    check(step_until_phase(rendering, prepare::Phase::rendering), "job reaches active render phase");
    const auto reads_before_block = read_calls;
    const auto writes_before_block = write_calls;
    static_cast<void>(rendering.step());
    check(read_calls > reads_before_block && write_calls == writes_before_block && rendering.snapshot().input_frames == 0U,
          "one render step reads and decodes a source block without processing or appending it");
    const auto reads_after_decode = read_calls;
    const auto writes_after_decode = write_calls;
    static_cast<void>(rendering.step());
    check(read_calls == reads_after_decode && write_calls == writes_after_decode && rendering.snapshot().input_frames == 0U,
          "the next render step processes and packs the block without additional I/O or progress publication");
    static_cast<void>(rendering.step());
    check(read_calls == reads_after_decode && write_calls > writes_after_decode && rendering.snapshot().input_frames != 0U,
          "a separate append step writes packed PCM before advancing source counters");
    check(rendering.cancel().is_ok(), "STOP during render is accepted");
    check(pump(rendering), "render STOP cleanup finishes");
    check(rendering.snapshot().phase == prepare::Phase::cancelled && !files.contains(full_path("STOPR.WAV")) && !files.contains(full_path("STOPR.JSN")) && !mounted && open_files == 0U,
          "STOP during rendering leaves no published artifact or owned resource");
    const auto cancel_before_append = [&plain_json](std::string_view output_name, std::size_t render_steps, std::string_view boundary)
    {
        reset();
        put_file("INPUT.WAV", make_wav(samples(4096U)));
        put_file("INPUT.SCN", std::vector<std::uint8_t>(plain_json.begin(), plain_json.end()));
        PrepareJob job;
        check(job.begin(request_for(output_name), resources()).is_ok(), "intermediate-boundary cancel case admitted");
        check(step_until_phase(job, prepare::Phase::rendering), "intermediate-boundary case reaches rendering");
        const auto writes_before = write_calls;

        for (std::size_t index = 0U; index < render_steps; ++index)
        {
            static_cast<void>(job.step());
        }

        check(write_calls == writes_before && job.snapshot().input_frames == 0U,
              boundary == "read/decode" ? "source read/decode leaves the block uncommitted before STOP" : "DSP/packing leaves the block uncommitted before STOP");
        check(job.cancel().is_ok(), "STOP between render substeps is accepted");
        check(pump(job), "intermediate-boundary STOP cleanup finishes");
        std::string wav_name{output_name};
        std::string manifest_name{output_name};
        const auto extension = wav_name.find_last_of('.');

        if (extension != std::string::npos)
        {
            wav_name.resize(extension);
            manifest_name = wav_name + ".JSN";
            wav_name += ".WAV";
        }

        check(job.snapshot().phase == prepare::Phase::cancelled && !files.contains(full_path(wav_name)) && !files.contains(full_path(manifest_name)) && !mounted && open_files == 0U,
              "STOP before append publishes no artifact and closes all owned resources");
        check(write_calls == writes_before, "cancel before append performs no output write");
    };

    cancel_before_append("READCAN.WAV", 1U, "read/decode");
    cancel_before_append("PACKCAN.WAV", 2U, "process/pack");
    reset();
    put_file("INPUT.WAV", make_wav(samples(64U)));
    put_file("SECOND.WAV", make_wav(samples(64U)));
    const std::string secondary_json =
        R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[{"type":"second_source","source_path":"SECOND.WAV","si_db":0,"start_frame":0,"source_start_frame":0,"duration_frames":64,"frequency_offset_hz":0}]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(secondary_json.begin(), secondary_json.end()));
    PrepareJob calibrating;
    check(calibrating.begin(request_for("STOP2.WAV"), resources()).is_ok(), "calibration STOP case admitted");
    check(step_until_phase(calibrating, prepare::Phase::calibrating), "job reaches secondary calibration phase");
    static_cast<void>(calibrating.step());
    check(calibrating.cancel().is_ok(), "STOP during secondary calibration is accepted");
    check(pump(calibrating), "secondary calibration STOP cleanup finishes");
    check(calibrating.snapshot().phase == prepare::Phase::cancelled && !files.contains(full_path("STOP2.WAV")) && !files.contains(full_path("STOP2.JSN")) && !mounted && open_files == 0U,
          "STOP during calibration closes source/provider handles and publishes no artifact");
}

void test_committed_prefix_read_during_render()
{
    reset();
    const auto source = samples(signal_lab::engine_block_frames + 37U);
    put_file("INPUT.WAV", make_wav(source));
    const std::string scenario_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    const auto expected = render_expected(source, scenario_json);
    PrepareJob job;
    check(job.begin(request_for("PREFX.WAV"), resources()).is_ok(), "prefix-read render admitted");
    check(step_until_phase(job, prepare::Phase::rendering), "prefix-read job reaches rendering");
    static_cast<void>(job.step()); // decode first block
    static_cast<void>(job.step()); // process and pack first block
    static_cast<void>(job.step()); // append first block
    static_cast<void>(job.step()); // decode final partial block
    static_cast<void>(job.step()); // process and pack final partial block
    const std::vector<std::uint8_t> staged_before_read(packed.begin(), packed.end());
    check(job.commit_prefix().is_ok(), "current output prefix becomes readable after sync");
    check(job.committed_frames() == signal_lab::engine_block_frames, "only the appended first block is available to the prefix reader");
    constexpr std::size_t prefix_bytes = std::size_t{32U} * audio::pcm24_bytes_per_sample;
    auto prefix = std::span<std::uint8_t>{stream_read_scratch.data(), prefix_bytes};
    check(job.read_prefix(0U, prefix).is_ok(), "bounded prefix read succeeds while the next block is staged");
    check(std::equal(prefix.begin(), prefix.end(), file_bytes("PREFX.TMP").begin() + 44), "prefix read returns exact packed PCM bytes from the completed first block");
    check(std::equal(packed.begin(), packed.end(), staged_before_read.begin()), "prefix read leaves the independently staged next block unchanged");
    static_cast<void>(job.step()); // append final partial block
    check(pump(job), "prefix-read render completes after the staged block is appended");
    check(job.snapshot().phase == prepare::Phase::complete && job.snapshot().output_frames == expected.size(), "prefix reads do not change final render frame accounting");

    if (files.contains(full_path("PREFX.WAV")))
    {
        const auto& wav = file_bytes("PREFX.WAV");

        for (std::size_t index = 0U; index < expected.size(); ++index)
        {
            check(audio::read_pcm24_le(wav.data() + 44U + index * audio::pcm24_bytes_per_sample) == expected[index], "interleaved prefix reads preserve exact final PCM");
        }
    }

    check(!mounted && open_files == 0U, "prefix-read render closes its file and releases media");
}

void test_prefix_sync_and_read_failures_do_not_publish()
{
    const std::string scenario_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[]})";
    const auto prime_second_block = [&scenario_json](PrepareJob& job, std::string_view output_name)
    {
        reset();
        put_file("INPUT.WAV", make_wav(samples(signal_lab::engine_block_frames + 8U)));
        put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
        check(job.begin(request_for(output_name), resources()).is_ok(), "prefix fault case admitted");
        check(step_until_phase(job, prepare::Phase::rendering), "prefix fault case reaches rendering");

        for (unsigned int index = 0U; index < 5U; ++index)
        {
            static_cast<void>(job.step());
        }
    };

    PrepareJob sync_failure;
    prime_second_block(sync_failure, "SYNCPFX.WAV");
    fail_sync_path = full_path("SYNCPFX.TMP");
    check(!sync_failure.commit_prefix().is_ok(), "prefix sync failure is reported to its owner");
    check(pump(sync_failure), "prefix sync failure cleanup completes");
    check(sync_failure.snapshot().phase == prepare::Phase::failed && !files.contains(full_path("SYNCPFX.WAV")) && !files.contains(full_path("SYNCPFX.JSN")) && !mounted && open_files == 0U,
          "failed prefix sync publishes no artifact and closes the file and media owner");
    PrepareJob read_failure;
    prime_second_block(read_failure, "READPFX.WAV");
    check(read_failure.commit_prefix().is_ok(), "read-failure prefix is first committed successfully");
    fail_read_path = full_path("READPFX.TMP");
    check(!read_failure.read_prefix(0U, std::span<std::uint8_t>{stream_read_scratch.data(), 12U}).is_ok(), "prefix read failure is reported to its owner");
    check(pump(read_failure), "prefix read failure cleanup completes");
    check(read_failure.snapshot().phase == prepare::Phase::failed && !files.contains(full_path("READPFX.WAV")) && !files.contains(full_path("READPFX.JSN")) && !mounted && open_files == 0U,
          "failed prefix read publishes no artifact and closes the file and media owner");
}

void test_managed_render_lifecycle()
{
    reset();
    const auto source = samples(2U * signal_lab::engine_block_frames + 13U);
    put_file("INPUT.WAV", make_wav(source));
    const std::string scenario_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    const auto expected = render_expected(source, scenario_json);
    PrepareJob job;
    check(job.begin_managed(request_for("MANAGED.WAV"), resources()).is_ok(), "managed render admission succeeds");
    check(step_until_phase(job, prepare::Phase::rendering), "managed job completes setup and reaches rendering");
    const auto writes_at_ready = write_calls;
    check(job.step().code == m110::StatusCode::busy && write_calls == writes_at_ready && job.managed_produced_frames() == 0U, "ordinary job steps do not consume or append managed render blocks");
    check(!job.finish_managed().is_ok(), "managed finish is refused before every source frame is appended");
    std::uint64_t completed_frames{};

    while (completed_frames < source.size())
    {
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(signal_lab::engine_block_frames, source.size() - completed_frames));
        const auto before_read = read_calls;
        const auto before_write = write_calls;
        const m110::Status decoded = job.produce_step(frames);
        check(decoded.code == m110::StatusCode::busy && read_calls > before_read && write_calls == before_write, "managed produce read/decode is one bounded substep and performs no output write");
        const auto before_process_reads = read_calls;
        const m110::Status processed = job.produce_step(frames);
        check(processed.is_ok() && read_calls == before_process_reads && write_calls == before_write && job.managed_produced_frames() == completed_frames + frames &&
                  job.snapshot().output_frames == completed_frames,
              "managed DSP/pack stages frames without publishing them as appended output");
        check(!job.append_produced(frames - 1U).is_ok() && write_calls == before_write, "managed append refuses a frame-count mismatch without writing");
        check(job.append_produced(frames).is_ok() && write_calls > before_write, "managed append writes exactly the staged block");
        completed_frames += frames;
        check(job.snapshot().input_frames == completed_frames && job.snapshot().output_frames == completed_frames, "managed public counters advance only after append");
    }

    check(job.finish_managed().is_ok(), "explicit finish begins verification only after all frames append");
    check(pump(job), "managed render verifies, publishes, and closes");
    check(job.snapshot().phase == prepare::Phase::complete && job.snapshot().output_frames == expected.size() && files.contains(full_path("MANAGED.WAV")) && files.contains(full_path("MANAGED.JSN")),
          "managed render publishes the final artifact only after owner finish");

    if (files.contains(full_path("MANAGED.WAV")))
    {
        const auto& wav = file_bytes("MANAGED.WAV");

        for (std::size_t index = 0U; index < expected.size(); ++index)
        {
            check(audio::read_pcm24_le(wav.data() + 44U + index * audio::pcm24_bytes_per_sample) == expected[index], "managed artifact matches the sequential engine oracle exactly");
        }
    }

    check(!mounted && open_files == 0U, "managed success releases file and media ownership");
    reset();
    put_file("INPUT.WAV", make_wav(samples(32U)));
    const std::string slip_json =
        R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[{"type":"sample_slip","events":[{"kind":"delete","length_samples":1,"at_seconds":0}]}]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(slip_json.begin(), slip_json.end()));
    PrepareJob slip_job;
    check(slip_job.begin_managed(request_for("NOSLIP.WAV"), resources()).is_ok(), "managed sample-slip refusal case admitted");
    check(pump(slip_job), "managed sample-slip refusal cleans up");
    check(slip_job.snapshot().phase == prepare::Phase::failed && !files.contains(full_path("NOSLIP.TMP")) && !files.contains(full_path("NOSLIP.WAV")) && !files.contains(full_path("NOSLIP.JSN")) &&
              !mounted && open_files == 0U,
          "managed variable-length stages are refused before output creation");
    reset();
    put_file("INPUT.WAV", make_wav(samples(signal_lab::engine_block_frames + 1U)));
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    PrepareJob cancelled;
    check(cancelled.begin_managed(request_for("CANCELM.WAV"), resources()).is_ok(), "managed cancellation case admitted");
    check(step_until_phase(cancelled, prepare::Phase::rendering), "managed cancellation case reaches rendering");
    const auto first_block = static_cast<std::uint32_t>(signal_lab::engine_block_frames);
    static_cast<void>(cancelled.produce_step(first_block));
    check(cancelled.produce_step(first_block).is_ok(), "managed cancellation stages a block before append");
    const auto writes_before_cancel = write_calls;
    check(cancelled.cancel().is_ok(), "managed STOP is accepted before append");
    check(pump(cancelled), "managed STOP cleanup completes");
    check(cancelled.snapshot().phase == prepare::Phase::cancelled && write_calls == writes_before_cancel && !files.contains(full_path("CANCELM.WAV")) && !files.contains(full_path("CANCELM.JSN")) &&
              !mounted && open_files == 0U,
          "managed cancellation discards staged PCM, publishes nothing, and closes resources");
    reset();
    const auto reuse_source = samples(37U);
    put_file("INPUT.WAV", make_wav(reuse_source));
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    check(cancelled.begin(request_for("REUSE.WAV"), resources()).is_ok(), "the same job instance accepts a new request after cancelled output cleanup");
    check(pump(cancelled), "reused job completes after cancelling its prior active writer");
    check(cancelled.snapshot().phase == prepare::Phase::complete && cancelled.snapshot().output_frames == reuse_source.size() && files.contains(full_path("REUSE.WAV")) &&
              files.contains(full_path("REUSE.JSN")) && !mounted && open_files == 0U,
          "cancelled writer state does not leak into the next completed artifact");
}

void test_calibrated_secondary_source()
{
    reset();
    const auto primary = samples(64U);
    const auto second = samples(64U);
    put_file("INPUT.WAV", make_wav(primary));
    put_file("SECOND.WAV", make_wav(second));
    const std::string scenario_json =
        R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[{"type":"second_source","source_path":"SECOND.WAV","si_db":0,"start_frame":0,"source_start_frame":0,"duration_frames":64,"frequency_offset_hz":0}]})";
    put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    std::array<float, 64U> secondary_samples{};
    double energy{};

    for (std::size_t index = 0U; index < second.size(); ++index)
    {
        secondary_samples[index] = audio::pcm24_to_float(second[index]);
        energy += static_cast<double>(secondary_samples[index]) * secondary_samples[index];
    }

    const double secondary_rms = std::sqrt(energy / static_cast<double>(second.size()));
    signal_lab::FixedSecondarySource fixed_secondary{secondary_samples, secondary_rms};
    const auto expected = render_expected(primary, scenario_json, &fixed_secondary);
    PrepareJob job;
    check(job.begin(request_for("SECOUT.WAV"), resources()).is_ok(), "calibrated secondary request admitted");
    check(pump(job), "secondary source hashes, calibrates, renders and closes");
    const auto result = job.snapshot();
    check(result.phase == prepare::Phase::complete && result.output_frames == expected.size(), "secondary-source artifact publishes only after measured provider use and cleanup");

    if (files.contains(full_path("SECOUT.WAV")))
    {
        const auto& wav = file_bytes("SECOUT.WAV");

        for (std::size_t index = 0U; index < expected.size(); ++index)
        {
            check(audio::read_pcm24_le(wav.data() + 44U + index * 3U) == expected[index], "calibrated SD provider output matches an independent fixed-source engine oracle");
        }
    }

    check(files.contains(full_path("SECOUT.JSN")) && open_files == 0U && !mounted, "secondary metadata is published only after provider close and media release");
}

void test_io_faults_never_publish_a_completion_marker()
{
    const std::string scenario_json = R"({"schema":"signal-lab.scenario/1","source_gain_db":0,"reference_rms":0.25,"impairments":[]})";
    const auto seed_files = [&]()
    {
        put_file("INPUT.WAV", make_wav(samples(9U)));
        put_file("INPUT.SCN", std::vector<std::uint8_t>(scenario_json.begin(), scenario_json.end()));
    };

    const auto check_failed = [&](PrepareJob& job, std::string_view output)
    {
        check(pump(job), "injected failure reaches a cleaned terminal state");
        const std::size_t dot = output.find('.');
        const std::string manifest = std::string{output.substr(0U, dot)} + ".JSN";
        check(job.snapshot().phase == prepare::Phase::failed && !files.contains(full_path(manifest)), "an I/O failure never publishes a completion manifest");
        check(!mounted && open_files == 0U, "injected failure releases handles and mount after cleanup");
    };

    reset();
    seed_files();
    fail_mount_once = true;
    PrepareJob mount_failure;
    check(mount_failure.begin(request_for("MOUNT.WAV"), resources()).is_ok(), "mount-failure case admitted");
    check_failed(mount_failure, "MOUNT.WAV");
    reset();
    seed_files();
    PrepareJob read_failure;
    check(read_failure.begin(request_for("READ.WAV"), resources()).is_ok(), "short-read case admitted");
    static_cast<void>(read_failure.step());
    static_cast<void>(read_failure.step());
    short_read_path = full_path("INPUT.WAV");
    check_failed(read_failure, "READ.WAV");
    reset();
    seed_files();
    fail_write_path = full_path("WRITE.TMP");
    PrepareJob write_failure;
    check(write_failure.begin(request_for("WRITE.WAV"), resources()).is_ok(), "write-failure case admitted");
    check_failed(write_failure, "WRITE.WAV");
    reset();
    seed_files();
    PrepareJob append_failure;
    check(append_failure.begin(request_for("APPEND.WAV"), resources()).is_ok(), "render append-failure case admitted");
    check(step_until_phase(append_failure, prepare::Phase::rendering), "append-failure job reaches its render phase");
    static_cast<void>(append_failure.step());
    static_cast<void>(append_failure.step());
    fail_write_path = full_path("APPEND.TMP");
    static_cast<void>(append_failure.step());
    check(pump(append_failure), "render append failure completes cleanup");
    check(append_failure.snapshot().phase == prepare::Phase::failed && !files.contains(full_path("APPEND.WAV")) && !files.contains(full_path("APPEND.JSN")),
          "append I/O failure does not publish a partial WAV or completion manifest");
    reset();
    seed_files();
    short_write_path = full_path("SHORT.TMP");
    PrepareJob short_write;
    check(short_write.begin(request_for("SHORT.WAV"), resources()).is_ok(), "short-write case admitted");
    check_failed(short_write, "SHORT.WAV");
    reset();
    seed_files();
    fail_sync_path = full_path("SYNC.TMP");
    PrepareJob sync_failure;
    check(sync_failure.begin(request_for("SYNC.WAV"), resources()).is_ok(), "sync-failure case admitted");
    check_failed(sync_failure, "SYNC.WAV");
    reset();
    seed_files();
    fail_seek_path = full_path("SEEK.TMP");
    PrepareJob seek_failure;
    check(seek_failure.begin(request_for("SEEK.WAV"), resources()).is_ok(), "seek-failure case admitted");
    check_failed(seek_failure, "SEEK.WAV");
    reset();
    seed_files();
    fail_rename_target = full_path("REN.JSN");
    PrepareJob rename_failure;
    check(rename_failure.begin(request_for("REN.WAV"), resources()).is_ok(), "manifest-rename failure case admitted");
    check_failed(rename_failure, "REN.WAV");
    check(files.contains(full_path("REN.WAV")) && !files.contains(full_path("REN.JSN")), "WAV-first rename leaves only an unpublished orphan if manifest rename fails");
    reset();
    seed_files();
    corrupt_readback_pad = true;
    PrepareJob pad_failure;
    check(pad_failure.begin(request_for("PAD.WAV"), resources()).is_ok(), "odd-pad corruption case admitted");
    check_failed(pad_failure, "PAD.WAV");
    check(!files.contains(full_path("PAD.WAV")) && files.contains(full_path("PAD.TMP")), "nonzero read-back padding is rejected before publication");
    reset();
    seed_files();
    corrupt_readback_pcm = true;
    PrepareJob pcm_failure;
    check(pcm_failure.begin(request_for("PCM.WAV"), resources()).is_ok(), "PCM corruption case admitted");
    check_failed(pcm_failure, "PCM.WAV");
    check(!files.contains(full_path("PCM.WAV")) && files.contains(full_path("PCM.TMP")), "read-back PCM corruption is rejected before publication");
    reset();
    seed_files();
    put_file("STALE.TMP", {0x01U});
    PrepareJob stale_temp;
    check(stale_temp.begin(request_for("STALE.WAV"), resources()).is_ok(), "stale temporary check is deferred to job steps");
    check_failed(stale_temp, "STALE.WAV");
    check(file_bytes("STALE.TMP") == std::vector<std::uint8_t>{0x01U}, "pre-existing temporary data is not removed or overwritten");
    reset();
    seed_files();
    fail_unmount_once = true;
    PrepareJob unmount_failure;
    check(unmount_failure.begin(request_for("UNMOUNT.WAV"), resources()).is_ok(), "unmount retry case admitted");
    check(pump(unmount_failure), "unmount error retries without dropping filesystem ownership");
    check(unmount_failure.snapshot().phase == prepare::Phase::failed && !mounted && open_files == 0U, "transient unmount error is retained in the terminal failure result");
}

} // namespace

extern "C" FRESULT f_mount(FATFS* filesystem, const char* drive, unsigned char immediate)
{
    static_cast<void>(drive);
    static_cast<void>(immediate);
    ++mount_calls;
    mounted = filesystem != nullptr;

    if (filesystem != nullptr)
    {
        filesystem->mounted = 1;
    }

    if (filesystem != nullptr && fail_mount_once)
    {
        fail_mount_once = false;
        return FR_DISK_ERR;
    }

    if (filesystem == nullptr && fail_unmount_once)
    {
        fail_unmount_once = false;
        mounted = true;
        return FR_DISK_ERR;
    }

    return FR_OK;
}

extern "C" FRESULT f_open(FIL* file, const char* path, unsigned char mode)
{
    if (file == nullptr || path == nullptr)
    {
        return FR_INVALID_OBJECT;
    }

    const std::string key{path};

    if ((mode & FA_CREATE_NEW) != 0U)
    {
        if (files.contains(key))
        {
            return FR_EXIST;
        }

        files[key] = {};
    }

    auto found = files.find(key);

    if (found == files.end())
    {
        return FR_NO_FILE;
    }

    file->data = found->second.bytes.data();
    file->size = static_cast<FSIZE_t>(found->second.bytes.size());
    file->position = 0U;
    file->opened = 1;
    file->context = &found->second;
    file->mode = mode;
    ++open_files;
    return FR_OK;
}

extern "C" FRESULT f_read(FIL* file, void* destination, UINT requested, UINT* actual)
{
    ++read_calls;

    if (file == nullptr || file->opened == 0 || actual == nullptr || destination == nullptr)
    {
        return FR_INVALID_OBJECT;
    }

    const auto* record = static_cast<const File*>(file->context);
    const std::string* path{};

    for (const auto& [name, candidate] : files)
    {
        if (&candidate == record)
        {
            path = &name;
            break;
        }
    }

    if (path != nullptr && *path == fail_read_path)
    {
        return FR_DISK_ERR;
    }

    const std::size_t before = file->position;
    const std::size_t remaining = record->bytes.size() - before;
    *actual = static_cast<UINT>(std::min<std::size_t>(requested, remaining));

    if (path != nullptr && *path == short_read_path && *actual != 0U)
    {
        --*actual;
        short_read_path.clear();
    }

    std::memcpy(destination, record->bytes.data() + file->position, *actual);
    file->position += *actual;

    if (corrupt_readback_pad && path != nullptr && path->ends_with(".TMP") && record->bytes.size() > 44U && (record->bytes.size() & 1U) == 0U && before <= record->bytes.size() - 1U &&
        before + *actual > record->bytes.size() - 1U)
    {
        static_cast<std::uint8_t*>(destination)[record->bytes.size() - 1U - before] = 0x7FU;
        corrupt_readback_pad = false;
    }

    if (corrupt_readback_pcm && path != nullptr && path->ends_with(".TMP") && before <= 44U && before + *actual > 44U)
    {
        static_cast<std::uint8_t*>(destination)[44U - before] ^= 0x01U;
        corrupt_readback_pcm = false;
    }

    return FR_OK;
}

extern "C" FRESULT f_lseek(FIL* file, FSIZE_t offset)
{
    if (file == nullptr || file->opened == 0)
    {
        return FR_INVALID_OBJECT;
    }

    const auto* record = static_cast<const File*>(file->context);

    for (const auto& [name, candidate] : files)
    {
        if (&candidate == record && name == fail_seek_path)
        {
            fail_seek_path.clear();
            return FR_DISK_ERR;
        }
    }

    if (offset > record->bytes.size())
    {
        return FR_DISK_ERR;
    }

    file->position = offset;
    return FR_OK;
}

extern "C" FRESULT f_close(FIL* file)
{
    if (file == nullptr || file->opened == 0)
    {
        return FR_INVALID_OBJECT;
    }

    const auto* record = static_cast<const File*>(file->context);

    for (const auto& [name, candidate] : files)
    {
        if (&candidate == record && name == fail_close_path)
        {
            return FR_DISK_ERR;
        }
    }

    file->opened = 0;
    file->context = nullptr;
    --open_files;
    return FR_OK;
}

extern "C" FRESULT f_write(FIL* file, const void* source, UINT requested, UINT* actual)
{
    ++write_calls;

    if (file == nullptr || file->opened == 0 || actual == nullptr || source == nullptr || (file->mode & FA_WRITE) == 0U)
    {
        return FR_INVALID_OBJECT;
    }

    auto* record = static_cast<File*>(file->context);

    for (auto& [name, candidate] : files)
    {
        if (&candidate == record && name == fail_write_path)
        {
            return FR_DISK_ERR;
        }
    }

    if (file->position > record->bytes.size())
    {
        record->bytes.resize(file->position);
    }

    if (requested > std::numeric_limits<std::size_t>::max() - file->position)
    {
        return FR_DISK_ERR;
    }

    std::string path;

    for (const auto& [name, candidate] : files)
    {
        if (&candidate == record)
        {
            path = name;
            break;
        }
    }

    const std::size_t written = path == short_write_path && requested != 0U ? requested - 1U : requested;
    const std::size_t end = file->position + written;

    if (end > record->bytes.size())
    {
        record->bytes.resize(end);
    }

    std::memcpy(record->bytes.data() + file->position, source, written);
    file->position = static_cast<FSIZE_t>(end);
    file->size = static_cast<FSIZE_t>(record->bytes.size());
    file->data = record->bytes.data();
    *actual = static_cast<UINT>(written);

    if (written != requested)
    {
        short_write_path.clear();
    }

    return FR_OK;
}

extern "C" FRESULT f_sync(FIL* file)
{
    ++sync_calls;

    if (file == nullptr || file->opened == 0)
    {
        return FR_INVALID_OBJECT;
    }

    const auto* record = static_cast<const File*>(file->context);

    for (const auto& [name, candidate] : files)
    {
        if (&candidate == record && name == fail_sync_path)
        {
            return FR_DISK_ERR;
        }
    }

    return FR_OK;
}

extern "C" FRESULT f_stat(const char* path, FILINFO* info)
{
    if (path == nullptr || info == nullptr)
    {
        return FR_INVALID_OBJECT;
    }

    if (fail_stat_path == path)
    {
        fail_stat_path.clear();
        return FR_DISK_ERR;
    }

    const auto found = files.find(std::string{path});

    if (found == files.end())
    {
        return FR_NO_FILE;
    }

    info->fsize = static_cast<FSIZE_t>(found->second.bytes.size());
    return FR_OK;
}

extern "C" FRESULT f_rename(const char* old_path, const char* new_path)
{
    if (old_path == nullptr || new_path == nullptr)
    {
        return FR_INVALID_OBJECT;
    }

    if (fail_rename_target == new_path)
    {
        return FR_DISK_ERR;
    }

    const auto found = files.find(std::string{old_path});

    if (found == files.end())
    {
        return FR_NO_FILE;
    }

    if (files.contains(std::string{new_path}))
    {
        return FR_EXIST;
    }

    files[std::string{new_path}] = std::move(found->second);
    files.erase(found);
    return FR_OK;
}

extern "C" FRESULT f_mkdir(const char* path)
{
    static_cast<void>(path);
    return FR_OK;
}

extern "C" FRESULT f_opendir(DIR* directory, const char* path)
{
    static_cast<void>(directory);
    static_cast<void>(path);
    return FR_NO_PATH;
}

extern "C" FRESULT f_readdir(DIR* directory, FILINFO* info)
{
    static_cast<void>(directory);
    static_cast<void>(info);
    return FR_NO_PATH;
}

extern "C" FRESULT f_closedir(DIR* directory)
{
    static_cast<void>(directory);
    return FR_OK;
}

int main()
{
    {
        PrepareJob job;
        auto missing = resources();
        missing.engine = nullptr;
        check(!job.begin(request_for(), missing).is_ok() && !job.busy(), "missing borrowed engine refuses before I/O");
        missing = resources();
        missing.watterson = nullptr;
        check(!job.begin(request_for(), missing).is_ok() && !job.busy(), "missing borrowed channel refuses before I/O");
        missing = resources();
        missing.secondary = nullptr;
        check(!job.begin(request_for(), missing).is_ok() && !job.busy(), "missing borrowed secondary refuses before I/O");
        missing = resources();
        missing.scenario = nullptr;
        check(!job.begin(request_for(), missing).is_ok() && !job.busy(), "missing borrowed scenario refuses before I/O");
    }

    test_exact_and_odd_pcm();
    test_existing_output_and_cancel_cleanup();
    test_cleanup_failure_is_retryable();
    test_stop_during_render_and_secondary_calibration();
    test_committed_prefix_read_during_render();
    test_prefix_sync_and_read_failures_do_not_publish();
    test_managed_render_lifecycle();
    test_calibrated_secondary_source();
    test_io_faults_never_publish_a_completion_marker();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d preparation owner checks failed\n", failures);
    }

    return failures == 0 ? 0 : 1;
}
