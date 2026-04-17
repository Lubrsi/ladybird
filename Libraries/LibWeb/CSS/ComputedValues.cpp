/*
 * Copyright (c) 2020-2026, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/ComputedValues.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(ComputedValues);
GC_DEFINE_ALLOCATOR(ImmutableComputedValues);
GC_DEFINE_ALLOCATOR(MutableComputedValues);

void ComputedValues::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_noninherited.visit_edges(visitor);
}

GC::Ref<ComputedValues> ComputedValues::clone_inherited_values() const
{
    auto clone = heap().allocate<ComputedValues>();
    clone->m_inherited = m_inherited;
    return clone;
}

}
