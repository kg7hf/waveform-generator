#pragma once

// Generator-local import of the original-EVK WM8960/SAI/eDMA mechanism.
// P1.1 is a build checkpoint only. This imported circular full-duplex scheduler
// is not yet the qualified finite-descriptor WAV playback transport.
// The peripheral is configured for signed 32-bit I2S words, interleaved L,R.
// The optional local tone writes PCM16 into the most significant 16 bits.
// Hardware word alignment, levels, timing, and fault behavior still require
// the P1.4 qualification. Do not infer playback integrity from a successful link.

#include "common/status.hpp"

#include <cstddef>
#include <cstdint>

namespace m110::imxrt1170
{

inline constexpr std::uint32_t codec_sample_rate_hz = 48000U;

// Stereo transport: both physical 32-bit I2S slots are present.
inline constexpr std::uint16_t codec_channels = 2U;

// Largest block per ping-pong buffer. Static buffers are sized to 2 x this x
// channels. 512 frames is ~10.7 ms at 48 kHz.
inline constexpr std::uint16_t codec_max_frames_per_block = 512U;

// Invoked from the AUDIO TASK (not ISR) with interleaved raw I2S words.
// Each buffer holds frames x codec_channels words in non-cacheable DMA memory.
// The hook must be bounded and perform no file I/O, allocation, or logging.
using CodecBlockHook = void (*)(void* context,
                                const std::uint32_t* capture,
                                std::uint32_t* playback,
                                std::size_t frames) noexcept;

struct CodecConfig
{
    std::uint16_t frames_per_block{256U};
    // Audio-task priority. Must be the highest application priority so a filled
    // block is serviced before the eDMA can lap it. 0 => configMAX_PRIORITIES-1.
    std::uint32_t task_priority{0U};
};

class Wm8960Codec
{
public:
    // Bring up clocks/pins/DMAMUX/eDMA/SAI1 and the WM8960 codec; create (but do
    // not start) the audio task. Does not start streaming.
    [[nodiscard]] Status configure(const CodecConfig& config, CodecBlockHook hook, void* context) noexcept;
    // Start the loop transfers -> 48 kHz full-duplex streaming (hook per block).
    [[nodiscard]] Status start() noexcept;
    // Stop streaming. Safe to call when not started.
    [[nodiscard]] Status stop() noexcept;
    [[nodiscard]] Result<std::uint8_t> set_receive_level_percent(std::uint8_t percent) noexcept;
    [[nodiscard]] Result<std::uint8_t> set_transmit_level_percent(std::uint8_t percent) noexcept;
    [[nodiscard]] Result<std::uint8_t> receive_level_percent() const noexcept;
    [[nodiscard]] Result<std::uint8_t> transmit_level_percent() const noexcept;

    [[nodiscard]] bool is_configured() const noexcept { return configured_; }
    [[nodiscard]] bool is_running() const noexcept { return running_; }

    // Diagnostics (for on-target health checks / SWD observables).
    [[nodiscard]] std::uint32_t blocks_processed() const noexcept { return blocks_processed_; }
    [[nodiscard]] std::uint32_t rx_overruns() const noexcept { return rx_overruns_; }
    [[nodiscard]] std::uint32_t rx_errors() const noexcept { return rx_errors_; }
    [[nodiscard]] std::uint32_t tx_errors() const noexcept { return tx_errors_; }
    [[nodiscard]] std::uint32_t max_backlog_blocks() const noexcept { return max_backlog_blocks_; }
    [[nodiscard]] std::uint32_t max_block_cycles() const noexcept { return max_block_cycles_; }

    // ISR entry points (invoked by the SAI eDMA completion callbacks).
    void on_rx_block_complete() noexcept;
    void on_rx_error() noexcept;
    void on_tx_error() noexcept;

    // Audio-task body (invoked only by the driver's static task trampoline).
    void run_audio_task() noexcept;

private:
    void process_block(std::uint32_t half_index) noexcept;

    CodecBlockHook hook_{nullptr};
    void* context_{nullptr};
    std::uint16_t frames_{0U};
    bool configured_{false};
    volatile bool running_{false};
    // Defaults reproduce the current vendor initialization: LINEIN raw 0x17
    // (0 dB) and the generic 70-percent headphone setting (raw 0x67).
    std::uint8_t receive_level_percent_{37U};
    std::uint8_t transmit_level_percent_{70U};

    volatile std::uint32_t rx_produced_{0U};
    std::uint32_t rx_consumed_{0U};

    volatile std::uint32_t blocks_processed_{0U};
    volatile std::uint32_t rx_overruns_{0U};
    volatile std::uint32_t rx_errors_{0U};
    volatile std::uint32_t tx_errors_{0U};
    volatile std::uint32_t max_backlog_blocks_{0U};
    volatile std::uint32_t max_block_cycles_{0U};
};

// The single driver instance (this target has no heap for a PIMPL allocation).
[[nodiscard]] Wm8960Codec& wm8960_codec() noexcept;

} // namespace m110::imxrt1170
