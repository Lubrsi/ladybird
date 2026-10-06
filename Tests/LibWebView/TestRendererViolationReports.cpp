/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalNavigation.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>

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

struct Page {
    ByteString headers;
    ByteString body;
};

// Answers each path with its page, and leaves a request for /held unanswered while the server lives.
class TestHttpServer {
public:
    explicit TestHttpServer(HashMap<ByteString, Page> pages)
        : m_pages(move(pages))
    {
        m_socket = MUST(Core::System::socket(AF_INET, SOCK_STREAM, 0));

        int reuse_address = 1;
        MUST(Core::System::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        MUST(Core::System::bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
        MUST(Core::System::listen(m_socket, 16));

        socklen_t address_size = sizeof(address);
        VERIFY(::getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
        m_port = ntohs(address.sin_port);

        m_thread = Threading::Thread::construct("TestHttpServer"sv, [this] -> intptr_t {
            run();
            return 0;
        });
        m_thread->start();
    }

    ~TestHttpServer()
    {
        m_shutting_down.store(true);
        wake_server();
        (void)m_thread->join();
        for (auto client : m_held_clients)
            (void)Core::System::close(client);
        (void)Core::System::close(m_socket);
    }

    URL::URL url(StringView host, StringView path) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://{}:{}{}", host, m_port, path)).release_value();
    }

private:
    void run()
    {
        while (!m_shutting_down.load()) {
            auto client = ::accept(m_socket, nullptr, nullptr);
            if (client < 0)
                return;
            if (respond(client))
                m_held_clients.append(client);
            else
                (void)Core::System::close(client);
        }
    }

    void wake_server()
    {
        auto socket = Core::System::socket(AF_INET, SOCK_STREAM, 0);
        if (socket.is_error())
            return;

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(m_port);
        (void)::connect(socket.value(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
        (void)Core::System::close(socket.value());
    }

    // Returns whether the request is held, unanswered.
    bool respond(int client)
    {
        StringBuilder request;
        Array<u8, 4096> buffer;
        while (!request.string_view().contains("\r\n\r\n"sv)) {
            auto read = Core::System::read(client, buffer);
            if (read.is_error() || read.value() == 0)
                return false;
            request.append(StringView { buffer.span().trim(read.value()) });
        }

        auto request_line = request.string_view().split_view("\r\n"sv).first().split_view(' ');
        if (request_line.size() < 2)
            return false;
        auto path = request_line[1];
        if (path == "/held"sv)
            return true;

        auto page = m_pages.get(path);
        auto response = page.has_value()
            ? ByteString::formatted("HTTP/1.1 200 OK\r\n{}Content-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", page->headers, page->body.length(), page->body)
            : ByteString { "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"sv };
        (void)write_all(client, response.bytes());
        return false;
    }

    static bool write_all(int fd, ReadonlyBytes bytes)
    {
        while (!bytes.is_empty()) {
            auto written = Core::System::write(fd, bytes);
            if (written.is_error() || written.value() == 0)
                return false;
            bytes = bytes.slice(written.value());
        }
        return true;
    }

    HashMap<ByteString, Page> m_pages;
    Vector<int> m_held_clients;
    int m_socket { -1 };
    u16 m_port { 0 };
    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_shutting_down { false };
};

bool spin_until_or_timeout(Function<bool()> condition, int timeout_ms)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(timeout_ms), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return condition() || timed_out; });
    return condition();
}

void expect(bool condition, StringView what)
{
    if (condition)
        return;
    warnln("FAIL: {}", what);
    VERIFY_NOT_REACHED();
}

// The liar's page holds a child frame whose document has the given environment's id.
void claim_environment(WebView::HeadlessWebView& liar, Web::HTML::EnvironmentId const& environment_id, u64 namespace_id)
{
    auto& stub = static_cast<WebContentClientStub&>(liar.page().client());
    auto& traversable = liar.traversable();

    Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry;
    initial_history_entry.url = URL::about_blank();
    initial_history_entry.document_state.id = { .namespace_id = namespace_id, .local_id = 1 };

    Web::HTML::CrossProcessId frame_id { .namespace_id = namespace_id, .local_id = 2 };
    Web::HTML::CrossProcessId document_id { .namespace_id = namespace_id, .local_id = 3 };
    stub.did_create_child_frame(liar.page().id(), traversable.id(), traversable.active_document().id(), frame_id, {}, move(initial_history_entry), URL::Origin::create_opaque(), environment_id, document_id);
    expect(liar.page().hosted_environment(environment_id).has_value(), "a liar claims the source's environment"sv);
}

}

// A violation of a policy of a navigation's source is reported in the page the UI process found hosting the source
// document when the navigation was requested, whichever other pages claim the source's environment. The navigation's
// population worker is terminated for a report the navigation cannot have; a report from another page, or for a
// navigation the worker is not running, is dropped.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestRendererViolationReports-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

    signal(SIGPIPE, SIG_IGN);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    // The source's policy allows its frame's own site only. Each load of the frame and each violation retitles it.
    HashMap<ByteString, Page> pages;
    pages.set("/source"sv, {
                               .headers = "Content-Security-Policy: frame-src http://target.test:*\r\n",
                               .body = R"(<!DOCTYPE html><body><script>
        let loads = 0;
        document.addEventListener("securitypolicyviolation", event => (document.title = `violation: ${event.effectiveDirective}`));
        const frame = document.createElement("iframe");
        frame.name = "target";
        frame.onload = () => (document.title = `loaded ${++loads}`);
        frame.src = `http://target.test:${location.port}/target`;
        document.body.append(frame);
        function navigateTarget(host, path) {
            const link = document.createElement("a");
            link.target = "target";
            link.href = `http://${host}:${location.port}${path}`;
            document.body.append(link);
            link.click();
        }
    </script>)",
                           });
    pages.set("/target"sv, { .headers = {}, .body = "<!DOCTYPE html>target" });
    TestHttpServer server { move(pages) };

    Vector<NonnullOwnPtr<WebView::HeadlessWebView>> liars;
    auto create_liar = [&] {
        auto liar = WebView::HeadlessWebView::create(theme, { 800, 600 });
        bool loaded = false;
        liar->on_load_finish = [&](auto const&) { loaded = true; };
        liar->load(server.url("liar.test"sv, "/target"sv));
        expect(spin_until_or_timeout([&] { return loaded; }, 10'000), "a liar's page loads"sv);
        liar->on_load_finish = {};
        liars.append(move(liar));
    };

    // Liars are created both before and after the source.
    create_liar();
    create_liar();
    auto source_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    create_liar();
    create_liar();

    Vector<ByteString> titles;
    source_view->on_title_change = [&](auto const& title) { titles.append(title.to_utf8().to_byte_string()); };
    auto saw_title = [&](StringView title) { return titles.contains_slow(title); };

    source_view->load(server.url("source.test"sv, "/source"sv));
    expect(spin_until_or_timeout([&] { return saw_title("loaded 1"sv); }, 10'000), "the source's frame loads"sv);

    auto& target = *source_view->traversable().children().first();
    auto environment_id = source_view->traversable().active_document().relevant_global_object().relevant_settings_object().id();
    for (size_t i = 0; i < liars.size(); ++i)
        claim_environment(*liars[i], environment_id, 0xfeed0000 + i);

    source_view->run_javascript("navigateTarget('blocked.test', '/blocked')"_string);
    expect(spin_until_or_timeout([&] { return saw_title("violation: frame-src"sv) && saw_title("loaded 2"sv); }, 10'000), "the source sees the violation of its frame-src"sv);

    source_view->run_javascript("navigateTarget('target.test', '/held')"_string);
    RefPtr<WebView::CanonicalNavigation> held_navigation;
    expect(spin_until_or_timeout([&] {
        held_navigation = target.uncommitted_navigation();
        return held_navigation && held_navigation->state.has<WebView::CanonicalNavigation::CreatingNavigationParams>();
    },
               10'000),
        "the frame's navigation is fetching"sv);

    // The page running a navigation's population is its worker.
    auto worker = held_navigation->page;
    auto& source_page = source_view->page();
    expect(worker && &worker->client() != &source_page.client(), "another renderer runs the frame's navigation"sv);

    auto report = [&](WebView::WebContentPage& page, Utf16String navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective directive) {
        auto& stub = static_cast<WebContentClientStub&>(page.client());
        stub.did_report_navigation_violation(page.id(), target.id(), move(navigation_id), Web::ContentSecurityPolicy::PolicyId { 1 }, directive);
    };

    report(*worker, "not-the-navigation"_utf16, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc);
    expect(worker->client().is_open(), "a report for another navigation is dropped"sv);

    report(source_page, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc);
    expect(source_page.client().is_open(), "a report from a page that is not the navigation's worker is dropped"sv);

    report(*worker, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::WorkerSrc);
    expect(!worker->client().is_open(), "the worker is terminated for a directive the navigation cannot violate"sv);

    outln("PASS: violation reports reach the source's page, and impossible reports terminate their renderer");
    return 0;
}
