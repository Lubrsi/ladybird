/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Fetching/FetchByteChannelTee.h>

namespace Web::Fetch::Fetching {

class TeeSourceSink final : public FetchByteChannelConsumerSink {
public:
    explicit TeeSourceSink(NonnullRefPtr<FetchByteChannelTee> tee)
        : m_tee(move(tee))
    {
    }

    // The coordinator is the sole armer of the source's waiter, so no ticket check is needed.
    virtual void wake(u64) override { m_tee->pump(); }

private:
    NonnullRefPtr<FetchByteChannelTee> m_tee;
};

class TeeBranchSink final : public FetchByteChannelProducerSink {
public:
    TeeBranchSink(NonnullRefPtr<FetchByteChannelTee> tee, size_t branch_index)
        : m_tee(move(tee))
        , m_branch_index(branch_index)
    {
    }

    virtual void grant_credit(u64 byte_count) override { m_tee->on_branch_credit(m_branch_index, byte_count); }
    virtual void consumer_cancelled() override { m_tee->on_branch_cancelled(m_branch_index); }

private:
    NonnullRefPtr<FetchByteChannelTee> m_tee;
    size_t const m_branch_index;
};

NonnullRefPtr<FetchByteChannelTee> FetchByteChannelTee::create(FetchByteChannel::Watermarks branch_watermarks, NonnullRefPtr<FetchByteChannelConsumerSink> first_branch_sink, NonnullRefPtr<FetchByteChannelConsumerSink> second_branch_sink)
{
    auto tee = adopt_ref(*new FetchByteChannelTee);
    tee->m_branches[0].channel = FetchByteChannel::create(branch_watermarks, move(first_branch_sink), adopt_ref(*new TeeBranchSink(*tee, 0)));
    tee->m_branches[1].channel = FetchByteChannel::create(branch_watermarks, move(second_branch_sink), adopt_ref(*new TeeBranchSink(*tee, 1)));
    for (auto& branch : tee->m_branches)
        branch.demand = branch.channel->initial_credit();
    return tee;
}

NonnullRefPtr<FetchByteChannelConsumerSink> FetchByteChannelTee::create_source_sink()
{
    return adopt_ref(*new TeeSourceSink(*this));
}

void FetchByteChannelTee::attach_source(NonnullRefPtr<FetchByteChannel> source)
{
    {
        Sync::MutexLocker locker { m_mutex };
        VERIFY(!m_source && !m_detached);
        m_source = move(source);
    }
    pump();
}

NonnullRefPtr<FetchByteChannel> FetchByteChannelTee::first_branch() const
{
    Sync::MutexLocker locker { m_mutex };
    return *m_branches[0].channel;
}

NonnullRefPtr<FetchByteChannel> FetchByteChannelTee::second_branch() const
{
    Sync::MutexLocker locker { m_mutex };
    return *m_branches[1].channel;
}

// Breaks the reference ring (channel → sink → tee → channel) once the source terminal has
// propagated; the branches live on through their consumers' references.
void FetchByteChannelTee::detach_locked()
{
    m_detached = true;
    m_source = nullptr;
    for (auto& branch : m_branches)
        branch.channel = nullptr;
}

void FetchByteChannelTee::pump()
{
    {
        Sync::MutexLocker locker { m_mutex };
        if (m_pumping) {
            m_pump_again = true;
            return;
        }
        m_pumping = true;
    }
    for (;;) {
        pump_once();
        Sync::MutexLocker locker { m_mutex };
        if (!m_pump_again) {
            m_pumping = false;
            return;
        }
        m_pump_again = false;
    }
}

void FetchByteChannelTee::deliver(Vector<FetchByteChannel::TakenBytes> const& taken, Vector<NonnullRefPtr<FetchByteChannel>, 2> const& branches)
{
    for (auto const& slice : taken) {
        auto chunk = slice.offset == 0 && slice.length == slice.chunk.size()
            ? slice.chunk
            : MUST(Core::ImmutableBytes::copy(slice.bytes()));
        for (auto const& branch : branches)
            (void)branch->write(chunk);
    }
}

void FetchByteChannelTee::pump_once()
{
    RefPtr<FetchByteChannel> source;
    u64 demand = 0;
    {
        Sync::MutexLocker locker { m_mutex };
        source = m_source;
        for (auto const& branch : m_branches) {
            if (!branch.cancelled)
                demand = max(demand, branch.demand);
        }
    }
    if (!source)
        return;

    auto result = source->take_up_to(demand);

    u64 taken_byte_count = 0;
    for (auto const& slice : result.taken)
        taken_byte_count += slice.length;

    Vector<NonnullRefPtr<FetchByteChannel>, 2> live_branches;
    {
        Sync::MutexLocker locker { m_mutex };
        if (m_detached)
            return;
        for (auto& branch : m_branches) {
            if (branch.cancelled)
                continue;
            branch.demand -= min(branch.demand, taken_byte_count);
            live_branches.append(*branch.channel);
        }
    }

    deliver(result.taken, live_branches);

    switch (result.state) {
    case FetchByteChannel::State::Open: {
        bool has_demand = false;
        {
            Sync::MutexLocker locker { m_mutex };
            if (m_detached)
                return;
            for (auto const& branch : m_branches)
                has_demand |= !branch.cancelled && branch.demand > 0;
        }
        // With no branch demand the source may buffer up to its credit bound; only a terminal
        // needs to wake the coordinator then, or arming against the buffered bytes would spin.
        (void)source->arm_waiter(has_demand ? FetchByteChannel::WakeOn::BytesOrTerminal : FetchByteChannel::WakeOn::TerminalOnly);
        return;
    }
    case FetchByteChannel::State::Closed:
        // Nothing further is coming, so residual source bytes become branch spool immediately —
        // chunks are shared, and holding them in the source would keep the ring alive for a
        // delivery nobody reads.
        if (result.buffered_byte_count > 0)
            deliver(source->take_all().taken, live_branches);
        for (auto const& branch : live_branches)
            (void)branch->close();
        break;
    case FetchByteChannel::State::Errored:
        for (auto const& branch : live_branches) {
            auto message = result.error.has_value() ? MUST(ByteBuffer::copy(result.error->message)) : ByteBuffer {};
            (void)branch->error({ .message = move(message) });
        }
        break;
    case FetchByteChannel::State::ConsumerCancelled:
        // The producer side revoked the delivery out from under the tee (stop teardown); both
        // branches settle the way a directly-cancelled channel would.
        for (auto const& branch : live_branches)
            (void)branch->cancel();
        break;
    }

    Sync::MutexLocker locker { m_mutex };
    detach_locked();
}

void FetchByteChannelTee::on_branch_credit(size_t branch_index, u64 byte_count)
{
    {
        Sync::MutexLocker locker { m_mutex };
        if (m_detached || m_branches[branch_index].cancelled)
            return;
        m_branches[branch_index].demand += byte_count;
    }
    pump();
}

void FetchByteChannelTee::on_branch_cancelled(size_t branch_index)
{
    RefPtr<FetchByteChannel> source_to_cancel;
    {
        Sync::MutexLocker locker { m_mutex };
        m_branches[branch_index].cancelled = true;
        m_branches[branch_index].demand = 0;
        if (m_branches[0].cancelled && m_branches[1].cancelled) {
            source_to_cancel = m_source;
            detach_locked();
        }
    }
    if (source_to_cancel)
        (void)source_to_cancel->cancel();
}

}
