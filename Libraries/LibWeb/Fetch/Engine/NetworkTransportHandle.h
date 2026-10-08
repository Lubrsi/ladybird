/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Weakable.h>
#include <LibRequests/Request.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>

namespace Web::Fetch::Engine {

// Controls the RequestServer request a fetch's response arrives through.
//
// Every method runs on the loop the handle was created on.
class WEB_API NetworkTransportHandle final : public FetchBodyDeliveryGate::Transport
    , public Weakable<NetworkTransportHandle> {
public:
    [[nodiscard]] static NonnullRefPtr<NetworkTransportHandle> create(NonnullRefPtr<Requests::Request>, int request_server_client_id, u64 request_server_request_id);

    // The ids another client adopts the request by: those RequestServer gave it, or the lease it was adopted under.
    [[nodiscard]] int request_server_client_id() const { return m_request_server_client_id; }
    [[nodiscard]] u64 request_server_request_id() const { return m_request_server_request_id; }

    [[nodiscard]] bool has_file_backed_response_body() const;

    virtual void resume_body_delivery_up_to(u64 byte_count) override;
    virtual void pause_body_delivery() override;
    virtual void stop() override;

    void resume_body_delivery();
    void release_transfer_lease();

    void set_on_requires_network(Function<void()>);
    void set_on_teardown(Function<void(Requests::Request::TeardownReason)>);

private:
    NetworkTransportHandle(NonnullRefPtr<Requests::Request>, int request_server_client_id, u64 request_server_request_id);

    NonnullRefPtr<Requests::Request> m_request;
    int m_request_server_client_id { -1 };
    u64 m_request_server_request_id { 0 };
};

}
