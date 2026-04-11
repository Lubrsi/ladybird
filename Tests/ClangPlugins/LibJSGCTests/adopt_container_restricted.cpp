/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/ConservativeHashMap.h>
#include <LibGC/ConservativeHashTable.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>

class CellWithAdoptInInitializer : public GC::Cell {
    GC_CELL(CellWithAdoptInInitializer, GC::Cell);

public:
    explicit CellWithAdoptInInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        : m_cells(GC::adopt_root_vector(move(cells)))
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    Vector<GC::Ptr<GC::Cell>> m_cells;
};

class CellWithAdoptInBody : public GC::Cell {
    GC_CELL(CellWithAdoptInBody, GC::Cell);

public:
    explicit CellWithAdoptInBody(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
    {
        // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
        m_cells = GC::adopt_root_vector(move(cells));
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    Vector<GC::Ptr<GC::Cell>> m_cells;
};

class CellWithAdoptAssignmentToMember : public GC::Cell {
    GC_CELL(CellWithAdoptAssignmentToMember, GC::Cell);

public:
    void replace(GC::ConservativeHashMap<int, GC::Ptr<GC::Cell>>&& cells)
    {
        m_cells = GC::adopt_conservative_hash_map(move(cells));
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    HashMap<int, GC::Ptr<GC::Cell>> m_cells;
};

struct NonCellStorageWithVisitedMember {
    void replace(GC::ConservativeHashMap<int, GC::Ptr<GC::Cell>>&& cells)
    {
        m_cells = GC::adopt_conservative_hash_map(move(cells));
    }

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        visitor.visit(m_cells);
    }

    HashMap<int, GC::Ptr<GC::Cell>> m_cells;
};

struct NonCellStorageWithoutVisitEdges {
    void replace(GC::ConservativeHashMap<int, GC::Ptr<GC::Cell>>&& cells)
    {
        // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes or direct assignments to traced members}}
        m_cells = GC::adopt_conservative_hash_map(move(cells));
    }

    HashMap<int, GC::Ptr<GC::Cell>> m_cells;
};

class CellWithAdoptInLambdaInitializer : public GC::Cell {
    GC_CELL(CellWithAdoptInLambdaInitializer, GC::Cell);

public:
    explicit CellWithAdoptInLambdaInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        : m_cells([&] {
            // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
            return GC::adopt_root_vector(move(cells));
        }())
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    Vector<GC::Ptr<GC::Cell>> m_cells;
};

class BaseWithVector : public GC::Cell {
    GC_CELL(BaseWithVector, GC::Cell);

public:
    explicit BaseWithVector(Vector<GC::Ptr<GC::Cell>> cells)
        : m_cells(move(cells))
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_cells);
    }

private:
    Vector<GC::Ptr<GC::Cell>> m_cells;
};

// Forwarding an adopt result to a base class constructor that takes a plain
// `Vector<...>` by value is rejected. This is fundamentally different from a
// direct member initializer like `m_cells(GC::adopt_root_vector(move(cells)))`:
// in that case the adopt result is materialised directly into the storage of
// the traced field, and is reachable via visit_edges from the moment the
// constructor body begins. Here, the adopt result is first materialised as the
// `BaseWithVector` constructor's by-value `cells` parameter, which is an
// unrooted plain `Vector<GC::Ptr<GC::Cell>>` living on the callee's stack
// frame for the duration of the base ctor call. If a GC happens to run during
// that call (it normally won't, but the invariant has to hold for any GC
// scheduling), the contents are unreachable. The plugin therefore restricts
// adopt to direct member initializers of *the current class*, not forwarded
// to base ctors. To pass adopted contents to a base class, the base ctor must
// itself take a `RootVector<...>&&` and call adopt in its own member-init list.
class CellWithAdoptInBaseInitializer : public BaseWithVector {
    GC_CELL(CellWithAdoptInBaseInitializer, BaseWithVector);

public:
    explicit CellWithAdoptInBaseInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
        : BaseWithVector(GC::adopt_root_vector(move(cells)))
    {
    }
};

struct NonCellWithAdoptInInitializer {
    explicit NonCellWithAdoptInInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
        : m_cells(GC::adopt_root_vector(move(cells)))
    {
    }

    Vector<GC::Ptr<GC::Cell>> m_cells;
};

Vector<GC::Ptr<GC::Cell>> adopt_root_vector_in_helper(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
    return GC::adopt_root_vector(move(cells));
}

Vector<GC::Ptr<GC::Cell>> adopt_conservative_vector_in_helper(GC::ConservativeVector<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
    return GC::adopt_conservative_vector(move(cells));
}

HashMap<int, GC::Ptr<GC::Cell>> adopt_root_hash_map_in_helper(GC::RootHashMap<int, GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
    return GC::adopt_root_hash_map(move(cells));
}

HashMap<int, GC::Ptr<GC::Cell>> adopt_conservative_hash_map_in_helper(GC::ConservativeHashMap<int, GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
    return GC::adopt_conservative_hash_map(move(cells));
}

HashTable<GC::Ptr<GC::Cell>> adopt_root_hash_table_in_helper(GC::RootHashTable<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
    return GC::adopt_root_hash_table(move(cells));
}

HashTable<GC::Ptr<GC::Cell>> adopt_conservative_hash_table_in_helper(GC::ConservativeHashTable<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in constructor member initializers of GC::Cell-derived classes}}
    return GC::adopt_conservative_hash_table(move(cells));
}
