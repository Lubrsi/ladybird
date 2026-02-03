/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/MemoryStream.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/Proxy.h>
#include <LibDNS/Resolver.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cache/CacheRequest.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <RequestServer/CacheLevel.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestPipe.h>

struct curl_slist;

namespace RequestServer {

class Request : public HTTP::CacheRequest {
public:
    enum class Type : u8 {
        Fetch,
        ResolveOnly,
        Connect,
        BackgroundRevalidation,
    };

    virtual ~Request() override;

    u64 request_id() const { return m_request_id; }
    Type type() const { return m_type; }
    URL::URL const& url() const { return m_url; }
    ByteString const& method() const { return m_method; }

    virtual void notify_request_unblocked(Badge<HTTP::DiskCache>) override;
    void notify_fetch_complete(Badge<CURLMultiHandleSession>, int result_code);

protected:
    virtual void on_request_started([[maybe_unused]] int reader_fd) { }
    virtual void on_headers_became_available([[maybe_unused]] NonnullRefPtr<HTTP::HeaderList> response_headers, [[maybe_unused]] Optional<u32> status_code, [[maybe_unused]] Optional<String> reason_phrase) { }
    virtual void on_request_finished([[maybe_unused]] u64 total_size, [[maybe_unused]] Requests::RequestTimingInfo timing_info, [[maybe_unused]] Optional<Requests::NetworkError> network_error) { }
    virtual void on_request_complete() { }
    virtual void on_start_revalidation_request([[maybe_unused]] ByteString method, [[maybe_unused]] URL::URL url, [[maybe_unused]] NonnullRefPtr<HTTP::HeaderList> request_headers, [[maybe_unused]] ByteBuffer request_body, [[maybe_unused]] Core::ProxyData proxy_data) { }

    Request(
        u64 request_id,
        Type type,
        Optional<HTTP::DiskCache&> disk_cache,
        HTTP::CacheMode cache_mode,
        void* curl_multi,
        Variant<NonnullRefPtr<Resolver>, NonnullRefPtr<DNS::LookupResult const>> dns,
        URL::URL url,
        ByteString method,
        NonnullRefPtr<HTTP::HeaderList> request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    Request(
        u64 request_id,
        Type type,
        void* curl_multi,
        Variant<NonnullRefPtr<Resolver>, NonnullRefPtr<DNS::LookupResult const>> dns,
        URL::URL url);

    void process();

    enum class State : u8 {
        Init,              // Decide whether to service this request from cache or the network.
        ReadCache,         // Read the cached response from disk.
        WaitForCache,      // Wait for an existing cache entry to complete before proceeding.
        FailedCacheOnly,   // An only-if-cached request failed to find a cache entry.
        ServeSubstitution, // Serve content from a local file substitution.
        DNSLookup,         // Resolve the URL's host.
        Connect,           // Issue a network request to connect to the URL.
        Fetch,             // Issue a network request to fetch the URL.
        Complete,          // Finalize the request with the client.
        Error,             // Any error occured during the request's lifetime.
    };

    void transition_to_state(State);

private:
    static constexpr StringView state_name(State state)
    {
        switch (state) {
        case State::Init:
            return "Init"sv;
        case State::ReadCache:
            return "ReadCache"sv;
        case State::WaitForCache:
            return "WaitForCache"sv;
        case State::FailedCacheOnly:
            return "FailedCacheOnly"sv;
        case State::ServeSubstitution:
            return "ServeSubstitution"sv;
        case State::DNSLookup:
            return "DNSLookup"sv;
        case State::Connect:
            return "Connect"sv;
        case State::Fetch:
            return "Fetch"sv;
        case State::Complete:
            return "Complete"sv;
        case State::Error:
            return "Error"sv;
        }
        VERIFY_NOT_REACHED();
    }

    void handle_initial_state();
    void handle_read_cache_state();
    void handle_failed_cache_only_state();
    void handle_serve_substitution_state();
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

    virtual bool is_revalidation_request() const override;
    ErrorOr<void> revalidation_failed();

    bool is_cache_only_request() const;

    u32 acquire_status_code() const;
    Requests::RequestTimingInfo acquire_timing_info() const;

    u64 m_request_id { 0 };
    Type m_type { Type::Fetch };
    State m_state { State::Init };

    Optional<HTTP::DiskCache&> m_disk_cache;
    HTTP::CacheMode m_cache_mode { HTTP::CacheMode::Default };

    void* m_curl_multi_handle { nullptr };
    void* m_curl_easy_handle { nullptr };
    Vector<curl_slist*> m_curl_string_lists;
    Optional<int> m_curl_result_code;

    Variant<NonnullRefPtr<Resolver>, NonnullRefPtr<DNS::LookupResult const>> m_dns;
    RefPtr<Core::Promise<NonnullRefPtr<DNS::LookupResult const>>> m_pending_dns_request;

    URL::URL m_url;
    ByteString m_method;

    UnixDateTime m_request_start_time { UnixDateTime::now() };
    NonnullRefPtr<HTTP::HeaderList> m_request_headers;
    ByteBuffer m_request_body;

    ByteString m_alt_svc_cache_path;
    Core::ProxyData m_proxy_data;

    Optional<u32> m_status_code;
    Optional<String> m_reason_phrase;

    NonnullRefPtr<HTTP::HeaderList> m_response_headers;
    bool m_sent_response_headers_to_client { false };

    AllocatingMemoryStream m_response_buffer;
    RefPtr<Core::Notifier> m_client_writer_notifier;
    Optional<RequestPipe> m_client_request_pipe;
    size_t m_bytes_transferred_to_client { 0 };

    Optional<Requests::NetworkError> m_network_error;
};

class RequestFromClient final : public Request {
public:
    static NonnullOwnPtr<RequestFromClient> fetch(
        u64 request_id,
        Optional<HTTP::DiskCache&> disk_cache,
        HTTP::CacheMode cache_mode,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        NonnullRefPtr<HTTP::HeaderList> request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    static NonnullOwnPtr<RequestFromClient> connect(
        u64 request_id,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        CacheLevel cache_level);

    static NonnullOwnPtr<RequestFromClient> revalidate(
        u64 request_id,
        Optional<HTTP::DiskCache&> disk_cache,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        NonnullRefPtr<HTTP::HeaderList> request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

private:
    RequestFromClient(
        u64 request_id,
        Type type,
        Optional<HTTP::DiskCache&> disk_cache,
        HTTP::CacheMode cache_mode,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        NonnullRefPtr<HTTP::HeaderList> request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    RequestFromClient(
        u64 request_id,
        Type type,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url);

    virtual void on_request_started(int reader_fd) override;
    virtual void on_headers_became_available(NonnullRefPtr<HTTP::HeaderList> response_headers, Optional<u32> status_code, Optional<String> reason_phrase) override;
    virtual void on_request_finished(u64 total_size, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error) override;
    virtual void on_request_complete() override;
    virtual void on_start_revalidation_request(ByteString method, URL::URL url, NonnullRefPtr<HTTP::HeaderList> request_headers, ByteBuffer request_body, Core::ProxyData proxy_data) override;

    ConnectionFromClient& m_client;
};

}
