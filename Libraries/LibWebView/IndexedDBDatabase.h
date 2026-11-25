/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Traits.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibDatabase/Database.h>
#include <LibWebCommon/IndexedDB/KeyEncoding.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/IndexedDBErrorKind.h>
#include <LibWebView/IndexedDBQuota.h>

namespace WebView {

// Only the kind of a failure is meant for whoever made the request. The cause is SQLite's own.
struct WEBVIEW_API IndexedDBError {
    using Kind = IndexedDBErrorKind;

    static IndexedDBError from(Database::Error&&);

    Kind kind { Kind::Unknown };

    // When set, the database finished its writer while handling this failure. The request may have come from a reader.
    bool lost_the_writer { false };

    Optional<Database::Error> cause {};
};

// https://w3c.github.io/IndexedDB/#key-path-construct
using IndexedDBKeyPath = Variant<Utf16String, Vector<Utf16String>>;

struct IndexedDBIndexDescription {
    i64 id { 0 };
    Utf16String name;
    IndexedDBKeyPath key_path;
    bool is_unique { false };
    bool is_multi_entry { false };
};

struct IndexedDBObjectStoreDescription {
    i64 id { 0 };
    Utf16String name;
    Optional<IndexedDBKeyPath> key_path;
    bool has_key_generator { false };
    Vector<IndexedDBIndexDescription> indexes;
};

// https://w3c.github.io/IndexedDB/#range-construct
// The bounds are keys as a renderer encoded them, which the database checks.
struct IndexedDBKeyRange {
    Optional<ReadonlyBytes> lower_bound;
    bool lower_bound_is_open { false };
    Optional<ReadonlyBytes> upper_bound;
    bool upper_bound_is_open { false };
};

struct IndexedDBRecord {
    ByteBuffer key;
    Optional<ByteBuffer> value;
};

// https://w3c.github.io/IndexedDB/#index-construct
// A record of an index, with the value of the record that it refers to.
struct IndexedDBIndexRecord {
    ByteBuffer key;
    ByteBuffer primary_key;
    Optional<ByteBuffer> value;
};

// The keys that a renderer extracted from a value for one index, as it encoded them. A key may hold a marker in the
// place of the key that the database generates for the record and injects into the value.
struct IndexedDBIndexKeys {
    i64 index_id { 0 };
    ReadonlySpan<ReadonlyBytes> keys;
};

// https://w3c.github.io/IndexedDB/#cursor-position
// Where a cursor over an index is: its position, and its object store position.
struct IndexedDBIndexCursorPosition {
    ReadonlyBytes key;
    ReadonlyBytes primary_key;
};

// What a cursor over an index is to go to: a key, and with continuePrimaryKey() a primary key as well.
struct IndexedDBIndexCursorSeek {
    ReadonlyBytes key;
    Optional<ReadonlyBytes> primary_key;
};

// The serialization of a value around the place of the key that the database generates and injects.
struct IndexedDBValueSegments {
    ReadonlyBytes before_the_key;
    ReadonlyBytes after_the_key;
};

using IndexedDBValue = Variant<ReadonlyBytes, IndexedDBValueSegments>;

// https://w3c.github.io/IndexedDB/#database-construct
class WEBVIEW_API IndexedDBDatabase : public RefCounted<IndexedDBDatabase> {
public:
    class Writer;

    enum class Direction : u8 {
        Ascending,
        Descending,
    };

    enum class WithValues : u8 {
        No,
        Yes,
    };

    enum class NoOverwrite : u8 {
        No,
        Yes,
    };

    enum class KeyGenerator : u8 {
        No,
        Yes,
    };

    enum class Unique : u8 {
        No,
        Yes,
    };

    enum class MultiEntry : u8 {
        No,
        Yes,
    };

    // https://w3c.github.io/IndexedDB/#enumdef-idbcursordirection
    // The directions "nextunique" and "prevunique" read the first record of each key.
    enum class RecordsOfEachKey : u8 {
        All,
        First,
    };

    // https://webidl.spec.whatwg.org/#EnforceRange
    // A version is an [EnforceRange] unsigned long long, and open() throws for 0.
    static constexpr u64 LARGEST_VERSION = (1ULL << 53) - 1;

    // https://www.sqlite.org/limits.html
    // SQLite's default limit on the length of a string or BLOB, which the databases leave alone.
    static constexpr size_t MAXIMUM_CELL_SIZE = 1'000'000'000;

    // A value that is longer than this is stored in chunks of at most this length. A value is read back from the
    // chunks that it was stored in, whatever this was then.
    static constexpr size_t VALUE_CHUNK_SIZE = 1 * MiB;

    // Where the bytes of a database are counted, and what the database counts for besides its pages.
    struct Quotas {
        NonnullRefPtr<IndexedDBQuota> budget_of_storage_key;
        NonnullRefPtr<IndexedDBCeiling> ceiling_of_session;
        u64 charge { 0 };
    };

    static ErrorOr<NonnullRefPtr<IndexedDBDatabase>, IndexedDBError> create_transient(String const& storage_key, Utf16String const& name, UnixDateTime now, Quotas const&);
    ~IndexedDBDatabase();

    // The version of the last upgrade that committed, or 0. A read of the stored version would see an upgrade that is
    // still under way, since it shares its connection.
    u64 version() const { return m_committed_version; }

    bool has_writer() const { return m_writer != nullptr; }
    ErrorOr<NonnullOwnPtr<Writer>, IndexedDBError> begin_writer();

    // https://w3c.github.io/IndexedDB/#upgrade-a-database
    // The version has to be greater than version().
    ErrorOr<NonnullOwnPtr<Writer>, IndexedDBError> begin_upgrade(u64 version);

    ErrorOr<Vector<IndexedDBObjectStoreDescription>, IndexedDBError> object_stores();
    ErrorOr<u64, IndexedDBError> count(i64 object_store_id, IndexedDBKeyRange const&);
    ErrorOr<Vector<IndexedDBRecord>, IndexedDBError> read_records(i64 object_store_id, IndexedDBKeyRange const&, Direction, Optional<u32> count, WithValues);

    ErrorOr<u64, IndexedDBError> count_index_records(i64 index_id, IndexedDBKeyRange const&);
    ErrorOr<Vector<IndexedDBIndexRecord>, IndexedDBError> read_index_records(i64 index_id, IndexedDBKeyRange const&, Direction, RecordsOfEachKey, Optional<u32> count, WithValues);

    // https://w3c.github.io/IndexedDB/#iterate-a-cursor
    // A cursor is kept by its renderer, which says where the cursor is and gets the record that the cursor comes to.
    ErrorOr<Optional<IndexedDBRecord>, IndexedDBError> iterate_cursor(i64 object_store_id, IndexedDBKeyRange const&, Direction, Optional<ReadonlyBytes> position, Optional<ReadonlyBytes> key, u32 count, WithValues);
    ErrorOr<Optional<IndexedDBIndexRecord>, IndexedDBError> iterate_index_cursor(i64 index_id, IndexedDBKeyRange const&, Direction, RecordsOfEachKey, Optional<IndexedDBIndexCursorPosition> const&, Optional<IndexedDBIndexCursorSeek> const&, u32 count, WithValues);

    ErrorOr<void, IndexedDBError> mark_opened(UnixDateTime now);
    ErrorOr<UnixDateTime, IndexedDBError> last_open_or_write_time();

private:
    struct Statements {
        Database::StatementID set_version { 0 };
        Database::StatementID set_last_open_or_write_time { 0 };
        Database::StatementID get_last_open_or_write_time { 0 };
        Database::StatementID get_object_stores { 0 };
        Database::StatementID get_object_store { 0 };
        Database::StatementID create_object_store { 0 };
        Database::StatementID rename_object_store { 0 };
        Database::StatementID delete_object_store { 0 };
        Database::StatementID set_key_generator { 0 };
        Database::StatementID get_record { 0 };
        Database::StatementID delete_record { 0 };
        Database::StatementID insert_record { 0 };
        Database::StatementID insert_value_chunk { 0 };
        Database::StatementID get_length_of_value_chunks { 0 };
        Database::StatementID get_value_chunks { 0 };
        Database::StatementID get_indexes { 0 };
        Database::StatementID get_index { 0 };
        Database::StatementID create_index { 0 };
        Database::StatementID rename_index { 0 };
        Database::StatementID delete_index { 0 };
        Database::StatementID get_whether_index_has_key { 0 };
        Database::StatementID insert_index_record { 0 };
    };

    enum class RangeSource : u8 {
        ObjectStore,
        Index,
    };

    enum class RangeStatement : u8 {
        Count,
        ReadKeys,
        ReadKeysAndValues,
        Delete,
        IterateKeys,
        IterateKeysAndValues,
    };

    enum class RangeBound : u8 {
        None,
        Closed,
        Open,
    };

    // What a cursor's position or the place that it is to go to is given by.
    enum class CursorBound : u8 {
        None,
        Key,
        KeyAndPrimaryKey,
    };

    // Each shape of range is a statement of its own, so that SQLite can seek to a bound in the index on the key.
    struct RangeStatementShape {
        bool operator==(RangeStatementShape const&) const = default;

        RangeSource source { RangeSource::ObjectStore };
        RangeStatement statement { RangeStatement::Count };
        RangeBound lower_bound { RangeBound::None };
        RangeBound upper_bound { RangeBound::None };
        bool is_descending { false };
        bool has_the_first_record_of_each_key { false };
        CursorBound position { CursorBound::None };
        CursorBound seek { CursorBound::None };

        // The column that the source's records are sorted by first.
        StringView key() const;
        void append_order_to(StringBuilder& sql) const;
    };

    struct RangeStatementShapeTraits : public DefaultTraits<RangeStatementShape> {
        static unsigned hash(RangeStatementShape const& shape)
        {
            auto statement = pair_int_hash(to_underlying(shape.source), to_underlying(shape.statement));
            auto bounds = pair_int_hash(to_underlying(shape.lower_bound), to_underlying(shape.upper_bound));
            auto order = pair_int_hash(shape.is_descending, shape.has_the_first_record_of_each_key);
            auto cursor = pair_int_hash(to_underlying(shape.position), to_underlying(shape.seek));
            return pair_int_hash(pair_int_hash(statement, bounds), pair_int_hash(order, cursor));
        }
    };

    struct CheckedKeyRange;
    struct CheckedCursorBounds;

    struct KeysOfObjectStore {
        bool are_in_line { false };
        Optional<i64> generator_current_number;
    };

    struct Index {
        i64 id { 0 };
        i64 object_store_id { 0 };
        bool is_unique { false };
        bool is_multi_entry { false };
    };

    struct FoundRecord {
        i64 id { 0 };
        Optional<ByteBuffer> exact_form_of_key;
    };

    IndexedDBDatabase(NonnullRefPtr<Database::Database>, Statements, NonnullOwnPtr<IndexedDBHeldPages>, Quotas const&);

    ErrorOr<void, IndexedDBError> charge_the_storage_key();

    IndexedDBError failure(Database::Error&&);
    IndexedDBError failure_to_give_a_name(Database::Error&&);
    static ErrorOr<CheckedKeyRange, IndexedDBError> check_key_range(IndexedDBKeyRange const&);
    ErrorOr<Database::StatementID, IndexedDBError> range_statement(RangeSource, RangeStatement, CheckedKeyRange const&, Direction, RecordsOfEachKey);
    ErrorOr<u64, IndexedDBError> count_the_records_in_a_range(RangeSource, i64 source_id, IndexedDBKeyRange const&);

    ErrorOr<Database::StatementID, IndexedDBError> prepare_range_statement(RangeStatementShape const&, StringView sql);
    ErrorOr<Database::StatementID, IndexedDBError> cursor_statement(RangeSource, WithValues, CheckedKeyRange const&, CheckedCursorBounds const&, Direction, RecordsOfEachKey);
    template<typename Record>
    ErrorOr<Optional<Record>, IndexedDBError> iterate_a_cursor(RangeSource, i64 source_id, IndexedDBKeyRange const&, Direction, RecordsOfEachKey, CheckedCursorBounds, u32 count, WithValues, ErrorOr<Record, Database::Error> (IndexedDBDatabase::*read_found_record)(Database::ResultRow&, WithValues));

    ErrorOr<IndexedDBRecord, Database::Error> read_record(Database::ResultRow&, WithValues);
    ErrorOr<IndexedDBIndexRecord, Database::Error> read_index_record(Database::ResultRow&, WithValues);
    ErrorOr<ByteBuffer, Database::Error> read_value(Database::ResultRow&);
    ErrorOr<ByteBuffer, Database::Error> read_value_chunks(i64 record_id);

    ErrorOr<NonnullOwnPtr<Writer>, IndexedDBError> begin_writer_for(Optional<u64> upgrade_version);
    void writer_did_finish(Optional<u64> committed_version);

    ErrorOr<void, IndexedDBError> write_version(u64);
    ErrorOr<void, IndexedDBError> write_last_open_or_write_time(UnixDateTime);
    ErrorOr<UnixDateTime, IndexedDBError> write_time_of_commit(UnixDateTime now);
    void did_commit_last_open_or_write_time(UnixDateTime);

    ErrorOr<i64, IndexedDBError> add_object_store(Utf16String const& name, Optional<String> const& serialized_key_path, KeyGenerator);
    ErrorOr<void, IndexedDBError> set_name_of_object_store(i64 object_store_id, Utf16String const& name);
    ErrorOr<void, IndexedDBError> remove_object_store(i64 object_store_id);

    ErrorOr<KeysOfObjectStore, IndexedDBError> keys_of_object_store(i64 object_store_id);
    ErrorOr<void, IndexedDBError> set_key_generator(i64 object_store_id, i64 current_number);
    ErrorOr<u64, IndexedDBError> generate_a_key(i64 object_store_id, i64 current_number);
    ErrorOr<void, IndexedDBError> possibly_update_the_key_generator(i64 object_store_id, i64 current_number, ReadonlyBytes checked_key);

    ErrorOr<Optional<FoundRecord>, IndexedDBError> find_record(i64 object_store_id, ReadonlyBytes sort_form_of_key);
    ErrorOr<i64, IndexedDBError> add_record(i64 object_store_id, Web::IndexedDB::CheckedKey const&, ReadonlySpan<ReadonlyBytes> parts_of_value);
    ErrorOr<void, IndexedDBError> add_value_chunks(i64 record_id, ReadonlySpan<ReadonlyBytes> parts_of_value);
    ErrorOr<void, IndexedDBError> remove_record(i64 record_id);
    ErrorOr<void, IndexedDBError> remove_records(i64 object_store_id, CheckedKeyRange const&);

    ErrorOr<i64, IndexedDBError> add_index(i64 object_store_id, Utf16String const& name, String const& serialized_key_path, Unique, MultiEntry);
    ErrorOr<void, IndexedDBError> set_name_of_index(i64 index_id, Utf16String const& name);
    ErrorOr<void, IndexedDBError> remove_index(i64 index_id);

    ErrorOr<Index, IndexedDBError> index(i64 index_id);
    ErrorOr<bool, IndexedDBError> index_has_key(i64 index_id, ReadonlyBytes sort_form_of_key);
    ErrorOr<void, IndexedDBError> add_index_record(i64 index_id, Web::IndexedDB::CheckedKey const& index_key, i64 record_id, Web::IndexedDB::CheckedKey const& key_of_record);
    ErrorOr<void, IndexedDBError> add_index_records(Index const&, ReadonlySpan<ReadonlyBytes> keys, Optional<u64> injected_generated_key, i64 record_id, Web::IndexedDB::CheckedKey const& key_of_record);

    NonnullRefPtr<Database::Database> m_sqlite_database;
    Statements m_statements;
    HashMap<RangeStatementShape, Database::StatementID, RangeStatementShapeTraits> m_range_statements;

    NonnullOwnPtr<IndexedDBHeldPages> m_held_pages;
    NonnullRefPtr<IndexedDBCeiling> m_ceiling_of_session;
    NonnullRefPtr<IndexedDBQuota> m_budget_of_storage_key;
    u64 m_charge { 0 };
    u64 m_bytes_taken_of_budget { 0 };

    u64 m_committed_version { 0 };
    Writer* m_writer { nullptr };

    // The time of an open that happened while a writer was at work. Kept until that time or a later one is committed,
    // so that the writer's rollback can't undo it.
    Optional<UnixDateTime> m_held_open_time;
};

// https://w3c.github.io/IndexedDB/#transaction-construct
// The SQL transaction of the one read/write or upgrade transaction that has started on a database. Each request runs
// in a savepoint, so that one that fails undoes only itself. A change to the object stores or to their indexes that was
// accepted and then cannot be made aborts the upgrade.
class WEBVIEW_API IndexedDBDatabase::Writer {
    AK_MAKE_NONCOPYABLE(Writer);
    AK_MAKE_NONMOVABLE(Writer);

public:
    AK_ALLOC_WITH_KMALLOC;

    ~Writer();

    bool is_open() const;
    bool is_upgrade() const { return m_upgrade_version.has_value(); }

    // Changes to the object stores and to their indexes are an upgrade's alone.
    ErrorOr<i64, IndexedDBError> create_object_store(Utf16String const& name, Optional<IndexedDBKeyPath> const&, KeyGenerator);
    ErrorOr<void, IndexedDBError> rename_object_store(i64 object_store_id, Utf16String const& name);
    ErrorOr<void, IndexedDBError> delete_object_store(i64 object_store_id);

    ErrorOr<i64, IndexedDBError> create_index(i64 object_store_id, Utf16String const& name, IndexedDBKeyPath const&, Unique, MultiEntry);
    ErrorOr<void, IndexedDBError> rename_index(i64 index_id, Utf16String const& name);
    ErrorOr<void, IndexedDBError> delete_index(i64 index_id);

    // Gives a record that is older than an index its keys in that index. A renderer extracts them from the value.
    ErrorOr<void, IndexedDBError> fill_index(i64 index_id, ReadonlyBytes key_of_record, ReadonlySpan<ReadonlyBytes> keys);

    struct StoredRecord {
        Optional<u64> generated_key;
    };
    ErrorOr<StoredRecord, IndexedDBError> store_record(i64 object_store_id, Optional<ReadonlyBytes> key, IndexedDBValue const&, NoOverwrite, ReadonlySpan<IndexedDBIndexKeys>);
    ErrorOr<void, IndexedDBError> delete_records(i64 object_store_id, IndexedDBKeyRange const&);
    ErrorOr<void, IndexedDBError> clear(i64 object_store_id);

    ErrorOr<void, IndexedDBError> commit(UnixDateTime now);
    ErrorOr<void, IndexedDBError> abort();

private:
    friend class IndexedDBDatabase;

    Writer(IndexedDBDatabase&, Database::Database::Transaction, Optional<u64> upgrade_version);

    template<typename Request>
    auto run_request(Request&&) -> decltype(declval<Request>()());

    template<typename Request>
    auto run_schema_change(Request&&) -> decltype(declval<Request>()());

    void finish(Optional<u64> committed_version);

    NonnullRefPtr<IndexedDBDatabase> m_database;
    Database::Database::Transaction m_transaction;
    Optional<u64> m_upgrade_version;
    bool m_is_finished { false };
};

}

template<>
struct AK::Formatter<WebView::IndexedDBError> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, WebView::IndexedDBError const& error)
    {
        TRY(Formatter<FormatString>::format(builder, "{}"sv, error.kind));
        if (error.lost_the_writer)
            TRY(builder.put_string(", which lost the writer"sv));
        if (error.cause.has_value())
            TRY(Formatter<FormatString>::format(builder, ": {}"sv, *error.cause));
        return {};
    }
};
