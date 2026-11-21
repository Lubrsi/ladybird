/*
 * Copyright (c) 2022-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <LibCore/Directory.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/ResultRow.h>

#include <sqlite3.h>

namespace Database {

// https://www.sqlite.org/c3ref/errcode.html
// The call's own result is only the primary code, as the connection leaves extended result codes off.
#define SQL_TRY(expression)                                                       \
    ({                                                                            \
        /* Ignore -Wshadow to allow nesting the macro. */                         \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", auto _sql_result = (expression));        \
        if (_sql_result != SQLITE_OK) [[unlikely]]                                \
            return Error::from_result_code(sqlite3_extended_errcode(m_database)); \
    })

#define SQL_MUST(expression)                                                                                                     \
    ({                                                                                                                           \
        /* Ignore -Wshadow to allow nesting the macro. */                                                                        \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", auto _sql_result = (expression));                                                       \
        if (_sql_result != SQLITE_OK) [[unlikely]] {                                                                             \
            warnln("\033[31;1mDatabase error\033[0m: {}: {}", Error::from_result_code(_sql_result), sqlite3_errmsg(m_database)); \
            VERIFY_NOT_REACHED();                                                                                                \
        }                                                                                                                        \
    })

#define ENUMERATE_SQL_TYPES        \
    __ENUMERATE_TYPE(String)       \
    __ENUMERATE_TYPE(Utf16String)  \
    __ENUMERATE_TYPE(ByteString)   \
    __ENUMERATE_TYPE(ByteBuffer)   \
    __ENUMERATE_TYPE(UnixDateTime) \
    __ENUMERATE_TYPE(i8)           \
    __ENUMERATE_TYPE(i16)          \
    __ENUMERATE_TYPE(i32)          \
    __ENUMERATE_TYPE(long)         \
    __ENUMERATE_TYPE(long long)    \
    __ENUMERATE_TYPE(u8)           \
    __ENUMERATE_TYPE(u16)          \
    __ENUMERATE_TYPE(u32)          \
    __ENUMERATE_TYPE(double)       \
    __ENUMERATE_TYPE(bool)

#define ENUMERATE_SQL_BIND_ONLY_TYPES \
    __ENUMERATE_TYPE(Bytes)           \
    __ENUMERATE_TYPE(ReadonlyBytes)

// https://www.sqlite.org/c3ref/open.html
static ErrorOr<sqlite3*, Error> open_connection(char const* filename)
{
    sqlite3* sql_database { nullptr };

    if (auto result = sqlite3_open(filename, &sql_database); result != SQLITE_OK) {
        // SQLite usually returns a connection even when it fails to open the database.
        if (sql_database) {
            result = sqlite3_extended_errcode(sql_database);
            sqlite3_close(sql_database);
        }

        return Error::from_result_code(result);
    }

    return sql_database;
}

// https://www.sqlite.org/compile.html#omit_memorydb
// SQLite built without in-memory databases opens a file named ":memory:" instead. Only its compile-time options
// tell, so a build that reports none of them counts as one without.
static bool sqlite_has_in_memory_databases()
{
    bool reports_compile_time_options = sqlite3_compileoption_get(0) != nullptr;
    return reports_compile_time_options && !sqlite3_compileoption_used("OMIT_MEMORYDB");
}

// https://www.sqlite.org/pragma.html#pragma_temp_store
// SQLite accepts the pragma and still writes temporary files when it was built with SQLITE_TEMP_STORE=0 or without
// in-memory databases. A setting it does not report counts as unsupported.
static ErrorOr<void, Error> ensure_temp_store_is_supported(Database::TempStore temp_store)
{
    if (temp_store == Database::TempStore::Default)
        return {};

    bool temporary_files_can_be_in_memory = sqlite3_compileoption_used("TEMP_STORE=1")
        || sqlite3_compileoption_used("TEMP_STORE=2")
        || sqlite3_compileoption_used("TEMP_STORE=3");

    if (!temporary_files_can_be_in_memory || !sqlite_has_in_memory_databases())
        return Error::from_string_literal("This build of SQLite cannot keep temporary files in memory");

    return {};
}

ErrorOr<NonnullRefPtr<Database>, Error> Database::create_memory_backed(Options options)
{
    if (!sqlite_has_in_memory_databases())
        return Error::from_string_literal("This build of SQLite has no in-memory databases");

    TRY(ensure_temp_store_is_supported(options.temp_store));

    auto* sql_database = TRY(open_connection(":memory:"));
    return create(sql_database, options);
}

ErrorOr<NonnullRefPtr<Database>, Error> Database::create(ByteString const& directory, StringView name, Options options)
{
    TRY(ensure_temp_store_is_supported(options.temp_store));

    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    LexicalPath database_path { ByteString::formatted("{}/{}.db", directory, name) };
    auto* sql_database = TRY(open_connection(database_path.string().characters()));
    return create(sql_database, options, database_path);
}

ErrorOr<NonnullRefPtr<Database>, Error> Database::create(sqlite3* sql_database, Options options, Optional<LexicalPath> database_path)
{
    auto database = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) Database(sql_database, move(database_path))));

    options.journal_mode = TRY(database->set_journal_mode_pragma(options.journal_mode));
    TRY(database->set_synchronous_pragma(options.synchronous));
    TRY(database->set_foreign_keys_pragma(options.foreign_keys));
    TRY(database->set_temp_store_pragma(options.temp_store));
    database->m_options = options;

    return database;
}

Database::Database(sqlite3* database, Optional<LexicalPath> database_path)
    : m_database_path(move(database_path))
    , m_database(database)
{
    VERIFY(m_database);
}

Database::~Database()
{
    for (auto& prepared : m_prepared_statements)
        sqlite3_finalize(prepared.statement);

    sqlite3_close(m_database);
}

ErrorOr<StatementID, Error> Database::prepare_statement(StringView statement)
{
    // A statement is prepared against the schema as the open transaction has it.
    TRY(roll_back_waiting_transaction());

    sqlite3_stmt* prepared_statement { nullptr };
    SQL_TRY(sqlite3_prepare_v2(m_database, statement.characters_without_null_termination(), static_cast<int>(statement.length()), &prepared_statement, nullptr));

    auto parameter_count = static_cast<size_t>(sqlite3_bind_parameter_count(prepared_statement));
    Bitmap bound_parameters;
    if (parameter_count > 0) {
        auto result = Bitmap::create(parameter_count, false);
        if (result.is_error()) {
            sqlite3_finalize(prepared_statement);
            return result.release_error();
        }
        bound_parameters = result.release_value();
    }

    auto statement_id = m_prepared_statements.size();
    m_prepared_statements.append({
        .statement = prepared_statement,
        .bound_parameters = move(bound_parameters),
    });

    return statement_id;
}

void Database::execute_statement_internal(StatementID statement_id, OnResult on_result)
{
    if (auto result = try_execute_statement_internal(statement_id, move(on_result)); result.is_error()) [[unlikely]] {
        warnln("\033[31;1mDatabase error\033[0m: {}: {}", result.error(), sqlite3_errmsg(m_database));
        VERIFY_NOT_REACHED();
    }
}

Database::StatementExecutionOutcome Database::execute_interruptible_statement_internal(StatementID statement_id, OnResult on_result)
{
    if (auto result = roll_back_waiting_transaction(); result.is_error()) [[unlikely]] {
        warnln("\033[31;1mDatabase error\033[0m: {}: {}", result.error(), sqlite3_errmsg(m_database));
        VERIFY_NOT_REACHED();
    }

    auto* statement = prepared_statement(statement_id).statement;

    while (true) {
        auto result = sqlite3_step(statement);

        switch (result) {
        case SQLITE_DONE:
            SQL_MUST(sqlite3_reset(statement));
            return StatementExecutionOutcome::Completed;

        case SQLITE_ROW:
            if (on_result)
                MUST(on_result(statement_id));
            continue;

        case SQLITE_INTERRUPT:
            // sqlite3_reset() reports the interrupted statement's error code, so intentionally ignore
            // its result here. Resetting still makes the prepared statement reusable.
            sqlite3_reset(statement);
            return StatementExecutionOutcome::Interrupted;

        default:
            sqlite3_reset(statement);
            warnln("\033[31;1mDatabase error\033[0m: {}: {}", Error::from_result_code(result), sqlite3_errmsg(m_database));
            VERIFY_NOT_REACHED();
        }
    }
}

void Database::interrupt()
{
    sqlite3_interrupt(m_database);
}

ErrorOr<void, Error> Database::try_execute_statement_internal(StatementID statement_id, OnResult on_result)
{
    TRY(roll_back_waiting_transaction());

    auto* statement = prepared_statement(statement_id).statement;

    Optional<Error> row_error;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (!on_result)
            continue;
        if (auto result = on_result(statement_id); result.is_error()) {
            row_error = result.release_error();
            break;
        }
    }

    // Reset the statement and re-report any sqlite3_step() failure.
    SQL_TRY(sqlite3_reset(statement));
    if (row_error.has_value())
        return row_error.release_value();
    return {};
}

int Database::bound_parameter_count(StatementID statement_id)
{
    auto* statement = prepared_statement(statement_id).statement;
    return sqlite3_bind_parameter_count(statement);
}

template<typename ValueType>
void Database::apply_placeholder(StatementID statement_id, int index, ValueType const& value)
{
    if (auto result = try_apply_placeholder(statement_id, index, value); result.is_error()) [[unlikely]] {
        warnln("\033[31;1mDatabase error\033[0m: {}: {}", result.error(), sqlite3_errmsg(m_database));
        VERIFY_NOT_REACHED();
    }
}

#define __ENUMERATE_TYPE(type)                                                             \
    template DATABASE_API void Database::apply_placeholder(StatementID, int, type const&); \
    template DATABASE_API void Database::apply_placeholder(StatementID, int, Optional<type> const&);
ENUMERATE_SQL_TYPES
ENUMERATE_SQL_BIND_ONLY_TYPES
#undef __ENUMERATE_TYPE

static int bind_blob_bytes(sqlite3_stmt* statement, int index, ReadonlyBytes bytes)
{
    // SQLite binds a null pointer as NULL, so empty blobs need a non-null sentinel.
    static constexpr u8 empty_blob {};
    return sqlite3_bind_blob64(statement, index, bytes.is_empty() ? &empty_blob : bytes.data(), bytes.size(), SQLITE_TRANSIENT);
}

template<typename ValueType>
ErrorOr<void, Error> Database::try_apply_placeholder(StatementID statement_id, int index, ValueType const& value)
{
    auto* statement = prepared_statement(statement_id).statement;

    if constexpr (IsSpecializationOf<ValueType, Optional>) {
        if (!value.has_value()) {
            SQL_TRY(sqlite3_bind_null(statement, index));
            return {};
        }
        TRY(try_apply_placeholder(statement_id, index, *value));
    } else if constexpr (IsSame<ValueType, String>) {
        auto bytes = value.bytes();
        SQL_TRY(sqlite3_bind_text64(statement, index, reinterpret_cast<char const*>(bytes.data()), bytes.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    } else if constexpr (IsSame<ValueType, Utf16String>) {
        // Stored as WTF-8 TEXT; SQLite's own UTF-16 interface would replace lonely surrogates with U+FFFD.
        TRY(try_apply_placeholder(statement_id, index, value.to_utf8()));
    } else if constexpr (IsSame<ValueType, ByteString>) {
        SQL_TRY(bind_blob_bytes(statement, index, value.bytes()));
    } else if constexpr (IsOneOf<ValueType, ByteBuffer, Bytes, ReadonlyBytes>) {
        SQL_TRY(bind_blob_bytes(statement, index, value));
    } else if constexpr (IsSame<ValueType, UnixDateTime>) {
        TRY(try_apply_placeholder(statement_id, index, value.milliseconds_since_epoch()));
    } else if constexpr (IsSame<ValueType, double>) {
        SQL_TRY(sqlite3_bind_double(statement, index, value));
    } else if constexpr (IsIntegral<ValueType>) {
        static_assert(!Detail::is_unsupported_unsigned_sql_integer<ValueType>);
        static_assert(sizeof(ValueType) <= sizeof(sqlite3_int64));
        SQL_TRY(sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)));
    } else {
        static_assert(DependentFalse<ValueType>);
    }

    return {};
}

#define __ENUMERATE_TYPE(type)                                                                                 \
    template DATABASE_API ErrorOr<void, Error> Database::try_apply_placeholder(StatementID, int, type const&); \
    template DATABASE_API ErrorOr<void, Error> Database::try_apply_placeholder(StatementID, int, Optional<type> const&);
ENUMERATE_SQL_TYPES
ENUMERATE_SQL_BIND_ONLY_TYPES
#undef __ENUMERATE_TYPE

template<typename ValueType>
requires(!Detail::is_unsupported_unsigned_sql_integer<ValueType>)
ValueType Database::result_column(StatementID statement_id, int column)
{
    auto* statement = prepared_statement(statement_id).statement;

    if constexpr (IsSame<ValueType, String>) {
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
        auto length = sqlite3_column_bytes(statement, column);
        return MUST(String::from_utf8(StringView { text, static_cast<size_t>(length) }));
    } else if constexpr (IsSame<ValueType, Utf16String>) {
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
        auto length = sqlite3_column_bytes(statement, column);
        return Utf16String::from_utf8(StringView { text, static_cast<size_t>(length) });
    } else if constexpr (IsSame<ValueType, ByteString>) {
        auto const* blob = sqlite3_column_blob(statement, column);
        auto length = sqlite3_column_bytes(statement, column);
        return ByteString { reinterpret_cast<char const*>(blob), static_cast<size_t>(length) };
    } else if constexpr (IsSame<ValueType, ByteBuffer>) {
        auto const* blob = sqlite3_column_blob(statement, column);
        auto length = sqlite3_column_bytes(statement, column);
        return MUST(ByteBuffer::copy(blob, static_cast<size_t>(length)));
    } else if constexpr (IsSame<ValueType, UnixDateTime>) {
        auto milliseconds = result_column<sqlite3_int64>(statement_id, column);
        return UnixDateTime::from_milliseconds_since_epoch(milliseconds);
    } else if constexpr (IsSame<ValueType, double>) {
        return sqlite3_column_double(statement, column);
    } else if constexpr (IsIntegral<ValueType>) {
        if constexpr (sizeof(ValueType) <= sizeof(int))
            return static_cast<ValueType>(sqlite3_column_int(statement, column));
        else
            return static_cast<ValueType>(sqlite3_column_int64(statement, column));
    } else {
        static_assert(DependentFalse<ValueType>);
    }
}

#define __ENUMERATE_TYPE(type) \
    template DATABASE_API type Database::result_column(StatementID, int);
ENUMERATE_SQL_TYPES
#undef __ENUMERATE_TYPE

ErrorOr<i64, Database::ColumnReadError> Database::result_i64_checked(StatementID statement_id, int column)
{
    auto* statement = prepared_statement(statement_id).statement;
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER)
        return ColumnReadError::WrongType;
    return static_cast<i64>(sqlite3_column_int64(statement, column));
}

ErrorOr<StringView, Database::ColumnReadError> Database::result_text_column_bounded(Badge<ResultRow>, StatementID statement_id, int column, size_t max_bytes)
{
    auto* statement = prepared_statement(statement_id).statement;
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
        return ColumnReadError::WrongType;
    // sqlite3_column_bytes() must follow the accessor; taken first it can report a converted length.
    auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
    auto length = static_cast<size_t>(sqlite3_column_bytes(statement, column));
    if (length > max_bytes)
        return ColumnReadError::TooLarge;
    return StringView { text, length };
}

ErrorOr<ReadonlyBytes, Database::ColumnReadError> Database::result_blob_column_bounded(Badge<ResultRow>, StatementID statement_id, int column, size_t max_bytes)
{
    auto* statement = prepared_statement(statement_id).statement;
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB)
        return ColumnReadError::WrongType;
    auto const* blob = sqlite3_column_blob(statement, column);
    auto length = static_cast<size_t>(sqlite3_column_bytes(statement, column));
    if (length > max_bytes)
        return ColumnReadError::TooLarge;
    return ReadonlyBytes { blob, length };
}

template<typename ValueType>
ErrorOr<void, Error> Database::bind_parameter(StatementID statement_id, StringView name, ValueType const& value)
{
    auto& prepared = prepared_statement(statement_id);

    // The placeholder is resolved as a C string, so an embedded NUL would target a shorter, different one.
    if (name.contains('\0'))
        return Error::from_string_literal("Bound parameter name contains an embedded NUL");

    auto placeholder = ByteString::formatted(":{}", name);
    auto index = sqlite3_bind_parameter_index(prepared.statement, placeholder.characters());
    if (index == 0)
        return Error::from_string_literal("Unknown bound parameter name");

    TRY(try_apply_placeholder(statement_id, index, value));

    prepared.bound_parameters.set(index - 1, true);
    return {};
}

#define __ENUMERATE_TYPE(type)                                                                                 \
    template DATABASE_API ErrorOr<void, Error> Database::bind_parameter(StatementID, StringView, type const&); \
    template DATABASE_API ErrorOr<void, Error> Database::bind_parameter(StatementID, StringView, Optional<type> const&);
ENUMERATE_SQL_TYPES
ENUMERATE_SQL_BIND_ONLY_TYPES
#undef __ENUMERATE_TYPE

ErrorOr<void, Error> Database::try_step_bound_statement(StatementID statement_id, OnResultRow on_result)
{
    auto& prepared = prepared_statement(statement_id);

    // Reject unbound parameters, which SQLite otherwise treats as NULL.
    if (auto unbound = prepared.bound_parameters.find_first_unset(); unbound.has_value()) {
        auto parameter_index = static_cast<int>(*unbound) + 1;
        auto const* name = sqlite3_bind_parameter_name(prepared.statement, parameter_index);
        dbgln("Database: refusing to step statement with unbound parameter {} ({})", parameter_index, name ? name : "?");
        return Error::from_string_literal("Statement executed with an unbound parameter");
    }

    if (!on_result)
        return try_execute_statement_internal(statement_id, {});

    ResultRow row { {}, *this, statement_id };
    return try_execute_statement_internal(statement_id, [&](StatementID) { return on_result(row); });
}

ErrorOr<int, Error> Database::result_column_index(StatementID statement_id, StringView name)
{
    auto& prepared = prepared_statement(statement_id);

    auto generation = sqlite3_stmt_status(prepared.statement, SQLITE_STMTSTATUS_REPREPARE, 0);
    if (generation != prepared.result_column_generation) {
        prepared.result_column_indices.clear();
        auto column_count = sqlite3_column_count(prepared.statement);
        for (int column = 0; column < column_count; ++column) {
            auto const* column_name = sqlite3_column_name(prepared.statement, column);
            if (column_name == nullptr)
                continue;
            if (prepared.result_column_indices.set(ByteString { column_name }, column) == HashSetResult::ReplacedExistingEntry)
                prepared.result_column_indices.set(ByteString { column_name }, -1);
        }
        prepared.result_column_generation = generation;
    }

    auto match = prepared.result_column_indices.get(name);
    if (!match.has_value())
        return Error::from_string_literal("Unknown result column name");
    if (*match == -1)
        return Error::from_string_literal("Ambiguous result column name");
    return *match;
}

bool Database::result_column_is_null(Badge<ResultRow>, StatementID statement_id, int column)
{
    return sqlite3_column_type(prepared_statement(statement_id).statement, column) == SQLITE_NULL;
}

i64 Database::last_insert_rowid() const
{
    return sqlite3_last_insert_rowid(m_database);
}

void Database::clear_bound_parameters(StatementID statement_id)
{
    auto& prepared = prepared_statement(statement_id);
    SQL_MUST(sqlite3_clear_bindings(prepared.statement));
    if (prepared.bound_parameters.size() > 0)
        prepared.bound_parameters.fill(false);
}

ErrorOr<void, Error> Database::execute_raw(ByteString const& sql)
{
    TRY(roll_back_waiting_transaction());
    return execute_raw_internal(sql);
}

ErrorOr<void, Error> Database::execute_raw_internal(ByteString const& sql)
{
    SQL_TRY(sqlite3_exec(m_database, sql.characters(), nullptr, nullptr, nullptr));
    return {};
}

ErrorOr<Database::Transaction, Error> Database::begin_transaction()
{
    TRY(roll_back_waiting_transaction());

    if (has_open_transaction())
        return Error::from_string_literal("A transaction is already open");

    TRY(execute_raw_internal("BEGIN IMMEDIATE;"));

    m_open_transaction = OpenTransaction { .id = ++m_last_transaction_id };
    return Transaction { *this, m_open_transaction->id };
}

bool Database::has_open_transaction()
{
    // https://www.sqlite.org/c3ref/get_autocommit.html
    // After some errors SQLite rolls the whole transaction back, which shows only as autocommit mode being back on.
    if (m_open_transaction.has_value() && sqlite3_get_autocommit(m_database) != 0)
        m_open_transaction.clear();

    return m_open_transaction.has_value();
}

ErrorOr<void, Error> Database::roll_back_waiting_transaction()
{
    if (!has_open_transaction() || !m_open_transaction->is_waiting_for_rollback)
        return {};

    return rollback_transaction(m_open_transaction->id);
}

bool Database::is_transaction_open(u64 transaction_id)
{
    return has_open_transaction() && m_open_transaction->id == transaction_id;
}

ErrorOr<Database::Savepoint, Error> Database::begin_savepoint(u64 transaction_id)
{
    if (!is_transaction_open(transaction_id))
        return Error::from_string_literal("The transaction is no longer open");
    if (m_open_transaction->is_waiting_for_rollback)
        return Error::from_string_literal("The transaction is waiting to be rolled back");
    if (m_open_transaction->savepoint_id.has_value())
        return Error::from_string_literal("The transaction already has an open savepoint");

    // A name of its own keeps the SQL sent for one savepoint from matching a later one.
    auto savepoint_id = ++m_last_savepoint_id;
    TRY(execute_raw_internal(ByteString::formatted("SAVEPOINT savepoint_{};", savepoint_id)));

    m_open_transaction->savepoint_id = savepoint_id;
    return Savepoint { *this, savepoint_id };
}

ErrorOr<void, Error> Database::commit_transaction(u64 transaction_id)
{
    if (!is_transaction_open(transaction_id))
        return Error::from_string_literal("The transaction is no longer open");
    if (m_open_transaction->is_waiting_for_rollback)
        return Error::from_string_literal("The transaction is waiting to be rolled back");

    // https://www.sqlite.org/lang_savepoint.html
    // COMMIT would release the savepoint and keep its changes.
    if (m_open_transaction->savepoint_id.has_value())
        return Error::from_string_literal("The transaction has an open savepoint");

    TRY(execute_raw_internal("COMMIT;"));

    m_open_transaction.clear();
    return {};
}

ErrorOr<void, Error> Database::rollback_transaction(u64 transaction_id)
{
    if (!is_transaction_open(transaction_id))
        return {};

    auto rolled_back = execute_raw_internal("ROLLBACK;");
    if (rolled_back.is_error()) {
        // Whoever asked for the rollback takes the transaction to be over, and may be gone before it can ask again.
        // Nothing else runs inside the transaction from here on: the next SQL on the connection rolls it back first.
        if (has_open_transaction())
            m_open_transaction->is_waiting_for_rollback = true;
        return rolled_back;
    }

    m_open_transaction.clear();
    return {};
}

bool Database::is_savepoint_open(u64 savepoint_id)
{
    return has_open_transaction() && m_open_transaction->savepoint_id == savepoint_id;
}

ErrorOr<void, Error> Database::release_savepoint(u64 savepoint_id)
{
    if (!is_savepoint_open(savepoint_id))
        return Error::from_string_literal("The savepoint is no longer open");
    if (m_open_transaction->is_waiting_for_rollback)
        return Error::from_string_literal("The transaction is waiting to be rolled back");

    TRY(execute_raw_internal(ByteString::formatted("RELEASE savepoint_{};", savepoint_id)));

    m_open_transaction->savepoint_id.clear();
    return {};
}

ErrorOr<void, Error> Database::rollback_savepoint(u64 savepoint_id)
{
    if (!is_savepoint_open(savepoint_id))
        return {};

    // The savepoint goes with its transaction.
    if (m_open_transaction->is_waiting_for_rollback)
        return rollback_transaction(m_open_transaction->id);

    // https://www.sqlite.org/lang_savepoint.html
    // ROLLBACK TO leaves the savepoint in place.
    TRY(execute_raw_internal(ByteString::formatted("ROLLBACK TO savepoint_{0}; RELEASE savepoint_{0};", savepoint_id)));

    m_open_transaction->savepoint_id.clear();
    return {};
}

Database::Transaction::~Transaction()
{
    if (auto result = rollback(); result.is_error())
        warnln("\033[31;1mDatabase error\033[0m: Unable to roll back transaction: {}", result.error());
}

bool Database::Transaction::is_open() const
{
    return m_database->is_transaction_open(m_id);
}

ErrorOr<Database::Savepoint, Error> Database::Transaction::savepoint()
{
    return m_database->begin_savepoint(m_id);
}

ErrorOr<void, Error> Database::Transaction::commit()
{
    return m_database->commit_transaction(m_id);
}

ErrorOr<void, Error> Database::Transaction::rollback()
{
    return m_database->rollback_transaction(m_id);
}

Database::Savepoint::~Savepoint()
{
    if (auto result = rollback(); result.is_error())
        warnln("\033[31;1mDatabase error\033[0m: Unable to roll back savepoint: {}", result.error());
}

ErrorOr<void, Error> Database::Savepoint::release()
{
    return m_database->release_savepoint(m_id);
}

ErrorOr<void, Error> Database::Savepoint::rollback()
{
    return m_database->rollback_savepoint(m_id);
}

ErrorOr<void, Error> Database::transaction(Function<ErrorOr<void, Error>()> callback)
{
    auto transaction = TRY(begin_transaction());
    TRY(callback());
    TRY(transaction.commit());
    return {};
}

ErrorOr<MigrationOutcome, Error> Database::migrate(StringView store_name, ReadonlySpan<Migration> migrations, MigrationMode mode)
{
    VERIFY(!migrations.is_empty());
    VERIFY(migrations.first().version >= 1);
    for (size_t i = 1; i < migrations.size(); ++i)
        VERIFY(migrations[i].version > migrations[i - 1].version);

    auto store = TRY(String::from_utf8(store_name));
    auto latest_version = migrations.last().version;

    // Fast path: only reads, so a database from a newer version of Ladybird is left untouched.
    if (auto recorded = TRY(schema_version(store_name)); recorded.has_value()) {
        if (*recorded > latest_version)
            return MigrationOutcome::DatabaseTooNew;
        if (*recorded == latest_version)
            return MigrationOutcome::Success;
    }

    auto transaction = TRY(begin_transaction());

    TRY(execute_raw("CREATE TABLE IF NOT EXISTS SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));

    // Re-read under the write lock; a concurrent process may have migrated since the fast path.
    auto recorded = TRY(schema_version(store_name));
    if (recorded.has_value()) {
        if (*recorded > latest_version)
            return MigrationOutcome::DatabaseTooNew;

        if (*recorded == latest_version) {
            if (mode == MigrationMode::CheckOnly)
                return MigrationOutcome::Success;

            TRY(transaction.commit());
            return MigrationOutcome::Success;
        }
    }

    auto current_version = recorded.value_or(0);

    if (mode == MigrationMode::CheckOnly)
        return MigrationOutcome::Success;

    for (auto const& migration : migrations) {
        if (migration.version <= current_version)
            continue;

        if (!migration.sql.is_empty())
            TRY(execute_raw(migration.sql));

        if (migration.backfill)
            TRY(migration.backfill(*this));
    }

    auto update_version = TRY(prepare_statement("INSERT INTO SchemaVersions (store, version) VALUES (?, ?) ON CONFLICT(store) DO UPDATE SET version = excluded.version;"sv));
    TRY(try_execute_statement(update_version, {}, store, latest_version));

    TRY(transaction.commit());
    return MigrationOutcome::Success;
}

ErrorOr<bool, Error> Database::table_exists(StringView table)
{
    if (!m_table_exists_statement.has_value())
        m_table_exists_statement = TRY(prepare_statement("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;"sv));

    bool exists = false;
    TRY(try_execute_statement(
        *m_table_exists_statement,
        [&](auto) -> ErrorOr<void, Error> {
            exists = true;
            return {};
        },
        TRY(String::from_utf8(table))));
    return exists;
}

ErrorOr<Optional<u32>, Error> Database::schema_version(StringView store)
{
    if (!TRY(table_exists("SchemaVersions"sv)))
        return OptionalNone {};

    if (!m_schema_version_statement.has_value())
        m_schema_version_statement = TRY(prepare_statement("SELECT version FROM SchemaVersions WHERE store = ?;"sv));

    Optional<u32> version;
    TRY(try_execute_statement(
        *m_schema_version_statement,
        [&](auto statement_id) -> ErrorOr<void, Error> {
            version = result_column<u32>(statement_id, 0);
            return {};
        },
        TRY(String::from_utf8(store))));
    return version;
}

ErrorOr<Database::JournalMode, Error> Database::set_journal_mode_pragma(JournalMode journal_mode)
{
    auto journal_mode_string = [&]() {
        switch (journal_mode) {
        case JournalMode::Delete:
            return "DELETE"sv;
        case JournalMode::Truncate:
            return "TRUNCATE"sv;
        case JournalMode::Persist:
            return "PERSIST"sv;
        case JournalMode::Memory:
            return "MEMORY"sv;
        case JournalMode::WriteAheadLog:
            return "WAL"sv;
        case JournalMode::Off:
            return "OFF"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    auto pragma = ByteString::formatted("PRAGMA journal_mode={};", journal_mode_string);
    auto statement_id = TRY(prepare_statement(pragma));
    Optional<JournalMode> applied_journal_mode;
    TRY(try_execute_statement(statement_id, [&](auto result_statement_id) -> ErrorOr<void, Error> {
        auto applied_mode = result_column<String>(result_statement_id, 0);
        if (applied_mode.equals_ignoring_ascii_case("delete"sv))
            applied_journal_mode = JournalMode::Delete;
        else if (applied_mode.equals_ignoring_ascii_case("truncate"sv))
            applied_journal_mode = JournalMode::Truncate;
        else if (applied_mode.equals_ignoring_ascii_case("persist"sv))
            applied_journal_mode = JournalMode::Persist;
        else if (applied_mode.equals_ignoring_ascii_case("memory"sv))
            applied_journal_mode = JournalMode::Memory;
        else if (applied_mode.equals_ignoring_ascii_case("wal"sv))
            applied_journal_mode = JournalMode::WriteAheadLog;
        else if (applied_mode.equals_ignoring_ascii_case("off"sv))
            applied_journal_mode = JournalMode::Off;
        else
            return Error::from_string_literal("PRAGMA journal_mode returned an unknown mode");
        return {};
    }));

    if (!applied_journal_mode.has_value())
        return Error::from_string_literal("PRAGMA journal_mode returned no mode");
    return *applied_journal_mode;
}

ErrorOr<void, Error> Database::set_synchronous_pragma(Synchronous synchronous)
{
    auto synchronous_string = [&]() {
        switch (synchronous) {
        case Synchronous::Off:
            return "OFF"sv;
        case Synchronous::Normal:
            return "NORMAL"sv;
        case Synchronous::Full:
            return "FULL"sv;
        case Synchronous::Extra:
            return "EXTRA"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    auto pragma = ByteString::formatted("PRAGMA synchronous={};", synchronous_string);
    TRY(execute_raw(pragma));

    return {};
}

ErrorOr<void, Error> Database::set_busy_timeout(i32 milliseconds)
{
    if (milliseconds < 0)
        return Error::from_string_literal("Database busy timeout must not be negative");
    SQL_TRY(sqlite3_busy_timeout(m_database, milliseconds));
    return {};
}

ErrorOr<void, Error> Database::set_foreign_keys_pragma(ForeignKeys foreign_keys)
{
    TRY(execute_raw(foreign_keys == ForeignKeys::Yes ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;"));
    if (foreign_keys == ForeignKeys::No)
        return {};

    // https://www.sqlite.org/foreignkeys.html#fk_enable
    // SQLite can be built to parse foreign keys without enforcing them. Such a build takes the pragma without acting
    // on it, and has no value to read back.
    auto statement_id = TRY(prepare_statement("PRAGMA foreign_keys;"sv));
    size_t row_count = 0;
    bool are_enforced = false;
    TRY(try_execute_statement(statement_id, [&](auto result_statement_id) -> ErrorOr<void, Error> {
        ++row_count;
        are_enforced = result_column<i32>(result_statement_id, 0) == 1;
        return {};
    }));

    if (row_count != 1 || !are_enforced)
        return Error::from_string_literal("This build of SQLite does not enforce foreign keys");
    return {};
}

ErrorOr<void, Error> Database::set_temp_store_pragma(TempStore temp_store)
{
    if (temp_store == TempStore::Memory)
        TRY(execute_raw("PRAGMA temp_store=MEMORY;"));

    return {};
}

}
