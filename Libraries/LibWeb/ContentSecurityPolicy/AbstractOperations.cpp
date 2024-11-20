/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/AbstractOperations.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#contains-a-header-delivered-content-security-policy
bool csp_list_contains_header_delivered_policy(Vector<Policy> const& csp_list)
{
    // A CSP list contains a header-delivered Content Security Policy if it contains a policy whose source is "header".
    auto header_delivered_entry = csp_list.find_if([](auto const& policy) {
        return policy.source == Policy::Source::Header;
    });

    return !header_delivered_entry.is_end();
}

}