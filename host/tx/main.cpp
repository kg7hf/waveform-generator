// Positional interface and PCM quantization preserve the donor tx_to_pcm tool.
// This local generator additionally retains payload + manifest beside the WAV.
#include "native-m110/source.hpp"
#include "waveform-source/wav_generator.hpp"
#include "host/tx/sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#ifndef WFG_TX_BUILD_ID
#define WFG_TX_BUILD_ID "unidentified-build"
#endif
#ifndef WFG_TX_SOURCE_MANIFEST_SHA256
#define WFG_TX_SOURCE_MANIFEST_SHA256 "unidentified-source"
#endif

namespace
{
namespace fs = std::filesystem;

std::string json_string(std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const unsigned char byte : value)
    {
        if (byte == '"' || byte == '\\') { result += '\\'; result += static_cast<char>(byte); }
        else if (byte < 32U) { result += "\\u00"; result += hex[byte >> 4U]; result += hex[byte & 15U]; }
        else { result += static_cast<char>(byte); }
    }
    return result + '"';
}

bool same_file(const fs::path& first, const fs::path& second)
{
    std::error_code error;
    return fs::equivalent(first, second, error) && !error;
}

struct Files
{
    std::FILE* input{};
    std::FILE* output{};
    std::FILE* payload{};
    std::FILE* manifest{};
    std::array<fs::path, 3U> temporary{};
    ~Files()
    {
        if (input != nullptr) { std::fclose(input); }
        if (output != nullptr) { std::fclose(output); }
        if (payload != nullptr) { std::fclose(payload); }
        if (manifest != nullptr) { std::fclose(manifest); }
        for (const auto& path : temporary) { if (!path.empty()) { std::error_code error; fs::remove(path, error); } }
    }
};

bool close_file(std::FILE*& file)
{
    if (file == nullptr) { return true; }
    const auto result = std::fclose(file);
    file = nullptr;
    return result == 0;
}

class Payload final : public native_m110::ByteSource
{
public:
    std::FILE* input{};
    std::FILE* retained{};
    std::string_view text{};
    std::size_t cursor{};
    wfg_tx::Sha256 digest{};
    bool failed{};
    std::size_t read(std::uint8_t* bytes, std::size_t capacity) noexcept override
    {
        std::size_t count{};
        if (input != nullptr)
        {
            count = std::fread(bytes, 1U, capacity, input);
            if (std::ferror(input) != 0) { failed = true; return 0U; }
        }
        else
        {
            count = std::min(capacity, text.size() - cursor);
            if (count != 0U) { std::memcpy(bytes, text.data() + cursor, count); }
            cursor += count;
        }
        if (retained != nullptr && std::fwrite(bytes, 1U, count, retained) != count) { failed = true; return 0U; }
        digest.update(bytes, count);
        return count;
    }
};

void put16(std::uint8_t* bytes, std::uint16_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8U);
}
class FileSink final : public waveform_source::ByteSink
{
public:
    explicit FileSink(std::FILE* file) : file_{file} {}
    bool write(const std::uint8_t* bytes, std::size_t count) noexcept override { return std::fwrite(bytes, 1U, count, file_) == count; }
private:
    std::FILE* file_{};
};

bool publish(const fs::path& temporary, const fs::path& destination)
{
    std::error_code error;
    fs::remove(destination, error);
    if (error) { return false; }
    fs::rename(temporary, destination, error);
    return !error;
}

int generate(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: wfg_m110_tx <75|150|300|600|1200|2400|4800> <short|long|zero> <output.wav|output.pcm> <message...>\n"
                             "       wfg_m110_tx <rate> <interleave> <output.wav|output.pcm> --file <binary-payload>\n");
        return 2;
    }
    std::uint32_t rate{};
    const std::string_view rate_text{argv[1]};
    const auto parsed = std::from_chars(rate_text.data(), rate_text.data() + rate_text.size(), rate);
    m110::BodyMode mode{};
    if (parsed.ec != std::errc{} || parsed.ptr != rate_text.data() + rate_text.size() || !native_m110::parse_mode(rate, argv[2], mode))
    {
        std::fprintf(stderr, "invalid rate/interleave combination\n"); return 2;
    }
    const fs::path output_path = fs::absolute(argv[3]);
    auto payload_path = output_path; payload_path.replace_extension(".BIN");
    auto manifest_path = output_path; manifest_path.replace_extension(".JSON");
    const auto extension = output_path.extension().string();
    const bool wav = extension == ".wav" || extension == ".WAV";
    if (!wav && extension != ".pcm" && extension != ".PCM")
    {
        std::fprintf(stderr, "output extension must be .wav or .pcm\n"); return 2;
    }
    Files files;
    Payload payload;
    std::string text;
    std::size_t payload_bytes{};
    fs::path original_payload;
    const bool file_input = std::string_view{argv[4]} == "--file";
    if (file_input)
    {
        if (argc != 6) { std::fprintf(stderr, "--file requires exactly one binary payload path\n"); return 2; }
        original_payload = fs::absolute(argv[5]);
        std::error_code error;
        const auto bytes = fs::file_size(original_payload, error);
        if (error || bytes == 0U || bytes > std::numeric_limits<std::size_t>::max())
        {
            std::fprintf(stderr, "binary payload must be non-empty and addressable\n"); return 2;
        }
        if (same_file(original_payload, output_path) || same_file(original_payload, manifest_path))
        {
            std::fprintf(stderr, "output WAV/manifest must not overwrite the input payload\n"); return 2;
        }
        payload_bytes = static_cast<std::size_t>(bytes);
        files.input = std::fopen(original_payload.string().c_str(), "rb");
        if (files.input == nullptr) { std::fprintf(stderr, "cannot open input payload\n"); return 2; }
        payload.input = files.input;
    }
    else
    {
        for (int index = 4; index < argc; ++index)
        {
            if (!text.empty()) { text += ' '; }
            text += argv[index];
        }
        payload.text = text;
        payload_bytes = text.size();
    }

    // On the host the bounded source is static to keep its workspace off the stack.
    static native_m110::Source source;
    const auto configured = source.configure(mode, payload, payload_bytes, wav ? 48000U : 0U);
    if (!configured.is_ok()) { std::fprintf(stderr, "cannot plan transmission: %s\n", configured.message); return 2; }
    if (source.total_frames() > (std::numeric_limits<std::uint32_t>::max() - 36U) / 2U)
    {
        std::fprintf(stderr, "output exceeds the supported RIFF/PCM artifact size\n"); return 2;
    }
    const bool reuse_payload = file_input && same_file(original_payload, payload_path);
    files.temporary = {fs::path{output_path.string() + ".partial"},
                       reuse_payload ? fs::path{} : fs::path{payload_path.string() + ".partial"},
                       fs::path{manifest_path.string() + ".partial"}};
    for (const auto& path : files.temporary)
    {
        if (file_input && !path.empty() && same_file(original_payload, path))
        {
            // Do not let the cleanup destructor remove this caller-owned input.
            files.temporary = {};
            std::fprintf(stderr, "temporary artifact path aliases the input payload\n"); return 2;
        }
    }
    files.output = std::fopen(files.temporary[0].string().c_str(), "wb");
    if (!reuse_payload) { files.payload = std::fopen(files.temporary[1].string().c_str(), "wb"); }
    if (files.output == nullptr || (!reuse_payload && files.payload == nullptr))
    {
        std::fprintf(stderr, "cannot create output artifacts\n"); return 1;
    }
    payload.retained = files.payload;
    const auto expected_frames = source.total_frames();
    const auto data_bytes = static_cast<std::uint32_t>(expected_frames * 2U);
    std::string output_hash, pcm_hash;
    std::uint64_t written{};
    if (wav)
    {
        static waveform_source::WavGenerator job;
        FileSink sink{files.output};
        const auto profile = std::to_string(rate) + ':' + argv[2];
        const auto status = job.begin(source, profile, payload, payload_bytes, sink);
        if (!status.is_ok()) { std::fprintf(stderr, "failed to begin WAV: %s\n", status.message); return 1; }
        while (job.state() == waveform_source::JobState::running) { (void)job.step(); }
        if (job.state() != waveform_source::JobState::complete || payload.failed)
        {
            std::fprintf(stderr, "native artifact generation failed: %s\n", job.status().message); return 1;
        }
        written = job.frames_written();
        const auto hashes = job.hashes();
        output_hash = hashes.wav;
        pcm_hash = hashes.pcm;
    }
    else
    {
        std::array<std::int16_t, 2048U> frames{};
        std::array<std::uint8_t, 4096U> bytes{};
        wfg_tx::Sha256 digest;
        for (;;)
        {
            const auto count = source.read(frames.data(), frames.size());
            if (!source.status().is_ok() || payload.failed)
            {
                std::fprintf(stderr, "native generation failed: %s\n", source.status().message); return 1;
            }
            if (count == 0U) { break; }
            for (std::size_t index = 0U; index < count; ++index) { put16(bytes.data() + 2U * index, static_cast<std::uint16_t>(frames[index])); }
            if (std::fwrite(bytes.data(), 2U, count, files.output) != count) { std::fprintf(stderr, "failed to write PCM\n"); return 1; }
            digest.update(bytes.data(), 2U * count); written += count;
        }
        output_hash = pcm_hash = digest.hex();
    }
    if (written != expected_frames || !close_file(files.input) || !close_file(files.output) || !close_file(files.payload))
    {
        std::fprintf(stderr, "failed to finish complete artifacts\n"); return 1;
    }
    const auto mode_name = std::to_string(rate) + (mode.interleave == m110::BodyInterleave::long_block ? "L" : mode.interleave == m110::BodyInterleave::short_block ? "S" : "Z");
    const auto& plan = source.plan();
    const auto manifest = std::string{"{\n  \"schema\": \"wfg.native-m110-artifact/1\",\n  \"test_id\": "} + json_string(output_path.stem().string()) +
        ",\n  \"kind\": \"reference_perfect\",\n  \"mode\": " + json_string(mode_name) + ",\n  \"rate_bps\": " + std::to_string(rate) +
        ",\n  \"interleave\": " + json_string(argv[2]) + ",\n  \"generator\": {\"name\": \"wfg-native-m110/0.1.0\", \"build_id\": " +
        json_string(WFG_TX_BUILD_ID) + ", \"source_manifest_sha256\": " + json_string(WFG_TX_SOURCE_MANIFEST_SHA256) +
        ", \"donor_revision\": \"b20ae7dfab068937f438a10bd37c73429f54e591\"},\n  \"payload\": {\"path\": " + json_string(payload_path.filename().string()) +
        ", \"bytes\": " + std::to_string(payload_bytes) + ", \"payload_bytes\": " + std::to_string(payload_bytes) + ", \"sha256\": " + json_string(payload.digest.hex()) +
        ", \"bit_order\": \"lsb_first_within_octet\", \"input_kind\": " + json_string(file_input ? "file" : "text") + "},\n  \"wav\": {\"path\": " +
        json_string(output_path.filename().string()) + ", \"bytes\": " + std::to_string(static_cast<std::uint64_t>(data_bytes) + (wav ? 44U : 0U)) + ", \"sha256\": " + json_string(output_hash) +
        ", \"samples\": " + std::to_string(written) + ", \"sample_rate_hz\": 48000, \"channels\": 1, \"encoding\": \"pcm_s16le\", \"container\": " + json_string(wav ? "RIFF/WAVE" : "raw") +
        ", \"pcm\": {\"data_offset_bytes\": " + std::to_string(wav ? 44U : 0U) + ", \"data_bytes\": " + std::to_string(data_bytes) + ", \"sha256\": " + json_string(pcm_hash) +
        "}},\n  \"framing\": {\"waveform_samples\": " + std::to_string(plan.audio_samples) + ", \"body_blocks\": " + std::to_string(plan.body_blocks) +
        ", \"preamble_symbols\": " + std::to_string(plan.preamble_symbols) + ", \"trailing_silence_samples\": " + std::to_string(wav ? 48000U : 0U) +
        ", \"eom_bits\": 32, \"flush_bits\": 144},\n  \"conventions\": {\"quantization\": \"float * 32767, saturate int16, round halfway away from zero (donor tx_to_pcm)\", "
        "\"artifact_paths\": \"relative to this manifest; retain WAV, BIN and JSON together\", \"numerics\": \"unchanged imported transmitter math; no hardware-native parity claim\"}\n}\n";
    files.manifest = std::fopen(files.temporary[2].string().c_str(), "wb");
    if (files.manifest == nullptr || std::fwrite(manifest.data(), 1U, manifest.size(), files.manifest) != manifest.size() || !close_file(files.manifest))
    {
        std::fprintf(stderr, "failed to write artifact manifest\n"); return 1;
    }
    if ((!reuse_payload && !publish(files.temporary[1], payload_path)) || !publish(files.temporary[0], output_path) || !publish(files.temporary[2], manifest_path))
    {
        std::fprintf(stderr, "failed to publish artifact files\n"); return 1;
    }
    std::fprintf(stdout, "wrote %zu waveform samples at 48000 Hz (%zu body blocks) to %s%s\nretained %s and %s\n",
                 plan.audio_samples, plan.body_blocks, output_path.string().c_str(), wav ? " with one second of trailing silence" : "",
                 payload_path.string().c_str(), manifest_path.string().c_str());
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try { return generate(argc, argv); }
    catch (const std::exception& error) { std::fprintf(stderr, "wfg_m110_tx: %s\n", error.what()); return 1; }
}
