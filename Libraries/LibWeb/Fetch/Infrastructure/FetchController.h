/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/WeakPtr.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/FetchControlBlock.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>

namespace Web::Fetch::Fetching {

class PendingResponse;

}

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-controller
class WEB_API FetchController : public JS::Cell {
    GC_CELL(FetchController, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchController);

public:
    using State = FetchControlBlock::State;

    [[nodiscard]] static GC::Ref<FetchController> create(FetchControllerRegistry&);

    [[nodiscard]] FetchControllerToken token() const { return m_token; }
    [[nodiscard]] NonnullRefPtr<FetchControlBlock> const& control_block() const { return m_control_block; }

    void set_report_timing_steps(Function<void(JS::Object&)> report_timing_steps);
    void set_next_manual_redirect_steps(Function<void()> next_manual_redirect_steps);

    [[nodiscard]] State state() const { return m_control_block->state(); }
    [[nodiscard]] RefPtr<FetchTimingInfo> full_timing_info() const { return m_control_block->full_timing_info(); }

    void report_timing(JS::Object&) const;
    void process_next_manual_redirect() const;
    [[nodiscard]] NonnullRefPtr<FetchTimingInfo> extract_full_timing_info() const;
    void abort(JS::Realm&, Optional<JS::Value>);
    Optional<HTML::IPCSerializationRecord> const& serialized_abort_reason() const { return m_serialized_abort_reason; }
    void terminate();

    void set_algorithms(Badge<FetchParams>, GC::Ref<FetchAlgorithms const>);
    [[nodiscard]] GC::Ref<FetchAlgorithms const> algorithms() const;

    [[nodiscard]] TaskDestination const& task_destination() const { return m_task_destination; }
    void set_task_destination(TaskDestination task_destination) { m_task_destination = move(task_destination); }

    [[nodiscard]] bool response_arrived() const { return m_control_block->response_arrived(); }
    [[nodiscard]] GC::Ptr<Fetching::PendingResponse> pending_preloaded_response() const { return m_pending_preloaded_response; }
    void set_pending_preloaded_response(GC::Ptr<Fetching::PendingResponse> pending_preloaded_response) { m_pending_preloaded_response = pending_preloaded_response; }

    void set_network_transport(RefPtr<Engine::NetworkTransportHandle> const&);
    bool requires_network() const { return m_control_block->requires_network(); }
    bool has_started_request() const { return m_control_block->has_started_request(); }
    void set_inner_fetch_controller(GC::Ref<FetchController>);

    void stop_fetch();
    void stop_request();
    void set_stop_steps(GC::Ref<GC::Function<void()>> stop_steps) { m_stop_steps = stop_steps; }

    u64 next_fetch_task_id() { return m_next_fetch_task_id++; }
    void fetch_task_queued(u64 fetch_task_id, HTML::TaskID event_id);
    void fetch_task_complete(u64 fetch_task_id);

private:
    explicit FetchController(FetchControllerRegistry&);

    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    WeakPtr<FetchControllerRegistry> m_registry;
    FetchControllerToken m_token;

    // NB: The state and the full timing info of this controller are kept in its control block.
    NonnullRefPtr<FetchControlBlock> m_control_block;

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // report timing steps (default null)
    //    Null or an algorithm accepting a global object.
    GC::Ptr<GC::Function<void(JS::Object&)>> m_report_timing_steps;

    // https://fetch.spec.whatwg.org/#fetch-controller-report-timing-steps
    // serialized abort reason (default null)
    //     Null or a Record (result of StructuredSerialize).
    Optional<HTML::IPCSerializationRecord> m_serialized_abort_reason;

    // https://fetch.spec.whatwg.org/#fetch-controller-next-manual-redirect-steps
    // next manual redirect steps (default null)
    //     Null or an algorithm accepting nothing.
    GC::Ptr<GC::Function<void()>> m_next_manual_redirect_steps;

    // https://fetch.spec.whatwg.org/#fetch-params-process-request-body
    // process request body chunk length (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-request-end-of-body
    // process request end-of-body (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-early-hints-response
    // process early hints response (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response
    // process response (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response-end-of-body
    // process response end-of-body (default null)
    // https://fetch.spec.whatwg.org/#fetch-params-process-response-consume-body
    // process response consume body (default null)
    //     Null or an algorithm.
    // NB: These are the algorithms of this controller's fetch params.
    GC::Ptr<FetchAlgorithms const> m_algorithms;

    // https://fetch.spec.whatwg.org/#fetch-params-task-destination
    // task destination (default null)
    //     Null, a global object, or a parallel queue.
    // NB: This is the task destination of this controller's fetch params.
    TaskDestination m_task_destination;

    // NB: Assumes one waiting consumer for a pending preloaded response.
    // Widen this if preload handoff ever supports multiple consumers.
    GC::Ptr<Fetching::PendingResponse> m_pending_preloaded_response;

    WeakPtr<Engine::NetworkTransportHandle> m_network_transport;

    HashMap<u64, HTML::TaskID> m_ongoing_fetch_tasks;
    u64 m_next_fetch_task_id { 0 };

    // AD-HOC: Run when the fetch is stopped, for whatever was waiting on its response.
    GC::Ptr<GC::Function<void()>> m_stop_steps;
};

class FetchControllerHolder : public JS::Cell {
    GC_CELL(FetchControllerHolder, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchControllerHolder);

public:
    static GC::Ref<FetchControllerHolder> create();

    [[nodiscard]] GC::Ptr<FetchController> const& controller() const { return m_controller; }
    void set_controller(GC::Ref<FetchController> controller) { m_controller = controller; }

private:
    FetchControllerHolder();

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<FetchController> m_controller;
};

}
