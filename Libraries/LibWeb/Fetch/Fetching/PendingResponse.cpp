/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(PendingResponse);

GC::Ref<PendingResponse> PendingResponse::create(Infrastructure::FetchParams const& fetch_params)
{
    return GC::Heap::the().allocate<PendingResponse>(fetch_params);
}

GC::Ref<PendingResponse> PendingResponse::create(Infrastructure::FetchParams const& fetch_params, GC::Ref<Infrastructure::Response> response)
{
    return GC::Heap::the().allocate<PendingResponse>(fetch_params, response);
}

PendingResponse::PendingResponse(Infrastructure::FetchParams const& fetch_params, GC::Ptr<Infrastructure::Response> response)
    : m_fetch_params(fetch_params)
    , m_response(response)
{
    m_fetch_params->add_pending_response({}, *this);
}

void PendingResponse::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_fetch_params);
    visitor.visit(m_response);
}

void PendingResponse::when_loaded(Callback callback)
{
    VERIFY(!m_callback);
    m_callback = GC::create_function(GC::Heap::the(), move(callback));
    if (m_response)
        run_callback();
}

void PendingResponse::resolve(GC::Ref<Infrastructure::Response> response)
{
    VERIFY(!m_response);
    m_response = response;
    if (m_callback)
        run_callback();
}

void PendingResponse::run_callback()
{
    VERIFY(m_callback);
    VERIFY(m_response);
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this] {
        VERIFY(m_callback);
        VERIFY(m_response);
        m_callback->function()(*m_response);
        m_fetch_params->remove_pending_response({}, *this);
    }));
}

}
