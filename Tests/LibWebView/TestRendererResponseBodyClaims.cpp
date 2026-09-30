/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/TCPServer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/Page/NavigationTarget.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigation.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/RequestServerSiteBindings.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WorkerProcessManager.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

Web::HTML::NavigationPopulationResult result_with_response_body_of(int request_server_client_id, u64 request_server_request_id)
{
    auto url = URL::Parser::basic_parse("https://victim.example/"sv).release_value();
    Web::HTML::NavigationParamsDescriptor navigation_params {
        .id = {},
        .navigable_id = {},
        .request = {},
        .response = {},
        .fetch_timing_info = {},
        .coop_enforcement_result = { .url = url, .origin = url.origin(), .opener_policy = {} },
        .reserved_environment = {},
        .origin = url.origin(),
        .policy_container = {},
        .opener_policy = {},
        .about_base_url = {},
        .agent_cluster_id = {},
        .new_browsing_context_group_id = {},
    };
    navigation_params.response.body = Web::HTML::NavigationResponseBodyHandle {
        .request_server_client_id = request_server_client_id,
        .request_server_request_id = request_server_request_id,
    };
    return {
        .navigation_params = move(navigation_params),
        .redirected_url = {},
        .classic_history_api_state = {},
        .replacement_document_state = {},
    };
}

}

// The UI process takes over a navigation's response body only from a RequestServer client it gave the renderer naming
// it. A renderer naming the client of another live renderer, of a worker or of the UI process is terminated, and any
// other client's body is left alone.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestRendererResponseBodyClaims-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto create_view = [&] {
        return WebView::HeadlessWebView::create(theme, { 800, 600 });
    };

    auto victim_view = create_view();
    auto victim_client_id = victim_view->page().client().request_server_site_bindings().client_id().value();

    auto view = create_view();
    auto& stub = static_cast<WebContentClientStub&>(view->page().client());
    auto page_id = view->page().id();
    auto navigable_id = view->traversable().id();
    auto own_client_id = view->page().client().request_server_site_bindings().client_id().value();
    VERIFY(own_client_id != victim_client_id);

    // A renderer's own response body is not misbehavior.
    stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(own_client_id, 0));
    stub.did_finish_history_navigation_params_creation(page_id, app->allocate_ui_process_cross_process_id(), { .request = {}, .result = result_with_response_body_of(own_client_id, 0) });
    VERIFY(view->page().client().is_open());

    // Nor is the body of a client no live renderer holds. The UI process takes none of it, whether the result is
    // discarded or continues a navigation in flight.
    auto new_client = MUST(WebView::connect_new_request_server_client(WebView::Application::default_session(), RequestServer::SiteBinding::Bound));
    auto other_client = make_ref_counted<Requests::RequestClient>(MUST(new_client.handle.create_transport()));
    // Requests to this server stay in flight, and leased, for the rest of the test.
    auto silent_server = MUST(Core::TCPServer::try_create());
    MUST(silent_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
    Vector<NonnullOwnPtr<Core::TCPSocket>> silent_connections;
    silent_server->on_ready_to_accept = [&] { silent_connections.append(MUST(silent_server->accept())); };
    auto silent_url = URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/", *silent_server->local_port())).release_value();
    auto other_request = other_client->start_request("GET"sv, silent_url, {}, {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, Requests::RequestClient::TransferLease::Yes);
    auto other_result = [&] { return result_with_response_body_of(other_client->request_server_client_id(), other_request->id()); };

    stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, other_result());
    stub.did_finish_history_navigation_params_creation(page_id, app->allocate_ui_process_cross_process_id(), { .request = {}, .result = other_result() });

    auto navigation_id = Utf16String::from_utf8(generate_random_uuid());
    Web::HTML::NavigationStartRequest start_request {
        .navigable_id = navigable_id,
        .url = URL::about_blank(),
        .document_resource = {},
        .request_referrer = Web::Fetch::Infrastructure::RequestReferrer::Client,
        .request_referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
        .initiator_origin = URL::Origin::create_opaque(),
        .initiator_base_url = {},
        .navigable_target_name = {},
        .source_snapshot_params = Web::HTML::create_navigation_source_snapshot_without_a_source_document(),
        .target_snapshot_params = {},
        .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
        .history_handling = Web::Bindings::NavigationHistoryBehavior::Replace,
        .user_involvement = Web::HTML::UserNavigationInvolvement::None,
        .navigation_id = navigation_id,
        .classic_history_api_state = {},
        .navigation_api_state = {},
        .navigation_api_key = {},
        .navigation_api_id = {},
        .directive_state_id = {},
        .directive_state_value = {},
    };
    stub.did_request_navigation_start(page_id, navigable_id, Web::NavigationTarget::TopLevel, URL::about_blank(), navigation_id, move(start_request));
    stub.did_complete_navigation_unload_check(page_id, navigable_id, navigation_id);
    VERIFY(view->traversable().navigation_with_id(navigation_id)->state.has<WebView::CanonicalNavigation::CreatingNavigationParams>());
    stub.did_finish_navigation_params_creation(page_id, navigable_id, navigation_id, other_result());
    VERIFY(!view->traversable().has_uncommitted_navigation());
    VERIFY(view->page().client().is_open());

    // Every adoption above has reached RequestServer once the UI process's own client is answered.
    auto other_request_server = WebView::RequestServerManager::the().instance_for_client(other_client->request_server_client_id());
    VERIFY(other_request_server);
    (void)other_request_server->ui_client().send_sync<Messages::RequestServer::GetClientId>();
    VERIFY(other_client->send_sync<Messages::RequestServer::StopRequest>(other_request->id())->success());

    // A body fetched by an earlier client of the population worker is still the worker's own, so the UI process stops
    // it along with a discarded result.
    auto later_client = MUST(WebView::connect_new_request_server_client(WebView::Application::default_session(), RequestServer::SiteBinding::Bound));
    WebView::RequestServerSiteBindings population_worker_bindings;
    population_worker_bindings.did_connect(other_client->request_server_client_id());
    population_worker_bindings.did_connect(later_client.client_id);
    auto earlier_request = other_client->start_request("GET"sv, silent_url, {}, {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, Requests::RequestClient::TransferLease::Yes);
    auto earlier_result = result_with_response_body_of(other_client->request_server_client_id(), earlier_request->id());
    WebView::NavigationLoader::discard(population_worker_bindings, earlier_result);
    (void)other_request_server->ui_client().send_sync<Messages::RequestServer::GetClientId>();
    VERIFY(!other_client->send_sync<Messages::RequestServer::StopRequest>(earlier_request->id())->success());

    auto expect_rejected = [&](StringView what, Function<void(WebContentClientStub&, Web::PageId, Web::HTML::CrossProcessId)> send) {
        auto view = create_view();
        Optional<WebView::ViewImplementation::WebContentCrashReason> crash_reason;
        view->on_web_content_crashed = [&](auto reason) { crash_reason = reason; };

        send(static_cast<WebContentClientStub&>(view->page().client()), view->page().id(), view->traversable().id());
        Core::EventLoop::current().spin_until([&]() { return crash_reason.has_value(); });

        if (crash_reason != WebView::ViewImplementation::WebContentCrashReason::RejectedIPC) {
            warnln("FAIL: {} was not rejected", what);
            VERIFY_NOT_REACHED();
        }
    };

    expect_rejected("a navigation's response body fetched by another renderer"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(victim_client_id, 0));
    });
    expect_rejected("a history navigation's response body fetched by another renderer"sv, [&](auto& stub, auto page_id, auto) {
        stub.did_finish_history_navigation_params_creation(page_id, app->allocate_ui_process_cross_process_id(), { .request = {}, .result = result_with_response_body_of(victim_client_id, 0) });
    });
    expect_rejected("a navigation's response body held by the UI process"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(other_request_server->ui_client().request_server_client_id(), 0));
    });

    auto worker_view = create_view();
    worker_view->load_html("<script>new Worker('data:text/javascript,setInterval(() => {}, 1000)')</script>"sv);
    Optional<int> worker_client_id;
    Core::EventLoop::current().spin_until([&]() {
        WebView::WorkerProcessManager::the().for_each_request_server_site_bindings([&](auto& bindings) {
            worker_client_id = bindings.client_id();
            return worker_client_id.has_value() ? IterationDecision::Break : IterationDecision::Continue;
        });
        return worker_client_id.has_value();
    });
    expect_rejected("a navigation's response body fetched by a worker"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(*worker_client_id, 0));
    });

    outln("PASS: renderers cannot hand the UI process another renderer's navigation response body");
    return 0;
}
