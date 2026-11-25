/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

// https://w3c.github.io/IndexedDB/#connection-queue
// The requests to open and to delete one database, which are handled one at a time, and the connections to it.
//
// The queue does none of the work. Whoever owns it asks for the next thing to do, does it, says how it went where
// that matters, and asks again.
class WEBVIEW_API IndexedDBConnectionQueue {
    AK_MAKE_NONCOPYABLE(IndexedDBConnectionQueue);
    AK_MAKE_NONMOVABLE(IndexedDBConnectionQueue);

public:
    AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, RequestID);
    AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ConnectionID);

    enum class RequestKind : u8 {
        Open,
        Delete,
    };

    enum class NeedsUpgrade : u8 {
        No,
        Yes,
    };

    enum class WasCommitted : u8 {
        No,
        Yes,
    };

    enum class Outcome : u8 {
        Opened,
        Aborted,
    };

    enum class Answer : u8 {
        Taken,
        // The notification is one that the connection was sent, and nothing waits for its answer any more.
        TooLate,
        NeverAskedFor,
    };

    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // Steps 4 to 7, which find the database or make it, and tell whether it has to be upgraded.
    struct FindDatabase {
        RequestID request;
    };

    // A connection is told of a notification by its number among the ones that the connection was sent.
    struct NotifyOfVersionChange {
        RequestID request;
        ConnectionID connection;
        u64 notification { 0 };
    };

    struct NotifyOfBlock {
        RequestID request;
    };

    struct Upgrade {
        RequestID request;
        ConnectionID connection;
    };

    struct Delete {
        RequestID request;
    };

    using Action = Variant<FindDatabase, NotifyOfVersionChange, NotifyOfBlock, Upgrade, Delete>;

    IndexedDBConnectionQueue() = default;

    RequestID add_request(RequestKind);

    // For a request whose maker is gone. One that is being upgraded for ends with its upgrade instead.
    void withdraw_request(RequestID);

    // https://w3c.github.io/IndexedDB/#close-a-database-connection
    // A connection is closed, and gone from here, once it was asked to close and its transactions have ended.
    void ask_to_close(ConnectionID);
    void transaction_was_created(ConnectionID);
    void transaction_did_end(ConnectionID);
    bool has_connection(ConnectionID) const;

    Answer take_answer_to_version_change(ConnectionID, u64 notification);

    bool is_empty() const { return m_requests.is_empty() && m_connections.is_empty(); }

    Optional<Action> next_action();

    void did_fail_to_find_database(RequestID);
    ConnectionID did_find_database(RequestID, NeedsUpgrade);

    void did_fail_to_begin_upgrade(RequestID);
    void did_begin_upgrade(RequestID);
    Outcome upgrade_did_end(RequestID, WasCommitted);

    void did_delete(RequestID);

private:
    enum class Phase : u8 {
        Arrived,
        FindingDatabase,
        WaitingForConnections,
        BeginningUpgrade,
        Upgrading,
        Deleting,
    };

    struct Request {
        RequestID id;
        RequestKind kind { RequestKind::Open };
        Phase phase { Phase::Arrived };

        // An open's own, from when its database was found.
        Optional<ConnectionID> connection {};

        bool has_told_of_the_block { false };
    };

    struct Connection {
        bool is_closed() const { return is_close_pending && live_transactions == 0; }

        ConnectionID id;
        bool is_close_pending { false };
        u64 live_transactions { 0 };

        Optional<RequestID> last_request_notified_of {};
        u64 notifications_sent { 0 };
        Optional<u64> notification_awaiting_its_answer {};
    };

    Request& first_request(RequestID, Phase);
    void finish_first_request();

    Connection* find_connection(ConnectionID);
    ConnectionID add_connection();
    void remove_connection(ConnectionID);
    void remove_connection_if_closed(Connection&);

    template<typename Callback>
    bool any_connection_other_than_the_one_of(Request const&, Callback) const;

    Vector<Request> m_requests;
    Vector<Connection> m_connections;

    u64 m_last_request_id { 0 };
    u64 m_last_connection_id { 0 };
};

}
