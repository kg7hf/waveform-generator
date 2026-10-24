// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// USB descriptors: the player's composite device and the fixture's CDC device
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: TinyUSB runs every descriptor callback in the wfg_usb task.
// Contract: the player image (PID 0x4012, read-only 0x4013) is always one
// composite device: CDC on interfaces 0/1, MSC on 2 and UAC2 capture on 3/4
// (usb/usb_composite.h). The scaffold and tone images (PID 0x4011) present the
// CDC function only. Every image reports the OCOTP serial number.

#include "tusb.h"
#include "usb/usb_composite.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/device_identity.h"
#if defined(CPU_MIMXRT1064DVL6A)
#include "MIMXRT1064.h"
#else
#include "MIMXRT1176_cm7.h"
#endif

#if defined(WFG_PLAYER_IMAGE) && !defined(M110_USB_HIGH_SPEED)
#error "The player's composite device is defined for the high-speed controller build"
#endif

enum
{
    string_index_manufacturer = 1,
    string_index_product = 2,
    string_index_serial = 3,
    string_index_cdc = 4,
    string_index_msc = 5,
    string_index_audio_function = 6,
    string_max_chars = 32,
};

// TinyUSB takes every descriptor as bytes. A const void* converts implicitly
// in C, so typed descriptors reach it without a cast.
static const uint8_t* descriptor_bytes(const void* descriptor)
{
    return descriptor;
}

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = wfg_usb_vendor_id,
#if defined(WFG_PLAYER_IMAGE)
    .idProduct = wfg_usb_product_id_player_image,
    .bcdDevice = wfg_usb_bcd_device_player,
#else
    .idProduct = wfg_usb_product_id_fixture,
    .bcdDevice = wfg_usb_bcd_device_fixture,
#endif
    .iManufacturer = string_index_manufacturer,
    .iProduct = string_index_product,
    .iSerialNumber = string_index_serial,
    .bNumConfigurations = 1,
};

const uint8_t* tud_descriptor_device_cb(void)
{
    return descriptor_bytes(&device_descriptor);
}

#if defined(WFG_PLAYER_IMAGE)
// The UAC2 capture function. Signal path: input terminal 1 (line connector,
// front left + right) -> feature unit 2 (master mute and volume, stored but
// never applied) -> output terminal 3 (USB streaming), all clocked by clock
// source 4, an internal fixed 48 kHz clock synchronized to SOF. The streaming
// interface has alternate 0 (no endpoint) and one alternate per format, in
// ascending order, each Type I PCM on synchronous isochronous IN 0x84 with its
// own wMaxPacketSize: 1, the first, 24 bits in 4-byte subslots, stereo; 2, 16
// bits in 2-byte subslots, stereo; 3 (WFG_UAC_MONO_ALT only), 16 bits, one
// channel (usb/usb_composite.h).
enum
{
    uac_bcd_adc = 0x0200,
    uac_terminal_line_connector = 0x0603, // Termt20 2.4, external terminal types
    uac_channel_config = AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT,
    // The mono alternate's single channel: front centre, which Windows calls
    // mono. It carries the cluster's left channel.
    uac_mono_channel_config = AUDIO20_CHANNEL_CONFIG_FRONT_CENTER,
    uac_clock_attributes = AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK | AUDIO20_CLOCK_SOURCE_ATT_CLK_SYC_SOF,
    uac_clock_controls = (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS) |
                         (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_VAL_POS),
    uac_master_controls = (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS) |
                          (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS),
    uac_endpoint_attributes = TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_SYNCHRONOUS | TUSB_ISO_EP_ATT_DATA,
    // One packet per millisecond: every frame at FS, every 2^(4-1) microframes at HS.
    uac_full_speed_interval = 1,
    uac_high_speed_interval = 4,
    uac_control_length = TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN +
                         TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(wfg_uac_channels) + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN,
    // An alternate that carries a format: its interface, AS general, format
    // type, endpoint and class-specific endpoint descriptors.
    uac_format_alternate_length = TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN +
                                  TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN +
                                  TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN,
    uac_function_length = TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN +
                          uac_control_length + TUD_AUDIO20_DESC_STD_AS_LEN +
                          wfg_uac_last_alternate * uac_format_alternate_length,
    interface_count = wfg_usb_player_interface_count,
    configuration_length = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + uac_function_length,
    // bEndpointAddress bits 3..0; bit 7 is the direction.
    endpoint_number_mask = 0x0F,
};

_Static_assert(wfg_usb_interface_cdc_data == wfg_usb_interface_cdc_control + 1, "CDC data follows CDC control");
_Static_assert(wfg_usb_interface_msc == wfg_usb_interface_cdc_data + 1, "MSC follows the CDC interfaces");
_Static_assert(wfg_uac_interface_control == wfg_usb_interface_msc + 1, "audio control follows MSC");
_Static_assert(wfg_uac_interface_streaming == wfg_uac_interface_control + 1, "audio streaming follows audio control");
_Static_assert(interface_count == wfg_uac_interface_streaming + 1, "bNumInterfaces counts every interface");
_Static_assert((wfg_usb_endpoint_cdc_notification & endpoint_number_mask) < (wfg_usb_endpoint_cdc_in & endpoint_number_mask) &&
               (wfg_usb_endpoint_cdc_in & endpoint_number_mask) < (wfg_usb_endpoint_msc_in & endpoint_number_mask) &&
               (wfg_usb_endpoint_msc_in & endpoint_number_mask) < (wfg_uac_endpoint_in & endpoint_number_mask),
               "each function has its own endpoint numbers");
_Static_assert((wfg_usb_endpoint_cdc_out & endpoint_number_mask) == (wfg_usb_endpoint_cdc_in & endpoint_number_mask) &&
               (wfg_usb_endpoint_msc_out & endpoint_number_mask) == (wfg_usb_endpoint_msc_in & endpoint_number_mask),
               "bulk OUT and IN of a function share their endpoint number");
_Static_assert((wfg_uac_endpoint_in & endpoint_number_mask) < TUP_DCD_ENDPOINT_MAX, "the controller has the audio endpoint");
_Static_assert(uac_channel_config == 0x3, "FL | FR");
_Static_assert(uac_mono_channel_config == 0x4, "FC");
_Static_assert(wfg_uac_alternate_pcm24_stereo == 1 && wfg_uac_alternate_pcm16_stereo == 2 && wfg_uac_alternate_pcm16_mono == 3,
               "alternates follow alternate 0 in ascending order, 24-bit stereo first");
_Static_assert(wfg_uac_last_alternate == wfg_uac_alternate_pcm16_stereo || wfg_uac_last_alternate == wfg_uac_alternate_pcm16_mono,
               "the 24- and 16-bit stereo formats always, the mono one by option");
_Static_assert(uac_clock_attributes == 0x05, "internal fixed clock synchronized to SOF");
_Static_assert(uac_clock_controls == 0x05, "frequency and validity read-only");
_Static_assert(uac_master_controls == 0x0F, "master mute and volume host-programmable");
_Static_assert(uac_endpoint_attributes == 0x0D, "synchronous isochronous data endpoint");
_Static_assert((1U << (uac_high_speed_interval - 1)) == 8U, "HS packets every 8 microframes");

// Full- and high-speed copies differ only in the bulk packet sizes and the
// isochronous bInterval.
static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, interface_count, 0, configuration_length, 0, 100),
    TUD_CDC_DESCRIPTOR(wfg_usb_interface_cdc_control, string_index_cdc, wfg_usb_endpoint_cdc_notification, 16,
                       wfg_usb_endpoint_cdc_out, wfg_usb_endpoint_cdc_in, 64),
    TUD_MSC_DESCRIPTOR(wfg_usb_interface_msc, string_index_msc, wfg_usb_endpoint_msc_out, wfg_usb_endpoint_msc_in, 64),
    TUD_AUDIO20_DESC_IAD(wfg_uac_interface_control, 2, string_index_audio_function),
    TUD_AUDIO20_DESC_STD_AC(wfg_uac_interface_control, 0, string_index_audio_function),
    TUD_AUDIO20_DESC_CS_AC(uac_bcd_adc, AUDIO20_FUNC_IO_BOX, uac_control_length, 0),
    TUD_AUDIO20_DESC_CLK_SRC(wfg_uac_entity_clock_source, uac_clock_attributes, uac_clock_controls, 0, 0),
    TUD_AUDIO20_DESC_INPUT_TERM(wfg_uac_entity_input_terminal, uac_terminal_line_connector, 0,
                                wfg_uac_entity_clock_source, wfg_uac_channels, uac_channel_config, 0, 0, 0),
    TUD_AUDIO20_DESC_FEATURE_UNIT(wfg_uac_entity_feature_unit, wfg_uac_entity_input_terminal, 0,
                                  uac_master_controls, 0, 0),
    TUD_AUDIO20_DESC_OUTPUT_TERM(wfg_uac_entity_output_terminal, AUDIO_TERM_TYPE_USB_STREAMING, 0,
                                 wfg_uac_entity_feature_unit, wfg_uac_entity_clock_source, 0, 0),
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, 0, 0, 0),
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, wfg_uac_alternate_pcm24_stereo, 1, 0),
    TUD_AUDIO20_DESC_CS_AS_INT(wfg_uac_entity_output_terminal, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, wfg_uac_channels, uac_channel_config, 0),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(wfg_uac_pcm24_subslot_bytes, wfg_uac_pcm24_resolution_bits),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(wfg_uac_endpoint_in, uac_endpoint_attributes, wfg_uac_pcm24_stereo_packet_bytes,
                                   uac_full_speed_interval),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, wfg_uac_alternate_pcm16_stereo, 1, 0),
    TUD_AUDIO20_DESC_CS_AS_INT(wfg_uac_entity_output_terminal, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, wfg_uac_channels, uac_channel_config, 0),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(wfg_uac_pcm16_subslot_bytes, wfg_uac_pcm16_resolution_bits),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(wfg_uac_endpoint_in, uac_endpoint_attributes, wfg_uac_pcm16_stereo_packet_bytes,
                                   uac_full_speed_interval),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
#if defined(WFG_UAC_MONO_ALT)
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, wfg_uac_alternate_pcm16_mono, 1, 0),
    TUD_AUDIO20_DESC_CS_AS_INT(wfg_uac_entity_output_terminal, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, wfg_uac_mono_channels, uac_mono_channel_config, 0),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(wfg_uac_pcm16_subslot_bytes, wfg_uac_pcm16_resolution_bits),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(wfg_uac_endpoint_in, uac_endpoint_attributes, wfg_uac_pcm16_mono_packet_bytes,
                                   uac_full_speed_interval),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
#endif
};

static const uint8_t high_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, interface_count, 0, configuration_length, 0, 100),
    TUD_CDC_DESCRIPTOR(wfg_usb_interface_cdc_control, string_index_cdc, wfg_usb_endpoint_cdc_notification, 16,
                       wfg_usb_endpoint_cdc_out, wfg_usb_endpoint_cdc_in, 512),
    TUD_MSC_DESCRIPTOR(wfg_usb_interface_msc, string_index_msc, wfg_usb_endpoint_msc_out, wfg_usb_endpoint_msc_in, 512),
    TUD_AUDIO20_DESC_IAD(wfg_uac_interface_control, 2, string_index_audio_function),
    TUD_AUDIO20_DESC_STD_AC(wfg_uac_interface_control, 0, string_index_audio_function),
    TUD_AUDIO20_DESC_CS_AC(uac_bcd_adc, AUDIO20_FUNC_IO_BOX, uac_control_length, 0),
    TUD_AUDIO20_DESC_CLK_SRC(wfg_uac_entity_clock_source, uac_clock_attributes, uac_clock_controls, 0, 0),
    TUD_AUDIO20_DESC_INPUT_TERM(wfg_uac_entity_input_terminal, uac_terminal_line_connector, 0,
                                wfg_uac_entity_clock_source, wfg_uac_channels, uac_channel_config, 0, 0, 0),
    TUD_AUDIO20_DESC_FEATURE_UNIT(wfg_uac_entity_feature_unit, wfg_uac_entity_input_terminal, 0,
                                  uac_master_controls, 0, 0),
    TUD_AUDIO20_DESC_OUTPUT_TERM(wfg_uac_entity_output_terminal, AUDIO_TERM_TYPE_USB_STREAMING, 0,
                                 wfg_uac_entity_feature_unit, wfg_uac_entity_clock_source, 0, 0),
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, 0, 0, 0),
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, wfg_uac_alternate_pcm24_stereo, 1, 0),
    TUD_AUDIO20_DESC_CS_AS_INT(wfg_uac_entity_output_terminal, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, wfg_uac_channels, uac_channel_config, 0),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(wfg_uac_pcm24_subslot_bytes, wfg_uac_pcm24_resolution_bits),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(wfg_uac_endpoint_in, uac_endpoint_attributes, wfg_uac_pcm24_stereo_packet_bytes,
                                   uac_high_speed_interval),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, wfg_uac_alternate_pcm16_stereo, 1, 0),
    TUD_AUDIO20_DESC_CS_AS_INT(wfg_uac_entity_output_terminal, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, wfg_uac_channels, uac_channel_config, 0),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(wfg_uac_pcm16_subslot_bytes, wfg_uac_pcm16_resolution_bits),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(wfg_uac_endpoint_in, uac_endpoint_attributes, wfg_uac_pcm16_stereo_packet_bytes,
                                   uac_high_speed_interval),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
#if defined(WFG_UAC_MONO_ALT)
    TUD_AUDIO20_DESC_STD_AS_INT(wfg_uac_interface_streaming, wfg_uac_alternate_pcm16_mono, 1, 0),
    TUD_AUDIO20_DESC_CS_AS_INT(wfg_uac_entity_output_terminal, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, wfg_uac_mono_channels, uac_mono_channel_config, 0),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(wfg_uac_pcm16_subslot_bytes, wfg_uac_pcm16_resolution_bits),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(wfg_uac_endpoint_in, uac_endpoint_attributes, wfg_uac_pcm16_mono_packet_bytes,
                                   uac_high_speed_interval),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
#endif
};
#else
// The fixture's CDC function alone, on the composite's CDC numbering.
enum
{
    interface_count = wfg_usb_interface_cdc_data + 1,
    configuration_length = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN,
};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, interface_count, 0, configuration_length, 0, 100),
    TUD_CDC_DESCRIPTOR(wfg_usb_interface_cdc_control, string_index_cdc, wfg_usb_endpoint_cdc_notification, 16,
                       wfg_usb_endpoint_cdc_out, wfg_usb_endpoint_cdc_in, 64),
};

#if defined(M110_USB_HIGH_SPEED)
static const uint8_t high_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, interface_count, 0, configuration_length, 0, 100),
    TUD_CDC_DESCRIPTOR(wfg_usb_interface_cdc_control, string_index_cdc, wfg_usb_endpoint_cdc_notification, 16,
                       wfg_usb_endpoint_cdc_out, wfg_usb_endpoint_cdc_in, 512),
};
#endif
#endif

_Static_assert(sizeof(configuration_descriptor) == configuration_length, "full-speed configuration length");

#if defined(M110_USB_HIGH_SPEED)
_Static_assert(sizeof(high_speed_configuration_descriptor) == configuration_length, "FS and HS configurations have equal length");

static const tusb_desc_device_qualifier_t device_qualifier_descriptor = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

// The configuration for one bus speed; both have configuration_length bytes.
static const uint8_t* speed_configuration(bool high_speed)
{
    return high_speed ? high_speed_configuration_descriptor : configuration_descriptor;
}
#endif

const uint8_t* tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
#if defined(M110_USB_HIGH_SPEED)
    return speed_configuration(tud_speed_get() == TUSB_SPEED_HIGH);
#else
    return configuration_descriptor;
#endif
}

#if defined(M110_USB_HIGH_SPEED)
const uint8_t* tud_descriptor_device_qualifier_cb(void)
{
    return descriptor_bytes(&device_qualifier_descriptor);
}

// The other speed's configuration, rebuilt on request with its descriptor type.
// TinyUSB sends it from this buffer after the callback returns.
const uint8_t* tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    static uint8_t other_speed_configuration_descriptor[configuration_length];
    _Static_assert(sizeof(other_speed_configuration_descriptor) == sizeof(high_speed_configuration_descriptor),
                   "the other-speed buffer holds either configuration");
    (void)index;
    const uint8_t* const source = speed_configuration(tud_speed_get() != TUSB_SPEED_HIGH);
    memcpy(other_speed_configuration_descriptor, source, sizeof(other_speed_configuration_descriptor));
    other_speed_configuration_descriptor[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return other_speed_configuration_descriptor;
}
#endif

static const char* const string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "Waveform Generator",
#if defined(WFG_PLAYER_IMAGE)
#if defined(WFG_MSC_READ_ONLY)
    WFG_BOARD_LABEL " Waveform Player RO",
#else
    WFG_BOARD_LABEL " Waveform Player",
#endif
#else
    WFG_BOARD_LABEL " Waveform Fixture",
#endif
    NULL,
    "Waveform Fixture CDC",
#if defined(WFG_MSC_READ_ONLY)
    "Waveform microSD read-only",
#else
    "Waveform microSD LUN",
#endif
#if defined(WFG_PLAYER_IMAGE)
    // iFunction and AC iInterface of the UAC2 function. Windows may name the
    // capture endpoint after it ("Line (RT1170 Player)"), so it stays stable.
    WFG_BOARD_LABEL " Player",
#endif
};

#if defined(WFG_PLAYER_IMAGE)
_Static_assert(sizeof(string_descriptors) / sizeof(string_descriptors[0]) == string_index_audio_function + 1,
               "the audio function string has its index");
#endif

static size_t append_hex_word(uint16_t* output, uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    for (size_t nibble = 0; nibble < 8; ++nibble)
    {
        const uint32_t shift = (uint32_t)((7U - nibble) * 4U);
        output[nibble] = (uint16_t)digits[(value >> shift) & 0x0FU];
    }

    return 8;
}

static size_t write_serial(uint16_t* output)
{
    size_t count = append_hex_word(output, WFG_SERIAL_WORD0);
    count += append_hex_word(output + count, WFG_SERIAL_WORD1);
    return count;
}

// TinyUSB sends the string from string_descriptor after the callback returns.
const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t language_id)
{
    static uint16_t string_descriptor[string_max_chars + 1];
    (void)language_id;
    size_t count = 0;

    if (index == 0)
    {
        memcpy(&string_descriptor[1], string_descriptors[0], 2);
        count = 1;
    }
    else if (index == string_index_serial)
    {
        count = write_serial(&string_descriptor[1]);
    }
    else
    {
        const size_t descriptor_count = sizeof(string_descriptors) / sizeof(string_descriptors[0]);

        if (index >= descriptor_count || string_descriptors[index] == NULL)
        {
            return NULL;
        }

        const char* const source = string_descriptors[index];
        count = strlen(source);

        if (count > string_max_chars)
        {
            count = string_max_chars;
        }

        for (size_t character = 0; character < count; ++character)
        {
            string_descriptor[1 + character] = (uint16_t)source[character];
        }
    }

    string_descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * count + 2U));
    return string_descriptor;
}
