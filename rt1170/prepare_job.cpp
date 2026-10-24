// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "prepare_job.hpp"

#include "build_identity.hpp"
#include "common/pcm24.hpp"
#include "common/uint64_text.hpp"
#include "common/wav.hpp"
#include "secondary_wav.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/stage_resources.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/watterson.hpp"
#include "wfg_build_info.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace waveform_generator
{
namespace
{

inline constexpr std::string_view media_directory{"2:/WG/"};
inline constexpr std::size_t media_path_bytes = media_directory.size() + 13U;
inline constexpr std::size_t io_chunk_bytes = 4096U;

inline constexpr m110::Status prepare_busy{.code = m110::StatusCode::busy, .message = "prepare job is already active"};
inline constexpr m110::Status prepare_bad_request{.code = m110::StatusCode::invalid_argument, .message = "prepare request has invalid filenames, retention, or resources"};
inline constexpr m110::Status prepare_io_error{.code = m110::StatusCode::io_error, .message = "prepare job FatFs operation failed"};
inline constexpr m110::Status prepare_short_read{.code = m110::StatusCode::io_error, .message = "prepare job received a short WAV or scenario read"};
inline constexpr m110::Status prepare_missing_reference{.code = m110::StatusCode::invalid_configuration, .message = "prepare requires scenario reference_rms"};
inline constexpr m110::Status prepare_bad_scenario_source{.code = m110::StatusCode::invalid_configuration, .message = "prepare requires a simple 8.3 second-source WAV basename"};
inline constexpr m110::Status prepare_existing_output{.code = m110::StatusCode::invalid_argument, .message = "prepare output, temporary, manifest, or companion scenario already exists"};
inline constexpr m110::Status prepare_alias{.code = m110::StatusCode::invalid_argument, .message = "prepare source, scenario, secondary, output, or companion names alias"};
inline constexpr m110::Status prepare_bad_wav{.code = m110::StatusCode::invalid_configuration, .message = "prepare input must be canonical mono PCM24 at 48 kHz"};
inline constexpr m110::Status prepare_bad_manifest{.code = m110::StatusCode::buffer_too_small, .message = "prepare manifest buffer is too small"};
inline constexpr m110::Status prepare_bad_verify{.code = m110::StatusCode::io_error, .message = "prepared WAV read-back did not match its finalized artifact"};
inline constexpr m110::Status prepare_no_job{.code = m110::StatusCode::unavailable, .message = "prepare job is not active"};
inline constexpr m110::Status prepare_cancelled{.code = m110::StatusCode::unavailable, .message = "prepare job was cancelled"};
inline constexpr m110::Status prepare_managed_pending{.code = m110::StatusCode::busy, .message = "managed render awaits owner action"};
inline constexpr m110::Status prepare_managed_mismatch{.code = m110::StatusCode::invalid_argument, .message = "managed render frame count or stage order is invalid"};
inline constexpr m110::Status prepare_managed_slip{.code = m110::StatusCode::invalid_configuration, .message = "managed rendering requires fixed-length impairment stages"};

[[nodiscard]] std::size_t nul_length(const std::array<char, 13U>& name) noexcept
{
    for (std::size_t index = 0U; index < name.size(); ++index)
    {
        if (name[index] == '\0')
        {
            return index;
        }
    }

    return name.size();
}

[[nodiscard]] char ascii_upper(char value) noexcept
{
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}

[[nodiscard]] m110::Status valid_component(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 8U)
    {
        return prepare_bad_request;
    }

    for (const char character : value)
    {
        const char upper = ascii_upper(character);

        if (!((upper >= 'A' && upper <= 'Z') || (upper >= '0' && upper <= '9') || upper == '_' || upper == '-'))
        {
            return prepare_bad_request;
        }
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Status normalize_83(const std::array<char, 13U>& source, std::string_view extension, std::array<char, 13U>& destination) noexcept
{
    destination = {};
    const std::size_t length = nul_length(source);

    if (length == source.size() || length < extension.size() + 1U)
    {
        return prepare_bad_request;
    }

    const std::string_view input{source.data(), length};
    const std::size_t dot = input.find('.');

    if (dot == std::string_view::npos || input.find('.', dot + 1U) != std::string_view::npos || input.size() - dot != extension.size() || !valid_component(input.substr(0U, dot)).is_ok())
    {
        return prepare_bad_request;
    }

    for (std::size_t index = 0U; index < extension.size(); ++index)
    {
        if (ascii_upper(input[dot + index]) != extension[index])
        {
            return prepare_bad_request;
        }
    }

    for (std::size_t index = 0U; index < length; ++index)
    {
        destination[index] = ascii_upper(source[index]);
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Status normalize_secondary_name(const char* source, std::array<char, 13U>& destination) noexcept
{
    if (source == nullptr)
    {
        return prepare_bad_scenario_source;
    }

    std::array<char, 13U> raw{};
    std::size_t index{};

    while (index < raw.size() && source[index] != '\0')
    {
        raw[index] = source[index];
        ++index;
    }

    return index < raw.size() ? normalize_83(raw, ".WAV", destination) : prepare_bad_scenario_source;
}

[[nodiscard]] m110::Status require_distinct(const std::array<char, 13U>& left, const std::array<char, 13U>& right) noexcept
{
    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        if (ascii_upper(left[index]) != ascii_upper(right[index]))
        {
            return m110::Status::success();
        }

        if (left[index] == '\0')
        {
            return prepare_alias;
        }
    }

    return m110::Status::success();
}

void replace_extension(const std::array<char, 13U>& input, std::string_view extension, std::array<char, media_path_bytes>& output) noexcept
{
    output = {};
    std::copy(media_directory.begin(), media_directory.end(), output.begin());
    const std::size_t dot = std::string_view{input.data()}.find('.');
    std::copy_n(input.data(), dot, output.data() + media_directory.size());
    output[media_directory.size() + dot] = '.';

    for (std::size_t index = 0U; index < extension.size(); ++index)
    {
        output[media_directory.size() + dot + index] = extension[index];
    }

    output[media_directory.size() + dot + extension.size()] = '\0';
}

void sha_hex(const waveform_source::Sha256& digest, std::array<char, 65U>& output) noexcept
{
    char text[65U]{};
    digest.hex(text);
    std::copy_n(text, output.size(), output.data());
}

void make_path(const std::array<char, 13U>& name, std::array<char, media_path_bytes>& path) noexcept
{
    std::copy(media_directory.begin(), media_directory.end(), path.begin());
    const std::size_t length = std::char_traits<char>::length(name.data());
    std::copy_n(name.data(), length, path.begin() + static_cast<std::ptrdiff_t>(media_directory.size()));
    path[media_directory.size() + length] = '\0';
}

[[nodiscard]] m110::Status read_exact(FIL& file, std::uint64_t offset, std::span<std::uint8_t> bytes, std::span<std::uint8_t> scratch) noexcept
{
    if (bytes.empty())
    {
        return m110::Status::success();
    }

    if (bytes.size() > std::numeric_limits<UINT>::max() || offset > std::numeric_limits<FSIZE_t>::max() || offset > std::numeric_limits<std::uint64_t>::max() - 3U ||
        scratch.size() < bytes.size() + (offset % 4U))
    {
        return prepare_io_error;
    }

    if (f_lseek(&file, static_cast<FSIZE_t>(offset)) != FR_OK)
    {
        return prepare_io_error;
    }

    auto* const aligned = scratch.data() + (offset % 4U);
    UINT actual{};
    const UINT requested = static_cast<UINT>(bytes.size());

    if (f_read(&file, aligned, requested, &actual) != FR_OK || actual != requested)
    {
        return prepare_short_read;
    }

    if (bytes.data() != aligned)
    {
        std::memcpy(bytes.data(), aligned, bytes.size());
    }

    return m110::Status::success();
}

} // namespace

enum class PrepareJob::Work : std::uint8_t
{
    idle,
    mount,
    source_open,
    source_hash,
    scenario_open,
    scenario_read,
    scenario_close,
    scenario_parse,
    path_checks,
    secondary_hash_open,
    secondary_hash,
    secondary_hash_close,
    secondary_open,
    secondary_measure,
    secondary_close,
    configure,
    output_create,
    render_read_decode,
    render_process_pack,
    render_append,
    output_finish,
    output_sync,
    output_close,
    source_close,
    verify_open,
    verify_header,
    verify_file,
    verify_close,
    manifest_create,
    manifest_write,
    manifest_sync,
    manifest_close,
    publish_wav,
    publish_manifest,
    cleanup,
    terminal,
};

m110::Status PrepareJob::begin(const prepare::Request& request, Resources resources) noexcept
{
    return begin_internal(request, resources, false);
}

m110::Status PrepareJob::begin_managed(const prepare::Request& request, Resources resources) noexcept
{
    return begin_internal(request, resources, true);
}

m110::Status PrepareJob::begin_internal(const prepare::Request& request, Resources resources, const bool managed) noexcept
{
    if (busy())
    {
        return prepare_busy;
    }

    snapshot_ = {};
    snapshot_.request = request;
    terminal_phase_ = prepare::Phase::idle;
    pending_failure_ = m110::Status::success();
    cancel_requested_ = false;
    manifest_published_ = false;
    mounted_ = false;
    source_open_ = false;
    work_open_ = false;
    output_open_ = false;
    manifest_open_ = false;
    secondary_open_ = false;
    engine_configured_ = false;
    has_secondary_ = false;
    managed_ = false;
    source_file_bytes_ = 0U;
    source_frames_ = 0U;
    source_cursor_ = 0U;
    render_block_input_frames_ = 0U;
    render_block_output_frames_ = 0U;
    managed_request_frames_ = 0U;
    scenario_file_bytes_ = 0U;
    scenario_cursor_ = 0U;
    secondary_file_bytes_ = 0U;
    hash_cursor_ = 0U;
    rendered_frames_ = 0U;
    managed_produced_frames_ = 0U;
    verified_file_bytes_ = 0U;
    verified_pcm_bytes_ = 0U;
    final_wav_bytes_ = 0U;
    output_frames_ = 0U;
    path_check_index_ = 0U;
    source_digest_ = {};
    scenario_digest_ = {};
    secondary_digest_ = {};
    verify_file_digest_ = {};
    verify_pcm_digest_ = {};
    wav_summary_ = {};
    std::array<char, 13U> source_name{};
    std::array<char, 13U> scenario_name{};
    std::array<char, 13U> output_name{};
    m110::Status validation = normalize_83(request.source, ".WAV", source_name);

    if (validation.is_ok())
    {
        validation = normalize_83(request.scenario, ".SCN", scenario_name);
    }

    if (validation.is_ok())
    {
        validation = normalize_83(request.output, ".WAV", output_name);
    }

    if (validation.is_ok() && (request.retention != prepare::Retention::temporary && request.retention != prepare::Retention::keep))
    {
        validation = prepare_bad_request;
    }

    if (validation.is_ok())
    {
        validation = validate_admission(request, resources);
    }

    if (!validation.is_ok())
    {
        snapshot_.phase = prepare::Phase::failed;
        snapshot_.status = validation;
        work_ = Work::terminal;
        return validation;
    }

    validation = require_distinct(source_name, scenario_name);

    if (validation.is_ok())
    {
        validation = require_distinct(source_name, output_name);
    }

    if (validation.is_ok())
    {
        validation = require_distinct(scenario_name, output_name);
    }

    if (!validation.is_ok())
    {
        snapshot_.phase = prepare::Phase::failed;
        snapshot_.status = validation;
        work_ = Work::terminal;
        return validation;
    }

    snapshot_.request.source = source_name;
    snapshot_.request.scenario = scenario_name;
    snapshot_.request.output = output_name;
    engine_ = resources.engine;
    watterson_ = resources.watterson;
    secondary_ = resources.secondary;
    scenario_ = resources.scenario;
    input_ = resources.input;
    output_ = resources.output;
    scenario_text_ = resources.scenario_text;
    packed_ = resources.packed;
    stream_read_scratch_ = resources.stream_read_scratch;
    make_path(source_name, source_path_);
    make_path(scenario_name, scenario_path_);
    make_path(output_name, output_path_);
    replace_extension(output_name, ".TMP", temporary_wav_path_);
    replace_extension(output_name, ".JSN", final_manifest_path_);
    replace_extension(output_name, ".JMP", temporary_manifest_path_);
    replace_extension(output_name, ".SCN", companion_scenario_path_);
    manifest_cursor_ = 0U;
    snapshot_.seed = request.seed;
    snapshot_.phase = prepare::Phase::starting;
    snapshot_.status = m110::Status::success();
    managed_ = managed;
    work_ = Work::mount;
    return m110::Status::success();
}

m110::Status PrepareJob::validate_admission(const prepare::Request& request, const Resources& resources) const noexcept
{
    const std::size_t minimum_packed = std::max(signal_lab::engine_block_frames * audio::pcm24_bytes_per_sample + 3U, signal_lab::engine_capacity_frames * audio::pcm24_bytes_per_sample);
    static_cast<void>(request);

    if (resources.engine == nullptr || resources.watterson == nullptr || resources.secondary == nullptr || resources.scenario == nullptr || resources.input.size() < signal_lab::engine_block_frames ||
        resources.output.size() < signal_lab::engine_capacity_frames || resources.packed.size() < minimum_packed || resources.scenario_text.size() < 256U ||
        resources.stream_read_scratch.size() < 4U || reinterpret_cast<std::uintptr_t>(resources.stream_read_scratch.data()) % 4U != 0U)
    {
        return prepare_bad_request;
    }

    return m110::Status::success();
}

bool PrepareJob::busy() const noexcept
{
    return (work_ != Work::idle && work_ != Work::terminal) || mounted_ || source_open_ || work_open_ || output_open_ || manifest_open_ || secondary_open_ || engine_configured_;
}

m110::Status PrepareJob::step() noexcept
{
    if (work_ == Work::idle)
    {
        return prepare_no_job;
    }

    if (work_ == Work::terminal)
    {
        return snapshot_.status;
    }

    if (cancel_requested_ && work_ != Work::cleanup && !manifest_published_)
    {
        terminal_phase_ = prepare::Phase::cancelled;
        pending_failure_ = prepare_cancelled;
        work_ = Work::cleanup;
    }

    if (managed_ && snapshot_.phase == prepare::Phase::rendering && (work_ == Work::render_read_decode || work_ == Work::render_process_pack || work_ == Work::render_append))
    {
        return prepare_managed_pending;
    }

    return advance();
}

m110::Status PrepareJob::produce_step(const std::uint32_t frames) noexcept
{
    if (!managed_ || snapshot_.phase != prepare::Phase::rendering || cancel_requested_ || work_ == Work::cleanup)
    {
        return prepare_no_job;
    }

    if (frames == 0U || frames > signal_lab::engine_block_frames || source_cursor_ > source_frames_ || frames > source_frames_ - source_cursor_)
    {
        return prepare_managed_mismatch;
    }

    if (work_ == Work::render_read_decode)
    {
        managed_request_frames_ = frames;
        const m110::Status decoded = render_read_decode_step();
        return decoded.is_ok() ? prepare_managed_pending : decoded;
    }

    if (work_ == Work::render_process_pack && managed_request_frames_ == frames)
    {
        const m110::Status processed = render_process_pack_step();

        if (!processed.is_ok())
        {
            return processed;
        }

        if (render_block_output_frames_ != frames)
        {
            return fail(prepare_managed_slip);
        }

        managed_produced_frames_ = rendered_frames_ + render_block_output_frames_;
        return m110::Status::success();
    }

    return prepare_managed_mismatch;
}

m110::Status PrepareJob::append_produced(const std::uint32_t frames) noexcept
{
    if (!managed_ || snapshot_.phase != prepare::Phase::rendering || cancel_requested_ || work_ == Work::cleanup)
    {
        return prepare_no_job;
    }

    if (work_ != Work::render_append || managed_request_frames_ != frames || frames == 0U || render_block_input_frames_ != frames || render_block_output_frames_ != frames)
    {
        return prepare_managed_mismatch;
    }

    const m110::Status appended = render_append_step();

    if (appended.is_ok())
    {
        managed_request_frames_ = 0U;
    }

    return appended;
}

m110::Status PrepareJob::finish_managed() noexcept
{
    if (!managed_ || snapshot_.phase != prepare::Phase::rendering || cancel_requested_ || work_ == Work::cleanup)
    {
        return prepare_no_job;
    }

    if (source_cursor_ != source_frames_ || managed_produced_frames_ != source_frames_ || rendered_frames_ != source_frames_ || managed_request_frames_ != 0U || work_ != Work::render_read_decode)
    {
        return prepare_managed_mismatch;
    }

    managed_ = false;
    work_ = Work::output_finish;
    return m110::Status::success();
}

m110::Status PrepareJob::cancel() noexcept
{
    if (!busy() || (manifest_published_ && work_ != Work::cleanup))
    {
        return prepare_no_job;
    }

    cancel_requested_ = true;

    if (work_ == Work::cleanup && snapshot_.phase == prepare::Phase::cleanup_failed)
    {
        snapshot_.phase = prepare::Phase::starting;
    }

    return m110::Status::success();
}

m110::Status PrepareJob::advance() noexcept
{
    if (work_ == Work::cleanup)
    {
        return cleanup_step();
    }

    if (work_ == Work::source_open)
    {
        return open_source_step();
    }

    if (work_ == Work::scenario_open)
    {
        return open_scenario_step();
    }

    if (work_ == Work::scenario_parse)
    {
        return parse_scenario_step();
    }

    if (work_ == Work::configure)
    {
        return configure_step();
    }

    if (work_ == Work::render_read_decode)
    {
        return render_read_decode_step();
    }

    if (work_ == Work::render_process_pack)
    {
        return render_process_pack_step();
    }

    if (work_ == Work::render_append)
    {
        return render_append_step();
    }

    if (work_ == Work::output_finish)
    {
        return finish_output_step();
    }

    if (work_ == Work::manifest_create)
    {
        return manifest_create_step();
    }

    if (work_ == Work::manifest_write)
    {
        return manifest_write_step();
    }

    if (work_ == Work::path_checks)
    {
        return path_check_step();
    }

    if (work_ == Work::source_hash)
    {
        return hash_source_step();
    }

    if (work_ == Work::scenario_read)
    {
        return scenario_read_step();
    }

    if (work_ == Work::secondary_hash)
    {
        return hash_secondary_step();
    }

    if (work_ == Work::verify_header)
    {
        return verify_header_step();
    }

    if (work_ == Work::verify_file)
    {
        return verify_file_step();
    }

    return dispatch_work();
}

m110::Status PrepareJob::commit_prefix() noexcept
{
    if (snapshot_.phase != prepare::Phase::rendering || !output_open_ || cancel_requested_ || work_ == Work::cleanup)
    {
        return prepare_no_job;
    }

    const auto committed = sink_.commit_frames(output_frames_);
    return committed.is_ok() ? committed : fail(committed);
}

m110::Status PrepareJob::read_prefix(const std::uint64_t first_frame, const std::span<std::uint8_t> output) noexcept
{
    if (snapshot_.phase != prepare::Phase::rendering || !output_open_ || cancel_requested_ || work_ == Work::cleanup)
    {
        return prepare_no_job;
    }

    const auto read = sink_.read_frames(first_frame, output);
    // Invalid ranges are caller refusals; only a latched storage failure ends
    // the preparation job and transfers it to its existing cleanup path.
    return sink_.status().is_ok() ? read : fail(sink_.status());
}

m110::Status PrepareJob::dispatch_work() noexcept
{
    if (work_ == Work::idle || work_ == Work::terminal)
    {
        return snapshot_.status;
    }

    if (work_ == Work::mount)
    {
        mounted_ = true;

        if (f_mount(&filesystem_, "2:/", 1U) != FR_OK)
        {
            return fail(prepare_io_error);
        }

        work_ = Work::source_open;
        return m110::Status::success();
    }

    if (work_ == Work::scenario_close)
    {
        if (const auto closed = close_work(); !closed.is_ok())
        {
            return fail(closed);
        }

        work_ = Work::scenario_parse;
        return m110::Status::success();
    }

    if (work_ >= Work::secondary_hash_open && work_ <= Work::secondary_close)
    {
        return dispatch_secondary_work();
    }

    if (work_ >= Work::output_create && work_ <= Work::source_close)
    {
        return dispatch_output_work();
    }

    if (work_ >= Work::verify_open && work_ <= Work::verify_close)
    {
        return dispatch_verify_work();
    }

    if (work_ >= Work::manifest_create && work_ <= Work::publish_manifest)
    {
        return dispatch_manifest_work();
    }

    return fail(m110::Status{.code = m110::StatusCode::internal_error, .message = "prepare job has an unknown work state"});
}

m110::Status PrepareJob::dispatch_secondary_work() noexcept
{
    switch (work_)
    {
    case Work::secondary_hash_open:
        if (f_open(&work_file_, secondary_path_.data(), FA_READ) != FR_OK)
        {
            return fail(prepare_io_error);
        }

        work_open_ = true;
        secondary_file_bytes_ = f_size(&work_file_);
        hash_cursor_ = 0U;
        secondary_digest_ = {};
        snapshot_.phase = prepare::Phase::hashing_secondary;
        work_ = Work::secondary_hash;
        return m110::Status::success();

    case Work::secondary_hash_close:
        if (const auto closed = close_work(); !closed.is_ok())
        {
            return fail(closed);
        }

        work_ = Work::secondary_open;
        return m110::Status::success();

    case Work::secondary_open:
    {
        secondary_open_ = true;
        const std::string_view basename{secondary_path_.data() + media_directory.size()};

        if (const auto opened = secondary_->open(basename, scenario_->second_source); !opened.is_ok())
        {
            return fail(opened);
        }

        snapshot_.phase = prepare::Phase::calibrating;
        work_ = Work::secondary_measure;
        return m110::Status::success();
    }

    case Work::secondary_measure:
    {
        const auto progress = secondary_->measure_step(input_.first(signal_lab::engine_block_frames));

        if (!progress.has_value())
        {
            return fail(progress.error());
        }

        if (progress.value() == SecondaryMeasureProgress::complete)
        {
            sha_hex(secondary_digest_, secondary_sha256_);
            work_ = Work::configure;
        }

        return m110::Status::success();
    }

    case Work::secondary_close:
        return close_secondary_step();

    default:
        return fail(m110::Status{.code = m110::StatusCode::internal_error, .message = "prepare secondary dispatch mismatch"});
    }
}

m110::Status PrepareJob::dispatch_output_work() noexcept
{
    switch (work_)
    {
    case Work::output_create:
        if (f_open(&output_file_, temporary_wav_path_.data(), FA_READ | FA_WRITE | FA_CREATE_NEW) != FR_OK)
        {
            return fail(prepare_io_error);
        }

        output_open_ = true;

        if (const auto bound = sink_.bind(&output_file_, stream_read_scratch_); !bound.is_ok())
        {
            return fail(bound);
        }

        if (const auto started = writer_.begin(sink_, {.sample_rate_hz = signal_lab::sample_rate_hz, .channels = 1U}); !started.is_ok())
        {
            return fail(started);
        }

        snapshot_.phase = prepare::Phase::rendering;
        source_cursor_ = 0U;
        output_frames_ = 0U;
        render_block_input_frames_ = 0U;
        render_block_output_frames_ = 0U;
        work_ = Work::render_read_decode;
        return m110::Status::success();

    case Work::output_sync:
        if (const auto synced = sink_.commit_frames(output_frames_); !synced.is_ok())
        {
            return fail(synced);
        }

        work_ = Work::output_close;
        return m110::Status::success();

    case Work::output_close:
        if (const auto closed = close_output(); !closed.is_ok())
        {
            return fail(closed);
        }

        work_ = Work::source_close;
        return m110::Status::success();

    case Work::source_close:
        if (const auto closed = close_source(); !closed.is_ok())
        {
            return fail(closed);
        }

        work_ = Work::verify_open;
        return m110::Status::success();

    default:
        return fail(m110::Status{.code = m110::StatusCode::internal_error, .message = "prepare output dispatch mismatch"});
    }
}

m110::Status PrepareJob::dispatch_verify_work() noexcept
{
    switch (work_)
    {
    case Work::verify_open:
        if (f_open(&work_file_, temporary_wav_path_.data(), FA_READ) != FR_OK)
        {
            return fail(prepare_io_error);
        }

        work_open_ = true;

        if (f_size(&work_file_) != final_wav_bytes_ || final_wav_bytes_ > std::numeric_limits<std::uint32_t>::max())
        {
            return fail(prepare_bad_verify);
        }

        snapshot_.phase = prepare::Phase::verifying;
        work_ = Work::verify_header;
        return m110::Status::success();

    case Work::verify_close:
        if (const auto closed = close_work(); !closed.is_ok())
        {
            return fail(closed);
        }

        work_ = Work::manifest_create;
        return m110::Status::success();

    default:
        return fail(m110::Status{.code = m110::StatusCode::internal_error, .message = "prepare verification dispatch mismatch"});
    }
}

m110::Status PrepareJob::dispatch_manifest_work() noexcept
{
    switch (work_)
    {
    case Work::manifest_sync:
        if (f_sync(&manifest_file_) != FR_OK)
        {
            return fail(prepare_io_error);
        }

        work_ = Work::manifest_close;
        return m110::Status::success();

    case Work::manifest_close:
        if (const auto closed = close_manifest(); !closed.is_ok())
        {
            return fail(closed);
        }

        work_ = Work::publish_wav;
        return m110::Status::success();

    case Work::publish_wav:
    case Work::publish_manifest:
        return publish_step();

    default:
        return fail(m110::Status{.code = m110::StatusCode::internal_error, .message = "prepare manifest dispatch mismatch"});
    }
}

m110::Status PrepareJob::open_source_step() noexcept
{
    if (f_open(&source_file_, source_path_.data(), FA_READ) != FR_OK)
    {
        return fail(prepare_io_error);
    }

    source_open_ = true;
    source_file_bytes_ = f_size(&source_file_);

    if (source_file_bytes_ > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(prepare_bad_wav);
    }

    WavPayload payload{};
    const WavReader reader{.context = this, .file_size = static_cast<std::uint32_t>(source_file_bytes_), .read = &read_source_callback};

    if (validate_pcm24_mono_48k_wav(reader, payload) != WavValidationError::none)
    {
        return fail(pending_failure_.is_ok() ? prepare_bad_wav : pending_failure_);
    }

    data_offset_ = payload.offset;
    source_frames_ = payload.bytes / audio::pcm24_bytes_per_sample;
    snapshot_.total_input_frames = source_frames_;
    hash_cursor_ = 0U;
    source_digest_ = {};
    snapshot_.phase = prepare::Phase::hashing_source;
    work_ = Work::source_hash;
    return m110::Status::success();
}

m110::Status PrepareJob::hash_source_step() noexcept
{
    if (hash_cursor_ == source_file_bytes_)
    {
        sha_hex(source_digest_, source_sha256_);
        work_ = Work::scenario_open;
        return m110::Status::success();
    }

    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(io_chunk_bytes, source_file_bytes_ - hash_cursor_));
    auto bytes = packed_.first(count);

    if (const m110::Status read = read_exact(source_file_, hash_cursor_, bytes, packed_); !read.is_ok())
    {
        return fail(read);
    }

    source_digest_.update(bytes.data(), bytes.size());
    hash_cursor_ += count;
    return m110::Status::success();
}

m110::Status PrepareJob::open_scenario_step() noexcept
{
    if (f_open(&work_file_, scenario_path_.data(), FA_READ) != FR_OK)
    {
        return fail(prepare_io_error);
    }

    work_open_ = true;
    scenario_file_bytes_ = f_size(&work_file_);

    if (scenario_file_bytes_ == 0U || scenario_file_bytes_ >= scenario_text_.size() || scenario_file_bytes_ > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(prepare_bad_request);
    }

    scenario_length_ = static_cast<std::uint32_t>(scenario_file_bytes_);
    scenario_cursor_ = 0U;
    scenario_digest_ = {};
    work_ = Work::scenario_read;
    return m110::Status::success();
}

m110::Status PrepareJob::scenario_read_step() noexcept
{
    if (scenario_cursor_ == scenario_file_bytes_)
    {
        scenario_text_[scenario_length_] = '\0';
        sha_hex(scenario_digest_, scenario_sha256_);
        work_ = Work::scenario_close;
        return m110::Status::success();
    }

    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(io_chunk_bytes, scenario_file_bytes_ - scenario_cursor_));
    auto target = std::span<std::uint8_t>{reinterpret_cast<std::uint8_t*>(scenario_text_.data() + scenario_cursor_), count};

    if (const m110::Status read = read_exact(work_file_, scenario_cursor_, target, packed_); !read.is_ok())
    {
        return fail(read);
    }

    scenario_digest_.update(target.data(), target.size());
    scenario_cursor_ += count;
    return m110::Status::success();
}

m110::Status PrepareJob::parse_scenario_step() noexcept
{
    if (const m110::Status parsed = signal_lab::parse_scenario(scenario_text_.data(), scenario_length_, *scenario_); !parsed.is_ok())
    {
        return fail(parsed);
    }

    if (!scenario_->has_reference_rms || !std::isfinite(scenario_->reference_rms) || !(scenario_->reference_rms > 0.0))
    {
        return fail(prepare_missing_reference);
    }

    if (snapshot_.request.seed_override)
    {
        scenario_->seed = snapshot_.request.seed;
    }

    snapshot_.seed = scenario_->seed;
    has_secondary_ = false;

    for (std::uint32_t index = 0U; index < scenario_->stage_count; ++index)
    {
        if (managed_ && scenario_->stages[index].type == signal_lab::StageType::sample_slip)
        {
            return fail(prepare_managed_slip);
        }

        if (scenario_->stages[index].type == signal_lab::StageType::second_source)
        {
            has_secondary_ = true;
        }
    }

    if (has_secondary_)
    {
        std::array<char, 13U> secondary_name{};

        if (const m110::Status name_status = normalize_secondary_name(scenario_->second_source_path, secondary_name); !name_status.is_ok())
        {
            return fail(name_status);
        }

        const std::array<char, 13U>& source_name = snapshot_.request.source;
        const std::array<char, 13U>& scenario_name = snapshot_.request.scenario;
        const std::array<char, 13U>& output_name = snapshot_.request.output;

        if (!require_distinct(source_name, secondary_name).is_ok() || !require_distinct(scenario_name, secondary_name).is_ok() || !require_distinct(output_name, secondary_name).is_ok())
        {
            return fail(prepare_alias);
        }

        make_path(secondary_name, secondary_path_);
    }

    const std::string_view companion_name{companion_scenario_path_.data() + media_directory.size()};
    const std::string_view scenario_name{scenario_path_.data() + media_directory.size()};

    if (companion_name == scenario_name)
    {
        return fail(prepare_alias);
    }

    path_check_index_ = 0U;
    work_ = Work::path_checks;
    return m110::Status::success();
}

m110::Status PrepareJob::path_check_step() noexcept
{
    if (path_check_index_ == 5U)
    {
        work_ = has_secondary_ ? Work::secondary_hash_open : Work::configure;
        return m110::Status::success();
    }

    const std::array<char, media_path_bytes>* target{};

    switch (path_check_index_)
    {
    case 0U:
        target = &output_path_;
        break;

    case 1U:
        target = &temporary_wav_path_;
        break;

    case 2U:
        target = &final_manifest_path_;
        break;

    case 3U:
        target = &temporary_manifest_path_;
        break;

    case 4U:
        target = &companion_scenario_path_;
        break;

    default:
        return fail(prepare_bad_request);
    }

    FILINFO info{};
    const FRESULT result = f_stat(target->data(), &info);

    if (result == FR_OK)
    {
        return fail(prepare_existing_output);
    }

    if (result != FR_NO_FILE && result != FR_NO_PATH)
    {
        return fail(prepare_io_error);
    }

    ++path_check_index_;
    return m110::Status::success();
}

m110::Status PrepareJob::hash_secondary_step() noexcept
{
    if (hash_cursor_ == secondary_file_bytes_)
    {
        sha_hex(secondary_digest_, secondary_sha256_);
        work_ = Work::secondary_hash_close;
        return m110::Status::success();
    }

    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(io_chunk_bytes, secondary_file_bytes_ - hash_cursor_));
    auto bytes = packed_.first(count);

    if (const m110::Status read = read_exact(work_file_, hash_cursor_, bytes, packed_); !read.is_ok())
    {
        return fail(read);
    }

    secondary_digest_.update(bytes.data(), bytes.size());
    hash_cursor_ += count;
    return m110::Status::success();
}

m110::Status PrepareJob::configure_step() noexcept
{
    if (snapshot_.phase != prepare::Phase::calibrating)
    {
        snapshot_.phase = prepare::Phase::starting;
    }

    const signal_lab::StageResources resources{watterson_, nullptr, has_secondary_ ? static_cast<signal_lab::SecondarySource*>(secondary_) : nullptr};
    engine_configured_ = true;

    if (const m110::Status configured = engine_->configure(*scenario_, scenario_->reference_rms, source_frames_, resources); !configured.is_ok())
    {
        return fail(configured);
    }

    work_ = Work::output_create;
    return m110::Status::success();
}

m110::Status PrepareJob::render_read_decode_step() noexcept
{
    if (source_cursor_ == source_frames_)
    {
        work_ = Work::output_finish;
        return m110::Status::success();
    }

    const std::size_t frames = managed_ ? managed_request_frames_ : static_cast<std::size_t>(std::min<std::uint64_t>(signal_lab::engine_block_frames, source_frames_ - source_cursor_));
    const std::uint64_t byte_offset = static_cast<std::uint64_t>(data_offset_) + source_cursor_ * audio::pcm24_bytes_per_sample;
    const std::size_t packed_bytes = frames * audio::pcm24_bytes_per_sample;

    if (byte_offset > std::numeric_limits<std::uint32_t>::max() || packed_bytes + byte_offset % 4U > packed_.size())
    {
        return fail(prepare_bad_wav);
    }

    if (const m110::Status read = read_exact(source_file_, byte_offset, std::span<std::uint8_t>{packed_.data() + (byte_offset % 4U), packed_bytes}, packed_); !read.is_ok())
    {
        return fail(read);
    }

    const auto* source_bytes = packed_.data() + (byte_offset % 4U);

    for (std::size_t index = 0U; index < frames; ++index)
    {
        input_[index] = audio::pcm24_to_float(audio::read_pcm24_le(source_bytes + index * audio::pcm24_bytes_per_sample));
    }

    render_block_input_frames_ = static_cast<std::uint32_t>(frames);
    render_block_output_frames_ = 0U;
    work_ = Work::render_process_pack;
    return m110::Status::success();
}

m110::Status PrepareJob::render_process_pack_step() noexcept
{
    if (render_block_input_frames_ == 0U || render_block_input_frames_ > input_.size())
    {
        return fail(prepare_bad_request);
    }

    const auto processed = engine_->process(input_.first(render_block_input_frames_), output_);

    if (!processed.has_value())
    {
        return fail(processed.error());
    }

    const std::size_t produced = processed.value();

    if (managed_ && produced != render_block_input_frames_)
    {
        return fail(prepare_managed_slip);
    }

    if (produced > output_.size() || produced * audio::pcm24_bytes_per_sample > packed_.size())
    {
        return fail(prepare_bad_request);
    }

    for (std::size_t index = 0U; index < produced; ++index)
    {
        audio::write_pcm24_le(packed_.data() + index * audio::pcm24_bytes_per_sample, signal_lab::det::quantize_pcm24(output_[index]));
    }

    render_block_output_frames_ = static_cast<std::uint32_t>(produced);
    work_ = Work::render_append;
    return m110::Status::success();
}

m110::Status PrepareJob::render_append_step() noexcept
{
    if (source_cursor_ > source_frames_ || render_block_input_frames_ == 0U || render_block_input_frames_ > source_frames_ - source_cursor_ ||
        render_block_output_frames_ > packed_.size() / audio::pcm24_bytes_per_sample)
    {
        return fail(prepare_bad_request);
    }

    const std::size_t packed_bytes = static_cast<std::size_t>(render_block_output_frames_) * audio::pcm24_bytes_per_sample;

    if (const m110::Status appended = writer_.append_frames(packed_.first(packed_bytes)); !appended.is_ok())
    {
        return fail(appended);
    }

    source_cursor_ += render_block_input_frames_;
    rendered_frames_ += render_block_output_frames_;
    snapshot_.input_frames = source_cursor_;
    snapshot_.output_frames = rendered_frames_;
    output_frames_ = rendered_frames_;
    render_block_input_frames_ = 0U;
    render_block_output_frames_ = 0U;
    work_ = Work::render_read_decode;
    return m110::Status::success();
}

m110::Status PrepareJob::finish_output_step() noexcept
{
    if (const m110::Status finished = writer_.finish(wav_summary_); !finished.is_ok())
    {
        return fail(finished);
    }

    if (wav_summary_.frames != output_frames_ || wav_summary_.file_bytes < artifact::pcm24_wav_header_bytes)
    {
        return fail(prepare_bad_verify);
    }

    snapshot_.output_frames = wav_summary_.frames;
    final_wav_bytes_ = wav_summary_.file_bytes;
    engine_output_digest_ = engine_->stats().output_digest;
    detach_engine();
    snapshot_.phase = prepare::Phase::verifying;
    work_ = secondary_open_ ? Work::secondary_close : Work::output_sync;
    return m110::Status::success();
}

m110::Status PrepareJob::close_secondary_step() noexcept
{
    const m110::Status closed = secondary_->close();

    if (!closed.is_ok())
    {
        return fail(closed);
    }

    secondary_open_ = false;
    work_ = Work::output_sync;
    return m110::Status::success();
}

m110::Status PrepareJob::verify_header_step() noexcept
{
    constexpr std::size_t header_bytes = artifact::pcm24_wav_header_bytes;

    if (final_wav_bytes_ > std::numeric_limits<std::uint32_t>::max() || packed_.size() < header_bytes + 3U)
    {
        return fail(prepare_bad_verify);
    }

    auto* const header = packed_.data();

    if (const m110::Status read = read_exact(work_file_, 0U, std::span<std::uint8_t>{header, header_bytes}, packed_); !read.is_ok())
    {
        return fail(read);
    }

    const std::uint32_t data_bytes = wav_read_le32(header + 40U);

    if (wav_read_le32(header) != wav_fourcc('R', 'I', 'F', 'F') || wav_read_le32(header + 4U) != final_wav_bytes_ - 8U || wav_read_le32(header + 8U) != wav_fourcc('W', 'A', 'V', 'E') ||
        wav_read_le32(header + 12U) != wav_fourcc('f', 'm', 't', ' ') || wav_read_le32(header + 16U) != 16U || wav_read_le16(header + 20U) != 1U || wav_read_le16(header + 22U) != 1U ||
        wav_read_le32(header + 24U) != signal_lab::sample_rate_hz || wav_read_le32(header + 28U) != signal_lab::sample_rate_hz * 3U || wav_read_le16(header + 32U) != 3U ||
        wav_read_le16(header + 34U) != 24U || wav_read_le32(header + 36U) != wav_fourcc('d', 'a', 't', 'a') || data_bytes != wav_summary_.data_bytes ||
        data_bytes % audio::pcm24_bytes_per_sample != 0U || artifact::pcm24_wav_header_bytes + data_bytes + (data_bytes & 1U) != final_wav_bytes_)
    {
        return fail(prepare_bad_verify);
    }

    verified_file_bytes_ = 0U;
    verified_pcm_bytes_ = 0U;
    verify_file_digest_ = {};
    verify_pcm_digest_ = {};
    work_ = Work::verify_file;
    return m110::Status::success();
}

m110::Status PrepareJob::verify_file_step() noexcept
{
    if (verified_file_bytes_ == final_wav_bytes_)
    {
        sha_hex(verify_file_digest_, output_file_sha256_);
        std::array<char, 65U> pcm_sha{};
        sha_hex(verify_pcm_digest_, pcm_sha);

        if (verified_pcm_bytes_ != wav_summary_.data_bytes || std::string_view{pcm_sha.data()} != std::string_view{wav_summary_.pcm_sha256.data()})
        {
            return fail(prepare_bad_verify);
        }

        snapshot_.wav_sha256 = output_file_sha256_;
        snapshot_.pcm_sha256 = pcm_sha;
        work_ = Work::verify_close;
        return m110::Status::success();
    }

    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(io_chunk_bytes, final_wav_bytes_ - verified_file_bytes_));
    const std::uint64_t offset = verified_file_bytes_;

    if (count + offset % 4U > packed_.size() || f_lseek(&work_file_, static_cast<FSIZE_t>(offset)) != FR_OK)
    {
        return fail(prepare_io_error);
    }

    auto* const bytes = packed_.data() + (offset % 4U);
    UINT actual{};

    if (f_read(&work_file_, bytes, static_cast<UINT>(count), &actual) != FR_OK || actual != count)
    {
        return fail(prepare_short_read);
    }

    verify_file_digest_.update(bytes, count);
    const std::uint64_t data_start = artifact::pcm24_wav_header_bytes;
    const std::uint64_t data_end = data_start + wav_summary_.data_bytes;
    const std::uint64_t overlap_start = std::max(offset, data_start);
    const std::uint64_t overlap_end = std::min(offset + count, data_end);

    if (overlap_end > overlap_start)
    {
        const std::size_t pcm_offset = static_cast<std::size_t>(overlap_start - offset);
        const std::size_t pcm_count = static_cast<std::size_t>(overlap_end - overlap_start);
        verify_pcm_digest_.update(bytes + pcm_offset, pcm_count);
        verified_pcm_bytes_ += pcm_count;
    }

    const std::uint64_t pad_offset = data_end;

    if ((wav_summary_.data_bytes & 1U) != 0U && offset <= pad_offset && offset + count > pad_offset && bytes[static_cast<std::size_t>(pad_offset - offset)] != 0U)
    {
        return fail(prepare_bad_verify);
    }

    verified_file_bytes_ += count;
    return m110::Status::success();
}

m110::Status PrepareJob::manifest_create_step() noexcept
{
    const auto seed_text = uint64_text(snapshot_.seed);
    const auto input_frames_text = uint64_text(source_frames_);
    const auto output_frames_text = uint64_text(snapshot_.output_frames);
    const auto digest_text = uint64_text<16U, 16U>(engine_output_digest_);
    const int length =
        std::snprintf(scenario_text_.data(), scenario_text_.size(),
                      "{\"schema\":\"waveform-generator.prepared/2\",\"complete\":true,\"retention\":\"%s\","
                      "\"source\":\"%s\",\"source_sha256\":\"%s\",\"scenario\":\"%s\",\"scenario_sha256\":\"%s\","
                      "\"secondary\":\"%s\",\"secondary_sha256\":\"%s\",\"seed\":%s,\"sample_rate_hz\":%u,"
                      "\"channels\":1,\"bits_per_sample\":24,\"input_frames\":%s,\"output_frames\":%s,"
                      "\"wav_sha256\":\"%s\",\"pcm_sha256\":\"%s\",\"engine_output_fnv1a64\":\"%s\","
                      "\"engine_algorithm_version\":1,\"firmware_commit\":\"%.*s\"}\n",
                      snapshot_.request.retention == prepare::Retention::keep ? "keep" : "temporary", snapshot_.request.source.data(), source_sha256_.data(), snapshot_.request.scenario.data(),
                      scenario_sha256_.data(), has_secondary_ ? secondary_path_.data() + media_directory.size() : "", has_secondary_ ? secondary_sha256_.data() : "", seed_text.data(),
                      static_cast<unsigned>(signal_lab::sample_rate_hz), input_frames_text.data(), output_frames_text.data(), snapshot_.wav_sha256.data(), snapshot_.pcm_sha256.data(),
                      digest_text.data(), static_cast<int>(build_info::fw_commit.size()), build_info::fw_commit.data());

    if (length <= 0 || static_cast<std::size_t>(length) >= scenario_text_.size())
    {
        return fail(prepare_bad_manifest);
    }

    manifest_length_ = static_cast<std::uint32_t>(length);

    if (f_open(&manifest_file_, temporary_manifest_path_.data(), FA_WRITE | FA_CREATE_NEW) != FR_OK)
    {
        return fail(prepare_io_error);
    }

    manifest_open_ = true;
    manifest_cursor_ = 0U;
    work_ = Work::manifest_write;
    snapshot_.phase = prepare::Phase::publishing;
    return m110::Status::success();
}

m110::Status PrepareJob::manifest_write_step() noexcept
{
    if (manifest_cursor_ == manifest_length_)
    {
        work_ = Work::manifest_sync;
        return m110::Status::success();
    }

    const UINT count = static_cast<UINT>(std::min<std::uint32_t>(io_chunk_bytes, manifest_length_ - manifest_cursor_));
    UINT actual{};

    if (f_write(&manifest_file_, scenario_text_.data() + manifest_cursor_, count, &actual) != FR_OK || actual != count)
    {
        return fail(prepare_io_error);
    }

    manifest_cursor_ += count;
    return m110::Status::success();
}

m110::Status PrepareJob::publish_step() noexcept
{
    if (work_ == Work::publish_wav)
    {
        if (f_rename(temporary_wav_path_.data(), output_path_.data()) != FR_OK)
        {
            return fail(prepare_io_error);
        }

        work_ = Work::publish_manifest;
        return m110::Status::success();
    }

    if (f_rename(temporary_manifest_path_.data(), final_manifest_path_.data()) != FR_OK)
    {
        return fail(prepare_io_error);
    }

    manifest_published_ = true;
    terminal_phase_ = prepare::Phase::complete;
    pending_failure_ = m110::Status::success();
    work_ = Work::cleanup;
    return m110::Status::success();
}

m110::Status PrepareJob::read_source_callback(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t count) noexcept
{
    auto& self = *static_cast<PrepareJob*>(context);
    const m110::Status result = read_exact(self.source_file_, offset, std::span<std::uint8_t>{destination, count}, self.packed_);

    if (!result.is_ok() && self.pending_failure_.is_ok())
    {
        self.pending_failure_ = result;
    }

    return result;
}

m110::Status PrepareJob::close_work() noexcept
{
    if (work_open_ && f_close(&work_file_) != FR_OK)
    {
        return prepare_io_error;
    }

    work_open_ = false;
    return m110::Status::success();
}

m110::Status PrepareJob::close_output() noexcept
{
    if (output_open_ && f_close(&output_file_) != FR_OK)
    {
        return prepare_io_error;
    }

    output_open_ = false;
    sink_.unbind();

    // A cancelled/failed render can leave the incremental WAV writer active
    // even after its file and sink have been discarded. Reset that state only
    // after the output handle closes successfully, so this PrepareJob can be
    // reused without weakening close-failure retry behavior. Completed writers
    // retain their summary state for the successful publication path.
    if (writer_.state() == artifact::Pcm24ArtifactState::active)
    {
        writer_.cancel();
    }

    return m110::Status::success();
}

m110::Status PrepareJob::close_source() noexcept
{
    if (source_open_ && f_close(&source_file_) != FR_OK)
    {
        return prepare_io_error;
    }

    source_open_ = false;
    return m110::Status::success();
}

m110::Status PrepareJob::close_manifest() noexcept
{
    if (manifest_open_ && f_close(&manifest_file_) != FR_OK)
    {
        return prepare_io_error;
    }

    manifest_open_ = false;
    return m110::Status::success();
}

m110::Status PrepareJob::unmount() noexcept
{
    if (mounted_ && f_mount(nullptr, "2:/", 0U) != FR_OK)
    {
        return prepare_io_error;
    }

    mounted_ = false;
    return m110::Status::success();
}

void PrepareJob::detach_engine() noexcept
{
    if (engine_configured_ && engine_ != nullptr)
    {
        engine_->configure_passthrough();
    }

    engine_configured_ = false;
}

m110::Status PrepareJob::fail(m110::Status failure) noexcept
{
    if (pending_failure_.is_ok())
    {
        pending_failure_ = failure;
        terminal_phase_ = prepare::Phase::failed;
    }

    snapshot_.status = pending_failure_;
    work_ = Work::cleanup;
    return failure;
}

m110::Status PrepareJob::cleanup_step() noexcept
{
    detach_engine();
    m110::Status result = m110::Status::success();

    if (secondary_open_)
    {
        result = secondary_->close();

        if (result.is_ok())
        {
            secondary_open_ = false;
        }
    }
    else if (manifest_open_)
    {
        result = close_manifest();
    }
    else if (output_open_)
    {
        result = close_output();
    }
    else if (work_open_)
    {
        result = close_work();
    }
    else if (source_open_)
    {
        result = close_source();
    }
    else if (mounted_)
    {
        result = unmount();
    }
    else
    {
        const prepare::Phase final_phase = terminal_phase_ == prepare::Phase::idle ? prepare::Phase::failed : terminal_phase_;
        snapshot_.phase = final_phase;
        snapshot_.status = pending_failure_;
        work_ = Work::terminal;
        return pending_failure_;
    }

    if (!result.is_ok())
    {
        if (pending_failure_.is_ok() || terminal_phase_ == prepare::Phase::cancelled)
        {
            pending_failure_ = result;
            terminal_phase_ = prepare::Phase::failed;
        }

        snapshot_.phase = prepare::Phase::cleanup_failed;
        snapshot_.status = result;
        return result;
    }

    return m110::Status::success();
}

} // namespace waveform_generator
