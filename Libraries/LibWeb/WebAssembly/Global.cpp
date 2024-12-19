/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Global);

// https://webassembly.github.io/spec/js-api/#tovaluetype
static Wasm::ValueType bindings_value_type_to_wasm_value_type(Bindings::ValueType value_type)
{
    switch (value_type) {
    case Bindings::ValueType::I32:
        // 1. If s equals "i32", return i32.
        return Wasm::ValueType { Wasm::ValueType::I32 };
    case Bindings::ValueType::I64:
        // 2. If s equals "i64", return i64.
        return Wasm::ValueType { Wasm::ValueType::I64 };
    case Bindings::ValueType::F32:
        // 3. If s equals "f32", return f32.
        return Wasm::ValueType { Wasm::ValueType::F32 };
    case Bindings::ValueType::F64:
        // 4. If s equals "f64", return f64.
        return Wasm::ValueType { Wasm::ValueType::F64 };
    case Bindings::ValueType::V128:
        // 5. If s equals "v128", return v128.
        return Wasm::ValueType { Wasm::ValueType::V128 };
    case Bindings::ValueType::Anyfunc:
        // 6. If s equals "anyfunc", return funcref.
        return Wasm::ValueType { Wasm::ValueType::FunctionReference };
    case Bindings::ValueType::Externref:
        // 7. If s equals "externref", return externref.
        return Wasm::ValueType { Wasm::ValueType::ExternReference };
    }

    // 8. Assert: This step is not reached.
    VERIFY_NOT_REACHED();
}


WebIDL::ExceptionOr<GC::Ref<Global>> Global::construct_impl(JS::Realm& realm, GlobalDescriptor& descriptor, JS::Value value)
{
    auto& vm = realm.vm();

    // 1. Let mutable be descriptor["mutable"].
    auto mutable_ = descriptor.mutable_;

    // 2. Let valuetype be ToValueType(descriptor["value"]).
    auto value_type = bindings_value_type_to_wasm_value_type(descriptor.value);

    // 3. If valuetype is v128,
    if (value_type.kind() == Wasm::ValueType::V128) {
        // 1. Throw a TypeError exception.
        return vm.throw_completion<JS::TypeError>("Cannot create a global vector variable"sv);
    }

    // 4. If v is missing,
    //    1. Let value be DefaultValue(valuetype).Otherwise,
    // 5. Otherwise,
    //    1. Let value be ToWebAssemblyValue(v, valuetype).
    auto global_value = vm.argument_count() == 1
        ? Detail::default_webassembly_value(vm, value_type)
        : TRY(Detail::to_webassembly_value(vm, value, value_type));

    // 6. If mutable is true, let globaltype be var valuetype; otherwise, let globaltype be const valuetype.
    Wasm::GlobalType global_type { value_type, mutable_ };

    // 7. Let store be the current agent’s associated store.
    auto& cache = Detail::get_cache(realm);

    // 8. Let (store, globaladdr) be global_alloc(store, globaltype, value).
    // 9. Set the current agent’s associated store to store.
    auto address = cache.abstract_machine().store().allocate(global_type, global_value);
    if (!address.has_value())
        return vm.throw_completion<JS::TypeError>("Wasm Global allocation failed"sv);

    // dbgln("allocated {}", address.value());

    // 10. Initialize this from globaladdr.
    return realm.create<Global>(realm, *address);
}

Global::Global(JS::Realm& realm, Wasm::GlobalAddress address)
    : Bindings::PlatformObject(realm)
    , m_address(address)
{
}

Global::~Global() = default;

void Global::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Global, WebAssembly.Global);
}

JS::ThrowCompletionOr<JS::Value> Global::get_global_value() const
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let store be the current agent’s associated store.
    auto& cache = Detail::get_cache(realm);

    // 2. Let globaladdr be global.[[Global]].
    auto global_address = m_address;

    // 3. Let globaltype be global_type(store, globaladdr).
    auto* global_instance = cache.abstract_machine().store().get(global_address);

    // 4. If globaltype is of the form mut v128, throw a TypeError.
    if (global_instance->is_mutable() && global_instance->type().type().kind() == Wasm::ValueType::V128)
        return vm.throw_completion<JS::TypeError>("Cannot read mutable vector types"sv);

    // 5. Let value be global_read(store, globaladdr).
    // 6. Return ToJSValue(value).
    // dbgln("getting {}", global_address);
    auto result = Detail::to_js_value(vm, global_instance->value(), global_instance->type().type());
    // dbgln("result was {}", result);
    return result;
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
JS::ThrowCompletionOr<JS::Value> Global::value() const
{
    // 1. Return GetGlobalValue(this).
    return get_global_value();
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
JS::ThrowCompletionOr<void> Global::set_value(JS::Value value)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let store be the current agent’s associated store.
    auto& cache = Detail::get_cache(realm);

    // 2. Let globaladdr be this.[[Global]].
    auto global_address = m_address;

    // 3. Let mut valuetype be global_type(store, globaladdr).
    auto* value_type = cache.abstract_machine().store().get(global_address);

    // 4. If valuetype is v128, throw a TypeError.
    if (value_type->type().type().kind() == Wasm::ValueType::V128)
        return vm.throw_completion<JS::TypeError>("Cannot create a global vector variable"sv);

    // 5. If mut is const, throw a TypeError.
    if (!value_type->is_mutable())
        return vm.throw_completion<JS::TypeError>("Cannot assign to constant global variable"sv);

    // dbgln("setting {} to {}", global_address, value);

    // 6. Let value be ToWebAssemblyValue(the given value, valuetype).
    auto wasm_value = TRY(Detail::to_webassembly_value(vm, value, value_type->type().type()));

    // 7. Let store be global_write(store, globaladdr, value).
    value_type->set_value(wasm_value);

    // FIXME: 8. If store is error, throw a RangeError exception.
    //        This in unreachable as set_value does not return any error. The only case where global_write can return
    //        an error is if the global value is immutable, but that has already been protected against above.

    // 9. Set the current agent’s associated store to store.
    // NOTE: No-op.
    return {};
}

// https://webassembly.github.io/spec/js-api/#dom-global-valueof
JS::ThrowCompletionOr<JS::Value> Global::value_of() const
{
    // 1. Return GetGlobalValue(this).
    return get_global_value();
}

}
