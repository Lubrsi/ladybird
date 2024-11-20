/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DefaultSource.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>

namespace Web::ContentSecurityPolicy::Directives {

DefaultSource::DefaultSource(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
    dbgln("hello");
}

Directive::Result DefaultSource::pre_request_check(GC::Ref<Fetch::Infrastructure::Request const>, Policy const&) const
{
    // 1. Let name be the result of executing § 6.8.1 Get the effective directive for request on request.
    // auto name = get_the_effective_directive_for_request(request);
    dbgln("pre-request check");
    return Result::Blocked;
}

Directive::Result DefaultSource::post_request_check(GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Fetch::Infrastructure::Response const>, Policy const&) const
{
    TODO();
}

Directive::Result DefaultSource::inline_check(GC::Ref<DOM::Element const>, String const&, Policy const&, String const&) const
{
    TODO();
}

}
