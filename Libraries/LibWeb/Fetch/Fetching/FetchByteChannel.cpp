/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Fetching/FetchByteChannel.h>

namespace Web::Fetch::Fetching {

NonnullRefPtr<FetchByteChannel> FetchByteChannel::create(Watermarks watermarks, NonnullRefPtr<FetchByteChannelConsumerSink> consumer_sink, NonnullRefPtr<FetchByteChannelProducerSink> producer_sink)
{
    return adopt_ref(*new FetchByteChannel(watermarks, move(consumer_sink), move(producer_sink)));
}

FetchByteChannel::FetchByteChannel(Watermarks watermarks, NonnullRefPtr<FetchByteChannelConsumerSink> consumer_sink, NonnullRefPtr<FetchByteChannelProducerSink> producer_sink)
    : m_watermarks(watermarks)
    , m_consumer_sink(move(consumer_sink))
    , m_producer_sink(move(producer_sink))
    , m_outstanding_transport_credit(watermarks.high)
{
    VERIFY(m_watermarks.low <= m_watermarks.high);
}

// Every notification is selected under the mutex, moved out, and invoked here after unlocking;
// a sink may reenter the channel without deadlocking.
void FetchByteChannel::dispatch(PendingNotifications const& notifications)
{
    if (notifications.wake_ticket.has_value())
        m_consumer_sink->wake(*notifications.wake_ticket);
    if (notifications.credit_grant.has_value())
        m_producer_sink->grant_credit(*notifications.credit_grant);
    if (notifications.consumer_cancelled)
        m_producer_sink->consumer_cancelled();
}

Optional<u64> FetchByteChannel::consume_armed_waiter_locked()
{
    if (!m_waiter_armed)
        return {};
    m_waiter_armed = false;
    return m_armed_ticket;
}

// This implements the buffer-filling steps of HTTP-network fetch's transmission loop.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
FetchByteChannel::WriteResult FetchByteChannel::write(Core::ImmutableBytes bytes)
{
    PendingNotifications notifications;
    {
        Sync::MutexLocker locker { m_mutex };
        if (m_state != State::Open)
            return WriteResult::RejectedByTerminal;
        if (bytes.is_empty())
            return WriteResult::Accepted;

        auto size = bytes.size();

        // 7. Append bytes to buffer.
        m_buffered_byte_count += size;
        m_chunks.append({ move(bytes), 0 });

        // 8. If the size of buffer is larger than an upper limit chosen by the user agent, ask
        //    the user agent to suspend the ongoing fetch.
        // AD-HOC: enforced ahead of time through transport credit — the transport was granted
        // permission for at most a high watermark's worth of undrained bytes, so exhausting the
        // credit consumed here is what suspends delivery (see the resume step in take_locked()).
        m_outstanding_transport_credit -= min(m_outstanding_transport_credit, size);
        if (m_armed_wake_on == WakeOn::BytesOrTerminal)
            notifications.wake_ticket = consume_armed_waiter_locked();
    }
    dispatch(notifications);
    return WriteResult::Accepted;
}

FetchByteChannel::TerminateResult FetchByteChannel::terminate_locked(State terminal_state, Optional<Error> error, PendingNotifications& notifications)
{
    VERIFY(terminal_state != State::Open);
    if (m_state != State::Open)
        return TerminateResult::AlreadyTerminated;

    m_state = terminal_state;
    if (terminal_state == State::Errored) {
        m_chunks.clear();
        m_buffered_byte_count = 0;
        m_error = move(error);
    }
    notifications.wake_ticket = consume_armed_waiter_locked();
    return TerminateResult::Terminated;
}

FetchByteChannel::TerminateResult FetchByteChannel::close()
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        Sync::MutexLocker locker { m_mutex };
        result = terminate_locked(State::Closed, {}, notifications);
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TerminateResult FetchByteChannel::error(Error error)
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        Sync::MutexLocker locker { m_mutex };
        result = terminate_locked(State::Errored, move(error), notifications);
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TerminateResult FetchByteChannel::cancel()
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        Sync::MutexLocker locker { m_mutex };
        // A Closed channel still owns its undrained buffer, and Streams cancellation must be
        // able to discard those backing bytes even after network EOF.
        if (m_state == State::Open || (m_state == State::Closed && !m_chunks.is_empty())) {
            m_state = State::ConsumerCancelled;
            m_chunks.clear();
            m_buffered_byte_count = 0;
            notifications.wake_ticket = consume_armed_waiter_locked();
            notifications.consumer_cancelled = true;
            result = TerminateResult::Terminated;
        }
    }
    dispatch(notifications);
    return result;
}

// This implements the buffer-draining half of the pullAlgorithm in HTTP-network fetch.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
FetchByteChannel::TakeResult FetchByteChannel::take_locked(u64 limit, PendingNotifications& notifications)
{
    TakeResult result;

    u64 remaining = limit;
    while (remaining > 0 && !m_chunks.is_empty()) {
        auto& front = m_chunks.first();
        auto available = front.bytes.size() - front.offset;
        auto length = min<u64>(available, remaining);

        result.taken.append({ front.bytes, front.offset, static_cast<size_t>(length) });
        remaining -= length;
        m_buffered_byte_count -= length;

        if (length == available) {
            m_chunks.take_first();
        } else {
            front.offset += length;
        }
    }

    result.state = m_state;
    result.error = m_error;
    result.buffered_byte_count = m_buffered_byte_count;

    // 1. If the size of buffer is smaller than a lower limit chosen by the user agent and the
    //    ongoing fetch is suspended, resume the fetch.
    // AD-HOC: "suspended" is represented as exhausted transport credit, because a pause request
    // cannot stop LibRequests' current notifier read loop and file-backed bodies bypass the pause
    // path entirely; resuming is granting fresh credit, batched so the transport is re-credited
    // only once the buffer drains below the low watermark.
    if (m_state == State::Open && m_buffered_byte_count < m_watermarks.low) {
        auto target = m_watermarks.high - m_buffered_byte_count;
        if (target > m_outstanding_transport_credit) {
            notifications.credit_grant = target - m_outstanding_transport_credit;
            m_outstanding_transport_credit = target;
        }
    }

    return result;
}

FetchByteChannel::TakeResult FetchByteChannel::take_up_to(u64 limit)
{
    PendingNotifications notifications;
    TakeResult result;
    {
        Sync::MutexLocker locker { m_mutex };
        result = take_locked(limit, notifications);
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TakeResult FetchByteChannel::take_all()
{
    PendingNotifications notifications;
    TakeResult result;
    {
        Sync::MutexLocker locker { m_mutex };
        result = take_locked(NumericLimits<u64>::max(), notifications);
    }
    dispatch(notifications);
    return result;
}

u64 FetchByteChannel::arm_waiter(WakeOn wake_on)
{
    PendingNotifications notifications;
    u64 ticket = 0;
    {
        Sync::MutexLocker locker { m_mutex };
        ticket = m_next_ticket++;
        if (m_state != State::Open || (wake_on == WakeOn::BytesOrTerminal && !m_chunks.is_empty())) {
            notifications.wake_ticket = ticket;
        } else {
            m_waiter_armed = true;
            m_armed_wake_on = wake_on;
            m_armed_ticket = ticket;
        }
    }
    dispatch(notifications);
    return ticket;
}

void FetchByteChannel::disarm_waiter()
{
    Sync::MutexLocker locker { m_mutex };
    m_waiter_armed = false;
}

}
