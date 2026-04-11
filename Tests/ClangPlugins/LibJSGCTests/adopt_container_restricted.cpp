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
        // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
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
        // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
        m_cells = GC::adopt_conservative_hash_map(move(cells));
    }

    HashMap<int, GC::Ptr<GC::Cell>> m_cells;
};

class CellWithAdoptInLambdaInitializer : public GC::Cell {
    GC_CELL(CellWithAdoptInLambdaInitializer, GC::Cell);

public:
    explicit CellWithAdoptInLambdaInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        : m_cells([&] {
            // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
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

// Forwarding an adopt result to a base ctor with a by-value `Vector<...>`
// parameter is rejected: the parameter is unrooted on the callee's stack.
class CellWithAdoptInBaseInitializer : public BaseWithVector {
    GC_CELL(CellWithAdoptInBaseInitializer, BaseWithVector);

public:
    explicit CellWithAdoptInBaseInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
        : BaseWithVector(GC::adopt_root_vector(move(cells)))
    {
    }
};

struct NonCellWithAdoptInInitializer {
    explicit NonCellWithAdoptInInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
        : m_cells(GC::adopt_root_vector(move(cells)))
    {
    }

    Vector<GC::Ptr<GC::Cell>> m_cells;
};

struct NonCellWithVisitEdgesAdoptInInitializer {
    explicit NonCellWithVisitEdgesAdoptInInitializer(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
        : m_cells(GC::adopt_root_vector(move(cells)))
    {
    }

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        visitor.visit(m_cells);
    }

    Vector<GC::Ptr<GC::Cell>> m_cells;
};

Vector<GC::Ptr<GC::Cell>> adopt_root_vector_in_helper(GC::RootVector<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
    return GC::adopt_root_vector(move(cells));
}

Vector<GC::Ptr<GC::Cell>> adopt_conservative_vector_in_helper(GC::ConservativeVector<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
    return GC::adopt_conservative_vector(move(cells));
}

HashMap<int, GC::Ptr<GC::Cell>> adopt_root_hash_map_in_helper(GC::RootHashMap<int, GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
    return GC::adopt_root_hash_map(move(cells));
}

HashMap<int, GC::Ptr<GC::Cell>> adopt_conservative_hash_map_in_helper(GC::ConservativeHashMap<int, GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
    return GC::adopt_conservative_hash_map(move(cells));
}

HashTable<GC::Ptr<GC::Cell>> adopt_root_hash_table_in_helper(GC::RootHashTable<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
    return GC::adopt_root_hash_table(move(cells));
}

HashTable<GC::Ptr<GC::Cell>> adopt_conservative_hash_table_in_helper(GC::ConservativeHashTable<GC::Ptr<GC::Cell>>&& cells)
{
    // expected-error@+1 {{GC container adopt functions may only be used in a member initializer list or a non-constructor assignment to a traced member}}
    return GC::adopt_conservative_hash_table(move(cells));
}
