/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Agent.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionsStack.h>
#include <LibWeb/HTML/Scripting/Agent.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#similar-origin-window-agent
class SimilarOriginWindowAgent final : public Agent {
    GC_CELL(SimilarOriginWindowAgent, Agent);
    GC_DECLARE_ALLOCATOR(SimilarOriginWindowAgent);

public:
    static GC::Ref<SimilarOriginWindowAgent> create(GC::Heap&);

    // https://dom.spec.whatwg.org/#mutation-observer-compound-microtask-queued-flag
    // Each similar-origin window agent has a mutation observer microtask queued (a boolean), which is initially false. [HTML]
    bool mutation_observer_microtask_queued { false };

    // https://dom.spec.whatwg.org/#mutation-observer-list
    // Each similar-origin window agent also has pending mutation observers (a set of zero or more MutationObserver objects), which is initially empty.
    Vector<GC::Ref<DOM::MutationObserver>> pending_mutation_observers;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reactions-stack
    // Each similar-origin window agent has a custom element reactions stack, which is initially empty.
    CustomElementReactionsStack custom_element_reactions_stack {};

    // https://dom.spec.whatwg.org/#signal-slot-list
    // Each similar-origin window agent has signal slots (a set of slots), which is initially empty. [HTML]
    Vector<GC::Ref<HTMLSlotElement>> signal_slots;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#current-element-queue
    // A similar-origin window agent's current element queue is the element queue at the top of its custom element reactions stack.
    auto& current_element_queue() { return custom_element_reactions_stack.element_queue_stack.last(); }
    auto const& current_element_queue() const { return custom_element_reactions_stack.element_queue_stack.last(); }

    // https://html.spec.whatwg.org/multipage/custom-elements.html#active-custom-element-constructor-map
    // Each similar-origin window agent has an associated active custom element constructor map, which is a map of
    // constructors to CustomElementRegistry objects.
    HashMap<GC::Ref<JS::FunctionObject>, GC::Ref<CustomElementRegistry>> active_custom_element_constructor_map;

private:
    explicit SimilarOriginWindowAgent(CanBlock);

    virtual void visit_edges(Visitor&) override;
};

WEB_API SimilarOriginWindowAgent& relevant_similar_origin_window_agent(JS::Object const&);

}
