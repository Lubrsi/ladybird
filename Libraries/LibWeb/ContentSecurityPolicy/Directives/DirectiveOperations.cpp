/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#effective-directive-for-a-request
Optional<StringView> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request)
{
    // Each fetch directive controls a specific destination of request. Given a request request, the following algorithm
    // returns either null or the name of the request’s effective directive:
    // 1. If request’s initiator is "prefetch" or "prerender", return default-src.
    if (request->initiator() == Fetch::Infrastructure::Request::Initiator::Prefetch || request->initiator() == Fetch::Infrastructure::Request::Initiator::Prerender)
        return "default-src"sv;

    // 2. Switch on request’s destination, and execute the associated steps:
    // the empty string
    //      1. Return connect-src.
    if (!request->destination().has_value())
        return "connect-src"sv;

    switch (request->destination().value()) {
    // "manifest"
    //      1. Return manifest-src.
    case Fetch::Infrastructure::Request::Destination::Manifest:
        return "manifest-src"sv;
    // "object"
    // "embed"
    //      1. Return object-src.
    case Fetch::Infrastructure::Request::Destination::Object:
    case Fetch::Infrastructure::Request::Destination::Embed:
        return "object-src"sv;
    // "frame"
    // "iframe"
    //      1. Return frame-src.
    case Fetch::Infrastructure::Request::Destination::Frame:
    case Fetch::Infrastructure::Request::Destination::IFrame:
        return "frame-src"sv;
    // "audio"
    // "track"
    // "video"
    //      1. Return media-src.
    case Fetch::Infrastructure::Request::Destination::Audio:
    case Fetch::Infrastructure::Request::Destination::Track:
    case Fetch::Infrastructure::Request::Destination::Video:
        return "media-src"sv;
    // "font"
    //      1. Return font-src.
    case Fetch::Infrastructure::Request::Destination::Font:
        return "font-src"sv;
    // "image"
    //      1. Return img-src.
    case Fetch::Infrastructure::Request::Destination::Image:
        return "img-src"sv;
    // "style"
    //      1. Return style-src-elem.
    case Fetch::Infrastructure::Request::Destination::Style:
        return "style-src-elem"sv;
    // "script"
    // "xslt"
    // "audioworklet"
    // "paintworklet"
    //      1. Return script-src-elem.
    case Fetch::Infrastructure::Request::Destination::Script:
    case Fetch::Infrastructure::Request::Destination::XSLT:
    case Fetch::Infrastructure::Request::Destination::AudioWorklet:
    case Fetch::Infrastructure::Request::Destination::PaintWorklet:
        return "script-src-elem"sv;
    // "serviceworker"
    // "sharedworker"
    // "worker"
    //      1. Return worker-src.
    case Fetch::Infrastructure::Request::Destination::ServiceWorker:
    case Fetch::Infrastructure::Request::Destination::SharedWorker:
    case Fetch::Infrastructure::Request::Destination::Worker:
        return "worker-src"sv;
    // "json"
    // "webidentity"
    //      1. Return connect-src.
    case Fetch::Infrastructure::Request::Destination::JSON:
    case Fetch::Infrastructure::Request::Destination::WebIdentity:
        return "connect-src"sv;
    // "report"
    //      1. Return null.
    case Fetch::Infrastructure::Request::Destination::Report:
        return {};
    // 3. Return connect-src.
    // Spec Note: The algorithm returns connect-src as a default fallback. This is intended for new fetch destinations
    //            that are added and which don’t explicitly fall into one of the other categories.
    default:
        return "connect-src"sv;
    }
}

}
