/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/Export.h>

namespace Wasm {

// The wasm operand stack: a fixed virtual reservation, so values never move and
// push/pop are pointer bumps. Compiled code can cache the base and top pointers.
// Regions are recycled through a per-thread pool since a Configuration (and thus
// its stack) is created for every call into wasm.
class WASM_API ValueStack {
    AK_MAKE_NONCOPYABLE(ValueStack);
    AK_MAKE_NONMOVABLE(ValueStack);

public:
    static constexpr size_t reservation_size = 64 * MiB;
    static constexpr size_t max_values = reservation_size / sizeof(Value);

    ValueStack();
    ~ValueStack();

    ALWAYS_INLINE size_t size() const { return static_cast<size_t>(m_top - m_base); }
    ALWAYS_INLINE bool is_empty() const { return m_top == m_base; }

    ALWAYS_INLINE void append(Value value)
    {
        VERIFY(m_top != m_limit);
        *m_top++ = value;
    }
    ALWAYS_INLINE void unchecked_append(Value value) { *m_top++ = value; }

    ALWAYS_INLINE Value take_last()
    {
        VERIFY(m_top != m_base);
        return *--m_top;
    }
    ALWAYS_INLINE Value unsafe_take_last() { return *--m_top; }
    ALWAYS_INLINE Value& last() { return *(m_top - 1); }
    ALWAYS_INLINE Value& unsafe_last() { return *(m_top - 1); }

    ALWAYS_INLINE Value* data() { return m_base; }
    ALWAYS_INLINE Value const* data() const { return m_base; }
    ALWAYS_INLINE Value* begin() { return m_base; }
    ALWAYS_INLINE Value* end() { return m_top; }
    ALWAYS_INLINE Value const* begin() const { return m_base; }
    ALWAYS_INLINE Value const* end() const { return m_top; }
    ALWAYS_INLINE Span<Value> span() { return { m_base, size() }; }
    ALWAYS_INLINE ReadonlySpan<Value> span() const { return { m_base, size() }; }

    ALWAYS_INLINE void shrink(size_t new_size, bool = false)
    {
        VERIFY(new_size <= size());
        m_top = m_base + new_size;
    }

    void remove(size_t index, size_t count)
    {
        VERIFY(index + count <= size());
        __builtin_memmove(m_base + index, m_base + index + count, (size() - index - count) * sizeof(Value));
        m_top -= count;
    }

    // Capacity is fixed; this is the frame-entry headroom check for stack usage hints.
    ALWAYS_INLINE void ensure_capacity(size_t total) { VERIFY(total <= max_values); }

    // Region-style use (call records): grab a block, release back to a saved mark.
    ALWAYS_INLINE Value* allocate(size_t count)
    {
        VERIFY(static_cast<size_t>(m_limit - m_top) >= count);
        auto* result = m_top;
        m_top += count;
        return result;
    }
    ALWAYS_INLINE Value* mark() const { return m_top; }
    ALWAYS_INLINE void release_to(Value* mark)
    {
        VERIFY(mark >= m_base && mark <= m_top);
        m_top = mark;
    }

    // The conservative GC scans a few slots above the top: a Value returned by
    // unsafe_take_last may be mid-flight in a caller when a collection runs.
    size_t conservative_scan_size() const { return min(size() + 8, max_values); }

    static constexpr size_t base_offset() { return __builtin_offsetof(ValueStack, m_base); }
    static constexpr size_t top_offset() { return __builtin_offsetof(ValueStack, m_top); }

private:
    Value* m_base { nullptr };
    Value* m_top { nullptr };
    Value* m_limit { nullptr };
};

}
