// Exercise the actual firmware artifact owner with an in-memory FatFs model.
// File visibility, bounded work, ownership, and injected I/O failures are tested;
// SD hardware behavior, power-loss atomicity, and RTOS scheduling are not.
#include "rt1170/tx_artifact.hpp"
#include "rt1170/player.hpp"
#include "waveform-source/sha256.hpp"
#include "ff.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
using waveform_generator::tx_protocol::Kind;
using waveform_generator::tx_protocol::Reply;
using waveform_generator::tx_protocol::Request;
using namespace waveform_generator;
struct File { std::string path; std::vector<std::uint8_t> bytes; };
std::map<std::string, File> files;
bool local = true, mounted{}, directory{}, player_busy{};
unsigned int open_files{}, notifications{}, rename_calls{};
int failures{};
std::string failed_write_path, short_write_path, failed_sync_path, failed_read_path, failed_rename_destination;
bool failed_mount{}, failed_mkdir{};

void check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
bool exists(const char* name) { return files.contains(std::string("2:/WG/") + name); }
std::vector<std::uint8_t>& bytes(const char* name) { return files.at(std::string("2:/WG/") + name).bytes; }
void put(const char* name, std::vector<std::uint8_t> data)
{
    const auto path = std::string("2:/WG/") + name;
    files[path] = {path, std::move(data)};
}
Reply command(const Request& request)
{
    check(submit_tx_request(request), "TX mailbox accepts request");
    service_tx_artifact();
    Reply reply;
    check(take_tx_reply(reply), "TX owner publishes reply");
    return reply;
}
Reply command(Kind kind) { Request request; request.kind = kind; return command(request); }
void fresh()
{
    failed_write_path.clear(); short_write_path.clear(); failed_sync_path.clear(); failed_read_path.clear(); failed_rename_destination.clear();
    failed_mount = failed_mkdir = player_busy = false; local = true;
    disconnect_tx_artifact(); service_tx_artifact();
    check(command(Kind::reset).ok, "RESET is accepted");
    check(!mounted && open_files == 0U && !tx_artifact_busy(), "RESET releases all files and media ownership");
    files.clear(); directory = false; rename_calls = 0U;
}
Request generation(const char* output = "GEN.WAV", const char* input = "PAY.BIN", const char* profile = "4800:zero")
{
    Request request; request.kind = Kind::generate_file;
    std::strcpy(request.name, output); std::strcpy(request.input_name, input); std::strcpy(request.profile, profile);
    return request;
}
Reply upload(const std::uint8_t* input, std::size_t count)
{
    Request request; request.kind = Kind::data; request.size = static_cast<std::uint16_t>(count);
    std::memcpy(request.data, input, count);
    return command(request);
}
void finish(const char* output)
{
    unsigned int steps{};
    while (tx_artifact_snapshot().state == 2U && ++steps < 10000U)
    {
        check(!exists(output), "no final WAV is published while generation is incomplete");
        const auto before = tx_artifact_snapshot().frames_written;
        service_tx_artifact();
        const auto after = tx_artifact_snapshot().frames_written;
        check(after >= before && after - before <= 2048U, "one owner service call writes at most 2048 frames");
    }
    check(steps < 10000U, "generation reaches a terminal state in a bounded number of service calls");
}
std::uint32_t little32(const std::vector<std::uint8_t>& data, std::size_t at)
{
    return data[at] | (std::uint32_t{data[at + 1U]} << 8U) | (std::uint32_t{data[at + 2U]} << 16U) | (std::uint32_t{data[at + 3U]} << 24U);
}
std::string sha(const std::uint8_t* data, std::size_t size)
{
    waveform_source::Sha256 hash; hash.update(data, size); return hash.hex();
}
void verify_artifact(const char* output, const char* manifest, const char* payload)
{
    const auto snapshot = tx_artifact_snapshot();
    check(snapshot.state == 3U && snapshot.frames_written == snapshot.total_frames, "completed artifact accounts for every declared frame");
    check(!mounted && open_files == 0U && !tx_artifact_busy(), "complete artifact releases files and media ownership");
    check(exists(output) && exists(manifest) && exists(payload), "complete WAV, payload, and manifest are retained");
    if (!exists(output) || !exists(manifest) || !exists(payload)) return;
    const auto& wav = bytes(output);
    check(wav.size() == 44U + snapshot.total_frames * 3U && wav.size() >= 144044U, "WAV contains declared PCM24 and one-second tail");
    if (wav.size() < 144044U) return;
    check(std::memcmp(wav.data(), "RIFF", 4U) == 0 && std::memcmp(wav.data() + 8U, "WAVEfmt ", 8U) == 0 &&
          std::memcmp(wav.data() + 36U, "data", 4U) == 0 && little32(wav, 4U) == wav.size() - 8U &&
          little32(wav, 24U) == 48000U && little32(wav, 40U) == wav.size() - 44U,
          "published WAV header matches mono PCM24 48 kHz artifact length");
    check(wav[20U] == 1U && wav[22U] == 1U && wav[34U] == 24U, "WAV uses mono linear PCM24");
    check(std::all_of(wav.end() - 144000, wav.end(), [](std::uint8_t value) { return value == 0U; }), "published WAV has exactly the required silent tail region");
    check(std::any_of(wav.begin() + 44, wav.end() - 144000, [](std::uint8_t value) { return value != 0U; }), "generated M110 waveform contains nonzero signal");
    const auto& source = bytes(payload);
    const auto& encoded = bytes(manifest);
    const std::string metadata(encoded.begin(), encoded.end());
    const auto wav_sha = sha(wav.data(), wav.size());
    const auto payload_sha = sha(source.data(), source.size());
    const auto pcm_sha = sha(wav.data() + 44U, wav.size() - 44U);
    check(metadata.find("\"schema\":\"waveform-artifact/1\"") != std::string::npos &&
          metadata.find("\"encoder\":\"M110B\"") != std::string::npos &&
          metadata.find("\"complete\":true") != std::string::npos, "manifest identifies encoder and committed schema");
    check(metadata.find("\"source_manifest_sha256\":\"" WFG_SOURCE_MANIFEST_SHA256 "\"") != std::string::npos,
          "manifest retains the firmware source identity");
    check(metadata.find("\"path\":\"" + std::string(payload) + "\"") != std::string::npos &&
          metadata.find("\"bytes\":" + std::to_string(source.size())) != std::string::npos &&
          metadata.find("\"sha256\":\"" + payload_sha + "\"") != std::string::npos, "manifest payload identity matches uploaded bytes");
    check(metadata.find("\"sha256\":\"" + wav_sha + "\"") != std::string::npos &&
          metadata.find("\"pcm_sha256\":\"" + pcm_sha + "\"") != std::string::npos &&
          wav_sha == snapshot.wav_sha256, "manifest and STATUS hashes match the actual published WAV and PCM");
}

void test_upload_and_publish()
{
    fresh();
    Request request; request.kind = Kind::select; std::strcpy(request.name, "UPLOAD.WAV");
    check(command(request).ok, "select upload artifact name");
    request = {}; request.kind = Kind::mode; request.rate = 4800U; request.interleave = 2U;
    check(command(request).ok, "set legacy 4800U profile");
    std::vector<std::uint8_t> payload(517U);
    for (std::size_t i = 0U; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 131U);
    for (std::size_t at = 0U; at < payload.size(); at += 256U)
    {
        const auto count = std::min<std::size_t>(256U, payload.size() - at);
        const auto reply = upload(payload.data() + at, count);
        check(reply.ok && reply.payload_bytes == at + count, "binary chunks append exactly, including NUL and high bytes");
    }
    check(bytes("UPLOAD.BIN") == payload && tx_artifact_snapshot().state == 1U, "retained upload is byte exact");
    check(tx_artifact_busy() && !exists("UPLOAD.WAV") && !exists("UPLOAD.TMP"), "receiving upload owns media but has not begun waveform output");
    tx_protocol::CommandParser parser;
    for (const auto ch : std::string("CMD:SENDBUFFER")) parser.feed(static_cast<std::uint8_t>(ch));
    check(parser.ready() && parser.valid() && parser.request().kind == Kind::send, "production SENDBUFFER command starts artifact generation");
    const auto reply = command(parser.request());
    check(reply.ok && reply.generating && tx_artifact_snapshot().frames_written <= 2048U,
          "SENDBUFFER acknowledges an incremental generation job");
    check(exists("UPLOAD.TMP") && !exists("UPLOAD.WAV") && !exists("UPLOAD.JSON"), "only temporary output exists before completion");
    finish("UPLOAD.WAV");
    verify_artifact("UPLOAD.WAV", "UPLOAD.JSON", "UPLOAD.BIN");
    check(!exists("UPLOAD.TMP") && !exists("UPLOAD.JMP") && rename_calls == 2U, "successful publish renames both temporary artifacts");
}

void test_existing_payload_and_rejections()
{
    fresh(); put("PAY.BIN", {'a','b','c'});
    auto request = generation("FROMBIN.WAV", "PAY.BIN", "600:short");
    check(command(request).ok, "generate_file accepts an existing binary payload");
    finish("FROMBIN.WAV"); verify_artifact("FROMBIN.WAV", "FROMBIN.JSON", "PAY.BIN");
    const auto& metadata_bytes = bytes("FROMBIN.JSON");
    const std::string metadata(metadata_bytes.begin(), metadata_bytes.end());
    check(metadata.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != std::string::npos,
          "payload SHA matches the independent standard abc vector");
    const auto preserved_wav = bytes("FROMBIN.WAV"), preserved_manifest = bytes("FROMBIN.JSON");
    const auto duplicate = command(request);
    check(!duplicate.ok && std::strcmp(duplicate.error, "OUTPUT_EXISTS") == 0 &&
          bytes("FROMBIN.WAV") == preserved_wav && bytes("FROMBIN.JSON") == preserved_manifest && !exists("FROMBIN.TMP"),
          "existing committed output is preserved without opening a temporary file");

    fresh(); put("PAY.BIN", {'a','b','c'});
    request = generation("INVALID.WAV", "PAY.BIN", "bogus");
    check(!command(request).ok && !exists("INVALID.TMP") && !exists("INVALID.WAV") && open_files == 0U,
          "invalid encoder profile is rejected before writing even a WAV header");
    request = generation(); std::strcpy(request.encoder, "MISSING");
    const auto unknown = command(request);
    check(!unknown.ok && std::strcmp(unknown.error, "UNKNOWN_ENCODER") == 0 && !exists("GEN.TMP"), "unregistered encoders fail before output creation");
    request = generation(); std::strcpy(request.name, "../X.WAV");
    check(!command(request).ok && files.size() == 1U, "invalid output path cannot create an artifact");

    fresh(); put("PAY.BIN", {'a'}); put("GEN.JMP", {'s','t','a','l','e'});
    const auto stale = command(generation());
    check(!stale.ok && std::strcmp(stale.error, "OUTPUT_EXISTS") == 0 && !exists("GEN.TMP"), "uncommitted files are preserved for recovery rather than overwritten");
}

void test_reset_disconnect_and_busy()
{
    fresh(); const std::uint8_t payload[]{0U, 0xFFU, 0x55U};
    check(upload(payload, sizeof(payload)).ok, "partial upload starts");
    disconnect_tx_artifact(); service_tx_artifact();
    check(tx_artifact_snapshot().state == 4U && std::strcmp(tx_artifact_snapshot().error, "UPLOAD_DISCONNECTED") == 0 &&
          !tx_artifact_busy() && open_files == 0U && exists("GEN.BIN") && !exists("GEN.TMP"), "disconnect aborts partial upload, releases media, and retains diagnostic bytes");
    check(!command(Kind::send).ok, "aborted upload cannot later be sent as complete");

    fresh(); put("PAY.BIN", {'a','b','c'});
    check(command(generation()).ok, "generation starts before disconnect");
    const auto before = tx_artifact_snapshot().frames_written;
    disconnect_tx_artifact(); service_tx_artifact();
    check(tx_artifact_snapshot().state == 2U && tx_artifact_snapshot().frames_written > before, "disconnect leaves an acknowledged generation running");
    finish("GEN.WAV"); verify_artifact("GEN.WAV", "GEN.JSON", "PAY.BIN");

    fresh(); put("PAY.BIN", {'a'});
    check(command(generation()).ok && tx_artifact_busy(), "generation reserves media");
    const auto busy = command(Kind::send);
    check(!busy.ok && std::strcmp(busy.error, "BUSY") == 0 && tx_artifact_snapshot().state == 2U, "additional TX mutations do not interrupt an active generation");
    check(command(Kind::reset).ok && tx_artifact_snapshot().state == 0U && !tx_artifact_busy() && open_files == 0U,
          "RESET aborts generation and releases every resource");
    service_tx_artifact();
    check(exists("GEN.TMP") && !exists("GEN.WAV") && !exists("GEN.JSON"), "aborted generation never publishes incomplete output");

    fresh(); local = false;
    const auto host = upload(payload, sizeof(payload));
    check(!host.ok && std::strcmp(host.error, "MEDIA_NOT_LOCAL") == 0 && files.empty(), "host-owned media prevents TX writes");
    local = true; player_busy = true;
    const auto playing = upload(payload, sizeof(payload));
    check(!playing.ok && std::strcmp(playing.error, "BUSY") == 0 && files.empty(), "active playback prevents TX filesystem access");
    player_busy = false; failed_mount = true;
    check(!upload(payload, sizeof(payload)).ok && files.empty() && !tx_artifact_busy(), "mount failure does not acquire media");
    failed_mount = false; failed_mkdir = true;
    check(!upload(payload, sizeof(payload)).ok && files.empty() && !mounted, "directory creation failure releases mounted media");
}

void test_io_failures()
{
    fresh(); const std::uint8_t payload[]{'a','b','c'};
    short_write_path = "2:/WG/GEN.BIN";
    const auto upload_reply = upload(payload, sizeof(payload));
    check(!upload_reply.ok && std::strcmp(upload_reply.error, "PAYLOAD_WRITE_ERROR") == 0 &&
          tx_artifact_snapshot().payload_bytes == 0U && open_files == 0U && !exists("GEN.WAV"), "short successful FatFs upload writes are rejected and not acknowledged as consumed");

    fresh(); put("PAY.BIN", {'a','b','c'}); failed_write_path = "2:/WG/GEN.TMP";
    check(!command(generation()).ok && tx_artifact_snapshot().state == 4U && open_files == 0U && !exists("GEN.WAV"), "WAV header write errors abort before publication");

    fresh(); put("PAY.BIN", {'a','b','c'});
    check(command(generation()).ok, "generation starts before PCM failure");
    short_write_path = "2:/WG/GEN.TMP";
    service_tx_artifact();
    check(tx_artifact_snapshot().state == 4U && open_files == 0U && !exists("GEN.WAV") && !exists("GEN.JSON"), "short PCM write aborts and retains only temporary output");

    fresh(); put("PAY.BIN", {'a','b','c'}); failed_read_path = "2:/WG/PAY.BIN";
    (void)command(generation()); finish("GEN.WAV");
    check(tx_artifact_snapshot().state == 4U && !exists("GEN.WAV") && !exists("GEN.JSON") && open_files == 0U,
          "payload read failure cannot be published as successful generation");

    for (const auto* fault : {"sync", "manifest", "first_rename", "second_rename"})
    {
        fresh(); put("PAY.BIN", {'a','b','c'});
        check(command(generation()).ok, "generation starts before injected finalization failure");
        if (std::strcmp(fault, "sync") == 0) failed_sync_path = "2:/WG/GEN.TMP";
        else if (std::strcmp(fault, "manifest") == 0) short_write_path = "2:/WG/GEN.JMP";
        else failed_rename_destination = std::strcmp(fault, "first_rename") == 0 ? "2:/WG/GEN.WAV" : "2:/WG/GEN.JSON";
        finish("GEN.WAV");
        check(tx_artifact_snapshot().state == 4U && std::strcmp(tx_artifact_snapshot().error, "ARTIFACT_FINALIZE_ERROR") == 0 &&
              !exists("GEN.WAV") && !exists("GEN.JSON") && exists("GEN.TMP") && !mounted && open_files == 0U,
              "failed finalization releases resources and leaves no committed artifact pair");
        if (std::strcmp(fault, "second_rename") == 0) check(rename_calls == 3U && exists("GEN.JMP"), "failed metadata publish rolls the WAV name back to TMP");
    }
}
} // namespace

extern "C"
{
FRESULT f_mount(FATFS* filesystem, const char*, unsigned char)
{
    if (filesystem && failed_mount) return FR_DISK_ERR;
    mounted = filesystem != nullptr; return FR_OK;
}
FRESULT f_mkdir(const char*) { if (failed_mkdir) return FR_DISK_ERR; const bool existed = directory; directory = true; return existed ? FR_EXIST : FR_OK; }
FRESULT f_open(FIL* file, const char* path, unsigned char mode)
{
    if (!mounted) return FR_DISK_ERR;
    auto found = files.find(path);
    if ((mode & FA_CREATE_NEW) != 0U)
    {
        if (found != files.end()) return FR_EXIST;
        found = files.emplace(path, File{path, {}}).first;
    }
    if (found == files.end()) return FR_NO_FILE;
    file->context = &found->second; file->mode = mode; file->position = 0U;
    file->data = found->second.bytes.data(); file->size = static_cast<FSIZE_t>(found->second.bytes.size());
    file->opened = 1; ++open_files; return FR_OK;
}
FRESULT f_close(FIL* file)
{
    if (!file->opened) return FR_DISK_ERR;
    file->opened = 0; --open_files; return FR_OK;
}
FRESULT f_read(FIL* file, void* data, UINT requested, UINT* actual)
{
    *actual = 0U;
    if (!file->opened || (file->mode & FA_READ) == 0U) return FR_DISK_ERR;
    const auto& item = *static_cast<File*>(file->context);
    if (item.path == failed_read_path) return FR_DISK_ERR;
    *actual = std::min<UINT>(requested, file->size - file->position);
    std::memcpy(data, item.bytes.data() + file->position, *actual); file->position += *actual; return FR_OK;
}
FRESULT f_write(FIL* file, const void* data, UINT requested, UINT* actual)
{
    *actual = 0U;
    if (!file->opened || (file->mode & FA_WRITE) == 0U) return FR_DISK_ERR;
    auto& item = *static_cast<File*>(file->context);
    if (item.path == failed_write_path) return FR_DISK_ERR;
    *actual = item.path == short_write_path && requested ? requested - 1U : requested;
    item.bytes.resize(file->position + *actual);
    std::memcpy(item.bytes.data() + file->position, data, *actual);
    file->position += *actual; file->size = static_cast<FSIZE_t>(item.bytes.size()); return FR_OK;
}
FRESULT f_sync(FIL* file)
{
    if (!file->opened) return FR_DISK_ERR;
    return static_cast<File*>(file->context)->path == failed_sync_path ? FR_DISK_ERR : FR_OK;
}
FRESULT f_lseek(FIL* file, FSIZE_t offset)
{
    if (!file->opened || offset > file->size) return FR_DISK_ERR;
    file->position = offset; return FR_OK;
}
FRESULT f_stat(const char* path, FILINFO* info)
{
    if (!mounted) return FR_DISK_ERR;
    const auto found = files.find(path);
    if (found == files.end()) return FR_NO_FILE;
    info->fsize = static_cast<FSIZE_t>(found->second.bytes.size()); return FR_OK;
}
FRESULT f_rename(const char* from, const char* to)
{
    ++rename_calls;
    if (!mounted || to == failed_rename_destination) return FR_DISK_ERR;
    if (files.contains(to)) return FR_EXIST;
    auto node = files.extract(from);
    if (node.empty()) return FR_NO_FILE;
    node.key() = to; node.mapped().path = to; files.insert(std::move(node)); return FR_OK;
}
bool WFG_MediaIsLocal() { return local; }
}
namespace waveform_generator
{
LiveSnapshot player_live_snapshot() noexcept { LiveSnapshot result; result.busy = player_busy; return result; }
void notify_player_task() noexcept { ++notifications; }
}

int main()
{
    test_upload_and_publish();
    test_existing_payload_and_rejections();
    test_reset_disconnect_and_busy();
    test_io_failures();
    fresh();
    check(notifications != 0U, "mailbox and disconnect wake the owner task");
    if (failures == 0) { std::puts("TX artifact owner tests: PASS"); return 0; }
    std::printf("TX artifact owner tests: %d failures\n", failures); return 1;
}
