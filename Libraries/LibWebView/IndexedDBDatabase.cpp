/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
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
    template<typename Bind>
    ErrorOr<void, Database::Error> bind_with_object_store(Bind& bind, i64 object_store_id) const
    {
        TRY(bind("store"sv, object_store_id));
        if (lower_bound.has_value())
            TRY(bind("lower_bound"sv, *lower_bound));
        if (upper_bound.has_value())
            TRY(bind("upper_bound"sv, *upper_bound));
        return {};
    }

    Optional<ByteBuffer> lower_bound;
    bool lower_bound_is_open { false };
    Optional<ByteBuffer> upper_bound;
    bool upper_bound_is_open { false };
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

static ErrorOr<Web::IndexedDB::CheckedKey, IndexedDBError> check_key(ReadonlyBytes bytes)
{
    auto checked_key = Web::IndexedDB::check_received_key(bytes);
    if (checked_key.is_error())
        return checked_key.error() == Web::IndexedDB::KeyCheckError::InvalidKey ? invalid_claim() : out_of_memory();
    return checked_key.release_value();
}

// A key path is stored as JSON: a string, or an array of strings.
static ErrorOr<Optional<String>, IndexedDBError> serialize_key_path(Optional<IndexedDBKeyPath> const& key_path)
{
    if (!key_path.has_value())
        return Optional<String> {};

    // No valid key path has an unpaired surrogate.
    auto is_well_formed = [](Utf16String const& string) { return string.utf16_view().validate(); };
    bool is_all_well_formed = key_path->visit(
        [&](Utf16String const& string) { return is_well_formed(string); },
        [&](Vector<Utf16String> const& strings) { return all_of(strings, is_well_formed); });
    if (!is_all_well_formed)
        return invalid_claim();

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
        TRY(key_path->visit(
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
    return Optional<String> { serialized.release_value() };
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
        statements.get_record_id = TRY(database->prepare_statement("SELECT id FROM Records WHERE object_store_id = :store AND key = :key;"sv));
        statements.delete_record = TRY(database->prepare_statement("DELETE FROM Records WHERE id = :record;"sv));
        statements.insert_record = TRY(database->prepare_statement("INSERT INTO Records (object_store_id, key, exact_key, value) VALUES (:store, :key, :exact_key, :value);"sv));

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
IndexedDBError IndexedDBDatabase::failure_to_name_an_object_store(Database::Error&& cause)
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
            };
            TRY(object_stores.try_append(move(object_store)));
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

ErrorOr<Database::StatementID, IndexedDBError> IndexedDBDatabase::range_statement(RangeStatement statement, CheckedKeyRange const& range, Direction direction)
{
    auto kind_of_bound = [](Optional<ByteBuffer> const& bound, bool is_open) {
        if (!bound.has_value())
            return RangeBound::None;
        return is_open ? RangeBound::Open : RangeBound::Closed;
    };
    auto lower_bound = kind_of_bound(range.lower_bound, range.lower_bound_is_open);
    auto upper_bound = kind_of_bound(range.upper_bound, range.upper_bound_is_open);

    bool is_read = statement == RangeStatement::ReadKeys || statement == RangeStatement::ReadKeysAndValues;
    bool is_descending = is_read && direction == Direction::Descending;

    RangeStatementShape shape {
        .statement = statement,
        .lower_bound = lower_bound,
        .upper_bound = upper_bound,
        .is_descending = is_descending,
    };
    if (auto prepared = m_range_statements.get(shape); prepared.has_value())
        return *prepared;

    StringBuilder sql;
    switch (statement) {
    case RangeStatement::Count:
        sql.append("SELECT COUNT(*) AS count FROM Records"sv);
        break;
    case RangeStatement::ReadKeys:
        sql.append("SELECT COALESCE(exact_key, key) AS record_key FROM Records"sv);
        break;
    case RangeStatement::ReadKeysAndValues:
        sql.append("SELECT COALESCE(exact_key, key) AS record_key, value AS record_value FROM Records"sv);
        break;
    case RangeStatement::Delete:
        sql.append("DELETE FROM Records"sv);
        break;
    }
    sql.append(" WHERE object_store_id = :store"sv);

    if (lower_bound == RangeBound::Closed)
        sql.append(" AND key >= :lower_bound"sv);
    else if (lower_bound == RangeBound::Open)
        sql.append(" AND key > :lower_bound"sv);

    if (upper_bound == RangeBound::Closed)
        sql.append(" AND key <= :upper_bound"sv);
    else if (upper_bound == RangeBound::Open)
        sql.append(" AND key < :upper_bound"sv);

    if (is_read)
        sql.append(is_descending ? " ORDER BY key DESC LIMIT :limit"sv : " ORDER BY key ASC LIMIT :limit"sv);
    sql.append(';');

    auto prepared = m_sqlite_database->prepare_statement(sql.string_view());
    if (prepared.is_error())
        return failure(prepared.release_error());
    if (m_range_statements.try_set(shape, prepared.value()).is_error())
        return out_of_memory();

    return prepared.release_value();
}

// https://w3c.github.io/IndexedDB/#count-the-records-in-a-range
ErrorOr<u64, IndexedDBError> IndexedDBDatabase::count(i64 object_store_id, IndexedDBKeyRange const& key_range)
{
    auto range = TRY(check_key_range(key_range));
    auto statement = TRY(range_statement(RangeStatement::Count, range, Direction::Ascending));

    // 1. Let count be the number of records, if any, in source's list of records with key in range.
    auto count = m_sqlite_database->try_execute_bound_statement_one<u64>(
        statement,
        [&](auto& bind) { return range.bind_with_object_store(bind, object_store_id); },
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
    auto statement = TRY(range_statement(kind_of_statement, range, direction));

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
            TRY(range.bind_with_object_store(bind, object_store_id));
            return bind("limit"sv, limit);
        },
        [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            IndexedDBRecord record { .key = TRY(row.read_blob("record_key"sv, MAXIMUM_CELL_SIZE)), .value = {} };
            if (with_values == WithValues::Yes)
                record.value = TRY(row.read_blob("record_value"sv, MAXIMUM_CELL_SIZE));
            TRY(records.try_append(move(record)));
            return {};
        });
    if (result.is_error())
        return failure(result.release_error());

    return records;
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
        return failure_to_name_an_object_store(object_store_id.release_error());

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
        return failure_to_name_an_object_store(renamed_object_store.release_error());
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

ErrorOr<Optional<i64>, IndexedDBError> IndexedDBDatabase::id_of_record(i64 object_store_id, ReadonlyBytes sort_form_of_key)
{
    auto record_id = m_sqlite_database->try_execute_bound_statement_optional<i64>(
        m_statements.get_record_id,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("store"sv, object_store_id));
            return bind("key"sv, sort_form_of_key);
        },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (record_id.is_error())
        return failure(record_id.release_error());

    return record_id.release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBDatabase::add_record(i64 object_store_id, ReadonlyBytes sort_form_of_key, Optional<ByteBuffer> const& exact_form_of_key, ReadonlyBytes value)
{
    auto result = m_sqlite_database->try_execute_bound_statement(m_statements.insert_record, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("store"sv, object_store_id));
        TRY(bind("key"sv, sort_form_of_key));
        TRY(bind("exact_key"sv, exact_form_of_key));
        return bind("value"sv, value);
    });
    if (result.is_error())
        return failure(result.release_error());
    return {};
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
    auto statement = TRY(range_statement(RangeStatement::Delete, range, Direction::Ascending));

    auto result = m_sqlite_database->try_execute_bound_statement(statement, [&](auto& bind) {
        return range.bind_with_object_store(bind, object_store_id);
    });
    if (result.is_error())
        return failure(result.release_error());
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
    // https://w3c.github.io/IndexedDB/#valid-key-path
    // NB: This step rejects an empty sequence. Serializing the key path also rejects unpaired surrogates. The rest of
    //     what makes a key path valid is not checked.
    bool is_an_empty_sequence = key_path->has<Vector<Utf16String>>() && key_path->get<Vector<Utf16String>>().is_empty();
    if (is_an_empty_sequence)
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

        auto serialized_key_path = TRY(serialize_key_path(key_path));
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

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
ErrorOr<IndexedDBDatabase::Writer::StoredRecord, IndexedDBError> IndexedDBDatabase::Writer::store_record(i64 object_store_id, Optional<ReadonlyBytes> key, IndexedDBValue const& value, NoOverwrite no_overwrite)
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
        ByteBuffer sort_form_of_key;
        Optional<ByteBuffer> exact_form_of_key;

        if (key.has_value()) {
            auto checked_key = TRY(check_key(*key));
            sort_form_of_key = move(checked_key.sort_form);
            exact_form_of_key = move(checked_key.exact_form);
        }

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
                if (sort_form_of_key.try_append(encoded_key->span()).is_error())
                    return out_of_memory();

                // 3. If store also uses in-line keys, then run inject a key into a value using a key path with
                //    value, key and store's key path.
                // NB: The injection is the joining of the two parts further down.
            }
            // 2. Otherwise, run possibly update the key generator for store with key.
            else {
                TRY(m_database->possibly_update_the_key_generator(object_store_id, *keys.generator_current_number, sort_form_of_key));
            }
        } else if (!key.has_value()) {
            return invalid_claim();
        }

        auto existing_record = TRY(m_database->id_of_record(object_store_id, sort_form_of_key));

        // 2. If the no-overwrite flag was given to these steps and is true, and a record already exists in store
        //    with its key equal to key, then this operation failed with a "ConstraintError" DOMException. Abort this
        //    algorithm without taking any further steps.
        if (no_overwrite == NoOverwrite::Yes && existing_record.has_value())
            return IndexedDBError { .kind = IndexedDBError::Kind::Constraint };

        // 3. If a record already exists in store with its key equal to key, then remove the record from store using
        //    delete records from an object store.
        if (existing_record.has_value())
            TRY(m_database->remove_record(*existing_record));

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

        TRY(m_database->add_record(object_store_id, sort_form_of_key, exact_form_of_key, serialized_value.value()));

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
        TRY(m_database->remove_records(object_store_id, range));

        // 3. Return undefined.
        return {};
    });
}

// https://w3c.github.io/IndexedDB/#clear-an-object-store
ErrorOr<void, IndexedDBError> IndexedDBDatabase::Writer::clear(i64 object_store_id)
{
    // 1. Remove all records from store.
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
