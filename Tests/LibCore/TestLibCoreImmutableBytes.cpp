/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <string.h>

TEST_CASE(adopt_anonymous_buffer_views_its_bytes)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(64));
    auto const payload = "viewed through ImmutableBytes"sv;
    memcpy(buffer.data<void>(), payload.characters_without_null_termination(), payload.length());

    auto bytes = Core::ImmutableBytes::adopt_anonymous_buffer(buffer);
    EXPECT(bytes.is_valid());
    EXPECT(!bytes.is_file_backed());
    EXPECT(!bytes.is_readonly_mapped());
    EXPECT_EQ(bytes.size(), buffer.size());
    EXPECT_EQ(bytes.bytes().data(), buffer.bytes().data());
    EXPECT_EQ(bytes.bytes().slice(0, payload.length()), payload.bytes());
}

TEST_CASE(adopt_anonymous_buffer_keeps_the_mapping_alive)
{
    auto const payload = "outlives the handle it came from"sv;
    Core::ImmutableBytes bytes;
    {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(64));
        memcpy(buffer.data<void>(), payload.characters_without_null_termination(), payload.length());
        bytes = Core::ImmutableBytes::adopt_anonymous_buffer(move(buffer));
    }
    EXPECT_EQ(bytes.bytes().slice(0, payload.length()), payload.bytes());

    auto copy = MUST(bytes.copy_to_byte_buffer());
    EXPECT_EQ(copy.bytes().slice(0, payload.length()), payload.bytes());
}

TEST_CASE(adopt_invalid_anonymous_buffer_is_empty)
{
    auto bytes = Core::ImmutableBytes::adopt_anonymous_buffer(Core::AnonymousBuffer {});
    EXPECT(bytes.is_valid());
    EXPECT(bytes.is_empty());
    EXPECT(bytes.bytes().is_empty());
}

TEST_CASE(slice_views_the_same_storage)
{
    auto bytes = MUST(Core::ImmutableBytes::copy("0123456789"sv.bytes()));

    auto slice = bytes.slice(2, 5);
    EXPECT_EQ(slice.bytes(), "23456"sv.bytes());
    EXPECT_EQ(slice.bytes().data(), bytes.bytes().data() + 2);

    auto inner = slice.slice(1, 3);
    EXPECT_EQ(inner.bytes(), "345"sv.bytes());
    EXPECT_EQ(inner.bytes().data(), bytes.bytes().data() + 3);

    EXPECT_EQ(bytes.slice(0, bytes.size()).bytes().data(), bytes.bytes().data());
    EXPECT(bytes.slice(4, 0).is_empty());
}

TEST_CASE(slice_keeps_the_storage_alive)
{
    Core::ImmutableBytes slice;
    {
        auto bytes = MUST(Core::ImmutableBytes::copy("outlives the handle it was cut from"sv.bytes()));
        slice = bytes.slice(9, 10);
    }
    EXPECT_EQ(slice.bytes(), "the handle"sv.bytes());
}

TEST_CASE(slice_of_a_mapped_file_is_file_backed)
{
    auto const payload = "mapped from a file"sv;
    char path[] = "/tmp/TestLibCoreImmutableBytes.XXXXXX";
    auto fd = MUST(Core::System::mkstemp({ path, sizeof(path) }));
    MUST(Core::System::write(fd, payload.bytes()));

    auto bytes = MUST(Core::ImmutableBytes::map_from_fd_range_and_close(fd, StringView { path, sizeof(path) - 1 }, 0, payload.length()));
    MUST(Core::System::unlink(StringView { path, sizeof(path) - 1 }));
    EXPECT(bytes.is_file_backed());

    auto slice = bytes.slice(7, 6);
    EXPECT(slice.is_file_backed());
    EXPECT(!slice.is_readonly_mapped());
    EXPECT_EQ(slice.bytes(), "from a"sv.bytes());
}
