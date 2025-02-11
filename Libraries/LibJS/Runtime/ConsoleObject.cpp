/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2020, Emanuele Torre <torreemanuele6@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/ConsoleObjectPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ConsoleObject);

static GC::Ref<ConsoleObjectPrototype> create_console_prototype(Realm& realm)
{
    return realm.create<ConsoleObjectPrototype>(realm);
}

ConsoleObject::ConsoleObject(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, create_console_prototype(realm))
{
}

ConsoleObject::~ConsoleObject() = default;

void ConsoleObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_console);
}

void ConsoleObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    m_console = realm.create<Console>(realm);
    u8 attr = Attribute::Writable | Attribute::Enumerable | Attribute::Configurable;
    define_native_function(realm, vm.names.assert, assert_, 0, attr);
    define_native_function(realm, vm.names.clear, clear, 0, attr);
    define_native_function(realm, vm.names.debug, debug, 0, attr);
    define_native_function(realm, vm.names.error, error, 0, attr);
    define_native_function(realm, vm.names.info, info, 0, attr);
    define_native_function(realm, vm.names.log, log, 0, attr);
    define_native_function(realm, vm.names.table, table, 0, attr);
    define_native_function(realm, vm.names.trace, trace, 0, attr);
    define_native_function(realm, vm.names.warn, warn, 0, attr);
    define_native_function(realm, vm.names.dir, dir, 0, attr);
    define_native_function(realm, vm.names.dirxml, dirxml, 0, attr);
    define_native_function(realm, vm.names.count, count, 0, attr);
    define_native_function(realm, vm.names.countReset, count_reset, 0, attr);
    define_native_function(realm, vm.names.group, group, 0, attr);
    define_native_function(realm, vm.names.groupCollapsed, group_collapsed, 0, attr);
    define_native_function(realm, vm.names.groupEnd, group_end, 0, attr);
    define_native_function(realm, vm.names.time, time, 0, attr);
    define_native_function(realm, vm.names.timeLog, time_log, 0, attr);
    define_native_function(realm, vm.names.timeEnd, time_end, 0, attr);

    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "console"_string), Attribute::Configurable);
}

static void debug_console(VM& vm, StringView name)
{
    return;
    dbgln("{}", name);
    dbgln("== begin arguments");
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        dbgln("{}", vm.argument(i));
    }
    dbgln("== end arguments");
}

// 1.1.1. assert(condition, ...data), https://console.spec.whatwg.org/#assert
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::assert_)
{
    debug_console(vm, "assert"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().assert_();
}

// 1.1.2. clear(), https://console.spec.whatwg.org/#clear
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::clear)
{
    debug_console(vm, "clear"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().clear();
}

// 1.1.3. debug(...data), https://console.spec.whatwg.org/#debug
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::debug)
{
    debug_console(vm, "debug"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().debug();
}

// 1.1.4. error(...data), https://console.spec.whatwg.org/#error
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::error)
{
    debug_console(vm, "error"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().error();
}

// 1.1.5. info(...data), https://console.spec.whatwg.org/#info
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::info)
{
    debug_console(vm, "info"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().info();
}

// 1.1.6. log(...data), https://console.spec.whatwg.org/#log
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::log)
{
    debug_console(vm, "log"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().log();
}

// 1.1.7. table(tabularData, properties), https://console.spec.whatwg.org/#table
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::table)
{
    debug_console(vm, "table"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().table();
}

// 1.1.8. trace(...data), https://console.spec.whatwg.org/#trace
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::trace)
{
    debug_console(vm, "trace"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().trace();
}

// 1.1.9. warn(...data), https://console.spec.whatwg.org/#warn
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::warn)
{
    debug_console(vm, "warn"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().warn();
}

// 1.1.10. dir(item, options), https://console.spec.whatwg.org/#warn
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::dir)
{
    debug_console(vm, "dir"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().dir();
}

JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::dirxml)
{
    debug_console(vm, "dirxml"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().dirxml();
}

// 1.2.1. count(label), https://console.spec.whatwg.org/#count
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::count)
{
    debug_console(vm, "count"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().count();
}

// 1.2.2. countReset(label), https://console.spec.whatwg.org/#countreset
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::count_reset)
{
    debug_console(vm, "count_reset"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().count_reset();
}

// 1.3.1. group(...data), https://console.spec.whatwg.org/#group
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::group)
{
    debug_console(vm, "group"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().group();
}

// 1.3.2. groupCollapsed(...data), https://console.spec.whatwg.org/#groupcollapsed
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::group_collapsed)
{
    debug_console(vm, "group_collapsed"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().group_collapsed();
}

// 1.3.3. groupEnd(), https://console.spec.whatwg.org/#groupend
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::group_end)
{
    debug_console(vm, "group_end"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().group_end();
}

// 1.4.1. time(label), https://console.spec.whatwg.org/#time
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::time)
{
    debug_console(vm, "time"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().time();
}

// 1.4.2. timeLog(label, ...data), https://console.spec.whatwg.org/#timelog
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::time_log)
{
    debug_console(vm, "time_log"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().time_log();
}

// 1.4.3. timeEnd(label), https://console.spec.whatwg.org/#timeend
JS_DEFINE_NATIVE_FUNCTION(ConsoleObject::time_end)
{
    debug_console(vm, "time_end"sv);
    auto& console_object = *vm.current_realm()->intrinsics().console_object();
    return console_object.console().time_end();
}

}
