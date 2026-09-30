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
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/HTML/NavigationSourceSnapshot.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/Page/NavigationTarget.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalNavigation.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/HeadlessWebView.h>
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
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

struct RecordedSettings {
    Web::HTML::EnvironmentId id;
    URL::URL creation_url;
    URL::Origin origin;
    URL::URL top_level_creation_url;
    URL::Origin top_level_origin;
    bool has_cross_site_ancestor { false };
    Web::HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability { Web::HTML::CanUseCrossOriginIsolatedAPIs::No };
    u64 agent_cluster_id { 0 };
};

RecordedSettings recorded_settings_of_active_document(WebView::CanonicalNavigable const& navigable)
{
    auto const& document = navigable.active_document();
    auto const& settings = document.relevant_global_object().relevant_settings_object();
    return {
        .id = settings.id(),
        .creation_url = document.creation_url(),
        .origin = settings.origin(),
        .top_level_creation_url = settings.top_level_creation_url().value(),
        .top_level_origin = settings.top_level_origin().value(),
        .has_cross_site_ancestor = settings.has_cross_site_ancestor(),
        .cross_origin_isolated_capability = settings.cross_origin_isolated_capability(),
        .agent_cluster_id = settings.agent_cluster_id(),
    };
}

}

// A navigation renderers start or request has the UI process's record of its source document's environment: its origin,
// top-level creation URL, top-level origin, cross-site ancestor, agent cluster and cross-origin isolated capability.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestRendererSettingsClaims-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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

    bool failed = false;
    auto fail = [&](StringView what, StringView how) {
        warnln("FAIL: {} {}", what, how);
        failed = true;
    };

    auto victim_url = URL::Parser::basic_parse("https://victim.example/"sv).release_value();
    auto no_source_document = Web::HTML::create_navigation_source_snapshot_without_a_source_document();

    auto recorded_fetch_client = [&](RecordedSettings const& recorded) {
        return Web::HTML::SerializedEnvironmentSettingsObject {
            .id = recorded.id,
            .creation_url = recorded.creation_url,
            .top_level_creation_url = recorded.top_level_creation_url,
            .top_level_origin = recorded.top_level_origin,
            .api_base_url = recorded.creation_url,
            .origin = recorded.origin,
            .has_cross_site_ancestor = recorded.has_cross_site_ancestor,
            .policy_container = no_source_document.source_policy_container,
            .cross_origin_isolated_capability = recorded.cross_origin_isolated_capability,
            .agent_cluster_id = recorded.agent_cluster_id,
            .time_origin = 0,
            .global = Web::HTML::SerializedWindow { .associated_document = { .url = recorded.creation_url, .relevant_settings_object_is_secure_context = false } },
        };
    };
    auto forged_fetch_client = [&](RecordedSettings const& recorded) {
        auto fetch_client = recorded_fetch_client(recorded);
        fetch_client.origin = victim_url.origin();
        fetch_client.top_level_creation_url = victim_url;
        fetch_client.top_level_origin = victim_url.origin();
        fetch_client.has_cross_site_ancestor = !recorded.has_cross_site_ancestor;
        fetch_client.cross_origin_isolated_capability = recorded.cross_origin_isolated_capability == Web::HTML::CanUseCrossOriginIsolatedAPIs::Yes
            ? Web::HTML::CanUseCrossOriginIsolatedAPIs::No
            : Web::HTML::CanUseCrossOriginIsolatedAPIs::Yes;
        fetch_client.agent_cluster_id = recorded.agent_cluster_id + 1;
        return fetch_client;
    };
    auto source_snapshot_params_with = [&](Web::HTML::SerializedEnvironmentSettingsObject fetch_client) {
        auto source_snapshot_params = no_source_document;
        source_snapshot_params.fetch_client = move(fetch_client);
        return source_snapshot_params;
    };
    auto generate_navigation_id = [] {
        auto uuid = generate_random_uuid();
        return Utf16String::from_utf8_without_validation(uuid.bytes());
    };

    // A process requests population only for a navigation without a source document, which has no fetch client.
    {
        auto population_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        Optional<WebView::ViewImplementation::WebContentCrashReason> crash_reason;
        population_view->on_web_content_crashed = [&](auto reason) { crash_reason = reason; };

        auto& population_traversable = population_view->traversable();
        Web::HTML::NavigationPopulationRequest request {
            .navigable_id = population_traversable.id(),
            .history_entry = Web::HTML::create_pending_session_history_entry_descriptor(Web::HTML::create_initial_session_history_entry_descriptor({}, {}, {})),
            .source_snapshot_params = source_snapshot_params_with(recorded_fetch_client(recorded_settings_of_active_document(population_traversable))),
            .target_snapshot_params = {},
            .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
            .history_handling = Web::Bindings::NavigationHistoryBehavior::Replace,
            .user_involvement = Web::HTML::UserNavigationInvolvement::None,
            .navigation_id = generate_navigation_id(),
        };
        auto& population_stub = static_cast<WebContentClientStub&>(population_view->client());
        population_stub.did_request_navigation_population(population_view->page_id(), population_traversable.id(), Web::NavigationTarget::TopLevel, move(request));
        Core::EventLoop::current().spin_until([&] { return crash_reason.has_value(); });
        if (crash_reason != WebView::ViewImplementation::WebContentCrashReason::RejectedIPC)
            fail("a population request with a fetch client"sv, "is not rejected"sv);
    }

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 1; });

    view->load_html(R"~~~(<!DOCTYPE html><iframe sandbox srcdoc="sandboxed"></iframe><iframe></iframe><iframe></iframe>)~~~"sv);
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 2; });
    auto& traversable = view->traversable();
    Core::EventLoop::current().spin_until([&] {
        return traversable.children().size() == 3 && !traversable.children().first()->active_document().is_initial_about_blank();
    });
    auto& sandboxed_frame = *traversable.children()[0];
    auto& remote_frame = *traversable.children()[1];
    auto& lost_frame = *traversable.children()[2];

    auto& stub = static_cast<WebContentClientStub&>(view->client());
    auto page_id = view->page_id();

    // A sandboxed frame's document has a cross-site ancestor.
    if (!recorded_settings_of_active_document(sandboxed_frame).has_cross_site_ancestor)
        fail("the sandboxed frame's document"sv, "has no cross-site ancestor"sv);

    // Another process holds a page for the tab, and hosts the document of one of its frames.
    auto other_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto& other_client = other_view->client();
    VERIFY(&other_client != &view->client());
    traversable.represent_group_in(other_client);
    auto other_page_id = other_client.page_id_for_traversable(traversable).value();
    remote_frame.active_document().set_host(other_client.page(other_page_id));
    auto& other_stub = static_cast<WebContentClientStub&>(other_client);

    auto start_navigation = [&](WebContentClientStub& sender, Web::PageId sender_page_id, WebView::CanonicalNavigable& navigable, Utf16String const& navigation_id, URL::Origin initiator_origin, Web::HTML::SerializedEnvironmentSettingsObject fetch_client) {
        Web::HTML::NavigationStartRequest request {
            .navigable_id = navigable.id(),
            .url = URL::about_blank(),
            .document_resource = {},
            .request_referrer = Web::Fetch::Infrastructure::RequestReferrer::Client,
            .request_referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
            .initiator_origin = move(initiator_origin),
            .initiator_base_url = {},
            .navigable_target_name = {},
            .source_snapshot_params = source_snapshot_params_with(move(fetch_client)),
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
        sender.did_request_navigation_start(sender_page_id, navigable.id(), Web::NavigationTarget::IFrame, URL::about_blank(), navigation_id, move(request));
    };

    auto request_navigation_of_navigable = [&](WebView::CanonicalNavigable& navigable, Utf16String const& navigation_id, URL::Origin initiator_origin, Web::HTML::SerializedEnvironmentSettingsObject fetch_client) {
        Web::HTML::PreparedNavigationDescriptor navigation {
            .url = URL::about_blank(),
            .document_resource = {},
            .history_handling = Web::Bindings::NavigationHistoryBehavior::Replace,
            .navigation_api_state = {},
            .referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
            .user_involvement = Web::HTML::UserNavigationInvolvement::None,
            .navigation_id = navigation_id,
            .initial_insertion = Web::HTML::InitialInsertion::No,
            .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
            .source_snapshot_params = source_snapshot_params_with(move(fetch_client)),
            .initiator_origin_snapshot = move(initiator_origin),
            .initiator_base_url_snapshot = URL::about_blank(),
        };
        stub.did_request_navigation_of_navigable(page_id, navigable.id(), move(navigation));
    };

    auto is_ongoing_navigation = [](WebView::CanonicalNavigable const& navigable, Utf16String const& navigation_id) {
        auto const& ongoing_navigation = navigable.ongoing_navigation();
        return ongoing_navigation.has_value() && ongoing_navigation->navigation_id == navigation_id && ongoing_navigation->start_request.has_value();
    };

    auto expect_recorded_source = [&](StringView what, WebView::CanonicalNavigable const& navigable, RecordedSettings const& recorded) {
        auto const& start_request = *navigable.ongoing_navigation()->start_request;
        if (start_request.initiator_origin != recorded.origin)
            fail(what, "has the claimed initiator origin"sv);

        auto const& fetch_client = start_request.source_snapshot_params.fetch_client;
        if (!fetch_client.has_value()) {
            fail(what, "has no fetch client"sv);
            return;
        }
        if (fetch_client->id != recorded.id)
            fail(what, "has another environment as its fetch client"sv);
        if (fetch_client->origin != recorded.origin)
            fail(what, "has the claimed origin"sv);
        if (fetch_client->top_level_creation_url != recorded.top_level_creation_url)
            fail(what, "has the claimed top-level creation URL"sv);
        if (fetch_client->top_level_origin != recorded.top_level_origin)
            fail(what, "has the claimed top-level origin"sv);
        if (fetch_client->has_cross_site_ancestor != recorded.has_cross_site_ancestor)
            fail(what, "has the claimed cross-site ancestor"sv);
        if (fetch_client->agent_cluster_id != recorded.agent_cluster_id)
            fail(what, "has the claimed agent cluster"sv);
        if (fetch_client->cross_origin_isolated_capability != recorded.cross_origin_isolated_capability)
            fail(what, "has the claimed cross-origin isolated capability"sv);
    };

    // A navigation from a claimed source goes on from the recorded one, or is refused while one from the recorded source
    // goes on.
    auto expect_claims_replaced_or_refused = [&](StringView what, WebView::CanonicalNavigable& navigable, RecordedSettings const& recorded, Function<void(Utf16String const&, URL::Origin, Web::HTML::SerializedEnvironmentSettingsObject)> const& send) {
        auto navigation_id = generate_navigation_id();
        send(navigation_id, victim_url.origin(), forged_fetch_client(recorded));
        if (is_ongoing_navigation(navigable, navigation_id)) {
            expect_recorded_source(what, navigable, recorded);
            return;
        }
        navigation_id = generate_navigation_id();
        send(navigation_id, recorded.origin, recorded_fetch_client(recorded));
        if (!is_ongoing_navigation(navigable, navigation_id))
            fail(what, "does not start, even from the recorded source"sv);
    };

    expect_claims_replaced_or_refused("a navigation a frame starts"sv, sandboxed_frame, recorded_settings_of_active_document(sandboxed_frame), [&](auto const& navigation_id, auto initiator_origin, auto fetch_client) {
        start_navigation(stub, page_id, sandboxed_frame, navigation_id, move(initiator_origin), move(fetch_client));
    });

    // The UI process navigates a frame whose document was lost with the process hosting it, for the page requesting it.
    lost_frame.did_lose_active_document();
    expect_claims_replaced_or_refused("a navigation of a frame whose document was lost"sv, lost_frame, recorded_settings_of_active_document(traversable), [&](auto const& navigation_id, auto initiator_origin, auto fetch_client) {
        request_navigation_of_navigable(lost_frame, navigation_id, move(initiator_origin), move(fetch_client));
    });

    auto recorded = recorded_settings_of_active_document(traversable);

    // A process names only an environment it hosts as the source of a navigation it starts on its own.
    auto unrequested_navigation_id = generate_navigation_id();
    start_navigation(other_stub, other_page_id, remote_frame, unrequested_navigation_id, recorded.origin, recorded_fetch_client(recorded));
    if (is_ongoing_navigation(remote_frame, unrequested_navigation_id))
        fail("a navigation from an environment another process hosts"sv, "starts"sv);

    // The process hosting a frame's document starts the navigation another process requests of the frame from the
    // source held for it, whatever the process claims.
    auto requested_navigation_id = generate_navigation_id();
    request_navigation_of_navigable(remote_frame, requested_navigation_id, victim_url.origin(), forged_fetch_client(recorded));
    start_navigation(other_stub, other_page_id, remote_frame, requested_navigation_id, victim_url.origin(), forged_fetch_client(recorded));
    if (is_ongoing_navigation(remote_frame, requested_navigation_id))
        expect_recorded_source("a navigation requested of a frame another process hosts"sv, remote_frame, recorded);
    else
        fail("a navigation requested of a frame another process hosts"sv, "does not start"sv);

    if (failed)
        return 1;

    outln("PASS: navigations go on with the UI process's record of their source, whatever renderers claim");
    return 0;
}
