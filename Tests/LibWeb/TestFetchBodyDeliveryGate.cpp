/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Fetching/FetchBodyDeliveryGate.h>

namespace {

using Web::Fetch::Fetching::FetchBodyDeliveryGate;
using NavigationHold = FetchBodyDeliveryGate::NavigationHold;

struct TransportCall {
    enum class Op : u8 {
        Resume,
        Pause,
        Stop,
    };
    Op op;
    u64 value { 0 };

    bool operator==(TransportCall const&) const = default;
};

struct RecordingTransport final : FetchBodyDeliveryGate::Transport {
    virtual void resume_body_delivery_up_to(u64 byte_count) override { calls.append({ TransportCall::Op::Resume, byte_count }); }
    virtual void pause_body_delivery() override { calls.append({ TransportCall::Op::Pause, 0 }); }
    virtual void stop() override { calls.append({ TransportCall::Op::Stop, 0 }); }

    Vector<TransportCall> take_calls() { return move(calls); }

    Vector<TransportCall> calls;
};

struct GateHarness {
    explicit GateHarness(NavigationHold hold)
        : gate(FetchBodyDeliveryGate::create(hold))
        , transport(adopt_ref(*new RecordingTransport))
    {
    }

    NonnullRefPtr<FetchBodyDeliveryGate> gate;
    NonnullRefPtr<RecordingTransport> transport;
};

}

TEST_CASE(navigation_hold_blocks_credited_delivery)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Pause } }));

    // Channel demand alone must never release the navigation hold.
    harness.gate->grant_credit(100);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Pause } }));
}

TEST_CASE(bounded_hold_release_permits_bytes_from_the_current_delivery_point)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(100);
    (void)harness.transport->take_calls();

    harness.gate->release_hold_up_to(10);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 10 } }));

    // A further bounded release counts from what has been delivered since, matching the sniff
    // wait's semantics.
    harness.gate->did_deliver(10);
    harness.gate->release_hold_up_to(5);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 5 } }));
}

TEST_CASE(hold_release_cannot_bypass_channel_backpressure)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(10);
    (void)harness.transport->take_calls();

    // The full hold release leaves the channel's credit as the only bound.
    harness.gate->release_hold();
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 10 } }));

    harness.gate->did_deliver(10);
    harness.gate->release_hold();
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Pause } }));

    harness.gate->grant_credit(30);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 30 } }));
}

TEST_CASE(delivery_consumes_the_smaller_of_both_allowances)
{
    GateHarness harness { NavigationHold::Held };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(100);
    harness.gate->release_hold_up_to(20);
    (void)harness.transport->take_calls();

    harness.gate->did_deliver(15);
    harness.gate->grant_credit(50);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 5 } }));
}

TEST_CASE(allowances_granted_before_attach_apply_at_attach)
{
    GateHarness harness { NavigationHold::None };
    harness.gate->grant_credit(40);
    harness.gate->attach_transport(harness.transport);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 40 } }));
}

TEST_CASE(attach_after_termination_stops_the_transport)
{
    GateHarness harness { NavigationHold::None };
    harness.gate->consumer_cancelled();
    harness.gate->attach_transport(harness.transport);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Stop } }));
}

TEST_CASE(consumer_cancellation_stops_an_attached_transport_once)
{
    GateHarness harness { NavigationHold::None };
    harness.gate->attach_transport(harness.transport);
    harness.gate->grant_credit(10);
    (void)harness.transport->take_calls();

    harness.gate->consumer_cancelled();
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Stop } }));

    harness.gate->consumer_cancelled();
    harness.gate->grant_credit(10);
    harness.gate->release_hold();
    EXPECT(harness.transport->take_calls().is_empty());
}

TEST_CASE(detached_transport_is_never_touched_again)
{
    // Teardown detaches the gate before the channel cancellation reaches it, so a transferred
    // transport keeps running for its new owner.
    GateHarness harness { NavigationHold::None };
    harness.gate->attach_transport(harness.transport);
    (void)harness.transport->take_calls();

    harness.gate->detach_transport();
    harness.gate->consumer_cancelled();
    harness.gate->grant_credit(10);
    harness.gate->release_hold();
    EXPECT(harness.transport->take_calls().is_empty());
}

TEST_CASE(channel_sink_posts_commands_onto_the_creating_loop)
{
    Core::EventLoop loop;
    GateHarness harness { NavigationHold::None };
    harness.gate->attach_transport(harness.transport);
    (void)harness.transport->take_calls();

    auto sink = harness.gate->create_channel_sink();
    sink->grant_credit(25);
    EXPECT(harness.transport->take_calls().is_empty());

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Resume, 25 } }));

    sink->consumer_cancelled();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(harness.transport->take_calls(), (Vector { TransportCall { TransportCall::Op::Stop } }));
}
