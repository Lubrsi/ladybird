/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Fetching/BodyStreamPullSource.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>

using Web::Fetch::Engine::FetchByteChannel;
using Web::Fetch::Engine::FetchByteChannelProducerSink;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchParams;
using Web::Streams::ReadableStream;

namespace {

class IgnoringProducerSink final : public FetchByteChannelProducerSink {
public:
    virtual void grant_credit(u64) override { }
    virtual void consumer_cancelled() override { }
};

GC::Ref<FetchParams> create_sync_xhr_fetch_params()
{
    auto algorithms = FetchAlgorithms::create(FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::SyncAccumulate });
    auto request = Web::Fetch::Infrastructure::Request::create();
    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    return FetchParams::create(move(request), algorithms, move(timing_info));
}

struct BodyStreamHarness {
    BodyStreamHarness()
        : realm(create_test_document()->relevant_settings_object().realm())
        , execution_context(*realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes)
    {
    }

    void create_stream()
    {
        channel = FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new IgnoringProducerSink));
        stream = Web::Fetch::Fetching::create_body_stream_from_channel(*realm, fetch_params, *channel);
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
    GC::Ptr<Web::Streams::ReadableStream> stream;
};

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
    harness.fetch_params = create_sync_xhr_fetch_params();
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
