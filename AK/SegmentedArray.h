/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

namespace AK {

// An append-only array whose elements never move.
//
// Appends must be serialized by the caller. Readers may index any slot below a size they have
// observed, concurrently with further appends. Access to an element's contents is not synchronized.
template<typename T, size_t first_segment_size = 64>
class SegmentedArray {
    AK_MAKE_NONCOPYABLE(SegmentedArray);
    AK_MAKE_NONMOVABLE(SegmentedArray);

    static_assert(is_power_of_two(first_segment_size));

public:
    static constexpr size_t segment_count = 32;

    SegmentedArray() = default;

    ~SegmentedArray()
    {
        auto size = m_size.load(memory_order_relaxed);
        size_t segment_start = 0;
        for (size_t segment_index = 0; segment_index < segment_count; ++segment_index) {
            auto* segment = m_segments[segment_index].load(memory_order_relaxed);
            if (!segment)
                break;
            auto constructed_count = min(segment_size(segment_index), size - segment_start);
            for (size_t i = 0; i < constructed_count; ++i)
                segment[i].~T();
            kfree(segment);
            segment_start += segment_size(segment_index);
        }
    }

    size_t size() const { return m_size.load(memory_order_acquire); }
    bool is_empty() const { return size() == 0; }

    T& operator[](size_t index)
    {
        VERIFY(index < size());
        return element_at(index);
    }

    T const& operator[](size_t index) const
    {
        VERIFY(index < size());
        return element_at(index);
    }

    T& append(T&& value) { return empend(move(value)); }
    T& append(T const& value) { return empend(value); }

    template<typename... Args>
    T& empend(Args&&... args)
    {
        auto index = m_size.load(memory_order_relaxed);
        auto location = locate(index);
        VERIFY(location.segment_index < segment_count);

        auto* segment = m_segments[location.segment_index].load(memory_order_relaxed);
        if (!segment) {
            segment = static_cast<T*>(kmalloc_array(segment_size(location.segment_index), sizeof(T)));
            m_segments[location.segment_index].store(segment, memory_order_release);
        }

        auto* element = new (&segment[location.offset]) T(forward<Args>(args)...);
        m_size.store(index + 1, memory_order_release);
        return *element;
    }

private:
    struct Location {
        size_t segment_index { 0 };
        size_t offset { 0 };
    };

    static constexpr size_t first_segment_shift = count_required_bits(first_segment_size) - 1;

    static constexpr size_t segment_size(size_t segment_index) { return first_segment_size << segment_index; }

    // Segment k holds indices [first_segment_size * (2^k - 1), first_segment_size * (2^(k+1) - 1)),
    // so shifting an index up by first_segment_size lands it in a power-of-two range whose exponent
    // is the segment number.
    static constexpr Location locate(size_t index)
    {
        auto shifted = index + first_segment_size;
        auto segment_index = count_required_bits(shifted) - 1 - first_segment_shift;
        return { segment_index, shifted - segment_size(segment_index) };
    }

    T& element_at(size_t index) const
    {
        auto location = locate(index);
        auto* segment = m_segments[location.segment_index].load(memory_order_acquire);
        return segment[location.offset];
    }

    Array<Atomic<T*>, segment_count> m_segments {};
    Atomic<size_t> m_size { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::SegmentedArray;
#endif
