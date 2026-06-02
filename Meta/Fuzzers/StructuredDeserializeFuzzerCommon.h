/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>

namespace StructuredDeserializeFuzzer {

inline JS::Realm& realm()
{
    // Build lazily after LibGC's type-isolated allocators exist.
    static struct Globals {
        Globals() { (void)Web::Bindings::create_a_principal_javascript_realm(); }
    } globals;

    return *Web::Bindings::main_thread_vm().current_realm();
}

inline void collect_garbage()
{
    // Collect between inputs so only real leaks survive to exit.
    Web::Bindings::main_thread_vm().heap().collect_garbage();
}

}
