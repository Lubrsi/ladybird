/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibWebView/Settings.h>

namespace WebView {

class WorkerImplementation
    : public RefCounted<WorkerImplementation>
    , public SettingsObserver {
public:
    static NonnullRefPtr<WorkerImplementation> create(u64 worker_id, NonnullRefPtr<WebWorkerClient>);
    ~WorkerImplementation();

    u64 worker_id() const { return m_worker_id; }
    WebWorkerClient& client() { return *m_client; }

    void initialize_client();

    Function<void()> on_close;

protected:
    virtual void languages_changed() override;
    virtual void global_privacy_control_changed() override;

private:
    WorkerImplementation(u64, NonnullRefPtr<WebWorkerClient>);
    void setup_callbacks();

    u64 m_worker_id;
    NonnullRefPtr<WebWorkerClient> m_client;
};

}
