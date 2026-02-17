/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWebView/SessionHistoryEntryData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DocumentState);

DocumentState::DocumentState() = default;

DocumentState::~DocumentState() = default;

GC::Ref<DocumentState> DocumentState::clone() const
{
    GC::Ref<DocumentState> cloned = *heap().allocate<DocumentState>();
    cloned->m_document = m_document;
    cloned->m_history_policy_container = m_history_policy_container;
    cloned->m_request_referrer = m_request_referrer;
    cloned->m_request_referrer_policy = m_request_referrer_policy;
    cloned->m_initiator_origin = m_initiator_origin;
    cloned->m_origin = m_origin;
    cloned->m_about_base_url = m_about_base_url;
    cloned->m_nested_histories = m_nested_histories;
    cloned->m_resource = m_resource;
    cloned->m_reload_pending = m_reload_pending;
    cloned->m_ever_populated = m_ever_populated;
    cloned->m_navigable_target_name = m_navigable_target_name;
    return cloned;
}

WebView::SerializedDocumentState DocumentState::serialize() const
{
    WebView::SerializedDocumentState serialized;

    serialized.origin = m_origin;
    serialized.initiator_origin = m_initiator_origin;
    serialized.about_base_url = m_about_base_url;

    m_request_referrer.visit(
        [&](URL::URL const& url) {
            serialized.request_referrer = url;
        },
        [&](Fetch::Infrastructure::Request::Referrer referrer) {
            switch (referrer) {
            case Fetch::Infrastructure::Request::Referrer::NoReferrer:
                serialized.request_referrer = WebView::SerializedReferrer::NoReferrer;
                break;
            case Fetch::Infrastructure::Request::Referrer::Client:
                serialized.request_referrer = WebView::SerializedReferrer::Client;
                break;
            }
        });

    serialized.request_referrer_policy = m_request_referrer_policy;
    serialized.navigable_target_name = m_navigable_target_name;
    serialized.reload_pending = m_reload_pending;
    serialized.ever_populated = m_ever_populated;
    serialized.resource = m_resource;

    m_history_policy_container.visit(
        [&](GC::Ref<PolicyContainer> const& policy_container) {
            serialized.history_policy_container = policy_container->serialize();
        },
        [&](Client) {
            serialized.history_policy_container = {};
        });

    for (auto const& nested_history : m_nested_histories) {
        WebView::SerializedDocumentState::SerializedNestedHistory serialized_nested;
        serialized_nested.id = nested_history.id;
        for (auto const& entry : nested_history.entries)
            serialized_nested.entries.append(entry->serialize());
        serialized.nested_histories.append(move(serialized_nested));
    }

    return serialized;
}

GC::Ref<DocumentState> DocumentState::create_from_serialized(GC::Heap& heap, WebView::SerializedDocumentState const& serialized)
{
    GC::Ref<DocumentState> state = *heap.allocate<DocumentState>();

    state->m_origin = serialized.origin;
    state->m_initiator_origin = serialized.initiator_origin;
    state->m_about_base_url = serialized.about_base_url;

    serialized.request_referrer.visit(
        [&](URL::URL const& url) {
            state->m_request_referrer = url;
        },
        [&](WebView::SerializedReferrer referrer) {
            switch (referrer) {
            case WebView::SerializedReferrer::NoReferrer:
                state->m_request_referrer = Fetch::Infrastructure::Request::Referrer::NoReferrer;
                break;
            case WebView::SerializedReferrer::Client:
                state->m_request_referrer = Fetch::Infrastructure::Request::Referrer::Client;
                break;
            }
        });

    state->m_request_referrer_policy = serialized.request_referrer_policy;
    state->m_navigable_target_name = serialized.navigable_target_name;
    state->m_reload_pending = serialized.reload_pending;
    state->m_ever_populated = true;
    state->m_resource = serialized.resource;

    if (serialized.history_policy_container.has_value())
        state->m_history_policy_container = create_a_policy_container_from_serialized_policy_container(heap, serialized.history_policy_container.value());
    else
        state->m_history_policy_container = Client::Tag;

    for (auto const& serialized_nested : serialized.nested_histories) {
        DocumentState::NestedHistory nested;
        nested.id = serialized_nested.id;
        for (auto const& serialized_entry : serialized_nested.entries)
            nested.entries.append(SessionHistoryEntry::create_from_serialized(heap, serialized_entry));
        state->m_nested_histories.append(move(nested));
    }

    // Document is not serializable; restored entries have document = nullptr.
    state->m_document = nullptr;

    return state;
}

void DocumentState::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    m_history_policy_container.visit(
        [&](GC::Ref<PolicyContainer> const& policy_container) { visitor.visit(policy_container); },
        [](auto const&) {});
    for (auto& nested_history : m_nested_histories) {
        visitor.visit(nested_history.entries);
    }
}

}
