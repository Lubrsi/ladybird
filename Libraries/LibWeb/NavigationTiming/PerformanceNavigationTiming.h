/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
 
#pragma once

#include <LibWeb/Bindings/PerformanceNavigationTimingPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>

namespace Web::NavigationTiming {

class PerformanceNavigationTiming final : public ResourceTiming::PerformanceResourceTiming {
    WEB_PLATFORM_OBJECT(PerformanceNavigationTiming, ResourceTiming::PerformanceResourceTiming);
    GC_DECLARE_ALLOCATOR(PerformanceNavigationTiming);

public:
    virtual ~PerformanceNavigationTiming() override;

    // FIXME: Add service worker timing info
    static void create_the_navigation_timing_entry(GC::Ref<DOM::Document> document, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> fetch_timing, u16 redirect_count, Bindings::NavigationTimingType navigation_type, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, HighResolutionTime::DOMHighResTimeStamp critical_ch_restart, Fetch::Infrastructure::Response::BodyInfo body_info, Fetch::Infrastructure::Status response_status);

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    // NOTE: The empty state represents Infinite size.
    static Optional<u64> max_buffer_size() { return OptionalNone {}; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual FlyString const& entry_type() const override;

    // ^ResourceTiming::PerformanceResourceTiming
    virtual HighResolutionTime::DOMHighResTimeStamp worker_start() const override;
    virtual HighResolutionTime::DOMHighResTimeStamp redirect_start() const override;
    virtual HighResolutionTime::DOMHighResTimeStamp redirect_end() const override;
    virtual HighResolutionTime::DOMHighResTimeStamp fetch_start() const override;

    HighResolutionTime::DOMHighResTimeStamp unload_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp unload_event_end() const;
    HighResolutionTime::DOMHighResTimeStamp dom_interactive() const;
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_end() const;
    HighResolutionTime::DOMHighResTimeStamp dom_complete() const;
    HighResolutionTime::DOMHighResTimeStamp load_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp load_event_end() const;
    Bindings::NavigationTimingType type() const;
    u16 redirect_count() const;
    HighResolutionTime::DOMHighResTimeStamp critical_ch_restart() const;

private:
    PerformanceNavigationTiming(JS::Realm&, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, GC::Ref<DOM::DocumentLoadTimingInfo> document_load_timing, GC::Ref<DOM::DocumentUnloadTimingInfo> previous_document_unloading_timing);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://w3c.github.io/navigation-timing/#dfn-document-load-timing
    // A PerformanceNavigationTiming has an associated document load timing info document load timing.
    GC::Ref<DOM::DocumentLoadTimingInfo> m_document_load_timing;

    // https://w3c.github.io/navigation-timing/#dfn-previous-document-unload-timing
    // A PerformanceNavigationTiming has an associated document unload timing info previous document unload timing.
    GC::Ref<DOM::DocumentUnloadTimingInfo> m_previous_document_unloading_timing;

    // https://w3c.github.io/navigation-timing/#dfn-redirect-count
    // A PerformanceNavigationTiming has an associated number redirect count.
    u16 m_redirect_count { 0 };

    // https://w3c.github.io/navigation-timing/#dfn-navigation-type
    // A PerformanceNavigationTiming has an associated NavigationTimingType navigation type.
    Bindings::NavigationTimingType m_navigation_type { Bindings::NavigationTimingType::Navigate };

    // https://w3c.github.io/navigation-timing/#dfn-critical-ch-restart-time
    // A PerformanceNavigationTiming has an associated DOMHighResTimeStamp Critical-CH restart time.
    HighResolutionTime::DOMHighResTimeStamp m_critical_ch_restart_time { 0.0 };

    // https://w3c.github.io/navigation-timing/#dfn-not-restored-reasons
    // FIXME: A PerformanceNavigationTiming has an associated NotRestoredReasons not restored reasons.

    // https://w3c.github.io/navigation-timing/#dfn-service-worker-timing
    // FIXME: A PerformanceNavigationTiming has an associated null or service worker timing info service worker timing.
};

}
