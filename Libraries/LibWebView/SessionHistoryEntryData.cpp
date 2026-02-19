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

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-target-history-entry
// Same algorithm as Navigable::get_the_target_history_entry(), but operates on serialized data.
static SerializedSessionHistoryEntry const* get_target_entry(Vector<SerializedSessionHistoryEntry> const& entries, int step)
{
    // Return the item in entries that has the greatest step less than or equal to step.
    SerializedSessionHistoryEntry const* result = nullptr;
    for (auto const& entry : entries) {
        if (entry.step >= 0 && entry.step <= step) {
            if (!result || result->step < entry.step)
                result = &entry;
        }
    }
    return result;
}

static bool entries_share_document(SerializedSessionHistoryEntry const& a, SerializedSessionHistoryEntry const& b)
{
    return a.document_state.document_id.has_value()
        && a.document_state.document_id == b.document_state.document_id;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#get-all-navigables-whose-current-session-history-entry-will-change-or-reload
Vector<String> get_changing_navigable_ids(Vector<SerializedSessionHistoryEntry> const& entries, int current_step, int target_step)
{
    // 1. Let results be an empty list.
    Vector<String> results;

    struct NavigableToCheck {
        Vector<SerializedSessionHistoryEntry> const* entries;
        Optional<String> navigable_id;
    };

    // 2. Let navigablesToCheck be « traversable ».
    Vector<NavigableToCheck> navigables_to_check;
    navigables_to_check.append({ &entries, {} });

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto const* target_entry = get_target_entry(*navigable.entries, target_step);
        if (!target_entry)
            continue;

        // 2. If targetEntry is not navigable's current session history entry or targetEntry's document state's reload
        //    pending is true, then append navigable to results.
        auto const* current_entry = get_target_entry(*navigable.entries, current_step);
        bool is_changing = !current_entry || current_entry->step != target_entry->step || target_entry->document_state.reload_pending;

        if (is_changing && navigable.navigable_id.has_value())
            results.append(navigable.navigable_id.value());

        // 3. If targetEntry's document is navigable's document, and targetEntry's document state's reload pending is
        //    false, then extend navigablesToCheck with the child navigables of navigable.
        if (current_entry && !target_entry->document_state.reload_pending && entries_share_document(*current_entry, *target_entry)) {
            for (auto const& nested : target_entry->document_state.nested_histories)
                navigables_to_check.append({ &nested.entries, nested.id });
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-only-need-history-object-length/index-update
Vector<String> get_non_changing_navigable_ids(Vector<SerializedSessionHistoryEntry> const& entries, int current_step, int target_step)
{
    // 1. Let results be an empty list.
    Vector<String> results;

    struct NavigableToCheck {
        Vector<SerializedSessionHistoryEntry> const* entries;
        Optional<String> navigable_id;
    };

    // 2. Let navigablesToCheck be « traversable ».
    Vector<NavigableToCheck> navigables_to_check;
    navigables_to_check.append({ &entries, {} });

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto const* target_entry = get_target_entry(*navigable.entries, target_step);
        if (!target_entry)
            continue;

        auto const* current_entry = get_target_entry(*navigable.entries, current_step);

        // 2. If targetEntry is navigable's current session history entry and targetEntry's document state's reload pending is false, then:
        if (current_entry && current_entry->step == target_entry->step && !target_entry->document_state.reload_pending) {
            // 1. Append navigable to results.
            if (navigable.navigable_id.has_value())
                results.append(navigable.navigable_id.value());

            // 2. Extend navigablesToCheck with navigable's child navigables.
            for (auto const& nested : target_entry->document_state.nested_histories)
                navigables_to_check.append({ &nested.entries, nested.id });
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-history-object-length-and-index
ScriptHistoryLengthAndIndex compute_script_history_length_and_index(Vector<SerializedSessionHistoryEntry> const& entries, int target_step)
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps(entries);

    // 2. Let scriptHistoryLength be the size of steps.
    auto script_history_length = steps.size();

    // 3. Assert: steps contains step.
    VERIFY(steps.contains_slow(target_step));

    // 4. Let scriptHistoryIndex be the index of step in steps.
    auto script_history_index = *steps.find_first_index(target_step);

    // 5. Return (scriptHistoryLength, scriptHistoryIndex).
    return ScriptHistoryLengthAndIndex {
        .script_history_length = script_history_length,
        .script_history_index = script_history_index,
    };
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
    TRY(encoder.encode(state.document_id));
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
    state.document_id = TRY(decoder.decode<Optional<u64>>());
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
