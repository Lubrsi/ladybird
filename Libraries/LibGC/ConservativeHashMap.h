/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/IntrusiveList.h>
#include <LibGC/Forward.h>

namespace GC {

class GC_API ConservativeHashMapBase {
    AK_MAKE_NONCOPYABLE(ConservativeHashMapBase);

public:
    virtual void for_each_possible_value(AK::Function<void(FlatPtr)> callback) const = 0;

protected:
    explicit ConservativeHashMapBase(Heap&);
    ~ConservativeHashMapBase();

    Heap* m_heap { nullptr };
    IntrusiveListNode<ConservativeHashMapBase> m_list_node;

public:
    using List = IntrusiveList<&ConservativeHashMapBase::m_list_node>;
};

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>, bool IsOrdered = false>
class GC_API ConservativeHashMap final
    : public ConservativeHashMapBase
    , public HashMap<K, V, KeyTraits, ValueTraits, IsOrdered> {

    using HashMapBase = HashMap<K, V, KeyTraits, ValueTraits, IsOrdered>;

public:
    explicit ConservativeHashMap(Heap& heap)
        : ConservativeHashMapBase(heap)
    {
    }

    ConservativeHashMap(ConservativeHashMap const& other)
        : ConservativeHashMapBase(*other.m_heap)
        , HashMapBase(static_cast<HashMapBase const&>(other))
    {
    }

    ConservativeHashMap(ConservativeHashMap&& other)
        : ConservativeHashMapBase(*other.m_heap)
        , HashMapBase(move(static_cast<HashMapBase&>(other)))
    {
    }

    ConservativeHashMap& operator=(ConservativeHashMap const& other)
    {
        if (&other == this)
            return *this;
        HashMapBase::operator=(static_cast<HashMapBase const&>(other));
        return *this;
    }

    ~ConservativeHashMap() = default;

    virtual void for_each_possible_value(AK::Function<void(FlatPtr)> callback) const override
    {
        // Only scan live entries. Dead/free buckets may contain stale pointers
        // that would incorrectly keep GC cells alive.
        auto scan_bytes = [&](ReadonlyBytes bytes) {
            for (size_t i = 0; i + sizeof(FlatPtr) <= bytes.size(); i += sizeof(FlatPtr)) {
                FlatPtr value;
                memcpy(&value, bytes.offset(i), sizeof(FlatPtr));
                callback(value);
            }
        };
        for (auto& [key, value] : *this) {
            scan_bytes({ &key, sizeof(K) });
            scan_bytes({ &value, sizeof(V) });
        }
    }
};

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>>
using OrderedConservativeHashMap = ConservativeHashMap<K, V, KeyTraits, ValueTraits, true>;

// Move the underlying storage of `conservative_hash_map` into a plain HashMap
// that can be stored as a directly-traced field of a GC::Cell-derived class.
// The returned HashMap is no longer registered with the heap; the destination
// is expected to be traced via visit_edges().
//
// Only callable from the member-initializer list of a GC::Cell-derived class's
// constructor (`m_field(GC::adopt_conservative_hash_map(move(chm)))`) or a
// direct assignment to a traced member field
// (`m_field = GC::adopt_conservative_hash_map(move(chm));`). Enforced at
// compile time by LibJSGCPluginAction's VisitCallExpr check — see
// Tests/ClangPlugins/LibJSGCTests/adopt_container_restricted.cpp for the full
// set of allowed and rejected contexts.
template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>, bool IsOrdered = false>
HashMap<K, V, KeyTraits, ValueTraits, IsOrdered> adopt_conservative_hash_map(ConservativeHashMap<K, V, KeyTraits, ValueTraits, IsOrdered>&& conservative_hash_map)
{
    return move(static_cast<HashMap<K, V, KeyTraits, ValueTraits, IsOrdered>&>(conservative_hash_map));
}

}
