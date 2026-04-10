/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify -Xclang -verify-ignore-unexpected=note %plugin_opts% -c %s -o %t 2>&1

// AK::Vector has a deleted constrained constructor and operator= that block
// implicit slicing from any strict descendant of Vector (e.g. GC::RootVector,
// GC::ConservativeVector). The forwarding-reference signature
// `Vector(Derived&&)` captures every value category — lvalue copy, rvalue
// move, prvalue — so all the slicing forms below should fail to compile.
//
// Reference binding (`Vector const&` parameter, `Vector&` parameter) is
// unaffected because it does not go through a constructor call, so the
// negative cases at the bottom of this file must still compile.

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Vector.h>
#include <LibGC/ConservativeHashMap.h>
#include <LibGC/ConservativeHashTable.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Object.h>

// Helpers used by the slicing tests below.
void takes_vector_by_value(Vector<GC::Ref<JS::Object>>);
void takes_vector_by_const_ref(Vector<GC::Ref<JS::Object>> const&);
void takes_vector_by_ref(Vector<GC::Ref<JS::Object>>&);

// Pass-by-value (lvalue) — `Derived` deduces to `RootVector<...>&`, the
// deleted forwarding-reference overload is selected.
void test_pass_by_value_lvalue(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(root_vec);
}

// Pass-by-value (rvalue via move) — `Derived` deduces to `RootVector<...>`,
// the deleted overload still wins because identity is a better match than
// derived-to-base.
void test_pass_by_value_rvalue(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(move(root_vec));
}

// Pass-by-value (prvalue from a function returning RootVector by value).
static GC::RootVector<GC::Ref<JS::Object>> make_root_vec(GC::Heap& heap)
{
    return GC::RootVector<GC::Ref<JS::Object>> { heap };
}

void test_pass_by_value_prvalue(GC::Heap& heap)
{
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(make_root_vec(heap));
}

// Returning a local RootVector from a function with a `Vector` return type
// triggers slicing on the return.
Vector<GC::Ref<JS::Object>> test_return_slicing(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> tmp(heap);
    // expected-error@+1 {{call to deleted constructor}}
    return tmp;
}

// Member-initialiser list slicing — the field is a plain Vector, the
// argument is a RootVector.
struct WidgetWithVectorField {
    Vector<GC::Ref<JS::Object>> m_items;
    explicit WidgetWithVectorField(GC::RootVector<GC::Ref<JS::Object>> const& source)
        // expected-error@+1 {{call to deleted constructor}}
        : m_items(source)
    {
    }
};

// Slicing via the deleted operator= overload. We need an existing plain
// Vector to assign into; we use a parameter rather than a local variable to
// keep this test focused on the operator= path (a local
// `Vector<GC::Ref<JS::Object>>` would also be flagged by the existing
// VisitVarDecl plugin check, which would muddy the test).
void test_assignment_slicing(Vector<GC::Ref<JS::Object>>& dest, GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    // expected-error@+1 {{overload resolution selected deleted operator}}
    dest = root_vec;
}

// ConservativeVector is also a strict descendant of Vector and must be
// blocked the same way.
void test_conservative_vector_slicing(GC::Heap& heap)
{
    GC::ConservativeVector<GC::Ref<JS::Object>> cons_vec(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(cons_vec);
}

// ===== HashMap downgrade tests =====

void takes_hashmap_by_value(HashMap<int, GC::Ref<JS::Object>>);
void takes_hashmap_by_const_ref(HashMap<int, GC::Ref<JS::Object>> const&);

void test_root_hashmap_pass_by_value(GC::Heap& heap)
{
    GC::RootHashMap<int, GC::Ref<JS::Object>> root_map(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_hashmap_by_value(root_map);
}

void test_root_hashmap_assignment_slicing(HashMap<int, GC::Ref<JS::Object>>& dest, GC::Heap& heap)
{
    GC::RootHashMap<int, GC::Ref<JS::Object>> root_map(heap);
    // expected-error@+1 {{overload resolution selected deleted operator}}
    dest = root_map;
}

void test_conservative_hashmap_slicing(GC::Heap& heap)
{
    GC::ConservativeHashMap<int, GC::Ref<JS::Object>> cons_map(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_hashmap_by_value(cons_map);
}

// Reference binding to HashMap const& must still compile.
void test_root_hashmap_const_ref_ok(GC::Heap& heap)
{
    GC::RootHashMap<int, GC::Ref<JS::Object>> root_map(heap);
    takes_hashmap_by_const_ref(root_map);
}

// ===== HashTable downgrade tests =====

void takes_hashtable_by_value(HashTable<GC::Ref<JS::Object>>);
void takes_hashtable_by_const_ref(HashTable<GC::Ref<JS::Object>> const&);

void test_root_hashtable_pass_by_value(GC::Heap& heap)
{
    GC::RootHashTable<GC::Ref<JS::Object>> root_set(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_hashtable_by_value(root_set);
}

void test_root_hashtable_assignment_slicing(HashTable<GC::Ref<JS::Object>>& dest, GC::Heap& heap)
{
    GC::RootHashTable<GC::Ref<JS::Object>> root_set(heap);
    // expected-error@+1 {{overload resolution selected deleted operator}}
    dest = root_set;
}

void test_conservative_hashtable_slicing(GC::Heap& heap)
{
    GC::ConservativeHashTable<GC::Ref<JS::Object>> cons_set(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_hashtable_by_value(cons_set);
}

void test_root_hashtable_const_ref_ok(GC::Heap& heap)
{
    GC::RootHashTable<GC::Ref<JS::Object>> root_set(heap);
    takes_hashtable_by_const_ref(root_set);
}

// Copying RootHashMap to RootHashMap (and the same for RootHashTable) is
// fine — both register with the heap.
void test_root_hashmap_to_root_hashmap_copy_ok(GC::Heap& heap)
{
    GC::RootHashMap<int, GC::Ref<JS::Object>> a(heap);
    GC::RootHashMap<int, GC::Ref<JS::Object>> b = a;
    (void)b;
}

void test_root_hashtable_to_root_hashtable_copy_ok(GC::Heap& heap)
{
    GC::RootHashTable<GC::Ref<JS::Object>> a(heap);
    GC::RootHashTable<GC::Ref<JS::Object>> b = a;
    (void)b;
}

// ===== Negative cases — these MUST still compile =====

// Reference binding to `Vector const&` — no constructor call, no slicing.
void test_const_ref_binding_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    takes_vector_by_const_ref(root_vec);
}

// Reference binding to `Vector&` — also a base-class reference bind.
// Mutating through this reference mutates the underlying RootVector storage,
// which remains rooted, so this is also safe.
void test_mutable_ref_binding_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    takes_vector_by_ref(root_vec);
}

// Copying RootVector to RootVector (lvalue copy ctor) is fine — both
// instances register with the heap and trace their elements.
void test_root_to_root_copy_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> a(heap);
    GC::RootVector<GC::Ref<JS::Object>> b = a;
    (void)b;
}

// Copying RootVector to RootVector (rvalue move ctor) is fine.
void test_root_to_root_move_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> a(heap);
    GC::RootVector<GC::Ref<JS::Object>> b = move(a);
    (void)b;
}
