/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibRequests/RequestClient.h>

namespace Web::Fetch::Engine {

struct FetchExecutorServices {
    // Commands are posted to it; its death drops them.
    NonnullRefPtr<Core::WeakEventLoopReference> event_loop;

    RefPtr<Requests::RequestClient> request_client;
};

}
