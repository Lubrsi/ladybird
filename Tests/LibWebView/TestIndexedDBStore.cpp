/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf16View.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/IndexedDB/KeyEncoding.h>
#include <LibWebView/IndexedDBStore.h>
#include <sqlite3.h>

using WebView::IndexedDBDatabase;
using WebView::IndexedDBError;
using WebView::IndexedDBKeyRange;
using WebView::IndexedDBStore;

using Direction = IndexedDBDatabase::Direction;
using KeyGenerator = IndexedDBDatabase::KeyGenerator;
using NoOverwrite = IndexedDBDatabase::NoOverwrite;
using WithValues = IndexedDBDatabase::WithValues;

// Lets a failed comparison of keys or values print them.
template<>
struct AK::Formatter<ByteBuffer> : Formatter<ReadonlyBytes> {
    ErrorOr<void> format(FormatBuilder& builder, ByteBuffer const& bytes)
    {
        return Formatter<ReadonlyBytes>::format(builder, bytes.bytes());
    }
};

// Keys as LibWebCommon's key encoding writes them.
static constexpr Array<u8, 9> NUMBER_MINUS_5 { 0x10, 0x3f, 0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static constexpr Array<u8, 9> NUMBER_NEGATIVE_0 { 0x10, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static constexpr Array<u8, 9> NUMBER_0 { 0x10, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static constexpr Array<u8, 9> NUMBER_20_5 { 0x10, 0xc0, 0x34, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00 };
static constexpr Array<u8, 9> NUMBER_INFINITY { 0x10, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static constexpr Array<u8, 9> DATE_1 { 0x20, 0xbf, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static constexpr Array<u8, 9> DATE_2 { 0x20, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static constexpr Array<u8, 9> DATE_1000 { 0x20, 0xc0, 0x8f, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00 };
static constexpr Array<u8, 3> STRING_EMPTY { 0x30, 0x00, 0x00 };
static constexpr Array<u8, 5> STRING_A { 0x30, 0x00, 0x61, 0x00, 0x00 };
static constexpr Array<u8, 9> STRING_A_NUL { 0x30, 0x00, 0x61, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 };
static constexpr Array<u8, 7> STRING_AB { 0x30, 0x00, 0x61, 0x00, 0x62, 0x00, 0x00 };
static constexpr Array<u8, 2> ARRAY_EMPTY { 0x50, 0x00 };
static constexpr Array<u8, 11> ARRAY_OF_1 { 0x50, 0x10, 0xbf, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static constexpr Array<u8, 4> ARRAY_OF_AN_EMPTY_ARRAY { 0x50, 0x50, 0x00, 0x00 };

static constexpr u64 TWO_TO_THE_53 = 1ULL << 53;

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

// A binary key: 0x40, the bytes with 0x00 and 0x01 escaped, 0x00.
template<typename... Bytes>
static ByteBuffer binary_key(Bytes... payload)
{
    ByteBuffer key;
    key.append(0x40);
    for (u8 byte : { static_cast<u8>(payload)... }) {
        if (byte <= 0x01)
            key.append(0x01);
        key.append(byte <= 0x01 ? byte + 1 : byte);
    }
    key.append(0x00);
    return key;
}

static ByteBuffer binary_key()
{
    return MUST(ByteBuffer::copy(Array<u8, 2> { 0x40, 0x00 }.span()));
}

static IndexedDBKeyRange only(ReadonlyBytes key)
{
    return { .lower_bound = key, .lower_bound_is_open = false, .upper_bound = key, .upper_bound_is_open = false };
}

template<typename T>
static Optional<IndexedDBError::Kind> kind_of_failure(ErrorOr<T, IndexedDBError> const& result)
{
    if (!result.is_error())
        return {};
    return result.error().kind;
}

static ReadonlyBytes bytes_of(ReadonlyBytes bytes)
{
    return bytes;
}

static ReadonlyBytes bytes_of(ByteBuffer const& bytes)
{
    return bytes.bytes();
}

static ByteBuffer buffer_of(StringView text)
{
    return MUST(ByteBuffer::copy(text.bytes()));
}

static auto put(IndexedDBDatabase::Writer& writer, i64 object_store, auto const& key, StringView value)
{
    return writer.store_record(object_store, bytes_of(key), value.bytes(), NoOverwrite::No);
}

static auto add(IndexedDBDatabase::Writer& writer, i64 object_store, auto const& key, StringView value)
{
    return writer.store_record(object_store, bytes_of(key), value.bytes(), NoOverwrite::Yes);
}

// Stores a record whose key the object store generates, and returns that key.
static u64 put_with_a_generated_key(IndexedDBDatabase::Writer& writer, i64 object_store)
{
    return MUST(writer.store_record(object_store, {}, "value"sv.bytes(), NoOverwrite::No)).generated_key.release_value();
}

static Vector<ByteBuffer> keys_of(IndexedDBDatabase& database, i64 object_store, IndexedDBKeyRange const& range = {}, Direction direction = Direction::Ascending, Optional<u32> limit = {})
{
    Vector<ByteBuffer> keys;
    for (auto& record : MUST(database.read_records(object_store, range, direction, limit, WithValues::No))) {
        EXPECT(!record.value.has_value());
        keys.append(move(record.key));
    }
    return keys;
}

static Optional<ByteBuffer> value_at(IndexedDBDatabase& database, i64 object_store, auto const& key)
{
    auto records = MUST(database.read_records(object_store, only(bytes_of(key)), Direction::Ascending, {}, WithValues::Yes));
    if (records.is_empty())
        return {};
    EXPECT_EQ(records.size(), 1u);
    return records[0].value.release_value();
}

struct StoreWithADatabase {
    NonnullOwnPtr<IndexedDBStore> store;
    NonnullRefPtr<IndexedDBDatabase> database;
    i64 object_store { 0 };
};

// A store with a database at version 1, which has one object store.
static StoreWithADatabase store_with_a_database(Optional<WebView::IndexedDBKeyPath> key_path = {}, KeyGenerator key_generator = KeyGenerator::No)
{
    auto store = MUST(IndexedDBStore::create_transient());
    auto database = MUST(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;

    auto upgrade = MUST(database->begin_upgrade(1));
    auto object_store = MUST(upgrade->create_object_store("store"_utf16, key_path, key_generator));
    MUST(upgrade->commit(at(1)));

    return { move(store), move(database), object_store };
}

TEST_CASE(connections_to_one_storage_key_and_name_share_a_database)
{
    auto [store, database, object_store] = store_with_a_database();

    auto opened_again = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, {}, at(2)));
    EXPECT_EQ(opened_again.database.ptr(), database.ptr());
    EXPECT_EQ(opened_again.version, 1u);
    EXPECT(!opened_again.needs_upgrade);

    auto writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "written through the first"sv));
    TRY_OR_FAIL(writer->commit(at(3)));

    EXPECT_EQ(value_at(*opened_again.database, object_store, number_key(1)), buffer_of("written through the first"sv));
}

TEST_CASE(a_database_outlives_its_last_user)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    i64 object_store = 0;
    {
        auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
        auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));
        object_store = TRY_OR_FAIL(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
        TRY_OR_FAIL(put(*upgrade, object_store, number_key(1), "marker"sv));
        TRY_OR_FAIL(upgrade->commit(at(1)));
    }

    // Nothing holds the database here, and no open of it is under way.
    auto opened_again = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, {}, at(2)));
    EXPECT_EQ(opened_again.version, 1u);
    EXPECT(!opened_again.needs_upgrade);
    EXPECT_EQ(value_at(*opened_again.database, object_store, number_key(1)), buffer_of("marker"sv));
}

TEST_CASE(a_storage_key_has_databases_of_its_own)
{
    auto [store, database, object_store] = store_with_a_database();

    auto opened = TRY_OR_FAIL(store->open_database("https://other.example"_string, "database"_utf16, {}, at(2)));
    EXPECT_NE(opened.database.ptr(), database.ptr());
    EXPECT_EQ(opened.version, 1u);
    EXPECT(opened.needs_upgrade);
    EXPECT_EQ(opened.database->version(), 0u);
    EXPECT(TRY_OR_FAIL(opened.database->object_stores()).is_empty());

    EXPECT(TRY_OR_FAIL(store->databases("https://other.example"_string)).is_empty());
    EXPECT_EQ(TRY_OR_FAIL(store->databases(example_com())).size(), 1u);
}

TEST_CASE(a_store_does_not_see_the_databases_of_another)
{
    auto [store, database, object_store] = store_with_a_database();

    auto other_store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    EXPECT(TRY_OR_FAIL(other_store->databases(example_com())).is_empty());

    auto opened = TRY_OR_FAIL(other_store->open_database(example_com(), "database"_utf16, {}, at(2)));
    EXPECT(opened.needs_upgrade);
    EXPECT(TRY_OR_FAIL(opened.database->object_stores()).is_empty());
}

TEST_CASE(an_open_follows_the_rules_for_versions)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto open = [&](Optional<u64> version) {
        return store->open_database(example_com(), "database"_utf16, version, at(1));
    };

    // Without a version, a database that does not exist is opened at version 1.
    auto first = TRY_OR_FAIL(open({}));
    EXPECT_EQ(first.version, 1u);
    EXPECT(first.needs_upgrade);

    auto at_three = TRY_OR_FAIL(open(3));
    EXPECT_EQ(at_three.version, 3u);
    EXPECT(at_three.needs_upgrade);

    auto upgrade = TRY_OR_FAIL(at_three.database->begin_upgrade(at_three.version));
    TRY_OR_FAIL(upgrade->commit(at(1)));

    // Without a version, a database that exists is opened at the version it has.
    auto at_its_version = TRY_OR_FAIL(open({}));
    EXPECT_EQ(at_its_version.version, 3u);
    EXPECT(!at_its_version.needs_upgrade);

    EXPECT(!TRY_OR_FAIL(open(3)).needs_upgrade);
    EXPECT_EQ(kind_of_failure(open(2)), IndexedDBError::Kind::Version);

    auto newer = TRY_OR_FAIL(open(5));
    EXPECT_EQ(newer.version, 5u);
    EXPECT(newer.needs_upgrade);

    EXPECT_EQ(kind_of_failure(open(0)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(open(TWO_TO_THE_53)), IndexedDBError::Kind::InvalidClaim);
    EXPECT(!open(TWO_TO_THE_53 - 1).is_error());
}

TEST_CASE(an_open_starts_over_after_a_first_upgrade_that_did_not_commit)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto open = [&](Optional<u64> version) {
        return store->open_database(example_com(), "database"_utf16, version, at(1));
    };

    {
        auto first = TRY_OR_FAIL(open(3));
        auto upgrade = TRY_OR_FAIL(first.database->begin_upgrade(first.version));
        TRY_OR_FAIL(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
        TRY_OR_FAIL(upgrade->abort());
    }
    EXPECT(TRY_OR_FAIL(store->databases(example_com())).is_empty());

    auto again = TRY_OR_FAIL(open({}));
    EXPECT_EQ(again.version, 1u);
    EXPECT(again.needs_upgrade);
    EXPECT(TRY_OR_FAIL(again.database->object_stores()).is_empty());
}

TEST_CASE(a_version_is_the_last_one_committed)
{
    auto [store, database, object_store] = store_with_a_database();
    auto listed_versions = [&] {
        Vector<u64> versions;
        for (auto const& info : MUST(store->databases(example_com())))
            versions.append(info.version);
        return versions;
    };

    // A database whose first upgrade has not committed is not listed.
    auto second = TRY_OR_FAIL(store->open_database(example_com(), "second"_utf16, 1, at(2))).database;
    EXPECT_EQ(listed_versions(), (Vector<u64> { 1 }));
    {
        auto upgrade = TRY_OR_FAIL(second->begin_upgrade(1));
        EXPECT_EQ(second->version(), 0u);
        EXPECT_EQ(listed_versions(), (Vector<u64> { 1 }));

        TRY_OR_FAIL(upgrade->commit(at(2)));
        EXPECT_EQ(second->version(), 1u);
        EXPECT_EQ(listed_versions(), (Vector<u64> { 1, 1 }));
    }

    // A later upgrade shows once it commits, and not at all if it is aborted.
    {
        auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
        EXPECT_EQ(listed_versions(), (Vector<u64> { 1, 1 }));
        TRY_OR_FAIL(upgrade->abort());
        EXPECT_EQ(listed_versions(), (Vector<u64> { 1, 1 }));
    }
    {
        auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
        TRY_OR_FAIL(upgrade->commit(at(3)));
        EXPECT_EQ(listed_versions(), (Vector<u64> { 2, 1 }));
    }

    auto databases = TRY_OR_FAIL(store->databases(example_com()));
    EXPECT_EQ(databases[0].name, "database"_utf16);
    EXPECT_EQ(databases[1].name, "second"_utf16);

    // The version of a database only grows.
    EXPECT_EQ(kind_of_failure(database->begin_upgrade(2)), IndexedDBError::Kind::Version);
    EXPECT_EQ(kind_of_failure(database->begin_upgrade(1)), IndexedDBError::Kind::Version);
    EXPECT_EQ(kind_of_failure(database->begin_upgrade(TWO_TO_THE_53)), IndexedDBError::Kind::InvalidClaim);
    EXPECT(!database->has_writer());
    EXPECT_EQ(database->version(), 2u);
}

TEST_CASE(a_deleted_database_is_gone)
{
    auto [store, database, object_store] = store_with_a_database();

    EXPECT_EQ(TRY_OR_FAIL(store->delete_database(example_com(), "no such database"_utf16)), 0u);
    EXPECT_EQ(TRY_OR_FAIL(store->delete_database("https://other.example"_string, "database"_utf16)), 0u);
    EXPECT_EQ(TRY_OR_FAIL(store->databases(example_com())).size(), 1u);

    EXPECT_EQ(TRY_OR_FAIL(store->delete_database(example_com(), "database"_utf16)), 1u);
    EXPECT(TRY_OR_FAIL(store->databases(example_com())).is_empty());

    // The store has let go of it, so it is freed with the last connection to it.
    EXPECT_EQ(database->ref_count(), 1u);
    EXPECT_EQ(TRY_OR_FAIL(store->delete_database(example_com(), "database"_utf16)), 0u);

    auto opened = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, {}, at(2)));
    EXPECT_NE(opened.database.ptr(), database.ptr());
    EXPECT(opened.needs_upgrade);
    EXPECT(TRY_OR_FAIL(opened.database->object_stores()).is_empty());
}

TEST_CASE(object_stores_are_created_renamed_and_deleted)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;

    char16_t const name_with_a_lone_surrogate[] = { u'a', 0xd800 };
    auto odd_name = Utf16String::from_utf16(Utf16View { name_with_a_lone_surrogate, 2 });

    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));
    auto plain = TRY_OR_FAIL(upgrade->create_object_store("plain"_utf16, {}, KeyGenerator::No));
    auto with_a_path = TRY_OR_FAIL(upgrade->create_object_store(odd_name, WebView::IndexedDBKeyPath { "a.b"_utf16 }, KeyGenerator::Yes));
    auto with_paths = TRY_OR_FAIL(upgrade->create_object_store("with paths"_utf16, WebView::IndexedDBKeyPath { Vector { "a"_utf16, ""_utf16 } }, KeyGenerator::No));
    TRY_OR_FAIL(put(*upgrade, plain, number_key(1), "value"sv));

    // A key path is read back as it was given, also where its text has to be escaped to be stored.
    auto odd_path = "\"quoted\" \\ \n é \U0001f600"_utf16;
    auto odd_ascii_path = "\"quoted\" \\ \n"_utf16;
    auto with_an_odd_path = TRY_OR_FAIL(upgrade->create_object_store("with an odd path"_utf16, WebView::IndexedDBKeyPath { odd_path }, KeyGenerator::No));
    auto with_an_odd_ascii_path = TRY_OR_FAIL(upgrade->create_object_store("with an odd ASCII path"_utf16, WebView::IndexedDBKeyPath { odd_ascii_path }, KeyGenerator::No));
    auto with_an_empty_path = TRY_OR_FAIL(upgrade->create_object_store("with an empty path"_utf16, WebView::IndexedDBKeyPath { ""_utf16 }, KeyGenerator::No));
    {
        auto object_stores = TRY_OR_FAIL(database->object_stores());
        EXPECT_EQ(object_stores.size(), 6u);
        EXPECT_EQ(object_stores[3].key_path->get<Utf16String>(), odd_path);
        EXPECT_EQ(object_stores[4].key_path->get<Utf16String>(), odd_ascii_path);
        EXPECT_EQ(object_stores[5].key_path->get<Utf16String>(), ""_utf16);
    }
    TRY_OR_FAIL(upgrade->delete_object_store(with_an_odd_path));
    TRY_OR_FAIL(upgrade->delete_object_store(with_an_odd_ascii_path));
    TRY_OR_FAIL(upgrade->delete_object_store(with_an_empty_path));

    auto object_stores = TRY_OR_FAIL(database->object_stores());
    EXPECT_EQ(object_stores.size(), 3u);
    EXPECT_EQ(object_stores[0].id, plain);
    EXPECT_EQ(object_stores[0].name, "plain"_utf16);
    EXPECT(!object_stores[0].key_path.has_value());
    EXPECT(!object_stores[0].has_key_generator);
    EXPECT_EQ(object_stores[1].id, with_a_path);
    EXPECT_EQ(object_stores[1].name, odd_name);
    EXPECT_EQ(object_stores[1].key_path->get<Utf16String>(), "a.b"_utf16);
    EXPECT(object_stores[1].has_key_generator);
    EXPECT_EQ(object_stores[2].id, with_paths);
    EXPECT_EQ(object_stores[2].key_path->get<Vector<Utf16String>>(), (Vector { "a"_utf16, ""_utf16 }));

    // A renderer that runs LibWeb throws for each of these before it sends anything.
    auto create_other = [&](Optional<WebView::IndexedDBKeyPath> key_path, KeyGenerator key_generator) {
        return kind_of_failure(upgrade->create_object_store("other"_utf16, key_path, key_generator));
    };
    EXPECT_EQ(kind_of_failure(upgrade->create_object_store("plain"_utf16, {}, KeyGenerator::No)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(WebView::IndexedDBKeyPath { odd_name }, KeyGenerator::No), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(WebView::IndexedDBKeyPath { Vector<Utf16String> {} }, KeyGenerator::No), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(WebView::IndexedDBKeyPath { Vector { "a"_utf16 } }, KeyGenerator::Yes), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(WebView::IndexedDBKeyPath { ""_utf16 }, KeyGenerator::Yes), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(upgrade->rename_object_store(plain, "with paths"_utf16)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(upgrade->rename_object_store(1000, "other"_utf16)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(upgrade->delete_object_store(1000)), IndexedDBError::Kind::InvalidClaim);
    EXPECT(upgrade->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->object_stores()).size(), 3u);

    TRY_OR_FAIL(upgrade->rename_object_store(with_paths, "renamed"_utf16));
    TRY_OR_FAIL(upgrade->delete_object_store(plain));
    TRY_OR_FAIL(upgrade->commit(at(1)));

    object_stores = TRY_OR_FAIL(database->object_stores());
    EXPECT_EQ(object_stores.size(), 2u);
    EXPECT_EQ(object_stores[0].name, odd_name);
    EXPECT_EQ(object_stores[1].id, with_paths);
    EXPECT_EQ(object_stores[1].name, "renamed"_utf16);

    // The records of an object store go with it.
    EXPECT_EQ(TRY_OR_FAIL(database->count(plain, {})), 0u);
}

TEST_CASE(names_that_differ_in_any_code_unit_are_different_names)
{
    char16_t const with_a_nul[] = { u'a', 0 };
    char16_t const with_a_byte_order_mark[] = { 0xfeff, u'a' };
    char16_t const with_a_lone_surrogate[] = { u'a', 0xd800 };
    Vector const names {
        "a"_utf16,
        Utf16String::from_utf16(Utf16View { with_a_nul, 2 }),
        Utf16String::from_utf16(Utf16View { with_a_byte_order_mark, 2 }),
        Utf16String::from_utf16(Utf16View { with_a_lone_surrogate, 2 }),
    };

    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    for (size_t i = 0; i < names.size(); ++i) {
        auto opened = TRY_OR_FAIL(store->open_database(example_com(), names[i], i + 1, at(1)));
        EXPECT(opened.needs_upgrade);

        auto upgrade = TRY_OR_FAIL(opened.database->begin_upgrade(opened.version));
        for (auto const& name : names)
            TRY_OR_FAIL(upgrade->create_object_store(name, {}, KeyGenerator::No));
        TRY_OR_FAIL(upgrade->commit(at(1)));

        auto object_stores = TRY_OR_FAIL(opened.database->object_stores());
        EXPECT_EQ(object_stores.size(), names.size());
        for (size_t j = 0; j < object_stores.size(); ++j)
            EXPECT_EQ(object_stores[j].name, names[j]);
    }

    auto databases = TRY_OR_FAIL(store->databases(example_com()));
    EXPECT_EQ(databases.size(), names.size());
    for (size_t i = 0; i < databases.size(); ++i) {
        EXPECT_EQ(databases[i].name, names[i]);
        EXPECT_EQ(databases[i].version, i + 1);
    }
}

TEST_CASE(a_writer_that_does_not_commit_leaves_nothing)
{
    auto [store, database, object_store] = store_with_a_database();

    auto writer = TRY_OR_FAIL(database->begin_upgrade(2));
    TRY_OR_FAIL(writer->create_object_store("another"_utf16, {}, KeyGenerator::No));
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 1u);

    TRY_OR_FAIL(writer->abort());
    EXPECT(!writer->is_open());
    EXPECT_EQ(database->version(), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->object_stores()).size(), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);
    EXPECT_EQ(kind_of_failure(put(*writer, object_store, number_key(1), "value"sv)), IndexedDBError::Kind::TransactionLost);
    EXPECT_EQ(kind_of_failure(writer->commit(at(2))), IndexedDBError::Kind::TransactionLost);

    // A writer that is let go of is rolled back as well.
    {
        auto dropped = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(put(*dropped, object_store, number_key(1), "value"sv));
    }
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);
}

TEST_CASE(a_database_has_one_writer_at_a_time)
{
    auto [store, database, object_store] = store_with_a_database();
    EXPECT(!database->has_writer());

    auto writer = TRY_OR_FAIL(database->begin_writer());
    EXPECT(database->has_writer());
    EXPECT(database->begin_writer().is_error());
    EXPECT(database->begin_upgrade(2).is_error());

    // The refused ones took nothing from the one that is open, and the version is as it was.
    EXPECT(writer->is_open());
    EXPECT_EQ(database->version(), 1u);
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));
    TRY_OR_FAIL(writer->commit(at(2)));
    EXPECT(!database->has_writer());
    EXPECT_EQ(database->version(), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 1u);

    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
    EXPECT(database->has_writer());
    EXPECT(database->begin_writer().is_error());
    EXPECT(upgrade->is_open());
}

TEST_CASE(only_an_upgrade_changes_object_stores)
{
    auto [store, database, object_store] = store_with_a_database();

    // A renderer that runs LibWeb throws for each of these before it sends anything.
    auto writer = TRY_OR_FAIL(database->begin_writer());
    EXPECT(!writer->is_upgrade());
    EXPECT_EQ(kind_of_failure(writer->create_object_store("another"_utf16, {}, KeyGenerator::No)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(writer->rename_object_store(object_store, "renamed"_utf16)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(writer->delete_object_store(object_store)), IndexedDBError::Kind::InvalidClaim);

    EXPECT(writer->is_open());
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));
    TRY_OR_FAIL(writer->commit(at(2)));

    auto object_stores = TRY_OR_FAIL(database->object_stores());
    EXPECT_EQ(object_stores.size(), 1u);
    EXPECT_EQ(object_stores[0].name, "store"_utf16);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 1u);
    EXPECT_EQ(database->version(), 1u);
}

TEST_CASE(add_refuses_a_key_that_is_taken_and_put_replaces_its_record)
{
    auto [store, database, object_store] = store_with_a_database();
    auto writer = TRY_OR_FAIL(database->begin_upgrade(2));

    TRY_OR_FAIL(add(*writer, object_store, number_key(1), "first"sv));
    EXPECT_EQ(kind_of_failure(add(*writer, object_store, number_key(1), "second"sv)), IndexedDBError::Kind::Constraint);
    EXPECT(writer->is_open());
    EXPECT_EQ(value_at(*database, object_store, number_key(1)), buffer_of("first"sv));

    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "third"sv));
    EXPECT_EQ(value_at(*database, object_store, number_key(1)), buffer_of("third"sv));
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 1u);

    // An object store has keys of its own.
    auto other_object_store = TRY_OR_FAIL(writer->create_object_store("other"_utf16, {}, KeyGenerator::No));
    TRY_OR_FAIL(add(*writer, other_object_store, number_key(1), "elsewhere"sv));
    EXPECT_EQ(value_at(*database, object_store, number_key(1)), buffer_of("third"sv));
}

TEST_CASE(negative_zero_is_read_back_with_its_sign)
{
    auto [store, database, object_store] = store_with_a_database();
    auto writer = TRY_OR_FAIL(database->begin_writer());

    TRY_OR_FAIL(add(*writer, object_store, NUMBER_NEGATIVE_0, "negative"sv));
    EXPECT_EQ(keys_of(*database, object_store)[0].bytes(), NUMBER_NEGATIVE_0.span());

    // It is the same key as 0.
    EXPECT_EQ(value_at(*database, object_store, NUMBER_0.span()), buffer_of("negative"sv));
    EXPECT_EQ(kind_of_failure(add(*writer, object_store, NUMBER_0, "positive"sv)), IndexedDBError::Kind::Constraint);

    TRY_OR_FAIL(put(*writer, object_store, NUMBER_0, "positive"sv));
    EXPECT_EQ(keys_of(*database, object_store).size(), 1u);
    EXPECT_EQ(keys_of(*database, object_store)[0].bytes(), NUMBER_0.span());
    EXPECT_EQ(value_at(*database, object_store, NUMBER_NEGATIVE_0.span()), buffer_of("positive"sv));
}

TEST_CASE(a_range_has_open_and_closed_bounds)
{
    auto [store, database, object_store] = store_with_a_database();
    auto writer = TRY_OR_FAIL(database->begin_upgrade(2));
    for (u64 number = 1; number <= 5; ++number)
        TRY_OR_FAIL(put(*writer, object_store, number_key(number), "value"sv));

    auto two = number_key(2);
    auto four = number_key(4);
    auto keys_in = [&](IndexedDBKeyRange const& range, Direction direction = Direction::Ascending, Optional<u32> limit = {}) {
        Vector<u64> numbers;
        for (auto const& key : keys_of(*database, object_store, range, direction, limit))
            numbers.append(static_cast<u64>(Web::IndexedDB::number_of_key(key.bytes()).release_value()));
        return numbers;
    };
    IndexedDBKeyRange closed { .lower_bound = two.bytes(), .lower_bound_is_open = false, .upper_bound = four.bytes(), .upper_bound_is_open = false };
    IndexedDBKeyRange open { .lower_bound = two.bytes(), .lower_bound_is_open = true, .upper_bound = four.bytes(), .upper_bound_is_open = true };
    IndexedDBKeyRange from_two { .lower_bound = two.bytes(), .lower_bound_is_open = false, .upper_bound = {}, .upper_bound_is_open = false };
    IndexedDBKeyRange below_four { .lower_bound = {}, .lower_bound_is_open = false, .upper_bound = four.bytes(), .upper_bound_is_open = true };

    EXPECT_EQ(keys_in({}), (Vector<u64> { 1, 2, 3, 4, 5 }));
    EXPECT_EQ(keys_in(closed), (Vector<u64> { 2, 3, 4 }));
    EXPECT_EQ(keys_in(open), (Vector<u64> { 3 }));
    EXPECT_EQ(keys_in(from_two), (Vector<u64> { 2, 3, 4, 5 }));
    EXPECT_EQ(keys_in(below_four), (Vector<u64> { 1, 2, 3 }));

    EXPECT_EQ(keys_in({}, Direction::Descending), (Vector<u64> { 5, 4, 3, 2, 1 }));
    EXPECT_EQ(keys_in(closed, Direction::Descending), (Vector<u64> { 4, 3, 2 }));
    EXPECT_EQ(keys_in(closed, Direction::Ascending, 2), (Vector<u64> { 2, 3 }));
    EXPECT_EQ(keys_in(closed, Direction::Descending, 2), (Vector<u64> { 4, 3 }));

    // A count of 0 is no count.
    EXPECT_EQ(keys_in(closed, Direction::Ascending, 0), (Vector<u64> { 2, 3, 4 }));

    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 5u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, closed)), 3u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, open)), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, from_two)), 4u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, below_four)), 3u);

    TRY_OR_FAIL(writer->delete_records(object_store, open));
    EXPECT_EQ(keys_in({}), (Vector<u64> { 1, 2, 4, 5 }));
    TRY_OR_FAIL(writer->delete_records(object_store, from_two));
    EXPECT_EQ(keys_in({}), (Vector<u64> { 1 }));

    auto other_object_store = TRY_OR_FAIL(writer->create_object_store("other"_utf16, {}, KeyGenerator::No));
    TRY_OR_FAIL(put(*writer, other_object_store, number_key(1), "value"sv));
    TRY_OR_FAIL(writer->clear(object_store));
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(other_object_store, {})), 1u);
}

enum class Bound : u8 {
    None,
    Closed,
    Open,
};

// Goes through the nine ranges that a lower and an upper bound of each kind make.
template<typename Callback>
static void for_each_kind_of_range(ReadonlyBytes lower_key, ReadonlyBytes upper_key, Callback callback)
{
    for (auto lower : { Bound::None, Bound::Closed, Bound::Open }) {
        for (auto upper : { Bound::None, Bound::Closed, Bound::Open }) {
            IndexedDBKeyRange range {
                .lower_bound = lower == Bound::None ? Optional<ReadonlyBytes> {} : lower_key,
                .lower_bound_is_open = lower == Bound::Open,
                .upper_bound = upper == Bound::None ? Optional<ReadonlyBytes> {} : upper_key,
                .upper_bound_is_open = upper == Bound::Open,
            };
            callback(range, lower, upper);
        }
    }
}

TEST_CASE(a_range_of_every_shape_counts_reads_and_deletes_its_records)
{
    auto [store, database, object_store] = store_with_a_database();
    auto value_of = [](u64 number) { return MUST(String::formatted("value of {}", number)); };
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        for (u64 number = 1; number <= 5; ++number) {
            auto value = value_of(number);
            TRY_OR_FAIL(put(*writer, object_store, number_key(number), value));
        }
        TRY_OR_FAIL(writer->commit(at(2)));
    }

    auto keys_of_numbers = [](Vector<u64> const& numbers) {
        Vector<ByteBuffer> keys;
        for (auto number : numbers)
            keys.append(number_key(number));
        return keys;
    };
    auto values_of_numbers = [&](Vector<u64> const& numbers) {
        Vector<ByteBuffer> values;
        for (auto number : numbers) {
            auto value = value_of(number);
            values.append(buffer_of(value));
        }
        return values;
    };

    auto two = number_key(2);
    auto four = number_key(4);

    // Every shape is a statement of its own, and all of them are the one database's.
    for_each_kind_of_range(two, four, [&](IndexedDBKeyRange const& range, Bound lower, Bound upper) {
        Vector<u64> inside;
        Vector<u64> outside;
        for (u64 number = 1; number <= 5; ++number) {
            bool is_above_the_lower_bound = lower == Bound::None || number > 2 || (lower == Bound::Closed && number == 2);
            bool is_below_the_upper_bound = upper == Bound::None || number < 4 || (upper == Bound::Closed && number == 4);
            if (is_above_the_lower_bound && is_below_the_upper_bound)
                inside.append(number);
            else
                outside.append(number);
        }
        auto inside_descending = inside;
        inside_descending.reverse();

        EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, range)), inside.size());

        for (auto direction : { Direction::Ascending, Direction::Descending }) {
            auto const& expected = direction == Direction::Ascending ? inside : inside_descending;

            EXPECT_EQ(keys_of(*database, object_store, range, direction), keys_of_numbers(expected));

            Vector<ByteBuffer> keys;
            Vector<ByteBuffer> values;
            for (auto& record : TRY_OR_FAIL(database->read_records(object_store, range, direction, {}, WithValues::Yes))) {
                keys.append(move(record.key));
                values.append(record.value.release_value());
            }
            EXPECT_EQ(keys, keys_of_numbers(expected));
            EXPECT_EQ(values, values_of_numbers(expected));
        }

        // The abort puts the records back for the next shape.
        auto writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(writer->delete_records(object_store, range));
        EXPECT_EQ(keys_of(*database, object_store), keys_of_numbers(outside));
        TRY_OR_FAIL(writer->abort());
    });

    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 5u);
}

TEST_CASE(records_are_sorted_as_their_keys_compare)
{
    auto [store, database, object_store] = store_with_a_database();

    // In the order of the keys: numbers, dates, strings, binary and arrays, and within binary by its bytes, with one
    // that another starts with first.
    Vector<ByteBuffer> keys;
    auto append = [&](auto const& key) { keys.append(MUST(ByteBuffer::copy(bytes_of(key)))); };
    append(NUMBER_MINUS_5);
    append(NUMBER_0);
    append(number_key(1));
    append(NUMBER_20_5);
    append(number_key(TWO_TO_THE_53));
    append(DATE_1);
    append(DATE_2);
    append(STRING_EMPTY);
    append(STRING_A);
    append(STRING_A_NUL);
    append(STRING_AB);
    append(binary_key());
    append(binary_key(0x00));
    append(binary_key(0x00, 0x00));
    append(binary_key(0x00, 0x01));
    append(binary_key(0x00, 0xff));
    append(binary_key(0x01));
    append(binary_key(0x01, 0x00));
    append(binary_key(0x02));
    append(binary_key(0xff));
    append(binary_key(0xff, 0x00));
    append(binary_key(0xff, 0xff));
    append(ARRAY_EMPTY);
    append(ARRAY_OF_1);
    append(ARRAY_OF_AN_EMPTY_ARRAY);

    // 7 shares no factor with the number of keys, so this visits each of them once, out of order.
    VERIFY(keys.size() % 7 != 0);
    auto writer = TRY_OR_FAIL(database->begin_writer());
    for (size_t i = 0; i < keys.size(); ++i)
        TRY_OR_FAIL(add(*writer, object_store, keys[(i * 7) % keys.size()], "value"sv));

    EXPECT_EQ(keys_of(*database, object_store), keys);

    auto keys_reversed = keys;
    keys_reversed.reverse();
    EXPECT_EQ(keys_of(*database, object_store, {}, Direction::Descending), keys_reversed);

    // From the first binary key up to, and not including, the first array.
    IndexedDBKeyRange binary_keys { .lower_bound = keys[11].bytes(), .lower_bound_is_open = false, .upper_bound = keys[22].bytes(), .upper_bound_is_open = true };
    Vector<ByteBuffer> binary_keys_in_order;
    for (size_t i = 11; i < 22; ++i)
        binary_keys_in_order.append(keys[i]);
    EXPECT_EQ(keys_of(*database, object_store, binary_keys), binary_keys_in_order);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, binary_keys)), 11u);
}

TEST_CASE(a_key_generator_counts_up_from_1)
{
    auto [store, database, object_store] = store_with_a_database({}, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_upgrade(2));

    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 1u);
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 2u);
    EXPECT_EQ(keys_of(*database, object_store), (Vector { number_key(1), number_key(2) }));

    // A number that is given as a key moves the generator past it, also when it is the number that is next.
    TRY_OR_FAIL(put(*writer, object_store, number_key(3), "value"sv));
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 4u);
    TRY_OR_FAIL(put(*writer, object_store, number_key(10), "value"sv));
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 11u);
    TRY_OR_FAIL(put(*writer, object_store, NUMBER_20_5, "value"sv));
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 21u);

    // Nothing moves it back, and no key that is not a number moves it.
    TRY_OR_FAIL(put(*writer, object_store, number_key(5), "value"sv));
    TRY_OR_FAIL(put(*writer, object_store, NUMBER_MINUS_5, "value"sv));
    TRY_OR_FAIL(put(*writer, object_store, DATE_1000, "value"sv));
    TRY_OR_FAIL(put(*writer, object_store, ARRAY_OF_1, "value"sv));
    TRY_OR_FAIL(writer->delete_records(object_store, {}));
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 22u);

    // An object store has a generator of its own.
    auto other_object_store = TRY_OR_FAIL(writer->create_object_store("other"_utf16, {}, KeyGenerator::Yes));
    EXPECT_EQ(put_with_a_generated_key(*writer, other_object_store), 1u);
}

TEST_CASE(a_key_generator_stops_after_2_to_the_53)
{
    auto [store, database, object_store] = store_with_a_database({}, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_upgrade(2));

    // Numbers that do not fit 32 bits are generated like any other.
    TRY_OR_FAIL(put(*writer, object_store, number_key((1ULL << 31) - 2), "value"sv));
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), (1ULL << 31) - 1);
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 1ULL << 31);
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), (1ULL << 31) + 1);

    TRY_OR_FAIL(put(*writer, object_store, number_key(TWO_TO_THE_53 - 2), "value"sv));
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), TWO_TO_THE_53 - 1);
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), TWO_TO_THE_53);
    EXPECT_EQ(keys_of(*database, object_store, {}, Direction::Descending, 1)[0], number_key(TWO_TO_THE_53));

    auto past_the_end = writer->store_record(object_store, {}, "value"sv.bytes(), NoOverwrite::No);
    EXPECT_EQ(kind_of_failure(past_the_end), IndexedDBError::Kind::Constraint);
    EXPECT(writer->is_open());

    // A key can still be given.
    TRY_OR_FAIL(put(*writer, object_store, number_key(3), "value"sv));

    // A number past the end, infinity included, ends a generator too.
    auto other_object_store = TRY_OR_FAIL(writer->create_object_store("other"_utf16, {}, KeyGenerator::Yes));
    TRY_OR_FAIL(put(*writer, other_object_store, NUMBER_INFINITY, "value"sv));
    auto after_infinity = writer->store_record(other_object_store, {}, "value"sv.bytes(), NoOverwrite::No);
    EXPECT_EQ(kind_of_failure(after_infinity), IndexedDBError::Kind::Constraint);
}

TEST_CASE(an_abort_puts_a_key_generator_back)
{
    auto [store, database, object_store] = store_with_a_database({}, KeyGenerator::Yes);
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 1u);
        EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 2u);
        TRY_OR_FAIL(writer->commit(at(2)));
    }
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 3u);
        TRY_OR_FAIL(put(*writer, object_store, number_key(100), "value"sv));
        TRY_OR_FAIL(writer->abort());
    }

    auto writer = TRY_OR_FAIL(database->begin_writer());
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 3u);
}

TEST_CASE(a_generated_key_is_put_between_the_parts_of_a_value)
{
    auto [store, database, object_store] = store_with_a_database(WebView::IndexedDBKeyPath { "id"_utf16 }, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_writer());

    WebView::IndexedDBValueSegments segments { .before_the_key = "before"sv.bytes(), .after_the_key = "after"sv.bytes() };
    auto stored = TRY_OR_FAIL(writer->store_record(object_store, {}, segments, NoOverwrite::No));
    EXPECT_EQ(stored.generated_key, 1u);

    // The key is in the form that the serialization of a value has for a number: a double, least significant byte
    // first.
    auto one = AK::convert_between_host_and_little_endian(bit_cast<u64>(1.0));
    ByteBuffer expected_value;
    expected_value.append("before"sv.bytes());
    expected_value.append(&one, sizeof(one));
    expected_value.append("after"sv.bytes());
    EXPECT_EQ(value_at(*database, object_store, number_key(1)), expected_value);

    // A value has no place for the key where its holder drops its properties when serialized, as a Date does. Such
    // a value is whole.
    auto whole = TRY_OR_FAIL(writer->store_record(object_store, {}, "whole"sv.bytes(), NoOverwrite::No));
    EXPECT_EQ(whole.generated_key, 2u);
    EXPECT_EQ(value_at(*database, object_store, number_key(2)), buffer_of("whole"sv));
}

TEST_CASE(the_parts_of_a_value_are_only_taken_where_a_key_is_injected)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
    auto writer = TRY_OR_FAIL(database->begin_upgrade(1));

    auto generator_and_path = TRY_OR_FAIL(writer->create_object_store("generator and path"_utf16, WebView::IndexedDBKeyPath { "id"_utf16 }, KeyGenerator::Yes));
    auto generator_only = TRY_OR_FAIL(writer->create_object_store("generator only"_utf16, {}, KeyGenerator::Yes));
    auto path_only = TRY_OR_FAIL(writer->create_object_store("path only"_utf16, WebView::IndexedDBKeyPath { "id"_utf16 }, KeyGenerator::No));
    auto neither = TRY_OR_FAIL(writer->create_object_store("neither"_utf16, {}, KeyGenerator::No));

    WebView::IndexedDBValueSegments segments { .before_the_key = "before"sv.bytes(), .after_the_key = "after"sv.bytes() };
    auto key = number_key(7);
    auto store_segments = [&](i64 object_store, Optional<ReadonlyBytes> key) {
        return kind_of_failure(writer->store_record(object_store, key, segments, NoOverwrite::No));
    };

    // An object store with out-of-line keys generates a key and injects nothing.
    EXPECT_EQ(store_segments(generator_only, {}), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(put_with_a_generated_key(*writer, generator_only), 1u);

    // A key that is given is not generated.
    EXPECT_EQ(store_segments(generator_and_path, key.bytes()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(store_segments(generator_only, key.bytes()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(store_segments(path_only, key.bytes()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(store_segments(neither, key.bytes()), IndexedDBError::Kind::InvalidClaim);

    // Without a key generator the key has to be given.
    EXPECT_EQ(store_segments(path_only, {}), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(writer->store_record(neither, {}, "value"sv.bytes(), NoOverwrite::No)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(writer->store_record(1000, key.bytes(), "value"sv.bytes(), NoOverwrite::No)), IndexedDBError::Kind::InvalidClaim);

    EXPECT_EQ(store_segments(generator_and_path, {}), Optional<IndexedDBError::Kind> {});
    EXPECT(writer->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->count(generator_and_path, {})), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(generator_only, {})), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(path_only, {})), 0u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(neither, {})), 0u);
}

TEST_CASE(bytes_that_are_not_a_key_are_refused)
{
    auto [store, database, object_store] = store_with_a_database();
    auto writer = TRY_OR_FAIL(database->begin_writer());

    Array<u8, 1> const unknown_tag { 0x60 };
    Array<u8, 3> const truncated_number { 0x10, 0x80, 0x00 };
    Array<u8, 10> const number_and_a_byte { 0x10, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    Array<u8, 1> const generated_key_marker { 0x11 };

    ReadonlyBytes const not_keys[] = { unknown_tag.span(), truncated_number.span(), number_and_a_byte.span(), generated_key_marker.span(), {} };
    for (auto bytes : not_keys) {
        EXPECT_EQ(kind_of_failure(put(*writer, object_store, bytes, "value"sv)), IndexedDBError::Kind::InvalidClaim);

        IndexedDBKeyRange from_it { .lower_bound = bytes, .lower_bound_is_open = false, .upper_bound = {}, .upper_bound_is_open = false };
        IndexedDBKeyRange up_to_it { .lower_bound = {}, .lower_bound_is_open = false, .upper_bound = bytes, .upper_bound_is_open = false };
        EXPECT_EQ(kind_of_failure(database->count(object_store, from_it)), IndexedDBError::Kind::InvalidClaim);
        EXPECT_EQ(kind_of_failure(database->count(object_store, up_to_it)), IndexedDBError::Kind::InvalidClaim);
        EXPECT_EQ(kind_of_failure(database->read_records(object_store, from_it, Direction::Ascending, {}, WithValues::No)), IndexedDBError::Kind::InvalidClaim);
        EXPECT_EQ(kind_of_failure(writer->delete_records(object_store, up_to_it)), IndexedDBError::Kind::InvalidClaim);
    }

    EXPECT(writer->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);
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

// Enough for statements over small records, and not for a cell of megabytes.
template<typename Callback>
static auto with_little_memory_for_sqlite(Callback callback)
{
    return with_room_for_sqlite(128 * KiB, move(callback));
}

template<typename Callback>
static auto with_no_memory_left_for_sqlite(Callback callback)
{
    return with_room_for_sqlite(1, move(callback));
}

// A key is one cell however large it is, so that reading it back takes one allocation of its size.
static ByteBuffer binary_key_of_4_mebibytes()
{
    auto key = MUST(ByteBuffer::create_uninitialized(4 * MiB + 2));
    key.bytes().fill(0x61);
    key[0] = 0x40;
    key[key.size() - 1] = 0x00;
    return key;
}

TEST_CASE(a_read_that_fails_can_lose_the_writer)
{
    auto [store, database, object_store] = store_with_a_database();
    auto large_key = binary_key_of_4_mebibytes();
    i64 other_object_store = 0;
    {
        auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
        other_object_store = TRY_OR_FAIL(upgrade->create_object_store("other"_utf16, {}, KeyGenerator::No));
        TRY_OR_FAIL(put(*upgrade, other_object_store, large_key, "value"sv));
        TRY_OR_FAIL(upgrade->commit(at(2)));
    }
    auto read_the_other_object_store = [&] {
        return database->read_records(other_object_store, {}, Direction::Ascending, {}, WithValues::No);
    };

    // With memory to spare, a read while a writer is at work takes nothing from it.
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));
        EXPECT_EQ(TRY_OR_FAIL(read_the_other_object_store())[0].key, large_key);
        EXPECT(writer->is_open());
        TRY_OR_FAIL(writer->abort());
    }

    auto writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));

    auto failed_read = with_little_memory_for_sqlite(read_the_other_object_store);
    EXPECT_EQ(kind_of_failure(failed_read), IndexedDBError::Kind::OutOfMemory);
    EXPECT(failed_read.error().lost_the_writer);

    // The writer is known to be lost before it is asked for anything else.
    EXPECT(!writer->is_open());
    EXPECT(!database->has_writer());
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);

    auto request_of_the_lost_writer = put(*writer, object_store, number_key(2), "value"sv);
    EXPECT_EQ(kind_of_failure(request_of_the_lost_writer), IndexedDBError::Kind::TransactionLost);
    EXPECT(!request_of_the_lost_writer.error().lost_the_writer);
    EXPECT_EQ(kind_of_failure(writer->commit(at(3))), IndexedDBError::Kind::TransactionLost);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);

    auto next_writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*next_writer, object_store, number_key(3), "value"sv));
    TRY_OR_FAIL(next_writer->commit(at(4)));
    EXPECT_EQ(keys_of(*database, object_store), (Vector { number_key(3) }));
    EXPECT_EQ(TRY_OR_FAIL(read_the_other_object_store())[0].key, large_key);
}

TEST_CASE(a_change_to_the_object_stores_that_fails_ends_the_upgrade)
{
    auto [store, database, object_store] = store_with_a_database();

    // SQLite has no room for a name of this size.
    auto large_name = Utf16String::repeated('a', 4 * MiB);
    auto expect_the_upgrade_to_end = [&](auto change_the_object_stores) {
        auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
        TRY_OR_FAIL(put(*upgrade, object_store, number_key(1), "value"sv));

        auto failed_change = with_little_memory_for_sqlite([&] { return change_the_object_stores(*upgrade); });
        EXPECT_EQ(kind_of_failure(failed_change), IndexedDBError::Kind::OutOfMemory);
        EXPECT(failed_change.error().lost_the_writer);

        // The renderer has made the change on its side, so nothing of the upgrade is kept.
        EXPECT(!upgrade->is_open());
        EXPECT(!database->has_writer());
        EXPECT_EQ(database->version(), 1u);
        EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);

        auto object_stores = TRY_OR_FAIL(database->object_stores());
        EXPECT_EQ(object_stores.size(), 1u);
        EXPECT_EQ(object_stores[0].name, "store"_utf16);

        EXPECT_EQ(kind_of_failure(upgrade->commit(at(2))), IndexedDBError::Kind::TransactionLost);
    };

    expect_the_upgrade_to_end([&](IndexedDBDatabase::Writer& upgrade) -> ErrorOr<void, IndexedDBError> {
        TRY(upgrade.create_object_store(large_name, {}, KeyGenerator::No));
        return {};
    });
    expect_the_upgrade_to_end([&](IndexedDBDatabase::Writer& upgrade) {
        return upgrade.rename_object_store(object_store, large_name);
    });

    // A change that is refused is the renderer's to answer for, and takes nothing from the upgrade.
    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
    auto refused_change = upgrade->delete_object_store(1000);
    EXPECT_EQ(kind_of_failure(refused_change), IndexedDBError::Kind::InvalidClaim);
    EXPECT(!refused_change.error().lost_the_writer);
    EXPECT(upgrade->is_open());
    TRY_OR_FAIL(upgrade->commit(at(2)));
    EXPECT_EQ(database->version(), 2u);
}

TEST_CASE(a_request_that_fails_undoes_what_it_changed)
{
    auto [store, database, object_store] = store_with_a_database({}, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_writer());
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 1u);
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 2u);

    // SQLite has no room for a value of this size. By the time it is asked to store it, the request has taken a key
    // from the generator, or has deleted the record that it replaces.
    auto const large_value = MUST(ByteBuffer::create_zeroed(4 * MiB));

    auto generating_a_key = with_little_memory_for_sqlite([&] {
        return writer->store_record(object_store, {}, large_value.bytes(), NoOverwrite::No);
    });
    EXPECT_EQ(kind_of_failure(generating_a_key), IndexedDBError::Kind::OutOfMemory);
    EXPECT(!generating_a_key.error().lost_the_writer);

    auto first_key = number_key(1);
    auto replacing_a_record = with_little_memory_for_sqlite([&] {
        return writer->store_record(object_store, first_key.bytes(), large_value.bytes(), NoOverwrite::No);
    });
    EXPECT_EQ(kind_of_failure(replacing_a_record), IndexedDBError::Kind::OutOfMemory);
    EXPECT(!replacing_a_record.error().lost_the_writer);

    EXPECT(writer->is_open());
    EXPECT_EQ(value_at(*database, object_store, first_key), buffer_of("value"sv));
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 3u);
}

TEST_CASE(a_writer_dropped_while_its_rollback_fails_is_rolled_back_before_the_next_read)
{
    auto [store, database, object_store] = store_with_a_database();
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(put(*writer, object_store, number_key(1), "committed"sv));
        TRY_OR_FAIL(writer->commit(at(10)));
    }

    // A writer that is let go of does not say whether its rollback failed, so one that is aborted goes first.
    {
        auto aborted_writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(put(*aborted_writer, object_store, number_key(1), "not committed"sv));

        auto failed_abort = with_no_memory_left_for_sqlite([&] { return aborted_writer->abort(); });
        EXPECT_EQ(kind_of_failure(failed_abort), IndexedDBError::Kind::OutOfMemory);
    }

    OwnPtr<IndexedDBDatabase::Writer> writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "not committed"sv));
    TRY_OR_FAIL(put(*writer, object_store, number_key(2), "not committed"sv));
    TRY_OR_FAIL(database->mark_opened(at(20)));

    with_no_memory_left_for_sqlite([&] { writer = nullptr; });
    EXPECT(!database->has_writer());

    EXPECT_EQ(keys_of(*database, object_store), (Vector { number_key(1) }));
    EXPECT_EQ(value_at(*database, object_store, number_key(1)), buffer_of("committed"sv));

    // The open at 20 could not be written while the rollback was still to do. A later open takes its place, so that
    // the next writer to finish does not write the earlier time over it.
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(10));
    TRY_OR_FAIL(database->mark_opened(at(30)));
    {
        auto aborted_writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(aborted_writer->abort());
    }
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(30));

    auto next_writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*next_writer, object_store, number_key(3), "value"sv));
    TRY_OR_FAIL(next_writer->commit(at(40)));
    EXPECT_EQ(keys_of(*database, object_store), (Vector { number_key(1), number_key(3) }));
}

TEST_CASE(a_database_keeps_the_time_of_its_last_open_or_write)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto open = [&](Optional<u64> version, UnixDateTime now) {
        return MUST(store->open_database(example_com(), "database"_utf16, version, now)).database;
    };

    auto database = open(1, at(10));
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(10));

    i64 object_store = 0;
    {
        auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));
        object_store = TRY_OR_FAIL(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
        TRY_OR_FAIL(upgrade->commit(at(20)));
    }
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(20));

    // An open that needs no upgrade counts, and one that needs one leaves the time to the upgrade's commit.
    (void)open({}, at(30));
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(30));
    (void)open(2, at(40));
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(30));

    // Reading does not count, and neither does a writer that is aborted.
    TRY_OR_FAIL(database->count(object_store, {}));
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));
        TRY_OR_FAIL(writer->abort());
    }
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(30));

    // An open while a writer is at work still counts when the writer is aborted.
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));
        (void)open({}, at(50));
        EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(30));
        TRY_OR_FAIL(writer->abort());
    }
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(50));

    // The later of such an open and the writer's commit is kept.
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        (void)open({}, at(70));
        TRY_OR_FAIL(writer->commit(at(60)));
    }
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(70));
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        (void)open({}, at(80));
        TRY_OR_FAIL(writer->commit(at(90)));
    }
    EXPECT_EQ(TRY_OR_FAIL(database->last_open_or_write_time()), at(90));
}

TEST_CASE(a_failed_allocation_is_told_apart_from_other_failures)
{
    auto kind_of = [](Database::Error error) { return IndexedDBError::from(move(error)).kind; };

    EXPECT_EQ(kind_of(Database::Error::from_result_code(to_underlying(Database::ResultCode::NoMemory))), IndexedDBError::Kind::OutOfMemory);
    EXPECT_EQ(kind_of(AK::Error::from_errno(ENOMEM)), IndexedDBError::Kind::OutOfMemory);
    EXPECT_EQ(kind_of(Database::Error::from_result_code(to_underlying(Database::ResultCode::Full))), IndexedDBError::Kind::Unknown);
    EXPECT_EQ(kind_of(AK::Error::from_errno(EIO)), IndexedDBError::Kind::Unknown);
    EXPECT_EQ(kind_of(Database::Error::from_string_literal("Some other failure")), IndexedDBError::Kind::Unknown);
}
