/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Checked.h>
#include <AK/Platform.h>
#include <new>
#include <stdlib.h>
#include <mimalloc.h>

#define kcalloc mi_calloc
#define kmalloc mi_malloc
#define kmalloc_good_size mi_malloc_good_size

inline void kfree_sized(void* ptr, size_t size)
{
    mi_free_size(ptr, size);
}

using std::nothrow;

inline void* kmalloc_array(AK::Checked<size_t> a, AK::Checked<size_t> b)
{
    auto size = a * b;
    VERIFY(!size.has_overflow());
    return kmalloc(size.value());
}

inline void* kmalloc_array(AK::Checked<size_t> a, AK::Checked<size_t> b, AK::Checked<size_t> c)
{
    auto size = a * b * c;
    VERIFY(!size.has_overflow());
    return kmalloc(size.value());
}
