/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Forward.h>
#include <LibWebSocket/ConnectionInfo.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>
#include <LibWebSocket/Message.h>

namespace WebSocket {

namespace FFI {

struct WebSocketRustCodec;
struct WebSocketRustFrame;

}

enum class ReadyState {
    Connecting = 0,
    Open = 1,
    Closing = 2,
    Closed = 3,
};

// https://datatracker.ietf.org/doc/html/rfc6455#section-7.4.1
enum class CloseStatusCode : u16 {
    Normal = 1000,
    GoingAway = 1001,
    ProtocolError = 1002,
    UnsupportedData = 1003,
    NoStatusReceived = 1005,
    AbnormalClosure = 1006,
    InvalidPayload = 1007,
    PolicyViolation = 1008,
    MessageTooBig = 1009,
    MissingExtension = 1010,
    UnexpectedCondition = 1011,
};

class WebSocket final {
    AK_MAKE_NONCOPYABLE(WebSocket);
    AK_MAKE_NONMOVABLE(WebSocket);

public:
    static NonnullOwnPtr<WebSocket> create(ConnectionInfo, NonnullRefPtr<WebSocketImpl>);
    ~WebSocket();

    ReadyState ready_state();

    // Call this to start the WebSocket connection.
    void start();

    // This can only be used if the `ready_state` is `ReadyState::Open`
    void send(Message const&);

    // This can only be used if the `ready_state` is `ReadyState::Open`
    void close(u16 code = to_underlying(CloseStatusCode::NoStatusReceived), ByteString const& reason = {});

    Function<void()> on_open;
    Function<void(u16 code, ByteString reason, bool was_clean)> on_close;
    Function<void(Message message)> on_message;
    Function<void(ReadyState)> on_ready_state_change;

    enum class Error {
        CouldNotEstablishConnection,
        ConnectionUpgradeFailed,
        ServerClosedSocket,
    };

    Function<void(Error)> on_error;

private:
    WebSocket(ConnectionInfo, NonnullRefPtr<WebSocketImpl>);

    void drain_read();
    void handle_frame(FFI::WebSocketRustFrame const&);

    void notify_open();
    void notify_close(u16 code, ByteString reason, bool was_clean);
    void notify_error(Error);
    void notify_message(Message);

    void discard_connection();

    enum class InternalState {
        NotStarted,
        EstablishingProtocolConnection,
        Open,
        Closing,
        Closed,
        Errored,
    };

    InternalState m_state { InternalState::NotStarted };

    void set_state(InternalState);

    void fail_connection(u16 close_status_code, Error, ByteString const& reason);

    u16 m_last_close_code { to_underlying(CloseStatusCode::NoStatusReceived) };
    ByteString m_last_close_message;

    ConnectionInfo m_connection;
    NonnullRefPtr<WebSocketImpl> m_impl;
    RefPtr<Core::Timer> m_closing_handshake_timer;
    FFI::WebSocketRustCodec* m_codec { nullptr };
};

}
