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
using MultiEntry = IndexedDBDatabase::MultiEntry;
using NoOverwrite = IndexedDBDatabase::NoOverwrite;
using RecordsOfEachKey = IndexedDBDatabase::RecordsOfEachKey;
using Unique = IndexedDBDatabase::Unique;
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

// Bytes that are no stored key. The marker stands in an index key for the key that a database generates.
static constexpr Array<u8, 1> UNKNOWN_TAG { 0x60 };
static constexpr Array<u8, 1> GENERATED_KEY_MARKER { 0x11 };
static constexpr Array<u8, 3> ARRAY_OF_A_GENERATED_KEY_MARKER { 0x50, 0x11, 0x00 };

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

// The whole number that a key is the key of.
static u64 number_of(ReadonlyBytes key)
{
    return static_cast<u64>(Web::IndexedDB::number_of_key(key).release_value());
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

// A value of the given length. Its bytes repeat every 251, which the length of a chunk is no multiple of, so the few
// chunks of a value in these tests differ from one another, and do not read back as the value when they are out of
// order.
static ByteBuffer value_of_length(size_t length, u8 first_byte)
{
    static_assert(IndexedDBDatabase::VALUE_CHUNK_SIZE % 251 != 0);

    auto value = MUST(ByteBuffer::create_uninitialized(length));
    for (size_t i = 0; i < length; ++i)
        value[i] = static_cast<u8>(first_byte + (i % 251));
    return value;
}

// A comparison that does not print megabytes where the two differ.
static bool is_the_value(Optional<ByteBuffer> const& read_back, ReadonlyBytes expected)
{
    return read_back.has_value() && read_back->bytes() == expected;
}

static auto put(IndexedDBDatabase::Writer& writer, i64 object_store, auto const& key, StringView value)
{
    return writer.store_record(object_store, bytes_of(key), value.bytes(), NoOverwrite::No, {});
}

static auto add(IndexedDBDatabase::Writer& writer, i64 object_store, auto const& key, StringView value)
{
    return writer.store_record(object_store, bytes_of(key), value.bytes(), NoOverwrite::Yes, {});
}

// Stores a record whose key the object store generates, and returns that key.
static u64 put_with_a_generated_key(IndexedDBDatabase::Writer& writer, i64 object_store)
{
    return MUST(writer.store_record(object_store, {}, "value"sv.bytes(), NoOverwrite::No, {})).generated_key.release_value();
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
    EXPECT_EQ(create_other(WebView::IndexedDBKeyPath { Vector { "a"_utf16, odd_name } }, KeyGenerator::No), IndexedDBError::Kind::InvalidClaim);
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
            numbers.append(number_of(key));
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

    auto past_the_end = writer->store_record(object_store, {}, "value"sv.bytes(), NoOverwrite::No, {});
    EXPECT_EQ(kind_of_failure(past_the_end), IndexedDBError::Kind::Constraint);
    EXPECT(writer->is_open());

    // A key can still be given.
    TRY_OR_FAIL(put(*writer, object_store, number_key(3), "value"sv));

    // A number past the end, infinity included, ends a generator too.
    auto other_object_store = TRY_OR_FAIL(writer->create_object_store("other"_utf16, {}, KeyGenerator::Yes));
    TRY_OR_FAIL(put(*writer, other_object_store, NUMBER_INFINITY, "value"sv));
    auto after_infinity = writer->store_record(other_object_store, {}, "value"sv.bytes(), NoOverwrite::No, {});
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
    auto stored = TRY_OR_FAIL(writer->store_record(object_store, {}, segments, NoOverwrite::No, {}));
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
    auto whole = TRY_OR_FAIL(writer->store_record(object_store, {}, "whole"sv.bytes(), NoOverwrite::No, {}));
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
        return kind_of_failure(writer->store_record(object_store, key, segments, NoOverwrite::No, {}));
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
    EXPECT_EQ(kind_of_failure(writer->store_record(neither, {}, "value"sv.bytes(), NoOverwrite::No, {})), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(writer->store_record(1000, key.bytes(), "value"sv.bytes(), NoOverwrite::No, {})), IndexedDBError::Kind::InvalidClaim);

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

    Array<u8, 3> const truncated_number { 0x10, 0x80, 0x00 };
    Array<u8, 10> const number_and_a_byte { 0x10, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    ReadonlyBytes const not_keys[] = { UNKNOWN_TAG.span(), truncated_number.span(), number_and_a_byte.span(), GENERATED_KEY_MARKER.span(), {} };
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

// Enough for statements over small records, and not for a cell of a mebibyte or more.
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

// A binary key of that many bytes.
static ByteBuffer binary_key_of_length(size_t length)
{
    auto key = MUST(ByteBuffer::create_uninitialized(length + 2));
    key.bytes().fill(0x61);
    key[0] = 0x40;
    key[key.size() - 1] = 0x00;
    return key;
}

// A key is one cell however large it is, so that reading it back takes one allocation of its size.
static ByteBuffer binary_key_of_4_mebibytes()
{
    return binary_key_of_length(4 * MiB);
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

    // SQLite has no room for a value of this size, nor for one chunk of it. By the time it is asked to store it, the
    // request has taken a key from the generator, or has deleted the record that it replaces.
    auto const large_value = MUST(ByteBuffer::create_zeroed(4 * MiB));

    auto generating_a_key = with_little_memory_for_sqlite([&] {
        return writer->store_record(object_store, {}, large_value.bytes(), NoOverwrite::No, {});
    });
    EXPECT_EQ(kind_of_failure(generating_a_key), IndexedDBError::Kind::OutOfMemory);
    EXPECT(!generating_a_key.error().lost_the_writer);

    auto first_key = number_key(1);
    auto replacing_a_record = with_little_memory_for_sqlite([&] {
        return writer->store_record(object_store, first_key.bytes(), large_value.bytes(), NoOverwrite::No, {});
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
    EXPECT_EQ(kind_of(Database::Error::from_result_code(to_underlying(Database::ResultCode::TooBig))), IndexedDBError::Kind::Unknown);
    EXPECT_EQ(kind_of(AK::Error::from_errno(EIO)), IndexedDBError::Kind::Unknown);
    EXPECT_EQ(kind_of(Database::Error::from_string_literal("Some other failure")), IndexedDBError::Kind::Unknown);
}

struct StoreWithAnIndex {
    NonnullOwnPtr<IndexedDBStore> store;
    NonnullRefPtr<IndexedDBDatabase> database;
    i64 object_store { 0 };
    i64 index { 0 };
};

// A store with a database at version 1, which has one object store with one index.
static StoreWithAnIndex store_with_an_index(Unique unique = Unique::No, MultiEntry multi_entry = MultiEntry::No, Optional<WebView::IndexedDBKeyPath> key_path_of_object_store = {}, KeyGenerator key_generator = KeyGenerator::No)
{
    auto store = MUST(IndexedDBStore::create_transient());
    auto database = MUST(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;

    auto upgrade = MUST(database->begin_upgrade(1));
    auto object_store = MUST(upgrade->create_object_store("store"_utf16, key_path_of_object_store, key_generator));
    auto index = MUST(upgrade->create_index(object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, unique, multi_entry));
    MUST(upgrade->commit(at(1)));

    return { move(store), move(database), object_store, index };
}

// Stores a record with its keys in one index. Without a key, the object store generates one.
static auto store_with_index_keys(IndexedDBDatabase::Writer& writer, i64 object_store, Optional<ReadonlyBytes> key, WebView::IndexedDBValue const& value, i64 index, ReadonlySpan<ReadonlyBytes> keys)
{
    Array index_keys { WebView::IndexedDBIndexKeys { .index_id = index, .keys = keys } };
    return writer.store_record(object_store, key, value, NoOverwrite::No, index_keys.span());
}

static auto put_with_index_keys(IndexedDBDatabase::Writer& writer, i64 object_store, auto const& key, StringView value, i64 index, ReadonlySpan<ReadonlyBytes> keys)
{
    return store_with_index_keys(writer, object_store, bytes_of(key), value.bytes(), index, keys);
}

static auto put_with_index_key(IndexedDBDatabase::Writer& writer, i64 object_store, auto const& key, StringView value, i64 index, auto const& index_key)
{
    Array keys { bytes_of(index_key) };
    return put_with_index_keys(writer, object_store, key, value, index, keys.span());
}

static auto fill_index(IndexedDBDatabase::Writer& upgrade, i64 index, auto const& key_of_record, ReadonlySpan<ReadonlyBytes> keys)
{
    return upgrade.fill_index(index, bytes_of(key_of_record), keys);
}

// A record of an index as "key/primary key", where both are whole numbers.
static String key_and_primary_key(u64 key, u64 primary_key)
{
    return MUST(String::formatted("{}/{}", key, primary_key));
}

static Vector<String> records_of_index(IndexedDBDatabase& database, i64 index, IndexedDBKeyRange const& range = {}, Direction direction = Direction::Ascending, RecordsOfEachKey records_of_each_key = RecordsOfEachKey::All, Optional<u32> count = {})
{
    Vector<String> records;
    for (auto const& record : MUST(database.read_index_records(index, range, direction, records_of_each_key, count, WithValues::No))) {
        EXPECT(!record.value.has_value());
        records.append(key_and_primary_key(number_of(record.key), number_of(record.primary_key)));
    }
    return records;
}

TEST_CASE(indexes_are_created_renamed_and_deleted)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));

    auto first_store = TRY_OR_FAIL(upgrade->create_object_store("first"_utf16, {}, KeyGenerator::No));
    TRY_OR_FAIL(upgrade->create_object_store("without indexes"_utf16, {}, KeyGenerator::No));
    auto last_store = TRY_OR_FAIL(upgrade->create_object_store("last"_utf16, {}, KeyGenerator::No));

    // An object store has index names of its own. The indexes are not created in the order of their object stores.
    auto by_a_of_last = TRY_OR_FAIL(upgrade->create_index(last_store, "by a"_utf16, WebView::IndexedDBKeyPath { Vector { "a"_utf16, "b"_utf16 } }, Unique::No, MultiEntry::No));
    auto by_a = TRY_OR_FAIL(upgrade->create_index(first_store, "by a"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::Yes, MultiEntry::No));
    auto by_tags = TRY_OR_FAIL(upgrade->create_index(first_store, "by tags"_utf16, WebView::IndexedDBKeyPath { "tags"_utf16 }, Unique::No, MultiEntry::Yes));
    {
        auto object_stores = TRY_OR_FAIL(database->object_stores());
        EXPECT_EQ(object_stores.size(), 3u);

        auto const& indexes_of_first = object_stores[0].indexes;
        EXPECT_EQ(indexes_of_first.size(), 2u);
        EXPECT_EQ(indexes_of_first[0].id, by_a);
        EXPECT_EQ(indexes_of_first[0].name, "by a"_utf16);
        EXPECT_EQ(indexes_of_first[0].key_path.get<Utf16String>(), "a"_utf16);
        EXPECT(indexes_of_first[0].is_unique);
        EXPECT(!indexes_of_first[0].is_multi_entry);
        EXPECT_EQ(indexes_of_first[1].id, by_tags);
        EXPECT_EQ(indexes_of_first[1].name, "by tags"_utf16);
        EXPECT(!indexes_of_first[1].is_unique);
        EXPECT(indexes_of_first[1].is_multi_entry);

        EXPECT(object_stores[1].indexes.is_empty());

        auto const& indexes_of_last = object_stores[2].indexes;
        EXPECT_EQ(indexes_of_last.size(), 1u);
        EXPECT_EQ(indexes_of_last[0].id, by_a_of_last);
        EXPECT_EQ(indexes_of_last[0].name, "by a"_utf16);
        EXPECT_EQ(indexes_of_last[0].key_path.get<Vector<Utf16String>>(), (Vector { "a"_utf16, "b"_utf16 }));
    }

    // A renderer that runs LibWeb throws for each of these before it sends anything.
    char16_t const path_with_a_lone_surrogate[] = { u'a', 0xd800 };
    auto odd_path = Utf16String::from_utf16(Utf16View { path_with_a_lone_surrogate, 2 });
    auto create_other = [&](i64 object_store, WebView::IndexedDBKeyPath key_path, MultiEntry multi_entry) {
        return kind_of_failure(upgrade->create_index(object_store, "other"_utf16, key_path, Unique::No, multi_entry));
    };
    auto a_path = WebView::IndexedDBKeyPath { "a"_utf16 };
    EXPECT_EQ(kind_of_failure(upgrade->create_index(first_store, "by a"_utf16, a_path, Unique::No, MultiEntry::No)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(1000, a_path, MultiEntry::No), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(first_store, WebView::IndexedDBKeyPath { Vector<Utf16String> {} }, MultiEntry::No), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(first_store, WebView::IndexedDBKeyPath { Vector { "a"_utf16 } }, MultiEntry::Yes), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(create_other(first_store, WebView::IndexedDBKeyPath { odd_path }, MultiEntry::No), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(upgrade->rename_index(by_a, "by tags"_utf16)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(upgrade->rename_index(1000, "other"_utf16)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(upgrade->delete_index(1000)), IndexedDBError::Kind::InvalidClaim);
    EXPECT(upgrade->is_open());

    TRY_OR_FAIL(upgrade->rename_index(by_tags, "renamed"_utf16));
    TRY_OR_FAIL(upgrade->delete_index(by_a));

    // The indexes of an object store go with it.
    TRY_OR_FAIL(upgrade->delete_object_store(last_store));
    EXPECT_EQ(kind_of_failure(upgrade->rename_index(by_a_of_last, "other"_utf16)), IndexedDBError::Kind::InvalidClaim);

    TRY_OR_FAIL(put(*upgrade, first_store, number_key(1), "value"sv));
    TRY_OR_FAIL(upgrade->commit(at(1)));

    auto object_stores = TRY_OR_FAIL(database->object_stores());
    EXPECT_EQ(object_stores.size(), 2u);
    EXPECT_EQ(object_stores[0].indexes.size(), 1u);
    EXPECT_EQ(object_stores[0].indexes[0].id, by_tags);
    EXPECT_EQ(object_stores[0].indexes[0].name, "renamed"_utf16);
    EXPECT(object_stores[1].indexes.is_empty());

    // Changes to the indexes are an upgrade's alone.
    auto writer = TRY_OR_FAIL(database->begin_writer());
    EXPECT_EQ(kind_of_failure(writer->create_index(first_store, "other"_utf16, a_path, Unique::No, MultiEntry::No)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(writer->rename_index(by_tags, "other"_utf16)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(writer->delete_index(by_tags)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(fill_index(*writer, by_tags, number_key(1), {})), IndexedDBError::Kind::InvalidClaim);
    EXPECT(writer->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->object_stores())[0].indexes[0].name, "renamed"_utf16);
}

TEST_CASE(an_index_orders_its_records_by_key_and_then_by_primary_key)
{
    auto [store, database, object_store, index] = store_with_an_index();
    auto writer = TRY_OR_FAIL(database->begin_writer());

    // Records 1 and 2 have the index key 10, 3 and 4 have 20, 5 and 6 have 30. They are stored in another order.
    for (u64 primary_key : { 6, 1, 4, 5, 2, 3 }) {
        auto index_key = number_key(((primary_key + 1) / 2) * 10);
        auto value = MUST(String::formatted("value of {}", primary_key));
        TRY_OR_FAIL(put_with_index_key(*writer, object_store, number_key(primary_key), value, index, index_key));
    }

    auto read = [&](Direction direction, RecordsOfEachKey records_of_each_key, Optional<u32> count = {}) {
        return records_of_index(*database, index, {}, direction, records_of_each_key, count);
    };
    EXPECT_EQ(read(Direction::Ascending, RecordsOfEachKey::All), (Vector { "10/1"_string, "10/2"_string, "20/3"_string, "20/4"_string, "30/5"_string, "30/6"_string }));
    EXPECT_EQ(read(Direction::Descending, RecordsOfEachKey::All), (Vector { "30/6"_string, "30/5"_string, "20/4"_string, "20/3"_string, "10/2"_string, "10/1"_string }));

    // In either direction the first record of a key is the one with the lowest primary key.
    EXPECT_EQ(read(Direction::Ascending, RecordsOfEachKey::First), (Vector { "10/1"_string, "20/3"_string, "30/5"_string }));
    EXPECT_EQ(read(Direction::Descending, RecordsOfEachKey::First), (Vector { "30/5"_string, "20/3"_string, "10/1"_string }));

    // A count is taken from where the direction starts.
    EXPECT_EQ(read(Direction::Ascending, RecordsOfEachKey::All, 1), (Vector { "10/1"_string }));
    EXPECT_EQ(read(Direction::Descending, RecordsOfEachKey::All, 1), (Vector { "30/6"_string }));
    EXPECT_EQ(read(Direction::Ascending, RecordsOfEachKey::First, 2), (Vector { "10/1"_string, "20/3"_string }));
    EXPECT_EQ(read(Direction::Descending, RecordsOfEachKey::First, 1), (Vector { "30/5"_string }));
    EXPECT_EQ(read(Direction::Descending, RecordsOfEachKey::First, 2), (Vector { "30/5"_string, "20/3"_string }));
    EXPECT_EQ(read(Direction::Ascending, RecordsOfEachKey::All, 0).size(), 6u);

    // A range is one of index keys.
    auto const ten = number_key(10);
    auto const twenty = number_key(20);
    auto const thirty = number_key(30);
    IndexedDBKeyRange above_ten { .lower_bound = ten.bytes(), .lower_bound_is_open = true, .upper_bound = {}, .upper_bound_is_open = false };
    IndexedDBKeyRange up_to_twenty { .lower_bound = {}, .lower_bound_is_open = false, .upper_bound = twenty.bytes(), .upper_bound_is_open = false };
    IndexedDBKeyRange below_thirty { .lower_bound = ten.bytes(), .lower_bound_is_open = false, .upper_bound = thirty.bytes(), .upper_bound_is_open = true };
    EXPECT_EQ(records_of_index(*database, index, only(twenty)), (Vector { "20/3"_string, "20/4"_string }));
    EXPECT_EQ(records_of_index(*database, index, above_ten), (Vector { "20/3"_string, "20/4"_string, "30/5"_string, "30/6"_string }));
    EXPECT_EQ(records_of_index(*database, index, up_to_twenty, Direction::Descending), (Vector { "20/4"_string, "20/3"_string, "10/2"_string, "10/1"_string }));
    EXPECT_EQ(records_of_index(*database, index, below_thirty, Direction::Descending, RecordsOfEachKey::First, 1), (Vector { "20/3"_string }));

    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 6u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, only(twenty))), 2u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, above_ten)), 4u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, below_thirty)), 4u);

    // A record of an index has the value of the record that it refers to.
    auto last = TRY_OR_FAIL(database->read_index_records(index, {}, Direction::Descending, RecordsOfEachKey::All, 1, WithValues::Yes));
    EXPECT_EQ(last.size(), 1u);
    EXPECT_EQ(last[0].key, thirty);
    EXPECT_EQ(last[0].primary_key, number_key(6));
    EXPECT_EQ(last[0].value, buffer_of("value of 6"sv));

    auto first_of_twenty = TRY_OR_FAIL(database->read_index_records(index, only(twenty), Direction::Ascending, RecordsOfEachKey::First, {}, WithValues::Yes));
    EXPECT_EQ(first_of_twenty.size(), 1u);
    EXPECT_EQ(first_of_twenty[0].value, buffer_of("value of 3"sv));

    // Both keys are read back as they were written, with or without the value.
    TRY_OR_FAIL(put_with_index_key(*writer, object_store, NUMBER_NEGATIVE_0, "value of -0"sv, index, NUMBER_NEGATIVE_0));
    for (auto with_values : { WithValues::No, WithValues::Yes }) {
        auto at_zero = TRY_OR_FAIL(database->read_index_records(index, only(NUMBER_0), Direction::Ascending, RecordsOfEachKey::All, {}, with_values));
        EXPECT_EQ(at_zero.size(), 1u);
        if (at_zero.size() != 1)
            return;

        EXPECT_EQ(at_zero[0].key.bytes(), NUMBER_NEGATIVE_0.span());
        EXPECT_EQ(at_zero[0].primary_key.bytes(), NUMBER_NEGATIVE_0.span());
        if (with_values == WithValues::Yes)
            EXPECT_EQ(at_zero[0].value, buffer_of("value of -0"sv));
    }
}

TEST_CASE(an_index_only_looks_at_its_own_records)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));

    auto object_store = TRY_OR_FAIL(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
    auto first_index = TRY_OR_FAIL(upgrade->create_index(object_store, "first"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));
    auto second_index = TRY_OR_FAIL(upgrade->create_index(object_store, "second"_utf16, WebView::IndexedDBKeyPath { "b"_utf16 }, Unique::Yes, MultiEntry::No));

    // Record 1 has the key 7 in the first index. That is not in the way of record 2 having it in the second index,
    // which is unique.
    auto const seven = number_key(7);
    TRY_OR_FAIL(put_with_index_key(*upgrade, object_store, number_key(1), "value"sv, first_index, seven));
    TRY_OR_FAIL(put_with_index_key(*upgrade, object_store, number_key(2), "value"sv, second_index, seven));

    // Nor does record 1 come before record 2 among the records that the second index has of the key.
    EXPECT_EQ(records_of_index(*database, second_index, {}, Direction::Ascending, RecordsOfEachKey::First), (Vector { "7/2"_string }));
    EXPECT_EQ(records_of_index(*database, second_index, {}, Direction::Descending, RecordsOfEachKey::First), (Vector { "7/2"_string }));
    EXPECT_EQ(records_of_index(*database, first_index), (Vector { "7/1"_string }));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(second_index, {})), 1u);
}

TEST_CASE(an_index_is_of_one_object_store)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));

    // The indexes are made in the other order than their object stores, so that no index has the id of its own.
    auto first_object_store = TRY_OR_FAIL(upgrade->create_object_store("first"_utf16, {}, KeyGenerator::No));
    auto second_object_store = TRY_OR_FAIL(upgrade->create_object_store("second"_utf16, {}, KeyGenerator::No));
    auto index_of_the_second = TRY_OR_FAIL(upgrade->create_index(second_object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));
    auto index_of_the_first = TRY_OR_FAIL(upgrade->create_index(first_object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));
    EXPECT_NE(index_of_the_first, first_object_store);
    EXPECT_NE(index_of_the_second, second_object_store);

    EXPECT_EQ(TRY_OR_FAIL(database->object_store_of_index(index_of_the_first)), first_object_store);
    EXPECT_EQ(TRY_OR_FAIL(database->object_store_of_index(index_of_the_second)), second_object_store);

    TRY_OR_FAIL(upgrade->delete_index(index_of_the_first));
    EXPECT_EQ(kind_of_failure(database->object_store_of_index(index_of_the_first)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(database->object_store_of_index(1000)), IndexedDBError::Kind::InvalidClaim);
}

TEST_CASE(the_records_of_an_index_go_with_the_record_that_they_refer_to)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));

    auto object_store = TRY_OR_FAIL(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
    auto index = TRY_OR_FAIL(upgrade->create_index(object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));
    auto other_index = TRY_OR_FAIL(upgrade->create_index(object_store, "other index"_utf16, WebView::IndexedDBKeyPath { "b"_utf16 }, Unique::No, MultiEntry::No));

    for (u64 number = 1; number <= 4; ++number) {
        auto const key = number_key(number);
        Array keys { key.bytes() };
        Array index_keys {
            WebView::IndexedDBIndexKeys { .index_id = index, .keys = keys.span() },
            WebView::IndexedDBIndexKeys { .index_id = other_index, .keys = keys.span() },
        };
        TRY_OR_FAIL(upgrade->store_record(object_store, key.bytes(), "value"sv.bytes(), NoOverwrite::No, index_keys.span()));
    }
    EXPECT_EQ(records_of_index(*database, index), (Vector { "1/1"_string, "2/2"_string, "3/3"_string, "4/4"_string }));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(other_index, {})), 4u);

    // A record that is replaced has the keys of its new value, and none where it names no index.
    TRY_OR_FAIL(put_with_index_key(*upgrade, object_store, number_key(1), "value"sv, index, number_key(10)));
    EXPECT_EQ(records_of_index(*database, index), (Vector { "2/2"_string, "3/3"_string, "4/4"_string, "10/1"_string }));
    EXPECT_EQ(records_of_index(*database, other_index), (Vector { "2/2"_string, "3/3"_string, "4/4"_string }));

    auto two = number_key(2);
    auto three = number_key(3);
    IndexedDBKeyRange two_to_three { .lower_bound = two.bytes(), .lower_bound_is_open = false, .upper_bound = three.bytes(), .upper_bound_is_open = false };
    TRY_OR_FAIL(upgrade->delete_records(object_store, two_to_three));
    EXPECT_EQ(records_of_index(*database, index), (Vector { "4/4"_string, "10/1"_string }));
    EXPECT_EQ(records_of_index(*database, other_index), (Vector { "4/4"_string }));

    // An index that is deleted takes its records and no others.
    TRY_OR_FAIL(upgrade->delete_index(other_index));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(other_index, {})), 0u);
    EXPECT_EQ(records_of_index(*database, index), (Vector { "4/4"_string, "10/1"_string }));
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 2u);

    TRY_OR_FAIL(upgrade->clear(object_store));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 0u);

    TRY_OR_FAIL(put_with_index_key(*upgrade, object_store, number_key(1), "value"sv, index, number_key(10)));
    TRY_OR_FAIL(upgrade->delete_object_store(object_store));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 0u);
}

TEST_CASE(a_unique_index_refuses_a_key_that_another_record_has)
{
    auto [store, database, object_store, index] = store_with_an_index(Unique::Yes, MultiEntry::No, {}, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_writer());
    auto const ten = number_key(10);
    Array keys { ten.bytes() };

    TRY_OR_FAIL(put_with_index_key(*writer, object_store, number_key(1), "first"sv, index, ten));

    auto refused = put_with_index_key(*writer, object_store, number_key(2), "second"sv, index, ten);
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Constraint);
    EXPECT(!refused.error().lost_the_writer);
    EXPECT(writer->is_open());
    EXPECT_EQ(keys_of(*database, object_store), (Vector { number_key(1) }));
    EXPECT_EQ(records_of_index(*database, index), (Vector { "10/1"_string }));

    // A request that is refused by an index gives back the key that it generated.
    auto refused_with_a_generated_key = store_with_index_keys(*writer, object_store, {}, "second"sv.bytes(), index, keys.span());
    EXPECT_EQ(kind_of_failure(refused_with_a_generated_key), IndexedDBError::Kind::Constraint);
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 2u);

    // -0 and 0 are one key.
    TRY_OR_FAIL(put_with_index_key(*writer, object_store, number_key(3), "third"sv, index, NUMBER_NEGATIVE_0));
    EXPECT_EQ(kind_of_failure(put_with_index_key(*writer, object_store, number_key(4), "fourth"sv, index, NUMBER_0)), IndexedDBError::Kind::Constraint);

    // A record is not in the way of its own replacement.
    TRY_OR_FAIL(put_with_index_key(*writer, object_store, number_key(1), "first again"sv, index, ten));
    EXPECT_EQ(value_at(*database, object_store, number_key(1)), buffer_of("first again"sv));

    // A put that is refused leaves the record that it would have replaced, with its place in the index.
    auto const fifty = number_key(50);
    TRY_OR_FAIL(put_with_index_key(*writer, object_store, number_key(5), "fifth"sv, index, fifty));
    auto refused_replacement = put_with_index_key(*writer, object_store, number_key(5), "fifth again"sv, index, ten);
    EXPECT_EQ(kind_of_failure(refused_replacement), IndexedDBError::Kind::Constraint);
    EXPECT_EQ(value_at(*database, object_store, number_key(5)), buffer_of("fifth"sv));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, only(ten))), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, only(fifty))), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 3u);

    // So does one whose value is in chunks, where the value that is refused is in chunks too.
    auto const six = number_key(6);
    auto const sixty = number_key(60);
    Array keys_of_the_sixth { sixty.bytes() };
    auto const value_in_three_chunks = value_of_length(2 * IndexedDBDatabase::VALUE_CHUNK_SIZE + 1, 1);
    auto const value_in_two_chunks = value_of_length(IndexedDBDatabase::VALUE_CHUNK_SIZE + 1, 2);
    TRY_OR_FAIL(store_with_index_keys(*writer, object_store, six.bytes(), value_in_three_chunks.bytes(), index, keys_of_the_sixth.span()));

    auto refused_replacement_in_chunks = store_with_index_keys(*writer, object_store, six.bytes(), value_in_two_chunks.bytes(), index, keys.span());
    EXPECT_EQ(kind_of_failure(refused_replacement_in_chunks), IndexedDBError::Kind::Constraint);
    EXPECT(is_the_value(value_at(*database, object_store, six), value_in_three_chunks));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, only(ten))), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, only(sixty))), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 4u);
}

TEST_CASE(a_multi_entry_index_has_a_record_for_each_key)
{
    auto [store, database, object_store, index] = store_with_an_index(Unique::Yes, MultiEntry::Yes);
    auto writer = TRY_OR_FAIL(database->begin_writer());
    auto const ten = number_key(10);
    auto const twenty = number_key(20);
    auto const thirty = number_key(30);

    // Equal keys of one record are one record of the index.
    Array keys_of_first { ten.bytes(), twenty.bytes(), ten.bytes() };
    TRY_OR_FAIL(put_with_index_keys(*writer, object_store, number_key(1), "value"sv, index, keys_of_first.span()));
    EXPECT_EQ(records_of_index(*database, index), (Vector { "10/1"_string, "20/1"_string }));

    // Of -0 and 0, which are one key, the first is the one that is read back.
    Array zeros { NUMBER_NEGATIVE_0.span(), NUMBER_0.span() };
    TRY_OR_FAIL(put_with_index_keys(*writer, object_store, number_key(2), "value"sv, index, zeros.span()));
    auto records_at_zero = TRY_OR_FAIL(database->read_index_records(index, only(NUMBER_0), Direction::Ascending, RecordsOfEachKey::All, {}, WithValues::No));
    EXPECT_EQ(records_at_zero.size(), 1u);
    EXPECT_EQ(records_at_zero[0].key.bytes(), NUMBER_NEGATIVE_0.span());

    // An array among the keys is a key, and is not taken apart.
    Array an_array { ARRAY_OF_1.span() };
    TRY_OR_FAIL(put_with_index_keys(*writer, object_store, number_key(3), "value"sv, index, an_array.span()));
    auto records_at_the_array = TRY_OR_FAIL(database->read_index_records(index, only(ARRAY_OF_1), Direction::Ascending, RecordsOfEachKey::All, {}, WithValues::No));
    EXPECT_EQ(records_at_the_array.size(), 1u);
    EXPECT_EQ(records_at_the_array[0].primary_key, number_key(3));

    // A record with no keys has no records in the index.
    TRY_OR_FAIL(put_with_index_keys(*writer, object_store, number_key(4), "value"sv, index, {}));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 4u);

    // One key that another record has is enough for a unique index to refuse all of them.
    Array keys_with_one_taken { thirty.bytes(), twenty.bytes() };
    auto refused = put_with_index_keys(*writer, object_store, number_key(5), "value"sv, index, keys_with_one_taken.span());
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Constraint);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, only(thirty))), 0u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 4u);
}

TEST_CASE(index_keys_are_taken_as_the_index_allows)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));

    auto object_store = TRY_OR_FAIL(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
    auto index = TRY_OR_FAIL(upgrade->create_index(object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));
    auto other_object_store = TRY_OR_FAIL(upgrade->create_object_store("other"_utf16, {}, KeyGenerator::No));
    auto index_of_other = TRY_OR_FAIL(upgrade->create_index(other_object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));

    auto const ten = number_key(10);
    auto const twenty = number_key(20);
    Array one_key { ten.bytes() };
    Array two_keys { ten.bytes(), twenty.bytes() };
    Array not_a_key { UNKNOWN_TAG.span() };

    // A renderer that runs LibWeb sends none of these.
    auto put_with = [&](i64 named_index, ReadonlySpan<ReadonlyBytes> keys) {
        return kind_of_failure(put_with_index_keys(*upgrade, object_store, number_key(1), "value"sv, named_index, keys));
    };
    EXPECT_EQ(put_with(index, two_keys.span()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(put_with(index, not_a_key.span()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(put_with(index_of_other, one_key.span()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(put_with(1000, one_key.span()), IndexedDBError::Kind::InvalidClaim);

    Array the_index_twice {
        WebView::IndexedDBIndexKeys { .index_id = index, .keys = one_key.span() },
        WebView::IndexedDBIndexKeys { .index_id = index, .keys = {} },
    };
    auto named_twice = upgrade->store_record(object_store, ten.bytes(), "value"sv.bytes(), NoOverwrite::No, the_index_twice.span());
    EXPECT_EQ(kind_of_failure(named_twice), IndexedDBError::Kind::InvalidClaim);

    EXPECT(upgrade->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 0u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index_of_other, {})), 0u);
}

TEST_CASE(a_marker_in_an_index_key_becomes_the_generated_key)
{
    auto [store, database, object_store, index] = store_with_an_index(Unique::No, MultiEntry::No, WebView::IndexedDBKeyPath { "id"_utf16 }, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_writer());

    Array marker_as_a_key { GENERATED_KEY_MARKER.span() };
    Array array_as_a_key { ARRAY_OF_A_GENERATED_KEY_MARKER.span() };
    WebView::IndexedDBValueSegments segments { .before_the_key = "before"sv.bytes(), .after_the_key = "after"sv.bytes() };
    auto store_with_keys = [&](Optional<ReadonlyBytes> key, WebView::IndexedDBValue value, ReadonlySpan<ReadonlyBytes> keys) {
        return store_with_index_keys(*writer, object_store, key, value, index, keys);
    };

    EXPECT_EQ(TRY_OR_FAIL(store_with_keys({}, segments, marker_as_a_key.span())).generated_key, 1u);
    EXPECT_EQ(records_of_index(*database, index), (Vector { "1/1"_string }));

    // The key that is injected can be in an index key while the value has no place for it.
    EXPECT_EQ(TRY_OR_FAIL(store_with_keys({}, "whole"sv.bytes(), marker_as_a_key.span())).generated_key, 2u);
    EXPECT_EQ(records_of_index(*database, index), (Vector { "1/1"_string, "2/2"_string }));

    EXPECT_EQ(TRY_OR_FAIL(store_with_keys({}, segments, array_as_a_key.span())).generated_key, 3u);
    auto const three = number_key(3);
    ByteBuffer array_of_three;
    array_of_three.append(0x50);
    array_of_three.append(three.bytes());
    array_of_three.append(0x00);
    auto records_at_the_array = TRY_OR_FAIL(database->read_index_records(index, only(array_of_three), Direction::Ascending, RecordsOfEachKey::All, {}, WithValues::No));
    EXPECT_EQ(records_at_the_array.size(), 1u);
    EXPECT_EQ(records_at_the_array[0].key, array_of_three);
    EXPECT_EQ(records_at_the_array[0].primary_key, number_key(3));

    // A key that is given is not generated, so no index key of that record holds a marker.
    auto const nine = number_key(9);
    EXPECT_EQ(kind_of_failure(store_with_keys(nine.bytes(), "value"sv.bytes(), marker_as_a_key.span())), IndexedDBError::Kind::InvalidClaim);
    EXPECT(writer->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 3u);
}

TEST_CASE(a_marker_is_refused_where_no_generated_key_is_injected)
{
    // An object store with out-of-line keys generates a key and injects nothing.
    auto [store, database, object_store, index] = store_with_an_index(Unique::No, MultiEntry::No, {}, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_writer());

    Array marker_as_a_key { GENERATED_KEY_MARKER.span() };

    auto refused = store_with_index_keys(*writer, object_store, {}, "value"sv.bytes(), index, marker_as_a_key.span());
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::InvalidClaim);
    EXPECT(writer->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 0u);

    // The key that the refused request generated is the next one again.
    EXPECT_EQ(put_with_a_generated_key(*writer, object_store), 1u);
}

TEST_CASE(filling_an_index_gives_older_records_their_keys)
{
    auto [store, database, object_store] = store_with_a_database();

    // An array of 0 and an array of -0 are one key in two forms, as 0 and -0 are.
    auto array_of = [](ReadonlyBytes key) {
        ByteBuffer array;
        array.append(0x50);
        array.append(key);
        array.append(0x00);
        return array;
    };
    auto const array_of_zero = array_of(NUMBER_0);
    auto const array_of_negative_zero = array_of(NUMBER_NEGATIVE_0);
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        for (u64 number = 1; number <= 3; ++number) {
            auto value = MUST(String::formatted("value of {}", number));
            TRY_OR_FAIL(put(*writer, object_store, number_key(number), value));
        }
        TRY_OR_FAIL(put(*writer, object_store, NUMBER_NEGATIVE_0, "value"sv));
        TRY_OR_FAIL(put(*writer, object_store, array_of_zero, "value"sv));
        TRY_OR_FAIL(writer->commit(at(2)));
    }

    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));

    // Another index comes first, so that the index is not numbered as its object store is.
    auto other_object_store = TRY_OR_FAIL(upgrade->create_object_store("other"_utf16, {}, KeyGenerator::No));
    TRY_OR_FAIL(upgrade->create_index(other_object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));
    auto index = TRY_OR_FAIL(upgrade->create_index(object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::Yes));
    EXPECT(index != object_store);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 0u);

    auto const ten = number_key(10);
    auto const twenty = number_key(20);
    Array keys_of_first { ten.bytes(), twenty.bytes() };
    Array keys_of_third { ten.bytes() };
    TRY_OR_FAIL(fill_index(*upgrade, index, number_key(1), keys_of_first.span()));
    TRY_OR_FAIL(fill_index(*upgrade, index, number_key(2), {}));
    TRY_OR_FAIL(fill_index(*upgrade, index, number_key(3), keys_of_third.span()));
    EXPECT_EQ(records_of_index(*database, index), (Vector { "10/1"_string, "10/3"_string, "20/1"_string }));

    // A record of the index has the value of the record that it was filled for.
    auto at_ten = TRY_OR_FAIL(database->read_index_records(index, only(ten), Direction::Ascending, RecordsOfEachKey::All, {}, WithValues::Yes));
    EXPECT_EQ(at_ten.size(), 2u);
    if (at_ten.size() != 2)
        return;

    EXPECT_EQ(at_ten[0].value, buffer_of("value of 1"sv));
    EXPECT_EQ(at_ten[1].value, buffer_of("value of 3"sv));

    // The key of the record is read back as the record has it, whichever form of it the fill names.
    auto const thirty = number_key(30);
    Array keys_of_the_zeros { thirty.bytes() };
    TRY_OR_FAIL(fill_index(*upgrade, index, NUMBER_0, keys_of_the_zeros.span()));
    TRY_OR_FAIL(fill_index(*upgrade, index, array_of_negative_zero, keys_of_the_zeros.span()));

    auto at_thirty = TRY_OR_FAIL(database->read_index_records(index, only(thirty), Direction::Ascending, RecordsOfEachKey::All, {}, WithValues::No));
    EXPECT_EQ(at_thirty.size(), 2u);
    if (at_thirty.size() != 2)
        return;

    EXPECT_EQ(at_thirty[0].primary_key.bytes(), NUMBER_NEGATIVE_0.span());
    EXPECT_EQ(at_thirty[1].primary_key, array_of_zero);

    // A renderer that runs LibWeb sends none of these.
    Array marker_as_a_key { GENERATED_KEY_MARKER.span() };
    EXPECT_EQ(kind_of_failure(fill_index(*upgrade, index, number_key(4), keys_of_third.span())), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(fill_index(*upgrade, index, number_key(2), marker_as_a_key.span())), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(fill_index(*upgrade, 1000, number_key(2), keys_of_third.span())), IndexedDBError::Kind::InvalidClaim);
    EXPECT(upgrade->is_open());
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 5u);

    // The records of the index go with the record that they were filled for.
    auto const three = number_key(3);
    TRY_OR_FAIL(upgrade->delete_records(object_store, only(three)));
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, only(ten))), 1u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 4u);

    TRY_OR_FAIL(upgrade->commit(at(3)));
    EXPECT_EQ(database->version(), 2u);
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 4u);
}

TEST_CASE(a_fill_that_a_unique_index_refuses_ends_the_upgrade)
{
    auto [store, database, object_store] = store_with_a_database();
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));
        TRY_OR_FAIL(put(*writer, object_store, number_key(2), "value"sv));
        TRY_OR_FAIL(writer->commit(at(2)));
    }

    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
    auto index = TRY_OR_FAIL(upgrade->create_index(object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::Yes, MultiEntry::No));

    auto const ten = number_key(10);
    Array keys { ten.bytes() };
    TRY_OR_FAIL(fill_index(*upgrade, index, number_key(1), keys.span()));

    auto refused = fill_index(*upgrade, index, number_key(2), keys.span());
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Constraint);
    EXPECT(refused.error().lost_the_writer);

    // Nothing of the upgrade is kept, the records of the index that it did store included.
    EXPECT(!upgrade->is_open());
    EXPECT(!database->has_writer());
    EXPECT_EQ(database->version(), 1u);
    EXPECT(TRY_OR_FAIL(database->object_stores())[0].indexes.is_empty());
    EXPECT_EQ(TRY_OR_FAIL(database->count_index_records(index, {})), 0u);
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 2u);
}

// The key of the record that a cursor over an object store comes to, where the keys are whole numbers.
static Optional<u64> step_of_cursor(IndexedDBDatabase& database, i64 object_store, Direction direction, Optional<u64> position, Optional<u64> key = {}, u32 count = 1, IndexedDBKeyRange const& range = {})
{
    auto const position_key = position.map(number_key);
    auto const seek_key = key.map(number_key);

    auto record = MUST(database.iterate_cursor(object_store, range, direction, Optional<ReadonlyBytes> { position_key }, Optional<ReadonlyBytes> { seek_key }, count, WithValues::No));
    if (!record.has_value())
        return {};

    EXPECT(!record->value.has_value());
    return number_of(record->key);
}

TEST_CASE(a_cursor_steps_through_the_records_of_an_object_store)
{
    auto [store, database, object_store] = store_with_a_database();
    auto writer = TRY_OR_FAIL(database->begin_writer());
    for (u64 number : { 30, 10, 50, 20, 40 })
        TRY_OR_FAIL(put(*writer, object_store, number_key(number), "value"sv));

    auto step = [&](Direction direction, Optional<u64> position, Optional<u64> key = {}, u32 count = 1, IndexedDBKeyRange const& range = {}) {
        return step_of_cursor(*database, object_store, direction, position, key, count, range);
    };

    // A cursor that is nowhere yet comes to the first record of its direction. Each step starts from the key of the
    // record that the step before came to, and the step after the last record comes to none.
    auto walk = [&](Direction direction) {
        Vector<u64> numbers;
        Optional<u64> position;
        for (size_t steps = 0; steps < 10; ++steps) {
            position = step(direction, position);
            if (!position.has_value())
                break;
            numbers.append(*position);
        }
        return numbers;
    };
    auto read = [&](Direction direction) {
        Vector<u64> numbers;
        for (auto const& key : keys_of(*database, object_store, {}, direction))
            numbers.append(number_of(key));
        return numbers;
    };
    EXPECT_EQ(walk(Direction::Ascending), (Vector<u64> { 10, 20, 30, 40, 50 }));
    EXPECT_EQ(walk(Direction::Descending), (Vector<u64> { 50, 40, 30, 20, 10 }));
    EXPECT_EQ(walk(Direction::Ascending), read(Direction::Ascending));
    EXPECT_EQ(walk(Direction::Descending), read(Direction::Descending));

    // A position is not always the key of a record.
    EXPECT_EQ(step(Direction::Ascending, 25), 30u);
    EXPECT_EQ(step(Direction::Descending, 25), 20u);

    // A key to go to is the least that the key of the record may be, or the most for a cursor that goes down.
    EXPECT_EQ(step(Direction::Ascending, 10, 30), 30u);
    EXPECT_EQ(step(Direction::Ascending, 10, 35), 40u);
    EXPECT_EQ(step(Direction::Descending, 50, 30), 30u);
    EXPECT_EQ(step(Direction::Descending, 50, 35), 30u);
    EXPECT_EQ(step(Direction::Ascending, {}, 30), 30u);
    EXPECT(!step(Direction::Ascending, 10, 55).has_value());

    // The record is past the position all the same.
    EXPECT_EQ(step(Direction::Ascending, 30, 10), 40u);
    EXPECT_EQ(step(Direction::Descending, 30, 50), 20u);

    // A count is the number of records to go on by.
    EXPECT_EQ(step(Direction::Ascending, {}, {}, 2), 20u);
    EXPECT_EQ(step(Direction::Ascending, 20, {}, 3), 50u);
    EXPECT_EQ(step(Direction::Descending, 40, {}, 2), 20u);
    EXPECT(!step(Direction::Ascending, 20, {}, 4).has_value());
    EXPECT(!step(Direction::Ascending, {}, {}, 0xffffffff).has_value());

    // The record is in the range.
    auto const twenty = number_key(20);
    auto const forty = number_key(40);
    IndexedDBKeyRange from_twenty_to_below_forty { .lower_bound = twenty.bytes(), .lower_bound_is_open = false, .upper_bound = forty.bytes(), .upper_bound_is_open = true };
    EXPECT_EQ(step(Direction::Ascending, {}, {}, 1, from_twenty_to_below_forty), 20u);
    EXPECT_EQ(step(Direction::Descending, {}, {}, 1, from_twenty_to_below_forty), 30u);
    EXPECT_EQ(step(Direction::Ascending, {}, 10, 2, from_twenty_to_below_forty), 30u);
    EXPECT(!step(Direction::Ascending, 30, {}, 1, from_twenty_to_below_forty).has_value());
    EXPECT(!step(Direction::Descending, 20, {}, 1, from_twenty_to_below_forty).has_value());

    // A cursor goes on from a position whose record is gone, and comes to a record that was stored after its last step.
    auto const thirty = number_key(30);
    TRY_OR_FAIL(writer->delete_records(object_store, only(thirty)));
    EXPECT_EQ(step(Direction::Ascending, 30), 40u);
    EXPECT_EQ(step(Direction::Descending, 30), 20u);

    TRY_OR_FAIL(put(*writer, object_store, number_key(35), "value"sv));
    EXPECT_EQ(step(Direction::Ascending, 30), 35u);

    // A renderer that runs LibWeb throws for a count of 0 before it sends anything, and only sends keys.
    auto const ten = number_key(10);
    EXPECT_EQ(kind_of_failure(database->iterate_cursor(object_store, {}, Direction::Ascending, {}, {}, 0, WithValues::No)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(database->iterate_cursor(object_store, {}, Direction::Ascending, UNKNOWN_TAG.span(), {}, 1, WithValues::No)), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(kind_of_failure(database->iterate_cursor(object_store, {}, Direction::Ascending, ten.bytes(), UNKNOWN_TAG.span(), 1, WithValues::No)), IndexedDBError::Kind::InvalidClaim);
}

// IndexedDBIndexCursorPosition and IndexedDBIndexCursorSeek with whole numbers for keys.
struct IndexCursorPosition {
    u64 key { 0 };
    u64 primary_key { 0 };
};

struct IndexCursorSeek {
    u64 key { 0 };
    Optional<u64> primary_key;
};

// The record that a cursor over an index comes to, where its key and its primary key are whole numbers.
static Optional<IndexCursorPosition> step_of_index_cursor(IndexedDBDatabase& database, i64 index, Direction direction, RecordsOfEachKey records_of_each_key, Optional<IndexCursorPosition> position, Optional<IndexCursorSeek> seek = {}, u32 count = 1, IndexedDBKeyRange const& range = {})
{
    Optional<ByteBuffer> position_key;
    Optional<ByteBuffer> position_primary_key;
    Optional<WebView::IndexedDBIndexCursorPosition> cursor_position;
    if (position.has_value()) {
        position_key = number_key(position->key);
        position_primary_key = number_key(position->primary_key);
        cursor_position = WebView::IndexedDBIndexCursorPosition { .key = position_key->bytes(), .primary_key = position_primary_key->bytes() };
    }

    Optional<ByteBuffer> seek_key;
    Optional<ByteBuffer> seek_primary_key;
    Optional<WebView::IndexedDBIndexCursorSeek> cursor_seek;
    if (seek.has_value()) {
        seek_key = number_key(seek->key);
        cursor_seek = WebView::IndexedDBIndexCursorSeek { .key = seek_key->bytes(), .primary_key = {} };
    }
    if (seek.has_value() && seek->primary_key.has_value()) {
        seek_primary_key = number_key(*seek->primary_key);
        cursor_seek->primary_key = seek_primary_key->bytes();
    }

    auto record = MUST(database.iterate_index_cursor(index, range, direction, records_of_each_key, cursor_position, cursor_seek, count, WithValues::No));
    if (!record.has_value())
        return {};

    EXPECT(!record->value.has_value());
    return IndexCursorPosition { .key = number_of(record->key), .primary_key = number_of(record->primary_key) };
}

TEST_CASE(a_cursor_steps_through_the_records_of_an_index)
{
    auto [store, database, object_store, index] = store_with_an_index();
    auto writer = TRY_OR_FAIL(database->begin_writer());

    // Records 1 and 2 have the index key 30, 3 and 4 have 20, 5 and 6 have 10, so that a later key has lower primary
    // keys. They are stored in another order.
    for (u64 primary_key : { 6, 1, 4, 5, 2, 3 }) {
        auto index_key = number_key((4 - ((primary_key + 1) / 2)) * 10);
        TRY_OR_FAIL(put_with_index_key(*writer, object_store, number_key(primary_key), "value"sv, index, index_key));
    }

    using Position = IndexCursorPosition;
    using Seek = IndexCursorSeek;
    auto as_text = [](Optional<Position> const& record) {
        if (!record.has_value())
            return "none"_string;
        return key_and_primary_key(record->key, record->primary_key);
    };
    auto step = [&](Direction direction, RecordsOfEachKey records_of_each_key, Optional<Position> position, Optional<Seek> seek = {}, u32 count = 1, IndexedDBKeyRange const& range = {}) {
        return as_text(step_of_index_cursor(*database, index, direction, records_of_each_key, position, seek, count, range));
    };

    // Each step starts from the key and the primary key of the record that the step before came to.
    auto walk = [&](Direction direction, RecordsOfEachKey records_of_each_key) {
        Vector<String> records;
        Optional<Position> position;
        for (size_t steps = 0; steps < 10; ++steps) {
            position = step_of_index_cursor(*database, index, direction, records_of_each_key, position);
            if (!position.has_value())
                break;
            records.append(as_text(position));
        }
        return records;
    };
    EXPECT_EQ(walk(Direction::Ascending, RecordsOfEachKey::All), records_of_index(*database, index));
    EXPECT_EQ(walk(Direction::Ascending, RecordsOfEachKey::All), (Vector { "10/5"_string, "10/6"_string, "20/3"_string, "20/4"_string, "30/1"_string, "30/2"_string }));
    EXPECT_EQ(walk(Direction::Descending, RecordsOfEachKey::All), (Vector { "30/2"_string, "30/1"_string, "20/4"_string, "20/3"_string, "10/6"_string, "10/5"_string }));

    // "nextunique" and "prevunique" come to the first record of each key, which has the lowest primary key.
    EXPECT_EQ(walk(Direction::Ascending, RecordsOfEachKey::First), (Vector { "10/5"_string, "20/3"_string, "30/1"_string }));
    EXPECT_EQ(walk(Direction::Descending, RecordsOfEachKey::First), (Vector { "30/1"_string, "20/3"_string, "10/5"_string }));

    // They go past every record of the key that the cursor is at, whatever primary key it is at.
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::First, Position { 20, 1 }), "30/1"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::First, Position { 30, 2 }), "20/3"sv);

    // A key to go to is the least that the key of the record may be, or the most for a cursor that goes down.
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 10, 5 }, Seek { 20, {} }), "20/3"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 10, 5 }, Seek { 25, {} }), "30/1"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::All, Position { 30, 2 }, Seek { 20, {} }), "20/4"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::First, Position { 10, 5 }, Seek { 30, {} }), "30/1"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::First, Position { 30, 1 }, Seek { 10, {} }), "10/5"sv);

    // A primary key to go to is the least that a record of that key may have, or the most for a cursor that goes down.
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 10, 5 }, Seek { 20, 4 }), "20/4"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::All, Position { 30, 2 }, Seek { 20, 3 }), "20/3"sv);

    // It says nothing of the records of the keys that come after.
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 20, 3 }, Seek { 20, 99 }), "30/1"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::All, Position { 30, 2 }, Seek { 20, 2 }), "10/6"sv);

    // The record is past the position all the same.
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 20, 3 }, Seek { 10, 5 }), "20/4"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::All, Position { 20, 4 }, Seek { 30, 2 }), "20/3"sv);

    // A count is the number of records to go on by, which for "nextunique" and "prevunique" is a number of keys.
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, {}, {}, 3), "20/3"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::All, Position { 30, 2 }, {}, 2), "20/4"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::First, {}, {}, 2), "20/3"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::First, {}, {}, 3), "10/5"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::First, {}, {}, 4), "none"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, {}, {}, 0xffffffff), "none"sv);

    // The key of the record is in the range.
    auto const twenty = number_key(20);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, {}, {}, 1, only(twenty)), "20/3"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 20, 3 }, {}, 1, only(twenty)), "20/4"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 20, 4 }, {}, 1, only(twenty)), "none"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::First, {}, {}, 1, only(twenty)), "20/3"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::First, Position { 20, 3 }, {}, 1, only(twenty)), "none"sv);

    // A cursor goes on from a position whose record is gone.
    auto const three = number_key(3);
    TRY_OR_FAIL(writer->delete_records(object_store, only(three)));
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::All, Position { 20, 3 }), "20/4"sv);
    EXPECT_EQ(step(Direction::Descending, RecordsOfEachKey::All, Position { 20, 3 }), "10/6"sv);
    EXPECT_EQ(step(Direction::Ascending, RecordsOfEachKey::First, Position { 10, 5 }), "20/4"sv);

    auto const ten = number_key(10);
    auto const one = number_key(1);
    auto refused = [&](RecordsOfEachKey records_of_each_key, ReadonlyBytes position_key, ReadonlyBytes position_primary_key, ReadonlyBytes seek_key, Optional<ReadonlyBytes> seek_primary_key, u32 count = 1) {
        WebView::IndexedDBIndexCursorPosition position { .key = position_key, .primary_key = position_primary_key };
        WebView::IndexedDBIndexCursorSeek seek { .key = seek_key, .primary_key = seek_primary_key };
        return kind_of_failure(database->iterate_index_cursor(index, {}, Direction::Ascending, records_of_each_key, position, seek, count, WithValues::No));
    };
    EXPECT(!refused(RecordsOfEachKey::All, ten, one, twenty, one.bytes()).has_value());
    EXPECT(!refused(RecordsOfEachKey::First, ten, one, twenty, {}).has_value());

    // A renderer that runs LibWeb throws for each of these before it sends anything, and only sends keys.
    EXPECT_EQ(refused(RecordsOfEachKey::First, ten, one, twenty, one.bytes()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(refused(RecordsOfEachKey::All, ten, one, twenty, one.bytes(), 0), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(refused(RecordsOfEachKey::All, UNKNOWN_TAG, one, twenty, one.bytes()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(refused(RecordsOfEachKey::All, ten, UNKNOWN_TAG, twenty, one.bytes()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(refused(RecordsOfEachKey::First, ten, UNKNOWN_TAG, twenty, {}), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(refused(RecordsOfEachKey::All, ten, one, UNKNOWN_TAG, one.bytes()), IndexedDBError::Kind::InvalidClaim);
    EXPECT_EQ(refused(RecordsOfEachKey::All, ten, one, twenty, UNKNOWN_TAG.span()), IndexedDBError::Kind::InvalidClaim);
}

TEST_CASE(a_cursor_gets_the_keys_as_they_were_written_and_the_value_of_the_record)
{
    auto [store, database, object_store, index] = store_with_an_index();
    auto writer = TRY_OR_FAIL(database->begin_writer());

    // Records 1, 2 and 3 have the index keys 30, 20 and 10.
    for (u64 primary_key : { 1, 2, 3 }) {
        auto index_key = number_key((4 - primary_key) * 10);
        auto value = MUST(String::formatted("value of {}", primary_key));
        TRY_OR_FAIL(put_with_index_key(*writer, object_store, number_key(primary_key), value, index, index_key));
    }

    // A step without the value comes first, and the same step with the value is another statement.
    EXPECT_EQ(step_of_cursor(*database, object_store, Direction::Ascending, {}), 1u);
    EXPECT(step_of_index_cursor(*database, index, Direction::Ascending, RecordsOfEachKey::All, {}).has_value());

    // The value is that of the record that the cursor comes to, and not of one that it goes past.
    auto third = TRY_OR_FAIL(database->iterate_cursor(object_store, {}, Direction::Ascending, {}, {}, 3, WithValues::Yes));
    EXPECT(third.has_value());
    if (!third.has_value())
        return;

    EXPECT_EQ(third->key, number_key(3));
    EXPECT_EQ(third->value, buffer_of("value of 3"sv));

    // For an index it is the value of the record that the index's record refers to.
    auto third_of_index = TRY_OR_FAIL(database->iterate_index_cursor(index, {}, Direction::Ascending, RecordsOfEachKey::All, {}, {}, 3, WithValues::Yes));
    EXPECT(third_of_index.has_value());
    if (!third_of_index.has_value())
        return;

    EXPECT_EQ(third_of_index->key, number_key(30));
    EXPECT_EQ(third_of_index->primary_key, number_key(1));
    EXPECT_EQ(third_of_index->value, buffer_of("value of 1"sv));

    // Both keys are read back as they were written, with or without the value.
    TRY_OR_FAIL(put_with_index_key(*writer, object_store, NUMBER_NEGATIVE_0, "value of -0"sv, index, NUMBER_NEGATIVE_0));
    for (auto with_values : { WithValues::No, WithValues::Yes }) {
        auto first = TRY_OR_FAIL(database->iterate_cursor(object_store, {}, Direction::Ascending, {}, {}, 1, with_values));
        auto first_of_index = TRY_OR_FAIL(database->iterate_index_cursor(index, {}, Direction::Ascending, RecordsOfEachKey::All, {}, {}, 1, with_values));
        EXPECT(first.has_value());
        EXPECT(first_of_index.has_value());
        if (!first.has_value() || !first_of_index.has_value())
            return;

        EXPECT_EQ(first->key.bytes(), NUMBER_NEGATIVE_0.span());
        EXPECT_EQ(first_of_index->key.bytes(), NUMBER_NEGATIVE_0.span());
        EXPECT_EQ(first_of_index->primary_key.bytes(), NUMBER_NEGATIVE_0.span());

        EXPECT_EQ(first->value.has_value(), with_values == WithValues::Yes);
        EXPECT_EQ(first_of_index->value.has_value(), with_values == WithValues::Yes);
        if (with_values == WithValues::Yes) {
            EXPECT_EQ(first->value, buffer_of("value of -0"sv));
            EXPECT_EQ(first_of_index->value, buffer_of("value of -0"sv));
        }
    }

    // A renderer sends back the keys that it got, so a position and a key to go to can be -0. They are compared as the
    // records are sorted, where -0 is 0.
    auto const negative_zero = MUST(ByteBuffer::copy(NUMBER_NEGATIVE_0.span()));
    auto key_at = [&](Direction direction, Optional<ReadonlyBytes> position, Optional<ReadonlyBytes> key) -> Optional<ByteBuffer> {
        auto record = MUST(database->iterate_cursor(object_store, {}, direction, position, key, 1, WithValues::No));
        if (!record.has_value())
            return {};
        return move(record->key);
    };
    EXPECT_EQ(key_at(Direction::Ascending, NUMBER_NEGATIVE_0.span(), {}), number_key(1));
    EXPECT_EQ(key_at(Direction::Ascending, NUMBER_0.span(), {}), number_key(1));
    EXPECT_EQ(key_at(Direction::Descending, {}, NUMBER_NEGATIVE_0.span()), negative_zero);

    auto primary_key_at = [&](Direction direction, Optional<WebView::IndexedDBIndexCursorPosition> position, Optional<WebView::IndexedDBIndexCursorSeek> seek) -> Optional<ByteBuffer> {
        auto record = MUST(database->iterate_index_cursor(index, {}, direction, RecordsOfEachKey::All, position, seek, 1, WithValues::No));
        if (!record.has_value())
            return {};
        return move(record->primary_key);
    };
    WebView::IndexedDBIndexCursorPosition at_negative_zero { .key = NUMBER_NEGATIVE_0.span(), .primary_key = NUMBER_NEGATIVE_0.span() };
    WebView::IndexedDBIndexCursorSeek to_key_negative_zero { .key = NUMBER_NEGATIVE_0.span(), .primary_key = {} };
    WebView::IndexedDBIndexCursorSeek to_record_negative_zero { .key = NUMBER_NEGATIVE_0.span(), .primary_key = NUMBER_NEGATIVE_0.span() };
    EXPECT_EQ(primary_key_at(Direction::Ascending, at_negative_zero, {}), number_key(3));
    EXPECT_EQ(primary_key_at(Direction::Descending, {}, to_key_negative_zero), negative_zero);
    EXPECT_EQ(primary_key_at(Direction::Descending, {}, to_record_negative_zero), negative_zero);
}

TEST_CASE(a_cursor_only_comes_to_the_records_of_its_source)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient());
    auto database = TRY_OR_FAIL(store->open_database(example_com(), "database"_utf16, 1, at(1))).database;
    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(1));

    auto first_store = TRY_OR_FAIL(upgrade->create_object_store("first"_utf16, {}, KeyGenerator::No));
    auto second_store = TRY_OR_FAIL(upgrade->create_object_store("second"_utf16, {}, KeyGenerator::No));
    auto first_index = TRY_OR_FAIL(upgrade->create_index(first_store, "first"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));
    auto second_index = TRY_OR_FAIL(upgrade->create_index(first_store, "second"_utf16, WebView::IndexedDBKeyPath { "b"_utf16 }, Unique::No, MultiEntry::No));

    // Records 1 and 3 of the first object store have the key 7, each in one of its indexes. The second object store
    // has a record 2.
    auto const seven = number_key(7);
    TRY_OR_FAIL(put_with_index_key(*upgrade, first_store, number_key(1), "value"sv, first_index, seven));
    TRY_OR_FAIL(put_with_index_key(*upgrade, first_store, number_key(3), "value"sv, second_index, seven));
    TRY_OR_FAIL(put(*upgrade, second_store, number_key(2), "value"sv));

    EXPECT_EQ(step_of_cursor(*database, first_store, Direction::Ascending, 1), 3u);
    EXPECT_EQ(step_of_cursor(*database, first_store, Direction::Descending, 3), 1u);
    EXPECT_EQ(step_of_cursor(*database, second_store, Direction::Ascending, {}), 2u);
    EXPECT(!step_of_cursor(*database, second_store, Direction::Ascending, 2).has_value());

    for (auto records_of_each_key : { RecordsOfEachKey::All, RecordsOfEachKey::First }) {
        for (auto direction : { Direction::Ascending, Direction::Descending }) {
            auto first_of_second_index = step_of_index_cursor(*database, second_index, direction, records_of_each_key, {});
            EXPECT(first_of_second_index.has_value());
            if (!first_of_second_index.has_value())
                return;

            EXPECT_EQ(first_of_second_index->primary_key, 3u);
            EXPECT(!step_of_index_cursor(*database, second_index, direction, records_of_each_key, first_of_second_index).has_value());
        }
    }
}

TEST_CASE(a_value_is_read_back_whole_whatever_its_length)
{
    auto [store, database, object_store, index] = store_with_an_index();
    auto writer = TRY_OR_FAIL(database->begin_writer());

    // A value of up to the length of a chunk is in its record, and a longer one is in chunks.
    constexpr size_t chunk = IndexedDBDatabase::VALUE_CHUNK_SIZE;
    Array<size_t, 6> lengths { 0, chunk - 1, chunk, chunk + 1, 2 * chunk, 2 * chunk + chunk / 2 };

    for (size_t n = 0; n < lengths.size(); ++n) {
        u64 number = n + 1;
        auto count = static_cast<u32>(number);
        auto const key = number_key(number);
        auto const index_key = number_key(number * 10);
        auto const value = value_of_length(lengths[n], static_cast<u8>(n));
        Array index_keys { index_key.bytes() };
        TRY_OR_FAIL(store_with_index_keys(*writer, object_store, key.bytes(), value.bytes(), index, index_keys.span()));

        EXPECT(is_the_value(value_at(*database, object_store, key), value));

        auto records_of_the_index_key = TRY_OR_FAIL(database->read_index_records(index, only(index_key), Direction::Ascending, RecordsOfEachKey::All, {}, WithValues::Yes));
        EXPECT_EQ(records_of_the_index_key.size(), 1u);
        if (records_of_the_index_key.size() == 1)
            EXPECT(is_the_value(records_of_the_index_key[0].value, value));

        // A cursor goes past the records before this one and gets the value of this one.
        auto found = TRY_OR_FAIL(database->iterate_cursor(object_store, {}, Direction::Ascending, {}, {}, count, WithValues::Yes));
        EXPECT(found.has_value());
        if (found.has_value())
            EXPECT(is_the_value(found->value, value));

        auto found_in_the_index = TRY_OR_FAIL(database->iterate_index_cursor(index, {}, Direction::Ascending, RecordsOfEachKey::All, {}, {}, count, WithValues::Yes));
        EXPECT(found_in_the_index.has_value());
        if (found_in_the_index.has_value())
            EXPECT(is_the_value(found_in_the_index->value, value));
    }

    // One read gives every record its own value, the ones in chunks among the others.
    auto all_records = TRY_OR_FAIL(database->read_records(object_store, {}, Direction::Ascending, {}, WithValues::Yes));
    auto all_records_of_the_index = TRY_OR_FAIL(database->read_index_records(index, {}, Direction::Ascending, RecordsOfEachKey::All, {}, WithValues::Yes));
    EXPECT_EQ(all_records.size(), lengths.size());
    EXPECT_EQ(all_records_of_the_index.size(), lengths.size());
    if (all_records.size() != lengths.size() || all_records_of_the_index.size() != lengths.size())
        return;

    for (size_t n = 0; n < lengths.size(); ++n) {
        auto const value = value_of_length(lengths[n], static_cast<u8>(n));
        EXPECT(is_the_value(all_records[n].value, value));
        EXPECT(is_the_value(all_records_of_the_index[n].value, value));
    }

    // Without their values the records are read as any others are.
    EXPECT_EQ(keys_of(*database, object_store).size(), lengths.size());
    EXPECT_EQ(records_of_index(*database, index).size(), lengths.size());
    EXPECT_EQ(step_of_cursor(*database, object_store, Direction::Descending, {}), lengths.size());
}

TEST_CASE(a_generated_key_is_put_between_the_parts_of_a_large_value)
{
    auto [store, database, object_store] = store_with_a_database(WebView::IndexedDBKeyPath { "id"_utf16 }, KeyGenerator::Yes);
    auto writer = TRY_OR_FAIL(database->begin_writer());

    struct Parts {
        size_t before_the_key { 0 };
        size_t after_the_key { 0 };
    };
    constexpr size_t chunk = IndexedDBDatabase::VALUE_CHUNK_SIZE;
    constexpr size_t key_in_the_value = sizeof(double);
    Array cases {
        // With the key the value is as long as a chunk, and then one byte longer.
        Parts { 100, chunk - key_in_the_value - 100 },
        Parts { 100, chunk - key_in_the_value - 99 },
        // The key lies across the place where the first chunk of a whole value would end.
        Parts { chunk - 3, chunk / 2 },
        // Each part is longer than a chunk.
        Parts { chunk + chunk / 2, 2 * chunk + 5 },
        // The key is the first thing in the value, or the last.
        Parts { 0, chunk + 1 },
        Parts { chunk + 1, 0 },
    };

    u64 generated_key = 0;
    for (auto const& parts : cases) {
        ++generated_key;
        auto const before_the_key = value_of_length(parts.before_the_key, 1);
        auto const after_the_key = value_of_length(parts.after_the_key, 2);

        WebView::IndexedDBValueSegments segments { .before_the_key = before_the_key.bytes(), .after_the_key = after_the_key.bytes() };
        auto stored = TRY_OR_FAIL(writer->store_record(object_store, {}, segments, NoOverwrite::No, {}));
        EXPECT_EQ(stored.generated_key, generated_key);

        auto number = AK::convert_between_host_and_little_endian(bit_cast<u64>(static_cast<double>(generated_key)));
        ByteBuffer expected_value;
        expected_value.append(before_the_key.bytes());
        expected_value.append(&number, sizeof(number));
        expected_value.append(after_the_key.bytes());
        EXPECT(is_the_value(value_at(*database, object_store, number_key(generated_key)), expected_value));
    }
}

TEST_CASE(the_chunks_of_a_value_go_with_its_record)
{
    auto [store, database, object_store] = store_with_a_database();
    auto writer = TRY_OR_FAIL(database->begin_writer());

    constexpr size_t chunk = IndexedDBDatabase::VALUE_CHUNK_SIZE;
    auto const value_in_three_chunks = value_of_length(2 * chunk + 1, 1);
    auto const value_in_two_chunks = value_of_length(chunk + 1, 2);
    auto const short_value = value_of_length(10, 3);
    auto put_value = [&](ReadonlyBytes key, ByteBuffer const& value) {
        return writer->store_record(object_store, key, value.bytes(), NoOverwrite::No, {});
    };

    // A record that is stored once another is deleted has none of the chunks of that one.
    auto const one = number_key(1);
    auto const two = number_key(2);
    TRY_OR_FAIL(put_value(one, value_in_three_chunks));
    TRY_OR_FAIL(writer->delete_records(object_store, only(one)));
    TRY_OR_FAIL(put_value(two, value_in_two_chunks));
    EXPECT(is_the_value(value_at(*database, object_store, two), value_in_two_chunks));
    EXPECT(!value_at(*database, object_store, one).has_value());

    // Nor has a record that takes the place of another, whichever of the two values is in chunks.
    TRY_OR_FAIL(put_value(two, short_value));
    EXPECT(is_the_value(value_at(*database, object_store, two), short_value));
    TRY_OR_FAIL(put_value(two, value_in_three_chunks));
    EXPECT(is_the_value(value_at(*database, object_store, two), value_in_three_chunks));
    TRY_OR_FAIL(put_value(two, value_in_two_chunks));
    EXPECT(is_the_value(value_at(*database, object_store, two), value_in_two_chunks));

    // Clearing the object store takes the chunks too.
    TRY_OR_FAIL(writer->clear(object_store));
    TRY_OR_FAIL(put_value(one, value_in_three_chunks));
    EXPECT(is_the_value(value_at(*database, object_store, one), value_in_three_chunks));
    EXPECT_EQ(TRY_OR_FAIL(database->count(object_store, {})), 1u);
}

TEST_CASE(a_value_in_chunks_is_read_a_chunk_at_a_time)
{
    auto [store, database, object_store] = store_with_a_database(WebView::IndexedDBKeyPath { "id"_utf16 }, KeyGenerator::Yes);

    // Two values of four chunks and more. The second comes in parts, of which the first is a short one.
    constexpr size_t chunk = IndexedDBDatabase::VALUE_CHUNK_SIZE;
    auto const whole_value = value_of_length(4 * chunk, 1);
    auto const before_the_key = value_of_length(100, 2);
    auto const after_the_key = value_of_length(4 * chunk, 3);
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        TRY_OR_FAIL(writer->store_record(object_store, {}, whole_value.bytes(), NoOverwrite::No, {}));

        WebView::IndexedDBValueSegments segments { .before_the_key = before_the_key.bytes(), .after_the_key = after_the_key.bytes() };
        TRY_OR_FAIL(writer->store_record(object_store, {}, segments, NoOverwrite::No, {}));
        TRY_OR_FAIL(writer->commit(at(2)));
    }

    // SQLite has room for two chunks, which a value in one cell would not fit in.
    Array<size_t, 2> lengths { whole_value.size(), before_the_key.size() + sizeof(double) + after_the_key.size() };
    for (u64 number = 1; number <= 2; ++number) {
        auto const key = number_key(number);
        auto records = with_room_for_sqlite(2 * chunk, [&] {
            return database->read_records(object_store, only(key), Direction::Ascending, {}, WithValues::Yes);
        });
        EXPECT(!records.is_error());
        if (records.is_error())
            continue;

        EXPECT_EQ(records.value().size(), 1u);
        if (records.value().size() == 1)
            EXPECT_EQ(records.value()[0].value->size(), lengths[number - 1]);
    }
}

TEST_CASE(a_put_that_fails_between_two_chunks_can_lose_the_writer)
{
    auto [store, database, object_store] = store_with_a_database();
    auto writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "value"sv));

    // SQLite has room to take a chunk, and runs out of it while it stores the chunks.
    constexpr size_t chunk = IndexedDBDatabase::VALUE_CHUNK_SIZE;
    auto const key = number_key(2);
    auto const value_in_four_chunks = value_of_length(4 * chunk, 1);
    auto failed = with_room_for_sqlite(2 * chunk, [&] {
        return writer->store_record(object_store, key.bytes(), value_in_four_chunks.bytes(), NoOverwrite::No, {});
    });
    EXPECT_EQ(kind_of_failure(failed), IndexedDBError::Kind::OutOfMemory);
    if (!failed.is_error())
        return;

    // SQLite undoes the statement that failed or everything that the writer did. The failure, the writer and the
    // database say the same about which of the two it was.
    bool has_the_earlier_put = value_at(*database, object_store, number_key(1)).has_value();
    EXPECT_EQ(failed.error().lost_the_writer, !has_the_earlier_put);
    EXPECT_EQ(writer->is_open(), has_the_earlier_put);
    EXPECT_EQ(database->has_writer(), has_the_earlier_put);
    EXPECT(!value_at(*database, object_store, key).has_value());
}

TEST_CASE(a_value_is_not_read_in_part_when_a_chunk_cannot_be_read)
{
    auto [store, database, object_store] = store_with_a_database(WebView::IndexedDBKeyPath { "id"_utf16 }, KeyGenerator::Yes);

    // The first two chunks of the value are short ones: the part before the key, and the key.
    constexpr size_t chunk = IndexedDBDatabase::VALUE_CHUNK_SIZE;
    auto const before_the_key = value_of_length(100, 1);
    auto const after_the_key = value_of_length(chunk + 1, 2);
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        WebView::IndexedDBValueSegments segments { .before_the_key = before_the_key.bytes(), .after_the_key = after_the_key.bytes() };
        TRY_OR_FAIL(writer->store_record(object_store, {}, segments, NoOverwrite::No, {}));
        TRY_OR_FAIL(writer->commit(at(2)));
    }
    auto const key_of_the_value = number_key(1);
    auto read_the_value = [&] {
        return database->read_records(object_store, only(key_of_the_value), Direction::Ascending, {}, WithValues::Yes);
    };

    auto writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*writer, object_store, number_key(2), "value"sv));

    // SQLite has room for the two short chunks and not for the third.
    auto failed = with_little_memory_for_sqlite(read_the_value);
    EXPECT_EQ(kind_of_failure(failed), IndexedDBError::Kind::OutOfMemory);
    if (!failed.is_error())
        return;

    // The read fails as a read that finds no room for a record does, and the writer is lost where SQLite undid what
    // it did.
    bool has_the_put_of_the_writer = TRY_OR_FAIL(database->count(object_store, {})) == 2;
    EXPECT_EQ(failed.error().lost_the_writer, !has_the_put_of_the_writer);
    EXPECT_EQ(writer->is_open(), has_the_put_of_the_writer);

    auto records = TRY_OR_FAIL(read_the_value());
    EXPECT_EQ(records.size(), 1u);
    if (records.size() == 1)
        EXPECT_EQ(records[0].value->size(), before_the_key.size() + sizeof(double) + after_the_key.size());
}

static String other_example()
{
    return "https://other.example"_string;
}

struct DatabaseWithAnObjectStore {
    NonnullRefPtr<IndexedDBDatabase> database;
    i64 object_store { 0 };
};

// Opens a database of the store at version 1, and gives it one object store.
static DatabaseWithAnObjectStore open_a_database(IndexedDBStore& store, String const& storage_key, Utf16String const& name)
{
    auto database = MUST(store.open_database(storage_key, name, 1, at(1))).database;

    auto upgrade = MUST(database->begin_upgrade(1));
    auto object_store = MUST(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
    MUST(upgrade->commit(at(1)));

    return { move(database), object_store };
}

// Stores a value of the given length under a whole number.
static auto put_of_length(IndexedDBDatabase::Writer& writer, i64 object_store, u64 key, size_t length)
{
    auto const key_of_record = number_key(key);
    auto const value = value_of_length(length, 1);
    return writer.store_record(object_store, key_of_record.bytes(), value.bytes(), NoOverwrite::No, {});
}

// Stores a value of the given length in a transaction of its own.
static ErrorOr<void, IndexedDBError> commit_a_put_of_length(IndexedDBDatabase& database, i64 object_store, u64 key, size_t length)
{
    auto writer = TRY(database.begin_writer());
    TRY(put_of_length(*writer, object_store, key, length));
    return writer->commit(at(2));
}

TEST_CASE(the_databases_of_a_storage_key_share_its_budget)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 1 * MiB, .ceiling_of_the_session = 64 * MiB, .charge_for_a_database = 0 }));
    auto [first, store_of_first] = open_a_database(*store, example_com(), "first"_utf16);
    auto [second, store_of_second] = open_a_database(*store, example_com(), "second"_utf16);
    auto [other, store_of_other] = open_a_database(*store, other_example(), "first"_utf16);

    TRY_OR_FAIL(commit_a_put_of_length(*first, store_of_first, 1, 600 * KiB));

    // The second database has next to nothing in it, and its storage key no room for as much again.
    auto writer = TRY_OR_FAIL(second->begin_writer());
    TRY_OR_FAIL(put(*writer, store_of_second, number_key(1), "small"sv));
    auto refused = put_of_length(*writer, store_of_second, 2, 600 * KiB);
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Quota);

    // Only the request is undone, and its writer goes on.
    EXPECT(refused.is_error() && !refused.error().lost_the_writer);
    EXPECT(writer->is_open());
    TRY_OR_FAIL(put_of_length(*writer, store_of_second, 3, 100 * KiB));
    TRY_OR_FAIL(writer->commit(at(3)));
    EXPECT_EQ(keys_of(*second, store_of_second), (Vector { number_key(1), number_key(3) }));

    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*first, store_of_first, 2, 600 * KiB)), IndexedDBError::Kind::Quota);

    // Another storage key has a budget of its own.
    TRY_OR_FAIL(commit_a_put_of_length(*other, store_of_other, 1, 600 * KiB));
}

TEST_CASE(a_writer_is_charged_for_what_it_deleted_until_it_ends)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 1 * MiB, .ceiling_of_the_session = 64 * MiB, .charge_for_a_database = 0 }));
    auto [first, store_of_first] = open_a_database(*store, example_com(), "first"_utf16);
    auto [second, store_of_second] = open_a_database(*store, example_com(), "second"_utf16);

    TRY_OR_FAIL(commit_a_put_of_length(*first, store_of_first, 1, 600 * KiB));
    auto put_as_much_again = [&] { return commit_a_put_of_length(*second, store_of_second, 1, 600 * KiB); };

    // A rollback would bring the deleted record back, so its room is not given away before the writer ends.
    {
        auto writer = TRY_OR_FAIL(first->begin_writer());
        TRY_OR_FAIL(writer->delete_records(store_of_first, {}));
        EXPECT_EQ(kind_of_failure(put_as_much_again()), IndexedDBError::Kind::Quota);

        TRY_OR_FAIL(writer->abort());
        EXPECT_EQ(kind_of_failure(put_as_much_again()), IndexedDBError::Kind::Quota);
    }

    auto writer = TRY_OR_FAIL(first->begin_writer());
    TRY_OR_FAIL(writer->delete_records(store_of_first, {}));
    TRY_OR_FAIL(writer->commit(at(3)));
    TRY_OR_FAIL(put_as_much_again());
}

TEST_CASE(a_value_is_replaced_by_one_of_its_length_where_the_budget_has_no_room_for_both)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 1 * MiB, .ceiling_of_the_session = 64 * MiB, .charge_for_a_database = 0 }));
    auto [database, object_store] = open_a_database(*store, example_com(), "database"_utf16);

    TRY_OR_FAIL(commit_a_put_of_length(*database, object_store, 1, 700 * KiB));
    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*database, object_store, 2, 700 * KiB)), IndexedDBError::Kind::Quota);

    TRY_OR_FAIL(commit_a_put_of_length(*database, object_store, 1, 700 * KiB));
    EXPECT_EQ(keys_of(*database, object_store), (Vector { number_key(1) }));
}

TEST_CASE(a_value_that_is_larger_than_the_budget_is_refused_before_it_is_written)
{
    // The session has no room for the value either. A writer that began to write it would be lost.
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 1 * MiB, .ceiling_of_the_session = 1536 * KiB, .charge_for_a_database = 0 }));
    auto [database, object_store] = open_a_database(*store, example_com(), "database"_utf16);

    auto writer = TRY_OR_FAIL(database->begin_writer());
    TRY_OR_FAIL(put(*writer, object_store, number_key(1), "small"sv));

    auto refused = put_of_length(*writer, object_store, 2, 2 * MiB);
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Quota);
    EXPECT(refused.is_error() && !refused.error().lost_the_writer);
    EXPECT(writer->is_open());

    TRY_OR_FAIL(writer->commit(at(3)));
    EXPECT_EQ(keys_of(*database, object_store), (Vector { number_key(1) }));
}

TEST_CASE(the_databases_of_a_session_share_its_ceiling)
{
    // Each storage key has room for two of the values, and the session for the first and less than another.
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 8 * MiB, .ceiling_of_the_session = 2560 * KiB, .charge_for_a_database = 0 }));
    auto [first, store_of_first] = open_a_database(*store, example_com(), "first"_utf16);
    auto [second, store_of_second] = open_a_database(*store, other_example(), "second"_utf16);

    TRY_OR_FAIL(commit_a_put_of_length(*first, store_of_first, 1, 1536 * KiB));

    // SQLite leaves the transaction open over a value that is one row, and ends it over one that is in chunks.
    for (size_t length : { 900 * KiB, 1536 * KiB }) {
        auto writer = TRY_OR_FAIL(second->begin_writer());
        TRY_OR_FAIL(put(*writer, store_of_second, number_key(1), "small"sv));
        auto refused = put_of_length(*writer, store_of_second, 2, length);
        EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Quota);

        // As on a disk that is full, the writer is lost either way, and what it wrote before goes with it.
        EXPECT(refused.is_error() && refused.error().lost_the_writer);
        EXPECT(!writer->is_open());
        EXPECT(keys_of(*second, store_of_second).is_empty());
    }

    TRY_OR_FAIL(commit_a_put_of_length(*second, store_of_second, 2, 512 * KiB));
}

TEST_CASE(a_request_that_is_undone_keeps_its_room_of_the_session_until_its_writer_ends)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 8 * MiB, .ceiling_of_the_session = 3 * MiB, .charge_for_a_database = 0 }));
    auto [second, store_of_second] = open_a_database(*store, other_example(), "second"_utf16);

    auto first = TRY_OR_FAIL(store->open_database(example_com(), "first"_utf16, 1, at(1))).database;
    auto upgrade = TRY_OR_FAIL(first->begin_upgrade(1));
    auto store_of_first = TRY_OR_FAIL(upgrade->create_object_store("store"_utf16, {}, KeyGenerator::No));
    auto index = TRY_OR_FAIL(upgrade->create_index(store_of_first, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::Yes, MultiEntry::No));
    auto const ten = number_key(10);
    TRY_OR_FAIL(put_with_index_key(*upgrade, store_of_first, number_key(1), "first"sv, index, ten));
    TRY_OR_FAIL(upgrade->commit(at(1)));

    // The value is written before the index refuses its key.
    auto writer = TRY_OR_FAIL(first->begin_writer());
    auto const two = number_key(2);
    auto const value = value_of_length(1536 * KiB, 1);
    Array keys { ten.bytes() };
    auto refused = store_with_index_keys(*writer, store_of_first, two.bytes(), value.bytes(), index, keys.span());
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Constraint);
    EXPECT(writer->is_open());

    // SQLite keeps the memory of the pages that the request took for as long as the writer is open, whatever the
    // writer does next.
    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*second, store_of_second, 1, 1536 * KiB)), IndexedDBError::Kind::Quota);
    TRY_OR_FAIL(put(*writer, store_of_first, two, "small"sv));
    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*second, store_of_second, 1, 1536 * KiB)), IndexedDBError::Kind::Quota);

    TRY_OR_FAIL(writer->abort());
    TRY_OR_FAIL(commit_a_put_of_length(*second, store_of_second, 1, 1536 * KiB));
}

TEST_CASE(each_database_counts_for_a_charge_besides_its_pages)
{
    constexpr u64 charge = 1 * MiB;
    constexpr u64 room_for_two_databases = 2 * charge + charge / 2;
    auto open = [](IndexedDBStore& store, String const& storage_key, Utf16String const& name) {
        return store.open_database(storage_key, name, 1, at(1));
    };

    {
        auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = room_for_two_databases, .ceiling_of_the_session = 64 * MiB, .charge_for_a_database = charge }));

        // A database that was never upgraded is at version 0 and counts like any other. An open of a database that
        // is there does not count again.
        TRY_OR_FAIL(open(*store, example_com(), "first"_utf16));
        TRY_OR_FAIL(open(*store, example_com(), "first"_utf16));
        TRY_OR_FAIL(open(*store, example_com(), "first"_utf16));
        TRY_OR_FAIL(open(*store, example_com(), "second"_utf16));
        EXPECT_EQ(kind_of_failure(open(*store, example_com(), "third"_utf16)), IndexedDBError::Kind::Quota);

        TRY_OR_FAIL(open(*store, other_example(), "third"_utf16));

        // A database that is deleted gives back what it counted for.
        TRY_OR_FAIL(store->delete_database(example_com(), "first"_utf16));
        TRY_OR_FAIL(open(*store, example_com(), "third"_utf16));
    }

    {
        auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 64 * MiB, .ceiling_of_the_session = room_for_two_databases, .charge_for_a_database = charge }));

        TRY_OR_FAIL(open(*store, example_com(), "first"_utf16));
        TRY_OR_FAIL(open(*store, other_example(), "first"_utf16));
        EXPECT_EQ(kind_of_failure(open(*store, example_com(), "second"_utf16)), IndexedDBError::Kind::Quota);

        TRY_OR_FAIL(store->delete_database(example_com(), "first"_utf16));
        TRY_OR_FAIL(open(*store, example_com(), "second"_utf16));
    }
}

TEST_CASE(an_index_fill_that_the_budget_refuses_ends_the_upgrade)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 1 * MiB, .ceiling_of_the_session = 64 * MiB, .charge_for_a_database = 0 }));
    auto [database, object_store] = open_a_database(*store, example_com(), "database"_utf16);
    {
        auto writer = TRY_OR_FAIL(database->begin_writer());
        for (u64 key = 1; key <= 4; ++key)
            TRY_OR_FAIL(put(*writer, object_store, number_key(key), "value"sv));
        TRY_OR_FAIL(writer->commit(at(2)));
    }

    auto upgrade = TRY_OR_FAIL(database->begin_upgrade(2));
    auto index = TRY_OR_FAIL(upgrade->create_index(object_store, "index"_utf16, WebView::IndexedDBKeyPath { "a"_utf16 }, Unique::No, MultiEntry::No));

    // The keys of the four records are more than the budget has room for.
    auto const large_key = binary_key_of_length(400 * KiB);
    Array keys { large_key.bytes() };
    Optional<IndexedDBError> refusal;
    for (u64 key = 1; key <= 4 && !refusal.has_value(); ++key) {
        if (auto filled = fill_index(*upgrade, index, number_key(key), keys.span()); filled.is_error())
            refusal = filled.release_error();
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
    // "if creating the index failed due to quota reasons, a QuotaExceededError must be used as error"
    EXPECT(refusal.has_value());
    if (!refusal.has_value())
        return;
    EXPECT_EQ(refusal->kind, IndexedDBError::Kind::Quota);
    EXPECT(refusal->lost_the_writer);
    EXPECT(!upgrade->is_open());

    EXPECT_EQ(database->version(), 1u);
    EXPECT(TRY_OR_FAIL(database->object_stores())[0].indexes.is_empty());
}

TEST_CASE(the_names_of_databases_take_room_of_the_session)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 64 * MiB, .ceiling_of_the_session = 1 * MiB, .charge_for_a_database = 0 }));

    // A session has the name of a database in the database and in the catalog of its databases. The database of the
    // first name leaves room for the value, and the catalog does not.
    auto [database, object_store] = open_a_database(*store, example_com(), Utf16String::repeated('a', 175 * KiB));
    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*database, object_store, 1, 512 * KiB)), IndexedDBError::Kind::Quota);

    // A database of the second name has room, and its row of the catalog has not.
    EXPECT_EQ(kind_of_failure(store->open_database(example_com(), Utf16String::repeated('b', 175 * KiB), 1, at(1))), IndexedDBError::Kind::Quota);
}

TEST_CASE(the_ceiling_of_a_session_is_a_quarter_of_the_memory_of_the_machine)
{
    auto ceiling_with = [](u64 bytes_of_physical_memory) {
        return IndexedDBStore::Limits::for_a_machine_with(bytes_of_physical_memory).ceiling_of_the_session;
    };

    EXPECT_EQ(ceiling_with(24 * GiB), 6 * GiB);
    EXPECT_EQ(ceiling_with(2 * GiB), 512 * MiB);

    // A query that fails gives 0, or a negative number that reads as a huge one.
    EXPECT_EQ(ceiling_with(0), 1 * GiB);
    EXPECT_EQ(ceiling_with(static_cast<u64>(-16384)), 1 * GiB);
}

TEST_CASE(a_write_that_the_session_has_no_room_for_gives_back_free_pages)
{
    // The value takes more pages than one step of a vacuum gives back.
    constexpr size_t length = 6 * MiB;
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 16 * MiB, .ceiling_of_the_session = 8 * MiB, .charge_for_a_database = 0 }));
    auto [first, store_of_first] = open_a_database(*store, example_com(), "first"_utf16);
    auto [second, store_of_second] = open_a_database(*store, other_example(), "second"_utf16);

    TRY_OR_FAIL(commit_a_put_of_length(*first, store_of_first, 1, length));

    // A database with a writer keeps the pages that the writer freed, since a rollback takes them again. A vacuum
    // inside the writer's transaction would also keep a copy of each page that it gives back.
    {
        auto writer = TRY_OR_FAIL(first->begin_writer());
        TRY_OR_FAIL(writer->clear(store_of_first));

        auto memory_of_sqlite = sqlite3_memory_used();
        EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*second, store_of_second, 1, length)), IndexedDBError::Kind::Quota);
        EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*second, store_of_second, 1, length)), IndexedDBError::Kind::Quota);
        EXPECT(sqlite3_memory_used() < memory_of_sqlite + static_cast<i64>(1 * MiB));

        TRY_OR_FAIL(writer->commit(at(3)));
    }

    // The pages are free and still the first database's. The write that finds no room is lost with what its writer
    // wrote before it, and a write that comes after it has the room.
    auto writer = TRY_OR_FAIL(second->begin_writer());
    TRY_OR_FAIL(put(*writer, store_of_second, number_key(2), "small"sv));
    auto refused = put_of_length(*writer, store_of_second, 1, length);
    EXPECT_EQ(kind_of_failure(refused), IndexedDBError::Kind::Quota);
    EXPECT(refused.is_error() && refused.error().lost_the_writer);
    EXPECT(keys_of(*second, store_of_second).is_empty());

    TRY_OR_FAIL(commit_a_put_of_length(*second, store_of_second, 1, length));
}

TEST_CASE(an_open_that_the_session_has_no_room_for_gives_back_free_pages)
{
    constexpr u64 charge = 1 * MiB;
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 16 * MiB, .ceiling_of_the_session = 3 * charge + charge / 2, .charge_for_a_database = charge }));
    auto [first, store_of_first] = open_a_database(*store, example_com(), "first"_utf16);

    TRY_OR_FAIL(commit_a_put_of_length(*first, store_of_first, 1, 2 * MiB));
    {
        auto writer = TRY_OR_FAIL(first->begin_writer());
        TRY_OR_FAIL(writer->clear(store_of_first));
        TRY_OR_FAIL(writer->commit(at(3)));
    }

    // The free pages of the first database leave no room for what another database counts for.
    auto open_another = [&] { return store->open_database(other_example(), "second"_utf16, 1, at(4)); };
    EXPECT_EQ(kind_of_failure(open_another()), IndexedDBError::Kind::Quota);
    TRY_OR_FAIL(open_another());
}

TEST_CASE(the_catalog_gives_back_the_pages_of_a_name_that_is_gone)
{
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 64 * MiB, .ceiling_of_the_session = 1 * MiB, .charge_for_a_database = 0 }));

    auto const long_name = Utf16String::repeated('a', 175 * KiB);
    TRY_OR_FAIL(store->open_database(example_com(), long_name, 1, at(1)));
    TRY_OR_FAIL(store->delete_database(example_com(), long_name));

    // The catalog still has the pages of the name, which leave no room for the value.
    auto [database, object_store] = open_a_database(*store, example_com(), "database"_utf16);
    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*database, object_store, 1, 700 * KiB)), IndexedDBError::Kind::Quota);
    TRY_OR_FAIL(commit_a_put_of_length(*database, object_store, 1, 700 * KiB));
}

TEST_CASE(a_database_is_held_to_the_room_of_its_session_while_it_is_created)
{
    constexpr u64 ceiling = 1 * MiB;
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 64 * MiB, .ceiling_of_the_session = ceiling, .charge_for_a_database = 0 }));

    // SQLite copies the name to bind it, and again into the row that it is to write. Past the two copies the
    // database takes no more than its session has room for, where the pages of the whole name are a third copy.
    constexpr size_t length_of_name = 16 * MiB;
    auto const name = Utf16String::repeated('a', length_of_name);
    auto memory_of_sqlite = sqlite3_memory_used();
    sqlite3_memory_highwater(1);

    EXPECT_EQ(kind_of_failure(store->open_database(example_com(), name, 1, at(1))), IndexedDBError::Kind::Quota);

    auto most_memory_taken = sqlite3_memory_highwater(0) - memory_of_sqlite;
    EXPECT(most_memory_taken < static_cast<i64>(2 * length_of_name + ceiling + length_of_name / 2));
}

TEST_CASE(an_open_that_its_storage_key_has_no_room_for_leaves_free_pages_where_they_are)
{
    constexpr u64 charge = 1 * MiB;
    auto store = TRY_OR_FAIL(IndexedDBStore::create_transient({ .budget_of_a_storage_key = 4 * charge, .ceiling_of_the_session = 8 * charge, .charge_for_a_database = charge }));
    auto [first, store_of_first] = open_a_database(*store, example_com(), "first"_utf16);

    TRY_OR_FAIL(commit_a_put_of_length(*first, store_of_first, 1, 2 * MiB));
    {
        auto writer = TRY_OR_FAIL(first->begin_writer());
        TRY_OR_FAIL(writer->clear(store_of_first));
        TRY_OR_FAIL(writer->commit(at(3)));
    }

    // A fourth database is more than its storage key has room for, and the session has room for it.
    for (auto const& name : { "first"_utf16, "second"_utf16, "third"_utf16 })
        TRY_OR_FAIL(store->open_database(other_example(), name, 1, at(4)));
    EXPECT_EQ(kind_of_failure(store->open_database(other_example(), "fourth"_utf16, 1, at(4))), IndexedDBError::Kind::Quota);

    // The free pages of the first database are still held, and leave the session no room for the value.
    auto [third, store_of_third] = open_a_database(*store, "https://third.example"_string, "database"_utf16);
    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*third, store_of_third, 1, 1536 * KiB)), IndexedDBError::Kind::Quota);
    TRY_OR_FAIL(commit_a_put_of_length(*third, store_of_third, 1, 1536 * KiB));
}

// Two databases of one storage key. The first gives back the pages of a value that it no longer has, and then the
// second is filled until its storage key has no room for more. Returns how many small values the second took last.
template<typename Callback>
static size_t small_values_that_a_storage_key_has_room_for_after_a_vacuum(Callback after_the_vacuum)
{
    constexpr size_t length = 16 * MiB;
    auto store = MUST(IndexedDBStore::create_transient({ .budget_of_a_storage_key = length + 2 * MiB, .ceiling_of_the_session = length + 8 * MiB, .charge_for_a_database = 0 }));
    auto [first, store_of_first] = open_a_database(*store, example_com(), "first"_utf16);
    auto [second, store_of_second] = open_a_database(*store, example_com(), "second"_utf16);

    MUST(commit_a_put_of_length(*first, store_of_first, 1, length));
    {
        auto writer = MUST(first->begin_writer());
        MUST(writer->clear(store_of_first));
        MUST(writer->commit(at(3)));
    }

    // The session has no room for the value while the first database holds its free pages.
    EXPECT_EQ(kind_of_failure(commit_a_put_of_length(*second, store_of_second, 1, length - 4 * MiB)), IndexedDBError::Kind::Quota);
    after_the_vacuum(*first);

    MUST(commit_a_put_of_length(*second, store_of_second, 1, length));

    // Each of the small values takes a page.
    auto const small_value = value_of_length(3000, 1);
    auto writer = MUST(second->begin_writer());
    size_t values_taken = 0;
    for (u64 key = 2; key < 2000; ++key) {
        auto const key_of_record = number_key(key);
        if (writer->store_record(store_of_second, key_of_record.bytes(), small_value.bytes(), NoOverwrite::No, {}).is_error())
            break;
        ++values_taken;
    }
    return values_taken;
}

TEST_CASE(a_vacuum_takes_its_pages_off_what_the_storage_key_is_charged_for)
{
    // A writer that ends charges its storage key for what its database has in use by then. After a vacuum the
    // storage key has to have been charged like that already.
    auto with_a_writer_that_ends = small_values_that_a_storage_key_has_room_for_after_a_vacuum([](IndexedDBDatabase& first) {
        auto writer = MUST(first.begin_writer());
        MUST(writer->abort());
    });
    auto without_one = small_values_that_a_storage_key_has_room_for_after_a_vacuum([](IndexedDBDatabase&) { });

    EXPECT(with_a_writer_that_ends > 0);
    EXPECT_EQ(without_one, with_a_writer_that_ends);
}
