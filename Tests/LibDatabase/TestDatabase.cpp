/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <Libraries/LibDatabase/Database.h>
#include <Libraries/LibDatabase/ResultRow.h>
#include <sqlite3.h>

using DB = Database::Database;

TEST_CASE(string_can_contain_null_bytes)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    database->execute_statement(TRY_OR_FAIL(database->prepare_statement(R"#(
        CREATE TABLE WebStorage (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )#"sv)),
        {});

    auto get_item_statement = TRY_OR_FAIL(database->prepare_statement("SELECT value FROM WebStorage WHERE key = ?;"sv));
    auto set_item_statement = TRY_OR_FAIL(database->prepare_statement("INSERT OR REPLACE INTO WebStorage VALUES (?, ?);"sv));
    auto delete_item_statement = TRY_OR_FAIL(database->prepare_statement("DELETE FROM WebStorage WHERE key = ?;"sv));

    auto get_item = [&](String const& key) {
        Optional<String> result;
        database->execute_statement(
            get_item_statement,
            [&](auto statement_id) -> ErrorOr<void, Database::Error> {
                result = database->result_column<String>(statement_id, 0);
                return {};
            },
            key);
        return result;
    };

    auto set_item = [&](String const& key, String const& value) {
        database->execute_statement(
            set_item_statement,
            {},
            key,
            value);
    };

    auto remove_item = [&](String const& key) {
        database->execute_statement(
            delete_item_statement,
            {},
            key);
    };

    EXPECT_EQ(get_item("my_key"_string), Optional<String> {});
    set_item("my_key"_string, "my_value"_string);
    EXPECT_EQ(get_item("my_key"_string), Optional<String> { "my_value"_string });
    set_item("my_key"_string, "my_value_with_\0_null"_string);
    EXPECT_EQ(get_item("my_key"_string), Optional<String> { "my_value_with_\0_null"_string });
    remove_item("my_key"_string);
    EXPECT_EQ(get_item("my_key"_string), Optional<String> {});
}

TEST_CASE(double_values_can_be_bound_and_read)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    auto statement = TRY_OR_FAIL(database->prepare_statement("SELECT ?;"sv));

    double result = 0;
    database->execute_statement(
        statement,
        [&](auto statement_id) -> ErrorOr<void, Database::Error> {
            result = database->result_column<double>(statement_id, 0);
            return {};
        },
        3.25);

    EXPECT_EQ(result, 3.25);
}

TEST_CASE(interrupted_statement_can_be_reused)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    auto statement = TRY_OR_FAIL(database->prepare_statement(R"#(
        SELECT 1
        UNION ALL SELECT 2
        UNION ALL SELECT 3;
    )#"sv));

    size_t row_count = 0;
    auto outcome = database->execute_interruptible_statement(statement, [&](auto) -> ErrorOr<void, Database::Error> {
        ++row_count;
        database->interrupt();
        return {};
    });
    EXPECT_EQ(outcome, Database::Database::StatementExecutionOutcome::Interrupted);
    EXPECT_EQ(row_count, 1u);

    row_count = 0;
    outcome = database->execute_interruptible_statement(statement, [&](auto) -> ErrorOr<void, Database::Error> {
        ++row_count;
        return {};
    });
    EXPECT_EQ(outcome, Database::Database::StatementExecutionOutcome::Completed);
    EXPECT_EQ(row_count, 3u);
}

TEST_CASE(busy_timeout_can_be_configured)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    TRY_OR_FAIL(database->set_busy_timeout(250));

    auto statement = TRY_OR_FAIL(database->prepare_statement("PRAGMA busy_timeout;"sv));
    i32 busy_timeout = 0;
    database->execute_statement(statement, [&](auto statement_id) -> ErrorOr<void, Database::Error> {
        busy_timeout = database->result_column<i32>(statement_id, 0);
        return {};
    });
    EXPECT_EQ(busy_timeout, 250);
}

static i32 read_foreign_keys_pragma(DB& database)
{
    auto statement = MUST(database.prepare_statement("PRAGMA foreign_keys;"sv));
    i32 value = -1;
    database.execute_statement(statement, [&](auto statement_id) -> ErrorOr<void, Database::Error> { value = database.result_column<i32>(statement_id, 0); return {}; });
    return value;
}

TEST_CASE(foreign_keys_option_controls_pragma)
{
    auto default_database = TRY_OR_FAIL(DB::create_memory_backed());
    EXPECT_EQ(read_foreign_keys_pragma(default_database), 0);
    EXPECT(default_database->options().foreign_keys == DB::ForeignKeys::No);

    auto foreign_keys_database = TRY_OR_FAIL(DB::create_memory_backed({ .foreign_keys = DB::ForeignKeys::Yes }));
    EXPECT_EQ(read_foreign_keys_pragma(foreign_keys_database), 1);
    EXPECT(foreign_keys_database->options().foreign_keys == DB::ForeignKeys::Yes);
}

TEST_CASE(options_report_the_applied_journal_mode)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed({
        .journal_mode = DB::JournalMode::WriteAheadLog,
        .synchronous = DB::Synchronous::Extra,
        .foreign_keys = DB::ForeignKeys::Yes,
    }));
    EXPECT(database->options().journal_mode == DB::JournalMode::Memory);
    EXPECT(database->options().synchronous == DB::Synchronous::Extra);
    EXPECT(database->options().foreign_keys == DB::ForeignKeys::Yes);
}

static i32 count_rows(DB& database, StringView table)
{
    auto statement = MUST(database.prepare_statement(MUST(String::formatted("SELECT COUNT(*) FROM {};", table))));
    i32 count = -1;
    database.execute_statement(statement, [&](auto statement_id) -> ErrorOr<void, Database::Error> { count = database.result_column<i32>(statement_id, 0); return {}; });
    return count;
}

TEST_CASE(foreign_key_cascade_follows_the_pragma)
{
    auto create_schema = [](DB& database) {
        database.execute_statement(MUST(database.prepare_statement("CREATE TABLE parent (id INTEGER PRIMARY KEY);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("CREATE TABLE child (id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id) ON DELETE CASCADE);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("INSERT INTO parent (id) VALUES (1);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("INSERT INTO child (id, parent_id) VALUES (10, 1);"sv)), {});
        database.execute_statement(MUST(database.prepare_statement("DELETE FROM parent WHERE id = 1;"sv)), {});
    };

    auto enabled = TRY_OR_FAIL(DB::create_memory_backed({ .foreign_keys = DB::ForeignKeys::Yes }));
    create_schema(enabled);
    EXPECT_EQ(count_rows(enabled, "child"sv), 0);

    auto disabled = TRY_OR_FAIL(DB::create_memory_backed());
    create_schema(disabled);
    EXPECT_EQ(count_rows(disabled, "child"sv), 1);
}

TEST_CASE(transaction_commits_on_success_and_rolls_back_on_error)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (v INTEGER);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (?);"sv));

    TRY_OR_FAIL(database->transaction([&]() -> ErrorOr<void, Database::Error> {
        TRY(database->try_execute_statement(insert, {}, 1));
        return {};
    }));
    EXPECT_EQ(count_rows(*database, "t"sv), 1);

    auto rolled_back = database->transaction([&]() -> ErrorOr<void, Database::Error> {
        TRY(database->try_execute_statement(insert, {}, 2));
        return Error::from_string_literal("deliberate failure");
    });
    EXPECT(rolled_back.is_error());
    EXPECT_EQ(count_rows(*database, "t"sv), 1);
}

static NonnullRefPtr<DB> create_database_with_an_id_table()
{
    auto database = MUST(DB::create_memory_backed());
    MUST(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v BLOB);"sv));
    return database;
}

static Vector<i64> stored_ids(DB& database)
{
    Vector<i64> ids;
    auto select = MUST(database.prepare_statement("SELECT id FROM t ORDER BY id;"sv));

    database.execute_statement(select, [&](auto statement_id) -> ErrorOr<void, Database::Error> {
        ids.append(database.result_column<i64>(statement_id, 0));
        return {};
    });

    return ids;
}

TEST_CASE(open_transaction_commits_its_changes)
{
    auto database = create_database_with_an_id_table();

    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    EXPECT(transaction.is_open());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    TRY_OR_FAIL(transaction.commit());
    EXPECT(!transaction.is_open());

    // A connection sees its own uncommitted rows, so only rolling a later transaction back shows that this one committed.
    auto later_transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(later_transaction.rollback());
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 1 }));
}

TEST_CASE(open_transaction_rolls_back_when_asked_or_dropped)
{
    auto database = create_database_with_an_id_table();

    {
        auto transaction = TRY_OR_FAIL(database->begin_transaction());
        TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

        TRY_OR_FAIL(transaction.rollback());
        EXPECT(!transaction.is_open());
        EXPECT(stored_ids(*database).is_empty());
    }

    {
        auto transaction = TRY_OR_FAIL(database->begin_transaction());
        TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));
    }
    EXPECT(stored_ids(*database).is_empty());

    auto later_transaction = TRY_OR_FAIL(database->begin_transaction());
    EXPECT(later_transaction.is_open());
}

TEST_CASE(second_transaction_is_refused_while_one_is_open)
{
    auto database = create_database_with_an_id_table();
    auto transaction = TRY_OR_FAIL(database->begin_transaction());

    auto second_transaction = database->begin_transaction();
    EXPECT(second_transaction.is_error());
    EXPECT(!second_transaction.error().result_code().has_value());

    auto callback_transaction = database->transaction([]() -> ErrorOr<void, Database::Error> { return {}; });
    EXPECT(callback_transaction.is_error());
    EXPECT(!callback_transaction.error().result_code().has_value());

    EXPECT(transaction.is_open());
    TRY_OR_FAIL(transaction.commit());
}

TEST_CASE(savepoint_rollback_undoes_only_its_own_changes)
{
    auto database = create_database_with_an_id_table();
    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    auto savepoint = TRY_OR_FAIL(transaction.savepoint());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));

    auto duplicate = database->execute_raw("INSERT INTO t (id) VALUES (1);"sv);
    EXPECT(duplicate.is_error());
    EXPECT(duplicate.error().has_result_code(Database::ResultCode::ConstraintPrimaryKey));
    EXPECT(transaction.is_open());

    TRY_OR_FAIL(savepoint.rollback());
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 1 }));

    TRY_OR_FAIL(transaction.commit());
}

TEST_CASE(savepoint_rolls_back_when_dropped)
{
    auto database = create_database_with_an_id_table();
    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    {
        auto savepoint = TRY_OR_FAIL(transaction.savepoint());
        TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));
    }
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 1 }));

    TRY_OR_FAIL(transaction.commit());
}

TEST_CASE(savepoint_release_keeps_its_changes)
{
    auto database = create_database_with_an_id_table();
    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    auto savepoint = TRY_OR_FAIL(transaction.savepoint());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));
    TRY_OR_FAIL(savepoint.release());

    auto second_release = savepoint.release();
    EXPECT(second_release.is_error());
    EXPECT(!second_release.error().result_code().has_value());

    TRY_OR_FAIL(savepoint.rollback());
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 1, 2 }));

    TRY_OR_FAIL(transaction.commit());
}

TEST_CASE(transaction_has_one_savepoint_at_a_time)
{
    auto database = create_database_with_an_id_table();
    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    auto savepoint = TRY_OR_FAIL(transaction.savepoint());

    auto second_savepoint = transaction.savepoint();
    EXPECT(second_savepoint.is_error());
    EXPECT(!second_savepoint.error().result_code().has_value());

    auto commit_with_a_savepoint = transaction.commit();
    EXPECT(commit_with_a_savepoint.is_error());
    EXPECT(!commit_with_a_savepoint.error().result_code().has_value());
    EXPECT(transaction.is_open());

    TRY_OR_FAIL(savepoint.release());

    auto later_savepoint = TRY_OR_FAIL(transaction.savepoint());
    TRY_OR_FAIL(later_savepoint.release());
    TRY_OR_FAIL(transaction.commit());
}

TEST_CASE(transaction_rollback_closes_its_savepoint)
{
    auto database = create_database_with_an_id_table();
    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    auto savepoint = TRY_OR_FAIL(transaction.savepoint());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    TRY_OR_FAIL(transaction.rollback());
    EXPECT(stored_ids(*database).is_empty());

    auto release = savepoint.release();
    EXPECT(release.is_error());
    EXPECT(!release.error().result_code().has_value());

    TRY_OR_FAIL(savepoint.rollback());
}

TEST_CASE(failed_commit_can_leave_the_transaction_open)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed({ .foreign_keys = DB::ForeignKeys::Yes }));
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE parent (id INTEGER PRIMARY KEY);"sv));
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED);"sv));

    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, parent_id) VALUES (1, 5);"sv));

    // https://www.sqlite.org/foreignkeys.html#fk_deferred
    // A deferred foreign key is checked at the commit, and a commit that fails the check leaves the transaction open.
    auto failed_commit = transaction.commit();
    EXPECT(failed_commit.is_error());
    EXPECT_EQ(failed_commit.error().result_code(), Optional<int> { SQLITE_CONSTRAINT_FOREIGNKEY });
    EXPECT(transaction.is_open());

    TRY_OR_FAIL(database->execute_raw("INSERT INTO parent (id) VALUES (5);"sv));
    TRY_OR_FAIL(transaction.commit());
    EXPECT(!transaction.is_open());
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 1 }));
}

TEST_CASE(full_database_can_lose_the_whole_transaction)
{
    auto database = create_database_with_an_id_table();
    TRY_OR_FAIL(database->execute_raw("PRAGMA max_page_count = 8;"sv));

    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));
    auto savepoint = TRY_OR_FAIL(transaction.savepoint());

    // Eight pages hold less than this at any page size.
    auto full = database->execute_raw("INSERT INTO t (id, v) VALUES (2, zeroblob(1000000));"sv);
    EXPECT(full.is_error());
    EXPECT(full.error().has_result_code(Database::ResultCode::Full));

    // https://www.sqlite.org/lang_transaction.html
    // SQLite rolled the whole transaction back, so the row from before the savepoint is gone as well.
    EXPECT(!transaction.is_open());
    EXPECT(stored_ids(*database).is_empty());

    auto release = savepoint.release();
    EXPECT(release.is_error());
    EXPECT(!release.error().result_code().has_value());

    auto later_savepoint = transaction.savepoint();
    EXPECT(later_savepoint.is_error());
    EXPECT(!later_savepoint.error().result_code().has_value());

    auto commit = transaction.commit();
    EXPECT(commit.is_error());
    EXPECT(!commit.error().result_code().has_value());

    TRY_OR_FAIL(savepoint.rollback());
    TRY_OR_FAIL(transaction.rollback());

    auto later_transaction = TRY_OR_FAIL(database->begin_transaction());
    EXPECT(later_transaction.is_open());
}

TEST_CASE(full_database_can_keep_the_transaction)
{
    auto database = create_database_with_an_id_table();
    TRY_OR_FAIL(database->execute_raw("PRAGMA max_page_count = 40;"sv));

    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));
    auto savepoint = TRY_OR_FAIL(transaction.savepoint());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));

    // Forty pages hold less than these rows at any page size. SQLite undoes one statement that inserts them all.
    auto full = database->execute_raw(R"#(
        WITH RECURSIVE numbers(i) AS (SELECT 10 UNION ALL SELECT i + 1 FROM numbers WHERE i < 10000)
        INSERT INTO t (id, v) SELECT i, zeroblob(500) FROM numbers;
    )#"sv);
    EXPECT(full.is_error());
    EXPECT(full.error().has_result_code(Database::ResultCode::Full));

    EXPECT(transaction.is_open());
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 1, 2 }));

    TRY_OR_FAIL(savepoint.rollback());
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 1 }));

    TRY_OR_FAIL(transaction.commit());
}

TEST_CASE(stale_handles_leave_the_next_transaction_alone)
{
    auto database = create_database_with_an_id_table();

    Optional<DB::Transaction> stale_transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));
    Optional<DB::Savepoint> stale_savepoint = TRY_OR_FAIL(stale_transaction->savepoint());

    // https://www.sqlite.org/lang_conflict.html
    // This conflict clause rolls the whole transaction back.
    auto conflict = database->execute_raw("INSERT OR ROLLBACK INTO t (id) VALUES (1);"sv);
    EXPECT(conflict.is_error());
    EXPECT(conflict.error().has_result_code(Database::ResultCode::ConstraintPrimaryKey));
    EXPECT(!stale_transaction->is_open());

    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    auto savepoint = TRY_OR_FAIL(transaction.savepoint());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (20);"sv));

    auto stale_release = stale_savepoint->release();
    EXPECT(stale_release.is_error());
    EXPECT(!stale_release.error().result_code().has_value());

    TRY_OR_FAIL(stale_savepoint->rollback());
    stale_savepoint.clear();
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 20 }));

    TRY_OR_FAIL(savepoint.release());

    auto stale_commit = stale_transaction->commit();
    EXPECT(stale_commit.is_error());
    EXPECT(!stale_commit.error().result_code().has_value());

    auto stale_later_savepoint = stale_transaction->savepoint();
    EXPECT(stale_later_savepoint.is_error());
    EXPECT(!stale_later_savepoint.error().result_code().has_value());

    TRY_OR_FAIL(stale_transaction->rollback());
    stale_transaction.clear();

    EXPECT(transaction.is_open());
    EXPECT_EQ(stored_ids(*database), (Vector<i64> { 20 }));

    // Had a stale handle committed for this transaction, the row would outlast this rollback.
    TRY_OR_FAIL(transaction.rollback());
    EXPECT(stored_ids(*database).is_empty());
}

TEST_CASE(lost_transaction_takes_no_savepoint_and_no_commit)
{
    auto database = create_database_with_an_id_table();
    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    // https://www.sqlite.org/lang_conflict.html
    // This conflict clause rolls the whole transaction back.
    auto conflict = database->execute_raw("INSERT OR ROLLBACK INTO t (id) VALUES (1);"sv);
    EXPECT(conflict.is_error());

    // https://www.sqlite.org/lang_savepoint.html
    // Sent to SQLite, a savepoint would begin a transaction of its own here.
    auto savepoint = transaction.savepoint();
    EXPECT(savepoint.is_error());
    EXPECT(!savepoint.error().result_code().has_value());

    auto commit = transaction.commit();
    EXPECT(commit.is_error());
    EXPECT(!commit.error().result_code().has_value());

    EXPECT(!transaction.is_open());
    TRY_OR_FAIL(transaction.rollback());
}

// https://www.sqlite.org/c3ref/hard_heap_limit64.html
// Setting the hard limit lowers the soft limit with it, so both are put back.
template<typename Callback>
static auto with_no_memory_left_for_sqlite(Callback callback)
{
    auto hard_limit = sqlite3_hard_heap_limit64(-1);
    auto soft_limit = sqlite3_soft_heap_limit64(-1);
    ScopeGuard restore_the_limits = [&] {
        sqlite3_hard_heap_limit64(hard_limit);
        sqlite3_soft_heap_limit64(soft_limit);
    };

    sqlite3_hard_heap_limit64(sqlite3_memory_used() + 1);
    return callback();
}

// https://www.sqlite.org/malloc.html#lookaside
// A connection takes its small allocations out of a pool that it already has, which the heap limit does not cover.
// Prepared statements keep what they took from it, so enough of them leave the pool empty.
static void use_up_the_lookaside_pool(DB& database)
{
    for (size_t i = 0; i < 500; ++i)
        MUST(database.prepare_statement("SELECT 1;"sv));
}

TEST_CASE(transaction_whose_rollback_failed_stays_open_and_only_rolls_back)
{
    auto database = create_database_with_an_id_table();
    use_up_the_lookaside_pool(*database);

    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    auto failed_rollback = with_no_memory_left_for_sqlite([&] { return transaction.rollback(); });
    EXPECT(failed_rollback.is_error());
    EXPECT_EQ(failed_rollback.error().result_code(), Optional<int> { SQLITE_NOMEM });
    EXPECT(transaction.is_open());

    auto savepoint = transaction.savepoint();
    EXPECT(savepoint.is_error());
    EXPECT(!savepoint.error().result_code().has_value());

    auto commit = transaction.commit();
    EXPECT(commit.is_error());
    EXPECT(!commit.error().result_code().has_value());
    EXPECT(transaction.is_open());

    TRY_OR_FAIL(transaction.rollback());
    EXPECT(!transaction.is_open());
    EXPECT(stored_ids(*database).is_empty());
}

TEST_CASE(savepoint_goes_with_a_transaction_whose_rollback_failed)
{
    auto database = create_database_with_an_id_table();
    use_up_the_lookaside_pool(*database);

    auto transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));
    auto savepoint = TRY_OR_FAIL(transaction.savepoint());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));

    auto failed_rollback = with_no_memory_left_for_sqlite([&] { return transaction.rollback(); });
    EXPECT(failed_rollback.is_error());

    auto release = savepoint.release();
    EXPECT(release.is_error());
    EXPECT(!release.error().result_code().has_value());

    // Rolling the savepoint back is another try at rolling back the transaction.
    TRY_OR_FAIL(savepoint.rollback());
    EXPECT(!transaction.is_open());
    EXPECT(stored_ids(*database).is_empty());
}

// Leaves the connection with a transaction that nothing holds any more and that could not be rolled back.
static void drop_a_transaction_whose_rollback_fails(DB& database, StringView sql_inside_the_transaction)
{
    use_up_the_lookaside_pool(database);

    // A transaction that is dropped does not say whether its rollback failed, so one that is rolled back goes first.
    {
        auto transaction = MUST(database.begin_transaction());
        MUST(database.execute_raw(sql_inside_the_transaction));

        auto failed_rollback = with_no_memory_left_for_sqlite([&] { return transaction.rollback(); });
        EXPECT(failed_rollback.is_error());
        MUST(transaction.rollback());
    }

    Optional<DB::Transaction> transaction = MUST(database.begin_transaction());
    MUST(database.execute_raw(sql_inside_the_transaction));

    with_no_memory_left_for_sqlite([&] { transaction.clear(); });
}

TEST_CASE(dropped_transaction_whose_rollback_failed_is_rolled_back_before_the_next_sql)
{
    auto ids_read_by = [](DB& database, auto execute) {
        Vector<i64> ids;
        execute([&](auto statement_id) -> ErrorOr<void, Database::Error> {
            ids.append(database.result_column<i64>(statement_id, 0));
            return {};
        });
        return ids;
    };

    {
        auto database = create_database_with_an_id_table();
        auto select = TRY_OR_FAIL(database->prepare_statement("SELECT id FROM t;"sv));
        drop_a_transaction_whose_rollback_fails(*database, "INSERT INTO t (id) VALUES (1);"sv);

        auto ids = ids_read_by(*database, [&](auto on_result) { database->execute_statement(select, move(on_result)); });
        EXPECT(ids.is_empty());
    }

    {
        auto database = create_database_with_an_id_table();
        auto select = TRY_OR_FAIL(database->prepare_statement("SELECT id FROM t;"sv));
        drop_a_transaction_whose_rollback_fails(*database, "INSERT INTO t (id) VALUES (1);"sv);

        auto ids = ids_read_by(*database, [&](auto on_result) {
            EXPECT_EQ(database->execute_interruptible_statement(select, move(on_result)), DB::StatementExecutionOutcome::Completed);
        });
        EXPECT(ids.is_empty());
    }

    {
        auto database = create_database_with_an_id_table();
        drop_a_transaction_whose_rollback_fails(*database, "INSERT INTO t (id) VALUES (1);"sv);

        // Run inside the dropped transaction, this row would go with it.
        TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));
        EXPECT_EQ(stored_ids(*database), (Vector<i64> { 2 }));
    }

    {
        auto database = create_database_with_an_id_table();
        drop_a_transaction_whose_rollback_fails(*database, "INSERT INTO t (id) VALUES (1);"sv);

        auto transaction = TRY_OR_FAIL(database->begin_transaction());
        TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (2);"sv));
        TRY_OR_FAIL(transaction.commit());
        EXPECT_EQ(stored_ids(*database), (Vector<i64> { 2 }));
    }

    {
        auto database = create_database_with_an_id_table();
        drop_a_transaction_whose_rollback_fails(*database, "DROP TABLE t;"sv);

        // The table is gone for as long as the dropped transaction is open.
        auto select = database->prepare_statement("SELECT id FROM t;"sv);
        EXPECT(!select.is_error());
    }
}

TEST_CASE(statement_fails_while_a_dropped_transaction_cannot_be_rolled_back)
{
    auto database = create_database_with_an_id_table();
    use_up_the_lookaside_pool(*database);

    // Running this statement takes no memory, so it only fails for the rollback that it waits for.
    auto select_a_constant = TRY_OR_FAIL(database->prepare_statement("SELECT 1;"sv));

    Optional<DB::Transaction> transaction = TRY_OR_FAIL(database->begin_transaction());
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id) VALUES (1);"sv));

    size_t rows_read = 0;
    auto read = with_no_memory_left_for_sqlite([&] {
        transaction.clear();

        return database->try_execute_statement(select_a_constant, [&](auto) -> ErrorOr<void, Database::Error> {
            ++rows_read;
            return {};
        });
    });

    EXPECT(read.is_error());
    EXPECT_EQ(read.error().result_code(), Optional<int> { SQLITE_NOMEM });
    EXPECT_EQ(rows_read, 0u);

    EXPECT(stored_ids(*database).is_empty());
}

// A column with no declared type has NONE affinity, so each bound value keeps its storage class.
static Database::StatementID prepare_untyped_value_table(DB& database)
{
    database.execute_statement(MUST(database.prepare_statement("CREATE TABLE t (i INTEGER PRIMARY KEY, v);"sv)), {});
    return MUST(database.prepare_statement("SELECT v FROM t WHERE i = :i;"sv));
}

TEST_CASE(checked_reads_reject_wrong_storage_class)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    auto select = prepare_untyped_value_table(*database);

    auto insert_value = MUST(database->prepare_statement("INSERT INTO t (i, v) VALUES (?, ?);"sv));
    auto insert_null = MUST(database->prepare_statement("INSERT INTO t (i) VALUES (?);"sv));

    database->execute_statement(insert_value, {}, 1, "hello"_string);
    database->execute_statement(insert_value, {}, 2, static_cast<i64>(42));
    database->execute_statement(insert_value, {}, 3, ByteString { "\x01\x02\x03", 3 });
    database->execute_statement(insert_null, {}, 4);
    database->execute_statement(insert_value, {}, 5, ByteString {});

    auto with_value = [&](i32 key, auto fn) {
        TRY_OR_FAIL(database->try_execute_bound_statement(
            select,
            [&](auto& bind) -> ErrorOr<void, Database::Error> { return bind("i"sv, key); },
            [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> { fn(row); return {}; }));
    };

    with_value(1, [&](Database::ResultRow& row) {
        EXPECT_EQ(TRY_OR_FAIL(row.read_text("v"sv, 1024)), "hello"_string);
        EXPECT(row.read_integer<i64>("v"sv).is_error());
        auto blob = row.read_blob("v"sv, 1024);
        EXPECT(blob.is_error());
        EXPECT_EQ(blob.error().string_literal(), "Column is not a blob"sv);
    });
    with_value(2, [&](Database::ResultRow& row) {
        EXPECT_EQ(TRY_OR_FAIL(row.read_integer<i64>("v"sv)), 42);
        auto text = row.read_text("v"sv, 1024);
        EXPECT(text.is_error());
        EXPECT_EQ(text.error().string_literal(), "Column is not text"sv);
        EXPECT(row.read_blob("v"sv, 1024).is_error());
    });
    with_value(3, [&](Database::ResultRow& row) {
        EXPECT_EQ(TRY_OR_FAIL(row.read_blob("v"sv, 1024)), MUST(ByteBuffer::copy("\x01\x02\x03"sv.bytes())));
        EXPECT(row.read_integer<i64>("v"sv).is_error());
        EXPECT(row.read_text("v"sv, 1024).is_error());
    });
    with_value(4, [&](Database::ResultRow& row) {
        // A SQL NULL is not any of the expected storage classes.
        EXPECT(row.read_integer<i64>("v"sv).is_error());
        EXPECT(row.read_text("v"sv, 1024).is_error());
        EXPECT(row.read_blob("v"sv, 1024).is_error());
    });
    with_value(5, [&](Database::ResultRow& row) {
        // A zero-length blob is a valid empty value, distinct from NULL.
        EXPECT(TRY_OR_FAIL(row.read_blob("v"sv, 1024)).is_empty());
        EXPECT_EQ(TRY_OR_FAIL(row.is_null("v"sv)), false);
    });
}

TEST_CASE(bounded_reads_reject_oversized_cells)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    auto select = prepare_untyped_value_table(*database);

    auto insert_value = MUST(database->prepare_statement("INSERT INTO t (i, v) VALUES (?, ?);"sv));
    database->execute_statement(insert_value, {}, 1, "abcdefghij"_string);
    database->execute_statement(insert_value, {}, 2, ByteString { "0123456789", 10 });

    auto with_value = [&](i32 key, auto fn) {
        TRY_OR_FAIL(database->try_execute_bound_statement(
            select,
            [&](auto& bind) -> ErrorOr<void, Database::Error> { return bind("i"sv, key); },
            [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> { fn(row); return {}; }));
    };

    with_value(1, [&](Database::ResultRow& row) {
        auto oversized_text = row.read_text("v"sv, 4);
        EXPECT(oversized_text.is_error());
        EXPECT_EQ(oversized_text.error().string_literal(), "Text column exceeds the size limit"sv);
        EXPECT(!row.read_text("v"sv, 10).is_error());
    });
    with_value(2, [&](Database::ResultRow& row) {
        auto oversized_blob = row.read_blob("v"sv, 4);
        EXPECT(oversized_blob.is_error());
        EXPECT_EQ(oversized_blob.error().string_literal(), "Blob column exceeds the size limit"sv);
        EXPECT(!row.read_blob("v"sv, 10).is_error());
    });
}

TEST_CASE(named_binding_is_order_independent)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE named (a INTEGER NOT NULL, b TEXT NOT NULL, c INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO named (a, b, c) VALUES (:a, :b, :c);"sv));

    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("c"sv, static_cast<i64>(30)));
        TRY(bind("b"sv, "two"_string));
        TRY(bind("a"sv, static_cast<i64>(10)));
        return {};
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT c, b, a FROM named;"sv));
    EXPECT_EQ(TRY_OR_FAIL(database->result_column_index(select, "c"sv)), 0);
    EXPECT_EQ(TRY_OR_FAIL(database->result_column_index(select, "b"sv)), 1);
    EXPECT_EQ(TRY_OR_FAIL(database->result_column_index(select, "a"sv)), 2);

    bool read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(
        select,
        [](auto&) -> ErrorOr<void, Database::Error> { return {}; },
        [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            read = true;
            EXPECT_EQ(TRY(row.read_integer<i64>("a"sv)), 10);
            EXPECT_EQ(TRY(row.read_text("b"sv, 1024)), "two"_string);
            EXPECT_EQ(TRY(row.read_integer<i64>("c"sv)), 30);
            return {};
        }));
    EXPECT(read);
}

TEST_CASE(bind_rejects_unknown_and_nul_names)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE named (a INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO named (a) VALUES (:a);"sv));

    auto unknown = database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        return bind("missing"sv, static_cast<i64>(1));
    });
    EXPECT(unknown.is_error());
    EXPECT_EQ(unknown.error().string_literal(), "Unknown bound parameter name"sv);

    // The name up to the NUL ("a") is a real parameter, but the supplied name is not, so it must be
    // rejected rather than silently bound as ":a".
    auto embedded_nul = database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        return bind(StringView { "a\0b", 3 }, static_cast<i64>(1));
    });
    EXPECT(embedded_nul.is_error());
    EXPECT_EQ(embedded_nul.error().string_literal(), "Bound parameter name contains an embedded NUL"sv);
}

TEST_CASE(bound_execute_requires_every_parameter)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (owner_id INTEGER NOT NULL, v INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (owner_id, v) VALUES (:owner_id, :v);"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("owner_id"sv, static_cast<i64>(1)));
        TRY(bind("v"sv, static_cast<i64>(42)));
        return {};
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t WHERE owner_id = :owner_id;"sv));
    auto missing = database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void, Database::Error> { return {}; });
    EXPECT(missing.is_error());
    EXPECT_EQ(missing.error().string_literal(), "Statement executed with an unbound parameter"sv);

    bool matched = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [&](auto& bind) -> ErrorOr<void, Database::Error> { return bind("owner_id"sv, static_cast<i64>(1)); }, [&](Database::ResultRow&) -> ErrorOr<void, Database::Error> { matched = true; return {}; }));
    EXPECT(matched);
}

TEST_CASE(bound_execute_clears_state_between_cycles)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (a INTEGER NOT NULL, b INTEGER NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (a, b) VALUES (:a, :b);"sv));

    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("a"sv, static_cast<i64>(1)));
        TRY(bind("b"sv, static_cast<i64>(2)));
        return {};
    }));

    EXPECT(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
                       return bind("a"sv, static_cast<i64>(3));
                   })
            .is_error());

    EXPECT(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
                       TRY(bind("a"sv, static_cast<i64>(4)));
                       return bind("missing"sv, static_cast<i64>(0));
                   })
            .is_error());
    EXPECT(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
                       return bind("b"sv, static_cast<i64>(5));
                   })
            .is_error());

    EXPECT_EQ(count_rows(*database, "t"sv), 1);
}

TEST_CASE(result_column_index_rejects_ambiguous_and_missing_names)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (a INTEGER, b INTEGER);"sv)), {});
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (a, b) VALUES (1, 2);"sv)), {});

    auto duplicate = TRY_OR_FAIL(database->prepare_statement("SELECT a AS id, b AS id FROM t;"sv));
    bool checked = false;
    database->execute_statement(duplicate, [&](auto) -> ErrorOr<void, Database::Error> {
        checked = true;
        auto ambiguous = database->result_column_index(duplicate, "id"sv);
        EXPECT(ambiguous.is_error());
        EXPECT_EQ(ambiguous.error().string_literal(), "Ambiguous result column name"sv);
        auto missing = database->result_column_index(duplicate, "nope"sv);
        EXPECT(missing.is_error());
        EXPECT_EQ(missing.error().string_literal(), "Unknown result column name"sv);
        return {};
    });
    EXPECT(checked);
}

TEST_CASE(bound_execute_handles_a_zero_parameter_statement)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (v INTEGER NOT NULL);"sv)), {});
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (1);"sv)), {});

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    bool ran = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void, Database::Error> { return {}; }, [&](Database::ResultRow&) -> ErrorOr<void, Database::Error> { ran = true; return {}; }));
    EXPECT(ran);
}

TEST_CASE(utf16_text_round_trips_lonely_surrogates)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (s TEXT NOT NULL);"sv)), {});

    static constexpr char16_t code_units[] = { 'a', 0xd800, 'b' };
    auto text = Utf16String::from_utf16(Utf16View { code_units, 3 });

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (s) VALUES (:s);"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        return bind("s"sv, text);
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT s FROM t;"sv));
    bool read = false;
    database->execute_statement(select, [&](auto statement_id) -> ErrorOr<void, Database::Error> {
        read = true;
        EXPECT_EQ(database->result_column<Utf16String>(statement_id, 0), text);
        return {};
    });
    EXPECT(read);

    bool row_read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void, Database::Error> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        row_read = true;
        EXPECT_EQ(TRY(row.read_utf16_text("s"sv, 1024)), text);
        return {}; }));
    EXPECT(row_read);
}

TEST_CASE(utf16_text_read_rejects_malformed_bytes)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (s TEXT NOT NULL);"sv)), {});
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (s) VALUES (CAST(x'ff' AS TEXT));"sv)), {});

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT s FROM t;"sv));
    bool read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, [](auto&) -> ErrorOr<void, Database::Error> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        read = true;
        EXPECT(row.read_utf16_text("s"sv, 1024).is_error());
        return {}; }));
    EXPECT(read);
}

TEST_CASE(named_binding_supports_more_than_64_parameters)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());

    StringBuilder builder;
    builder.append("SELECT "sv);
    for (int i = 0; i < 70; ++i) {
        if (i != 0)
            builder.append(", "sv);
        builder.appendff(":p{}", i);
    }
    auto statement = TRY_OR_FAIL(database->prepare_statement(builder.string_view()));

    bool ran = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(statement, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        for (int i = 0; i < 70; ++i)
            TRY(bind(ByteString::formatted("p{}", i), static_cast<i64>(i)));
        return {}; }, [&](Database::ResultRow&) -> ErrorOr<void, Database::Error> { ran = true; return {}; }));
    EXPECT(ran);

    EXPECT(database->try_execute_bound_statement(statement, [&](auto& bind) -> ErrorOr<void, Database::Error> {
                       for (int i = 0; i < 69; ++i)
                           TRY(bind(ByteString::formatted("p{}", i), static_cast<i64>(i)));
                       return {};
                   })
            .is_error());
}

TEST_CASE(result_row_reads_columns_by_name)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    database->execute_statement(TRY_OR_FAIL(database->prepare_statement("CREATE TABLE t (n INTEGER NOT NULL, s TEXT NOT NULL, b BLOB NOT NULL);"sv)), {});
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (n, s, b) VALUES (:n, :s, :b);"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        TRY(bind("n"sv, static_cast<i64>(7)));
        TRY(bind("s"sv, "hi"_string));
        TRY(bind("b"sv, ByteString { "\x01\x02", 2 }));
        return {};
    }));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT b, s, n FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void, Database::Error> { return {}; };
    bool read = false;
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        read = true;
        EXPECT_EQ(TRY(row.read_integer<i64>("n"sv)), 7);
        EXPECT_EQ(TRY(row.read_text("s"sv, 1024)), "hi"_string);
        EXPECT_EQ(TRY(row.read_blob("b"sv, 1024)), MUST(ByteBuffer::copy("\x01\x02"sv.bytes())));
        return {};
    }));
    EXPECT(read);

    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT(row.read_integer<i64>("nope"sv).is_error());
        EXPECT(row.read_integer<i64>("s"sv).is_error());
        return {};
    }));
}

TEST_CASE(typed_integer_reads_check_range_and_storage_class)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (v INTEGER, s TEXT NOT NULL);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v, s) VALUES (:v, :s);"sv));
    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v, s FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void, Database::Error> { return {}; };

    auto store = [&](i64 value) {
        TRY_OR_FAIL(database->execute_raw("DELETE FROM t;"sv));
        TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("v"sv, value));
            TRY(bind("s"sv, "text"_string));
            return {};
        }));
    };

    store(static_cast<i64>(NumericLimits<i32>::max()) + 1);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT(row.read_integer<i32>("v"sv).is_error());
        EXPECT_EQ(TRY(row.read_integer<u32>("v"sv)), static_cast<u32>(NumericLimits<i32>::max()) + 1);
        EXPECT_EQ(TRY(row.read_integer<i64>("v"sv)), static_cast<i64>(NumericLimits<i32>::max()) + 1);
        return {};
    }));

    store(static_cast<i64>(NumericLimits<u32>::max()) + 1);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT(row.read_integer<u32>("v"sv).is_error());
        return {};
    }));

    store(-1);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT(row.read_integer<u32>("v"sv).is_error());
        EXPECT(row.read_integer<u64>("v"sv).is_error());
        EXPECT_EQ(TRY(row.read_integer<i32>("v"sv)), -1);
        return {};
    }));

    store(0);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT(row.read_integer<i64>("s"sv).is_error());
        EXPECT(row.read_optional_integer<i64>("s"sv).is_error());
        EXPECT_EQ(TRY(row.is_null("v"sv)), false);
        EXPECT_EQ(TRY(row.read_optional_integer<i64>("v"sv)), Optional<i64> { 0 });
        return {};
    }));

    auto select_max = TRY_OR_FAIL(database->prepare_statement("SELECT MAX(v) AS maximum FROM t WHERE v > 100;"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(select_max, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT_EQ(TRY(row.is_null("maximum"sv)), true);
        EXPECT_EQ(TRY(row.read_optional_integer<i64>("maximum"sv)), Optional<i64> {});
        EXPECT(row.read_integer<i64>("maximum"sv).is_error());
        return {};
    }));
}

TEST_CASE(borrowed_bytes_bind_as_blobs)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (b BLOB);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (b) VALUES (:b);"sv));
    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT b, typeof(b) AS kind, b IS NULL AS absent FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void, Database::Error> { return {}; };

    auto payload = MUST(ByteBuffer::copy("\x01\x02\x03"sv.bytes()));
    TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        return bind("b"sv, payload.bytes());
    }));
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT_EQ(TRY(row.read_text("kind"sv, 16)), "blob"_string);
        EXPECT_EQ(TRY(row.read_bool("absent"sv)), false);
        EXPECT_EQ(TRY(row.read_blob("b"sv, 1024)), payload);
        return {};
    }));

    auto store = [&](ReadonlyBytes bytes) {
        TRY_OR_FAIL(database->execute_raw("DELETE FROM t;"sv));
        TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
            return bind("b"sv, bytes);
        }));
    };

    ByteBuffer const empty_buffer {};
    for (auto bytes : { ReadonlyBytes {}, empty_buffer.bytes() }) {
        store(bytes);
        TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
            EXPECT_EQ(TRY(row.read_text("kind"sv, 16)), "blob"_string);
            EXPECT_EQ(TRY(row.read_bool("absent"sv)), false);
            EXPECT(TRY(row.read_blob("b"sv, 1024)).is_empty());
            return {};
        }));
    }
}

TEST_CASE(optional_placeholders_bind_null_when_empty)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (v INTEGER, s TEXT);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v, s) VALUES (:v, :s);"sv));
    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT v, s FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void, Database::Error> { return {}; };

    auto store = [&](Optional<u32> value, Optional<String> text) {
        TRY_OR_FAIL(database->execute_raw("DELETE FROM t;"sv));
        TRY_OR_FAIL(database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("v"sv, value));
            TRY(bind("s"sv, text));
            return {};
        }));
    };

    store(7u, "text"_string);
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT_EQ(TRY(row.is_null("v"sv)), false);
        EXPECT_EQ(TRY(row.read_optional_integer<u32>("v"sv)), Optional<u32> { 7 });
        EXPECT_EQ(TRY(row.read_optional_text("s"sv, 64)), Optional<String> { "text"_string });
        return {};
    }));

    store({}, {});
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT_EQ(TRY(row.is_null("v"sv)), true);
        EXPECT_EQ(TRY(row.read_optional_integer<u32>("v"sv)), Optional<u32> {});
        EXPECT_EQ(TRY(row.read_optional_text("s"sv, 64)), Optional<String> {});
        return {};
    }));

    store(0u, String {});
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT_EQ(TRY(row.is_null("v"sv)), false);
        EXPECT_EQ(TRY(row.read_optional_integer<u32>("v"sv)), Optional<u32> { 0 });
        EXPECT_EQ(TRY(row.read_optional_text("s"sv, 64)), Optional<String> { String {} });
        return {};
    }));
}

TEST_CASE(column_name_cache_survives_a_reprepare)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (a INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (a) VALUES (7);"sv));

    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT * FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void, Database::Error> { return {}; };

    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT_EQ(TRY(row.read_integer<i64>("a"sv)), 7);
        return {};
    }));

    TRY_OR_FAIL(database->execute_raw("ALTER TABLE t RENAME COLUMN a TO b;"sv));
    TRY_OR_FAIL(database->try_execute_bound_statement(select, no_binds, [&](Database::ResultRow& row) -> ErrorOr<void, Database::Error> {
        EXPECT_EQ(TRY(row.read_integer<i64>("b"sv)), 7);
        EXPECT(row.read_integer<i64>("a"sv).is_error());
        return {};
    }));
}

TEST_CASE(optional_row_execution_maps_the_row_if_there_is_one)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10), (2, 20);"sv));

    auto select_by_id = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t WHERE id = :id;"sv));
    auto read_v = [](Database::ResultRow& row) -> ErrorOr<i64, Database::Error> {
        return row.read_integer<i64>("v"sv);
    };

    auto value = database->try_execute_bound_statement_optional<i64>(select_by_id, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("id"sv, static_cast<i64>(2)); }, read_v);
    EXPECT_EQ(TRY_OR_FAIL(move(value)), Optional<i64> { 20 });

    auto none = database->try_execute_bound_statement_optional<i64>(select_by_id, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("id"sv, static_cast<i64>(99)); }, read_v);
    EXPECT_EQ(TRY_OR_FAIL(move(none)), Optional<i64> {});

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    auto many = database->try_execute_bound_statement_optional<i64>(select_all, [](auto&) -> ErrorOr<void, Database::Error> { return {}; }, read_v);
    EXPECT(many.is_error());
    EXPECT_EQ(many.error().string_literal(), "Statement returned more than one row"sv);

    // A row that cannot be read is an error, and not the absence of a row.
    auto read_a_column_that_is_not_there = [](Database::ResultRow& row) -> ErrorOr<i64, Database::Error> {
        return row.read_integer<i64>("w"sv);
    };
    auto unreadable = database->try_execute_bound_statement_optional<i64>(select_by_id, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("id"sv, static_cast<i64>(1)); }, read_a_column_that_is_not_there);
    EXPECT(unreadable.is_error());

    auto again = database->try_execute_bound_statement_optional<i64>(select_by_id, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("id"sv, static_cast<i64>(1)); }, read_v);
    EXPECT_EQ(TRY_OR_FAIL(move(again)), Optional<i64> { 10 });
}

TEST_CASE(optional_row_execution_tells_whether_a_returning_clause_gave_a_row)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10);"sv));

    auto update = TRY_OR_FAIL(database->prepare_statement("UPDATE t SET v = :v WHERE id = :id RETURNING id;"sv));
    auto read_id = [](Database::ResultRow& row) -> ErrorOr<i64, Database::Error> {
        return row.read_integer<i64>("id"sv);
    };
    auto update_row = [&](i64 id) {
        return database->try_execute_bound_statement_optional<i64>(
            update,
            [&](auto& bind) -> ErrorOr<void, Database::Error> {
                TRY(bind("v"sv, static_cast<i64>(30)));
                return bind("id"sv, id);
            },
            read_id);
    };

    EXPECT_EQ(TRY_OR_FAIL(update_row(1)), Optional<i64> { 1 });
    EXPECT_EQ(TRY_OR_FAIL(update_row(2)), Optional<i64> {});
}

TEST_CASE(one_row_execution_maps_the_single_row)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10), (2, 20);"sv));

    auto select_by_id = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t WHERE id = :id;"sv));
    auto read_v = [](Database::ResultRow& row) -> ErrorOr<i64, Database::Error> {
        return row.read_integer<i64>("v"sv);
    };

    auto value = database->try_execute_bound_statement_one<i64>(select_by_id, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("id"sv, static_cast<i64>(2)); }, read_v);
    EXPECT_EQ(TRY_OR_FAIL(move(value)), 20);

    auto none = database->try_execute_bound_statement_one<i64>(select_by_id, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("id"sv, static_cast<i64>(99)); }, read_v);
    EXPECT(none.is_error());
    EXPECT_EQ(none.error().string_literal(), "Statement returned no rows"sv);

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    auto many = database->try_execute_bound_statement_one<i64>(select_all, [](auto&) -> ErrorOr<void, Database::Error> { return {}; }, read_v);
    EXPECT(many.is_error());
    EXPECT_EQ(many.error().string_literal(), "Statement returned more than one row"sv);

    auto again = database->try_execute_bound_statement_one<i64>(select_by_id, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("id"sv, static_cast<i64>(1)); }, read_v);
    EXPECT_EQ(TRY_OR_FAIL(move(again)), 10);
}

TEST_CASE(one_row_execution_reads_a_returning_clause)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (:v) RETURNING id;"sv));
    auto read_id = [](Database::ResultRow& row) -> ErrorOr<i64, Database::Error> {
        return row.read_integer<i64>("id"sv);
    };

    auto first = database->try_execute_bound_statement_one<i64>(insert, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("v"sv, static_cast<i64>(10)); }, read_id);
    EXPECT_EQ(TRY_OR_FAIL(move(first)), 1);

    auto second = database->try_execute_bound_statement_one<i64>(insert, [](auto& bind) -> ErrorOr<void, Database::Error> { return bind("v"sv, static_cast<i64>(20)); }, read_id);
    EXPECT_EQ(TRY_OR_FAIL(move(second)), 2);
}

TEST_CASE(last_insert_rowid_is_that_of_the_last_insert_that_succeeded)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER UNIQUE);"sv));

    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (7, 10);"sv));
    EXPECT_EQ(database->last_insert_rowid(), 7);

    auto duplicate = database->execute_raw("INSERT INTO t (id, v) VALUES (8, 10);"sv);
    EXPECT(duplicate.is_error());
    EXPECT_EQ(database->last_insert_rowid(), 7);
}

TEST_CASE(bounded_execution_stops_before_the_callback_past_the_budget)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10), (2, 20), (3, 30);"sv));

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    size_t callback_invocations = 0;
    auto result = database->try_execute_bound_statement(
        select_all,
        2uz,
        [](auto&) -> ErrorOr<void, Database::Error> { return {}; },
        [&](Database::ResultRow&) -> ErrorOr<void, Database::Error> {
            ++callback_invocations;
            return {};
        });
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Statement returned more rows than the caller allowed"sv);
    EXPECT_EQ(callback_invocations, 2uz);
}

TEST_CASE(collect_bounded_statement_maps_rows_under_the_budget)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void, Database::Error> { return {}; };
    auto read_v = [](Database::ResultRow& row) -> ErrorOr<i64, Database::Error> {
        return row.read_integer<i64>("v"sv);
    };

    auto empty = TRY_OR_FAIL(database->try_collect_bound_statement<i64>(select_all, 0uz, no_binds, read_v));
    EXPECT(empty.is_empty());

    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 10), (2, 20);"sv));
    auto zero_budget = database->try_collect_bound_statement<i64>(select_all, 0uz, no_binds, read_v);
    EXPECT(zero_budget.is_error());
    EXPECT_EQ(zero_budget.error().string_literal(), "Statement returned more rows than the caller allowed"sv);

    auto at_limit = TRY_OR_FAIL(database->try_collect_bound_statement<i64>(select_all, 2uz, no_binds, read_v));
    EXPECT_EQ(at_limit.size(), 2uz);
    EXPECT_EQ(at_limit[0], 10);
    EXPECT_EQ(at_limit[1], 20);

    auto past_limit = database->try_collect_bound_statement<i64>(select_all, 1uz, no_binds, read_v);
    EXPECT(past_limit.is_error());
    EXPECT_EQ(past_limit.error().string_literal(), "Statement returned more rows than the caller allowed"sv);

    auto mapper_failure = database->try_collect_bound_statement<i64>(select_all, 2uz, no_binds,
        [](Database::ResultRow&) -> ErrorOr<i64, Database::Error> { return Error::from_string_literal("mapper failed"); });
    EXPECT(mapper_failure.is_error());
    EXPECT_EQ(mapper_failure.error().string_literal(), "mapper failed"sv);

    auto again = TRY_OR_FAIL(database->try_collect_bound_statement<i64>(select_all, 2uz, no_binds, read_v));
    EXPECT_EQ(again.size(), 2uz);
}

TEST_CASE(collect_set_deduplicates_but_budget_counts_rows)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO t (id, v) VALUES (1, 5), (2, 5), (3, 7);"sv));

    auto select_all = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t;"sv));
    auto no_binds = [](auto&) -> ErrorOr<void, Database::Error> { return {}; };
    auto read_v = [](Database::ResultRow& row) -> ErrorOr<i64, Database::Error> {
        return row.read_integer<i64>("v"sv);
    };

    auto values = TRY_OR_FAIL(database->try_collect_bound_statement_set<i64>(select_all, 3uz, no_binds, read_v));
    EXPECT_EQ(values.size(), 2uz);
    EXPECT(values.contains(5));
    EXPECT(values.contains(7));

    auto over_budget = database->try_collect_bound_statement_set<i64>(select_all, 2uz, no_binds, read_v);
    EXPECT(over_budget.is_error());
    EXPECT_EQ(over_budget.error().string_literal(), "Statement returned more rows than the caller allowed"sv);
}

TEST_CASE(integral_binds_widen_to_64_bit)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"sv));

    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (id, v) VALUES (?, ?);"sv));
    auto select_by_id = TRY_OR_FAIL(database->prepare_statement("SELECT v FROM t WHERE id = ?;"sv));
    auto stored_value = [&](i64 id) {
        Optional<i64> value;
        database->execute_statement(select_by_id, [&](auto statement_id) -> ErrorOr<void, Database::Error> {
            value = database->result_column<i64>(statement_id, 0);
            return {}; }, id);
        return value;
    };

    TRY_OR_FAIL(database->try_execute_statement(insert, {}, static_cast<i64>(1), NumericLimits<u32>::max()));
    EXPECT_EQ(stored_value(1), Optional<i64> { 4294967295 });

    TRY_OR_FAIL(database->try_execute_statement(insert, {}, static_cast<i64>(2), NumericLimits<i64>::min()));
    EXPECT_EQ(stored_value(2), Optional<i64> { NumericLimits<i64>::min() });

    TRY_OR_FAIL(database->try_execute_statement(insert, {}, static_cast<i64>(3), NumericLimits<i64>::max()));
    EXPECT_EQ(stored_value(3), Optional<i64> { NumericLimits<i64>::max() });
}

TEST_CASE(constraint_violations_carry_their_extended_result_code)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT UNIQUE);"sv));
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (id, name) VALUES (:id, :name);"sv));

    auto insert_row = [&](i64 id, String const& name) {
        return database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
            TRY(bind("id"sv, id));
            TRY(bind("name"sv, name));
            return {};
        });
    };
    TRY_OR_FAIL(insert_row(1, "a"_string));

    auto duplicate_name = insert_row(2, "a"_string);
    EXPECT(duplicate_name.is_error());
    EXPECT(duplicate_name.error().has_result_code(Database::ResultCode::ConstraintUnique));

    auto duplicate_id = insert_row(1, "b"_string);
    EXPECT(duplicate_id.is_error());
    EXPECT(duplicate_id.error().has_result_code(Database::ResultCode::ConstraintPrimaryKey));
}

TEST_CASE(full_database_carries_its_result_code)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (v BLOB);"sv));
    TRY_OR_FAIL(database->execute_raw("PRAGMA max_page_count = 8;"sv));
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (:v);"sv));

    // Eight pages hold less than this at any page size.
    auto value = TRY_OR_FAIL(ByteBuffer::create_zeroed(MiB));

    auto result = database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        return bind("v"sv, value);
    });
    EXPECT(result.is_error());
    EXPECT(result.error().has_result_code(Database::ResultCode::Full));
}

TEST_CASE(value_over_the_length_limit_fails_at_the_bind)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE t (v BLOB);"sv));
    auto insert = TRY_OR_FAIL(database->prepare_statement("INSERT INTO t (v) VALUES (:v);"sv));

    // https://www.sqlite.org/limits.html
    // No build of SQLite takes a value this long. SQLite refuses it before reading it, so no page is touched.
    auto value = TRY_OR_FAIL(ByteBuffer::create_uninitialized(static_cast<size_t>(NumericLimits<i32>::max()) + 1));

    bool bind_failed = false;
    auto result = database->try_execute_bound_statement(insert, [&](auto& bind) -> ErrorOr<void, Database::Error> {
        auto bound = bind("v"sv, value);
        bind_failed = bound.is_error();
        return bound;
    });
    EXPECT(bind_failed);
    EXPECT(result.is_error());
    EXPECT(result.error().has_result_code(Database::ResultCode::TooBig));
}

TEST_CASE(failure_in_a_row_callback_carries_no_result_code)
{
    auto database = TRY_OR_FAIL(DB::create_memory_backed());
    auto select = TRY_OR_FAIL(database->prepare_statement("SELECT 1;"sv));

    auto result = database->try_execute_statement(select, [](auto) -> ErrorOr<void, Database::Error> {
        return Error::from_string_literal("deliberate failure");
    });
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "deliberate failure"sv);
    EXPECT(!result.error().result_code().has_value());
}

TEST_CASE(failed_open_reports_its_result_code_and_closes_its_connection)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-database-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());

    // A directory where the database file belongs makes the open fail.
    TRY_OR_FAIL(Core::Directory::create(ByteString::formatted("{}/Blocked.db", database_directory), Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    // SQLite counts the memory of every connection that is open.
    auto memory_in_use = sqlite3_memory_used();
    {
        auto database = TRY_OR_FAIL(DB::create_memory_backed());
        EXPECT(sqlite3_memory_used() > memory_in_use);
    }
    EXPECT_EQ(sqlite3_memory_used(), memory_in_use);

    auto blocked = DB::create(database_directory, "Blocked"sv);
    EXPECT(blocked.is_error());
    EXPECT_EQ(sqlite3_memory_used(), memory_in_use);

    // A file system may report an extended code that names the cause.
    auto primary_result_code = blocked.error().result_code().value_or(SQLITE_OK) & 0xff;
    EXPECT_EQ(primary_result_code, SQLITE_CANTOPEN);
}

// https://www.sqlite.org/vfs.html
// Counts the files that SQLite opens while it is alive, by standing in for SQLite's default VFS.
class OpenedFileCounter {
public:
    OpenedFileCounter()
        : m_default_vfs(sqlite3_vfs_find(nullptr))
        , m_counting_vfs(*m_default_vfs)
    {
        VERIFY(!s_the);
        s_the = this;

        m_counting_vfs.zName = "counting";
        m_counting_vfs.xOpen = [](sqlite3_vfs*, char const* name, sqlite3_file* file, int flags, int* out_flags) {
            ++s_the->m_count;
            return s_the->m_default_vfs->xOpen(s_the->m_default_vfs, name, file, flags, out_flags);
        };
        sqlite3_vfs_register(&m_counting_vfs, 1);
    }

    ~OpenedFileCounter()
    {
        sqlite3_vfs_register(m_default_vfs, 1);
        sqlite3_vfs_unregister(&m_counting_vfs);
        s_the = nullptr;
    }

    size_t count() const { return m_count; }

private:
    static inline OpenedFileCounter* s_the { nullptr };

    sqlite3_vfs* m_default_vfs { nullptr };
    sqlite3_vfs m_counting_vfs;
    size_t m_count { 0 };
};

// The rows outgrow SQLite's page caches, so each statement needs temporary storage beyond them.
static void run_statements_that_need_temporary_storage(DB& database)
{
    MUST(database.execute_raw(R"#(
        PRAGMA temp.cache_size = 10;
        CREATE TABLE t (id INTEGER PRIMARY KEY, k INTEGER, u INTEGER UNIQUE, v BLOB);
        WITH RECURSIVE numbers(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM numbers WHERE i < 20000)
        INSERT INTO t SELECT i, random(), i, randomblob(200) FROM numbers;

        SELECT count(*) FROM (SELECT v FROM t ORDER BY k, v);
        CREATE TEMP TABLE copy AS SELECT * FROM t;
        SELECT count(*) FROM (SELECT DISTINCT v FROM t);
        VACUUM;
    )#"sv));
}

TEST_CASE(memory_temp_store_keeps_temporary_files_off_disk)
{
    OpenedFileCounter opened_files;

    {
        auto database = TRY_OR_FAIL(DB::create_memory_backed({ .temp_store = DB::TempStore::Memory }));
        run_statements_that_need_temporary_storage(*database);

        // https://www.sqlite.org/tempfiles.html#statement_journal_files
        // The pragma does not cover the journal that undoes a statement which fails after it has changed rows.
        auto transaction = TRY_OR_FAIL(database->begin_transaction());
        TRY_OR_FAIL(database->execute_raw("UPDATE t SET k = k + 1; UPDATE t SET u = 119999 WHERE id = 20000;"sv));

        auto failed_update = database->execute_raw("UPDATE t SET u = u + 100000 WHERE id < 20000;"sv);
        EXPECT(failed_update.is_error());
        EXPECT(failed_update.error().has_result_code(Database::ResultCode::ConstraintUnique));

        i64 rows_as_before_the_update = 0;
        auto count_rows_as_before = TRY_OR_FAIL(database->prepare_statement("SELECT count(*) FROM t WHERE u = id;"sv));
        database->execute_statement(count_rows_as_before, [&](auto statement_id) -> ErrorOr<void, Database::Error> {
            rows_as_before_the_update = database->result_column<i64>(statement_id, 0);
            return {};
        });
        EXPECT_EQ(rows_as_before_the_update, 19999);
    }
    EXPECT_EQ(opened_files.count(), 0uz);

    {
        auto database = TRY_OR_FAIL(DB::create_memory_backed());
        TRY_OR_FAIL(database->execute_raw("PRAGMA temp_store = FILE;"sv));
        run_statements_that_need_temporary_storage(*database);
    }
    EXPECT(opened_files.count() > 0);
}
