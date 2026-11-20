/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The connections of the thread's agent that a realm's end has to close, and its transactions that wait for clean-up,
//! https://w3c.github.io/IndexedDB/#cleanup-indexed-database-transactions

use core::cell::RefCell;

use libjs_rust::api::{EmbedderCell, Gc, GcWeak, MarkedVec, Realm, Vm};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrapped};
use crate::indexed_db::idb_database::IDBDatabase;
use crate::indexed_db::idb_transaction::IDBTransaction;

type WeakConnection = GcWeak<EmbedderCell<Wrapped<IDBDatabase>>>;
type WeakTransaction = GcWeak<EmbedderCell<Wrapped<IDBTransaction>>>;

/// What IndexedDB tracks of the thread's agent: references that do not keep their cells alive, which a collected cell
/// leaves empty.
pub(crate) struct Agent {
    // The connections whose close pending flag is unset. The others are pruned when a realm's connections are taken
    // to be closed, and once the list has grown to twice what was left after the last pruning.
    connections: Vec<WeakConnection>,
    connections_after_pruning: usize,

    // The transactions that were given a cleanup event loop since the last clean-up. Every microtask checkpoint runs
    // the clean-up, which looks at these and at no other transaction.
    transactions_awaiting_cleanup: Vec<WeakTransaction>,
}

thread_local! {
    static AGENT: RefCell<Agent> = const {
        RefCell::new(Agent {
            connections: Vec::new(),
            connections_after_pruning: 0,
            transactions_awaiting_cleanup: Vec::new(),
        })
    };
}

impl Agent {
    const CONNECTIONS_BEFORE_FIRST_PRUNING: usize = 128;

    fn connection_with_close_pending_unset(connection: &WeakConnection) -> Option<Ref<IDBDatabase>> {
        connection.get().filter(|connection| !connection.close_pending())
    }

    pub(crate) fn add_connection(vm: &Vm, connection: Ref<IDBDatabase>) {
        AGENT.with_borrow_mut(|agent| {
            let pruning_threshold = (2 * agent.connections_after_pruning).max(Self::CONNECTIONS_BEFORE_FIRST_PRUNING);
            if agent.connections.len() >= pruning_threshold {
                agent
                    .connections
                    .retain(|connection| Self::connection_with_close_pending_unset(connection).is_some());
                agent.connections_after_pruning = agent.connections.len();
            }

            agent.connections.push(GcWeak::new(vm.heap(), connection));
        });
    }

    /// Takes the connections of the realm whose close pending flag is unset, for the caller to close, and prunes the
    /// list on the way.
    pub(crate) fn take_connections_of_realm_to_close(vm: &Vm, realm: Gc<Realm>) -> MarkedVec<'_, Ref<IDBDatabase>> {
        let connections = MarkedVec::new(vm);
        AGENT.with_borrow_mut(|agent| {
            agent.connections.retain(|connection| {
                let Some(connection) = Self::connection_with_close_pending_unset(connection) else {
                    return false;
                };
                if connection.realm() != realm {
                    return true;
                }

                connections.push(connection);
                false
            });
            agent.connections_after_pruning = agent.connections.len();
        });
        connections
    }

    pub(crate) fn add_transaction_awaiting_cleanup(vm: &Vm, transaction: Ref<IDBTransaction>) {
        AGENT.with_borrow_mut(|agent| {
            agent
                .transactions_awaiting_cleanup
                .push(GcWeak::new(vm.heap(), transaction));
        });
    }

    /// Takes the transactions whose cleanup event loop is the event loop out of those that wait for clean-up, and
    /// drops the ones that have none any more: a transaction that committed or aborted in the task that created it.
    pub(crate) fn take_transactions_awaiting_cleanup<'vm>(
        vm: &'vm Vm,
        event_loop: &CppCell,
    ) -> MarkedVec<'vm, Ref<IDBTransaction>> {
        let transactions = MarkedVec::new(vm);
        AGENT.with_borrow_mut(|agent| {
            agent.transactions_awaiting_cleanup.retain(|transaction| {
                let Some(transaction) = transaction.get() else {
                    return false;
                };
                let Some(cleanup_event_loop) = transaction.cleanup_event_loop() else {
                    return false;
                };
                if cleanup_event_loop != event_loop.as_ptr() {
                    return true;
                }

                transactions.push(transaction);
                false
            });
        });
        transactions
    }

    /// Forgets the connections and transactions of the thread's agent, whose references into its heap have to go before
    /// the heap does.
    pub(crate) fn shut_down() {
        AGENT.with_borrow_mut(|agent| {
            agent.connections.clear();
            agent.connections_after_pruning = 0;
            agent.transactions_awaiting_cleanup.clear();
        });
    }
}
