// Generator-local P1.1 import; see wm8960_codec.hpp for qualification limits.
// DMA buffers retain their original non-cacheable placement and static task.

#include "wm8960_codec.hpp"

#if defined(CPU_MIMXRT1176DVMAA_cm7)

extern "C"
{
#include "fsl_clock.h"
#include "fsl_codec_adapter.h"
#include "fsl_codec_common.h"
#include "fsl_common.h"
#include "fsl_dmamux.h"
#include "fsl_edma.h"
#include "fsl_iomuxc.h"
#include "fsl_sai_edma.h"
#include "fsl_wm8960.h"

#include "FreeRTOS.h"
#include "task.h"
}

namespace m110::imxrt1170
{
namespace
{

// --- Fixed audio format (proven values from experiment 005) ----------------
constexpr std::uint32_t k_mclk_hz = 12288000U;   // 48 kHz x 256
constexpr std::uint32_t k_slot_bits = 32U;       // 24 valid bits in 32-bit slots
constexpr std::uint8_t k_sai_tx_channel = 0U;
constexpr std::uint8_t k_tx_dma_channel = 0U;
constexpr std::uint8_t k_rx_dma_channel = 1U;

// Number of ping-pong buffers per direction (double-buffer).
constexpr std::uint32_t k_loop_count = 2U;

// eDMA channel NVIC priority. MUST be numerically >= configLIBRARY_MAX_SYSCALL_
// INTERRUPT_PRIORITY so the RX completion ISR may call the *FromISR notify API.
constexpr std::uint32_t k_audio_irq_priority = 6U;

// --- Non-cacheable DMA buffers (no manual cache maintenance needed) ---------
constexpr std::size_t k_words_max =
    2U * static_cast<std::size_t>(codec_max_frames_per_block) * codec_channels;

AT_NONCACHEABLE_SECTION_ALIGN(std::uint32_t g_rx_buffer[k_words_max], 32);
AT_NONCACHEABLE_SECTION_ALIGN(std::uint32_t g_tx_buffer[k_words_max], 32);

// SAI eDMA handles kept in quick-access RAM (proven placement from experiment).
AT_QUICKACCESS_SECTION_DATA(sai_edma_handle_t g_tx_sai_handle);
AT_QUICKACCESS_SECTION_DATA(sai_edma_handle_t g_rx_sai_handle);
edma_handle_t g_tx_dma_handle;
edma_handle_t g_rx_dma_handle;
codec_handle_t g_codec_handle;

sai_transfer_t g_rx_xfer[k_loop_count];
sai_transfer_t g_tx_xfer[k_loop_count];

// Codec configs are populated by codec_config_initialize() using field
// assignment (order-independent) rather than aggregate designated initializers,
// whose member order C++ requires to match the SDK struct declaration exactly.
// g_wm8960_config must persist: the wm8960 handle keeps a pointer to it.
wm8960_config_t g_wm8960_config{};
codec_config_t g_codec_config{};

void codec_config_initialize() noexcept
{
    g_wm8960_config.route = kWM8960_RoutePlaybackandRecord;
    g_wm8960_config.bus = kWM8960_BusI2S;
    g_wm8960_config.format.mclk_HZ = k_mclk_hz;
    g_wm8960_config.format.sampleRate = 48000U;
    g_wm8960_config.format.bitWidth = k_slot_bits;
    g_wm8960_config.master_slave = true; // WM8960 masters the bit/frame clock
    g_wm8960_config.enableSpeaker = false;
    // Analog interop RX arrives from MS-DMT at LINE level on the HP-mic jack
    // (HP_MIC1P -> LINPUT3). Capture it as a LINE input through the input boost
    // mixer (~+6 dB), NOT the differential-mic recipe: the mic path would enable
    // MICBIAS into the HP-mic divider and apply +29 dB mic boost, clipping a
    // line-level source. Close the right input (on-board Main Board MIC) so it
    // adds no noise and MICBIAS stays disabled.
    g_wm8960_config.leftInputSource = kWM8960_InputLineINPUT3;
    g_wm8960_config.rightInputSource = kWM8960_InputClosed;
    g_wm8960_config.playSource = kWM8960_PlaySourceDAC;
    g_wm8960_config.slaveAddress = WM8960_I2C_ADDR;
    g_wm8960_config.i2cConfig.codecI2CInstance = 5U;
    g_wm8960_config.i2cConfig.codecI2CSourceClock = 24000000U;

    g_codec_config.codecDevType = kCODEC_WM8960;
    g_codec_config.codecDevConfig = &g_wm8960_config;
}

// --- Audio task -------------------------------------------------------------
// The DD-009 receiver's acquisition-commit path (preamble training plus the
// delay-spread estimator) peaks near 6 KiB of live frames inside this task;
// 12 KiB leaves measured headroom.
constexpr std::uint32_t k_audio_task_stack_words = 3072U;
constexpr std::uint32_t k_audio_task_default_priority = configMAX_PRIORITIES - 1U;

StaticTask_t g_audio_task_control;
StackType_t g_audio_task_stack[k_audio_task_stack_words];
TaskHandle_t g_audio_task_handle = nullptr;

void audio_task_entry(void*) noexcept
{
    wm8960_codec().run_audio_task();
}

// --- SAI eDMA completion callbacks (run in eDMA channel ISR) ----------------
void rx_edma_callback(I2S_Type*, sai_edma_handle_t*, status_t status, void*) noexcept
{
    if (status == kStatus_SAI_RxError)
    {
        wm8960_codec().on_rx_error();
        return;
    }

    wm8960_codec().on_rx_block_complete();
}

void tx_edma_callback(I2S_Type*, sai_edma_handle_t*, status_t status, void*) noexcept
{
    if (status == kStatus_SAI_TxError)
    {
        wm8960_codec().on_tx_error();
    }
}

// --- Bring-up helpers -------------------------------------------------------
void audio_pins_initialize() noexcept
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_Iomuxc_Lpsr);
    // SAI1: MCLK, RX_DATA0, TX_DATA0, TX_BCLK, TX_SYNC.
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_17_SAI1_MCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_20_SAI1_RX_DATA00, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_21_SAI1_TX_DATA00, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_22_SAI1_TX_BCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_23_SAI1_TX_SYNC, 1U);
    // LPI2C5 for codec control.
    IOMUXC_SetPinMux(IOMUXC_GPIO_LPSR_04_LPI2C5_SDA, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_LPSR_05_LPI2C5_SCL, 1U);
    // MCLK is an output (the RT1170 supplies it to the codec).
    IOMUXC_GPR->GPR0 = (IOMUXC_GPR->GPR0 & ~IOMUXC_GPR_GPR0_SAI1_MCLK_DIR_MASK) |
                       IOMUXC_GPR_GPR0_SAI1_MCLK_DIR(1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_17_SAI1_MCLK, 0x02U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_20_SAI1_RX_DATA00, 0x02U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_21_SAI1_TX_DATA00, 0x02U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_22_SAI1_TX_BCLK, 0x02U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_23_SAI1_TX_SYNC, 0x02U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_LPSR_04_LPI2C5_SDA, 0x0AU);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_LPSR_05_LPI2C5_SCL, 0x0AU);
}

void audio_clocks_initialize() noexcept
{
    clock_audio_pll_config_t audio_pll_config{};
    audio_pll_config.loopDivider = 32U;
    audio_pll_config.postDivider = 1U;
    audio_pll_config.numerator = 768U;
    audio_pll_config.denominator = 1000U;
    CLOCK_InitAudioPll(&audio_pll_config);
    CLOCK_SetRootClockMux(kCLOCK_Root_Lpi2c5, 1U);
    CLOCK_SetRootClockMux(kCLOCK_Root_Sai1, 4U); // AUDIO_PLL
    // The RT1170 audio PLL is 393.216 MHz. Divide by 32 to put the declared
    // 12.288-MHz MCLK on the codec pin (48 kHz x 256). A divider of 16 drove
    // the WM8960 at 24.576 MHz while programming it as 12.288 MHz, so its
    // master-mode LRCLK ran at 96 kHz and compressed every waveform by 2:1.
    CLOCK_SetRootClockDiv(kCLOCK_Root_Sai1, 32U);
}

void dma_and_sai_initialize(std::uint16_t frames) noexcept
{
    edma_config_t dma_config;

    DMAMUX_Init(DMAMUX0);
    DMAMUX_SetSource(DMAMUX0, k_tx_dma_channel, static_cast<std::uint8_t>(kDmaRequestMuxSai1Tx));
    DMAMUX_EnableChannel(DMAMUX0, k_tx_dma_channel);
    DMAMUX_SetSource(DMAMUX0, k_rx_dma_channel, static_cast<std::uint8_t>(kDmaRequestMuxSai1Rx));
    DMAMUX_EnableChannel(DMAMUX0, k_rx_dma_channel);

    EDMA_GetDefaultConfig(&dma_config);
    EDMA_Init(DMA0, &dma_config);
    EDMA_CreateHandle(&g_tx_dma_handle, DMA0, k_tx_dma_channel);
    EDMA_CreateHandle(&g_rx_dma_handle, DMA0, k_rx_dma_channel);

    SAI_Init(SAI1);
    SAI_TransferTxCreateHandleEDMA(SAI1, &g_tx_sai_handle, &tx_edma_callback, nullptr, &g_tx_dma_handle);
    SAI_TransferRxCreateHandleEDMA(SAI1, &g_rx_sai_handle, &rx_edma_callback, nullptr, &g_rx_dma_handle);

    sai_transceiver_t sai_config;
    SAI_GetClassicI2SConfig(&sai_config, kSAI_WordWidth32bits, kSAI_Stereo, 1UL << k_sai_tx_channel);
    sai_config.syncMode = kSAI_ModeAsync;
    sai_config.bitClock.bclkPolarity = kSAI_PolarityActiveLow;
    sai_config.masterSlave = kSAI_Slave; // codec is master
    SAI_TransferTxSetConfigEDMA(SAI1, &g_tx_sai_handle, &sai_config);
    sai_config.syncMode = kSAI_ModeSync;
    SAI_TransferRxSetConfigEDMA(SAI1, &g_rx_sai_handle, &sai_config);
    SAI_TxSetBitClockRate(SAI1, k_mclk_hz, 48000U, k_slot_bits, codec_channels);
    SAI_RxSetBitClockRate(SAI1, k_mclk_hz, 48000U, k_slot_bits, codec_channels);

    // Keep the audio eDMA channel interrupts inside the FreeRTOS-maskable band so
    // the RX callback may notify the audio task.
    NVIC_SetPriority(DMA0_DMA16_IRQn, k_audio_irq_priority); // channel 0 (TX)
    NVIC_SetPriority(DMA1_DMA17_IRQn, k_audio_irq_priority); // channel 1 (RX)

    const std::uint32_t block_bytes =
        static_cast<std::uint32_t>(frames) * codec_channels * sizeof(std::uint32_t);
    for (std::uint32_t i = 0U; i < k_loop_count; ++i)
    {
        const std::size_t offset = static_cast<std::size_t>(i) * frames * codec_channels;
        g_rx_xfer[i].data = reinterpret_cast<std::uint8_t*>(&g_rx_buffer[offset]);
        g_rx_xfer[i].dataSize = block_bytes;
        g_tx_xfer[i].data = reinterpret_cast<std::uint8_t*>(&g_tx_buffer[offset]);
        g_tx_xfer[i].dataSize = block_bytes;
    }
}

constexpr std::uint32_t percent_to_raw(std::uint8_t percent, std::uint32_t minimum, std::uint32_t maximum) noexcept
{
    return percent == 0U ? minimum : minimum + (static_cast<std::uint32_t>(percent - 1U) * (maximum - minimum) + 49U) / 99U;
}

static_assert(percent_to_raw(37U, 0U, 0x3FU) == 0x17U);
static_assert(percent_to_raw(70U, 0x30U, 0x7FU) == 0x67U);

wm8960_handle_t* codec_device_handle() noexcept
{
    return reinterpret_cast<wm8960_handle_t*>(g_codec_handle.codecDevHandle);
}

status_t apply_receive_level(std::uint8_t percent) noexcept
{
    auto* handle = codec_device_handle();

    if (percent == 0U)
    {
        // LINEIN has no mute code. Mute at the ADC digital volume instead.
        return WM8960_SetVolume(handle, kWM8960_ModuleADC, 0U);
    }

    const auto line_input = percent_to_raw(percent, 0U, 0x3FU);

    const auto line_status = WM8960_SetVolume(handle, kWM8960_ModuleLineIn, line_input);

    if (line_status != kStatus_Success)
    {
        return line_status;
    }

    // Raw 0xC3 is the vendor's unity ADC value. Restore it after a mute or a
    // successful LINEIN update without introducing a second logical gain.
    return WM8960_SetVolume(handle, kWM8960_ModuleADC, 0xC3U);
}

status_t apply_transmit_level(std::uint8_t percent) noexcept
{
    // The direct driver correctly treats raw 0..0x2F as mute. The generic
    // percent adapter instead maps logical zero to 0x30 (-73 dB), so do not use
    // CODEC_SetVolume for this runtime control.
    const auto headphone = percent == 0U ? 0U : percent_to_raw(percent, 0x30U, 0x7FU);
    return WM8960_SetVolume(codec_device_handle(), kWM8960_ModuleHP, headphone);
}

} // namespace

// ---------------------------------------------------------------------------
Wm8960Codec& wm8960_codec() noexcept
{
    static Wm8960Codec instance;
    return instance;
}

Status Wm8960Codec::configure(const CodecConfig& config, CodecBlockHook hook, void* context) noexcept
{
    if (configured_)
    {
        return {StatusCode::invalid_argument, "WM8960 codec already configured"};
    }
    if (hook == nullptr)
    {
        return {StatusCode::invalid_argument, "null codec block hook"};
    }
    if (config.frames_per_block == 0U || config.frames_per_block > codec_max_frames_per_block)
    {
        return {StatusCode::invalid_argument, "frames_per_block out of range"};
    }

    hook_ = hook;
    context_ = context;
    frames_ = config.frames_per_block;
    running_ = false;
    rx_produced_ = 0U;
    rx_consumed_ = 0U;

    audio_pins_initialize();
    audio_clocks_initialize();
    dma_and_sai_initialize(frames_);
    codec_config_initialize();

    if (CODEC_Init(&g_codec_handle, &g_codec_config) != kStatus_Success)
    {
        return {StatusCode::unavailable, "WM8960 codec init failed"};
    }
    if (CODEC_SetVolume(&g_codec_handle,
                        kCODEC_PlayChannelHeadphoneLeft | kCODEC_PlayChannelHeadphoneRight, 70U) != kStatus_Success)
    {
        return {StatusCode::internal_error, "WM8960 headphone volume failed"};
    }
    if (CODEC_SetVolume(&g_codec_handle, kCODEC_VolumeDAC, 100U) != kStatus_Success)
    {
        return {StatusCode::internal_error, "WM8960 DAC volume failed"};
    }

    std::uint32_t priority = config.task_priority != 0U ? config.task_priority : k_audio_task_default_priority;
    if (priority >= configMAX_PRIORITIES)
    {
        priority = configMAX_PRIORITIES - 1U;
    }

    if (g_audio_task_handle == nullptr)
    {
        g_audio_task_handle = xTaskCreateStatic(
            &audio_task_entry, "m110_audio", k_audio_task_stack_words, nullptr,
            priority, g_audio_task_stack, &g_audio_task_control);

        if (g_audio_task_handle == nullptr)
        {
            return {StatusCode::unavailable, "audio task creation failed"};
        }
    }

    configured_ = true;
    return Status::success();
}

Status Wm8960Codec::start() noexcept
{
    if (!configured_)
    {
        return {StatusCode::invalid_argument, "WM8960 codec not configured"};
    }
    if (running_)
    {
        return Status::success();
    }

    const std::size_t words = 2U * static_cast<std::size_t>(frames_) * codec_channels;
    for (std::size_t i = 0U; i < words; ++i)
    {
        g_rx_buffer[i] = 0U;
        g_tx_buffer[i] = 0U;
    }

    rx_produced_ = 0U;
    rx_consumed_ = 0U;
    running_ = true;

    // Start TX first, then RX, so the frame clock is running when RX arms.
    if (SAI_TransferSendLoopEDMA(SAI1, &g_tx_sai_handle, g_tx_xfer, k_loop_count) != kStatus_Success)
    {
        running_ = false;
        return {StatusCode::unavailable, "SAI TX loop start failed"};
    }
    if (SAI_TransferReceiveLoopEDMA(SAI1, &g_rx_sai_handle, g_rx_xfer, k_loop_count) != kStatus_Success)
    {
        SAI_TransferTerminateSendEDMA(SAI1, &g_tx_sai_handle);
        running_ = false;
        return {StatusCode::unavailable, "SAI RX loop start failed"};
    }

    return Status::success();
}

Status Wm8960Codec::stop() noexcept
{
    if (!configured_)
    {
        return Status::success();
    }

    running_ = false;
    SAI_TransferTerminateReceiveEDMA(SAI1, &g_rx_sai_handle);
    SAI_TransferTerminateSendEDMA(SAI1, &g_tx_sai_handle);
    return Status::success();
}

Result<std::uint8_t> Wm8960Codec::set_receive_level_percent(std::uint8_t percent) noexcept
{
    if (!configured_)
    {
        return Status{StatusCode::unavailable, "WM8960 codec not configured"};
    }

    if (percent > 100U)
    {
        return Status{StatusCode::invalid_argument, "WM8960 receive level exceeds 100 percent"};
    }

    if (apply_receive_level(percent) != kStatus_Success)
    {
        static_cast<void>(apply_receive_level(receive_level_percent_));
        return Status{StatusCode::internal_error, "WM8960 receive level update failed"};
    }

    receive_level_percent_ = percent;
    return percent;
}

Result<std::uint8_t> Wm8960Codec::set_transmit_level_percent(std::uint8_t percent) noexcept
{
    if (!configured_)
    {
        return Status{StatusCode::unavailable, "WM8960 codec not configured"};
    }

    if (percent > 100U)
    {
        return Status{StatusCode::invalid_argument, "WM8960 transmit level exceeds 100 percent"};
    }

    if (apply_transmit_level(percent) != kStatus_Success)
    {
        static_cast<void>(apply_transmit_level(transmit_level_percent_));
        return Status{StatusCode::internal_error, "WM8960 transmit level update failed"};
    }

    transmit_level_percent_ = percent;
    return percent;
}

Result<std::uint8_t> Wm8960Codec::receive_level_percent() const noexcept
{
    return configured_ ? Result<std::uint8_t>{receive_level_percent_} : Result<std::uint8_t>{Status{StatusCode::unavailable, "WM8960 codec not configured"}};
}

Result<std::uint8_t> Wm8960Codec::transmit_level_percent() const noexcept
{
    return configured_ ? Result<std::uint8_t>{transmit_level_percent_} : Result<std::uint8_t>{Status{StatusCode::unavailable, "WM8960 codec not configured"}};
}

void Wm8960Codec::process_block(std::uint32_t half_index) noexcept
{
    const std::uint32_t started_at = DWT->CYCCNT;
    const std::size_t words = static_cast<std::size_t>(frames_) * codec_channels;
    const std::size_t first = static_cast<std::size_t>(half_index) * words;

    // Buffers are non-cacheable -> read/write directly, no cache maintenance.
    hook_(context_, &g_rx_buffer[first], &g_tx_buffer[first], frames_);

    const std::uint32_t elapsed = DWT->CYCCNT - started_at;
    if (elapsed > max_block_cycles_)
    {
        max_block_cycles_ = elapsed;
    }
}

void Wm8960Codec::run_audio_task() noexcept
{
    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));

        if (!running_)
        {
            continue;
        }

        while (rx_consumed_ != rx_produced_)
        {
            const std::uint32_t backlog = rx_produced_ - rx_consumed_;
            if (backlog > max_backlog_blocks_)
            {
                max_backlog_blocks_ = backlog;
            }

            // Double-buffer: a backlog of 2+ means the eDMA has begun overwriting
            // a half we had not processed -- a real drop. Record and resync to
            // the freshest half. Must stay zero for a drop-free digital stream.
            if (backlog >= k_loop_count)
            {
                rx_overruns_ = rx_overruns_ + 1U;
                rx_consumed_ = rx_produced_ - 1U;
            }

            const std::uint32_t half = rx_consumed_ % k_loop_count;
            process_block(half);
            ++rx_consumed_; // rx_consumed_ is not volatile
            blocks_processed_ = blocks_processed_ + 1U;
        }
    }
}

void Wm8960Codec::on_rx_block_complete() noexcept
{
    rx_produced_ = rx_produced_ + 1U; // volatile: no compound ++
    BaseType_t higher_priority_woken = pdFALSE;
    if (g_audio_task_handle != nullptr)
    {
        vTaskNotifyGiveFromISR(g_audio_task_handle, &higher_priority_woken);
    }
    portYIELD_FROM_ISR(higher_priority_woken);
}

void Wm8960Codec::on_rx_error() noexcept
{
    rx_errors_ = rx_errors_ + 1U; // volatile: no compound ++
}

void Wm8960Codec::on_tx_error() noexcept
{
    tx_errors_ = tx_errors_ + 1U; // volatile: no compound ++
}

} // namespace m110::imxrt1170

#endif // CPU_MIMXRT1176DVMAA_cm7
