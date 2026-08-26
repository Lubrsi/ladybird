/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Engine/FetchByteChannel.h>

namespace Web::Fetch::Engine {

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

// A sink may reenter the channel.
void FetchByteChannel::dispatch(PendingNotifications const& notifications)
{
    for (auto const& notification : notifications) {
        notification.visit(
            [&](Wake const& wake) { m_consumer_sink->wake(wake.ticket); },
            [&](GrantCredit const& grant) { m_producer_sink->grant_credit(grant.byte_count); },
            [&](ConsumerCancelled) { m_producer_sink->consumer_cancelled(); },
            [&](Misbehaved const& misbehaved) { m_producer_sink->misbehaved(misbehaved.reason); });
    }
}

void FetchByteChannelProducerSink::misbehaved(StringView reason)
{
    dbgln("FetchByteChannel: The transport misbehaved: {}", reason);
    VERIFY_NOT_REACHED();
}

void FetchByteChannel::wake_armed_waiter_locked(PendingNotifications& notifications)
{
    if (!m_waiter_armed)
        return;
    m_waiter_armed = false;
    notifications.append(Wake { m_armed_ticket });
}

// This implements the buffer-filling steps of HTTP-network fetch's transmission loop.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
FetchByteChannel::WriteResult FetchByteChannel::write(Core::ImmutableBytes bytes)
{
    PendingNotifications notifications;
    auto result = WriteResult::Rejected;
    {
        MutexLocker locker { m_mutex };
        if (m_transport_ended) {
            notifications.append(Misbehaved { "The transport delivered after ending the body"sv });
        } else if (m_state != State::Open) {
            // The consumer cancelled and the transport has not heard yet.
        } else if (bytes.size() > m_outstanding_transport_credit) {
            notifications.append(Misbehaved { "The transport delivered more than it was granted"sv });
        } else {
            result = WriteResult::Accepted;
            auto size = bytes.size();
            if (size > 0) {
                // 7. Append bytes to buffer.
                m_buffered_byte_count += size;
                m_chunks.append({ move(bytes), 0 });

                // 8. If the size of buffer is larger than an upper limit chosen by the user agent, ask
                //    the user agent to suspend the ongoing fetch.
                // Spent credit is the suspension: the transport delivers only what it was granted.
                m_outstanding_transport_credit -= size;
                if (m_armed_wake_on == WakeOn::BytesOrTerminal)
                    wake_armed_waiter_locked(notifications);
            }
        }
    }
    dispatch(notifications);
    return result;
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
    wake_armed_waiter_locked(notifications);
    return TerminateResult::Terminated;
}

FetchByteChannel::TerminateResult FetchByteChannel::close()
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        MutexLocker locker { m_mutex };
        if (m_transport_ended) {
            notifications.append(Misbehaved { "The transport ended the body twice"sv });
        } else {
            m_transport_ended = true;
            result = terminate_locked(State::Closed, {}, notifications);
        }
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TerminateResult FetchByteChannel::error(Error error)
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        MutexLocker locker { m_mutex };
        if (m_transport_ended) {
            notifications.append(Misbehaved { "The transport ended the body twice"sv });
        } else {
            m_transport_ended = true;
            result = terminate_locked(State::Errored, move(error), notifications);
        }
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TerminateResult FetchByteChannel::cancel()
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        MutexLocker locker { m_mutex };
        // A Closed channel keeps its undrained buffer, which a Streams cancellation may still discard.
        if (m_state == State::Open || (m_state == State::Closed && !m_chunks.is_empty())) {
            m_state = State::ConsumerCancelled;
            m_chunks.clear();
            m_buffered_byte_count = 0;
            wake_armed_waiter_locked(notifications);
            notifications.append(ConsumerCancelled {});
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
    if (m_state == State::Open && m_buffered_byte_count < m_watermarks.low) {
        auto target = m_watermarks.high - m_buffered_byte_count;
        if (target > m_outstanding_transport_credit) {
            notifications.append(GrantCredit { target - m_outstanding_transport_credit });
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
        MutexLocker locker { m_mutex };
        result = take_locked(limit, notifications);
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TakeResult FetchByteChannel::take_all()
{
    return take_up_to(NumericLimits<u64>::max());
}

WakeTicket FetchByteChannel::arm_waiter(WakeOn wake_on)
{
    PendingNotifications notifications;
    WakeTicket ticket { 0 };
    {
        MutexLocker locker { m_mutex };
        VERIFY(!m_waiter_armed);
        ticket = WakeTicket { m_next_ticket++ };
        if (m_state != State::Open || (wake_on == WakeOn::BytesOrTerminal && !m_chunks.is_empty())) {
            notifications.append(Wake { ticket });
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
    MutexLocker locker { m_mutex };
    m_waiter_armed = false;
}

}
