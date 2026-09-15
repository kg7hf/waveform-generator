// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "rt1170/tx_artifact.hpp"
#include <cstring>
#include <initializer_list>

int main()
{
    using namespace waveform_generator;
    tx_protocol::Request request;
    tx_protocol::Reply reply;
    request.kind = tx_protocol::Kind::info;

    if (!submit_tx_request(request) || submit_tx_request(request) || !take_tx_reply(reply) || !reply.ok)
    {
        return 1;
    }

    for (const auto kind :
{
    tx_protocol::Kind::generate_file, tx_protocol::Kind::data,
                tx_protocol::Kind::send, tx_protocol::Kind::mode
})
    {
        request.kind = kind;

        if (!submit_tx_request(request) || !take_tx_reply(reply) || reply.ok)
        {
            return 2;
        }

        #if defined(WFG_MSC_READ_ONLY)

        if (std::strcmp(reply.error, "READ_ONLY_IMAGE") != 0)
        {
            return 3;
        }

        #else

        if (std::strcmp(reply.error, "ENCODERS_DISABLED") != 0)
        {
            return 3;
        }

        #endif
    }

    if (tx_artifact_busy())
    {
        return 4;
    }

    if (!submit_tx_request(request))
    {
        return 5;
    }

    disconnect_tx_artifact();

    if (take_tx_reply(reply))
    {
        return 6;
    }

    return 0;
}
