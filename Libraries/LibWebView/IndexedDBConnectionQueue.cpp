/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/IndexedDBConnectionQueue.h>

namespace WebView {

IndexedDBConnectionQueue::RequestID IndexedDBConnectionQueue::add_request(RequestKind kind)
{
    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // 2. Add request to queue.
    RequestID id { ++m_last_request_id };
    m_requests.append({ .id = id, .kind = kind });
    return id;
}

void IndexedDBConnectionQueue::withdraw_request(RequestID id)
{
    auto index = m_requests.find_first_index_if([&](auto const& request) { return request.id == id; });
    if (!index.has_value())
        return;

    bool is_waiting_for_its_turn = *index != 0;
    if (is_waiting_for_its_turn) {
        m_requests.remove(*index);
        return;
    }

    auto& request = m_requests.first();
    switch (request.phase) {
    case Phase::Arrived:
        break;
    case Phase::WaitingForConnections:
        // Nobody else knows of its connection, which has no transactions. What it had the other connections told is
        // no longer waited for.
        if (request.connection.has_value())
            remove_connection(*request.connection);
        for (auto& connection : m_connections) {
            if (connection.last_request_notified_of == id)
                connection.notification_awaiting_its_answer.clear();
        }
        break;
    case Phase::Upgrading:
        return;
    case Phase::FindingDatabase:
    case Phase::BeginningUpgrade:
    case Phase::Deleting:
        // What the queue asked for last is reported before anything else happens.
        VERIFY_NOT_REACHED();
    }

    finish_first_request();
}

IndexedDBConnectionQueue::Request& IndexedDBConnectionQueue::first_request(RequestID id, Phase phase)
{
    VERIFY(!m_requests.is_empty());

    auto& request = m_requests.first();
    VERIFY(request.id == id);
    VERIFY(request.phase == phase);
    return request;
}

void IndexedDBConnectionQueue::finish_first_request()
{
    m_requests.remove(0);
}

IndexedDBConnectionQueue::Connection* IndexedDBConnectionQueue::find_connection(ConnectionID id)
{
    for (auto& connection : m_connections) {
        if (connection.id == id)
            return &connection;
    }
    return nullptr;
}

bool IndexedDBConnectionQueue::has_connection(ConnectionID id) const
{
    return m_connections.contains([&](auto const& connection) { return connection.id == id; });
}

IndexedDBConnectionQueue::ConnectionID IndexedDBConnectionQueue::add_connection()
{
    ConnectionID id { ++m_last_connection_id };
    m_connections.append({ .id = id });
    return id;
}

void IndexedDBConnectionQueue::remove_connection(ConnectionID id)
{
    m_connections.remove_first_matching([&](auto const& connection) { return connection.id == id; });
}

void IndexedDBConnectionQueue::remove_connection_if_closed(Connection& connection)
{
    if (connection.is_closed())
        remove_connection(connection.id);
}

// https://w3c.github.io/IndexedDB/#close-a-database-connection
void IndexedDBConnectionQueue::ask_to_close(ConnectionID id)
{
    auto* connection = find_connection(id);
    if (!connection)
        return;

    // 1. Set connection's close pending flag to true.
    connection->is_close_pending = true;

    // 3. Wait for all transactions created using connection to complete. Once they are complete, connection is
    //    closed.
    remove_connection_if_closed(*connection);
}

void IndexedDBConnectionQueue::transaction_was_created(ConnectionID id)
{
    auto* connection = find_connection(id);
    VERIFY(connection);
    ++connection->live_transactions;
}

void IndexedDBConnectionQueue::transaction_did_end(ConnectionID id)
{
    auto* connection = find_connection(id);
    VERIFY(connection);
    VERIFY(connection->live_transactions > 0);
    --connection->live_transactions;

    // https://w3c.github.io/IndexedDB/#close-a-database-connection
    // 3. Wait for all transactions created using connection to complete. Once they are complete, connection is
    //    closed.
    remove_connection_if_closed(*connection);
}

IndexedDBConnectionQueue::Answer IndexedDBConnectionQueue::take_answer_to_version_change(ConnectionID id, u64 notification)
{
    // A connection that has closed is not waited for, whatever it was sent.
    auto* connection = find_connection(id);
    if (!connection)
        return Answer::TooLate;

    if (notification == 0 || notification > connection->notifications_sent)
        return Answer::NeverAskedFor;
    if (connection->notification_awaiting_its_answer != notification)
        return Answer::TooLate;

    connection->notification_awaiting_its_answer.clear();
    return Answer::Taken;
}

template<typename Callback>
bool IndexedDBConnectionQueue::any_connection_other_than_the_one_of(Request const& request, Callback callback) const
{
    for (auto const& connection : m_connections) {
        if (request.connection != connection.id && callback(connection))
            return true;
    }
    return false;
}

Optional<IndexedDBConnectionQueue::Action> IndexedDBConnectionQueue::next_action()
{
    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // 3. Wait until all previous requests in queue have been processed.
    if (m_requests.is_empty())
        return {};
    auto& request = m_requests.first();

    if (request.phase == Phase::Arrived && request.kind == RequestKind::Open) {
        request.phase = Phase::FindingDatabase;
        return FindDatabase { request.id };
    }

    // https://w3c.github.io/IndexedDB/#delete-a-database
    // 4. Let db be the database named name in storageKey, if one exists. Otherwise, return 0 (zero).
    // NB: A database that has connections exists. Whoever deletes the database finds out whether there is one, once
    //     nothing is connected to it.
    if (request.phase == Phase::Arrived)
        request.phase = Phase::WaitingForConnections;

    if (request.phase != Phase::WaitingForConnections)
        return {};

    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // NB: These are the steps 5 to 9 of deleting a database as well, which has no connection of its own.
    // 10.1. Let openConnections be the set of all connections, except connection, associated with db.
    // 10.2. For each entry of openConnections that does not have its close pending flag set to true, queue a
    //       database task to fire a version change event named versionchange at entry with db's version and
    //       version.
    for (auto& connection : m_connections) {
        bool is_the_one_of_the_request = request.connection == connection.id;
        bool was_told = connection.last_request_notified_of == request.id;
        if (is_the_one_of_the_request || connection.is_close_pending || was_told)
            continue;

        connection.last_request_notified_of = request.id;
        connection.notification_awaiting_its_answer = ++connection.notifications_sent;
        return NotifyOfVersionChange { request.id, connection.id, connection.notifications_sent };
    }

    // 10.3. Wait for all of the events to be fired.
    // NB: A connection that closes before it answers is not waited for: "Firing this event might cause one or more
    //     of the other objects in openConnections to be closed, in which case the versionchange event is not fired
    //     at those objects, even if that hasn't yet been done."
    bool an_answer_is_awaited = any_connection_other_than_the_one_of(request, [](auto const& connection) {
        return connection.notification_awaiting_its_answer.has_value();
    });
    if (an_answer_is_awaited)
        return {};

    // 10.4. If any of the connections in openConnections are still not closed, queue a database task to fire a
    //       version change event named blocked at request with db's version and version.
    bool another_connection_is_open = any_connection_other_than_the_one_of(request, [](auto const&) { return true; });
    if (another_connection_is_open && !request.has_told_of_the_block) {
        request.has_told_of_the_block = true;
        return NotifyOfBlock { request.id };
    }

    // 10.5. Wait until all connections in openConnections are closed.
    if (another_connection_is_open)
        return {};

    // 10.6. Run upgrade a database using connection, version and request.
    if (request.kind == RequestKind::Open) {
        request.phase = Phase::BeginningUpgrade;
        return Upgrade { request.id, *request.connection };
    }

    // https://w3c.github.io/IndexedDB/#delete-a-database
    // 11. Delete db. If this fails for any reason, return an appropriate error (e.g. a QuotaExceededError, or an
    //     "UnknownError" DOMException).
    request.phase = Phase::Deleting;
    return Delete { request.id };
}

void IndexedDBConnectionQueue::did_fail_to_find_database(RequestID id)
{
    (void)first_request(id, Phase::FindingDatabase);
    finish_first_request();
}

// https://w3c.github.io/IndexedDB/#open-a-database-connection
IndexedDBConnectionQueue::ConnectionID IndexedDBConnectionQueue::did_find_database(RequestID id, NeedsUpgrade needs_upgrade)
{
    auto& request = first_request(id, Phase::FindingDatabase);

    // 8. Let connection be a new connection to db.
    auto connection = add_connection();

    // 10. If db's version is less than version, then:
    if (needs_upgrade == NeedsUpgrade::Yes) {
        request.connection = connection;
        request.phase = Phase::WaitingForConnections;
        return connection;
    }

    // 11. Return connection.
    finish_first_request();
    return connection;
}

void IndexedDBConnectionQueue::did_fail_to_begin_upgrade(RequestID id)
{
    auto& request = first_request(id, Phase::BeginningUpgrade);
    remove_connection(*request.connection);
    finish_first_request();
}

void IndexedDBConnectionQueue::did_begin_upgrade(RequestID id)
{
    // https://w3c.github.io/IndexedDB/#upgrade-a-database
    // 11. Wait for transaction to finish.
    first_request(id, Phase::BeginningUpgrade).phase = Phase::Upgrading;
}

// https://w3c.github.io/IndexedDB/#open-a-database-connection
IndexedDBConnectionQueue::Outcome IndexedDBConnectionQueue::upgrade_did_end(RequestID id, WasCommitted was_committed)
{
    auto connection = *first_request(id, Phase::Upgrading).connection;
    finish_first_request();

    // 10.7. If connection was closed, return a newly created "AbortError" DOMException and abort these steps.
    if (!has_connection(connection))
        return Outcome::Aborted;

    // 10.8. If request's error is set, run the steps to close a database connection with connection, return a newly
    //       created "AbortError" DOMException and abort these steps.
    if (was_committed == WasCommitted::No) {
        remove_connection(connection);
        return Outcome::Aborted;
    }

    // 11. Return connection.
    return Outcome::Opened;
}

void IndexedDBConnectionQueue::did_delete(RequestID id)
{
    (void)first_request(id, Phase::Deleting);
    finish_first_request();
}

}
