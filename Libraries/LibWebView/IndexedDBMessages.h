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

// The n-th index that an upgrade created.
struct IndexOfUpgrade {
    u64 number { 0 };
};

// An index is named by its id, or by its number in the upgrade that created it.
using IndexName = Variant<i64, IndexOfUpgrade>;

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

// The keys that the renderer extracted from a value for one index, as it encoded them. A key may hold a marker in the
// place of the key that the database generates for the record and injects into the value.
struct KeysOfIndex {
    IndexName index;
    Vector<ByteBuffer> keys;
};

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
struct StoreRecord {
    ObjectStoreName object_store;
    Optional<ByteBuffer> key {};
    Value value;
    IndexedDBDatabase::NoOverwrite no_overwrite { IndexedDBDatabase::NoOverwrite::No };
    Vector<KeysOfIndex> index_keys {};
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

// https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-index
struct ReadIndexRecords {
    IndexName index;
    KeyRange range;
    IndexedDBDatabase::Direction direction { IndexedDBDatabase::Direction::Ascending };
    IndexedDBDatabase::RecordsOfEachKey records_of_each_key { IndexedDBDatabase::RecordsOfEachKey::All };
    Optional<u32> count {};
    IndexedDBDatabase::WithValues with_values { IndexedDBDatabase::WithValues::Yes };
};

// https://w3c.github.io/IndexedDB/#count-the-records-in-a-range
struct CountIndexRecords {
    IndexName index;
    KeyRange range;
};

// https://w3c.github.io/IndexedDB/#cursor-position
// Where a cursor over an index is: its position, and its object store position.
struct IndexCursorPosition {
    IndexedDBIndexCursorPosition as_position_of_database() const { return { key.bytes(), primary_key.bytes() }; }

    ByteBuffer key;
    ByteBuffer primary_key;
};

// What a cursor over an index is to go to: a key, and with continuePrimaryKey() a primary key as well.
struct IndexCursorSeek {
    IndexedDBIndexCursorSeek as_seek_of_database() const
    {
        return { key.bytes(), primary_key.map([](auto const& bytes) { return bytes.bytes(); }) };
    }

    ByteBuffer key;
    Optional<ByteBuffer> primary_key {};
};

// https://w3c.github.io/IndexedDB/#iterate-a-cursor
struct IterateIndexCursor {
    IndexName index;
    KeyRange range;
    IndexedDBDatabase::Direction direction { IndexedDBDatabase::Direction::Ascending };
    IndexedDBDatabase::RecordsOfEachKey records_of_each_key { IndexedDBDatabase::RecordsOfEachKey::All };
    Optional<IndexCursorPosition> position {};
    Optional<IndexCursorSeek> seek {};
    u32 count { 1 };
    IndexedDBDatabase::WithValues with_values { IndexedDBDatabase::WithValues::Yes };
};

using Operation = Variant<StoreRecord, DeleteRecords, Clear, ReadRecords, Count, IterateCursor, ReadIndexRecords, CountIndexRecords, IterateIndexCursor>;

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

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
// The index is the n-th that the upgrade creates. The records that its object store has by then get their keys in
// the index from the renderer, which is asked for them one record at a time with ExtractIndexKeys.
struct CreateIndex {
    ConnectionNumber upgrade;
    ObjectStoreName object_store;
    Utf16String name;
    IndexedDBKeyPath key_path;
    IndexedDBDatabase::Unique unique { IndexedDBDatabase::Unique::No };
    IndexedDBDatabase::MultiEntry multi_entry { IndexedDBDatabase::MultiEntry::No };
};

struct RenameIndex {
    ConnectionNumber upgrade;
    IndexName index;
    Utf16String name;
};

struct DeleteIndex {
    ConnectionNumber upgrade;
    IndexName index;
};

// The answer to an ExtractIndexKeys, with its token: the keys that the index is to have for the record, which can be
// none. A renderer that cannot read the value asks for the abort of the upgrade in place of an answer.
struct IndexKeys {
    ConnectionNumber upgrade;
    u64 token { 0 };
    Vector<ByteBuffer> keys;
};

using Command = Variant<Open, DeleteDatabase, ListDatabases, CloseConnection, VersionChangeHandled, CreateTransaction, PlaceRequest, Commit, Abort, CreateObjectStore, RenameObjectStore, DeleteObjectStore, CreateIndex, RenameIndex, DeleteIndex, IndexKeys>;

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

struct IndexRecordsRead {
    Vector<IndexedDBIndexRecord> records;
};

struct IndexCursorIterated {
    Optional<IndexedDBIndexRecord> record {};
};

using ResultOfRequest = Variant<RecordStored, RecordsDeleted, RecordsRead, RecordsCounted, CursorIterated, IndexRecordsRead, IndexCursorIterated, IndexedDBErrorKind>;

struct RequestResult {
    TransactionName transaction;
    u64 request { 0 };
    ResultOfRequest result;
};

// An upgrade asks its renderer for the keys that an index of the upgrade is to have for a record that is older than
// the index. The index is given by its number in the upgrade, and the answer is an IndexKeys with the token.
struct ExtractIndexKeys {
    ConnectionNumber upgrade;
    u64 index { 0 };
    u64 token { 0 };
    ByteBuffer value;
};

// The last that a channel hears of a transaction that was committed. An upgrade's has the ids of the object stores
// and of the indexes that the upgrade created, in the order in which it created them.
struct TransactionCompleted {
    TransactionName transaction;
    Vector<i64> created_object_stores;
    Vector<i64> created_indexes;
};

// The last that a channel hears of a transaction that was not committed. The cause is there when the session ended
// the transaction. Without one the renderer asked for the abort, and has the cause.
struct TransactionAborted {
    TransactionName transaction;
    Optional<IndexedDBErrorKind> cause {};
};

using Reply = Variant<OpenSucceeded, UpgradeNeeded, OpenSucceededAfterUpgrade, OpenFailed, VersionChange, Blocked, DeleteSucceeded, DeleteFailed, DatabaseList, RequestResult, ExtractIndexKeys, TransactionCompleted, TransactionAborted>;

}
