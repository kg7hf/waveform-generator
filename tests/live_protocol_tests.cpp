// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/live_protocol.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using namespace waveform_generator::live_protocol;

// The checks below were written for parse()'s bool form: whether it takes
// `text`, its request then in `request`.
static bool parse(std::string_view text, std::uint32_t& last_sequence, Request& request)
{
    const m110::Result<Request> parsed = waveform_generator::live_protocol::parse(text, last_sequence);

    if (!parsed.has_value())
    {
        return false;
    }

    request = parsed.value();
    return true;
}

// Whether parse() refuses `text` with bad_request, the wire's BAD_REQUEST.
static bool refused(std::string_view text, std::uint32_t& last_sequence)
{
    const m110::Status status = waveform_generator::live_protocol::parse(text, last_sequence).status();
    return status.code == bad_request.code && std::string_view{status.message} == "BAD_REQUEST";
}

// A refused line, the connection's last sequence after it, and the seq of its
// reply.
struct Refusal
{
    std::string_view line;
    std::uint32_t last_after;
    std::uint32_t reply_seq;
};

// Every refusal is bad_request, whatever refused the line: the PLAYLIST
// parser's own causes become it, as on the wire. The reply's seq
// (reply_sequence()) is the sequence the line starts with when parse() could
// read one, refused or not, else 0; and that sequence is consumed only when
// it is above the last one. Starts after the sequence 5.
static void check_refusals()
{
    namespace cut = waveform_generator::cut;
    namespace playlist = waveform_generator::playlist;
    constexpr std::uint32_t first_last = 5U;
    constexpr auto refusals = std::to_array<Refusal>({
        {"5 INFO?", 5U, 5U},
        {"4 INFO?", 5U, 4U},
        {"6 UNKNOWN", 6U, 6U},
        {"7 PLAYLIST:a.pl", 7U, 7U},
        {"8 PLAYLIST ACK:01", 8U, 8U},
        {"0 INFO?", 8U, 0U},
        {"4294967296 INFO?", 8U, 0U},
        {"01 INFO?", 8U, 0U},
        {"9", 8U, 0U},
        {"9 INFO?\x01", 8U, 0U},
        {"9 INFO?\x1f", 8U, 0U},
        {"9 INFO?\x7f", 8U, 0U},
        {"9 INFO?\r\r", 8U, 0U},
    });
    std::uint32_t seq = first_last;

    for (const Refusal& refusal : refusals)
    {
        REQUIRE((refused(refusal.line, seq) && seq == refusal.last_after && reply_sequence(refusal.line) == refusal.reply_seq));
    }

    Request request;
    REQUIRE((std::string_view{parse_playlist_command("PLAYLIST:a.pl", request).message} == playlist::bad_name.message &&
             std::string_view{parse_playlist_command("PLAYLIST ACK:01", request).message} == playlist::bad_ack.message));
    // A CUT of one entry too many; the longest line, and one byte more.
    constexpr std::string_view cut_head{"9 CUT:1:1:zero"};
    constexpr std::string_view cut_entry{",1:1:zero"};
    constexpr std::string_view longest_head{"10 CW FREQ "};
    std::string too_many{cut_head};

    for (std::size_t index = 0U; index < cut::max_entries; ++index)
    {
        too_many += cut_entry;
    }

    const std::string longest = std::string{longest_head} + std::string(wire_capacity - 1U - longest_head.size(), '0');
    const std::uint32_t cut_seq = reply_sequence(too_many);
    const std::uint32_t longest_seq = reply_sequence(longest);
    REQUIRE((refused(too_many, seq) && seq == cut_seq && cut_seq == refusals.back().last_after + 1U));
    REQUIRE((longest.size() + 1U == wire_capacity && refused(longest, seq) && seq == longest_seq && longest_seq == cut_seq + 1U));
    REQUIRE((refused(longest + "0", seq) && reply_sequence(longest + "0") == 0U && seq == longest_seq));
    const std::string accepted = std::to_string(longest_seq + 1U) + " INFO?\r";
    REQUIRE((waveform_generator::live_protocol::parse(accepted, seq).has_value() && reply_sequence(accepted) == longest_seq + 1U && seq == longest_seq + 1U));
    REQUIRE((uint64("18446744073709551615").value() == UINT64_MAX && uint64("0").value() == 0U));
    REQUIRE((std::string_view{uint64("18446744073709551616").status().message} == bad_request.message && !uint64("01").has_value() && !uint64("").has_value() && !uint64("-1").has_value()));
}

// CUT: the entries as given (the player judges the rules), none for CUT alone,
// and one request carries 16 entries of 32-bit offsets after a 10-digit
// sequence. Uses the sequences 46 to 54, then 4294967292 to 4294967294.
static void check_cut(std::uint32_t& seq, Request& r)
{
    namespace cut = waveform_generator::cut;
    REQUIRE((parse("46 CUT", seq, r) && r.kind == Kind::cut && r.cuts.empty()));
    REQUIRE((parse("47 CUT:0:4800:zero,96000:1:drop\r", seq, r) && r.kind == Kind::cut && r.cuts.size() == 2U && r.cuts.entries()[1] == cut::parse_entry("96000:1:drop").value()));
    REQUIRE((parse("48 CUT:1:0:drop,1:1:zero", seq, r) && r.cuts.size() == 2U));
    REQUIRE((!parse("49 CUT:", seq, r)));
    REQUIRE((!parse("50 AT:0 CUT", seq, r)));
    REQUIRE((!parse("51 CUT ", seq, r)));
    REQUIRE((!parse("52 CUT:1:1:drop,", seq, r)));
    REQUIRE((!parse("53 CUT:1:1:Drop", seq, r)));
    REQUIRE((!parse("54 cut", seq, r)));
    std::string widest{"4294967294 CUT:"};
    std::string seventeen{"4294967292 CUT:1:1:zero"};

    for (std::size_t index = 0U; index < cut::max_entries; ++index)
    {
        widest += (index == 0U ? "" : ",") + std::string{widest_cut_entry};
        seventeen += ",1:1:zero";
    }

    REQUIRE((widest.size() + request_end.size() <= wire_capacity && !parse(seventeen, seq, r)));
    REQUIRE((!parse(std::string{"4294967293 CUT:"} + std::string(wire_capacity, '1'), seq, r)));
    REQUIRE((parse(widest + "\r", seq, r) && r.kind == Kind::cut && r.cuts.size() == cut::max_entries));
}

// PLAYLIST?, PLAYLIST:<NAME>[:CONTINUE] and PLAYLIST ACK:<seq> on a
// connection of their own: the name is an upper-case 8.3 name whose
// extension may be left out, the policy word is CONTINUE alone, and the ACK
// number is a canonical uint32 (live_command.hpp asserts that none is a
// live control).
static void check_playlist()
{
    namespace playlist = waveform_generator::playlist;
    std::uint32_t seq{};
    Request r;
    REQUIRE((parse("1 PLAYLIST?", seq, r) && r.kind == Kind::playlist_status));
    REQUIRE((parse("2 PLAYLIST?\r", seq, r) && r.kind == Kind::playlist_status));
    REQUIRE((!parse("3 PLAYLIST? ", seq, r) && !parse("4 playlist?", seq, r) && !parse("5 AT:0 PLAYLIST?", seq, r) && !parse("6 PLAYLIST", seq, r)));
    REQUIRE((parse("7 PLAYLIST:BATTERY.PL", seq, r) && r.kind == Kind::playlist_start && std::string_view{std::span{r.name}.data()} == "BATTERY.PL" && r.policy == playlist::Policy::stop));
    REQUIRE((parse("8 PLAYLIST:BATTERY.PL:CONTINUE\r", seq, r) && r.kind == Kind::playlist_start && std::string_view{std::span{r.name}.data()} == "BATTERY.PL" &&
             r.policy == playlist::Policy::continue_on_error));
    REQUIRE((parse("9 PLAYLIST:LADDER", seq, r) && std::string_view{std::span{r.name}.data()} == "LADDER" && parse("10 PLAYLIST:ABCDEFGH.XYZ", seq, r)));
    REQUIRE((!parse("11 PLAYLIST:battery.pl", seq, r) && !parse("12 PLAYLIST:BATTERY.PL:continue", seq, r) && !parse("13 PLAYLIST:BATTERY.PL:", seq, r) && !parse("14 PLAYLIST:", seq, r) &&
             !parse("15 PLAYLIST:../A.PL", seq, r) && !parse("16 PLAYLIST:ABCDEFGHI.PL", seq, r) && !parse("17 PLAYLIST:A.PL:CONTINUE:CONTINUE", seq, r) && !parse("18 PLAYLIST:A.PLAY", seq, r) &&
             !parse("19 AT:0 PLAYLIST:A.PL", seq, r) && !parse("20 PLAYLIST: A.PL", seq, r)));
    REQUIRE((parse("21 PLAYLIST ACK:0", seq, r) && r.kind == Kind::playlist_ack && r.ack_seq == 0U));
    REQUIRE((parse("22 PLAYLIST ACK:4294967295", seq, r) && r.ack_seq == UINT32_MAX));
    REQUIRE((!parse("23 PLAYLIST ACK:4294967296", seq, r) && !parse("24 PLAYLIST ACK:01", seq, r) && !parse("25 PLAYLIST ACK:", seq, r) && !parse("26 PLAYLIST ACK:-1", seq, r) &&
             !parse("27 PLAYLIST ACK 5", seq, r) && !parse("28 PLAYLIST  ACK:5", seq, r) && !parse("29 AT:0 PLAYLIST ACK:5", seq, r)));
    // Each refusal has its own cause, though the wire answers BAD_REQUEST for all.
    const auto cause = [](std::string_view text)
    {
        Request request;
        return std::string_view{parse_playlist_command(text, request).message};
    };

    constexpr std::uint32_t acknowledged = 7U;
    REQUIRE((parse_playlist_command("PLAYLIST:A.PL", r).is_ok() && r.kind == Kind::playlist_start && parse_playlist_command("PLAYLIST ACK:" + std::to_string(acknowledged), r).is_ok() &&
             r.kind == Kind::playlist_ack && r.ack_seq == acknowledged));
    REQUIRE((cause("PLAYLIST:battery.pl") == playlist::bad_name.message && cause("PLAYLIST:") == playlist::bad_name.message && cause("PLAYLIST:ABCDEFGHI.PL") == playlist::bad_name.message));
    REQUIRE((cause("PLAYLIST:A.PL:CONTNUE") == playlist::bad_policy.message && cause("PLAYLIST:A.PL:") == playlist::bad_policy.message &&
             cause("PLAYLIST:A.PL:CONTINUE:CONTINUE") == playlist::bad_policy.message));
    REQUIRE((cause("PLAYLIST ACK:01") == playlist::bad_ack.message && cause("PLAYLIST ACK:") == playlist::bad_ack.message && cause("PLAYLIST ACK:4294967296") == playlist::bad_ack.message));
    REQUIRE((cause("PLAYLIST") == playlist::bad_command.message && cause("PLAYLIST ACK 5") == playlist::bad_command.message && cause("PLAYLISTS:A.PL") == playlist::bad_command.message));
}

// ENGINE? is a plain query on a connection of its own, like PLAYLIST?.
static void check_engine_query()
{
    std::uint32_t seq{};
    Request r;
    REQUIRE((parse("1 ENGINE?", seq, r) && r.kind == Kind::engine_status && r.seq == seq));
    REQUIRE((parse("2 ENGINE?\r", seq, r) && r.kind == Kind::engine_status));
    REQUIRE((!parse("3 ENGINE? ", seq, r) && !parse("4 engine?", seq, r) && !parse("5 AT:0 ENGINE?", seq, r) && !parse("6 ENGINE", seq, r)));
    static_assert(Kind::engine_status < Kind::cw_on, "ENGINE? is a query, not a live control");
}

// FILES? is a read-only query. Its optional offset is a canonical uint64,
// defaulting to zero; it remains before live controls in the kind ordering.
static void check_files_query()
{
    std::uint32_t seq{};
    Request r;
    REQUIRE((parse("1 FILES?", seq, r) && r.kind == Kind::files && r.integer == 0U));
    REQUIRE((parse("2 FILES? 0", seq, r) && r.kind == Kind::files && r.integer == 0U));
    REQUIRE((parse("3 FILES? 18446744073709551615", seq, r) && r.kind == Kind::files && r.integer == UINT64_MAX));

    for (const std::string_view text :
         {"4 FILES? +1", "5 FILES? -1", "6 FILES? 01", "7 FILES? 18446744073709551616", "8 FILES? 1 2", "9 FILES? ", "10 FILES? 1 ", "11 AT:0 FILES?", "12 FILES? ?", "13 files?"})
    {
        REQUIRE((!parse(text, seq, r)));
    }

    REQUIRE((!parse("14 FILES??", seq, r) && !parse("15 FILES?\rX", seq, r)));
    static_assert(Kind::files < Kind::cw_on, "FILES? is a query, not a live control");
}

static void check_prepare_commands()
{
    std::uint32_t seq{};
    Request request;
    REQUIRE((parse("1 PREPARE?", seq, request) && request.kind == Kind::prepare_status));
    REQUIRE((parse("2 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP", seq, request) && request.kind == Kind::prepare_file && std::string_view{request.preparation.output.data()} == "OUT.WAV" &&
             std::string_view{request.preparation.source.data()} == "SRC.WAV" && std::string_view{request.preparation.scenario.data()} == "SCN.SCN" &&
             request.preparation.retention == waveform_generator::prepare::Retention::keep && !request.preparation.seed_override));
    REQUIRE((parse("3 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:18446744073709551615", seq, request) && request.preparation.retention == waveform_generator::prepare::Retention::temporary &&
             request.preparation.seed_override && request.preparation.seed == UINT64_MAX));
    static_assert(Kind::prepare_status < Kind::cw_on && Kind::prepare_file < Kind::cw_on);

    for (const std::string_view line :
         {"4 PREPARE:out.WAV:SRC.WAV:SCN.SCN:KEEP", "5 PREPARE:OUT.WAV:../SRC.WAV:SCN.SCN:KEEP", "6 PREPARE:OUT.WAV:SRC.WAV:SCN.WAV:KEEP", "7 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:keep",
          "8 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:01", "9 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:-1", "10 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:18446744073709551616",
          "11 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:", "12 AT:0 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP", "13 PREPARE:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:2", "14 PREPARE? ", "15 AT:0 PREPARE?"})
    {
        REQUIRE((!parse(line, seq, request)));
    }

    REQUIRE((parse("16 PREPAREPLAY?", seq, request) && request.kind == Kind::prepare_play_status));
    REQUIRE((!parse("17 AT:0 PREPAREPLAY?", seq, request)));
    REQUIRE((!parse("18 PREPAREPLAY? trailing", seq, request)));
}

static void check_prepare_play_commands()
{
    std::uint32_t seq{};
    Request request;
    REQUIRE((parse("1 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0", seq, request) && request.kind == Kind::prepare_play && request.preparation_rate == 1U && request.preparation_margin_frames == 0U &&
             request.preparation.retention == waveform_generator::prepare::Retention::keep && !request.preparation.seed_override));
    seq = 0U;
    REQUIRE((parse("1 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:1:0", seq, request) && request.kind == Kind::prepare_play && request.preparation_rate == 1U && request.preparation_margin_frames == 0U &&
             request.preparation.retention == waveform_generator::prepare::Retention::temporary && !request.preparation.seed_override));
    REQUIRE((parse("2 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:4294967295:4294967295:18446744073709551615", seq, request) && request.preparation_rate == UINT32_MAX &&
             request.preparation_margin_frames == UINT32_MAX && request.preparation.seed_override && request.preparation.seed == UINT64_MAX &&
             request.preparation.retention == waveform_generator::prepare::Retention::temporary));
    static_assert(Kind::prepare_play < Kind::cw_on);

    for (const std::string_view line :
         {"3 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:0:0", "4 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:01:0", "5 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:+1:0",
          "6 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:-1:0", "7 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:4294967296:0", "8 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:01",
          "9 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:+1", "10 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:-1", "11 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:4294967296",
          "12 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0:01", "13 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0:18446744073709551616", "14 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1",
          "15 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0:1:2", "16 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:TEMP:1:0:garbage", "17 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:keep:1:0",
          "18 PREPAREPLAY:out.WAV:SRC.WAV:SCN.SCN:KEEP:1:0", "19 AT:0 PREPAREPLAY:OUT.WAV:SRC.WAV:SCN.SCN:KEEP:1:0"})
    {
        REQUIRE((!parse(line, seq, request)));
    }
}

TEST_CASE("Live protocol parsing preserves command and sequence rules")
{
    check_playlist();
    check_engine_query();
    check_files_query();
    check_prepare_commands();
    check_prepare_play_commands();
    check_refusals();
    std::uint32_t seq{};
    Request r;
    REQUIRE((parse("1 INFO?", seq, r) && r.kind == Kind::info && r.seq == 1));
    REQUIRE((!parse("1 PLAY", seq, r) && seq == 1));
    REQUIRE((!parse("2 UNKNOWN", seq, r) && seq == 2));
    REQUIRE((!parse("02 PLAY", seq, r) && seq == 2));
    REQUIRE((!parse("3  PLAY", seq, r) && seq == 3));
    REQUIRE((parse("4 LOAD:REF_600.WAV\r", seq, r) && r.kind == Kind::load));
    REQUIRE((!parse("5 LOAD:../X.WAV", seq, r)));
    REQUIRE((!parse("6 LOAD:X.WAV ", seq, r)));
    REQUIRE((!parse("7 PLAY\rNOW", seq, r)));
    REQUIRE((parse("8 AT:18446744073709551615 CW CI -3", seq, r) && r.scheduled && r.frame == UINT64_MAX && r.value == -3));
    REQUIRE((!parse("9 AT:18446744073709551616 CW ON", seq, r)));
    REQUIRE((!parse("10 AT:0 STOP", seq, r)));
    REQUIRE((!parse("11 M110:600:long:PAYLOAD.BIN", seq, r)));
    REQUIRE((!parse("12 M110:600:long:PAYLOAD.WAV", seq, r)));
    REQUIRE((parse("13 GENERATE:600:long:PAYLOAD.BIN:TEST.WAV", seq, r) && r.rate == 600 && r.interleave == 1));
    seq = 12;
    REQUIRE((parse("13 SEED:18446744073709551615", seq, r) && r.integer == UINT64_MAX));
    REQUIRE((parse("14 REFERENCE:0.125", seq, r) && r.value == 0.125));
    REQUIRE((!parse("15 REFERENCE:nan", seq, r)));
    REQUIRE((parse("16 SWEEP CW FREQ 300 3400 16 500", seq, r) && r.steps == 16 && r.interval_ms == 500));
    REQUIRE((parse("17 SWEEP FADE 6 30 5 1000 250", seq, r) && r.kind == Kind::sweep_fade && r.duration_ms == 250));
    REQUIRE((!parse("18 SWEEP FADE 6 30 17 1000 250", seq, r)));
    REQUIRE((parse("19 PLAY:0123456789abcdef0123456789abcdef", seq, r) && r.run_id[31] == 'f'));
    REQUIRE((!parse("20 PLAY:0123456789ABCDEF0123456789abcdef", seq, r)));
    REQUIRE((parse("21 CW 1 ON", seq, r) && r.kind == Kind::cw_on && r.oscillator == 1 && r.value == 1));
    REQUIRE((parse("22 CW 3 FREQ 2800", seq, r) && r.kind == Kind::cw_frequency && r.oscillator == 3 && r.value == 2800));
    REQUIRE((parse("23 SWEEP CW 2 CI 20 -3 4 100", seq, r) && r.kind == Kind::sweep_ci && r.oscillator == 2 && r.steps == 4));
    REQUIRE((!parse("24 CW 4 ON", seq, r)));
    REQUIRE((!parse(std::string("25 CW FREQ ") + std::string(192, '0'), seq, r)));
    REQUIRE((parse("26 ENCODE TONE 1000 INPUT.BIN OUT.WAV", seq, r) && r.kind == Kind::generate_file && std::string(r.encoder) == "TONE" && std::string(r.profile) == "1000"));
    REQUIRE((parse("27 ENCODE CUSTOM R50:S170:F1000 INPUT.BIN OUT.WAV", seq, r) && std::string(r.profile) == "R50:S170:F1000"));
    REQUIRE((!parse("28 ENCODE lower 1000 INPUT.BIN OUT.WAV", seq, r)));
    REQUIRE((!parse("29 AT:0 ENCODE TONE 1000 INPUT.BIN OUT.WAV", seq, r)));
    REQUIRE((!parse(std::string("30 ENCODE TONE ") + std::string(32U, 'X') + " INPUT.BIN OUT.WAV", seq, r)));
    REQUIRE((!parse("31 ENCODE TONE bad\"profile INPUT.BIN OUT.WAV", seq, r)));
    REQUIRE((parse("32 OUTPUT CODEC", seq, r) && r.kind == Kind::output_codec && r.seq == seq));
    REQUIRE((parse("33 OUTPUT USB\r", seq, r) && r.kind == Kind::output_usb));
    REQUIRE((!parse("34 OUTPUT usb", seq, r)));
    REQUIRE((!parse("35 OUTPUT", seq, r)));
    REQUIRE((!parse("36 OUTPUT USB ", seq, r)));
    REQUIRE((!parse("37 OUTPUT  USB", seq, r)));
    REQUIRE((!parse("38 OUTPUT SPEAKER", seq, r)));
    REQUIRE((!parse("39 AT:0 OUTPUT USB", seq, r)));
    REQUIRE((parse("40 USB?", seq, r) && r.kind == Kind::usb_diagnostics && r.seq == seq));
    REQUIRE((parse("41 USB?\r", seq, r) && r.kind == Kind::usb_diagnostics));
    REQUIRE((!parse("42 AT:0 USB?", seq, r)));
    REQUIRE((!parse("43 USB? ", seq, r)));
    REQUIRE((!parse("44 usb?", seq, r)));
    REQUIRE((!parse("45 USB", seq, r)));
    check_cut(seq, r);
    REQUIRE((parse("4294967295 STOP", seq, r)));
    REQUIRE((!parse("4294967296 STOP", seq, r)));
}
