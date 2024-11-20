/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::ContentSecurityPolicy {

Violation::Violation(JS::Object& global_object, Policy policy, String directive)
    : m_global_object(global_object)
    , m_policy(policy)
    , m_effective_directive(directive)
{
}

// https://w3c.github.io/webappsec-csp/#create-violation-for-global
GC::Ref<Violation> Violation::create_a_violation_object_for_global_policy_and_directive(JS::Realm& realm, JS::Object& global_object, Policy const& policy, String directive)
{
    // 1. Let violation be a new violation whose global object is global, policy is policy, effective directive is
    //    directive, and resource is null.
    auto violation = realm.create<Violation>(global_object, policy, directive);

    // FIXME: 2. If the user agent is currently executing script, and can extract a source file’s URL, line number,
    //           and column number from the global, set violation’s source file, line number, and column number
    //           accordingly.
    // SPEC ISSUE 1:  Is this kind of thing specified anywhere? I didn’t see anything that looked useful in [ECMA262].

    // 3. If global is a Window object, set violation’s referrer to global’s document's referrer.
    if (auto* window = dynamic_cast<HTML::Window*>(&global_object)) {
        violation->m_referrer = window->associated_document().referrer();
    }

    // FIXME: 4. Set violation’s status to the HTTP status code for the resource associated with violation’s global object.
    // SPEC ISSUE 2: How, exactly, do we get the status code? We don’t actually store it anywhere.

    // 5. Return violation.
    return violation;
}

// https://w3c.github.io/webappsec-csp/#create-violation-for-request
GC::Ref<Violation> Violation::create_a_violation_object_for_request_and_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request, Policy const& policy)
{
    // 1. Let directive be the result of executing § 6.8.1 Get the effective directive for request on request.
    auto directive = Directives::get_the_effective_directive_for_request(request);

    // NOTE: The spec assumes that the effective directive of a Violation is a non-empty string.
    //       See the definition of m_effective_directive.
    VERIFY(directive.has_value());

    auto directive_as_string = MUST(String::from_utf8(directive.value()));

    // 2. Let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive
    //      on request’s client’s global object, policy, and directive.
    auto violation = create_a_violation_object_for_global_policy_and_directive(realm, request->client()->global_object(), policy, directive_as_string);

    // 3. Set violation’s resource to request’s url.
    // Spec Note: We use request’s url, and not its current url, as the latter might contain information about redirect
    //            targets to which the page MUST NOT be given access.
    violation->m_resource = request->url();

    // 4. Return violation.
    return violation;
}

void Violation::visit_edges(Cell::Visitor& visitor)
{
    visitor.visit(m_global_object);
    visitor.visit(m_element);
}

// https://w3c.github.io/webappsec-csp/#violation-url
URL::URL Violation::url() const
{
    // Each violation has a url which is its global object’s URL.
    if (auto* window = dynamic_cast<HTML::Window*>(m_global_object.ptr())) {
        return window->associated_document().url();
    }

    if (auto* worker = dynamic_cast<HTML::WorkerGlobalScope*>(m_global_object.ptr())) {
        return worker->url();
    }

    TODO();
}

// https://w3c.github.io/webappsec-csp/#report-violation
void Violation::report_a_violation()
{
    TODO();
}

}
