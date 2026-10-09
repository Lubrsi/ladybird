/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
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

// Answers every request with the same response, pausing after the first pause_after bytes of its body when given.
class TestHttpServer {
public:
    TestHttpServer(ByteString headers, ByteBuffer body, Optional<size_t> pause_after)
        : m_headers(move(headers))
        , m_body(move(body))
        , m_pause_after(pause_after)
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
        (void)Core::System::close(m_socket);
    }

    URL::URL url() const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/file", m_port)).release_value();
    }

    size_t request_count() const { return m_request_count.load(); }

private:
    void run()
    {
        while (!m_shutting_down.load()) {
            auto client = ::accept(m_socket, nullptr, nullptr);
            if (client < 0)
                return;
            respond(client);
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

    void respond(int client)
    {
        StringBuilder request;
        Array<u8, 4096> buffer;
        while (!request.string_view().contains("\r\n\r\n"sv)) {
            auto read = Core::System::read(client, buffer);
            if (read.is_error() || read.value() == 0)
                return;
            request.append(StringView { buffer.span().trim(read.value()) });
        }
        m_request_count.fetch_add(1);

        auto head = ByteString::formatted("HTTP/1.1 200 OK\r\n{}Content-Length: {}\r\nConnection: close\r\n\r\n", m_headers, m_body.size());
        if (!write_all(client, head.bytes()))
            return;

        auto body = m_body.bytes();
        if (m_pause_after.has_value()) {
            if (!write_all(client, body.trim(*m_pause_after)))
                return;
            MUST(Core::System::sleep_ms(100));
            body = body.slice(*m_pause_after);
        }
        (void)write_all(client, body);
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

    ByteString m_headers;
    ByteBuffer m_body;
    Optional<size_t> m_pause_after;
    int m_socket { -1 };
    u16 m_port { 0 };
    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_shutting_down { false };
    Atomic<size_t> m_request_count { 0 };
};

class FinishedDownloadRecorder final : public WebView::FileDownloaderObserver {
public:
    virtual void download_updated(WebView::FileDownloader::Download const& download) override
    {
        if (!WebView::FileDownloader::status_is_active(download.status))
            m_finished = download;
    }

    Optional<WebView::FileDownloader::Download> const& finished() const { return m_finished; }

private:
    Optional<WebView::FileDownloader::Download> m_finished;
};

ByteBuffer make_body(size_t size)
{
    auto body = MUST(ByteBuffer::create_uninitialized(size));
    for (size_t i = 0; i < size; ++i)
        body[i] = static_cast<u8>((i * 31 + (i >> 8) * 17) & 0xff);
    return body;
}

WebView::FileDownloader::Download download_navigation_response(Core::AnonymousBuffer const& theme, TestHttpServer const& server)
{
    FinishedDownloadRecorder recorder;

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    view->load(server.url());
    Core::EventLoop::current().spin_until([&] { return recorder.finished().has_value(); });
    return *recorder.finished();
}

void expect_download_holds_body(StringView what, WebView::FileDownloader::Download const& download, ByteBuffer const& body)
{
    if (download.status != WebView::FileDownloader::DownloadStatus::Completed) {
        warnln("FAIL: {}: the download ended with status {}", what, to_underlying(download.status));
        VERIFY_NOT_REACHED();
    }

    auto file = MUST(Core::File::open(download.destination.string(), Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());
    if (contents.bytes() != body.bytes()) {
        warnln("FAIL: {}: the file holds {} bytes, not the {} the server sent", what, contents.size(), body.size());
        VERIFY_NOT_REACHED();
    }
}

void expect_navigation_downloads_body(Core::AnonymousBuffer const& theme, StringView what, ByteString headers, ByteBuffer const& body, Optional<size_t> pause_after = {})
{
    TestHttpServer server { move(headers), body, pause_after };
    expect_download_holds_body(what, download_navigation_response(theme, server), body);
}

void expect_navigation_downloads_cached_body(Core::AnonymousBuffer const& theme, StringView what, ByteString headers, ByteBuffer const& body)
{
    TestHttpServer server { move(headers), body, {} };
    expect_download_holds_body(what, download_navigation_response(theme, server), body);

    // The disk cache finishes its entry some time after the first download completes.
    for (size_t attempt = 0; attempt < 20; ++attempt) {
        auto request_count = server.request_count();
        expect_download_holds_body(what, download_navigation_response(theme, server), body);
        if (server.request_count() == request_count)
            return;
        MUST(Core::System::sleep_ms(250));
    }
    warnln("FAIL: {}: no download came from the disk cache", what);
    VERIFY_NOT_REACHED();
}

}

// A navigation response that becomes a download is handed back to the UI process and saved byte for byte, whether no
// byte of it was read first or its first bytes were read to sniff its type, also when those bytes arrive in parts or
// the disk cache serves the response.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestNavigationDownloads-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed));
    });
    VERIFY(setenv("XDG_CONFIG_HOME", test_directory.characters(), 1) == 0);
    VERIFY(setenv("XDG_CACHE_HOME", test_directory.characters(), 1) == 0);
    VERIFY(setenv("XDG_DOWNLOAD_DIR", test_directory.characters(), 1) == 0);

    signal(SIGPIPE, SIG_IGN);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto body = make_body(1 * MiB);
    expect_navigation_downloads_body(theme, "an attachment"sv, "Content-Disposition: attachment; filename=\"attachment.bin\"\r\n"sv, body);
    expect_navigation_downloads_body(theme, "a response sniffed as unrenderable"sv, "Content-Type: application/octet-stream\r\n"sv, body);
    expect_navigation_downloads_body(theme, "a response sniffed from bytes that arrive in parts"sv, "Content-Type: application/octet-stream\r\n"sv, body, 500);
    expect_navigation_downloads_cached_body(theme, "a response sniffed from the disk cache"sv, "Cache-Control: max-age=3600\r\nContent-Type: application/octet-stream\r\n"sv, body);

    outln("PASS: navigation responses download byte for byte");
    return 0;
}
