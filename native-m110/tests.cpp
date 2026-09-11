#include "native-m110/source.hpp"
#include "native-m110/wav_generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
int failures{};
void check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", message); }
}

std::vector<std::int16_t> oracle(m110::BodyMode mode, const std::vector<std::uint8_t>& payload)
{
    const auto planned = m110::body_transmission_plan(mode, payload.size());
    check(static_cast<bool>(planned), "oracle plan");
    if (!planned) { return {}; }
    const auto plan = planned.value();
    std::vector<std::uint8_t> framed(plan.framed_bits), symbols(plan.transmitted_symbols), coded(plan.block.coded_bits), matrix(plan.block.coded_bits), interleaved(plan.block.coded_bits);
    std::vector<float> audio(plan.audio_samples);
    check(m110::generate_body_transmission_audio(plan, payload, {framed, symbols, coded, matrix, interleaved}, audio).is_ok(), "unchanged donor whole-message render");
    std::vector<std::int16_t> pcm(audio.size());
    for (std::size_t i = 0U; i < audio.size(); ++i) { pcm[i] = static_cast<std::int16_t>(std::lround(std::clamp(audio[i] * 32767.0F, -32768.0F, 32767.0F))); }
    return pcm;
}

class ShortBytes final : public native_m110::ByteSource
{
public:
    explicit ShortBytes(std::span<const std::uint8_t> bytes) : memory_{bytes} {}
    std::size_t read(std::uint8_t* bytes, std::size_t capacity) noexcept override { return memory_.read(bytes, std::min<std::size_t>(capacity, 3U)); }
private:
    native_m110::MemoryBytes memory_;
};

void compare(m110::BodyMode mode, const std::vector<std::uint8_t>& payload, std::size_t read_size)
{
    const auto expected = oracle(mode, payload);
    static native_m110::Source source;
    ShortBytes input{payload};
    check(source.configure(mode, input, payload.size(), 0U).is_ok(), "stream configure");
    check(source.total_frames() == expected.size(), "known total frames");
    std::vector<std::int16_t> block(read_size);
    std::size_t offset{};
    for (;;)
    {
        const auto count = source.read(block.data(), block.size());
        check(source.status().is_ok(), "stream status");
        if (count == 0U) { break; }
        check(offset + count <= expected.size(), "stream length never exceeds plan");
        if (offset + count > expected.size()) { break; }
        if (!std::equal(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(count), expected.begin() + static_cast<std::ptrdiff_t>(offset)))
        {
            check(false, "stream PCM is byte-identical to donor whole-message render"); break;
        }
        offset += count;
    }
    check(offset == expected.size(), "stream complete");
}

class VectorSink final : public native_m110::ByteSink
{
public:
    std::vector<std::uint8_t> bytes;
    bool refuse{};
    bool write(const std::uint8_t* data, std::size_t count) noexcept override
    {
        if (refuse) { return false; }
        bytes.insert(bytes.end(), data, data + count);
        return true;
    }
};

void artifact_tests()
{
    const std::vector<std::uint8_t> payload{0U, 1U, 0x80U, 0xFFU};
    const m110::BodyMode mode{m110::DataRate::bps600, m110::BodyInterleave::short_block};
    static native_m110::Source source;
    native_m110::MemoryBytes input{payload};
    VectorSink sink;
    native_m110::WavGenerator job;
    check(job.begin(source, mode, input, payload.size(), sink, 7U).is_ok(), "WAV begin");
    check(sink.bytes.size() == 44U, "begin writes header only");
    check(job.hashes().wav[0] == '\0', "incomplete job has no final artifact hashes");
    while (job.state() == native_m110::JobState::running)
    {
        const auto before = job.frames_written();
        (void)job.step(17U);
        check(job.frames_written() - before <= 17U, "incremental step respects frame budget");
    }
    check(job.state() == native_m110::JobState::complete, "WAV job completed");
    auto expected = oracle(mode, payload);
    expected.resize(expected.size() + 7U, 0);
    check(sink.bytes.size() == 44U + expected.size() * 3U, "PCM24 WAV length");
    for (std::size_t i = 0U; i < expected.size(); ++i)
    {
        const auto value = waveform_generator::audio::read_pcm24_le(sink.bytes.data() + 44U + 3U * i);
        const double scaled = static_cast<double>(value) * 32767.0 /
                              waveform_generator::audio::pcm24_scale;
        const auto legacy = static_cast<std::int16_t>(scaled + (scaled < 0.0 ? -0.5 : 0.5));
        if (std::abs(static_cast<int>(legacy) - static_cast<int>(expected[i])) > 1)
        {
            check(false, "PCM24 WAV preserves donor waveform plus exact trailing silence"); break;
        }
    }
    const auto hashes = job.hashes();
    native_m110::Sha256 payload_hash, wav_hash, pcm_hash;
    payload_hash.update(payload.data(), payload.size());
    wav_hash.update(sink.bytes.data(), sink.bytes.size());
    pcm_hash.update(sink.bytes.data() + 44U, sink.bytes.size() - 44U);
    check(payload_hash.hex() == hashes.payload && wav_hash.hex() == hashes.wav && pcm_hash.hex() == hashes.pcm, "incremental artifact hashes");

    input.reset(payload); sink.bytes.clear(); sink.refuse = true;
    check(!job.begin(source, mode, input, payload.size(), sink).is_ok(), "header write refusal fails");
    check(job.state() == native_m110::JobState::failed, "header failure state");
    input.reset(payload); sink.refuse = false;
    check(job.begin(source, mode, input, payload.size(), sink).is_ok(), "retry after failure");
    sink.refuse = true;
    check(job.step() == native_m110::JobState::failed, "PCM write refusal fails");
    check(job.hashes().wav[0] == '\0', "failed job has no final digest");
    job.stop();
    check(job.state() == native_m110::JobState::idle && source.read(nullptr, 1U) == 0U, "stop ends job and source");

    input.reset(payload); sink.refuse = false;
    check(job.begin(source, mode, input, payload.size() + 1U, sink).is_ok(), "short payload plans");
    while (job.state() == native_m110::JobState::running) { (void)job.step(); }
    check(job.state() == native_m110::JobState::failed && job.status().code == m110::StatusCode::io_error, "short payload is not a successful truncated WAV");
}

void hash_tests()
{
    native_m110::Sha256 hash;
    check(hash.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA256 empty");
    hash.update("a", 1U); hash.update("bc", 2U);
    check(hash.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 abc split");
    hash = {};
    const std::array<std::uint8_t, 1000U> zeros{};
    std::array<std::uint8_t, 1000U> a = zeros; a.fill('a');
    for (int i = 0; i < 1000; ++i) { hash.update(a.data(), a.size()); }
    check(hash.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA256 million a");
}
}

int main()
{
    hash_tests();
    std::vector<std::uint8_t> payload(31U);
    for (std::size_t i = 0U; i < payload.size(); ++i) { payload[i] = static_cast<std::uint8_t>(i * 71U); }
    for (const auto rate : {m110::DataRate::bps75, m110::DataRate::bps150, m110::DataRate::bps300,
                            m110::DataRate::bps600, m110::DataRate::bps1200, m110::DataRate::bps2400})
    {
        for (const auto interleave : {m110::BodyInterleave::short_block, m110::BodyInterleave::long_block}) { compare({rate, interleave}, payload, 2048U); }
    }
    compare({m110::DataRate::bps4800, m110::BodyInterleave::zero}, payload, 2048U);
    payload.resize(1000U, 0xA5U);
    for (const auto block : {1U, 17U, 8191U}) { compare({m110::DataRate::bps600, m110::BodyInterleave::long_block}, payload, block); }
    compare({m110::DataRate::bps300, m110::BodyInterleave::short_block}, {}, 127U);
    artifact_tests();
    std::printf("native M110 tests: %d failures; Source=%zu bytes WavGenerator=%zu bytes\n", failures, sizeof(native_m110::Source), sizeof(native_m110::WavGenerator));
    return failures == 0 ? 0 : 1;
}
