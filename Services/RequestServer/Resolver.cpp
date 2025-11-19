/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Request.h"


#include <AK/LexicalPath.h>
#include <LibCore/StandardPaths.h>
#include <LibRequests/Request.h>
#include <LibTLS/TLSv12.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

static ByteString g_default_certificate_path;

ByteString const& default_certificate_path()
{
    return g_default_certificate_path;
}

void set_default_certificate_path(ByteString default_certificate_path)
{
    g_default_certificate_path = move(default_certificate_path);
}

DNSInfo& DNSInfo::the()
{
    static DNSInfo g_dns_info;
    return g_dns_info;
}

class DNSRequest final : public Request {
public:
    static NonnullOwnPtr<DNSRequest> fetch(
        NetworkOrdered<u16> original_query_id,
        void* curl_multi,
        DNS::LookupResult const& dns_result,
        URL::URL url,
        ByteString method,
        HTTP::HeaderMap request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Function<void(DNS::Messages::Message)> on_complete)
    {
        auto request = adopt_own(*new DNSRequest { original_query_id, curl_multi, dns_result, move(url), move(method), move(request_headers), move(request_body), move(alt_svc_cache_path), move(on_complete) });
        request->process();

        return request;
    }

private:
    DNSRequest(
        NetworkOrdered<u16> original_query_id,
        void* curl_multi,
        DNS::LookupResult const& dns_result,
        URL::URL url,
        ByteString method,
        HTTP::HeaderMap request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Function<void(DNS::Messages::Message)> on_complete)
            : Request(
                OptionalNone {},
                curl_multi,
                NonnullRefPtr { dns_result },
                move(url),
                move(method),
                move(request_headers),
                move(request_body),
                move(alt_svc_cache_path),
                {})
            , m_original_query_id(original_query_id)
            , m_on_complete(move(on_complete))
    {
    }

    virtual void request_started(int reader_fd) override
    {
        m_read_stream = MUST(Requests::ReadStream::create(reader_fd));
    }

    virtual void request_finished(u64, Requests::RequestTimingInfo, Optional<Requests::NetworkError> network_error) override
    {
        if (network_error.has_value()) {
            dbgln("DNS: (HTTPS) Query failed: {}", network_error_to_string(network_error.value()));
            return;
        }

        auto result = DNS::Messages::Message::from_raw(*m_read_stream);
        if (result.is_error()) {
            dbgln("DNS: (HTTPS) Failed to parse message: {}", result.release_error());
            return;
        }

        m_on_complete(result.release_value());
    }

    NetworkOrdered<u16> m_original_query_id;
    OwnPtr<Requests::ReadStream> m_read_stream;
    Function<void(DNS::Messages::Message)> m_on_complete;
};

// https://datatracker.ietf.org/doc/html/rfc8484
class HTTPSResolverTunnel final : public DNS::ResolverTunnel {
public:
    static ErrorOr<NonnullOwnPtr<HTTPSResolverTunnel>> create(NonnullRefPtr<Resolver> resolver, URL::URL url)
    {
        if (url.scheme() != "https")
            return Error::from_string_literal("DNS-over-HTTPS URL must have the https scheme");

        if (!url.host().has_value())
            return Error::from_string_literal("DNS-over-HTTPS URL must have a hostname");

        if (url.includes_credentials()
            || url.query().has_value()
            || url.fragment().has_value()) {
            return Error::from_string_literal("DNS-over-HTTPS URL is only allowed to have a scheme, host, port and path");
        }

        // Since we're setting up the tunnel and since the URL can be a hostname, we have to use the system resolver first.
        auto serialized_host = url.serialized_host();

        // FIXME: Handle expiry.
        auto resolved_host_result = TRY(resolver->dns.lookup_with_system_resolver(serialized_host));

        return adopt_own(*new HTTPSResolverTunnel(move(resolved_host_result), move(url)));
    }

    virtual ~HTTPSResolverTunnel() override = default;

    virtual ErrorOr<void> dispatch_query(DNS::Messages::Message query) override
    {
        // "Using the GET method is friendlier to many HTTP cache implementations."
        // "In order to maximize HTTP cache friendliness, DoH clients using media formats that include the ID field
        // from the DNS message header, such as "application/dns-message", SHOULD use a DNS ID of 0 in every DNS
        // request. HTTP correlates the request and response, thus eliminating the need for the ID in a media type
        // such as "application/dns-message". The use of a varying DNS ID can cause semantically equivalent DNS
        // queries to be cached separately."
        // NOTE: Since DNS::Resolver requires the response to have the passed in ID for the query, we stash away
        //       the original ID and restore it when sending back the response.
        auto original_query_id = query.header.id;
        query.header.id = 0;

        ByteBuffer query_bytes;
        TRY(query.to_raw(query_bytes));

        // "When the HTTP method is GET, the single variable "dns" is defined as the content of the DNS request (as
        // described in Section 6), encoded with base64url [RFC4648]."
        // "When using the GET method, the data payload for this media type MUST be encoded with base64url [RFC4648]
        // and then provided as a variable named "dns" to the URI Template expansion. Padding characters for
        // base64url MUST NOT be included."
        auto encoded_query = TRY(encode_base64url(query_bytes, AK::OmitPadding::Yes));

        auto copy_url = m_url;
        copy_url.set_query(TRY(String::formatted("dns={}", encoded_query)));

        // "The DoH client SHOULD include an HTTP Accept request header field to indicate what type of content can be
        // understood in response. Irrespective of the value of the Accept request header field, the client MUST be
        // prepared to process "application/dns-message" (as described in Section 6) responses but MAY also process
        // other DNS-related media types it receives."
        HTTP::HeaderMap request_headers;
        request_headers.set("Accept"sv, "application/dns-message"sv);

        auto on_complete = [this, original_query_id](DNS::Messages::Message result) {
            result.header.id = original_query_id;
            if (on_message_received)
                on_message_received(move(result));
        };

        auto dns_request = DNSRequest::fetch(
            original_query_id,
            m_curl_multi_handle_session.curl_multi_handle(),
            m_resolved_host_result,
            move(copy_url),
            "GET"sv,
            move(request_headers),
            ByteBuffer {},
            m_curl_multi_handle_session.alt_svc_cache_path(),
            move(on_complete));

        m_active_requests.append(move(dns_request));

        return {};
    }

    virtual bool is_open() const override
    {
        // Sockets are handled automatically by curl as we make requests, so the tunnel is always open.
        return true;
    }

private:
    HTTPSResolverTunnel(NonnullRefPtr<DNS::LookupResult const> resolved_host_result, URL::URL url)
        : m_url(move(url))
        , m_resolved_host_result(move(resolved_host_result))
    {
    }

    CURLMultiHandleSession m_curl_multi_handle_session;
    URL::URL m_url;
    NonnullRefPtr<DNS::LookupResult const> m_resolved_host_result;
    Vector<NonnullOwnPtr<Request>> m_active_requests;
};

NonnullRefPtr<Resolver> Resolver::default_resolver()
{
    static WeakPtr<Resolver> g_resolver {};

    if (auto resolver = g_resolver.strong_ref())
        return *resolver;

    auto resolver = adopt_ref(*new Resolver([] -> NonnullRefPtr<Core::Promise<MaybeOwned<DNS::ResolverTunnel>>> {
        auto promise = Core::Promise<MaybeOwned<DNS::ResolverTunnel>>::construct();

        auto result = [] -> ErrorOr<MaybeOwned<DNS::ResolverTunnel>> {
            auto& dns_info = DNSInfo::the();

            if (auto* udp_socket_info = dns_info.info.get_pointer<DNSOverUDPSocketInfo>(); udp_socket_info) {
                return adopt_own(*new DNS::UDPSocketResolverTunnel(
                    TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(udp_socket_info->server_address))))
                ));
            }

            if (auto* tls_socket_info = dns_info.info.get_pointer<DNSOverTLSSocketInfo>(); tls_socket_info) {
                TLS::Options options;

                if (!g_default_certificate_path.is_empty())
                    options.root_certificates_path = g_default_certificate_path;

                return adopt_own(*new DNS::TLSSocketResolverTunnel(
                    TRY(TLS::TLSv12::connect(tls_socket_info->server_address, tls_socket_info->server_hostname, move(options)))
                ));
            }

            if (auto* https_info = dns_info.info.get_pointer<DNSOverHTTPSInfo>(); https_info) {
                return TRY(HTTPSResolverTunnel::create(default_resolver(), https_info->resolver_url));
            }

            return Error::from_string_literal("No DNS server configured");
        }();

        if (!result.is_error()) {
            promise->resolve(result.release_value());
        } else {
            promise->reject(result.release_error());
        }

        return promise;
    }));

    g_resolver = resolver;
    return resolver;
}

Resolver::Resolver(DNS::Resolver::CreateTunnelFunction create_tunnel)
    : dns(move(create_tunnel))
{
}

}
