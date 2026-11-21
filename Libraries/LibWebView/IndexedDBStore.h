/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibDatabase/Database.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/IndexedDBDatabase.h>

namespace WebView {

// The IndexedDB databases of one browsing session. A page's connection is not a connection of SQLite's: every
// connection to a storage key and name is served by the one IndexedDBDatabase the session has for them.
class WEBVIEW_API IndexedDBStore {
    AK_MAKE_NONCOPYABLE(IndexedDBStore);
    AK_MAKE_NONMOVABLE(IndexedDBStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    // A store that keeps nothing in files. A database of it lasts until it is deleted or the store goes, whether or
    // not anything is connected to it, since closing an in-memory SQLite database destroys it.
    static ErrorOr<NonnullOwnPtr<IndexedDBStore>, IndexedDBError> create_transient();
    ~IndexedDBStore();

    struct OpenedDatabase {
        NonnullRefPtr<IndexedDBDatabase> database;
        u64 version { 0 };
        bool needs_upgrade { false };
    };
    ErrorOr<OpenedDatabase, IndexedDBError> open_database(String const& storage_key, Utf16String const& name, Optional<u64> version, UnixDateTime now);

    // Returns the version that the database had, or 0 if there was none.
    ErrorOr<u64, IndexedDBError> delete_database(String const& storage_key, Utf16String const& name);

    struct DatabaseInfo {
        Utf16String name;
        u64 version { 0 };
    };
    ErrorOr<Vector<DatabaseInfo>, IndexedDBError> databases(String const& storage_key);

private:
    struct Statements {
        Database::StatementID find_database { 0 };
        Database::StatementID insert_database { 0 };
        Database::StatementID delete_database { 0 };
        Database::StatementID list_databases { 0 };
    };

    IndexedDBStore(NonnullRefPtr<Database::Database> catalog, Statements);

    ErrorOr<Optional<i64>, IndexedDBError> id_of_database(String const& storage_key, Utf16String const& name);
    ErrorOr<NonnullRefPtr<IndexedDBDatabase>, IndexedDBError> create_database(String const& storage_key, Utf16String const& name, UnixDateTime now);
    ErrorOr<void, IndexedDBError> remove_from_catalog(i64 database_id);

    // Which databases the session has, by storage key and name.
    NonnullRefPtr<Database::Database> m_catalog;
    Statements m_statements;

    HashMap<i64, NonnullRefPtr<IndexedDBDatabase>> m_databases;
};

}
