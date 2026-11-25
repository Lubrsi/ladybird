/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>
#include <LibWebView/IndexedDBConnectionQueue.h>
#include <LibWebView/IndexedDBMessages.h>
#include <LibWebView/IndexedDBStore.h>
#include <LibWebView/IndexedDBTransactionScheduler.h>

namespace WebView {

// The IndexedDB databases of one browsing session, as the renderers of the session use them.
//
// A renderer is not trusted. Each command is checked against what its channel has before anything is done for it, and
// a channel whose renderer claimed what cannot be so is retired.
class WEBVIEW_API IndexedDBSession {
    AK_MAKE_NONCOPYABLE(IndexedDBSession);
    AK_MAKE_NONMOVABLE(IndexedDBSession);

public:
    AK_ALLOC_WITH_KMALLOC;

    AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, AgentID);
    AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ChannelID);

    // The times that the store keeps are read from the clock.
    IndexedDBSession(NonnullOwnPtr<IndexedDBStore>, Function<UnixDateTime()> clock);
    ~IndexedDBSession();

    // An agent hears the replies to its channels in one order, which is the order in which things happened.
    AgentID add_agent();

    // The channels of an agent are retired together, before anything that waited for one of them goes on.
    void retire_agent(AgentID);

    // Whoever adds a channel says whose it is. Neither the agent nor the storage key is a renderer's to name.
    ChannelID add_channel(AgentID, String storage_key);

    // Ends what the channel has: its transactions are aborted, its connections closed and its requests withdrawn.
    void retire_channel(ChannelID);

    // Commands are handled in the order of these calls. One for a channel that is retired does nothing.
    void handle(ChannelID, IndexedDBMessages::Command);

    struct ReplyToChannel {
        ChannelID channel;
        IndexedDBMessages::Reply reply;
    };
    Vector<ReplyToChannel> take_replies(AgentID);

    // The channels whose renderer claimed what cannot be so. The session has retired each of them by itself. A claim
    // can be found out while the command of another channel is handled, since a command can wait for its turn.
    Vector<ChannelID> take_channels_at_fault();

private:
    struct Agent;
    struct Channel;
    struct Connection;
    struct FactoryRequest;
    struct NamedDatabase;
    struct Transaction;
    struct QueuedRequest;

    void reply(ChannelID, IndexedDBMessages::Reply);

    void retire_for_a_false_claim(Channel&);
    void release_what_it_has(Channel&);
    void withdraw(NamedDatabase&, IndexedDBConnectionQueue::RequestID);
    void forget(Connection&);
    void forget_if_closed(NamedDatabase&, IndexedDBConnectionQueue::ConnectionID);

    Connection* connection_named_by(Channel&, IndexedDBMessages::ConnectionNumber);
    Transaction* transaction_named_by(Channel&, IndexedDBMessages::TransactionName const&);
    Transaction* upgrade_named_by(Channel&, IndexedDBMessages::ConnectionNumber);

    NamedDatabase& named_database(String const& storage_key, Utf16String const& name);
    void add_factory_request(Channel&, Utf16String const& name, IndexedDBConnectionQueue::RequestKind, Optional<u64> version);

    void handle(Channel&, IndexedDBMessages::Open&);
    void handle(Channel&, IndexedDBMessages::DeleteDatabase&);
    void handle(Channel&, IndexedDBMessages::ListDatabases&);
    void handle(Channel&, IndexedDBMessages::CloseConnection&);
    void handle(Channel&, IndexedDBMessages::VersionChangeHandled&);
    void handle(Channel&, IndexedDBMessages::CreateTransaction&);
    void handle(Channel&, IndexedDBMessages::PlaceRequest&);
    void handle(Channel&, IndexedDBMessages::Commit&);
    void handle(Channel&, IndexedDBMessages::Abort&);
    void handle(Channel&, IndexedDBMessages::CreateObjectStore&);
    void handle(Channel&, IndexedDBMessages::RenameObjectStore&);
    void handle(Channel&, IndexedDBMessages::DeleteObjectStore&);
    void handle(Channel&, IndexedDBMessages::CreateIndex&);
    void handle(Channel&, IndexedDBMessages::RenameIndex&);
    void handle(Channel&, IndexedDBMessages::DeleteIndex&);
    void handle(Channel&, IndexedDBMessages::IndexKeys&);

    template<typename Change>
    void place_change(Channel&, Transaction&, Change&);

    void has_work(NamedDatabase&);
    void run_what_can_run();
    void run(NamedDatabase&);

    void perform(NamedDatabase&, IndexedDBTransactionScheduler::Begin const&);
    void perform(NamedDatabase&, IndexedDBTransactionScheduler::Run const&);
    void perform(NamedDatabase&, IndexedDBTransactionScheduler::Commit const&);
    void perform(NamedDatabase&, IndexedDBTransactionScheduler::RollBack const&);

    void perform(NamedDatabase&, IndexedDBConnectionQueue::FindDatabase const&);
    void perform(NamedDatabase&, IndexedDBConnectionQueue::NotifyOfVersionChange const&);
    void perform(NamedDatabase&, IndexedDBConnectionQueue::NotifyOfBlock const&);
    void perform(NamedDatabase&, IndexedDBConnectionQueue::Upgrade const&);
    void perform(NamedDatabase&, IndexedDBConnectionQueue::Delete const&);

    // What became of something that a transaction was asked to do: it is done with, or it waits for its renderer.
    enum class Progress : u8 {
        Done,
        Waiting,
    };

    ErrorOr<void, IndexedDBError> carry_out(Transaction&, QueuedRequest const&);
    ErrorOr<Progress, IndexedDBError> carry_out(Transaction&, u64 place, IndexedDBMessages::CreateIndex const&);
    ErrorOr<Progress, IndexedDBError> continue_the_fill(Transaction&);
    void did_run(Transaction&, u64 place, Optional<IndexedDBError> const& failure);

    void did_fail(NamedDatabase&, ChannelID, IndexedDBError const&);
    void did_finish(Transaction&, IndexedDBConnectionQueue::WasCommitted);
    void did_not_open(NamedDatabase&, IndexedDBConnectionQueue::RequestID, IndexedDBError const&);
    Connection& tell_of_connection(NamedDatabase&, FactoryRequest const&, IndexedDBConnectionQueue::ConnectionID);
    void drop_a_database_that_has_no_version(NamedDatabase&);

    NonnullOwnPtr<IndexedDBStore> m_store;
    Function<UnixDateTime()> m_clock;

    HashMap<AgentID, NonnullOwnPtr<Agent>> m_agents;
    HashMap<ChannelID, NonnullOwnPtr<Channel>> m_channels;
    u64 m_last_agent_id { 0 };
    u64 m_last_channel_id { 0 };
    Vector<ChannelID> m_channels_at_fault;

    // By storage key and then by name. One is here while it has a request or a connection.
    HashMap<String, HashMap<Utf16String, NonnullOwnPtr<NamedDatabase>>> m_databases;
    Vector<NamedDatabase*> m_databases_with_work;
};

}
