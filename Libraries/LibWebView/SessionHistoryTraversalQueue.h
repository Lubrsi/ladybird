/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/NavigationType.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWebView/Export.h>

namespace WebView {

// Result of the "check if unloading is canceled" phase of the traversal protocol.
// Maps to the spec's return values from "checking if unloading is canceled".
enum class TraversalUnloadingCheckResult : u8 {
    Continue = 0,
    CanceledByBeforeUnload = 1,
    CanceledByNavigate = 2,
    InitiatorDisallowed = 3,
};

// A traversal command that the UI process orchestrates via the multi-phase IPC protocol.
// Corresponds to "apply the traverse history step" in the spec.
struct TraversalCommand {
    i32 target_step { 0 };

    bool check_for_cancelation { true };
    Optional<Web::Bindings::NavigationType> navigation_type;
    Web::HTML::UserNavigationInvolvement user_involvement { Web::HTML::UserNavigationInvolvement::BrowserUI };

    // For script-initiated traversals: opaque handle referencing a (SourceSnapshotParams, Navigable)
    // pair held in WebContent. Neither type is serializable (they contain GC references), so
    // WebContent stores them locally and the UI sends this ID back when executing the traversal.
    // Empty for browser-UI initiated traversals.
    Optional<u64> source_snapshot_and_initiator_id;

    // Pre-computed by UI at dequeue time using serialized session history functions:
    Vector<String> changing_navigable_ids;
    Vector<String> non_changing_navigable_ids;
    size_t script_history_length { 0 };
    size_t script_history_index { 0 };

    // For Navigation::traverseTo cancel propagation: opaque ID referencing a cancel callback
    // held in WebContent. If Phase B cancels, UI sends traversal_canceled with this ID.
    Optional<u64> cancel_callback_id;
};

// A synchronous navigation step that can jump the queue during traversal processing.
// These correspond to "append session history synchronous navigation steps" in the spec.
// WebContent holds the actual operation closure, referenced by operation_id.
struct SynchronousNavigationCommand {
    u64 operation_id { 0 };
    String target_navigable_id;
};

// An asynchronous operation that gets forwarded to WebContent for execution.
// Used for close and iframe readiness operations that don't go through the phase protocol.
// WebContent holds the actual operation closure, referenced by operation_id.
struct AsyncOperationCommand {
    u64 operation_id { 0 };
};

// A prep-then-apply command for operations that use the UI-orchestrated phase protocol.
// After prep, WC sends typed parameters and UI drives the phase protocol (B → S → CD → E → F).
struct PrepAndApplyCommand {
    u64 prep_operation_id { 0 };
};

using SessionHistoryCommand = Variant<TraversalCommand, SynchronousNavigationCommand, AsyncOperationCommand, PrepAndApplyCommand>;

// UI-side session history traversal queue.
// Commands are enqueued from WebContent (via IPC) or from the UI itself, and processed sequentially.
// This mirrors the spec's "session history traversal queue" but lives in the UI process so it
// survives WebContent process switches.
class WEBVIEW_API SessionHistoryTraversalQueue {
public:
    void append(SessionHistoryCommand);
    void clear() { m_queue.clear(); }
    bool is_empty() const { return m_queue.is_empty(); }
    size_t size() const { return m_queue.size(); }

    Optional<SessionHistoryCommand> dequeue();

    // For synchronous navigation queue-jumping during traversal processing (spec step 14 of
    // "apply the history step"). Returns and removes the first SynchronousNavigationCommand whose
    // target_navigable_id is NOT in the excluded set.
    Optional<SynchronousNavigationCommand> take_first_synchronous_navigation_not_targeting(HashTable<String> const& excluded_navigable_ids);

private:
    Vector<SessionHistoryCommand> m_queue;
};

}
