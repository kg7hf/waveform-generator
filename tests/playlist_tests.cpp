// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// PLAYLIST batch mode's portable logic (common/playlist.hpp) on the host: the
// file parser on valid files (comments, blank lines, tabs, CR LF, every
// option, the lane's six-item battery) fed whole and in any split, each
// refusal with the line it names, the line's characters (every printable one
// from space to '~' in a comment, nothing past it) and the routes' upper-case
// spelling, the limits (256 items, 511-character
// lines, 24-character labels, 16 cuts, 3600 s gaps), unique labels, the cut=
// grammar and rules of the CUT command, scn= and seed=, att= and its exact
// integer gains (0 dB leaves every PCM24 sample as it is), dur= (1 to
// 86400 s), the result ring
// (numbering, the full ring, acknowledgement, copies across its wrap), and
// the timing rules: gaps in whole milliseconds of 48 frames, the stream index
// where the next USB item starts, and how an item's outcome ends a playlist.
// With --check <file> it prints the parser's verdict on a playlist file as one
// JSON line, the code ("OK" when accepted), the line it names and the items,
// which tests/test_make_playlist.py (WFG_PLAYLIST_CHECKER) matches with the
// host's parser, host/make_playlist.py. With --next-start <first> <sent>
// <gap_frames>... it prints next_start_index() of each triple as one JSON
// array, which the same test matches with the host's offset prediction.
// --gain-table prints the attenuation gains and --attenuate <tenths>
// <sample>... the samples attenuated, which the same test matches entry by
// entry and sample by sample with host/attenuation.py.

#include "common/playlist.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
namespace pl = waveform_generator::playlist;
namespace cut = waveform_generator::cut;

class Checks
{
public:
    void expect(bool condition, const std::string& what)
    {
        if (!condition)
        {
            ++failures_;
            std::fputs(("FAIL: " + what + "\n").c_str(), stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

constexpr pl::OfferedRoutes both_routes{.usb = true};
constexpr pl::OfferedRoutes codec_only{};
// A gap in 48 kHz frames, from milliseconds.
[[nodiscard]] constexpr pl::GapFrames gap_ms(std::uint32_t milliseconds) noexcept
{
    return {.value = milliseconds * pl::frames_per_millisecond};
}

constexpr std::uint32_t five_seconds_ms = 5000U;
// The battery's items.
constexpr std::uint32_t battery_items = 6U;

// The bytes of `text` as the card stores them.
[[nodiscard]] std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

// The one table the tests parse into: it holds 256 items, too large for the stack.
[[nodiscard]] pl::ItemTable& table()
{
    static pl::ItemTable instance;
    return instance;
}

struct FileResult
{
    m110::Status status{};
    pl::LineNumber line{};
    std::uint32_t items{};
};

// A whole file through a fresh parser into the cleared table, fed in pieces
// of `split` bytes.
[[nodiscard]] FileResult parse_file(std::string_view text, std::size_t split = std::numeric_limits<std::size_t>::max(), pl::OfferedRoutes offered = both_routes)
{
    auto& items = table();
    items.clear();
    pl::FileParser parser{};
    const auto bytes = bytes_of(text);
    std::span<const std::uint8_t> rest{bytes};

    while (!rest.empty())
    {
        const auto piece = rest.first(std::min(split, rest.size()));
        const auto fed = parser.feed(piece, items, offered);

        if (!fed.is_ok())
        {
            return {.status = fed, .line = parser.line(), .items = items.size()};
        }

        rest = rest.subspan(piece.size());
    }

    const auto finished = parser.finish(items, offered);
    return {.status = finished, .line = parser.line(), .items = items.size()};
}

// The lane's acceptance battery (docs/live-control-protocol.md): six items
// with 5 s gaps on the USB route, with comments, a blank line, tabs, CR LF
// and every option.
constexpr std::string_view battery{"# lane acceptance battery: six items, 5 s gaps, USB route\r\n"
                                   "C24L05.WAV 5 USB label=C24L05\r\n"
                                   "C12L05.WAV 5 USB label=C12L05   # comment after an item\r\n"
                                   "\r\n"
                                   "C06L05.WAV\t5\tUSB\tlabel=C06L05\r\n"
                                   "FSRAND.WAV 5 USB label=FSRAND cut=48000:480:zero,96000:480:drop\r\n"
                                   "   # an indented comment\r\n"
                                   "C06L05N.WAV 5 USB label=C06L05N scn=NOISE.SCN seed=4294967295\r\n"
                                   "C24L10.WAV 5.000 USB label=C24L10"};

void test_battery(Checks& checks)
{
    for (const std::size_t split : {std::numeric_limits<std::size_t>::max(), std::size_t{1U}, std::size_t{7U}})
    {
        const auto name = "the battery fed in pieces of " + std::to_string(std::min<std::size_t>(split, battery.size()));
        const auto result = parse_file(battery, split);
        checks.expect(result.status.is_ok() && result.items == battery_items, name + ": every line parses into six items");

        if (result.items != battery_items)
        {
            continue;
        }

        const auto items = table().items();
        constexpr auto files = std::to_array<std::string_view>({"C24L05.WAV", "C12L05.WAV", "C06L05.WAV", "FSRAND.WAV", "C06L05N.WAV", "C24L10.WAV"});
        constexpr auto lines = std::to_array<std::uint32_t>({2U, 3U, 5U, 6U, 8U, 9U});
        bool fields = true;

        for (std::size_t index = 0U; index < items.size(); ++index)
        {
            const auto& item = items[index];
            fields = fields && item.file.view() == files.at(index) && item.gap == gap_ms(five_seconds_ms) && item.route == pl::Route::usb &&
                     item.label.view() == files.at(index).substr(0U, files.at(index).find('.')) && item.line.value == lines.at(index);
        }

        checks.expect(fields, name + ": files, 5 s gaps, the USB route, labels and line numbers");
        const auto& cuts = items[3].cuts;
        checks.expect(cuts.size() == 2U && cuts.entries()[0] == cut::parse_entry("48000:480:zero").value() && cuts.entries()[1] == cut::parse_entry("96000:480:drop").value(),
                      name + ": cut= gives the item its CUT schedule");
        checks.expect(items[4].scenario.view() == "NOISE.SCN" && items[4].seed == std::numeric_limits<std::uint32_t>::max() && items[0].scenario.empty() && !items[0].seed.has_value() &&
                          items[0].cuts.empty(),
                      name + ": scn= and seed= belong to their item only");
    }
}

// Whole seconds, decimals, and the limits; every other spelling is refused.
void test_gaps(Checks& checks)
{
    constexpr std::uint32_t max_ms = pl::max_gap_milliseconds;
    const auto valid = std::to_array<std::pair<std::string_view, std::uint32_t>>({
        {"0", 0U},
        {"5", five_seconds_ms},
        {"0.5", 500U},
        {"0.25", 250U},
        {"0.125", 125U},
        {"10.010", 10010U},
        {"3600", max_ms},
        {"3600.000", max_ms},
        {"0.001", 1U},
        {"0.000", 0U},
    });

    for (const auto& [text, milliseconds] : valid)
    {
        const auto gap = pl::parse_gap(text);
        checks.expect(gap.has_value() && gap.value() == gap_ms(milliseconds) && pl::gap_milliseconds(gap.value()) == milliseconds,
                      "the gap " + std::string{text} + " is " + std::to_string(milliseconds) + " ms of 48 frames");
    }

    for (const std::string_view text : {"", "-1", "+5", "05", "00", "1.", ".5", "1.2345", "3600.001", "3601", "9999", "99999", "1e3", "1,5", "5s", "0x10", "1..2"})
    {
        checks.expect(pl::parse_gap(text).status().message == pl::bad_gap.message, "the gap '" + std::string{text} + "' is refused");
    }

    checks.expect(pl::max_gap.value == max_ms * pl::frames_per_millisecond, "the longest gap is 3600 s of 48 kHz frames");
}

struct Refusal
{
    std::string text;
    std::string_view code;
    std::uint32_t line;
};

// A file whose line `line` (after `line - 1` lines of comments and blank
// lines) is `bad`.
[[nodiscard]] std::string with_bad_line(std::string_view bad, std::uint32_t line)
{
    std::string text;

    for (std::uint32_t before = 1U; before < line; ++before)
    {
        text += before % 2U == 0U ? "\n" : "# comment\n";
    }

    return text + std::string{bad} + "\nLAST.WAV 0 CODEC\n";
}

// Each refusal names the line it found, and its code.
void test_refusals_name_their_line(Checks& checks)
{
    const std::string seventeen_cuts = []
    {
        constexpr std::uint32_t spacing = 10U;
        std::string entries{"A.WAV 0 USB cut="};

        for (std::uint32_t index = 0U; index <= cut::max_entries; ++index)
        {
            entries += (index == 0U ? "" : ",") + std::to_string(index * spacing) + ":1:zero";
        }

        return entries;
    }();
    const std::vector<Refusal> refusals{
        {.text = with_bad_line("A.WAV 5", 3U), .code = "PLAYLIST_SYNTAX", .line = 3U},
        {.text = with_bad_line("A.WAV", 1U), .code = "PLAYLIST_SYNTAX", .line = 1U},
        {.text = with_bad_line("A.WAV 5 USB foo=1", 4U), .code = "PLAYLIST_SYNTAX", .line = 4U},
        {.text = with_bad_line("A.WAV 5 USB label=a label=b", 2U), .code = "PLAYLIST_SYNTAX", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB LABEL=a", 2U), .code = "PLAYLIST_SYNTAX", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB label =a", 2U), .code = "PLAYLIST_SYNTAX", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB\x01", 5U), .code = "PLAYLIST_SYNTAX", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB # caf\xc3\xa9", 6U), .code = "PLAYLIST_SYNTAX", .line = 6U},
        {.text = with_bad_line("A.WAV 5 USB # \x7f", 5U), .code = "PLAYLIST_SYNTAX", .line = 5U},
        {.text = with_bad_line("\xef\xbb\xbf"
                               "A.WAV 5 USB",
                               1U),
         .code = "PLAYLIST_SYNTAX",
         .line = 1U},
        {.text = with_bad_line("a.wav 5 USB", 2U), .code = "PLAYLIST_FILE", .line = 2U},
        {.text = with_bad_line("TOOLONGNA.WAV 5 USB", 2U), .code = "PLAYLIST_FILE", .line = 2U},
        {.text = with_bad_line("A.MP3 5 USB", 2U), .code = "PLAYLIST_FILE", .line = 2U},
        {.text = with_bad_line("../A.WAV 5 USB", 2U), .code = "PLAYLIST_FILE", .line = 2U},
        {.text = with_bad_line("A.WAV 5s USB", 7U), .code = "PLAYLIST_GAP", .line = 7U},
        {.text = with_bad_line("A.WAV 3600.5 USB", 7U), .code = "PLAYLIST_GAP", .line = 7U},
        {.text = with_bad_line("A.WAV 5 usb", 3U), .code = "PLAYLIST_ROUTE", .line = 3U},
        {.text = with_bad_line("A.WAV 5 codec", 3U), .code = "PLAYLIST_ROUTE", .line = 3U},
        {.text = with_bad_line("A.WAV 5 Codec", 3U), .code = "PLAYLIST_ROUTE", .line = 3U},
        {.text = with_bad_line("A.WAV 5 SPEAKER", 3U), .code = "PLAYLIST_ROUTE", .line = 3U},
        {.text = with_bad_line("A.WAV 5 USB label=", 2U), .code = "PLAYLIST_LABEL", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB label=a/b", 2U), .code = "PLAYLIST_LABEL", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB label=" + std::string(pl::label_max_chars + 1U, 'x'), 2U), .code = "PLAYLIST_LABEL", .line = 2U},
        {.text = "A.WAV 0 USB label=one\nB.WAV 0 USB\n# again\nC.WAV 0 USB label=one\n", .code = "PLAYLIST_LABEL_DUPLICATE", .line = 4U},
        {.text = with_bad_line("A.WAV 5 USB scn=fade.scn", 2U), .code = "PLAYLIST_SCN", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB scn=FADE.WAV", 2U), .code = "PLAYLIST_SCN", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB scn=", 2U), .code = "PLAYLIST_SCN", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB scn=F.SCN seed=-1", 2U), .code = "PLAYLIST_SEED", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB scn=F.SCN seed=01", 2U), .code = "PLAYLIST_SEED", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB scn=F.SCN seed=4294967296", 2U), .code = "PLAYLIST_SEED", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB seed=7", 3U), .code = "PLAYLIST_SEED_NO_SCN", .line = 3U},
        {.text = with_bad_line("A.WAV 5 USB cut=1:1", 2U), .code = "PLAYLIST_CUT", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=1:1:DROP", 2U), .code = "PLAYLIST_CUT", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=", 2U), .code = "PLAYLIST_CUT", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=1:1:drop,", 2U), .code = "PLAYLIST_CUT", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=CUT:1:1:drop", 2U), .code = "PLAYLIST_CUT", .line = 2U},
        {.text = with_bad_line(seventeen_cuts, 2U), .code = "PLAYLIST_CUT", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=1:0:drop", 2U), .code = "CUT_RANGE", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=1:4801:zero", 2U), .code = "CUT_RANGE", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=200:10:zero,100:10:drop", 2U), .code = "CUT_UNSORTED", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=100:10:drop,105:1:zero", 2U), .code = "CUT_OVERLAP", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB cut=0:4800:drop,4800:4800:drop,9600:4800:drop,14400:1:drop", 2U), .code = "CUT_DROP_RUN", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB att=3", 4U), .code = "PLAYLIST_ATT", .line = 4U},
        {.text = with_bad_line("A.WAV 5 USB att=0.5", 4U), .code = "PLAYLIST_ATT", .line = 4U},
        {.text = with_bad_line("A.WAV 5 USB att=+1", 4U), .code = "PLAYLIST_ATT", .line = 4U},
        {.text = with_bad_line("A.WAV 5 USB att=-5.25", 3U), .code = "PLAYLIST_ATT", .line = 3U},
        {.text = with_bad_line("A.WAV 5 USB att=-40.1", 3U), .code = "PLAYLIST_ATT", .line = 3U},
        {.text = with_bad_line("A.WAV 5 USB att=-05", 3U), .code = "PLAYLIST_ATT", .line = 3U},
        {.text = with_bad_line("A.WAV 5 USB att=-20dB", 3U), .code = "PLAYLIST_ATT", .line = 3U},
        {.text = with_bad_line("A.WAV 5 USB att=", 3U), .code = "PLAYLIST_ATT", .line = 3U},
        {.text = with_bad_line("A.WAV 5 USB att=-1 att=-2", 2U), .code = "PLAYLIST_SYNTAX", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB ATT=-1", 2U), .code = "PLAYLIST_SYNTAX", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB dur=0", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=0.999", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=86400.001", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=100000", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=1.2345", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=01", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=60s", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=-1", 5U), .code = "PLAYLIST_DUR", .line = 5U},
        {.text = with_bad_line("A.WAV 5 USB dur=1 dur=2", 2U), .code = "PLAYLIST_SYNTAX", .line = 2U},
        {.text = with_bad_line("A.WAV 5 USB # " + std::string(pl::max_line_chars, '#'), 2U), .code = "PLAYLIST_LINE_TOO_LONG", .line = 2U},
        {.text = "# nothing but comments\n\n   \t\n# and blank lines\n", .code = "PLAYLIST_EMPTY", .line = 0U},
        {.text = "", .code = "PLAYLIST_EMPTY", .line = 0U},
    };

    for (const auto& refusal : refusals)
    {
        const auto result = parse_file(refusal.text);
        const auto shown = refusal.text.substr(0U, std::min<std::size_t>(refusal.text.size(), 60U));
        checks.expect(std::string_view{result.status.message} == refusal.code && result.line.value == refusal.line, "'" + shown + "' is " + std::string{refusal.code} + " at line " +
                                                                                                                        std::to_string(refusal.line) + ", not " + result.status.message + " at line " +
                                                                                                                        std::to_string(result.line.value));
    }

    const auto unoffered = parse_file(with_bad_line("A.WAV 5 USB", 3U), std::numeric_limits<std::size_t>::max(), codec_only);
    checks.expect(unoffered.status.message == pl::route_unavailable.message && unoffered.line.value == 3U, "a USB item on an image without the sound card is PLAYLIST_ROUTE_UNAVAILABLE at its line");
    checks.expect(parse_file("A.WAV 5 CODEC\n", std::numeric_limits<std::size_t>::max(), codec_only).status.is_ok(), "codec items need no sound card");
}

// A comment may hold every printable character, space to '~' (DEL, one past
// it, is refused above), on a line of its own and after an item, which
// keeps its fields.
void test_comment_characters(Checks& checks)
{
    std::string printable(std::size_t{pl::last_printable - pl::first_printable} + 1U, pl::first_printable);
    std::ranges::iota(printable, pl::first_printable);
    constexpr std::size_t printable_count = 95U;
    checks.expect(printable.size() == printable_count && printable.back() == '~', "the test's comment holds the 95 printable characters");
    const auto result = parse_file("# " + printable + "\nA.WAV 0 CODEC # " + printable + "\n");
    checks.expect(result.status.is_ok() && result.items == 1U && table().items()[0].route == pl::Route::codec && table().items()[0].line.value == 2U && table().items()[0].label.empty(),
                  "every printable character, '~' included, stands in a comment");
}

// The limits hold exactly: 256 items, a 511-character line (its CR
// included), a 24-character label, 16 cuts, the widest seed and gap.
void test_limits(Checks& checks)
{
    std::string most;

    for (std::size_t index = 0U; index < pl::max_items; ++index)
    {
        most += "I" + std::to_string(index) + ".WAV 0 CODEC label=L" + std::to_string(index) + "\n";
    }

    const auto full = parse_file(most);
    checks.expect(full.status.is_ok() && full.items == pl::max_items, "256 items fit");
    const auto over = parse_file(most + "# one more\nLAST.WAV 0 CODEC\n");
    checks.expect(over.status.message == pl::too_many_items.message && over.line.value == pl::max_items + 2U, "a 257th item is PLAYLIST_TOO_MANY at its line");
    const std::string item{"A.WAV 3600 USB label=" + std::string(pl::label_max_chars, 'x') + " scn=F.SCN seed=4294967295 # "};
    const std::string longest = item + std::string(pl::max_line_chars - item.size() - 1U, '#') + "\r";
    checks.expect(longest.size() == pl::max_line_chars, "the test's longest line is 511 characters, its CR included");
    const auto at_limit = parse_file(longest + "\n" + std::string(pl::max_line_chars, '#'));
    checks.expect(at_limit.status.is_ok() && at_limit.items == 1U && table().items()[0].gap == pl::max_gap && table().items()[0].label.view().size() == pl::label_max_chars,
                  "a 511-character line, the longest gap and label and the widest seed are taken (a last line needs no LF)");
    const auto too_long = parse_file(longest + "#\n");
    checks.expect(too_long.status.message == pl::line_too_long.message && too_long.line.value == 1U, "a 512-character line is refused");
    std::string sixteen{"A.WAV 0 USB cut="};

    for (std::uint32_t index = 0U; index < cut::max_entries; ++index)
    {
        sixteen += (index == 0U ? "" : ",") + std::to_string(std::numeric_limits<std::uint32_t>::max() - cut::max_entries + index + 1U) + ":4800:zero";
    }

    const auto sixteen_cuts = parse_file(sixteen);
    checks.expect(sixteen_cuts.status.is_ok() && table().items()[0].cuts.size() == cut::max_entries, "16 cuts with 32-bit offsets fit one item");
}

namespace att = waveform_generator::attenuation;

// round_half_even(sample * gain / 2^31), written out with integer division
// and its remainder rather than att::apply()'s shift.
[[nodiscard]] std::int32_t divided(std::int32_t sample, std::uint32_t gain)
{
    constexpr auto unity = std::int64_t{att::unity_q31};
    const std::int64_t product = std::int64_t{sample} * std::int64_t{gain};
    auto quotient = product / unity;
    auto remainder = product % unity;

    if (remainder < 0)
    {
        remainder += unity;
        --quotient;
    }

    const bool up = 2 * remainder > unity || (2 * remainder == unity && quotient % 2 != 0);
    return static_cast<std::int32_t>(quotient + (up ? 1 : 0));
}

// att=: every spelling of 0 to -40.0 dB in tenths and nothing else, an item
// with each option and att=, the table falling from exactly 1 at 0 dB,
// every PCM24 sample unchanged at 0 dB, zero at every entry, and the
// shift-and-round arithmetic equal to plain division rounded half to even at
// every entry, on the extremes, around zero and on a spread of samples.
void test_attenuation(Checks& checks)
{
    const auto valid = std::to_array<std::pair<std::string_view, std::uint32_t>>({
        {"0", 0U},
        {"0.0", 0U},
        {"-0", 0U},
        {"-0.0", 0U},
        {"-0.1", 1U},
        {"-1", 10U},
        {"-6.5", 65U},
        {"-20", 200U},
        {"-20.0", 200U},
        {"-39.9", 399U},
        {"-40", 400U},
        {"-40.0", 400U},
    });

    for (const auto& [text, tenths] : valid)
    {
        const auto parsed = att::parse(text);
        checks.expect(parsed.has_value() && parsed.value().value() == tenths, "att=" + std::string{text} + " is " + std::to_string(tenths) + " tenths of a dB");
    }

    for (const std::string_view text : {"1", "0.1", "+0", "+1", "-40.1", "-41", "-99", "-100", "-5.25", "-05", "-00", "-.5", "-5.", "-", "", "--1", "-1e1", "-20dB", " -1"})
    {
        checks.expect(att::parse(text).status().message == pl::bad_attenuation.message, "att=" + std::string{text} + " is refused with PLAYLIST_ATT");
    }

    constexpr att::Tenths twelve_point_three = att::Tenths::of(123U).value();
    checks.expect(!att::Tenths::of(att::max_tenths + 1U).has_value() && att::Tenths::of(att::max_tenths + 1U).status().message == pl::bad_attenuation.message &&
                      att::Tenths::of(att::max_tenths).has_value(),
                  "an attenuation past -40.0 dB cannot be made: Tenths::of() refuses it with PLAYLIST_ATT");
    const auto line = parse_file("A.WAV 0.5 USB label=a scn=F.SCN seed=1 cut=10:1:zero att=-12.3\nB.WAV 0 CODEC att=0\n");
    checks.expect(line.status.is_ok() && line.items == 2U && table().items()[0].attenuation == twelve_point_three && table().items()[1].attenuation == att::none,
                  "att= stands with every other option, and att=0 is 0 dB");
    const std::span gains{att::gains_q31};
    checks.expect(gains.front() == att::unity_q31 && std::ranges::is_sorted(gains, std::ranges::greater{}) && std::ranges::adjacent_find(gains) == gains.end(),
                  "the table falls from exactly 1 at 0 dB with every tenth");
    bool unity = true;

    for (std::int32_t sample = waveform_generator::audio::pcm24_min; sample <= waveform_generator::audio::pcm24_max; ++sample)
    {
        unity = unity && att::apply(att::Gain::of(att::none), sample) == sample;
    }

    checks.expect(unity, "0 dB passes every PCM24 sample through unchanged");
    std::vector<std::int32_t> samples{waveform_generator::audio::pcm24_min, waveform_generator::audio::pcm24_max, -2, -1, 0, 1, 2};
    constexpr std::uint32_t spread_step = 9973U;

    for (std::int32_t sample = waveform_generator::audio::pcm24_min; sample < waveform_generator::audio::pcm24_max - static_cast<std::int32_t>(spread_step);
         sample += static_cast<std::int32_t>(spread_step))
    {
        samples.push_back(sample);
    }

    bool rounded = true;
    bool zero = true;

    for (std::uint32_t tenths = 0U; tenths <= att::max_tenths; ++tenths)
    {
        const auto gain = att::Gain::of(att::Tenths::of(tenths).value());
        zero = zero && att::apply(gain, 0) == 0 && gain.q31() == gains[tenths];

        for (const auto sample : samples)
        {
            rounded = rounded && att::apply(gain, sample) == divided(sample, gain.q31());
        }
    }

    checks.expect(zero, "zero stays zero at every attenuation");
    checks.expect(rounded, "the shift-and-round product is plain division rounded half to even, at every attenuation");
}

// dur=: 1 to 86400 s in whole milliseconds of 48 frames, spelt as a gap,
// beside every other option; an item without it has none, and the
// end causes' names.
void test_durations(Checks& checks)
{
    constexpr std::uint32_t one_hour_ms = 3600000U;
    const auto valid = std::to_array<std::pair<std::string_view, std::uint32_t>>({
        {"1", 1000U},
        {"1.000", 1000U},
        {"1.001", 1001U},
        {"60", 60000U},
        {"3600", one_hour_ms},
        {"18000", 5U * one_hour_ms},
        {"86399.999", 86399999U},
        {"86400", 86400000U},
        {"86400.0", 86400000U},
    });

    for (const auto& [text, milliseconds] : valid)
    {
        const auto parsed = pl::parse_duration(text);
        checks.expect(parsed.has_value() && parsed.value().value == milliseconds * pl::frames_per_millisecond, "dur=" + std::string{text} + " is " + std::to_string(milliseconds) + " ms of 48 frames");
    }

    for (const std::string_view text : {"", "0", "0.5", "0.999", "-1", "+1", "01", "1.", ".5", "1.2345", "86400.001", "86401", "99999", "100000", "1e3", "5s"})
    {
        checks.expect(pl::parse_duration(text).status().message == pl::bad_duration.message, "dur=" + std::string{text} + " is refused with PLAYLIST_DUR");
    }

    const auto line = parse_file("A.WAV 1 USB dur=3600 att=-30 cut=172799000:4800:zero label=hour scn=F.SCN seed=2\nB.WAV 0 CODEC\n");
    checks.expect(line.status.is_ok() && line.items == 2U && table().items()[0].duration == pl::DurationFrames{.value = one_hour_ms * pl::frames_per_millisecond} &&
                      !table().items()[1].duration.has_value(),
                  "dur= stands with every other option, and an item without it has none");
    constexpr std::uint32_t day_frames = 4147200000U;
    checks.expect(pl::max_duration.value == day_frames && pl::max_duration.value > pl::max_gap.value, "the longest duration is 24 h of 48 kHz frames");
    checks.expect(pl::end_cause_name(pl::EndCause::missed_interval) == "missed_interval" && pl::end_cause_name(pl::EndCause::no_stream) == "no_stream" &&
                      pl::end_cause_name(pl::EndCause::stream_closed) == "stream_closed",
                  "the end causes' names");
}

// Labels are optional and unique when given; the same file may appear twice.
void test_labels(Checks& checks)
{
    constexpr std::uint32_t items = 5U;
    const auto result = parse_file("A.WAV 0 USB\nA.WAV 0 USB\nA.WAV 0 USB label=x\nA.WAV 0 USB label=X\nA.WAV 0 USB label=a.b-c_D9\n");
    checks.expect(result.status.is_ok() && result.items == items && table().items().front().label.empty() && table().items().back().label.view() == "a.b-c_D9",
                  "items without labels never clash, labels are case-sensitive, and . - _ are label characters");
}

// The names the protocol takes.
void test_names(Checks& checks)
{
    for (const std::string_view name : {"BATTERY.PL", "LADDER", "A", "B_1.TXT", "ABCDEFGH.XYZ"})
    {
        checks.expect(pl::is_playlist_name(name), std::string{name} + " is a playlist name");
    }

    for (const std::string_view name : {"", "battery.pl", "ABCDEFGHI.PL", "A.PLAY", ".PL", "A.", "A.B.C", "A-B.PL", "A B.PL", "../A.PL"})
    {
        checks.expect(!pl::is_playlist_name(name), "'" + std::string{name} + "' is no playlist name");
    }

    checks.expect(pl::is_short_name("C24L05.WAV", pl::wav_extension) && !pl::is_short_name("C24L05.wav", pl::wav_extension) && !pl::is_short_name("C24L05", pl::wav_extension) &&
                      pl::is_short_name("NOISE.SCN", pl::scenario_extension),
                  "an item's WAV and scenario are upper-case 8.3 names with their extensions");
}

[[nodiscard]] pl::ResultBlock block_for(std::uint32_t item)
{
    return {.item = item, .file = pl::ShortName::from("A.WAV")};
}

// The ring numbers its blocks from 1, holds 32, refuses a 33rd, frees by
// acknowledgement and copies the oldest first across its wrap.
void test_result_ring(Checks& checks)
{
    pl::ResultRing ring;
    checks.expect(ring.size() == 0U && ring.oldest_seq() == 0U && ring.next_seq() == pl::first_seq, "an empty ring numbers its first block 1");
    bool pushed = true;

    for (std::uint32_t item = 0U; item < pl::result_capacity; ++item)
    {
        pushed = ring.push(block_for(item)).is_ok() && pushed;
    }

    checks.expect(pushed && ring.full() && ring.size() == pl::result_capacity && ring.oldest_seq() == 1U && ring.next_seq() == pl::result_capacity + 1U, "32 blocks fill the ring, numbered 1 to 32");
    const auto refused = ring.push(block_for(pl::result_capacity));
    checks.expect(refused.message == pl::ring_full.message && ring.size() == pl::result_capacity && ring.next_seq() == pl::result_capacity + 1U, "a full ring refuses a block and changes nothing");
    const auto beyond = ring.acknowledge(ring.next_seq());
    checks.expect(!beyond.has_value() && beyond.status().message == pl::ack_range.message && ring.size() == pl::result_capacity,
                  "an acknowledgement of a number not given yet is PLAYLIST_ACK_RANGE and frees nothing");
    constexpr std::uint32_t acknowledged = 5U;
    const auto freed = ring.acknowledge(acknowledged);
    checks.expect(freed.has_value() && freed.value() == acknowledged && ring.size() == pl::result_capacity - acknowledged && ring.oldest_seq() == acknowledged + 1U,
                  "acknowledging 5 frees blocks 1 to 5");
    const auto again = ring.acknowledge(acknowledged - 2U);
    checks.expect(again.has_value() && again.value() == 0U && ring.oldest_seq() == acknowledged + 1U, "an acknowledgement of freed blocks frees nothing");

    for (std::uint32_t item = 0U; item < acknowledged; ++item)
    {
        pushed = ring.push(block_for(pl::result_capacity + item)).is_ok() && pushed;
    }

    std::array<pl::ResultBlock, pl::blocks_per_reply> oldest{};
    const auto copied = ring.copy_oldest(oldest);
    checks.expect(pushed && ring.full() && copied == pl::blocks_per_reply && oldest[0].seq == acknowledged + 1U && oldest[1].seq == acknowledged + 2U &&
                      oldest.back().seq == acknowledged + pl::blocks_per_reply && oldest[0].item == acknowledged,
                  "the ring wraps, and copies its oldest blocks first");
    const auto all = ring.acknowledge(ring.next_seq() - 1U);
    checks.expect(all.has_value() && all.value() == pl::result_capacity && ring.size() == 0U && ring.oldest_seq() == 0U && ring.copy_oldest(oldest) == 0U,
                  "acknowledging the newest number frees every block");
    checks.expect(ring.next_seq() == pl::result_capacity + acknowledged + 1U, "numbers are never given twice: 37 blocks so far, the next is 38");
    ring.clear();
    checks.expect(ring.size() == 0U && ring.next_seq() == pl::first_seq, "clear() is the ring at boot");
}

// Device time: a USB item's next item starts the gap after the packet that
// ends it; the arithmetic wraps as stream indices do. An item's outcome ends
// the playlist only under STOP or the stop policy.
void test_timing(Checks& checks)
{
    constexpr std::uint32_t first = 4800U;
    constexpr std::uint32_t five_seconds = five_seconds_ms * pl::frames_per_millisecond;
    // Five seconds of frames, whole packets; and one frame more.
    constexpr std::uint32_t whole_packets = 240000U;
    constexpr std::uint32_t one_frame_more = whole_packets + 1U;
    checks.expect(pl::whole_chunk_frames(0U) == 0U && pl::whole_chunk_frames(1U) == pl::chunk_frames && pl::whole_chunk_frames(pl::chunk_frames) == pl::chunk_frames &&
                      pl::whole_chunk_frames(pl::chunk_frames + 1U) == 2U * pl::chunk_frames,
                  "frames round up to whole 48-frame packets");
    checks.expect(pl::next_start_index(first, whole_packets, gap_ms(five_seconds_ms)) == first + whole_packets + five_seconds &&
                      pl::next_start_index(first, one_frame_more, gap_ms(five_seconds_ms)) == first + whole_packets + pl::chunk_frames + five_seconds,
                  "a whole-packet item is followed by exactly its gap; a partial last packet's rest of silence comes first");
    constexpr std::uint32_t near_wrap = std::numeric_limits<std::uint32_t>::max() - pl::chunk_frames + 1U;
    checks.expect(pl::next_start_index(near_wrap, pl::chunk_frames, gap_ms(1U)) == pl::chunk_frames, "the index wraps after 2^32 frames");
    using pl::Outcome;
    using pl::Policy;
    using pl::State;
    checks.expect(!pl::end_after(Outcome::done, Policy::stop, false).has_value() && !pl::end_after(Outcome::done, Policy::continue_on_error, false).has_value() &&
                      pl::end_after(Outcome::error, Policy::stop, false) == State::error && !pl::end_after(Outcome::error, Policy::continue_on_error, false).has_value() &&
                      pl::end_after(Outcome::aborted, Policy::continue_on_error, false) == State::aborted && pl::end_after(Outcome::done, Policy::continue_on_error, true) == State::aborted &&
                      pl::end_after(Outcome::error, Policy::stop, true) == State::aborted,
                  "STOP aborts whatever the outcome; an error ends the playlist only under the stop policy");
    pl::ResultBlock block{.scheduled_index = first};
    block.play.sent_frames = pl::chunk_frames;
    block.play.first_frame_index = first;
    checks.expect(!block.start_late(), "an item that started at its stream index is on time");
    block.play.first_frame_index = first + pl::chunk_frames;
    checks.expect(block.start_late(), "one that started a packet later is late");
    block.play.sent_frames = 0U;
    block.play.first_frame_index = 0U;
    checks.expect(!block.start_late(), "a scheduled item that sent nothing (STOP while it waited, a fault before it armed) never started late");
    block.play.sent_frames = pl::chunk_frames;
    block.scheduled_index.reset();
    checks.expect(!block.start_late(), "an item without a schedule is never late");
    checks.expect(pl::state_name(State::paused_ring_full) == "paused_ring_full" && pl::policy_name(Policy::continue_on_error) == "continue" && pl::outcome_name(Outcome::aborted) == "aborted" &&
                      pl::route_name(pl::Route::usb) == "usb" && pl::is_active(State::gap) && !pl::is_active(State::done) && !pl::is_active(State::idle),
                  "the PLAYLIST? words");
}

// A JSON string, or null when empty: the checker's texts are names and
// labels, none of which needs escaping.
[[nodiscard]] std::string json_text(std::string_view text)
{
    return text.empty() ? std::string{"null"} : "\"" + std::string{text} + "\"";
}

// An item as the checker reports it.
[[nodiscard]] std::string item_json(const pl::Item& item)
{
    std::string cuts;

    for (const auto& entry : item.cuts.entries())
    {
        cuts += (cuts.empty() ? "" : ",") + std::to_string(entry.offset.value) + ":" + std::to_string(entry.length) + ":" + (entry.kind == cut::CutKind::drop ? "drop" : "zero");
    }

    return R"({"line":)" + std::to_string(item.line.value) + R"(,"file":)" + json_text(item.file.view()) + R"(,"gap_frames":)" + std::to_string(item.gap.value) + R"(,"route":)" +
           json_text(pl::route_name(item.route)) + R"(,"label":)" + json_text(item.label.view()) + R"(,"scn":)" + json_text(item.scenario.view()) + R"(,"seed":)" +
           (item.seed.has_value() ? std::to_string(*item.seed) : std::string{"null"}) + R"(,"cut":)" + json_text(cuts) + R"(,"att_tenths":)" + std::to_string(item.attenuation.value()) +
           R"(,"dur_frames":)" + (item.duration.has_value() ? std::to_string(item.duration->value) : std::string{"null"}) + "}";
}

// --check <file>: the parser's verdict on a playlist file, fed in 100-byte
// pieces as the player feeds its read buffer.
int check_file(const char* name)
{
    constexpr std::size_t piece = 100U;
    std::ifstream input{name, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    if (!input.good() && !input.eof())
    {
        std::fputs("cannot read the playlist file\n", stderr);
        return 2;
    }

    const auto result = parse_file(bytes, piece);
    std::string items;

    for (const auto& item : table().items())
    {
        items += (items.empty() ? "" : ",") + item_json(item);
    }

    const std::string code = result.status.is_ok() ? std::string{"OK"} : std::string{result.status.message};
    std::fputs((R"({"code":")" + code + R"(","line":)" + std::to_string(result.line.value) + R"(,"items":[)" + items + "]}\n").c_str(), stdout);
    return 0;
}

// A decimal uint32 argument, or none.
[[nodiscard]] std::optional<std::uint32_t> uint32_argument(std::string_view text)
{
    std::uint32_t value{};
    const char* const first = std::to_address(text.cbegin());
    const char* const last = std::to_address(text.cend());
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last ? std::optional{value} : std::nullopt;
}

// --next-start <first> <sent> <gap_frames>...: next_start_index() of each
// triple, as one JSON array; 2 for arguments that are not whole triples of
// decimal uint32s.
int print_next_starts(std::span<char* const> numbers)
{
    constexpr std::size_t triple = 3U;

    if (numbers.empty() || numbers.size() % triple != 0U)
    {
        std::fputs("--next-start takes <first> <sent> <gap_frames> triples\n", stderr);
        return 2;
    }

    std::string starts;

    for (std::size_t index = 0U; index < numbers.size(); index += triple)
    {
        const auto first = uint32_argument(numbers[index]);
        const auto sent = uint32_argument(numbers[index + 1U]);
        const auto gap = uint32_argument(numbers[index + 2U]);

        if (!first.has_value() || !sent.has_value() || !gap.has_value())
        {
            std::fputs("--next-start takes decimal uint32s\n", stderr);
            return 2;
        }

        starts += (starts.empty() ? "" : ",") + std::to_string(pl::next_start_index(*first, *sent, pl::GapFrames{.value = *gap}));
    }

    std::fputs(("[" + starts + "]\n").c_str(), stdout);
    return 0;
}

// --gain-table: the firmware's attenuation gains (common/attenuation_gains.hpp)
// as one JSON array, entry n for n tenths of a dB.
int print_gain_table()
{
    std::string entries;

    for (const std::uint32_t gain : att::gains_q31)
    {
        entries += (entries.empty() ? "" : ",") + std::to_string(gain);
    }

    std::fputs(("[" + entries + "]\n").c_str(), stdout);
    return 0;
}

// A decimal int32 argument, or none.
[[nodiscard]] std::optional<std::int32_t> int32_argument(std::string_view text)
{
    std::int32_t value{};
    const char* const first = std::to_address(text.cbegin());
    const char* const last = std::to_address(text.cend());
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last ? std::optional{value} : std::nullopt;
}

// --attenuate <tenths> <sample>...: each PCM24 sample as att::apply() leaves
// it at that attenuation, as one JSON array; 2 for an attenuation past
// 400 tenths or an argument that is not a decimal.
int print_attenuated(std::span<char* const> numbers)
{
    const auto tenths = numbers.empty() ? std::nullopt : uint32_argument(numbers.front());

    if (!tenths.has_value() || *tenths > att::max_tenths)
    {
        std::fputs("--attenuate takes <tenths 0..400> <sample>...\n", stderr);
        return 2;
    }

    const auto gain = att::Gain::of(att::Tenths::of(*tenths).value());
    std::string values;

    for (const char* const number : numbers.subspan(1U))
    {
        const auto sample = int32_argument(number);

        if (!sample.has_value() || *sample < waveform_generator::audio::pcm24_min || *sample > waveform_generator::audio::pcm24_max)
        {
            std::fputs("--attenuate takes PCM24 samples\n", stderr);
            return 2;
        }

        values += (values.empty() ? "" : ",") + std::to_string(att::apply(gain, *sample));
    }

    std::fputs(("[" + values + "]\n").c_str(), stdout);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    constexpr std::string_view check_flag{"--check"};
    constexpr std::string_view next_start_flag{"--next-start"};
    constexpr std::string_view gain_table_flag{"--gain-table"};
    constexpr std::string_view attenuate_flag{"--attenuate"};
    constexpr std::size_t check_args = 3U;

    if (args.size() == check_args && std::string_view{args[1]} == check_flag)
    {
        return check_file(args[2]);
    }

    if (args.size() >= 2U && std::string_view{args[1]} == next_start_flag)
    {
        return print_next_starts(args.subspan(2U));
    }

    if (args.size() == 2U && std::string_view{args[1]} == gain_table_flag)
    {
        return print_gain_table();
    }

    if (args.size() >= 2U && std::string_view{args[1]} == attenuate_flag)
    {
        return print_attenuated(args.subspan(2U));
    }

    Checks checks;
    test_battery(checks);
    test_gaps(checks);
    test_refusals_name_their_line(checks);
    test_comment_characters(checks);
    test_limits(checks);
    test_attenuation(checks);
    test_durations(checks);
    test_labels(checks);
    test_names(checks);
    test_result_ring(checks);
    test_timing(checks);

    if (checks.failures() != 0)
    {
        std::fputs(("playlist tests: " + std::to_string(checks.failures()) + " failures\n").c_str(), stdout);
        return 1;
    }

    std::puts("playlist tests: PASS");
    return 0;
}
