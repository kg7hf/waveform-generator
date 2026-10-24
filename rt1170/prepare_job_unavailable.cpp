// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "prepare_job.hpp"

namespace waveform_generator
{
namespace
{
inline constexpr m110::Status read_only_prepare{.code = m110::StatusCode::unavailable, .message = "READ_ONLY_IMAGE"};
}

// The inspection image has FF_FS_READONLY=1: it must never link a local
// writer or advertise preparation. Keep the status query available.
m110::Status PrepareJob::begin(const prepare::Request& request, Resources) noexcept
{
    snapshot_ = {};
    snapshot_.request = request;
    snapshot_.phase = prepare::Phase::failed;
    snapshot_.status = read_only_prepare;
    return read_only_prepare;
}

m110::Status PrepareJob::begin_managed(const prepare::Request& request, Resources resources) noexcept
{
    return begin(request, resources);
}

m110::Status PrepareJob::step() noexcept
{
    return read_only_prepare;
}

m110::Status PrepareJob::produce_step(std::uint32_t) noexcept
{
    return read_only_prepare;
}

m110::Status PrepareJob::append_produced(std::uint32_t) noexcept
{
    return read_only_prepare;
}

m110::Status PrepareJob::finish_managed() noexcept
{
    return read_only_prepare;
}

m110::Status PrepareJob::cancel() noexcept
{
    return read_only_prepare;
}

bool PrepareJob::busy() const noexcept
{
    return false;
}

m110::Status PrepareJob::commit_prefix() noexcept
{
    return read_only_prepare;
}

m110::Status PrepareJob::read_prefix(std::uint64_t, std::span<std::uint8_t>) noexcept
{
    return read_only_prepare;
}
} // namespace waveform_generator
