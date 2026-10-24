// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// pathsim_chain: the channel-sim chain driver on the host (guide: docs/modules/pathsim.md).
//
//   pathsim_chain [pathsim options] in.wav out.wav
//
// An 8 kHz PCM16 input runs pathsim's job (byte-identical to the lane's pathsim.exe,
// gate 1); a 48 kHz mono input runs the lane's whole chain, run_pathsim.py's job
// (gate 2). pathsim is by Moe Wheatley AE4JY, reworked by Vojtech Bubnik OK1IAK
// (GNU GPL v3); the chain driver is waveform-generator's.

#include "cli/options.hpp"
#include "cli/wav_file.hpp"
#include "common/host_critical_error.hpp"
#include "core/channel_spec.hpp"
#include "core/lane_chain.hpp"
#include "core/pathsim_driver.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace
{
namespace cs = channel_sim;
namespace cli = channel_sim::cli;
using m110::Status;
using m110::StatusCode;

// Exit status (docs/modules/pathsim.md): 0 done, 1 a file or run failed or a
// critical error (common/host_critical_error.hpp), 2 a usage or channel error.
constexpr int exit_ok = 0;
constexpr int exit_failure = 1;
constexpr int exit_usage = 2;
constexpr int summary_decimals = 2;
constexpr double decibels_per_decade = 20.0;
constexpr std::uint16_t lane_channels = 1U;
constexpr std::size_t line_capacity = 512U;
constexpr std::size_t integer_digits = 24U; // a 64-bit integer in decimal, with room
constexpr std::size_t fixed_digits = 48U;
#if defined(_WIN32)
constexpr unsigned file_index_high_shift = 32U; // nFileIndexHigh holds the index's upper 32 bits
#endif

// A line of text in a fixed buffer, written to stderr.
class Line
{
public:
    Line& text(std::string_view part) noexcept
    {
        for (const char letter : part)
        {
            put(letter);
        }

        return *this;
    }

    Line& count(std::uint64_t value) noexcept
    {
        std::array<char, integer_digits> digits{};
        const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        return error == std::errc{} ? text(std::string_view{digits.data(), end}) : text("?");
    }

    Line& fixed(double value) noexcept
    {
        std::array<char, fixed_digits> digits{};
        const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value, std::chars_format::fixed, summary_decimals);
        return error == std::errc{} ? text(std::string_view{digits.data(), end}) : text("?");
    }

    void emit() noexcept
    {
        put('\n');
        static_cast<void>(std::fwrite(buffer_.data(), 1U, used_, stderr));
        used_ = 0U;
    }

private:
    void put(char letter) noexcept
    {
        if (used_ < buffer_.size())
        {
            std::span<char>{buffer_}[used_] = letter;
            ++used_;
        }
    }

    std::array<char, line_capacity> buffer_{};
    std::size_t used_{};
};

void print_help() noexcept
{
    Line line;
    line.text("pathsim_chain - the HF lane's pathsim chain (PathSim by Moe Wheatley AE4JY, reworked by Vojtech Bubnik OK1IAK)").emit();
    line.text("usage: pathsim_chain [options] input_file output_file").emit();
    line.text("  8 kHz PCM16 input: pathsim alone (as pathsim.exe); 48 kHz mono input: 48k -> 8k (-12 dB) -> pathsim -> 48k").emit();
    line.text("  -i, --input_file FILE  -o, --output_file FILE   --help").emit();
    line.text("  --seed N (default 1)  --snr DB  --spread HZ  --offset HZ").emit();
    line.text("  --delay2 MS  --spread2 HZ  --offset2 HZ  --delay3 MS  --spread3 HZ  --offset3 HZ  --truth-out FILE").emit();
    line.text("  --chain auto|8k|48k   --resample-gain-db DB (48 kHz chain only, default -12)").emit();
    line.text("propagation conditions:").emit();

    for (std::size_t index = 0U; index < cs::preset_count(); ++index)
    {
        line.text("  --").text(cs::preset_name(index)).text("  ").text(cs::preset_title(index)).emit();
    }
}

[[nodiscard]] int fail(std::string_view what, Status status, int code) noexcept
{
    Line{}.text("pathsim_chain: ").text(what).text(": ").text(status.message).emit();
    return code;
}

[[nodiscard]] m110::Result<cli::ChainMode> resolve_mode(const cli::CliOptions& options, const cli::WavFormat& format) noexcept
{
    const cli::ChainMode requested = options.mode.value_or(cli::ChainMode::automatic);
    const bool narrow = format.rate == cs::pathsim_sample_rate && format.encoding == cli::WavEncoding::pcm16;
    const bool wide = format.rate == cs::lane_sample_rate && format.channels == lane_channels;
    const bool automatic = requested == cli::ChainMode::automatic;

    if ((automatic || requested == cli::ChainMode::pathsim_8k) && narrow)
    {
        return cli::ChainMode::pathsim_8k;
    }

    if ((automatic || requested == cli::ChainMode::lane_48k) && wide)
    {
        return cli::ChainMode::lane_48k;
    }

    return Status{StatusCode::invalid_argument, "input must be 8000 Hz PCM16 (pathsim) or 48000 Hz mono (lane chain)"};
}

// --resample-gain-db is the 48 kHz chain's down-conversion gain; pathsim's own job
// (8 kHz in) has no resampler, so the option would change nothing.
[[nodiscard]] Status check_mode_options(const cli::CliOptions& options, cli::ChainMode mode) noexcept
{
    if (mode == cli::ChainMode::pathsim_8k && options.resample_gain_db.has_value())
    {
        return {StatusCode::invalid_argument, "--resample-gain-db applies to the 48 kHz chain only; this input runs pathsim at 8 kHz"};
    }

    return Status::success();
}

// A file's identity: its volume and its index there (Windows), or its device and
// inode. Two names name one file exactly when both name existing files of one
// identity, whatever the spelling: case, "..", a link or a short name.
struct FileIdentity
{
    std::uint64_t volume{};
    std::uint64_t index{};

    constexpr bool operator==(const FileIdentity&) const noexcept = default;
};

// The identity of the file `path` names, as the C runtime and pathsim open it (the
// ANSI code page on Windows, unconverted); none when it names no file this
// program can query.
[[nodiscard]] std::optional<FileIdentity> file_identity(const char* path) noexcept
{
    if (path == nullptr)
    {
        return std::nullopt;
    }

#if defined(_WIN32)
    // No access right is asked for, so a file another program holds open still
    // answers; the backup flag lets a directory answer as well.
    HANDLE const file = CreateFileA(path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    const bool known = GetFileInformationByHandle(file, &information) != 0;
    static_cast<void>(CloseHandle(file));

    if (!known)
    {
        return std::nullopt;
    }

    return FileIdentity{.volume = information.dwVolumeSerialNumber, .index = (std::uint64_t{information.nFileIndexHigh} << file_index_high_shift) | information.nFileIndexLow};
#else
    struct stat status{};

    if (::stat(path, &status) != 0)
    {
        return std::nullopt;
    }

    return FileIdentity{.volume = status.st_dev, .index = status.st_ino};
#endif
}

// Whether both names name one existing file.
[[nodiscard]] bool same_existing_file(const char* first, const char* second) noexcept
{
    const std::optional<FileIdentity> identity = file_identity(first);
    return identity.has_value() && identity == file_identity(second);
}

// The sink truncates the output when it opens it, and pathsim writes the truth file
// as it runs, so the files must be three. Checked before any of them is opened, by
// identity, which is exact for files that exist: the input exists (or opening it
// fails), so a name that names no file is not the input. An output and a truth
// file that are both still unwritten are checked by claim_truth_file().
[[nodiscard]] Status check_distinct_files(const cli::CliOptions& options) noexcept
{
    if (same_existing_file(options.input, options.output))
    {
        return {StatusCode::invalid_argument, "the output file is the input file"};
    }

    if (same_existing_file(options.input, options.truth))
    {
        return {StatusCode::invalid_argument, "the truth file is the input file"};
    }

    if (same_existing_file(options.output, options.truth))
    {
        return {StatusCode::invalid_argument, "the truth file is the output file"};
    }

    return Status::success();
}

// Two names of files not yet written have no identity to compare, and comparing
// their spellings misses case, short names and links. So a truth file that does
// not exist yet is created, empty, just before the sink opens the output: if the
// output's name then names it too, the two are one file, it is removed again and
// the run is refused, so nothing is written. Otherwise pathsim writes it from the
// start (a run that leaves it empty fails check_truth()). A truth file that
// cannot be created is reported here, before the output is written.
[[nodiscard]] Status claim_truth_file(const char* truth, const char* output) noexcept
{
    if (truth == nullptr || file_identity(truth).has_value())
    {
        return Status::success();
    }

    {
        const std::ofstream created{truth, std::ios::binary | std::ios::app};

        if (!created.is_open())
        {
            return {StatusCode::io_error, "cannot create the truth file"};
        }
    }

    if (same_existing_file(output, truth))
    {
        static_cast<void>(std::remove(truth));
        return {StatusCode::invalid_argument, "the truth file is the output file"};
    }

    return Status::success();
}

// pathsim gives no error when it cannot open the truth file; the file must exist now.
[[nodiscard]] Status check_truth(const char* path) noexcept
{
    if (path == nullptr)
    {
        return Status::success();
    }

    std::ifstream file{path, std::ios::binary};

    if (!file.is_open() || file.peek() == std::ifstream::traits_type::eof())
    {
        return {StatusCode::io_error, "pathsim did not write the truth file"};
    }

    return Status::success();
}

void report_pathsim(Line& line, const cs::PathsimRunReport& report, cs::Seed seed) noexcept
{
    line.text("pathsim_chain: pathsim ").count(report.samples).text(" samples in ").count(report.blocks).text(" blocks, seed ").count(seed.value);
    line.text(", active ").count(report.active_samples);

    if (report.seed_rms > 0.0)
    {
        line.text(", level seed ").fixed(decibels_per_decade * std::log10(report.seed_rms)).text(" dBFS");
    }
}

[[nodiscard]] int run_pathsim(const cs::PathsimJob& job, cli::WavFileSource& source, cli::WavFileSink& sink) noexcept
{
    static cs::PathsimDriver driver;
    const m110::Result<cs::PathsimRunReport> report = driver.run(job, source, sink);

    if (!report)
    {
        return fail("pathsim", report.status(), exit_failure);
    }

    const Status finished = sink.finish();

    if (!finished.is_ok())
    {
        return fail("output", finished, exit_failure);
    }

    Line line;
    report_pathsim(line, report.value(), job.seed);
    line.emit();
    return exit_ok;
}

[[nodiscard]] int run_lane_chain(const cs::PathsimJob& job, const cli::CliOptions& options, cli::WavFileSource& source, cli::WavFileSink& sink) noexcept
{
    static cs::LaneChain chain;
    cli::WavUnitSource unit{source};
    cs::LaneChainConfig config{};

    if (options.resample_gain_db.has_value())
    {
        config.downsample_gain = cs::Decibels{*options.resample_gain_db};
    }

    const m110::Result<cs::LaneChainReport> report = chain.run(job, config, unit, sink);

    if (!report)
    {
        return fail("lane chain", report.status(), exit_failure);
    }

    const Status finished = sink.finish();

    if (!finished.is_ok())
    {
        return fail("output", finished, exit_failure);
    }

    Line line;
    report_pathsim(line, report.value().pathsim, job.seed);
    line.emit();
    line.text("pathsim_chain: 48 kHz ").count(report.value().input_samples).text(" samples in, ").count(report.value().output_samples);
    line.text(" out; kernel misses ").count(report.value().downsample_kernel_misses).text(" down, ").count(report.value().upsample_kernel_misses).text(" up");
    line.emit();
    return exit_ok;
}

[[nodiscard]] int run(const cli::CliOptions& options) noexcept
{
    const m110::Result<cs::PathsimJob> job = build_job(options);

    if (!job)
    {
        return fail("channel", job.status(), exit_usage);
    }

    const Status distinct = check_distinct_files(options);

    if (!distinct.is_ok())
    {
        return fail("files", distinct, exit_usage);
    }

    static cli::WavFileSource source;
    const Status opened = source.open(options.input);

    if (!opened.is_ok())
    {
        return fail(options.input, opened, exit_failure);
    }

    const m110::Result<cli::ChainMode> mode = resolve_mode(options, source.format());

    if (!mode)
    {
        return fail(options.input, mode.status(), exit_usage);
    }

    const Status mode_options = check_mode_options(options, mode.value());

    if (!mode_options.is_ok())
    {
        return fail("options", mode_options, exit_usage);
    }

    const Status claimed = claim_truth_file(options.truth, options.output);

    if (!claimed.is_ok())
    {
        return fail("files", claimed, claimed.code == StatusCode::invalid_argument ? exit_usage : exit_failure);
    }

    const bool narrow = mode.value() == cli::ChainMode::pathsim_8k;
    static cli::WavFileSink sink;
    const Status created = narrow ? sink.open(options.output, source.format().rate, source.format().channels) : sink.open(options.output, cs::lane_sample_rate, lane_channels);

    if (!created.is_ok())
    {
        return fail(options.output, created, exit_failure);
    }

    const int code = narrow ? run_pathsim(job.value(), source, sink) : run_lane_chain(job.value(), options, source, sink);

    if (code != exit_ok)
    {
        return code;
    }

    const Status truth = check_truth(options.truth);
    return truth.is_ok() ? exit_ok : fail(options.truth, truth, exit_failure);
}
} // namespace

int main(int argc, char** argv)
{
    m110::install_host_critical_error_handler("pathsim_chain", exit_failure);
    const std::span<const char* const> arguments{argv, static_cast<std::size_t>(argc)};
    cli::CliParser parser;
    const Status parsed = parser.parse(arguments.subspan(1U));

    if (!parsed.is_ok())
    {
        Line{}.text("pathsim_chain: ").text(parsed.message).text(": ").text(parser.failed_argument()).emit();
        Line{}.text("pathsim_chain --help lists the options").emit();
        return exit_usage;
    }

    if (parser.options().help)
    {
        print_help();
        return exit_ok;
    }

    if (parser.options().input == nullptr || parser.options().output == nullptr)
    {
        return fail("arguments", Status{StatusCode::invalid_argument, "input / output not specified"}, exit_usage);
    }

    return run(parser.options());
}
