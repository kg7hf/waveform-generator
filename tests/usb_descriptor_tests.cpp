// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The player's composite USB device exactly as rt1170/usb/usb_descriptors.c
// presents it, read back on the host against the USB 2.0 and USB Audio 2.0
// layouts: device identity, the five interfaces and their numbering, both
// interface associations, unique endpoints, the UAC2 topology, the capture
// formats of the streaming interface's alternates (and the rules Windows'
// usbaudio2 sets for them), what may differ between full and high speed, the
// other-speed configuration and the strings. Built once per player PID
// (writable and read-only), and once more with the mono alternate
// (WFG_UAC_MONO_ALT, writable PID). Enumeration by a real host remains a bench
// check.
// Guide: docs/modules/protocols-media-and-artifacts.md

#include "usb/usb_composite.h"
#include "usb_descriptor_bus.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

extern "C"
{
    const std::uint8_t* tud_descriptor_device_cb();
    const std::uint8_t* tud_descriptor_configuration_cb(std::uint8_t index);
    const std::uint8_t* tud_descriptor_device_qualifier_cb();
    const std::uint8_t* tud_descriptor_other_speed_configuration_cb(std::uint8_t index);
    const std::uint16_t* tud_descriptor_string_cb(std::uint8_t index, std::uint16_t language_id);
}

namespace
{
using Bytes = std::span<const std::uint8_t>;

// USB 2.0 table 9-5 and the class-specific types of the USB Audio 2.0 specification.
constexpr std::uint8_t type_device = 0x01U;
constexpr std::uint8_t type_configuration = 0x02U;
constexpr std::uint8_t type_string = 0x03U;
constexpr std::uint8_t type_interface = 0x04U;
constexpr std::uint8_t type_endpoint = 0x05U;
constexpr std::uint8_t type_device_qualifier = 0x06U;
constexpr std::uint8_t type_other_speed_configuration = 0x07U;
constexpr std::uint8_t type_interface_association = 0x0BU;
constexpr std::uint8_t type_class_interface = 0x24U;

// Field offsets (USB 2.0 tables 9-8, 9-10, 9-12, 9-13 and the IAD ECN).
constexpr std::size_t header_bytes = 2U;
constexpr std::size_t device_bytes = 18U;
constexpr std::size_t device_usb_version = 2U;
constexpr std::size_t device_class = 4U;
constexpr std::size_t device_subclass = 5U;
constexpr std::size_t device_protocol = 6U;
constexpr std::size_t device_vendor = 8U;
constexpr std::size_t device_product = 10U;
constexpr std::size_t device_release = 12U;
constexpr std::size_t device_serial_string = 16U;
constexpr std::size_t device_configurations = 17U;
constexpr std::size_t qualifier_bytes = 10U;
constexpr std::size_t qualifier_configurations = 8U;
constexpr std::size_t configuration_bytes = 9U;
constexpr std::size_t configuration_total_length = 2U;
constexpr std::size_t configuration_interfaces = 4U;
constexpr std::size_t interface_number = 2U;
constexpr std::size_t interface_alternate = 3U;
constexpr std::size_t interface_endpoints = 4U;
constexpr std::size_t interface_class = 5U;
constexpr std::size_t interface_subclass = 6U;
constexpr std::size_t interface_string = 8U;
constexpr std::size_t endpoint_address = 2U;
constexpr std::size_t endpoint_attributes = 3U;
constexpr std::size_t endpoint_max_packet = 4U;
constexpr std::size_t endpoint_interval = 6U;
constexpr std::size_t association_first = 2U;
constexpr std::size_t association_count = 3U;
constexpr std::size_t association_class = 4U;
constexpr std::size_t association_string = 7U;
constexpr std::size_t class_subtype = 2U;

// USB Audio 2.0 class-specific descriptors (tables 4-5 to 4-13, 4-27; Frmts20 2.3.1.6).
constexpr std::uint8_t subtype_ac_header = 0x01U;
constexpr std::uint8_t subtype_input_terminal = 0x02U;
constexpr std::uint8_t subtype_output_terminal = 0x03U;
constexpr std::uint8_t subtype_feature_unit = 0x06U;
constexpr std::uint8_t subtype_clock_source = 0x0AU;
constexpr std::uint8_t subtype_as_general = 0x01U;
constexpr std::uint8_t subtype_format_type = 0x02U;
constexpr std::size_t ac_header_version = 3U;
constexpr std::size_t ac_header_total_length = 6U;
constexpr std::size_t entity_id = 3U;
constexpr std::size_t clock_attributes = 4U;
constexpr std::size_t clock_controls = 5U;
constexpr std::size_t terminal_type = 4U;
constexpr std::size_t input_terminal_clock = 7U;
constexpr std::size_t input_terminal_channels = 8U;
constexpr std::size_t output_terminal_source = 7U;
constexpr std::size_t output_terminal_clock = 8U;
constexpr std::size_t feature_unit_source = 4U;
constexpr std::size_t as_general_terminal = 3U;
constexpr std::size_t as_general_format_type = 5U;
constexpr std::size_t as_general_formats = 6U;
constexpr std::size_t as_general_channels = 10U;
constexpr std::size_t as_general_channel_config = 11U;
constexpr std::size_t as_general_bytes = 16U;
constexpr std::size_t format_type = 3U;
constexpr std::size_t format_type_bytes = 6U;
constexpr std::size_t format_subslot = 4U;
constexpr std::size_t format_bits = 5U;

// The wire contract, written out as the lane and the plan document it, not read
// back from usb/usb_composite.h: a change there must fail here rather than move
// the expectations with it. Only the INFO cross-check below uses that header.
constexpr std::uint16_t vendor_id = 0xCAFEU;
#if defined(WFG_UAC_MONO_ALT)
constexpr std::uint16_t player_release = 0x0103U;
#else
constexpr std::uint16_t player_release = 0x0102U;
#endif
constexpr std::uint8_t player_interface_count = 5U;
constexpr std::uint8_t interface_cdc_control = 0U;
constexpr std::uint8_t interface_cdc_data = 1U;
constexpr std::uint8_t interface_msc = 2U;
constexpr std::uint8_t interface_audio_control = 3U;
constexpr std::uint8_t interface_audio_streaming = 4U;
constexpr std::uint8_t endpoint_cdc_notification = 0x81U;
constexpr std::uint8_t endpoint_cdc_out = 0x02U;
constexpr std::uint8_t endpoint_cdc_in = 0x82U;
constexpr std::uint8_t endpoint_msc_out = 0x03U;
constexpr std::uint8_t endpoint_msc_in = 0x83U;
constexpr std::uint8_t endpoint_audio_in = 0x84U;
constexpr std::uint8_t entity_input_terminal = 1U;
constexpr std::uint8_t entity_feature_unit = 2U;
constexpr std::uint8_t entity_output_terminal = 3U;
constexpr std::uint8_t entity_clock_source = 4U;
constexpr std::uint8_t audio_channels = 2U;
constexpr std::uint32_t front_left_right = 0x00000003U;
#if defined(WFG_UAC_MONO_ALT)
constexpr std::uint32_t front_center = 0x00000004U;
#endif

// One alternate of the streaming interface and the format it carries, in the
// order the descriptors list them after alternate 0: 48 frames per packet.
struct StreamAlternate
{
    std::uint8_t alternate{};
    std::uint8_t channels{};
    std::uint32_t channel_config{};
    std::uint8_t subslot_bytes{};
    std::uint8_t bits{};
    std::uint16_t packet_bytes{};
};

constexpr auto stream_alternates = std::to_array<StreamAlternate>({
    {.alternate = 1U, .channels = 2U, .channel_config = front_left_right, .subslot_bytes = 4U, .bits = 24U, .packet_bytes = 384U},
    {.alternate = 2U, .channels = 2U, .channel_config = front_left_right, .subslot_bytes = 2U, .bits = 16U, .packet_bytes = 192U},
#if defined(WFG_UAC_MONO_ALT)
    {.alternate = 3U, .channels = 1U, .channel_config = front_center, .subslot_bytes = 2U, .bits = 16U, .packet_bytes = 96U},
#endif
});

static_assert(std::ranges::all_of(stream_alternates,
                                  [](const StreamAlternate& format)
                                  {
                                      constexpr std::uint32_t frames_per_packet = 48U;
                                      return format.packet_bytes == frames_per_packet * format.channels * format.subslot_bytes;
                                  }),
              "every packet is 48 frames of its format");

// Expected values.
constexpr std::uint16_t usb_2_0 = 0x0200U;
constexpr std::uint16_t audio_2_0 = 0x0200U;
constexpr std::uint8_t class_misc = 0xEFU;
constexpr std::uint8_t subclass_common = 0x02U;
constexpr std::uint8_t protocol_iad = 0x01U;
constexpr std::uint8_t class_cdc = 0x02U;
constexpr std::uint8_t subclass_acm = 0x02U;
constexpr std::uint8_t class_cdc_data = 0x0AU;
constexpr std::uint8_t class_msc = 0x08U;
constexpr std::uint8_t subclass_scsi = 0x06U;
constexpr std::uint8_t class_audio = 0x01U;
constexpr std::uint8_t subclass_audio_control = 0x01U;
constexpr std::uint8_t subclass_audio_streaming = 0x02U;
constexpr std::uint8_t transfer_interrupt = 0x03U;
constexpr std::uint8_t transfer_bulk = 0x02U;
constexpr std::uint8_t iso_synchronous_data = 0x0DU;
constexpr std::uint8_t clock_internal_fixed_sof = 0x05U;
constexpr std::uint8_t clock_read_only_controls = 0x05U;
constexpr std::uint16_t terminal_line_connector = 0x0603U;
constexpr std::uint16_t terminal_usb_streaming = 0x0101U;
constexpr std::uint8_t format_type_i = 0x01U;
constexpr std::uint32_t format_pcm = 0x00000001U;
constexpr std::uint16_t full_speed_bulk_bytes = 64U;
constexpr std::uint16_t high_speed_bulk_bytes = 512U;
constexpr std::uint16_t notification_bytes = 16U;
constexpr std::uint8_t full_speed_audio_interval = 1U;
constexpr std::uint8_t high_speed_audio_interval = 4U;
// TinyUSB's CDC notification endpoint: every frame or microframe (unchanged storage function).
constexpr std::uint8_t notification_interval = 1U;
constexpr std::uint8_t serial_string_index = 3U;
constexpr std::uint8_t cdc_string_index = 4U;
constexpr std::uint8_t msc_string_index = 5U;
constexpr std::uint8_t first_unused_string_index = 7U;
constexpr std::uint16_t english_us = 0x0409U;
constexpr unsigned byte_bits = 8U;
constexpr std::uint16_t low_byte = 0x00FFU;
constexpr std::string_view audio_function_name{"RT1170 Player"};
// The serial the fuse words of usb_descriptor_bus.c spell, as hex digits.
constexpr std::string_view serial_number{"0123456776543210"};
#if defined(WFG_MSC_READ_ONLY)
constexpr std::uint16_t expected_product_id = 0x4013U;
constexpr std::string_view product_name{"RT1170 Waveform Player RO"};
constexpr std::string_view msc_name{"Waveform microSD read-only"};
#else
constexpr std::uint16_t expected_product_id = 0x4012U;
constexpr std::string_view product_name{"RT1170 Waveform Player"};
constexpr std::string_view msc_name{"Waveform microSD LUN"};
#endif

class Checks
{
public:
    void expect(bool condition, const char* what) noexcept
    {
        if (!condition)
        {
            ++failures_;
            std::fputs("FAIL: ", stdout);
            std::fputs(what, stdout);
            std::fputs("\n", stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

struct Interface
{
    std::uint8_t number{};
    std::uint8_t alternate{};
    std::uint8_t endpoints{};
    std::uint8_t class_code{};
    std::uint8_t subclass{};
    std::uint8_t string_index{};

    constexpr bool operator==(const Interface&) const noexcept = default;
};

struct Endpoint
{
    std::uint8_t address{};
    std::uint8_t attributes{};
    std::uint32_t max_packet{};
    std::uint8_t interval{};

    constexpr bool operator==(const Endpoint&) const noexcept = default;
};

struct Association
{
    std::uint8_t first{};
    std::uint8_t count{};
    std::uint8_t class_code{};
    std::uint8_t string_index{};

    constexpr bool operator==(const Association&) const noexcept = default;
};

// A class-specific interface descriptor and the interface (and alternate) it
// belongs to.
struct ClassDescriptor
{
    std::uint8_t owner{};
    std::uint8_t alternate{};
    Bytes bytes;
};

// A configuration descriptor and everything it contains, in order. The spans
// point into bytes: moving keeps that buffer, copying would not, so copies are
// refused.
struct Configuration
{
    Configuration() = default;
    Configuration(const Configuration&) = delete;
    Configuration& operator=(const Configuration&) = delete;
    Configuration(Configuration&&) noexcept = default;
    Configuration& operator=(Configuration&&) noexcept = default;
    ~Configuration() = default;

    std::vector<std::uint8_t> bytes;
    std::vector<Bytes> descriptors;
    std::vector<Interface> interfaces;
    std::vector<Endpoint> endpoints;
    std::vector<Association> associations;
    std::vector<ClassDescriptor> class_descriptors;
    // Endpoint descriptors found after each interface descriptor, in order.
    std::vector<std::uint8_t> endpoints_seen;
    // The interface number each endpoint descriptor belongs to, in order.
    std::vector<std::uint8_t> endpoint_owners;
    bool well_formed{};
};

// The little-endian 16-bit field at at.
[[nodiscard]] std::uint32_t le16(Bytes bytes, std::size_t at) noexcept
{
    return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1U]} << byte_bits);
}

// The little-endian 32-bit field at at.
[[nodiscard]] std::uint32_t le32(Bytes bytes, std::size_t at) noexcept
{
    return le16(bytes, at) | (le16(bytes, at + 2U) << (2U * byte_bits));
}

void add_descriptor(Configuration& configuration, Bytes descriptor)
{
    configuration.descriptors.push_back(descriptor);
    const auto type = descriptor[1];

    if (type == type_interface)
    {
        configuration.interfaces.push_back({.number = descriptor[interface_number],
                                            .alternate = descriptor[interface_alternate],
                                            .endpoints = descriptor[interface_endpoints],
                                            .class_code = descriptor[interface_class],
                                            .subclass = descriptor[interface_subclass],
                                            .string_index = descriptor[interface_string]});
        configuration.endpoints_seen.push_back(0U);
    }
    else if (type == type_endpoint && !configuration.endpoints_seen.empty())
    {
        configuration.endpoints.push_back(
            {.address = descriptor[endpoint_address], .attributes = descriptor[endpoint_attributes], .max_packet = le16(descriptor, endpoint_max_packet), .interval = descriptor[endpoint_interval]});
        configuration.endpoint_owners.push_back(configuration.interfaces.back().number);
        ++configuration.endpoints_seen.back();
    }
    else if (type == type_interface_association)
    {
        configuration.associations.push_back(
            {.first = descriptor[association_first], .count = descriptor[association_count], .class_code = descriptor[association_class], .string_index = descriptor[association_string]});
    }
    else if (type == type_class_interface && !configuration.interfaces.empty())
    {
        configuration.class_descriptors.push_back({.owner = configuration.interfaces.back().number, .alternate = configuration.interfaces.back().alternate, .bytes = descriptor});
    }
}

// Copies the configuration at raw (its wTotalLength bytes) and splits it into
// descriptors. well_formed: every descriptor fits and they end exactly at wTotalLength.
[[nodiscard]] Configuration parse_configuration(const std::uint8_t* raw)
{
    Configuration configuration;
    const Bytes header{raw, configuration_bytes};
    const Bytes whole{raw, le16(header, configuration_total_length)};
    configuration.bytes.assign(whole.begin(), whole.end());
    const Bytes bytes{configuration.bytes};
    std::size_t at = 0U;

    while (at + header_bytes <= bytes.size())
    {
        const std::size_t length = bytes[at];

        if (length < header_bytes || at + length > bytes.size())
        {
            return configuration;
        }

        add_descriptor(configuration, bytes.subspan(at, length));
        at += length;
    }

    configuration.well_formed = at == bytes.size() && !configuration.descriptors.empty() && configuration.descriptors.front().size() == configuration_bytes;
    return configuration;
}

[[nodiscard]] Configuration configuration_at(bool high_speed)
{
    wfg_test_set_high_speed(high_speed);
    return parse_configuration(tud_descriptor_configuration_cb(0U));
}

// Every endpoint in order: the same isochronous IN endpoint once per format
// alternate, with that format's packet size and the same interval.
[[nodiscard]] std::vector<Endpoint> expected_endpoints(bool high_speed)
{
    const std::uint16_t bulk = high_speed ? high_speed_bulk_bytes : full_speed_bulk_bytes;
    const std::uint8_t audio_interval = high_speed ? high_speed_audio_interval : full_speed_audio_interval;
    std::vector<Endpoint> endpoints{
        {.address = endpoint_cdc_notification, .attributes = transfer_interrupt, .max_packet = notification_bytes, .interval = notification_interval},
        {.address = endpoint_cdc_out, .attributes = transfer_bulk, .max_packet = bulk, .interval = 0U},
        {.address = endpoint_cdc_in, .attributes = transfer_bulk, .max_packet = bulk, .interval = 0U},
        {.address = endpoint_msc_out, .attributes = transfer_bulk, .max_packet = bulk, .interval = 0U},
        {.address = endpoint_msc_in, .attributes = transfer_bulk, .max_packet = bulk, .interval = 0U},
    };

    for (const auto& format : stream_alternates)
    {
        endpoints.push_back({.address = endpoint_audio_in, .attributes = iso_synchronous_data, .max_packet = format.packet_bytes, .interval = audio_interval});
    }

    return endpoints;
}

[[nodiscard]] std::vector<Interface> expected_interfaces(std::uint8_t audio_string)
{
    std::vector<Interface> interfaces{
        {.number = interface_cdc_control, .alternate = 0U, .endpoints = 1U, .class_code = class_cdc, .subclass = subclass_acm, .string_index = cdc_string_index},
        {.number = interface_cdc_data, .alternate = 0U, .endpoints = 2U, .class_code = class_cdc_data, .subclass = 0U, .string_index = 0U},
        {.number = interface_msc, .alternate = 0U, .endpoints = 2U, .class_code = class_msc, .subclass = subclass_scsi, .string_index = msc_string_index},
        {.number = interface_audio_control, .alternate = 0U, .endpoints = 0U, .class_code = class_audio, .subclass = subclass_audio_control, .string_index = audio_string},
        {.number = interface_audio_streaming, .alternate = 0U, .endpoints = 0U, .class_code = class_audio, .subclass = subclass_audio_streaming, .string_index = 0U},
    };

    for (const auto& format : stream_alternates)
    {
        interfaces.push_back(
            {.number = interface_audio_streaming, .alternate = format.alternate, .endpoints = 1U, .class_code = class_audio, .subclass = subclass_audio_streaming, .string_index = 0U});
    }

    return interfaces;
}

// The string descriptor at index as text, or empty when the device stalls it.
[[nodiscard]] std::vector<std::uint16_t> string_units(std::uint8_t index)
{
    const std::uint16_t* const raw = tud_descriptor_string_cb(index, english_us);

    if (raw == nullptr)
    {
        return {};
    }

    const std::span<const std::uint16_t> header{raw, 1U};
    const std::size_t bytes = header[0] & low_byte;

    if ((header[0] >> byte_bits) != type_string || bytes < header_bytes || bytes % 2U != 0U)
    {
        return {};
    }

    const auto text = std::span<const std::uint16_t>{raw, bytes / 2U}.subspan(1U);
    return {text.begin(), text.end()};
}

[[nodiscard]] bool string_is(std::uint8_t index, std::string_view expected)
{
    const auto units = string_units(index);
    const auto same = [](std::uint16_t unit, char character) { return unit == std::uint16_t{std::bit_cast<unsigned char>(character)}; };

    return !expected.empty() && std::ranges::equal(units, expected, same);
}

// The first class-specific descriptor of subtype in interface owner, or null.
[[nodiscard]] const ClassDescriptor* find_class_descriptor(const Configuration& configuration, std::uint8_t owner, std::uint8_t subtype)
{
    const auto found = std::ranges::find_if(configuration.class_descriptors, [&](const ClassDescriptor& descriptor)
                                            { return descriptor.owner == owner && descriptor.bytes.size() > class_subtype && descriptor.bytes[class_subtype] == subtype; });

    return found == configuration.class_descriptors.end() ? nullptr : &*found;
}

// The class-specific descriptors of subtype in one alternate of interface owner.
[[nodiscard]] std::vector<Bytes> alternate_class_descriptors(const Configuration& configuration, std::uint8_t owner, std::uint8_t alternate, std::uint8_t subtype)
{
    std::vector<Bytes> found;

    for (const auto& descriptor : configuration.class_descriptors)
    {
        if (descriptor.owner == owner && descriptor.alternate == alternate && descriptor.bytes.size() > class_subtype && descriptor.bytes[class_subtype] == subtype)
        {
            found.push_back(descriptor.bytes);
        }
    }

    return found;
}

// The audio function's string index (its IAD's iFunction), or 0 without one.
[[nodiscard]] std::uint8_t audio_string_index(const Configuration& configuration)
{
    const auto found = std::ranges::find(configuration.associations, std::uint8_t{interface_audio_control}, &Association::first);
    return found == configuration.associations.end() ? std::uint8_t{} : found->string_index;
}

void test_device(Checks& checks)
{
    const Bytes device{tud_descriptor_device_cb(), device_bytes};
    checks.expect(device[0] == device_bytes && device[1] == type_device && le16(device, device_usb_version) == usb_2_0, "a USB 2.0 device descriptor");
    checks.expect(device[device_class] == class_misc && device[device_subclass] == subclass_common && device[device_protocol] == protocol_iad, "the composite device announces interface associations");
    checks.expect(le16(device, device_vendor) == vendor_id && le16(device, device_product) == expected_product_id && le16(device, device_product) == wfg_usb_product_id_player_image,
                  "VID 0xCAFE with the player PID that INFO reports as usb_pid");
    checks.expect(le16(device, device_release) == player_release && le16(device, device_release) == wfg_usb_bcd_device_player,
                  "bcdDevice 0x0102 (0x0103 with the mono alternate): it moved on with the descriptor set, so Windows reads it again");
    checks.expect(device[device_serial_string] == serial_string_index && device[device_configurations] == 1U, "one configuration and the serial string");
    const Bytes qualifier{tud_descriptor_device_qualifier_cb(), qualifier_bytes};
    checks.expect(qualifier[0] == qualifier_bytes && qualifier[1] == type_device_qualifier && qualifier[qualifier_configurations] == 1U, "the device qualifier offers one other-speed configuration");
}

// What is the same at both speeds: the five interfaces, both IADs, unique endpoints.
void check_layout(Checks& checks, const Configuration& configuration, bool high_speed)
{
    checks.expect(configuration.well_formed, "the configuration splits into descriptors that end exactly at wTotalLength");
    const Bytes header{configuration.bytes};
    auto numbers = configuration.interfaces | std::views::transform(&Interface::number) | std::ranges::to<std::vector>();
    std::ranges::sort(numbers);
    const auto [last, end] = std::ranges::unique(numbers);
    numbers.erase(last, end);
    checks.expect(header.size() >= configuration_bytes && header[1] == type_configuration, "a configuration descriptor");
    checks.expect(header.size() >= configuration_bytes && header[configuration_interfaces] == player_interface_count && numbers.size() == player_interface_count &&
                      numbers.back() == player_interface_count - 1,
                  "bNumInterfaces counts interfaces 0..4, each present");
    const auto audio_string = audio_string_index(configuration);
    checks.expect(configuration.interfaces == expected_interfaces(audio_string), "CDC 0/1, MSC 2, audio control 3, audio streaming 4: alternate 0, then one alternate per format in ascending order");
    const std::vector<Association> associations{
        {.first = interface_cdc_control, .count = 2U, .class_code = class_cdc, .string_index = 0U},
        {.first = interface_audio_control, .count = 2U, .class_code = class_audio, .string_index = audio_string},
    };
    checks.expect(configuration.associations == associations && audio_string != 0U, "IADs group CDC 0/1 and the audio function 3/4");
    checks.expect(configuration.endpoints == expected_endpoints(high_speed), "endpoints, packet sizes and intervals for the bus speed");
    const auto declared = configuration.interfaces | std::views::transform(&Interface::endpoints) | std::ranges::to<std::vector>();
    checks.expect(declared == configuration.endpoints_seen, "every interface declares the endpoints that follow it");
    // Alternates of one interface share its endpoints; interfaces never do.
    std::vector<std::pair<std::uint8_t, std::uint8_t>> owned;

    for (std::size_t index = 0U; index < configuration.endpoints.size() && index < configuration.endpoint_owners.size(); ++index)
    {
        owned.emplace_back(configuration.endpoints[index].address, configuration.endpoint_owners[index]);
    }

    std::ranges::sort(owned);
    const auto [shared_end, owned_end] = std::ranges::unique(owned);
    owned.erase(shared_end, owned_end);
    const auto same_address = [](const auto& first, const auto& second) { return first.first == second.first; };

    checks.expect(std::ranges::adjacent_find(owned, same_address) == owned.end(), "no endpoint address is used by two interfaces");
}

// Each format alternate: one AS general descriptor for output terminal 3 as
// Type I PCM with the format's channels, and one Type I format descriptor with
// its subslot and bit resolution. Windows' usbaudio2 needs the same
// bTerminalLink in every alternate, the AS general bFormatType equal to the
// format descriptor's, and exactly one bmFormats bit for Type I.
void check_stream_formats(Checks& checks, const Configuration& configuration)
{
    checks.expect(alternate_class_descriptors(configuration, interface_audio_streaming, 0U, subtype_as_general).empty() &&
                      alternate_class_descriptors(configuration, interface_audio_streaming, 0U, subtype_format_type).empty(),
                  "alternate 0 carries no format");

    for (const auto& expected : stream_alternates)
    {
        const auto general = alternate_class_descriptors(configuration, interface_audio_streaming, expected.alternate, subtype_as_general);
        const auto format = alternate_class_descriptors(configuration, interface_audio_streaming, expected.alternate, subtype_format_type);

        if (general.size() != 1U || format.size() != 1U || general.front().size() != as_general_bytes || format.front().size() != format_type_bytes)
        {
            checks.expect(false, "each format alternate has one AS general and one Type I format descriptor");
            continue;
        }

        const Bytes as_general = general.front();
        const Bytes type = format.front();
        checks.expect(as_general[as_general_terminal] == entity_output_terminal && as_general[as_general_format_type] == format_type_i && le32(as_general, as_general_formats) == format_pcm &&
                          std::has_single_bit(le32(as_general, as_general_formats)) && type[format_type] == as_general[as_general_format_type],
                      "every alternate streams output terminal 3 as Type I PCM, one bmFormats bit, its format type matching");
        checks.expect(as_general[as_general_channels] == expected.channels && le32(as_general, as_general_channel_config) == expected.channel_config,
                      "alternates 1 and 2 carry the stereo cluster (FL | FR); the mono alternate one channel (FC)");
        checks.expect(type[format_subslot] == expected.subslot_bytes && type[format_bits] == expected.bits,
                      "alternate 1 is 24 bits in 4-byte subslots; alternates 2 and 3 are 16 bits in 2-byte subslots");
    }
}

// The UAC2 control topology and the stream formats.
void check_audio(Checks& checks, const Configuration& configuration)
{
    const auto* const header = find_class_descriptor(configuration, interface_audio_control, subtype_ac_header);
    const auto* const clock = find_class_descriptor(configuration, interface_audio_control, subtype_clock_source);
    const auto* const input = find_class_descriptor(configuration, interface_audio_control, subtype_input_terminal);
    const auto* const feature = find_class_descriptor(configuration, interface_audio_control, subtype_feature_unit);
    const auto* const output = find_class_descriptor(configuration, interface_audio_control, subtype_output_terminal);

    if (header == nullptr || clock == nullptr || input == nullptr || feature == nullptr || output == nullptr)
    {
        checks.expect(false, "the audio function has its header, clock, terminals and feature unit");
        return;
    }

    std::size_t control_length = 0U;

    for (const auto& descriptor : configuration.class_descriptors)
    {
        control_length += descriptor.owner == interface_audio_control ? descriptor.bytes.size() : 0U;
    }

    checks.expect(le16(header->bytes, ac_header_version) == audio_2_0 && le16(header->bytes, ac_header_total_length) == control_length,
                  "the UAC2 header's wTotalLength covers exactly the class-specific control descriptors");
    checks.expect(clock->bytes[entity_id] == entity_clock_source && clock->bytes[clock_attributes] == clock_internal_fixed_sof && clock->bytes[clock_controls] == clock_read_only_controls,
                  "clock source 4: internal fixed, synchronized to SOF, frequency and validity read-only");
    checks.expect(input->bytes[entity_id] == entity_input_terminal && le16(input->bytes, terminal_type) == terminal_line_connector && input->bytes[input_terminal_clock] == entity_clock_source &&
                      input->bytes[input_terminal_channels] == audio_channels,
                  "input terminal 1: a two-channel line connector on clock 4");
    checks.expect(feature->bytes[entity_id] == entity_feature_unit && feature->bytes[feature_unit_source] == entity_input_terminal, "feature unit 2 takes input terminal 1");
    checks.expect(output->bytes[entity_id] == entity_output_terminal && le16(output->bytes, terminal_type) == terminal_usb_streaming && output->bytes[output_terminal_source] == entity_feature_unit &&
                      output->bytes[output_terminal_clock] == entity_clock_source,
                  "output terminal 3 streams feature unit 2 to USB on clock 4");
    check_stream_formats(checks, configuration);
}

// Equal descriptors, except that an endpoint's wMaxPacketSize and bInterval may differ.
[[nodiscard]] bool same_but_endpoint_timing(Bytes full, Bytes high)
{
    if (full.size() != high.size())
    {
        return false;
    }

    if (full[1] != type_endpoint || full.size() <= endpoint_interval)
    {
        return std::ranges::equal(full, high);
    }

    return std::ranges::equal(full.first(endpoint_max_packet), high.first(endpoint_max_packet)) && std::ranges::equal(full.subspan(endpoint_interval + 1U), high.subspan(endpoint_interval + 1U));
}

// Full and high speed differ only in the bulk packet sizes and the isochronous bInterval.
void check_speed_differences(Checks& checks, const Configuration& full, const Configuration& high)
{
    checks.expect(full.bytes.size() == high.bytes.size() && std::ranges::equal(full.descriptors, high.descriptors, same_but_endpoint_timing),
                  "FS and HS configurations have equal length and differ only in endpoint packet sizes and intervals");
}

void test_configurations(Checks& checks)
{
    const auto high = configuration_at(true);
    const auto full = configuration_at(false);
    check_layout(checks, high, true);
    check_layout(checks, full, false);
    check_audio(checks, high);
    check_audio(checks, full);
    check_speed_differences(checks, full, high);
    wfg_test_set_high_speed(true);
    const auto other_at_high = parse_configuration(tud_descriptor_other_speed_configuration_cb(0U));
    wfg_test_set_high_speed(false);
    const auto other_at_full = parse_configuration(tud_descriptor_other_speed_configuration_cb(0U));
    const auto is_other_speed_copy = [](const Configuration& other, const Configuration& source)
    {
        return other.bytes.size() == source.bytes.size() && other.bytes.size() > 1U && other.bytes[1] == type_other_speed_configuration &&
               std::ranges::equal(Bytes{other.bytes}.subspan(header_bytes), Bytes{source.bytes}.subspan(header_bytes)) && other.bytes[0] == source.bytes[0];
    };

    checks.expect(is_other_speed_copy(other_at_high, full), "at high speed the other-speed configuration is the full-speed one");
    checks.expect(is_other_speed_copy(other_at_full, high), "at full speed the other-speed configuration is the high-speed one");
    wfg_test_set_high_speed(true);
}

void test_strings(Checks& checks)
{
    const auto language = string_units(0U);
    checks.expect(language.size() == 1U && language.front() == english_us, "string 0 offers US English");
    checks.expect(string_is(2U, product_name), "the product string is unchanged");
    checks.expect(string_is(serial_string_index, serial_number), "the serial number is the OCOTP fuse words in hex");
    checks.expect(string_is(cdc_string_index, "Waveform Fixture CDC") && string_is(msc_string_index, msc_name), "the CDC and MSC interface strings are unchanged");
    checks.expect(string_is(audio_string_index(configuration_at(true)), audio_function_name), "the audio function's iFunction and iInterface name it RT1170 Player");
    checks.expect(string_units(first_unused_string_index).empty(), "an unknown string index stalls");
}
} // namespace

int main()
{
    Checks checks;
    test_device(checks);
    test_configurations(checks);
    test_strings(checks);

    if (checks.failures() != 0)
    {
        std::fputs("USB descriptor tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("USB descriptor tests: PASS\n", stdout);
    return 0;
}
