/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CircularQueue.h>
#include <AK/IntrusiveList.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

namespace GC {

class GC_API HeapBlock : public HeapBlockBase {
    AK_MAKE_NONCOPYABLE(HeapBlock);
    AK_MAKE_NONMOVABLE(HeapBlock);

public:
    using HeapBlockBase::BLOCK_SIZE;
    static NonnullOwnPtr<HeapBlock> create_with_cell_size(Heap&, CellAllocator&, size_t cell_size, bool overrides_must_survive_garbage_collection, bool overrides_finalize);

    size_t cell_size() const { return m_cell_size; }
    size_t cell_count() const { return (HeapBlock::BLOCK_SIZE - sizeof(HeapBlock)) / m_cell_size; }
    bool is_full() const { return !has_lazy_freelist() && !m_freelist; }
    bool has_quarantined_cells() const { return !m_quarantine.is_empty(); }

    ALWAYS_INLINE Cell* allocate()
    {
        Cell* allocated_cell = nullptr;
        if (m_freelist) {
            VERIFY(is_valid_freelist_entry(m_freelist));
            auto* next = decode_freelist_next(m_freelist, m_freelist->next);
            VERIFY(!next || is_valid_freelist_entry(next));
            allocated_cell = exchange(m_freelist, next);
        } else if (has_lazy_freelist()) {
            allocated_cell = cell(m_next_lazy_freelist_index++);
        }

        if (allocated_cell) {
            ASAN_UNPOISON_MEMORY_REGION(allocated_cell, m_cell_size);
        }
        return allocated_cell;
    }

    // Pressure-escape path used by CellAllocator only when it would otherwise grow the heap.
    // Quarantine intentionally does not contribute to `is_full()`, so this is the only way a
    // quarantined cell can be reused before FIFO eviction pushes it onto the freelist.
    Cell* drain_one_quarantined()
    {
        if (m_quarantine.is_empty())
            return nullptr;
        Cell* cell = m_quarantine.dequeue();
        ASAN_UNPOISON_MEMORY_REGION(cell, m_cell_size);
        return cell;
    }

    void deallocate(Cell*);

    template<typename Callback>
    void for_each_cell(Callback callback)
    {
        auto end = has_lazy_freelist() ? m_next_lazy_freelist_index : cell_count();
        for (size_t i = 0; i < end; ++i)
            callback(cell(i));
    }

    template<Cell::State state, typename Callback>
    void for_each_cell_in_state(Callback callback)
    {
        for_each_cell([&](auto* cell) {
            if (cell->state() == state)
                callback(cell);
        });
    }

    static HeapBlock* from_cell(Cell const* cell)
    {
        return static_cast<HeapBlock*>(HeapBlockBase::from_cell(cell));
    }

    Cell* cell_from_possible_pointer(FlatPtr pointer)
    {
        if (pointer < reinterpret_cast<FlatPtr>(m_storage))
            return nullptr;
        size_t cell_index = (pointer - reinterpret_cast<FlatPtr>(m_storage)) / m_cell_size;
        auto end = has_lazy_freelist() ? m_next_lazy_freelist_index : cell_count();
        if (cell_index >= end)
            return nullptr;
        return cell(cell_index);
    }

    bool is_valid_cell_pointer(Cell const* cell)
    {
        return cell_from_possible_pointer((FlatPtr)cell);
    }

    IntrusiveListNode<HeapBlock> m_list_node;

    CellAllocator& cell_allocator() { return m_cell_allocator; }

    bool overrides_must_survive_garbage_collection() const { return m_overrides_must_survive_garbage_collection; }
    bool overrides_finalize() const { return m_overrides_finalize; }

private:
    HeapBlock(Heap&, CellAllocator&, size_t cell_size, bool overrides_must_survive_garbage_collection, bool overrides_finalize);

    bool has_lazy_freelist() const { return m_next_lazy_freelist_index < cell_count(); }

    struct FreelistEntry final : public Cell {
        GC_CELL(FreelistEntry, Cell);

        RawPtr<FreelistEntry> next;
    };

    // Each freed cell's `next` pointer is XOR-encoded with a per-block secret and the entry's
    // own address, so a partial overwrite of a freed cell cannot redirect future allocations.
    ALWAYS_INLINE FreelistEntry* encode_freelist_next(FreelistEntry const* entry, FreelistEntry const* next) const
    {
        return reinterpret_cast<FreelistEntry*>(reinterpret_cast<FlatPtr>(next) ^ m_freelist_secret ^ reinterpret_cast<FlatPtr>(entry));
    }
    ALWAYS_INLINE FreelistEntry* decode_freelist_next(FreelistEntry const* entry, FreelistEntry const* encoded_next) const
    {
        return encode_freelist_next(entry, encoded_next);
    }

    void push_to_freelist(FreelistEntry* entry)
    {
        entry->next = encode_freelist_next(entry, m_freelist);
        m_freelist = entry;
    }

    void quarantine(FreelistEntry* entry)
    {
        if (m_quarantine.size() == QUARANTINE_SIZE)
            push_to_freelist(m_quarantine.dequeue());
        m_quarantine.enqueue(entry);
    }

    // Stricter than `is_valid_cell_pointer`: requires cell-start alignment and Dead state, so a
    // decoded freelist link can't pass validation by pointing into the middle of a cell or at a
    // live cell.
    bool is_valid_freelist_entry(FreelistEntry const* entry) const
    {
        auto entry_addr = reinterpret_cast<FlatPtr>(entry);
        auto storage_addr = reinterpret_cast<FlatPtr>(m_storage);
        if (entry_addr < storage_addr)
            return false;
        auto offset = entry_addr - storage_addr;
        if (offset % m_cell_size != 0)
            return false;
        size_t cell_index = offset / m_cell_size;
        auto end = has_lazy_freelist() ? m_next_lazy_freelist_index : cell_count();
        if (cell_index >= end)
            return false;
        return entry->state() == Cell::State::Dead;
    }

    Cell* cell(size_t index)
    {
        return reinterpret_cast<Cell*>(&m_storage[index * cell_size()]);
    }

    CellAllocator& m_cell_allocator;
    u32 m_cell_size { 0 };
    u32 m_next_lazy_freelist_index { 0 };

    bool m_overrides_must_survive_garbage_collection { false };
    bool m_overrides_finalize { false };

    Ptr<FreelistEntry> m_freelist;
    FlatPtr m_freelist_secret { 0 };

    // Recently freed cells are held here in a FIFO queue before joining the real freelist.
    // Defeats the "free + immediately reallocate" pattern UAF exploits rely on. Quarantine
    // does not make a block usable for normal allocation; a quarantined cell only becomes
    // reusable when FIFO eviction pushes it onto the freelist or when CellAllocator would
    // otherwise grow the heap (see `drain_one_quarantined`).
    static constexpr size_t QUARANTINE_SIZE = 16;
    CircularQueue<Ptr<FreelistEntry>, QUARANTINE_SIZE> m_quarantine;

    alignas(__BIGGEST_ALIGNMENT__) u8 m_storage[];

public:
    static constexpr size_t min_possible_cell_size = sizeof(FreelistEntry);
};

}
