/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/AtomicRefCounted.h>
#include <AK/RefPtr.h>
#include <LibSync/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Fetching/FetchByteChannel.h>

namespace Web::Fetch::Fetching {

// A bounded engine-internal fan-out over one FetchByteChannel, for bodies whose stream was
// never materialized (a materialized stream clones through the genuine Streams tee). The
// coordinator is the source's sole destructive consumer, with the Streams tee's topology:
// either branch's demand drains the source, every drained byte reaches both non-cancelled
// branches — a slow branch spools in its own channel — and the source is cancelled only once
// both branches are. A source terminal settles both branches and releases the coordinator's
// channel references, so an unread tee cannot outlive its delivery.
//
// "Demand" is branch buffer capacity — each branch starts with one high watermark of it, not a
// byte stream's zero pull demand — so the fan-out may run one bounded stage ahead of any
// reader. It must not directly back JS-visible lazy tee branches until pull demand is separated
// from capacity credit.
//
// Holds only sendable state under its mutex; all channel calls happen outside it.
class WEB_API FetchByteChannelTee final : public AtomicRefCounted<FetchByteChannelTee> {
public:
    [[nodiscard]] static NonnullRefPtr<FetchByteChannelTee> create(FetchByteChannel::Watermarks branch_watermarks, NonnullRefPtr<FetchByteChannelConsumerSink> first_branch_sink, NonnullRefPtr<FetchByteChannelConsumerSink> second_branch_sink);

    // Construct the source channel with this sink, then attach the channel; the coordinator
    // starts draining at attachment.
    [[nodiscard]] NonnullRefPtr<FetchByteChannelConsumerSink> create_source_sink();
    void attach_source(NonnullRefPtr<FetchByteChannel>);

    // Valid until the tee settles; wire branches to their consumers at creation.
    [[nodiscard]] NonnullRefPtr<FetchByteChannel> first_branch() const;
    [[nodiscard]] NonnullRefPtr<FetchByteChannel> second_branch() const;

private:
    friend class TeeSourceSink;
    friend class TeeBranchSink;

    FetchByteChannelTee() = default;

    void pump();
    void pump_once();
    static void deliver(Vector<FetchByteChannel::TakenBytes> const&, Vector<NonnullRefPtr<FetchByteChannel>, 2> const&);
    void on_branch_credit(size_t branch_index, u64 byte_count);
    void on_branch_cancelled(size_t branch_index);
    void detach_locked();

    struct Branch {
        RefPtr<FetchByteChannel> channel;
        u64 demand { 0 };
        bool cancelled { false };
    };

    mutable Sync::Mutex m_mutex;
    RefPtr<FetchByteChannel> m_source;
    Array<Branch, 2> m_branches;
    bool m_pumping { false };
    bool m_pump_again { false };
    bool m_detached { false };
};

}
