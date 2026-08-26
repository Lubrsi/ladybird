/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Fetch/Fetching/FetchByteChannel.h>

namespace {

using Web::Fetch::Fetching::FetchByteChannel;
using Web::Fetch::Fetching::FetchByteChannelConsumerSink;
using Web::Fetch::Fetching::FetchByteChannelProducerSink;
using TerminateResult = FetchByteChannel::TerminateResult;
using WriteResult = FetchByteChannel::WriteResult;

struct SinkEvent {
    enum class Kind : u8 {
        Wake,
        CreditGrant,
        ConsumerCancelled,
    };
    Kind kind;
    u64 value { 0 };
};

class RecordingConsumerSink final : public FetchByteChannelConsumerSink {
public:
    explicit RecordingConsumerSink(Vector<SinkEvent>& events)
        : m_events(events)
    {
    }

    virtual void wake(u64 ticket) override
    {
        m_events.append({ SinkEvent::Kind::Wake, ticket });
        if (on_wake)
            on_wake(ticket);
    }

    Function<void(u64)> on_wake;

private:
    Vector<SinkEvent>& m_events;
};

class RecordingProducerSink final : public FetchByteChannelProducerSink {
public:
    explicit RecordingProducerSink(Vector<SinkEvent>& events)
        : m_events(events)
    {
    }

    virtual void grant_credit(u64 byte_count) override
    {
        m_events.append({ SinkEvent::Kind::CreditGrant, byte_count });
        if (on_grant_credit)
            on_grant_credit(byte_count);
    }

    virtual void consumer_cancelled() override
    {
        m_events.append({ SinkEvent::Kind::ConsumerCancelled });
    }

    Function<void(u64)> on_grant_credit;

private:
    Vector<SinkEvent>& m_events;
};

struct ChannelHarness {
    explicit ChannelHarness(FetchByteChannel::Watermarks watermarks = { .low = 16, .high = 64 })
        : consumer_sink(adopt_ref(*new RecordingConsumerSink(events)))
        , producer_sink(adopt_ref(*new RecordingProducerSink(events)))
        , channel(FetchByteChannel::create(watermarks, *consumer_sink, *producer_sink))
    {
    }

    Vector<SinkEvent> take_events() { return move(events); }

    Vector<SinkEvent> events;
    NonnullRefPtr<RecordingConsumerSink> consumer_sink;
    NonnullRefPtr<RecordingProducerSink> producer_sink;
    NonnullRefPtr<FetchByteChannel> channel;
};

struct LatestTicketSink final : FetchByteChannelConsumerSink {
    virtual void wake(u64 ticket) override { latest.store(ticket, AK::MemoryOrder::memory_order_release); }
    Atomic<u64> latest { 0 };
};

struct NullProducerSink final : FetchByteChannelProducerSink {
    virtual void grant_credit(u64) override { }
    virtual void consumer_cancelled() override { }
};

Core::ImmutableBytes bytes_of(StringView text)
{
    return MUST(Core::ImmutableBytes::copy(text.bytes()));
}

ByteString flatten(FetchByteChannel::TakeResult const& result)
{
    ByteBuffer flat;
    for (auto const& taken : result.taken)
        flat.append(taken.bytes());
    return ByteString { flat.bytes() };
}

}

TEST_CASE(write_take_ordering_and_chunk_integrity)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("hello "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("byte "sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("channel"sv)), WriteResult::Accepted);

    auto result = harness.channel->take_all();
    EXPECT_EQ(flatten(result), "hello byte channel"sv);
    EXPECT_EQ(result.state, FetchByteChannel::State::Open);
    EXPECT_EQ(result.buffered_byte_count, 0u);
}

TEST_CASE(take_up_to_leaves_correctly_offset_remainder)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("0123456789"sv)), WriteResult::Accepted);

    auto first = harness.channel->take_up_to(4);
    EXPECT_EQ(flatten(first), "0123"sv);
    EXPECT_EQ(first.buffered_byte_count, 6u);

    auto second = harness.channel->take_up_to(3);
    EXPECT_EQ(flatten(second), "456"sv);
    EXPECT_EQ(second.buffered_byte_count, 3u);

    auto rest = harness.channel->take_all();
    EXPECT_EQ(flatten(rest), "789"sv);
    EXPECT_EQ(rest.buffered_byte_count, 0u);
}

TEST_CASE(take_up_to_spanning_multiple_chunks)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("abc"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("def"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("ghi"sv)), WriteResult::Accepted);

    auto result = harness.channel->take_up_to(5);
    EXPECT_EQ(flatten(result), "abcde"sv);
    EXPECT_EQ(result.buffered_byte_count, 4u);
}

TEST_CASE(credit_accounting_with_batched_replenishment)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };
    EXPECT_EQ(harness.channel->initial_credit(), 64u);

    // Writes consume transport credit; nothing is replenished while the buffer sits at or
    // above the low watermark.
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 40)))), WriteResult::Accepted);
    auto result = harness.channel->take_up_to(20);
    EXPECT_EQ(result.buffered_byte_count, 20u);
    EXPECT(harness.take_events().is_empty());

    // Draining below low replenishes back up to a high watermark's worth of
    // buffered-plus-outstanding bytes in one batch.
    result = harness.channel->take_up_to(10);
    EXPECT_EQ(result.buffered_byte_count, 10u);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::CreditGrant);
    EXPECT_EQ(events[0].value, 30u); // 64 high - 10 buffered - 24 outstanding

    // No further grant until the buffer crosses the low watermark again.
    result = harness.channel->take_up_to(1);
    EXPECT_EQ(result.buffered_byte_count, 9u);
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 1u);
}

TEST_CASE(exact_watermark_edge_transitions)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };

    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 64)))), WriteResult::Accepted);
    (void)harness.take_events();

    // Draining to exactly the low watermark does not replenish; below it does.
    auto result = harness.channel->take_up_to(48);
    EXPECT_EQ(result.buffered_byte_count, 16u);
    EXPECT(harness.take_events().is_empty());

    result = harness.channel->take_up_to(1);
    EXPECT_EQ(result.buffered_byte_count, 15u);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::CreditGrant);
    EXPECT_EQ(events[0].value, 49u);
}

TEST_CASE(uncredited_writes_buffer_beyond_the_high_watermark)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };

    // Credit paces the transport but never rejects delivered bytes: a file-backed body
    // (Requests::Request::set_request_body_file()) arrives as one delivery of the whole
    // payload, bypassing credit entirely.
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 200)))), WriteResult::Accepted);
    EXPECT(harness.take_events().is_empty());

    auto result = harness.channel->take_up_to(150);
    EXPECT_EQ(result.buffered_byte_count, 50u);
    EXPECT(harness.take_events().is_empty());

    // Credit replenishment resumes with sane accounting once the backlog drains below low.
    result = harness.channel->take_up_to(45);
    EXPECT_EQ(result.buffered_byte_count, 5u);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::CreditGrant);
    EXPECT_EQ(events[0].value, 59u); // 64 high - 5 buffered - 0 outstanding

    result = harness.channel->take_all();
    EXPECT_EQ(result.taken.size(), 1u);
    EXPECT_EQ(result.buffered_byte_count, 0u);
}

TEST_CASE(armed_waiter_fires_on_write_and_arms_late)
{
    ChannelHarness harness;

    auto ticket = harness.channel->arm_waiter();
    EXPECT(harness.take_events().is_empty());

    EXPECT_EQ(harness.channel->write(bytes_of("x"sv)), WriteResult::Accepted);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
    EXPECT_EQ(events[0].value, ticket);

    // Arming with bytes already pending fires immediately with the fresh ticket.
    auto late_ticket = harness.channel->arm_waiter();
    EXPECT(late_ticket != ticket);
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, late_ticket);
}

TEST_CASE(waiter_is_one_shot_and_disarmable)
{
    ChannelHarness harness;

    (void)harness.channel->arm_waiter();
    EXPECT_EQ(harness.channel->write(bytes_of("a"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->write(bytes_of("b"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.take_events().size(), 1u);

    (void)harness.channel->take_all();
    (void)harness.take_events();

    (void)harness.channel->arm_waiter();
    harness.channel->disarm_waiter();
    EXPECT_EQ(harness.channel->write(bytes_of("c"sv)), WriteResult::Accepted);
    EXPECT(harness.take_events().is_empty());
}

TEST_CASE(producer_terminals_are_first_wins)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    EXPECT_EQ(harness.channel->error({}), TerminateResult::AlreadyTerminated);
    EXPECT_EQ(harness.channel->close(), TerminateResult::AlreadyTerminated);

    // A drained Closed delivery has nothing left to cancel.
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::AlreadyTerminated);

    auto result = harness.channel->take_all();
    EXPECT_EQ(result.state, FetchByteChannel::State::Closed);
}

TEST_CASE(cancellation_supersedes_an_undrained_close)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("still buffered"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);

    // Network EOF leaves the stream readable until the buffer drains, so Streams cancellation
    // must still be able to discard the backing bytes.
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::ConsumerCancelled);

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT_EQ(result.state, FetchByteChannel::State::ConsumerCancelled);
    EXPECT_EQ(result.buffered_byte_count, 0u);

    EXPECT_EQ(harness.channel->cancel(), TerminateResult::AlreadyTerminated);
    EXPECT_EQ(harness.channel->close(), TerminateResult::AlreadyTerminated);
}

TEST_CASE(cancellation_after_error_changes_nothing)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("junk"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->error({}), TerminateResult::Terminated);

    // The error already discarded the buffer and settled the consumer.
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::AlreadyTerminated);
    auto result = harness.channel->take_all();
    EXPECT_EQ(result.state, FetchByteChannel::State::Errored);
}

TEST_CASE(close_preserves_buffered_bytes_until_drained)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("tail"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);

    EXPECT_EQ(harness.channel->write(bytes_of("dropped"sv)), WriteResult::RejectedByTerminal);

    auto result = harness.channel->take_all();
    EXPECT_EQ(flatten(result), "tail"sv);
    EXPECT_EQ(result.state, FetchByteChannel::State::Closed);
    EXPECT_EQ(result.buffered_byte_count, 0u);
}

TEST_CASE(error_discards_buffered_bytes_immediately)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("junk"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->error({ .message = MUST(ByteBuffer::copy("connection reset"sv.bytes())) }), TerminateResult::Terminated);

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT_EQ(result.state, FetchByteChannel::State::Errored);
    EXPECT_EQ(result.buffered_byte_count, 0u);
    EXPECT(result.error.has_value());
    EXPECT_EQ(StringView { result.error->message }, "connection reset"sv);
}

TEST_CASE(terminals_wake_an_armed_waiter)
{
    for (auto terminal : { FetchByteChannel::State::Closed, FetchByteChannel::State::Errored, FetchByteChannel::State::ConsumerCancelled }) {
        ChannelHarness harness;
        auto ticket = harness.channel->arm_waiter();

        switch (terminal) {
        case FetchByteChannel::State::Closed:
            EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
            break;
        case FetchByteChannel::State::Errored:
            EXPECT_EQ(harness.channel->error({}), TerminateResult::Terminated);
            break;
        case FetchByteChannel::State::ConsumerCancelled:
            EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);
            break;
        case FetchByteChannel::State::Open:
            VERIFY_NOT_REACHED();
        }

        auto events = harness.take_events();
        EXPECT(!events.is_empty());
        EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
        EXPECT_EQ(events[0].value, ticket);
    }
}

TEST_CASE(cancellation_notifies_the_producer)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("in flight"sv)), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->cancel(), TerminateResult::Terminated);

    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::ConsumerCancelled);

    auto result = harness.channel->take_all();
    EXPECT(result.taken.is_empty());
    EXPECT_EQ(result.state, FetchByteChannel::State::ConsumerCancelled);
}

TEST_CASE(no_credit_replenishment_after_terminal)
{
    ChannelHarness harness { { .low = 16, .high = 64 } };
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 40)))), WriteResult::Accepted);
    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    (void)harness.take_events();

    auto result = harness.channel->take_all();
    EXPECT_EQ(result.state, FetchByteChannel::State::Closed);
    EXPECT(harness.take_events().is_empty());
}

TEST_CASE(sink_reentrancy_does_not_deadlock)
{
    // Hooks run outside the channel lock, so a sink may reenter the channel.
    ChannelHarness harness { { .low = 16, .high = 64 } };

    harness.consumer_sink->on_wake = [&](u64) {
        auto result = harness.channel->take_all();
        EXPECT(!result.taken.is_empty());
    };
    (void)harness.channel->arm_waiter();
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('x', 40)))), WriteResult::Accepted);

    harness.consumer_sink->on_wake = nullptr;
    harness.producer_sink->on_grant_credit = [&](u64) {
        EXPECT_EQ(harness.channel->write(bytes_of("reentrant"sv)), WriteResult::Accepted);
    };
    // Consume credit again so draining issues a grant, whose hook writes back into the channel.
    EXPECT_EQ(harness.channel->write(bytes_of(MUST(String::repeated('y', 40)))), WriteResult::Accepted);
    (void)harness.channel->take_all();
    auto result = harness.channel->take_all();
    EXPECT_EQ(flatten(result), "reentrant"sv);
}

TEST_CASE(concurrent_writer_never_loses_a_wakeup)
{
    auto consumer_sink = adopt_ref(*new LatestTicketSink);
    auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, *consumer_sink, adopt_ref(*new NullProducerSink));

    static constexpr u64 write_count = 20'000;
    auto writer = Threading::Thread::construct("channel writer"sv, [&channel]() -> intptr_t {
        for (u64 i = 0; i < write_count; ++i)
            (void)channel->write(MUST(Core::ImmutableBytes::copy("x"sv.bytes())));
        (void)channel->close();
        return 0;
    });
    writer->start();

    // Every arming is owed exactly one wake carrying its own ticket — fired immediately when
    // bytes or a terminal are already pending, from the writer's thread otherwise. A lost
    // wakeup hangs this loop.
    u64 total_bytes_taken = 0;
    for (;;) {
        auto ticket = channel->arm_waiter();
        while (consumer_sink->latest.load(AK::MemoryOrder::memory_order_acquire) < ticket)
            ;
        auto result = channel->take_all();
        for (auto const& taken : result.taken)
            total_bytes_taken += taken.length;
        if (result.state != FetchByteChannel::State::Open && result.buffered_byte_count == 0)
            break;
    }
    MUST(writer->join());
    EXPECT_EQ(total_bytes_taken, write_count);
}

TEST_CASE(stale_ticket_identifiable_after_rearm)
{
    ChannelHarness harness;

    auto first_ticket = harness.channel->arm_waiter();
    harness.channel->disarm_waiter();
    auto second_ticket = harness.channel->arm_waiter();
    EXPECT(first_ticket != second_ticket);

    EXPECT_EQ(harness.channel->write(bytes_of("x"sv)), WriteResult::Accepted);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, second_ticket);
}

TEST_CASE(terminal_only_waiter_ignores_writes)
{
    ChannelHarness harness;
    EXPECT_EQ(harness.channel->write(bytes_of("pending"sv)), WriteResult::Accepted);

    // Buffered bytes neither fire nor consume a TerminalOnly waiter.
    auto ticket = harness.channel->arm_waiter(FetchByteChannel::WakeOn::TerminalOnly);
    EXPECT(harness.take_events().is_empty());
    EXPECT_EQ(harness.channel->write(bytes_of("more"sv)), WriteResult::Accepted);
    EXPECT(harness.take_events().is_empty());

    EXPECT_EQ(harness.channel->close(), TerminateResult::Terminated);
    auto events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, SinkEvent::Kind::Wake);
    EXPECT_EQ(events[0].value, ticket);

    // Arming against an existing terminal fires immediately.
    auto late_ticket = harness.channel->arm_waiter(FetchByteChannel::WakeOn::TerminalOnly);
    events = harness.take_events();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, late_ticket);
}
