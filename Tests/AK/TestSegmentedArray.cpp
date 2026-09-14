/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/SegmentedArray.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

TEST_CASE(elements_keep_their_addresses_across_growth)
{
    SegmentedArray<size_t, 4> array;
    Vector<size_t*> addresses;

    for (size_t i = 0; i < 10'000; ++i)
        addresses.append(&array.append(i));

    EXPECT_EQ(array.size(), 10'000u);
    for (size_t i = 0; i < addresses.size(); ++i) {
        EXPECT_EQ(&array[i], addresses[i]);
        EXPECT_EQ(array[i], i);
    }
}

TEST_CASE(indices_are_contiguous_across_segment_boundaries)
{
    SegmentedArray<size_t, 2> array;
    // Segment sizes are 2, 4, 8, ... so these indices sit on every boundary of the first few segments.
    for (size_t i = 0; i < 2 + 4 + 8 + 16 + 32; ++i)
        array.append(i);

    for (size_t i = 0; i < array.size(); ++i)
        EXPECT_EQ(array[i], i);
}

TEST_CASE(destructor_destroys_every_element)
{
    static size_t s_live_count = 0;
    struct Counted {
        Counted() { ++s_live_count; }
        ~Counted() { --s_live_count; }
    };

    {
        SegmentedArray<Counted, 8> array;
        for (size_t i = 0; i < 1'000; ++i)
            array.empend();
        EXPECT_EQ(s_live_count, 1'000u);
    }
    EXPECT_EQ(s_live_count, 0u);
}

TEST_CASE(readers_observe_published_elements_while_appending)
{
    constexpr size_t element_count = 200'000;
    SegmentedArray<size_t, 16> array;
    Atomic<bool> writer_done { false };

    auto writer = Threading::Thread::construct("SegmentedArrayWriter"sv, [&] {
        for (size_t i = 0; i < element_count; ++i)
            array.append(i);
        writer_done.store(true);
        return 0;
    });
    writer->start();

    size_t verified_up_to = 0;
    while (true) {
        bool done = writer_done.load();
        auto size = array.size();
        for (; verified_up_to < size; ++verified_up_to)
            EXPECT_EQ(array[verified_up_to], verified_up_to);
        if (done)
            break;
    }
    MUST(writer->join());

    EXPECT_EQ(verified_up_to, element_count);
}
