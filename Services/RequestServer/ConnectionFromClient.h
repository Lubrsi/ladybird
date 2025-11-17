/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <RequestServer/RequestServerEndpoint.h>

namespace RequestServer {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override;

    virtual void die() override;

    void request_complete(Badge<RequestFromClient>, int request_id);

private:
    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::RequestServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::RequestServer::ConnectNewClientResponse connect_new_client() override;
    virtual Messages::RequestServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;

    virtual Messages::RequestServer::IsSupportedProtocolResponse is_supported_protocol(ByteString) override;
    virtual void set_socket_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally) override;
    virtual void set_https_dns_server(URL::URL resolver_url, bool validate_dnssec_locally) override;
    virtual void set_use_system_dns() override;
    virtual void start_request(i32 request_id, ByteString, URL::URL, HTTP::HeaderMap, ByteBuffer, Core::ProxyData) override;
    virtual Messages::RequestServer::StopRequestResponse stop_request(i32) override;
    virtual Messages::RequestServer::SetCertificateResponse set_certificate(i32, ByteString, ByteString) override;
    virtual void ensure_connection(URL::URL url, ::RequestServer::CacheLevel cache_level) override;

    virtual void estimate_cache_size_accessed_since(u64 cache_size_estimation_id, UnixDateTime since) override;
    virtual void remove_cache_entries_accessed_since(UnixDateTime since) override;

    virtual void websocket_connect(i64 websocket_id, URL::URL, ByteString, Vector<ByteString>, Vector<ByteString>, HTTP::HeaderMap) override;
    virtual void websocket_send(i64 websocket_id, bool, ByteBuffer) override;
    virtual void websocket_close(i64 websocket_id, u16, ByteString) override;
    virtual Messages::RequestServer::WebsocketSetCertificateResponse websocket_set_certificate(i64, ByteString, ByteString) override;

    static ErrorOr<IPC::File> create_client_socket();

    HashMap<i32, NonnullOwnPtr<Request>> m_active_requests;
    HashMap<i32, RefPtr<WebSocket::WebSocket>> m_websockets;

    NonnullOwnPtr<CURLMultiHandleSession> m_curl_multi_handle_session;

    NonnullRefPtr<Resolver> m_resolver;
};

}
