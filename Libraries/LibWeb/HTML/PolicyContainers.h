/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibIPC/Forward.h>
#include <LibWeb/HTML/EmbedderPolicy.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#policy-container
// A policy container is a struct containing policies that apply to a Document, a WorkerGlobalScope, or a WorkletGlobalScope. It has the following items:
struct PolicyContainer {
    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-csp-list
    // A CSP list, which is a CSP list. It is initially empty.
    Vector<ContentSecurityPolicy::Policy> csp_list;

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-embedder-policy
    // An embedder policy, which is an embedder policy. It is initially a new embedder policy.
    EmbedderPolicy embedder_policy {};

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-referrer-policy
    // A referrer policy, which is a referrer policy. It is initially the default referrer policy.
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };
};

// https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
PolicyContainer create_a_policy_container_from_a_fetch_response(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ptr<Environment> environment);

}

namespace IPC {
template<>
ErrorOr<void> encode(IPC::Encoder&, Web::HTML::PolicyContainer const&);

template<>
ErrorOr<Web::HTML::PolicyContainer> decode(IPC::Decoder&);
}
