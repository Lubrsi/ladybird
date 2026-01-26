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

void WebWorkerClient::did_set_cookie(URL::URL url, Web::Cookie::ParsedCookie cookie, Web::Cookie::Source source)
{
    Application::cookie_jar().set_cookie(url, cookie, source);
}

void WebWorkerClient::did_update_cookie(Web::Cookie::Cookie cookie)
{
    Application::cookie_jar().update_cookie(move(cookie));
}

Messages::WebWorkerClient::DidRequestAllCookiesCookiestoreResponse WebWorkerClient::did_request_all_cookies_cookiestore(URL::URL url)
{
    return Application::cookie_jar().get_all_cookies_cookiestore(url);
}

Messages::WebWorkerClient::DidRequestNamedCookieResponse WebWorkerClient::did_request_named_cookie(URL::URL url, String name)
{
    return Application::cookie_jar().get_named_cookie(url, name);
}

WebWorkerClient::WebWorkerClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
{
}

}
