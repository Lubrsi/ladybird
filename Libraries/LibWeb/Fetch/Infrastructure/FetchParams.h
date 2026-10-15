/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/RefCounted.h>
#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
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
class WEB_API FetchParams : public RefCounted<FetchParams> {
public:
    struct PreloadedResponseCandidatePendingTag { };
    using PreloadedResponseCandidate = Variant<Empty, PreloadedResponseCandidatePendingTag, NonnullRefPtr<Response>>;

    [[nodiscard]] static NonnullRefPtr<FetchParams> create(FetchController&, NonnullRefPtr<Request>, GC::Ref<FetchAlgorithms const>, NonnullRefPtr<FetchTimingInfo>);
    [[nodiscard]] static NonnullRefPtr<FetchParams> copy(FetchParams const&);

    [[nodiscard]] NonnullRefPtr<Request> request() const { return m_request; }
    void set_request(NonnullRefPtr<Request> request) { m_request = move(request); }
    [[nodiscard]] GC::Ref<FetchController> controller() const;
    [[nodiscard]] FetchControlBlock& control_block() const { return *m_control_block; }
    [[nodiscard]] NonnullRefPtr<FetchTimingInfo> timing_info() const { return m_timing_info; }

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

private:
    FetchParams(NonnullRefPtr<Request>, GC::Ref<FetchAlgorithms const>, FetchController&, NonnullRefPtr<FetchTimingInfo>);
    FetchParams(FetchParams const&);

    // https://fetch.spec.whatwg.org/#fetch-params-request
    // request
    //     A request.
    NonnullRefPtr<Request> m_request;

    // NB: The six algorithms and the task destination of a fetch params are kept on its controller.

    // https://fetch.spec.whatwg.org/#fetch-params-cross-origin-isolated-capability
    // cross-origin isolated capability (default false)
    //     A boolean.
    HTML::CanUseCrossOriginIsolatedAPIs m_cross_origin_isolated_capability { HTML::CanUseCrossOriginIsolatedAPIs::No };

    // https://fetch.spec.whatwg.org/#fetch-params-controller
    // controller (default a new fetch controller)
    //     A fetch controller.
    // NB: The controller is named by its token, in the registry that issued it.
    FetchControllerToken m_controller_token;
    WeakPtr<FetchControllerRegistry> m_controller_registry;
    NonnullRefPtr<FetchControlBlock> m_control_block;

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
};

}
