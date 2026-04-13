/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(PseudoElement);
GC_DEFINE_ALLOCATOR(PseudoElementTreeNode);

void PseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_cascaded_properties);
    visitor.visit(m_computed_properties);
    visitor.visit(m_layout_node);
    visitor.visit(m_counters_set);
}

GC::Ptr<CSS::CountersSet const> PseudoElement::counters_set() const
{
    return m_counters_set;
}

CSS::CountersSet& PseudoElement::ensure_counters_set()
{
    if (!m_counters_set)
        m_counters_set = heap().allocate<CSS::CountersSet>();
    return *m_counters_set;
}

void PseudoElement::set_counters_set(GC::Ptr<CSS::CountersSet> counters_set)
{
    m_counters_set = counters_set;
}

}
