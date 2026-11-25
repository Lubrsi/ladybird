/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/IndexedDB/KeyEncoding.h>
#include <LibWebView/IndexedDBSession.h>
#include <sqlite3.h>

using namespace WebView::IndexedDBMessages;

using WebView::IndexedDBDatabase;
using WebView::IndexedDBStore;

using Kind = WebView::IndexedDBErrorKind;
using Mode = WebView::IndexedDBTransactionScheduler::Mode;
using Session = WebView::IndexedDBSession;

// The ids that a database gives its first two object stores.
static constexpr i64 BOOKS = 1;
static constexpr i64 NOTES = 2;

static UnixDateTime at(i64 seconds)
{
    return UnixDateTime::from_seconds_since_epoch(seconds);
}

static String example_com()
{
    return "https://example.com"_string;
}

// The key of a whole number from 1 to 2^53.
static ByteBuffer number_key(u64 number)
{
    return MUST(ByteBuffer::copy(Web::IndexedDB::encoded_generated_key(number).release_value().span()));
}

// A binary key of that many bytes.
static ByteBuffer binary_key_of_length(size_t length)
{
    auto key = MUST(ByteBuffer::create_uninitialized(length + 2));
    key.bytes().fill(0x61);
    key[0] = 0x40;
    key[key.size() - 1] = 0x00;
    return key;
}

static ByteBuffer buffer_of(StringView text)
{
    return MUST(ByteBuffer::copy(text.bytes()));
}

template<typename T, typename Describe>
static String list_of(Vector<T> const& items, Describe describe)
{
    StringBuilder list;
    list.append('[');
    for (auto const& item : items) {
        if (list.length() > 1)
            list.append(", "sv);
        list.append(describe(item));
    }
    list.append(']');
    return MUST(list.to_string());
}

static String described(TransactionName const& transaction)
{
    return transaction.visit(
        [](TransactionNumber number) { return MUST(String::formatted("transaction {}", number.value())); },
        [](UpgradeOf const& upgrade) { return MUST(String::formatted("the upgrade of connection {}", upgrade.connection.value())); });
}

// The object stores as "books=1", with their indexes where they have any: "books=1 with the indexes [by_title=1]".
static String described(Vector<WebView::IndexedDBObjectStoreDescription> const& object_stores)
{
    return list_of(object_stores, [](auto const& object_store) {
        if (object_store.indexes.is_empty())
            return MUST(String::formatted("{}={}", object_store.name, object_store.id));

        auto indexes = list_of(object_store.indexes, [](auto const& index) { return MUST(String::formatted("{}={}", index.name, index.id)); });
        return MUST(String::formatted("{}={} with the indexes {}", object_store.name, object_store.id, indexes));
    });
}

static String described(Optional<u64> version)
{
    if (!version.has_value())
        return "no version"_string;
    return MUST(String::formatted("version {}", *version));
}

// A record whose key is a whole number and whose value is text, as "1=one", or as "1" without its value.
static String described(WebView::IndexedDBRecord const& record)
{
    auto number = Web::IndexedDB::number_of_key(record.key);
    auto key = number.has_value() ? MUST(String::formatted("{}", static_cast<u64>(*number))) : MUST(String::formatted("a key of {} bytes", record.key.size()));
    if (!record.value.has_value())
        return key;
    return MUST(String::formatted("{}={}", key, StringView { record.value->bytes() }));
}

// A record of an index whose keys are whole numbers, as "7/1=one": the key in the index, the key of the record that
// it refers to, and that record's value where it was asked for.
static String described(WebView::IndexedDBIndexRecord const& record)
{
    auto key = static_cast<u64>(Web::IndexedDB::number_of_key(record.key).release_value());
    auto primary_key = static_cast<u64>(Web::IndexedDB::number_of_key(record.primary_key).release_value());
    if (!record.value.has_value())
        return MUST(String::formatted("{}/{}", key, primary_key));
    return MUST(String::formatted("{}/{}={}", key, primary_key, StringView { record.value->bytes() }));
}

static String described(ResultOfRequest const& result)
{
    return result.visit(
        [](RecordStored const& stored) {
            if (!stored.generated_key.has_value())
                return "stored"_string;
            return MUST(String::formatted("stored with the generated key {}", *stored.generated_key));
        },
        [](RecordsDeleted const&) { return "deleted"_string; },
        [](RecordsRead const& read) {
            return MUST(String::formatted("read {}", list_of(read.records, [](auto const& record) { return described(record); })));
        },
        [](RecordsCounted const& counted) { return MUST(String::formatted("counted {}", counted.count)); },
        [](CursorIterated const& iterated) {
            if (!iterated.record.has_value())
                return "the cursor is at the end"_string;
            return MUST(String::formatted("the cursor is at {}", described(*iterated.record)));
        },
        [](IndexRecordsRead const& read) {
            return MUST(String::formatted("read {}", list_of(read.records, [](auto const& record) { return described(record); })));
        },
        [](IndexCursorIterated const& iterated) {
            if (!iterated.record.has_value())
                return "the cursor is at the end"_string;
            return MUST(String::formatted("the cursor is at {}", described(*iterated.record)));
        },
        [](Kind kind) { return MUST(String::formatted("failed for {}", kind)); });
}

static String described(Reply const& reply)
{
    return reply.visit(
        [](OpenSucceeded const& open) {
            return MUST(String::formatted("open {} has connection {} at version {} with object stores {}", open.request.value(), open.connection.value(), open.version, described(open.object_stores)));
        },
        [](UpgradeNeeded const& upgrade) {
            return MUST(String::formatted("open {} needs an upgrade of connection {} from version {} to {} with object stores {}", upgrade.request.value(), upgrade.connection.value(), upgrade.old_version, upgrade.version, described(upgrade.object_stores)));
        },
        [](OpenSucceededAfterUpgrade const& open) { return MUST(String::formatted("open {} succeeded after its upgrade", open.request.value())); },
        [](OpenFailed const& open) {
            return open.reason.visit(
                [&](Kind kind) { return MUST(String::formatted("open {} failed for {}", open.request.value(), kind)); },
                [&](OpenFailed::UpgradeDidNotLeaveAConnection const&) {
                    return MUST(String::formatted("open {} failed since its upgrade left it no connection", open.request.value()));
                });
        },
        [](VersionChange const& change) {
            return MUST(String::formatted("connection {} hears of a change from version {} to {} (notification {})", change.connection.value(), change.old_version, described(change.version), change.notification));
        },
        [](Blocked const& blocked) {
            return MUST(String::formatted("request {} is blocked from version {} to {}", blocked.request.value(), blocked.old_version, described(blocked.version)));
        },
        [](DeleteSucceeded const& deleted) { return MUST(String::formatted("delete {} succeeded from version {}", deleted.request.value(), deleted.old_version)); },
        [](DeleteFailed const& failed) { return MUST(String::formatted("delete {} failed for {}", failed.request.value(), failed.kind)); },
        [](DatabaseList const& list) {
            return list.result.visit(
                [&](Vector<IndexedDBStore::DatabaseInfo> const& databases) {
                    auto names = list_of(databases, [](auto const& database) { return MUST(String::formatted("{} at version {}", database.name, database.version)); });
                    return MUST(String::formatted("list {} has {}", list.request.value(), names));
                },
                [&](Kind kind) { return MUST(String::formatted("list {} failed for {}", list.request.value(), kind)); });
        },
        [](RequestResult const& result) {
            return MUST(String::formatted("result {} of {}: {}", result.request, described(result.transaction), described(result.result)));
        },
        [](ExtractIndexKeys const& extract) {
            return MUST(String::formatted("the upgrade of connection {} is asked for the keys of its index {} for the value {} (token {})", extract.upgrade.value(), extract.index, StringView { extract.value.bytes() }, extract.token));
        },
        [](TransactionCompleted const& completed) {
            auto ids = [](Vector<i64> const& ids) { return list_of(ids, [](i64 id) { return String::number(id); }); };

            StringBuilder text;
            text.appendff("{} completed", described(completed.transaction));
            if (!completed.created_object_stores.is_empty())
                text.appendff(", having created object stores {}", ids(completed.created_object_stores));
            if (!completed.created_indexes.is_empty())
                text.appendff(", having created indexes {}", ids(completed.created_indexes));
            return MUST(text.to_string());
        },
        [](TransactionAborted const& aborted) {
            if (!aborted.cause.has_value())
                return MUST(String::formatted("{} aborted", described(aborted.transaction)));
            return MUST(String::formatted("{} aborted for {}", described(aborted.transaction), *aborted.cause));
        });
}

// One channel of a renderer, and the agent that hears the replies to it.
struct Page {
    void send(Command command) { session.handle(channel, move(command)); }

    // What the agent has heard since it was last asked, as text. A reply to another of its channels says which.
    Vector<String> has_heard()
    {
        Vector<String> replies;
        for (auto const& reply : session.take_replies(agent)) {
            if (reply.channel == channel)
                replies.append(described(reply.reply));
            else
                replies.append(MUST(String::formatted("on channel {}: {}", reply.channel.value(), described(reply.reply))));
        }
        return replies;
    }

    Session& session;
    Session::AgentID agent;
    Session::ChannelID channel;
};

static NonnullOwnPtr<Session> session_over(NonnullOwnPtr<IndexedDBStore> store)
{
    return make<Session>(move(store), [] { return at(1); });
}

static NonnullOwnPtr<Session> a_session()
{
    return session_over(MUST(IndexedDBStore::create_transient()));
}

static Page a_page(Session& session, String storage_key = example_com())
{
    auto agent = session.add_agent();
    return { session, agent, session.add_channel(agent, move(storage_key)) };
}

static Page another_channel_of(Page const& page)
{
    return { page.session, page.agent, page.session.add_channel(page.agent, example_com()) };
}

static ConnectionNumber connection(u64 number)
{
    return ConnectionNumber { number };
}

static TransactionName transaction(u64 number)
{
    return TransactionNumber { number };
}

static TransactionName upgrade_of(u64 connection)
{
    return UpgradeOf { ConnectionNumber { connection } };
}

static PlaceRequest put(TransactionName transaction, ObjectStoreName object_store, u64 key, StringView value)
{
    return { move(transaction), StoreRecord { .object_store = move(object_store), .key = number_key(key), .value = buffer_of(value) } };
}

static PlaceRequest add(TransactionName transaction, ObjectStoreName object_store, u64 key, StringView value)
{
    return { move(transaction), StoreRecord { .object_store = move(object_store), .key = number_key(key), .value = buffer_of(value), .no_overwrite = IndexedDBDatabase::NoOverwrite::Yes } };
}

static PlaceRequest put_with_the_key(TransactionName transaction, ObjectStoreName object_store, ByteBuffer key)
{
    return { move(transaction), StoreRecord { .object_store = move(object_store), .key = move(key), .value = buffer_of("value"sv) } };
}

static PlaceRequest read_all(TransactionName transaction, ObjectStoreName object_store)
{
    return { move(transaction), ReadRecords { .object_store = move(object_store), .range = {} } };
}

static PlaceRequest count(TransactionName transaction, ObjectStoreName object_store)
{
    return { move(transaction), Count { .object_store = move(object_store), .range = {} } };
}

static CreateTransaction a_transaction(Mode mode, Vector<i64> scope, u64 connection_number = 1)
{
    return { .connection = connection(connection_number), .mode = mode, .scope = move(scope) };
}

static CreateIndex an_index(u64 upgrade, ObjectStoreName object_store, IndexedDBDatabase::Unique unique = IndexedDBDatabase::Unique::No)
{
    return { .upgrade = connection(upgrade), .object_store = move(object_store), .name = "by_number"_utf16, .key_path = WebView::IndexedDBKeyPath { "number"_utf16 }, .unique = unique };
}

// The answer to an extraction: the keys that the renderer found in the value, which are whole numbers here.
static IndexKeys keys_for_index(u64 upgrade, u64 token, Vector<u64> const& keys)
{
    Vector<ByteBuffer> encoded_keys;
    for (auto key : keys)
        encoded_keys.append(number_key(key));
    return { .upgrade = connection(upgrade), .token = token, .keys = move(encoded_keys) };
}

static PlaceRequest put_with_an_index_key(TransactionName transaction, ObjectStoreName object_store, u64 key, StringView value, IndexName index, u64 index_key)
{
    Vector<KeysOfIndex> index_keys;
    index_keys.append({ move(index), { number_key(index_key) } });
    return { move(transaction), StoreRecord { .object_store = move(object_store), .key = number_key(key), .value = buffer_of(value), .index_keys = move(index_keys) } };
}

static PlaceRequest read_index(TransactionName transaction, IndexName index)
{
    return { move(transaction), ReadIndexRecords { .index = move(index), .range = {} } };
}

// Has the page open the database "database" at version 1 and create the object stores "books" and "notes" in the
// upgrade. The page is left with its first connection.
static void open_a_database(Page& page)
{
    page.send(Open { .name = "database"_utf16, .version = 1 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "notes"_utf16 });
    page.send(Commit { .transaction = upgrade_of(1), .results_started = 0 });

    EXPECT_EQ(page.has_heard(), (Vector {
                                    "open 1 needs an upgrade of connection 1 from version 0 to 1 with object stores []"_string,
                                    "the upgrade of connection 1 completed, having created object stores [1, 2]"_string,
                                    "open 1 succeeded after its upgrade"_string,
                                }));
}

// https://www.sqlite.org/c3ref/hard_heap_limit64.html
// Runs the callback with SQLite unable to allocate more than this on top of what it has. The limit is the process's,
// and setting the hard one sets the soft one too, so both are put back, the hard one first.
template<typename Callback>
static auto with_room_for_sqlite(i64 bytes, Callback callback)
{
    auto hard_limit = sqlite3_hard_heap_limit64(-1);
    auto soft_limit = sqlite3_soft_heap_limit64(-1);
    ScopeGuard restore_the_limits = [&] {
        sqlite3_hard_heap_limit64(hard_limit);
        sqlite3_soft_heap_limit64(soft_limit);
    };

    sqlite3_hard_heap_limit64(sqlite3_memory_used() + bytes);
    return callback();
}

// Enough for statements over small records, and not for a cell of a mebibyte or more.
template<typename Callback>
static auto with_little_memory_for_sqlite(Callback callback)
{
    return with_room_for_sqlite(128 * KiB, move(callback));
}

TEST_CASE(an_open_of_a_new_database_upgrades_it)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16, .version = 2 });
    EXPECT_EQ(page.has_heard(), (Vector { "open 1 needs an upgrade of connection 1 from version 0 to 2 with object stores []"_string }));

    // What the upgrade creates can be used at once, by its number.
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(put(upgrade_of(1), ObjectStoreOfUpgrade { 1 }, 1, "one"sv));
    page.send(read_all(upgrade_of(1), ObjectStoreOfUpgrade { 1 }));
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "result 1 of the upgrade of connection 1: stored"_string,
                                    "result 2 of the upgrade of connection 1: read [1=one]"_string,
                                }));

    page.send(Commit { .transaction = upgrade_of(1), .results_started = 2 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "the upgrade of connection 1 completed, having created object stores [1]"_string,
                                    "open 1 succeeded after its upgrade"_string,
                                }));

    // From then on it has the id that the page was told.
    page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    page.send(count(transaction(1), BOOKS));
    EXPECT_EQ(page.has_heard(), (Vector { "result 1 of transaction 1: counted 1"_string }));
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});
}

TEST_CASE(an_open_follows_the_version_of_its_database)
{
    auto session = a_session();
    auto first = a_page(*session);
    open_a_database(first);

    // An open that needs no upgrade is told what the database has.
    auto second = a_page(*session);
    second.send(Open { .name = "database"_utf16 });
    second.send(Open { .name = "database"_utf16, .version = 1 });
    EXPECT_EQ(second.has_heard(), (Vector {
                                      "open 1 has connection 1 at version 1 with object stores [books=1, notes=2]"_string,
                                      "open 2 has connection 2 at version 1 with object stores [books=1, notes=2]"_string,
                                  }));

    second.send(Open { .name = "newer"_utf16, .version = 5 });
    second.send(Commit { .transaction = upgrade_of(3), .results_started = 0 });
    second.send(Open { .name = "newer"_utf16, .version = 4 });
    EXPECT_EQ(second.has_heard(), (Vector {
                                      "open 3 needs an upgrade of connection 3 from version 0 to 5 with object stores []"_string,
                                      "the upgrade of connection 3 completed"_string,
                                      "open 3 succeeded after its upgrade"_string,
                                      "open 4 failed for Version"_string,
                                  }));
}

TEST_CASE(a_storage_key_has_databases_of_its_own)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    auto page_of_another_site = a_page(*session, "https://example.org"_string);
    page_of_another_site.send(ListDatabases {});
    page_of_another_site.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(page_of_another_site.has_heard(), (Vector {
                                                    "list 1 has []"_string,
                                                    "open 2 needs an upgrade of connection 1 from version 0 to 1 with object stores []"_string,
                                                }));

    // A database is listed from when its first upgrade is committed.
    page.send(ListDatabases {});
    EXPECT_EQ(page.has_heard(), (Vector { "list 2 has [database at version 1]"_string }));
}

TEST_CASE(an_upgrade_waits_until_the_other_connections_are_closed)
{
    auto session = a_session();
    auto first = a_page(*session);
    open_a_database(first);

    auto second = a_page(*session);
    second.send(Open { .name = "database"_utf16, .version = 2 });
    EXPECT_EQ(first.has_heard(), (Vector { "connection 1 hears of a change from version 1 to version 2 (notification 1)"_string }));
    EXPECT_EQ(second.has_heard(), Vector<String> {});

    first.send(VersionChangeHandled { .connection = connection(1), .notification = 1 });
    EXPECT_EQ(second.has_heard(), (Vector { "request 1 is blocked from version 1 to version 2"_string }));

    first.send(CloseConnection { .connection = connection(1) });
    EXPECT_EQ(second.has_heard(), (Vector { "open 1 needs an upgrade of connection 1 from version 1 to 2 with object stores [books=1, notes=2]"_string }));

    // What still names the connection that has closed is of something that has ended, and no fault.
    first.send(VersionChangeHandled { .connection = connection(1), .notification = 1 });
    first.send(CloseConnection { .connection = connection(1) });
    first.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    first.send(count(transaction(1), BOOKS));
    EXPECT_EQ(first.has_heard(), Vector<String> {});
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});
}

TEST_CASE(a_delete_waits_until_every_connection_is_closed)
{
    auto session = a_session();
    auto first = a_page(*session);
    open_a_database(first);

    auto second = a_page(*session);
    second.send(DeleteDatabase { .name = "database"_utf16 });
    EXPECT_EQ(first.has_heard(), (Vector { "connection 1 hears of a change from version 1 to no version (notification 1)"_string }));
    EXPECT_EQ(second.has_heard(), Vector<String> {});

    // A connection that closes in place of an answer is not waited for, and nothing is blocked.
    first.send(CloseConnection { .connection = connection(1) });
    EXPECT_EQ(second.has_heard(), (Vector { "delete 1 succeeded from version 1"_string }));

    second.send(ListDatabases {});
    second.send(DeleteDatabase { .name = "database"_utf16 });
    EXPECT_EQ(second.has_heard(), (Vector {
                                      "list 2 has []"_string,
                                      "delete 3 succeeded from version 0"_string,
                                  }));
}

TEST_CASE(the_requests_of_a_channel_are_answered_by_their_numbers)
{
    auto session = a_session();
    auto first = a_page(*session);
    open_a_database(first);

    // The first open waits for a connection to close. The requests after it are for other queues, and do not wait.
    auto second = a_page(*session);
    second.send(Open { .name = "database"_utf16, .version = 2 });
    second.send(Open { .name = "other"_utf16 });
    second.send(ListDatabases {});
    EXPECT_EQ(second.has_heard(), (Vector {
                                      "open 2 needs an upgrade of connection 1 from version 0 to 1 with object stores []"_string,
                                      "list 3 has [database at version 1]"_string,
                                  }));

    first.send(CloseConnection { .connection = connection(1) });
    EXPECT_EQ(second.has_heard(), (Vector { "open 1 needs an upgrade of connection 2 from version 1 to 2 with object stores [books=1, notes=2]"_string }));
}

TEST_CASE(a_transaction_waits_for_the_earlier_ones_that_it_cannot_run_beside)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    // The second waits for the first. The third only reads, and waits for the second although that has not started.
    // The fourth has other object stores.
    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    page.send(a_transaction(Mode::ReadOnly, { NOTES }));

    page.send(put(transaction(2), BOOKS, 1, "one"sv));
    page.send(count(transaction(3), BOOKS));
    page.send(count(transaction(4), NOTES));
    EXPECT_EQ(page.has_heard(), (Vector { "result 1 of transaction 4: counted 0"_string }));

    page.send(Commit { .transaction = transaction(1), .results_started = 0 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "transaction 1 completed"_string,
                                    "result 1 of transaction 2: stored"_string,
                                }));

    page.send(Commit { .transaction = transaction(2), .results_started = 1 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "transaction 2 completed"_string,
                                    "result 1 of transaction 3: counted 1"_string,
                                }));
}

TEST_CASE(an_agent_hears_of_things_in_the_order_in_which_they_happened)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    auto other_channel = another_channel_of(page);
    other_channel.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector { "on channel 2: open 1 has connection 1 at version 1 with object stores [books=1, notes=2]"_string }));

    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(put(transaction(1), BOOKS, 1, "one"sv));
    other_channel.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    other_channel.send(read_all(transaction(1), BOOKS));
    EXPECT_EQ(page.has_heard(), (Vector { "result 1 of transaction 1: stored"_string }));

    // What one channel waited for is heard of before what came of the wait.
    page.send(Commit { .transaction = transaction(1), .results_started = 1 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "transaction 1 completed"_string,
                                    "on channel 2: result 1 of transaction 1: read [1=one]"_string,
                                }));
}

TEST_CASE(the_requests_of_a_transaction_are_carried_out_in_order)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "numbered"_utf16, .key_generator = IndexedDBDatabase::KeyGenerator::Yes });
    page.send(Commit { .transaction = upgrade_of(1), .results_started = 0 });
    (void)page.has_heard();

    page.send(a_transaction(Mode::ReadWrite, { BOOKS, NOTES }));
    page.send(put(transaction(1), BOOKS, 1, "one"sv));
    page.send(put(transaction(1), BOOKS, 2, "two"sv));
    page.send(put(transaction(1), BOOKS, 3, "three"sv));
    page.send(add(transaction(1), BOOKS, 3, "another three"sv));
    page.send(PlaceRequest { transaction(1), DeleteRecords { .object_store = BOOKS, .range = { .lower_bound = number_key(2), .upper_bound = number_key(2) } } });
    page.send(PlaceRequest { transaction(1), ReadRecords { .object_store = BOOKS, .range = {}, .direction = IndexedDBDatabase::Direction::Descending, .with_values = IndexedDBDatabase::WithValues::No } });
    page.send(PlaceRequest { transaction(1), IterateCursor { .object_store = BOOKS, .range = {} } });
    page.send(PlaceRequest { transaction(1), IterateCursor { .object_store = BOOKS, .range = {}, .position = number_key(1) } });
    page.send(PlaceRequest { transaction(1), IterateCursor { .object_store = BOOKS, .range = {}, .position = number_key(3) } });
    page.send(PlaceRequest { transaction(1), Clear { .object_store = BOOKS } });
    page.send(count(transaction(1), BOOKS));
    page.send(PlaceRequest { transaction(1), StoreRecord { .object_store = NOTES, .value = buffer_of("value"sv) } });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "result 1 of transaction 1: stored"_string,
                                    "result 2 of transaction 1: stored"_string,
                                    "result 3 of transaction 1: stored"_string,
                                    "result 4 of transaction 1: failed for Constraint"_string,
                                    "result 5 of transaction 1: deleted"_string,
                                    "result 6 of transaction 1: read [3, 1]"_string,
                                    "result 7 of transaction 1: the cursor is at 1=one"_string,
                                    "result 8 of transaction 1: the cursor is at 3=three"_string,
                                    "result 9 of transaction 1: the cursor is at the end"_string,
                                    "result 10 of transaction 1: deleted"_string,
                                    "result 11 of transaction 1: counted 0"_string,
                                    "result 12 of transaction 1: stored with the generated key 1"_string,
                                }));
}

TEST_CASE(a_commit_forgives_the_failures_that_its_renderer_had_started_on)
{
    // What a page hears of a transaction that puts one record, fails to add another, and is asked to commit, and
    // then what the object store has.
    auto commit_after_a_failed_add = [](u64 results_started) {
        auto session = a_session();
        auto page = a_page(*session);
        open_a_database(page);

        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(put(transaction(1), BOOKS, 1, "one"sv));
        page.send(Commit { .transaction = transaction(1), .results_started = 1 });
        (void)page.has_heard();

        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(put(transaction(2), BOOKS, 2, "two"sv));
        page.send(add(transaction(2), BOOKS, 1, "unreached"sv));
        EXPECT_EQ(page.has_heard(), (Vector {
                                        "result 1 of transaction 2: stored"_string,
                                        "result 2 of transaction 2: failed for Constraint"_string,
                                    }));

        page.send(Commit { .transaction = transaction(2), .results_started = results_started });
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
        page.send(read_all(transaction(3), BOOKS));
        return page.has_heard();
    };

    // The page called commit() before it had the result of the add, so the add failed in a committing transaction.
    EXPECT_EQ(commit_after_a_failed_add(0), (Vector {
                                                "transaction 2 aborted for Constraint"_string,
                                                "result 1 of transaction 3: read [1=one]"_string,
                                            }));
    EXPECT_EQ(commit_after_a_failed_add(1), (Vector {
                                                "transaction 2 aborted for Constraint"_string,
                                                "result 1 of transaction 3: read [1=one]"_string,
                                            }));

    // The page called commit() in the error handler of the add, which had prevented the abort.
    EXPECT_EQ(commit_after_a_failed_add(2), (Vector {
                                                "transaction 2 completed"_string,
                                                "result 1 of transaction 3: read [1=one, 2=two]"_string,
                                            }));
}

TEST_CASE(a_transaction_that_has_ended_takes_nothing_more)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(put(transaction(1), BOOKS, 1, "one"sv));
    page.send(Abort { .transaction = transaction(1) });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "result 1 of transaction 1: stored"_string,
                                    "transaction 1 aborted"_string,
                                }));

    // What was on its way when the transaction ended is no fault of the renderer's.
    page.send(put(transaction(1), BOOKS, 2, "two"sv));
    page.send(Commit { .transaction = transaction(1), .results_started = 2 });
    page.send(Abort { .transaction = transaction(1) });
    EXPECT_EQ(page.has_heard(), Vector<String> {});
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});

    page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    page.send(count(transaction(2), BOOKS));
    EXPECT_EQ(page.has_heard(), (Vector { "result 1 of transaction 2: counted 0"_string }));
}

TEST_CASE(a_connection_that_is_asked_to_close_keeps_its_transactions)
{
    auto session = a_session();
    auto first = a_page(*session);
    open_a_database(first);

    first.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    first.send(put(transaction(1), BOOKS, 1, "one"sv));
    (void)first.has_heard();

    auto second = a_page(*session);
    second.send(Open { .name = "database"_utf16, .version = 2 });
    first.send(VersionChangeHandled { .connection = connection(1), .notification = 1 });
    first.send(CloseConnection { .connection = connection(1) });
    EXPECT_EQ(second.has_heard(), (Vector { "request 1 is blocked from version 1 to version 2"_string }));
    (void)first.has_heard();

    first.send(put(transaction(1), BOOKS, 2, "two"sv));
    first.send(Commit { .transaction = transaction(1), .results_started = 2 });
    EXPECT_EQ(first.has_heard(), (Vector {
                                     "result 2 of transaction 1: stored"_string,
                                     "transaction 1 completed"_string,
                                 }));
    EXPECT_EQ(second.has_heard(), (Vector { "open 1 needs an upgrade of connection 1 from version 1 to 2 with object stores [books=1, notes=2]"_string }));

    // The connection is closed, so a transaction of it is one that has ended.
    first.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    first.send(count(transaction(2), BOOKS));
    EXPECT_EQ(first.has_heard(), Vector<String> {});
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});
}

TEST_CASE(an_upgrade_that_is_aborted_leaves_the_database_as_it_was)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    page.send(CloseConnection { .connection = connection(1) });
    page.send(Open { .name = "database"_utf16, .version = 2 });
    page.send(CreateObjectStore { .upgrade = connection(2), .name = "more"_utf16 });
    page.send(DeleteObjectStore { .upgrade = connection(2), .object_store = BOOKS });
    page.send(RenameObjectStore { .upgrade = connection(2), .object_store = NOTES, .name = "renamed"_utf16 });
    page.send(put(upgrade_of(2), ObjectStoreOfUpgrade { 1 }, 1, "one"sv));
    page.send(Abort { .transaction = upgrade_of(2) });

    // The page hears of the end of the transaction before it hears what came of the open.
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "open 2 needs an upgrade of connection 2 from version 1 to 2 with object stores [books=1, notes=2]"_string,
                                    "result 1 of the upgrade of connection 2: stored"_string,
                                    "the upgrade of connection 2 aborted"_string,
                                    "open 2 failed since its upgrade left it no connection"_string,
                                }));

    page.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector { "open 3 has connection 3 at version 1 with object stores [books=1, notes=2]"_string }));
}

TEST_CASE(an_upgrade_changes_the_object_stores)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    page.send(CloseConnection { .connection = connection(1) });
    page.send(Open { .name = "database"_utf16, .version = 2 });
    page.send(CreateObjectStore { .upgrade = connection(2), .name = "more"_utf16 });
    page.send(RenameObjectStore { .upgrade = connection(2), .object_store = ObjectStoreOfUpgrade { 1 }, .name = "most"_utf16 });
    page.send(DeleteObjectStore { .upgrade = connection(2), .object_store = BOOKS });
    page.send(RenameObjectStore { .upgrade = connection(2), .object_store = NOTES, .name = "renamed"_utf16 });
    page.send(Commit { .transaction = upgrade_of(2), .results_started = 0 });
    page.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "open 2 needs an upgrade of connection 2 from version 1 to 2 with object stores [books=1, notes=2]"_string,
                                    "the upgrade of connection 2 completed, having created object stores [3]"_string,
                                    "open 2 succeeded after its upgrade"_string,
                                    "open 3 has connection 3 at version 2 with object stores [renamed=2, most=3]"_string,
                                }));
}

TEST_CASE(a_database_whose_first_upgrade_was_not_committed_is_deleted)
{
    // A storage key with room for one database and not for two.
    IndexedDBStore::Limits limits {
        .budget_of_a_storage_key = 1 * MiB + 512 * KiB,
        .ceiling_of_the_session = 64 * MiB,
        .charge_for_a_database = 1 * MiB,
    };
    auto session = session_over(MUST(IndexedDBStore::create_transient(limits)));
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(Abort { .transaction = upgrade_of(1) });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "open 1 needs an upgrade of connection 1 from version 0 to 1 with object stores []"_string,
                                    "the upgrade of connection 1 aborted"_string,
                                    "open 1 failed since its upgrade left it no connection"_string,
                                }));

    page.send(ListDatabases {});
    page.send(Open { .name = "another"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "list 2 has []"_string,
                                    "open 3 needs an upgrade of connection 2 from version 0 to 1 with object stores []"_string,
                                }));
}

TEST_CASE(an_open_whose_connection_is_closed_during_its_upgrade_fails)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CloseConnection { .connection = connection(1) });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(Commit { .transaction = upgrade_of(1), .results_started = 0 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "open 1 needs an upgrade of connection 1 from version 0 to 1 with object stores []"_string,
                                    "the upgrade of connection 1 completed, having created object stores [1]"_string,
                                    "open 1 failed since its upgrade left it no connection"_string,
                                }));

    // The upgrade was committed all the same.
    page.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector { "open 2 has connection 2 at version 1 with object stores [books=1]"_string }));
}

TEST_CASE(a_change_to_the_object_stores_that_cannot_be_made_ends_the_upgrade)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    page.send(CloseConnection { .connection = connection(1) });
    page.send(Open { .name = "database"_utf16, .version = 2 });
    (void)page.has_heard();

    // SQLite has no room for a name of this size.
    auto large_name = Utf16String::repeated('a', 4 * MiB);
    with_little_memory_for_sqlite([&] {
        page.send(CreateObjectStore { .upgrade = connection(2), .name = large_name });
    });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "the upgrade of connection 2 aborted for OutOfMemory"_string,
                                    "open 2 failed since its upgrade left it no connection"_string,
                                }));
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});

    page.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector { "open 3 has connection 3 at version 1 with object stores [books=1, notes=2]"_string }));
}

TEST_CASE(a_read_that_loses_the_writer_ends_the_transaction_that_had_it)
{
    auto session = a_session();
    auto writer = a_page(*session);
    open_a_database(writer);

    // A key is one cell however large it is, so that reading it back takes one allocation of its size.
    writer.send(a_transaction(Mode::ReadWrite, { NOTES }));
    writer.send(put_with_the_key(transaction(1), NOTES, binary_key_of_length(4 * MiB)));
    writer.send(Commit { .transaction = transaction(1), .results_started = 1 });

    writer.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    writer.send(put(transaction(2), BOOKS, 1, "one"sv));
    EXPECT_EQ(writer.has_heard(), (Vector {
                                      "result 1 of transaction 1: stored"_string,
                                      "transaction 1 completed"_string,
                                      "result 1 of transaction 2: stored"_string,
                                  }));

    auto reader = a_page(*session);
    reader.send(Open { .name = "database"_utf16 });
    reader.send(a_transaction(Mode::ReadOnly, { NOTES }));
    (void)reader.has_heard();

    // The read fails for its own transaction, and the writer that it cost is another's.
    with_little_memory_for_sqlite([&] {
        reader.send(PlaceRequest { transaction(1), ReadRecords { .object_store = NOTES, .range = {}, .with_values = IndexedDBDatabase::WithValues::No } });
    });
    EXPECT_EQ(reader.has_heard(), (Vector { "result 1 of transaction 1: failed for OutOfMemory"_string }));
    EXPECT_EQ(writer.has_heard(), (Vector { "transaction 2 aborted for OutOfMemory"_string }));
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});

    // The reader's transaction goes on. The writer's left nothing, and another can begin.
    reader.send(count(transaction(1), NOTES));
    EXPECT_EQ(reader.has_heard(), (Vector { "result 2 of transaction 1: counted 1"_string }));

    writer.send(put(transaction(2), BOOKS, 2, "two"sv));
    writer.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    writer.send(count(transaction(3), BOOKS));
    EXPECT_EQ(writer.has_heard(), (Vector { "result 1 of transaction 3: counted 0"_string }));
}

TEST_CASE(a_request_that_the_session_has_no_room_for_ends_its_transaction)
{
    // What a page hears of a put of 16 MiB in a session that may hold 8 MiB, and then of a count.
    auto put_more_than_the_session_holds = [](Function<TransactionName(Page&)> begin_a_transaction) {
        IndexedDBStore::Limits limits {
            .budget_of_a_storage_key = 1 * GiB,
            .ceiling_of_the_session = 8 * MiB,
            .charge_for_a_database = 1 * MiB,
        };
        auto session = session_over(MUST(IndexedDBStore::create_transient(limits)));
        auto page = a_page(*session);
        open_a_database(page);

        auto transaction = begin_a_transaction(page);
        (void)page.has_heard();

        auto large_value = MUST(ByteBuffer::create_zeroed(16 * MiB));
        page.send(PlaceRequest { transaction, StoreRecord { .object_store = BOOKS, .key = number_key(1), .value = move(large_value) } });
        page.send(count(transaction, BOOKS));
        return page.has_heard();
    };

    // The request has its failure, and the transaction that had the writer ends for the same cause.
    auto in_a_transaction = put_more_than_the_session_holds([](Page& page) {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        return transaction(1);
    });
    EXPECT_EQ(in_a_transaction, (Vector {
                                    "result 1 of transaction 1: failed for Quota"_string,
                                    "transaction 1 aborted for Quota"_string,
                                }));

    auto in_an_upgrade = put_more_than_the_session_holds([](Page& page) {
        page.send(CloseConnection { .connection = connection(1) });
        page.send(Open { .name = "database"_utf16, .version = 2 });
        return upgrade_of(2);
    });
    EXPECT_EQ(in_an_upgrade, (Vector {
                                 "result 1 of the upgrade of connection 2: failed for Quota"_string,
                                 "the upgrade of connection 2 aborted for Quota"_string,
                                 "open 2 failed since its upgrade left it no connection"_string,
                             }));
}

TEST_CASE(a_transaction_that_cannot_begin_or_commit_is_aborted)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    // SQLite has no memory to begin a transaction with.
    with_room_for_sqlite(1, [&] {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    });
    EXPECT_EQ(page.has_heard(), (Vector { "transaction 1 aborted for OutOfMemory"_string }));

    // Nor to write a commit with. Nothing is left of either, and the database has no writer.
    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(put(transaction(2), BOOKS, 1, "one"sv));
    with_room_for_sqlite(1, [&] {
        page.send(Commit { .transaction = transaction(2), .results_started = 1 });
    });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "result 1 of transaction 2: stored"_string,
                                    "transaction 2 aborted for OutOfMemory"_string,
                                }));

    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(count(transaction(3), BOOKS));
    page.send(put(transaction(3), BOOKS, 2, "two"sv));
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "result 1 of transaction 3: counted 0"_string,
                                    "result 2 of transaction 3: stored"_string,
                                }));
}

TEST_CASE(what_a_retired_channel_wrote_is_rolled_back)
{
    auto session = a_session();
    auto writer = a_page(*session);
    open_a_database(writer);

    writer.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    writer.send(put(transaction(1), BOOKS, 1, "original"sv));
    writer.send(Commit { .transaction = transaction(1), .results_started = 1 });

    writer.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    writer.send(put(transaction(2), BOOKS, 1, "changed"sv));
    (void)writer.has_heard();

    auto reader = a_page(*session);
    reader.send(Open { .name = "database"_utf16 });
    reader.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    reader.send(read_all(transaction(1), BOOKS));
    EXPECT_EQ(reader.has_heard(), (Vector { "open 1 has connection 1 at version 1 with object stores [books=1, notes=2]"_string }));

    session->retire_channel(writer.channel);
    EXPECT_EQ(reader.has_heard(), (Vector { "result 1 of transaction 1: read [1=original]"_string }));

    // Nobody is told of the end of what the channel had, and what is still sent for it does nothing.
    writer.send(Commit { .transaction = transaction(2), .results_started = 1 });
    EXPECT_EQ(writer.has_heard(), Vector<String> {});
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});
}

TEST_CASE(a_retired_channel_gives_up_its_requests_and_its_connections)
{
    auto session = a_session();
    auto first = a_page(*session);
    open_a_database(first);

    // The second page waits to upgrade, and the third waits behind it.
    auto second = a_page(*session);
    second.send(Open { .name = "database"_utf16, .version = 2 });
    auto third = a_page(*session);
    third.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(first.has_heard(), (Vector { "connection 1 hears of a change from version 1 to version 2 (notification 1)"_string }));

    session->retire_channel(second.channel);
    EXPECT_EQ(third.has_heard(), (Vector { "open 1 has connection 1 at version 1 with object stores [books=1, notes=2]"_string }));

    // The third page then wants the upgrade, and the first is told again, by another number.
    third.send(CloseConnection { .connection = connection(1) });
    third.send(Open { .name = "database"_utf16, .version = 2 });
    EXPECT_EQ(first.has_heard(), (Vector { "connection 1 hears of a change from version 1 to version 2 (notification 2)"_string }));

    session->retire_channel(first.channel);
    EXPECT_EQ(third.has_heard(), (Vector { "open 2 needs an upgrade of connection 2 from version 1 to 2 with object stores [books=1, notes=2]"_string }));

    // A channel that is retired during its upgrade leaves the database as it was.
    third.send(CreateObjectStore { .upgrade = connection(2), .name = "more"_utf16 });
    session->retire_channel(third.channel);

    auto fourth = a_page(*session);
    fourth.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(fourth.has_heard(), (Vector { "open 1 has connection 1 at version 1 with object stores [books=1, notes=2]"_string }));
}

TEST_CASE(the_channels_of_an_agent_are_retired_together)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    // One channel of the agent waits to delete the database that another channel of it has open.
    auto other_channel = another_channel_of(page);
    other_channel.send(DeleteDatabase { .name = "database"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector { "connection 1 hears of a change from version 1 to no version (notification 1)"_string }));

    session->retire_agent(page.agent);

    auto onlooker = a_page(*session);
    onlooker.send(ListDatabases {});
    EXPECT_EQ(onlooker.has_heard(), (Vector { "list 1 has [database at version 1]"_string }));
}

TEST_CASE(a_claim_can_be_found_false_while_another_channel_is_served)
{
    auto session = a_session();
    auto first = a_page(*session);
    open_a_database(first);

    auto second = a_page(*session);
    second.send(Open { .name = "database"_utf16 });
    (void)second.has_heard();

    // The second page's request has bytes for its key that are no key, and waits for the first page's transaction.
    first.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    second.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    second.send(put_with_the_key(transaction(1), BOOKS, MUST(ByteBuffer::copy(Array<u8, 1> { 0x60 }.span()))));
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});

    first.send(Commit { .transaction = transaction(1), .results_started = 0 });
    EXPECT_EQ(first.has_heard(), (Vector { "transaction 1 completed"_string }));
    EXPECT_EQ(second.has_heard(), Vector<String> {});
    EXPECT_EQ(session->take_channels_at_fault(), (Vector { second.channel }));

    // What the second page had is over, so the first can write at once.
    first.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    first.send(put(transaction(2), BOOKS, 1, "one"sv));
    EXPECT_EQ(first.has_heard(), (Vector { "result 1 of transaction 2: stored"_string }));

    // Nothing more is done for the channel.
    second.send(Open { .name = "another"_utf16 });
    second.send(Commit { .transaction = upgrade_of(2), .results_started = 0 });
    EXPECT_EQ(second.has_heard(), Vector<String> {});

    first.send(ListDatabases {});
    EXPECT_EQ(first.has_heard(), (Vector { "list 2 has [database at version 1]"_string }));
}

TEST_CASE(a_channel_that_claims_what_cannot_be_so_is_retired)
{
    // The page has the database of open_a_database() and its first connection. The last thing that it sends is the
    // claim.
    auto expect_a_false_claim = [](StringView claim, Function<void(Page&)> send_the_claim) {
        auto session = a_session();
        auto page = a_page(*session);
        open_a_database(page);

        send_the_claim(page);
        if (session->take_channels_at_fault() != Vector { page.channel })
            FAIL(MUST(String::formatted("The channel was not retired for {}", claim)));
        (void)page.has_heard();

        // The channel is told nothing from then on.
        page.send(ListDatabases {});
        if (!page.has_heard().is_empty())
            FAIL(MUST(String::formatted("The channel was answered after {}", claim)));
    };

    expect_a_false_claim("a connection that it was not told of"sv, [](Page& page) {
        page.send(CloseConnection { .connection = connection(2) });
    });
    expect_a_false_claim("a connection without a number"sv, [](Page& page) {
        page.send(CloseConnection { .connection = connection(0) });
    });
    expect_a_false_claim("a transaction that it did not create"sv, [](Page& page) {
        page.send(Abort { .transaction = transaction(1) });
    });
    expect_a_false_claim("a transaction without object stores"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, {}));
    });
    expect_a_false_claim("a change of records in a transaction that only reads"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
        page.send(put(transaction(1), BOOKS, 1, "one"sv));
    });
    expect_a_false_claim("a request for an object store outside the scope of its transaction"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
        page.send(count(transaction(1), NOTES));
    });
    expect_a_false_claim("an object store by its number in a transaction that is no upgrade"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
        page.send(count(transaction(1), ObjectStoreOfUpgrade { 1 }));
    });
    expect_a_false_claim("a commit that has started on a result which it was not sent"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
        page.send(Commit { .transaction = transaction(1), .results_started = 1 });
    });
    expect_a_false_claim("a second commit of a transaction"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(Commit { .transaction = transaction(2), .results_started = 0 });
        page.send(Commit { .transaction = transaction(2), .results_started = 0 });
    });
    expect_a_false_claim("a request of a transaction that it has asked to commit"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(Commit { .transaction = transaction(2), .results_started = 0 });
        page.send(count(transaction(2), BOOKS));
    });
    expect_a_false_claim("an answer to a notification that it was not sent"sv, [](Page& page) {
        page.send(VersionChangeHandled { .connection = connection(1), .notification = 1 });
    });
    expect_a_false_claim("an upgrade transaction of a connection that was opened without one"sv, [](Page& page) {
        page.send(Open { .name = "database"_utf16 });
        page.send(Abort { .transaction = upgrade_of(2) });
    });
    expect_a_false_claim("a transaction of a connection that is being upgraded"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }, 2));
    });
    expect_a_false_claim("a transaction of a connection that it has asked to close"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(CloseConnection { .connection = connection(1) });
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }));
    });
    expect_a_false_claim("an object store that its upgrade has not created"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(CreateObjectStore { .upgrade = connection(2), .name = "first"_utf16 });
        page.send(count(upgrade_of(2), ObjectStoreOfUpgrade { 2 }));
    });
    expect_a_false_claim("an object store of its upgrade without a number"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(CreateObjectStore { .upgrade = connection(2), .name = "first"_utf16 });
        page.send(count(upgrade_of(2), ObjectStoreOfUpgrade { 0 }));
    });
    expect_a_false_claim("the rename of an object store that its upgrade has not created"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(RenameObjectStore { .upgrade = connection(2), .object_store = ObjectStoreOfUpgrade { 1 }, .name = "renamed"_utf16 });
    });
    expect_a_false_claim("the deletion of an object store that its upgrade has not created"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(DeleteObjectStore { .upgrade = connection(2), .object_store = ObjectStoreOfUpgrade { 1 } });
    });

    // The store finds these out, when it is asked to do what was sent.
    expect_a_false_claim("a version that no database has"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16, .version = 0 });
    });
    expect_a_false_claim("bytes for a key that are no key"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(put_with_the_key(transaction(1), BOOKS, MUST(ByteBuffer::copy(Array<u8, 1> { 0x60 }.span()))));
    });
    expect_a_false_claim("an object store with a name that is taken"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(CreateObjectStore { .upgrade = connection(2), .name = "first"_utf16 });
        page.send(CreateObjectStore { .upgrade = connection(2), .name = "first"_utf16 });
    });
}

TEST_CASE(an_index_is_filled_with_the_keys_that_its_renderer_extracts)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(put(upgrade_of(1), ObjectStoreOfUpgrade { 1 }, 1, "c"sv));
    page.send(put(upgrade_of(1), ObjectStoreOfUpgrade { 1 }, 2, "a"sv));
    page.send(put(upgrade_of(1), ObjectStoreOfUpgrade { 1 }, 3, "b"sv));
    (void)page.has_heard();

    // The object store has records by the time that the index is asked for. What comes after the index waits until
    // each of them has its keys, and so does the commit.
    page.send(an_index(1, ObjectStoreOfUpgrade { 1 }));
    page.send(read_index(upgrade_of(1), IndexOfUpgrade { 1 }));
    page.send(Commit { .transaction = upgrade_of(1), .results_started = 3 });
    EXPECT_EQ(page.has_heard(), (Vector { "the upgrade of connection 1 is asked for the keys of its index 1 for the value c (token 1)"_string }));

    page.send(keys_for_index(1, 1, { 30 }));
    EXPECT_EQ(page.has_heard(), (Vector { "the upgrade of connection 1 is asked for the keys of its index 1 for the value a (token 2)"_string }));

    // A value that has nothing at the key path of the index has no key in it.
    page.send(keys_for_index(1, 2, {}));
    EXPECT_EQ(page.has_heard(), (Vector { "the upgrade of connection 1 is asked for the keys of its index 1 for the value b (token 3)"_string }));

    page.send(keys_for_index(1, 3, { 20 }));
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "result 4 of the upgrade of connection 1: read [20/3=b, 30/1=c]"_string,
                                    "the upgrade of connection 1 completed, having created object stores [1], having created indexes [1]"_string,
                                    "open 1 succeeded after its upgrade"_string,
                                }));
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});

    auto another_page = a_page(*session);
    another_page.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(another_page.has_heard(), (Vector { "open 1 has connection 1 at version 1 with object stores [books=1 with the indexes [by_number=1]]"_string }));
}

TEST_CASE(an_index_of_an_object_store_without_records_needs_no_keys)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(an_index(1, ObjectStoreOfUpgrade { 1 }));
    page.send(Commit { .transaction = upgrade_of(1), .results_started = 0 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "open 1 needs an upgrade of connection 1 from version 0 to 1 with object stores []"_string,
                                    "the upgrade of connection 1 completed, having created object stores [1], having created indexes [1]"_string,
                                    "open 1 succeeded after its upgrade"_string,
                                }));
}

TEST_CASE(an_abort_is_taken_while_an_index_is_filled)
{
    // What a page hears when it aborts an upgrade that waits for the keys of an index and was asked to commit, then
    // sends the keys all the same, and opens the database again.
    auto abort_during_a_fill = [](Function<void(Page&)> open_the_database, u64 upgraded_connection, ObjectStoreName object_store) {
        auto session = a_session();
        auto page = a_page(*session);
        open_the_database(page);

        page.send(put(upgrade_of(upgraded_connection), object_store, 1, "a"sv));
        page.send(an_index(upgraded_connection, object_store));
        page.send(Commit { .transaction = upgrade_of(upgraded_connection), .results_started = 1 });
        (void)page.has_heard();

        page.send(Abort { .transaction = upgrade_of(upgraded_connection) });
        page.send(keys_for_index(upgraded_connection, 1, { 10 }));
        page.send(Open { .name = "database"_utf16 });

        EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});
        return page.has_heard();
    };

    // The page hears of the end of the upgrade once, and then of its open. A database that the upgrade was the first
    // of is as if it had never been.
    auto in_a_first_upgrade = abort_during_a_fill(
        [](Page& page) {
            page.send(Open { .name = "database"_utf16 });
            page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
        },
        1, ObjectStoreOfUpgrade { 1 });
    EXPECT_EQ(in_a_first_upgrade, (Vector {
                                      "the upgrade of connection 1 aborted"_string,
                                      "open 1 failed since its upgrade left it no connection"_string,
                                      "open 2 needs an upgrade of connection 2 from version 0 to 1 with object stores []"_string,
                                  }));

    // A database that was there before has the version and the object stores from before.
    auto in_a_later_upgrade = abort_during_a_fill(
        [](Page& page) {
            open_a_database(page);
            page.send(CloseConnection { .connection = connection(1) });
            page.send(Open { .name = "database"_utf16, .version = 2 });
        },
        2, BOOKS);
    EXPECT_EQ(in_a_later_upgrade, (Vector {
                                      "the upgrade of connection 2 aborted"_string,
                                      "open 2 failed since its upgrade left it no connection"_string,
                                      "open 3 has connection 3 at version 1 with object stores [books=1, notes=2]"_string,
                                  }));
}

TEST_CASE(a_record_that_breaks_a_unique_index_ends_the_upgrade)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(put(upgrade_of(1), ObjectStoreOfUpgrade { 1 }, 1, "a"sv));
    page.send(put(upgrade_of(1), ObjectStoreOfUpgrade { 1 }, 2, "b"sv));
    page.send(an_index(1, ObjectStoreOfUpgrade { 1 }, IndexedDBDatabase::Unique::Yes));
    page.send(keys_for_index(1, 1, { 7 }));
    (void)page.has_heard();

    page.send(keys_for_index(1, 2, { 7 }));
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "the upgrade of connection 1 aborted for Constraint"_string,
                                    "open 1 failed since its upgrade left it no connection"_string,
                                }));
    EXPECT_EQ(session->take_channels_at_fault(), Vector<Session::ChannelID> {});
}

TEST_CASE(a_channel_that_is_retired_while_its_index_is_filled_leaves_the_database_as_it_was)
{
    auto session = a_session();
    auto page = a_page(*session);
    open_a_database(page);

    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(put(transaction(1), BOOKS, 1, "a"sv));
    page.send(Commit { .transaction = transaction(1), .results_started = 1 });
    page.send(CloseConnection { .connection = connection(1) });

    page.send(Open { .name = "database"_utf16, .version = 2 });
    page.send(an_index(2, BOOKS));
    session->retire_channel(page.channel);

    auto another_page = a_page(*session);
    another_page.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(another_page.has_heard(), (Vector { "open 1 has connection 1 at version 1 with object stores [books=1, notes=2]"_string }));
}

TEST_CASE(requests_store_the_keys_of_indexes_and_read_through_them)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(an_index(1, ObjectStoreOfUpgrade { 1 }, IndexedDBDatabase::Unique::Yes));
    page.send(put_with_an_index_key(upgrade_of(1), ObjectStoreOfUpgrade { 1 }, 1, "one"sv, IndexOfUpgrade { 1 }, 20));
    page.send(Commit { .transaction = upgrade_of(1), .results_started = 1 });
    (void)page.has_heard();

    // The index has the id that the page was told, as the object store has.
    i64 const index = 1;
    auto iterate_from = [&](Optional<IndexCursorPosition> position) {
        return PlaceRequest { transaction(1), IterateIndexCursor { .index = index, .range = {}, .position = move(position) } };
    };

    page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    page.send(put_with_an_index_key(transaction(1), BOOKS, 2, "two"sv, index, 10));
    page.send(put_with_an_index_key(transaction(1), BOOKS, 3, "three"sv, index, 10));
    page.send(read_index(transaction(1), index));
    page.send(PlaceRequest { transaction(1), ReadIndexRecords { .index = index, .range = {}, .direction = IndexedDBDatabase::Direction::Descending, .count = 1, .with_values = IndexedDBDatabase::WithValues::No } });
    page.send(PlaceRequest { transaction(1), CountIndexRecords { .index = index, .range = { .lower_bound = number_key(15) } } });
    page.send(iterate_from({}));
    page.send(iterate_from(IndexCursorPosition { .key = number_key(10), .primary_key = number_key(2) }));
    page.send(iterate_from(IndexCursorPosition { .key = number_key(20), .primary_key = number_key(1) }));
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "result 1 of transaction 1: stored"_string,
                                    "result 2 of transaction 1: failed for Constraint"_string,
                                    "result 3 of transaction 1: read [10/2=two, 20/1=one]"_string,
                                    "result 4 of transaction 1: read [20/1]"_string,
                                    "result 5 of transaction 1: counted 1"_string,
                                    "result 6 of transaction 1: the cursor is at 10/2=two"_string,
                                    "result 7 of transaction 1: the cursor is at 20/1=one"_string,
                                    "result 8 of transaction 1: the cursor is at the end"_string,
                                }));
}

TEST_CASE(an_upgrade_renames_and_deletes_indexes)
{
    auto session = a_session();
    auto page = a_page(*session);

    page.send(Open { .name = "database"_utf16 });
    page.send(CreateObjectStore { .upgrade = connection(1), .name = "books"_utf16 });
    page.send(an_index(1, ObjectStoreOfUpgrade { 1 }));
    page.send(Commit { .transaction = upgrade_of(1), .results_started = 0 });
    page.send(CloseConnection { .connection = connection(1) });
    (void)page.has_heard();

    // The index from before has its id. The ones that this upgrade creates have their numbers in it.
    page.send(Open { .name = "database"_utf16, .version = 2 });
    page.send(RenameIndex { .upgrade = connection(2), .index = i64 { 1 }, .name = "renamed"_utf16 });
    page.send(CreateIndex { .upgrade = connection(2), .object_store = BOOKS, .name = "short-lived"_utf16, .key_path = WebView::IndexedDBKeyPath { "a"_utf16 } });
    page.send(CreateIndex { .upgrade = connection(2), .object_store = BOOKS, .name = "another"_utf16, .key_path = WebView::IndexedDBKeyPath { "b"_utf16 } });
    page.send(DeleteIndex { .upgrade = connection(2), .index = IndexOfUpgrade { 1 } });
    page.send(RenameIndex { .upgrade = connection(2), .index = IndexOfUpgrade { 2 }, .name = "kept"_utf16 });
    page.send(Commit { .transaction = upgrade_of(2), .results_started = 0 });
    page.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(page.has_heard(), (Vector {
                                    "open 2 needs an upgrade of connection 2 from version 1 to 2 with object stores [books=1 with the indexes [by_number=1]]"_string,
                                    "the upgrade of connection 2 completed, having created indexes [2, 3]"_string,
                                    "open 2 succeeded after its upgrade"_string,
                                    "open 3 has connection 3 at version 2 with object stores [books=1 with the indexes [renamed=1, kept=3]]"_string,
                                }));
}

TEST_CASE(a_channel_that_claims_what_cannot_be_so_of_an_index_is_retired)
{
    // The page has opened "database" with the object stores "books", which has a record and an index, and "notes".
    // It has its second connection, and the last thing that it sends is the claim.
    auto expect_a_false_claim = [](StringView claim, Function<void(Page&)> send_the_claim) {
        auto session = a_session();
        auto page = a_page(*session);
        open_a_database(page);

        page.send(a_transaction(Mode::ReadWrite, { BOOKS }));
        page.send(put(transaction(1), BOOKS, 1, "a"sv));
        page.send(Commit { .transaction = transaction(1), .results_started = 1 });
        page.send(CloseConnection { .connection = connection(1) });

        page.send(Open { .name = "database"_utf16, .version = 2 });
        page.send(an_index(2, BOOKS));
        page.send(keys_for_index(2, 1, { 10 }));
        page.send(Commit { .transaction = upgrade_of(2), .results_started = 0 });
        (void)page.has_heard();
        if (!session->take_channels_at_fault().is_empty())
            FAIL("The channel was retired before it claimed anything");

        send_the_claim(page);
        if (session->take_channels_at_fault() != Vector { page.channel })
            FAIL(MUST(String::formatted("The channel was not retired for {}", claim)));
        (void)page.has_heard();

        page.send(ListDatabases {});
        if (!page.has_heard().is_empty())
            FAIL(MUST(String::formatted("The channel was answered after {}", claim)));
    };

    // Has the page begin a third upgrade, of another database, in which it puts two records and asks for an index.
    auto begin_to_fill_an_index = [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(CreateObjectStore { .upgrade = connection(3), .name = "first"_utf16 });
        page.send(put(upgrade_of(3), ObjectStoreOfUpgrade { 1 }, 1, "a"sv));
        page.send(put(upgrade_of(3), ObjectStoreOfUpgrade { 1 }, 2, "b"sv));
        page.send(an_index(3, ObjectStoreOfUpgrade { 1 }));
    };

    expect_a_false_claim("keys for an index while its upgrade waits for none"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(keys_for_index(3, 1, { 10 }));
    });
    expect_a_false_claim("keys with a token that it was not given"sv, [&](Page& page) {
        begin_to_fill_an_index(page);
        page.send(keys_for_index(3, 2, { 10 }));
    });
    expect_a_false_claim("keys for a record that has its keys"sv, [&](Page& page) {
        begin_to_fill_an_index(page);
        page.send(keys_for_index(3, 1, { 10 }));
        page.send(keys_for_index(3, 1, { 10 }));
    });
    expect_a_false_claim("keys for an index that has been filled"sv, [&](Page& page) {
        begin_to_fill_an_index(page);
        page.send(keys_for_index(3, 1, { 10 }));
        page.send(keys_for_index(3, 2, { 20 }));
        page.send(keys_for_index(3, 2, { 20 }));
    });
    expect_a_false_claim("two keys for a record in an index that takes one"sv, [&](Page& page) {
        begin_to_fill_an_index(page);
        page.send(keys_for_index(3, 1, { 10, 20 }));
    });
    expect_a_false_claim("a change to the object stores of an upgrade that it has asked to commit"sv, [&](Page& page) {
        begin_to_fill_an_index(page);
        page.send(Commit { .transaction = upgrade_of(3), .results_started = 2 });
        page.send(CreateObjectStore { .upgrade = connection(3), .name = "second"_utf16 });
    });
    expect_a_false_claim("an index of an object store that its upgrade has not created"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(an_index(3, ObjectStoreOfUpgrade { 1 }));
    });
    expect_a_false_claim("an index that its upgrade has not created"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(read_index(upgrade_of(3), IndexOfUpgrade { 1 }));
    });
    expect_a_false_claim("an index of its upgrade without a number"sv, [&](Page& page) {
        begin_to_fill_an_index(page);
        page.send(read_index(upgrade_of(3), IndexOfUpgrade { 0 }));
    });
    expect_a_false_claim("the rename of an index that its upgrade has not created"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(RenameIndex { .upgrade = connection(3), .index = IndexOfUpgrade { 1 }, .name = "renamed"_utf16 });
    });
    expect_a_false_claim("the deletion of an index that its upgrade has not created"sv, [](Page& page) {
        page.send(Open { .name = "other"_utf16 });
        page.send(DeleteIndex { .upgrade = connection(3), .index = IndexOfUpgrade { 1 } });
    });
    expect_a_false_claim("keys for an index by its number in a transaction that is no upgrade"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS }, 2));
        page.send(put_with_an_index_key(transaction(2), BOOKS, 2, "b"sv, IndexOfUpgrade { 1 }, 20));
    });

    // The database finds these out, when it is asked to do what was sent.
    i64 const index_of_books = 1;
    expect_a_false_claim("an index that the database does not have"sv, [](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { BOOKS }, 2));
        page.send(read_index(transaction(2), i64 { 1000 }));
    });
    expect_a_false_claim("an index of an object store outside the scope of its transaction"sv, [&](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { NOTES }, 2));
        page.send(read_index(transaction(2), index_of_books));
    });
    expect_a_false_claim("the count of an index of an object store outside the scope of its transaction"sv, [&](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { NOTES }, 2));
        page.send(PlaceRequest { transaction(2), CountIndexRecords { .index = index_of_books, .range = {} } });
    });
    expect_a_false_claim("a cursor over an index of an object store outside the scope of its transaction"sv, [&](Page& page) {
        page.send(a_transaction(Mode::ReadOnly, { NOTES }, 2));
        page.send(PlaceRequest { transaction(2), IterateIndexCursor { .index = index_of_books, .range = {} } });
    });
    expect_a_false_claim("keys for an index of another object store than the record's"sv, [&](Page& page) {
        page.send(a_transaction(Mode::ReadWrite, { BOOKS, NOTES }, 2));
        page.send(put_with_an_index_key(transaction(2), NOTES, 1, "a"sv, index_of_books, 20));
    });
}

TEST_CASE(the_times_that_the_store_keeps_are_the_clock_s)
{
    auto store = MUST(IndexedDBStore::create_transient());
    auto database = MUST(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;

    auto now = at(5);
    Session session { move(store), [&] { return now; } };

    auto first = a_page(session);
    open_a_database(first);
    EXPECT_EQ(MUST(database->last_open_or_write_time()), at(5));

    now = at(9);
    auto second = a_page(session);
    second.send(Open { .name = "database"_utf16 });
    EXPECT_EQ(MUST(database->last_open_or_write_time()), at(9));

    now = at(12);
    second.send(a_transaction(Mode::ReadWrite, { BOOKS }));
    second.send(put(transaction(1), BOOKS, 1, "one"sv));
    second.send(Commit { .transaction = transaction(1), .results_started = 1 });
    EXPECT_EQ(MUST(database->last_open_or_write_time()), at(12));
}
