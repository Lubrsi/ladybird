/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <GLES2/gl2.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibIPC/Limits.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLContextProxyBase.h>
#include <stdlib.h>
#include <string.h>

namespace Web::WebGL {

static Optional<u64> command_stream_statistics_report_interval()
{
    static auto const report_interval = []() -> Optional<u64> {
        auto const* value = getenv("LADYBIRD_WEBGL_COMMAND_PROFILE");
        if (!value)
            return {};
        if (*value == '\0')
            return 600;
        return StringView { value, strlen(value) }.to_number<u64>().value_or(600);
    }();
    return report_interval;
}

class WebGLCommandStreamStatistics {
public:
    explicit WebGLCommandStreamStatistics(u64 report_interval)
        : m_report_interval(report_interval)
    {
    }

    void record_command(WebGLCommandType type, WebGLCommandLayout const& layout)
    {
        auto& command = m_commands[to_underlying(type)];
        ++command.count;
        command.payload_bytes += layout.payload_size;
        command.inline_data_bytes += layout.inline_data_size;
        command.internal_padding_bytes += layout.internal_padding_size;
        command.trailing_padding_bytes += layout.trailing_padding_size;
        command.record_bytes += layout.record_size;
    }

    void record_out_of_line_command() { ++m_out_of_line_command_count; }
    void record_oversized_record() { ++m_oversized_record_count; }
    void record_opportunistic_rewind() { ++m_opportunistic_rewind_count; }
    void record_capacity_wrap() { ++m_capacity_wrap_count; }

    void record_wait(bool succeeded, u64 duration_nanoseconds)
    {
        ++m_wait_count;
        if (!succeeded)
            ++m_failed_wait_count;
        m_wait_duration_nanoseconds += duration_nanoseconds;
    }

    void record_shared_flush(size_t bytes)
    {
        ++m_shared_flush_count;
        m_shared_flush_bytes += bytes;
    }

    void record_out_of_line_flush(size_t bytes)
    {
        ++m_out_of_line_flush_count;
        m_out_of_line_flush_bytes += bytes;
    }

    void did_present()
    {
        ++m_presentation_count;
        if (m_report_interval != 0 && m_presentation_count >= m_report_interval)
            report_and_reset();
    }

    void report_remaining()
    {
        if (m_presentation_count != 0 || total_command_count() != 0)
            report_and_reset();
    }

private:
    struct CommandStatistics {
        u64 count { 0 };
        u64 payload_bytes { 0 };
        u64 inline_data_bytes { 0 };
        u64 internal_padding_bytes { 0 };
        u64 trailing_padding_bytes { 0 };
        u64 record_bytes { 0 };
    };

    u64 total_command_count() const
    {
        u64 count = 0;
        for (auto const& command : m_commands)
            count += command.count;
        return count;
    }

    void report_and_reset()
    {
        CommandStatistics total;
        Array<u16, webgl_command_type_count> command_indices;
        for (u16 index = 0; index < webgl_command_type_count; ++index) {
            auto const& command = m_commands[index];
            total.count += command.count;
            total.payload_bytes += command.payload_bytes;
            total.inline_data_bytes += command.inline_data_bytes;
            total.internal_padding_bytes += command.internal_padding_bytes;
            total.trailing_padding_bytes += command.trailing_padding_bytes;
            total.record_bytes += command.record_bytes;
            command_indices[index] = index;
        }

        quick_sort(command_indices, [&](u16 left, u16 right) {
            return m_commands[left].count > m_commands[right].count;
        });

        auto commands_per_presentation = m_presentation_count == 0 ? 0.0 : static_cast<double>(total.count) / m_presentation_count;
        dbgln("WebGL command profile: presentations={} commands={} commands/presentation={:.2f}", m_presentation_count, total.count, commands_per_presentation);
        dbgln("  bytes: records={} headers={} payload={} inline={} internal-padding={} trailing-padding={}", total.record_bytes, total.count * sizeof(WebGLCommandHeader), total.payload_bytes, total.inline_data_bytes, total.internal_padding_bytes, total.trailing_padding_bytes);
        dbgln("  transport: shared-flushes={} shared-bytes={} out-of-line-commands={} out-of-line-flushes={} out-of-line-bytes={} oversized={} opportunistic-rewinds={} capacity-wraps={} waits={} failed-waits={} wait-time-ns={}", m_shared_flush_count, m_shared_flush_bytes, m_out_of_line_command_count, m_out_of_line_flush_count, m_out_of_line_flush_bytes, m_oversized_record_count, m_opportunistic_rewind_count, m_capacity_wrap_count, m_wait_count, m_failed_wait_count, m_wait_duration_nanoseconds);

        for (auto index : command_indices) {
            auto const& command = m_commands[index];
            if (command.count == 0)
                break;
            auto type = static_cast<WebGLCommandType>(index);
            dbgln("  {}: count={} records={} payload={} inline={} internal-padding={} trailing-padding={}", to_string(type), command.count, command.record_bytes, command.payload_bytes, command.inline_data_bytes, command.internal_padding_bytes, command.trailing_padding_bytes);
        }

        m_commands = {};
        m_presentation_count = 0;
        m_shared_flush_count = 0;
        m_shared_flush_bytes = 0;
        m_out_of_line_command_count = 0;
        m_out_of_line_flush_count = 0;
        m_out_of_line_flush_bytes = 0;
        m_oversized_record_count = 0;
        m_opportunistic_rewind_count = 0;
        m_capacity_wrap_count = 0;
        m_wait_count = 0;
        m_failed_wait_count = 0;
        m_wait_duration_nanoseconds = 0;
    }

    Array<CommandStatistics, webgl_command_type_count> m_commands {};
    u64 m_report_interval { 0 };
    u64 m_presentation_count { 0 };
    u64 m_shared_flush_count { 0 };
    u64 m_shared_flush_bytes { 0 };
    u64 m_out_of_line_command_count { 0 };
    u64 m_out_of_line_flush_count { 0 };
    u64 m_out_of_line_flush_bytes { 0 };
    u64 m_oversized_record_count { 0 };
    u64 m_opportunistic_rewind_count { 0 };
    u64 m_capacity_wrap_count { 0 };
    u64 m_wait_count { 0 };
    u64 m_failed_wait_count { 0 };
    u64 m_wait_duration_nanoseconds { 0 };
};

void WebGLContextProxyBase::record_command_stream_statistics(WebGLCommandType type, ReadonlyBytes payload, ReadonlyBytes inline_data)
{
    m_command_stream_statistics->record_command(type, WebGLCommandList::record_layout(payload, inline_data));
}

WebGLContextProxyBase::WebGLContextProxyBase(NonnullRefPtr<RemoteWebGLTransport> transport, WebGLVersion webgl_version, Vector<String> supported_extensions)
    : m_transport(move(transport))
    , m_webgl_version(webgl_version)
    , m_supported_extensions(move(supported_extensions))
{
    if (auto report_interval = command_stream_statistics_report_interval(); report_interval.has_value())
        m_command_stream_statistics = make<WebGLCommandStreamStatistics>(report_interval.value());
    initialize_shared_command_buffer();
}

WebGLContextProxyBase::~WebGLContextProxyBase()
{
    if (m_command_stream_statistics)
        m_command_stream_statistics->report_remaining();
    m_transport->destroy_context();
}

void WebGLContextProxyBase::restore(NonnullRefPtr<RemoteWebGLTransport> transport, Vector<String> supported_extensions)
{
    if (m_command_stream_statistics)
        m_command_stream_statistics->report_remaining();

    m_transport = move(transport);
    m_supported_extensions = move(supported_extensions);
    m_lost = false;
    m_out_of_line_commands.clear_with_capacity();
    m_pending_bitmaps.clear_with_capacity();
    m_string_cache.clear();
    initialize_shared_command_buffer();
}

void WebGLContextProxyBase::initialize_shared_command_buffer()
{
    m_shared_data_cursor = 0;
    m_shared_data_flush_base = 0;
    m_last_published_flush_sequence_number = 0;

    auto shared_command_buffer_or_error = WebGLSharedCommandBuffer::create();
    if (shared_command_buffer_or_error.is_error()) {
        m_shared_command_buffer = {};
        return;
    }
    m_shared_command_buffer = shared_command_buffer_or_error.release_value();
    m_transport->set_shared_command_buffer(m_shared_command_buffer.buffer());
}

Bytes WebGLContextProxyBase::prepare_record_destination_slow(WebGLCommandType type, ReadonlyBytes payload, ReadonlyBytes inline_data, size_t record_size)
{
    if (m_lost)
        return {};

    if (!m_shared_command_buffer.is_valid()) {
        if (m_command_stream_statistics) {
            record_command_stream_statistics(type, payload, inline_data);
            m_command_stream_statistics->record_out_of_line_command();
        }
        m_out_of_line_commands.append_bytes(type, payload, inline_data);
        if (m_out_of_line_commands.size_in_bytes() >= max_pending_command_bytes)
            flush_commands();
        return {};
    }

    auto data_region = m_shared_command_buffer.data_region();
    if (record_size > data_region.size()) {
        if (m_command_stream_statistics) {
            record_command_stream_statistics(type, payload, inline_data);
            m_command_stream_statistics->record_oversized_record();
        }
        flush_commands();
        WebGLCommandList oversized_command_list;
        oversized_command_list.append_bytes(type, payload, inline_data);
        m_transport->send_commands(oversized_command_list.buffer(), {});
        return {};
    }

    rewind_shared_data_cursor_if_all_published_commands_executed();
    ensure_shared_data_capacity(record_size);
    if (m_lost)
        return {};

    if (m_command_stream_statistics)
        record_command_stream_statistics(type, payload, inline_data);
    return data_region.slice(m_shared_data_cursor, record_size);
}

void WebGLContextProxyBase::rewind_shared_data_cursor_if_all_published_commands_executed()
{
    if (m_shared_data_cursor == 0 || m_shared_data_cursor != m_shared_data_flush_base)
        return;
    if (m_shared_command_buffer.executed_flush_sequence_number() >= m_last_published_flush_sequence_number) {
        if (m_command_stream_statistics)
            m_command_stream_statistics->record_opportunistic_rewind();
        m_shared_data_cursor = 0;
        m_shared_data_flush_base = 0;
    }
}

void WebGLContextProxyBase::ensure_shared_data_capacity(size_t record_size)
{
    auto data_region_capacity = m_shared_command_buffer.data_region().size();
    VERIFY(record_size <= data_region_capacity);
    if (record_size <= data_region_capacity - m_shared_data_cursor)
        return;

    if (m_command_stream_statistics)
        m_command_stream_statistics->record_capacity_wrap();
    flush_commands();
    VERIFY(m_shared_data_cursor == m_shared_data_flush_base);
    if (m_shared_command_buffer.executed_flush_sequence_number() < m_last_published_flush_sequence_number) {
        Optional<Core::ElapsedTimer> wait_timer;
        if (m_command_stream_statistics)
            wait_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        auto wait_succeeded = m_transport->wait_until_published_commands_executed();
        if (m_command_stream_statistics)
            m_command_stream_statistics->record_wait(wait_succeeded, wait_timer->elapsed_time().to_nanoseconds());
        if (!wait_succeeded) {
            set_lost();
            return;
        }
        VERIFY(m_shared_command_buffer.executed_flush_sequence_number() >= m_last_published_flush_sequence_number);
    }
    m_shared_data_cursor = 0;
    m_shared_data_flush_base = 0;
}

void WebGLContextProxyBase::flush_commands()
{
    if (m_shared_command_buffer.is_valid()) {
        if (m_shared_data_cursor == m_shared_data_flush_base)
            return;
        auto flushed_bytes = m_shared_data_cursor - m_shared_data_flush_base;
        if (m_command_stream_statistics)
            m_command_stream_statistics->record_shared_flush(flushed_bytes);
        m_last_published_flush_sequence_number++;
        m_transport->send_commands_from_shared_buffer(
            m_shared_data_flush_base,
            flushed_bytes,
            m_last_published_flush_sequence_number,
            m_pending_bitmaps);
        m_shared_data_flush_base = m_shared_data_cursor;
        m_pending_bitmaps.clear_with_capacity();
        return;
    }

    if (m_out_of_line_commands.is_empty())
        return;
    if (m_command_stream_statistics)
        m_command_stream_statistics->record_out_of_line_flush(m_out_of_line_commands.size_in_bytes());
    m_transport->send_commands(m_out_of_line_commands.buffer(), m_pending_bitmaps);
    m_out_of_line_commands.clear_with_capacity();
    m_pending_bitmaps.clear_with_capacity();
}

u32 WebGLContextProxyBase::append_pending_bitmap(Gfx::DecodedImageFrame frame)
{
    static_assert(IPC::MAX_MESSAGE_FD_COUNT > 1);
    // WebGL command bytes are transferred in one anonymous buffer attachment.
    static constexpr size_t max_bitmap_attachments_per_message = IPC::MAX_MESSAGE_FD_COUNT - 1;

    if (m_pending_bitmaps.size() >= max_bitmap_attachments_per_message)
        flush_commands();

    // Reserve space for the upcoming bitmap-referencing command now: a capacity flush
    // between this append and its record would publish the bitmap with the wrong batch.
    static constexpr size_t reserved_record_size_for_bitmap_command = 256;
    static_assert(sizeof(WebGLCommandHeader) + sizeof(Commands::TexImage2DFromBitmap) <= reserved_record_size_for_bitmap_command);
    static_assert(sizeof(WebGLCommandHeader) + sizeof(Commands::TexSubImage2DFromBitmap) <= reserved_record_size_for_bitmap_command);
    static_assert(sizeof(WebGLCommandHeader) + sizeof(Commands::TexImage3DFromBitmap) <= reserved_record_size_for_bitmap_command);
    static_assert(sizeof(WebGLCommandHeader) + sizeof(Commands::TexSubImage3DFromBitmap) <= reserved_record_size_for_bitmap_command);
    if (m_shared_command_buffer.is_valid())
        ensure_shared_data_capacity(reserved_record_size_for_bitmap_command);

    auto bitmap_index = static_cast<u32>(m_pending_bitmaps.size());
    m_pending_bitmaps.append(move(frame));
    return bitmap_index;
}

ByteBuffer WebGLContextProxyBase::send_sync_call(ByteBuffer request)
{
    if (m_lost)
        return {};
    flush_commands();
    auto reply = m_transport->sync_call(move(request));
    if (reply.is_empty())
        set_lost();
    return reply;
}

ReadPixelsResult WebGLContextProxyBase::read_pixels_robust_angle_into_shared_buffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size, Core::AnonymousBuffer const& pixels)
{
    flush_commands();
    return m_transport->read_pixels_robust_angle(x, y, width, height, format, type, buf_size, pixels);
}

void WebGLContextProxyBase::set_size(Gfx::IntSize const& size)
{
    record(Commands::SetDrawingBufferSize { .width = size.width(), .height = size.height() });
}

void WebGLContextProxyBase::present_canvas_for_compositing(bool preserve_drawing_buffer)
{
    flush_commands();
    m_transport->present_canvas(preserve_drawing_buffer);
    if (m_command_stream_statistics)
        m_command_stream_statistics->did_present();
}

RefPtr<Gfx::Bitmap> WebGLContextProxyBase::read_back_drawing_buffer(Gfx::IntRect const& rect)
{
    if (m_lost)
        return nullptr;
    flush_commands();
    auto bitmap = m_transport->read_back_drawing_buffer(rect);
    if (!bitmap.is_valid())
        return nullptr;
    return bitmap.bitmap();
}

void WebGLContextProxyBase::read_pixels_into_pixel_pack_buffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, long long offset)
{
    record(Commands::ReadPixelsIntoPixelPackBuffer {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .format = format,
        .type = type,
        .offset = static_cast<GLintptr>(offset),
    });
}

void WebGLContextProxyBase::tex_image2d_from_bitmap(GLenum target, GLint level, GLint internalformat, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexImage2DFromBitmap {
        .target = target,
        .level = level,
        .internalformat = internalformat,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

void WebGLContextProxyBase::tex_sub_image2d_from_bitmap(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexSubImage2DFromBitmap {
        .target = target,
        .level = level,
        .xoffset = xoffset,
        .yoffset = yoffset,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

void WebGLContextProxyBase::tex_image3d_from_bitmap(GLenum target, GLint level, GLint internalformat, GLsizei depth, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexImage3DFromBitmap {
        .target = target,
        .level = level,
        .internalformat = internalformat,
        .depth = depth,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

void WebGLContextProxyBase::tex_sub_image3d_from_bitmap(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei depth, GLenum format, GLenum type, Gfx::DecodedImageFrame frame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha)
{
    if (m_lost)
        return;
    auto bitmap_index = append_pending_bitmap(move(frame));
    auto has_explicit_destination_size = destination_size.has_value();
    record(Commands::TexSubImage3DFromBitmap {
        .target = target,
        .level = level,
        .xoffset = xoffset,
        .yoffset = yoffset,
        .zoffset = zoffset,
        .depth = depth,
        .format = format,
        .type = type,
        .bitmap_index = bitmap_index,
        .has_explicit_destination_size = has_explicit_destination_size,
        .destination_width = has_explicit_destination_size ? destination_size->width() : 0,
        .destination_height = has_explicit_destination_size ? destination_size->height() : 0,
        .flip_y = flip_y,
        .premultiply_alpha = premultiply_alpha,
    });
}

bool WebGLContextProxyBase::read_buffer_sub_data(GLenum target, long long offset, Bytes destination)
{
    if (m_lost)
        return false;
    if (destination.is_empty())
        return true;

    auto shared_data_or_error = Core::AnonymousBuffer::create_with_size(destination.size());
    if (shared_data_or_error.is_error()) {
        set_pending_local_error(GL_OUT_OF_MEMORY);
        return false;
    }

    auto shared_data = shared_data_or_error.release_value();
    flush_commands();
    if (!m_transport->read_buffer_sub_data(target, static_cast<GLintptr>(offset), static_cast<GLintptr>(destination.size()), shared_data))
        return false;
    if (m_lost)
        return false;
    __builtin_memcpy(destination.data(), shared_data.data<void>(), destination.size());
    return true;
}

void WebGLContextProxy::shader_source(GLuint shader, GLsizei count, GLchar const* const* string, GLint const* length)
{
    VERIFY(count == 1);
    auto source_length = length ? static_cast<size_t>(length[0]) : __builtin_strlen(string[0]);
    ByteBuffer source_bytes = MUST(ByteBuffer::create_uninitialized(source_length + 1));
    __builtin_memcpy(source_bytes.data(), string[0], source_length);
    source_bytes[source_length] = 0;

    Commands::ShaderSource command { .shader = shader, .source = {} };
    command.source = { WebGLCommandList::first_inline_data_offset(sizeof(command)), static_cast<u32>(source_bytes.size()) };
    record(command, source_bytes);
}

static ByteBuffer pack_strings(GLsizei count, GLchar const* const* strings)
{
    StringBuilder builder;
    for (GLsizei i = 0; i < count; ++i) {
        builder.append({ strings[i], __builtin_strlen(strings[i]) });
        builder.append('\0');
    }
    return MUST(builder.to_byte_buffer());
}

void WebGLContextProxy::transform_feedback_varyings(GLuint program, GLsizei count, GLchar const* const* varyings, GLenum bufferMode)
{
    auto varyings_bytes = pack_strings(count, varyings);
    Commands::TransformFeedbackVaryings command { .program = program, .count = count, .varyings = {}, .buffer_mode = bufferMode };
    command.varyings = { WebGLCommandList::first_inline_data_offset(sizeof(command)), static_cast<u32>(varyings_bytes.size()) };
    record(command, varyings_bytes);
}

void WebGLContextProxy::read_pixels_robust_angle(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, GLsizei* length, GLsizei* columns, GLsizei* rows, void* pixels)
{
    if (is_lost())
        return;

    Core::AnonymousBuffer shared_pixels;
    if (bufSize > 0) {
        auto shared_pixels_or_error = Core::AnonymousBuffer::create_with_size(static_cast<size_t>(bufSize));
        if (shared_pixels_or_error.is_error()) {
            set_pending_local_error(GL_OUT_OF_MEMORY);
            return;
        }
        shared_pixels = shared_pixels_or_error.release_value();
    }

    auto result = read_pixels_robust_angle_into_shared_buffer(x, y, width, height, format, type, bufSize, shared_pixels);
    if (is_lost())
        return;
    if (length)
        *length = result.length;
    if (columns)
        *columns = result.columns;
    if (rows)
        *rows = result.rows;
    if (pixels && result.length > 0) {
        VERIFY(result.length <= bufSize);
        __builtin_memcpy(pixels, shared_pixels.data<void>(), static_cast<size_t>(result.length));
    }
}

GLubyte const* WebGLContextProxy::get_string(GLenum name)
{
    if (auto cached = m_string_cache.get(name); cached.has_value())
        return cached.value()->data();

    SyncCalls::GetString::Request request { .name = name };
    auto reply_bytes = send_sync_call(WebGLSyncCall::encode_request<SyncCalls::GetString>(request));
    if (is_lost())
        return reinterpret_cast<GLubyte const*>("");
    auto reply = WebGLSyncCall::decode_reply<SyncCalls::GetString::Reply>(reply_bytes);
    auto resolved = WebGLCommandList::resolve_string_span(reply_bytes, reply.value);
    auto value = make<ByteBuffer>(MUST(ByteBuffer::copy(resolved)));
    auto const* data = value->data();
    m_string_cache.set(name, move(value));
    return data;
}

void WebGLContextProxy::get_vertex_attrib_pointerv_robust_angle(GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, void** pointer)
{
    (void)bufSize;
    SyncCalls::GetVertexAttribPointervRobustANGLE::Request request { .index = index, .pname = pname };
    auto reply_bytes = send_sync_call(WebGLSyncCall::encode_request<SyncCalls::GetVertexAttribPointervRobustANGLE>(request));
    if (is_lost())
        return;
    auto reply = WebGLSyncCall::decode_reply<SyncCalls::GetVertexAttribPointervRobustANGLE::Reply>(reply_bytes);
    if (length)
        *length = 1;
    if (pointer)
        *pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(reply.pointer));
}

void WebGLContextProxy::get_uniform_indices(GLuint program, GLsizei uniformCount, GLchar const* const* uniformNames, GLuint* uniformIndices)
{
    auto names_bytes = pack_strings(uniformCount, uniformNames);
    SyncCalls::GetUniformIndices::Request request { .program = program, .uniform_count = uniformCount, .uniform_names = {} };
    request.uniform_names = { WebGLCommandList::first_inline_data_offset(sizeof(request)), static_cast<u32>(names_bytes.size()) };
    auto reply_bytes = send_sync_call(WebGLSyncCall::encode_request<SyncCalls::GetUniformIndices>(request, names_bytes));
    if (is_lost())
        return;
    auto reply = WebGLSyncCall::decode_reply<SyncCalls::GetUniformIndices::Reply>(reply_bytes);
    if (uniformIndices)
        WebGLCommandList::copy_data_span(reply_bytes, reply.uniform_indices, { uniformIndices, static_cast<size_t>(uniformCount) * sizeof(GLuint) });
}

void* WebGLContextProxy::map_buffer_range(GLenum, GLintptr, GLsizeiptr, GLbitfield)
{
    // getBufferSubData() goes through read_buffer_sub_data() instead; nothing else maps.
    VERIFY_NOT_REACHED();
}

GLboolean WebGLContextProxy::unmap_buffer(GLenum)
{
    VERIFY_NOT_REACHED();
}

}
