/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/NonnullOwnPtr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Engine/EngineTask.h>
#include <LibWeb/Fetch/Engine/FetchExecutorServices.h>

namespace Web::Fetch::Engine {

// Commands reach the engine on its thread, in posting order, and never after shutdown.
class WEB_API FetchExecutor : public AtomicRefCounted<FetchExecutor> {
public:
    virtual ~FetchExecutor();

    void post(EngineCommand);

    // Drops every command that has not started running; one that has started completes.
    virtual void shutdown();

    FetchExecutorServices const& services() const { return m_services; }

protected:
    FetchExecutor(FetchExecutorServices, NonnullOwnPtr<EngineCommandSink>);

    virtual void post_task(EngineTask) = 0;

private:
    FetchExecutorServices m_services;
    NonnullOwnPtr<EngineCommandSink> m_engine;
    Atomic<bool> m_shut_down { false };
};

}
