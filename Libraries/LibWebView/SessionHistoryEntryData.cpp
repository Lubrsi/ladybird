/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/SessionHistoryEntryData.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::POSTResource::Directive const& directive)
{
    TRY(encoder.encode(directive.type));
    TRY(encoder.encode(directive.value));
    return {};
}

template<>
ErrorOr<Web::HTML::POSTResource::Directive> decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<String>());
    auto value = TRY(decoder.decode<String>());
    return Web::HTML::POSTResource::Directive { type, move(value) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::POSTResource const& resource)
{
    TRY(encoder.encode(resource.request_body));
    TRY(encoder.encode(resource.request_content_type));
    TRY(encoder.encode(resource.request_content_type_directives));
    return {};
}

template<>
ErrorOr<Web::HTML::POSTResource> decode(Decoder& decoder)
{
    auto request_body = TRY(decoder.decode<Optional<ByteBuffer>>());
    auto request_content_type = TRY(decoder.decode<Web::HTML::POSTResource::RequestContentType>());
    auto request_content_type_directives = TRY(decoder.decode<Vector<Web::HTML::POSTResource::Directive>>());
    return Web::HTML::POSTResource { move(request_body), request_content_type, move(request_content_type_directives) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::SerializedDocumentState::SerializedNestedHistory const& nested_history)
{
    TRY(encoder.encode(nested_history.id));
    TRY(encoder.encode(nested_history.entries));
    return {};
}

template<>
ErrorOr<WebView::SerializedDocumentState::SerializedNestedHistory> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<String>());
    auto entries = TRY(decoder.decode<Vector<WebView::SerializedSessionHistoryEntry>>());
    return WebView::SerializedDocumentState::SerializedNestedHistory { move(id), move(entries) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::SerializedDocumentState const& state)
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
ErrorOr<WebView::SerializedDocumentState> decode(Decoder& decoder)
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
ErrorOr<void> encode(Encoder& encoder, WebView::SerializedSessionHistoryEntry const& entry)
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
ErrorOr<WebView::SerializedSessionHistoryEntry> decode(Decoder& decoder)
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
