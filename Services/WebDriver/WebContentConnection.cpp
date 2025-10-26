/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebDriver/Client.h>
#include <WebDriver/WebContentConnection.h>

namespace WebDriver {

WebContentConnection::WebContentConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<WebDriverClientEndpoint, WebDriverServerEndpoint>(*this, move(transport), 1)
{
}

void WebContentConnection::die()
{
    if (on_close)
        on_close();
}

void WebContentConnection::driver_execution_complete(int request_id, Web::WebDriver::Response response)
{
    m_request_id_allocator.deallocate(request_id);
    auto request_completion = m_pending_requests.take(request_id);
    VERIFY(request_completion.has_value());
    request_completion.value()(move(response));
}

int WebContentConnection::create_pending_request(Function<void(Web::WebDriver::Response)> on_complete)
{
    auto request_id = m_request_id_allocator.allocate();
    auto result = m_pending_requests.set(request_id, move(on_complete));
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
    return request_id;
}

}
