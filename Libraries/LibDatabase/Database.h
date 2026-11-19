/*
 * Copyright (c) 2022-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Bitmap.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibDatabase/Error.h>
#include <LibDatabase/Forward.h>

struct sqlite3;
struct sqlite3_stmt;

namespace Database {

struct Migration {
    u32 version { 0 };

    // Executed first. May contain multiple statements.
    ByteString sql {};

    // Optional, runs after `sql`. Use this to bind placeholder values. Use
    // try_execute_statement to allow a failure to roll the migration transaction back.
    Function<ErrorOr<void, Error>(Database&)> backfill {};
};

namespace Detail {

// Reject unsigned long on all platforms to keep binding behavior portable.
template<typename ValueType>
constexpr bool is_unsupported_unsigned_sql_integer_for_payload = IsUnsigned<ValueType> && !IsOneOf<ValueType, bool, u8, u16, u32>;

template<typename ValueType>
constexpr bool is_unsupported_unsigned_sql_integer_for_payload<Optional<ValueType>> = is_unsupported_unsigned_sql_integer_for_payload<ValueType>;

template<typename ValueType>
constexpr bool is_unsupported_unsigned_sql_integer = is_unsupported_unsigned_sql_integer_for_payload<RemoveCVReference<ValueType>>;
static_assert(is_unsupported_unsigned_sql_integer<u64> && is_unsupported_unsigned_sql_integer<unsigned long> && is_unsupported_unsigned_sql_integer<unsigned long long>);
static_assert(!is_unsupported_unsigned_sql_integer<u32> && !is_unsupported_unsigned_sql_integer<i64> && !is_unsupported_unsigned_sql_integer<bool>);
static_assert(is_unsupported_unsigned_sql_integer<Optional<u64>> && !is_unsupported_unsigned_sql_integer<Optional<u32>>);

template<typename ValueType>
consteval void assert_supported_placeholder()
{
    static_assert(!is_unsupported_unsigned_sql_integer<ValueType>,
        "SQLite INTEGER is signed; bind a validated u32/i64 or an explicitly encoded value");
}

template<typename ResultType>
constexpr bool is_result_with_database_error = false;

template<typename ValueType>
constexpr bool is_result_with_database_error<ErrorOr<ValueType, Error>> = true;

template<typename ResultType>
consteval void assert_callback_returns_database_error()
{
    static_assert(is_result_with_database_error<RemoveCVReference<ResultType>>,
        "A callback passed to Database must return ErrorOr<T, Database::Error>");
}

}

class ResultRow;

class DATABASE_API Database : public RefCounted<Database> {
public:
    // https://www.sqlite.org/pragma.html#pragma_journal_mode
    enum class JournalMode {
        Delete,
        Truncate,
        Persist,
        Memory,
        WriteAheadLog,
        Off,
    };

    // https://www.sqlite.org/pragma.html#pragma_synchronous
    enum class Synchronous {
        Off,
        Normal,
        Full,
        Extra,
    };

    // https://www.sqlite.org/pragma.html#pragma_foreign_keys
    enum class ForeignKeys {
        No,
        Yes,
    };

    struct Options {
        JournalMode journal_mode { JournalMode::WriteAheadLog };
        Synchronous synchronous { Synchronous::Normal };
        ForeignKeys foreign_keys { ForeignKeys::No };
    };

    static ErrorOr<NonnullRefPtr<Database>, Error> create_memory_backed(Options);
    static ErrorOr<NonnullRefPtr<Database>, Error> create_memory_backed() { return create_memory_backed(Options {}); }
    static ErrorOr<NonnullRefPtr<Database>, Error> create(ByteString const& directory, StringView name, Options);
    static ErrorOr<NonnullRefPtr<Database>, Error> create(ByteString const& directory, StringView name) { return create(directory, name, Options {}); }
    ~Database();

    using OnResult = Function<ErrorOr<void, Error>(StatementID)>;
    using OnResultRow = Function<ErrorOr<void, Error>(ResultRow&)>;

    enum class StatementExecutionOutcome {
        Completed,
        Interrupted,
    };

    Optional<LexicalPath> const& database_path() const { return m_database_path; }

    Options const& options() const { return m_options; }

    ErrorOr<StatementID, Error> prepare_statement(StringView statement);

    void execute_statement(StatementID statement_id, OnResult on_result)
    {
        VERIFY(bound_parameter_count(statement_id) == 0);
        execute_statement_internal(statement_id, move(on_result));
    }

    template<typename... PlaceholderValues>
    void execute_statement(StatementID statement_id, OnResult on_result, PlaceholderValues&&... placeholder_values)
    {
        (Detail::assert_supported_placeholder<PlaceholderValues>(), ...);
        int index = 1;
        (apply_placeholder(statement_id, index++, forward<PlaceholderValues>(placeholder_values)), ...);

        VERIFY(bound_parameter_count(statement_id) == index - 1);
        execute_statement_internal(statement_id, move(on_result));
    }

    // Unlike execute_statement(), this treats sqlite3_interrupt() as an expected outcome. Other SQL
    // errors remain fatal, matching execute_statement().
    template<typename... PlaceholderValues>
    StatementExecutionOutcome execute_interruptible_statement(StatementID statement_id, OnResult on_result, PlaceholderValues&&... placeholder_values)
    {
        (Detail::assert_supported_placeholder<PlaceholderValues>(), ...);
        int index = 1;
        (apply_placeholder(statement_id, index++, forward<PlaceholderValues>(placeholder_values)), ...);

        VERIFY(bound_parameter_count(statement_id) == index - 1);
        return execute_interruptible_statement_internal(statement_id, move(on_result));
    }

    // SQLite permits this to be called from another thread. The caller must keep this Database alive
    // until interrupt() returns.
    void interrupt();

    template<typename ValueType>
    requires(!Detail::is_unsupported_unsigned_sql_integer<ValueType>)
    ValueType result_column(StatementID, int column);

    // Checked reads reject SQLite coercions; bounded reads also reject oversized cells before copying.
    enum class ColumnReadError {
        WrongType,
        TooLarge,
    };
    ErrorOr<i64, ColumnReadError> result_i64_checked(StatementID, int column);

    // Borrow the current row's storage; ResultRow controls the lifetime.
    ErrorOr<StringView, ColumnReadError> result_text_column_bounded(Badge<ResultRow>, StatementID, int column, size_t max_bytes);
    ErrorOr<ReadonlyBytes, ColumnReadError> result_blob_column_bounded(Badge<ResultRow>, StatementID, int column, size_t max_bytes);

    // Aggregates such as MAX() yield NULL over zero rows even when the schema stores no NULLs.
    bool result_column_is_null(Badge<ResultRow>, StatementID, int column);

    template<typename BindCallback>
    ErrorOr<void, Error> try_execute_bound_statement(StatementID statement_id, BindCallback&& bind_all, OnResultRow on_result = {})
    {
        ScopeGuard clear_on_exit = [&] { clear_bound_parameters(statement_id); };
        auto bind = [&](StringView name, auto const& value) -> ErrorOr<void, Error> {
            Detail::assert_supported_placeholder<decltype(value)>();
            return bind_parameter(statement_id, name, value);
        };
        Detail::assert_callback_returns_database_error<decltype(bind_all(bind))>();
        TRY(bind_all(bind));
        return try_step_bound_statement(statement_id, move(on_result));
    }

    template<typename T, typename BindCallback, typename ReadCallback>
    ErrorOr<T, Error> try_execute_bound_statement_one(StatementID statement_id, BindCallback&& bind_all, ReadCallback&& read_row)
    {
        Optional<T> result;
        TRY(try_execute_bound_statement(statement_id, forward<BindCallback>(bind_all), [&](ResultRow& row) -> ErrorOr<void, Error> {
            if (result.has_value())
                return Error::from_string_literal("Statement returned more than one row");
            Detail::assert_callback_returns_database_error<decltype(read_row(row))>();
            result = TRY(read_row(row));
            return {};
        }));
        if (!result.has_value())
            return Error::from_string_literal("Statement returned no rows");
        return result.release_value();
    }

    // https://www.sqlite.org/lang_returning.html
    // DML RETURNING performs every change and buffers all output during the first step,
    // so a result-row budget cannot bound its work.
    template<typename BindCallback>
    ErrorOr<void, Error> try_execute_bound_statement(StatementID statement_id, size_t maximum_rows, BindCallback&& bind_all, OnResultRow on_result)
    {
        size_t row_count = 0;
        return try_execute_bound_statement(statement_id, forward<BindCallback>(bind_all), [&](ResultRow& row) -> ErrorOr<void, Error> {
            if (row_count == maximum_rows)
                return Error::from_string_literal("Statement returned more rows than the caller allowed");
            ++row_count;
            return on_result(row);
        });
    }

    template<typename T, typename BindCallback, typename ReadCallback>
    ErrorOr<Vector<T>, Error> try_collect_bound_statement(StatementID statement_id, size_t maximum_rows, BindCallback&& bind_all, ReadCallback&& read_row)
    {
        Vector<T> rows;
        TRY(try_execute_bound_statement(statement_id, maximum_rows, forward<BindCallback>(bind_all), [&](ResultRow& row) -> ErrorOr<void, Error> {
            Detail::assert_callback_returns_database_error<decltype(read_row(row))>();
            TRY(rows.try_append(TRY(read_row(row))));
            return {};
        }));
        return rows;
    }

    // The budget counts result rows, not distinct values.
    template<typename T, typename BindCallback, typename ReadCallback>
    ErrorOr<HashTable<T>, Error> try_collect_bound_statement_set(StatementID statement_id, size_t maximum_rows, BindCallback&& bind_all, ReadCallback&& read_row)
    {
        HashTable<T> values;
        TRY(try_execute_bound_statement(statement_id, maximum_rows, forward<BindCallback>(bind_all), [&](ResultRow& row) -> ErrorOr<void, Error> {
            Detail::assert_callback_returns_database_error<decltype(read_row(row))>();
            TRY(values.try_set(TRY(read_row(row))));
            return {};
        }));
        return values;
    }

    ErrorOr<int, Error> result_column_index(StatementID, StringView name);

    ErrorOr<void, Error> execute_raw(ByteString const& sql);

    class Savepoint;
    class Transaction;

    // While a transaction is open, transaction control goes through its handles, never through raw SQL.
    ErrorOr<Transaction, Error> begin_transaction();

    ErrorOr<void, Error> transaction(Function<ErrorOr<void, Error>()> callback);

    // Error-returning sibling of execute_statement, for callers that must handle failures
    // (e.g. migration backfills, which roll the migration transaction back) instead of
    // aborting the process.
    template<typename... PlaceholderValues>
    ErrorOr<void, Error> try_execute_statement(StatementID statement_id, OnResult on_result, PlaceholderValues&&... placeholder_values)
    {
        (Detail::assert_supported_placeholder<PlaceholderValues>(), ...);
        if constexpr (sizeof...(PlaceholderValues) > 0) {
            int index = 1;
            Optional<Error> bind_error;

            auto bind = [&](auto const& value) {
                if (bind_error.has_value())
                    return;
                if (auto result = try_apply_placeholder(statement_id, index++, value); result.is_error())
                    bind_error = result.release_error();
            };
            (bind(forward<PlaceholderValues>(placeholder_values)), ...);

            if (bind_error.has_value())
                return bind_error.release_value();
        }

        VERIFY(bound_parameter_count(statement_id) == sizeof...(PlaceholderValues));
        return try_execute_statement_internal(statement_id, move(on_result));
    }

    // Brings the named store's schema to the latest version by replaying, in order, every
    // migration newer than the version recorded for it in the SchemaVersions table. Shipped
    // migration text is immutable; schema changes append a new version. Baseline (first)
    // migrations use CREATE ... IF NOT EXISTS so pre-versioning databases adopt them as a
    // no-op; later migrations must not.
    ErrorOr<MigrationOutcome, Error> migrate(StringView store_name, ReadonlySpan<Migration> migrations, MigrationMode = MigrationMode::Apply);

    ErrorOr<bool, Error> table_exists(StringView table);

    // The version recorded for the store in SchemaVersions, or empty if it has none yet.
    ErrorOr<Optional<u32>, Error> schema_version(StringView store);

    // https://www.sqlite.org/c3ref/busy_timeout.html
    ErrorOr<void, Error> set_busy_timeout(i32 milliseconds);

private:
    static ErrorOr<NonnullRefPtr<Database>, Error> create(sqlite3*, Options, Optional<LexicalPath> database_path = {});
    Database(sqlite3*, Optional<LexicalPath> database_path);

    ErrorOr<JournalMode, Error> set_journal_mode_pragma(JournalMode);
    ErrorOr<void, Error> set_synchronous_pragma(Synchronous);
    ErrorOr<void, Error> set_foreign_keys_pragma(ForeignKeys);

    void execute_statement_internal(StatementID, OnResult);
    StatementExecutionOutcome execute_interruptible_statement_internal(StatementID, OnResult);
    ErrorOr<void, Error> try_execute_statement_internal(StatementID, OnResult);

    int bound_parameter_count(StatementID);

    template<typename ValueType>
    void apply_placeholder(StatementID statement_id, int index, ValueType const& value);

    template<typename ValueType>
    ErrorOr<void, Error> try_apply_placeholder(StatementID statement_id, int index, ValueType const& value);

    template<typename ValueType>
    ErrorOr<void, Error> bind_parameter(StatementID statement_id, StringView name, ValueType const& value);
    ErrorOr<void, Error> try_step_bound_statement(StatementID, OnResultRow);
    void clear_bound_parameters(StatementID statement_id);

    ErrorOr<void, Error> execute_raw_internal(ByteString const& sql);

    bool has_open_transaction();
    ErrorOr<void, Error> roll_back_waiting_transaction();

    bool is_transaction_open(u64 transaction_id);
    ErrorOr<Savepoint, Error> begin_savepoint(u64 transaction_id);
    ErrorOr<void, Error> commit_transaction(u64 transaction_id);
    ErrorOr<void, Error> rollback_transaction(u64 transaction_id);

    bool is_savepoint_open(u64 savepoint_id);
    ErrorOr<void, Error> release_savepoint(u64 savepoint_id);
    ErrorOr<void, Error> rollback_savepoint(u64 savepoint_id);

    struct OpenTransaction {
        u64 id { 0 };
        Optional<u64> savepoint_id {};
        bool is_waiting_for_rollback { false };
    };

    struct PreparedStatement {
        sqlite3_stmt* statement { nullptr };
        Bitmap bound_parameters;
        // Clear cached, owned column names after SQLite reprepares the statement; duplicate names map to -1.
        HashMap<ByteString, int> result_column_indices {};
        int result_column_generation { -1 };
    };

    ALWAYS_INLINE PreparedStatement& prepared_statement(StatementID statement_id)
    {
        VERIFY(statement_id < m_prepared_statements.size());
        return m_prepared_statements[statement_id];
    }

    Optional<LexicalPath> m_database_path;
    Options m_options;
    sqlite3* m_database { nullptr };
    Vector<PreparedStatement> m_prepared_statements;
    Optional<StatementID> m_table_exists_statement;
    Optional<StatementID> m_schema_version_statement;

    Optional<OpenTransaction> m_open_transaction;
    u64 m_last_transaction_id { 0 };
    u64 m_last_savepoint_id { 0 };
};

class DATABASE_API Database::Transaction {
    AK_MAKE_NONCOPYABLE(Transaction);

public:
    // The source's destructor still runs, so it keeps its reference and gives up only its id.
    Transaction(Transaction&& other)
        : m_database(other.m_database)
        , m_id(exchange(other.m_id, 0))
    {
    }
    Transaction& operator=(Transaction&&) = delete;
    ~Transaction();

    bool is_open() const;

    ErrorOr<Savepoint, Error> savepoint();
    ErrorOr<void, Error> commit();
    ErrorOr<void, Error> rollback();

private:
    friend class Database;

    Transaction(Database& database, u64 id)
        : m_database(database)
        , m_id(id)
    {
    }

    NonnullRefPtr<Database> m_database;
    u64 m_id { 0 };
};

class DATABASE_API Database::Savepoint {
    AK_MAKE_NONCOPYABLE(Savepoint);

public:
    Savepoint(Savepoint&& other)
        : m_database(other.m_database)
        , m_id(exchange(other.m_id, 0))
    {
    }
    Savepoint& operator=(Savepoint&&) = delete;
    ~Savepoint();

    ErrorOr<void, Error> release();
    ErrorOr<void, Error> rollback();

private:
    friend class Database;

    Savepoint(Database& database, u64 id)
        : m_database(database)
        , m_id(id)
    {
    }

    NonnullRefPtr<Database> m_database;
    u64 m_id { 0 };
};

}
