// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The Watterson channel: validation, normalisation, the path gains, the chunk
// loop, the report and digest, and the scenario parser of the stage.
// Guide: docs/modules/signal-lab-watterson.md; design docs/design/watterson-stage.md.
//
// Float order (part of the stage's bit-exact definition, design 8):
//   h(n)  = h[m] + mu (h[m + 1] - h[m]),  mu = float(double(j) * (1.0 / M))     Rayleigh paths
//   g(n)  = h(n) * float(r(n)) as a complex float product                       with a shift
//   term  = (Re g * xI(n - L)) - (Im g * xQ(n - L))
//   y(n)  = term_0 + term_1 + ... in path order, each sum rounded in float
// r(n) is double: set exactly from the integer phase at n mod 1024 = 0, then
// multiplied by the double step once per frame.

#include "signal_lab/watterson.hpp"

#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "watterson_scenario.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace signal_lab
{
namespace
{

constexpr const char* paths_message = "watterson.paths: required, 1 to 4 path objects";
constexpr const char* finite_message = "watterson.paths[]: value is not finite";
constexpr const char* delay_message = "watterson.paths[].delay_ms: number in [0, 20]";
constexpr const char* power_message = "watterson.paths[].power_db: number in [-60, 60]";
constexpr const char* fading_message = R"(watterson.paths[].fading: "rayleigh" or "fixed")";
constexpr const char* spread_message = "watterson.paths[].spread_hz: required for a rayleigh path, number in [0.1, 10]";
constexpr const char* spread_fixed_message = "watterson.paths[].spread_hz: not allowed on a fixed path";
constexpr const char* shift_message = "watterson.paths[].shift_hz: number in [-75, 75]";
constexpr const char* phase_message = "watterson.paths[].phase_deg: number in [-360, 360], fixed paths only";
constexpr const char* stage_member_message = "watterson: unknown member";
constexpr const char* path_member_message = "watterson.paths[]: unknown member";
constexpr const char* stage_duplicate_message = "watterson: duplicate member";
constexpr const char* path_duplicate_message = "watterson.paths[]: duplicate member";
constexpr const char* contiguity_message = "watterson: first_input_frame does not follow the previous block";
constexpr const char* unconfigured_message = "watterson: process() before a successful configure()";
constexpr const char* single_message = "watterson: at most one stage per scenario";
constexpr const char* first_message = "watterson: must be the first stage (noise and interference follow the channel, E.4.1)";
constexpr const char* channel_message = "watterson: needs a WattersonChannel (StageResources.watterson)";

// FNV-1a 64 (Fowler, Noll, Vo): the offset basis and the prime.
constexpr std::uint64_t fnv_offset_basis = 0xCBF29CE484222325ULL;
constexpr std::uint64_t fnv_prime = 0x100000001B3ULL;
constexpr std::uint32_t bits_per_byte = 8U;
constexpr std::uint64_t byte_mask = 0xFFU;
constexpr std::uint32_t bytes_per_word = 8U;

class Fnv1a
{
public:
    void add(std::uint64_t word) noexcept
    {
        for (std::uint32_t byte = 0U; byte < bytes_per_word; ++byte)
        {
            mHash ^= (word >> (bits_per_byte * byte)) & byte_mask;
            mHash *= fnv_prime;
        }
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return mHash; }

private:
    std::uint64_t mHash{fnv_offset_basis};
};

[[nodiscard]] m110::Status invalid(const char* message) noexcept
{
    return m110::Status{m110::StatusCode::invalid_configuration, message};
}

[[nodiscard]] bool within(double value, double low, double high) noexcept
{
    return std::isfinite(value) && value >= low && value <= high;
}

[[nodiscard]] bool finite_path(const WattersonPath& path) noexcept
{
    return std::isfinite(path.delay.in(units::ms)) && std::isfinite(path.level.in(units::dB)) && std::isfinite(path.spread.in(units::Hz)) && std::isfinite(path.shift.in(units::Hz)) &&
           std::isfinite(path.phase.in(units::degrees));
}

[[nodiscard]] m110::Status validate_kind(const WattersonPath& path) noexcept
{
    const double spread = path.spread.in(units::Hz);

    switch (path.fading)
    {
    case PathFading::rayleigh:
        if (!within(spread, watterson_min_spread.in(units::Hz), watterson_max_spread.in(units::Hz)))
        {
            return invalid(spread_message);
        }

        return path.phase == units::Angle{} ? m110::Status::success() : invalid(phase_message);

    case PathFading::fixed:
        if (spread != 0.0)
        {
            return invalid(spread_fixed_message);
        }

        return within(path.phase.in(units::degrees), -watterson_max_phase.in(units::degrees), watterson_max_phase.in(units::degrees)) ? m110::Status::success() : invalid(phase_message);
    }

    return invalid(fading_message);
}

[[nodiscard]] m110::Status validate_path(const WattersonPath& path) noexcept
{
    if (!finite_path(path))
    {
        return invalid(finite_message);
    }

    if (!within(path.delay.in(units::ms), 0.0, watterson_max_delay.in(units::ms)))
    {
        return invalid(delay_message);
    }

    if (!within(path.level.in(units::dB), -watterson_max_relative_level.in(units::dB), watterson_max_relative_level.in(units::dB)))
    {
        return invalid(power_message);
    }

    if (!within(path.shift.in(units::Hz), -watterson_max_shift.in(units::Hz), watterson_max_shift.in(units::Hz)))
    {
        return invalid(shift_message);
    }

    return validate_kind(path);
}

// ---- scenario members (design 9.3) ----

enum class PathField : std::uint8_t
{
    delay,
    power,
    spread,
    shift,
    phase,
};

struct NumberMember
{
    std::string_view name;
    PathField field;
    double low;
    double high;
    const char* message;
};

constexpr std::array<NumberMember, 5U> path_numbers{{
    {"delay_ms", PathField::delay, 0.0, watterson_max_delay.in(units::ms), delay_message},
    {"power_db", PathField::power, -watterson_max_relative_level.in(units::dB), watterson_max_relative_level.in(units::dB), power_message},
    {"spread_hz", PathField::spread, watterson_min_spread.in(units::Hz), watterson_max_spread.in(units::Hz), spread_message},
    {"shift_hz", PathField::shift, -watterson_max_shift.in(units::Hz), watterson_max_shift.in(units::Hz), shift_message},
    {"phase_deg", PathField::phase, -watterson_max_phase.in(units::degrees), watterson_max_phase.in(units::degrees), phase_message},
}};

constexpr std::string_view fading_member = "fading";
constexpr std::string_view type_member = "type";
constexpr std::string_view paths_member = "paths";
constexpr std::string_view stream_id_member = "stream_id";

// What a path object named, beyond its values: spread and phase decide the kind checks,
// and seen holds one bit per member (path_numbers in order, then fading), so a member
// named twice is refused instead of one reader taking the first and another the last.
struct PathPresence
{
    std::uint32_t seen{};
    bool spread{};
    bool phase{};
};

constexpr std::uint32_t fading_bit = 1U << path_numbers.size();
static_assert(path_numbers.size() < std::numeric_limits<decltype(PathPresence::seen)>::digits, "one bit per member in PathPresence::seen");

// The seen bit of a numeric path member: its position in path_numbers (0 if unknown).
[[nodiscard]] std::uint32_t number_bit(std::string_view key) noexcept
{
    std::uint32_t bit = 1U;

    for (const NumberMember& rule : path_numbers)
    {
        if (rule.name == key)
        {
            return bit;
        }

        bit <<= 1U;
    }

    return 0U;
}

// Marks a member as seen; false when it was already.
[[nodiscard]] bool first_sighting(PathPresence& presence, std::uint32_t bit) noexcept
{
    const bool first = (presence.seen & bit) == 0U;
    presence.seen |= bit;
    return first;
}

[[nodiscard]] std::string_view key_of(const json::Value& member) noexcept
{
    return std::string_view{member.key, member.key_length};
}

void assign(WattersonPath& path, PathField field, double value) noexcept
{
    switch (field)
    {
    case PathField::delay:
        path.delay = units::Duration{value, units::ms};
        break;
    case PathField::power:
        path.level = units::Gain{value, units::dB};
        break;
    case PathField::spread:
        path.spread = units::FrequencyDelta{value, units::Hz};
        break;
    case PathField::shift:
        path.shift = units::FrequencyDelta{value, units::Hz};
        break;
    case PathField::phase:
        path.phase = units::Angle{value, units::degrees};
        break;
    }
}

[[nodiscard]] m110::Status read_fading(const json::Value& member, WattersonPath& path) noexcept
{
    if (member.equals("rayleigh"))
    {
        path.fading = PathFading::rayleigh;
        return m110::Status::success();
    }

    if (member.equals("fixed"))
    {
        path.fading = PathFading::fixed;
        return m110::Status::success();
    }

    return invalid(fading_message);
}

[[nodiscard]] m110::Status read_path_member(const json::Value& member, WattersonPath& path, PathPresence& presence) noexcept
{
    const std::string_view key = key_of(member);

    if (key == fading_member)
    {
        return first_sighting(presence, fading_bit) ? read_fading(member, path) : invalid(path_duplicate_message);
    }

    const auto rule = std::ranges::find(path_numbers, key, &NumberMember::name);

    if (rule == path_numbers.end())
    {
        return invalid(path_member_message);
    }

    if (!first_sighting(presence, number_bit(key)))
    {
        return invalid(path_duplicate_message);
    }

    if (member.type != json::Type::number || !within(member.number, rule->low, rule->high))
    {
        return invalid(rule->message);
    }

    assign(path, rule->field, member.number);
    presence.spread = presence.spread || rule->field == PathField::spread;
    presence.phase = presence.phase || rule->field == PathField::phase;
    return m110::Status::success();
}

[[nodiscard]] m110::Status check_presence(const WattersonPath& path, PathPresence presence) noexcept
{
    if (path.fading == PathFading::rayleigh)
    {
        if (!presence.spread)
        {
            return invalid(spread_message);
        }

        return presence.phase ? invalid(phase_message) : m110::Status::success();
    }

    return presence.spread ? invalid(spread_fixed_message) : m110::Status::success();
}

[[nodiscard]] m110::Status read_path(const json::Document& document, const json::Value& object, WattersonPath& path) noexcept
{
    if (object.type != json::Type::object)
    {
        return invalid(paths_message);
    }

    path = WattersonPath{};
    PathPresence presence{};
    std::uint32_t index = object.first_child;

    for (std::uint32_t member = 0U; member < object.child_count; ++member)
    {
        const json::Value* value = document.child(index);

        if (value == nullptr)
        {
            return invalid(paths_message);
        }

        const m110::Status status = read_path_member(*value, path, presence);

        if (!status.is_ok())
        {
            return status;
        }

        index = value->next_sibling;
    }

    return check_presence(path, presence);
}

[[nodiscard]] m110::Status read_paths(const json::Document& document, const json::Value& paths, WattersonParams& params) noexcept
{
    if (paths.type != json::Type::array || paths.child_count == 0U || paths.child_count > watterson_max_paths)
    {
        return invalid(paths_message);
    }

    const std::span<WattersonPath, watterson_max_paths> slots{params.paths};

    for (std::uint32_t index = 0U; index < paths.child_count; ++index)
    {
        const json::Value* item = document.at(paths, index);

        if (item == nullptr)
        {
            return invalid(paths_message);
        }

        const m110::Status status = read_path(document, *item, slots[index]);

        if (!status.is_ok())
        {
            return status;
        }
    }

    params.path_count = paths.child_count;
    return m110::Status::success();
}

[[nodiscard]] m110::Status check_stage_members(const json::Document& document, const json::Value& object) noexcept
{
    std::uint32_t index = object.first_child;
    std::uint32_t types = 0U;
    std::uint32_t paths = 0U;
    std::uint32_t stream_ids = 0U;

    for (std::uint32_t member = 0U; member < object.child_count; ++member)
    {
        const json::Value* value = document.child(index);

        if (value == nullptr || (key_of(*value) != type_member && key_of(*value) != paths_member && key_of(*value) != stream_id_member))
        {
            return invalid(stage_member_message);
        }

        types += key_of(*value) == type_member ? 1U : 0U;
        paths += key_of(*value) == paths_member ? 1U : 0U;
        stream_ids += key_of(*value) == stream_id_member ? 1U : 0U;

        if (types > 1U || paths > 1U || stream_ids > 1U)
        {
            return invalid(stage_duplicate_message);
        }

        index = value->next_sibling;
    }

    return m110::Status::success();
}

} // namespace

m110::Status validate_watterson(const WattersonParams& params) noexcept
{
    if (params.path_count == 0U || params.path_count > watterson_max_paths)
    {
        return invalid(paths_message);
    }

    for (const WattersonPath& path : std::span{params.paths}.first(params.path_count))
    {
        const m110::Status status = validate_path(path);

        if (!status.is_ok())
        {
            return status;
        }
    }

    return m110::Status::success();
}

m110::Status parse_watterson_stage(const json::Document& document, const json::Value& object, WattersonParams& params) noexcept
{
    params = WattersonParams{};

    if (object.type != json::Type::object)
    {
        return invalid(paths_message);
    }

    const m110::Status members = check_stage_members(document, object);

    if (!members.is_ok())
    {
        return members;
    }

    const json::Value* paths = document.find(object, "paths");

    if (paths == nullptr)
    {
        return invalid(paths_message);
    }

    const m110::Status read = read_paths(document, *paths, params);
    return read.is_ok() ? validate_watterson(params) : read;
}

m110::Status validate_watterson_stage(const Scenario& scenario, std::uint32_t index) noexcept
{
    if (index != 0U)
    {
        return invalid(std::span{scenario.stages}.front().type == StageType::watterson ? single_message : first_message);
    }

    return validate_watterson(scenario.watterson);
}

// ---- the engine's adapter ----

m110::Status WattersonStage::prepare(const StageContext& context) noexcept
{
    const StageRandomKey key = context.random_key();
    const m110::Status configured = mChannel == nullptr ? invalid(channel_message) : mChannel->configure(*mParams, key.seed, key.index, mObserver);

    if (!configured.is_ok())
    {
        return configured;
    }

    mStats = StageStats{};
    mStats.events_scheduled = 1U;
    return m110::Status::success();
}

m110::Result<std::size_t> WattersonStage::process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t /*capacity*/) noexcept
{
    const std::span<float> output{block, frames};
    const m110::Status processed = mChannel == nullptr ? invalid(channel_message) : mChannel->process(output, units::SampleIndex{first_input_frame});

    if (!processed.is_ok())
    {
        return processed;
    }

    for (const float sample : output)
    {
        mStats.component_peak = std::max(mStats.component_peak, std::abs(sample));
        mStats.component_energy += as_double(sample) * as_double(sample);
    }

    mStats.component_frames += frames;
    mStats.events_applied = 1U;
    return frames;
}

// ---- one path ----

void WattersonPathState::clear() noexcept
{
    mDoppler.clear();
    mNode = ComplexFloat{};
    mDelta = ComplexFloat{};
    mNextNode = ComplexFloat{};
    mFixedGain = ComplexFloat{};
    mRotator = ComplexDouble{};
    mRotatorStep = ComplexDouble{};
    mInverseFramesPerNode = 0.0;
    mPhaseOffset = 0.0;
    mNodeIndex = 0U;
    mDelay = units::SampleCount{};
    mFramesPerNode = 0U;
    mNodeOffset = 0U;
    mPhaseIncrement = 0U;
    mIndex = 0U;
    mFading = PathFading::fixed;
    mRotates = false;
}

m110::Status WattersonPathState::configure_rotator(const WattersonPath& path, double phase_radians, WattersonPathReport& report) noexcept
{
    const m110::Result<units::PhaseStep> converted = units::phase_step(path.shift, stream_rate);

    if (!converted.has_value())
    {
        return converted.status();
    }

    const units::PhaseStep step = converted.value();
    mPhaseIncrement = step.word();
    mRotates = mPhaseIncrement != 0U;
    mPhaseOffset = mRotates ? phase_radians : 0.0;
    const double step_angle = det::two_pi * as_double(as_signed(mPhaseIncrement)) / units::phase_turn;
    mRotatorStep = ComplexDouble{det::cos(step_angle), det::sin(step_angle)};
    mRotator = exact_rotator(units::SampleIndex{});
    report.phase_step = step;
    report.realised_shift = units::realised_shift(step, stream_rate);
    return m110::Status::success();
}

m110::Status WattersonPathState::configure_fixed(const Setup& setup, WattersonPathReport& report) noexcept
{
    clear();
    const WattersonPath& path = *setup.path;
    mIndex = setup.index;
    mFading = PathFading::fixed;
    const m110::Result<units::SampleCount> delay = units::to_frames(path.delay, stream_rate);

    if (!delay.has_value())
    {
        return delay.status();
    }

    mDelay = delay.value();
    const double amplitude = std::sqrt(setup.power);
    const double phase = path.phase.in(units::radians);

    if (const m110::Status rotator = configure_rotator(path, phase, report); !rotator.is_ok())
    {
        return rotator;
    }

    const ComplexFloat truth{to_float(amplitude * det::cos(phase)), to_float(amplitude * det::sin(phase))};
    // With a shift the phase rides on the rotator; without one the gain holds it.
    mFixedGain = mRotates ? ComplexFloat{to_float(amplitude), 0.0F} : truth;
    report.fading = PathFading::fixed;
    report.delay = mDelay;
    report.amplitude = amplitude;
    report.interpolation_power = 1.0;
    report.gain = units::gain_from_amplitude(amplitude);
    report.phase = path.phase;

    if (setup.observer != nullptr)
    {
        setup.observer->on_tap(TapRecord{units::SampleIndex{}, mIndex, truth});
    }

    return m110::Status::success();
}

m110::Status WattersonPathState::configure_rayleigh(const Setup& setup, WattersonPathReport& report) noexcept
{
    clear();
    const WattersonPath& path = *setup.path;
    mIndex = setup.index;
    mFading = PathFading::rayleigh;
    const m110::Result<units::SampleCount> delay = units::to_frames(path.delay, stream_rate);
    const m110::Result<DopplerDesign> designed = design_doppler(path.spread, stream_rate);

    if (!delay.has_value() || !designed.has_value())
    {
        return delay.has_value() ? designed.status() : delay.status();
    }

    mDelay = delay.value();
    const DopplerDesign& design = designed.value();
    mDoppler.design(design);
    // Design 6: a_i = sqrt(p_i / v_i), v_i from the stored coefficients' own rho1.
    const double power = interpolation_power(mDoppler.lag_one_correlation(), design.frames_per_node);
    const double amplitude = std::sqrt(setup.power / power);
    mFramesPerNode = narrow_count(to_size(design.frames_per_node.value()));
    mInverseFramesPerNode = 1.0 / as_double(mFramesPerNode);

    if (const m110::Status rotator = configure_rotator(path, 0.0, report); !rotator.is_ok())
    {
        return rotator;
    }

    mNode = mDoppler.prime(setup.generator, to_float(amplitude / std::sqrt(mDoppler.energy())));
    mNextNode = mDoppler.next_node();
    mDelta = mNextNode - mNode;
    report.fading = PathFading::rayleigh;
    report.delay = mDelay;
    report.frames_per_node = design.frames_per_node;
    report.node_rate = design.node_rate;
    report.rate_multiple = design.rate_multiple;
    report.doppler_taps = design.taps;
    report.amplitude = amplitude;
    report.interpolation_power = power;
    report.gain = units::gain_from_amplitude(amplitude);

    if (setup.observer != nullptr)
    {
        setup.observer->on_tap(TapRecord{units::SampleIndex{}, mIndex, mNode});
        setup.observer->on_tap(TapRecord{units::SampleIndex{mFramesPerNode}, mIndex, mNextNode});
    }

    return m110::Status::success();
}

ComplexDouble WattersonPathState::exact_rotator(units::SampleIndex frame) const noexcept
{
    const std::uint32_t phase = low_word(frame.value() * mPhaseIncrement);
    const double angle = det::two_pi * as_double(phase) / units::phase_turn + mPhaseOffset;
    return ComplexDouble{det::cos(angle), det::sin(angle)};
}

void WattersonPathState::advance_node(TapObserver* observer) noexcept
{
    mNodeOffset = 0U;
    ++mNodeIndex;
    mNode = mNextNode;
    mNextNode = mDoppler.next_node();
    mDelta = mNextNode - mNode;

    if (observer != nullptr)
    {
        observer->on_tap(TapRecord{units::SampleIndex{(mNodeIndex + 1U) * mFramesPerNode}, mIndex, mNextNode});
    }
}

ComplexFloat WattersonPathState::next_gain(units::SampleIndex frame, TapObserver* observer) noexcept
{
    ComplexFloat gain = mFixedGain;

    if (mFading == PathFading::rayleigh)
    {
        const float mu = to_float(as_double(mNodeOffset) * mInverseFramesPerNode);
        gain = mNode + mu * mDelta;
        ++mNodeOffset;

        if (mNodeOffset == mFramesPerNode)
        {
            advance_node(observer);
        }
    }

    if (mRotates)
    {
        if ((frame.value() & (rotator_resync_frames - 1U)) == 0U)
        {
            mRotator = exact_rotator(frame);
        }

        gain = gain * ComplexFloat{to_float(mRotator.real), to_float(mRotator.imag)};
        mRotator = mRotator * mRotatorStep;
    }

    return gain;
}

void WattersonPathState::render(std::span<float> output, units::SampleIndex first, const AnalyticRing& ring, bool accumulate, TapObserver* observer) noexcept
{
    for (std::size_t frame = 0U; frame < output.size(); ++frame)
    {
        const units::SampleIndex now = first + units::SampleCount{frame};
        const AnalyticSample input = ring.read(now - mDelay);
        const ComplexFloat gain = next_gain(now, observer);
        const float term = (gain.real * input.in_phase) - (gain.imag * input.quadrature);
        output[frame] = accumulate ? output[frame] + term : term;
    }
}

// ---- the channel ----

m110::Status WattersonChannel::configure(const WattersonParams& params, std::uint64_t seed, std::uint32_t stage_index, TapObserver* observer) noexcept
{
    mConfigured = false;
    mFault = false;
    const m110::Status valid = validate_watterson(params);

    if (!valid.is_ok())
    {
        return valid;
    }

    mAnalytic.configure();
    mRing.clear();
    mReport.paths.fill(WattersonPathReport{});
    double total = 0.0;

    for (const WattersonPath& path : std::span{params.paths}.first(params.path_count))
    {
        total += units::power_ratio(path.level);
    }

    const std::span<WattersonPathState, watterson_max_paths> paths{mPaths};
    const std::span<WattersonPathReport, watterson_max_paths> reports{mReport.paths};
    const std::span<const WattersonPath, watterson_max_paths> settings{params.paths};

    for (std::uint32_t index = 0U; index < watterson_max_paths; ++index)
    {
        if (index >= params.path_count)
        {
            paths[index].clear();
            continue;
        }

        const WattersonPath& path = settings[index];
        const WattersonPathState::Setup setup{&path, units::power_ratio(path.level) / total, index, det::Pcg32{seed, watterson_rng_family, watterson_streams_per_stage * stage_index + index},
                                              observer};
        const m110::Status configured = path.fading == PathFading::rayleigh ? paths[index].configure_rayleigh(setup, reports[index]) : paths[index].configure_fixed(setup, reports[index]);

        if (!configured.is_ok())
        {
            return configured;
        }
    }

    mPathCount = params.path_count;
    mNextFrame = units::SampleIndex{};
    mObserver = observer;
    finish_report(params, seed, stage_index);
    mConfigured = true;
    return m110::Status::success();
}

void WattersonChannel::finish_report(const WattersonParams& params, std::uint64_t seed, std::uint32_t stage_index) noexcept
{
    mReport.path_count = params.path_count;
    mReport.hilbert_taps = hilbert_taps;
    mReport.latency = hilbert_latency;
    Fnv1a digest;
    digest.add(watterson_algorithm_version);
    digest.add(stream_rate.hertz());
    digest.add(seed);
    digest.add(stage_index);
    digest.add(params.path_count);
    digest.add(hilbert_taps);
    digest.add(tap_rate_multiple_min);
    digest.add(bits_of(doppler_truncation_decades));

    for (const WattersonPathReport& path : std::span{mReport.paths}.first(params.path_count))
    {
        digest.add(path.fading == PathFading::rayleigh ? 1U : 0U);
        digest.add(path.delay.value());
        digest.add(path.frames_per_node.value());
        digest.add(path.doppler_taps);
        digest.add(path.phase_step.word());
        digest.add(bits_of(path.amplitude));
        digest.add(bits_of(path.interpolation_power));
        digest.add(bits_of(path.phase.in(units::degrees)));
    }

    mReport.parameter_digest = digest.value();
}

m110::Status WattersonChannel::process(std::span<float> block, units::SampleIndex first_frame) noexcept
{
    if (!mConfigured)
    {
        return m110::Status{m110::StatusCode::invalid_configuration, unconfigured_message};
    }

    if (mFault || first_frame != mNextFrame)
    {
        mFault = true;
        return m110::Status{m110::StatusCode::invalid_argument, contiguity_message};
    }

    const std::size_t chunk = to_size(analytic_chunk_frames.value());

    for (std::size_t offset = 0U; offset < block.size(); offset += chunk)
    {
        const std::size_t count = std::min(chunk, block.size() - offset);
        process_chunk(block.subspan(offset, count), first_frame + units::SampleCount{offset});
    }

    mNextFrame = first_frame + units::SampleCount{block.size()};
    return m110::Status::success();
}

void WattersonChannel::process_chunk(std::span<float> chunk, units::SampleIndex first) noexcept
{
    // Design 4.2: the whole chunk enters the ring before any path reads it.
    mAnalytic.process(chunk, first, mRing);
    const std::span<WattersonPathState, watterson_max_paths> paths{mPaths};

    for (std::uint32_t index = 0U; index < mPathCount; ++index)
    {
        paths[index].render(chunk, first, mRing, index > 0U, mObserver);
    }
}

} // namespace signal_lab
