/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionToServer.h>
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/Export.h>
#include <Services/WebWorker/WebWorkerClientEndpoint.h>
#include <Services/WebWorker/WebWorkerServerEndpoint.h>

namespace WebView {

class WEB_API WebWorkerClient final
    : public IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>
    , public WebWorkerClientEndpoint {
    C_OBJECT_ABSTRACT(WebWorkerClient);

public:
    explicit WebWorkerClient(NonnullOwnPtr<IPC::Transport>);

    virtual void did_close_worker() override;
    virtual Messages::WebWorkerClient::DidRequestCookieResponse did_request_cookie(URL::URL, Web::Cookie::Source) override;
    virtual void did_set_cookie(URL::URL, Web::Cookie::ParsedCookie, Web::Cookie::Source) override;
    virtual void did_update_cookie(Web::Cookie::Cookie) override;
    virtual Messages::WebWorkerClient::DidRequestAllCookiesCookiestoreResponse did_request_all_cookies_cookiestore(URL::URL) override;
    virtual Messages::WebWorkerClient::DidRequestNamedCookieResponse did_request_named_cookie(URL::URL, String) override;

    Function<void()> on_worker_close;

private:
    virtual void die() override;
};

}
