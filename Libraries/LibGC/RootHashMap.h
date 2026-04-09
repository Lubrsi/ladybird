/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/IntrusiveList.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>

namespace GC {

class GC_API RootHashMapBase {
public:
    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>&) const = 0;

protected:
    explicit RootHashMapBase(Heap&);
    ~RootHashMapBase();

    void assign_heap(Heap*);

    Heap* m_heap { nullptr };
    IntrusiveListNode<RootHashMapBase> m_list_node;

public:
    using List = IntrusiveList<&RootHashMapBase::m_list_node>;
};

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>, bool IsOrdered = false>
class RootHashMap final
    : public RootHashMapBase
    , public HashMap<K, V, KeyTraits, ValueTraits, IsOrdered> {

    using HashMapBase = HashMap<K, V, KeyTraits, ValueTraits, IsOrdered>;

public:
    explicit RootHashMap(Heap& heap)
        : RootHashMapBase(heap)
    {
    }

    RootHashMap(RootHashMap const& other)
        : RootHashMapBase(*other.m_heap)
        , HashMapBase(other)
    {
    }

    RootHashMap(RootHashMap&& other)
        : RootHashMapBase(*other.m_heap)
        , HashMapBase(move(static_cast<HashMapBase&>(other)))
    {
    }

    RootHashMap& operator=(RootHashMap const& other)
    {
        if (&other == this)
            return *this;

        assign_heap(other.m_heap);
        HashMapBase::operator=(other);
        return *this;
    }

    RootHashMap& operator=(RootHashMap&& other)
    {
        assign_heap(other.m_heap);
        HashMapBase::operator=(move(static_cast<HashMapBase&>(other)));
        return *this;
    }

    ~RootHashMap() = default;

    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>& roots) const override
    {
        static constexpr bool KeyIsGCType = IsBaseOf<NanBoxedValue, K> || IsConvertible<K, Cell const*>;
        static constexpr bool ValueIsGCType = IsBaseOf<NanBoxedValue, V> || IsConvertible<V, Cell const*>;
        static_assert(KeyIsGCType || ValueIsGCType,
            "RootHashMap requires at least one of key or value types to be convertible to Cell const* or derive from NanBoxedValue");
        for (auto& [key, value] : *this) {
            if constexpr (IsBaseOf<NanBoxedValue, K>) {
                if (key.is_cell())
                    roots.set(&const_cast<K&>(key).as_cell(), HeapRoot { .type = HeapRoot::Type::RootHashMap });
            } else if constexpr (IsConvertible<K, Cell const*>) {
                roots.set(const_cast<Cell*>(static_cast<Cell const*>(key)), HeapRoot { .type = HeapRoot::Type::RootHashMap });
            }

            if constexpr (IsBaseOf<NanBoxedValue, V>) {
                if (value.is_cell())
                    roots.set(&const_cast<V&>(value).as_cell(), HeapRoot { .type = HeapRoot::Type::RootHashMap });
            } else if constexpr (IsConvertible<V, Cell const*>) {
                roots.set(const_cast<Cell*>(static_cast<Cell const*>(value)), HeapRoot { .type = HeapRoot::Type::RootHashMap });
            }
        }
    }
};

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>>
using OrderedRootHashMap = RootHashMap<K, V, KeyTraits, ValueTraits, true>;

// Move a RootHashMap's storage into a plain HashMap for direct tracing as a
// GC::Cell-derived field.
template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>, bool IsOrdered = false>
HashMap<K, V, KeyTraits, ValueTraits, IsOrdered> adopt_root_hash_map(RootHashMap<K, V, KeyTraits, ValueTraits, IsOrdered>&& root_hash_map)
{
    return move(static_cast<HashMap<K, V, KeyTraits, ValueTraits, IsOrdered>&>(root_hash_map));
}

}
