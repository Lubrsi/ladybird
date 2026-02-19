/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWebView/Export.h>

namespace WebView {

// Mirrors Web::Fetch::Infrastructure::Request::Referrer for IPC without pulling in Requests.h
enum class SerializedReferrer : u8 {
    NoReferrer,
    Client,
};

// Mirrors Web::HTML::ScrollRestorationMode for IPC without pulling in SessionHistoryEntry.h
enum class SerializedScrollRestorationMode : u8 {
    Auto,
    Manual,
};

struct SerializedDocumentState {
    // Unique identifier for the document this state belongs to. Entries sharing the same document
    // (e.g. from pushState/replaceState/fragment navigations) have the same document_id.
    // Assigned during serialization based on DocumentState* identity.
    Optional<u64> document_id;

    Optional<URL::Origin> origin;
    Optional<URL::Origin> initiator_origin;
    Optional<URL::URL> about_base_url;

    // Request referrer: either a URL or one of the Referrer enum values (NoReferrer, Client).
    Variant<URL::URL, SerializedReferrer> request_referrer { SerializedReferrer::Client };

    Web::ReferrerPolicy::ReferrerPolicy request_referrer_policy { Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY };
    String navigable_target_name;
    bool reload_pending { false };
    bool ever_populated { false };

    // Resource: empty, a String (URL), or a POSTResource.
    Variant<Empty, String, Web::HTML::POSTResource> resource {};

    // History policy container: nullopt means "client" (the default per spec).
    Optional<Web::HTML::SerializedPolicyContainer> history_policy_container;

    struct SerializedNestedHistory {
        String id;
        Vector<struct SerializedSessionHistoryEntry> entries;
    };

    Vector<SerializedNestedHistory> nested_histories;
};

struct SerializedSessionHistoryEntry {
    // Step: either a non-negative integer or "pending" (represented as i32 with -1 as sentinel for pending).
    i32 step { -1 };

    URL::URL url;

    SerializedDocumentState document_state;

    // classic history API state and navigation API state are SerializationRecord = IPC::MessageDataType = Vector<u8, 1024>
    IPC::MessageDataType classic_history_api_state;
    IPC::MessageDataType navigation_api_state;

    String navigation_api_key;
    String navigation_api_id;

    SerializedScrollRestorationMode scroll_restoration_mode { SerializedScrollRestorationMode::Auto };

    Optional<Web::HTML::SerializedPolicyContainer> policy_container;

    Optional<ByteString> browsing_context_name;
};

WEBVIEW_API Vector<int> get_all_used_history_steps(Vector<SerializedSessionHistoryEntry> const& entries);

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
// Same algorithm as TraversableNavigable::get_the_used_step(), but operates on serialized data.
WEBVIEW_API int get_the_used_step(Vector<SerializedSessionHistoryEntry> const& entries, int step);

struct ScriptHistoryLengthAndIndex {
    size_t script_history_length { 0 };
    size_t script_history_index { 0 };
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#get-all-navigables-whose-current-session-history-entry-will-change-or-reload
// Same algorithm as TraversableNavigable::get_all_navigables_whose_current_session_history_entry_will_change_or_reload(),
// but operates on serialized data. Returns navigable IDs from nested histories; the traversable itself is not included.
WEBVIEW_API Vector<String> get_changing_navigable_ids(Vector<SerializedSessionHistoryEntry> const& entries, int current_step, int target_step);

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-only-need-history-object-length/index-update
// Same algorithm as TraversableNavigable::get_all_navigables_that_only_need_history_object_length_index_update(),
// but operates on serialized data. Returns navigable IDs from nested histories; the traversable itself is not included.
WEBVIEW_API Vector<String> get_non_changing_navigable_ids(Vector<SerializedSessionHistoryEntry> const& entries, int current_step, int target_step);

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-history-object-length-and-index
// Same algorithm as TraversableNavigable::get_the_history_object_length_and_index(), but operates on serialized data.
WEBVIEW_API ScriptHistoryLengthAndIndex compute_script_history_length_and_index(Vector<SerializedSessionHistoryEntry> const& entries, int target_step);
}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, Web::HTML::POSTResource::Directive const&);

template<>
WEBVIEW_API ErrorOr<Web::HTML::POSTResource::Directive> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, Web::HTML::POSTResource const&);

template<>
WEBVIEW_API ErrorOr<Web::HTML::POSTResource> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::SerializedDocumentState::SerializedNestedHistory const&);

template<>
WEBVIEW_API ErrorOr<WebView::SerializedDocumentState::SerializedNestedHistory> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::SerializedDocumentState const&);

template<>
WEBVIEW_API ErrorOr<WebView::SerializedDocumentState> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::SerializedSessionHistoryEntry const&);

template<>
WEBVIEW_API ErrorOr<WebView::SerializedSessionHistoryEntry> decode(Decoder&);

}
