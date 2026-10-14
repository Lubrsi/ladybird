/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWebCommon/FileAPI/SerializedBlobURLEntry.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-params
class WEB_API FetchParams : public JS::Cell {
    GC_CELL(FetchParams, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchParams);

public:
    struct PreloadedResponseCandidatePendingTag { };
    using PreloadedResponseCandidate = Variant<Empty, PreloadedResponseCandidatePendingTag, NonnullRefPtr<Response>>;

    [[nodiscard]] static GC::Ref<FetchParams> create(NonnullRefPtr<Request>, GC::Ref<FetchAlgorithms const>, NonnullRefPtr<FetchTimingInfo>);
    [[nodiscard]] static GC::Ref<FetchParams> copy(FetchParams const&);

    [[nodiscard]] NonnullRefPtr<Request> request() const { return m_request; }
    void set_request(NonnullRefPtr<Request> request) { m_request = move(request); }
    [[nodiscard]] GC::Ref<FetchController> controller() const { return m_controller; }
    [[nodiscard]] NonnullRefPtr<FetchTimingInfo> timing_info() const { return m_timing_info; }

    [[nodiscard]] Engine::BodyIntent body_intent() const { return m_body_intent; }
    void set_body_intent(Engine::BodyIntent body_intent) { m_body_intent = body_intent; }

    [[nodiscard]] HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const { return m_cross_origin_isolated_capability; }
    void set_cross_origin_isolated_capability(HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability) { m_cross_origin_isolated_capability = cross_origin_isolated_capability; }

    [[nodiscard]] PreloadedResponseCandidate& preloaded_response_candidate() { return m_preloaded_response_candidate; }
    [[nodiscard]] PreloadedResponseCandidate const& preloaded_response_candidate() const { return m_preloaded_response_candidate; }
    void set_preloaded_response_candidate(PreloadedResponseCandidate preloaded_response_candidate) { m_preloaded_response_candidate = move(preloaded_response_candidate); }

    [[nodiscard]] bool is_aborted() const;
    [[nodiscard]] bool is_canceled() const;

    [[nodiscard]] bool has_response_body_transfer_lease() const { return m_has_response_body_transfer_lease; }
    void set_has_response_body_transfer_lease(bool value) { m_has_response_body_transfer_lease = value; }

    [[nodiscard]] Optional<FileAPI::SerializedBlobURLEntry> const& blob_url_entry() const { return m_blob_url_entry; }
    void set_blob_url_entry(Optional<FileAPI::SerializedBlobURLEntry> blob_url_entry) { m_blob_url_entry = move(blob_url_entry); }

    void add_pending_response(Badge<Fetching::PendingResponse>, GC::Ref<Fetching::PendingResponse> pending_response) const
    {
        VERIFY(!m_pending_responses.contains_slow(pending_response));
        m_pending_responses.append(pending_response);
    }

    void remove_pending_response(Badge<Fetching::PendingResponse>, GC::Ref<Fetching::PendingResponse> pending_response) const
    {
        m_pending_responses.remove_first_matching([&](auto entry) { return entry == pending_response; });
    }

private:
    FetchParams(NonnullRefPtr<Request>, GC::Ref<FetchAlgorithms const>, GC::Ref<FetchController>, NonnullRefPtr<FetchTimingInfo>);
    FetchParams(FetchParams const&);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#fetch-params-request
    // request
    //     A request.
    NonnullRefPtr<Request> m_request;

    // NB: The six algorithms and the task destination of a fetch params are kept on its controller.

    Engine::BodyIntent m_body_intent;

    // https://fetch.spec.whatwg.org/#fetch-params-cross-origin-isolated-capability
    // cross-origin isolated capability (default false)
    //     A boolean.
    HTML::CanUseCrossOriginIsolatedAPIs m_cross_origin_isolated_capability { HTML::CanUseCrossOriginIsolatedAPIs::No };

    // https://fetch.spec.whatwg.org/#fetch-params-controller
    // controller (default a new fetch controller)
    //     A fetch controller.
    GC::Ref<FetchController> m_controller;

    // https://fetch.spec.whatwg.org/#fetch-params-timing-info
    // timing info
    //     A fetch timing info.
    NonnullRefPtr<FetchTimingInfo> m_timing_info;

    // https://fetch.spec.whatwg.org/#fetch-params-preloaded-response-candidate
    // preloaded response candidate (default null)
    //     Null, "pending", or a response.
    PreloadedResponseCandidate m_preloaded_response_candidate;

    // Non-spec process integration: a navigation response is retained while the UI process selects its document host.
    bool m_has_response_body_transfer_lease { false };

    // The blob URL entry of the request's URL, when that is a blob: URL, as it was when the fetch started.
    Optional<FileAPI::SerializedBlobURLEntry> m_blob_url_entry;

    // The pending responses of this fetch whose callbacks have yet to run.
    mutable Vector<GC::Ref<Fetching::PendingResponse>> m_pending_responses;
};

}
