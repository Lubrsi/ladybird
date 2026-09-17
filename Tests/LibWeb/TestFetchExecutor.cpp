/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ThreadID.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
#include <fcntl.h>

using namespace Web::Fetch::Engine;

namespace {

// Records what reaches the engine, and on which thread.
struct RecordedCommands {
    Vector<u64> fetch_ids;
    Vector<AK::ThreadID> threads;
    Optional<IPC::TransportHandle> adopted_transport;
};

class RecordingSink final : public EngineCommandSink {
public:
    explicit RecordingSink(RecordedCommands& recorded)
        : m_recorded(recorded)
    {
    }

    virtual void handle_command(EngineCommand command) override
    {
        m_recorded.threads.append(AK::ThreadID::current());
        command.payload.visit(
            [&](Command::Stop const& stop) { m_recorded.fetch_ids.append(stop.fetch_id.value()); },
            [&](Command::AdoptNetworkTransport& adopt) { m_recorded.adopted_transport = move(adopt.handle); },
            [](auto const&) { FAIL("unexpected command"); });
    }

private:
    RecordedCommands& m_recorded;
};

FetchExecutorServices services_for_current_thread()
{
    return { .event_loop = Core::EventLoop::current_weak(), .request_client = nullptr };
}

IPC::TransportHandle transport_over_a_pipe(int& read_end, int& write_end)
{
    auto pipe_fds = MUST(Core::System::pipe2(O_CLOEXEC));
    read_end = pipe_fds[0];
    write_end = pipe_fds[1];
    return IPC::TransportHandle { IPC::File::adopt_fd(read_end) };
}

bool fd_is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

}

TEST_CASE(commands_run_on_the_loop_in_posting_order)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));

    for (u64 id = 1; id <= 3; ++id)
        executor->post(Command::Stop { .fetch_id = FetchId(id) });
    EXPECT(recorded.fetch_ids.is_empty());

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(recorded.fetch_ids, (Vector<u64> { 1, 2, 3 }));
    for (auto thread : recorded.threads)
        EXPECT(thread.is_current_thread());
}

TEST_CASE(an_executor_on_another_thread_runs_its_commands_there)
{
    Core::EventLoop main_loop;
    RecordedCommands recorded;
    Atomic<InlineFetchExecutor*> published { nullptr };
    Atomic<bool> release { false };

    auto thread = Threading::Thread::construct("FetchExecutor"sv, [&]() -> intptr_t {
        Core::EventLoop loop;
        auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));
        published.store(executor.ptr());
        while (!release.load())
            loop.pump(Core::EventLoop::WaitMode::WaitForEvents);
        return 0;
    });
    thread->start();

    while (!published.load())
        MUST(Core::System::sleep_ms(1));
    auto& executor = *published.load();
    for (u64 id = 1; id <= 3; ++id)
        executor.post(Command::Stop { .fetch_id = FetchId(id) });
    executor.post(Command::AdoptNetworkTransport { .handle = {} });
    executor.services().event_loop->take()->deferred_invoke([&] { release.store(true); });
    executor.services().event_loop->take()->wake();
    MUST(thread->join());

    EXPECT_EQ(recorded.fetch_ids, (Vector<u64> { 1, 2, 3 }));
    EXPECT_EQ(recorded.threads.size(), 4u);
    for (auto recorded_thread : recorded.threads) {
        EXPECT(!recorded_thread.is_current_thread());
        EXPECT_EQ(recorded_thread, recorded.threads.first());
    }
}

TEST_CASE(a_command_posted_after_shutdown_is_destroyed_at_once)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));

    int read_end = -1;
    int write_end = -1;
    auto handle = transport_over_a_pipe(read_end, write_end);
    executor->shutdown();
    executor->post(Command::AdoptNetworkTransport { .handle = move(handle) });
    EXPECT(!fd_is_open(read_end));

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(recorded.threads.is_empty());
    MUST(Core::System::close(write_end));
}

TEST_CASE(a_pending_command_is_dropped_by_shutdown)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));

    int read_end = -1;
    int write_end = -1;
    executor->post(Command::AdoptNetworkTransport { .handle = transport_over_a_pipe(read_end, write_end) });
    executor->shutdown();
    EXPECT(fd_is_open(read_end));

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(recorded.threads.is_empty());
    EXPECT(!fd_is_open(read_end));
    MUST(Core::System::close(write_end));
}

TEST_CASE(a_network_transport_is_adopted_exactly_once)
{
    Core::EventLoop loop;
    RecordedCommands recorded;
    auto executor = InlineFetchExecutor::create(services_for_current_thread(), make<RecordingSink>(recorded));

    int read_end = -1;
    int write_end = -1;
    executor->post(Command::AdoptNetworkTransport { .handle = transport_over_a_pipe(read_end, write_end) });
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    // The engine now owns the descriptor; nothing else closed it, and dropping the engine's handle does.
    EXPECT(recorded.adopted_transport.has_value());
    EXPECT(fd_is_open(read_end));
    recorded.adopted_transport.clear();
    EXPECT(!fd_is_open(read_end));
    MUST(Core::System::close(write_end));
}
