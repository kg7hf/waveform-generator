#include "usb_checkpoint.hpp"
#include "usb/usb_tinyusb_port.h"

#if defined(WFG_PLAYER_IMAGE)
#include "player.hpp"
#include "platform/waveform_msc.h"
#include "platform/wm8960_codec.hpp"
#include "status_contract.hpp"
#endif

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

#include <cstdint>
#include <cstring>

namespace waveform_generator
{

constexpr std::uint32_t usb_stack_words = 1024U;
StaticTask_t usb_task_control;
StackType_t usb_task_stack[usb_stack_words];
TaskHandle_t usb_task_handle{};

#if defined(WFG_PLAYER_IMAGE)
constexpr std::size_t control_line_capacity = 32U;
constexpr std::size_t response_capacity = cdc_response_capacity;
char control_line[control_line_capacity]{};
std::size_t control_line_size{};
bool control_line_too_long{};
char response_buffer[response_capacity]{};
std::size_t response_size{};
std::size_t response_written{};
bool response_overflowed{};

void append_char(char value) noexcept
{
    if (response_size + 1U < response_capacity)
    {
        response_buffer[response_size] = value;
        response_size++;
    }
    else
    {
        response_overflowed = true;
    }
}

void append_literal(const char* value) noexcept
{
    while (*value != '\0')
    {
        append_char(*value);
        value++;
    }
}

void append_uint(std::uint32_t value) noexcept
{
    char digits[10]{};
    std::size_t digit_count = 0U;

    do
    {
        digits[digit_count] = static_cast<char>('0' + (value % 10U));
        digit_count++;
        value /= 10U;
    } while (value != 0U && digit_count < sizeof(digits));

    while (digit_count != 0U)
    {
        digit_count--;
        append_char(digits[digit_count]);
    }
}

void append_field(const char* name, std::uint32_t value) noexcept
{
    append_char(' ');
    append_literal(name);
    append_char('=');
    append_uint(value);
}

void finish_response() noexcept
{
    if (response_overflowed || response_size + 2U > response_capacity)
    {
        response_size = 0U;
        response_written = 0U;
        response_overflowed = false;
        append_literal("ERR RESPONSE TOO LONG");
    }

    response_buffer[response_size] = '\n';
    response_size++;
    response_buffer[response_size] = '\0';
    response_written = 0U;
}

void begin_response(const char* prefix) noexcept
{
    response_size = 0U;
    response_written = 0U;
    response_overflowed = false;
    append_literal(prefix);
}

void queue_error(const char* message) noexcept
{
    begin_response("ERR ");
    append_literal(message);
    finish_response();
}

void queue_simple_ok(const char* message) noexcept
{
    begin_response("OK ");
    append_literal(message);
    finish_response();
}

void queue_status() noexcept
{
    const auto media = WFG_MediaGetSnapshot();
    const auto player = player_snapshot();
    const auto& codec = m110::imxrt1170::wm8960_codec();

    begin_response("OK STATUS");
    append_field("media_state", media.state);
    append_field("media_error", media.error);
    append_field("card_ready", media.card_ready);
    append_field("msc_ready", media.msc_ready);
    append_field("msc_read_only", media.msc_read_only);
    append_field("block_count", media.block_count);
    append_field("block_size", media.block_size);
    append_field("init_attempts", media.init_attempts);
    append_field("host_init_status", media.host_init_status);
    append_field("host_detect_status", media.host_detect_status);
    append_field("card_init_status", media.card_init_status);
    append_field("disk_init_status", media.disk_init_status);
    append_field("test_ready_calls", media.test_ready_calls);
    append_field("not_ready_responses", media.not_ready_responses);
    append_field("read_calls", media.read_calls);
    append_field("read_blocks", media.read_blocks);
    append_field("write_calls", media.write_calls);
    append_field("write_blocks", media.write_blocks);
    append_field("read_errors", media.read_errors);
    append_field("write_errors", media.write_errors);
    append_field("invalid_requests", media.invalid_requests);
    append_field("sync_cache_calls", media.sync_cache_calls);
    append_field("eject_requests", media.eject_requests);
    append_field("ownership_handoffs", media.ownership_handoffs);
    append_field("cd_gpio3_level", media.cd_gpio3_level);
    append_field("cd_cm7_gpio3_level", media.cd_cm7_gpio3_level);
    append_field("cd_inserted", media.cd_inserted);
    append_field("player_state", player.state);
    append_field("player_error", player.error);
    append_field("data_bytes", player.data_bytes);
    append_field("file_frames_total", player.file_frames_total);
    append_field("file_frames_enqueued", player.file_frames_enqueued);
    append_field("ring_fill_frames", player.ring_fill_frames);
    append_field("ring_min_frames", player.ring_min_frames);
    append_field("ring_min_pre_eof_frames", player.ring_min_pre_eof_frames);
    append_field("ring_max_frames", player.ring_max_frames);
    append_field("frames_requested", player.frames_requested);
    append_field("file_frames_submitted", player.file_frames_submitted);
    append_field("pcm_drained", player.pcm_drained);
    append_field("eof_count", player.eof_count);
    append_field("eof_silence_frames", player.eof_silence_frames);
    append_field("sd_read_calls", player.sd_read_calls);
    append_field("sd_bytes_read", player.sd_bytes_read);
    append_field("sd_short_reads", player.sd_short_reads);
    append_field("sd_read_errors", player.sd_read_errors);
    append_field("underruns", player.underruns);
    append_field("first_underrun_frame", player.first_underrun_frame);
    append_field("max_sd_read_cycles", player.max_sd_read_cycles);
    append_field("audio_running", codec.is_running() ? 1U : 0U);
    append_field("audio_blocks", codec.blocks_processed());
    append_field("audio_overruns", codec.rx_overruns());
    append_field("audio_rx_errors", codec.rx_errors());
    append_field("audio_tx_errors", codec.tx_errors());
    append_field("audio_max_backlog_blocks", codec.max_backlog_blocks());
    append_field("audio_max_block_cycles", codec.max_block_cycles());
    finish_response();
}

void handle_control_line() noexcept
{
    control_line[control_line_size] = '\0';

    if (std::strcmp(control_line, "STATUS") == 0)
    {
        queue_status();
    }
    else if (std::strcmp(control_line, "MEDIA HOST") == 0)
    {
        if (!player_allows_media_host())
        {
            queue_error("BUSY");
        }
        else
        {
            const wfg_media_host_request_result_t result = WFG_MediaRequestHost();
            if (result == kWFG_MediaHostRequestReady)
            {
                queue_simple_ok("MEDIA HOST");
            }
            else if (result == kWFG_MediaHostRequestInitializing)
            {
                queue_simple_ok("MEDIA HOST INIT");
            }
            else
            {
                queue_error("MEDIA HOST FAILED");
            }
        }
    }
    else if (std::strcmp(control_line, "MEDIA LOCAL") == 0)
    {
        if (WFG_MediaSetLocal())
        {
            queue_simple_ok("MEDIA LOCAL");
        }
        else
        {
            queue_error("MEDIA LOCAL FAILED");
        }
    }
    else if (std::strcmp(control_line, "PLAY") == 0)
    {
        const auto status = request_player_play();
        if (status.is_ok())
        {
            queue_simple_ok("PLAY");
        }
        else
        {
            queue_error(status.message);
        }
    }
    else
    {
        queue_error("BAD REQUEST");
    }
}

void service_response() noexcept
{
    while (response_written < response_size)
    {
        const auto written = m110_tinyusb_cdc_write(
            response_buffer + response_written,
            static_cast<std::uint32_t>(response_size - response_written));
        if (written == 0U)
        {
            break;
        }
        response_written += written;
    }

    m110_tinyusb_cdc_flush();
    if (response_written == response_size)
    {
        response_size = 0U;
        response_written = 0U;
    }
}

void service_control() noexcept
{
    if (response_size != 0U)
    {
        service_response();
        return;
    }

    while (m110_tinyusb_cdc_available() != 0U && response_size == 0U)
    {
        char byte{};
        if (m110_tinyusb_cdc_read(&byte, 1U) != 1U)
        {
            break;
        }

        if (control_line_too_long)
        {
            if (byte == '\n')
            {
                control_line_too_long = false;
                control_line_size = 0U;
                queue_error("LINE TOO LONG");
            }
            continue;
        }

        if (byte == '\r')
        {
            continue;
        }
        if (byte == '\n')
        {
            handle_control_line();
            control_line_size = 0U;
        }
        else if (control_line_size + 1U < control_line_capacity)
        {
            control_line[control_line_size] = byte;
            control_line_size++;
        }
        else
        {
            control_line_too_long = true;
            control_line_size = 0U;
        }
    }

    if (response_size != 0U)
    {
        service_response();
    }
}
#endif

void usb_task_entry(void*) noexcept
{
    if (!m110_tinyusb_stack_initialize())
    {
        vTaskSuspend(nullptr);
    }

    for (;;)
    {
        m110_tinyusb_task(10U);
#if defined(WFG_PLAYER_IMAGE)
        service_control();
#endif
    }
}

m110::Status start_usb_checkpoint() noexcept
{
    if (usb_task_handle != nullptr)
    {
        return {m110::StatusCode::invalid_argument, "USB checkpoint already started"};
    }

    if (!m110_usb_hardware_initialize())
    {
        return {m110::StatusCode::unavailable, "USB hardware initialization failed"};
    }

    usb_task_handle = xTaskCreateStatic(&usb_task_entry, "wfg_usb", usb_stack_words,
                                      nullptr, 3U, usb_task_stack, &usb_task_control);
    if (usb_task_handle == nullptr)
    {
        return {m110::StatusCode::unavailable, "USB checkpoint task creation failed"};
    }

    return m110::Status::success();
}

} // namespace waveform_generator
