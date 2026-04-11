/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/IntrusiveList.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>

namespace GC {

class GC_API RootVectorBase {
public:
    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>&) const = 0;

protected:
    explicit RootVectorBase(Heap&);
    ~RootVectorBase();

    void assign_heap(Heap*);

    Heap* m_heap { nullptr };
    IntrusiveListNode<RootVectorBase> m_list_node;

public:
    using List = IntrusiveList<&RootVectorBase::m_list_node>;
};

template<typename T, size_t inline_capacity>
class RootVector final
    : public RootVectorBase
    , public Vector<T, inline_capacity> {

    using VectorBase = Vector<T, inline_capacity>;

public:
    explicit RootVector(Heap& heap)
        : RootVectorBase(heap)
    {
    }

    ~RootVector() = default;

    RootVector(Heap& heap, ReadonlySpan<T> other)
        : RootVectorBase(heap)
        , Vector<T, inline_capacity>(other)
    {
    }

    RootVector(RootVector const& other)
        : RootVectorBase(*other.m_heap)
        , Vector<T, inline_capacity>(static_cast<VectorBase const&>(other))
    {
    }

    RootVector(RootVector&& other)
        : RootVectorBase(*other.m_heap)
        , VectorBase(move(static_cast<VectorBase&>(other)))
    {
    }

    RootVector& operator=(RootVector const& other)
    {
        if (&other == this)
            return *this;

        assign_heap(other.m_heap);
        VectorBase::operator=(static_cast<VectorBase const&>(other));
        return *this;
    }

    RootVector& operator=(RootVector&& other)
    {
        assign_heap(other.m_heap);
        VectorBase::operator=(move(static_cast<VectorBase&>(other)));
        return *this;
    }

    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>& roots) const override
    {
        static_assert(IsBaseOf<NanBoxedValue, T> || IsConvertible<T, Cell const*>,
            "RootVector element type must be convertible to Cell const* or derive from NanBoxedValue");
        for (auto& value : *this) {
            if constexpr (IsBaseOf<NanBoxedValue, T>) {
                if (value.is_cell())
                    roots.set(&const_cast<T&>(value).as_cell(), HeapRoot { .type = HeapRoot::Type::RootVector });
            } else {
                roots.set(const_cast<Cell*>(static_cast<Cell const*>(value)), HeapRoot { .type = HeapRoot::Type::RootVector });
            }
        }
    }
};

template<typename T>
RootVector(Heap&, ReadonlySpan<T> const&) -> RootVector<T>;

template<typename T>
RootVector(Heap&, Span<T> const&) -> RootVector<T>;

template<typename T>
RootVector(Heap&, Vector<T> const&) -> RootVector<T>;

// Move the underlying storage of `root_vector` into a plain Vector that can be
// stored as a directly-traced field of a GC::Cell-derived class. The returned
// Vector is no longer registered with the heap; the destination is expected to
// be traced via visit_edges().
//
// Only callable from the member-initializer list of a GC::Cell-derived class's
// constructor (`m_field(GC::adopt_root_vector(move(rv)))`) or a direct
// assignment to a traced member field (`m_field = GC::adopt_root_vector(move(rv));`).
// Enforced at compile time by LibJSGCPluginAction's VisitCallExpr check — see
// Tests/ClangPlugins/LibJSGCTests/adopt_container_restricted.cpp for the full
// set of allowed and rejected contexts.
template<typename T, size_t inline_capacity>
Vector<T, inline_capacity> adopt_root_vector(RootVector<T, inline_capacity>&& root_vector)
{
    return move(static_cast<Vector<T, inline_capacity>&>(root_vector));
}

}
