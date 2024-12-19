/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
 
#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/GlobalPrototype.h>

namespace Web::WebAssembly {

struct GlobalDescriptor {
    Bindings::ValueType value;
    bool mutable_ { false };
};

class Global : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Global, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Global);

public:
    static WebIDL::ExceptionOr<GC::Ref<Global>> construct_impl(JS::Realm&, GlobalDescriptor& descriptor, JS::Value value);

    virtual ~Global() override;

    JS::ThrowCompletionOr<JS::Value> value() const;
    JS::ThrowCompletionOr<void> set_value(JS::Value);

    JS::ThrowCompletionOr<JS::Value> value_of() const;

    Wasm::GlobalAddress address() const { return m_address; }

private:
    Global(JS::Realm&, Wasm::GlobalAddress);

    virtual void initialize(JS::Realm&) override;

    JS::ThrowCompletionOr<JS::Value> get_global_value() const;

    Wasm::GlobalAddress m_address;
};

}
