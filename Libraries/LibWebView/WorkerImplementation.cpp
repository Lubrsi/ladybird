/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerImplementation.h>

namespace WebView {

NonnullRefPtr<WorkerImplementation> WorkerImplementation::create(u64 id, NonnullRefPtr<WebWorkerClient> client)
{
    return adopt_ref(*new WorkerImplementation(id, move(client)));
}

WorkerImplementation::WorkerImplementation(u64 id, NonnullRefPtr<WebWorkerClient> client)
    : m_worker_id(id)
    , m_client(move(client))
{
}

WorkerImplementation::~WorkerImplementation() = default;

void WorkerImplementation::initialize_client()
{
    m_client->on_worker_close = [this]() {
        if (on_close)
            on_close();
    };

    languages_changed();
    global_privacy_control_changed();
}

void WorkerImplementation::languages_changed()
{
    m_client->async_set_preferred_languages(Application::settings().languages());
}

void WorkerImplementation::global_privacy_control_changed()
{
    auto gpc = Application::settings().global_privacy_control() == GlobalPrivacyControl::Yes;
    m_client->async_set_enable_global_privacy_control(gpc);
}

}
