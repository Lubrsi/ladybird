/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionToServer.h>
#include <LibWeb/Cookie/Cookie.h>
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

    Function<void()> on_worker_close;

private:
    virtual void die() override;
};

}
