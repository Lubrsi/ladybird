/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify -Xclang -verify-ignore-unexpected=note %plugin_opts% -c %s -o %t 2>&1

// AK::Vector / HashMap / HashTable reject implicit slicing from any strict
// descendant via a constrained deleted ctor and operator=. Reference binding
// paths remain compilable; see the negative cases at the bottom.

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

void test_pass_by_value_lvalue(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(root_vec);
}

void test_pass_by_value_rvalue(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(move(root_vec));
}

static GC::RootVector<GC::Ref<JS::Object>> make_root_vec(GC::Heap& heap)
{
    return GC::RootVector<GC::Ref<JS::Object>> { heap };
}

void test_pass_by_value_prvalue(GC::Heap& heap)
{
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(make_root_vec(heap));
}

Vector<GC::Ref<JS::Object>> test_return_slicing(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> tmp(heap);
    // expected-error@+1 {{call to deleted constructor}}
    return tmp;
}

struct WidgetWithVectorField {
    Vector<GC::Ref<JS::Object>> m_items;
    explicit WidgetWithVectorField(GC::RootVector<GC::Ref<JS::Object>> const& source)
        // expected-error@+1 {{call to deleted constructor}}
        : m_items(source)
    {
    }
};

// `dest` is a parameter rather than a local to avoid colliding with the
// VisitVarDecl plugin check that would also flag a local Vector<GC::Ref<...>>.
void test_assignment_slicing(Vector<GC::Ref<JS::Object>>& dest, GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    // expected-error@+1 {{overload resolution selected deleted operator}}
    dest = root_vec;
}

void test_conservative_vector_slicing(GC::Heap& heap)
{
    GC::ConservativeVector<GC::Ref<JS::Object>> cons_vec(heap);
    // expected-error@+1 {{call to deleted constructor}}
    takes_vector_by_value(cons_vec);
}

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

void test_root_hashmap_const_ref_ok(GC::Heap& heap)
{
    GC::RootHashMap<int, GC::Ref<JS::Object>> root_map(heap);
    takes_hashmap_by_const_ref(root_map);
}

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

// Negative cases below must still compile.

void test_const_ref_binding_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    takes_vector_by_const_ref(root_vec);
}

void test_mutable_ref_binding_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> root_vec(heap);
    takes_vector_by_ref(root_vec);
}

void test_root_to_root_copy_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> a(heap);
    GC::RootVector<GC::Ref<JS::Object>> b = a;
    (void)b;
}

void test_root_to_root_move_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> a(heap);
    GC::RootVector<GC::Ref<JS::Object>> b = move(a);
    (void)b;
}
