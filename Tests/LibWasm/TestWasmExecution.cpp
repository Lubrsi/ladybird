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
