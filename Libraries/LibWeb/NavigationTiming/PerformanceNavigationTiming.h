/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceNavigationTimingPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Export.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>

namespace Web::NavigationTiming {

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
class WEB_API PerformanceNavigationTiming : public ResourceTiming::PerformanceResourceTiming {
    WEB_PLATFORM_OBJECT(PerformanceNavigationTiming, ResourceTiming::PerformanceResourceTiming);
    GC_DECLARE_ALLOCATOR(PerformanceNavigationTiming);

public:
    virtual ~PerformanceNavigationTiming() override;

    static void create_the_navigation_timing_entry(GC::Ref<DOM::Document> document, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> fetch_timing, u16 redirect_count, Bindings::NavigationTimingType navigation_type, Fetch::Infrastructure::Response::BodyInfo body_info, Optional<Fetch::Infrastructure::Response::CacheState> cache_mode);

    // NOTE: These three functions are answered by the registry for the given entry type.
    // https://w3c.github.io/timing-entrytypes-registry/#registry

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    static Optional<u64> max_buffer_size() { return OptionalNone {}; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual FlyString const& entry_type() const override;
    virtual HighResolutionTime::DOMHighResTimeStamp duration() const override;

    virtual HighResolutionTime::DOMHighResTimeStamp redirect_start() const override;
    virtual HighResolutionTime::DOMHighResTimeStamp redirect_end() const override;
    virtual HighResolutionTime::DOMHighResTimeStamp worker_start() const override;
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

protected:
    PerformanceNavigationTiming(JS::Realm&, String const& name, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    GC::Ref<DOM::Document> m_document;

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-redirectcount
    u16 m_redirect_count { 0 };

    // https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-type
    Bindings::NavigationTimingType m_navigation_type { Bindings::NavigationTimingType::Navigate };

    // https://w3c.github.io/navigation-timing/#dfn-critical-ch-restart-time
    HighResolutionTime::DOMHighResTimeStamp m_critical_ch_restart_time { 0 };
};

}
