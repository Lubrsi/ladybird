/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/Cache/MemoryCache.h>
#include <LibWeb/Fetch/Fetching/NetworkBodyPump.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(NetworkBodyPump);

NetworkBodyPump::NetworkBodyPump(GC::Ptr<Infrastructure::FetchParams const> fetch_params, NonnullRefPtr<FetchByteChannel> channel, NonnullRefPtr<FetchBodyDeliveryGate> delivery_gate, RefPtr<HTTP::MemoryCache> http_cache)
    : m_fetch_params(fetch_params)
    , m_channel(move(channel))
    , m_delivery_gate(move(delivery_gate))
    , m_http_cache(move(http_cache))
{
}

NetworkBodyPump::~NetworkBodyPump() = default;

void NetworkBodyPump::set_body(GC::Ref<Fetch::Infrastructure::Body> body)
{
    m_body = body;
    // Flush any bytes that were buffered before the body was set
    if (!m_pre_body_sniff_buffer.is_empty()) {
        m_body->append_sniff_bytes(m_pre_body_sniff_buffer);
        m_pre_body_sniff_buffer.clear();
    }
    // If the stream already completed before the body was set,
    // we missed the terminal sniff call in handle_network_complete / handle_network_error.
    if (m_network_complete)
        m_body->set_sniff_bytes_complete();
    if (m_network_failed)
        m_body->set_sniff_bytes_failed();
}

void NetworkBodyPump::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_params);
    visitor.visit(m_response);
    visitor.visit(m_body);
}

// This implements the transmission steps of HTTP-network fetch.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
void NetworkBodyPump::handle_network_data(Requests::ResponseData data)
{
    // 1. Run these steps, but abort when fetchParams is canceled:
    if (m_fetch_params && m_fetch_params->is_canceled()) {
        m_channel->cancel();
        return;
    }

    // 1. If one or more bytes have been transmitted from response’s message body, then:
    auto bytes = data.bytes();
    if (bytes.is_empty())
        return;

    // 1. Let bytes be the transmitted bytes.

    // FIXME: 2. Let codings be the result of extracting header list values given `Content-Encoding` and response’s header list.
    // FIXME: 3. Increase response’s body info’s encoded size by bytes’s length.
    // FIXME: 4. Set bytes to the result of handling content codings given codings and bytes.
    // FIXME: 5. Increase response’s body info’s decoded size by bytes’s length.
    // FIXME: 6. If bytes is failure, then terminate fetchParams’s controller.

    // Capture bytes for MIME sniffing
    if (m_body) {
        if (auto const& immutable_bytes = data.immutable_bytes(); immutable_bytes.has_value() && immutable_bytes->is_file_backed() && m_body->source().has<Empty>() && bytes.size() == immutable_bytes->size())
            m_body->set_source(*immutable_bytes, static_cast<u64>(immutable_bytes->size()));
        m_body->append_sniff_bytes(bytes);
    } else if (m_pre_body_sniff_buffer.size() < Infrastructure::MAX_SNIFF_BYTES) {
        auto space_remaining = Infrastructure::MAX_SNIFF_BYTES - m_pre_body_sniff_buffer.size();
        m_pre_body_sniff_buffer.append(bytes.slice(0, min(bytes.size(), space_remaining)));
    }

    if (m_http_cache && !m_cache_body_replaces_network_buffer) {
        if (auto const& immutable_bytes = data.immutable_bytes(); immutable_bytes.has_value() && immutable_bytes->is_file_backed() && m_cache_buffer.is_empty() && !m_cache_body.has_value()) {
            m_cache_body = *immutable_bytes;
        } else {
            if (m_cache_body.has_value()) {
                m_cache_buffer.append(m_cache_body->bytes());
                m_cache_body.clear();
            }
            m_cache_buffer.append(bytes);
        }
    }

    m_delivery_gate->did_deliver(bytes.size());

    // 7. Append bytes to buffer. (Steps 7 and 8 live in FetchByteChannel::write().)
    auto chunk = data.immutable_bytes().has_value() && data.immutable_bytes()->size() == bytes.size()
        ? *data.immutable_bytes()
        : MUST(Core::ImmutableBytes::copy(bytes));
    m_channel->write(move(chunk));
}

void NetworkBodyPump::handle_network_complete()
{
    m_network_complete = true;
    // Mark sniff bytes as complete when the stream ends
    if (m_body)
        m_body->set_sniff_bytes_complete();

    finalize_cache_entry();

    // 2. Otherwise, if the bytes transmission for response’s message body is done normally and stream is readable,
    //    then close stream, and abort these in-parallel steps.
    // NOTE: The channel's Closed terminal preserves buffered bytes; the stream closes once the
    //       consumer has drained them.
    m_channel->close();
}

void NetworkBodyPump::handle_network_error(StringView error_message)
{
    m_network_failed = true;
    // A response that died mid-body must not satisfy a sniff waiter with partial bytes.
    if (m_body)
        m_body->set_sniff_bytes_failed();

    m_channel->error({ .message = MUST(ByteBuffer::copy(error_message.bytes())) });
}

void NetworkBodyPump::set_cached_response_body(Core::ImmutableBytes body)
{
    if (!m_http_cache)
        return;

    m_cache_buffer.clear();
    m_cache_body = move(body);
    m_cache_body_replaces_network_buffer = true;
}

void NetworkBodyPump::finalize_cache_entry()
{
    if (!m_http_cache || !m_fetch_params)
        return;

    auto request = m_fetch_params->request();
    if (!m_fetch_params->is_canceled() && m_response && request->cache_mode() != HTTP::CacheMode::NoStore) {
        auto response_body = m_cache_body.has_value()
            ? m_cache_body.release_value()
            : Core::ImmutableBytes::adopt(move(m_cache_buffer));
        m_http_cache->finalize_entry(request->current_url(), request->method(), request->header_list(), m_response->status(), m_response->header_list(), move(response_body));
    }

    m_http_cache.clear();
    m_cache_body.clear();
}

}
