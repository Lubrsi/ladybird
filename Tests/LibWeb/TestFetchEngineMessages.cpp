/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/MainFetchEvent.h>
#include <fcntl.h>

using namespace Web::Fetch::Engine;

// A command is handed to the engine exactly once, so none is ever duplicated.
static_assert(!IsCopyConstructible<EngineCommand>);
static_assert(IsMoveConstructible<EngineCommand>);

// A completed preload response is bytes or a network error; a streaming or null delivered body cannot be expressed.
static_assert(!IsConstructible<Command::CompletedPreloadResponse::Completed, NullBody>);
static_assert(!IsConstructible<Delivered<NonNullLocalBodyHandle>, NullBody>);

static IPC::File open_pipe_end(int& other_end)
{
    auto pipe_fds = MUST(Core::System::pipe2(O_CLOEXEC));
    other_end = pipe_fds[1];
    return IPC::File::adopt_fd(pipe_fds[0]);
}

static bool fd_is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

TEST_CASE(commands_round_trip_their_payloads)
{
    Vector<EngineCommand> commands;
    commands.append(Command::StartFetch { .input = { .fetch_id = FetchId(1), .body_intent = BodyIntent::DrainAndDiscard, .blob_url_entry = {} } });
    commands.append(Command::Abort { .fetch_id = FetchId(2) });
    commands.append(Command::Terminate { .fetch_id = FetchId(3) });
    commands.append(Command::Stop { .fetch_id = FetchId(4) });
    commands.append(Command::ResolveRedirect { .delivery_id = DeliveryId(5), .continuation_id = ContinuationId(6), .resolution = Command::ResolveRedirect::AcceptAsFinal {} });
    commands.append(Command::PreloadResponseResolved {
        .fetch_id = FetchId(7),
        .response = { .outcome = Command::CompletedPreloadResponse::Completed { .body = Delivered<NonNullLocalBodyHandle> { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("preloaded"sv.bytes()))) } } },
    });
    commands.append(Command::TransportControl { .request_id = RequestId(8), .transport_epoch = TransportEpoch(9), .operation = Command::TransportControl::ResumeBodyDeliveryUpTo { .byte_count = 4096 } });

    Vector<u64> seen_ids;
    for (auto& command : commands) {
        command.payload.visit(
            [&](Command::StartFetch const& start) {
                EXPECT_EQ(start.input.body_intent, BodyIntent::DrainAndDiscard);
                EXPECT(!start.input.blob_url_entry.has_value());
                seen_ids.append(start.input.fetch_id.value());
            },
            [&](Command::Abort const& abort) { seen_ids.append(abort.fetch_id.value()); },
            [&](Command::Terminate const& terminate) { seen_ids.append(terminate.fetch_id.value()); },
            [&](Command::Stop const& stop) { seen_ids.append(stop.fetch_id.value()); },
            [&](Command::ResolveRedirect const& resolve) {
                EXPECT(resolve.resolution.has<Command::ResolveRedirect::AcceptAsFinal>());
                EXPECT_EQ(resolve.continuation_id, ContinuationId(6));
                seen_ids.append(resolve.delivery_id.value());
            },
            [&](Command::PreloadResponseResolved const& preload) {
                auto const& completed = preload.response.outcome.get<Command::CompletedPreloadResponse::Completed>();
                EXPECT_EQ(completed.body.handle().bytes(), "preloaded"sv.bytes());
                seen_ids.append(preload.fetch_id.value());
            },
            [&](Command::TransportControl const& control) {
                EXPECT_EQ(control.transport_epoch, TransportEpoch(9));
                EXPECT_EQ(control.operation.get<Command::TransportControl::ResumeBodyDeliveryUpTo>().byte_count, 4096u);
                seen_ids.append(control.request_id.value());
            },
            [&](Command::AdoptNetworkTransport const&) { FAIL("no transport was posted"); });
    }
    EXPECT_EQ(seen_ids, (Vector<u64> { 1, 2, 3, 4, 5, 7, 8 }));
}

TEST_CASE(adopt_network_transport_owns_its_handle)
{
    int write_end = -1;
    auto file = open_pipe_end(write_end);
    auto read_end = file.fd();

    Optional<EngineCommand> command = Command::AdoptNetworkTransport { .handle = IPC::TransportHandle { move(file) } };
    EXPECT(fd_is_open(read_end));

    // Moving the command moves the handle; the source no longer owns anything to close.
    EngineCommand moved = command.release_value();
    EXPECT(fd_is_open(read_end));

    moved = Command::Stop { .fetch_id = FetchId(1) };
    EXPECT(!fd_is_open(read_end));
    MUST(Core::System::close(write_end));
}

TEST_CASE(delivered_body_requires_a_valid_handle)
{
    EXPECT_DEATH("a delivered body is never null", (void)Delivered<NonNullLocalBodyHandle> { Core::ImmutableBytes {} });
}

TEST_CASE(events_round_trip_their_payloads)
{
    FetchTimingSeed timing_seed;
    timing_seed.start_time = 12.5;
    timing_seed.render_blocking = true;

    Vector<MainFetchEvent> events;
    events.append(Event::ResponseDelivery { .delivery_id = DeliveryId(1), .body = NullBody {}, .timing_seed = move(timing_seed) });
    events.append(Event::EndOfBody { .delivery_id = DeliveryId(2), .final_sizes = { .encoded_body_size = 10, .decoded_body_size = 20 }, .end_time = 99 });
    events.append(Event::ConsumeBodyResult { .delivery_id = DeliveryId(3), .outcome = Event::ConsumeBodyResult::Bytes { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("body"sv.bytes()))) } });
    events.append(Event::BodyChannelReadable { .delivery_id = DeliveryId(4), .ticket = 7 });
    events.append(Event::DeliveryTerminal { .delivery_id = DeliveryId(5), .outcome = Event::DeliveryTerminal::Revoked {} });
    events.append(Event::RequestTransmissionStarted { .transmission_id = TransmissionId(6), .generation = TransmissionGeneration(1) });
    events.append(Event::RequestTransmissionEnded {
        .transmission_id = TransmissionId(7),
        .generation = TransmissionGeneration(2),
        .terminal = {
            .request_id = RequestId(70),
            .transport_epoch = TransportEpoch(3),
            .transport_complete_at = MonotonicTime::now(),
            .outcome = RequestTerminal::CommittedAttemptFailed { .attempt_id = AttemptId(4), .error = Requests::NetworkError::IncompleteContent, .encoded_body_size = 512 },
        },
    });

    Vector<u64> seen_ids;
    for (auto const& event : events) {
        event.visit(
            [&](Event::ResponseDelivery const& delivery) {
                EXPECT(delivery.body.has<NullBody>());
                EXPECT_EQ(delivery.timing_seed.start_time, 12.5);
                EXPECT(delivery.timing_seed.render_blocking);
                seen_ids.append(delivery.delivery_id.value());
            },
            [&](Event::EndOfBody const& end) {
                EXPECT_EQ(end.final_sizes.decoded_body_size, 20u);
                seen_ids.append(end.delivery_id.value());
            },
            [&](Event::ConsumeBodyResult const& result) {
                EXPECT_EQ(result.outcome.get<Event::ConsumeBodyResult::Bytes>().bytes.bytes(), "body"sv.bytes());
                seen_ids.append(result.delivery_id.value());
            },
            [&](Event::BodyChannelReadable const& readable) {
                EXPECT_EQ(readable.ticket, 7u);
                seen_ids.append(readable.delivery_id.value());
            },
            [&](Event::DeliveryTerminal const& terminal) {
                EXPECT(terminal.outcome.has<Event::DeliveryTerminal::Revoked>());
                seen_ids.append(terminal.delivery_id.value());
            },
            [&](Event::RequestTransmissionStarted const& started) { seen_ids.append(started.transmission_id.value()); },
            [&](Event::RequestTransmissionEnded const& ended) {
                EXPECT_EQ(ended.terminal.request_id, RequestId(70));
                auto const& failed = ended.terminal.outcome.get<RequestTerminal::CommittedAttemptFailed>();
                EXPECT_EQ(failed.error, Requests::NetworkError::IncompleteContent);
                EXPECT_EQ(failed.encoded_body_size, 512u);
                seen_ids.append(ended.transmission_id.value());
            });
    }
    EXPECT_EQ(seen_ids, (Vector<u64> { 1, 2, 3, 4, 5, 6, 7 }));
}
