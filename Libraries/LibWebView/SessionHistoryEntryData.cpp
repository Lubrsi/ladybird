/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/SessionHistoryEntryData.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
// This is the same algorithm as TraversableNavigable::get_all_used_history_steps(), but operates on serialized data.
Vector<int> get_all_used_history_steps(Vector<SerializedSessionHistoryEntry> const& entries)
{
    // 2. Let steps be an empty ordered set of non-negative integers.
    OrderedHashTable<int> steps;

    // 3. Let entryLists be the ordered set « traversable's session history entries ».
    Vector<Vector<SerializedSessionHistoryEntry> const*> entry_lists;
    entry_lists.append(&entries);

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto const* entry_list = entry_lists.take_first();

        // 1. For each entry of entryList:
        for (auto const& entry : *entry_list) {
            // 1. Append entry's step to steps.
            // NOTE: -1 is the sentinel for "pending", skip it.
            if (entry.step >= 0)
                steps.set(entry.step);

            // 2. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto const& nested_history : entry.document_state.nested_histories)
                entry_lists.append(&nested_history.entries);
        }
    }

    // 5. Return steps, sorted.
    auto sorted_steps = steps.values();
    quick_sort(sorted_steps);
    return sorted_steps;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
int get_the_used_step(Vector<SerializedSessionHistoryEntry> const& entries, int step)
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps(entries);

    // 2. Return the greatest item in steps that is less than or equal to step.
    VERIFY(!steps.is_empty());
    Optional<int> result;
    for (auto s : steps) {
        if (s <= step) {
            if (!result.has_value() || *result < s)
                result = s;
        }
    }
    return result.value();
}
}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder& encoder, Web::HTML::POSTResource::Directive const& directive)
{
    TRY(encoder.encode(directive.type));
    TRY(encoder.encode(directive.value));
    return {};
}

template<>
WEBVIEW_API ErrorOr<Web::HTML::POSTResource::Directive> decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<String>());
    auto value = TRY(decoder.decode<String>());
    return Web::HTML::POSTResource::Directive { type, move(value) };
}

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder& encoder, Web::HTML::POSTResource const& resource)
{
    TRY(encoder.encode(resource.request_body));
    TRY(encoder.encode(resource.request_content_type));
    TRY(encoder.encode(resource.request_content_type_directives));
    return {};
}

template<>
WEBVIEW_API ErrorOr<Web::HTML::POSTResource> decode(Decoder& decoder)
{
    auto request_body = TRY(decoder.decode<Optional<ByteBuffer>>());
    auto request_content_type = TRY(decoder.decode<Web::HTML::POSTResource::RequestContentType>());
    auto request_content_type_directives = TRY(decoder.decode<Vector<Web::HTML::POSTResource::Directive>>());
    return Web::HTML::POSTResource { move(request_body), request_content_type, move(request_content_type_directives) };
}

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder& encoder, WebView::SerializedDocumentState::SerializedNestedHistory const& nested_history)
{
    TRY(encoder.encode(nested_history.id));
    TRY(encoder.encode(nested_history.entries));
    return {};
}

template<>
WEBVIEW_API ErrorOr<WebView::SerializedDocumentState::SerializedNestedHistory> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<String>());
    auto entries = TRY(decoder.decode<Vector<WebView::SerializedSessionHistoryEntry>>());
    return WebView::SerializedDocumentState::SerializedNestedHistory { move(id), move(entries) };
}

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder& encoder, WebView::SerializedDocumentState const& state)
{
    TRY(encoder.encode(state.origin));
    TRY(encoder.encode(state.initiator_origin));
    TRY(encoder.encode(state.about_base_url));
    TRY(encoder.encode(state.request_referrer));
    TRY(encoder.encode(state.request_referrer_policy));
    TRY(encoder.encode(state.navigable_target_name));
    TRY(encoder.encode(state.reload_pending));
    TRY(encoder.encode(state.ever_populated));
    TRY(encoder.encode(state.resource));
    TRY(encoder.encode(state.history_policy_container));
    TRY(encoder.encode(state.nested_histories));
    return {};
}

template<>
WEBVIEW_API ErrorOr<WebView::SerializedDocumentState> decode(Decoder& decoder)
{
    WebView::SerializedDocumentState state {};
    state.origin = TRY(decoder.decode<Optional<URL::Origin>>());
    state.initiator_origin = TRY(decoder.decode<Optional<URL::Origin>>());
    state.about_base_url = TRY(decoder.decode<Optional<URL::URL>>());
    state.request_referrer = TRY(decoder.decode<Variant<URL::URL, WebView::SerializedReferrer>>());
    state.request_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());
    state.navigable_target_name = TRY(decoder.decode<String>());
    state.reload_pending = TRY(decoder.decode<bool>());
    state.ever_populated = TRY(decoder.decode<bool>());
    state.resource = TRY(decoder.decode<Variant<Empty, String, Web::HTML::POSTResource>>());
    state.history_policy_container = TRY(decoder.decode<Optional<Web::HTML::SerializedPolicyContainer>>());
    state.nested_histories = TRY(decoder.decode<Vector<WebView::SerializedDocumentState::SerializedNestedHistory>>());
    return state;
}

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder& encoder, WebView::SerializedSessionHistoryEntry const& entry)
{
    TRY(encoder.encode(entry.step));
    TRY(encoder.encode(entry.url));
    TRY(encoder.encode(entry.document_state));
    TRY(encoder.encode(entry.classic_history_api_state));
    TRY(encoder.encode(entry.navigation_api_state));
    TRY(encoder.encode(entry.navigation_api_key));
    TRY(encoder.encode(entry.navigation_api_id));
    TRY(encoder.encode(entry.scroll_restoration_mode));
    TRY(encoder.encode(entry.policy_container));
    TRY(encoder.encode(entry.browsing_context_name));
    return {};
}

template<>
WEBVIEW_API ErrorOr<WebView::SerializedSessionHistoryEntry> decode(Decoder& decoder)
{
    WebView::SerializedSessionHistoryEntry entry {};
    entry.step = TRY(decoder.decode<i32>());
    entry.url = TRY(decoder.decode<URL::URL>());
    entry.document_state = TRY(decoder.decode<WebView::SerializedDocumentState>());
    entry.classic_history_api_state = TRY(decoder.decode<IPC::MessageDataType>());
    entry.navigation_api_state = TRY(decoder.decode<IPC::MessageDataType>());
    entry.navigation_api_key = TRY(decoder.decode<String>());
    entry.navigation_api_id = TRY(decoder.decode<String>());
    entry.scroll_restoration_mode = TRY(decoder.decode<WebView::SerializedScrollRestorationMode>());
    entry.policy_container = TRY(decoder.decode<Optional<Web::HTML::SerializedPolicyContainer>>());
    entry.browsing_context_name = TRY(decoder.decode<Optional<ByteString>>());
    return entry;
}

}
