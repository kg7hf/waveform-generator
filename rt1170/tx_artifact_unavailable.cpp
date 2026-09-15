// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "tx_artifact.hpp"
#include "player.hpp"
extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

// Builds without an encoder (or with read-only media) have no artifact writer
// or encoder workspace. WAV playback and live effects remain available.
namespace waveform_generator
{
namespace
{
bool pending{};
tx_protocol::Reply reply{};
}
bool submit_tx_request(const tx_protocol::Request& request) noexcept
{
    taskENTER_CRITICAL();

    if (pending)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    reply = {};
    reply.ok = request.kind == tx_protocol::Kind::info || request.kind == tx_protocol::Kind::query;
    #if defined(WFG_MSC_READ_ONLY)
    reply.error = reply.ok ? "none" : "READ_ONLY_IMAGE";
    #else
    reply.error = reply.ok ? "none" : "ENCODERS_DISABLED";
    #endif
    pending = true;
    taskEXIT_CRITICAL();
    return true;
}
bool take_tx_reply(tx_protocol::Reply& result) noexcept
{
    taskENTER_CRITICAL();

    if (!pending)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    result = reply;
    pending = false;
    taskEXIT_CRITICAL();
    return true;
}
bool tx_artifact_busy() noexcept
{
    return false;
}
TxArtifactSnapshot tx_artifact_snapshot() noexcept
{
    return {};
}
void service_tx_artifact() noexcept { }
void disconnect_tx_artifact() noexcept
{
    taskENTER_CRITICAL();
    pending = false;
    taskEXIT_CRITICAL();
}
void abort_tx_artifact() noexcept { }
}
