/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceNavigationTiming);

PerformanceNavigationTiming::PerformanceNavigationTiming(JS::Realm& realm, String const& name, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info)
    : ResourceTiming::PerformanceResourceTiming(realm, name, 0, 0, timing_info)
    , m_document(as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document())
{
}

PerformanceNavigationTiming::~PerformanceNavigationTiming() = default;

void PerformanceNavigationTiming::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceNavigationTiming);
    Base::initialize(realm);
}

void PerformanceNavigationTiming::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
FlyString const& PerformanceNavigationTiming::entry_type() const
{
    // The entryType getter step is to return the DOMString "navigation".
    return PerformanceTimeline::EntryTypes::navigation;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::duration() const
{
    // The duration getter step is to return a DOMHighResTimeStamp equal to the difference between
    // loadEventEnd and this's startTime.
    return m_document->load_timing_info().load_event_end_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::redirect_start() const
{
    // 1. If this's redirect count is 0, return 0.
    if (m_redirect_count == 0)
        return 0;

    // 2. Otherwise return this's redirectStart.
    return Base::redirect_start();
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::redirect_end() const
{
    // 1. If this's redirect count is 0, return 0.
    if (m_redirect_count == 0)
        return 0;

    // 2. Otherwise return this's redirectEnd.
    return Base::redirect_end();
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::worker_start() const
{
    // FIXME: 1. Let workerTiming be this's service worker timing.
    // FIXME: 2. If workerTiming is null, then return this's prototype's workerStart.
    // FIXME: 3. Return workerTiming's start time.
    return Base::worker_start();
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::fetch_start() const
{
    // FIXME: 1. Let workerTiming be this's service worker timing.
    // FIXME: 2. If workerTiming is null, then return this's prototype's fetchStart.
    // FIXME: 3. Return workerTiming's fetch event dispatch time.
    return Base::fetch_start();
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_start() const
{
    // The unloadEventStart getter steps are to return this's previous document unload timing's
    // unload event start time.
    return m_document->previous_document_unload_timing().unload_event_start_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_end() const
{
    // The unloadEventEnd getter steps are to return this's previous document unload timing's
    // unload event end time.
    return m_document->previous_document_unload_timing().unload_event_end_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-dominteractive
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_interactive() const
{
    // The domInteractive getter steps are to return this's document load timing's DOM interactive time.
    return m_document->load_timing_info().dom_interactive_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_start() const
{
    // The domContentLoadedEventStart getter steps are to return this's document load timing's
    // DOM content loaded event start time.
    return m_document->load_timing_info().dom_content_loaded_event_start_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_end() const
{
    // The domContentLoadedEventEnd getter steps are to return this's document load timing's
    // DOM content loaded event end time.
    return m_document->load_timing_info().dom_content_loaded_event_end_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcomplete
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_complete() const
{
    // The domComplete getter steps are to return this's document load timing's DOM complete time.
    return m_document->load_timing_info().dom_complete_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_start() const
{
    // The loadEventStart getter steps are to return this's document load timing's load event start time.
    return m_document->load_timing_info().load_event_start_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_end() const
{
    // The loadEventEnd getter steps are to return this's document load timing's load event end time.
    return m_document->load_timing_info().load_event_end_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-type
Bindings::NavigationTimingType PerformanceNavigationTiming::type() const
{
    // The type getter steps are to return this's navigation type.
    return m_navigation_type;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-redirectcount
u16 PerformanceNavigationTiming::redirect_count() const
{
    // The redirectCount getter steps are to return this's redirect count.
    return m_redirect_count;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-criticalchrestart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::critical_ch_restart() const
{
    // The criticalCHRestart getter steps are to return this's `Critical-CH` restart time.
    return m_critical_ch_restart_time;
}

// https://w3c.github.io/navigation-timing/#dfn-create-the-navigation-timing-entry
void PerformanceNavigationTiming::create_the_navigation_timing_entry(GC::Ref<DOM::Document> document, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> fetch_timing, u16 redirect_count, Bindings::NavigationTimingType navigation_type, Fetch::Infrastructure::Response::BodyInfo body_info, Optional<Fetch::Infrastructure::Response::CacheState> cache_mode)
{
    // 1. Let global be document's relevant global object.
    auto& global = HTML::relevant_global_object(*document);
    auto& window_or_worker = as<HTML::WindowOrWorkerGlobalScopeMixin>(global);
    auto& realm = HTML::relevant_realm(*document);

    // 2. Let navigationTimingEntry be a new PerformanceNavigationTiming object in global's realm.
    auto navigation_timing_entry = realm.create<PerformanceNavigationTiming>(realm, document->url_string(), fetch_timing);

    // 3. Setup the resource timing entry for navigationTimingEntry given "navigation", document's URL, fetchTiming, cacheMode, and bodyInfo.
    navigation_timing_entry->setup_the_resource_timing_entry("navigation"_fly_string, document->url_string(), fetch_timing, cache_mode, move(body_info), 0);

    // 4. Set navigationTimingEntry's document load timing to document's load timing info.
    // NOTE: We read document load timing info live from m_document.

    // 5. Set navigationTimingEntry's previous document unload timing to document's previous document unload timing.
    // NOTE: We read previous document unload timing live from m_document.

    // 6. Set navigationTimingEntry's redirect count to redirectCount.
    navigation_timing_entry->m_redirect_count = redirect_count;

    // 7. Set navigationTimingEntry's navigation type to navigationType.
    navigation_timing_entry->m_navigation_type = navigation_type;

    // FIXME: 8. Set navigationTimingEntry's service worker timing to serviceWorkerTiming.

    // 9. Set document's navigation timing entry to navigationTimingEntry.
    document->set_navigation_timing_entry(navigation_timing_entry);

    // FIXME: 10. Set navigationTimingEntry's `Critical-CH` restart time to criticalCHRestart.

    // FIXME: 11. Set navigationTimingEntry's not restored reasons to the result of creating a NotRestoredReasons object given document's not restored reasons.

    // 12. Add navigationTimingEntry to global's performance entry buffer.
    window_or_worker.queue_performance_entry(navigation_timing_entry);
}

}
