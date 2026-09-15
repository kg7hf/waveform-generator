// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include "common/tx_protocol.hpp"
#include <cstdint>

namespace waveform_generator
{
struct TxArtifactSnapshot
{
    // 0 idle, 1 receiving payload, 2 creating WAV, 3 complete, 4 failed/aborted.
    std::uint32_t state{};
    std::uint32_t payload_bytes{};
    std::uint64_t frames_written{};
    std::uint64_t total_frames{};
    std::uint16_t rate{600};
    std::uint8_t interleave{1};
    char filename[13] {"GEN.WAV"};
    char wav_sha256[65] {};
    const char* error{"none"};
};
[[nodiscard]] bool submit_tx_request(const tx_protocol::Request&) noexcept;
[[nodiscard]] bool take_tx_reply(tx_protocol::Reply&) noexcept;
[[nodiscard]] bool tx_artifact_busy() noexcept;
[[nodiscard]] TxArtifactSnapshot tx_artifact_snapshot() noexcept;
void service_tx_artifact() noexcept; // player task only, at most one generation block
void disconnect_tx_artifact() noexcept; // asynchronous cancellation of partial upload
void abort_tx_artifact() noexcept; // player owner only; completed artifacts are retained
}
