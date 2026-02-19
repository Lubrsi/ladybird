/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/SessionHistoryTraversalQueue.h>

namespace WebView {

void SessionHistoryTraversalQueue::append(SessionHistoryCommand command)
{
    m_queue.append(move(command));
}

Optional<SessionHistoryCommand> SessionHistoryTraversalQueue::dequeue()
{
    if (m_queue.is_empty())
        return {};
    return m_queue.take_first();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
Optional<SynchronousNavigationCommand> SessionHistoryTraversalQueue::take_first_synchronous_navigation_not_targeting(HashTable<String> const& excluded_navigable_ids)
{
    auto index = m_queue.find_first_index_if([&](auto const& command) {
        if (auto* sync_command = command.template get_pointer<SynchronousNavigationCommand>())
            return !excluded_navigable_ids.contains(sync_command->target_navigable_id);
        return false;
    });

    if (!index.has_value())
        return {};

    auto command = m_queue.take(*index);
    return command.template get<SynchronousNavigationCommand>();
}

}
