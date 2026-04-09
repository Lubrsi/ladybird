/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>

namespace GC {

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>, bool IsOrdered = false>
class HeapHashMap : public Cell {
    GC_CELL(HeapHashMap, Cell);

    // NB: GC_DECLARE_ALLOCATOR / GC_DEFINE_ALLOCATOR macros can't handle
    //     multi-parameter templates, so we expand them manually.
    using gc_allocator_marker = HeapHashMap;
    static TypeIsolatingCellAllocator<HeapHashMap> cell_allocator;

public:
    HeapHashMap() = default;
    virtual ~HeapHashMap() override = default;

    auto& map() { return m_map; }
    auto const& map() const { return m_map; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_map);
    }

private:
    HashMap<K, V, KeyTraits, ValueTraits, IsOrdered> m_map;
};

template<typename K, typename V, typename KeyTraits, typename ValueTraits, bool IsOrdered>
TypeIsolatingCellAllocator<HeapHashMap<K, V, KeyTraits, ValueTraits, IsOrdered>>
    HeapHashMap<K, V, KeyTraits, ValueTraits, IsOrdered>::cell_allocator { "HeapHashMap"sv, HeapHashMap::OVERRIDES_MUST_SURVIVE_GARBAGE_COLLECTION, HeapHashMap::OVERRIDES_FINALIZE };

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>>
using OrderedHeapHashMap = HeapHashMap<K, V, KeyTraits, ValueTraits, true>;

}
