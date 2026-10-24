// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// A corpus of WFG-LIVE/1 request lines and what common/live_protocol.cpp's
// parser makes of each, as the reply to the line shows it. The seeds are
// every command, its boundaries and the refusals tests/live_protocol_tests.cpp
// names; the edits of a seed are the lines small changes to it make (a
// character deleted, doubled, swapped with the next or replaced, a number or
// a word replaced, a truncation, a suffix, an AT: prefix, a changed sequence).
// tests/live_protocol_corpus.txt keeps each seed's outcome and a digest of
// its edits' outcomes, which live_protocol_corpus_tests.cpp compares;
// player_live_host_tests.cpp sends every refused line through the USB
// service and reads the reply.

#pragma once

#include "common/live_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace live_protocol_corpus
{

namespace protocol = waveform_generator::live_protocol;

// A line, and the connection's last sequence before it.
struct Line
{
    std::uint32_t last_before{};
    std::string text;
};

// What the parser made of a line, as its reply shows it: the reply's seq and
// the connection's last sequence after the line, then the request when it was
// accepted, else the reply's error code.
struct Outcome
{
    bool accepted{};
    std::uint32_t reply_seq{};
    std::uint32_t last_after{};
    std::string error;
    protocol::Request request{};
};

// The parser's verdict on `line`, as rt1170/usb_service.cpp's
// handle_control_line() replies to it: a refused line's reply carries
// reply_sequence() and the refusal's message, its error code.
[[nodiscard]] inline Outcome outcome_of(const Line& line)
{
    Outcome outcome;
    outcome.last_after = line.last_before;
    const m110::Result<protocol::Request> parsed = protocol::parse(line.text, outcome.last_after);
    outcome.accepted = parsed.has_value();

    if (parsed.has_value())
    {
        outcome.request = parsed.value();
        outcome.reply_seq = outcome.request.seq;
    }
    else
    {
        outcome.reply_seq = protocol::reply_sequence(line.text);
        outcome.error = parsed.status().message;
    }

    return outcome;
}

inline constexpr auto kind_names = std::to_array<std::string_view>({
    "info",
    "status",
    "counters",
    "usb_diagnostics",
    "playlist_status",
    "engine_status",
    "files",
    "prepare_status",
    "prepare_play_status",
    "load",
    "play",
    "stop",
    "media_host",
    "media_local",
    "generate_file",
    "output_codec",
    "output_usb",
    "cut",
    "playlist_start",
    "playlist_ack",
    "seed",
    "reference",
    "prepare_file",
    "prepare_play",
    "cw_on",
    "cw_frequency",
    "cw_ci",
    "static_on",
    "static_rate",
    "static_peak",
    "fade",
    "sweep_frequency",
    "sweep_ci",
    "sweep_fade",
});

static_assert(kind_names.size() == std::size_t{std::to_underlying(protocol::Kind::sweep_fade)} + 1U, "a name for every request kind");

// `text` with every byte outside printable ASCII, and each quote and
// backslash, written as \xHH, \" or \\.
[[nodiscard]] inline std::string escaped(std::string_view text)
{
    constexpr std::string_view hex_digits{"0123456789abcdef"};
    constexpr unsigned int nibble_bits = 4U;
    constexpr unsigned int nibble_mask = 0x0fU;
    constexpr unsigned char first_printable = 0x20U;
    constexpr unsigned char last_printable = 0x7eU;
    std::string out;

    for (const char character : text)
    {
        const auto byte = static_cast<unsigned char>(character);

        if (character == '"' || character == '\\')
        {
            out += '\\';
            out += character;
        }
        else if (byte >= first_printable && byte <= last_printable)
        {
            out += character;
        }
        else
        {
            out += "\\x";
            out += hex_digits.at(byte >> nibble_bits);
            out += hex_digits.at(byte & nibble_mask);
        }
    }

    return out;
}

// The shortest decimal that reads back as `value`.
template <class Number> [[nodiscard]] std::string number(Number value)
{
    constexpr std::size_t widest = 32U;
    std::array<char, widest> digits{};
    const std::to_chars_result written = std::to_chars(digits.data(), std::next(digits.data(), widest), value);
    return {digits.data(), written.ptr};
}

// A character field's bytes up to its last one that is not NUL, quoted.
[[nodiscard]] inline std::string characters(std::span<const char> field)
{
    const std::string_view bytes{field.data(), field.size()};
    const std::size_t last = bytes.find_last_not_of('\0');
    return '"' + escaped(bytes.substr(0U, last == std::string_view::npos ? 0U : last + 1U)) + '"';
}

// Appends " name=value" when `value` is not its type's default.
template <class Value> void append_field(std::string& record, std::string_view name, const Value& value)
{
    if (value != Value{})
    {
        record += ' ';
        record += name;
        record += '=';
        record += number(value);
    }
}

inline void append_characters(std::string& record, std::string_view name, std::span<const char> field)
{
    if (std::ranges::any_of(field, [](char character) { return character != '\0'; }))
    {
        record += ' ';
        record += name;
        record += '=';
        record += characters(field);
    }
}

inline void append_cuts(std::string& record, const waveform_generator::cut::CutList& cuts)
{
    if (cuts.empty())
    {
        return;
    }

    record += " cuts=";
    const char* separator = "";

    for (const waveform_generator::cut::CutEntry& entry : cuts.entries())
    {
        record += separator;
        record += number(entry.offset.value) + ':' + number(entry.length) + ':' + number(std::to_underlying(entry.kind));
        separator = ",";
    }
}

// An outcome as a record shows it: "seq=<s> last=<l> error=<code>", or
// "seq=<s> last=<l> ok kind=<kind>" and every request field that is not its
// default.
[[nodiscard]] inline std::string render(const Outcome& outcome)
{
    std::string text = "seq=" + number(outcome.reply_seq) + " last=" + number(outcome.last_after);

    if (!outcome.accepted)
    {
        return text + " error=" + outcome.error;
    }

    const protocol::Request& request = outcome.request;
    text += " ok kind=";
    text += kind_names.at(std::to_underlying(request.kind));
    append_characters(text, "name", request.name);
    append_characters(text, "run_id", request.run_id);
    append_characters(text, "input", request.input_name);
    append_characters(text, "encoder", request.encoder);
    append_characters(text, "profile", request.profile);
    append_field(text, "rate", request.rate);
    append_field(text, "interleave", request.interleave);
    append_field(text, "integer", request.integer);
    append_field(text, "scheduled", std::uint32_t{request.scheduled});
    append_field(text, "frame", request.frame);
    append_field(text, "value", request.value);
    append_field(text, "end", request.end_value);
    append_field(text, "steps", request.steps);
    append_field(text, "interval", request.interval_ms);
    append_field(text, "duration", request.duration_ms);
    append_field(text, "oscillator", request.oscillator);
    append_cuts(text, request.cuts);
    append_field(text, "policy", std::uint32_t{std::to_underlying(request.policy)});
    append_field(text, "ack", request.ack_seq);

    if (request.kind == protocol::Kind::prepare_file)
    {
        append_characters(text, "prepare_source", request.preparation.source);
        append_characters(text, "prepare_scenario", request.preparation.scenario);
        append_characters(text, "prepare_output", request.preparation.output);
        append_field(text, "prepare_retention", std::to_underlying(request.preparation.retention));
        append_field(text, "prepare_seed_override", std::uint32_t{request.preparation.seed_override});
        append_field(text, "prepare_seed", request.preparation.seed);
    }

    if (request.kind == protocol::Kind::prepare_play)
    {
        append_characters(text, "prepare_source", request.preparation.source);
        append_characters(text, "prepare_scenario", request.preparation.scenario);
        append_characters(text, "prepare_output", request.preparation.output);
        append_field(text, "prepare_retention", std::to_underlying(request.preparation.retention));
        append_field(text, "prepare_seed_override", std::uint32_t{request.preparation.seed_override});
        append_field(text, "prepare_seed", request.preparation.seed);
        append_field(text, "prepare_rate", request.preparation_rate);
        append_field(text, "prepare_margin_frames", request.preparation_margin_frames);
    }

    return text;
}

// The line `size` bytes long: `head`, then `filler` up to the size.
[[nodiscard]] inline std::string padded(std::string_view head, std::size_t size, char filler)
{
    std::string text{head};
    text.resize(std::max(size, head.size()), filler);
    return text;
}

// A CUT of `count` copies of `entry` after `head`.
[[nodiscard]] inline std::string cut_of(std::string_view head, std::size_t count, std::string_view entry)
{
    std::string text{head};

    for (std::size_t index = 0U; index < count; ++index)
    {
        text += index == 0U ? "" : ",";
        text += entry;
    }

    return text;
}

[[nodiscard]] inline std::vector<std::string> seed_texts()
{
    std::vector<std::string> texts{
        // The queries and plain commands.
        "1 INFO?",
        "1 STATUS?",
        "1 COUNTERS?",
        "1 USB?",
        "1 PLAYLIST?",
        "1 ENGINE?",
        "1 FILES?",
        "1 FILES? 0",
        "1 FILES? 18446744073709551615",
        "1 PLAY",
        "1 STOP",
        "1 MEDIA HOST",
        "1 MEDIA LOCAL",
        "1 PREPARE?",
        "1 PREPAREPLAY?",
        "1 AT:0 PREPAREPLAY?",
        "1 PREPAREPLAY? EXTRA",
        "1 PREPARE:OUT.WAV:SRC.WAV:TEST.SCN:KEEP",
        "1 PREPARE:OUT.WAV:SRC.WAV:TEST.SCN:TEMP:18446744073709551615",
        "1 PREPARE:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:0",
        "1 PREPARE:OUT.WAV:SRC.WAV:TEST.SCN:TEMP:01",
        "1 PREPARE:OUT.WAV:SRC.WAV:TEST.SCN:TEMP:18446744073709551616",
        "1 PREPARE:OUT.WAV:SRC.WAV:TEST.SCN:keep",
        "1 AT:0 PREPARE:OUT.WAV:SRC.WAV:TEST.SCN:KEEP",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:1200:4096",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:TEMP:4294967295:4294967295:18446744073709551615",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:1:0:0",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:0:0",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:01:0",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:4294967296:0",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:1:01",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:1:4294967296",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:1:0:18446744073709551616",
        "1 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:1:0:1:2",
        "1 AT:0 PREPAREPLAY:OUT.WAV:SRC.WAV:TEST.SCN:KEEP:1:0",
        "1 OUTPUT CODEC",
        "1 OUTPUT USB",
        "2 INFO?\r",
        "3 PLAY\r",
        "1 STATUS",
        "1 OUTPUT",
        "1 OUTPUT SPEAKER",
        "1 OUTPUT usb",
        "1 MEDIA",
        // The sequence.
        "0 INFO?",
        "01 INFO?",
        "4294967295 STOP",
        "4294967296 STOP",
        "18446744073709551616 STOP",
        "INFO?",
        " 1 INFO?",
        "1",
        "1 ",
        "",
        "\r",
        "1  INFO?",
        "1 INFO?\r\r",
        "1 INFO?\rX",
        "-1 INFO?",
        "+1 INFO?",
        "1\tINFO?",
        "1 INFO?\x01",
        "1 INFO?\x7f",
        "1 INFO?\x80",
        // PLAY:<run id>.
        "1 PLAY:0123456789abcdef0123456789abcdef",
        "1 PLAY:0123456789ABCDEF0123456789abcdef",
        "1 PLAY:0123456789abcdef0123456789abcde",
        "1 PLAY:0123456789abcdef0123456789abcdef0",
        "1 PLAY:",
        "1 PLAY:0123456789abcdef0123456789abcdeg",
        // LOAD.
        "1 LOAD:REF_600.WAV",
        "4 LOAD:REF_600.WAV\r",
        "1 LOAD:A.WAV",
        "1 LOAD:ABCDEFGH.WAV",
        "1 LOAD:ABCDEFGHI.WAV",
        "1 LOAD:.WAV",
        "1 LOAD:A.BIN",
        "1 LOAD:a.WAV",
        "1 LOAD:A.WAVX",
        "1 LOAD:A",
        "1 LOAD:../X.WAV",
        "1 LOAD:X.WAV ",
        "1 LOAD:",
        "1 LOAD:A.wav",
        "1 LOAD:A.B.WAV",
        // SEED and REFERENCE.
        "1 SEED:0",
        "1 SEED:18446744073709551615",
        "1 SEED:18446744073709551616",
        "1 SEED:",
        "1 SEED:01",
        "1 SEED:-1",
        "1 REFERENCE:0.125",
        "1 REFERENCE:1",
        "1 REFERENCE:1.0",
        "1 REFERENCE:0",
        "1 REFERENCE:1.0000001",
        "1 REFERENCE:-0.5",
        "1 REFERENCE:nan",
        "1 REFERENCE:",
        "1 REFERENCE:.5",
        "1 REFERENCE:0.",
        "1 REFERENCE:0.12345678901234",
        "1 REFERENCE:0.123456789012345",
        "1 REFERENCE:0.1234567890123456",
        "1 REFERENCE:00.5",
        "1 REFERENCE:0.5.",
        "1 REFERENCE:-",
        // GENERATE, at each rate and interleave.
        "13 GENERATE:600:long:PAYLOAD.BIN:TEST.WAV",
        "1 GENERATE:75:short:A.BIN:B.WAV",
        "1 GENERATE:150:zero:A.BIN:B.WAV",
        "1 GENERATE:300:long:A.BIN:B.WAV",
        "1 GENERATE:1200:short:A.BIN:B.WAV",
        "1 GENERATE:2400:long:A.BIN:B.WAV",
        "1 GENERATE:4800:zero:A.BIN:B.WAV",
        "1 GENERATE:100:long:A.BIN:B.WAV",
        "1 GENERATE:9600:long:A.BIN:B.WAV",
        "1 GENERATE:600:medium:A.BIN:B.WAV",
        "1 GENERATE:600:long:A.WAV:B.WAV",
        "1 GENERATE:600:long:A.BIN:B.BIN",
        "1 GENERATE:600:long:A.BIN",
        "1 GENERATE:600:long:A.BIN:B.WAV:C",
        "1 GENERATE:600:long:A.BIN:B.WAV:",
        "1 GENERATE::long:A.BIN:B.WAV",
        "1 GENERATE:0600:long:A.BIN:B.WAV",
        "1 AT:0 GENERATE:600:long:A.BIN:B.WAV",
        "11 M110:600:long:PAYLOAD.BIN",
        // ENCODE.
        "26 ENCODE TONE 1000 INPUT.BIN OUT.WAV",
        "27 ENCODE CUSTOM R50:S170:F1000 INPUT.BIN OUT.WAV",
        "28 ENCODE lower 1000 INPUT.BIN OUT.WAV",
        "29 AT:0 ENCODE TONE 1000 INPUT.BIN OUT.WAV",
        "31 ENCODE TONE bad\"profile INPUT.BIN OUT.WAV",
        "1 ENCODE TONE bad\\profile INPUT.BIN OUT.WAV",
        "1 ENCODE ABCDEFGHIJKLMNO 1000 A.BIN B.WAV",
        "1 ENCODE ABCDEFGHIJKLMNOP 1000 A.BIN B.WAV",
        "1 ENCODE TONE ABCDEFGHIJKLMNOPQRSTUVWXYZ01234 A.BIN B.WAV",
        "1 ENCODE TONE ABCDEFGHIJKLMNOPQRSTUVWXYZ012345 A.BIN B.WAV",
        "1 ENCODE TONE 1000 A.WAV B.WAV",
        "1 ENCODE TONE 1000 A.BIN B.BIN",
        "1 ENCODE TONE 1000 A.BIN",
        "1 ENCODE TONE 1000 A.BIN B.WAV X",
        "1 ENCODE A_1 x A.BIN B.WAV",
        "1 ENCODE TONE ~ A.BIN B.WAV",
        // CW and STATIC.
        "1 CW ON",
        "1 CW OFF",
        "1 STATIC ON",
        "1 STATIC OFF",
        "1 CW 0 ON",
        "21 CW 1 ON",
        "1 CW 3 OFF",
        "24 CW 4 ON",
        "1 CW 01 ON",
        "1 STATIC 1 ON",
        "1 CW FREQ 1800",
        "1 CW CI 3",
        "1 STATIC RATE 1",
        "1 STATIC PEAK 20",
        "22 CW 3 FREQ 2800",
        "1 CW 1 CI -3",
        "1 CW 2 RATE 3",
        "1 STATIC FREQ 1",
        "1 CW FREQ -1.5",
        "1 CW FREQ 1800.25",
        "1 CW 4 FREQ 1800",
        "1 CW 0 CI 3",
        "1 STATIC 0 RATE 1",
        "1 CW FREQ",
        "1 CW",
        "1 CW ON OFF",
        "1 CW ONN",
        "1 FADE NOW",
        "1 STATIC PEAK x",
        // FADE NOW.
        "1 FADE NOW 18 250",
        "1 FADE NOW 18 0",
        "1 FADE NOW 18 1",
        "1 FADE NOW 18 3600000",
        "1 FADE NOW 18 3600001",
        "1 FADE NOW -3 1",
        "1 FADE LATER 18 250",
        "1 FADE NOW 18",
        "1 FADE NOW 18 250 1",
        "1 FADE NOW x 250",
        "1 FADE NOW 18 2.5",
        // SWEEP.
        "16 SWEEP CW FREQ 300 3400 16 500",
        "1 SWEEP CW CI 20 -3 16 1000",
        "1 SWEEP CW 2 FREQ 300 3400 16 500",
        "23 SWEEP CW 2 CI 20 -3 4 100",
        "1 SWEEP CW 4 FREQ 300 3400 16 500",
        "17 SWEEP FADE 6 30 5 1000 250",
        "18 SWEEP FADE 6 30 17 1000 250",
        "1 SWEEP CW FREQ 300 3400 1 500",
        "1 SWEEP CW FREQ 300 3400 2 500",
        "1 SWEEP CW FREQ 300 3400 17 500",
        "1 SWEEP CW FREQ 300 3400 16 0",
        "1 SWEEP CW FREQ 300 3400 16 3600000",
        "1 SWEEP CW FREQ 300 3400 16 3600001",
        "1 SWEEP FADE 6 30 5 1000 0",
        "1 SWEEP FADE 6 30 5 1000 3600000",
        "1 SWEEP FADE 6 30 5 1000 3600001",
        "1 SWEEP CW RATE 1 2 3 4",
        "1 SWEEP STATIC RATE 1 2 3 4",
        "1 SWEEP FADE 6 30 5 1000",
        "1 SWEEP FADE 6 30 5 1000 250 1",
        "1 SWEEP CW 1 FADE 6 30 5 1000",
        "1 SWEEP CW x FREQ 300 3400 16 500",
        "1 SWEEP CW FREQ x 3400 16 500",
        "1 SWEEP CW FREQ 300 x 16 500",
        "1 SWEEP CW FREQ 300 3400 x 500",
        "1 SWEEP CW FREQ 300 3400 16 x",
        "1 SWEEP FADE 6 30 5 x 250",
        // AT:<frame>.
        "1 AT:0 CW ON",
        "8 AT:18446744073709551615 CW CI -3",
        "9 AT:18446744073709551616 CW ON",
        "10 AT:0 STOP",
        "1 AT:0 SWEEP CW FREQ 300 3400 16 500",
        "1 AT:0 FADE NOW 18 250",
        "1 AT:5",
        "1 AT:5 ",
        "1 AT: CW ON",
        "1 AT:01 CW ON",
        "1 AT:0 CW 3 FREQ 2800",
        "1 AT:0 AT:0 CW ON",
        "1 AT:0 INFO?",
        "1 AT:0 SEED:1",
        "1 AT:0 REFERENCE:0.5",
        "1 AT:0 LOAD:A.WAV",
        "1 AT:0 PLAY:0123456789abcdef0123456789abcdef",
        "1 AT:x CW ON",
        // CUT.
        "46 CUT",
        "47 CUT:0:4800:zero,96000:1:drop\r",
        "48 CUT:1:0:drop,1:1:zero",
        "49 CUT:",
        "50 AT:0 CUT",
        "51 CUT ",
        "52 CUT:1:1:drop,",
        "53 CUT:1:1:Drop",
        "54 cut",
        "1 CUT:01:1:drop",
        "1 CUT:4294967296:1:drop",
        "1 CUT:1:1",
        "1 CUT:1:1:drop:1",
        "1 CUT:,1:1:drop",
        "1 CUT:4294967295:4800:drop",
        "1 CUTS",
        "1 CUT:1:4801:drop",
        // PLAYLIST.
        "7 PLAYLIST:BATTERY.PL",
        "8 PLAYLIST:BATTERY.PL:CONTINUE\r",
        "9 PLAYLIST:LADDER",
        "10 PLAYLIST:ABCDEFGH.XYZ",
        "11 PLAYLIST:battery.pl",
        "12 PLAYLIST:BATTERY.PL:continue",
        "13 PLAYLIST:BATTERY.PL:",
        "14 PLAYLIST:",
        "15 PLAYLIST:../A.PL",
        "16 PLAYLIST:ABCDEFGHI.PL",
        "17 PLAYLIST:A.PL:CONTINUE:CONTINUE",
        "18 PLAYLIST:A.PLAY",
        "19 AT:0 PLAYLIST:A.PL",
        "20 PLAYLIST: A.PL",
        "21 PLAYLIST ACK:0",
        "22 PLAYLIST ACK:4294967295",
        "23 PLAYLIST ACK:4294967296",
        "24 PLAYLIST ACK:01",
        "25 PLAYLIST ACK:",
        "26 PLAYLIST ACK:-1",
        "27 PLAYLIST ACK 5",
        "28 PLAYLIST  ACK:5",
        "29 AT:0 PLAYLIST ACK:5",
        "1 PLAYLIST",
        "1 PLAYLISTS:A.PL",
        "1 PLAYLIST? ",
        "1 playlist?",
        "1 PLAYLIST:A.PL:CONTNUE",
        // ENGINE? and USB?.
        "3 ENGINE? ",
        "4 engine?",
        "5 AT:0 ENGINE?",
        "6 ENGINE",
        "7 FILES? +1",
        "8 FILES? -1",
        "9 FILES? 01",
        "10 FILES? 18446744073709551616",
        "11 FILES? 1 2",
        "12 AT:0 FILES?",
        "13 AT:0 FILES? 1",
        "14 FILES",
        "42 AT:0 USB?",
        "43 USB? ",
        "44 usb?",
        "45 USB",
    };
    const std::string_view widest_entry = protocol::widest_cut_entry;
    texts.push_back(cut_of("4294967294 CUT:", waveform_generator::cut::max_entries, widest_entry) + "\r");
    texts.push_back(cut_of("4294967292 CUT:", waveform_generator::cut::max_entries + 1U, "1:1:zero"));
    texts.push_back(std::string{"4294967293 CUT:"} + std::string(protocol::wire_capacity, '1'));
    // The line's capacity: a line of wire_capacity - 1 bytes, its CR
    // included, reaches the command; one of wire_capacity does not.
    constexpr std::array<std::size_t, 3U> capacity_steps{protocol::wire_capacity - 1U, protocol::wire_capacity, protocol::wire_capacity + 1U};

    for (const std::size_t size : capacity_steps)
    {
        texts.push_back(padded("5 CW FREQ ", size, '0'));
        texts.push_back(padded("5 CW FREQ ", size - 1U, '0') + "\r");
        texts.push_back(padded("5 ", size, 'X'));
    }

    return texts;
}

// Whether the edits of a line longer than this change only some of its
// characters (edited_position()), and whether it is a seed after a fresh
// connection only.
inline constexpr std::size_t short_line = 80U;

// The seeds: each seed text after a fresh connection, and the short ones
// that read a sequence also after the sequence before it and after the same
// one.
[[nodiscard]] inline std::vector<Line> seeds()
{
    std::vector<Line> lines;

    for (const std::string& text : seed_texts())
    {
        lines.push_back({.last_before = 0U, .text = text});
    }

    for (const std::string& text : seed_texts())
    {
        const std::uint32_t seq = outcome_of({.last_before = 0U, .text = text}).reply_seq;

        if (seq == 0U || text.size() > short_line)
        {
            continue;
        }

        if (seq > 1U)
        {
            lines.push_back({.last_before = seq - 1U, .text = text});
        }

        lines.push_back({.last_before = seq, .text = text});
    }

    return lines;
}

// The tokens a line's edits replace: numbers (digits with any '.' and '-'
// next to them) and words (letters, '_' and '?').
[[nodiscard]] inline std::vector<std::pair<std::size_t, std::size_t>> tokens(std::string_view text, std::string_view alphabet, bool needs_digit)
{
    std::vector<std::pair<std::size_t, std::size_t>> found;
    std::size_t at = 0U;

    while (at < text.size())
    {
        const std::size_t begin = text.find_first_of(alphabet, at);

        if (begin == std::string_view::npos)
        {
            break;
        }

        const std::size_t end = std::min(text.find_first_not_of(alphabet, begin), text.size());
        const std::string_view token = text.substr(begin, end - begin);

        if (!needs_digit || token.find_first_of("0123456789") != std::string_view::npos)
        {
            found.emplace_back(begin, end - begin);
        }

        at = end;
    }

    return found;
}

inline constexpr std::string_view number_alphabet{"0123456789.-"};
inline constexpr std::string_view word_alphabet{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_?"};
inline constexpr auto replacement_characters = std::to_array<char>({' ', ':', '.', ',', '0', '1', '9', 'A', 'Z', 'a', '-', '_', '"', '\\', '\r', '\t', '\x7f', '\x80', '?'});

inline constexpr auto replacement_numbers = std::to_array<std::string_view>({
    "",
    "0",
    "1",
    "2",
    "01",
    "00",
    "9",
    "15",
    "16",
    "17",
    "75",
    "100",
    "4800",
    "3600000",
    "3600001",
    "4294967295",
    "4294967296",
    "18446744073709551615",
    "18446744073709551616",
    "99999999999999999999",
    "-1",
    "-0",
    "0.5",
    ".5",
    "5.",
    "1.2.3",
    "-.5",
    "1e3",
    "1234567890123456",
    "12345678901234567",
    "-1234567890123456",
    "0.000000000000001",
    "x",
});

inline constexpr auto replacement_words = std::to_array<std::string_view>({
    "",          "X",    "ON",       "OFF",  "FREQ",  "CI",     "RATE",  "PEAK", "NOW",  "CW",      "STATIC",   "FADE",      "SWEEP",     "short", "long",
    "zero",      "drop", "CONTINUE", "ACK",  "WAV",   "BIN",    "INFO?", "PLAY", "STOP", "CUT",     "PLAYLIST", "ENCODE",    "GENERATE",  "LOAD",  "SEED",
    "REFERENCE", "AT",   "MEDIA",    "HOST", "LOCAL", "OUTPUT", "CODEC", "USB",  "USB?", "ENGINE?", "STATUS?",  "COUNTERS?", "PLAYLIST?",
});

inline constexpr auto suffixes = std::to_array<std::string_view>({"\r", "\r\r", " ", "  ", "\t", "\x7f", "\x80", " X", ":", ","});

inline constexpr auto at_prefixes = std::to_array<std::string_view>({
    "AT:0 ",
    "AT:1 ",
    "AT:01 ",
    "AT: ",
    "AT:",
    "AT:18446744073709551615 ",
    "AT:18446744073709551616 ",
    "AT:x ",
    "at:0 ",
    "AT:0  ",
    "AT:0:",
});

// Whether the edits of a long line change the character at `at`: its first
// 48 and last 16 characters, and every 16th between.
[[nodiscard]] inline bool edited_position(std::size_t at, std::size_t size)
{
    constexpr std::size_t head = 48U;
    constexpr std::size_t tail = 16U;
    constexpr std::size_t stride = 16U;
    return size <= short_line || at < head || at + tail >= size || at % stride == 0U;
}

// The lines one small change to `text` makes, each once and none equal to
// `text`, in a fixed order.
[[nodiscard]] inline std::vector<std::string> edits_of(std::string_view text)
{
    std::vector<std::string> edits;
    std::set<std::string, std::less<>> seen{std::string{text}};
    const auto add = [&edits, &seen](std::string edit)
    {
        if (seen.insert(edit).second)
        {
            edits.push_back(std::move(edit));
        }
    };

    const std::string line{text};

    for (std::size_t at = 0U; at < line.size(); ++at)
    {
        add(line.substr(0U, at));

        if (!edited_position(at, line.size()))
        {
            continue;
        }

        add(line.substr(0U, at) + line.substr(at + 1U));
        add(line.substr(0U, at + 1U) + line.substr(at));

        if (at + 1U < line.size())
        {
            std::string swapped = line;
            std::swap(swapped.at(at), swapped.at(at + 1U));
            add(std::move(swapped));
        }

        for (const char replacement : replacement_characters)
        {
            std::string replaced = line;
            replaced.at(at) = replacement;
            add(std::move(replaced));
        }
    }

    for (const auto& [begin, size] : tokens(line, number_alphabet, true))
    {
        for (const std::string_view replacement : replacement_numbers)
        {
            add(line.substr(0U, begin) + std::string{replacement} + line.substr(begin + size));
        }
    }

    for (const auto& [begin, size] : tokens(line, word_alphabet, false))
    {
        std::string lower = line.substr(begin, size);
        std::ranges::transform(lower, lower.begin(), [](char character) { return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character; });

        add(line.substr(0U, begin) + lower + line.substr(begin + size));

        for (const std::string_view replacement : replacement_words)
        {
            add(line.substr(0U, begin) + std::string{replacement} + line.substr(begin + size));
        }
    }

    for (const std::string_view suffix : suffixes)
    {
        add(line + std::string{suffix});
    }

    const std::size_t space = line.find(' ');

    if (space != std::string::npos)
    {
        for (const std::string_view prefix : at_prefixes)
        {
            add(line.substr(0U, space + 1U) + std::string{prefix} + line.substr(space + 1U));
        }

        add(line.substr(0U, space) + line.substr(space + 1U));
        add(line.substr(space + 1U));
        add("A" + line.substr(space));
    }

    return edits;
}

} // namespace live_protocol_corpus
