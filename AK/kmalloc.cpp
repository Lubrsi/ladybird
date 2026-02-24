/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Tracy.h>
#include <AK/kmalloc.h>

#include <cstddef>
#include <cstring>

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
// LeakSanitizer does not reliably trace references stored in mimalloc-managed
// AK containers, so sanitizer builds fall back to the system allocator.
#    define AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED 1
#else
#    include <mimalloc.h>
#endif

static bool allocation_needs_explicit_alignment(size_t alignment)
{
    return alignment > alignof(std::max_align_t);
}

#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED

static void* aligned_alloc_with_system_allocator(size_t size, size_t alignment, bool zeroed)
{
    void* ptr = nullptr;
    auto actual_size = size == 0 ? static_cast<size_t>(1) : size;
    if (auto result = posix_memalign(&ptr, alignment, actual_size); result != 0)
        return nullptr;
    if (zeroed)
        __builtin_memset(ptr, 0, actual_size);
    return ptr;
}

void* ak_kcalloc(size_t count, size_t size)
{
    void* ptr = calloc(count, size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, count * size);
    }
    return ptr;
}

void* ak_kmalloc(size_t size)
{
    void* ptr = malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, size);
    }
    return ptr;
}

void* ak_kmalloc(HeapPartition, size_t size)
{
    return ak_kmalloc(size);
}

void* ak_krealloc(void* ptr, size_t size)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
    void* new_ptr = realloc(ptr, size);
    if (new_ptr) {
        TRACY_ALLOCATED_MEMORY(new_ptr, size);
    }
    return new_ptr;
}

void* ak_krealloc(HeapPartition, void* ptr, size_t size)
{
    return ak_krealloc(ptr, size);
}

size_t ak_kmalloc_good_size(size_t size)
{
    return size;
}

void ak_kfree(void* ptr)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
    free(ptr);
}

void ak_kmalloc_collect()
{
}

extern "C" {
void* ladybird_rust_alloc(size_t size, size_t alignment);
void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment);
void ladybird_rust_dealloc(void* ptr, size_t alignment);
void* ladybird_rust_realloc(void* ptr, size_t old_size, size_t new_size, size_t alignment);
}

extern "C" void* ladybird_rust_alloc(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = aligned_alloc_with_system_allocator(size, alignment, false);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = aligned_alloc_with_system_allocator(size, alignment, true);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = calloc(1, size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void ladybird_rust_dealloc(void* ptr, size_t)
{
    if (ptr) {
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    free(ptr);
}

extern "C" void* ladybird_rust_realloc(void* ptr, size_t old_size, size_t new_size, size_t alignment)
{
    if (!allocation_needs_explicit_alignment(alignment)) {
        if (ptr) {
            TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
        }
        void* new_ptr = realloc(ptr, new_size);
        if (new_ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
        }
        return new_ptr;
    }

    auto* new_ptr = aligned_alloc_with_system_allocator(new_size, alignment, false);
    if (!new_ptr)
        return nullptr;
    if (ptr) {
        __builtin_memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    free(ptr);
    TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
    return new_ptr;
}

#else

void* ak_kcalloc(size_t count, size_t size)
{
    void* ptr = mi_calloc(count, size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, count * size);
    }
    return ptr;
}

void* ak_kmalloc(size_t size)
{
    void* ptr = mi_malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, size);
    }
    return ptr;
}

static thread_local mi_heap_t* s_string_heap = nullptr;

static mi_heap_t* heap_for_partition(HeapPartition partition)
{
    switch (partition) {
    case HeapPartition::General:
        return mi_heap_get_default();
    case HeapPartition::ArrayBuffer:
        static mi_heap_t* array_buffer_heap = mi_heap_new();
        return array_buffer_heap;
    case HeapPartition::JSObjectStorage:
        static mi_heap_t* js_object_storage_heap = mi_heap_new();
        return js_object_storage_heap;
    case HeapPartition::Layout:
        static mi_heap_t* layout_heap = mi_heap_new();
        return layout_heap;
    case HeapPartition::String:
        if (!s_string_heap)
            s_string_heap = mi_heap_new();
        return s_string_heap;
    }
    VERIFY_NOT_REACHED();
}

void* ak_kmalloc(HeapPartition partition, size_t size)
{
    return mi_heap_malloc(heap_for_partition(partition), size);
}

void* ak_krealloc(void* ptr, size_t size)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
    void* new_ptr = mi_realloc(ptr, size);
    if (new_ptr) {
        TRACY_ALLOCATED_MEMORY(new_ptr, size);
    }
    return new_ptr;
}

void* ak_krealloc(HeapPartition partition, void* ptr, size_t size)
{
    return mi_heap_realloc(heap_for_partition(partition), ptr, size);
}

size_t ak_kmalloc_good_size(size_t size)
{
    return mi_good_size(size);
}

void ak_kfree(void* ptr)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
    mi_free(ptr);
}

void ak_kmalloc_collect()
{
    // mi_collect() only visits the calling thread's default heap, so the string heap has to be collected separately.
    // The remaining partitions are shared between threads and are left to their owners.
    if (s_string_heap)
        mi_heap_collect(s_string_heap, true);

    mi_collect(true);
}

extern "C" {
void* ladybird_rust_alloc(size_t size, size_t alignment);
void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment);
void ladybird_rust_dealloc(void* ptr, size_t alignment);
void* ladybird_rust_realloc(void* ptr, size_t old_size, size_t new_size, size_t alignment);
}

extern "C" void* ladybird_rust_alloc(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = mi_malloc_aligned(size, alignment);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = mi_malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = mi_zalloc_aligned(size, alignment);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = mi_zalloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void ladybird_rust_dealloc(void* ptr, size_t)
{
    if (ptr) {
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    mi_free(ptr);
}

extern "C" void* ladybird_rust_realloc(void* ptr, size_t, size_t new_size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        if (ptr) {
            TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
        }
        void* new_ptr = mi_realloc_aligned(ptr, new_size, alignment);
        if (new_ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
        }
        return new_ptr;
    }

    if (ptr) {
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    void* new_ptr = mi_realloc(ptr, new_size);
    if (new_ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
    }
    return new_ptr;
}

#endif

// However deceptively simple these functions look, they must not be inlined.
// Memory allocated in one translation unit has to be deallocatable in another
// translation unit, so these functions must be the same everywhere.
// By making these functions global, this invariant is enforced.

static void record_operator_new_allocation(void* ptr, [[maybe_unused]] size_t size)
{
    if (ptr)
        TRACY_ALLOCATED_MEMORY(ptr, size);
}

static void* allocate_for_operator_new(size_t size)
{
#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED
    auto* ptr = ak_kmalloc(size);
    VERIFY(ptr);
    return ptr;
#else
    auto* ptr = mi_new(size);
    record_operator_new_allocation(ptr, size);
    return ptr;
#endif
}

static void* allocate_for_operator_new_nothrow(size_t size) noexcept
{
#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED
    return ak_kmalloc(size);
#else
    auto* ptr = mi_new_nothrow(size);
    record_operator_new_allocation(ptr, size);
    return ptr;
#endif
}

static void* allocate_aligned_for_operator_new(size_t size, size_t alignment)
{
#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED
    auto* ptr = aligned_alloc_with_system_allocator(size, alignment, false);
    VERIFY(ptr);
#else
    auto* ptr = mi_new_aligned(size, alignment);
#endif
    record_operator_new_allocation(ptr, size);
    return ptr;
}

static void* allocate_aligned_for_operator_new_nothrow(size_t size, size_t alignment) noexcept
{
#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED
    auto* ptr = aligned_alloc_with_system_allocator(size, alignment, false);
#else
    auto* ptr = mi_new_aligned_nothrow(size, alignment);
#endif
    record_operator_new_allocation(ptr, size);
    return ptr;
}

static void deallocate_for_operator_delete(void* ptr)
{
    ak_kfree(ptr);
}

static void deallocate_for_operator_delete(void* ptr, [[maybe_unused]] size_t size)
{
#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED
    ak_kfree(ptr);
#else
    if (ptr)
        TRACY_FREED_MEMORY(ptr);
    mi_free_size(ptr, size);
#endif
}

static void deallocate_aligned_for_operator_delete(void* ptr, [[maybe_unused]] size_t alignment)
{
#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED
    ak_kfree(ptr);
#else
    if (ptr)
        TRACY_FREED_MEMORY(ptr);
    mi_free_aligned(ptr, alignment);
#endif
}

static void deallocate_aligned_for_operator_delete(void* ptr, [[maybe_unused]] size_t size, [[maybe_unused]] size_t alignment)
{
#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED
    ak_kfree(ptr);
#else
    if (ptr)
        TRACY_FREED_MEMORY(ptr);
    mi_free_size_aligned(ptr, size, alignment);
#endif
}

void* operator new(size_t size)
{
    return allocate_for_operator_new(size);
}

void* operator new(size_t size, std::nothrow_t const&) noexcept
{
    return allocate_for_operator_new_nothrow(size);
}

void operator delete(void* ptr) noexcept
{
    deallocate_for_operator_delete(ptr);
}

void operator delete(void* ptr, std::nothrow_t const&) noexcept
{
    deallocate_for_operator_delete(ptr);
}

void operator delete(void* ptr, size_t size) noexcept
{
    deallocate_for_operator_delete(ptr, size);
}

void* operator new[](size_t size)
{
    return allocate_for_operator_new(size);
}

void* operator new[](size_t size, std::nothrow_t const&) noexcept
{
    return allocate_for_operator_new_nothrow(size);
}

void operator delete[](void* ptr) noexcept
{
    deallocate_for_operator_delete(ptr);
}

void operator delete[](void* ptr, std::nothrow_t const&) noexcept
{
    deallocate_for_operator_delete(ptr);
}

void operator delete[](void* ptr, size_t size) noexcept
{
    deallocate_for_operator_delete(ptr, size);
}

void* operator new(size_t size, std::align_val_t alignment)
{
    return allocate_aligned_for_operator_new(size, static_cast<size_t>(alignment));
}

void* operator new(size_t size, std::align_val_t alignment, std::nothrow_t const&) noexcept
{
    return allocate_aligned_for_operator_new_nothrow(size, static_cast<size_t>(alignment));
}

void operator delete(void* ptr, std::align_val_t alignment) noexcept
{
    deallocate_aligned_for_operator_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete(void* ptr, size_t size, std::align_val_t alignment) noexcept
{
    deallocate_aligned_for_operator_delete(ptr, size, static_cast<size_t>(alignment));
}

void operator delete(void* ptr, std::align_val_t alignment, std::nothrow_t const&) noexcept
{
    deallocate_aligned_for_operator_delete(ptr, static_cast<size_t>(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment)
{
    return allocate_aligned_for_operator_new(size, static_cast<size_t>(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment, std::nothrow_t const&) noexcept
{
    return allocate_aligned_for_operator_new_nothrow(size, static_cast<size_t>(alignment));
}

void operator delete[](void* ptr, std::align_val_t alignment) noexcept
{
    deallocate_aligned_for_operator_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete[](void* ptr, size_t size, std::align_val_t alignment) noexcept
{
    deallocate_aligned_for_operator_delete(ptr, size, static_cast<size_t>(alignment));
}

void operator delete[](void* ptr, std::align_val_t alignment, std::nothrow_t const&) noexcept
{
    deallocate_aligned_for_operator_delete(ptr, static_cast<size_t>(alignment));
}
