/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
#include <LibSync/Mutex.h>
#include <LibWeb/Export.h>

namespace Web::Fetch::Fetching {

// Sinks are invoked outside the channel lock, potentially from the peer's thread once the
// producer and consumer live on different threads. Implementations route to their own event
// loop and must not hold GC state reachable from the invoking thread.
class WEB_API FetchByteChannelConsumerSink : public AtomicRefCounted<FetchByteChannelConsumerSink> {
public:
    virtual ~FetchByteChannelConsumerSink() = default;

    // The ticket identifies which arming this wake answers; a consumer that re-armed since can
    // reject a stale wake without dropping the pull promise its own arming must settle.
    virtual void wake(u64 ticket) = 0;
};

class WEB_API FetchByteChannelProducerSink : public AtomicRefCounted<FetchByteChannelProducerSink> {
public:
    virtual ~FetchByteChannelProducerSink() = default;

    // Grants the transport permission to deliver this many further bytes
    // (Requests::Request::resume_body_delivery_up_to() semantics).
    virtual void grant_credit(u64 byte_count) = 0;

    virtual void consumer_cancelled() = 0;
};

// The internal buffer between the network layer and a response body's stream, implementing the
// buffer the specification describes in HTTP-network fetch:
// https://fetch.spec.whatwg.org/#ref-for-connection%E2%91%A2 ("This represents an internal
// buffer inside the network layer of the user agent") — the watermarks are the specification's
// UA-chosen lower/upper limits, and credit grants are its suspend/resume of the ongoing fetch.
class WEB_API FetchByteChannel final : public AtomicRefCounted<FetchByteChannel> {
public:
    struct Watermarks {
        u64 low { 0 };
        u64 high { 0 };
    };

    enum class State : u8 {
        Open,
        Closed,
        Errored,
        ConsumerCancelled,
    };

    // Sendable terminal error; the consumer materializes a realm-local exception from it.
    struct Error {
        ByteBuffer message;
    };

    struct TakenBytes {
        Core::ImmutableBytes chunk;
        size_t offset { 0 };
        size_t length { 0 };

        [[nodiscard]] ReadonlyBytes bytes() const LIFETIME_BOUND { return chunk.bytes().slice(offset, length); }
    };

    // The state fields are a snapshot taken atomically with the removal of the bytes, so the
    // consumer can act on a terminal without a second racy query.
    struct TakeResult {
        Vector<TakenBytes> taken;
        State state { State::Open };
        Optional<Error> error;
        u64 buffered_byte_count { 0 };
    };

    [[nodiscard]] static NonnullRefPtr<FetchByteChannel> create(Watermarks, NonnullRefPtr<FetchByteChannelConsumerSink>, NonnullRefPtr<FetchByteChannelProducerSink>);

    // The credit the producer should hand the transport before the first write.
    [[nodiscard]] u64 initial_credit() const { return m_watermarks.high; }

    enum class WriteResult : u8 {
        Accepted,
        RejectedByTerminal,
    };

    // Producer terminals are first-wins; consumer cancellation additionally supersedes a Closed
    // channel whose buffer is undrained, discarding it.
    enum class TerminateResult : u8 {
        Terminated,
        AlreadyTerminated,
    };

    // Producer side.
    WriteResult write(Core::ImmutableBytes);
    TerminateResult close();
    TerminateResult error(Error);

    // Consumer side.
    [[nodiscard]] TakeResult take_up_to(u64 limit);
    [[nodiscard]] TakeResult take_all();
    TerminateResult cancel();

    enum class WakeOn : u8 {
        BytesOrTerminal,
        // Buffered bytes neither fire nor consume the waiter; only a terminal does. For a
        // consumer that currently has no demand but must still observe close/error/cancel.
        TerminalOnly,
    };

    // Arms the one-shot waiter and returns the ticket its wake will carry. If a wake-worthy
    // condition is already pending, the wake fires before this returns (outside the lock).
    u64 arm_waiter(WakeOn = WakeOn::BytesOrTerminal);
    void disarm_waiter();

private:
    FetchByteChannel(Watermarks, NonnullRefPtr<FetchByteChannelConsumerSink>, NonnullRefPtr<FetchByteChannelProducerSink>);

    struct BufferedChunk {
        Core::ImmutableBytes bytes;
        size_t offset { 0 };
    };

    struct PendingNotifications {
        Optional<u64> wake_ticket;
        Optional<u64> credit_grant;
        bool consumer_cancelled { false };
    };

    [[nodiscard]] TakeResult take_locked(u64 limit, PendingNotifications&);
    [[nodiscard]] TerminateResult terminate_locked(State, Optional<Error>, PendingNotifications&);
    [[nodiscard]] Optional<u64> consume_armed_waiter_locked();
    void dispatch(PendingNotifications const&);

    Watermarks const m_watermarks;
    NonnullRefPtr<FetchByteChannelConsumerSink> const m_consumer_sink;
    NonnullRefPtr<FetchByteChannelProducerSink> const m_producer_sink;

    mutable Sync::Mutex m_mutex;
    Vector<BufferedChunk> m_chunks;
    u64 m_buffered_byte_count { 0 };
    u64 m_outstanding_transport_credit { 0 };
    State m_state { State::Open };
    Optional<Error> m_error;
    bool m_waiter_armed { false };
    WakeOn m_armed_wake_on { WakeOn::BytesOrTerminal };
    u64 m_armed_ticket { 0 };
    u64 m_next_ticket { 1 };
};

}
