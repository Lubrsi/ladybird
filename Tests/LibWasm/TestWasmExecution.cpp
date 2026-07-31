/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibCore/File.h>
#include <LibTest/TestCase.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWasm/Constants.h>

TEST_CASE(compiled_to_interpreter_call_restores_label_stack)
{
    auto file = MUST(Core::File::open("Fixtures/label-stack-cleanup.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    Wasm::FunctionType label_stack_size_type { {}, { Wasm::ValueType(Wasm::ValueType::I32) } };
    auto label_stack_size = machine.store().allocate(Wasm::HostFunction {
        [](Wasm::Configuration& configuration, Span<Wasm::Value>) -> Wasm::Result {
            Vector<Wasm::Value> result;
            result.append(Wasm::Value(static_cast<i32>(configuration.label_stack().size())));
            return Wasm::Result { move(result) };
        },
        label_stack_size_type,
        "label_stack_size" });
    VERIFY(label_stack_size.has_value());

    Vector<Wasm::ExternValue> imports;
    imports.append(*label_stack_size);
    auto instance = MUST(machine.instantiate(*module, move(imports)));

    EXPECT(!module->code_section().functions()[0].func().body().compiled_instructions.cranelift_compiled);
    EXPECT(module->code_section().functions()[1].func().body().compiled_instructions.cranelift_compiled);

    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, {});
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 1);
}

TEST_CASE(reentrant_invoke_uses_independent_execution_state)
{
    auto file = MUST(Core::File::open("Fixtures/label-stack-cleanup.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    Optional<Wasm::FunctionAddress> run;
    bool should_reenter { true };
    Wasm::FunctionType label_stack_size_type { {}, { Wasm::ValueType(Wasm::ValueType::I32) } };
    auto label_stack_size = machine.store().allocate(Wasm::HostFunction {
        [&](Wasm::Configuration& configuration, Span<Wasm::Value>) -> Wasm::Result {
            if (should_reenter) {
                should_reenter = false;
                return machine.invoke(*run, {});
            }

            Vector<Wasm::Value> result;
            result.append(Wasm::Value(static_cast<i32>(configuration.label_stack().size())));
            return Wasm::Result { move(result) };
        },
        label_stack_size_type,
        "label_stack_size" });
    VERIFY(label_stack_size.has_value());

    Vector<Wasm::ExternValue> imports;
    imports.append(*label_stack_size);
    auto instance = MUST(machine.instantiate(*module, move(imports)));

    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, {});
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 1);
}

TEST_CASE(reentrant_compiled_memory_fault_uses_innermost_recovery_context)
{
    auto parse_fixture = [](StringView path) {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
        auto bytes = MUST(file->read_until_eof());
        FixedMemoryStream stream { bytes.bytes() };
        return MUST(Wasm::Module::parse(stream));
    };

    Wasm::AbstractMachine machine;

    auto inner_module = parse_fixture("Fixtures/memory-guard-trap.wasm"sv);
    auto inner_instance = MUST(machine.instantiate(*inner_module, {}));
    Optional<Wasm::FunctionAddress> load_high;
    for (auto const& export_ : inner_instance->exports()) {
        if (export_.name() == "load_high"sv)
            load_high = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(load_high.has_value());

    Wasm::FunctionType host_type { {}, { Wasm::ValueType(Wasm::ValueType::I32) } };
    auto call_inner = machine.store().allocate(Wasm::HostFunction {
        [&](Wasm::Configuration&, Span<Wasm::Value>) -> Wasm::Result {
            return machine.invoke(*load_high, { Wasm::Value(static_cast<i32>(0)) });
        },
        host_type,
        "call_inner" });
    VERIFY(call_inner.has_value());

    auto outer_module = parse_fixture("Fixtures/label-stack-cleanup.wasm"sv);
    Vector<Wasm::ExternValue> imports;
    imports.append(*call_inner);
    auto outer_instance = MUST(machine.instantiate(*outer_module, move(imports)));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : outer_instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, {});
    EXPECT(result.is_trap());
    EXPECT_EQ(result.trap().format(), "Memory access out of bounds"sv);
}

TEST_CASE(compiled_indirect_call_preserves_arguments_and_result)
{
    auto file = MUST(Core::File::open("Fixtures/indirect-call.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    auto instance = MUST(machine.instantiate(*module, {}));

    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, { Wasm::Value(static_cast<i32>(41)) });
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 42);
}

TEST_CASE(compiled_memory_access_traps_out_of_bounds)
{
    auto file = MUST(Core::File::open("Fixtures/memory-guard-trap.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    auto instance = MUST(machine.instantiate(*module, {}));

    auto find_export = [&](StringView name) {
        Optional<Wasm::FunctionAddress> address;
        for (auto const& export_ : instance->exports()) {
            if (export_.name() == name)
                address = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(address.has_value());
        return *address;
    };
    auto load = find_export("load"sv);
    auto store = find_export("store"sv);
    auto load_high = find_export("load_high"sv);

    auto invoke = [&](Wasm::FunctionAddress address, Vector<Wasm::Value> arguments) {
        return machine.invoke(address, move(arguments));
    };

    // The memory starts at one page; accesses fully inside it should succeed.
    auto in_bounds = invoke(load, { Wasm::Value(static_cast<i32>(0)) });
    EXPECT(!in_bounds.is_trap());
    EXPECT_EQ(in_bounds.values()[0].to<i32>(), 0);

    auto store_in_bounds = invoke(store, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size - 4)), Wasm::Value(static_cast<i32>(0x1337)) });
    EXPECT(!store_in_bounds.is_trap());
    auto load_back = invoke(load, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size - 4)) });
    EXPECT(!load_back.is_trap());
    EXPECT_EQ(load_back.values()[0].to<i32>(), 0x1337);

    auto expect_oob_trap = [](Wasm::Result const& result) {
        EXPECT(result.is_trap());
        EXPECT_EQ(result.trap().format(), "Memory access out of bounds"sv);
    };

    // Loads and stores just-or-partially past the boundary should trap.
    expect_oob_trap(invoke(load, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size - 1)) }));
    expect_oob_trap(invoke(load, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size)) }));
    // A store just past the committed boundary must not write outside the memory either.
    expect_oob_trap(invoke(store, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size)), Wasm::Value(static_cast<i32>(0x42)) }));

    // base + offset can reach high into the guarded reservation; those accesses must trap too.
    expect_oob_trap(invoke(load_high, { Wasm::Value(static_cast<i32>(0)) }));
    expect_oob_trap(invoke(load_high, { Wasm::Value(static_cast<i32>(0xffffffff)) }));
}

TEST_CASE(call_record_reserves_forward_callee_inlined_locals)
{
    auto file = MUST(Core::File::open("Fixtures/call-record-forward-inlined-locals.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, {}, Wasm::CompileToNative::No));

    auto const& functions = module->code_section().functions();
    auto const& caller = functions[1].func().body().compiled_instructions;
    auto const& callee = functions[2].func().body().compiled_instructions;

    EXPECT(callee.cranelift_inlined_locals > 0);
    EXPECT(caller.max_call_rec_size >= 4 + callee.cranelift_inlined_locals);
}

TEST_CASE(native_direct_call_uses_call_record)
{
    auto file = MUST(Core::File::open("Fixtures/call-record-forward-inlined-locals.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));

    auto const& functions = module->code_section().functions();
    auto const& caller = functions[1].func().body().compiled_instructions;
    auto const& callee = functions[2].func().body().compiled_instructions;
    EXPECT(caller.cranelift_compiled);
    EXPECT(callee.cranelift_compiled);

    auto instance = MUST(machine.instantiate(*module, {}));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, {});
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 1);
}

TEST_CASE(native_direct_call_falls_back_for_imported_callee)
{
    auto file = MUST(Core::File::open("Fixtures/call-record-import-fallback.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));

    auto const& caller = module->code_section().functions()[0].func().body().compiled_instructions;
    EXPECT(caller.cranelift_compiled);

    Wasm::FunctionType sum_type {
        { Wasm::ValueType(Wasm::ValueType::I32), Wasm::ValueType(Wasm::ValueType::I32), Wasm::ValueType(Wasm::ValueType::I32), Wasm::ValueType(Wasm::ValueType::I32) },
        { Wasm::ValueType(Wasm::ValueType::I32) }
    };
    auto sum = machine.store().allocate(Wasm::HostFunction {
        [](Wasm::Configuration&, Span<Wasm::Value> arguments) -> Wasm::Result {
            i32 result = 0;
            for (auto const& argument : arguments)
                result += argument.to<i32>();
            return Wasm::Result { Vector<Wasm::Value> { Wasm::Value(result) } };
        },
        sum_type,
        "sum4" });
    VERIFY(sum.has_value());

    auto instance = MUST(machine.instantiate(*module, { *sum }));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, {});
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 10);
}

TEST_CASE(native_direct_call_restores_context_after_trap)
{
    auto file = MUST(Core::File::open("Fixtures/call-record-native-trap.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));

    for (auto const& function : module->code_section().functions())
        EXPECT(function.func().body().compiled_instructions.cranelift_compiled);

    auto instance = MUST(machine.instantiate(*module, {}));
    auto find_export = [&](StringView name) {
        Optional<Wasm::FunctionAddress> address;
        for (auto const& export_ : instance->exports()) {
            if (export_.name() == name)
                address = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(address.has_value());
        return *address;
    };

    auto trapped = machine.invoke(find_export("trap"sv), {});
    EXPECT(trapped.is_trap());
    EXPECT_EQ(trapped.trap().format(), "Integer division overflow"sv);

    auto recovered = machine.invoke(find_export("recover"sv), {});
    EXPECT(!recovered.is_trap());
    EXPECT_EQ(recovered.values().size(), 1u);
    EXPECT_EQ(recovered.values()[0].to<i32>(), 4);
}

TEST_CASE(native_direct_call_uses_typed_abi)
{
    auto file = MUST(Core::File::open("Fixtures/native-call-abi.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));

    auto const& functions = module->code_section().functions();
    EXPECT_EQ(functions.size(), 19u);
    for (size_t index = 0; index < 4; ++index)
        EXPECT(functions[index].func().body().compiled_instructions.cranelift_compiled);
    EXPECT(!functions[4].func().body().compiled_instructions.cranelift_compiled);
    EXPECT(!functions[5].func().body().compiled_instructions.cranelift_compiled);
    EXPECT(!functions[6].func().body().compiled_instructions.cranelift_compiled);
    for (size_t index = 7; index < functions.size(); ++index)
        EXPECT(functions[index].func().body().compiled_instructions.cranelift_compiled);

    auto const& sum4_compiled = functions[17].func().body().compiled_instructions;
    EXPECT_NE(Wasm::cranelift_native_entry_acquire(sum4_compiled), 0u);
    EXPECT_NE(Wasm::cranelift_native_entry_acquire(sum4_compiled), Wasm::cranelift_entry_acquire(sum4_compiled));

    Vector<Wasm::FunctionIndex> raw_caller_indices;
    for (auto const& export_ : module->export_section().entries()) {
        if (export_.name().starts_with("run_raw_"sv))
            raw_caller_indices.append(export_.description().get<Wasm::FunctionIndex>());
    }
    EXPECT_EQ(raw_caller_indices.size(), 2u);
    for (auto function_index : raw_caller_indices) {
        EXPECT(!functions[function_index.value()].func().body().compiled_instructions.cranelift_raw_calls.is_empty());
    }

    auto instance = MUST(machine.instantiate(*module, {}));
    auto invoke = [&](StringView name) {
        Optional<Wasm::FunctionAddress> address;
        for (auto const& export_ : instance->exports()) {
            if (export_.name() == name)
                address = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(address.has_value());
        auto result = machine.invoke(*address, {});
        EXPECT(!result.is_trap());
        EXPECT_EQ(result.values().size(), 1u);
        return result.values().take_first();
    };

    EXPECT_EQ(invoke("run_i32"sv).to<i32>(), 17);
    EXPECT_EQ(invoke("run_i64"sv).to<i64>(), 9999999997);
    EXPECT_EQ(invoke("run_f32"sv).to<float>(), 5.25f);
    EXPECT_EQ(invoke("run_f64"sv).to<double>(), 8.25);
    EXPECT_EQ(invoke("run_fallback_f64"sv).to<double>(), 9.25);
    EXPECT_EQ(invoke("run_raw_i32"sv).to<i32>(), 136);
    EXPECT_EQ(invoke("run_memory_i32"sv).to<i32>(), 10);
    EXPECT_EQ(invoke("run_memory_fallback_i32"sv).to<i32>(), 10);
    EXPECT_EQ(invoke("run_raw_memory_i32"sv).to<i32>(), 36);

    Optional<Wasm::FunctionAddress> sum4_address;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "sum4_i32"sv)
            sum4_address = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(sum4_address.has_value());
    Vector<Wasm::Value> sum4_arguments {
        Wasm::Value(static_cast<i32>(1)),
        Wasm::Value(static_cast<i32>(2)),
        Wasm::Value(static_cast<i32>(3)),
        Wasm::Value(static_cast<i32>(4)),
    };
    auto sum4_result = machine.invoke(*sum4_address, move(sum4_arguments));
    EXPECT(!sum4_result.is_trap());
    EXPECT_EQ(sum4_result.values().size(), 1u);
    EXPECT_EQ(sum4_result.values()[0].to<i32>(), 10);

    Optional<Wasm::FunctionAddress> trap_address;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run_fallback_trap"sv)
            trap_address = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(trap_address.has_value());
    auto trapped = machine.invoke(*trap_address, {});
    EXPECT(trapped.is_trap());
    EXPECT_EQ(trapped.trap().format(), "Unreachable"sv);
}

TEST_CASE(native_indirect_call_uses_typed_abi)
{
    auto file = MUST(Core::File::open("Fixtures/native-call-indirect-abi.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));

    auto const& functions = module->code_section().functions();
    EXPECT_EQ(functions.size(), 38u);
    Optional<Wasm::FunctionIndex> fallback_target_index;
    Vector<Wasm::FunctionIndex> raw_indirect_caller_indices;
    for (auto const& export_ : module->export_section().entries()) {
        if (export_.name() == "target_fallback_i32"sv)
            fallback_target_index = export_.description().get<Wasm::FunctionIndex>();
        if (export_.name().starts_with("run_nested_raw_"sv))
            raw_indirect_caller_indices.append(export_.description().get<Wasm::FunctionIndex>());
        if (!export_.name().starts_with("run_"sv))
            continue;
        auto function_index = export_.description().get<Wasm::FunctionIndex>().value();
        auto const& compiled = functions[function_index].func().body().compiled_instructions;
        EXPECT(compiled.cranelift_compiled);
        auto const expected_indirect_calls = export_.name().starts_with("run_nested_raw_"sv) ? 2u : 1u;
        EXPECT_EQ(compiled.cranelift_indirect_calls.size(), expected_indirect_calls);
    }
    VERIFY(fallback_target_index.has_value());
    EXPECT(!functions[fallback_target_index->value()].func().body().compiled_instructions.cranelift_compiled);
    EXPECT_EQ(raw_indirect_caller_indices.size(), 6u);
    for (auto function_index : raw_indirect_caller_indices) {
        auto const& raw_indirect_caller = functions[function_index.value()].func().body().compiled_instructions;
        bool has_raw_call_indirect = false;
        for (auto const& dispatch : raw_indirect_caller.dispatches) {
            if (dispatch.instruction->opcode() == Wasm::Instructions::call_indirect)
                has_raw_call_indirect = true;
        }
        EXPECT(has_raw_call_indirect);
    }

    auto instance = MUST(machine.instantiate(*module, {}));
    auto find_export = [&](StringView name) {
        Optional<Wasm::FunctionAddress> address;
        for (auto const& export_ : instance->exports()) {
            if (export_.name() == name)
                address = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(address.has_value());
        return *address;
    };
    auto invoke = [&](StringView name) {
        return machine.invoke(find_export(name), {});
    };
    auto invoke_value = [&](StringView name) {
        auto result = invoke(name);
        EXPECT(!result.is_trap());
        EXPECT_EQ(result.values().size(), 1u);
        return result.values().take_first();
    };

    EXPECT_EQ(invoke_value("run_i32"sv).to<i32>(), 17);
    EXPECT_EQ(invoke_value("run_i64"sv).to<i64>(), 9999999997);
    EXPECT_EQ(invoke_value("run_f32"sv).to<float>(), 5.25f);
    EXPECT_EQ(invoke_value("run_f64"sv).to<double>(), 8.25);
    EXPECT_EQ(invoke_value("run_sum9_i32"sv).to<i32>(), 10);
    EXPECT_EQ(invoke_value("run_fallback_i32"sv).to<i32>(), 42);
    EXPECT_EQ(invoke_value("run_mixed_i32_f32"sv).to<i32>(), 23);
    EXPECT_EQ(invoke_value("run_mixed_i32_i64_i32"sv).to<i32>(), 42);
    EXPECT_EQ(invoke_value("run_mixed_six"sv).to<i32>(), 21);
    EXPECT_EQ(invoke_value("run_mixed_f32_result"sv).to<float>(), 15.75f);
    EXPECT_EQ(invoke_value("run_mixed_f64"sv).to<i32>(), 28);
    EXPECT_EQ(invoke_value("run_br_table_then_indirect"sv).to<i32>(), 17);
    EXPECT_EQ(invoke_value("run_nested_raw_i32"sv).to<i32>(), 83);
    EXPECT_EQ(invoke_value("run_nested_raw_fallback_i32"sv).to<i32>(), 90);
    EXPECT_EQ(invoke_value("run_nested_raw_i64"sv).to<i64>(), 9999999983);
    EXPECT_EQ(invoke_value("run_nested_raw_f32"sv).to<float>(), 94.75f);
    EXPECT_EQ(invoke_value("run_nested_raw_f64"sv).to<double>(), 91.75);

    auto raw_void_result = invoke("run_nested_raw_void"sv);
    EXPECT(!raw_void_result.is_trap());
    if (!raw_void_result.is_trap()) {
        EXPECT_EQ(raw_void_result.values().size(), 1u);
        EXPECT_EQ(raw_void_result.values()[0].to<i32>(), 17);
    }

    Optional<Wasm::TableAddress> table_address;
    Optional<Wasm::FunctionAddress> replacement_address;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "table"sv)
            table_address = export_.value().get<Wasm::TableAddress>();
        if (export_.name() == "target_add_i32"sv)
            replacement_address = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(table_address.has_value());
    VERIFY(replacement_address.has_value());
    auto* table = machine.store().get(*table_address);
    VERIFY(table);
    table->set_element(machine.store(), 0, Wasm::Reference { Wasm::Reference::Func { *replacement_address, nullptr } });
    EXPECT_EQ(invoke_value("run_i32"sv).to<i32>(), 23);

    auto void_result = invoke("run_void"sv);
    EXPECT(!void_result.is_trap());
    EXPECT(void_result.values().is_empty());

    auto expect_trap = [&](StringView name, StringView message) {
        auto result = invoke(name);
        EXPECT(result.is_trap());
        EXPECT_EQ(result.trap().format(), message);
    };
    expect_trap("run_trap"sv, "unreachable executed"sv);
    expect_trap("run_type_mismatch"sv, "Indirect call type mismatch"sv);
    expect_trap("run_null"sv, "Table element is not a function reference"sv);
    expect_trap("run_out_of_bounds"sv, "Table index out of bounds"sv);
}

TEST_CASE(native_indirect_call_restores_context_after_cross_module_fallback)
{
    auto parse_module = [](StringView path) {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
        auto bytes = MUST(file->read_until_eof());
        FixedMemoryStream stream { bytes.bytes() };
        return MUST(Wasm::Module::parse(stream));
    };

    auto provider_module = parse_module("Fixtures/native-call-indirect-context-provider.wasm"sv);
    auto caller_module = parse_module("Fixtures/native-call-indirect-context-caller.wasm"sv);

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*provider_module));
    MUST(machine.validate(*caller_module));

    EXPECT(provider_module->code_section().functions()[0].func().body().compiled_instructions.cranelift_compiled);
    for (auto const& function : caller_module->code_section().functions())
        EXPECT(function.func().body().compiled_instructions.cranelift_compiled);

    auto provider_instance = MUST(machine.instantiate(*provider_module, {}));
    Optional<Wasm::FunctionAddress> provider_value;
    for (auto const& export_ : provider_instance->exports()) {
        if (export_.name() == "value"sv)
            provider_value = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(provider_value.has_value());

    auto caller_instance = MUST(machine.instantiate(*caller_module, { *provider_value }));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : caller_instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, {});
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 42);
}
