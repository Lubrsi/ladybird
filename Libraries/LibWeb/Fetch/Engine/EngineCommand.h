/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Variant.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Fetch/Engine/BodyPresentation.h>
#include <LibWeb/Fetch/Engine/CanonicalFetchInput.h>
#include <LibWeb/Fetch/Engine/Ids.h>

// Each carries copies only.
namespace Web::Fetch::Engine::Command {

struct StartFetch {
    CanonicalFetchInput input;
};

struct Abort {
    FetchId fetch_id;
};

struct Terminate {
    FetchId fetch_id;
};

struct Stop {
    FetchId fetch_id;
};

// The answer to a manual-redirect delivery's continuation, given exactly once.
struct ResolveRedirect {
    struct Follow { };
    struct AcceptAsFinal { };
    struct Abandon { };
    using Resolution = Variant<Follow, AcceptAsFinal, Abandon>;

    DeliveryId delivery_id;
    ContinuationId continuation_id;
    Resolution resolution;
};

// Only ever complete: all bytes present, or a network error with no body.
struct CompletedPreloadResponse {
    struct Completed {
        Delivered<NonNullLocalBodyHandle> body;
    };
    struct Failure {
        NullBody body;
    };

    Variant<Completed, Failure> outcome;
};

struct PreloadResponseResolved {
    FetchId fetch_id;
    CompletedPreloadResponse response;
};

struct TransportControl {
    struct ResumeBodyDeliveryUpTo {
        u64 byte_count { 0 };
    };
    struct PauseBodyDelivery { };
    struct StopTransport { };
    using Operation = Variant<ResumeBodyDeliveryUpTo, PauseBodyDelivery, StopTransport>;

    RequestId request_id;
    TransportEpoch transport_epoch;
    Operation operation;
};

struct AdoptNetworkTransport {
    IPC::TransportHandle handle;
};

}

namespace Web::Fetch::Engine {

struct EngineCommand {
    AK_MAKE_NONCOPYABLE(EngineCommand);
    AK_MAKE_DEFAULT_MOVABLE(EngineCommand);

public:
    using Payload = Variant<
        Command::StartFetch,
        Command::Abort,
        Command::Terminate,
        Command::Stop,
        Command::ResolveRedirect,
        Command::PreloadResponseResolved,
        Command::TransportControl,
        Command::AdoptNetworkTransport>;

    template<typename T>
    requires(Payload::can_contain<T>())
    EngineCommand(T command)
        : payload(move(command))
    {
    }

    Payload payload;
};

}
