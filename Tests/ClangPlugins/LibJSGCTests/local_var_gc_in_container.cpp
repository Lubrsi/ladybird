/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/ConservativeHashMap.h>
#include <LibGC/ConservativeHashTable.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyKey.h>

// Local variables with heap-allocating containers holding GC pointers should be flagged.
void test_local_vectors(GC::Heap& heap)
{
    // expected-error@+1 {{Variable with type 'Vector<GC::Ref<JS::Object>>' contains pointers to GC-managed objects but is not a GC root}}
    Vector<GC::Ref<JS::Object>> bad_ref_vector;

    // expected-error@+1 {{Variable with type 'Vector<GC::Ptr<JS::Object>>' contains pointers to GC-managed objects but is not a GC root}}
    Vector<GC::Ptr<JS::Object>> bad_ptr_vector;

    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    Vector<JS::Object*> bad_raw_ptr_vector;

    // These are fine - they register themselves with the GC heap
    GC::RootVector<GC::Ref<JS::Object>> good_root_vector(heap);
}

void test_local_hashmaps()
{
    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    HashMap<int, GC::Ptr<JS::Object>> bad_hashmap;

    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    HashMap<int, Vector<GC::Ref<JS::Object>>> bad_nested;
}

void test_local_hashtables()
{
    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    HashTable<JS::Object*> bad_hashtable;

    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    OrderedHashTable<JS::Object*> bad_ordered_hashtable;
}

// GC root and conservative containers register with the heap - they are safe
void test_gc_root_containers_ok(GC::Heap& heap)
{
    GC::RootVector<GC::Ref<JS::Object>> ok_root_vector(heap);
    GC::RootHashTable<GC::Ref<JS::Object>> ok_root_hash_table(heap);
    GC::RootHashMap<int, GC::Ref<JS::Object>> ok_root_hash_map(heap);
    GC::ConservativeVector<JS::PropertyKey> ok_conservative_vector(heap);
    GC::ConservativeHashTable<JS::PropertyKey> ok_conservative_hash_table(heap);
    GC::ConservativeHashMap<int, JS::PropertyKey> ok_conservative_hash_map(heap);
}

// Wrapper types around dangerous containers should also be caught
void test_wrapper_types()
{
    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    Optional<Vector<GC::Ref<JS::Object>>> bad_optional_vector;

    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    Optional<HashMap<int, GC::Ptr<JS::Object>>> bad_optional_hashmap;

    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    Variant<int, Vector<GC::Ref<JS::Object>>> bad_variant { 0 };

    // Non-GC wrapper types are fine
    Optional<Vector<int>> ok_optional_vector_int;
    Optional<int> ok_optional_int;
}

// Named wrapper structs containing dangerous containers should also be caught
struct WrapperWithGCContainer {
    Optional<HashTable<GC::Ptr<JS::Object>>> value;
};

struct SafeWrapper {
    int value;
};

void test_named_wrapper_types()
{
    // expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
    WrapperWithGCContainer bad_wrapper;

    // Non-GC wrapper structs are fine
    SafeWrapper ok_wrapper;
}

// Direct GC types on the stack are fine (conservative stack scanning finds them)
void test_direct_gc_types_ok()
{
    GC::Ptr<JS::Object> ok_ptr;
    (void)ok_ptr;
}

// References to containers are fine - the referent is managed elsewhere
void test_references_ok(Vector<GC::Ref<JS::Object>>& vec_ref)
{
    Vector<GC::Ref<JS::Object>>& local_ref = vec_ref;
    (void)local_ref;
}

// Static variables with GC pointers in containers should also be flagged
// expected-error@+1 {{contains pointers to GC-managed objects but is not a GC root}}
static Vector<GC::Ptr<JS::Object>> s_bad_static_vector;
