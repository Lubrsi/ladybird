/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Function.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/CacheState.h>
#include <LibRequests/Forward.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Loader/NavigatorCompatibilityMode.h>

namespace Web {

class WEB_API ResourceLoader : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ResourceLoader)

public:
    static void initialize(GC::Heap&, NonnullRefPtr<Requests::RequestClient>);
    static bool is_initialized();
    static ResourceLoader& the();

    void set_client(NonnullRefPtr<Requests::RequestClient>);

    using OnHeadersReceived = GC::Function<void(Requests::Request*, HTTP::HeaderList const& response_headers, Optional<u32> status_code, Optional<String> const& reason_phrase, Optional<Core::ImmutableBytes> javascript_bytecode, Optional<u64> javascript_bytecode_cache_vary_key, Requests::CacheState cache_state)>;
    using OnDataReceived = GC::Function<void(Requests::ResponseData data)>;
    using OnCachedBodyAvailable = GC::Function<void(Core::ImmutableBytes data)>;
    using OnComplete = GC::Function<void(bool success, Requests::RequestTimingInfo const& timing_info, Optional<StringView> error_message)>;

    RefPtr<Requests::Request> load(LoadRequest&, GC::Root<OnHeadersReceived>, GC::Root<OnDataReceived>, GC::Root<OnCachedBodyAvailable>, GC::Root<OnComplete>, Requests::RequestClient::TransferLease = Requests::RequestClient::TransferLease::No);

    RefPtr<Requests::RequestClient>& request_client() { return m_request_client; }

    void prefetch_dns(URL::URL const&, URL::URL const& source_url);
    void preconnect(URL::URL const&, URL::URL const& source_url);

    static bool is_known_hsts_host(Page&, String const& host);

    String const& platform() const { return m_platform; }
    void set_platform(String platform) { m_platform = move(platform); }

    NavigatorCompatibilityMode navigator_compatibility_mode() { return m_navigator_compatibility_mode; }
    void set_navigator_compatibility_mode(NavigatorCompatibilityMode mode) { m_navigator_compatibility_mode = mode; }

private:
    explicit ResourceLoader(GC::Heap&, NonnullRefPtr<Requests::RequestClient>);

    struct FileLoadResult {
        ReadonlyBytes data;
        NonnullRefPtr<HTTP::HeaderList> response_headers;
        Requests::RequestTimingInfo timing_info {};
    };
    template<typename FileHandler, typename ErrorHandler>
    void handle_file_load_request(LoadRequest& request, FileHandler on_file, ErrorHandler on_error);
    template<typename ResourceHandler, typename ErrorHandler>
    void handle_about_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error);
    template<typename ResourceHandler, typename ErrorHandler>
    void handle_resource_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error);

    RefPtr<Requests::Request> start_network_request(LoadRequest const&, Requests::RequestClient::TransferLease);
    void finish_network_request(NonnullRefPtr<Requests::Request>);

    GC::Heap& m_heap;
    RefPtr<Requests::RequestClient> m_request_client;
    HashTable<NonnullRefPtr<Requests::Request>> m_active_requests;

    String m_platform;
    NavigatorCompatibilityMode m_navigator_compatibility_mode;
};

}
