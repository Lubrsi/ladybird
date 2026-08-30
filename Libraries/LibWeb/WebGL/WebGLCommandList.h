/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/NumericLimits.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebGL/WebGLCommands.h>

namespace Web::WebGL {

struct WebGLCommandHeader {
    WebGLCommandType type;
    u32 payload_size { 0 }; // command struct + inline data + trailing padding
};
static_assert(IsTriviallyCopyable<WebGLCommandHeader>);

struct WebGLCommandLayout {
    size_t payload_size { 0 };
    size_t inline_data_size { 0 };
    size_t internal_padding_size { 0 };
    size_t trailing_padding_size { 0 };
    size_t record_size { 0 };
};

class WEB_API WebGLCommandList {
public:
    static constexpr size_t command_alignment = 16;

    static constexpr u32 first_inline_data_offset(size_t command_size)
    {
        return static_cast<u32>(align_up_to(command_size, command_alignment));
    }

    static constexpr u32 next_inline_data_offset(WebGLDataSpan previous)
    {
        return static_cast<u32>(align_up_to(previous.offset + previous.size, command_alignment));
    }

    template<typename Command>
    void append(Command const& command, ReadonlyBytes inline_data = {})
    {
        append_bytes(Command::command_type, { &command, sizeof(command) }, inline_data);
    }

    void append_bytes(WebGLCommandType, ReadonlyBytes payload, ReadonlyBytes inline_data);

    static WebGLCommandLayout record_layout(ReadonlyBytes payload, ReadonlyBytes inline_data);
    static size_t padded_record_size(ReadonlyBytes payload, ReadonlyBytes inline_data)
    {
        auto payload_layout_size = payload.size();
        if (!inline_data.is_empty())
            payload_layout_size = align_up_to(payload_layout_size, command_alignment) + inline_data.size();
        return align_up_to(sizeof(WebGLCommandHeader) + payload_layout_size, command_alignment);
    }
    static void write_record(Bytes destination, WebGLCommandType, ReadonlyBytes payload, ReadonlyBytes inline_data);

    template<typename Command>
    static constexpr size_t fixed_record_size()
    {
        return align_up_to(sizeof(WebGLCommandHeader) + sizeof(Command), command_alignment);
    }

    template<typename Command>
    static void write_record(Bytes destination, Command const& command)
    {
        static_assert(IsTriviallyCopyable<Command>);

        constexpr auto record_size = fixed_record_size<Command>();
        VERIFY(destination.size() == record_size);

        constexpr auto padded_payload_size = record_size - sizeof(WebGLCommandHeader);
        static_assert(padded_payload_size <= NumericLimits<u32>::max());

        WebGLCommandHeader header {
            .type = Command::command_type,
            .payload_size = static_cast<u32>(padded_payload_size),
        };
        __builtin_memcpy(destination.data(), &header, sizeof(header));

        auto payload = destination.slice(sizeof(header));
        __builtin_memcpy(payload.data(), &command, sizeof(command));
        __builtin_memset(payload.offset_pointer(sizeof(command)), 0, padded_payload_size - sizeof(command));
    }

    template<typename Command>
    static void write_record(Bytes destination, Command const& command, ReadonlyBytes inline_data)
    {
        if (inline_data.is_empty()) {
            write_record(destination, command);
            return;
        }

        static_assert(IsTriviallyCopyable<Command>);

        constexpr auto inline_data_offset = align_up_to(sizeof(Command), command_alignment);
        auto unpadded_payload_size = inline_data_offset + inline_data.size();
        auto record_size = align_up_to(sizeof(WebGLCommandHeader) + unpadded_payload_size, command_alignment);
        VERIFY(destination.size() == record_size);

        auto padded_payload_size = record_size - sizeof(WebGLCommandHeader);
        VERIFY(padded_payload_size <= NumericLimits<u32>::max());

        WebGLCommandHeader header {
            .type = Command::command_type,
            .payload_size = static_cast<u32>(padded_payload_size),
        };
        __builtin_memcpy(destination.data(), &header, sizeof(header));

        auto payload = destination.slice(sizeof(header));
        __builtin_memcpy(payload.data(), &command, sizeof(command));
        __builtin_memset(payload.offset_pointer(sizeof(command)), 0, inline_data_offset - sizeof(command));
        __builtin_memcpy(payload.offset_pointer(inline_data_offset), inline_data.data(), inline_data.size());
        __builtin_memset(payload.offset_pointer(unpadded_payload_size), 0, padded_payload_size - unpadded_payload_size);
    }

    template<typename Callback>
    static ErrorOr<void> for_each_command(ReadonlyBytes bytes, Callback&& callback)
    {
        size_t offset = 0;
        while (offset < bytes.size()) {
            if (bytes.size() - offset < sizeof(WebGLCommandHeader))
                return Error::from_string_literal("Truncated WebGL command header");
            WebGLCommandHeader header;
            __builtin_memcpy(&header, bytes.offset_pointer(offset), sizeof(header));
            if (to_underlying(header.type) >= webgl_command_type_count)
                return Error::from_string_literal("Invalid WebGL command type");
            if (header.payload_size > bytes.size() - offset - sizeof(header))
                return Error::from_string_literal("Truncated WebGL command payload");
            auto payload = bytes.slice(offset + sizeof(header), header.payload_size);
            TRY(visit_webgl_command_type(header.type, [&]<typename Command>() -> ErrorOr<void> {
                if (payload.size() < sizeof(Command))
                    return Error::from_string_literal("WebGL command payload too small");
                Command command;
                __builtin_memcpy(&command, payload.data(), sizeof(Command));
                return callback(command, payload);
            }));
            offset += sizeof(WebGLCommandHeader) + header.payload_size;
        }
        return {};
    }

    static ReadonlyBytes resolve_data_span(ReadonlyBytes payload, WebGLDataSpan span)
    {
        VERIFY(span.offset <= payload.size());
        VERIFY(span.size <= payload.size() - span.offset);
        return payload.slice(span.offset, span.size);
    }

    template<typename T>
    static Span<T const> resolve_typed_span(ReadonlyBytes payload, WebGLDataSpan span)
    {
        auto bytes = resolve_data_span(payload, span);
        VERIFY(reinterpret_cast<uintptr_t>(bytes.data()) % alignof(T) == 0);
        VERIFY(bytes.size() % sizeof(T) == 0);
        return Span<T const> { reinterpret_cast<T const*>(bytes.data()), bytes.size() / sizeof(T) };
    }

    static ReadonlyBytes resolve_string_span(ReadonlyBytes payload, WebGLDataSpan span)
    {
        auto bytes = resolve_data_span(payload, span);
        VERIFY(!bytes.is_empty());
        VERIFY(bytes[bytes.size() - 1] == 0);
        return bytes;
    }

    static void copy_data_span(ReadonlyBytes payload, WebGLDataSpan span, Bytes destination)
    {
        auto resolved = resolve_data_span(payload, span);
        VERIFY(resolved.size() <= destination.size());
        __builtin_memcpy(destination.data(), resolved.data(), resolved.size());
    }

    ReadonlyBytes bytes() const { return m_bytes; }
    ByteBuffer const& buffer() const { return m_bytes; }
    void clear_with_capacity() { m_bytes.set_size(0); }
    bool is_empty() const { return m_bytes.is_empty(); }
    size_t size_in_bytes() const { return m_bytes.size(); }

private:
    ByteBuffer m_bytes;
};

struct WebGLSyncCallHeader {
    WebGLSyncCallType type;
    u32 payload_size { 0 };
};
static_assert(IsTriviallyCopyable<WebGLSyncCallHeader>);

class WEB_API WebGLSyncCall {
public:
    template<typename Call>
    static ByteBuffer encode_request(typename Call::Request const& request, ReadonlyBytes inline_data = {})
    {
        return encode_request_bytes(Call::call_type, { &request, sizeof(request) }, inline_data);
    }

    template<typename Callback>
    static ErrorOr<ByteBuffer> dispatch_request(ReadonlyBytes bytes, Callback&& callback)
    {
        if (bytes.size() < sizeof(WebGLSyncCallHeader))
            return Error::from_string_literal("Truncated WebGL sync call header");
        WebGLSyncCallHeader header;
        __builtin_memcpy(&header, bytes.data(), sizeof(header));
        if (to_underlying(header.type) >= webgl_sync_call_type_count)
            return Error::from_string_literal("Invalid WebGL sync call type");
        if (header.payload_size != bytes.size() - sizeof(header))
            return Error::from_string_literal("Truncated WebGL sync call payload");
        auto payload = bytes.slice(sizeof(header), header.payload_size);
        return visit_webgl_sync_call_type(header.type, [&]<typename Call>() -> ErrorOr<ByteBuffer> {
            if (payload.size() < sizeof(typename Call::Request))
                return Error::from_string_literal("WebGL sync call payload too small");
            typename Call::Request request;
            __builtin_memcpy(&request, payload.data(), sizeof(request));
            return callback.template operator()<Call>(request, payload);
        });
    }

    template<typename Reply>
    static ByteBuffer encode_reply(Reply const& reply, ReadonlyBytes inline_data = {}, ReadonlyBytes more_inline_data = {})
    {
        return encode_reply_bytes({ &reply, sizeof(reply) }, inline_data, more_inline_data);
    }

    template<typename Reply>
    static Reply decode_reply(ReadonlyBytes bytes)
    {
        VERIFY(bytes.size() >= sizeof(Reply));
        Reply reply;
        __builtin_memcpy(&reply, bytes.data(), sizeof(reply));
        return reply;
    }

private:
    static ByteBuffer encode_request_bytes(WebGLSyncCallType, ReadonlyBytes request, ReadonlyBytes inline_data);
    static ByteBuffer encode_reply_bytes(ReadonlyBytes reply, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data);
};

}
