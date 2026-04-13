/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/AgentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>

namespace Web::Bindings {

class WebEngineCustomJobCallbackData final : public JS::JobCallback::CustomData {
    GC_CELL(WebEngineCustomJobCallbackData, JS::JobCallback::CustomData);
    GC_DECLARE_ALLOCATOR(WebEngineCustomJobCallbackData);

public:
    WebEngineCustomJobCallbackData(HTML::EnvironmentSettingsObject& incumbent_settings, OwnPtr<JS::ExecutionContext> active_script_context)
        : incumbent_settings(incumbent_settings)
        , active_script_context(move(active_script_context))
    {
    }

    virtual ~WebEngineCustomJobCallbackData() override = default;

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(incumbent_settings);
        if (active_script_context)
            active_script_context->visit_edges(visitor);
    }

    GC::Ref<HTML::EnvironmentSettingsObject> incumbent_settings;
    OwnPtr<JS::ExecutionContext> active_script_context;
};

HTML::Script* active_script();

WEB_API void initialize_main_thread_vm(AgentType);
WEB_API JS::VM& main_thread_vm();

void queue_mutation_observer_microtask();
WEB_API NonnullOwnPtr<JS::ExecutionContext> create_a_new_javascript_realm(JS::VM&, Function<JS::Object*(JS::Realm&)> create_global_object, Function<JS::Object*(JS::Realm&)> create_global_this_value);
WEB_API void invoke_custom_element_reactions(Vector<GC::Weak<DOM::Element>>& element_queue);

}
