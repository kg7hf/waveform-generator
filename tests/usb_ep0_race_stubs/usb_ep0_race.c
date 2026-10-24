// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The EP0 race of #32 on the pinned TinyUSB (usb_ep0_race.h)
// Guide: docs/modules/rt1170-player-and-audio.md#ep0-event-order

#include "usb_ep0_race.h"

#include "tusb.h"
#include "device/dcd.h"
#include "usb/usb_ep0_order.h"
#include "usb/usb_ep0_order_glue.h"

#include <stdio.h>
#include <string.h>

enum
{
    race_rhport = 0U,
    cdc_interface = 0U,
    cdc_notify_endpoint = 0x81U,
    cdc_notify_bytes = 8U,
    cdc_out_endpoint = 0x02U,
    cdc_in_endpoint = 0x82U,
    cdc_bulk_bytes = 512U,
    line_coding_bytes = 7U,
    configuration_total = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN,
    // bmRequestType of a CDC class request to the interface, host to device
    // and device to host (USB 2.0 9.3.1, CDC PSTN 6.3).
    class_out_to_interface = 0x21U,
    class_in_from_interface = 0xA1U,
    dtr_and_rts = 0x0003U,
    // The bit rate the host's SET_LINE_CODING carries (921600 baud, 8N1).
    host_bit_rate = 921600U,
};

// SET_LINE_CODING's data stage as the host sends it (CDC PSTN 6.3.10).
static uint8_t const host_line_coding[line_coding_bytes] = {U32_TO_U8S_LE(host_bit_rate), 0U, 0U, 8U};

// What the stand-in driver and TinyUSB's callbacks saw, held by race(), as
// the other host stubs hold their state.
struct race_state
{
    uint32_t assert_count;
    uint32_t line_state_calls;
    uint32_t line_coding_calls;
    uint32_t ep0_primes;
    // The buffer TinyUSB last queued on EP0 OUT, which the host's data fills.
    uint8_t* ep0_out_buffer;
    uint16_t ep0_out_bytes;
    // The stand-in controller's ENDPTCOMPLETE for EP0: a completion the
    // driver has not reported yet.
    bool ep0_pending;
};

static struct race_state* race(void)
{
    static struct race_state state;
    return &state;
}

void wfg_race_count_assert(void)
{
    ++race()->assert_count;
}

// The glue's view of the controller (usb_tinyusb_port.c reads ENDPTCOMPLETE).
static bool race_ep0_pending(void)
{
    return race()->ep0_pending;
}

uint32_t tusb_time_millis_api(void)
{
    return 0U;
}

// One high-speed device with one CDC-ACM function (interfaces 0 and 1).
static uint8_t const device_descriptor[] = {
    18U, TUSB_DESC_DEVICE, 0x00U, 0x02U, TUSB_CLASS_MISC, MISC_SUBCLASS_COMMON, MISC_PROTOCOL_IAD, CFG_TUD_ENDPOINT0_SIZE,
    0xFEU, 0xCAU, 0x12U, 0x40U, 0x00U, 0x01U, 0U, 0U, 0U, 1U,
};

static uint8_t const configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, configuration_total, 0x00, 100),
    TUD_CDC_DESCRIPTOR(cdc_interface, 0, cdc_notify_endpoint, cdc_notify_bytes, cdc_out_endpoint, cdc_in_endpoint, cdc_bulk_bytes),
};

uint8_t const* tud_descriptor_device_cb(void)
{
    return device_descriptor;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration_descriptor;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)index;
    (void)langid;
    return NULL;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)dtr;
    (void)rts;
    ++race()->line_state_calls;
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding)
{
    (void)itf;
    (void)p_line_coding;
    ++race()->line_coding_calls;
}

// The ChipIdea driver's place: every transfer is accepted, and EP0's are counted.
bool dcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init)
{
    (void)rhport;
    (void)rh_init;
    return true;
}

void dcd_int_handler(uint8_t rhport)
{
    (void)rhport;
}

void dcd_int_enable(uint8_t rhport)
{
    (void)rhport;
}

void dcd_int_disable(uint8_t rhport)
{
    (void)rhport;
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
    (void)rhport;
    (void)dev_addr;
}

void dcd_remote_wakeup(uint8_t rhport)
{
    (void)rhport;
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
    (void)rhport;
    (void)en;
}

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const* desc_ep)
{
    (void)rhport;
    (void)desc_ep;
    return true;
}

void dcd_edpt_close_all(uint8_t rhport)
{
    (void)rhport;
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes, bool is_isr)
{
    (void)rhport;
    (void)is_isr;

    if (tu_edpt_number(ep_addr) == 0U)
    {
        ++race()->ep0_primes;
    }

    if (ep_addr == 0x00U)
    {
        race()->ep0_out_buffer = buffer;
        race()->ep0_out_bytes = total_bytes;
    }

    return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    (void)ep_addr;
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    (void)ep_addr;
}

bool dcd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size)
{
    (void)rhport;
    (void)ep_addr;
    (void)largest_packet_size;
    return true;
}

bool dcd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const* desc_ep)
{
    (void)rhport;
    (void)desc_ep;
    return true;
}

static dcd_event_t setup_event(uint8_t request_type, uint8_t request, uint16_t value, uint16_t length)
{
    dcd_event_t event = {.rhport = race_rhport, .event_id = DCD_EVENT_SETUP_RECEIVED};
    const tusb_control_request_t setup = {
        .bmRequestType = request_type, .bRequest = request, .wValue = value, .wIndex = cdc_interface, .wLength = length};
    event.setup_received = setup;
    return event;
}

static dcd_event_t ep0_done(uint8_t ep_addr, uint32_t bytes)
{
    dcd_event_t event = {.rhport = race_rhport, .event_id = DCD_EVENT_XFER_COMPLETE};
    event.xfer_complete.ep_addr = ep_addr;
    event.xfer_complete.result = XFER_RESULT_SUCCESS;
    event.xfer_complete.len = bytes;
    return event;
}

// The start of a USB frame, as the driver reports it last in a pass once SOF
// interrupts are on.
static dcd_event_t start_of_frame(void)
{
    dcd_event_t event = {.rhport = race_rhport, .event_id = DCD_EVENT_SOF};
    event.sof.frame_count = 0U;
    return event;
}

// One interrupt in which the driver reports `event` and nothing is pending
// after the pass: through the EP0 order, as the firmware's interrupt does.
static void interrupt(dcd_event_t const* event)
{
    wfg_usb_ep0_order_begin();
    wfg_usb_ep0_route_event(event, true, &dcd_event_handler, &race_ep0_pending);

    while (wfg_usb_ep0_order_another_pass(false))
    {
    }

    wfg_usb_ep0_release_setups(&dcd_event_handler);
    wfg_usb_ep0_order_end();
}

// A whole control transfer the host completes in order: its SETUP, its data
// stage (when it has one) and its status stage, each answered by the task.
static void control_transfer(dcd_event_t const* setup, uint8_t data_endpoint, uint32_t data_bytes, uint8_t status_endpoint)
{
    interrupt(setup);
    tud_task();

    if (data_bytes != 0U)
    {
        const dcd_event_t data = ep0_done(data_endpoint, data_bytes);
        interrupt(&data);
        tud_task();
    }

    const dcd_event_t status = ep0_done(status_endpoint, 0U);
    interrupt(&status);
    tud_task();
}

bool wfg_race_enumerate(void)
{
    wfg_usb_ep0_order_reset();
    const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};

    if (!tusb_init(race_rhport, &init))
    {
        return false;
    }

    dcd_event_t reset = {.rhport = race_rhport, .event_id = DCD_EVENT_BUS_RESET};
    reset.bus_reset.speed = TUSB_SPEED_HIGH;
    interrupt(&reset);
    tud_task();
    const dcd_event_t configure = setup_event(0x00U, TUSB_REQ_SET_CONFIGURATION, 1U, 0U);
    control_transfer(&configure, 0U, 0U, TUSB_DIR_IN_MASK);
    const dcd_event_t open = setup_event(class_out_to_interface, CDC_REQUEST_SET_CONTROL_LINE_STATE, dtr_and_rts, 0U);
    control_transfer(&open, 0U, 0U, TUSB_DIR_IN_MASK);
    return tud_mounted() && tud_cdc_connected() && race()->assert_count == 0U;
}

// How the race's interrupt reaches TinyUSB: as the pinned driver reports it,
// through the EP0 order, or through it with SOF interrupts on, so that the
// pass that reports the SETUP ends with a SOF.
enum race_route
{
    race_as_pinned,
    race_through_order,
    race_through_order_with_sof,
};

// The interrupt in which the previous transfer's last stage completed after
// the driver read ENDPTCOMPLETE and the next SETUP arrived before it read
// ENDPTSETUPSTAT: TinyUSB's pinned driver reports the SETUP in it and the
// completion in the next interrupt; the EP0 order finds the completion
// pending after the pass and reports it first.
static void race_interrupt(enum race_route route, dcd_event_t const* next_setup, dcd_event_t const* last_stage)
{
    if (route == race_as_pinned)
    {
        dcd_event_handler(next_setup, true);
        dcd_event_handler(last_stage, true);
        return;
    }

    const dcd_event_t sof = start_of_frame();
    race()->ep0_pending = true;
    wfg_usb_ep0_order_begin();
    wfg_usb_ep0_route_event(next_setup, true, &dcd_event_handler, &race_ep0_pending);

    if (route == race_through_order_with_sof)
    {
        wfg_usb_ep0_route_event(&sof, true, &dcd_event_handler, &race_ep0_pending);
    }

    while (wfg_usb_ep0_order_another_pass(race_ep0_pending()))
    {
        race()->ep0_pending = false;
        wfg_usb_ep0_route_event(last_stage, true, &dcd_event_handler, &race_ep0_pending);
    }

    wfg_usb_ep0_release_setups(&dcd_event_handler);
    wfg_usb_ep0_order_end();
}

static struct wfg_race_result run_race(enum race_route route, bool in_data)
{
    struct race_state* const seen = race();
    const uint32_t asserts_before = seen->assert_count;
    const uint32_t line_states_before = seen->line_state_calls;
    const uint32_t line_codings_before = seen->line_coding_calls;
    const uint32_t primes_before = seen->ep0_primes;
    // The first transfer: DTR off (status IN), or GET_LINE_CODING (status OUT).
    const dcd_event_t first = in_data ? setup_event(class_out_to_interface, CDC_REQUEST_SET_CONTROL_LINE_STATE, 0U, 0U)
                                      : setup_event(class_in_from_interface, CDC_REQUEST_GET_LINE_CODING, 0U, line_coding_bytes);
    interrupt(&first);
    tud_task();

    if (!in_data)
    {
        const dcd_event_t data = ep0_done(TUSB_DIR_IN_MASK, line_coding_bytes);
        interrupt(&data);
        tud_task();
    }

    // Its last stage completes, and the next SETUP follows at once: an
    // IN-data request after an IN status stage, an OUT-data request after an
    // OUT one.
    const dcd_event_t last_stage = ep0_done(in_data ? TUSB_DIR_IN_MASK : 0x00U, 0U);
    const dcd_event_t next = in_data ? setup_event(class_in_from_interface, CDC_REQUEST_GET_LINE_CODING, 0U, line_coding_bytes)
                                     : setup_event(class_out_to_interface, CDC_REQUEST_SET_LINE_CODING, 0U, line_coding_bytes);
    race_interrupt(route, &next, &last_stage);
    tud_task();

    // The next transfer's data stage (the host's line coding fills the buffer
    // TinyUSB queued for it), then its status stage.
    if (!in_data && seen->ep0_out_buffer != NULL && seen->ep0_out_bytes >= line_coding_bytes)
    {
        memcpy(seen->ep0_out_buffer, host_line_coding, sizeof(host_line_coding));
    }

    const dcd_event_t data = ep0_done(in_data ? TUSB_DIR_IN_MASK : 0x00U, line_coding_bytes);
    interrupt(&data);
    tud_task();
    const dcd_event_t status = ep0_done(in_data ? 0x00U : TUSB_DIR_IN_MASK, 0U);
    interrupt(&status);
    tud_task();
    cdc_line_coding_t coding;
    tud_cdc_get_line_coding(&coding);
    const struct wfg_race_result result = {
        .asserts = seen->assert_count - asserts_before,
        .line_state_calls = seen->line_state_calls - line_states_before,
        .dtr = tud_cdc_connected(),
        .line_coding_calls = seen->line_coding_calls - line_codings_before,
        .line_coding_bitrate = coding.bit_rate,
        .ep0_primes = seen->ep0_primes - primes_before,
    };
    return result;
}

struct wfg_race_result wfg_race_in_data(bool through_ep0_order)
{
    return run_race(through_ep0_order ? race_through_order : race_as_pinned, true);
}

struct wfg_race_result wfg_race_out_data(bool through_ep0_order)
{
    return run_race(through_ep0_order ? race_through_order : race_as_pinned, false);
}

struct wfg_race_result wfg_race_in_data_with_sof(void)
{
    return run_race(race_through_order_with_sof, true);
}

// The route sample's record of what the glue passed on, held by route().
struct route_record
{
    char* text;
    size_t capacity;
    size_t used;
};

static struct route_record* route(void)
{
    static struct route_record record;
    return &record;
}

static void record_route(dcd_event_t const* event, bool in_isr)
{
    struct route_record* const record = route();
    const size_t room = record->used < record->capacity ? record->capacity - record->used : 0U;
    const char* separator = record->used == 0U ? "" : " ";
    char* const at = record->text + record->used;
    int written = 0;

    if (event->event_id == DCD_EVENT_SETUP_RECEIVED)
    {
        written = snprintf(at, room, "%sS%02X", separator, event->setup_received.bRequest);
    }
    else if (event->event_id == DCD_EVENT_XFER_COMPLETE)
    {
        written = snprintf(at, room, "%s%c%02X", separator, in_isr ? 'C' : 'c', event->xfer_complete.ep_addr);
    }
    else if (event->event_id == DCD_EVENT_SOF)
    {
        written = snprintf(at, room, "%sF", separator);
    }
    else
    {
        written = snprintf(at, room, "%sR", separator);
    }

    if (written > 0 && (size_t)written < room)
    {
        record->used += (size_t)written;
    }
}

static dcd_event_t sample_setup(uint8_t request)
{
    return setup_event(class_in_from_interface, request, 0U, 0U);
}

void wfg_race_route_sample(char* text, size_t capacity)
{
    if (text == NULL || capacity == 0U)
    {
        return;
    }

    *route() = (struct route_record){.text = text, .capacity = capacity, .used = 0U};
    text[0] = '\0';
    // Inside one interrupt: a SETUP; an EP0 IN completion; the audio IN
    // completion; another SETUP; a bus reset while an EP0 completion is
    // pending in the controller; a third SETUP; a SOF; an EP0 OUT completion
    // from task context, then the same from the interrupt.
    const dcd_event_t first = sample_setup(0x21U);
    const dcd_event_t ep0_in = ep0_done(TUSB_DIR_IN_MASK, 0U);
    const dcd_event_t audio_in = ep0_done(0x83U, 0U);
    const dcd_event_t second = sample_setup(0x0BU);
    const dcd_event_t reset = {.rhport = race_rhport, .event_id = DCD_EVENT_BUS_RESET};
    const dcd_event_t third = sample_setup(0x06U);
    const dcd_event_t sof = start_of_frame();
    const dcd_event_t ep0_out = ep0_done(0x00U, 0U);
    race()->ep0_pending = false;
    wfg_usb_ep0_order_begin();
    wfg_usb_ep0_route_event(&first, true, &record_route, &race_ep0_pending);
    wfg_usb_ep0_route_event(&ep0_in, true, &record_route, &race_ep0_pending);
    wfg_usb_ep0_route_event(&audio_in, true, &record_route, &race_ep0_pending);
    wfg_usb_ep0_route_event(&second, true, &record_route, &race_ep0_pending);
    race()->ep0_pending = true;
    wfg_usb_ep0_route_event(&reset, true, &record_route, &race_ep0_pending);
    race()->ep0_pending = false;
    wfg_usb_ep0_route_event(&third, true, &record_route, &race_ep0_pending);
    wfg_usb_ep0_route_event(&sof, true, &record_route, &race_ep0_pending);
    wfg_usb_ep0_route_event(&ep0_out, false, &record_route, &race_ep0_pending);
    wfg_usb_ep0_route_event(&ep0_out, true, &record_route, &race_ep0_pending);
    wfg_usb_ep0_release_setups(&record_route);
    wfg_usb_ep0_order_end();
    // Outside the interrupt's passes nothing is held.
    wfg_usb_ep0_route_event(&first, true, &record_route, &race_ep0_pending);
}

void wfg_race_route_full_hold(char* text, size_t capacity)
{
    // The hold keeps three SETUPs (usb_ep0_order.hpp's hold_capacity).
    const uint8_t setups_past_the_hold = 4U;

    if (text == NULL || capacity == 0U)
    {
        return;
    }

    *route() = (struct route_record){.text = text, .capacity = capacity, .used = 0U};
    text[0] = '\0';
    race()->ep0_pending = false;
    wfg_usb_ep0_order_begin();

    for (uint8_t request = 1U; request <= setups_past_the_hold; ++request)
    {
        const dcd_event_t setup = sample_setup(request);
        wfg_usb_ep0_route_event(&setup, true, &record_route, &race_ep0_pending);
    }

    wfg_usb_ep0_release_setups(&record_route);
    wfg_usb_ep0_order_end();
}
