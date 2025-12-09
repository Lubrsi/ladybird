/*
* Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/Vector.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/HeapBlock.h>
#include <sys/mman.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#    include <sanitizer/lsan_interface.h>
#endif

namespace GC {

BlockAllocator::BlockAllocator()
{
    m_block_heap = mi_heap_new();
    VERIFY(m_block_heap);
}

BlockAllocator::~BlockAllocator()
{
    mi_heap_destroy(m_block_heap);
}

void* BlockAllocator::allocate_block([[maybe_unused]] char const* name)
{
    auto* block = mi_heap_zalloc_aligned(m_block_heap, HeapBlock::block_size, PAGE_SIZE);
    VERIFY(block);
    LSAN_REGISTER_ROOT_REGION(block, HeapBlock::block_size);
    return block;
}

void BlockAllocator::deallocate_block(void* block)
{
    VERIFY(block);
    ASAN_POISON_MEMORY_REGION(block, HeapBlock::block_size);
    LSAN_UNREGISTER_ROOT_REGION(block, HeapBlock::block_size);
    mi_free_size_aligned(block, HeapBlock::block_size, PAGE_SIZE);
}

}
