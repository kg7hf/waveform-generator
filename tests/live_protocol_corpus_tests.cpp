// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The request corpus (live_protocol_corpus.hpp) against its record file,
// tests/live_protocol_corpus.txt: one record per seed, with the seed's
// outcome and the count, refusals and FNV-1a digest of its edits' outcomes,
// then the totals. The parser that returned bool wrote the file, so a parser
// whose records all match gives every line of the corpus the reply that one
// gave. A mismatch names its seed; --dump prints every line and its outcome,
// so the dumps of two builds compare line by line. --write <file> writes the
// file from the parser at hand, for a change that is meant to change a reply.

#include "live_protocol_corpus.hpp"

#include "common/status.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

namespace corpus = live_protocol_corpus;

constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;
constexpr std::string_view totals_prefix{"# totals "};
constexpr std::size_t shown_mismatches = 10U;

void say(std::FILE* stream, const std::string& text)
{
    static_cast<void>(std::fputs(text.c_str(), stream));
}

void add_to_digest(std::uint64_t& digest, std::string_view text)
{
    for (const char character : text)
    {
        digest ^= static_cast<unsigned char>(character);
        digest *= fnv_prime;
    }
}

[[nodiscard]] std::string hexadecimal(std::uint64_t value)
{
    constexpr std::size_t digits = 16U;
    constexpr int radix = 16;
    std::array<char, digits> buffer{};
    const std::to_chars_result written = std::to_chars(buffer.data(), std::next(buffer.data(), digits), value, radix);
    const std::string text{buffer.data(), written.ptr};
    return std::string(digits - text.size(), '0') + text;
}

// The lines the corpus holds and what the parser made of them.
struct Totals
{
    std::size_t lines{};
    std::size_t refused{};
    std::array<std::size_t, corpus::kind_names.size()> accepted{};

    void count(const corpus::Outcome& outcome)
    {
        ++lines;

        if (outcome.accepted)
        {
            ++accepted.at(std::to_underlying(outcome.request.kind));
        }
        else
        {
            ++refused;
        }
    }

    [[nodiscard]] std::string record() const
    {
        std::string text = std::string{totals_prefix} + "lines " + corpus::number(lines) + " refused " + corpus::number(refused) + " accepted";

        for (std::size_t kind = 0U; kind < accepted.size(); ++kind)
        {
            text += ' ';
            text += corpus::kind_names.at(kind);
            text += ' ';
            text += corpus::number(accepted.at(kind));
        }

        return text;
    }
};

// A line of a dump or a digest: the line, quoted, and its outcome.
[[nodiscard]] std::string described(std::string_view text, const corpus::Outcome& outcome)
{
    return '"' + corpus::escaped(text) + "\" -> " + corpus::render(outcome);
}

// A seed's record: "<last before> <seed and outcome> | edits <count>
// refused <count> digest <hex>"; each of its lines goes to `dump` too.
[[nodiscard]] std::string record_of(const corpus::Line& seed, Totals& totals, std::FILE* dump)
{
    const std::string last = corpus::number(seed.last_before);
    const corpus::Outcome outcome = corpus::outcome_of(seed);
    totals.count(outcome);
    std::string record = last + ' ' + described(seed.text, outcome);

    if (dump != nullptr)
    {
        say(dump, record + '\n');
    }

    std::uint64_t digest = fnv_offset_basis;
    std::size_t edits = 0U;
    std::size_t refused = 0U;

    for (const std::string& text : corpus::edits_of(seed.text))
    {
        const corpus::Outcome edited = corpus::outcome_of({.last_before = seed.last_before, .text = text});
        const std::string line = described(text, edited);
        add_to_digest(digest, line);
        add_to_digest(digest, "\n");
        totals.count(edited);
        ++edits;
        refused += edited.accepted ? 0U : 1U;

        if (dump != nullptr)
        {
            std::string dumped = "  ";
            dumped += last;
            dumped += ' ';
            dumped += line;
            dumped += '\n';
            say(dump, dumped);
        }
    }

    return record + " | edits " + corpus::number(edits) + " refused " + corpus::number(refused) + " digest " + hexadecimal(digest);
}

// Every seed's record, then the totals' record.
[[nodiscard]] std::vector<std::string> records(Totals& totals, std::FILE* dump)
{
    std::vector<std::string> lines;

    for (const corpus::Line& seed : corpus::seeds())
    {
        lines.push_back(record_of(seed, totals, dump));
    }

    lines.push_back(totals.record());
    return lines;
}

constexpr std::string_view file_header = "# WFG-LIVE/1 request corpus: tests/live_protocol_corpus.hpp makes the lines, tests/live_protocol_corpus_tests.cpp\n"
                                         "# compares them. <last sequence before> \"<seed>\" -> <outcome> | edits <n> refused <n> digest <FNV-1a of the\n"
                                         "# edits' \"<line>\" -> <outcome> lines>. An outcome is seq=<the reply's seq> last=<last sequence after>, then\n"
                                         "# error=<the reply's code>, or ok kind=<kind> and the request's fields that are not their default.\n";

[[nodiscard]] int write(const char* path)
{
    std::ofstream file{path, std::ios::binary};
    file << file_header;
    Totals totals;

    for (const std::string& line : records(totals, nullptr))
    {
        file << line << '\n';
    }

    return file.good() ? 0 : 1;
}

// The records of the file at `path`: its lines that are no comment, and its
// totals line; unavailable when the file cannot be read, invalid_argument
// when a line ends in CR.
[[nodiscard]] m110::Result<std::vector<std::string>> read_records(const char* path)
{
    std::ifstream file{path, std::ios::binary};

    if (!file.is_open())
    {
        return m110::Status{.code = m110::StatusCode::unavailable, .message = "the record file cannot be read"};
    }

    std::vector<std::string> lines;
    std::string line;

    while (std::getline(file, line))
    {
        if (line.ends_with('\r'))
        {
            return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "a line of the record file ends in CR"};
        }

        if (!line.starts_with('#') || line.starts_with(totals_prefix))
        {
            lines.push_back(line);
        }
    }

    return lines;
}

[[nodiscard]] int check(const char* path)
{
    const m110::Result<std::vector<std::string>> read = read_records(path);

    if (!read.has_value())
    {
        say(stderr, std::string{path} + ": " + read.status().message + '\n');
        return 1;
    }

    const std::vector<std::string>& expected = read.value();
    Totals totals;
    const std::vector<std::string> actual = records(totals, nullptr);
    const std::string none{"(none)"};
    std::size_t mismatches = 0U;

    for (std::size_t index = 0U; index < std::max(expected.size(), actual.size()); ++index)
    {
        const std::string& want = index < expected.size() ? expected.at(index) : none;
        const std::string& got = index < actual.size() ? actual.at(index) : none;

        if (want != got && ++mismatches <= shown_mismatches)
        {
            std::string report = "record " + corpus::number(index + 1U);
            report += "\n  expected ";
            report += want;
            report += "\n  actual   ";
            report += got;
            report += '\n';
            say(stderr, report);
        }
    }

    if (mismatches != 0U)
    {
        say(stderr, corpus::number(mismatches) + " of " + corpus::number(actual.size()) + " records differ (--dump prints every line, to compare two builds)\n");
        return 1;
    }

    if (std::ranges::find(totals.accepted, std::size_t{0U}) != totals.accepted.end())
    {
        say(stderr, "the parser takes no line of some request kind: " + actual.back() + '\n');
        return 1;
    }

    say(stdout, "live protocol corpus: " + corpus::number(actual.size() - 1U) + " seed records match; " + actual.back() + '\n');
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    constexpr std::size_t path_only = 2U;
    constexpr std::size_t option_and_path = 3U;

    if (args.size() == option_and_path && std::string_view{args[1]} == "--write")
    {
        return write(args[2]);
    }

    if (args.size() == path_only && std::string_view{args[1]} == "--dump")
    {
        Totals totals;
        static_cast<void>(records(totals, stdout));
        return 0;
    }

    if (args.size() == path_only)
    {
        return check(args[1]);
    }

    say(stderr, "usage: wfg_live_protocol_corpus_tests <corpus.txt> | --write <corpus.txt> | --dump\n");
    return 2;
}
