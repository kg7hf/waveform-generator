// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The player's composite USB device and the sound-card wire contract
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: constants only, shared by the descriptors (C), the TinyUSB glue (C),
// the portable UAC2 logic (rt1170/usb_audio.hpp) and INFO (rt1170/usb_service.cpp).
// Contract: every value here is visible on the wire. The player image is always
// one composite device: CDC on interfaces 0/1, MSC on 2 and UAC2 capture on 3/4.
// tusb_config.h repeats the packet and FIFO sizes as preprocessor literals;
// usb_audio_tinyusb.c checks them, and the UAC2 codes, against TinyUSB at
// compile time, and usb_descriptors.c checks the numbering.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Device identity. TinyUSB's development VID is suitable for this hardware
// spike only; a project-owned VID/PID is a productization gate. bcdDevice moves
// on whenever the player's descriptor set changes: a new hardware ID, so
// Windows reads the changed descriptors instead of a cached set. 0x0101: the
// UAC2 function joined CDC + MSC. 0x0102: the streaming interface gained its
// 16-bit stereo alternate setting; 0x0103 is the same set with the mono
// alternate as well (WFG_UAC_MONO_ALT), so the two never share an ID.
enum
{
    wfg_usb_vendor_id = 0xCAFE,
    wfg_usb_product_id_fixture = 0x4011,          // scaffold and tone images: CDC only
    wfg_usb_product_id_player = 0x4012,           // player: CDC + MSC + UAC2 capture
    wfg_usb_product_id_player_read_only = 0x4013, // the same functions, MSC writes refused
    wfg_usb_bcd_device_fixture = 0x0100,
    wfg_usb_bcd_device_player_stereo_formats = 0x0102,
    wfg_usb_bcd_device_player_mono_format = 0x0103,
    // The player image's: its descriptor set has the mono alternate or not.
#if defined(WFG_UAC_MONO_ALT)
    wfg_usb_bcd_device_player = wfg_usb_bcd_device_player_mono_format,
#else
    wfg_usb_bcd_device_player = wfg_usb_bcd_device_player_stereo_formats,
#endif
};

// The PID a player image enumerates with; INFO reports it as usb_pid.
#if defined(WFG_MSC_READ_ONLY)
enum
{
    wfg_usb_product_id_player_image = wfg_usb_product_id_player_read_only,
};
#else
enum
{
    wfg_usb_product_id_player_image = wfg_usb_product_id_player,
};
#endif

// Interface and endpoint numbering of the player's composite device, and the
// MSC class request the firmware watches for. The fixture images use the CDC
// part only.
enum
{
    wfg_usb_interface_cdc_control = 0,
    wfg_usb_interface_cdc_data = 1,
    wfg_usb_interface_msc = 2,
    wfg_uac_interface_control = 3,
    wfg_uac_interface_streaming = 4,
    wfg_usb_player_interface_count = 5,
    wfg_usb_endpoint_cdc_notification = 0x81,
    wfg_usb_endpoint_cdc_out = 0x02,
    wfg_usb_endpoint_cdc_in = 0x82,
    wfg_usb_endpoint_msc_out = 0x03,
    wfg_usb_endpoint_msc_in = 0x83,
    wfg_uac_endpoint_in = 0x84,
    // The Bulk-Only Mass Storage Reset (USB Mass Storage Class Bulk-Only
    // Transport 1.0, 3.1): a class request to the MSC interface, host to
    // device (bmRequestType 0x21 by USB 2.0 Table 9-2), bRequest 0xFF. The
    // host sends it as the first step of reset recovery, after it gives up on
    // a command. usb_audio_tinyusb.c checks both against TinyUSB.
    wfg_usb_request_type_class_interface_out = 0x21,
    wfg_msc_request_bot_reset = 0xFF,
};

// The UAC2 function and its stream formats, one per alternate setting of the
// streaming interface (alternate 0 has no endpoint). Every format is 48 kHz,
// one 48-frame packet per USB millisecond on the same isochronous IN endpoint;
// they differ in the samples' width and the channel count:
// - alternate 1, the first and the default: stereo, 24-bit samples
//   left-justified in 4-byte subslots (sample << 8), 384-byte packets;
// - alternate 2: stereo, 16-bit samples in 2-byte subslots, each the 24-bit
//   sample's top 16 bits (sample >> 8, truncated), 192-byte packets;
// - alternate 3, only in a WFG_UAC_MONO_ALT build: the left channel alone,
//   16-bit as alternate 2, 96-byte packets.
// The function's channel cluster (input terminal 1, feature unit 2) stays
// stereo, front left + right. Codes cite the USB Audio 2.0 specification,
// appendix A.
enum
{
    wfg_uac_alternate_pcm24_stereo = 1,
    wfg_uac_alternate_pcm16_stereo = 2,
    wfg_uac_alternate_pcm16_mono = 3,
    // A player image offers alternates 1 to wfg_uac_last_alternate. The mono
    // alternate is a build option (WFG_UAC_MONO_ALT, default off): whether
    // Windows' usbaudio2 takes a one-channel alternate beside the stereo ones,
    // on an interface whose terminal cluster is stereo, is a bench question
    // (docs/modules/protocols-media-and-artifacts.md).
#if defined(WFG_UAC_MONO_ALT)
    wfg_uac_last_alternate = wfg_uac_alternate_pcm16_mono,
#else
    wfg_uac_last_alternate = wfg_uac_alternate_pcm16_stereo,
#endif
    wfg_uac_entity_input_terminal = 1,
    wfg_uac_entity_feature_unit = 2,
    wfg_uac_entity_output_terminal = 3,
    wfg_uac_entity_clock_source = 4,
    wfg_uac_sample_rate_hz = 48000,
    wfg_uac_channels = 2, // the channel cluster, and the stereo formats
    wfg_uac_mono_channels = 1,
    wfg_uac_pcm24_subslot_bytes = 4,
    wfg_uac_pcm24_resolution_bits = 24,
    wfg_uac_pcm16_subslot_bytes = 2,
    wfg_uac_pcm16_resolution_bits = 16,
    wfg_uac_packet_frames = 48,
    wfg_uac_pcm24_stereo_packet_bytes = wfg_uac_packet_frames * wfg_uac_channels * wfg_uac_pcm24_subslot_bytes,
    wfg_uac_pcm16_stereo_packet_bytes = wfg_uac_packet_frames * wfg_uac_channels * wfg_uac_pcm16_subslot_bytes,
    wfg_uac_pcm16_mono_packet_bytes = wfg_uac_packet_frames * wfg_uac_mono_channels * wfg_uac_pcm16_subslot_bytes,
    // The largest packet, alternate 1's: TinyUSB's endpoint buffer and FIFO size.
    wfg_uac_max_packet_bytes = wfg_uac_pcm24_stereo_packet_bytes,
    wfg_uac_fifo_bytes = 4 * wfg_uac_max_packet_bytes,
    wfg_uac_request_cur = 0x01,          // A.14 CUR
    wfg_uac_request_range = 0x02,        // A.14 RANGE
    wfg_uac_clock_frequency = 0x01,      // A.17.1 CS_SAM_FREQ_CONTROL
    wfg_uac_clock_validity = 0x02,       // A.17.1 CS_CLOCK_VALID_CONTROL
    wfg_uac_feature_mute = 0x01,         // A.17.7 FU_MUTE_CONTROL
    wfg_uac_feature_volume = 0x02,       // A.17.7 FU_VOLUME_CONTROL
    wfg_uac_master_channel = 0,
    // Longest GET answer: a one-subrange 4-byte RANGE (2 + 3 * 4 bytes).
    wfg_uac_control_response_max_bytes = 14,
};

#ifdef __cplusplus
}
#endif
