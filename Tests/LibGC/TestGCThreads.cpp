/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/PrimitiveStorage.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

namespace {

Atomic<size_t> s_live_nodes { 0 };

class Node final : public GC::Cell {
    GC_CELL(Node, GC::Cell);
    GC_DECLARE_ALLOCATOR(Node);

public:
    virtual ~Node() override { s_live_nodes.fetch_sub(1); }

    GC::Ptr<Node> next;

private:
    Node() { s_live_nodes.fetch_add(1); }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(next);
    }
};

GC_DEFINE_ALLOCATOR(Node);

// LibTest's EXPECT macros are not thread-safe, so threads count failures and the test asserts on the total.
struct ChurnResult {
    Atomic<size_t> failures { 0 };
};

void check(ChurnResult& result, bool condition)
{
    if (!condition)
        result.failures.fetch_add(1);
}

NEVER_INLINE void scrub_stack()
{
    u8 volatile filler[8 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
}

NEVER_INLINE GC::Root<Node> allocate_chain(GC::Heap& heap, size_t length)
{
    auto head = GC::make_root(heap.allocate<Node>());
    GC::Ptr<Node> tail = head.ptr();
    for (size_t i = 1; i < length; ++i) {
        tail->next = heap.allocate<Node>();
        tail = tail->next;
    }
    return head;
}

void churn(GC::Heap& heap, ChurnResult& result, size_t rounds)
{
    auto& storage = GC::PrimitiveStorage::the();

    for (size_t round = 0; round < rounds; ++round) {
        auto chain = allocate_chain(heap, 500);
        GC::Weak<Node> weak_head = chain.ptr();
        GC::Weak<Node> weak_garbage = heap.allocate<Node>().ptr();

        auto handle = MUST(storage.try_allocate(4 * KiB));
        auto* bytes = storage.data(handle);
        check(result, bytes != nullptr);
        for (size_t i = 0; i < 4 * KiB; ++i)
            bytes[i] = static_cast<u8>(round + i);
        MUST(storage.try_resize(handle, 64 * KiB));
        check(result, storage.size(handle) == 64 * KiB);
        for (size_t i = 0; i < 4 * KiB; ++i)
            check(result, storage.data(handle)[i] == static_cast<u8>(round + i));

        scrub_stack();
        heap.collect_garbage();

        check(result, weak_head);
        check(result, !weak_garbage);
        check(result, storage.is_valid(handle));
        storage.free(handle);
        check(result, !storage.is_valid(handle));

        chain = {};
        scrub_stack();
        heap.collect_garbage();
        check(result, !weak_head);
    }
}

}

TEST_CASE(heaps_on_several_threads_allocate_and_collect_concurrently)
{
    constexpr size_t thread_count = 4;
    constexpr size_t rounds = 25;

    GC::Heap main_heap([](auto&) { });
    main_heap.set_incremental_sweep_enabled(false);
    EXPECT_EQ(&GC::Heap::the(), &main_heap);

    ChurnResult result;
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < thread_count; ++i) {
        threads.append(Threading::Thread::construct("GCThread"sv, [&result] {
            GC::Heap heap([](auto&) { });
            heap.set_incremental_sweep_enabled(false);
            check(result, &GC::Heap::the() == &heap);
            check(result, heap.owning_thread().is_current_thread());
            churn(heap, result, rounds);
            return 0;
        }));
        threads.last()->start();
    }

    churn(main_heap, result, rounds);

    for (auto& thread : threads)
        MUST(thread->join());

    EXPECT_EQ(result.failures.load(), 0u);
    EXPECT_EQ(&GC::Heap::the(), &main_heap);
    EXPECT_EQ(s_live_nodes.load(), 0u);
}

TEST_CASE(allocating_on_another_threads_heap_is_rejected)
{
    Sync::Mutex mutex;
    Sync::ConditionVariable condition { mutex };
    GC::Heap* foreign_heap = nullptr;
    bool release_thread = false;

    auto thread = Threading::Thread::construct("GCHeapOwner"sv, [&] {
        GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
        Sync::MutexLocker locker(mutex);
        foreign_heap = &heap;
        condition.broadcast();
        while (!release_thread)
            condition.wait();
        return 0;
    });
    thread->start();

    {
        Sync::MutexLocker locker(mutex);
        while (!foreign_heap)
            condition.wait();
    }

    EXPECT(!foreign_heap->owning_thread().is_current_thread());
    EXPECT_DEATH("allocating on a heap owned by another thread", (void)foreign_heap->allocate<Node>());

    {
        Sync::MutexLocker locker(mutex);
        release_thread = true;
        condition.broadcast();
    }
    MUST(thread->join());
}
