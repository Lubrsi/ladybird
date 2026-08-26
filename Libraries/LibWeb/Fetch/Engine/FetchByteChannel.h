/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
#include <LibWeb/Export.h>

namespace Web::Fetch::Engine {

// Invoked outside the channel lock, possibly from the peer's thread; holds no GC state.
class WEB_API FetchByteChannelConsumerSink : public AtomicRefCounted<FetchByteChannelConsumerSink> {
public:
    virtual ~FetchByteChannelConsumerSink() = default;

    // The ticket names the arm_waiter() call this wake answers; a stale wake is rejected.
    virtual void wake(u64 ticket) = 0;
};

class WEB_API FetchByteChannelProducerSink : public AtomicRefCounted<FetchByteChannelProducerSink> {
public:
    virtual ~FetchByteChannelProducerSink() = default;

    // Grants the transport permission to deliver this many further bytes, as Requests::Request does.
    virtual void grant_credit(u64 byte_count) = 0;

    virtual void consumer_cancelled() = 0;
};

// The buffer HTTP-network fetch keeps between the network layer and a response body's stream; credit grants are
// its suspend and resume.
// https://fetch.spec.whatwg.org/#ref-for-connection%E2%91%A2
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

    // The error a consumer turns into an exception in its own realm.
    struct Error {
        ByteBuffer message;
    };

    struct TakenBytes {
        Core::ImmutableBytes chunk;
        size_t offset { 0 };
        size_t length { 0 };

        [[nodiscard]] ReadonlyBytes bytes() const LIFETIME_BOUND { return chunk.bytes().slice(offset, length); }
    };

    // Snapshotted atomically with the removal of the bytes.
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

    // The first of close() and error() wins; cancel() also overrides a Closed channel with undrained bytes.
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
        // Buffered bytes neither fire nor consume the waiter; only a close, error or cancellation does.
        TerminalOnly,
    };

    // Returns the ticket the wake will carry; a wake already due fires before this returns, outside the lock.
    u64 arm_waiter(WakeOn = WakeOn::BytesOrTerminal);
    void disarm_waiter();

private:
    FetchByteChannel(Watermarks, NonnullRefPtr<FetchByteChannelConsumerSink>, NonnullRefPtr<FetchByteChannelProducerSink>);

    struct BufferedChunk {
        Core::ImmutableBytes bytes;
        size_t offset { 0 };
    };

    // Sink calls decided under the lock and made after it is released, in order.
    struct Wake {
        u64 ticket { 0 };
    };
    struct GrantCredit {
        u64 byte_count { 0 };
    };
    struct ConsumerCancelled { };
    using Notification = Variant<Wake, GrantCredit, ConsumerCancelled>;
    using PendingNotifications = Vector<Notification, 2>;

    [[nodiscard]] TakeResult take_locked(u64 limit, PendingNotifications&);
    [[nodiscard]] TerminateResult terminate_locked(State, Optional<Error>, PendingNotifications&);
    void wake_armed_waiter_locked(PendingNotifications&);
    void dispatch(PendingNotifications const&);

    Watermarks const m_watermarks;
    NonnullRefPtr<FetchByteChannelConsumerSink> const m_consumer_sink;
    NonnullRefPtr<FetchByteChannelProducerSink> const m_producer_sink;

    mutable Mutex m_mutex;
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
