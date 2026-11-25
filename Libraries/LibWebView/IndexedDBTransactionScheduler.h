/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>
#include <LibWebView/IndexedDBErrorKind.h>

namespace WebView {

// https://w3c.github.io/IndexedDB/#transaction-construct
// The transactions of one database: which of them may start, and what each of them is to do next.
//
// The scheduler does none of the work. Whoever owns it asks for the next thing to do, does it, says how it went, and
// asks again. It is one thing at a time, since how a thing goes can take away what would have come after it.
class WEBVIEW_API IndexedDBTransactionScheduler {
    AK_MAKE_NONCOPYABLE(IndexedDBTransactionScheduler);
    AK_MAKE_NONMOVABLE(IndexedDBTransactionScheduler);

public:
    AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, TransactionID);

    // https://w3c.github.io/IndexedDB/#transaction-mode
    enum class Mode : u8 {
        ReadOnly,
        ReadWrite,
    };

    // What a transaction is asked to do, in the order in which it was asked.
    enum class EntryKind : u8 {
        // https://w3c.github.io/IndexedDB/#request-construct
        // Something with a result, which whoever asked for it is told.
        Request,
        // A change to the object stores of the database, which has no result. One that fails ends its transaction.
        Change,
    };

    struct Begin {
        TransactionID transaction;
    };

    struct Run {
        TransactionID transaction;
        u64 place { 0 };
    };

    struct Commit {
        TransactionID transaction;
    };

    // The cause is there when the transaction is not ended at the wish of whoever made it.
    struct RollBack {
        TransactionID transaction;
        Optional<IndexedDBErrorKind> cause;
    };

    using Action = Variant<Begin, Run, Commit, RollBack>;

    IndexedDBTransactionScheduler() = default;

    // The scope is the ids of the transaction's object stores. Transactions count as created in the order of these
    // calls.
    TransactionID create_transaction(Mode, Vector<i64> scope);

    // https://w3c.github.io/IndexedDB/#upgrade-a-database
    // An upgrade transaction has the database to itself and has begun by the time that it is made known here.
    TransactionID create_upgrade_transaction();

    // Returns the place of the entry in the queue of its transaction, or nothing when the transaction was asked to
    // commit, after which it takes no more entries.
    Optional<u64> place(TransactionID, EntryKind);

    // Returns false for what a transaction cannot be asked: to commit twice, or with more results started on than
    // it has had.
    [[nodiscard]] bool ask_to_commit(TransactionID, u64 results_started);

    void ask_to_abort(TransactionID);

    // Ends a transaction for a cause that its owner found, such as the loss of its writer.
    void abort(TransactionID, IndexedDBErrorKind cause);

    Optional<Action> next_action();

    void did_begin(TransactionID);
    void did_fail_to_begin(TransactionID, IndexedDBErrorKind);
    void did_run(TransactionID, u64 place, Optional<IndexedDBErrorKind> failure);
    void did_commit(TransactionID);
    void did_fail_to_commit(TransactionID, IndexedDBErrorKind);
    void did_roll_back(TransactionID);

private:
    enum class State : u8 {
        Waiting,
        Beginning,
        Started,
    };

    struct FailedResult {
        u64 result { 0 };
        IndexedDBErrorKind cause { IndexedDBErrorKind::Unknown };
    };

    struct Transaction {
        AK_ALLOC_WITH_KMALLOC;

        bool writes() const { return is_upgrade || mode == Mode::ReadWrite; }
        bool overlaps(Transaction const&) const;
        void end_with(Optional<IndexedDBErrorKind> cause);

        TransactionID id;
        Mode mode { Mode::ReadOnly };
        bool is_upgrade { false };
        Vector<i64> scope;
        State state { State::Waiting };

        Queue<EntryKind> entries_to_run;
        u64 entries_placed { 0 };
        u64 entries_run { 0 };
        u64 results_run { 0 };
        Vector<FailedResult> failed_results;

        Optional<u64> results_started_at_commit;

        bool is_to_be_rolled_back { false };
        Optional<IndexedDBErrorKind> cause_of_roll_back;
    };

    Transaction& transaction(TransactionID);
    Transaction& add_transaction(Mode, Vector<i64> scope);
    void remove_transaction(TransactionID);
    bool can_start(Transaction const&) const;

    Action await_report_of(Action);
    template<typename ActionType>
    void take_report_of(TransactionID);

    // In the order in which they were created. One that has ended is not kept.
    Vector<NonnullOwnPtr<Transaction>> m_transactions;
    u64 m_last_transaction_id { 0 };

    Optional<Action> m_action_awaiting_its_report;
};

}
