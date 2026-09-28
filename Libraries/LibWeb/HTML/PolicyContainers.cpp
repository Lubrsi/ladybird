/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
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

    // https://wicg.github.io/scroll-to-text-fragment/#document-policy-integration
    // This specification defines a configuration point in Document Policy with name
    // "force-load-at-top". Its type is boolean with default value false.
    if (auto document_policy = response->header_list()->get("Document-Policy"sv); document_policy.has_value()) {
        // https://www.rfc-editor.org/rfc/rfc8941.html#name-parsing-a-dictionary
        // Parse this as a Structured Fields dictionary so commas inside strings or inner lists do
        // not accidentally create a force-load-at-top member. LibHTTP does not yet have a general
        // Structured Fields parser, so keep the member splitting local to this policy lookup.
        GenericLexer lexer { document_policy->view() };
        while (!lexer.is_eof()) {
            lexer.consume_while([](char code_unit) { return code_unit == ' ' || code_unit == '\t'; });
            auto const member_start = lexer.tell();
            auto member_end = member_start;
            bool inside_string = false;
            bool escaped = false;
            size_t inner_list_depth = 0;
            while (!lexer.is_eof()) {
                auto const code_unit = lexer.consume();
                if (inside_string) {
                    if (escaped) {
                        escaped = false;
                    } else if (code_unit == '\\') {
                        escaped = true;
                    } else if (code_unit == '"') {
                        inside_string = false;
                    }
                } else if (code_unit == '"') {
                    inside_string = true;
                } else if (code_unit == '(') {
                    ++inner_list_depth;
                } else if (code_unit == ')' && inner_list_depth > 0) {
                    --inner_list_depth;
                } else if (code_unit == ',' && inner_list_depth == 0) {
                    member_end = lexer.tell() - 1;
                    break;
                }
                member_end = lexer.tell();
            }

            auto member = document_policy->view().substring_view(member_start, member_end - member_start).trim_whitespace();
            GenericLexer member_lexer { member };
            auto const key = member_lexer.consume_while([](char code_unit) {
                return is_ascii_lower_alpha(code_unit) || is_ascii_digit(code_unit) || first_is_one_of(code_unit, '_', '-', '.', '*');
            });
            if (key != "force-load-at-top"sv)
                continue;

            // https://www.rfc-editor.org/rfc/rfc8941.html#name-dictionaries
            // A dictionary member with no value has the value Boolean true.
            if (member_lexer.is_eof() || member_lexer.next_is(';')) {
                result->force_load_at_top = true;
                continue;
            }

            if (!member_lexer.consume_specific("=?"sv) || member_lexer.is_eof())
                continue;
            auto const value = member_lexer.consume();
            if (!member_lexer.is_eof() && !member_lexer.next_is(';'))
                continue;
            if (value == '1')
                result->force_load_at_top = true;
            else if (value == '0')
                result->force_load_at_top = false;
        }
    }

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
    result->force_load_at_top = serialized_policy_container.force_load_at_top;
    return result;
}

// https://html.spec.whatwg.org/multipage/browsers.html#clone-a-policy-container
NonnullRefPtr<PolicyContainer> PolicyContainer::clone() const
{
    // 1. Let clone be a new policy container.
    auto clone = create();

    // 2. For each policy in policyContainer's CSP list, append a copy of policy into clone's CSP list.
    // NB: A policy in a CSP list is immutable and shared by its clones.
    clone->csp_list = csp_list;

    // 3. Set clone's embedder policy to a copy of policyContainer's embedder policy.
    // NOTE: This is a C++ copy.
    clone->embedder_policy = embedder_policy;

    // 4. Set clone's referrer policy to policyContainer's referrer policy.
    clone->referrer_policy = referrer_policy;

    // 5. Set clone's integrity policy to a copy of policyContainer's integrity policy.
    clone->integrity_policy = integrity_policy;

    // https://wicg.github.io/scroll-to-text-fragment/#document-policy-integration
    // This specification defines a configuration point in Document Policy with name
    // "force-load-at-top".
    clone->force_load_at_top = force_load_at_top;

    // 6. Return clone.
    return clone;
}

SerializedPolicyContainer PolicyContainer::serialize() const
{
    return SerializedPolicyContainer {
        .csp_list = csp_list.serialize(),
        .embedder_policy = embedder_policy,
        .referrer_policy = referrer_policy,
        .force_load_at_top = force_load_at_top,
    };
}

}
