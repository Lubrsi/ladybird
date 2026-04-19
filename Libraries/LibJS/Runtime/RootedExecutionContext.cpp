/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/RootedExecutionContext.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

RootedExecutionContext::RootedExecutionContext(VM& vm, u32 registers_and_locals_count, ReadonlySpan<Value> constants, u32 arguments_count)
    : m_vm(&vm)
    , m_ctx(ExecutionContext::create(registers_and_locals_count, constants, arguments_count))
{
    m_vm->did_create_rooted_execution_context({}, *this);
}

RootedExecutionContext::RootedExecutionContext(VM& vm, ExecutionContext const& source)
    : m_vm(&vm)
    , m_ctx(source.copy())
{
    m_vm->did_create_rooted_execution_context({}, *this);
}

RootedExecutionContext::RootedExecutionContext(VM& vm, NonnullOwnPtr<ExecutionContext> context)
    : m_vm(&vm)
    , m_ctx(move(context))
{
    m_vm->did_create_rooted_execution_context({}, *this);
}

RootedExecutionContext::~RootedExecutionContext()
{
    if (m_vm)
        m_vm->did_destroy_rooted_execution_context({}, *this);
}

NonnullOwnPtr<ExecutionContext> RootedExecutionContext::release()
{
    VERIFY(m_vm);
    VERIFY(m_ctx);
    m_vm->did_destroy_rooted_execution_context({}, *this);
    m_vm = nullptr;
    return m_ctx.release_nonnull();
}

}
