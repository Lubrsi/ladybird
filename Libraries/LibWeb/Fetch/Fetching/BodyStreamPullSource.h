/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/EventLoop.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Fetch/Fetching/FetchByteChannel.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

class BodyStreamPullSource;

// The sendable wake sink the channel holds: a loop reference and a registry token, no GC state.
// The consumer loop's wake registry roots the pull source only while an armed wake is owed, so
// an unread response cannot pin its stream graph through the channel.
class BodyStreamWakeSink final : public FetchByteChannelConsumerSink {
public:
    static NonnullRefPtr<BodyStreamWakeSink> create();

    u64 registry_token() const { return m_registry_token; }

    virtual void wake(u64 ticket) override;

private:
    explicit BodyStreamWakeSink(u64 registry_token);

    NonnullRefPtr<Core::WeakEventLoopReference> m_loop;
    u64 const m_registry_token { 0 };
};

// The consumer half of a streamed response body: the response stream's real pull algorithm.
// A pull arms the channel's waiter and returns a pending promise; the channel's wake posts
// back to this event loop, where a queued fetch task pulls the buffered bytes into the byte
// controller and settles that promise.
class BodyStreamPullSource final : public JS::Cell {
    GC_CELL(BodyStreamPullSource, JS::Cell);
    GC_DECLARE_ALLOCATOR(BodyStreamPullSource);

public:
    BodyStreamPullSource(GC::Ref<Infrastructure::FetchParams const>, GC::Ref<Streams::ReadableStream>, NonnullRefPtr<FetchByteChannel>, BodyStreamWakeSink const&);
    virtual ~BodyStreamPullSource() override;

    GC::Ref<WebIDL::Promise> pull(JS::Realm&);
    GC::Ref<WebIDL::Promise> cancel(JS::Realm&, JS::Value reason);

    enum class WakeResult : u8 {
        Consumed,
        StillAwaited,
    };
    WakeResult on_wake(u64 ticket);
    void handle_network_settled();

private:
    virtual void visit_edges(Visitor& visitor) override;

    void run_pull_task();

    GC::Ref<Infrastructure::FetchParams const> m_fetch_params;
    GC::Ref<Streams::ReadableStream> m_stream;
    NonnullRefPtr<FetchByteChannel> m_channel;
    u64 const m_registry_token { 0 };

    GC::Ptr<WebIDL::Promise> m_pending_pull_promise;
    u64 m_expected_ticket { 0 };

    // A detached source never arms the channel again: re-arming against a terminal channel would
    // spin an instant wake/settle cycle per read, so later pulls stay pending like a stopped
    // fetch's stream always has.
    bool m_detached { false };
};

}
