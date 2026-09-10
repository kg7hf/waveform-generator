#pragma once

#include "core/transmitter.hpp"
#include "signal_lab/sample_stream.hpp"
#include "waveform-source/encoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace native_m110
{

// The payload is sequential and its length is declared at configure(). A short
// read is allowed; zero before the declared end is an error. The source owns
// neither this object nor its storage, so both must outlive the transmission.
using ByteSource = waveform_source::ByteSource;

class MemoryBytes final : public ByteSource
{
public:
    explicit MemoryBytes(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes} {}
    void reset(std::span<const std::uint8_t> bytes) noexcept { bytes_ = bytes; position_ = 0U; }
    [[nodiscard]] std::size_t read(std::uint8_t* bytes, std::size_t capacity) noexcept override;
private:
    std::span<const std::uint8_t> bytes_{};
    std::size_t position_{};
};

// Bounded adapter of the imported M110 encoder. One information/interleaver
// block and one encoded segment are retained, regardless of payload duration.
// All mutable workspace belongs to this object; there is no heap allocation.
// Construct explicitly (placement new) before use in an embedded NOLOAD region.
class Source final : public waveform_source::Encoder
{
public:
    static constexpr std::size_t maximum_information_bits = 11520U;
    static constexpr std::size_t maximum_coded_bits = 23040U;
    static constexpr std::size_t maximum_segment_symbols = 11520U;
    static constexpr std::size_t render_frames = 480U;
    static constexpr std::size_t render_symbols = render_frames / m110::body_audio_samples_per_symbol;
    static constexpr std::size_t window_capacity = 2U * m110::body_audio_shaping_span_symbols + render_symbols;

    [[nodiscard]] m110::Status configure(m110::BodyMode mode, ByteSource& payload, std::size_t payload_bytes,
                                        std::uint32_t trailing_silence_frames = 48000U) noexcept;
    [[nodiscard]] m110::Status configure(std::string_view profile, ByteSource& payload, std::size_t payload_bytes) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] std::size_t read(std::int16_t* frames, std::size_t capacity) noexcept override;
    [[nodiscard]] std::size_t read_float(float* frames, std::size_t capacity) noexcept override;
    [[nodiscard]] std::uint64_t total_frames() const noexcept override { return total_frames_; }
    [[nodiscard]] m110::Status status() const noexcept override { return status_; }
    [[nodiscard]] const m110::BodyTransmissionPlan& plan() const noexcept { return plan_; }

private:
    [[nodiscard]] bool encode_segment(std::size_t index) noexcept;
    [[nodiscard]] std::uint8_t framed_bit(std::size_t index) noexcept;
    [[nodiscard]] bool refill_window(std::size_t output_symbols) noexcept;
    [[nodiscard]] bool pull_symbol(std::uint8_t& value) noexcept;
    [[nodiscard]] bool render_next() noexcept;

    m110::BodyTransmissionPlan plan_{};
    m110::BodyEncodeState encode_state_{};
    m110::BodyAudioStreamModulator modulator_{};
    ByteSource* payload_{};
    std::size_t payload_bytes_{};
    std::size_t fetched_bytes_{};
    std::array<std::uint8_t, 256U> payload_buffer_{};
    std::size_t payload_buffer_count_{};
    std::size_t payload_buffer_cursor_{};
    std::uint8_t octet_{};
    std::array<std::uint8_t, maximum_information_bits> information_{};
    std::array<std::uint8_t, maximum_coded_bits> coded_{};
    std::array<std::uint8_t, maximum_coded_bits> matrix_{};
    std::array<std::uint8_t, maximum_coded_bits> interleaved_{};
    std::array<std::uint8_t, maximum_segment_symbols> segment_{};
    std::size_t segment_index_{};
    std::array<std::uint8_t, window_capacity> window_{};
    std::size_t window_first_{};
    std::size_t window_size_{};
    std::size_t next_symbol_{};
    std::size_t rendered_symbols_{};
    std::array<float, render_frames> audio_{};
    std::size_t audio_count_{};
    std::size_t audio_cursor_{};
    std::uint32_t silence_remaining_{};
    std::uint64_t total_frames_{};
    m110::Status status_{m110::StatusCode::invalid_configuration, "native source is not configured"};
    bool configured_{};
};

[[nodiscard]] bool parse_mode(std::uint32_t rate, const char* interleave, m110::BodyMode& mode) noexcept;

inline constexpr std::size_t encoder_workspace_bytes = sizeof(Source);
inline constexpr std::size_t encoder_workspace_alignment = alignof(Source);
[[nodiscard]] waveform_source::Encoder* create_encoder(void* workspace, std::size_t bytes) noexcept;

} // namespace native_m110
