/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/DistinctNumeric.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebView/IndexedDBDatabase.h>
#include <LibWebView/IndexedDBErrorKind.h>
#include <LibWebView/IndexedDBStore.h>
#include <LibWebView/IndexedDBTransactionScheduler.h>

// What a renderer and an IndexedDBSession say to each other over a channel, which belongs to one storage key.
//
// A renderer is not trusted, so whatever a command names is a claim. What a channel makes is named by counting, from 1
// and in the order in which the channel made it, so that the renderer and the session count the same without a reply.
// A connection is named by the number that the session told the channel of it with.
namespace WebView::IndexedDBMessages {

// The channel's n-th request to open a database, to delete one or to list them.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, FactoryRequestNumber);

// https://w3c.github.io/IndexedDB/#database-connection
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ConnectionNumber);

// The n-th transaction that the channel created.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, TransactionNumber);

// https://w3c.github.io/IndexedDB/#upgrade-a-database
// The session makes an upgrade transaction, which is named by its connection.
struct UpgradeOf {
    ConnectionNumber connection;
};

using TransactionName = Variant<TransactionNumber, UpgradeOf>;

// The n-th object store that an upgrade created. A renderer has no id for it until the upgrade is committed.
struct ObjectStoreOfUpgrade {
    u64 number { 0 };
};

// An object store is named by its id, or by its number in the upgrade that created it.
using ObjectStoreName = Variant<i64, ObjectStoreOfUpgrade>;

// https://w3c.github.io/IndexedDB/#range-construct
// The bounds are keys as the renderer encoded them.
struct KeyRange {
    IndexedDBKeyRange as_range_of_database() const
    {
        return {
            .lower_bound = lower_bound.map([](auto const& bound) { return bound.bytes(); }),
            .lower_bound_is_open = lower_bound_is_open,
            .upper_bound = upper_bound.map([](auto const& bound) { return bound.bytes(); }),
            .upper_bound_is_open = upper_bound_is_open,
        };
    }

    Optional<ByteBuffer> lower_bound {};
    bool lower_bound_is_open { false };
    Optional<ByteBuffer> upper_bound {};
    bool upper_bound_is_open { false };
};

// The serialization of a value around the place of the key that the database generates and injects.
struct ValueSegments {
    ByteBuffer before_the_key;
    ByteBuffer after_the_key;
};

using Value = Variant<ByteBuffer, ValueSegments>;

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
struct StoreRecord {
    ObjectStoreName object_store;
    Optional<ByteBuffer> key {};
    Value value;
    IndexedDBDatabase::NoOverwrite no_overwrite { IndexedDBDatabase::NoOverwrite::No };
};

// https://w3c.github.io/IndexedDB/#delete-records-from-an-object-store
struct DeleteRecords {
    ObjectStoreName object_store;
    KeyRange range;
};

// https://w3c.github.io/IndexedDB/#clear-an-object-store
struct Clear {
    ObjectStoreName object_store;
};

// https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-object-store
struct ReadRecords {
    ObjectStoreName object_store;
    KeyRange range;
    IndexedDBDatabase::Direction direction { IndexedDBDatabase::Direction::Ascending };
    Optional<u32> count {};
    IndexedDBDatabase::WithValues with_values { IndexedDBDatabase::WithValues::Yes };
};

// https://w3c.github.io/IndexedDB/#count-the-records-in-a-range
struct Count {
    ObjectStoreName object_store;
    KeyRange range;
};

// https://w3c.github.io/IndexedDB/#iterate-a-cursor
// A cursor is kept by its renderer, which says where the cursor is and gets the record that the cursor comes to.
struct IterateCursor {
    ObjectStoreName object_store;
    KeyRange range;
    IndexedDBDatabase::Direction direction { IndexedDBDatabase::Direction::Ascending };
    Optional<ByteBuffer> position {};
    Optional<ByteBuffer> key {};
    u32 count { 1 };
    IndexedDBDatabase::WithValues with_values { IndexedDBDatabase::WithValues::Yes };
};

using Operation = Variant<StoreRecord, DeleteRecords, Clear, ReadRecords, Count, IterateCursor>;

// Commands, which a renderer sends.

// https://w3c.github.io/IndexedDB/#open-a-database-connection
struct Open {
    Utf16String name;
    Optional<u64> version {};
};

// https://w3c.github.io/IndexedDB/#delete-a-database
struct DeleteDatabase {
    Utf16String name;
};

// https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
struct ListDatabases {
};

// https://w3c.github.io/IndexedDB/#close-a-database-connection
struct CloseConnection {
    ConnectionNumber connection;
};

// The renderer has fired the versionchange event of a VersionChange.
struct VersionChangeHandled {
    ConnectionNumber connection;
    u64 notification { 0 };
};

// https://w3c.github.io/IndexedDB/#transaction-construct
// The scope is the ids of the transaction's object stores.
struct CreateTransaction {
    ConnectionNumber connection;
    IndexedDBTransactionScheduler::Mode mode { IndexedDBTransactionScheduler::Mode::ReadOnly };
    Vector<i64> scope;
};

// https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
// The request is its transaction's n-th, which its result is told with. Each step of a cursor is a request.
struct PlaceRequest {
    TransactionName transaction;
    Operation operation;
};

// https://w3c.github.io/IndexedDB/#commit-a-transaction
// The renderer says how many of the transaction's results it had started on when it asked. A request that failed
// before that is one that it has handled.
struct Commit {
    TransactionName transaction;
    u64 results_started { 0 };
};

// https://w3c.github.io/IndexedDB/#abort-a-transaction
struct Abort {
    TransactionName transaction;
};

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-createobjectstore
// The object store is the n-th that the upgrade creates.
struct CreateObjectStore {
    ConnectionNumber upgrade;
    Utf16String name;
    Optional<IndexedDBKeyPath> key_path {};
    IndexedDBDatabase::KeyGenerator key_generator { IndexedDBDatabase::KeyGenerator::No };
};

struct RenameObjectStore {
    ConnectionNumber upgrade;
    ObjectStoreName object_store;
    Utf16String name;
};

struct DeleteObjectStore {
    ConnectionNumber upgrade;
    ObjectStoreName object_store;
};

using Command = Variant<Open, DeleteDatabase, ListDatabases, CloseConnection, VersionChangeHandled, CreateTransaction, PlaceRequest, Commit, Abort, CreateObjectStore, RenameObjectStore, DeleteObjectStore>;

// Replies, which the session sends.

// An open that needed no upgrade has its connection.
struct OpenSucceeded {
    FactoryRequestNumber request;
    ConnectionNumber connection;
    u64 version { 0 };
    Vector<IndexedDBObjectStoreDescription> object_stores;
};

// https://w3c.github.io/IndexedDB/#upgrade-a-database
// The open has its connection, and the upgrade transaction of that connection has started. The object stores are the
// ones from before the upgrade.
struct UpgradeNeeded {
    FactoryRequestNumber request;
    ConnectionNumber connection;
    u64 old_version { 0 };
    u64 version { 0 };
    Vector<IndexedDBObjectStoreDescription> object_stores;
};

// The upgrade of an open was committed, and the open keeps the connection of its UpgradeNeeded.
struct OpenSucceededAfterUpgrade {
    FactoryRequestNumber request;
};

struct OpenFailed {
    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // An open whose upgrade did not leave it a connection fails with an "AbortError", whatever the upgrade ended for.
    struct UpgradeDidNotLeaveAConnection {
    };

    FactoryRequestNumber request;
    Variant<IndexedDBErrorKind, UpgradeDidNotLeaveAConnection> reason;
};

// https://w3c.github.io/IndexedDB/#fire-a-version-change-event
// A request waits for the connection to close. The renderer answers with VersionChangeHandled once it has fired the
// event. The notification is the connection's n-th. A database that is to be deleted has no version to change to.
struct VersionChange {
    ConnectionNumber connection;
    u64 notification { 0 };
    u64 old_version { 0 };
    Optional<u64> version {};
};

// The connections that the request waits for were told, and some are still open.
struct Blocked {
    FactoryRequestNumber request;
    u64 old_version { 0 };
    Optional<u64> version {};
};

struct DeleteSucceeded {
    FactoryRequestNumber request;
    u64 old_version { 0 };
};

struct DeleteFailed {
    FactoryRequestNumber request;
    IndexedDBErrorKind kind { IndexedDBErrorKind::Unknown };
};

struct DatabaseList {
    FactoryRequestNumber request;
    Variant<Vector<IndexedDBStore::DatabaseInfo>, IndexedDBErrorKind> result;
};

struct RecordStored {
    Optional<u64> generated_key {};
};

struct RecordsDeleted {
};

struct RecordsRead {
    Vector<IndexedDBRecord> records;
};

struct RecordsCounted {
    u64 count { 0 };
};

struct CursorIterated {
    Optional<IndexedDBRecord> record {};
};

using ResultOfRequest = Variant<RecordStored, RecordsDeleted, RecordsRead, RecordsCounted, CursorIterated, IndexedDBErrorKind>;

struct RequestResult {
    TransactionName transaction;
    u64 request { 0 };
    ResultOfRequest result;
};

// The last that a channel hears of a transaction that was committed. An upgrade's has the ids of the object stores
// that the upgrade created, in the order in which it created them.
struct TransactionCompleted {
    TransactionName transaction;
    Vector<i64> created_object_stores;
};

// The last that a channel hears of a transaction that was not committed. The cause is there when the session ended
// the transaction. Without one the renderer asked for the abort, and has the cause.
struct TransactionAborted {
    TransactionName transaction;
    Optional<IndexedDBErrorKind> cause {};
};

using Reply = Variant<OpenSucceeded, UpgradeNeeded, OpenSucceededAfterUpgrade, OpenFailed, VersionChange, Blocked, DeleteSucceeded, DeleteFailed, DatabaseList, RequestResult, TransactionCompleted, TransactionAborted>;

}
