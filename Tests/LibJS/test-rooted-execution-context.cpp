/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibGC/HeapRoot.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/RootedExecutionContext.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>

using namespace JS;

namespace {

struct TestVM {
    TestVM()
        : vm(VM::create())
        , execution_context(MUST(Realm::initialize_host_defined_realm(*vm, nullptr, nullptr)))
    {
    }

    ~TestVM()
    {
        vm->pop_execution_context();
    }

    NonnullRefPtr<VM> vm;
    NonnullOwnPtr<ExecutionContext> execution_context;
};

}

TEST_CASE(rooted_execution_context_registers_inner_cells_with_vm)
{
    IGNORE_GC TestVM test_vm;

    auto* sentinel = PrimitiveString::create(*test_vm.vm, "sentinel"_string).ptr();

    JS::RootedExecutionContext rooted(*test_vm.vm, 0, ReadonlySpan<Value> {}, 0);
    rooted->this_value = Value { sentinel };

    IGNORE_GC HashMap<GC::Cell*, GC::HeapRoot> roots;
    test_vm.vm->gather_roots(roots);

    EXPECT(roots.contains(sentinel));
}

TEST_CASE(rooted_execution_context_destruction_unregisters_from_vm)
{
    IGNORE_GC TestVM test_vm;

    auto* sentinel = PrimitiveString::create(*test_vm.vm, "decaying"_string).ptr();

    {
        JS::RootedExecutionContext rooted(*test_vm.vm, 0, ReadonlySpan<Value> {}, 0);
        rooted->this_value = Value { sentinel };

        IGNORE_GC HashMap<GC::Cell*, GC::HeapRoot> roots_while_alive;
        test_vm.vm->gather_roots(roots_while_alive);
        EXPECT(roots_while_alive.contains(sentinel));
    }

    IGNORE_GC HashMap<GC::Cell*, GC::HeapRoot> roots_after_dtor;
    test_vm.vm->gather_roots(roots_after_dtor);
}

TEST_CASE(rooted_execution_context_release_hands_off_ownership)
{
    IGNORE_GC TestVM test_vm;

    auto* sentinel = PrimitiveString::create(*test_vm.vm, "released"_string).ptr();

    IGNORE_GC NonnullOwnPtr<ExecutionContext> released = [&] {
        JS::RootedExecutionContext rooted(*test_vm.vm, 0, ReadonlySpan<Value> {}, 0);
        rooted->this_value = Value { sentinel };
        return rooted.release();
    }();

    EXPECT(released->this_value.has_value());
    EXPECT_EQ(&released->this_value.value().as_cell(), sentinel);
}

TEST_CASE(rooted_execution_context_copy_ctor_registers_fresh_context)
{
    IGNORE_GC TestVM test_vm;

    auto* sentinel = PrimitiveString::create(*test_vm.vm, "copied"_string).ptr();

    IGNORE_GC auto source = ExecutionContext::create(0, ReadonlySpan<Value> {}, 0);
    source->this_value = Value { sentinel };

    JS::RootedExecutionContext rooted(*test_vm.vm, *source);
    EXPECT(rooted->this_value.has_value());
    EXPECT_EQ(&rooted->this_value.value().as_cell(), sentinel);

    IGNORE_GC HashMap<GC::Cell*, GC::HeapRoot> roots;
    test_vm.vm->gather_roots(roots);
    EXPECT(roots.contains(sentinel));
}
