/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationprecommitcontroller
class NavigationPrecommitController final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(NavigationPrecommitController, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(NavigationPrecommitController);

public:
    [[nodiscard]] static GC::Ref<NavigationPrecommitController> create(GC::Ref<NavigateEvent>);

    WebIDL::ExceptionOr<void> redirect(Utf16String url, NavigationNavigateOptions const&);
    WebIDL::ExceptionOr<void> add_handler(WebIDL::CallbackType& handler);

    virtual ~NavigationPrecommitController() override;

private:
    explicit NavigationPrecommitController(GC::Ref<NavigateEvent>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationprecommitcontroller-event
    // Each NavigationPrecommitController has a NavigateEvent event.
    GC::Ref<NavigateEvent> m_event;
};

}
