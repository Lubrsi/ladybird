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
};

// A traversal command that the UI process orchestrates via the multi-phase IPC protocol.
// Corresponds to "apply the traverse history step" in the spec.
struct TraversalCommand {
    i32 target_step { 0 };

    // If set, target_step is resolved from current_step + delta at dequeue time.
    // Used for script-initiated delta traversals (history.back/forward) where the target
    // must be computed relative to the step that is current when this command actually runs.
    Optional<i32> delta;

    bool check_for_cancelation { true };
    Optional<Web::Bindings::NavigationType> navigation_type;
    Web::HTML::UserNavigationInvolvement user_involvement { Web::HTML::UserNavigationInvolvement::BrowserUI };

    // For script-initiated traversals: opaque handle referencing a (SourceSnapshotParams, Navigable)
    // pair held in WebContent. Neither type is serializable (they contain GC references), so
    // WebContent stores them locally and the UI sends this ID back when executing the traversal.
    // Empty for browser-UI initiated traversals.
    Optional<u64> source_snapshot_and_initiator_id;
};

// A synchronous navigation step that can jump the queue during traversal processing.
// These correspond to "append session history synchronous navigation steps" in the spec.
// WebContent holds the actual operation closure, referenced by operation_id.
struct SynchronousNavigationCommand {
    u64 operation_id { 0 };
    String target_navigable_id;
};

// An asynchronous operation that gets forwarded to WebContent for execution.
// Used for navigation finalizations, navigable creation/destruction, reload, close, etc.
// WebContent holds the actual operation closure, referenced by operation_id.
struct AsyncOperationCommand {
    u64 operation_id { 0 };
};

using SessionHistoryCommand = Variant<TraversalCommand, SynchronousNavigationCommand, AsyncOperationCommand>;

// UI-side session history traversal queue.
// Commands are enqueued from WebContent (via IPC) or from the UI itself, and processed sequentially.
// This mirrors the spec's "session history traversal queue" but lives in the UI process so it
// survives WebContent process switches.
class WEBVIEW_API SessionHistoryTraversalQueue {
public:
    void append(SessionHistoryCommand);
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
