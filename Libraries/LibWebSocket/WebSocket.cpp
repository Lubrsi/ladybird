/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWebSocket/RustFFI.h>
#include <LibWebSocket/WebSocket.h>

namespace WebSocket {

static constexpr int s_closing_handshake_timeout_ms = 30'000;

static void send_frame_bytes(void* transport, u8 const* data, size_t length)
{
    static_cast<WebSocketImpl*>(transport)->send({ data, length });
}

NonnullOwnPtr<WebSocket> WebSocket::create(ConnectionInfo connection, NonnullRefPtr<WebSocketImpl> impl)
{
    return adopt_own(*new WebSocket(move(connection), move(impl)));
}

WebSocket::WebSocket(ConnectionInfo connection, NonnullRefPtr<WebSocketImpl> impl)
    : m_connection(move(connection))
    , m_impl(move(impl))
    , m_codec(FFI::websocket_rust_codec_new(FFI::WebSocketRustRole::Client))
{
}

WebSocket::~WebSocket()
{
    FFI::websocket_rust_codec_free(m_codec);
}

void WebSocket::start()
{
    VERIFY(m_state == InternalState::NotStarted);

    m_impl->on_connection_error = [this] {
        if (ready_state() == ReadyState::Closed)
            return;
        if (m_state == InternalState::Closing) {
            // If the connection drops while we are waiting for the server's close frame, check if we actually received
            // one in the last read. If we did, we can consider this a clean close.
            bool was_clean = m_last_close_code != to_underlying(CloseStatusCode::NoStatusReceived);
            set_state(was_clean ? InternalState::Closed : InternalState::Errored);
            if (!was_clean)
                notify_error(Error::ServerClosedSocket);
            notify_close(m_last_close_code, m_last_close_message, was_clean);
            discard_connection();
            return;
        }
        fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), Error::CouldNotEstablishConnection, "Connection error (underlying socket)");
    };
    m_impl->on_connected = [this] {
        if (m_state != InternalState::EstablishingProtocolConnection)
            return;
        set_state(InternalState::Open);
        notify_open();
    };
    m_impl->on_ready_to_read = [this] {
        if (ready_state() == ReadyState::Closed)
            return;
        drain_read();
    };
    set_state(InternalState::EstablishingProtocolConnection);
    m_impl->connect(m_connection);
}

ReadyState WebSocket::ready_state()
{
    switch (m_state) {
    case InternalState::NotStarted:
    case InternalState::EstablishingProtocolConnection:
        return ReadyState::Connecting;
    case InternalState::Open:
        return ReadyState::Open;
    case InternalState::Closing:
        return ReadyState::Closing;
    case InternalState::Closed:
    case InternalState::Errored:
        return ReadyState::Closed;
    }
    VERIFY_NOT_REACHED();
}

void WebSocket::send(Message const& message)
{
    // Calling send on a socket that is not opened is not allowed
    VERIFY(m_state == InternalState::Open);
    auto const& data = message.data();
    FFI::websocket_rust_codec_encode_message_frame(m_codec, message.is_text(), data.data(), data.size(), m_impl.ptr(), send_frame_bytes);
}

void WebSocket::close(u16 code, ByteString const& message)
{
    // Section 3.1: close(code, reason): https://websockets.spec.whatwg.org/#the-websocket-interface
    switch (m_state) {
    case InternalState::Closed:
    case InternalState::Closing:
        // "If this’s ready state is CLOSING (2) or CLOSED (3)
        // Do nothing."
        break;
    case InternalState::NotStarted:
    case InternalState::EstablishingProtocolConnection:
        // "If the WebSocket connection is not yet established [WSP]
        // Fail the WebSocket connection and set this’s ready state to CLOSING (2)."
        set_state(InternalState::Closing);
        fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), Error::CouldNotEstablishConnection, "Closing connection that's not yet established");
        break;
    case InternalState::Open:
        // "If the WebSocket closing handshake has not yet been started [WSP]
        // Start the WebSocket closing handshake and set this’s ready state to CLOSING (2)."
        FFI::websocket_rust_codec_encode_close_frame(m_codec, code, message.bytes().data(), message.length(), m_impl.ptr(), send_frame_bytes);
        set_state(InternalState::Closing);
        break;
    case InternalState::Errored:
        // "Otherwise
        // Set this’s ready state to CLOSING (2)."
        set_state(InternalState::Closing);
        break;
    }
}

void WebSocket::drain_read()
{
    if (m_impl->eof()) {
        // The connection got closed by the server
        set_state(InternalState::Closed);
        notify_close(m_last_close_code, m_last_close_message, true);
        discard_connection();
        return;
    }

    switch (m_state) {
    case InternalState::NotStarted:
    case InternalState::EstablishingProtocolConnection: {
        auto initializing_bytes = m_impl->read(1024);
        if (!initializing_bytes.is_error())
            dbgln("drain_read() was called on a websocket that isn't opened yet. Read {} bytes from the socket.", initializing_bytes.value().size());
        break;
    }
    case InternalState::Open:
    case InternalState::Closing: {
        auto result = m_impl->read(65536);
        if (result.is_error()) {
            fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), Error::ServerClosedSocket, {});
            return;
        }
        auto bytes = result.release_value();
        FFI::websocket_rust_codec_feed(m_codec, bytes.data(), bytes.size());

        FFI::WebSocketRustFrame frame;
        while (ready_state() != ReadyState::Closed && FFI::websocket_rust_codec_next_frame(m_codec, &frame))
            handle_frame(frame);
        break;
    }
    case InternalState::Closed:
    case InternalState::Errored:
        VERIFY_NOT_REACHED();
    }
}

void WebSocket::handle_frame(FFI::WebSocketRustFrame const& frame)
{
    ReadonlyBytes payload { frame.payload, frame.payload_length };

    switch (frame.kind) {
    case FFI::WebSocketRustFrameKind::Text:
        notify_message(Message(MUST(ByteBuffer::copy(payload)), true));
        break;
    case FFI::WebSocketRustFrameKind::Binary:
        notify_message(Message(MUST(ByteBuffer::copy(payload)), false));
        break;
    case FFI::WebSocketRustFrameKind::Ping:
        // Immediately send a pong frame as a reply, with the given payload.
        if (m_state == InternalState::Open)
            FFI::websocket_rust_codec_encode_pong_frame(m_codec, payload.data(), payload.size(), m_impl.ptr(), send_frame_bytes);
        break;
    case FFI::WebSocketRustFrameKind::Pong:
        // We can safely ignore the pong
        break;
    case FFI::WebSocketRustFrameKind::Close:
        m_last_close_code = frame.close_code;
        m_last_close_message = ByteString(payload);
        close(m_last_close_code, m_last_close_message);
        break;
    case FFI::WebSocketRustFrameKind::ProtocolError:
        fail_connection(to_underlying(CloseStatusCode::ProtocolError), Error::ServerClosedSocket, ByteString(payload));
        break;
    }
}

void WebSocket::fail_connection(u16 close_status_code, Error error_code, ByteString const& reason)
{
    if (!reason.is_empty())
        dbgln("WebSocket: {}", reason);
    set_state(InternalState::Errored);
    notify_error(error_code);
    notify_close(close_status_code, reason, false);
    discard_connection();
}

void WebSocket::discard_connection()
{
    // The transport may be reporting the failure from inside one of its own callbacks, so it is only torn down here
    // and released by the destructor. Its callbacks stay attached and are ignored once the socket is closed.
    m_impl->discard_connection();
}

void WebSocket::notify_open()
{
    if (!on_open)
        return;
    on_open();
}

void WebSocket::notify_close(u16 code, ByteString reason, bool was_clean)
{
    if (!on_close)
        return;
    on_close(code, move(reason), was_clean);
}

void WebSocket::notify_error(Error error)
{
    if (!on_error)
        return;
    on_error(error);
}

void WebSocket::notify_message(Message message)
{
    if (!on_message)
        return;
    on_message(move(message));
}

void WebSocket::set_state(InternalState state)
{
    if (m_state == state)
        return;
    auto old_ready_state = ready_state();
    m_state = state;

    if (state == InternalState::Closing) {
        if (!m_closing_handshake_timer) {
            m_closing_handshake_timer = Core::Timer::create_single_shot(s_closing_handshake_timeout_ms, [this] {
                if (m_state != InternalState::Closing)
                    return;
                fail_connection(to_underlying(CloseStatusCode::AbnormalClosure), Error::ServerClosedSocket, "Timed out waiting for the peer's close frame");
            });
        } else {
            m_closing_handshake_timer->restart(s_closing_handshake_timeout_ms);
        }
    } else if (m_closing_handshake_timer) {
        m_closing_handshake_timer->stop();
    }

    auto new_ready_state = ready_state();
    if (old_ready_state != new_ready_state) {
        if (on_ready_state_change)
            on_ready_state_change(ready_state());
    }
}

}
