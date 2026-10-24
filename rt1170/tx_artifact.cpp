// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Target artifact owner
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: One worker owns local FatFs generation and pending response state.
// Contract: Honor media ownership and cancellation; terminal publication follows completed writes.

#include "tx_artifact.hpp"
#include "common/uint64_text.hpp"
#include "error_map.hpp"
#include "platform/waveform_msc.h"
#include "player.hpp"
#include "waveform-source/encoder_registry.hpp"
#include "waveform-source/wav_generator.hpp"

extern "C"
{
#include "FreeRTOS.h"
#include "ff.h"
#include "task.h"
}

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

namespace waveform_generator
{
namespace
{
alignas(32) __attribute__((section(".ocram.encoder"))) unsigned char encoder_storage[encoder_workspace_capacity];
alignas(waveform_source::WavGenerator) __attribute__((section(".ocram.wav_writer"))) unsigned char writer_storage[sizeof(waveform_source::WavGenerator)];
waveform_source::Encoder* encoder{};
waveform_source::WavGenerator* writer{};
FATFS fs{};
FIL payload_file{}, output_file{}, manifest_file{};
bool mounted{}, payload_open{}, output_open{}, manifest_open{}, cleanup_pending{};
std::atomic<bool> request_pending{}, response_pending{}, disconnected{};
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> processing_request{};
tx_protocol::Request request_slot{};
tx_protocol::Reply response_slot{};
TxArtifactSnapshot snapshot{};
std::uint16_t selected_rate{600};
std::uint8_t selected_interleave{1};
char selected_encoder[16]{};
char selected_profile[32]{};
const EncoderDescriptor* descriptor{};
char input_path[24]{"2:/WG/GEN.BIN"};
char output_path[24]{"2:/WG/GEN.WAV"};
char temporary_path[24]{"2:/WG/GEN.TMP"};
char manifest_path[24]{"2:/WG/GEN.JSON"};
char manifest_temporary_path[24]{"2:/WG/GEN.JMP"};

class FileBytes final : public waveform_source::ByteSource
{
    std::size_t read(std::uint8_t* bytes, std::size_t capacity) noexcept override
    {
        UINT actual{};
        return f_read(&payload_file, bytes, static_cast<UINT>(capacity), &actual) == FR_OK ? actual : 0;
    }
} file_bytes;

// The temporary WAV, created by its first write.
constexpr m110::Status output_not_created{.code = m110::StatusCode::io_error, .message = "the temporary WAV could not be created"};
constexpr m110::Status output_not_written{.code = m110::StatusCode::io_error, .message = "the card took less than the whole write"};

class FileSink final : public waveform_source::ByteSink
{
    m110::Status write(const std::uint8_t* bytes, std::size_t count) noexcept override
    {
        if (!output_open)
        {
            if (f_open(&output_file, temporary_path, FA_WRITE | FA_CREATE_NEW) != FR_OK)
            {
                return output_not_created;
            }

            output_open = true;
        }

        UINT actual{};
        return f_write(&output_file, bytes, static_cast<UINT>(count), &actual) == FR_OK && actual == count ? m110::Status::success() : output_not_written;
    }
} file_sink;

// Keep every handle marked open until FatFs confirms its close. In particular,
// f_close() can fail while syncing and leave the FIL valid for a later retry.
[[nodiscard]] m110::Status close_files() noexcept
{
    if (payload_open && f_close(&payload_file) == FR_OK)
    {
        payload_open = false;
    }

    if (output_open && f_close(&output_file) == FR_OK)
    {
        output_open = false;
    }

    if (manifest_open && f_close(&manifest_file) == FR_OK)
    {
        manifest_open = false;
    }

    if (payload_open || output_open || manifest_open)
    {
        cleanup_pending = true;
        return tx_refusal::artifact_finalize_error;
    }

    if (mounted)
    {
        if (f_mount(nullptr, "2:/", 0) != FR_OK)
        {
            cleanup_pending = true;
            return tx_refusal::artifact_finalize_error;
        }

        mounted = false;
    }

    cleanup_pending = false;
    return m110::Status::success();
}

[[nodiscard]] m110::Status fail(const char* error) noexcept
{
    if (writer)
    {
        writer->stop();
    }

    const m110::Status cleanup = close_files();
    taskENTER_CRITICAL();
    snapshot.state = 4;
    snapshot.error = cleanup.is_ok() ? error : cleanup.message;
    taskEXIT_CRITICAL();
    return cleanup;
}

// The card for the owner's files: success once it is Local and mounted with
// its WG directory; tx_refusal::media_not_local while the host owns it,
// media_failed when it does not mount or the directory cannot be made.
[[nodiscard]] m110::Status mount() noexcept
{
    if (cleanup_pending)
    {
        return refusal::busy;
    }

    if (!WFG_MediaIsLocal())
    {
        return tx_refusal::media_not_local;
    }

    if (mounted)
    {
        return m110::Status::success();
    }

    // FatFs registers fs before attempting an immediate mount. Retain ownership
    // before the call so even a failed mount is unregistered by close_files().
    mounted = true;

    if (f_mount(&fs, "2:/", 1) != FR_OK)
    {
        const m110::Status cleanup = close_files();
        return cleanup.is_ok() ? tx_refusal::media_failed : cleanup;
    }

    const auto directory = f_mkdir("2:/WG");

    if (directory != FR_OK && directory != FR_EXIST)
    {
        const m110::Status cleanup = close_files();
        return cleanup.is_ok() ? tx_refusal::media_failed : cleanup;
    }

    return m110::Status::success();
}

void make_path(char* path, const char* filename, const char* extension = nullptr) noexcept
{
    std::strcpy(path, "2:/WG/");
    std::strcat(path, filename);

    if (extension)
    {
        std::strcpy(std::strrchr(path, '.'), extension);
    }
}

void select_output(const char* name) noexcept
{
    taskENTER_CRITICAL();
    snapshot = TxArtifactSnapshot{};
    std::strcpy(snapshot.filename, name);
    taskEXIT_CRITICAL();
    make_path(output_path, name);
    make_path(input_path, name, ".BIN");
    make_path(temporary_path, name, ".TMP");
    make_path(manifest_path, name, ".JSON");
    make_path(manifest_temporary_path, name, ".JMP");
}

// Whether `path` is free for a new artifact: success when no file has the
// name, output_exists when one has, output_unchecked for a card error.
[[nodiscard]] m110::Status check_absent(const char* path) noexcept
{
    FILINFO info{};
    const FRESULT found = f_stat(path, &info);

    if (found == FR_NO_FILE)
    {
        return m110::Status::success();
    }

    return found == FR_OK ? tx_refusal::output_exists : tx_refusal::output_unchecked;
}

// Whether every name the selected artifact will take is free: success, or
// check_absent()'s Status for the first that is not.
[[nodiscard]] m110::Status check_outputs_absent() noexcept
{
    for (const char* path : {std::data(output_path), std::data(manifest_path), std::data(temporary_path), std::data(manifest_temporary_path)})
    {
        if (const m110::Status free = check_absent(path); !free.is_ok())
        {
            return free;
        }
    }

    return m110::Status::success();
}

// Opens the uploaded or named payload for the encoder, after closing an
// upload still open: its size, or the Status of the step that failed.
[[nodiscard]] m110::Result<FSIZE_t> open_payload() noexcept
{
    if (payload_open)
    {
        const bool good = f_sync(&payload_file) == FR_OK;
        const bool closed = f_close(&payload_file) == FR_OK;

        if (closed)
        {
            payload_open = false;
        }

        if (!good || !closed)
        {
            return tx_refusal::payload_write_error;
        }
    }

    if (f_open(&payload_file, input_path, FA_READ) != FR_OK)
    {
        return tx_refusal::bad_payload_file;
    }

    payload_open = true;
    const auto bytes = f_size(&payload_file);

    if (bytes > tx_protocol::maximum_upload_bytes)
    {
        return tx_refusal::payload_too_large;
    }

    return bytes;
}

// Starts the WAV job for the selected names and encoder: success (state 2),
// or the Status whose message is the reply's code (the encoder's or the
// writer's own message when begin() refuses). The caller fails the job.
[[nodiscard]] m110::Status start_generation() noexcept
{
    descriptor = find_encoder(std::data(selected_encoder));

    if (!descriptor)
    {
        return tx_refusal::unknown_encoder;
    }

    if (const m110::Status media = mount(); !media.is_ok())
    {
        return media;
    }

    if (const m110::Status free = check_outputs_absent(); !free.is_ok())
    {
        return free;
    }

    const m110::Result<FSIZE_t> payload = open_payload();

    if (!payload.has_value())
    {
        return payload.error();
    }

    const auto bytes = payload.value();

    if (writer)
    {
        writer->stop();
    }

    if (encoder)
    {
        encoder->~Encoder();
    }

    encoder = descriptor->construct(encoder_storage, sizeof(encoder_storage));

    if (!encoder)
    {
        return tx_refusal::encoder_workspace_error;
    }

    if (!writer)
        writer = new (writer_storage) waveform_source::WavGenerator{};

    const char* interleave = selected_interleave == 0 ? "short" : selected_interleave == 1 ? "long" : "zero";

    if (!selected_profile[0])
    {
        std::snprintf(selected_profile, sizeof(selected_profile), "%u:%s", static_cast<unsigned>(selected_rate), interleave);
    }

    const auto status = writer->begin(*encoder, selected_profile, file_bytes, static_cast<std::size_t>(bytes), file_sink);

    if (!status.is_ok())
    {
        return status;
    }

    taskENTER_CRITICAL();
    snapshot.state = 2;
    snapshot.payload_bytes = static_cast<std::uint32_t>(bytes);
    snapshot.frames_written = 0;
    snapshot.total_frames = writer->total_frames();
    snapshot.error = "none";
    taskEXIT_CRITICAL();
    return m110::Status::success();
}

// Closes the finished WAV and the payload, writes the manifest and publishes
// both under their final names: success (state 3), or
// tx_refusal::artifact_finalize_error for any step that failed.
[[nodiscard]] m110::Status publish_artifact() noexcept
{
    if (f_sync(&output_file) != FR_OK)
    {
        return tx_refusal::artifact_finalize_error;
    }

    const bool output_closed = !output_open || f_close(&output_file) == FR_OK;

    if (output_closed)
    {
        output_open = false;
    }

    const bool payload_closed = !payload_open || f_close(&payload_file) == FR_OK;

    if (payload_closed)
    {
        payload_open = false;
    }

    if (!output_closed || !payload_closed)
    {
        return tx_refusal::artifact_finalize_error;
    }

    const auto hashes = writer->hashes();
    const auto frames_text = uint64_text(snapshot.total_frames);
    char metadata[1536]{};
    const int count = std::snprintf(metadata, sizeof(metadata),
                                    "{\n\"schema\":\"waveform-artifact/1\",\"encoder\":\"%s\",\"generator\":\"%s\","
                                    "\"build_id\":\"%s\",\"profile\":\"%s\",\"sample_rate_hz\":48000,"
                                    "\"channels\":1,\"bits_per_sample\":24,\"frames\":%s,\"trailing_silence_frames\":48000,"
                                    "\"payload\":{\"path\":\"%s\",\"bytes\":%lu,\"sha256\":\"%s\"},"
                                    "\"wav\":{\"path\":\"%s\",\"sha256\":\"%s\",\"pcm_sha256\":\"%s\"},\"complete\":true}\n",
                                    descriptor->id, descriptor->version, WFG_BUILD_ID, selected_profile, frames_text.data(), input_path + 6, static_cast<unsigned long>(snapshot.payload_bytes),
                                    hashes.payload, snapshot.filename, hashes.wav, hashes.pcm);

    if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(metadata))
    {
        return tx_refusal::artifact_finalize_error;
    }

    if (f_open(&manifest_file, manifest_temporary_path, FA_WRITE | FA_CREATE_NEW) != FR_OK)
    {
        return tx_refusal::artifact_finalize_error;
    }

    manifest_open = true;
    UINT actual{};
    const bool wrote = f_write(&manifest_file, metadata, static_cast<UINT>(count), &actual) == FR_OK && actual == static_cast<UINT>(count);
    const bool synced = f_sync(&manifest_file) == FR_OK;
    const bool closed = f_close(&manifest_file) == FR_OK;

    if (closed)
    {
        manifest_open = false;
    }

    if (!wrote || !synced || !closed)
    {
        return tx_refusal::artifact_finalize_error;
    }

    if (f_rename(temporary_path, output_path) != FR_OK)
    {
        return tx_refusal::artifact_finalize_error;
    }

    if (f_rename(manifest_temporary_path, manifest_path) != FR_OK)
    {
        // Metadata is the commit marker. Return the WAV to its temporary name
        // if publishing that marker fails; report failure even if rollback fails.
        (void)f_rename(output_path, temporary_path);
        return tx_refusal::artifact_finalize_error;
    }

    taskENTER_CRITICAL();
    std::strcpy(snapshot.wav_sha256, hashes.wav);
    snapshot.state = 3;
    taskEXIT_CRITICAL();

    if (const m110::Status cleanup = close_files(); !cleanup.is_ok())
    {
        taskENTER_CRITICAL();
        snapshot.error = cleanup.message;
        taskEXIT_CRITICAL();
    }

    return m110::Status::success();
}

// A start's or an upload's failure ends the transaction: the job fails with
// its code. The Status, passed on.
[[nodiscard]] m110::Status failed_transaction(m110::Status failure) noexcept
{
    if (!failure.is_ok())
    {
        if (const m110::Status cleanup = fail(failure.message); !cleanup.is_ok())
        {
            return cleanup;
        }
    }

    return failure;
}

// RESET: aborts a running transaction, or releases the card, and selects
// the defaults again: GEN.WAV, and the mode STATUS reports before any
// (TxArtifactSnapshot's).
[[nodiscard]] m110::Status serve_reset() noexcept
{
    constexpr TxArtifactSnapshot defaults{};

    if (snapshot.state == 1 || snapshot.state == 2)
    {
        if (const m110::Status cleanup = fail("ABORTED"); !cleanup.is_ok())
        {
            return cleanup;
        }
    }
    else if (const m110::Status cleanup = close_files(); !cleanup.is_ok())
    {
        return cleanup;
    }

    selected_rate = defaults.rate;
    selected_interleave = defaults.interleave;
    selected_encoder[0] = 0;
    selected_profile[0] = 0;
    select_output("GEN.WAV");
    return m110::Status::success();
}

[[nodiscard]] m110::Status serve_mode(const tx_protocol::Request& request) noexcept
{
    if (!tx_protocol::mode_token(request.rate, request.interleave))
    {
        return tx_refusal::bad_mode;
    }

    selected_rate = request.rate;
    selected_interleave = request.interleave;
    selected_encoder[0] = 0;
    selected_profile[0] = 0;
    return m110::Status::success();
}

[[nodiscard]] m110::Status serve_select(const tx_protocol::Request& request) noexcept
{
    if (snapshot.state == 1)
    {
        return tx_refusal::upload_in_progress;
    }

    if (!tx_protocol::valid_filename(std::data(request.name), "WAV"))
    {
        return tx_refusal::bad_filename;
    }

    if (const m110::Status cleanup = close_files(); !cleanup.is_ok())
    {
        return cleanup;
    }

    select_output(std::data(request.name));
    selected_encoder[0] = 0;
    selected_profile[0] = 0;
    return m110::Status::success();
}

// Appends an upload's bytes to the payload file, created by the first.
[[nodiscard]] m110::Status append_payload(const tx_protocol::Request& request) noexcept
{
    if (!payload_open)
    {
        if (f_open(&payload_file, std::data(input_path), FA_WRITE | FA_CREATE_NEW) != FR_OK)
        {
            return failed_transaction(tx_refusal::payload_exists_or_io_error);
        }

        payload_open = true;
    }

    UINT actual{};

    if (f_write(&payload_file, std::data(request.data), request.size, &actual) != FR_OK || actual != request.size)
    {
        return failed_transaction(tx_refusal::payload_write_error);
    }

    taskENTER_CRITICAL();
    snapshot.payload_bytes += request.size;
    snapshot.state = 1;
    taskEXIT_CRITICAL();
    return m110::Status::success();
}

[[nodiscard]] m110::Status serve_data(const tx_protocol::Request& request) noexcept
{
    if (snapshot.state != 0 && snapshot.state != 1)
    {
        return tx_refusal::select_new_file;
    }

    if (request.size == 0 || request.size > tx_protocol::maximum_payload || request.size > tx_protocol::maximum_upload_bytes - snapshot.payload_bytes)
    {
        return tx_refusal::payload_too_large;
    }

    if (const m110::Status media = mount(); !media.is_ok())
    {
        return media;
    }

    return append_payload(request);
}

[[nodiscard]] m110::Status serve_send() noexcept
{
    if (snapshot.state != 1)
    {
        return tx_refusal::no_uploaded_payload;
    }

    return failed_transaction(start_generation());
}

[[nodiscard]] m110::Status serve_generate(const tx_protocol::Request& request) noexcept
{
    if (snapshot.state == 1)
    {
        return tx_refusal::upload_in_progress;
    }

    if (!tx_protocol::valid_filename(std::data(request.name), "WAV") || !tx_protocol::valid_filename(std::data(request.input_name), "BIN"))
    {
        return tx_refusal::bad_filename;
    }

    if (const m110::Status cleanup = close_files(); !cleanup.is_ok())
    {
        return cleanup;
    }

    select_output(std::data(request.name));
    make_path(std::data(input_path), std::data(request.input_name));
    selected_rate = request.rate;
    selected_interleave = request.interleave;
    std::strcpy(std::data(selected_encoder), std::data(request.encoder));
    std::strcpy(std::data(selected_profile), std::data(request.profile));
    return failed_transaction(start_generation());
}

// One request: success, or the Status whose message is the reply's code.
// INFO and QUERY change nothing; RESET retries held cleanup; other requests
// are refused while cleanup, playback or a generation is active.
[[nodiscard]] m110::Status serve_request(const tx_protocol::Request& request) noexcept
{
    using tx_protocol::Kind;

    switch (request.kind)
    {
    case Kind::info:
    case Kind::query:
        return m110::Status::success();

    case Kind::reset:
        return serve_reset();

    default:
        break;
    }

    if (cleanup_pending)
    {
        return refusal::busy;
    }

    if (player_live_snapshot().busy || snapshot.state == 2)
    {
        return refusal::busy;
    }

    switch (request.kind)
    {
    case Kind::mode:
        return serve_mode(request);

    case Kind::select:
        return serve_select(request);

    case Kind::data:
        return serve_data(request);

    case Kind::send:
        return serve_send();

    case Kind::generate_file:
        return serve_generate(request);

    default:
        return refusal::bad_request;
    }
}

// The finished job's next block, published once it is complete.
void continue_generation() noexcept
{
    const auto state = writer->step();
    taskENTER_CRITICAL();
    snapshot.frames_written = writer->frames_written();
    taskEXIT_CRITICAL();

    if (state == waveform_source::JobState::failed)
    {
        static_cast<void>(fail(writer->status().message));
    }
    else if (state == waveform_source::JobState::complete)
    {
        if (const m110::Status published = publish_artifact(); !published.is_ok())
        {
            static_cast<void>(fail(published.message));
        }
    }
}
} // namespace

m110::Status submit_tx_request(const tx_protocol::Request& request) noexcept
{
    taskENTER_CRITICAL();

    if (request_pending || response_pending || disconnected)
    {
        taskEXIT_CRITICAL();
        return refusal::busy;
    }

    request_slot = request;
    request_pending = true;
    taskEXIT_CRITICAL();
    notify_player_task();
    return m110::Status::success();
}

bool take_tx_reply(tx_protocol::Reply& reply) noexcept
{
    taskENTER_CRITICAL();

    if (!response_pending)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    reply = response_slot;
    response_pending = false;
    taskEXIT_CRITICAL();
    return true;
}

bool tx_artifact_busy() noexcept
{
    taskENTER_CRITICAL();
    const bool busy = mounted || payload_open || output_open || manifest_open || cleanup_pending || request_pending || processing_request || snapshot.state == 2;
    taskEXIT_CRITICAL();
    return busy;
}

TxArtifactSnapshot tx_artifact_snapshot() noexcept
{
    taskENTER_CRITICAL();
    auto result = snapshot;
    result.rate = selected_rate;
    result.interleave = selected_interleave;
    taskEXIT_CRITICAL();
    return result;
}

void disconnect_tx_artifact() noexcept
{
    disconnected = true;
    notify_player_task();
}

void abort_tx_artifact() noexcept
{
    if (snapshot.state == 1 || snapshot.state == 2)
    {
        static_cast<void>(fail("ABORTED"));
    }
    else if (cleanup_pending)
    {
        static_cast<void>(close_files());
    }
}

void service_tx_artifact() noexcept
{
    if (disconnected)
    {
        taskENTER_CRITICAL();
        disconnected = false;
        request_pending = false;
        response_pending = false;
        taskEXIT_CRITICAL();

        if (snapshot.state == 1)
        {
            static_cast<void>(fail("UPLOAD_DISCONNECTED"));
        }
        else if (cleanup_pending)
        {
            static_cast<void>(close_files());
        }
    }

    if (request_pending)
    {
        taskENTER_CRITICAL();
        const auto request = request_slot;
        request_pending = false;
        processing_request = true;
        taskEXIT_CRITICAL();
        const m110::Status served = serve_request(request);
        taskENTER_CRITICAL();
        response_slot = {served.is_ok(), served.is_ok() ? "none" : served.message, snapshot.payload_bytes, snapshot.frames_written, snapshot.state == 2, snapshot.state == 3};
        response_pending = true;
        processing_request = false;
        taskEXIT_CRITICAL();
    }

    if (snapshot.state == 2)
    {
        continue_generation();
    }
}
} // namespace waveform_generator
