/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ExternalEntityTable.h>

namespace GC {

ExternalEntityTableHandle ExternalEntityTable::allocate_entry(ExternalEntityTableTag tag)
{
    VERIFY(tag.is_balanced());

    u32 index;
    if (m_free_entry_indices.is_empty()) {
        VERIFY(m_entries.size() < ExternalEntityTableHandle::invalid_index);
        index = static_cast<u32>(m_entries.size());
        m_entries.empend();
    } else {
        index = m_free_entry_indices.take_last();
    }

    auto& entry = m_entries[index];
    auto state = entry.state.load(AK::MemoryOrder::memory_order_relaxed);
    VERIFY(!Entry::is_allocated(state));
    auto generation = Entry::generation_of(state);
    entry.state.store(Entry::make_state(generation, tag, true), AK::MemoryOrder::memory_order_relaxed);
    return { index, generation };
}

bool ExternalEntityTable::is_valid(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag) const
{
    return entry_for(handle, expected_tag) != nullptr;
}

void ExternalEntityTable::free_entry(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag)
{
    auto* entry = entry_for(handle, expected_tag);
    if (!entry)
        return;

    auto generation = handle.generation + 1;
    if (generation == 0)
        ++generation;
    entry->state.store(Entry::make_state(generation, {}, false), AK::MemoryOrder::memory_order_relaxed);
    m_free_entry_indices.append(handle.index);
}

ExternalEntityTable::Entry* ExternalEntityTable::entry_for(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag)
{
    if (!handle.is_valid() || handle.index >= m_entries.size())
        return nullptr;
    auto& entry = m_entries[handle.index];
    if (entry.state.load(AK::MemoryOrder::memory_order_relaxed) != Entry::make_state(handle.generation, expected_tag, true))
        return nullptr;
    return &entry;
}

ExternalEntityTable::Entry const* ExternalEntityTable::entry_for(ExternalEntityTableHandle handle, ExternalEntityTableTag expected_tag) const
{
    return const_cast<ExternalEntityTable&>(*this).entry_for(handle, expected_tag);
}

}
