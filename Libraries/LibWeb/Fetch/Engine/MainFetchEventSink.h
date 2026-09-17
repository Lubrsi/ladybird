/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Fetch/Engine/MainFetchEvent.h>

namespace Web::Fetch::Engine {

// Receives every event for a fetch on its agent's thread, in sending order.
class MainFetchEventSink {
public:
    virtual ~MainFetchEventSink() = default;

    virtual void handle_event(MainFetchEvent) = 0;
};

}
