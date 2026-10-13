/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::Request;
using Web::Fetch::Infrastructure::ResponseView;

namespace {

struct FetchHarness {
    FetchHarness()
        : document(create_test_document())
        , realm(document->relevant_settings_object().realm())
    {
    }

    // A request whose scheme fetch returns a network error, so its response has no body.
    NonnullRefPtr<Request> create_request() const
    {
        return create_request_for("ladybird-test:network-error"sv);
    }

    NonnullRefPtr<Request> create_request_for(StringView url_string) const
    {
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());

        auto request = Request::create();
        request->set_url_list({ url.release_value() });
        request->set_client(document->relevant_settings_object().address());
        return request;
    }

    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch(Request& request, FetchAlgorithms::Input input) const
    {
        Web::HTML::TemporaryExecutionContext execution_context { *realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        return Web::Fetch::Fetching::fetch(*realm, request, FetchAlgorithms::create(move(input)));
    }

    // Runs the fetch's steps and the tasks they queue until the condition holds.
    void run_tasks_until(Function<bool()> condition) const
    {
        Web::HTML::main_thread_event_loop().spin_until(GC::create_function(realm->heap(), move(condition)));
    }

    GC::Ref<Web::DOM::Document> document;
    GC::Ref<JS::Realm> realm;
};

}

TEST_CASE(each_algorithm_gets_the_same_response_in_order)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    Vector<StringView> calls;
    Vector<ResponseView const*> responses;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = [&](GC::Ref<ResponseView> response) {
        calls.append("process response"sv);
        responses.append(response.ptr());
    };
    input.process_response_end_of_body = [&](GC::Ref<ResponseView> response) {
        calls.append("process response end-of-body"sv);
        responses.append(response.ptr());
    };
    input.process_response_consume_body = [&](GC::Ref<ResponseView> response, FetchAlgorithms::BodyBytes body) {
        calls.append("process response consume body"sv);
        responses.append(response.ptr());
        EXPECT(body.has<Empty>());
    };

    auto request = harness.create_request();
    (void)harness.fetch(request, move(input));
    harness.run_tasks_until([&] { return calls.size() == 3; });

    EXPECT_EQ(calls, (Vector<StringView> { "process response"sv, "process response end-of-body"sv, "process response consume body"sv }));
    VERIFY(responses.size() == 3);
    EXPECT(responses[0]->is_network_error());
    EXPECT_EQ(responses[1], responses[0]);
    EXPECT_EQ(responses[2], responses[0]);
}

TEST_CASE(each_algorithm_gets_the_same_response_with_a_body_in_order)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    Vector<StringView> calls;
    Vector<ResponseView const*> responses;
    Optional<ByteBuffer> consumed_bytes;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = [&](GC::Ref<ResponseView> response) {
        calls.append("process response"sv);
        responses.append(response.ptr());
    };
    input.process_response_end_of_body = [&](GC::Ref<ResponseView> response) {
        calls.append("process response end-of-body"sv);
        responses.append(response.ptr());
    };
    input.process_response_consume_body = [&](GC::Ref<ResponseView> response, FetchAlgorithms::BodyBytes body) {
        calls.append("process response consume body"sv);
        responses.append(response.ptr());
        if (auto const* bytes = body.get_pointer<Core::ImmutableBytes>())
            consumed_bytes = MUST(ByteBuffer::copy(bytes->bytes()));
    };

    auto request = harness.create_request_for("data:text/plain,hello"sv);
    (void)harness.fetch(request, move(input));
    harness.run_tasks_until([&] { return calls.size() == 3; });

    EXPECT_EQ(calls, (Vector<StringView> { "process response"sv, "process response end-of-body"sv, "process response consume body"sv }));
    VERIFY(responses.size() == 3);
    EXPECT(!responses[0]->is_network_error());
    EXPECT_EQ(responses[1], responses[0]);
    EXPECT_EQ(responses[2], responses[0]);
    VERIFY(consumed_bytes.has_value());
    EXPECT_EQ(*consumed_bytes, MUST(ByteBuffer::copy("hello"sv.bytes())));
}

TEST_CASE(a_stopped_fetch_runs_no_algorithm_but_its_request_is_done)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    size_t call_count = 0;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = [&](GC::Ref<ResponseView>) { ++call_count; };
    input.process_response_end_of_body = [&](GC::Ref<ResponseView>) { ++call_count; };
    input.process_response_consume_body = [&](GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { ++call_count; };

    auto request = harness.create_request();
    auto controller = harness.fetch(request, move(input));
    controller->stop_fetch();
    harness.run_tasks_until([&] { return request->done(); });

    EXPECT_EQ(call_count, 0u);
    EXPECT(request->done());
}
