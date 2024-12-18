/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceNavigationTiming);

PerformanceNavigationTiming::PerformanceNavigationTiming(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, GC::Ref<DOM::DocumentLoadTimingInfo> document_load_timing, GC::Ref<DOM::DocumentUnloadTimingInfo> previous_document_unloading_timing)
    : PerformanceResourceTiming(realm, name, start_time, duration, timing_info)
    , m_document_load_timing(document_load_timing)
    , m_previous_document_unloading_timing(previous_document_unloading_timing)
{
}

PerformanceNavigationTiming::~PerformanceNavigationTiming() = default;

// https://w3c.github.io/navigation-timing/#performanceentry
FlyString const& PerformanceNavigationTiming::entry_type() const
{
    // The entryType getter step is to return the DOMString "navigation".
    return PerformanceTimeline::EntryTypes::navigation;
}

void PerformanceNavigationTiming::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceNavigationTiming);
}

void PerformanceNavigationTiming::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document_load_timing);
    visitor.visit(m_previous_document_unloading_timing);
}

void PerformanceNavigationTiming::create_the_navigation_timing_entry(GC::Ref<DOM::Document> document, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> fetch_timing, u16 redirect_count, Bindings::NavigationTimingType navigation_type, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, HighResolutionTime::DOMHighResTimeStamp critical_ch_restart, Fetch::Infrastructure::Response::BodyInfo body_info, Fetch::Infrastructure::Status response_status)
{
    // 1. Let global be document's relevant global object.
    auto& global = verify_cast<HTML::Window>(HTML::relevant_global_object(*document));

    // 2. Let navigationTimingEntry be a new PerformanceNavigationTiming object in global's realm.
    // 4. Set navigationTimingEntry's document load timing to document's load timing info
    // 5. Set navigationTimingEntry's previous document unload timing to document's previous document unload timing.
    auto& realm = global.realm();
    auto converted_start_time = ResourceTiming::convert_fetch_timestamp(fetch_timing->start_time(), global);
    auto converted_end_time = ResourceTiming::convert_fetch_timestamp(fetch_timing->end_time(), global);
    auto requested_url = document->url().to_string();
    auto navigation_timing_entry = realm.create<PerformanceNavigationTiming>(realm, requested_url, converted_start_time, converted_end_time - converted_start_time, fetch_timing, document->load_timing_info(), document->previous_document_unload_timing());

    // 3. Setup the resource timing entry for navigationTimingEntry given "navigation", document's URL, fetchTiming, cacheMode, and bodyInfo.
    navigation_timing_entry->setup_the_resource_timing_entry(PerformanceTimeline::EntryTypes::navigation, requested_url, fetch_timing, cache_mode, move(body_info), response_status);

    // 6. Set navigationTimingEntry's redirect count to redirectCount.
    navigation_timing_entry->m_redirect_count = redirect_count;

    // 7. Set navigationTimingEntry's navigation type to navigationType.
    navigation_timing_entry->m_navigation_type = navigation_type;

    // FIXME: 8. Set navigationTimingEntry's service worker timing to serviceWorkerTiming.
    // FIXME: 9. Set document's navigation timing entry to navigationTimingEntry.

    // 10. Set navigationTimingEntry's Critical-CH restart time to criticalCHRestart.
    navigation_timing_entry->m_critical_ch_restart_time = critical_ch_restart;

    // FIXME: 11. Set navigationTimingEntry's not restored reasons to the result of creating a NotRestoredReasons
    //            object given document's not restored reasons.

    // relevant_performance_entry_tuple
}

// https://w3c.github.io/navigation-timing/#PerformanceResourceTiming
// Spec Note: Though redirectStart and redirectEnd are exposed in PerformanceResourceTiming, they have a different
//            meaning in Navigation Timing, where they return zero for navigations with cross-origin redirects.
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::redirect_start() const
{
    // The redirectStart getter steps are to perform the following steps:
    // 1. If this's [=PerformanceNavigationTiming/redirect count] is 0, return 0.
    if (m_redirect_count == 0)
        return 0.0;

    // 2. Otherwise return this's redirectStart.
    return Base::redirect_start();
}

// https://w3c.github.io/navigation-timing/#PerformanceResourceTiming
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::redirect_end() const
{
    // The redirectEnd getter steps are to perform the following steps:
    // 1. If this's [=PerformanceNavigationTiming/redirect count] is 0, return 0.
    if (m_redirect_count == 0)
        return 0.0;

    // 2. Otherwise return this's redirectEnd.
    return Base::redirect_end();
}

// https://w3c.github.io/navigation-timing/#PerformanceResourceTiming
// Spec Note: Though workerStart is exposed in PerformanceResourceTiming, it has a different meaning in Navigation
//            Timing, as unlike subresources, a navigation may trigger the activation or running of a service worker.
//            In the context of Navigation Timing, workerStart returns the timestamp measured just before the worker
//            has been activated or started. See [service-workers] for a precise definition.
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::worker_start() const
{
    // FIXME: 1. Let workerTiming be this's service worker timing.

    // 2. If workerTiming is null, then return this's prototype's workerStart.
    // FIXME: If workerTiming is null
    return Base::worker_start();

    // FIXME: 3. Return workerTiming's start time.
}

// https://w3c.github.io/navigation-timing/#PerformanceResourceTiming
// Spec Note: When a service worker is used as part of the navigation, The fetchStart overload holds a different
//            meaning than the meaning in PerformanceResourceTiming. It returns the timestamp measured right before
//            the FetchEvent is dispatched for the service worker. The time difference between workerStart and
//            fetchStart in the document's navigation timing entry can be used to determine roughly how long it took
//            for the worker to be initialized or activated. See [service-workers] for a precise definition.
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::fetch_start() const
{
    // FIXME: 1. Let workerTiming be this's service worker timing.

    // 2. If workerTiming is null, then return this's prototype's fetchStart.
    // FIXME: If workerTiming is null
    return Base::fetch_start();

    // FIXME: 3. Return workerTiming's fetch event dispatch time.
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_start() const
{
    // The unloadEventStart getter steps are to return this's previous document unload timing's unload event start time.
    // Spec Note: If the previous document and the current document have the same origin, this timestamp is measured
    //            immediately before the user agent starts the unload event of the previous document. If there is no
    //            previous document or the previous document has a different origin than the current document, this
    //            attribute will return zero.
    return m_previous_document_unloading_timing->unload_event_start_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_end() const
{
    // The unloadEventEnd getter steps are to return this's previous document unload timing's unload event end time.
    // Spec Note: If the previous document and the current document have the same origin, this timestamp is measured
    //            immediately after the user agent handles the unload event of the previous document. If there is no
    //            previous document or the previous document has a different origin than the current document, this
    //            attribute will return zero.
    return m_previous_document_unloading_timing->unload_event_end_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-dominteractive
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_interactive() const
{
    // The domInteractive getter steps are to return this's document load timing's DOM interactive time.
    // Spec Note: This timestamp is measured before the user agent sets the current document readiness to "interactive".
    return m_document_load_timing->dom_interactive_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_start() const
{
    // The domContentLoadedEventStart getter steps are to return this's document load timing's DOM content loaded event
    // start time.
    // Spec Note: This timestamp is measured before the user agent dispatches the DOMContentLoaded event.
    return m_document_load_timing->dom_content_loaded_event_start_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_end() const
{
    // The domContentLoadedEventEnd getter steps are to return this's document load timing's DOM content loaded event
    // end time.
    // Spec Note: This timestamp is measured after the user agent completes handling of the DOMContentLoaded event.
    return m_document_load_timing->dom_content_loaded_event_end_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcomplete
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_complete() const
{
    // The domComplete getter steps are to return this's document load timing's DOM complete time.
    // Spec Note: This timestamp is measured before the user agent sets the current document readiness to "complete".
    //            See document readiness for a precise definition.
    return m_document_load_timing->dom_complete_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_start() const
{
    // The loadEventStart getter steps are to return this's document load timing's load event start time.
    // Spec Note: This timestamp is measured before the user agent dispatches the load event for the document.
    return m_document_load_timing->load_event_start_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_end() const
{
    // The loadEventEnd getter steps are to return this's document load timing's load event end time.
    // Spec Note: This timestamp is measured after the user agent completes handling the load event for the document.
    return m_document_load_timing->load_event_end_time;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-type
Bindings::NavigationTimingType PerformanceNavigationTiming::type() const
{
    // The type getter steps are to run the this's navigation type.
    // Spec Note: Client-side redirects, such as those using the Refresh pragma directive, are not considered HTTP
    //            redirects by this spec. In those cases, the type attribute SHOULD return appropriate value, such
    //            as reload if reloading the current page, or navigate if navigating to a new URL.
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
    // The criticalCHRestart getter steps are to return this's Critical-CH restart time.
    // Spec Note: If criticalCHRestart is not 0 it will be before all other timestamps except for navigationStart,
    //            unloadEventStart, and unloadEventEnd. This is because it marks the moment the redirection part of the
    //            navigation was restarted.
    return m_critical_ch_restart_time;
}

}
