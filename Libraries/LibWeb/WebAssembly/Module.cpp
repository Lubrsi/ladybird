/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ModulePrototype.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Module);

WebIDL::ExceptionOr<GC::Ref<Module>> Module::construct_impl(JS::Realm& realm, GC::Root<WebIDL::BufferSource>& bytes)
{
    auto& vm = realm.vm();

    auto stable_bytes_or_error = WebIDL::get_buffer_source_copy(bytes->raw_object());
    if (stable_bytes_or_error.is_error()) {
        VERIFY(stable_bytes_or_error.error().code() == ENOMEM);
        return vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
    }
    auto stable_bytes = stable_bytes_or_error.release_value();

    auto compiled_module = TRY(Detail::compile_a_webassembly_module(vm, move(stable_bytes)));
    return realm.create<Module>(realm, move(compiled_module));
}

Module::Module(JS::Realm& realm, NonnullRefPtr<Detail::CompiledWebAssemblyModule> compiled_module)
    : Bindings::PlatformObject(realm)
    , m_compiled_module(move(compiled_module))
{
}

void Module::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Module, WebAssembly.Module);
}

// 1. https://webassembly.github.io/spec/js-api/#dom-module-customsections
Vector<GC::Root<JS::ArrayBuffer>> Module::custom_sections(JS::VM& vm, GC::Ref<Module const> module, String const& section_name)
{
    auto& realm = *vm.current_realm();

    // 1. Let bytes be moduleObject.[[Bytes]].
    // 2. Let customSections be « ».
    Vector<GC::Root<JS::ArrayBuffer>> custom_sections;

    // 3. For each custom section customSection of bytes, interpreted according to the module grammar,
    auto decoder = TextCodec::decoder_for("UTF-8"sv);

    for (auto const& custom_section : module->compiled_module()->module->custom_sections()) {
        // 1. Let name be the name of customSection, decoded as UTF-8.
        // 2. Assert: name is not failure (moduleObject.[[Module]] is valid).
        auto name = MUST(decoder->to_utf8(custom_section.name()));

        // 3. If name equals sectionName as string values,
        if (name == section_name) {
            // 1. Append a new ArrayBuffer containing a copy of the bytes in bytes for the range matched by this
            //    customsec production to customSections.
            auto custom_section_bytes = MUST(ByteBuffer::copy(custom_section.contents()));
            auto custom_section_array_buffer = JS::ArrayBuffer::create(realm, move(custom_section_bytes));
            custom_sections.append(GC::make_root(custom_section_array_buffer));
        }
    }

    // 4. Return customSections.
    return custom_sections;
}

}
