/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/FontPlugin.h>

using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::Request;
using Web::Fetch::Infrastructure::ResponseView;

namespace {

void install_font_plugin()
{
    static Web::Platform::FontPlugin font_plugin { false };
    static bool installed = false;
    if (installed)
        return;
    Web::Platform::FontPlugin::install(font_plugin);
    installed = true;
}

GC::Ref<Web::DOM::Document> create_document()
{
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& page = Web::Bindings::principal_host_defined_page(*principal_realm);
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page.set_top_level_traversable(traversable);
    return *traversable->active_document();
}

struct FetchHarness {
    FetchHarness()
        : document(create_document())
        , realm(document->relevant_settings_object().realm())
    {
    }

    // A request whose scheme fetch returns a network error, so its response has no body.
    NonnullRefPtr<Request> create_request() const
    {
        auto url = URL::Parser::basic_parse("ladybird-test:network-error"sv);
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

static NEVER_INLINE void start_fetch_without_holding_its_controller(FetchHarness const& harness, Request& request, FetchAlgorithms::Input input)
{
    (void)harness.fetch(request, move(input));
}

static NEVER_INLINE void run_a_fetch_whose_controller_nothing_holds()
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    bool consumed_body = false;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response_consume_body = [&](GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { consumed_body = true; };

    // A worker's script fetch has no fetch record, so only its own steps hold its controller.
    auto request = harness.create_request();
    request->set_destination(Request::Destination::Worker);
    start_fetch_without_holding_its_controller(harness, request, move(input));
    GC::scrub_stack_for_testing();
    harness.run_tasks_until([&] {
        harness.realm->heap().collect_garbage();
        return consumed_body;
    });

    EXPECT(consumed_body);
}

TEST_CASE(a_fetch_whose_controller_nothing_holds_still_runs_its_algorithms)
{
    GC::scrub_stack_for_testing();
    run_a_fetch_whose_controller_nothing_holds();
}
