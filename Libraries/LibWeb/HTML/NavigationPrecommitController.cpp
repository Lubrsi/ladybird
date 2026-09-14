/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/NavigateEvent.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationDestination.h>
#include <LibWeb/HTML/NavigationPrecommitController.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigationPrecommitController);

GC::Ref<NavigationPrecommitController> NavigationPrecommitController::create(GC::Ref<NavigateEvent> event)
{
    return GC::Heap::the().allocate<NavigationPrecommitController>(event);
}

NavigationPrecommitController::NavigationPrecommitController(GC::Ref<NavigateEvent> event)
    : m_event(event)
{
}

NavigationPrecommitController::~NavigationPrecommitController() = default;

void NavigationPrecommitController::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_event);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationprecommitcontroller-redirect
WebIDL::ExceptionOr<void> NavigationPrecommitController::redirect(Utf16String url, NavigationNavigateOptions const& options)
{
    // 1. Assert: this's event's interception state is not "none".
    VERIFY(m_event->interception_state() != NavigateEvent::InterceptionState::None);

    // 2. Perform shared checks given this's event.
    TRY(m_event->perform_shared_checks());

    // 3. If this's event's interception state is not "intercepted", then throw an "InvalidStateError" DOMException.
    if (m_event->interception_state() != NavigateEvent::InterceptionState::Intercepted)
        return WebIDL::InvalidStateError::create("NavigateEvent has already been committed"_utf16);

    // 4. If this's event's navigationType is neither "push" nor "replace", then throw an "InvalidStateError"
    //    DOMException.
    auto navigation_type = m_event->navigation_type();
    if (navigation_type != NavigationType::Push && navigation_type != NavigationType::Replace)
        return WebIDL::InvalidStateError::create("Only push and replace navigations can be redirected"_utf16);

    // 5. Let document be this's relevant global object's associated Document.
    auto& window = m_event->relevant_window();
    auto& document = window.associated_document();

    // 6. Let destinationURL be the result of parsing url given document.
    auto destination_url = DOMURL::parse(url.utf16_view(), document.base_url());

    // 7. If destinationURL is failure, then throw a "SyntaxError" DOMException.
    if (!destination_url.has_value())
        return WebIDL::SyntaxError::create("Cannot redirect to an invalid URL"_utf16);

    // 8. If document cannot have its URL rewritten to destinationURL, then throw a "SecurityError" DOMException.
    if (!can_have_its_url_rewritten(document, *destination_url))
        return WebIDL::SecurityError::create("Cannot redirect to a URL the document's URL cannot be rewritten to"_utf16);

    // 9. If options["history"] is "push" or "replace", then set this's event's navigationType to options["history"].
    if (options.history == NavigationHistoryBehavior::Push)
        m_event->set_navigation_type(NavigationType::Push);
    else if (options.history == NavigationHistoryBehavior::Replace)
        m_event->set_navigation_type(NavigationType::Replace);

    // 10. If options["state"] exists:
    if (options.state.has_value()) {
        // 1. Let serializedState be the result of calling StructuredSerializeForStorage(options["state"]). This may
        //    throw an exception.
        auto serialized_state = TRY(structured_serialize_for_storage(window.vm(), *options.state));

        // 2. Set this's event's destination's state to serializedState.
        m_event->destination()->set_state(serialized_state);

        // 3. Set this's event's target's ongoing API method tracker's serialized state to serializedState.
        // NB: The tracker is null for navigations that did not start through the navigation API's methods.
        if (auto api_method_tracker = window.navigation()->ongoing_api_method_tracker())
            api_method_tracker->serialized_state = move(serialized_state);
    }

    // 11. Set this's event's destination's URL to destinationURL.
    m_event->destination()->set_url(*destination_url);

    // 12. If options["info"] exists, then set this's event's info to options["info"].
    if (options.info.has_value())
        m_event->set_info(*options.info);

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationprecommitcontroller-addhandler
WebIDL::ExceptionOr<void> NavigationPrecommitController::add_handler(WebIDL::CallbackType& handler)
{
    // 1. Assert: this's event's interception state is not "none".
    VERIFY(m_event->interception_state() != NavigateEvent::InterceptionState::None);

    // 2. Perform shared checks given this's event.
    TRY(m_event->perform_shared_checks());

    // 3. If this's event's interception state is not "intercepted", then throw an "InvalidStateError" DOMException.
    if (m_event->interception_state() != NavigateEvent::InterceptionState::Intercepted)
        return WebIDL::InvalidStateError::create("NavigateEvent has already been committed"_utf16);

    // 4. Append handler to this's event's navigation handler list.
    m_event->append_navigation_handler(handler);

    return {};
}

}
