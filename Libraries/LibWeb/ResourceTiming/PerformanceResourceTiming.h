/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Bindings/PerformanceResourceTimingPrototype.h>

namespace Web::ResourceTiming {

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming
class PerformanceResourceTiming final : public PerformanceTimeline::PerformanceEntry {
    WEB_PLATFORM_OBJECT(PerformanceResourceTiming, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(PerformanceResourceTiming);

public:
    virtual ~PerformanceResourceTiming();

    static void mark_resource_timing(GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, String const& requested_url, Fetch::Infrastructure::Request::InitiatorType initiator_type, JS::Object& global, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, Fetch::Infrastructure::Response::BodyInfo body_info, Fetch::Infrastructure::Status response_status, FlyString delivery_type = ""_fly_string);

    // NOTE: These three functions are answered by the registry for the given entry type.
    // https://w3c.github.io/timing-entrytypes-registry/#registry

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    static Optional<u64> max_buffer_size() { return 250; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual FlyString const& entry_type() const override;

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-initiatortype
    FlyString const& initiator_type() const { return m_initiator_type; }

    // https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-deliverytype
    FlyString const& delivery_type() const { return m_delivery_type; }

    FlyString next_hop_protocol() const;

    HighResolutionTime::DOMHighResTimeStamp worker_start() const;
    HighResolutionTime::DOMHighResTimeStamp redirect_start() const;
    HighResolutionTime::DOMHighResTimeStamp redirect_end() const;
    HighResolutionTime::DOMHighResTimeStamp fetch_start() const;
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_start() const;
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_end() const;
    HighResolutionTime::DOMHighResTimeStamp connect_start() const;
    HighResolutionTime::DOMHighResTimeStamp connect_end() const;
    HighResolutionTime::DOMHighResTimeStamp secure_connection_start() const;
    HighResolutionTime::DOMHighResTimeStamp request_start() const;
    HighResolutionTime::DOMHighResTimeStamp final_response_headers_start() const;
    HighResolutionTime::DOMHighResTimeStamp first_interim_response_start() const;
    HighResolutionTime::DOMHighResTimeStamp response_start() const;
    HighResolutionTime::DOMHighResTimeStamp response_end() const;
    u64 encoded_body_size() const;
    u64 decoded_body_size() const;
    u64 transfer_size() const;
    Fetch::Infrastructure::Status response_status() const;
    Bindings::RenderBlockingStatusType render_blocking_status() const;
    String const& content_type() const;

private:
    PerformanceResourceTiming(JS::Realm&, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    FlyString m_initiator_type;
    String m_requested_url;
    GC::Ref<Fetch::Infrastructure::FetchTimingInfo> m_timing_info;
    Fetch::Infrastructure::Response::BodyInfo m_response_body_info;
    Optional<Fetch::Infrastructure::Response::CacheState> m_cache_mode;
    Fetch::Infrastructure::Status m_response_status;
    FlyString m_delivery_type;
};

}