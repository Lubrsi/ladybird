/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWebView/IndexedDBConnectionQueue.h>

using Queue = WebView::IndexedDBConnectionQueue;

using Answer = Queue::Answer;
using NeedsUpgrade = Queue::NeedsUpgrade;
using Outcome = Queue::Outcome;
using RequestKind = Queue::RequestKind;
using WasCommitted = Queue::WasCommitted;

// What the queue asks for, as text.
static String described(Queue::Action const& action)
{
    return action.visit(
        [](Queue::FindDatabase const& find) { return MUST(String::formatted("find the database for request {}", find.request.value())); },
        [](Queue::NotifyOfVersionChange const& notify) {
            return MUST(String::formatted("tell connection {} of request {} (its notification {})", notify.connection.value(), notify.request.value(), notify.notification));
        },
        [](Queue::NotifyOfBlock const& notify) { return MUST(String::formatted("tell request {} that it is blocked", notify.request.value())); },
        [](Queue::Upgrade const& upgrade) { return MUST(String::formatted("upgrade for request {} on connection {}", upgrade.request.value(), upgrade.connection.value())); },
        [](Queue::Delete const& delete_) { return MUST(String::formatted("delete for request {}", delete_.request.value())); });
}

// Everything that the queue asks for until it asks for nothing more.
static Vector<String> asked_for(Queue& queue)
{
    Vector<String> actions;
    for (auto action = queue.next_action(); action.has_value(); action = queue.next_action()) {
        if (actions.size() == 100) {
            FAIL("The queue does not stop asking for things");
            break;
        }
        actions.append(described(*action));
    }
    return actions;
}

// Opens a connection to a database that needs no upgrade.
static Queue::ConnectionID open_a_connection(Queue& queue)
{
    auto request = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    return queue.did_find_database(request, NeedsUpgrade::No);
}

TEST_CASE(the_requests_for_a_database_are_handled_one_at_a_time)
{
    Queue queue;
    auto first = queue.add_request(RequestKind::Open);
    auto second = queue.add_request(RequestKind::Open);
    auto third = queue.add_request(RequestKind::Open);

    EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 1"_string }));
    EXPECT(asked_for(queue).is_empty());

    auto connection = queue.did_find_database(first, NeedsUpgrade::No);
    EXPECT(queue.has_connection(connection));
    EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 2"_string }));

    queue.did_fail_to_find_database(second);
    EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 3"_string }));

    queue.did_fail_to_find_database(third);
    EXPECT(asked_for(queue).is_empty());
}

TEST_CASE(an_upgrade_waits_until_the_other_connections_are_closed)
{
    Queue queue;
    auto first = open_a_connection(queue);
    auto second = open_a_connection(queue);

    // The second connection was asked to close, and is not closed while it has a transaction.
    queue.transaction_was_created(second);
    queue.ask_to_close(second);
    EXPECT(queue.has_connection(second));

    // Only a connection that was not asked to close is told of the change, and the request's own is not.
    auto request = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 3"_string }));
    auto connection_of_the_request = queue.did_find_database(request, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue), (Vector { "tell connection 1 of request 3 (its notification 1)"_string }));

    EXPECT_EQ(queue.take_answer_to_version_change(first, 1), Answer::Taken);
    EXPECT_EQ(asked_for(queue), (Vector { "tell request 3 that it is blocked"_string }));

    queue.ask_to_close(first);
    EXPECT(!queue.has_connection(first));
    EXPECT(asked_for(queue).is_empty());

    queue.transaction_did_end(second);
    EXPECT(!queue.has_connection(second));
    EXPECT_EQ(asked_for(queue), (Vector { "upgrade for request 3 on connection 3"_string }));
    EXPECT(queue.has_connection(connection_of_the_request));
}

TEST_CASE(a_request_is_told_of_a_block_once_every_notification_was_answered)
{
    Queue queue;
    auto first = open_a_connection(queue);
    auto second = open_a_connection(queue);

    auto request = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    queue.did_find_database(request, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue), (Vector { "tell connection 1 of request 3 (its notification 1)"_string, "tell connection 2 of request 3 (its notification 1)"_string }));

    EXPECT_EQ(queue.take_answer_to_version_change(second, 1), Answer::Taken);
    EXPECT(asked_for(queue).is_empty());

    EXPECT_EQ(queue.take_answer_to_version_change(first, 1), Answer::Taken);
    EXPECT_EQ(asked_for(queue), (Vector { "tell request 3 that it is blocked"_string }));

    // It is told once, however long the connections stay open.
    queue.ask_to_close(first);
    EXPECT(asked_for(queue).is_empty());
    queue.ask_to_close(second);
    EXPECT_EQ(asked_for(queue), (Vector { "upgrade for request 3 on connection 3"_string }));
}

TEST_CASE(a_request_is_not_told_of_a_block_when_the_other_connections_close)
{
    Queue queue;
    auto first = open_a_connection(queue);
    auto second = open_a_connection(queue);

    auto request = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    queue.did_find_database(request, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue).size(), 2u);

    // The first connection closes and then answers, and the second closes without an answer.
    queue.ask_to_close(first);
    EXPECT_EQ(queue.take_answer_to_version_change(first, 1), Answer::TooLate);
    EXPECT(asked_for(queue).is_empty());

    queue.ask_to_close(second);
    EXPECT_EQ(asked_for(queue), (Vector { "upgrade for request 3 on connection 3"_string }));
}

TEST_CASE(an_answer_counts_for_the_notification_that_it_names)
{
    Queue queue;
    auto connection = open_a_connection(queue);

    auto withdrawn_request = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    auto connection_of_the_withdrawn_request = queue.did_find_database(withdrawn_request, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue), (Vector { "tell connection 1 of request 2 (its notification 1)"_string }));

    // Whoever made the request is gone. The next request has the connection told again.
    queue.withdraw_request(withdrawn_request);
    EXPECT(!queue.has_connection(connection_of_the_withdrawn_request));

    auto request = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    queue.did_find_database(request, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue), (Vector { "tell connection 1 of request 3 (its notification 2)"_string }));

    // The answer to the first notification comes late, and is not an answer to the second.
    EXPECT_EQ(queue.take_answer_to_version_change(connection, 1), Answer::TooLate);
    EXPECT(asked_for(queue).is_empty());

    EXPECT_EQ(queue.take_answer_to_version_change(connection, 3), Answer::NeverAskedFor);
    EXPECT_EQ(queue.take_answer_to_version_change(connection, 0), Answer::NeverAskedFor);
    EXPECT(asked_for(queue).is_empty());

    EXPECT_EQ(queue.take_answer_to_version_change(connection, 2), Answer::Taken);
    EXPECT_EQ(asked_for(queue), (Vector { "tell request 3 that it is blocked"_string }));
    EXPECT_EQ(queue.take_answer_to_version_change(connection, 2), Answer::TooLate);
}

TEST_CASE(what_a_withdrawn_request_had_a_connection_told_is_not_waited_for)
{
    Queue queue;
    auto connection = open_a_connection(queue);
    queue.transaction_was_created(connection);

    auto withdrawn_request = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    queue.did_find_database(withdrawn_request, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    queue.withdraw_request(withdrawn_request);

    // The connection is asked to close before the next request comes to it, so that request has it told nothing.
    queue.ask_to_close(connection);
    auto request = queue.add_request(RequestKind::Delete);
    EXPECT_EQ(asked_for(queue), (Vector { "tell request 3 that it is blocked"_string }));

    queue.transaction_did_end(connection);
    EXPECT_EQ(asked_for(queue), (Vector { "delete for request 3"_string }));
    queue.did_delete(request);
    EXPECT(queue.is_empty());
}

TEST_CASE(a_request_that_is_withdrawn_lets_the_next_one_go_on)
{
    Queue queue;
    auto connection = open_a_connection(queue);

    auto first = queue.add_request(RequestKind::Open);
    auto second = queue.add_request(RequestKind::Open);
    auto third = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    queue.did_find_database(first, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue).size(), 1u);

    // One that waits for its turn, and then the one that everything waits for.
    queue.withdraw_request(second);
    queue.withdraw_request(first);
    EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 4"_string }));

    queue.did_fail_to_find_database(third);
    queue.withdraw_request(third);
    EXPECT(queue.has_connection(connection));
    EXPECT(!queue.is_empty());
}

TEST_CASE(a_delete_waits_until_every_connection_is_closed)
{
    Queue queue;

    // Nothing is connected to the database, if there is one.
    auto first = queue.add_request(RequestKind::Delete);
    EXPECT_EQ(asked_for(queue), (Vector { "delete for request 1"_string }));
    EXPECT(asked_for(queue).is_empty());
    queue.did_delete(first);
    EXPECT(queue.is_empty());

    auto connection = open_a_connection(queue);
    auto second = queue.add_request(RequestKind::Delete);
    auto open_behind_the_delete = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue), (Vector { "tell connection 1 of request 3 (its notification 1)"_string }));

    EXPECT_EQ(queue.take_answer_to_version_change(connection, 1), Answer::Taken);
    EXPECT_EQ(asked_for(queue), (Vector { "tell request 3 that it is blocked"_string }));

    queue.ask_to_close(connection);
    EXPECT_EQ(asked_for(queue), (Vector { "delete for request 3"_string }));

    queue.did_delete(second);
    EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 4"_string }));
    queue.did_fail_to_find_database(open_behind_the_delete);
    EXPECT(queue.is_empty());
}

TEST_CASE(an_open_ends_with_its_upgrade)
{
    // Asks for an open that needs an upgrade and begins the upgrade, with nothing else connected.
    auto begin_an_upgrade = [](Queue& queue, Queue::RequestID& request, Queue::ConnectionID& connection) {
        request = queue.add_request(RequestKind::Open);
        EXPECT_EQ(asked_for(queue).size(), 1u);
        connection = queue.did_find_database(request, NeedsUpgrade::Yes);
        EXPECT_EQ(asked_for(queue).size(), 1u);
        queue.did_begin_upgrade(request);
        queue.transaction_was_created(connection);
    };

    Queue::RequestID request;
    Queue::ConnectionID connection;

    // An upgrade that is committed leaves the open with its connection. Nothing else is handled before that.
    {
        Queue queue;
        begin_an_upgrade(queue, request, connection);
        auto open_behind_the_upgrade = queue.add_request(RequestKind::Open);
        EXPECT(asked_for(queue).is_empty());

        queue.transaction_did_end(connection);
        EXPECT_EQ(queue.upgrade_did_end(request, WasCommitted::Yes), Outcome::Opened);
        EXPECT(queue.has_connection(connection));
        EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 2"_string }));
        queue.did_fail_to_find_database(open_behind_the_upgrade);
    }

    // One that is aborted takes the connection with it.
    {
        Queue queue;
        begin_an_upgrade(queue, request, connection);
        queue.transaction_did_end(connection);
        EXPECT_EQ(queue.upgrade_did_end(request, WasCommitted::No), Outcome::Aborted);
        EXPECT(queue.is_empty());
    }

    // A connection that is asked to close during its upgrade is closed when the upgrade ends, however it ends.
    {
        Queue queue;
        begin_an_upgrade(queue, request, connection);
        queue.ask_to_close(connection);
        EXPECT(queue.has_connection(connection));

        queue.transaction_did_end(connection);
        EXPECT_EQ(queue.upgrade_did_end(request, WasCommitted::Yes), Outcome::Aborted);
        EXPECT(queue.is_empty());
    }
}

TEST_CASE(an_open_that_cannot_begin_its_upgrade_gives_up_its_connection)
{
    Queue queue;
    auto request = queue.add_request(RequestKind::Open);
    auto open_behind_it = queue.add_request(RequestKind::Open);
    EXPECT_EQ(asked_for(queue).size(), 1u);
    auto connection = queue.did_find_database(request, NeedsUpgrade::Yes);
    EXPECT_EQ(asked_for(queue), (Vector { "upgrade for request 1 on connection 1"_string }));

    queue.did_fail_to_begin_upgrade(request);
    EXPECT(!queue.has_connection(connection));
    EXPECT_EQ(asked_for(queue), (Vector { "find the database for request 2"_string }));
    queue.did_fail_to_find_database(open_behind_it);
    EXPECT(queue.is_empty());
}
