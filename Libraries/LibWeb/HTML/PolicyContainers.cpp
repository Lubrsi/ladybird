/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>
#include <LibWebCommon/HTML/SerializedPolicyContainer.h>

namespace Web::HTML {

NonnullRefPtr<PolicyContainer> PolicyContainer::create()
{
    return adopt_ref(*new PolicyContainer);
}

// https://html.spec.whatwg.org/multipage/browsers.html#requires-storing-the-policy-container-in-history
bool url_requires_storing_the_policy_container_in_history(URL::URL const& url)
{
    // 1. If url's scheme is "blob", then return false.
    if (url.scheme() == "blob"sv)
        return false;

    // 2. If url is local, then return true.
    // 3. Return false.
    return Fetch::Infrastructure::is_local_url(url);
}

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
NonnullRefPtr<PolicyContainer> create_a_policy_container_from_a_fetch_response(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ptr<Environment>)
{
    // FIXME: 1. If response's URL's scheme is "blob", then return a clone of response's URL's blob URL entry's
    //           environment's policy container.

    // 2. Let result be a new policy container.
    auto result = PolicyContainer::create();

    // 3. Set result's CSP list to the result of parsing a response's Content Security Policies given response.
    result->csp_list = ContentSecurityPolicy::Policy::parse_a_responses_content_security_policies(response);

    // FIXME: 4. If environment is non-null, then set result's embedder policy to the result of obtaining an embedder
    //           policy given response and environment. Otherwise, set it to "unsafe-none".

    // 5. Set result's referrer policy to the result of parsing the `Referrer-Policy` header given response.
    //    [REFERRERPOLICY]
    auto parsed_referrer_policy = ReferrerPolicy::parse_a_referrer_policy_from_a_referrer_policy_header(response);
    if (parsed_referrer_policy != ReferrerPolicy::ReferrerPolicy::EmptyString)
        result->referrer_policy = parsed_referrer_policy;

    // FIXME: 6. Parse Integrity-Policy headers with response and result.

    // 7. Return result.
    return result;
}

NonnullRefPtr<PolicyContainer> create_a_policy_container_from_serialized_policy_container(SerializedPolicyContainer const& serialized_policy_container)
{
    auto result = PolicyContainer::create();
    result->csp_list = ContentSecurityPolicy::PolicyList { serialized_policy_container.csp_list };
    result->embedder_policy = serialized_policy_container.embedder_policy;
    result->referrer_policy = serialized_policy_container.referrer_policy;
    return result;
}

// https://html.spec.whatwg.org/multipage/browsers.html#clone-a-policy-container
NonnullRefPtr<PolicyContainer> PolicyContainer::clone() const
{
    // 1. Let clone be a new policy container.
    auto clone = create();

    // 2. For each policy in policyContainer's CSP list, append a copy of policy into clone's CSP list.
    // NB: A policy in a CSP list is immutable, so the clone shares it.
    clone->csp_list = csp_list;

    // 3. Set clone's embedder policy to a copy of policyContainer's embedder policy.
    // NOTE: This is a C++ copy.
    clone->embedder_policy = embedder_policy;

    // 4. Set clone's referrer policy to policyContainer's referrer policy.
    clone->referrer_policy = referrer_policy;

    // 5. Set clone's integrity policy to a copy of policyContainer's integrity policy.
    clone->integrity_policy = integrity_policy;

    // 6. Return clone.
    return clone;
}

SerializedPolicyContainer PolicyContainer::serialize() const
{
    return SerializedPolicyContainer {
        .csp_list = csp_list.serialize(),
        .embedder_policy = embedder_policy,
        .referrer_policy = referrer_policy,
    };
}

}
