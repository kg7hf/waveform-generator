// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "prepared_stream.hpp"

namespace waveform_generator
{
// A read-only player never binds a prepared stream. Its member's virtual
// sink methods must still exist without linking any FatFs write operation.
m110::Status Rt1170PreparedStream::write_exact(std::span<const std::uint8_t>) noexcept
{
    return {.code = m110::StatusCode::unavailable, .message = "READ_ONLY_IMAGE"};
}

m110::Status Rt1170PreparedStream::seek(std::uint64_t) noexcept
{
    return {.code = m110::StatusCode::unavailable, .message = "READ_ONLY_IMAGE"};
}
} // namespace waveform_generator
