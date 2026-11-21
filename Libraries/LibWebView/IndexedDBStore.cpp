/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <LibDatabase/ResultRow.h>
#include <LibWebView/IndexedDBStore.h>

namespace WebView {

ErrorOr<NonnullOwnPtr<IndexedDBStore>, IndexedDBError> IndexedDBStore::create_transient()
{
    auto create = [&]() -> ErrorOr<NonnullOwnPtr<IndexedDBStore>, Database::Error> {
        auto catalog = TRY(Database::Database::create_memory_backed({ .temp_store = Database::Database::TempStore::Memory }));

        auto migrations = to_array<Database::Migration>({
            {
                .version = 1,
                // https://www.sqlite.org/autoinc.html
                // An id is never given out twice, also not after its database is deleted.
                .sql = R"#(
                    CREATE TABLE Databases (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        storage_key TEXT NOT NULL,
                        name TEXT NOT NULL,
                        UNIQUE (storage_key, name)
                    );
                )#"sv,
            },
        });
        auto outcome = TRY(catalog->migrate("IndexedDBCatalog"sv, migrations));
        VERIFY(outcome == Database::MigrationOutcome::Success);

        Statements statements {};
        statements.find_database = TRY(catalog->prepare_statement("SELECT id FROM Databases WHERE storage_key = :storage_key AND name = :name;"sv));
        statements.insert_database = TRY(catalog->prepare_statement("INSERT INTO Databases (storage_key, name) VALUES (:storage_key, :name);"sv));
        statements.delete_database = TRY(catalog->prepare_statement("DELETE FROM Databases WHERE id = :database;"sv));
        statements.list_databases = TRY(catalog->prepare_statement("SELECT id, name FROM Databases WHERE storage_key = :storage_key ORDER BY id;"sv));

        return TRY(adopt_nonnull_own_or_enomem(new (nothrow) IndexedDBStore(move(catalog), statements)));
    };

    auto store = create();
    if (store.is_error())
        return IndexedDBError::from(store.release_error());
    return store.release_value();
}

IndexedDBStore::IndexedDBStore(NonnullRefPtr<Database::Database> catalog, Statements statements)
    : m_catalog(move(catalog))
    , m_statements(statements)
{
}

IndexedDBStore::~IndexedDBStore() = default;

ErrorOr<Optional<i64>, IndexedDBError> IndexedDBStore::id_of_database(String const& storage_key, Utf16String const& name)
{
    auto database_id = m_catalog->try_execute_bound_statement_optional<i64>(
        m_statements.find_database,
        [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("storage_key"sv, storage_key));
            return bind("name"sv, name);
        },
        [](Database::ResultRow& row) { return row.read_integer<i64>("id"sv); });
    if (database_id.is_error())
        return IndexedDBError::from(database_id.release_error());

    return database_id.release_value();
}

ErrorOr<void, IndexedDBError> IndexedDBStore::remove_from_catalog(i64 database_id)
{
    auto result = m_catalog->try_execute_bound_statement(m_statements.delete_database, [&](auto& bind) {
        return bind("database"sv, database_id);
    });
    if (result.is_error())
        return IndexedDBError::from(result.release_error());
    return {};
}

ErrorOr<NonnullRefPtr<IndexedDBDatabase>, IndexedDBError> IndexedDBStore::create_database(String const& storage_key, Utf16String const& name, UnixDateTime now)
{
    // A row of the catalog stands for a database in m_databases. The row is the last thing that can fail, so that
    // there is never one to take back.
    auto database = TRY(IndexedDBDatabase::create_transient(storage_key, name, now));
    if (m_databases.try_ensure_capacity(m_databases.size() + 1).is_error())
        return IndexedDBError { .kind = IndexedDBError::Kind::OutOfMemory };

    auto result = m_catalog->try_execute_bound_statement(m_statements.insert_database, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("storage_key"sv, storage_key));
        return bind("name"sv, name);
    });
    if (result.is_error())
        return IndexedDBError::from(result.release_error());

    // The id column is the table's rowid.
    m_databases.set(m_catalog->last_insert_rowid(), database);

    return database;
}

// https://w3c.github.io/IndexedDB/#open-a-database-connection
ErrorOr<IndexedDBStore::OpenedDatabase, IndexedDBError> IndexedDBStore::open_database(String const& storage_key, Utf16String const& name, Optional<u64> requested_version, UnixDateTime now)
{
    if (requested_version.has_value() && (*requested_version == 0 || *requested_version > IndexedDBDatabase::LARGEST_VERSION))
        return IndexedDBError { .kind = IndexedDBError::Kind::InvalidClaim };

    // 4. Let db be the database named name in storageKey, or null otherwise.
    RefPtr<IndexedDBDatabase> database;
    if (auto database_id = TRY(id_of_database(storage_key, name)); database_id.has_value())
        database = m_databases.get(*database_id).release_value();

    // 5. If version is undefined, let version be 1 if db is null, or db's version otherwise.
    // AD-HOC: A database at version 0 is one whose first upgrade did not commit. This step would open it at version 0
    //         with no upgrade, where Chromium and Firefox start over at version 1.
    bool has_a_committed_version = database && database->version() != 0;
    auto version = requested_version.value_or(has_a_committed_version ? database->version() : 1);

    // 6. If db is null, let db be a new database with name name, version 0 (zero), and with no object stores. If
    //    this fails for any reason, return an appropriate error (e.g. a QuotaExceededError, or an "UnknownError"
    //    DOMException).
    if (!database)
        database = TRY(create_database(storage_key, name, now));

    // 7. If db's version is greater than version, return a newly created "VersionError" DOMException and abort
    //    these steps.
    if (database->version() > version)
        return IndexedDBError { .kind = IndexedDBError::Kind::Version };

    // 8. Let connection be a new connection to db.
    // 9. Set connection's version to version.
    // NB: The connection is the caller's, which sets its version from what is returned.

    // 10. If db's version is less than version, then:
    // NB: The upgrade is the caller's. Its commit marks the database as written, so only an open without one is
    //     marked as an open.
    bool needs_upgrade = database->version() < version;
    if (!needs_upgrade)
        TRY(database->mark_opened(now));

    // 11. Return connection.
    return OpenedDatabase { .database = database.release_nonnull(), .version = version, .needs_upgrade = needs_upgrade };
}

// https://w3c.github.io/IndexedDB/#delete-a-database
ErrorOr<u64, IndexedDBError> IndexedDBStore::delete_database(String const& storage_key, Utf16String const& name)
{
    // 4. Let db be the database named name in storageKey, if one exists. Otherwise, return 0 (zero).
    auto database_id = TRY(id_of_database(storage_key, name));
    if (!database_id.has_value())
        return 0;

    // 10. Let version be db's version.
    auto version = m_databases.get(*database_id).release_value()->version();

    // 11. Delete db. If this fails for any reason, return an appropriate error (e.g. a QuotaExceededError, or an
    //     "UnknownError" DOMException).
    TRY(remove_from_catalog(*database_id));
    m_databases.remove(*database_id);

    // 12. Return version.
    return version;
}

// https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
ErrorOr<Vector<IndexedDBStore::DatabaseInfo>, IndexedDBError> IndexedDBStore::databases(String const& storage_key)
{
    // 4. Run these steps in parallel:

    // 1. Let databases be the set of databases in storageKey. If this cannot be determined for any reason, then queue
    //    a database task to reject p with an appropriate error (e.g. an "UnknownError" DOMException) and terminate
    //    these steps.
    // 2. Let result be a new list.
    Vector<DatabaseInfo> result;

    // 3. For each db of databases:
    auto listed = m_catalog->try_execute_bound_statement(
        m_statements.list_databases,
        [&](auto& bind) { return bind("storage_key"sv, storage_key); },
        [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            auto database_id = TRY(row.read_integer<i64>("id"sv));
            auto const& database = *m_databases.get(database_id).release_value();

            // 1. If db's version is 0, then continue.
            if (database.version() == 0)
                return {};

            // 2. Let info be a new IDBDatabaseInfo dictionary.
            // 3. Set info's name dictionary member to db's name.
            // 4. Set info's version dictionary member to db's version.
            // 5. Append info to result.
            auto name = TRY(row.read_utf16_text("name"sv, IndexedDBDatabase::MAXIMUM_CELL_SIZE));
            TRY(result.try_append({ .name = move(name), .version = database.version() }));
            return {};
        });
    if (listed.is_error())
        return IndexedDBError::from(listed.release_error());

    return result;
}

}
