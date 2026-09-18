/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <LibCore/EventLoop.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

class BodyStreamPullSource;

// Never reused within a thread.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, BodyStreamWakeToken);

// A loop reference and a registry token, no GC state; the registry roots the pull source only while a wake is owed.
class BodyStreamWakeSink final : public Engine::FetchByteChannelConsumerSink {
public:
    static NonnullRefPtr<BodyStreamWakeSink> create();

    BodyStreamWakeToken registry_token() const { return m_registry_token; }

    virtual void wake(Engine::WakeTicket) override;

private:
    explicit BodyStreamWakeSink(BodyStreamWakeToken);

    NonnullRefPtr<Core::WeakEventLoopReference> m_loop;
    BodyStreamWakeToken const m_registry_token { 0 };
};

// The response stream's pull algorithm: a pull arms the channel's waiter, and the wake's queued fetch task pulls
// the buffered bytes into the controller.
class BodyStreamPullSource final : public JS::Cell {
    GC_CELL(BodyStreamPullSource, JS::Cell);
    GC_DECLARE_ALLOCATOR(BodyStreamPullSource);

public:
    BodyStreamPullSource(GC::Ref<Infrastructure::FetchParams const>, GC::Ref<Streams::ReadableStream>, NonnullRefPtr<Engine::FetchByteChannel>, BodyStreamWakeSink const&);
    virtual ~BodyStreamPullSource() override;

    GC::Ref<WebIDL::Promise> pull(JS::Realm&);
    GC::Ref<WebIDL::Promise> cancel(JS::Realm&, JS::Value reason);

    enum class WakeResult : u8 {
        Consumed,
        StillAwaited,
    };
    WakeResult on_wake(Engine::WakeTicket);
    void handle_network_settled();
    void deliver_while_paused();

private:
    virtual void visit_edges(Visitor& visitor) override;

    void run_pull_task();
    void drain_into_stream();

    GC::Ref<Infrastructure::FetchParams const> m_fetch_params;
    GC::Ref<Streams::ReadableStream> m_stream;
    NonnullRefPtr<Engine::FetchByteChannel> m_channel;
    BodyStreamWakeToken const m_registry_token { 0 };

    GC::Ptr<WebIDL::Promise> m_pending_pull_promise;
    Engine::WakeTicket m_expected_ticket { 0 };

    // Once the channel has ended, later pulls stay pending, as a stopped fetch's stream always has.
    bool m_detached { false };
};

}
