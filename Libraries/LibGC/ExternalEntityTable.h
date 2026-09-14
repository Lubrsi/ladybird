/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Noncopyable.h>
#include <AK/SegmentedArray.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGC/Export.h>

namespace GC {

struct ExternalEntityTableHandle {
    static constexpr u32 invalid_index = NumericLimits<u32>::max();

    u32 index { invalid_index };
    u32 generation { 0 };

    bool is_valid() const { return index != invalid_index && generation != 0; }
};

struct ExternalEntityTableTag {
    static constexpr u8 bit_count = 16;
    static constexpr u8 set_bit_count = bit_count / 2;
    static constexpr u16 tag_count = 12870;

    u16 value { 0 };

    constexpr bool is_balanced() const { return popcount(value) == set_bit_count; }
    constexpr bool operator==(ExternalEntityTableTag const&) const = default;
};

constexpr ExternalEntityTableTag balanced_external_entity_table_tag(u16 ordinal)
{
    VERIFY(ordinal < ExternalEntityTableTag::tag_count);

    u16 matching_tag = 0;
    for (u32 candidate = 0; candidate <= NumericLimits<u16>::max(); ++candidate) {
        if (popcount(candidate) != ExternalEntityTableTag::set_bit_count)
            continue;
        if (matching_tag == ordinal)
            return { static_cast<u16>(candidate) };
        ++matching_tag;
    }

    VERIFY_NOT_REACHED();
}

class GC_API ExternalEntityTable {
    AK_MAKE_NONCOPYABLE(ExternalEntityTable);
    AK_MAKE_NONMOVABLE(ExternalEntityTable);

protected:
    ExternalEntityTable() = default;

    ExternalEntityTableHandle allocate_entry(ExternalEntityTableTag);
    bool is_valid(ExternalEntityTableHandle, ExternalEntityTableTag) const;
    void free_entry(ExternalEntityTableHandle, ExternalEntityTableTag);

private:
    // Generation, tag and the allocated flag packed into one word, so that a thread checking a handle
    // it no longer owns reads a consistent state while another thread reuses the slot.
    struct Entry {
        static constexpr u64 initial_state = 1; // generation 1, no tag, not allocated

        static constexpr u32 generation_of(u64 state) { return static_cast<u32>(state); }
        static constexpr ExternalEntityTableTag tag_of(u64 state) { return { static_cast<u16>(state >> 32) }; }
        static constexpr bool is_allocated(u64 state) { return (state >> 48) & 1; }
        static constexpr u64 make_state(u32 generation, ExternalEntityTableTag tag, bool allocated)
        {
            return static_cast<u64>(generation) | (static_cast<u64>(tag.value) << 32) | (static_cast<u64>(allocated) << 48);
        }

        Atomic<u64> state { initial_state };
    };

    Entry* entry_for(ExternalEntityTableHandle, ExternalEntityTableTag);
    Entry const* entry_for(ExternalEntityTableHandle, ExternalEntityTableTag) const;

    // Callers serialize allocate_entry() and free_entry(); is_valid() needs no lock.
    SegmentedArray<Entry> m_entries;
    Vector<u32> m_free_entry_indices;
};

}
