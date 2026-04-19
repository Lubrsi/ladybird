/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/OwnPtr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ExecutionContext.h>

namespace JS {

class JS_API [[nodiscard]] RootedExecutionContext {
    AK_MAKE_NONCOPYABLE(RootedExecutionContext);
    AK_MAKE_NONMOVABLE(RootedExecutionContext);

public:
    RootedExecutionContext(VM& vm, u32 registers_and_locals_count, ReadonlySpan<Value> constants, u32 arguments_count);
    RootedExecutionContext(VM& vm, ExecutionContext const& source);
    RootedExecutionContext(VM& vm, NonnullOwnPtr<ExecutionContext>);

    ~RootedExecutionContext();

    ExecutionContext& operator*() { return *m_ctx; }
    ExecutionContext const& operator*() const { return *m_ctx; }
    ExecutionContext* operator->() { return m_ctx.ptr(); }
    ExecutionContext const* operator->() const { return m_ctx.ptr(); }
    ExecutionContext* ptr() { return m_ctx.ptr(); }

    NonnullOwnPtr<ExecutionContext> release();

    void visit_edges(Cell::Visitor& visitor)
    {
        if (m_ctx)
            m_ctx->visit_edges(visitor);
    }

private:
    friend class VM;

    VM* m_vm { nullptr };
    OwnPtr<ExecutionContext> m_ctx;
    IntrusiveListNode<RootedExecutionContext> m_list_node;

public:
    using List = IntrusiveList<&RootedExecutionContext::m_list_node>;
};

}
