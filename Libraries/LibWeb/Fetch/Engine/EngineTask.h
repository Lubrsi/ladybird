/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>

namespace Web::Fetch::Engine {

class FetchExecutor;

class EngineTask {
    AK_MAKE_NONCOPYABLE(EngineTask);

public:
    EngineTask(Badge<FetchExecutor>, Function<void()> function)
        : m_function(move(function))
    {
    }

    EngineTask(EngineTask&&) = default;
    EngineTask& operator=(EngineTask&&) = default;

    void run() { m_function(); }

private:
    Function<void()> m_function;
};

}
