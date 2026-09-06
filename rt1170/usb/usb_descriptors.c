#include "tusb.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MIMXRT1176_cm7.h"

// TinyUSB's development VID is suitable for this hardware spike only. A
// project-owned VID/PID is a productization gate before distribution.
enum
{
    usb_vendor_id = 0xCAFE,
    usb_product_id_cdc = 0x4011,
    usb_product_id_player = 0x4012,
    usb_product_id_player_read_only = 0x4013,
    usb_bcd_device = 0x0100,
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = usb_vendor_id,
    .idProduct =
#if defined(WFG_PLAYER_IMAGE)
#if defined(WFG_MSC_READ_ONLY)
        usb_product_id_player_read_only,
#else
        usb_product_id_player,
#endif
#else
        usb_product_id_cdc,
#endif
    .bcdDevice = usb_bcd_device,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t* tud_descriptor_device_cb(void)
{
    return (const uint8_t*)&device_descriptor;
}

enum
{
    interface_cdc_control = 0,
    interface_cdc_data,
#if defined(WFG_PLAYER_IMAGE)
    interface_msc,
#endif
    interface_count,
    endpoint_cdc_notification = 0x81,
    endpoint_cdc_out = 0x02,
    endpoint_cdc_in = 0x82,
    endpoint_msc_out = 0x03,
    endpoint_msc_in = 0x83,
#if defined(WFG_PLAYER_IMAGE)
    configuration_length = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN +
                           TUD_MSC_DESC_LEN,
#else
    configuration_length = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN,
#endif
};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, interface_count, 0, configuration_length, 0, 100),
    TUD_CDC_DESCRIPTOR(interface_cdc_control, 4, endpoint_cdc_notification, 16,
                       endpoint_cdc_out, endpoint_cdc_in, 64),
#if defined(WFG_PLAYER_IMAGE)
    TUD_MSC_DESCRIPTOR(interface_msc, 5, endpoint_msc_out, endpoint_msc_in, 64),
#endif
};

#if defined(M110_USB_HIGH_SPEED)
static const uint8_t high_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, interface_count, 0, configuration_length, 0, 100),
    TUD_CDC_DESCRIPTOR(interface_cdc_control, 4, endpoint_cdc_notification, 16,
                       endpoint_cdc_out, endpoint_cdc_in, 512),
#if defined(WFG_PLAYER_IMAGE)
    TUD_MSC_DESCRIPTOR(interface_msc, 5, endpoint_msc_out, endpoint_msc_in, 512),
#endif
};

static uint8_t other_speed_configuration_descriptor[configuration_length];

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
#endif

const uint8_t* tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
#if defined(M110_USB_HIGH_SPEED)
    return tud_speed_get() == TUSB_SPEED_HIGH ? high_speed_configuration_descriptor :
                                               configuration_descriptor;
#else
    return configuration_descriptor;
#endif
}

#if defined(M110_USB_HIGH_SPEED)
const uint8_t* tud_descriptor_device_qualifier_cb(void)
{
    return (const uint8_t*)&device_qualifier_descriptor;
}

const uint8_t* tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    const uint8_t* const source = tud_speed_get() == TUSB_SPEED_HIGH ?
                                  configuration_descriptor :
                                  high_speed_configuration_descriptor;
    memcpy(other_speed_configuration_descriptor, source, configuration_length);
    other_speed_configuration_descriptor[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return other_speed_configuration_descriptor;
}
#endif

static const char* const string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "Waveform Generator",
#if defined(WFG_PLAYER_IMAGE)
#if defined(WFG_MSC_READ_ONLY)
    "RT1170 Waveform Player RO",
#else
    "RT1170 Waveform Player",
#endif
#else
    "RT1170 Waveform Fixture",
#endif
    NULL,
    "Waveform Fixture CDC",
#if defined(WFG_MSC_READ_ONLY)
    "Waveform microSD read-only",
#else
    "Waveform microSD LUN",
#endif
};

static uint16_t string_descriptor[33];

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
    size_t count = append_hex_word(output, OCOTP->FUSEN[1].FUSE);
    count += append_hex_word(output + count, OCOTP->FUSEN[2].FUSE);
    return count;
}

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t language_id)
{
    (void)language_id;
    size_t count = 0;

    if (index == 0)
    {
        memcpy(&string_descriptor[1], string_descriptors[0], 2);
        count = 1;
    }
    else if (index == 3)
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

        if (count > 32U)
        {
            count = 32U;
        }

        for (size_t character = 0; character < count; ++character)
        {
            string_descriptor[1 + character] = (uint16_t)source[character];
        }
    }

    string_descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * count + 2U));
    return string_descriptor;
}
