/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>
#include <LibWeb/Fetch/Fetching/BodyStreamPullSource.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>

using Web::Fetch::Engine::FetchBodyDeliveryGate;
using Web::Fetch::Engine::FetchByteChannel;
using Web::Fetch::Engine::FetchByteChannelConsumerSink;
using Web::Fetch::Engine::FetchByteChannelProducerSink;
using Web::Fetch::Infrastructure::Body;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchParams;
using Web::Fetch::Infrastructure::MAX_SNIFF_BYTES;
using Web::Streams::ReadableStream;

namespace {

class IgnoringProducerSink final : public FetchByteChannelProducerSink {
public:
    virtual void grant_credit(u64) override { }
    virtual void consumer_cancelled() override { }
};

GC::Ref<FetchParams> create_sync_xhr_fetch_params(Web::Fetch::Infrastructure::FetchController& controller)
{
    auto algorithms = FetchAlgorithms::create(FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::SyncAccumulate });
    auto request = Web::Fetch::Infrastructure::Request::create();
    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    return FetchParams::create(controller, move(request), algorithms, move(timing_info));
}

struct BodyStreamHarness {
    BodyStreamHarness()
        : realm(create_test_document()->relevant_settings_object().realm())
        , execution_context(*realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes)
    {
    }

    void create_stream(FetchByteChannel::Watermarks watermarks = { .low = 16, .high = 64 })
    {
        channel = FetchByteChannel::create(watermarks, adopt_ref(*new IgnoringProducerSink));
        body = Web::Fetch::Fetching::create_body_from_channel(*realm, fetch_params, *channel);
        stream = body->stream();
    }

    double queued_byte_count() const
    {
        return stream->controller()->get<GC::Ref<Web::Streams::ReadableByteStreamController>>()->queue_total_size();
    }

    // Runs every deferred invocation queued so far, which includes every channel wake.
    static void run_wakes()
    {
        for (size_t i = 0; i < 3; ++i)
            Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    GC::Ref<JS::Realm> realm;
    Web::HTML::TemporaryExecutionContext execution_context;
    GC::Ptr<FetchParams const> fetch_params;
    RefPtr<FetchByteChannel> channel;
    GC::Ptr<Body> body;
    GC::Ptr<Web::Streams::ReadableStream> stream;
};

struct SniffWaiter {
    size_t wake_count { 0 };
    ByteBuffer bytes;
    Optional<Body::SniffOutcome> outcome;

    Body::SniffBytesCallback callback(GC::Heap& heap)
    {
        return GC::create_function(heap, [this](Body::SniffBytes sniff_bytes) {
            ++wake_count;
            bytes = MUST(ByteBuffer::copy(sniff_bytes.bytes));
            outcome = sniff_bytes.outcome;
        });
    }
};

Core::ImmutableBytes make_bytes(size_t size, u8 first_value)
{
    auto bytes = MUST(ByteBuffer::create_uninitialized(size));
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<u8>(first_value + i);
    return Core::ImmutableBytes::adopt(move(bytes));
}

// Runs channel wakes and networking tasks until the waiter has woken.
void wait_for_wake(GC::Heap& heap, SniffWaiter const& waiter)
{
    Web::HTML::main_thread_event_loop().spin_until(GC::create_function(heap, [&waiter] { return waiter.wake_count > 0; }));
}

// Runs channel wakes and every networking task they queue.
void run_wakes_and_tasks(GC::Heap& heap)
{
    BodyStreamHarness::run_wakes();
    bool reached = false;
    Web::HTML::queue_a_task(Web::HTML::Task::Source::Networking, Web::HTML::main_thread_event_loop(), nullptr, GC::create_function(heap, [&reached] {
        reached = true;
    }));
    Web::HTML::main_thread_event_loop().spin_until(GC::create_function(heap, [&reached] { return reached; }));
}

}

TEST_CASE(an_error_reaches_a_stream_nobody_reads)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream();

    (void)harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) });
    EXPECT(harness.stream->state() == ReadableStream::State::Readable);

    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->state() == ReadableStream::State::Errored);
}

TEST_CASE(an_empty_close_closes_a_stream_nobody_reads)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream();

    (void)harness.channel->close();
    EXPECT(harness.stream->state() == ReadableStream::State::Readable);

    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->state() == ReadableStream::State::Closed);
}

TEST_CASE(a_close_leaves_unread_bytes_readable)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream();

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("unread"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    (void)harness.channel->close();

    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->state() == ReadableStream::State::Readable);
    EXPECT_EQ(harness.queued_byte_count(), 0);
}

TEST_CASE(bytes_reach_a_stream_while_the_event_loop_is_paused)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    auto& controller_registry = static_cast<Web::HTML::Agent&>(*harness.realm->vm().agent()).fetch_controller_registry;
    harness.fetch_params = create_sync_xhr_fetch_params(Web::Fetch::Infrastructure::FetchController::create(controller_registry));
    auto pause = Web::HTML::main_thread_event_loop().pause(Web::HTML::EventLoop::UpdateTheRendering::No);
    harness.create_stream();

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("pushed"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    BodyStreamHarness::run_wakes();
    EXPECT_EQ(harness.queued_byte_count(), 6);

    (void)harness.channel->close();
    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->state() == ReadableStream::State::Readable);
    EXPECT_EQ(harness.queued_byte_count(), 6);
}

TEST_CASE(sniffing_waits_for_the_resource_header_and_leaves_it_in_the_channel)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));

    auto whole_body = make_bytes(2000, 0);
    EXPECT_EQ(harness.channel->write(whole_body.slice(0, 500)), FetchByteChannel::WriteResult::Accepted);
    run_wakes_and_tasks(heap);
    EXPECT_EQ(waiter.wake_count, 0u);

    EXPECT_EQ(harness.channel->write(whole_body.slice(500, 1500)), FetchByteChannel::WriteResult::Accepted);
    EXPECT_EQ(waiter.wake_count, 0u);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Complete);
    EXPECT_EQ(waiter.bytes.bytes(), whole_body.bytes().trim(MAX_SNIFF_BYTES));

    // A transfer of the body after sniffing still carries it from its first byte.
    ByteBuffer transferred;
    for (auto const& taken : harness.channel->transfer_out())
        transferred.append(taken.bytes());
    EXPECT_EQ(transferred.bytes(), whole_body.bytes());
}

TEST_CASE(sniffing_a_body_set_up_during_a_pause_waits_for_the_resource_header)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    auto& heap = harness.realm->heap();
    auto whole_body = make_bytes(2000, 0);
    {
        auto pause = Web::HTML::main_thread_event_loop().pause(Web::HTML::EventLoop::UpdateTheRendering::No);
        harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
        EXPECT_EQ(harness.channel->write(whole_body.slice(0, 100)), FetchByteChannel::WriteResult::Accepted);
        BodyStreamHarness::run_wakes();
    }

    EXPECT_EQ(harness.channel->write(whole_body.slice(100, 400)), FetchByteChannel::WriteResult::Accepted);
    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));
    run_wakes_and_tasks(heap);
    EXPECT_EQ(waiter.wake_count, 0u);

    EXPECT_EQ(harness.channel->write(whole_body.slice(500, 1500)), FetchByteChannel::WriteResult::Accepted);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Complete);
    EXPECT_EQ(waiter.bytes.bytes(), whole_body.bytes().trim(MAX_SNIFF_BYTES));

    ByteBuffer transferred;
    for (auto const& taken : harness.channel->transfer_out())
        transferred.append(taken.bytes());
    EXPECT_EQ(transferred.bytes(), whole_body.bytes());
}

TEST_CASE(sniffing_a_body_that_ends_short_gives_all_of_it)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("short"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    (void)harness.channel->close();
    EXPECT_EQ(waiter.wake_count, 0u);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Complete);
    EXPECT_EQ(waiter.bytes, MUST(ByteBuffer::copy("short"sv.bytes())));
}

TEST_CASE(sniffing_a_body_that_errors_fails)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("partial"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    (void)harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) });
    EXPECT_EQ(waiter.wake_count, 0u);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Failed);
}

TEST_CASE(a_body_over_bytes_sniffs_at_once)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    auto& heap = harness.realm->heap();

    auto bytes = make_bytes(2000, 0);
    auto body = Body::create_over_bytes(*harness.realm, bytes);
    auto sniff_bytes = body->sniff_bytes_if_available();
    EXPECT(sniff_bytes.has_value());
    EXPECT_EQ(sniff_bytes->bytes, bytes.bytes().trim(MAX_SNIFF_BYTES));

    SniffWaiter waiter;
    body->wait_for_sniff_bytes(waiter.callback(heap));
    EXPECT_EQ(waiter.wake_count, 1u);
}

namespace {

class CountingConsumerSink final : public FetchByteChannelConsumerSink {
public:
    virtual void wake(Web::Fetch::Engine::WakeTicket) override { ++wake_count; }

    size_t wake_count { 0 };
};

}

TEST_CASE(stopping_a_whole_body_transport_cancels_its_channel_and_wakes_its_reader)
{
    Core::EventLoop event_loop;
    auto gate = FetchBodyDeliveryGate::create(FetchBodyDeliveryGate::NavigationHold::None);
    auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, gate->create_channel_sink());
    gate->grant_credit(channel->initial_credit());
    auto reader = adopt_ref(*new CountingConsumerSink);
    channel->attach_consumer(reader);
    (void)channel->arm_waiter(FetchByteChannel::EndOnly {});

    auto transport = Web::Fetch::Fetching::create_whole_body_transport(*gate, *channel);
    transport->attach_to(gate);
    transport->set_body(make_bytes(1000, 0), [&](Requests::ResponseData data) {
        gate->did_deliver(data.bytes().size());
        (void)channel->write(MUST(Core::ImmutableBytes::copy(data.bytes())));
    });
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(channel->peek_up_to(0).buffered_byte_count, 64u);
    EXPECT_EQ(reader->wake_count, 0u);

    transport->stop();
    EXPECT_EQ(channel->peek_up_to(0).state, FetchByteChannel::State::ConsumerCancelled);
    EXPECT_EQ(reader->wake_count, 1u);
}
