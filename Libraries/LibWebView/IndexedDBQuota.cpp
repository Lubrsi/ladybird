/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibDatabase/ResultRow.h>
#include <LibWebView/IndexedDBQuota.h>

namespace WebView {

// https://www.sqlite.org/limits.html#max_page_count
static constexpr u64 LARGEST_PAGE_LIMIT = 4294967294;

// A vacuum takes memory for the pages that it moves until it is done, so free pages are given back a step at a time.
static constexpr u64 PAGES_OF_A_VACUUM_STEP = 1024;

ErrorOr<NonnullRefPtr<IndexedDBQuota>> IndexedDBQuota::create(u64 limit)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) IndexedDBQuota(limit));
}

IndexedDBQuota::IndexedDBQuota(u64 limit)
    : m_limit(limit)
{
}

bool IndexedDBQuota::try_change(u64 bytes_before, u64 bytes)
{
    if (bytes > bytes_before && bytes - bytes_before > room())
        return false;

    change(bytes_before, bytes);
    return true;
}

void IndexedDBQuota::change(u64 bytes_before, u64 bytes)
{
    VERIFY(m_taken >= bytes_before);
    m_taken = m_taken - bytes_before + bytes;
}

ErrorOr<NonnullRefPtr<IndexedDBCeiling>> IndexedDBCeiling::create(u64 limit)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) IndexedDBCeiling(limit));
}

IndexedDBCeiling::IndexedDBCeiling(u64 limit)
    : m_limit(limit)
{
}

void IndexedDBCeiling::change(u64 bytes_before, u64 bytes)
{
    VERIFY(m_held >= bytes_before);
    m_held = m_held - bytes_before + bytes;
}

void IndexedDBCeiling::give_back_free_pages()
{
    for (auto& database : m_databases)
        database.give_back_free_pages();
}

ErrorOr<NonnullOwnPtr<IndexedDBHeldPages>, Database::Error> IndexedDBHeldPages::create(NonnullRefPtr<Database::Database> database, NonnullRefPtr<IndexedDBCeiling> ceiling, u64 charge)
{
    auto get_pages = TRY(database->prepare_statement("SELECT page_count, page_size FROM pragma_page_count(), pragma_page_size();"sv));

    auto held_pages = TRY(adopt_nonnull_own_or_enomem(new (nothrow) IndexedDBHeldPages(move(database), move(ceiling), charge, get_pages)));
    TRY(held_pages->count());
    return held_pages;
}

IndexedDBHeldPages::IndexedDBHeldPages(NonnullRefPtr<Database::Database> database, NonnullRefPtr<IndexedDBCeiling> ceiling, u64 charge, Database::StatementID get_pages)
    : m_database(move(database))
    , m_ceiling(move(ceiling))
    , m_charge(charge)
    , m_get_pages(get_pages)
{
    m_ceiling->m_databases.append(*this);
}

IndexedDBHeldPages::~IndexedDBHeldPages()
{
    m_ceiling->m_databases.remove(*this);
    m_ceiling->change(m_bytes_held, 0);
}

ErrorOr<void, Database::Error> IndexedDBHeldPages::count()
{
    struct Pages {
        u64 count { 0 };
        u64 size { 0 };
    };

    auto pages = TRY(m_database->try_execute_bound_statement_one<Pages>(
        m_get_pages,
        [](auto&) -> ErrorOr<void, Database::Error> { return {}; },
        [](Database::ResultRow& row) -> ErrorOr<Pages, Database::Error> {
            return Pages {
                .count = TRY(row.read_integer<u64>("page_count"sv)),
                .size = TRY(row.read_integer<u64>("page_size"sv)),
            };
        }));

    // A savepoint that is rolled back takes the database back to the pages that it had, and SQLite keeps the memory
    // of the others until the transaction ends.
    if (m_transaction_is_open)
        pages.count = max(pages.count, m_page_count);

    auto bytes = m_charge + pages.count * pages.size;
    m_ceiling->change(m_bytes_held, bytes);

    m_page_count = pages.count;
    m_page_size = pages.size;
    m_bytes_held = bytes;
    return {};
}

ErrorOr<void, Database::Error> IndexedDBHeldPages::transaction_did_end()
{
    m_transaction_is_open = false;
    return count();
}

// https://www.sqlite.org/pragma.html#pragma_max_page_count
ErrorOr<void, Database::Error> IndexedDBHeldPages::allow_growth_into_the_room()
{
    auto page_limit = min(m_page_count + m_ceiling->room() / m_page_size, LARGEST_PAGE_LIMIT);

    // SQLite takes a limit of 0 as no change to the limit that it has. A database that has no page yet, and no room
    // for one, is full.
    if (page_limit == 0)
        return Database::Error::from_result_code(to_underlying(Database::ResultCode::Full));

    if (m_page_limit == page_limit)
        return {};

    TRY(m_database->execute_raw(ByteString::formatted("PRAGMA max_page_count = {};", page_limit)));
    m_page_limit = page_limit;
    return {};
}

// https://www.sqlite.org/pragma.html#pragma_incremental_vacuum
void IndexedDBHeldPages::give_back_free_pages()
{
    // A vacuum inside a transaction frees no memory before the transaction ends, and is undone if that is rolled
    // back.
    if (m_transaction_is_open)
        return;

    auto vacuum_a_step = ByteString::formatted("PRAGMA incremental_vacuum({});", PAGES_OF_A_VACUUM_STEP);
    bool gave_pages_back = false;
    while (true) {
        auto page_count_before = m_page_count;
        if (m_database->execute_raw(vacuum_a_step).is_error() || count().is_error())
            break;

        bool step_gave_pages_back = m_page_count < page_count_before;
        if (!step_gave_pages_back)
            break;
        gave_pages_back = true;
    }

    if (gave_pages_back && on_free_pages_given_back)
        on_free_pages_given_back();
}

}
