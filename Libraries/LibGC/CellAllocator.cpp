/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/Random.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>

namespace GC {

CellAllocator::CellAllocator(size_t cell_size, Optional<StringView> class_name, bool overrides_must_survive_garbage_collection, bool overrides_finalize)
    : m_class_name(class_name)
    , m_cell_size(cell_size)
    , m_overrides_must_survive_garbage_collection(overrides_must_survive_garbage_collection)
    , m_overrides_finalize(overrides_finalize)
{
}

Cell* CellAllocator::allocate_cell(Heap& heap)
{
    if (!m_list_node.is_in_list())
        heap.register_cell_allocator({}, *this);

    if (m_usable_blocks.is_empty()) {
        // Avoid permanently stranding quarantined cells when the alternative is growing the
        // heap. Reservoir-sample a random eligible full block so list order can't be groomed
        // by an attacker into force-draining a specific block.
        HeapBlock* drain_target = nullptr;
        size_t eligible_count = 0;
        for (auto& full_block : m_full_blocks) {
            if (!full_block.has_quarantined_cells())
                continue;
            ++eligible_count;
            if (get_random_uniform(eligible_count) == 0)
                drain_target = &full_block;
        }
        if (drain_target) {
            if (auto* cell = drain_target->drain_one_quarantined())
                return cell;
        }

        auto block = HeapBlock::create_with_cell_size(heap, *this, m_cell_size, m_overrides_must_survive_garbage_collection, m_overrides_finalize);
        auto block_ptr = reinterpret_cast<FlatPtr>(block.ptr());
        if (m_min_block_address > block_ptr)
            m_min_block_address = block_ptr;
        if (m_max_block_address < block_ptr)
            m_max_block_address = block_ptr;
        m_usable_blocks.append(*block.leak_ptr());
    }

    auto& block = *m_usable_blocks.last();
    auto* cell = block.allocate();
    VERIFY(cell);
    if (block.is_full())
        m_full_blocks.append(*m_usable_blocks.last());
    return cell;
}

void CellAllocator::block_did_become_empty(Badge<Heap>, HeapBlock& block)
{
    block.m_list_node.remove();
    // NOTE: HeapBlocks are managed by the BlockAllocator, so we don't want to `delete` the block here.
    block.~HeapBlock();
    m_block_allocator.deallocate_block(&block);
}

void CellAllocator::block_did_become_usable(Badge<Heap>, HeapBlock& block)
{
    VERIFY(!block.is_full());
    m_usable_blocks.append(block);
}

}
