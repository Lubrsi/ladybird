/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Forward.h>

namespace GC {

class GC_API BlockAllocator {
public:
    BlockAllocator();
    ~BlockAllocator();

    void* allocate_block(char const* name);
    void deallocate_block(void*);

private:
    mi_heap_t* m_block_heap;
};

}
