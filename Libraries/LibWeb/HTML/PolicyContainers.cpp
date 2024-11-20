/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
PolicyContainer create_a_policy_container_from_a_fetch_response(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ptr<Environment>)
{
    // FIXME: 1. If response's URL's scheme is "blob", then return a clone of response's URL's blob URL entry's
    //           environment's policy container.

    // 2. Let result be a new policy container.
    PolicyContainer result;

    // 3. Set result's CSP list to the result of parsing a response's Content Security Policies given response.
    result.csp_list = ContentSecurityPolicy::Policy::parse_a_responses_content_security_policies(response);

    // FIXME: 4. If environment is non-null, then set result's embedder policy to the result of obtaining an embedder
    //           policy given response and environment. Otherwise, set it to "unsafe-none".

    // 5. Set result's referrer policy to the result of parsing the `Referrer-Policy` header given response.
    //    [REFERRERPOLICY]
    result.referrer_policy = ReferrerPolicy::parse_a_referrer_policy_from_a_referrer_policy_header(response);

    // 6. Return result.
    return result;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(IPC::Encoder& encoder, Web::HTML::PolicyContainer const& policy_container)
{
    TRY(encode(encoder, policy_container.referrer_policy));

    return {};
}

template<>
ErrorOr<Web::HTML::PolicyContainer> decode(IPC::Decoder& decoder)
{
    auto referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());

    return Web::HTML::PolicyContainer { .referrer_policy = referrer_policy };
}

}
