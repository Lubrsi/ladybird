/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibIPC/MachBootstrapListener.h>
#include <LibIPC/MachBootstrapMessages.h>
#include <LibThreading/Thread.h>

namespace IPC {

static constexpr mach_msg_id_t STOP_LISTENER_MESSAGE_ID = 0x4C53544F;

MachBootstrapListener::MachBootstrapListener(ByteString server_port_name, BootstrapRequestHandler on_bootstrap_request)
    : m_thread(Threading::Thread::construct("MachBootstrapListener"sv, [this]() -> intptr_t { thread_loop(); return 0; }))
    , m_server_port_name(move(server_port_name))
    , m_on_bootstrap_request(move(on_bootstrap_request))
{
    VERIFY(m_on_bootstrap_request);
    if (auto err = allocate_server_port(); err.is_error())
        dbgln("Failed to allocate server port: {}", err.error());
    else
        start();
}

MachBootstrapListener::~MachBootstrapListener()
{
    stop();
}

void MachBootstrapListener::start()
{
    m_thread->start();
}

void MachBootstrapListener::stop()
{
    if (!m_thread->needs_to_be_joined())
        return;

    // The listener thread only wakes up for a message, so send it one that asks it to exit.
    mach_msg_header_t stop_message {};
    stop_message.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    stop_message.msgh_size = sizeof(stop_message);
    stop_message.msgh_remote_port = m_server_port_send_right.port();
    stop_message.msgh_local_port = MACH_PORT_NULL;
    stop_message.msgh_id = STOP_LISTENER_MESSAGE_ID;

    auto const ret = mach_msg(&stop_message, MACH_SEND_MSG, sizeof(stop_message), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (ret != KERN_SUCCESS) {
        dbgln("Failed to stop MachBootstrapListener: {}", mach_error_string(ret));
        VERIFY_NOT_REACHED();
    }

    (void)m_thread->join();
}

bool MachBootstrapListener::is_initialized()
{
    return MACH_PORT_VALID(m_server_port_recv_right.port()) && MACH_PORT_VALID(m_server_port_send_right.port());
}

ErrorOr<void> MachBootstrapListener::allocate_server_port()
{
    m_server_port_recv_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    m_server_port_send_right = TRY(m_server_port_recv_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    TRY(m_server_port_recv_right.register_with_bootstrap_server(m_server_port_name));

    dbgln_if(MACH_PORT_DEBUG, "Success! we created and attached mach port {:x} to bootstrap server with name {}", m_server_port_recv_right.port(), m_server_port_name);
    return {};
}

void MachBootstrapListener::thread_loop()
{
    while (true) {
        ReceivedMachMessage message {};

        // Get the pid of the child from the audit trailer so we can associate the port w/it
        mach_msg_options_t const options = MACH_RCV_MSG | MACH_RCV_TRAILER_TYPE(MACH_RCV_TRAILER_AUDIT) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);

        auto const ret = mach_msg(&message.header, options, 0, sizeof(message), m_server_port_recv_right.port(), MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (ret != KERN_SUCCESS) {
            dbgln("mach_msg failed: {}", mach_error_string(ret));
            break;
        }

        if (message.header.msgh_id == STOP_LISTENER_MESSAGE_ID)
            break;

        if (message.header.msgh_id == SELF_TASK_PORT_MESSAGE_ID) {
            auto const& task_port_message = message.body;
            VERIFY(MACH_MSGH_BITS_LOCAL(message.header.msgh_bits) == MACH_MSG_TYPE_MOVE_SEND);
            VERIFY(task_port_message.body.msgh_descriptor_count == 1);
            VERIFY(task_port_message.port_descriptor.type == MACH_MSG_PORT_DESCRIPTOR);
            auto pid = static_cast<pid_t>(task_port_message.trailer.msgh_audit.val[5]);
            auto task_port = Core::MachPort::adopt_right(task_port_message.port_descriptor.name, Core::MachPort::PortRight::Send);

            // Extract reply port from the message header (kernel swaps local/remote on receive)
            auto reply_port = Core::MachPort::adopt_right(message.header.msgh_remote_port, Core::MachPort::PortRight::SendOnce);

            dbgln_if(MACH_PORT_DEBUG, "Received bootstrap request from pid {} (task port {:x}, reply port {:x})", pid, task_port.port(), reply_port.port());
            m_on_bootstrap_request({ pid, move(task_port), move(reply_port) });
            continue;
        }

        VERIFY_NOT_REACHED();
    }
}

}
