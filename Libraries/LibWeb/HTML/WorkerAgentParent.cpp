/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerAgentParent);

WorkerAgentParent::WorkerAgentParent(URL::URL url, WorkerOptions const& options, GC::Ptr<MessagePort> outside_port, GC::Ref<EnvironmentSettingsObject> outside_settings, Bindings::AgentType agent_type)
    : m_worker_options(options)
    , m_agent_type(agent_type)
    , m_url(move(url))
    , m_outside_port(outside_port)
    , m_outside_settings(outside_settings)
{
}

void WorkerAgentParent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    m_message_port = MessagePort::create(realm);
    m_message_port->entangle_with(*m_outside_port);

    TransferDataEncoder data_holder;
    MUST(m_message_port->transfer_steps(data_holder));

    // FIXME: Specification says this supposed to happen in step 11 of onComplete handler defined in https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
    //        but that would require introducing a new IPC message type to communicate this from WebWorker to WebContent process,
    //        so let's do it here for now.
    m_outside_port->start();

    // Call new IPC - UI layer handles everything (cookie handling, settings forwarding, etc.)
    m_worker_id = Bindings::principal_host_defined_page(realm).client().start_worker_agent(
        m_url,
        m_worker_options.type,
        m_worker_options.credentials,
        m_worker_options.name,
        move(data_holder),
        m_outside_settings->serialize(),
        m_agent_type);
}

void WorkerAgentParent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_message_port);
    visitor.visit(m_outside_port);
    visitor.visit(m_outside_settings);
}

}
