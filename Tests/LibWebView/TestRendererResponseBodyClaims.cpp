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
#include <LibCore/StandardPaths.h>
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
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

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
// it. A renderer naming another live renderer's client is terminated, and any other client's body is left alone.

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
    auto victim_client_id = victim_view->client().request_server_client_id();

    auto view = create_view();
    auto& stub = static_cast<WebContentClientStub&>(view->client());
    auto page_id = view->page_id();
    auto navigable_id = view->client().page(page_id)->traversable().id();
    auto own_client_id = view->client().request_server_client_id();
    VERIFY(own_client_id != victim_client_id);

    // A renderer's own response body is not misbehavior.
    stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(own_client_id, 0));
    stub.did_finish_history_navigation_params_creation(page_id, app->allocate_ui_process_cross_process_id(), { .request = {}, .result = result_with_response_body_of(own_client_id, 0) });
    VERIFY(view->client().is_open());

    // Nor is the body of a client no live renderer holds, such as one from before RequestServer restarted. The UI
    // process takes none of it, whether the result is discarded or continues a navigation in flight.
    auto new_client = MUST(WebView::connect_new_request_server_client(WebView::Application::default_session()));
    auto other_client = make_ref_counted<Requests::RequestClient>(MUST(new_client.handle.create_transport()));
    auto unreachable_url = URL::Parser::basic_parse("http://127.0.0.1:9/"sv).release_value();
    auto other_request = other_client->start_request("GET"sv, unreachable_url, {}, {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, Requests::RequestClient::TransferLease::Yes);
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
    };
    stub.did_request_navigation_population(page_id, navigable_id, Web::NavigationTarget::TopLevel, Web::HTML::create_navigation_population_request(move(start_request), app->allocate_ui_process_cross_process_id()));
    stub.did_finish_navigation_params_creation(page_id, navigable_id, navigation_id, other_result());
    VERIFY(view->client().is_open());

    // Every adoption above has reached RequestServer once the UI process's own client is answered.
    (void)WebView::Application::request_server_client().send_sync<Messages::RequestServer::GetClientId>();
    VERIFY(other_client->send_sync<Messages::RequestServer::StopRequest>(other_request->id())->success());

    auto expect_rejected = [&](StringView what, Function<void(WebContentClientStub&, Web::PageId, Web::HTML::CrossProcessId)> send) {
        auto view = create_view();
        Optional<WebView::ViewImplementation::WebContentCrashReason> crash_reason;
        view->on_web_content_crashed = [&](auto reason) { crash_reason = reason; };

        auto page_id = view->page_id();
        send(static_cast<WebContentClientStub&>(view->client()), page_id, view->client().page(page_id)->traversable().id());
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

    outln("PASS: renderers cannot hand the UI process another renderer's navigation response body");
    return 0;
}
