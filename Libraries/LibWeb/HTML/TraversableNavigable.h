/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationType.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageShed.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibGfx/VulkanContext.h>
#endif

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#traversable-navigable
class WEB_API TraversableNavigable final : public Navigable {
    GC_CELL(TraversableNavigable, Navigable);
    GC_DECLARE_ALLOCATOR(TraversableNavigable);

public:
    static WebIDL::ExceptionOr<GC::Ref<TraversableNavigable>> create_a_new_top_level_traversable(GC::Ref<Page>, GC::Ptr<BrowsingContext> opener, String target_name);
    static WebIDL::ExceptionOr<GC::Ref<TraversableNavigable>> create_a_fresh_top_level_traversable(GC::Ref<Page>, URL::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> = Empty {});

    virtual ~TraversableNavigable() override;

    virtual bool is_top_level_traversable() const override;

    int current_session_history_step() const { return m_current_session_history_step; }
    Vector<GC::Ref<SessionHistoryEntry>>& session_history_entries() { return m_session_history_entries; }
    Vector<GC::Ref<SessionHistoryEntry>> const& session_history_entries() const { return m_session_history_entries; }
    void restore_session_history(i32 current_step, Vector<WebView::SerializedSessionHistoryEntry> entries);

    VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(VisibilityState);

    bool is_created_by_web_content() const { return m_is_created_by_web_content; }
    void set_is_created_by_web_content(bool value) { m_is_created_by_web_content = value; }

    struct HistoryObjectLengthAndIndex {
        u64 script_history_length;
        u64 script_history_index;
    };
    HistoryObjectLengthAndIndex get_the_history_object_length_and_index(int) const;

    enum class HistoryStepResult {
        InitiatorDisallowed,
        CanceledByBeforeUnload,
        CanceledByNavigate,
    };

    // AD-HOC: Dedicated function for same-document navigations (pushState, replaceState, fragment).
    // Implements the relevant subset of "apply the history step" (steps 2, 6-8, 12, 14-21)
    // directly inline with no spins. Same-document navigations always hit the fast path
    // (displayedEntry == targetEntry → update_only = true), so no cross-process coordination
    // or document population is needed. This avoids the full phase protocol's IPC round-trips,
    // which would deadlock when queue-jumping (spec step 14.1) runs these steps inline during
    // an active traversal.
    void apply_the_history_step_for_same_document_navigation(int step, Optional<Bindings::NavigationType> navigation_type, UserNavigationInvolvement);

    int get_the_used_step(int step) const;
    Vector<GC::Root<Navigable>> get_all_navigables_whose_current_session_history_entry_will_change_or_reload(int) const;
    Vector<GC::Root<Navigable>> get_all_navigables_that_only_need_history_object_length_index_update(int) const;
    Vector<GC::Root<Navigable>> get_all_navigables_that_might_experience_a_cross_document_traversal(int) const;

    Vector<int> get_all_used_history_steps() const;
    void clear_the_forward_session_history();
    void traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document = {});

    void close_top_level_traversable();
    void definitely_close_top_level_traversable();
    void destroy_top_level_traversable();

    // Store a (SourceSnapshotParams, Navigable) pair for later retrieval during traversal execution.
    // Returns an opaque ID that the UI sends back when executing the traversal.
    u64 store_source_snapshot_and_initiator(GC::Ptr<SourceSnapshotParams>, GC::Ptr<Navigable>);
    struct SourceSnapshotAndInitiator {
        GC::Ptr<SourceSnapshotParams> source_snapshot_params;
        GC::Ptr<Navigable> initiator;
    };
    Optional<SourceSnapshotAndInitiator> take_source_snapshot_and_initiator(u64 id);
    Optional<SourceSnapshotAndInitiator> get_source_snapshot_and_initiator(u64 id) const;

    // Store an operation closure for later execution via IPC round-trip.
    // Returns an operation ID that the UI sends back when it's time to execute.
    u64 store_session_history_operation(GC::Ref<GC::Function<NonnullRefPtr<Core::Promise<Empty>>()>> closure);
    GC::Ptr<GC::Function<NonnullRefPtr<Core::Promise<Empty>>()>> take_session_history_operation(u64 id);

    // Store a prep closure for the prep-and-apply protocol and notify the UI to enqueue a PrepAndApplyCommand.
    // The prep closure does operation-specific setup, then sends parameters via IPC.
    void store_session_history_prep(GC::Ref<GC::Function<void()>> closure);
    GC::Ptr<GC::Function<void()>> take_session_history_prep(u64 id);

    // Cancel callback infrastructure for Navigation::traverseTo.
    // When Phase B cancels, the UI sends back the cancel_callback_id and reason.
    u64 store_cancel_callback(GC::Ref<GC::Function<void(HistoryStepResult)>> callback);
    void run_cancel_callback(u64 id, HistoryStepResult reason);

    String window_handle() const { return m_window_handle; }
    void set_window_handle(String window_handle) { m_window_handle = move(window_handle); }

    [[nodiscard]] GC::Ptr<DOM::Node> currently_focused_area();

    // Resolve a set of navigable IDs (from UI-side pre-computation) to live Navigable objects.
    Vector<GC::Ref<Navigable>> resolve_navigable_ids(Vector<String> const& ids);

    enum class CheckIfUnloadingIsCanceledResult {
        CanceledByBeforeUnload,
        CanceledByNavigate,
        Continue,
    };
    CheckIfUnloadingIsCanceledResult check_if_unloading_is_canceled(Vector<GC::Root<Navigable>> navigables_that_need_before_unload);

    // Phase B: Check if unloading is canceled (spec steps 2-5 of "apply the history step").
    void traversal_check_if_unloading_is_canceled(int step, GC::Ptr<SourceSnapshotParams>, GC::Ptr<Navigable> initiator, UserNavigationInvolvement, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> on_complete);
    // Phase CD setup: Set current session history entry and ongoing navigation for all changing navigables (spec step 8).
    void traversal_setup_changing_navigables(int step, Vector<String> changing_navigable_ids);
    // Phase CD per-navigable: Populate document and activate entry for one navigable (spec steps 12+14 per-navigable).
    void traversal_process_navigable(String navigable_id, int step, GC::Ptr<SourceSnapshotParams>, UserNavigationInvolvement, Optional<Bindings::NavigationType>, size_t script_history_length, size_t script_history_index, GC::Ref<GC::Function<void()>> on_complete);
    // Phase E: Update non-changing navigables (spec steps 15-19).
    void traversal_update_non_changing_navigables(Vector<String> non_changing_navigable_ids, size_t script_history_length, size_t script_history_index, GC::Ref<GC::Function<void()>> on_complete);

    // Push current session history state to the UI process for serialized history computation.
    void push_session_history_to_ui();

    StorageAPI::StorageShed& storage_shed() { return m_storage_shed; }
    StorageAPI::StorageShed const& storage_shed() const { return m_storage_shed; }

    // https://w3c.github.io/geolocation/#dfn-emulated-position-data
    Geolocation::EmulatedPositionData const& emulated_position_data() const;
    void set_emulated_position_data(Geolocation::EmulatedPositionData data);

    void process_screenshot_requests();
    void queue_screenshot_task(Optional<UniqueNodeID> node_id)
    {
        m_screenshot_tasks.enqueue({ node_id });
        set_needs_repaint();
    }

private:
    TraversableNavigable(GC::Ref<Page>);

    virtual bool is_traversable() const override { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

    CheckIfUnloadingIsCanceledResult check_if_unloading_is_canceled(Vector<GC::Root<Navigable>> navigables_that_need_before_unload, GC::Ptr<TraversableNavigable> traversable, Optional<int> target_step, Optional<UserNavigationInvolvement> user_involvement_for_navigate_events);

    Vector<GC::Ref<SessionHistoryEntry>> get_session_history_entries_for_the_navigation_api(GC::Ref<Navigable>, int);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    int m_current_session_history_step { 0 };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
    Vector<GC::Ref<SessionHistoryEntry>> m_session_history_entries;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#system-visibility-state
    VisibilityState m_system_visibility_state { VisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-created-by-web-content
    bool m_is_created_by_web_content { false };

    // https://storage.spec.whatwg.org/#traversable-navigable-storage-shed
    // A traversable navigable holds a storage shed, which is a storage shed. A traversable navigable’s storage shed holds all session storage data.
    GC::Ref<StorageAPI::StorageShed> m_storage_shed;

    // Storage for source snapshot + initiator pairs, keyed by opaque IDs.
    // Used for script-initiated traversals where GC objects can't cross IPC.
    u64 m_next_source_snapshot_id { 1 };
    HashMap<u64, SourceSnapshotAndInitiator> m_source_snapshot_map;

    // Storage for operation closures, keyed by opaque IDs.
    // WebContent stores closures here, sends the ID to UI, and UI sends it back to execute.
    u64 m_next_operation_id { 1 };
    HashMap<u64, GC::Ref<GC::Function<NonnullRefPtr<Core::Promise<Empty>>()>>> m_operation_map;

    // Storage for prep closures (prep-and-apply protocol), keyed by opaque IDs.
    u64 m_next_prep_id { 1 };
    HashMap<u64, GC::Ref<GC::Function<void()>>> m_prep_map;

    // Cancel callbacks for Navigation::traverseTo promise rejection.
    u64 m_next_cancel_callback_id { 1 };
    HashMap<u64, GC::Ref<GC::Function<void(HistoryStepResult)>>> m_cancel_callback_map;

    String m_window_handle;

    // https://w3c.github.io/geolocation/#dfn-emulated-position-data
    Geolocation::EmulatedPositionData m_emulated_position_data;

    struct ScreenshotTask {
        Optional<Web::UniqueNodeID> node_id;
    };
    Queue<ScreenshotTask> m_screenshot_tasks;
};

struct BrowsingContextAndDocument {
    GC::Ref<HTML::BrowsingContext> browsing_context;
    GC::Ref<DOM::Document> document;
};

WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page);
void finalize_a_same_document_navigation(GC::Ref<TraversableNavigable> traversable, GC::Ref<Navigable> target_navigable, GC::Ref<SessionHistoryEntry> target_entry, GC::Ptr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior, UserNavigationInvolvement);

template<>
inline bool Navigable::fast_is<TraversableNavigable>() const { return is_traversable(); }

}
