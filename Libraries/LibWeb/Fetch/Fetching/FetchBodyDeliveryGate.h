/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NumericLimits.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibRequests/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Fetching/FetchByteChannel.h>

namespace Web::Fetch::Fetching {

// The transport-facing flow gate. Delivery is bounded by two independent cumulative
// allowances — the channel's credit and the navigation transfer/sniff hold — and the
// transport may deliver only up to the smaller of the two, so channel demand can never
// release the navigation hold and a hold release can never bypass channel backpressure.
//
// Every method is producer-loop-affine: the gate's counters and transport are unsynchronized,
// so the channel's notifications reach it only through create_channel_sink(), which posts them
// onto the loop the gate was created on.
class WEB_API FetchBodyDeliveryGate final : public AtomicRefCounted<FetchBodyDeliveryGate> {
public:
    enum class NavigationHold : u8 {
        None,
        Held,
    };

    // The delivery operations the gate drives, producer-loop-affine like the gate itself;
    // Requests::Request is the production implementation behind the adapting attach overload.
    class Transport : public RefCounted<Transport> {
    public:
        virtual ~Transport() = default;
        virtual void resume_body_delivery_up_to(u64 byte_count) = 0;
        virtual void pause_body_delivery() = 0;
        virtual void stop() = 0;
    };

    [[nodiscard]] static NonnullRefPtr<FetchBodyDeliveryGate> create(NavigationHold);

    [[nodiscard]] NonnullRefPtr<FetchByteChannelProducerSink> create_channel_sink();

    // Allowances granted before the transport exists are applied on attach.
    void attach_transport(NonnullRefPtr<Transport>);
    void attach_transport(NonnullRefPtr<Requests::Request>);

    // Reported by the pump for every delivered chunk; delivery consumes both allowances.
    void did_deliver(u64 byte_count);

    // The navigation hold's two release shapes: a bounded release permits this many further
    // bytes from the current delivery point, matching the sniff wait's semantics.
    void release_hold_up_to(u64 byte_count);
    void release_hold();

    void consumer_cancelled();

    // A detached transport must not be touched further: a transferred request keeps running for
    // its new owner.
    void detach_transport();

    void grant_credit(u64 byte_count);

private:
    explicit FetchBodyDeliveryGate(NavigationHold);

    static constexpr u64 unbounded = NumericLimits<u64>::max();

    void apply_to_transport();

    u64 m_channel_allowance { 0 };
    u64 m_hold_allowance { 0 };
    u64 m_delivered_byte_count { 0 };
    bool m_terminated { false };
    RefPtr<Transport> m_transport;
};

}
