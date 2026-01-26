/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/WebWorkerClient.h>

namespace WebView {

void WebWorkerClient::die()
{
    // FIXME: Notify WorkerAgent that the worker is dead
}

void WebWorkerClient::did_close_worker()
{
    if (on_worker_close)
        on_worker_close();
}

Messages::WebWorkerClient::DidRequestCookieResponse WebWorkerClient::did_request_cookie(URL::URL url, Web::Cookie::Source source)
{
    return Application::cookie_jar().get_cookie(url, source);
}

WebWorkerClient::WebWorkerClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
{
}

}
