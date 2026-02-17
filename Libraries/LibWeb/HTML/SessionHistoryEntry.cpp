/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWebView/SessionHistoryEntryData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SessionHistoryEntry);

void SessionHistoryEntry::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document_state);
    visitor.visit(m_original_source_browsing_context);
    visitor.visit(m_policy_container);
}

SessionHistoryEntry::SessionHistoryEntry()
    : m_classic_history_api_state(MUST(structured_serialize_for_storage(vm(), JS::js_null())))
    , m_navigation_api_state(MUST(structured_serialize_for_storage(vm(), JS::js_undefined())))
    , m_navigation_api_key(MUST(Crypto::generate_random_uuid()))
    , m_navigation_api_id(MUST(Crypto::generate_random_uuid()))
{
}

GC::Ref<SessionHistoryEntry> SessionHistoryEntry::clone() const
{
    GC::Ref<SessionHistoryEntry> entry = *heap().allocate<SessionHistoryEntry>();
    entry->m_step = m_step;
    entry->m_url = m_url;
    entry->m_document_state = m_document_state->clone();
    entry->m_classic_history_api_state = m_classic_history_api_state;
    entry->m_navigation_api_state = m_navigation_api_state;
    entry->m_navigation_api_key = m_navigation_api_key;
    entry->m_navigation_api_id = m_navigation_api_id;
    entry->m_scroll_restoration_mode = m_scroll_restoration_mode;
    entry->m_policy_container = m_policy_container;
    entry->m_browsing_context_name = m_browsing_context_name;
    entry->m_original_source_browsing_context = m_original_source_browsing_context;
    return entry;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-document
GC::Ptr<DOM::Document> SessionHistoryEntry::document() const
{
    // To get a session history entry's document, return its document state's document.
    if (!m_document_state)
        return {};
    return m_document_state->document();
}

WebView::SerializedSessionHistoryEntry SessionHistoryEntry::serialize() const
{
    WebView::SerializedSessionHistoryEntry serialized;

    serialized.step = m_step.visit(
        [](int step) -> i32 { return step; },
        [](Pending) -> i32 { return -1; });

    serialized.url = m_url;

    if (m_document_state)
        serialized.document_state = m_document_state->serialize();

    serialized.classic_history_api_state = m_classic_history_api_state;
    serialized.navigation_api_state = m_navigation_api_state;
    serialized.navigation_api_key = m_navigation_api_key;
    serialized.navigation_api_id = m_navigation_api_id;

    switch (m_scroll_restoration_mode) {
    case ScrollRestorationMode::Auto:
        serialized.scroll_restoration_mode = WebView::SerializedScrollRestorationMode::Auto;
        break;
    case ScrollRestorationMode::Manual:
        serialized.scroll_restoration_mode = WebView::SerializedScrollRestorationMode::Manual;
        break;
    }

    if (m_policy_container)
        serialized.policy_container = m_policy_container->serialize();

    serialized.browsing_context_name = m_browsing_context_name;

    return serialized;
}

GC::Ref<SessionHistoryEntry> SessionHistoryEntry::create_from_serialized(GC::Heap& heap, WebView::SerializedSessionHistoryEntry const& serialized)
{
    GC::Ref<SessionHistoryEntry> entry = *heap.allocate<SessionHistoryEntry>();

    if (serialized.step < 0)
        entry->m_step = Pending::Tag;
    else
        entry->m_step = static_cast<int>(serialized.step);

    entry->m_url = serialized.url;
    entry->m_document_state = DocumentState::create_from_serialized(heap, serialized.document_state);
    entry->m_classic_history_api_state = serialized.classic_history_api_state;
    entry->m_navigation_api_state = serialized.navigation_api_state;
    entry->m_navigation_api_key = serialized.navigation_api_key;
    entry->m_navigation_api_id = serialized.navigation_api_id;

    switch (serialized.scroll_restoration_mode) {
    case WebView::SerializedScrollRestorationMode::Auto:
        entry->m_scroll_restoration_mode = ScrollRestorationMode::Auto;
        break;
    case WebView::SerializedScrollRestorationMode::Manual:
        entry->m_scroll_restoration_mode = ScrollRestorationMode::Manual;
        break;
    }

    if (serialized.policy_container.has_value())
        entry->m_policy_container = create_a_policy_container_from_serialized_policy_container(heap, serialized.policy_container.value());

    entry->m_browsing_context_name = serialized.browsing_context_name;

    return entry;
}

}
