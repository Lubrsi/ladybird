/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SimilarOriginWindowAgent);

GC::Ref<SimilarOriginWindowAgent> SimilarOriginWindowAgent::create(GC::Heap& heap)
{
    // See 'creating an agent' step in: https://html.spec.whatwg.org/multipage/webappapis.html#obtain-similar-origin-window-agent
    auto agent = heap.allocate<SimilarOriginWindowAgent>(CanBlock::No);
    agent->event_loop = heap.allocate<HTML::EventLoop>(HTML::EventLoop::Type::Window);
    return agent;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-agent
SimilarOriginWindowAgent& relevant_similar_origin_window_agent(JS::Object const& object)
{
    // The relevant agent for a platform object platformObject is platformObject's relevant Realm's agent.
    // Spec Note: This pointer is not yet defined in the JavaScript specification; see tc39/ecma262#1357.
    return as<SimilarOriginWindowAgent>(*relevant_realm(object).vm().agent());
}

SimilarOriginWindowAgent::SimilarOriginWindowAgent(CanBlock can_block)
    : Agent(can_block)
{
}

void SimilarOriginWindowAgent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(pending_mutation_observers);
    visitor.visit(signal_slots);
    visitor.visit(active_custom_element_constructor_map);
}

}
