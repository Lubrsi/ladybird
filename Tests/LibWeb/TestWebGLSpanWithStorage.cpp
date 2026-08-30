/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace {

class WebGLRenderingContextBaseAccessor : public Web::WebGL::WebGLRenderingContextBase {
    WEB_NON_IDL_WRAPPABLE(WebGLRenderingContextBaseAccessor, Web::WebGL::WebGLRenderingContextBase);

public:
    using Web::WebGL::WebGLRenderingContextBase::with_float32_list;
    using Web::WebGL::WebGLRenderingContextBase::with_int32_list;
    using Web::WebGL::WebGLRenderingContextBase::with_uint32_list;
};

struct TestVM {
    TestVM()
        : vm(JS::VM::create())
        , execution_context(MUST(JS::Realm::initialize_host_defined_realm(*vm, nullptr, nullptr)))
    {
    }

    ~TestVM()
    {
        vm->pop_execution_context();
    }

    NonnullRefPtr<JS::VM> vm;
    NonnullOwnPtr<JS::ExecutionContext> execution_context;
};

template<typename ArrayType>
static GC::Ref<ArrayType> create_out_of_bounds_array(JS::Realm& realm)
{
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, 32));
    array_buffer->set_max_byte_length(32);
    MUST(array_buffer->try_resize(4));

    auto typed_array = ArrayType::create(realm, 0, array_buffer);
    typed_array->set_viewed_array_buffer(array_buffer.ptr());
    typed_array->set_array_length(4);
    typed_array->set_byte_length(4 * typed_array->element_size());
    typed_array->set_byte_offset(16);

    return typed_array;
}

}

TEST_CASE(float32_typed_list_borrows_array_buffer_storage)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Array values { 1.0f, 2.0f, 3.0f, 4.0f };
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, values.size() * sizeof(float)));
    array_buffer->overwrite(0, values.data(), values.size() * sizeof(float));
    Web::WebGL::WebGLRenderingContextBase::Float32List list {
        JS::Float32Array::create(realm, values.size(), array_buffer)
    };

    bool callback_was_invoked = false;
    MUST(WebGLRenderingContextBaseAccessor::with_float32_list(list, 1, 2, [&](ReadonlySpan<float> span) {
        callback_was_invoked = true;
        EXPECT_EQ(span.data(), reinterpret_cast<float const*>(array_buffer->data_at(sizeof(float))));
        EXPECT_EQ(span.size(), 2u);
        EXPECT_EQ(span[0], 2.0f);
        EXPECT_EQ(span[1], 3.0f);
    }));
    EXPECT(callback_was_invoked);
}

TEST_CASE(float32_vector_list_borrows_vector_storage)
{
    Web::WebGL::WebGLRenderingContextBase::Float32List list { Vector<float> { 1.0f, 2.0f, 3.0f, 4.0f } };
    auto* expected_data = list.get<Vector<float>>().data() + 1;

    MUST(WebGLRenderingContextBaseAccessor::with_float32_list(list, 1, 2, [&](ReadonlySpan<float> span) {
        EXPECT_EQ(span.data(), expected_data);
        EXPECT_EQ(span.size(), 2u);
        EXPECT_EQ(span[0], 2.0f);
        EXPECT_EQ(span[1], 3.0f);
    }));
}

TEST_CASE(out_of_bounds_float32_list_without_offset_is_empty)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Web::WebGL::WebGLRenderingContextBase::Float32List list { create_out_of_bounds_array<JS::Float32Array>(realm) };

    MUST(WebGLRenderingContextBaseAccessor::with_float32_list(list, 0, 0, [](ReadonlySpan<float> span) {
        EXPECT_EQ(span.size(), 0u);
    }));
    EXPECT(WebGLRenderingContextBaseAccessor::with_float32_list(list, 1, 0, [](ReadonlySpan<float>) { }).is_error());
    EXPECT(WebGLRenderingContextBaseAccessor::with_float32_list(list, 0, 1, [](ReadonlySpan<float>) { }).is_error());
}

TEST_CASE(out_of_bounds_int32_list_without_offset_is_empty)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Web::WebGL::WebGLRenderingContextBase::Int32List list { create_out_of_bounds_array<JS::Int32Array>(realm) };

    MUST(WebGLRenderingContextBaseAccessor::with_int32_list(list, 0, 0, [](ReadonlySpan<int> span) {
        EXPECT_EQ(span.size(), 0u);
    }));
    EXPECT(WebGLRenderingContextBaseAccessor::with_int32_list(list, 1, 0, [](ReadonlySpan<int>) { }).is_error());
    EXPECT(WebGLRenderingContextBaseAccessor::with_int32_list(list, 0, 1, [](ReadonlySpan<int>) { }).is_error());
}

TEST_CASE(out_of_bounds_uint32_list_without_offset_is_empty)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Web::WebGL::WebGLRenderingContextBase::Uint32List list { create_out_of_bounds_array<JS::Uint32Array>(realm) };

    MUST(WebGLRenderingContextBaseAccessor::with_uint32_list(list, 0, 0, [](ReadonlySpan<u32> span) {
        EXPECT_EQ(span.size(), 0u);
    }));
    EXPECT(WebGLRenderingContextBaseAccessor::with_uint32_list(list, 1, 0, [](ReadonlySpan<u32>) { }).is_error());
    EXPECT(WebGLRenderingContextBaseAccessor::with_uint32_list(list, 0, 1, [](ReadonlySpan<u32>) { }).is_error());
}

TEST_CASE(detached_float32_list_without_offset_is_empty)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, 4 * sizeof(float)));
    Web::WebGL::WebGLRenderingContextBase::Float32List list {
        JS::Float32Array::create(realm, 4, array_buffer)
    };
    MUST(JS::detach_array_buffer(realm.vm(), *array_buffer));

    MUST(WebGLRenderingContextBaseAccessor::with_float32_list(list, 0, 0, [](ReadonlySpan<float> span) {
        EXPECT_EQ(span.size(), 0u);
    }));
    EXPECT(WebGLRenderingContextBaseAccessor::with_float32_list(list, 1, 0, [](ReadonlySpan<float>) { }).is_error());
    EXPECT(WebGLRenderingContextBaseAccessor::with_float32_list(list, 0, 1, [](ReadonlySpan<float>) { }).is_error());
}

BENCHMARK_CASE(float32_typed_list_conversion)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Array values {
        1.0f,
        2.0f,
        3.0f,
        4.0f,
        5.0f,
        6.0f,
        7.0f,
        8.0f,
        9.0f,
        10.0f,
        11.0f,
        12.0f,
        13.0f,
        14.0f,
        15.0f,
        16.0f,
    };
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, values.size() * sizeof(float)));
    array_buffer->overwrite(0, values.data(), values.size() * sizeof(float));
    Web::WebGL::WebGLRenderingContextBase::Float32List list {
        JS::Float32Array::create(realm, values.size(), array_buffer)
    };

    float sum = 0;
    for (size_t iteration = 0; iteration < 1'000'000; ++iteration) {
        MUST(WebGLRenderingContextBaseAccessor::with_float32_list(list, 0, 0, [&](ReadonlySpan<float> span) {
            sum += span[iteration % values.size()];
        }));
    }
    EXPECT(sum > 0);
}
