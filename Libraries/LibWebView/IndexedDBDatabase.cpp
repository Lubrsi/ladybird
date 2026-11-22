/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonArraySerializer.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibDatabase/ResultRow.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/IndexedDB/KeyEncoding.h>
#include <LibWebView/IndexedDBDatabase.h>
#include <errno.h>
#include <math.h>

namespace WebView {

// https://w3c.github.io/IndexedDB/#key-generator-construct
static constexpr i64 LARGEST_GENERATED_KEY = 1LL << 53;

struct IndexedDBDatabase::CheckedKeyRange {
    // The source is the object store or the index whose records are looked at.
    template<typename Bind>
    ErrorOr<void, Database::Error> bind_with_source(Bind& bind, i64 source_id) const
    {
        TRY(bind("source"sv, source_id));
        if (lower_bound.has_value())
            TRY(bind("lower_bound"sv, *lower_bound));
        if (upper_bound.has_value())
            TRY(bind("upper_bound"sv, *upper_bound));
        return {};
    }

    static RangeBound kind_of(Optional<ByteBuffer> const& bound, bool is_open)
    {
        if (!bound.has_value())
            return RangeBound::None;
        return is_open ? RangeBound::Open : RangeBound::Closed;
    }

    RangeBound kind_of_lower_bound() const { return kind_of(lower_bound, lower_bound_is_open); }
    RangeBound kind_of_upper_bound() const { return kind_of(upper_bound, upper_bound_is_open); }

    // Each bound is given the expression for the key that it is compared with.
    void append_bounds_to(StringBuilder& sql, StringView key_of_lower_bound, StringView key_of_upper_bound) const
    {
        if (kind_of_lower_bound() == RangeBound::Closed)
            sql.appendff(" AND {} >= :lower_bound", key_of_lower_bound);
        else if (kind_of_lower_bound() == RangeBound::Open)
            sql.appendff(" AND {} > :lower_bound", key_of_lower_bound);

        if (kind_of_upper_bound() == RangeBound::Closed)
            sql.appendff(" AND {} <= :upper_bound", key_of_upper_bound);
        else if (kind_of_upper_bound() == RangeBound::Open)
            sql.appendff(" AND {} < :upper_bound", key_of_upper_bound);
    }

    Optional<ByteBuffer> lower_bound;
    bool lower_bound_is_open { false };
    Optional<ByteBuffer> upper_bound;
    bool upper_bound_is_open { false };
};

// https://w3c.github.io/IndexedDB/#iterate-a-cursor
// Where a cursor is, and the key and primary key that it is to go to, in the form that records are sorted by.
struct IndexedDBDatabase::CheckedCursorBounds {
    template<typename Bind>
    ErrorOr<void, Database::Error> bind_to(Bind& bind) const
    {
        if (position.has_value())
            TRY(bind("position"sv, *position));
        if (object_store_position.has_value())
            TRY(bind("object_store_position"sv, *object_store_position));
        if (key.has_value())
            TRY(bind("key"sv, *key));
        if (primary_key.has_value())
            TRY(bind("primary_key"sv, *primary_key));
        return {};
    }

    static CursorBound kind_of(Optional<ByteBuffer> const& key, Optional<ByteBuffer> const& primary_key)
    {
        if (!key.has_value())
            return CursorBound::None;
        return primary_key.has_value() ? CursorBound::KeyAndPrimaryKey : CursorBound::Key;
    }

    CursorBound kind_of_position() const { return kind_of(position, object_store_position); }
    CursorBound kind_of_seek() const { return kind_of(key, primary_key); }

    // The requirements of a found record that are not its range. They are quoted as "next" has them, and "prev" has
    // "less than" where "next" has "greater than". A term on key can be sought with, and one on key_to_filter_by
    // cannot.
    void append_requirements_to(StringBuilder& sql, StringView key, StringView key_to_filter_by, bool is_descending) const
    {
        auto is_past = is_descending ? "<"sv : ">"sv;
        auto is_at_or_past = is_descending ? "<="sv : ">="sv;

        // * If key is defined:
        //     * The record's key is greater than or equal to key.
        if (kind_of_seek() == CursorBound::Key)
            sql.appendff(" AND {} {} :key", key, is_at_or_past);

        // * If primaryKey is defined:
        //     * The record's key is equal to key and the record's value is greater than or equal to primaryKey
        //     * The record's key is greater than key.
        // https://www.sqlite.org/rowvalue.html#row_value_comparisons
        // NB: Row values compare their first columns, and their second where the first are equal. That is either of
        //     these two, and neither holds for a key that is less than key.
        if (kind_of_seek() == CursorBound::KeyAndPrimaryKey)
            sql.appendff(" AND ({}, IndexRecords.primary_key) {} (:key, :primary_key)", key, is_at_or_past);

        // NB: A renderer throws for a key to go to that is not past the position, so beside a key to go to the position
        //     only filters.
        auto key_of_position = kind_of_seek() == CursorBound::None ? key : key_to_filter_by;

        // * If position is defined and source is an object store:
        //     * The record's key is greater than position.
        // "nextunique"
        // * If position is defined:
        //     * The record's key is greater than position.
        if (kind_of_position() == CursorBound::Key)
            sql.appendff(" AND {} {} :position", key_of_position, is_past);

        // * If position is defined and source is an index:
        //     * The record's key is equal to position and the record's value is greater than object store position
        //     * The record's key is greater than position.
        if (kind_of_position() == CursorBound::KeyAndPrimaryKey)
            sql.appendff(" AND ({}, IndexRecords.primary_key) {} (:position, :object_store_position)", key_of_position, is_past);
    }

    Optional<ByteBuffer> position;
    Optional<ByteBuffer> object_store_position;
    Optional<ByteBuffer> key;
    Optional<ByteBuffer> primary_key;
};

IndexedDBError IndexedDBError::from(Database::Error&& cause)
{
    // SQLite and AK each have their own way of saying that an allocation failed.
    bool is_out_of_memory = cause.has_result_code(Database::ResultCode::NoMemory)
        || (cause.error().is_errno() && cause.error().code() == ENOMEM);

    auto kind = is_out_of_memory ? Kind::OutOfMemory : Kind::Unknown;
    return { .kind = kind, .cause = move(cause) };
}

static IndexedDBError invalid_claim()
{
    return { .kind = IndexedDBError::Kind::InvalidClaim };
}

static IndexedDBError out_of_memory()
{
    return { .kind = IndexedDBError::Kind::OutOfMemory };
}

static ErrorOr<Web::IndexedDB::CheckedKey, IndexedDBError> check_key(ReadonlyBytes bytes, Optional<u64> injected_generated_key = {})
{
    auto checked_key = Web::IndexedDB::check_received_key(bytes, injected_generated_key);
    if (checked_key.is_error())
        return checked_key.error() == Web::IndexedDB::KeyCheckError::InvalidKey ? invalid_claim() : out_of_memory();
    return checked_key.release_value();
}

// A key path is stored as JSON: a string, or an array of strings.
// https://w3c.github.io/IndexedDB/#valid-key-path
// NB: Of what makes a key path valid, this checks that a sequence is not empty and that no string has an unpaired
//     surrogate. The rest of what makes a key path valid is not checked.
static bool can_store_key_path(IndexedDBKeyPath const& key_path)
{
    auto is_well_formed = [](Utf16String const& string) { return string.utf16_view().validate(); };
    return key_path.visit(
        [&](Utf16String const& string) { return is_well_formed(string); },
        [&](Vector<Utf16String> const& strings) { return !strings.is_empty() && all_of(strings, is_well_formed); });
}

// The key path is one that can_store_key_path() takes.
static ErrorOr<String, IndexedDBError> serialize_key_path(IndexedDBKeyPath const& key_path)
{
    auto with_utf8_of = [](Utf16String const& string, auto use) -> ErrorOr<void> {
        auto view = string.utf16_view();

        // Text stored as ASCII is UTF-8 already, and converting it copies it with an allocation that aborts when it
        // fails.
        if (view.has_ascii_storage())
            return use(StringView { view.ascii_bytes() });

        auto utf8 = TRY(view.to_utf8(AllowLonelySurrogates::No));
        return use(utf8.bytes_as_string_view());
    };

    auto serialize = [&]() -> ErrorOr<String> {
        StringBuilder json;
        TRY(key_path.visit(
            [&](Utf16String const& string) {
                return with_utf8_of(string, [&](StringView utf8) -> ErrorOr<void> {
                    TRY(json.try_append('"'));
                    TRY(json.try_append_escaped_for_json(utf8));
                    return json.try_append('"');
                });
            },
            [&](Vector<Utf16String> const& strings) -> ErrorOr<void> {
                auto array = TRY(JsonArraySerializer<>::try_create(json));
                for (auto const& string : strings)
                    TRY(with_utf8_of(string, [&](StringView utf8) { return array.add(utf8); }));
                return array.finish();
            }));
        return json.to_string();
    };

    auto serialized = serialize();
    if (serialized.is_error())
        return out_of_memory();
    return serialized.release_value();
}

static ErrorOr<Optional<IndexedDBKeyPath>, Database::Error> parse_key_path(Optional<String> const& json)
{
    if (!json.has_value())
        return Optional<IndexedDBKeyPath> {};

    auto value = TRY(JsonValue::from_string(*json));
    if (value.is_string())
        return Optional<IndexedDBKeyPath> { Utf16String::from_utf8(value.as_string()) };

    if (!value.is_array())
        return Database::Error::from_string_literal("A stored key path is neither a string nor an array");

    Vector<Utf16String> strings;
    for (auto const& element : value.as_array().values()) {
        if (!element.is_string())
            return Database::Error::from_string_literal("A stored key path holds something other than a string");
        TRY(strings.try_append(Utf16String::from_utf8(element.as_string())));
    }
    return Optional<IndexedDBKeyPath> { move(strings) };
}

static ErrorOr<void, Database::Error> migrate_schema(Database::Database& database, String const& storage_key, Utf16String const& name, UnixDateTime now)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = 1,
            .sql = R"#(
                CREATE TABLE Metadata (
                    storage_key TEXT NOT NULL,
                    name TEXT NOT NULL,
                    version INTEGER NOT NULL,
                    last_open_or_write_time INTEGER NOT NULL
                );

                CREATE TABLE ObjectStores (
                    id INTEGER PRIMARY KEY,
                    name TEXT NOT NULL UNIQUE,
                    key_path TEXT,
                    key_generator INTEGER
                );

                CREATE TABLE Records (
                    id INTEGER PRIMARY KEY,
                    object_store_id INTEGER NOT NULL REFERENCES ObjectStores(id) ON DELETE CASCADE,
                    key BLOB NOT NULL,
                    exact_key BLOB,
                    value BLOB,
                    UNIQUE (object_store_id, key)
                );

                CREATE TABLE Indexes (
                    id INTEGER PRIMARY KEY,
                    object_store_id INTEGER NOT NULL REFERENCES ObjectStores(id) ON DELETE CASCADE,
                    name TEXT NOT NULL,
                    key_path TEXT NOT NULL,
                    is_unique INTEGER NOT NULL,
                    is_multi_entry INTEGER NOT NULL,
                    UNIQUE (object_store_id, name)
                );

                CREATE TABLE IndexRecords (
                    index_id INTEGER NOT NULL REFERENCES Indexes(id) ON DELETE CASCADE,
                    key BLOB NOT NULL,
                    primary_key BLOB NOT NULL,
                    exact_key BLOB,
                    exact_primary_key BLOB,
                    record_id INTEGER NOT NULL REFERENCES Records(id) ON DELETE CASCADE,
                    PRIMARY KEY (index_id, key, primary_key)
                ) WITHOUT ROWID;

                CREATE INDEX IndexRecordsByRecord ON IndexRecords(record_id);
            )#"sv,
            // The one row of Metadata is there from the migration's own transaction on.
            .backfill = [&](Database::Database&) -> ErrorOr<void, Database::Error> {
                auto insert_metadata = TRY(database.prepare_statement("INSERT INTO Metadata (storage_key, name, version, last_open_or_write_time) VALUES (:storage_key, :name, 0, :time);"sv));
                return database.try_execute_bound_statement(insert_metadata, [&](auto& bind) -> ErrorOr<void, Database::Error> {
                    TRY(bind("storage_key"sv, storage_key));
                    TRY(bind("name"sv, name));
                    return bind("time"sv, now);
                });
            },
        },
    });

    auto outcome = TRY(database.migrate("IndexedDB"sv, migrations));
    VERIFY(outcome == Database::MigrationOutcome::Success);
    return {};
}

ErrorOr<NonnullRefPtr<IndexedDBDatabase>, IndexedDBError> IndexedDBDatabase::create_transient(String const& storage_key, Utf16String const& name, UnixDateTime now)
{
    auto create = [&]() -> ErrorOr<NonnullRefPtr<IndexedDBDatabase>, Database::Error> {
        auto database = TRY(Database::Database::create_memory_backed({
            .foreign_keys = Database::Database::ForeignKeys::Yes,
            .temp_store = Database::Database::TempStore::Memory,
        }));

        // https://www.sqlite.org/pragma.html#pragma_auto_vacuum
        // SQLite only takes this before the database has its first table, which the migration creates.
        TRY(database->execute_raw("PRAGMA auto_vacuum = INCREMENTAL;"));
        TRY(migrate_schema(*database, storage_key, name, now));

        Statements statements {};
        statements.set_version = TRY(database->prepare_statement("UPDATE Metadata SET version = :version;"sv));
        statements.set_last_open_or_write_time = TRY(database->prepare_statement("UPDATE Metadata SET last_open_or_write_time = :time;"sv));
        statements.get_last_open_or_write_time = TRY(database->prepare_statement("SELECT last_open_or_write_time AS time FROM Metadata;"sv));
        statements.get_object_stores = TRY(database->prepare_statement("SELECT id, name, key_path, key_generator IS NOT NULL AS has_key_generator FROM ObjectStores ORDER BY id;"sv));
        statements.get_object_store = TRY(database->prepare_statement("SELECT key_path IS NOT NULL AS has_in_line_keys, key_generator FROM ObjectStores WHERE id = :store;"sv));
        statements.create_object_store = TRY(database->prepare_statement("INSERT INTO ObjectStores (name, key_path, key_generator) VALUES (:name, :key_path, :key_generator) RETURNING id;"sv));
        statements.rename_object_store = TRY(database->prepare_statement("UPDATE ObjectStores SET name = :name WHERE id = :store RETURNING id;"sv));
        statements.delete_object_store = TRY(database->prepare_statement("DELETE FROM ObjectStores WHERE id = :store RETURNING id;"sv));
        statements.set_key_generator = TRY(database->prepare_statement("UPDATE ObjectStores SET key_generator = :current_number WHERE id = :store;"sv));
        statements.get_record = TRY(database->prepare_statement("SELECT id, exact_key FROM Records WHERE object_store_id = :store AND key = :key;"sv));
        statements.delete_record = TRY(database->prepare_statement("DELETE FROM Records WHERE id = :record;"sv));
        statements.insert_record = TRY(database->prepare_statement("INSERT INTO Records (object_store_id, key, exact_key, value) VALUES (:store, :key, :exact_key, :value) RETURNING id;"sv));
        statements.get_indexes = TRY(database->prepare_statement("SELECT id, object_store_id, name, key_path, is_unique, is_multi_entry FROM Indexes ORDER BY object_store_id, id;"sv));
        statements.get_index = TRY(database->prepare_statement("SELECT object_store_id, is_unique, is_multi_entry FROM Indexes WHERE id = :index;"sv));
        statements.create_index = TRY(database->prepare_statement("INSERT INTO Indexes (object_store_id, name, key_path, is_unique, is_multi_entry) VALUES (:store, :name, :key_path, :is_unique, :is_multi_entry) RETURNING id;"sv));
        statements.rename_index = TRY(database->prepare_statement("UPDATE Indexes SET name = :name WHERE id = :index RETURNING id;"sv));
        statements.delete_index = TRY(database->prepare_statement("DELETE FROM Indexes WHERE id = :index RETURNING id;"sv));
        statements.get_whether_index_has_key = TRY(database->prepare_statement("SELECT EXISTS (SELECT 1 FROM IndexRecords WHERE index_id = :index AND key = :key) AS has_key;"sv));

        // https://www.sqlite.org/lang_upsert.html
        // Only a record's own keys can meet in the primary key, and of the equal ones the first is kept.
        statements.insert_index_record = TRY(database->prepare_statement(R"#(
            INSERT INTO IndexRecords (index_id, key, primary_key, exact_key, exact_primary_key, record_id)
            VALUES (:index, :key, :primary_key, :exact_key, :exact_primary_key, :record)
            ON CONFLICT DO NOTHING;
        )#"sv));

        return TRY(adopt_nonnull_ref_or_enomem(new (nothrow) IndexedDBDatabase(move(database), statements)));
    };

    auto database = create();
    if (database.is_error())
        return IndexedDBError::from(database.release_error());
    return database.release_value();
}

IndexedDBDatabase::IndexedDBDatabase(NonnullRefPtr<Database::Database> database, Statements statements)
    : m_sqlite_database(move(database))
    , m_statements(statements)
{
}

IndexedDBDatabase::~IndexedDBDatabase() = default;

IndexedDBError IndexedDBDatabase::failure(Database::Error&& cause)
{
    auto error = IndexedDBError::from(move(cause));

    // https://www.sqlite.org/c3ref/get_autocommit.html
    // After some errors SQLite rolls back the transaction that is open on the connection, and the failed statement
    // need not be the writer's own: a read that runs out of memory can do it too. Only autocommit mode tells.
    if (m_writer && !m_writer->m_transaction.is_open()) {
        error.lost_the_writer = true;
        m_writer->finish({});
    }

    return error;
}

// A renderer throws for a name that is taken before it sends anything.
IndexedDBError IndexedDBDatabase::failure_to_give_a_name(Database::Error&& cause)
{
    bool name_is_taken = cause.has_result_code(Database::ResultCode::ConstraintUnique);

    auto error = failure(move(cause));
    if (name_is_taken)
        error.kind = IndexedDBError::Kind::InvalidClaim;
    return error;
}

ErrorOr<NonnullOwnPtr<IndexedDBDatabase::Writer>, IndexedDBError> IndexedDBDatabase::begin_writer()
{
    return begin_writer_for({});
}

// https://w3c.github.io/IndexedDB/#upgrade-a-database
ErrorOr<NonnullOwnPtr<IndexedDBDatabase::Writer>, IndexedDBError> IndexedDBDatabase::begin_upgrade(u64 version)
{
    if (version > LARGEST_VERSION)
        return invalid_claim();

    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // NB: An open only upgrades a database whose version is less than the one it asks for.
    if (version <= m_committed_version)
        return IndexedDBError { .kind = IndexedDBError::Kind::Version };

    // 2. Let transaction be a new upgrade transaction with connection used as connection.
    auto writer = TRY(begin_writer_for(version));

    // 8. Set db's version to version. This change is considered part of the transaction, and so if the transaction is
    //    aborted, this change is reverted.
    TRY(writer->run_request([&] { return write_version(version); }));

    return writer;
}

ErrorOr<NonnullOwnPtr<IndexedDBDatabase::Writer>, IndexedDBError> IndexedDBDatabase::begin_writer_for(Optional<u64> upgrade_version)
{
    auto transaction = m_sqlite_database->begin_transaction();
    if (transaction.is_error())
        return failure(transaction.release_error());

    auto writer = adopt_nonnull_own_or_enomem(new (nothrow) Writer(*this, transaction.release_value(), upgrade_version));
    if (writer.is_error())
        return out_of_memory();

    m_writer = writer.value().ptr();
    return writer.release_value();
}

void IndexedDBDatabase::writer_did_finish(Optional<u64> committed_version)
{
    m_writer = nullptr;
    if (committed_version.has_value())
        m_committed_version = *committed_version;

    if (!m_held_open_time.has_value())
        return;

    auto held_open_time = *m_held_open_time;
    if (!write_last_open_or_write_time(held_open_time).is_error())
        did_commit_last_open_or_write_time(held_open_time);
}

ErrorOr<Vector<IndexedDBObjectStoreDescription>, IndexedDBError> IndexedDBDatabase::object_stores()
{
    Vector<IndexedDBObjectStoreDescription> object_stores;

    auto result = m_sqlite_database->try_execute_bound_statement(
        m_statements.get_object_stores,
        [](auto&) -> ErrorOr<void, Database::Error> { return {}; },
        [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            IndexedDBObjectStoreDescription object_store {
                .id = TRY(row.read_integer<i64>("id"sv)),
                .name = TRY(row.read_utf16_text("name"sv, MAXIMUM_CELL_SIZE)),
                .key_path = TRY(parse_key_path(TRY(row.read_optional_text("key_path"sv, MAXIMUM_CELL_SIZE)))),
                .has_key_generator = TRY(row.read_bool("has_key_generator"sv)),
                .indexes = {},
            };
            TRY(object_stores.try_append(move(object_store)));
            return {};
        });
    if (result.is_error())
        return failure(result.release_error());

    // Both lists are in the order of the object stores' ids, so each index belongs to the object store that the walk
    // has reached or to a later one.
    size_t position = 0;
    result = m_sqlite_database->try_execute_bound_statement(
        m_statements.get_indexes,
        [](auto&) -> ErrorOr<void, Database::Error> { return {}; },
        [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            auto object_store_id = TRY(row.read_integer<i64>("object_store_id"sv));
            while (position < object_stores.size() && object_stores[position].id < object_store_id)
                ++position;
            if (position == object_stores.size() || object_stores[position].id != object_store_id)
                return Database::Error::from_string_literal("A stored index is of an object store that does not exist");

            auto key_path = TRY(parse_key_path(TRY(row.read_text("key_path"sv, MAXIMUM_CELL_SIZE))));
            IndexedDBIndexDescription index {
                .id = TRY(row.read_integer<i64>("id"sv)),
                .name = TRY(row.read_utf16_text("name"sv, MAXIMUM_CELL_SIZE)),
                .key_path = key_path.release_value(),
                .is_unique = TRY(row.read_bool("is_unique"sv)),
                .is_multi_entry = TRY(row.read_bool("is_multi_entry"sv)),
            };
            TRY(object_stores[position].indexes.try_append(move(index)));
            return {};
        });
    if (result.is_error())
        return failure(result.release_error());

    return object_stores;
}

ErrorOr<IndexedDBDatabase::CheckedKeyRange, IndexedDBError> IndexedDBDatabase::check_key_range(IndexedDBKeyRange const& key_range)
{
    CheckedKeyRange checked_range {
        .lower_bound = {},
        .lower_bound_is_open = key_range.lower_bound_is_open,
        .upper_bound = {},
        .upper_bound_is_open = key_range.upper_bound_is_open,
    };

    // A range is compared in the form that records are sorted by.
    if (key_range.lower_bound.has_value())
        checked_range.lower_bound = TRY(check_key(*key_range.lower_bound)).sort_form;
    if (key_range.upper_bound.has_value())
        checked_range.upper_bound = TRY(check_key(*key_range.upper_bound)).sort_form;

    return checked_range;
}

// A key is read back as it was written, which for a key that holds a -0 is not the form that it is sorted by.
static constexpr auto KEY_OF_A_RECORD = "COALESCE(exact_key, key) AS record_key"sv;
static constexpr auto KEYS_OF_AN_INDEX_RECORD = "COALESCE(IndexRecords.exact_key, IndexRecords.key) AS index_key, COALESCE(exact_primary_key, primary_key) AS record_key"sv;

// An index sorts the records of one key by the keys of the records that they refer to.
static constexpr auto IS_THE_FIRST_RECORD_OF_ITS_KEY = " AND NOT EXISTS (SELECT 1 FROM IndexRecords AS Earlier WHERE Earlier.index_id = IndexRecords.index_id AND Earlier.key = IndexRecords.key AND Earlier.primary_key < IndexRecords.primary_key)"sv;

static ErrorOr<IndexedDBRecord, Database::Error> read_record(Database::ResultRow& row, IndexedDBDatabase::WithValues with_values)
{
    IndexedDBRecord record { .key = TRY(row.read_blob("record_key"sv, IndexedDBDatabase::MAXIMUM_CELL_SIZE)), .value = {} };
    if (with_values == IndexedDBDatabase::WithValues::Yes)
        record.value = TRY(row.read_blob("record_value"sv, IndexedDBDatabase::MAXIMUM_CELL_SIZE));

    return record;
}

static ErrorOr<IndexedDBIndexRecord, Database::Error> read_index_record(Database::ResultRow& row, IndexedDBDatabase::WithValues with_values)
{
    IndexedDBIndexRecord record {
        .key = TRY(row.read_blob("index_key"sv, IndexedDBDatabase::MAXIMUM_CELL_SIZE)),
        .primary_key = TRY(row.read_blob("record_key"sv, IndexedDBDatabase::MAXIMUM_CELL_SIZE)),
        .value = {},
    };
    if (with_values == IndexedDBDatabase::WithValues::Yes)
        record.value = TRY(row.read_blob("record_value"sv, IndexedDBDatabase::MAXIMUM_CELL_SIZE));

    return record;
}

StringView IndexedDBDatabase::RangeStatementShape::key() const
{
    return source == RangeSource::Index ? "IndexRecords.key"sv : "Records.key"sv;
}

void IndexedDBDatabase::RangeStatementShape::append_order_to(StringBuilder& sql) const
{
    auto order = is_descending ? "DESC"sv : "ASC"sv;
    sql.appendff(" ORDER BY {} {}", key(), order);
    if (source == RangeSource::Index)
        sql.appendff(", IndexRecords.primary_key {}", order);
}

ErrorOr<Database::StatementID, IndexedDBError> IndexedDBDatabase::range_statement(RangeSource source, RangeStatement statement, CheckedKeyRange const& range, Direction direction, RecordsOfEachKey records_of_each_key)
{
    bool is_read = statement == RangeStatement::ReadKeys || statement == RangeStatement::ReadKeysAndValues;
    bool is_of_an_index = source == RangeSource::Index;
    bool is_descending = is_read && direction == Direction::Descending;

    // An object store has one record for each key.
    bool has_the_first_record_of_each_key = is_read && is_of_an_index && records_of_each_key == RecordsOfEachKey::First;

    RangeStatementShape shape {
        .source = source,
        .statement = statement,
        .lower_bound = range.kind_of_lower_bound(),
        .upper_bound = range.kind_of_upper_bound(),
        .is_descending = is_descending,
        .has_the_first_record_of_each_key = has_the_first_record_of_each_key,
        .position = CursorBound::None,
        .seek = CursorBound::None,
    };
    if (auto prepared = m_range_statements.get(shape); prepared.has_value())
        return *prepared;

    StringBuilder sql;
    if (is_of_an_index) {
        switch (statement) {
        case RangeStatement::Count:
            sql.append("SELECT COUNT(*) AS count FROM IndexRecords"sv);
            break;
        case RangeStatement::ReadKeys:
            sql.appendff("SELECT {} FROM IndexRecords", KEYS_OF_AN_INDEX_RECORD);
            break;
        case RangeStatement::ReadKeysAndValues:
            sql.appendff("SELECT {}, Records.value AS record_value FROM IndexRecords JOIN Records ON Records.id = IndexRecords.record_id", KEYS_OF_AN_INDEX_RECORD);
            break;
        case RangeStatement::Delete:
        case RangeStatement::IterateKeys:
        case RangeStatement::IterateKeysAndValues:
            VERIFY_NOT_REACHED();
        }
        sql.append(" WHERE IndexRecords.index_id = :source"sv);
    } else {
        switch (statement) {
        case RangeStatement::Count:
            sql.append("SELECT COUNT(*) AS count FROM Records"sv);
            break;
        case RangeStatement::ReadKeys:
            sql.appendff("SELECT {} FROM Records", KEY_OF_A_RECORD);
            break;
        case RangeStatement::ReadKeysAndValues:
            sql.appendff("SELECT {}, value AS record_value FROM Records", KEY_OF_A_RECORD);
            break;
        case RangeStatement::Delete:
            sql.append("DELETE FROM Records"sv);
            break;
        case RangeStatement::IterateKeys:
        case RangeStatement::IterateKeysAndValues:
            VERIFY_NOT_REACHED();
        }
        sql.append(" WHERE Records.object_store_id = :source"sv);
    }
    range.append_bounds_to(sql, shape.key(), shape.key());

    if (has_the_first_record_of_each_key)
        sql.append(IS_THE_FIRST_RECORD_OF_ITS_KEY);

    if (is_read) {
        shape.append_order_to(sql);
        sql.append(" LIMIT :limit"sv);
    }
    sql.append(';');

    return prepare_range_statement(shape, sql.string_view());
}

ErrorOr<Database::StatementID, IndexedDBError> IndexedDBDatabase::prepare_range_statement(RangeStatementShape const& shape, StringView sql)
{
    auto prepared = m_sqlite_database->prepare_statement(sql);
    if (prepared.is_error())
        return failure(prepared.release_error());
    if (m_range_statements.try_set(shape, prepared.value()).is_error())
        return out_of_memory();

    return prepared.release_value();
}

// https://w3c.github.io/IndexedDB/#iterate-a-cursor
// A statement for the turns of step 9. Of the records that meet the requirements, it finds the one that :skip of them
// come before.
ErrorOr<Database::StatementID, IndexedDBError> IndexedDBDatabase::cursor_statement(RangeSource source, WithValues with_values, CheckedKeyRange const& range, CheckedCursorBounds const& bounds, Direction direction, RecordsOfEachKey records_of_each_key)
{
    bool is_of_an_index = source == RangeSource::Index;
    bool is_descending = direction == Direction::Descending;
    bool has_values = with_values == WithValues::Yes;

    // An object store has one record for each key.
    bool has_the_first_record_of_each_key = is_of_an_index && records_of_each_key == RecordsOfEachKey::First;

    RangeStatementShape shape {
        .source = source,
        .statement = has_values ? RangeStatement::IterateKeysAndValues : RangeStatement::IterateKeys,
        .lower_bound = range.kind_of_lower_bound(),
        .upper_bound = range.kind_of_upper_bound(),
        .is_descending = is_descending,
        .has_the_first_record_of_each_key = has_the_first_record_of_each_key,
        .position = bounds.kind_of_position(),
        .seek = bounds.kind_of_seek(),
    };
    if (auto prepared = m_range_statements.get(shape); prepared.has_value())
        return *prepared;

    StringBuilder sql;

    // A value is read for the record that is found, and for none of the records that are skipped.
    if (has_values) {
        auto keys_of_the_found_record = is_of_an_index ? "Found.index_key AS index_key, Found.record_key AS record_key"sv : "Found.record_key AS record_key"sv;
        sql.appendff("SELECT {}, Records.value AS record_value FROM (", keys_of_the_found_record);
    }

    if (is_of_an_index)
        sql.appendff("SELECT {}, record_id FROM IndexRecords WHERE IndexRecords.index_id = :source", KEYS_OF_AN_INDEX_RECORD);
    else
        sql.appendff("SELECT {}, id AS record_id FROM Records WHERE Records.object_store_id = :source", KEY_OF_A_RECORD);

    // https://www.sqlite.org/optoverview.html#disqualifying_where_clause_terms_using_unary_
    // Of the terms that say where the records start in the cursor's direction, SQLite seeks with one and filters with
    // the rest. The key to go to is the furthest on, then the position, then the range's bound, and a unary + keeps
    // the ones behind from being the one that is sought with.
    auto key = shape.key();
    auto key_to_filter_by = is_of_an_index ? "+IndexRecords.key"sv : "+Records.key"sv;
    bool starts_at_the_range = bounds.kind_of_position() == CursorBound::None && bounds.kind_of_seek() == CursorBound::None;
    auto key_of_first_bound = starts_at_the_range ? key : key_to_filter_by;

    // * The record's key is in range.
    if (is_descending)
        range.append_bounds_to(sql, key, key_of_first_bound);
    else
        range.append_bounds_to(sql, key_of_first_bound, key);

    bounds.append_requirements_to(sql, key, key_to_filter_by, is_descending);

    // "prevunique"
    // If temp record is defined, let found record be the first record in records whose key is equal to temp record's
    // key.
    // NB: The first record that "nextunique" finds past a position is the first record of its key as well.
    if (has_the_first_record_of_each_key)
        sql.append(IS_THE_FIRST_RECORD_OF_ITS_KEY);

    // "next"
    // Let found record be the first record in records which satisfy all of the following requirements:
    // "prev"
    // Let found record be the last record in records which satisfy all of the following requirements:
    shape.append_order_to(sql);
    sql.append(" LIMIT 1 OFFSET :skip"sv);

    if (has_values)
        sql.append(") AS Found JOIN Records ON Records.id = Found.record_id"sv);
    sql.append(';');

    return prepare_range_statement(shape, sql.string_view());
}

ErrorOr<u64, IndexedDBError> IndexedDBDatabase::count(i64 object_store_id, IndexedDBKeyRange const& key_range)
{
    return count_the_records_in_a_range(RangeSource::ObjectStore, object_store_id, key_range);
}

ErrorOr<u64, IndexedDBError> IndexedDBDatabase::count_index_records(i64 index_id, IndexedDBKeyRange const& key_range)
{
    return count_the_records_in_a_range(RangeSource::Index, index_id, key_range);
}

// https://w3c.github.io/IndexedDB/#count-the-records-in-a-range
ErrorOr<u64, IndexedDBError> IndexedDBDatabase::count_the_records_in_a_range(RangeSource source, i64 source_id, IndexedDBKeyRange const& key_range)
{
    auto range = TRY(check_key_range(key_range));
    auto statement = TRY(range_statement(source, RangeStatement::Count, range, Direction::Ascending, RecordsOfEachKey::All));

    // 1. Let count be the number of records, if any, in source's list of records with key in range.
    auto count = m_sqlite_database->try_execute_bound_statement_one<u64>(
        statement,
        [&](auto& bind) { return range.bind_with_source(bind, source_id); },
        [](Database::ResultRow& row) { return row.read_integer<u64>("count"sv); });
    if (count.is_error())
        return failure(count.release_error());

    // 2. Return count.
    return count.release_value();
}

// https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-object-store
ErrorOr<Vector<IndexedDBRecord>, IndexedDBError> IndexedDBDatabase::read_records(i64 object_store_id, IndexedDBKeyRange const& key_range, Direction direction, Optional<u32> count, WithValues with_values)
{
    auto range = TRY(check_key_range(key_range));
    auto kind_of_statement = with_values == WithValues::Yes ? RangeStatement::ReadKeysAndValues : RangeStatement::ReadKeys;
    auto statement = TRY(range_statement(RangeSource::ObjectStore, kind_of_statement, range, direction, RecordsOfEachKey::All));

    // 1. If count is not given or is 0 (zero), let count be infinity.
    // https://www.sqlite.org/lang_select.html#the_limit_clause
    // NB: A negative limit is no limit.
    i64 limit = count.has_value() && *count != 0 ? static_cast<i64>(*count) : -1;

    // 2. Let records be an empty list of records.
    Vector<IndexedDBRecord> records;

    // 3. If direction is "next" or "nextunique", set records to the first count of store's list of records whose key
    //    is in range.
    // 4. If direction is "prev" or "prevunique", set records to the last count of store's list of records whose key is
    //    in range.
    auto result = m_sqlite_database->try_execute_bound_statement(
        statement,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(range.bind_with_source(bind, object_store_id));
            return bind("limit"sv, limit);
        },
        [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            TRY(records.try_append(TRY(read_record(row, with_values))));
            return {};
        });
    if (result.is_error())
        return failure(result.release_error());

    return records;
}

// https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-index
ErrorOr<Vector<IndexedDBIndexRecord>, IndexedDBError> IndexedDBDatabase::read_index_records(i64 index_id, IndexedDBKeyRange const& key_range, Direction direction, RecordsOfEachKey records_of_each_key, Optional<u32> count, WithValues with_values)
{
    auto range = TRY(check_key_range(key_range));
    auto kind_of_statement = with_values == WithValues::Yes ? RangeStatement::ReadKeysAndValues : RangeStatement::ReadKeys;
    auto statement = TRY(range_statement(RangeSource::Index, kind_of_statement, range, direction, records_of_each_key));

    // 1. If count is not given or is 0 (zero), let count be infinity.
    // https://www.sqlite.org/lang_select.html#the_limit_clause
    // NB: A negative limit is no limit.
    i64 limit = count.has_value() && *count != 0 ? static_cast<i64>(*count) : -1;

    // 2. Let records be an empty list of records.
    Vector<IndexedDBIndexRecord> records;

    // 3. Switching on direction:
    //    "next"
    //    1. Set records to the first count of index's list of records whose key is in range.
    //    "prev"
    //    1. Set records to the last count of index's list of records whose key is in range.
    // AD-HOC: For "nextunique" and "prevunique" this reads the first record of each key in range, in the direction's
    //         order, and then takes the count, as a cursor iterates them and as web-platform-tests expects. The
    //         steps leave out the first record in range, and for "prevunique" a count takes the first keys in range,
    //         where a cursor starts from the last. See: https://github.com/w3c/IndexedDB/issues/480
    auto result = m_sqlite_database->try_execute_bound_statement(
        statement,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(range.bind_with_source(bind, index_id));
            return bind("limit"sv, limit);
        },
        [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            TRY(records.try_append(TRY(read_index_record(row, with_values))));
            return {};
        });
    if (result.is_error())
        return failure(result.release_error());

    return records;
}

ErrorOr<Optional<IndexedDBRecord>, IndexedDBError> IndexedDBDatabase::iterate_cursor(i64 object_store_id, IndexedDBKeyRange const& key_range, Direction direction, Optional<ReadonlyBytes> position, Optional<ReadonlyBytes> key, u32 count, WithValues with_values)
{
    CheckedCursorBounds bounds;
    if (position.has_value())
        bounds.position = TRY(check_key(*position)).sort_form;
    if (key.has_value())
        bounds.key = TRY(check_key(*key)).sort_form;

    // An object store has one record for each key.
    return iterate_a_cursor(RangeSource::ObjectStore, object_store_id, key_range, direction, RecordsOfEachKey::All, move(bounds), count, with_values, read_record);
}

ErrorOr<Optional<IndexedDBIndexRecord>, IndexedDBError> IndexedDBDatabase::iterate_index_cursor(i64 index_id, IndexedDBKeyRange const& key_range, Direction direction, RecordsOfEachKey records_of_each_key, Optional<IndexedDBIndexCursorPosition> const& position, Optional<IndexedDBIndexCursorSeek> const& seek, u32 count, WithValues with_values)
{
    CheckedCursorBounds bounds;
    if (position.has_value()) {
        bounds.position = TRY(check_key(position->key)).sort_form;
        bounds.object_store_position = TRY(check_key(position->primary_key)).sort_form;
    }
    if (seek.has_value()) {
        bounds.key = TRY(check_key(seek->key)).sort_form;
        if (seek->primary_key.has_value())
            bounds.primary_key = TRY(check_key(*seek->primary_key)).sort_form;
    }

    return iterate_a_cursor(RangeSource::Index, index_id, key_range, direction, records_of_each_key, move(bounds), count, with_values, read_index_record);
}

// https://w3c.github.io/IndexedDB/#iterate-a-cursor
// NB: The steps that set the cursor are the renderer's, which keeps it.
template<typename Record>
ErrorOr<Optional<Record>, IndexedDBError> IndexedDBDatabase::iterate_a_cursor(RangeSource source, i64 source_id, IndexedDBKeyRange const& key_range, Direction direction, RecordsOfEachKey records_of_each_key, CheckedCursorBounds bounds, u32 count, WithValues with_values, ErrorOr<Record, Database::Error> (*read_found_record)(Database::ResultRow&, WithValues))
{
    // 1. Let source be cursor's source.
    // 2. Let direction be cursor's direction.
    // NB: "nextunique" and "prevunique" are "next" and "prev" for an object store, which has one record for each key.
    bool visits_each_key_once = source == RangeSource::Index && records_of_each_key == RecordsOfEachKey::First;

    // 3. Assert: if primaryKey is given, source is an index and direction is "next" or "prev".
    // NB: A renderer throws for another direction before it sends the request, and only an index is asked for a
    //     primary key.
    if (bounds.primary_key.has_value() && visits_each_key_once)
        return invalid_claim();

    // 4. Let records be the list of records in source.
    // 5. Let range be cursor's range.
    auto range = TRY(check_key_range(key_range));

    // 6. Let position be cursor's position.
    // 7. Let object store position be cursor's object store position.
    // NB: "nextunique" and "prevunique" require a record's key to be past the position, whatever its value is.
    if (visits_each_key_once)
        bounds.object_store_position.clear();

    // 8. If count is not given, let count be 1.
    // NB: A request always gives the count.

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-advance
    // 1. If count is 0 (zero), throw a TypeError.
    // NB: A renderer does this before it sends the request.
    if (count == 0)
        return invalid_claim();

    // https://w3c.github.io/IndexedDB/#iterate-a-cursor
    // 9. While count is greater than 0:
    //    1. Switch on direction: [...]
    //    2. If found record is not defined, then: [...]
    //       4. Return null.
    //    3. Let position be found record's key.
    //    4. If source is an index, let object store position be found record's value.
    //    5. Decrease count by 1.
    // NB: Each turn finds the next record that meets the requirements, in the direction's order, so the last turn
    //     finds the one that count - 1 of them come before.
    auto statement = TRY(cursor_statement(source, with_values, range, bounds, direction, records_of_each_key));

    // 13. If cursor's key only flag is false, then:
    //     1. Let serialized be found record's value if source is an object store, or found record's referenced value
    //        otherwise.
    // NB: The renderer does the rest of step 13, and steps 10 to 12 and 14, with the record that it gets.
    auto found_record = m_sqlite_database->try_execute_bound_statement_optional<Record>(
        statement,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(range.bind_with_source(bind, source_id));
            TRY(bounds.bind_to(bind));
            return bind("skip"sv, static_cast<i64>(count) - 1);
        },
        [&](Database::ResultRow& row) { return read_found_record(row, with_values); });
    if (found_record.is_error())
        return failure(found_record.release_error());

    // 15. Return cursor.
    return found_record.release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::write_version(u64 version)
{
    auto result = m_sqlite_database->try_execute_bound_statement(m_statements.set_version, [&](auto& bind) {
        return bind("version"sv, static_cast<i64>(version));
    });
    if (result.is_error())
        return failure(result.release_error());
    return {};
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::write_last_open_or_write_time(UnixDateTime time)
{
    auto result = m_sqlite_database->try_execute_bound_statement(m_statements.set_last_open_or_write_time, [&](auto& bind) {
        return bind("time"sv, time);
    });
    if (result.is_error())
        return failure(result.release_error());
    return {};
}

ErrorOr<UnixDateTime, IndexedDBError> IndexedDBDatabase::write_time_of_commit(UnixDateTime now)
{
    // An open that happened while the writer was at work is not older than the write it is marked with.
    auto time = max(now, m_held_open_time.value_or(now));
    TRY(write_last_open_or_write_time(time));
    return time;
}

void IndexedDBDatabase::did_commit_last_open_or_write_time(UnixDateTime time)
{
    if (m_held_open_time.has_value() && *m_held_open_time <= time)
        m_held_open_time.clear();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::mark_opened(UnixDateTime now)
{
    if (m_writer) {
        m_held_open_time = max(now, m_held_open_time.value_or(now));
        return {};
    }

    TRY(write_last_open_or_write_time(now));
    did_commit_last_open_or_write_time(now);
    return {};
}

ErrorOr<UnixDateTime, IndexedDBError> IndexedDBDatabase::last_open_or_write_time()
{
    auto milliseconds = m_sqlite_database->try_execute_bound_statement_one<i64>(
        m_statements.get_last_open_or_write_time,
        [](auto&) -> ErrorOr<void, Database::Error> { return {}; },
        [](Database::ResultRow& row) { return row.read_integer<i64>("time"sv); });
    if (milliseconds.is_error())
        return failure(milliseconds.release_error());

    return UnixDateTime::from_milliseconds_since_epoch(milliseconds.value());
}

ErrorOr<i64, IndexedDBError> IndexedDBDatabase::add_object_store(Utf16String const& name, Optional<String> const& serialized_key_path, KeyGenerator key_generator)
{
    // https://w3c.github.io/IndexedDB/#key-generator-construct
    // The initial value of a key generator's current number is 1, set when the associated object store is created.
    Optional<i64> current_number;
    if (key_generator == KeyGenerator::Yes)
        current_number = 1;

    auto object_store_id = m_sqlite_database->try_execute_bound_statement_one<i64>(
        m_statements.create_object_store,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("name"sv, name));
            TRY(bind("key_path"sv, serialized_key_path));
            return bind("key_generator"sv, current_number);
        },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (object_store_id.is_error())
        return failure_to_give_a_name(object_store_id.release_error());

    return object_store_id.release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::set_name_of_object_store(i64 object_store_id, Utf16String const& name)
{
    auto renamed_object_store = m_sqlite_database->try_execute_bound_statement_optional<i64>(
        m_statements.rename_object_store,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("name"sv, name));
            return bind("store"sv, object_store_id);
        },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (renamed_object_store.is_error())
        return failure_to_give_a_name(renamed_object_store.release_error());
    if (!renamed_object_store.value().has_value())
        return invalid_claim();

    return {};
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::remove_object_store(i64 object_store_id)
{
    auto removed_object_store = m_sqlite_database->try_execute_bound_statement_optional<i64>(
        m_statements.delete_object_store,
        [&](auto& bind) { return bind("store"sv, object_store_id); },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (removed_object_store.is_error())
        return failure(removed_object_store.release_error());
    if (!removed_object_store.value().has_value())
        return invalid_claim();

    return {};
}

ErrorOr<IndexedDBDatabase::KeysOfObjectStore, IndexedDBError> IndexedDBDatabase::keys_of_object_store(i64 object_store_id)
{
    auto keys = m_sqlite_database->try_execute_bound_statement_optional<KeysOfObjectStore>(
        m_statements.get_object_store,
        [&](auto& bind) { return bind("store"sv, object_store_id); },
        [](Database::ResultRow& row) -> ErrorOr<KeysOfObjectStore, Database::Error> {
            return KeysOfObjectStore {
                .are_in_line = TRY(row.read_bool("has_in_line_keys"sv)),
                .generator_current_number = TRY(row.read_optional_integer<i64>("key_generator"sv)),
            };
        });
    if (keys.is_error())
        return failure(keys.release_error());
    if (!keys.value().has_value())
        return invalid_claim();

    return keys.release_value().release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::set_key_generator(i64 object_store_id, i64 current_number)
{
    auto result = m_sqlite_database->try_execute_bound_statement(m_statements.set_key_generator, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("current_number"sv, current_number));
        return bind("store"sv, object_store_id);
    });
    if (result.is_error())
        return failure(result.release_error());
    return {};
}

// https://w3c.github.io/IndexedDB/#generate-a-key
ErrorOr<u64, IndexedDBError> IndexedDBDatabase::generate_a_key(i64 object_store_id, i64 current_number)
{
    // 1. Let generator be store's key generator.
    // 2. Let key be generator's current number.
    auto key = current_number;

    // 3. If key is greater than 2^53 (9007199254740992), then return failure.
    if (key > LARGEST_GENERATED_KEY)
        return IndexedDBError { .kind = IndexedDBError::Kind::Constraint };

    // 4. Increase generator's current number by 1.
    TRY(set_key_generator(object_store_id, current_number + 1));

    // 5. Return key.
    return static_cast<u64>(key);
}

// https://w3c.github.io/IndexedDB/#possibly-update-the-key-generator
ErrorOr<void, IndexedDBError> IndexedDBDatabase::possibly_update_the_key_generator(i64 object_store_id, i64 current_number, ReadonlyBytes checked_key)
{
    // 1. If the type of key is not number, abort these steps.
    auto number = Web::IndexedDB::number_of_key(checked_key);
    if (!number.has_value())
        return {};

    // 2. Let value be the value of key.
    auto value = *number;

    // 3. Set value to the minimum of value and 2^53 (9007199254740992).
    value = min(value, static_cast<double>(LARGEST_GENERATED_KEY));

    // 4. Set value to the largest integer not greater than value.
    value = floor(value);

    // 5. Let generator be store's key generator.
    // 6. If value is greater than or equal to generator's current number, then set generator's current number to
    //    value + 1.
    if (value >= static_cast<double>(current_number))
        TRY(set_key_generator(object_store_id, static_cast<i64>(value) + 1));

    return {};
}

ErrorOr<Optional<IndexedDBDatabase::FoundRecord>, IndexedDBError> IndexedDBDatabase::find_record(i64 object_store_id, ReadonlyBytes sort_form_of_key)
{
    auto record = m_sqlite_database->try_execute_bound_statement_optional<FoundRecord>(
        m_statements.get_record,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("store"sv, object_store_id));
            return bind("key"sv, sort_form_of_key);
        },
        [](Database::ResultRow& row) -> ErrorOr<FoundRecord, Database::Error> {
            return FoundRecord {
                .id = TRY(row.read_integer<i64>("id"sv)),
                .exact_form_of_key = TRY(row.read_optional_blob("exact_key"sv, MAXIMUM_CELL_SIZE)),
            };
        });
    if (record.is_error())
        return failure(record.release_error());

    return record.release_value();
}

ErrorOr<i64, IndexedDBError> IndexedDBDatabase::add_record(i64 object_store_id, Web::IndexedDB::CheckedKey const& key, ReadonlyBytes value)
{
    auto record_id = m_sqlite_database->try_execute_bound_statement_one<i64>(
        m_statements.insert_record,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("store"sv, object_store_id));
            TRY(bind("key"sv, key.sort_form));
            TRY(bind("exact_key"sv, key.exact_form));
            return bind("value"sv, value);
        },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (record_id.is_error())
        return failure(record_id.release_error());

    return record_id.release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::remove_record(i64 record_id)
{
    auto result = m_sqlite_database->try_execute_bound_statement(m_statements.delete_record, [&](auto& bind) {
        return bind("record"sv, record_id);
    });
    if (result.is_error())
        return failure(result.release_error());
    return {};
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::remove_records(i64 object_store_id, CheckedKeyRange const& range)
{
    auto statement = TRY(range_statement(RangeSource::ObjectStore, RangeStatement::Delete, range, Direction::Ascending, RecordsOfEachKey::All));

    auto result = m_sqlite_database->try_execute_bound_statement(statement, [&](auto& bind) {
        return range.bind_with_source(bind, object_store_id);
    });
    if (result.is_error())
        return failure(result.release_error());
    return {};
}

ErrorOr<i64, IndexedDBError> IndexedDBDatabase::add_index(i64 object_store_id, Utf16String const& name, String const& serialized_key_path, Unique unique, MultiEntry multi_entry)
{
    auto index_id = m_sqlite_database->try_execute_bound_statement_one<i64>(
        m_statements.create_index,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("store"sv, object_store_id));
            TRY(bind("name"sv, name));
            TRY(bind("key_path"sv, serialized_key_path));
            TRY(bind("is_unique"sv, unique == Unique::Yes));
            return bind("is_multi_entry"sv, multi_entry == MultiEntry::Yes);
        },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (index_id.is_error())
        return failure_to_give_a_name(index_id.release_error());

    return index_id.release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::set_name_of_index(i64 index_id, Utf16String const& name)
{
    auto renamed_index = m_sqlite_database->try_execute_bound_statement_optional<i64>(
        m_statements.rename_index,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("name"sv, name));
            return bind("index"sv, index_id);
        },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (renamed_index.is_error())
        return failure_to_give_a_name(renamed_index.release_error());
    if (!renamed_index.value().has_value())
        return invalid_claim();

    return {};
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::remove_index(i64 index_id)
{
    auto removed_index = m_sqlite_database->try_execute_bound_statement_optional<i64>(
        m_statements.delete_index,
        [&](auto& bind) { return bind("index"sv, index_id); },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (removed_index.is_error())
        return failure(removed_index.release_error());
    if (!removed_index.value().has_value())
        return invalid_claim();

    return {};
}

ErrorOr<IndexedDBDatabase::Index, IndexedDBError> IndexedDBDatabase::index(i64 index_id)
{
    auto index = m_sqlite_database->try_execute_bound_statement_optional<Index>(
        m_statements.get_index,
        [&](auto& bind) { return bind("index"sv, index_id); },
        [&](Database::ResultRow& row) -> ErrorOr<Index, Database::Error> {
            return Index {
                .id = index_id,
                .object_store_id = TRY(row.read_integer<i64>("object_store_id"sv)),
                .is_unique = TRY(row.read_bool("is_unique"sv)),
                .is_multi_entry = TRY(row.read_bool("is_multi_entry"sv)),
            };
        });
    if (index.is_error())
        return failure(index.release_error());
    if (!index.value().has_value())
        return invalid_claim();

    return index.release_value().release_value();
}

ErrorOr<bool, IndexedDBError> IndexedDBDatabase::index_has_key(i64 index_id, ReadonlyBytes sort_form_of_key)
{
    auto has_key = m_sqlite_database->try_execute_bound_statement_one<bool>(
        m_statements.get_whether_index_has_key,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("index"sv, index_id));
            return bind("key"sv, sort_form_of_key);
        },
        [](Database::ResultRow& row) { return row.read_bool("has_key"sv); });
    if (has_key.is_error())
        return failure(has_key.release_error());

    return has_key.release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::add_index_record(i64 index_id, Web::IndexedDB::CheckedKey const& index_key, i64 record_id, Web::IndexedDB::CheckedKey const& key_of_record)
{
    auto result = m_sqlite_database->try_execute_bound_statement(m_statements.insert_index_record, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("index"sv, index_id));
        TRY(bind("key"sv, index_key.sort_form));
        TRY(bind("primary_key"sv, key_of_record.sort_form));
        TRY(bind("exact_key"sv, index_key.exact_form));
        TRY(bind("exact_primary_key"sv, key_of_record.exact_form));
        return bind("record"sv, record_id);
    });
    if (result.is_error())
        return failure(result.release_error());
    return {};
}

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
// What step 5 does for one index once it has the index key. A renderer extracts that key from the value. It sends the
// key by itself, or its subkeys where the index has the multiEntry flag and the key is an array key.
ErrorOr<void, IndexedDBError> IndexedDBDatabase::add_index_records(Index const& index, ReadonlySpan<ReadonlyBytes> keys, Optional<u64> injected_generated_key, i64 record_id, Web::IndexedDB::CheckedKey const& key_of_record)
{
    if (!index.is_multi_entry && keys.size() > 1)
        return invalid_claim();

    Vector<Web::IndexedDB::CheckedKey> checked_keys;
    if (checked_keys.try_ensure_capacity(keys.size()).is_error())
        return out_of_memory();
    for (auto key : keys)
        checked_keys.unchecked_append(TRY(check_key(key, injected_generated_key)));

    // 3. If index's multiEntry flag is false, or if index key is not an array key, and if index already contains a
    //    record with key equal to index key, and index's unique flag is true, then this operation failed with a
    //    "ConstraintError" DOMException. Abort this algorithm without taking any further steps.
    // 4. If index's multiEntry flag is true and index key is an array key, and if index already contains a record with
    //    key equal to any of the subkeys of index key, and index's unique flag is true, then this operation failed
    //    with a "ConstraintError" DOMException. Abort this algorithm without taking any further steps.
    if (index.is_unique) {
        for (auto const& key : checked_keys) {
            if (TRY(index_has_key(index.id, key.sort_form)))
                return IndexedDBError { .kind = IndexedDBError::Kind::Constraint };
        }
    }

    // 5. If index's multiEntry flag is false, or if index key is not an array key then store a record in index
    //    containing index key as its key and key as its value. The record is stored in index's list of records such
    //    that the list is sorted primarily on the records keys, and secondarily on the records values, in ascending
    //    order.
    // 6. If index's multiEntry flag is true and index key is an array key, then for each subkey of the subkeys of
    //    index key store a record in index containing subkey as its key and key as its value. The records are stored
    //    in index's list of records such that the list is sorted primarily on the records keys, and secondarily on the
    //    records values, in ascending order.
    for (auto const& key : checked_keys)
        TRY(add_index_record(index.id, key, record_id, key_of_record));

    return {};
}

IndexedDBDatabase::Writer::Writer(IndexedDBDatabase& database, Database::Database::Transaction transaction, Optional<u64> upgrade_version)
    : m_database(database)
    , m_transaction(move(transaction))
    , m_upgrade_version(upgrade_version)
{
}

IndexedDBDatabase::Writer::~Writer()
{
    (void)abort();
}

bool IndexedDBDatabase::Writer::is_open() const
{
    return !m_is_finished;
}

void IndexedDBDatabase::Writer::finish(Optional<u64> committed_version)
{
    m_is_finished = true;
    m_database->writer_did_finish(committed_version);
}

template<typename Request>
auto IndexedDBDatabase::Writer::run_request(Request&& request) -> decltype(declval<Request>()())
{
    if (m_is_finished)
        return IndexedDBError { .kind = IndexedDBError::Kind::TransactionLost };

    auto savepoint = m_transaction.savepoint();
    if (savepoint.is_error())
        return m_database->failure(savepoint.release_error());

    auto result = request();

    // A request whose savepoint can't be released has failed like one that failed by itself.
    if (!result.is_error()) {
        if (auto released = savepoint.value().release(); released.is_error())
            result = m_database->failure(released.release_error());
    }
    if (!result.is_error())
        return result;

    auto error = result.release_error();

    // Nothing is left to undo when SQLite has rolled the whole transaction back. Otherwise what the request changed
    // before it failed has to go, and when it can't be told apart any more, so does the rest.
    if (!m_is_finished && savepoint.value().rollback().is_error()) {
        (void)m_transaction.rollback();
        error.lost_the_writer = true;
        finish({});
    }

    return error;
}

template<typename Request>
auto IndexedDBDatabase::Writer::run_schema_change(Request&& request) -> decltype(declval<Request>()())
{
    if (m_is_finished)
        return IndexedDBError { .kind = IndexedDBError::Kind::TransactionLost };

    // https://w3c.github.io/IndexedDB/#dom-idbdatabase-createobjectstore
    // 2. Let transaction be database's upgrade transaction if it is not null, or throw an "InvalidStateError"
    //    DOMException otherwise.
    // NB: A renderer throws for each change to the object stores outside an upgrade, before it sends anything.
    if (!is_upgrade())
        return invalid_claim();

    auto result = run_request(forward<Request>(request));

    // A renderer has changed its own view of the object stores by the time it sends the change. Of a store that
    // cannot be created the spec says that "it must abort the transaction", and the same goes for the other changes.
    bool was_refused_as_a_claim = result.is_error() && result.error().kind == IndexedDBError::Kind::InvalidClaim;
    if (result.is_error() && !was_refused_as_a_claim && !m_is_finished) {
        (void)abort();
        result.error().lost_the_writer = true;
    }

    return result;
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-createobjectstore
static bool can_be_the_key_path_of_an_object_store(Optional<IndexedDBKeyPath> const& key_path, IndexedDBDatabase::KeyGenerator key_generator)
{
    if (!key_path.has_value())
        return true;

    // 5. If keyPath is not null and is not a valid key path, throw a "SyntaxError" DOMException.
    if (!can_store_key_path(*key_path))
        return false;

    // 8. If autoIncrement is true and keyPath is an empty string or any sequence (empty or otherwise), throw an
    //    "InvalidAccessError" DOMException.
    if (key_generator == IndexedDBDatabase::KeyGenerator::Yes) {
        bool is_an_empty_string = key_path->has<Utf16String>() && key_path->get<Utf16String>().is_empty();
        if (is_an_empty_string || key_path->has<Vector<Utf16String>>())
            return false;
    }

    return true;
}

ErrorOr<i64, IndexedDBError> IndexedDBDatabase::Writer::create_object_store(Utf16String const& name, Optional<IndexedDBKeyPath> const& key_path, KeyGenerator key_generator)
{
    return run_schema_change([&]() -> ErrorOr<i64, IndexedDBError> {
        // NB: A renderer throws for a key path like this before it sends anything.
        if (!can_be_the_key_path_of_an_object_store(key_path, key_generator))
            return invalid_claim();

        Optional<String> serialized_key_path;
        if (key_path.has_value())
            serialized_key_path = TRY(serialize_key_path(*key_path));

        return m_database->add_object_store(name, serialized_key_path, key_generator);
    });
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::rename_object_store(i64 object_store_id, Utf16String const& name)
{
    return run_schema_change([&] { return m_database->set_name_of_object_store(object_store_id, name); });
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::delete_object_store(i64 object_store_id)
{
    return run_schema_change([&] { return m_database->remove_object_store(object_store_id); });
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
static bool can_be_the_key_path_of_an_index(IndexedDBKeyPath const& key_path, IndexedDBDatabase::MultiEntry multi_entry)
{
    // 7. If keyPath is not a valid key path, throw a "SyntaxError" DOMException.
    if (!can_store_key_path(key_path))
        return false;

    // 10. If keyPath is a sequence and multiEntry is true, throw an "InvalidAccessError" DOMException.
    if (key_path.has<Vector<Utf16String>>() && multi_entry == IndexedDBDatabase::MultiEntry::Yes)
        return false;

    return true;
}

ErrorOr<i64, IndexedDBError> IndexedDBDatabase::Writer::create_index(i64 object_store_id, Utf16String const& name, IndexedDBKeyPath const& key_path, Unique unique, MultiEntry multi_entry)
{
    return run_schema_change([&]() -> ErrorOr<i64, IndexedDBError> {
        // NB: A renderer throws for a key path like this, and for an object store that is gone, before it sends
        //     anything.
        if (!can_be_the_key_path_of_an_index(key_path, multi_entry))
            return invalid_claim();
        TRY(m_database->keys_of_object_store(object_store_id));

        auto serialized_key_path = TRY(serialize_key_path(key_path));
        return m_database->add_index(object_store_id, name, serialized_key_path, unique, multi_entry);
    });
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::rename_index(i64 index_id, Utf16String const& name)
{
    return run_schema_change([&] { return m_database->set_name_of_index(index_id, name); });
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::delete_index(i64 index_id)
{
    return run_schema_change([&] { return m_database->remove_index(index_id); });
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
// NB: A record that breaks the unique flag of the index fails as a change to the object stores does, which aborts the
//     upgrade as createIndex() asks for.
ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::fill_index(i64 index_id, ReadonlyBytes key_of_record, ReadonlySpan<ReadonlyBytes> keys)
{
    return run_schema_change([&]() -> ErrorOr<void, IndexedDBError> {
        auto index = TRY(m_database->index(index_id));

        auto checked_key = TRY(check_key(key_of_record));
        auto record = TRY(m_database->find_record(index.object_store_id, checked_key.sort_form));
        if (!record.has_value())
            return invalid_claim();

        // The record's key is read back as the record has it, whichever of its forms was named.
        checked_key.exact_form = move(record->exact_form_of_key);

        // No key is generated for a record that is there already, so its index keys hold no marker.
        return m_database->add_index_records(index, keys, {}, record->id, checked_key);
    });
}

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
ErrorOr<IndexedDBDatabase::Writer::StoredRecord, IndexedDBError> IndexedDBDatabase::Writer::store_record(i64 object_store_id, Optional<ReadonlyBytes> key, IndexedDBValue const& value, NoOverwrite no_overwrite, ReadonlySpan<IndexedDBIndexKeys> index_keys)
{
    return run_request([&]() -> ErrorOr<StoredRecord, IndexedDBError> {
        auto keys = TRY(m_database->keys_of_object_store(object_store_id));

        // NB: The renderer serializes the value before the key is generated. Where the key is then injected, it sends
        //     the serialization in the two parts around the key's place. No other value has such a place. A value
        //     that the key is injected into can be whole too: a Date that holds the key drops it when serialized.
        bool injects_a_generated_key = keys.generator_current_number.has_value() && keys.are_in_line && !key.has_value();
        if (value.has<IndexedDBValueSegments>() && !injects_a_generated_key)
            return invalid_claim();

        Optional<u64> generated_key;
        Web::IndexedDB::CheckedKey key_of_record;

        if (key.has_value())
            key_of_record = TRY(check_key(*key));

        // 1. If store uses a key generator, then:
        if (keys.generator_current_number.has_value()) {
            // 1. If key is undefined, then:
            if (!key.has_value()) {
                // 1. Let key be the result of generating a key for store.
                // 2. If key is failure, then this operation failed with a "ConstraintError" DOMException. Abort this
                //    algorithm without taking any further steps.
                generated_key = TRY(m_database->generate_a_key(object_store_id, *keys.generator_current_number));

                auto encoded_key = Web::IndexedDB::encoded_generated_key(*generated_key);
                VERIFY(encoded_key.has_value());
                if (key_of_record.sort_form.try_append(encoded_key->span()).is_error())
                    return out_of_memory();

                // 3. If store also uses in-line keys, then run inject a key into a value using a key path with
                //    value, key and store's key path.
                // NB: The injection is the joining of the two parts further down.
            }
            // 2. Otherwise, run possibly update the key generator for store with key.
            else {
                TRY(m_database->possibly_update_the_key_generator(object_store_id, *keys.generator_current_number, key_of_record.sort_form));
            }
        } else if (!key.has_value()) {
            return invalid_claim();
        }

        auto existing_record = TRY(m_database->find_record(object_store_id, key_of_record.sort_form));

        // 2. If the no-overwrite flag was given to these steps and is true, and a record already exists in store
        //    with its key equal to key, then this operation failed with a "ConstraintError" DOMException. Abort this
        //    algorithm without taking any further steps.
        if (no_overwrite == NoOverwrite::Yes && existing_record.has_value())
            return IndexedDBError { .kind = IndexedDBError::Kind::Constraint };

        // 3. If a record already exists in store with its key equal to key, then remove the record from store using
        //    delete records from an object store.
        if (existing_record.has_value())
            TRY(m_database->remove_record(existing_record->id));

        // 4. Store a record in store containing key as its key and ! StructuredSerializeForStorage(value) as its
        //    value. The record is stored in the object store's list of records such that the list is sorted
        //    according to the key of the records in ascending order.
        ByteBuffer joined_value;
        auto serialized_value = value.visit(
            [](ReadonlyBytes whole_value) -> ErrorOr<ReadonlyBytes> { return whole_value; },
            [&](IndexedDBValueSegments const& segments) -> ErrorOr<ReadonlyBytes> {
                // NB: Inside the value the key is a number, which the renderer left out.
                auto key_in_the_value = Web::HTML::storage_format_bytes_of_number(static_cast<double>(*generated_key));

                TRY(joined_value.try_ensure_capacity(segments.before_the_key.size() + key_in_the_value.size() + segments.after_the_key.size()));
                TRY(joined_value.try_append(segments.before_the_key));
                TRY(joined_value.try_append(key_in_the_value.span()));
                TRY(joined_value.try_append(segments.after_the_key));
                return joined_value.bytes();
            });
        if (serialized_value.is_error())
            return out_of_memory();

        auto record_id = TRY(m_database->add_record(object_store_id, key_of_record, serialized_value.value()));

        // 5. For each index which references store:
        //    1. Let index key be the result of extracting a key from a value using a key path with value, index's key
        //       path, and index's multiEntry flag.
        //    2. If index key is an exception, or invalid, or failure, take no further actions for index, and continue
        //       these steps for the next index.
        // NB: The renderer did these two before it sent the request, and names the indexes that it has keys for. An
        //     index key can hold the key that was generated and injected, which the renderer sends as a marker.
        auto injected_generated_key = injects_a_generated_key ? generated_key : Optional<u64> {};

        HashTable<i64> named_indexes;
        for (auto const& keys_of_index : index_keys) {
            // NB: A renderer names each index once, and only the indexes of this object store.
            auto index = TRY(m_database->index(keys_of_index.index_id));
            if (index.object_store_id != object_store_id)
                return invalid_claim();

            auto was_named = named_indexes.try_set(index.id);
            if (was_named.is_error())
                return out_of_memory();
            if (was_named.value() != HashSetResult::InsertedNewEntry)
                return invalid_claim();

            TRY(m_database->add_index_records(index, keys_of_index.keys, injected_generated_key, record_id, key_of_record));
        }

        // 6. Return key.
        return StoredRecord { .generated_key = generated_key };
    });
}

// https://w3c.github.io/IndexedDB/#delete-records-from-an-object-store
ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::delete_records(i64 object_store_id, IndexedDBKeyRange const& key_range)
{
    return run_request([&]() -> ErrorOr<void, IndexedDBError> {
        auto range = TRY(m_database->check_key_range(key_range));

        // 1. Remove all records, if any, from store's list of records with key in range.
        // 2. For each index which references store, remove every record from index's list of records whose value is
        //    in range, if any such records exist.
        // NB: The records of an index go with the record that they refer to.
        TRY(m_database->remove_records(object_store_id, range));

        // 3. Return undefined.
        return {};
    });
}

// https://w3c.github.io/IndexedDB/#clear-an-object-store
ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::clear(i64 object_store_id)
{
    // 1. Remove all records from store.
    // 2. In all indexes which reference store, remove all records.
    TRY(delete_records(object_store_id, {}));

    // 3. Return undefined.
    return {};
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::commit(UnixDateTime now)
{
    if (m_is_finished)
        return IndexedDBError { .kind = IndexedDBError::Kind::TransactionLost };

    auto time = TRY(m_database->write_time_of_commit(now));

    if (auto committed = m_transaction.commit(); committed.is_error())
        return m_database->failure(committed.release_error());

    m_database->did_commit_last_open_or_write_time(time);
    finish(m_upgrade_version);
    return {};
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::abort()
{
    if (m_is_finished)
        return {};

    // The SQLite database tries a rollback that failed again before it runs anything else.
    auto rolled_back = m_transaction.rollback();
    finish({});
    if (rolled_back.is_error())
        return m_database->failure(rolled_back.release_error());

    return {};
}

}
