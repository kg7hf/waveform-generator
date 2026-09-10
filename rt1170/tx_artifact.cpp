#include "tx_artifact.hpp"
#include "player.hpp"
#include "platform/waveform_msc.h"
#include "encoder_catalog.hpp"
#include "waveform-source/wav_generator.hpp"

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
}
#include <cstdio>
#include <cstring>
#include <new>

namespace waveform_generator
{
namespace
{
__attribute__((section(".ocram.encoder"))) alignas(32) unsigned char encoder_storage[encoder_workspace_capacity];
__attribute__((section(".ocram.wav_writer"))) alignas(waveform_source::WavGenerator) unsigned char writer_storage[sizeof(waveform_source::WavGenerator)];
waveform_source::Encoder* encoder{};
waveform_source::WavGenerator* writer{};
FATFS fs{};
FIL payload_file{}, output_file{}, manifest_file{};
bool mounted{}, payload_open{}, output_open{};
volatile bool request_pending{}, response_pending{}, disconnected{};
volatile bool processing_request{};
tx_protocol::Request request_slot{};
tx_protocol::Reply response_slot{};
TxArtifactSnapshot snapshot{};
std::uint16_t selected_rate{600};
std::uint8_t selected_interleave{1};
char selected_encoder[16]{"M110B"};
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
class FileSink final : public waveform_source::ByteSink
{
    bool write(const std::uint8_t* bytes, std::size_t count) noexcept override
    {
        if (!output_open)
        {
            if (f_open(&output_file, temporary_path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
            output_open = true;
        }
        UINT actual{};
        return f_write(&output_file, bytes, static_cast<UINT>(count), &actual) == FR_OK && actual == count;
    }
} file_sink;

void close_files() noexcept
{
    if (payload_open) { (void)f_close(&payload_file); payload_open = false; }
    if (output_open) { (void)f_close(&output_file); output_open = false; }
    if (mounted) { (void)f_mount(nullptr, "2:/", 0); mounted = false; }
}
void fail(const char* error) noexcept
{
    if (writer) writer->stop();
    close_files();
    taskENTER_CRITICAL(); snapshot.state = 4; snapshot.error = error; taskEXIT_CRITICAL();
}
bool mount() noexcept
{
    if (!WFG_MediaIsLocal()) return false;
    if (mounted) return true;
    if (f_mount(&fs, "2:/", 1) != FR_OK) return false;
    mounted = true;
    const auto directory = f_mkdir("2:/WG");
    if (directory != FR_OK && directory != FR_EXIST) { close_files(); return false; }
    return true;
}
void make_path(char* path, const char* filename, const char* extension = nullptr) noexcept
{
    std::strcpy(path, "2:/WG/");
    std::strcat(path, filename);
    if (extension) std::strcpy(std::strrchr(path, '.'), extension);
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
bool absent(const char* path) noexcept
{
    FILINFO info{};
    return f_stat(path, &info) == FR_NO_FILE;
}
const char* start_generation() noexcept
{
    descriptor = find_encoder(selected_encoder);
    if (!descriptor) return "UNKNOWN_ENCODER";
    if (!mount()) return "MEDIA_NOT_LOCAL";
    if (!absent(output_path) || !absent(manifest_path) || !absent(temporary_path) || !absent(manifest_temporary_path)) return "OUTPUT_EXISTS";
    if (payload_open)
    {
        const bool good = f_sync(&payload_file) == FR_OK;
        const bool closed = f_close(&payload_file) == FR_OK;
        payload_open = false;
        if (!good || !closed) return "PAYLOAD_WRITE_ERROR";
    }
    if (f_open(&payload_file, input_path, FA_READ) != FR_OK) return "BAD_PAYLOAD_FILE";
    payload_open = true;
    const auto bytes = f_size(&payload_file);
    if (bytes > tx_protocol::maximum_upload_bytes) return "PAYLOAD_TOO_LARGE";
    if (writer) writer->stop();
    if (encoder) encoder->~Encoder();
    encoder = descriptor->construct(encoder_storage, sizeof(encoder_storage));
    if (!encoder) return "ENCODER_WORKSPACE_ERROR";
    if (!writer) writer = new (writer_storage) waveform_source::WavGenerator{};
    const char* interleave = selected_interleave == 0 ? "short" : selected_interleave == 1 ? "long" : "zero";
    if (!selected_profile[0]) std::snprintf(selected_profile, sizeof(selected_profile), "%u:%s", static_cast<unsigned>(selected_rate), interleave);
    const auto status = writer->begin(*encoder, selected_profile, file_bytes, static_cast<std::size_t>(bytes), file_sink);
    if (!status.is_ok()) return status.message;
    taskENTER_CRITICAL();
    snapshot.state = 2;
    snapshot.payload_bytes = static_cast<std::uint32_t>(bytes);
    snapshot.frames_written = 0;
    snapshot.total_frames = writer->total_frames();
    snapshot.error = "none";
    taskEXIT_CRITICAL();
    return nullptr;
}

bool publish_artifact() noexcept
{
    if (f_sync(&output_file) != FR_OK) return false;
    const bool output_closed = f_close(&output_file) == FR_OK;
    output_open = false;
    const bool payload_closed = f_close(&payload_file) == FR_OK;
    payload_open = false;
    if (!output_closed || !payload_closed) return false;
    const auto hashes = writer->hashes();
    char metadata[1536]{};
    const int count = std::snprintf(metadata, sizeof(metadata),
        "{\n\"schema\":\"waveform-artifact/1\",\"encoder\":\"%s\",\"generator\":\"%s\","
        "\"source_manifest_sha256\":\"%s\",\"profile\":\"%s\",\"sample_rate_hz\":48000,"
        "\"channels\":1,\"bits_per_sample\":24,\"frames\":%llu,\"trailing_silence_frames\":48000,"
        "\"payload\":{\"path\":\"%s\",\"bytes\":%lu,\"sha256\":\"%s\"},"
        "\"wav\":{\"path\":\"%s\",\"sha256\":\"%s\",\"pcm_sha256\":\"%s\"},\"complete\":true}\n",
        descriptor->id, descriptor->version, WFG_SOURCE_MANIFEST_SHA256, selected_profile,
        static_cast<unsigned long long>(snapshot.total_frames), input_path + 6,
        static_cast<unsigned long>(snapshot.payload_bytes), hashes.payload, snapshot.filename, hashes.wav, hashes.pcm);
    if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(metadata)) return false;
    if (f_open(&manifest_file, manifest_temporary_path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    UINT actual{};
    const bool wrote = f_write(&manifest_file, metadata, static_cast<UINT>(count), &actual) == FR_OK && actual == static_cast<UINT>(count);
    const bool synced = f_sync(&manifest_file) == FR_OK;
    const bool closed = f_close(&manifest_file) == FR_OK;
    if (!wrote || !synced || !closed) return false;
    if (f_rename(temporary_path, output_path) != FR_OK) return false;
    if (f_rename(manifest_temporary_path, manifest_path) != FR_OK)
    {
        // Metadata is the commit marker. Return the WAV to its temporary name
        // if publishing that marker fails; report failure even if rollback fails.
        (void)f_rename(output_path, temporary_path);
        return false;
    }
    taskENTER_CRITICAL();
    std::strcpy(snapshot.wav_sha256, hashes.wav);
    snapshot.state = 3;
    taskEXIT_CRITICAL();
    close_files();
    return true;
}
}

bool submit_tx_request(const tx_protocol::Request& request) noexcept
{
    taskENTER_CRITICAL();
    if (request_pending || response_pending || disconnected) { taskEXIT_CRITICAL(); return false; }
    request_slot = request;
    request_pending = true;
    taskEXIT_CRITICAL();
    notify_player_task();
    return true;
}
bool take_tx_reply(tx_protocol::Reply& reply) noexcept
{
    taskENTER_CRITICAL();
    if (!response_pending) { taskEXIT_CRITICAL(); return false; }
    reply = response_slot;
    response_pending = false;
    taskEXIT_CRITICAL();
    return true;
}
bool tx_artifact_busy() noexcept
{
    taskENTER_CRITICAL();
    const bool busy = mounted || request_pending || processing_request || snapshot.state == 2;
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
    if (snapshot.state == 1 || snapshot.state == 2) fail("ABORTED");
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
        if (snapshot.state == 1) fail("UPLOAD_DISCONNECTED");
    }
    if (request_pending)
    {
        taskENTER_CRITICAL();
        const auto request = request_slot;
        request_pending = false;
        processing_request = true;
        taskEXIT_CRITICAL();
        const char* error = nullptr;
        bool transaction_error = false;
        using tx_protocol::Kind;
        if (request.kind == Kind::info || request.kind == Kind::query) { }
        else if (request.kind == Kind::reset)
        {
            if (snapshot.state == 1 || snapshot.state == 2) abort_tx_artifact();
            else close_files();
            selected_rate = 600; selected_interleave = 1;
            std::strcpy(selected_encoder, "M110B"); selected_profile[0] = 0;
            select_output("GEN.WAV");
        }
        else if (player_live_snapshot().busy || snapshot.state == 2) error = "BUSY";
        else if (request.kind == Kind::mode)
        {
            if (!tx_protocol::mode_token(request.rate, request.interleave)) error = "BAD_MODE";
            else { selected_rate = request.rate; selected_interleave = request.interleave; std::strcpy(selected_encoder, "M110B"); selected_profile[0] = 0; }
        }
        else if (request.kind == Kind::select)
        {
            if (snapshot.state == 1) error = "UPLOAD_IN_PROGRESS";
            else if (!tx_protocol::valid_filename(request.name, "WAV")) error = "BAD_FILENAME";
            else { close_files(); select_output(request.name); std::strcpy(selected_encoder, "M110B"); selected_profile[0] = 0; }
        }
        else if (request.kind == Kind::data)
        {
            if (snapshot.state != 0 && snapshot.state != 1) error = "SELECT_NEW_FILE";
            else if (request.size == 0 || request.size > tx_protocol::maximum_payload ||
                     request.size > tx_protocol::maximum_upload_bytes - snapshot.payload_bytes) error = "PAYLOAD_TOO_LARGE";
            else if (!mount()) error = "MEDIA_NOT_LOCAL";
            else
            {
                if (!payload_open)
                {
                    if (f_open(&payload_file, input_path, FA_WRITE | FA_CREATE_NEW) != FR_OK) { error = "PAYLOAD_EXISTS_OR_IO_ERROR"; transaction_error = true; }
                    else payload_open = true;
                }
                if (!error)
                {
                    UINT actual{};
                    if (f_write(&payload_file, request.data, request.size, &actual) != FR_OK || actual != request.size) { error = "PAYLOAD_WRITE_ERROR"; transaction_error = true; }
                    else
                    {
                        taskENTER_CRITICAL();
                        snapshot.payload_bytes += request.size;
                        snapshot.state = 1;
                        taskEXIT_CRITICAL();
                    }
                }
            }
        }
        else if (request.kind == Kind::send)
        {
            if (snapshot.state != 1) error = "NO_UPLOADED_PAYLOAD";
            else { error = start_generation(); transaction_error = error != nullptr; }
        }
        else if (request.kind == Kind::generate_file)
        {
            if (snapshot.state == 1) error = "UPLOAD_IN_PROGRESS";
            else if (!tx_protocol::valid_filename(request.name, "WAV") || !tx_protocol::valid_filename(request.input_name, "BIN")) error = "BAD_FILENAME";
            else
            {
                close_files(); select_output(request.name);
                make_path(input_path, request.input_name);
                selected_rate = request.rate; selected_interleave = request.interleave;
                std::strcpy(selected_encoder, request.encoder);
                std::strcpy(selected_profile, request.profile);
                error = start_generation();
                transaction_error = error != nullptr;
            }
        }
        else error = "BAD_REQUEST";
        if (transaction_error) fail(error);
        taskENTER_CRITICAL();
        response_slot = {!error, error ? error : "none", snapshot.payload_bytes, snapshot.frames_written, snapshot.state == 2, snapshot.state == 3};
        response_pending = true;
        processing_request = false;
        taskEXIT_CRITICAL();
    }
    if (snapshot.state == 2)
    {
        const auto state = writer->step();
        taskENTER_CRITICAL(); snapshot.frames_written = writer->frames_written(); taskEXIT_CRITICAL();
        if (state == waveform_source::JobState::failed) fail(writer->status().message);
        else if (state == waveform_source::JobState::complete && !publish_artifact()) fail("ARTIFACT_FINALIZE_ERROR");
    }
}
}
