/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/MemoryStream.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Weakable.h>
#include <LibCore/Proxy.h>
#include <LibDNS/Resolver.h>
#include <LibHTTP/HeaderMap.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <RequestServer/CacheLevel.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestPipe.h>

struct curl_slist;

namespace RequestServer {

class Request : public Weakable<Request> {
public:
    virtual ~Request();

    URL::URL const& url() const { return m_url; }
    ByteString const& method() const { return m_method; }
    UnixDateTime request_start_time() const { return m_request_start_time; }

    void notify_request_unblocked(Badge<DiskCache>);
    void notify_fetch_complete(Badge<CURLMultiHandleSession>, int result_code);

protected:
    virtual void request_started(int /* reader_fd */) {}
    virtual void headers_received(HTTP::HeaderMap /* response_headers */, Optional<u32> /* status_code */, Optional<String> /* reason_phrase */) {}
    virtual void request_finished(u64 /* total_size */, Requests::RequestTimingInfo /* timing_info */, Optional<Requests::NetworkError> /* network_error */) {}
    virtual void request_complete() {}

    enum class State : u8 {
        Init,         // Decide whether to service this request from cache or the network.
        ReadCache,    // Read the cached response from disk.
        WaitForCache, // Wait for an existing cache entry to complete before proceeding.
        DNSLookup,    // Resolve the URL's host.
        Connect,      // Issue a network request to connect to the URL.
        Fetch,        // Issue a network request to fetch the URL.
        Complete,     // Finalize the request with the client.
        Error,        // Any error occured during the request's lifetime.
    };

    void transition_to_state(State);
    void process();

    Request(
        Optional<DiskCache&> disk_cache,
        void* curl_multi,
        Variant<NonnullRefPtr<Resolver>, NonnullRefPtr<DNS::LookupResult const>> dns,
        URL::URL url,
        ByteString method,
        HTTP::HeaderMap request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    Request(
        void* curl_multi,
        Variant<NonnullRefPtr<Resolver>, NonnullRefPtr<DNS::LookupResult const>> dns,
        URL::URL url);

private:
    enum class Type : u8 {
        Fetch,
        Connect,
    };

    void handle_initial_state();
    void handle_read_cache_state();
    void handle_dns_lookup_state();
    void handle_connect_state();
    void handle_fetch_state();
    void handle_complete_state();
    void handle_error_state();

    static size_t on_header_received(void* buffer, size_t size, size_t nmemb, void* user_data);
    static size_t on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data);

    ErrorOr<void> inform_client_request_started();
    void transfer_headers_to_client_if_needed();
    ErrorOr<void> write_queued_bytes_without_blocking();
    ErrorOr<void> revalidation_failed();

    u32 acquire_status_code() const;
    Requests::RequestTimingInfo acquire_timing_info() const;

    Type m_type { Type::Fetch };
    State m_state { State::Init };

    Optional<DiskCache&> m_disk_cache;

    void* m_curl_multi_handle { nullptr };
    void* m_curl_easy_handle { nullptr };
    Vector<curl_slist*> m_curl_string_lists;
    Optional<int> m_curl_result_code;

    Variant<NonnullRefPtr<Resolver>, NonnullRefPtr<DNS::LookupResult const>> m_dns;

    URL::URL m_url;
    ByteString m_method;

    UnixDateTime m_request_start_time { UnixDateTime::now() };
    HTTP::HeaderMap m_request_headers;
    ByteBuffer m_request_body;

    ByteString m_alt_svc_cache_path;
    Core::ProxyData m_proxy_data;

    u32 m_status_code { 0 };
    Optional<String> m_reason_phrase;

    HTTP::HeaderMap m_response_headers;
    bool m_sent_response_headers_to_client { false };

    AllocatingMemoryStream m_response_buffer;
    RefPtr<Core::Notifier> m_client_writer_notifier;
    Optional<RequestPipe> m_client_request_pipe;

    Optional<size_t> m_start_offset_of_response_resumed_from_cache;
    size_t m_bytes_transferred_to_client { 0 };

    Optional<CacheEntryReader&> m_cache_entry_reader;
    Optional<CacheEntryWriter&> m_cache_entry_writer;

    Optional<Requests::NetworkError> m_network_error;
};

class RequestFromClient final : public Request {
public:
    static NonnullOwnPtr<RequestFromClient> fetch(
        i32 request_id,
        ConnectionFromClient& client,
        Optional<DiskCache&> disk_cache,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        HTTP::HeaderMap request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    static NonnullOwnPtr<RequestFromClient> connect(
        i32 request_id,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        CacheLevel cache_level);

    virtual ~RequestFromClient() override = default;

private:
    RequestFromClient(
        i32 request_id,
        ConnectionFromClient& client,
        Optional<DiskCache&> disk_cache,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        HTTP::HeaderMap request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    RequestFromClient(
        i32 request_id,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url);

    virtual void request_started(int reader_fd) override;
    virtual void headers_received(HTTP::HeaderMap response_headers, Optional<u32> status_code, Optional<String> reason_phrase) override;
    virtual void request_finished(u64 total_size, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error) override;
    virtual void request_complete() override;

    i32 m_request_id { 0 };
    ConnectionFromClient& m_client;
};

}
