/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibURL/Origin.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#content-security-policy-object
// A policy defines allowed and restricted behaviors, and may be applied to a Document, WorkerGlobalScope,
// or WorkletGlobalScope.
class Policy {
public:
    enum class Disposition {
        Enforce,
        Report,
    };

    enum class Source {
        Header,
        Meta,
    };

    ~Policy() = default;

    static Policy parse_a_serialized_csp(Variant<ByteBuffer, String> serialized, Source source, Disposition disposition);
    static Vector<Policy> parse_a_responses_content_security_policies(GC::Ref<Fetch::Infrastructure::Response const> response);

    Vector<Directives::Directive> const& directives() const { return m_directives; }
    Disposition disposition() const { return m_disposition; }
    Source source() const { return m_source; }
    URL::Origin const& self_origin() const { return m_self_origin; }

    bool contains_directive_with_name(StringView name) const;

private:
    Policy() = default;

    // https://w3c.github.io/webappsec-csp/#policy-directive-set
    // Each policy has an associated directive set, which is an ordered set of directives that define the policy’s
    // implications when applied.
    Vector<Directives::Directive> m_directives;

    // https://w3c.github.io/webappsec-csp/#policy-disposition
    // Each policy has an associated disposition, which is either "enforce" or "report".
    Disposition m_disposition { Disposition::Enforce };

    // https://w3c.github.io/webappsec-csp/#policy-source
    // Each policy has an associated source, which is either "header" or "meta".
    Source m_source { Source::Header };

    // https://w3c.github.io/webappsec-csp/#policy-self-origin
    // Each policy has an associated self-origin, which is an origin that is used when matching the 'self' keyword.
    // Spec Note: This is needed to facilitate the 'self' checks of local scheme documents/workers that have inherited
    //            their policy but have an opaque origin. Most of the time this will simply be the environment settings
    //            object’s origin.
    URL::Origin m_self_origin;
};

}
