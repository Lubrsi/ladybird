/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/IntrusiveList.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <LibDatabase/Database.h>

namespace WebView {

class IndexedDBCeiling;

// A number of bytes that some databases may take together, and how many of them the databases have taken.
class IndexedDBQuota : public RefCounted<IndexedDBQuota> {
public:
    static ErrorOr<NonnullRefPtr<IndexedDBQuota>> create(u64 limit);

    u64 limit() const { return m_limit; }
    u64 room() const { return m_taken < m_limit ? m_limit - m_taken : 0; }
    bool has_nothing_taken() const { return m_taken == 0; }

    // A database takes a number of bytes in place of the number that it took before. More than before is refused
    // when there is no room for it.
    [[nodiscard]] bool try_change(u64 bytes_before, u64 bytes);
    void change(u64 bytes_before, u64 bytes);

private:
    explicit IndexedDBQuota(u64 limit);

    u64 m_limit { 0 };
    u64 m_taken { 0 };
};

// The pages that one SQLite database holds, its free pages too, as a part of what the databases of its session may
// hold together. SQLite keeps the database from growing past the room that the session has left.
class IndexedDBHeldPages {
    AK_MAKE_NONCOPYABLE(IndexedDBHeldPages);
    AK_MAKE_NONMOVABLE(IndexedDBHeldPages);

public:
    AK_ALLOC_WITH_KMALLOC;

    // The charge is for what the database takes besides its pages.
    static ErrorOr<NonnullOwnPtr<IndexedDBHeldPages>, Database::Error> create(NonnullRefPtr<Database::Database>, NonnullRefPtr<IndexedDBCeiling>, u64 charge);
    ~IndexedDBHeldPages();

    ErrorOr<void, Database::Error> count();
    ErrorOr<void, Database::Error> allow_growth_into_the_room();

    void transaction_did_begin() { m_transaction_is_open = true; }
    ErrorOr<void, Database::Error> transaction_did_end();

    void give_back_free_pages();
    Function<void()> on_free_pages_given_back;

private:
    IndexedDBHeldPages(NonnullRefPtr<Database::Database>, NonnullRefPtr<IndexedDBCeiling>, u64 charge, Database::StatementID get_pages);

    NonnullRefPtr<Database::Database> m_database;
    NonnullRefPtr<IndexedDBCeiling> m_ceiling;
    u64 m_charge { 0 };
    Database::StatementID m_get_pages { 0 };

    bool m_transaction_is_open { false };
    u64 m_page_count { 0 };
    u64 m_page_size { 0 };
    u64 m_bytes_held { 0 };
    Optional<u64> m_page_limit;

    IntrusiveListNode<IndexedDBHeldPages> m_list_node;

public:
    using List = IntrusiveList<&IndexedDBHeldPages::m_list_node>;
};

// What the databases of one session may hold together in memory, counted as the bytes of their pages and a charge for
// each database, and how much of it they hold.
class IndexedDBCeiling : public RefCounted<IndexedDBCeiling> {
public:
    static ErrorOr<NonnullRefPtr<IndexedDBCeiling>> create(u64 limit);

    u64 room() const { return m_held < m_limit ? m_limit - m_held : 0; }

    // The databases that have no transaction open give back their free pages.
    void give_back_free_pages();

private:
    friend class IndexedDBHeldPages;

    explicit IndexedDBCeiling(u64 limit);

    void change(u64 bytes_before, u64 bytes);

    u64 m_limit { 0 };
    u64 m_held { 0 };
    IndexedDBHeldPages::List m_databases;
};

}
