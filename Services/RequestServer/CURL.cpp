/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Request.h>
#include <RequestServer/WebSocketImplCurl.h>

namespace RequestServer {

ByteString build_curl_resolve_list(DNS::LookupResult const& dns_result, StringView host, u16 port)
{
    StringBuilder resolve_opt_builder;
    resolve_opt_builder.appendff("{}:{}:", host, port);

    for (auto const& [i, addr] : enumerate(dns_result.cached_addresses())) {
        auto formatted_address = addr.visit(
            [&](IPv4Address const& ipv4) { return ipv4.to_byte_string(); },
            [&](IPv6Address const& ipv6) { return MUST(ipv6.to_string()).to_byte_string(); });

        if (i > 0)
            resolve_opt_builder.append(',');
        resolve_opt_builder.append(formatted_address);
    }

    dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Resolve list: {}", resolve_opt_builder.string_view());
    return resolve_opt_builder.to_byte_string();
}

Requests::NetworkError curl_code_to_network_error(int code)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
        return Requests::NetworkError::UnableToResolveHost;
    case CURLE_COULDNT_RESOLVE_PROXY:
        return Requests::NetworkError::UnableToResolveProxy;
    case CURLE_COULDNT_CONNECT:
        return Requests::NetworkError::UnableToConnect;
    case CURLE_OPERATION_TIMEDOUT:
        return Requests::NetworkError::TimeoutReached;
    case CURLE_TOO_MANY_REDIRECTS:
        return Requests::NetworkError::TooManyRedirects;
    case CURLE_SSL_CONNECT_ERROR:
        return Requests::NetworkError::SSLHandshakeFailed;
    case CURLE_PEER_FAILED_VERIFICATION:
        return Requests::NetworkError::SSLVerificationFailed;
    case CURLE_URL_MALFORMAT:
        return Requests::NetworkError::MalformedUrl;
    case CURLE_BAD_CONTENT_ENCODING:
        return Requests::NetworkError::InvalidContentEncoding;
    default:
        return Requests::NetworkError::Unknown;
    }
}

CURLMultiHandleSession::CURLMultiHandleSession()
{
    m_curl_multi = curl_multi_init();

    auto set_option = [this](auto option, auto value) {
        auto result = curl_multi_setopt(m_curl_multi, option, value);
        VERIFY(result == CURLM_OK);
    };
    set_option(CURLMOPT_SOCKETFUNCTION, &on_socket_callback);
    set_option(CURLMOPT_SOCKETDATA, this);
    set_option(CURLMOPT_TIMERFUNCTION, &on_timeout_callback);
    set_option(CURLMOPT_TIMERDATA, this);

    m_timer = Core::Timer::create_single_shot(0, [this] {
        auto result = curl_multi_socket_action(m_curl_multi, CURL_SOCKET_TIMEOUT, 0, nullptr);
        VERIFY(result == CURLM_OK);
        check_active_requests();
    });
}

CURLMultiHandleSession::~CURLMultiHandleSession()
{
    curl_multi_cleanup(m_curl_multi);
    m_curl_multi = nullptr;
}

int CURLMultiHandleSession::on_socket_callback(CURL*, int sockfd, int what, void* user_data, void*)
{
    auto* client = static_cast<CURLMultiHandleSession*>(user_data);

    if (what == CURL_POLL_REMOVE) {
        client->m_read_notifiers.remove(sockfd);
        client->m_write_notifiers.remove(sockfd);
        return 0;
    }

    if (what & CURL_POLL_IN) {
        client->m_read_notifiers.ensure(sockfd, [client, sockfd, multi = client->m_curl_multi] {
            auto notifier = Core::Notifier::construct(sockfd, Core::NotificationType::Read);
            notifier->on_activation = [client, sockfd, multi] {
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_IN, nullptr);
                VERIFY(result == CURLM_OK);

                client->check_active_requests();
            };

            notifier->set_enabled(true);
            return notifier;
        });
    }

    if (what & CURL_POLL_OUT) {
        client->m_write_notifiers.ensure(sockfd, [client, sockfd, multi = client->m_curl_multi] {
            auto notifier = Core::Notifier::construct(sockfd, Core::NotificationType::Write);
            notifier->on_activation = [client, sockfd, multi] {
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_OUT, nullptr);
                VERIFY(result == CURLM_OK);

                client->check_active_requests();
            };

            notifier->set_enabled(true);
            return notifier;
        });
    }

    return 0;
}

void CURLMultiHandleSession::check_active_requests()
{
    int msgs_in_queue = 0;
    while (auto* msg = curl_multi_info_read(m_curl_multi, &msgs_in_queue)) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        void* application_private = nullptr;
        auto result = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &application_private);
        VERIFY(result == CURLE_OK);
        VERIFY(application_private != nullptr);

        // FIXME: Come up with a unified way to track websockets and standard fetches instead of this nasty tagged pointer
        if (reinterpret_cast<uintptr_t>(application_private) & websocket_private_tag) {
            auto* websocket_impl = reinterpret_cast<WebSocketImplCurl*>(reinterpret_cast<uintptr_t>(application_private) & ~websocket_private_tag);
            if (msg->data.result == CURLE_OK) {
                if (!websocket_impl->did_connect())
                    websocket_impl->on_connection_error();
            } else {
                websocket_impl->on_connection_error();
            }
            continue;
        }

        auto* request = static_cast<Request*>(application_private);
        request->notify_fetch_complete({}, msg->data.result);
    }
}

int CURLMultiHandleSession::on_timeout_callback(void*, long timeout_ms, void* user_data)
{
    auto* client = static_cast<CURLMultiHandleSession*>(user_data);
    if (!client->m_timer)
        return 0;

    if (timeout_ms < 0)
        client->m_timer->stop();
    else
        client->m_timer->restart(timeout_ms);

    return 0;
}

}
