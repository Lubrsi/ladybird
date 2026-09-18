/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
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
#include <LibWeb/Streams/ReadableStream.h>
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

    u64 allocate_token() { return m_next_token++; }

    void arm(u64 token, BodyStreamPullSource& source) { m_armed_sources.set(token, GC::make_root(source)); }
    void disarm(u64 token) { m_armed_sources.remove(token); }

    void notify(u64 token, u64 ticket)
    {
        auto it = m_armed_sources.find(token);
        if (it == m_armed_sources.end())
            return;
        if (it->value->on_wake(ticket) == BodyStreamPullSource::WakeResult::Consumed)
            m_armed_sources.remove(token);
    }

private:
    u64 m_next_token { 1 };
    HashMap<u64, GC::Root<BodyStreamPullSource>> m_armed_sources;
};

NonnullRefPtr<BodyStreamWakeSink> BodyStreamWakeSink::create()
{
    return adopt_ref(*new BodyStreamWakeSink(BodyStreamWakeRegistry::the().allocate_token()));
}

BodyStreamWakeSink::BodyStreamWakeSink(u64 registry_token)
    : m_loop(Core::EventLoop::current_weak())
    , m_registry_token(registry_token)
{
}

void BodyStreamWakeSink::wake(u64 ticket)
{
    auto loop = m_loop->take();
    if (!loop)
        return;
    loop->deferred_invoke([token = m_registry_token, ticket] {
        BodyStreamWakeRegistry::the().notify(token, ticket);
    });
}

BodyStreamPullSource::BodyStreamPullSource(GC::Ref<Infrastructure::FetchParams const> fetch_params, GC::Ref<Streams::ReadableStream> stream, NonnullRefPtr<Engine::FetchByteChannel> channel, BodyStreamWakeSink const& wake_sink)
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
    // NOTE: Resumption is the channel's credit replenishment, and the wait is its waiter, whose wake runs the
    //       queued fetch task in on_wake().
    VERIFY(!m_pending_pull_promise);
    m_pending_pull_promise = promise;
    if (!m_detached) {
        BodyStreamWakeRegistry::the().arm(m_registry_token, *this);
        m_expected_ticket = m_channel->arm_waiter();
    }

    // 3. Return promise.
    return promise;
}

GC::Ref<WebIDL::Promise> BodyStreamPullSource::cancel(JS::Realm& realm, JS::Value reason)
{
    m_channel->cancel();
    BodyStreamWakeRegistry::the().disarm(m_registry_token);
    m_detached = true;

    // A pull promise created before the cancellation still settles.
    if (auto promise = exchange(m_pending_pull_promise, nullptr))
        WebIDL::resolve_promise(realm, *promise, JS::js_undefined());

    // cancelAlgorithm: abort fetchParams's controller with reason, given reason.
    m_fetch_params->controller()->abort(realm, reason);
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

BodyStreamPullSource::WakeResult BodyStreamPullSource::on_wake(u64 ticket)
{
    if (!m_pending_pull_promise)
        return WakeResult::Consumed;
    // This waiter's wake is still owed; the source stays rooted for it.
    if (ticket != m_expected_ticket)
        return WakeResult::StillAwaited;

    // AD-HOC: A sync XHR send() pauses the HTML event loop while it waits for exactly these bytes, so the fetch task
    //         that would carry them cannot run until it returns. Run the pull now instead; the read request it
    //         fulfills settles synchronously (see the parallel-queue path in fetch_response_handover()).
    if (HTML::main_thread_event_loop().execution_paused()) {
        run_pull_task();
        return WakeResult::Consumed;
    }

    // 3. Queue a fetch task to run the following steps, with fetchParams’s task destination:
    // NOTE: Queued without controller tracking: FetchController::stop_fetch() deletes tracked
    //       tasks outright, which would strand this pull's promise. The task observes the
    //       channel's state instead.
    Infrastructure::queue_fetch_task(m_fetch_params->task_destination(), GC::create_function(heap(), [this] {
        run_pull_task();
    }));
    return WakeResult::Consumed;
}

// A failed transmission errors the stream regardless of read demand, and an empty completed body closes without
// one; with a pull pending, the wake carries the ending to run_pull_task() instead.
void BodyStreamPullSource::handle_network_settled()
{
    if (m_pending_pull_promise)
        return;

    if (HTML::main_thread_event_loop().execution_paused()) {
        deliver_while_paused();
        return;
    }

    auto& realm = m_stream->realm();
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto result = m_channel->take_up_to(0);
    switch (result.state) {
    case Engine::FetchByteChannel::State::Open:
    case Engine::FetchByteChannel::State::ConsumerCancelled:
        break;
    case Engine::FetchByteChannel::State::Closed:
        // Buffered bytes stay available to future pulls; only an already-drained body closes now.
        if (result.buffered_byte_count == 0 && m_stream->is_readable()) {
            m_stream->close();
            m_detached = true;
        }
        break;
    case Engine::FetchByteChannel::State::Errored: {
        auto message = result.error.has_value() ? Utf16String::from_utf8(StringView { result.error->message }) : "Load failed"_utf16;
        if (m_stream->is_readable())
            m_stream->error(JS::TypeError::create(realm, message));
        m_detached = true;
        break;
    }
    }
}

// AD-HOC: A sync XHR send() pauses the HTML event loop while it waits for exactly these bytes, and a stream set up
//         under that pause never starts pulling: the start promise's reaction is a microtask. So while the loop is
//         paused, bytes are pushed into the stream as they arrive, the way the specification's in-parallel steps
//         would deliver them (see the parallel-queue path in fetch_response_handover()).
void BodyStreamPullSource::deliver_while_paused()
{
    // A pending pull's wake carries these bytes to run_pull_task() itself.
    if (m_detached || m_pending_pull_promise)
        return;

    drain_into_stream();
}

void BodyStreamPullSource::run_pull_task()
{
    auto promise = exchange(m_pending_pull_promise, nullptr);
    if (!promise)
        return;

    drain_into_stream();

    // 3. Fulfill promise with undefined.
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

    // 1. Pull from bytes into stream.
    if (!result.taken.is_empty()) {
        ByteBuffer bytes;
        for (auto const& taken : result.taken)
            bytes.append(taken.bytes());
        if (auto pull_result = m_stream->pull_from_bytes(move(bytes)); pull_result.is_error()) {
            // A failed enqueue ends the whole body.
            auto error = WebIDL::exception_to_throw_completion(realm.vm(), realm, pull_result.release_error());
            if (m_stream->is_readable())
                m_stream->error(error.value());
            m_fetch_params->controller()->terminate();
            m_channel->cancel();
            m_detached = true;
            return;
        }
    }

    // 2. If stream is errored, then terminate fetchParams’s controller.
    if (m_stream->is_errored())
        m_fetch_params->controller()->terminate();

    switch (result.state) {
    case Engine::FetchByteChannel::State::Open:
        break;
    case Engine::FetchByteChannel::State::Closed:
        // Closes the stream once its buffered bytes have been drained through the pulls above.
        if (result.buffered_byte_count == 0 && m_stream->is_readable()) {
            m_stream->close();
            m_detached = true;
        }
        break;
    case Engine::FetchByteChannel::State::Errored: {
        auto message = result.error.has_value() ? Utf16String::from_utf8(StringView { result.error->message }) : "Load failed"_utf16;
        if (m_stream->is_readable())
            m_stream->error(JS::TypeError::create(realm, message));
        m_detached = true;
        break;
    }
    case Engine::FetchByteChannel::State::ConsumerCancelled:
        m_detached = true;
        break;
    }
}

}
