/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/Fetch/Fetching/BodyStreamPullSource.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(BodyStreamPullSource);

// Roots a pull source only while a wake is owed; tokens are never reused, and the consumer loop must outlive every
// source with a wake owed.
class BodyStreamWakeRegistry {
public:
    static BodyStreamWakeRegistry& the()
    {
        static thread_local NeverDestroyed<BodyStreamWakeRegistry> registry;
        return *registry;
    }

    BodyStreamWakeToken allocate_token() { return BodyStreamWakeToken { m_next_token++ }; }

    void arm(BodyStreamWakeToken token, BodyStreamPullSource& source)
    {
        m_armed_sources.ensure(token, [&] { return GC::make_root(source); });
    }
    void disarm(BodyStreamWakeToken token) { m_armed_sources.remove(token); }

    void notify(BodyStreamWakeToken token, Engine::WakeTicket ticket)
    {
        auto it = m_armed_sources.find(token);
        if (it == m_armed_sources.end())
            return;
        if (it->value->on_wake(ticket) == BodyStreamPullSource::WakeResult::Consumed)
            m_armed_sources.remove(token);
    }

private:
    u64 m_next_token { 1 };
    HashMap<BodyStreamWakeToken, GC::Root<BodyStreamPullSource>> m_armed_sources;
};

NonnullRefPtr<BodyStreamWakeSink> BodyStreamWakeSink::create()
{
    return adopt_ref(*new BodyStreamWakeSink(BodyStreamWakeRegistry::the().allocate_token()));
}

BodyStreamWakeSink::BodyStreamWakeSink(BodyStreamWakeToken registry_token)
    : m_loop(Core::EventLoop::current_weak())
    , m_registry_token(registry_token)
{
}

void BodyStreamWakeSink::wake(Engine::WakeTicket ticket)
{
    auto loop = m_loop->take();
    if (!loop)
        return;
    loop->deferred_invoke([token = m_registry_token, ticket] {
        BodyStreamWakeRegistry::the().notify(token, ticket);
    });
}

BodyStreamPullSource::BodyStreamPullSource(GC::Ptr<Infrastructure::FetchParams const> fetch_params, GC::Ref<Streams::ReadableStream> stream, NonnullRefPtr<Engine::FetchByteChannel> channel, BodyStreamWakeSink const& wake_sink)
    : m_fetch_params(fetch_params)
    , m_stream(stream)
    , m_channel(move(channel))
    , m_registry_token(wake_sink.registry_token())
{
}

BodyStreamPullSource::~BodyStreamPullSource() = default;

void BodyStreamPullSource::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_params);
    visitor.visit(m_stream);
    visitor.visit(m_pending_pull_promise);
}

// This implements the pullAlgorithm of HTTP-network fetch.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
GC::Ref<WebIDL::Promise> BodyStreamPullSource::pull(JS::Realm& realm)
{
    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Run the following steps in parallel:
    //    1. If the size of buffer is smaller than a lower limit chosen by the user agent and the
    //       ongoing fetch is suspended, resume the fetch.
    //    2. Wait until buffer is not empty.
    VERIFY(!m_pending_pull_promise);
    m_pending_pull_promise = promise;
    if (!m_detached) {
        if (auto push_ticket = exchange(m_push_ticket, {}); push_ticket.has_value())
            m_channel->disarm_waiter(*push_ticket);
        m_pull_ticket = arm_waiter(Engine::FetchByteChannel::BufferedAtLeast { 1 });
    }

    // 3. Return promise.
    return promise;
}

GC::Ref<WebIDL::Promise> BodyStreamPullSource::cancel(JS::Realm& realm, JS::Value reason)
{
    m_channel->cancel();
    BodyStreamWakeRegistry::the().disarm(m_registry_token);
    m_detached = true;
    m_end_ticket = {};
    m_pull_ticket = {};
    m_push_ticket = {};

    // A pull promise created before the cancellation still settles.
    if (auto promise = exchange(m_pending_pull_promise, nullptr))
        WebIDL::resolve_promise(realm, *promise, JS::js_undefined());

    // cancelAlgorithm: abort fetchParams's controller with reason, given reason.
    if (m_fetch_params)
        m_fetch_params->controller()->abort(realm, reason);
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

void BodyStreamPullSource::watch_channel()
{
    m_end_ticket = arm_waiter(Engine::FetchByteChannel::EndOnly {});
    push_while_paused();
}

Engine::WakeTicket BodyStreamPullSource::arm_waiter(Engine::FetchByteChannel::WakeOn wake_on)
{
    BodyStreamWakeRegistry::the().arm(m_registry_token, *this);
    return m_channel->arm_waiter(wake_on);
}

BodyStreamPullSource::WakeResult BodyStreamPullSource::on_wake(Engine::WakeTicket ticket)
{
    if (ticket == m_pull_ticket) {
        m_pull_ticket = {};
        on_pull_wake();
    } else if (ticket == m_push_ticket) {
        m_push_ticket = {};
        on_push_wake();
    } else if (ticket == m_end_ticket) {
        m_end_ticket = {};
        on_end_wake();
    }

    // The source stays rooted while any of its waiters is still owed a wake.
    if (m_end_ticket.has_value() || m_pull_ticket.has_value() || m_push_ticket.has_value())
        return WakeResult::StillAwaited;
    return WakeResult::Consumed;
}

void BodyStreamPullSource::on_pull_wake()
{
    if (!m_pending_pull_promise)
        return;

    // AD-HOC: While a sync XHR send() pauses the HTML event loop, the pull runs now rather than in a fetch task.
    if (HTML::main_thread_event_loop().execution_paused()) {
        run_pull_task();
        push_while_paused();
        return;
    }

    // 3. Queue a fetch task to run the following steps, with fetchParams’s task destination:
    // NB: This task stays queued when the fetch is stopped.
    auto steps = GC::create_function(heap(), [this] {
        run_pull_task();
    });
    if (m_fetch_params) {
        Infrastructure::queue_fetch_task(m_fetch_params->task_destination(), steps);
        return;
    }

    // AD-HOC: The tasks of a navigation body adopted from another process are queued without a document.
    HTML::queue_a_task(HTML::Task::Source::Networking, HTML::main_thread_event_loop(), nullptr, steps);
}

// AD-HOC: While a sync XHR send() pauses the HTML event loop, its bytes are pushed into its stream as they arrive.
void BodyStreamPullSource::push_while_paused()
{
    if (m_detached || m_pending_pull_promise || m_push_ticket.has_value() || !HTML::main_thread_event_loop().execution_paused())
        return;
    if (!m_fetch_params || m_fetch_params->algorithms()->body_intent() != Engine::BodyIntent::SyncAccumulate)
        return;
    m_push_ticket = arm_waiter(Engine::FetchByteChannel::BufferedAtLeast { 1 });
}

void BodyStreamPullSource::on_push_wake()
{
    if (m_detached || m_pending_pull_promise)
        return;
    drain_into_stream();
    push_while_paused();
}

// A pending pull or the push while paused carries the body's end itself.
void BodyStreamPullSource::on_end_wake()
{
    if (m_detached || m_pending_pull_promise || m_push_ticket.has_value())
        return;
    settle_without_reading();
}

// A failed transmission errors the stream regardless of read demand, and an empty completed body closes without one.
void BodyStreamPullSource::settle_without_reading()
{
    auto& realm = m_stream->realm();
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    settle_stream(m_channel->take_up_to(0));
}

// A closed channel closes the stream once its buffered bytes are drained, and an errored one errors it.
void BodyStreamPullSource::settle_stream(Engine::FetchByteChannel::TakeResult const& result)
{
    switch (result.state) {
    case Engine::FetchByteChannel::State::Open:
    case Engine::FetchByteChannel::State::ConsumerCancelled:
    case Engine::FetchByteChannel::State::Transferred:
        break;
    case Engine::FetchByteChannel::State::Closed:
        if (result.buffered_byte_count == 0 && m_stream->is_readable()) {
            m_stream->close();
            m_detached = true;
        }
        break;
    case Engine::FetchByteChannel::State::Errored: {
        auto message = result.error.has_value() ? Utf16String::from_utf8(StringView { result.error->message }) : "Load failed"_utf16;
        if (m_stream->is_readable())
            m_stream->error(JS::TypeError::create(m_stream->realm(), message));
        m_detached = true;
        break;
    }
    }
}

void BodyStreamPullSource::run_pull_task()
{
    auto promise = exchange(m_pending_pull_promise, nullptr);
    if (!promise)
        return;

    drain_into_stream();

    // 3. Resolve promise with undefined.
    auto& realm = m_stream->realm();
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    WebIDL::resolve_promise(realm, *promise, JS::js_undefined());
}

void BodyStreamPullSource::drain_into_stream()
{
    auto& realm = m_stream->realm();
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // The slices removed by one take reach the stream as one byte sequence, and a BYOB pull may consume only the
    // current view's length.
    auto result = [&] {
        if (auto byob_view = m_stream->current_byob_request_view(); byob_view.has_value())
            return m_channel->take_up_to(byob_view->byte_length());
        return m_channel->take_all();
    }();

    // 1. Pull from bytes buffer into stream.
    if (!result.taken.is_empty()) {
        ByteBuffer bytes;
        for (auto const& taken : result.taken)
            bytes.append(taken.bytes());
        auto pull_result = [&] {
            // NB: Without a BYOB request, pulling from bytes enqueues the bytes as they are.
            if (!m_stream->current_byob_request_view().has_value()) {
                auto controller = m_stream->controller()->get<GC::Ref<Streams::ReadableByteStreamController>>();
                return Streams::readable_byte_stream_controller_enqueue_native_bytes(realm, controller, move(bytes));
            }
            return m_stream->pull_from_bytes(move(bytes));
        }();
        if (pull_result.is_error()) {
            // A failed enqueue ends the whole body.
            auto error = WebIDL::exception_to_throw_completion(realm.vm(), realm, pull_result.release_error());
            if (m_stream->is_readable())
                m_stream->error(error.value());
            if (m_fetch_params)
                m_fetch_params->controller()->terminate();
            m_channel->cancel();
            m_detached = true;
            return;
        }
    }

    // 2. If stream is errored, then terminate fetchParams’s controller.
    if (m_stream->is_errored() && m_fetch_params)
        m_fetch_params->controller()->terminate();

    settle_stream(result);
    if (result.state == Engine::FetchByteChannel::State::ConsumerCancelled || result.state == Engine::FetchByteChannel::State::Transferred)
        m_detached = true;
}

// https://fetch.spec.whatwg.org/#concept-http-network-fetch
// NB: Steps 10 to 13 of HTTP-network fetch.
GC::Ref<Streams::ReadableStream> create_body_stream_from_channel(JS::Realm& realm, GC::Ptr<Infrastructure::FetchParams const> fetch_params, NonnullRefPtr<Engine::FetchByteChannel> channel)
{
    auto wake_sink = BodyStreamWakeSink::create();
    channel->attach_consumer(wake_sink);

    // 10. Let stream be a new ReadableStream.
    auto stream = GC::Heap::the().allocate<Streams::ReadableStream>();
    auto pull_source = GC::Heap::the().allocate<BodyStreamPullSource>(fetch_params, stream, move(channel), *wake_sink);

    // 11. Let pullAlgorithm be the following steps:
    auto pull_algorithm = GC::create_function(GC::Heap::the(), [&realm, pull_source]() {
        return pull_source->pull(realm);
    });

    // 12. Let cancelAlgorithm be an algorithm that aborts fetchParams’s controller with reason, given reason.
    auto cancel_algorithm = GC::create_function(GC::Heap::the(), [&realm, pull_source](JS::Value reason) {
        return pull_source->cancel(realm, reason);
    });

    // 13. Set up stream with byte reading support with pullAlgorithm set to pullAlgorithm, cancelAlgorithm set to cancelAlgorithm.
    stream->set_up_with_byte_reading_support(realm, pull_algorithm, cancel_algorithm);
    pull_source->watch_channel();

    return stream;
}

}
