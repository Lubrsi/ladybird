/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibCore/MachPort.h>
#include <LibThreading/Forward.h>

#if !defined(AK_OS_MACH)
#    error "MachBootstrapListener is only available on Mach kernel-based OS's"
#endif

namespace IPC {

class MachBootstrapListener {
    AK_MAKE_NONCOPYABLE(MachBootstrapListener);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct BootstrapRequest {
        pid_t pid { -1 };
        Core::MachPort task_port;
        Core::MachPort reply_port;
    };
    using BootstrapRequestHandler = Function<void(BootstrapRequest)>;

    MachBootstrapListener(ByteString server_port_name, BootstrapRequestHandler);
    ~MachBootstrapListener();

    bool is_initialized();

    ByteString const& server_port_name() const { return m_server_port_name; }

private:
    void start();
    void stop();
    void thread_loop();
    ErrorOr<void> allocate_server_port();

    NonnullRefPtr<Threading::Thread> m_thread;
    ByteString const m_server_port_name;
    BootstrapRequestHandler const m_on_bootstrap_request;
    Core::MachPort m_server_port_recv_right;
    Core::MachPort m_server_port_send_right;
};

}
