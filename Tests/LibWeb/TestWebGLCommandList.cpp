/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebGL/WebGLCommandList.h>

using Web::WebGL::WebGLCommandHeader;
using Web::WebGL::WebGLCommandList;
namespace Commands = Web::WebGL::Commands;

static ALWAYS_INLINE void preserve_record_write(void const* data)
{
    asm volatile(""
        :
        : "r"(data)
        : "memory");
}

static void benchmark_record_writer(Web::WebGL::WebGLCommandType type, ReadonlyBytes payload, ReadonlyBytes inline_data, size_t iteration_count)
{
    auto record_size = WebGLCommandList::padded_record_size(payload, inline_data);
    auto storage = MUST(ByteBuffer::create_uninitialized(4 * MiB));
    size_t cursor = 0;

    for (size_t iteration = 0; iteration < iteration_count; ++iteration) {
        if (record_size > storage.size() - cursor)
            cursor = 0;

        auto destination = storage.bytes().slice(cursor, record_size);
        WebGLCommandList::write_record(destination, type, payload, inline_data);
        preserve_record_write(destination.data());
        cursor += record_size;
    }
}

template<typename Command>
static void benchmark_typed_record_writer(Command const& command, size_t iteration_count)
{
    auto record_size = WebGLCommandList::padded_record_size({ &command, sizeof(command) }, {});
    auto storage = MUST(ByteBuffer::create_uninitialized(4 * MiB));
    size_t cursor = 0;

    for (size_t iteration = 0; iteration < iteration_count; ++iteration) {
        if (record_size > storage.size() - cursor)
            cursor = 0;

        auto destination = storage.bytes().slice(cursor, record_size);
        WebGLCommandList::write_record(destination, command);
        preserve_record_write(destination.data());
        cursor += record_size;
    }
}

template<typename Command>
static void benchmark_typed_record_writer(Command const& command, ReadonlyBytes inline_data, size_t iteration_count)
{
    auto record_size = WebGLCommandList::padded_record_size({ &command, sizeof(command) }, inline_data);
    auto storage = MUST(ByteBuffer::create_uninitialized(4 * MiB));
    size_t cursor = 0;

    for (size_t iteration = 0; iteration < iteration_count; ++iteration) {
        if (record_size > storage.size() - cursor)
            cursor = 0;

        auto destination = storage.bytes().slice(cursor, record_size);
        WebGLCommandList::write_record(destination, command, inline_data);
        preserve_record_write(destination.data());
        cursor += record_size;
    }
}

TEST_CASE(record_layout_without_inline_data)
{
    Array<u8, 5> payload {};
    auto layout = WebGLCommandList::record_layout(payload, {});

    EXPECT_EQ(layout.payload_size, payload.size());
    EXPECT_EQ(layout.inline_data_size, 0u);
    EXPECT_EQ(layout.internal_padding_size, 0u);
    EXPECT_EQ(layout.trailing_padding_size, 3u);
    EXPECT_EQ(layout.record_size, 16u);
    EXPECT_EQ(layout.record_size, sizeof(WebGLCommandHeader) + layout.payload_size + layout.trailing_padding_size);
}

TEST_CASE(record_layout_with_internal_and_trailing_padding)
{
    Array<u8, 5> payload {};
    Array<u8, 3> inline_data {};
    auto layout = WebGLCommandList::record_layout(payload, inline_data);

    EXPECT_EQ(layout.payload_size, payload.size());
    EXPECT_EQ(layout.inline_data_size, inline_data.size());
    EXPECT_EQ(layout.internal_padding_size, 11u);
    EXPECT_EQ(layout.trailing_padding_size, 5u);
    EXPECT_EQ(layout.record_size, 32u);
    EXPECT_EQ(layout.record_size, sizeof(WebGLCommandHeader) + layout.payload_size + layout.inline_data_size + layout.internal_padding_size + layout.trailing_padding_size);
}

TEST_CASE(record_layout_with_aligned_payload)
{
    Array<u8, 16> payload {};
    Array<u8, 4> inline_data {};
    auto layout = WebGLCommandList::record_layout(payload, inline_data);

    EXPECT_EQ(layout.internal_padding_size, 0u);
    EXPECT_EQ(layout.trailing_padding_size, 4u);
    EXPECT_EQ(layout.record_size, 32u);
}

template<typename Command>
static void expect_typed_record_to_match_generic_record(Command const& command, ReadonlyBytes inline_data = {})
{
    auto record_size = WebGLCommandList::padded_record_size({ &command, sizeof(command) }, inline_data);
    auto generic_record = MUST(ByteBuffer::create_uninitialized(record_size));
    auto typed_record = MUST(ByteBuffer::create_uninitialized(record_size));

    WebGLCommandList::write_record(generic_record, command.command_type, { &command, sizeof(command) }, inline_data);
    if (inline_data.is_empty())
        WebGLCommandList::write_record(typed_record, command);
    else
        WebGLCommandList::write_record(typed_record, command, inline_data);

    EXPECT_EQ(generic_record, typed_record);
}

TEST_CASE(typed_records_match_generic_records)
{
    Commands::ActiveTexture active_texture;
    expect_typed_record_to_match_generic_record(active_texture);

    Array<float, 16> values {};
    Commands::UniformMatrix4fv command {
        .count = 1,
        .value = {
            .offset = WebGLCommandList::first_inline_data_offset(sizeof(command)),
            .size = sizeof(values),
        },
    };
    expect_typed_record_to_match_generic_record(command, to_readonly_bytes(values.span()));
}

BENCHMARK_CASE(write_active_texture_records)
{
    Commands::ActiveTexture command;
    benchmark_record_writer(command.command_type, { &command, sizeof(command) }, {}, 2'000'000);
}

BENCHMARK_CASE(write_vertex_attrib_pointer_records)
{
    Commands::VertexAttribPointer command;
    benchmark_record_writer(command.command_type, { &command, sizeof(command) }, {}, 2'000'000);
}

BENCHMARK_CASE(write_uniform4fv_records)
{
    Array<float, 4> values {};
    Commands::Uniform4fv command {
        .count = 1,
        .value = {
            .offset = WebGLCommandList::first_inline_data_offset(sizeof(command)),
            .size = sizeof(values),
        },
    };
    benchmark_record_writer(command.command_type, { &command, sizeof(command) }, to_readonly_bytes(values.span()), 2'000'000);
}

BENCHMARK_CASE(write_uniform_matrix4fv_records)
{
    Array<float, 16> values {};
    Commands::UniformMatrix4fv command {
        .count = 1,
        .value = {
            .offset = WebGLCommandList::first_inline_data_offset(sizeof(command)),
            .size = sizeof(values),
        },
    };
    benchmark_record_writer(command.command_type, { &command, sizeof(command) }, to_readonly_bytes(values.span()), 1'000'000);
}

BENCHMARK_CASE(write_buffer_data_records)
{
    Array<u8, 4096> values {};
    Commands::BufferData command {
        .size = values.size(),
        .has_data = true,
        .data = {
            .offset = WebGLCommandList::first_inline_data_offset(sizeof(command)),
            .size = values.size(),
        },
    };
    benchmark_record_writer(command.command_type, { &command, sizeof(command) }, values, 100'000);
}

BENCHMARK_CASE(write_typed_active_texture_records)
{
    Commands::ActiveTexture command;
    benchmark_typed_record_writer(command, 2'000'000);
}

BENCHMARK_CASE(write_typed_vertex_attrib_pointer_records)
{
    Commands::VertexAttribPointer command;
    benchmark_typed_record_writer(command, 2'000'000);
}

BENCHMARK_CASE(write_typed_uniform4fv_records)
{
    Array<float, 4> values {};
    Commands::Uniform4fv command {
        .count = 1,
        .value = {
            .offset = WebGLCommandList::first_inline_data_offset(sizeof(command)),
            .size = sizeof(values),
        },
    };
    benchmark_typed_record_writer(command, to_readonly_bytes(values.span()), 2'000'000);
}

BENCHMARK_CASE(write_typed_uniform_matrix4fv_records)
{
    Array<float, 16> values {};
    Commands::UniformMatrix4fv command {
        .count = 1,
        .value = {
            .offset = WebGLCommandList::first_inline_data_offset(sizeof(command)),
            .size = sizeof(values),
        },
    };
    benchmark_typed_record_writer(command, to_readonly_bytes(values.span()), 1'000'000);
}

BENCHMARK_CASE(write_typed_buffer_data_records)
{
    Array<u8, 4096> values {};
    Commands::BufferData command {
        .size = values.size(),
        .has_data = true,
        .data = {
            .offset = WebGLCommandList::first_inline_data_offset(sizeof(command)),
            .size = values.size(),
        },
    };
    benchmark_typed_record_writer(command, values, 100'000);
}
