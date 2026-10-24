// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// TinyUSB UAC2 callbacks of the player's composite device, and its device-level
// mount, unmount and control-status callbacks
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: control requests, interface and configuration changes arrive in
// the wfg_usb task; tx_done and bus events arrive in the USB ISR. The stream
// state in usb_audio.cpp belongs to the USB interrupt level, so a task-context
// call that touches it runs inside taskENTER_CRITICAL(), which masks the USB IRQ
// (its priority is configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY).
// Contract: decodes TinyUSB requests into plain values for usb_audio.cpp and
// checks tusb_config.h and TinyUSB's UAC2 codes against usb/usb_composite.h.
// TinyUSB has one mount and one unmount callback per device; they also end the
// host's MSC session in platform/waveform_msc.c, and the control-status hook
// tells it of a Bulk-Only Mass Storage Reset.

#include "usb/usb_audio_callbacks.h"
#include "usb/usb_tinyusb_port.h"
#include "platform/waveform_msc.h"

#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "device/dcd.h"

_Static_assert(CFG_TUD_AUDIO == 1, "one UAC2 function");
_Static_assert(CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX == wfg_uac_max_packet_bytes, "the endpoint buffer holds the largest packet");
_Static_assert(wfg_uac_pcm16_stereo_packet_bytes <= wfg_uac_max_packet_bytes && wfg_uac_pcm16_mono_packet_bytes <= wfg_uac_max_packet_bytes,
               "and every other format's packet");
_Static_assert(CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ == wfg_uac_fifo_bytes, "the IN FIFO holds four of the largest packets");
_Static_assert(wfg_uac_fifo_bytes <= UINT16_MAX, "TinyUSB FIFO counts are 16-bit");
_Static_assert(!CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL, "every packet is 48 frames");
_Static_assert(!CFG_TUD_AUDIO_ENABLE_EP_OUT, "capture only");
_Static_assert(CFG_TUD_AUDIO_CTRL_BUF_SZ >= wfg_uac_control_response_max_bytes, "the control buffer holds every answer");
_Static_assert(sizeof(TickType_t) == sizeof(uint32_t), "the C entry carries the tick as uint32_t");
_Static_assert(configTICK_RATE_HZ == 1000, "one tick per USB millisecond");
// Unary + compares the values as int (the type of a C enumeration constant)
// rather than as two unrelated enumerations (-Wenum-compare).
_Static_assert(+AUDIO20_CS_REQ_CUR == +wfg_uac_request_cur, "UAC2 A.14 CUR");
_Static_assert(+AUDIO20_CS_REQ_RANGE == +wfg_uac_request_range, "UAC2 A.14 RANGE");
_Static_assert(+AUDIO20_CS_CTRL_SAM_FREQ == +wfg_uac_clock_frequency, "UAC2 A.17.1 sampling frequency");
_Static_assert(+AUDIO20_CS_CTRL_CLK_VALID == +wfg_uac_clock_validity, "UAC2 A.17.1 clock validity");
_Static_assert(+AUDIO20_FU_CTRL_MUTE == +wfg_uac_feature_mute, "UAC2 A.17.7 mute");
_Static_assert(+AUDIO20_FU_CTRL_VOLUME == +wfg_uac_feature_volume, "UAC2 A.17.7 volume");
_Static_assert(+MSC_REQ_RESET == +wfg_msc_request_bot_reset, "BOT 3.1 Bulk-Only Mass Storage Reset");

// USB 2.0 Table 9-2: bmRequestType holds the direction in bit 7, the type in
// bits 6..5 and the recipient in bits 4..0.
enum
{
    usb_request_direction_shift = 7,
    usb_request_type_shift = 5,
};

_Static_assert(((+TUSB_DIR_OUT << usb_request_direction_shift) | (+TUSB_REQ_TYPE_CLASS << usb_request_type_shift) | +TUSB_REQ_RCPT_INTERFACE) ==
                   +wfg_usb_request_type_class_interface_out,
               "a class request to an interface, host to device");

static wfg_uac_request_t decode_request(const tusb_control_request_t* request)
{
    const wfg_uac_request_t decoded = {
        .code = request->bRequest,
        .entity = tu_u16_high(request->wIndex),
        .selector = tu_u16_high(request->wValue),
        .channel = tu_u16_low(request->wValue),
    };
    return decoded;
}

static bool is_streaming_interface(const tusb_control_request_t* request)
{
    return tu_u16_low(request->wIndex) == wfg_uac_interface_streaming;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, const tusb_control_request_t* p_request)
{
    wfg_uac_response_t response = wfg_usb_audio_get_request(decode_request(p_request));

    if (response.result != kWFG_UacRequestAccepted)
    {
        return false;
    }

    // TinyUSB copies the answer into its control buffer and sends at most wLength.
    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, response.bytes, response.length);
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, const tusb_control_request_t* p_request, uint8_t* buffer)
{
    (void)rhport;
    // The data stage received wLength bytes, capped at the control buffer size.
    const uint16_t length = tu_min16(p_request->wLength, CFG_TUD_AUDIO_CTRL_BUF_SZ);
    return wfg_usb_audio_set_request(decode_request(p_request), buffer, length) == kWFG_UacRequestAccepted;
}

// An alternate other than 0 opens the stream in its format. TinyUSB has
// already opened the endpoint with that alternate's wMaxPacketSize and queued
// its zero-length primer; the pre-fill runs with the USB IRQ masked so no
// packet sees it half done. When the host goes from one format straight to
// another, TinyUSB closed the stream first (tud_audio_set_itf_close_ep_cb), and
// dropped its FIFO. The service watchdog starts with the stream; the
// completions run it. A request queued before a bus reset or unplug that the
// task has not reached yet is stale: the open is refused and the request stalls.
bool tud_audio_set_itf_cb(uint8_t rhport, const tusb_control_request_t* p_request)
{
    (void)rhport;
    const uint8_t alternate = tu_u16_low(p_request->wValue);

    if (!is_streaming_interface(p_request) || alternate == 0U)
    {
        return true;
    }

    taskENTER_CRITICAL();
    const wfg_uac_request_result_t result = wfg_usb_audio_open_stream(alternate);

    if (result == kWFG_UacRequestAccepted)
    {
        m110_usb_audio_watchdog_start();
    }

    taskEXIT_CRITICAL();
    return result == kWFG_UacRequestAccepted;
}

// Called when an open alternate is left: alternate 0, another format, or the
// same one again. TinyUSB calls it before it opens the next alternate.
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, const tusb_control_request_t* p_request)
{
    (void)rhport;

    if (is_streaming_interface(p_request))
    {
        taskENTER_CRITICAL();
        wfg_usb_audio_close_stream();
        m110_usb_audio_watchdog_stop();
        taskEXIT_CRITICAL();
    }

    return true;
}

// TinyUSB has just loaded the next packet from the FIFO and primed it; refill
// the FIFO. The service watchdog restarts from this priming. The frame number
// and tick are read here, as the completion is handled: they date the packet
// that just went out and feed the play's stall clock.
bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    m110_usb_audio_watchdog_restart();
    wfg_usb_audio_tx_done(n_bytes_sent, m110_usb_frame_number(), xTaskGetTickCountFromISR());
    return true;
}

// The stream ends with its configuration: closed, the service watchdog
// stopped. The caller masks the USB IRQ or is the USB ISR.
static void end_stream_configuration(void)
{
    wfg_usb_audio_deconfigure_stream();
    m110_usb_audio_watchdog_stop();
}

// TinyUSB sends no interface callback on bus reset or unplug, so the stream is
// closed here, as the event is queued, and cannot reopen until the next
// SET_CONFIGURATION. Both come from the USB ISR; a task-context caller masks it.
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;

    if (eventid != DCD_EVENT_BUS_RESET && eventid != DCD_EVENT_UNPLUGGED)
    {
        return;
    }

    if (in_isr)
    {
        end_stream_configuration();
    }
    else
    {
        taskENTER_CRITICAL();
        end_stream_configuration();
        taskEXIT_CRITICAL();
    }
}

// SET_CONFIGURATION other than 0, in the wfg_usb task: a new configuration
// starts with the stream closed, and alternate 1 may open it again. A host
// configures the device again after a bus reset, so the host's MSC session (its
// removal lock and any WRITE(10) it had started) ends here too.
void tud_mount_cb(void)
{
    taskENTER_CRITICAL();
    wfg_usb_audio_configure_stream();
    taskEXIT_CRITICAL();
    WFG_MediaEndHostSession();
}

// After the status stage of every control transfer, in the wfg_usb task:
// TinyUSB's hook for a controller driver (usbd.c; its weak default does
// nothing, and the ChipIdea driver does not define one). TinyUSB's MSC driver
// answers a Bulk-Only Mass Storage Reset without an MSC callback
// (msc_device.c proc_bot_reset), so the media layer learns of it here: it
// ends a WRITE(10) the host gave up on between its chunks (#44).
void dcd_edpt0_status_complete(uint8_t rhport, tusb_control_request_t const* request)
{
    (void)rhport;
    WFG_MediaControlTransferDone(request->bmRequestType, request->bRequest, tu_u16_low(request->wIndex));
}

// SET_CONFIGURATION(0), a processed unplug, or tud_deinit, in the wfg_usb task.
// TinyUSB resets the audio driver without an interface callback, so the stream
// is closed here; the host's MSC session is over as well.
void tud_umount_cb(void)
{
    taskENTER_CRITICAL();
    end_stream_configuration();
    taskEXIT_CRITICAL();
    WFG_MediaEndHostSession();
}

uint16_t m110_tinyusb_audio_in_queued_bytes(void)
{
    const tu_fifo_t* const fifo = tud_audio_get_ep_in_ff();
    return fifo == NULL ? 0U : tu_fifo_count(fifo);
}

uint16_t m110_tinyusb_audio_in_free_bytes(void)
{
    const tu_fifo_t* const fifo = tud_audio_get_ep_in_ff();
    return fifo == NULL ? 0U : tu_fifo_remaining(fifo);
}

uint16_t m110_tinyusb_audio_in_write(const void* bytes, uint16_t count)
{
    return tud_audio_write(bytes, count);
}
