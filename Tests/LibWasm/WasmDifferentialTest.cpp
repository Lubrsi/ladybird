/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WasmDifferentialTest.h"

#include <AK/MemoryStream.h>
#include <LibWasm/AbstractMachine/Validator.h>

namespace Wasm::TestSupport {

static FunctionAddress find_exported_function(ModuleInstance const& instance, StringView name)
{
    for (auto const& export_ : instance.exports()) {
        if (export_.name() == name) {
            VERIFY(export_.value().has<FunctionAddress>());
            return export_.value().get<FunctionAddress>();
        }
    }

    VERIFY_NOT_REACHED();
}

ExecutionSnapshot execute(ReadonlyBytes wasm, ExecutionMode mode, ReadonlySpan<Invocation> invocations)
{
    FixedMemoryStream stream { wasm };
    auto module = MUST(Module::parse(stream));

    AbstractMachine machine;
    auto compile_to_native = mode == ExecutionMode::Native ? CompileToNative::Yes : CompileToNative::No;
    MUST(machine.validate(*module, {}, compile_to_native));

    ExecutionSnapshot snapshot;
    snapshot.functions.ensure_capacity(module->code_section().functions().size());
    for (auto const& function : module->code_section().functions()) {
        auto const& compiled = function.func().body().compiled_instructions;
        snapshot.functions.unchecked_append({
            .eligible = compiled.cranelift_eligible,
            .compiled = compiled.cranelift_compiled,
            .has_entry = cranelift_entry_acquire(compiled) != 0,
        });
    }

    auto instance = MUST(machine.instantiate(*module, {}));
    snapshot.invocations.ensure_capacity(invocations.size());
    for (auto const& invocation : invocations) {
        Vector<Value> arguments;
        arguments.extend(invocation.arguments);
        auto result = machine.invoke(find_exported_function(*instance, invocation.export_name), move(arguments));

        InvocationOutcome outcome;
        if (result.is_trap()) {
            outcome.trap = result.trap().format();
        } else {
            outcome.values.ensure_capacity(result.values().size());
            for (auto const& value : result.values())
                outcome.values.unchecked_append(value.to<u128>());
        }
        snapshot.invocations.unchecked_append(move(outcome));
    }

    snapshot.memories.ensure_capacity(instance->memories().size());
    for (auto address : instance->memories()) {
        auto const* memory = machine.store().get(address);
        VERIFY(memory);
        Vector<u8> contents;
        auto bytes = memory->data().bytes();
        contents.append(bytes.data(), bytes.size());
        snapshot.memories.unchecked_append(move(contents));
    }

    snapshot.globals.ensure_capacity(instance->globals().size());
    for (auto address : instance->globals()) {
        auto const* global = machine.store().get(address);
        VERIFY(global);
        snapshot.globals.unchecked_append(global->value().to<u128>());
    }

    return snapshot;
}

static Optional<ByteString> compare_values(StringView category, size_t outer_index, ReadonlySpan<u128> lhs, ReadonlySpan<u128> rhs)
{
    if (lhs.size() != rhs.size())
        return ByteString::formatted("{} {} has {} values in the interpreter and {} in native code", category, outer_index, lhs.size(), rhs.size());

    for (size_t value_index = 0; value_index < lhs.size(); ++value_index) {
        if (lhs[value_index] != rhs[value_index]) {
            return ByteString::formatted(
                "{} {} value {} differs: interpreter={:016x}:{:016x}, native={:016x}:{:016x}",
                category,
                outer_index,
                value_index,
                lhs[value_index].high(),
                lhs[value_index].low(),
                rhs[value_index].high(),
                rhs[value_index].low());
        }
    }

    return {};
}

Optional<ByteString> find_first_difference(ExecutionSnapshot const& interpreter, ExecutionSnapshot const& native)
{
    if (interpreter.invocations.size() != native.invocations.size()) {
        return ByteString::formatted(
            "invocation count differs: interpreter={}, native={}",
            interpreter.invocations.size(),
            native.invocations.size());
    }

    for (size_t invocation_index = 0; invocation_index < interpreter.invocations.size(); ++invocation_index) {
        auto const& interpreter_outcome = interpreter.invocations[invocation_index];
        auto const& native_outcome = native.invocations[invocation_index];
        if (interpreter_outcome.trap != native_outcome.trap) {
            return ByteString::formatted(
                "invocation {} trap differs: interpreter='{}', native='{}'",
                invocation_index,
                interpreter_outcome.trap.value_or("<none>"),
                native_outcome.trap.value_or("<none>"));
        }
        if (auto difference = compare_values("invocation"sv, invocation_index, interpreter_outcome.values, native_outcome.values); difference.has_value())
            return difference;
    }

    if (interpreter.memories.size() != native.memories.size()) {
        return ByteString::formatted(
            "memory count differs: interpreter={}, native={}",
            interpreter.memories.size(),
            native.memories.size());
    }

    for (size_t memory_index = 0; memory_index < interpreter.memories.size(); ++memory_index) {
        auto const& interpreter_memory = interpreter.memories[memory_index];
        auto const& native_memory = native.memories[memory_index];
        if (interpreter_memory.size() != native_memory.size()) {
            return ByteString::formatted(
                "memory {} size differs: interpreter={}, native={}",
                memory_index,
                interpreter_memory.size(),
                native_memory.size());
        }
        for (size_t byte_index = 0; byte_index < interpreter_memory.size(); ++byte_index) {
            if (interpreter_memory[byte_index] != native_memory[byte_index]) {
                return ByteString::formatted(
                    "memory {} byte {} differs: interpreter={:#02x}, native={:#02x}",
                    memory_index,
                    byte_index,
                    interpreter_memory[byte_index],
                    native_memory[byte_index]);
            }
        }
    }

    if (auto difference = compare_values("global"sv, 0, interpreter.globals, native.globals); difference.has_value())
        return difference;

    return {};
}

}
