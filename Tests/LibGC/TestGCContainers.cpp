/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/ConservativeHashMap.h>
#include <LibGC/ConservativeHashTable.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapHashMap.h>
#include <LibGC/HeapHashTable.h>
#include <LibGC/HeapVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibTest/TestCase.h>

class TestCell : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

    // Padding to satisfy minimum cell size (must be >= sizeof(FreelistEntry)).
    u8 m_padding[16] {};
};

GC_DEFINE_ALLOCATOR(TestCell);

static GC::Heap& test_heap()
{
    static GC::Heap heap([](auto&) { });
    return heap;
}

class TestVisitor : public GC::Cell::Visitor {
    virtual void visit_impl(GC::Cell& cell) override { visited_cells.set(&cell); }
    virtual void visit_impl(ReadonlySpan<GC::NanBoxedValue>) override { }
    virtual void visit_possible_values(ReadonlyBytes) override { }

public:
    HashTable<GC::Cell*> visited_cells;
};

static bool possible_values_contain(GC::ConservativeVectorBase const& container, GC::Cell* cell)
{
    auto target = bit_cast<FlatPtr>(cell);
    for (auto value : container.possible_values()) {
        if (value == target)
            return true;
    }
    return false;
}

static bool possible_values_contain(GC::ConservativeHashTableBase const& container, GC::Cell* cell)
{
    bool found = false;
    auto target = bit_cast<FlatPtr>(cell);
    container.for_each_possible_value([&](FlatPtr value) {
        if (value == target)
            found = true;
    });
    return found;
}

static bool possible_values_contain(GC::ConservativeHashMapBase const& container, GC::Cell* cell)
{
    bool found = false;
    auto target = bit_cast<FlatPtr>(cell);
    container.for_each_possible_value([&](FlatPtr value) {
        if (value == target)
            found = true;
    });
    return found;
}

TEST_CASE(root_vector_reports_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ref<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_vector_ptr_reports_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ptr<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_hash_table_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashTable<GC::Ref<TestCell>> table(heap);

    auto cell = heap.allocate<TestCell>();
    table.set(cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    table.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_hash_map_value_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashMap<int, GC::Ref<TestCell>> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_hash_map_key_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashMap<GC::Ref<TestCell>, int> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(cell, 42);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_hash_map_key_and_value_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashMap<GC::Ref<TestCell>, GC::Ref<TestCell>> map(heap);

    auto key_cell = heap.allocate<TestCell>();
    auto value_cell = heap.allocate<TestCell>();
    map.set(key_cell, value_cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    EXPECT(roots.contains(key_cell.ptr()));
    EXPECT(roots.contains(value_cell.ptr()));
    EXPECT_EQ(roots.size(), 2u);
}

TEST_CASE(root_hash_map_non_gc_key_skipped)
{
    auto& heap = test_heap();
    GC::RootHashMap<int, GC::Ref<TestCell>> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    // Only the value should be reported, not the int key
    EXPECT_EQ(roots.size(), 1u);
    EXPECT(roots.contains(cell.ptr()));
}

TEST_CASE(empty_containers_report_no_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ref<TestCell>> vector(heap);
    GC::RootHashTable<GC::Ref<TestCell>> table(heap);
    GC::RootHashMap<int, GC::Ref<TestCell>> map(heap);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);
    table.gather_roots(roots);
    map.gather_roots(roots);

    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(cleared_container_reports_no_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ref<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);
    vector.clear();

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(conservative_vector_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeVector<GC::Ref<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);

    EXPECT(possible_values_contain(vector, cell.ptr()));
}

TEST_CASE(conservative_hash_table_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashTable<GC::Ref<TestCell>> table(heap);

    auto cell = heap.allocate<TestCell>();
    table.set(cell);

    EXPECT(possible_values_contain(table, cell.ptr()));
}

TEST_CASE(conservative_hash_map_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<int, GC::Ref<TestCell>> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);

    EXPECT(possible_values_contain(map, cell.ptr()));
}

TEST_CASE(cleared_conservative_hash_table_reports_no_stale_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashTable<GC::Ref<TestCell>> table(heap);

    auto cell = heap.allocate<TestCell>();
    table.set(cell);
    table.clear();

    EXPECT(!possible_values_contain(table, cell.ptr()));
}

TEST_CASE(removed_conservative_hash_table_entry_not_reported)
{
    auto& heap = test_heap();
    GC::ConservativeHashTable<GC::Ref<TestCell>> table(heap);

    auto cell = heap.allocate<TestCell>();
    table.set(cell);
    table.remove(cell);

    EXPECT(!possible_values_contain(table, cell.ptr()));
}

TEST_CASE(cleared_conservative_hash_map_reports_no_stale_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<int, GC::Ref<TestCell>> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);
    map.clear();

    EXPECT(!possible_values_contain(map, cell.ptr()));
}

TEST_CASE(removed_conservative_hash_map_entry_not_reported)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<int, GC::Ref<TestCell>> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);
    map.remove(42);

    EXPECT(!possible_values_contain(map, cell.ptr()));
}

TEST_CASE(conservative_hash_map_key_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<GC::Ref<TestCell>, int> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(cell, 42);

    EXPECT(possible_values_contain(map, cell.ptr()));
}

TEST_CASE(removed_conservative_hash_map_key_not_reported)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<GC::Ref<TestCell>, int> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(cell, 42);
    map.remove(cell);

    EXPECT(!possible_values_contain(map, cell.ptr()));
}

TEST_CASE(heap_vector_visit_edges_reports_cells)
{
    auto& heap = test_heap();
    auto vector = heap.allocate<GC::HeapVector<GC::Ref<TestCell>>>();

    auto cell = heap.allocate<TestCell>();
    vector->elements().append(cell);

    TestVisitor visitor;
    vector->visit_edges(visitor);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(empty_heap_vector_visit_edges_reports_nothing)
{
    auto& heap = test_heap();
    auto vector = heap.allocate<GC::HeapVector<GC::Ref<TestCell>>>();

    TestVisitor visitor;
    vector->visit_edges(visitor);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

TEST_CASE(heap_hash_table_visit_edges_reports_cells)
{
    auto& heap = test_heap();
    auto table = heap.allocate<GC::HeapHashTable<GC::Ref<TestCell>>>();

    auto cell = heap.allocate<TestCell>();
    table->table().set(cell);

    TestVisitor visitor;
    table->visit_edges(visitor);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(empty_heap_hash_table_visit_edges_reports_nothing)
{
    auto& heap = test_heap();
    auto table = heap.allocate<GC::HeapHashTable<GC::Ref<TestCell>>>();

    TestVisitor visitor;
    table->visit_edges(visitor);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

TEST_CASE(heap_hash_map_visit_edges_reports_value)
{
    auto& heap = test_heap();
    auto map = heap.allocate<GC::HeapHashMap<int, GC::Ref<TestCell>>>();

    auto cell = heap.allocate<TestCell>();
    map->map().set(42, cell);

    TestVisitor visitor;
    map->visit_edges(visitor);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(heap_hash_map_visit_edges_reports_key)
{
    auto& heap = test_heap();
    auto map = heap.allocate<GC::HeapHashMap<GC::Ref<TestCell>, int>>();

    auto cell = heap.allocate<TestCell>();
    map->map().set(cell, 42);

    TestVisitor visitor;
    map->visit_edges(visitor);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(heap_hash_map_visit_edges_reports_key_and_value)
{
    auto& heap = test_heap();
    auto map = heap.allocate<GC::HeapHashMap<GC::Ref<TestCell>, GC::Ref<TestCell>>>();

    auto key_cell = heap.allocate<TestCell>();
    auto value_cell = heap.allocate<TestCell>();
    map->map().set(key_cell, value_cell);

    TestVisitor visitor;
    map->visit_edges(visitor);

    EXPECT(visitor.visited_cells.contains(key_cell.ptr()));
    EXPECT(visitor.visited_cells.contains(value_cell.ptr()));
}

TEST_CASE(empty_heap_hash_map_visit_edges_reports_nothing)
{
    auto& heap = test_heap();
    auto map = heap.allocate<GC::HeapHashMap<int, GC::Ref<TestCell>>>();

    TestVisitor visitor;
    map->visit_edges(visitor);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

// Helpers for testing the GC::adopt_* functions. Each wraps a plain container
// in a Cell-derived class and uses adopt in a direct member assignment, which
// is one of the contexts the LibJSGCPluginAction VisitCallExpr check permits.
// The test cases below construct one of these helpers, populate a temporary
// Root/Conservative source container, hand it off via `replace`, and verify
// the destination has the contents while the source is left empty.

class TestRootVectorAdopter : public GC::Cell {
    GC_CELL(TestRootVectorAdopter, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestRootVectorAdopter);

public:
    void replace(GC::RootVector<GC::Ref<TestCell>>&& source)
    {
        m_cells = GC::adopt_root_vector(move(source));
    }

    Vector<GC::Ref<TestCell>> const& cells() const { return m_cells; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    Vector<GC::Ref<TestCell>> m_cells;
};
GC_DEFINE_ALLOCATOR(TestRootVectorAdopter);

class TestConservativeVectorAdopter : public GC::Cell {
    GC_CELL(TestConservativeVectorAdopter, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestConservativeVectorAdopter);

public:
    void replace(GC::ConservativeVector<GC::Ref<TestCell>>&& source)
    {
        m_cells = GC::adopt_conservative_vector(move(source));
    }

    Vector<GC::Ref<TestCell>> const& cells() const { return m_cells; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    Vector<GC::Ref<TestCell>> m_cells;
};
GC_DEFINE_ALLOCATOR(TestConservativeVectorAdopter);

class TestRootHashMapAdopter : public GC::Cell {
    GC_CELL(TestRootHashMapAdopter, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestRootHashMapAdopter);

public:
    void replace(GC::RootHashMap<int, GC::Ref<TestCell>>&& source)
    {
        m_cells = GC::adopt_root_hash_map(move(source));
    }

    HashMap<int, GC::Ref<TestCell>> const& cells() const { return m_cells; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    HashMap<int, GC::Ref<TestCell>> m_cells;
};
GC_DEFINE_ALLOCATOR(TestRootHashMapAdopter);

class TestConservativeHashMapAdopter : public GC::Cell {
    GC_CELL(TestConservativeHashMapAdopter, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestConservativeHashMapAdopter);

public:
    void replace(GC::ConservativeHashMap<int, GC::Ref<TestCell>>&& source)
    {
        m_cells = GC::adopt_conservative_hash_map(move(source));
    }

    HashMap<int, GC::Ref<TestCell>> const& cells() const { return m_cells; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    HashMap<int, GC::Ref<TestCell>> m_cells;
};
GC_DEFINE_ALLOCATOR(TestConservativeHashMapAdopter);

class TestRootHashTableAdopter : public GC::Cell {
    GC_CELL(TestRootHashTableAdopter, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestRootHashTableAdopter);

public:
    void replace(GC::RootHashTable<GC::Ref<TestCell>>&& source)
    {
        m_cells = GC::adopt_root_hash_table(move(source));
    }

    HashTable<GC::Ref<TestCell>> const& cells() const { return m_cells; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    HashTable<GC::Ref<TestCell>> m_cells;
};
GC_DEFINE_ALLOCATOR(TestRootHashTableAdopter);

class TestConservativeHashTableAdopter : public GC::Cell {
    GC_CELL(TestConservativeHashTableAdopter, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestConservativeHashTableAdopter);

public:
    void replace(GC::ConservativeHashTable<GC::Ref<TestCell>>&& source)
    {
        m_cells = GC::adopt_conservative_hash_table(move(source));
    }

    HashTable<GC::Ref<TestCell>> const& cells() const { return m_cells; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    HashTable<GC::Ref<TestCell>> m_cells;
};
GC_DEFINE_ALLOCATOR(TestConservativeHashTableAdopter);

TEST_CASE(adopt_root_vector_moves_storage)
{
    auto& heap = test_heap();
    auto adopter = heap.allocate<TestRootVectorAdopter>();

    GC::RootVector<GC::Ref<TestCell>> source(heap);
    auto cell = heap.allocate<TestCell>();
    source.append(cell);
    EXPECT_EQ(source.size(), 1u);

    adopter->replace(move(source));

    // Destination has the contents.
    EXPECT_EQ(adopter->cells().size(), 1u);
    EXPECT_EQ(adopter->cells()[0].ptr(), cell.ptr());

    // Source is empty (its underlying storage was moved out).
    EXPECT_EQ(source.size(), 0u);

    // The source's intrusive heap-list node is still alive until its
    // destructor runs at end of scope, but its now-empty Vector means
    // gather_roots reports nothing — the heap won't see stale entries.
    HashMap<GC::Cell*, GC::HeapRoot> roots;
    source.gather_roots(roots);
    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(adopt_conservative_vector_moves_storage)
{
    auto& heap = test_heap();
    auto adopter = heap.allocate<TestConservativeVectorAdopter>();

    GC::ConservativeVector<GC::Ref<TestCell>> source(heap);
    auto cell = heap.allocate<TestCell>();
    source.append(cell);
    EXPECT_EQ(source.size(), 1u);

    adopter->replace(move(source));

    EXPECT_EQ(adopter->cells().size(), 1u);
    EXPECT_EQ(adopter->cells()[0].ptr(), cell.ptr());

    // Source's underlying storage is empty, so possible_values reports nothing.
    EXPECT_EQ(source.size(), 0u);
    EXPECT(!possible_values_contain(source, cell.ptr()));
}

TEST_CASE(adopt_root_hash_map_moves_storage)
{
    auto& heap = test_heap();
    auto adopter = heap.allocate<TestRootHashMapAdopter>();

    GC::RootHashMap<int, GC::Ref<TestCell>> source(heap);
    auto cell = heap.allocate<TestCell>();
    source.set(42, cell);
    EXPECT_EQ(source.size(), 1u);

    adopter->replace(move(source));

    EXPECT_EQ(adopter->cells().size(), 1u);
    auto entry = adopter->cells().get(42);
    EXPECT(entry.has_value());
    EXPECT_EQ(entry->ptr(), cell.ptr());

    EXPECT_EQ(source.size(), 0u);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    source.gather_roots(roots);
    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(adopt_conservative_hash_map_moves_storage)
{
    auto& heap = test_heap();
    auto adopter = heap.allocate<TestConservativeHashMapAdopter>();

    GC::ConservativeHashMap<int, GC::Ref<TestCell>> source(heap);
    auto cell = heap.allocate<TestCell>();
    source.set(42, cell);
    EXPECT_EQ(source.size(), 1u);

    adopter->replace(move(source));

    EXPECT_EQ(adopter->cells().size(), 1u);
    auto entry = adopter->cells().get(42);
    EXPECT(entry.has_value());
    EXPECT_EQ(entry->ptr(), cell.ptr());

    EXPECT_EQ(source.size(), 0u);
    EXPECT(!possible_values_contain(source, cell.ptr()));
}

TEST_CASE(adopt_root_hash_table_moves_storage)
{
    auto& heap = test_heap();
    auto adopter = heap.allocate<TestRootHashTableAdopter>();

    GC::RootHashTable<GC::Ref<TestCell>> source(heap);
    auto cell = heap.allocate<TestCell>();
    source.set(cell);
    EXPECT_EQ(source.size(), 1u);

    adopter->replace(move(source));

    EXPECT_EQ(adopter->cells().size(), 1u);
    EXPECT(adopter->cells().contains(cell));

    EXPECT_EQ(source.size(), 0u);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    source.gather_roots(roots);
    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(adopt_conservative_hash_table_moves_storage)
{
    auto& heap = test_heap();
    auto adopter = heap.allocate<TestConservativeHashTableAdopter>();

    GC::ConservativeHashTable<GC::Ref<TestCell>> source(heap);
    auto cell = heap.allocate<TestCell>();
    source.set(cell);
    EXPECT_EQ(source.size(), 1u);

    adopter->replace(move(source));

    EXPECT_EQ(adopter->cells().size(), 1u);
    EXPECT(adopter->cells().contains(cell));

    EXPECT_EQ(source.size(), 0u);
    EXPECT(!possible_values_contain(source, cell.ptr()));
}
