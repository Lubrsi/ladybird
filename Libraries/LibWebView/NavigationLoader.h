/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibRequests/Forward.h>
#include <LibRequests/Request.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/RequestServerManager.h>

namespace WebView {

// UI-process owner of navigation population state. It keeps the pending entry
// and response body alive while the document host is being selected.
class WEBVIEW_API NavigationLoader final
    : public RefCounted<NavigationLoader>
    , public Weakable<NavigationLoader> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullRefPtr<NavigationLoader> create(Web::HTML::NavigationPopulationRequest request)
    {
        return adopt_ref(*new NavigationLoader(move(request)));
    }

    ~NavigationLoader();

    struct ResponseDocument {
        // Created for inline content that doesn't have a DOM: the error page for a failed navigation.
        bool is_inline_content { false };
        Web::HTML::OpenerPolicyEnforcementResult coop_enforcement_result;
        URL::URL response_url;
        Optional<URL::URL> request_current_url;
        URL::Origin origin;
        Web::HTML::OpenerPolicy opener_policy;
        Web::HTML::SandboxingFlagSet final_sandboxing_flag_set {};
        // The id of the window environment a process created the document with before the UI process heard of it.
        Optional<Web::HTML::EnvironmentId> environment_id;
        Web::HTML::CrossProcessId document_id;
    };
    Optional<ResponseDocument> response_document() const;
    void set_document(CanonicalDocument const&, CanonicalNavigable const&);

    void did_finish_navigation_params_creation(RequestServerSiteBindings const& population_worker_bindings, Web::HTML::NavigationPopulationResult);
    void acquire_response_body(RequestServerSiteBindings const& population_worker_bindings, Function<void(bool)> completion_steps);
    // Moves the response over to the RequestServer of the process that will host the document, if that is another one.
    void move_response_body_to(RequestServerInstance&, Function<void(bool)> completion_steps);
    bool response_body_matches(int request_server_client_id, u64 request_server_request_id) const;
    // The host may adopt the response body once. The steps run when RequestServer allows it, or has gone away.
    void let_host_adopt_response_body(WebContentPage const& host, Function<void()> steps);
    bool response_body_was_handed_to(WebContentClient const&) const;
    Web::HTML::NavigationPopulationRequest const& request() const { return m_request; }
    Web::HTML::NavigationPopulationResult const& result() const;
    Web::HTML::NavigationPopulationResult take_result();
    void reclaim_response_body_after_failed_handoff();

    static bool response_body_belongs_to_another_process(Web::HTML::NavigationPopulationResult const&, WebContentClient const&);
    static void discard(RequestServerSiteBindings const& population_worker_bindings, Web::HTML::NavigationPopulationResult&);

private:
    explicit NavigationLoader(Web::HTML::NavigationPopulationRequest request)
        : m_request(move(request))
    {
    }

    void determine_the_origin_of_the_response();
    void did_acquire(bool succeeded);
    void set_response_body_callbacks(Requests::Request&);
    void did_designate_response_body_adopter();
    void release_response_body();

    Web::HTML::NavigationPopulationRequest m_request;
    Optional<Web::HTML::NavigationPopulationResult> m_result;
    RefPtr<Requests::Request> m_response_body_request;
    RefPtr<RequestServerInstance> m_response_body_request_server;
    Optional<int> m_response_body_request_server_client_id;
    Optional<u64> m_response_body_request_server_request_id;
    bool m_response_body_was_handed_off { false };
    WeakPtr<WebContentClient> m_response_body_host;
    Function<void(bool)> m_completion_steps;
    Function<void()> m_steps_after_adopter_designation;
};

}
