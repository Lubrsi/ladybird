/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWebView/IndexedDBTransactionScheduler.h>

namespace WebView {

// https://w3c.github.io/IndexedDB/#transaction-construct
// Two transactions have overlapping scope if any object store is in both transactions' scope.
bool IndexedDBTransactionScheduler::Transaction::overlaps(Transaction const& other) const
{
    if (is_upgrade || other.is_upgrade)
        return true;

    // Both scopes are sorted.
    size_t index = 0;
    size_t index_in_other = 0;
    while (index < scope.size() && index_in_other < other.scope.size()) {
        if (scope[index] == other.scope[index_in_other])
            return true;

        if (scope[index] < other.scope[index_in_other])
            ++index;
        else
            ++index_in_other;
    }

    return false;
}

// A transaction ends once, for the first reason that there was.
void IndexedDBTransactionScheduler::Transaction::end_with(Optional<IndexedDBErrorKind> cause)
{
    if (is_to_be_rolled_back)
        return;

    is_to_be_rolled_back = true;
    cause_of_roll_back = cause;
}

IndexedDBTransactionScheduler::Transaction& IndexedDBTransactionScheduler::transaction(TransactionID id)
{
    for (auto& transaction : m_transactions) {
        if (transaction->id == id)
            return *transaction;
    }
    VERIFY_NOT_REACHED();
}

IndexedDBTransactionScheduler::Transaction& IndexedDBTransactionScheduler::add_transaction(Mode mode, Vector<i64> scope)
{
    quick_sort(scope);

    auto transaction = make<Transaction>();
    transaction->id = TransactionID { ++m_last_transaction_id };
    transaction->mode = mode;
    transaction->scope = move(scope);

    m_transactions.append(move(transaction));
    return *m_transactions.last();
}

void IndexedDBTransactionScheduler::remove_transaction(TransactionID id)
{
    m_transactions.remove_first_matching([&](auto const& transaction) { return transaction->id == id; });
}

IndexedDBTransactionScheduler::TransactionID IndexedDBTransactionScheduler::create_transaction(Mode mode, Vector<i64> scope)
{
    return add_transaction(mode, move(scope)).id;
}

IndexedDBTransactionScheduler::TransactionID IndexedDBTransactionScheduler::create_upgrade_transaction()
{
    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // NB: An upgrade runs once every other connection to its database is closed, which each is when its
    //     transactions are complete.
    VERIFY(m_transactions.is_empty());

    auto& upgrade = add_transaction(Mode::ReadWrite, {});
    upgrade.is_upgrade = true;
    upgrade.state = State::Started;
    return upgrade.id;
}

Optional<u64> IndexedDBTransactionScheduler::place(TransactionID id, EntryKind kind)
{
    auto& transaction = this->transaction(id);
    if (transaction.results_started_at_commit.has_value())
        return {};

    transaction.entries_to_run.enqueue(kind);
    return transaction.entries_placed++;
}

bool IndexedDBTransactionScheduler::ask_to_commit(TransactionID id, u64 results_started)
{
    auto& transaction = this->transaction(id);
    if (transaction.results_started_at_commit.has_value())
        return false;
    if (results_started > transaction.results_run)
        return false;

    transaction.results_started_at_commit = results_started;

    // https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
    // 5.3. If result is an error and transaction's state is committing, then run abort a transaction with transaction
    //      and result, and terminate these steps.
    // NB: Whoever made the transaction asked for the commit before it started on these results, so their requests
    //     were processed while the transaction was committing as far as it can tell. The first of them that failed
    //     is the one that the step is taken for.
    for (auto const& failed_result : transaction.failed_results) {
        if (failed_result.result >= results_started) {
            transaction.end_with(failed_result.cause);
            break;
        }
    }
    transaction.failed_results.clear();

    return true;
}

void IndexedDBTransactionScheduler::ask_to_abort(TransactionID id)
{
    transaction(id).end_with({});
}

void IndexedDBTransactionScheduler::abort(TransactionID id, IndexedDBErrorKind cause)
{
    transaction(id).end_with(cause);
}

// https://w3c.github.io/IndexedDB/#transaction-scheduling
bool IndexedDBTransactionScheduler::can_start(Transaction const& transaction) const
{
    for (auto const& other : m_transactions) {
        // The ones that come after it were created after it.
        if (other.ptr() == &transaction)
            break;

        // A read-only transactions tx can start when there are no read/write transactions which:
        // * Were created before tx; and
        // * have overlapping scopes with tx; and
        // * are not finished.
        // A read/write transaction tx can start when there are no transactions which:
        // * Were created before tx; and
        // * have overlapping scopes with tx; and
        // * are not finished.
        bool has_to_be_finished_first = transaction.writes() || other->writes();
        if (has_to_be_finished_first && transaction.overlaps(*other))
            return false;
    }

    if (!transaction.writes())
        return true;

    // Implementations may impose additional constraints. For example, implementations are not required to start
    // non-overlapping read/write transactions in parallel, or may impose limits on the number of started
    // transactions.
    // NB: A database has one writer at a time.
    for (auto const& other : m_transactions) {
        bool has_begun = other->state != State::Waiting;
        if (other.ptr() != &transaction && other->writes() && has_begun)
            return false;
    }

    return true;
}

IndexedDBTransactionScheduler::Action IndexedDBTransactionScheduler::await_report_of(Action action)
{
    m_action_awaiting_its_report = action;
    return action;
}

template<typename ActionType>
void IndexedDBTransactionScheduler::take_report_of(TransactionID id)
{
    VERIFY(m_action_awaiting_its_report.has_value());
    VERIFY(m_action_awaiting_its_report->has<ActionType>());
    VERIFY(m_action_awaiting_its_report->get<ActionType>().transaction == id);
    m_action_awaiting_its_report.clear();
}

Optional<IndexedDBTransactionScheduler::Action> IndexedDBTransactionScheduler::next_action()
{
    VERIFY(!m_action_awaiting_its_report.has_value());

    // A transaction that is to end comes first, since its end can be what another waits for.
    for (auto& transaction : m_transactions) {
        if (transaction->is_to_be_rolled_back)
            return await_report_of(RollBack { transaction->id, transaction->cause_of_roll_back });
    }

    // https://w3c.github.io/IndexedDB/#transaction-construct
    // 2. When an implementation is able to enforce the constraints for the transaction's scope and mode, defined
    //    below, the implementation must queue a database task to start the transaction asynchronously.
    // AD-HOC: No task is queued, and whoever made the transaction is not told of its start, which script cannot see.
    //         See https://github.com/w3c/IndexedDB/issues/487
    for (auto& transaction : m_transactions) {
        if (transaction->state != State::Waiting || !can_start(*transaction))
            continue;

        if (!transaction->writes()) {
            transaction->state = State::Started;
            continue;
        }

        transaction->state = State::Beginning;
        return await_report_of(Begin { transaction->id });
    }

    for (auto& transaction : m_transactions) {
        if (transaction->state != State::Started)
            continue;

        // https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
        // 5.1. Wait until request is the first item in transaction's request list that is not processed.
        if (!transaction->entries_to_run.is_empty())
            return await_report_of(Run { transaction->id, transaction->entries_run });

        // https://w3c.github.io/IndexedDB/#commit-a-transaction
        // 2.1. Wait until every item in transaction's request list is processed.
        if (transaction->results_started_at_commit.has_value())
            return await_report_of(Commit { transaction->id });
    }

    return {};
}

void IndexedDBTransactionScheduler::did_begin(TransactionID id)
{
    take_report_of<Begin>(id);
    transaction(id).state = State::Started;
}

void IndexedDBTransactionScheduler::did_fail_to_begin(TransactionID id, IndexedDBErrorKind cause)
{
    take_report_of<Begin>(id);

    // It has no writer, and waits for nothing but its end.
    auto& transaction = this->transaction(id);
    transaction.state = State::Waiting;
    transaction.end_with(cause);
}

void IndexedDBTransactionScheduler::did_run(TransactionID id, u64 place, Optional<IndexedDBErrorKind> failure)
{
    VERIFY(m_action_awaiting_its_report.has_value() && m_action_awaiting_its_report->has<Run>());
    VERIFY(m_action_awaiting_its_report->get<Run>().place == place);
    take_report_of<Run>(id);

    auto& transaction = this->transaction(id);
    auto kind = transaction.entries_to_run.dequeue();
    ++transaction.entries_run;

    if (kind == EntryKind::Change) {
        if (failure.has_value())
            transaction.end_with(*failure);
        return;
    }

    auto result = transaction.results_run++;
    if (!failure.has_value())
        return;

    // https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
    // 5.3. If result is an error and transaction's state is committing, then run abort a transaction with transaction
    //      and result, and terminate these steps.
    if (transaction.results_started_at_commit.has_value()) {
        transaction.end_with(*failure);
        return;
    }

    // 5.4. If result is an error, then revert all changes made by operation.
    // NB: Whoever ran the request did that. The failure is kept for a commit that is asked for before whoever made
    //     the transaction has started on this result.
    transaction.failed_results.append({ .result = result, .cause = *failure });
}

void IndexedDBTransactionScheduler::did_commit(TransactionID id)
{
    take_report_of<Commit>(id);

    // https://w3c.github.io/IndexedDB/#commit-a-transaction
    // 2.5.2. Set transaction's state to finished.
    // AD-HOC: A transaction counts as finished for the ones that wait for it when its changes are written, and not in
    //         a task of whoever made it, which the Hub does not wait for.
    remove_transaction(id);
}

void IndexedDBTransactionScheduler::did_fail_to_commit(TransactionID id, IndexedDBErrorKind cause)
{
    take_report_of<Commit>(id);

    // https://w3c.github.io/IndexedDB/#commit-a-transaction
    // 2.4. If an error occurs while writing the changes to the database, then run abort a transaction with
    //      transaction and an appropriate type for the error, for example a QuotaExceededError or an "UnknownError"
    //      DOMException, and terminate these steps.
    transaction(id).end_with(cause);
}

void IndexedDBTransactionScheduler::did_roll_back(TransactionID id)
{
    take_report_of<RollBack>(id);

    // https://w3c.github.io/IndexedDB/#abort-a-transaction
    // 4. Set transaction's state to finished.
    remove_transaction(id);
}

}
