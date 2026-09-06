#include "tx_artifact.hpp"
#include "player.hpp"
extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

// Inspection images retain a compile-time read-only FatFs profile and contain
// no SD artifact writer. DD008 can identify the image but cannot create files.
namespace waveform_generator
{
namespace { bool pending{}; tx_protocol::Reply reply{}; }
bool submit_tx_request(const tx_protocol::Request& request) noexcept
{
    taskENTER_CRITICAL();
    if (pending) { taskEXIT_CRITICAL(); return false; }
    reply = {};
    reply.ok = request.kind == tx_protocol::Kind::info || request.kind == tx_protocol::Kind::query;
    reply.error = reply.ok ? "none" : "READ_ONLY_IMAGE";
    pending = true;
    taskEXIT_CRITICAL();
    return true;
}
bool take_tx_reply(tx_protocol::Reply& result) noexcept
{
    taskENTER_CRITICAL();
    if (!pending) { taskEXIT_CRITICAL(); return false; }
    result = reply; pending = false;
    taskEXIT_CRITICAL();
    return true;
}
bool tx_artifact_busy() noexcept { return false; }
TxArtifactSnapshot tx_artifact_snapshot() noexcept { return {}; }
void service_tx_artifact() noexcept { }
void disconnect_tx_artifact() noexcept { taskENTER_CRITICAL(); pending = false; taskEXIT_CRITICAL(); }
void abort_tx_artifact() noexcept { }
}
