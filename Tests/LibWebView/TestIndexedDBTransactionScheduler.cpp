/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWebView/IndexedDBTransactionScheduler.h>

using Kind = WebView::IndexedDBErrorKind;
using Scheduler = WebView::IndexedDBTransactionScheduler;

using EntryKind = Scheduler::EntryKind;
using Mode = Scheduler::Mode;

// What the scheduler asks for, as text: "begin 2", "run 2/0", "commit 2", "roll back 2", "roll back 2 for Quota".
static String described(Scheduler::Action const& action)
{
    return action.visit(
        [](Scheduler::Begin const& begin) { return MUST(String::formatted("begin {}", begin.transaction.value())); },
        [](Scheduler::Run const& run) { return MUST(String::formatted("run {}/{}", run.transaction.value(), run.place)); },
        [](Scheduler::Commit const& commit) { return MUST(String::formatted("commit {}", commit.transaction.value())); },
        [](Scheduler::RollBack const& roll_back) {
            if (!roll_back.cause.has_value())
                return MUST(String::formatted("roll back {}", roll_back.transaction.value()));
            return MUST(String::formatted("roll back {} for {}", roll_back.transaction.value(), *roll_back.cause));
        });
}

// Does what the scheduler asks for until it asks for nothing more, and returns what that was. Each thing goes well,
// except the ones that are given a failure by their text.
static Vector<String> do_everything(Scheduler& scheduler, HashMap<String, Kind> const& failures = {})
{
    Vector<String> actions;
    for (auto action = scheduler.next_action(); action.has_value(); action = scheduler.next_action()) {
        if (actions.size() == 100) {
            FAIL("The scheduler does not stop asking for things");
            break;
        }

        auto description = described(*action);
        auto failure = failures.get(description);
        actions.append(move(description));

        action->visit(
            [&](Scheduler::Begin const& begin) {
                if (failure.has_value())
                    scheduler.did_fail_to_begin(begin.transaction, *failure);
                else
                    scheduler.did_begin(begin.transaction);
            },
            [&](Scheduler::Run const& run) { scheduler.did_run(run.transaction, run.place, failure); },
            [&](Scheduler::Commit const& commit) {
                if (failure.has_value())
                    scheduler.did_fail_to_commit(commit.transaction, *failure);
                else
                    scheduler.did_commit(commit.transaction);
            },
            [&](Scheduler::RollBack const& roll_back) { scheduler.did_roll_back(roll_back.transaction); });
    }
    return actions;
}

static void place_requests(Scheduler& scheduler, Scheduler::TransactionID transaction, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        EXPECT(scheduler.place(transaction, EntryKind::Request).has_value());
}

TEST_CASE(a_read_only_transaction_runs_its_requests_without_being_begun)
{
    Scheduler scheduler;
    auto reader = scheduler.create_transaction(Mode::ReadOnly, { 1 });

    EXPECT_EQ(scheduler.place(reader, EntryKind::Request), 0u);
    EXPECT_EQ(scheduler.place(reader, EntryKind::Request), 1u);
    EXPECT_EQ(do_everything(scheduler), (Vector { "run 1/0"_string, "run 1/1"_string }));

    EXPECT(scheduler.ask_to_commit(reader, 2));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string }));
}

TEST_CASE(what_is_placed_before_a_transaction_starts_is_run_in_order_once_it_does)
{
    Scheduler scheduler;
    auto first = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto second = scheduler.create_transaction(Mode::ReadWrite, { 1 });

    place_requests(scheduler, second, 3);
    EXPECT_EQ(do_everything(scheduler), (Vector { "begin 1"_string }));

    EXPECT(scheduler.ask_to_commit(first, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string, "begin 2"_string, "run 2/0"_string, "run 2/1"_string, "run 2/2"_string }));
}

TEST_CASE(a_transaction_waits_for_the_earlier_ones_that_it_cannot_run_beside)
{
    // A writer waits for every earlier transaction that has one of its object stores.
    {
        Scheduler scheduler;
        auto reader = scheduler.create_transaction(Mode::ReadOnly, { 1, 2 });
        auto writer = scheduler.create_transaction(Mode::ReadWrite, { 2, 3 });
        place_requests(scheduler, writer, 1);
        EXPECT(do_everything(scheduler).is_empty());

        EXPECT(scheduler.ask_to_commit(reader, 0));
        EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string, "begin 2"_string, "run 2/0"_string }));
    }

    // A reader waits for the earlier writers that have one of its object stores.
    {
        Scheduler scheduler;
        auto writer = scheduler.create_transaction(Mode::ReadWrite, { 2, 3 });
        auto reader = scheduler.create_transaction(Mode::ReadOnly, { 1, 2 });
        place_requests(scheduler, reader, 1);
        EXPECT_EQ(do_everything(scheduler), (Vector { "begin 1"_string }));

        EXPECT(scheduler.ask_to_commit(writer, 0));
        EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string, "run 2/0"_string }));
    }

    // Readers of one object store run together, and so do a writer and a reader of different ones.
    {
        Scheduler scheduler;
        auto first_reader = scheduler.create_transaction(Mode::ReadOnly, { 1 });
        auto second_reader = scheduler.create_transaction(Mode::ReadOnly, { 1 });
        auto writer = scheduler.create_transaction(Mode::ReadWrite, { 2 });
        place_requests(scheduler, first_reader, 1);
        place_requests(scheduler, second_reader, 1);
        place_requests(scheduler, writer, 1);
        EXPECT_EQ(do_everything(scheduler), (Vector { "begin 3"_string, "run 1/0"_string, "run 2/0"_string, "run 3/0"_string }));
    }
}

TEST_CASE(a_reader_waits_behind_a_writer_that_is_waiting_itself)
{
    Scheduler scheduler;
    auto first_reader = scheduler.create_transaction(Mode::ReadOnly, { 1 });
    auto writer = scheduler.create_transaction(Mode::ReadWrite, { 1, 2 });
    auto reader_behind_the_writer = scheduler.create_transaction(Mode::ReadOnly, { 2 });
    auto reader_of_another_object_store = scheduler.create_transaction(Mode::ReadOnly, { 3 });

    place_requests(scheduler, writer, 1);
    place_requests(scheduler, reader_behind_the_writer, 1);
    place_requests(scheduler, reader_of_another_object_store, 1);

    // The writer has not started, and the reader of its second object store still comes after it.
    EXPECT_EQ(do_everything(scheduler), (Vector { "run 4/0"_string }));

    EXPECT(scheduler.ask_to_commit(first_reader, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string, "begin 2"_string, "run 2/0"_string }));

    EXPECT(scheduler.ask_to_commit(writer, 1));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 2"_string, "run 3/0"_string }));
}

TEST_CASE(a_database_has_one_started_writer)
{
    Scheduler scheduler;
    auto reader = scheduler.create_transaction(Mode::ReadOnly, { 1 });
    auto writer_behind_the_reader = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto second_writer = scheduler.create_transaction(Mode::ReadWrite, { 2 });
    auto third_writer = scheduler.create_transaction(Mode::ReadWrite, { 3 });

    // The second writer has none of the others' object stores, and the third has to wait for it all the same.
    EXPECT_EQ(do_everything(scheduler), (Vector { "begin 3"_string }));

    // Of the writers that can start once it has finished, the one that was created first does.
    EXPECT(scheduler.ask_to_commit(reader, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string }));
    EXPECT(scheduler.ask_to_commit(second_writer, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 3"_string, "begin 2"_string }));

    EXPECT(scheduler.ask_to_commit(writer_behind_the_reader, 0));
    EXPECT(scheduler.ask_to_commit(third_writer, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 2"_string, "begin 4"_string, "commit 4"_string }));
}

TEST_CASE(a_commit_waits_until_everything_before_it_has_run)
{
    Scheduler scheduler;
    auto first = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto second = scheduler.create_transaction(Mode::ReadWrite, { 1 });

    // The second transaction is asked to commit before it has started.
    place_requests(scheduler, second, 2);
    EXPECT(scheduler.ask_to_commit(second, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "begin 1"_string }));

    EXPECT(scheduler.ask_to_commit(first, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string, "begin 2"_string, "run 2/0"_string, "run 2/1"_string, "commit 2"_string }));
}

TEST_CASE(a_commit_forgives_the_failures_that_its_number_covers)
{
    Scheduler scheduler;
    auto writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    place_requests(scheduler, writer, 2);

    HashMap<String, Kind> failures;
    failures.set("run 1/1"_string, Kind::Constraint);
    EXPECT_EQ(do_everything(scheduler, failures), (Vector { "begin 1"_string, "run 1/0"_string, "run 1/1"_string }));

    // Whoever made the transaction had started on both results, the failed one too, when it asked for the commit.
    EXPECT(scheduler.ask_to_commit(writer, 2));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string }));
}

TEST_CASE(a_commit_becomes_an_abort_for_the_first_failure_past_its_number)
{
    Scheduler scheduler;
    auto writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    place_requests(scheduler, writer, 4);

    HashMap<String, Kind> failures;
    failures.set("run 1/0"_string, Kind::Unknown);
    failures.set("run 1/2"_string, Kind::Constraint);
    failures.set("run 1/3"_string, Kind::Quota);
    EXPECT_EQ(do_everything(scheduler, failures).size(), 5u);

    // The first two results had been started on, the failed one among them, and the two after them had not.
    EXPECT(scheduler.ask_to_commit(writer, 2));
    EXPECT_EQ(do_everything(scheduler), (Vector { "roll back 1 for Constraint"_string }));
}

TEST_CASE(a_request_that_fails_after_the_commit_was_asked_for_ends_the_transaction)
{
    Scheduler scheduler;
    auto first = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto second = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    place_requests(scheduler, second, 3);
    EXPECT(scheduler.ask_to_commit(second, 0));
    EXPECT(scheduler.ask_to_commit(first, 0));

    // What comes after the failed request is not run.
    HashMap<String, Kind> failures;
    failures.set("run 2/1"_string, Kind::Constraint);
    EXPECT_EQ(do_everything(scheduler, failures), (Vector { "begin 1"_string, "commit 1"_string, "begin 2"_string, "run 2/0"_string, "run 2/1"_string, "roll back 2 for Constraint"_string }));
}

TEST_CASE(a_commit_is_refused_twice_and_with_more_results_than_there_were)
{
    Scheduler scheduler;
    auto writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    place_requests(scheduler, writer, 2);
    EXPECT_EQ(do_everything(scheduler).size(), 3u);

    EXPECT(!scheduler.ask_to_commit(writer, 3));
    EXPECT(scheduler.ask_to_commit(writer, 2));
    EXPECT(!scheduler.ask_to_commit(writer, 2));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string }));
}

TEST_CASE(a_transaction_that_was_asked_to_commit_takes_nothing_more)
{
    Scheduler scheduler;
    auto first = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto second = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    place_requests(scheduler, second, 1);
    EXPECT(scheduler.ask_to_commit(second, 0));

    EXPECT(!scheduler.place(second, EntryKind::Request).has_value());
    EXPECT(!scheduler.place(second, EntryKind::Change).has_value());

    EXPECT(scheduler.ask_to_commit(first, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "begin 1"_string, "commit 1"_string, "begin 2"_string, "run 2/0"_string, "commit 2"_string }));
}

TEST_CASE(a_change_that_fails_ends_its_transaction)
{
    Scheduler scheduler;
    auto upgrade = scheduler.create_upgrade_transaction();
    EXPECT_EQ(scheduler.place(upgrade, EntryKind::Change), 0u);
    EXPECT_EQ(scheduler.place(upgrade, EntryKind::Request), 1u);
    EXPECT_EQ(scheduler.place(upgrade, EntryKind::Change), 2u);
    EXPECT_EQ(scheduler.place(upgrade, EntryKind::Request), 3u);

    // A change has no result, so the request after the first one is the first result. No commit was asked for, and
    // the transaction ends all the same.
    HashMap<String, Kind> failures;
    failures.set("run 1/2"_string, Kind::Constraint);
    EXPECT_EQ(do_everything(scheduler, failures), (Vector { "run 1/0"_string, "run 1/1"_string, "run 1/2"_string, "roll back 1 for Constraint"_string }));
}

TEST_CASE(a_change_does_not_count_as_a_result)
{
    Scheduler scheduler;
    auto upgrade = scheduler.create_upgrade_transaction();
    EXPECT(scheduler.place(upgrade, EntryKind::Change).has_value());
    EXPECT(scheduler.place(upgrade, EntryKind::Request).has_value());
    EXPECT(scheduler.place(upgrade, EntryKind::Change).has_value());
    EXPECT_EQ(do_everything(scheduler).size(), 3u);

    EXPECT(!scheduler.ask_to_commit(upgrade, 2));
    EXPECT(scheduler.ask_to_commit(upgrade, 1));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string }));
}

TEST_CASE(an_abort_is_taken_until_the_transaction_has_ended)
{
    Scheduler scheduler;
    auto started_writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto waiting_writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto committing_writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });

    place_requests(scheduler, started_writer, 1);
    EXPECT_EQ(do_everything(scheduler), (Vector { "begin 1"_string, "run 1/0"_string }));

    // One that waits has nothing to undo, and is told of its end like any other.
    place_requests(scheduler, waiting_writer, 2);
    scheduler.ask_to_abort(waiting_writer);
    EXPECT_EQ(do_everything(scheduler), (Vector { "roll back 2"_string }));

    // One that was asked to commit and has not done so yet can still be aborted.
    place_requests(scheduler, committing_writer, 1);
    EXPECT(scheduler.ask_to_commit(committing_writer, 0));
    scheduler.ask_to_abort(committing_writer);
    EXPECT_EQ(do_everything(scheduler), (Vector { "roll back 3"_string }));

    place_requests(scheduler, started_writer, 1);
    scheduler.ask_to_abort(started_writer);
    EXPECT_EQ(do_everything(scheduler), (Vector { "roll back 1"_string }));
}

TEST_CASE(a_transaction_ends_for_the_first_reason_that_there_was)
{
    Scheduler scheduler;
    auto first = scheduler.create_transaction(Mode::ReadOnly, { 1 });
    auto second = scheduler.create_transaction(Mode::ReadOnly, { 1 });

    scheduler.abort(first, Kind::Quota);
    scheduler.ask_to_abort(first);
    scheduler.ask_to_abort(second);
    scheduler.abort(second, Kind::Quota);
    EXPECT_EQ(do_everything(scheduler), (Vector { "roll back 1 for Quota"_string, "roll back 2"_string }));
}

TEST_CASE(a_transaction_that_is_ended_while_another_runs_ends_before_anything_else_is_run)
{
    Scheduler scheduler;
    auto writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto reader = scheduler.create_transaction(Mode::ReadOnly, { 2 });
    auto writer_behind_the_writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    EXPECT_EQ(do_everything(scheduler), (Vector { "begin 1"_string }));

    place_requests(scheduler, reader, 2);
    auto run = scheduler.next_action();
    EXPECT(run.has_value());
    if (!run.has_value())
        return;
    EXPECT_EQ(described(*run), "run 2/0"_string);

    // The reader's request fails, and the writer loses its transaction over it.
    scheduler.abort(writer, Kind::OutOfMemory);
    scheduler.did_run(reader, 0, Kind::OutOfMemory);

    EXPECT_EQ(do_everything(scheduler), (Vector { "roll back 1 for OutOfMemory"_string, "begin 3"_string, "run 2/1"_string }));

    // The reader goes on, and its failed request is behind it.
    EXPECT(scheduler.ask_to_commit(reader, 2));
    EXPECT(scheduler.ask_to_commit(writer_behind_the_writer, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 2"_string, "commit 3"_string }));
}

TEST_CASE(a_writer_that_cannot_be_begun_ends_once_and_the_next_one_begins)
{
    Scheduler scheduler;
    auto first = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    auto second = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    place_requests(scheduler, first, 1);
    EXPECT(scheduler.ask_to_commit(first, 0));
    EXPECT(scheduler.ask_to_commit(second, 0));

    HashMap<String, Kind> failures;
    failures.set("begin 1"_string, Kind::OutOfMemory);
    EXPECT_EQ(do_everything(scheduler, failures), (Vector { "begin 1"_string, "roll back 1 for OutOfMemory"_string, "begin 2"_string, "commit 2"_string }));
}

TEST_CASE(a_commit_that_fails_is_an_abort_for_its_cause)
{
    Scheduler scheduler;
    auto writer = scheduler.create_transaction(Mode::ReadWrite, { 1 });
    EXPECT(scheduler.ask_to_commit(writer, 0));

    HashMap<String, Kind> failures;
    failures.set("commit 1"_string, Kind::Quota);
    EXPECT_EQ(do_everything(scheduler, failures), (Vector { "begin 1"_string, "commit 1"_string, "roll back 1 for Quota"_string }));
}

TEST_CASE(an_upgrade_has_the_database_to_itself)
{
    Scheduler scheduler;
    auto upgrade = scheduler.create_upgrade_transaction();
    auto reader = scheduler.create_transaction(Mode::ReadOnly, { 1 });
    auto writer = scheduler.create_transaction(Mode::ReadWrite, { 2 });
    place_requests(scheduler, reader, 1);
    place_requests(scheduler, writer, 1);

    // It has begun by the time that it is made, and runs what it is asked to at once.
    EXPECT_EQ(scheduler.place(upgrade, EntryKind::Change), 0u);
    EXPECT_EQ(do_everything(scheduler), (Vector { "run 1/0"_string }));

    EXPECT(scheduler.ask_to_commit(upgrade, 0));
    EXPECT_EQ(do_everything(scheduler), (Vector { "commit 1"_string, "begin 3"_string, "run 2/0"_string, "run 3/0"_string }));
}
