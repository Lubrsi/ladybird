/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <AK/RefPtr.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibWebView/IndexedDBSession.h>

namespace WebView {

namespace Message = IndexedDBMessages;

using ConnectionQueue = IndexedDBConnectionQueue;
using Scheduler = IndexedDBTransactionScheduler;

struct IndexedDBSession::Agent {
    AK_ALLOC_WITH_KMALLOC;

    Vector<ChannelID> channels;
    Vector<ReplyToChannel> replies;
};

struct IndexedDBSession::Channel {
    AK_ALLOC_WITH_KMALLOC;

    ChannelID id;
    AgentID agent;
    String storage_key;
    bool is_retired { false };

    u64 factory_requests_made { 0 };
    u64 connections_told_of { 0 };
    u64 transactions_created { 0 };

    // What the channel can still name. A number that it has used up and that is not here is of something that ended.
    HashMap<Message::ConnectionNumber, WeakPtr<Connection>> open_connections;
    HashMap<Message::TransactionNumber, WeakPtr<Transaction>> live_transactions;
};

// A request to open a database or to delete one, while it is in the connection queue of its database.
struct IndexedDBSession::FactoryRequest {
    ChannelID channel;
    Message::FactoryRequestNumber number;

    // The version that an open asked for, and from when its database was found the one that it opens it at. A
    // delete has none.
    Optional<u64> version;

    // The object stores as an open found them, kept for an open that waits to upgrade.
    Vector<IndexedDBObjectStoreDescription> object_stores {};

    bool is_being_upgraded_for { false };
};

struct IndexedDBSession::QueuedRequest {
    u64 number { 0 };
    Message::Operation operation;
};

// https://w3c.github.io/IndexedDB/#transaction-construct
struct IndexedDBSession::Transaction : public Weakable<Transaction> {
    AK_ALLOC_WITH_KMALLOC;

    using Entry = Variant<QueuedRequest, Message::CreateObjectStore, Message::RenameObjectStore, Message::DeleteObjectStore>;

    Transaction(NamedDatabase& database, Scheduler::TransactionID id, ChannelID channel, Message::TransactionName name, ConnectionQueue::ConnectionID connection)
        : database(database)
        , id(id)
        , channel(channel)
        , name(move(name))
        , connection(connection)
    {
    }

    bool is_upgrade() const { return open_request.has_value(); }
    bool has_in_scope(Message::ObjectStoreName const&) const;
    bool allows(Message::Operation const&) const;
    i64 id_of(Message::ObjectStoreName const&) const;

    ErrorOr<Message::ResultOfRequest, IndexedDBError> carry_out(Message::Operation const&);
    ErrorOr<void, IndexedDBError> carry_out(Message::CreateObjectStore const&);
    ErrorOr<void, IndexedDBError> carry_out(Message::RenameObjectStore const&);
    ErrorOr<void, IndexedDBError> carry_out(Message::DeleteObjectStore const&);

    NamedDatabase& database;
    Scheduler::TransactionID id;
    ChannelID channel;
    Message::TransactionName name;
    ConnectionQueue::ConnectionID connection;

    // The open that an upgrade transaction upgrades the database for.
    Optional<ConnectionQueue::RequestID> open_request;

    bool writes { false };

    // The ids of its object stores, sorted. An upgrade transaction's is every object store of the database.
    Vector<i64> scope;

    // There from when a transaction that writes has begun.
    OwnPtr<IndexedDBDatabase::Writer> writer;

    // What the transaction was asked to do and has not done, in the order in which it was asked.
    Queue<Entry> entries;
    u64 requests_placed { 0 };

    // An upgrade transaction's: how many object stores it was asked to create, and the ids of the ones that it has.
    u64 object_stores_to_create { 0 };
    Vector<i64> created_object_stores;
};

// https://w3c.github.io/IndexedDB/#database-connection
// A connection that its channel was told of.
struct IndexedDBSession::Connection : public Weakable<Connection> {
    AK_ALLOC_WITH_KMALLOC;

    Connection(NamedDatabase& database, ConnectionQueue::ConnectionID id, ChannelID channel, Message::ConnectionNumber number)
        : database(database)
        , id(id)
        , channel(channel)
        , number(number)
    {
    }

    NamedDatabase& database;
    ConnectionQueue::ConnectionID id;
    ChannelID channel;
    Message::ConnectionNumber number;

    bool is_close_pending { false };

    // An upgrade transaction is there while it is live. A connection of an open that needed no upgrade never has one.
    bool was_opened_by_an_upgrade { false };
    WeakPtr<Transaction> upgrade;
};

// What a storage key and a name stand for: the requests for a database of that name, the connections to the one that
// there is, and their transactions.
struct IndexedDBSession::NamedDatabase {
    AK_ALLOC_WITH_KMALLOC;

    NamedDatabase(String storage_key, Utf16String name)
        : storage_key(move(storage_key))
        , name(move(name))
    {
    }

    void begin_the_writer_of(Scheduler::TransactionID);

    String storage_key;
    Utf16String name;

    // The database as the request that was handled last found it or left it.
    RefPtr<IndexedDBDatabase> database;

    ConnectionQueue queue;
    Scheduler scheduler;

    HashMap<ConnectionQueue::RequestID, FactoryRequest> requests;
    HashMap<ConnectionQueue::ConnectionID, NonnullOwnPtr<Connection>> connections;
    HashMap<Scheduler::TransactionID, NonnullOwnPtr<Transaction>> transactions;

    // The transaction that has the writer of the database, which a failure of another transaction's request can end.
    Optional<Scheduler::TransactionID> transaction_of_the_writer;

    bool has_work { false };
};

void IndexedDBSession::NamedDatabase::begin_the_writer_of(Scheduler::TransactionID transaction_id)
{
    auto& transaction = *transactions.get(transaction_id).release_value();

    // A transaction has the writer of its database only once the writer has begun.
    auto writer = database->begin_writer();
    if (writer.is_error()) {
        scheduler.did_fail_to_begin(transaction_id, writer.error().kind);
        return;
    }

    transaction.writer = writer.release_value();
    transaction_of_the_writer = transaction_id;
    scheduler.did_begin(transaction_id);
}

bool IndexedDBSession::Transaction::has_in_scope(Message::ObjectStoreName const& object_store) const
{
    return object_store.visit(
        // The database knows which ids are of its object stores. An upgrade transaction may name any of them.
        [&](i64 object_store_id) { return is_upgrade() || binary_search(scope.span(), object_store_id) != nullptr; },
        // Only an upgrade transaction is asked to create object stores.
        [&](Message::ObjectStoreOfUpgrade const& created) { return created.number >= 1 && created.number <= object_stores_to_create; });
}

// https://w3c.github.io/IndexedDB/#transaction-construct
// A renderer throws for a request that the mode or the scope of its transaction does not allow, before it sends
// anything.
bool IndexedDBSession::Transaction::allows(Message::Operation const& operation) const
{
    bool changes_records = operation.has<Message::StoreRecord>() || operation.has<Message::DeleteRecords>() || operation.has<Message::Clear>();
    if (changes_records && !writes)
        return false;

    return operation.visit([&](auto const& specific) { return has_in_scope(specific.object_store); });
}

i64 IndexedDBSession::Transaction::id_of(Message::ObjectStoreName const& object_store) const
{
    return object_store.visit(
        [](i64 object_store_id) { return object_store_id; },
        // The change that creates an object store runs before whatever names the object store by its number, and
        // nothing more of an upgrade runs once one of its changes has failed.
        [&](Message::ObjectStoreOfUpgrade const& created) { return created_object_stores[created.number - 1]; });
}

// https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
// 5.2. Let result be the result of performing operation.
ErrorOr<Message::ResultOfRequest, IndexedDBError> IndexedDBSession::Transaction::carry_out(Message::Operation const& operation)
{
    using Result = ErrorOr<Message::ResultOfRequest, IndexedDBError>;

    auto& records = *database.database;
    auto bytes_of = [](Optional<ByteBuffer> const& buffer) { return buffer.map([](auto const& bytes) { return bytes.bytes(); }); };

    return operation.visit(
        [&](Message::StoreRecord const& store) -> Result {
            auto value = store.value.visit(
                [](ByteBuffer const& whole_value) -> IndexedDBValue { return whole_value.bytes(); },
                [](Message::ValueSegments const& segments) -> IndexedDBValue {
                    return IndexedDBValueSegments { segments.before_the_key.bytes(), segments.after_the_key.bytes() };
                });

            auto stored = TRY(writer->store_record(id_of(store.object_store), bytes_of(store.key), value, store.no_overwrite, {}));
            return Message::RecordStored { stored.generated_key };
        },
        [&](Message::DeleteRecords const& delete_records) -> Result {
            TRY(writer->delete_records(id_of(delete_records.object_store), delete_records.range.as_range_of_database()));
            return Message::RecordsDeleted {};
        },
        [&](Message::Clear const& clear) -> Result {
            TRY(writer->clear(id_of(clear.object_store)));
            return Message::RecordsDeleted {};
        },
        [&](Message::ReadRecords const& read) -> Result {
            auto read_records = TRY(records.read_records(id_of(read.object_store), read.range.as_range_of_database(), read.direction, read.count, read.with_values));
            return Message::RecordsRead { move(read_records) };
        },
        [&](Message::Count const& count) -> Result {
            auto number_of_records = TRY(records.count(id_of(count.object_store), count.range.as_range_of_database()));
            return Message::RecordsCounted { number_of_records };
        },
        [&](Message::IterateCursor const& iterate) -> Result {
            auto record = TRY(records.iterate_cursor(id_of(iterate.object_store), iterate.range.as_range_of_database(), iterate.direction, bytes_of(iterate.position), bytes_of(iterate.key), iterate.count, iterate.with_values));
            return Message::CursorIterated { move(record) };
        });
}

ErrorOr<void, IndexedDBError> IndexedDBSession::Transaction::carry_out(Message::CreateObjectStore const& create)
{
    auto object_store_id = TRY(writer->create_object_store(create.name, create.key_path, create.key_generator));
    created_object_stores.append(object_store_id);
    return {};
}

ErrorOr<void, IndexedDBError> IndexedDBSession::Transaction::carry_out(Message::RenameObjectStore const& rename)
{
    return writer->rename_object_store(id_of(rename.object_store), rename.name);
}

ErrorOr<void, IndexedDBError> IndexedDBSession::Transaction::carry_out(Message::DeleteObjectStore const& delete_object_store)
{
    return writer->delete_object_store(id_of(delete_object_store.object_store));
}

IndexedDBSession::IndexedDBSession(NonnullOwnPtr<IndexedDBStore> store, Function<UnixDateTime()> clock)
    : m_store(move(store))
    , m_clock(move(clock))
{
}

IndexedDBSession::~IndexedDBSession() = default;

IndexedDBSession::AgentID IndexedDBSession::add_agent()
{
    AgentID id { ++m_last_agent_id };
    m_agents.set(id, make<Agent>());
    return id;
}

IndexedDBSession::ChannelID IndexedDBSession::add_channel(AgentID agent_id, String storage_key)
{
    auto& agent = *m_agents.get(agent_id).release_value();

    auto channel = make<Channel>();
    channel->id = ChannelID { ++m_last_channel_id };
    channel->agent = agent_id;
    channel->storage_key = move(storage_key);

    auto id = channel->id;
    agent.channels.append(id);
    m_channels.set(id, move(channel));
    return id;
}

void IndexedDBSession::reply(ChannelID channel_id, Message::Reply reply)
{
    auto channel = m_channels.get(channel_id);
    if (!channel.has_value() || (*channel)->is_retired)
        return;

    auto& agent = *m_agents.get((*channel)->agent).release_value();
    agent.replies.append({ channel_id, move(reply) });
}

Vector<IndexedDBSession::ReplyToChannel> IndexedDBSession::take_replies(AgentID agent_id)
{
    auto& agent = *m_agents.get(agent_id).release_value();
    return exchange(agent.replies, {});
}

Vector<IndexedDBSession::ChannelID> IndexedDBSession::take_channels_at_fault()
{
    return exchange(m_channels_at_fault, {});
}

void IndexedDBSession::retire_for_a_false_claim(Channel& channel)
{
    if (channel.is_retired)
        return;

    m_channels_at_fault.append(channel.id);
    channel.is_retired = true;
    release_what_it_has(channel);
}

void IndexedDBSession::retire_channel(ChannelID channel_id)
{
    auto found_channel = m_channels.get(channel_id);
    if (!found_channel.has_value())
        return;
    auto& channel = **found_channel;

    if (!channel.is_retired) {
        channel.is_retired = true;
        release_what_it_has(channel);
    }
    run_what_can_run();

    auto& agent = *m_agents.get(channel.agent).release_value();
    agent.channels.remove_first_matching([&](auto id) { return id == channel_id; });
    m_channels.remove(channel_id);
}

void IndexedDBSession::retire_agent(AgentID agent_id)
{
    auto found_agent = m_agents.get(agent_id);
    if (!found_agent.has_value())
        return;
    auto& agent = **found_agent;

    // Every channel is retired before what any of them had is released, so that nothing which waited for one of
    // them is done for another.
    Vector<Channel*> channels_to_release;
    for (auto channel_id : agent.channels) {
        auto& channel = *m_channels.get(channel_id).release_value();
        if (!channel.is_retired)
            channels_to_release.append(&channel);
        channel.is_retired = true;
    }

    for (auto* channel : channels_to_release)
        release_what_it_has(*channel);
    run_what_can_run();

    for (auto channel_id : agent.channels)
        m_channels.remove(channel_id);
    m_agents.remove(agent_id);
}

// https://w3c.github.io/IndexedDB/#close-a-database-connection
// What a retired channel has is ended as the connections of a renderer that is gone are: with the forced flag.
void IndexedDBSession::release_what_it_has(Channel& channel)
{
    VERIFY(channel.is_retired);

    Vector<Transaction*> transactions;
    for (auto const& entry : channel.live_transactions) {
        if (auto* transaction = entry.value.ptr())
            transactions.append(transaction);
    }

    Vector<Connection*> connections;
    for (auto const& entry : channel.open_connections) {
        auto* connection = entry.value.ptr();
        if (!connection)
            continue;

        connections.append(connection);
        if (auto* upgrade = connection->upgrade.ptr())
            transactions.append(upgrade);
    }

    // 2. If the forced flag is true, then for each transaction created using connection run abort a transaction with
    //    transaction and newly created "AbortError" DOMException.
    for (auto* transaction : transactions) {
        transaction->database.scheduler.ask_to_abort(transaction->id);
        has_work(transaction->database);
    }

    for (auto* connection : connections) {
        auto& database = connection->database;
        auto connection_id = connection->id;

        // 1. Set connection's close pending flag to true.
        connection->is_close_pending = true;

        // 3. Wait for all transactions created using connection to complete. Once they are complete, connection is
        //    closed.
        database.queue.ask_to_close(connection_id);
        forget_if_closed(database, connection_id);
        has_work(database);
    }

    // The channel's requests are in the connection queues of its storage key.
    auto databases = m_databases.get(channel.storage_key);
    if (!databases.has_value())
        return;

    for (auto& entry : *databases) {
        auto& database = *entry.value;

        Vector<ConnectionQueue::RequestID> requests;
        for (auto const& request : database.requests) {
            if (request.value.channel == channel.id)
                requests.append(request.key);
        }

        for (auto request : requests)
            withdraw(database, request);
    }
}

void IndexedDBSession::withdraw(NamedDatabase& database, ConnectionQueue::RequestID request_id)
{
    // An open whose upgrade is under way ends with the upgrade transaction, which is aborted with its channel.
    if (database.requests.get(request_id)->is_being_upgraded_for)
        return;

    database.queue.withdraw_request(request_id);
    database.requests.remove(request_id);
    has_work(database);
}

void IndexedDBSession::forget(Connection& connection)
{
    auto& database = connection.database;
    auto connection_id = connection.id;

    if (auto channel = m_channels.get(connection.channel); channel.has_value())
        (*channel)->open_connections.remove(connection.number);
    database.connections.remove(connection_id);
}

void IndexedDBSession::forget_if_closed(NamedDatabase& database, ConnectionQueue::ConnectionID connection_id)
{
    if (database.queue.has_connection(connection_id))
        return;

    // The connection of an open that waits to upgrade is one that no channel was told of.
    if (auto connection = database.connections.get(connection_id); connection.has_value())
        forget(**connection);
}

IndexedDBSession::Connection* IndexedDBSession::connection_named_by(Channel& channel, Message::ConnectionNumber number)
{
    // A channel is told of each of its connections.
    if (number.value() == 0 || number.value() > channel.connections_told_of) {
        retire_for_a_false_claim(channel);
        return nullptr;
    }

    return channel.open_connections.get(number).value_or(nullptr);
}

IndexedDBSession::Transaction* IndexedDBSession::transaction_named_by(Channel& channel, Message::TransactionName const& name)
{
    return name.visit(
        [&](Message::TransactionNumber number) -> Transaction* {
            if (number.value() == 0 || number.value() > channel.transactions_created) {
                retire_for_a_false_claim(channel);
                return nullptr;
            }

            return channel.live_transactions.get(number).value_or(nullptr);
        },
        [&](Message::UpgradeOf const& upgrade_of) { return upgrade_named_by(channel, upgrade_of.connection); });
}

IndexedDBSession::Transaction* IndexedDBSession::upgrade_named_by(Channel& channel, Message::ConnectionNumber number)
{
    auto* connection = connection_named_by(channel, number);
    if (!connection)
        return nullptr;

    if (!connection->was_opened_by_an_upgrade) {
        retire_for_a_false_claim(channel);
        return nullptr;
    }

    return connection->upgrade.ptr();
}

IndexedDBSession::NamedDatabase& IndexedDBSession::named_database(String const& storage_key, Utf16String const& name)
{
    auto& databases = m_databases.ensure(storage_key);
    if (auto database = databases.get(name); database.has_value())
        return **database;

    auto database = make<NamedDatabase>(storage_key, name);
    auto& added_database = *database;
    databases.set(name, move(database));
    return added_database;
}

// https://w3c.github.io/IndexedDB/#transaction-scheduling
// AD-HOC: The spec has one order in which the agents of a storage key add their requests to a connection queue and
//         create their transactions. Agents in different processes do both at the same time, so the order here is
//         the one in which their commands are handled.
void IndexedDBSession::handle(ChannelID channel_id, Message::Command command)
{
    // A command can be on its way when its channel is retired.
    auto channel = m_channels.get(channel_id);
    if (!channel.has_value() || (*channel)->is_retired)
        return;

    command.visit([&](auto& specific) { handle(**channel, specific); });
    run_what_can_run();
}

void IndexedDBSession::add_factory_request(Channel& channel, Utf16String const& name, ConnectionQueue::RequestKind kind, Optional<u64> version)
{
    Message::FactoryRequestNumber number { ++channel.factory_requests_made };
    auto& database = named_database(channel.storage_key, name);

    // https://w3c.github.io/IndexedDB/#open-a-database-connection
    // 1. Let queue be the connection queue for storageKey and name.
    // 2. Add request to queue.
    auto request = database.queue.add_request(kind);
    database.requests.set(request, FactoryRequest { .channel = channel.id, .number = number, .version = version });
    has_work(database);
}

void IndexedDBSession::handle(Channel& channel, Message::Open& open)
{
    add_factory_request(channel, open.name, ConnectionQueue::RequestKind::Open, open.version);
}

void IndexedDBSession::handle(Channel& channel, Message::DeleteDatabase& delete_database)
{
    add_factory_request(channel, delete_database.name, ConnectionQueue::RequestKind::Delete, {});
}

// https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
void IndexedDBSession::handle(Channel& channel, Message::ListDatabases&)
{
    Message::FactoryRequestNumber number { ++channel.factory_requests_made };

    // 4.1. Let databases be the set of databases in storageKey. If this cannot be determined for any reason, then
    //      queue a database task to reject p with an appropriate error (e.g. an "UnknownError" DOMException) and
    //      terminate these steps.
    auto databases = m_store->databases(channel.storage_key);
    if (databases.is_error()) {
        reply(channel.id, Message::DatabaseList { number, databases.error().kind });
        return;
    }

    reply(channel.id, Message::DatabaseList { number, databases.release_value() });
}

// https://w3c.github.io/IndexedDB/#close-a-database-connection
void IndexedDBSession::handle(Channel& channel, Message::CloseConnection& close)
{
    auto* connection = connection_named_by(channel, close.connection);
    if (!connection)
        return;

    auto& database = connection->database;
    auto connection_id = connection->id;

    // 1. Set connection's close pending flag to true.
    connection->is_close_pending = true;

    // 3. Wait for all transactions created using connection to complete. Once they are complete, connection is
    //    closed.
    database.queue.ask_to_close(connection_id);
    forget_if_closed(database, connection_id);
    has_work(database);
}

void IndexedDBSession::handle(Channel& channel, Message::VersionChangeHandled& handled)
{
    auto* connection = connection_named_by(channel, handled.connection);
    if (!connection)
        return;

    auto& database = connection->database;
    auto answer = database.queue.take_answer_to_version_change(connection->id, handled.notification);
    if (answer == ConnectionQueue::Answer::NeverAskedFor) {
        retire_for_a_false_claim(channel);
        return;
    }

    has_work(database);
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-transaction
void IndexedDBSession::handle(Channel& channel, Message::CreateTransaction& create)
{
    Message::TransactionNumber number { ++channel.transactions_created };

    // A transaction of a connection that has closed is one that has ended.
    auto* connection = connection_named_by(channel, create.connection);
    if (!connection)
        return;

    // NB: A renderer throws in each of these steps before it sends anything.
    // 1. If a live upgrade transaction is associated with the connection, throw an "InvalidStateError" DOMException.
    if (connection->upgrade) {
        retire_for_a_false_claim(channel);
        return;
    }

    // 2. If this's close pending flag is true, then throw an "InvalidStateError" DOMException.
    if (connection->is_close_pending) {
        retire_for_a_false_claim(channel);
        return;
    }

    // 5. If scope is empty, throw an "InvalidAccessError" DOMException.
    if (create.scope.is_empty()) {
        retire_for_a_false_claim(channel);
        return;
    }

    // 7. Let transaction be a newly created transaction with this connection, mode, options' durability member, and
    //    the set of object stores named in scope.
    auto& database = connection->database;
    quick_sort(create.scope);
    auto id = database.scheduler.create_transaction(create.mode, create.scope);

    auto transaction = make<Transaction>(database, id, channel.id, number, connection->id);
    transaction->writes = create.mode == Scheduler::Mode::ReadWrite;
    transaction->scope = move(create.scope);

    database.queue.transaction_was_created(connection->id);
    channel.live_transactions.set(number, transaction->make_weak_ptr());
    database.transactions.set(id, move(transaction));
    has_work(database);
}

// https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
void IndexedDBSession::handle(Channel& channel, Message::PlaceRequest& place)
{
    // 1. Let transaction be the transaction associated with source.
    auto* transaction = transaction_named_by(channel, place.transaction);
    if (!transaction)
        return;

    if (!transaction->allows(place.operation)) {
        retire_for_a_false_claim(channel);
        return;
    }

    // 2. Assert: transaction's state is active.
    // NB: A renderer makes no request of a transaction that it has asked to commit.
    auto& database = transaction->database;
    if (!database.scheduler.place(transaction->id, Scheduler::EntryKind::Request).has_value()) {
        retire_for_a_false_claim(channel);
        return;
    }

    // 4. Add request to the end of transaction's request list.
    transaction->entries.enqueue(QueuedRequest { ++transaction->requests_placed, move(place.operation) });
    has_work(database);
}

// https://w3c.github.io/IndexedDB/#commit-a-transaction
void IndexedDBSession::handle(Channel& channel, Message::Commit& commit)
{
    auto* transaction = transaction_named_by(channel, commit.transaction);
    if (!transaction)
        return;

    // 1. Set transaction's state to committing.
    // NB: A renderer asks once, and has started on no result that it was not sent.
    auto& database = transaction->database;
    if (!database.scheduler.ask_to_commit(transaction->id, commit.results_started)) {
        retire_for_a_false_claim(channel);
        return;
    }

    has_work(database);
}

// https://w3c.github.io/IndexedDB/#abort-a-transaction
void IndexedDBSession::handle(Channel& channel, Message::Abort& abort)
{
    // 1. If transaction's state is finished, abort these steps.
    // NB: A channel may ask for the abort of a transaction of its own until the transaction has ended, also after it
    //     has asked for a commit that is yet to be carried out. For one that has ended, the abort was on its way.
    auto* transaction = transaction_named_by(channel, abort.transaction);
    if (!transaction)
        return;

    auto& database = transaction->database;
    database.scheduler.ask_to_abort(transaction->id);
    has_work(database);
}

template<typename Change>
void IndexedDBSession::place_change(Channel& channel, Transaction& upgrade, Change& change)
{
    // NB: A renderer changes nothing in a transaction that it has asked to commit.
    auto& database = upgrade.database;
    if (!database.scheduler.place(upgrade.id, Scheduler::EntryKind::Change).has_value()) {
        retire_for_a_false_claim(channel);
        return;
    }

    upgrade.entries.enqueue(move(change));
    has_work(database);
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-createobjectstore
void IndexedDBSession::handle(Channel& channel, Message::CreateObjectStore& create)
{
    // 2. Let transaction be database's upgrade transaction if it is not null, or throw an "InvalidStateError"
    //    DOMException otherwise.
    auto* upgrade = upgrade_named_by(channel, create.upgrade);
    if (!upgrade)
        return;

    ++upgrade->object_stores_to_create;
    place_change(channel, *upgrade, create);
}

void IndexedDBSession::handle(Channel& channel, Message::RenameObjectStore& rename)
{
    auto* upgrade = upgrade_named_by(channel, rename.upgrade);
    if (!upgrade)
        return;

    if (!upgrade->has_in_scope(rename.object_store)) {
        retire_for_a_false_claim(channel);
        return;
    }

    place_change(channel, *upgrade, rename);
}

void IndexedDBSession::handle(Channel& channel, Message::DeleteObjectStore& delete_object_store)
{
    auto* upgrade = upgrade_named_by(channel, delete_object_store.upgrade);
    if (!upgrade)
        return;

    if (!upgrade->has_in_scope(delete_object_store.object_store)) {
        retire_for_a_false_claim(channel);
        return;
    }

    place_change(channel, *upgrade, delete_object_store);
}

void IndexedDBSession::has_work(NamedDatabase& database)
{
    if (database.has_work)
        return;

    database.has_work = true;
    m_databases_with_work.append(&database);
}

void IndexedDBSession::run_what_can_run()
{
    while (!m_databases_with_work.is_empty()) {
        auto& database = *m_databases_with_work.take_first();
        database.has_work = false;
        run(database);

        if (database.has_work || !database.queue.is_empty())
            continue;

        // Nothing asks for a database of this name any more, and nothing is connected to one.
        VERIFY(database.requests.is_empty());
        VERIFY(database.connections.is_empty());
        VERIFY(database.transactions.is_empty());

        auto storage_key = database.storage_key;
        auto name = database.name;
        auto& databases = *m_databases.get(storage_key);
        databases.remove(name);
        if (databases.is_empty())
            m_databases.remove(storage_key);
    }
}

// What is done can make more to do or take away what there was to do, so one thing is asked for at a time, until
// neither the transactions nor the connection queue have anything.
void IndexedDBSession::run(NamedDatabase& database)
{
    for (;;) {
        if (auto action = database.scheduler.next_action(); action.has_value()) {
            action->visit([&](auto const& specific) { perform(database, specific); });
            continue;
        }

        if (auto action = database.queue.next_action(); action.has_value()) {
            action->visit([&](auto const& specific) { perform(database, specific); });
            continue;
        }

        return;
    }
}

void IndexedDBSession::perform(NamedDatabase& database, Scheduler::Begin const& begin)
{
    database.begin_the_writer_of(begin.transaction);
}

void IndexedDBSession::perform(NamedDatabase& database, Scheduler::Run const& run)
{
    auto& transaction = *database.transactions.get(run.transaction).release_value();
    auto channel = transaction.channel;

    auto entry = transaction.entries.dequeue();
    auto failure = entry.visit(
        [&](QueuedRequest const& request) { return carry_out(transaction, request); },
        [&](auto const& change) -> Optional<IndexedDBError> {
            // A change has no result. One that cannot be made ends the upgrade, which its renderer is told of.
            auto changed = transaction.carry_out(change);
            if (changed.is_error())
                return changed.release_error();
            return {};
        });

    database.scheduler.did_run(run.transaction, run.place, failure.map([](auto const& error) { return error.kind; }));
    if (failure.has_value())
        did_fail(database, channel, *failure);
}

// https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
Optional<IndexedDBError> IndexedDBSession::carry_out(Transaction& transaction, QueuedRequest const& request)
{
    auto result = transaction.carry_out(request.operation);

    // 5.6. Queue a database task to run these steps:
    if (!result.is_error()) {
        // 5.6.4.3. Fire a success event at request.
        reply(transaction.channel, Message::RequestResult { transaction.name, request.number, result.release_value() });
        return {};
    }
    auto error = result.release_error();

    // 5.6.3.3. Fire an error event at request.
    // NB: A renderer that claimed what cannot be so hears no more.
    if (error.kind != IndexedDBError::Kind::InvalidClaim)
        reply(transaction.channel, Message::RequestResult { transaction.name, request.number, error.kind });
    return error;
}

// What a failure of the store means besides the request that it was the failure of.
void IndexedDBSession::did_fail(NamedDatabase& database, ChannelID channel_id, IndexedDBError const& error)
{
    // The writer of the database is gone. It can be another transaction's than the one that the failure was in, since
    // a read can lose it.
    if (error.lost_the_writer && database.transaction_of_the_writer.has_value())
        database.scheduler.abort(*database.transaction_of_the_writer, error.kind);

    if (error.kind != IndexedDBError::Kind::InvalidClaim)
        return;

    if (auto channel = m_channels.get(channel_id); channel.has_value())
        retire_for_a_false_claim(**channel);
}

// https://w3c.github.io/IndexedDB/#commit-a-transaction
void IndexedDBSession::perform(NamedDatabase& database, Scheduler::Commit const& commit)
{
    auto& transaction = *database.transactions.get(commit.transaction).release_value();

    // 2.3. Attempt to write any outstanding changes made by transaction to the database, considering transaction's
    //      durability hint.
    // 2.4. If an error occurs while writing the changes to the database, then run abort a transaction with
    //      transaction and an appropriate type for the error, for example a QuotaExceededError or an "UnknownError"
    //      DOMException, and terminate these steps.
    if (transaction.writer) {
        if (auto committed = transaction.writer->commit(m_clock()); committed.is_error()) {
            database.scheduler.did_fail_to_commit(commit.transaction, committed.error().kind);
            return;
        }
    }
    database.scheduler.did_commit(commit.transaction);

    // 2.5. Queue a database task to run these steps:
    // 2.5.3. Fire an event named complete at transaction.
    reply(transaction.channel, Message::TransactionCompleted { transaction.name, transaction.created_object_stores });
    did_finish(transaction, ConnectionQueue::WasCommitted::Yes);
}

// https://w3c.github.io/IndexedDB/#abort-a-transaction
void IndexedDBSession::perform(NamedDatabase& database, Scheduler::RollBack const& roll_back)
{
    auto& transaction = *database.transactions.get(roll_back.transaction).release_value();

    // 2. All the changes made to the database by the transaction are reverted. For upgrade transactions this includes
    //    changes to the set of object stores and indexes, as well as the change to the version. Any object stores and
    //    indexes which were created during the transaction are now considered deleted for the purposes of other
    //    algorithms.
    // NB: The database tries a rollback that failed again before it runs anything else.
    if (transaction.writer)
        (void)transaction.writer->abort();
    database.scheduler.did_roll_back(roll_back.transaction);

    // 6. For each request of transaction's request list, abort the steps to asynchronously execute a request for
    //    request, set request's processed flag to true, and queue a database task to run these steps:
    // NB: The renderer has the list, and takes this step when it hears of the abort.
    // 7. Queue a database task to run these steps:
    // 7.2. Fire an event named abort at transaction with its bubbles attribute initialized to true.
    reply(transaction.channel, Message::TransactionAborted { transaction.name, roll_back.cause });
    did_finish(transaction, ConnectionQueue::WasCommitted::No);
}

void IndexedDBSession::did_finish(Transaction& transaction, ConnectionQueue::WasCommitted was_committed)
{
    auto& database = transaction.database;
    auto transaction_id = transaction.id;
    auto connection_id = transaction.connection;
    auto open_request = transaction.open_request;

    if (database.transaction_of_the_writer == transaction_id)
        database.transaction_of_the_writer.clear();

    if (auto channel = m_channels.get(transaction.channel); channel.has_value() && transaction.name.has<Message::TransactionNumber>())
        (*channel)->live_transactions.remove(transaction.name.get<Message::TransactionNumber>());
    database.transactions.remove(transaction_id);

    database.queue.transaction_did_end(connection_id);

    if (open_request.has_value()) {
        // https://w3c.github.io/IndexedDB/#upgrade-a-database
        // 11. Wait for transaction to finish.
        // NB: The queue takes the steps of the open that come after this one, and says what came of the open.
        auto outcome = database.queue.upgrade_did_end(*open_request, was_committed);
        auto request = database.requests.take(*open_request).release_value();

        if (outcome == ConnectionQueue::Outcome::Opened)
            reply(request.channel, Message::OpenSucceededAfterUpgrade { request.number });
        else
            reply(request.channel, Message::OpenFailed { request.number, Message::OpenFailed::UpgradeDidNotLeaveAConnection {} });

        drop_a_database_that_has_no_version(database);
    }

    forget_if_closed(database, connection_id);
}

// https://w3c.github.io/IndexedDB/#open-a-database-connection
// AD-HOC: The spec leaves a database whose first upgrade did not commit in place at version 0, where script cannot
//         tell it from no database: databases() leaves it out, and an open starts over. Chromium and Firefox delete
//         it, and so does this, which also stops it from being counted for its storage key.
void IndexedDBSession::drop_a_database_that_has_no_version(NamedDatabase& database)
{
    if (!database.database || database.database->version() != 0)
        return;

    // A delete that fails leaves the database for the next open to start over with.
    (void)m_store->delete_database(database.storage_key, database.name);
    database.database = nullptr;
}

// The connection gets its number when its channel is told of it, so a channel can name no connection before that.
IndexedDBSession::Connection& IndexedDBSession::tell_of_connection(NamedDatabase& database, FactoryRequest const& request, ConnectionQueue::ConnectionID connection_id)
{
    auto& channel = *m_channels.get(request.channel).release_value();
    Message::ConnectionNumber number { ++channel.connections_told_of };

    auto connection = make<Connection>(database, connection_id, channel.id, number);
    auto& told_connection = *connection;

    channel.open_connections.set(number, connection->make_weak_ptr());
    database.connections.set(connection_id, move(connection));
    return told_connection;
}

// The queue has been told that the request is over.
void IndexedDBSession::did_not_open(NamedDatabase& database, ConnectionQueue::RequestID request_id, IndexedDBError const& error)
{
    auto request = database.requests.take(request_id).release_value();

    // A renderer that claimed what cannot be so hears no more.
    if (error.kind != IndexedDBError::Kind::InvalidClaim)
        reply(request.channel, Message::OpenFailed { request.number, error.kind });

    did_fail(database, request.channel, error);
    drop_a_database_that_has_no_version(database);
}

// https://w3c.github.io/IndexedDB/#open-a-database-connection
void IndexedDBSession::perform(NamedDatabase& database, ConnectionQueue::FindDatabase const& find)
{
    auto& request = *database.requests.get(find.request);

    // NB: The store takes steps 4 to 7.
    auto opened = m_store->open_database(database.storage_key, database.name, request.version, m_clock());
    if (opened.is_error()) {
        database.queue.did_fail_to_find_database(find.request);
        did_not_open(database, find.request, opened.error());
        return;
    }
    database.database = opened.value().database;

    auto object_stores = database.database->object_stores();
    if (object_stores.is_error()) {
        database.queue.did_fail_to_find_database(find.request);
        did_not_open(database, find.request, object_stores.error());
        return;
    }

    // 8. Let connection be a new connection to db.
    // 9. Set connection's version to version.
    bool needs_upgrade = opened.value().needs_upgrade;
    auto connection_id = database.queue.did_find_database(find.request, needs_upgrade ? ConnectionQueue::NeedsUpgrade::Yes : ConnectionQueue::NeedsUpgrade::No);
    request.version = opened.value().version;

    // 10. If db's version is less than version, then:
    if (needs_upgrade) {
        request.object_stores = object_stores.release_value();
        return;
    }

    // 11. Return connection.
    auto& connection = tell_of_connection(database, request, connection_id);
    reply(request.channel, Message::OpenSucceeded { request.number, connection.number, opened.value().version, object_stores.release_value() });
    database.requests.remove(find.request);
}

// https://w3c.github.io/IndexedDB/#fire-a-version-change-event
void IndexedDBSession::perform(NamedDatabase& database, ConnectionQueue::NotifyOfVersionChange const& notify)
{
    auto const& connection = *database.connections.get(notify.connection).release_value();
    auto const& request = *database.requests.get(notify.request);

    // NB: A request to delete the database has no version, which is the null of its event.
    reply(connection.channel, Message::VersionChange { connection.number, notify.notification, database.database->version(), request.version });
}

// https://w3c.github.io/IndexedDB/#fire-a-version-change-event
void IndexedDBSession::perform(NamedDatabase& database, ConnectionQueue::NotifyOfBlock const& notify)
{
    auto const& request = *database.requests.get(notify.request);
    reply(request.channel, Message::Blocked { request.number, database.database->version(), request.version });
}

// https://w3c.github.io/IndexedDB/#upgrade-a-database
void IndexedDBSession::perform(NamedDatabase& database, ConnectionQueue::Upgrade const& upgrade)
{
    auto& request = *database.requests.get(upgrade.request);
    auto version = request.version.value();

    // 7. Let old version be db's version.
    auto old_version = database.database->version();

    // NB: The database takes steps 2 and 8.
    auto writer = database.database->begin_upgrade(version);
    if (writer.is_error()) {
        database.queue.did_fail_to_begin_upgrade(upgrade.request);
        did_not_open(database, upgrade.request, writer.error());
        return;
    }

    // 6. Start transaction.
    auto& connection = tell_of_connection(database, request, upgrade.connection);
    auto transaction_id = database.scheduler.create_upgrade_transaction();

    auto transaction = make<Transaction>(database, transaction_id, request.channel, Message::UpgradeOf { connection.number }, upgrade.connection);
    transaction->open_request = upgrade.request;
    transaction->writes = true;
    transaction->writer = writer.release_value();

    connection.was_opened_by_an_upgrade = true;
    connection.upgrade = transaction->make_weak_ptr();

    database.transactions.set(transaction_id, move(transaction));
    database.transaction_of_the_writer = transaction_id;
    database.queue.did_begin_upgrade(upgrade.request);
    database.queue.transaction_was_created(upgrade.connection);
    request.is_being_upgraded_for = true;

    // 10. Queue a database task to run these steps:
    // 10.5. Let didThrow be the result of firing a version change event named upgradeneeded at request with old
    //       version and version.
    reply(request.channel, Message::UpgradeNeeded { request.number, connection.number, old_version, version, move(request.object_stores) });
}

// https://w3c.github.io/IndexedDB/#delete-a-database
void IndexedDBSession::perform(NamedDatabase& database, ConnectionQueue::Delete const& delete_)
{
    auto request = database.requests.take(delete_.request).release_value();

    // NB: The store takes step 4 and steps 10 to 12.
    auto version = m_store->delete_database(database.storage_key, database.name);
    database.queue.did_delete(delete_.request);

    if (version.is_error()) {
        reply(request.channel, Message::DeleteFailed { request.number, version.error().kind });
        return;
    }

    database.database = nullptr;
    reply(request.channel, Message::DeleteSucceeded { request.number, version.value() });
}

}
