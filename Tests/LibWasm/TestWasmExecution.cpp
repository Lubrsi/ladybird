/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <LibCore/File.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWasm/Constants.h>
#include <stdlib.h>
#include <string.h>

static void append_unsigned_leb128(Vector<u8>& output, u32 value)
{
    do {
        auto byte = static_cast<u8>(value & 0x7f);
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        output.append(byte);
    } while (value != 0);
}

static void append_wasm_section(Vector<u8>& module, u8 section_id, Vector<u8>&& contents)
{
    module.append(section_id);
    append_unsigned_leb128(module, contents.size());
    module.extend(move(contents));
}

static Vector<u8> make_direct_call_chain_module(u32 function_count, Optional<u32> interpreted_function = {})
{
    VERIFY(function_count > 0);
    VERIFY(!interpreted_function.has_value() || interpreted_function.value() < function_count);

    Vector<u8> module { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };

    Vector<u8> type_section { 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f };
    append_wasm_section(module, 1, move(type_section));

    Vector<u8> function_section;
    append_unsigned_leb128(function_section, function_count);
    for (u32 function_index = 0; function_index < function_count; ++function_index)
        function_section.append(0);
    append_wasm_section(module, 3, move(function_section));

    if (interpreted_function.has_value()) {
        Vector<u8> memory_section { 0x01, 0x00, 0x01 };
        append_wasm_section(module, 5, move(memory_section));

        Vector<u8> global_section { 0x01, 0x70, 0x00, 0xd0, 0x70, 0x0b };
        append_wasm_section(module, 6, move(global_section));
    }

    Vector<u8> export_section { 0x01, 0x03, 'r', 'u', 'n', 0x00, 0x00 };
    append_wasm_section(module, 7, move(export_section));

    Vector<u8> code_section;
    append_unsigned_leb128(code_section, function_count);
    for (u32 function_index = 0; function_index < function_count; ++function_index) {
        Vector<u8> body { 0x00 };
        if (interpreted_function == function_index) {
            // global.get 0; drop
            // Reference-valued globals are not supported by the direct frontend yet, so the
            // fresh-compilation retirement policy leaves this function interpreted.
            body.extend(Vector<u8> { 0x23, 0x00, 0x1a });
        }
        if (function_index + 1 == function_count) {
            body.extend(Vector<u8> { 0x20, 0x00, 0x41, 0x01, 0x6a });
        } else if (function_index == 0) {
            body.extend(Vector<u8> { 0x20, 0x00 });
            body.append(0x10);
            append_unsigned_leb128(body, 1);
        } else {
            body.extend(Vector<u8> { 0x20, 0x00, 0x04, 0x7f, 0x20, 0x00, 0x10 });
            append_unsigned_leb128(body, function_index + 1);
            body.extend(Vector<u8> { 0x05, 0x41, 0x01, 0x0b });
        }
        body.append(0x0b);

        append_unsigned_leb128(code_section, body.size());
        code_section.extend(move(body));
    }
    append_wasm_section(module, 10, move(code_section));

    return module;
}

static Vector<u8> make_mutually_recursive_module()
{
    Vector<u8> module { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };

    Vector<u8> type_section { 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f };
    append_wasm_section(module, 1, move(type_section));

    Vector<u8> function_section { 0x02, 0x00, 0x00 };
    append_wasm_section(module, 3, move(function_section));

    Vector<u8> export_section { 0x01, 0x03, 'r', 'u', 'n', 0x00, 0x00 };
    append_wasm_section(module, 7, move(export_section));

    Vector<u8> code_section { 0x02 };
    for (u32 function_index = 0; function_index < 2; ++function_index) {
        Vector<u8> body {
            0x00,
            0x20,
            0x00,
            0x45,
            0x04,
            0x7f,
            0x41,
            0x00,
            0x05,
            0x20,
            0x00,
            0x41,
            0x01,
            0x6b,
            0x10,
            static_cast<u8>(1 - function_index),
            0x0b,
            0x0b,
        };
        append_unsigned_leb128(code_section, body.size());
        code_section.extend(move(body));
    }
    append_wasm_section(module, 10, move(code_section));

    return module;
}

static Vector<u8> make_call_indirect_callee_module()
{
    Vector<u8> module { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };

    Vector<u8> type_section { 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f };
    append_wasm_section(module, 1, move(type_section));

    Vector<u8> function_section { 0x02, 0x00, 0x00 };
    append_wasm_section(module, 3, move(function_section));

    Vector<u8> table_section { 0x01, 0x70, 0x00, 0x01 };
    append_wasm_section(module, 4, move(table_section));

    Vector<u8> export_section { 0x01, 0x03, 'r', 'u', 'n', 0x00, 0x00 };
    append_wasm_section(module, 7, move(export_section));

    Vector<u8> element_section { 0x01, 0x00, 0x41, 0x00, 0x0b, 0x01, 0x01 };
    append_wasm_section(module, 9, move(element_section));

    Vector<u8> code_section { 0x02 };
    Vector<u8> caller { 0x00, 0x20, 0x00, 0x41, 0x00, 0x11, 0x00, 0x00, 0x0b };
    append_unsigned_leb128(code_section, caller.size());
    code_section.extend(move(caller));
    Vector<u8> callee { 0x00, 0x20, 0x00, 0x41, 0x01, 0x6a, 0x0b };
    append_unsigned_leb128(code_section, callee.size());
    code_section.extend(move(callee));
    append_wasm_section(module, 10, move(code_section));

    return module;
}

static void expect_direct_frontend(Wasm::CompiledInstructions const& compiled)
{
    EXPECT(compiled.cranelift_compiled);
    EXPECT_NE(Wasm::cranelift_entry_acquire(compiled), 0u);
    EXPECT_NE(Wasm::cranelift_direct_native_entry_acquire(compiled), 0u);
    EXPECT_EQ(Wasm::cranelift_native_entry_acquire(compiled), 0u);
}

static void expect_interpreted(Wasm::CompiledInstructions const& compiled)
{
    EXPECT(!compiled.cranelift_compiled);
    EXPECT_EQ(Wasm::cranelift_entry_acquire(compiled), 0u);
    EXPECT_EQ(Wasm::cranelift_direct_native_entry_acquire(compiled), 0u);
    EXPECT_EQ(Wasm::cranelift_native_entry_acquire(compiled), 0u);
    EXPECT_EQ(Wasm::cranelift_osr_entry_acquire(compiled), 0u);
}

TEST_CASE(direct_osr_artifact_is_excluded_from_cache)
{
    auto parse_module = [] {
        auto file = MUST(Core::File::open("Fixtures/direct-osr-retention.wasm"sv, Core::File::OpenMode::Read));
        auto bytes = MUST(file->read_until_eof());
        FixedMemoryStream stream { bytes.bytes() };
        return MUST(Wasm::Module::parse(stream));
    };
    auto expect_published_osr = [](Wasm::CompiledInstructions const& compiled) {
        EXPECT(compiled.has_tier_up_checkpoints);
        EXPECT_EQ(compiled.tier_up_checkpoints.size(), 1u);
        expect_direct_frontend(compiled);
        EXPECT_NE(Wasm::cranelift_osr_entry_acquire(compiled), 0u);
        EXPECT_NE(compiled.cranelift_osr_code_start, 0u);
        EXPECT_NE(compiled.cranelift_osr_code_start, compiled.cranelift_code_start);
        EXPECT_NE(compiled.cranelift_osr_code_size, 0u);
        EXPECT_NE(compiled.cranelift_osr_traps, nullptr);
        EXPECT_NE(compiled.cranelift_osr_trap_count, 0u);
    };

    ByteBuffer cache_blob;
    {
        auto module = parse_module();
        Wasm::CompileCacheConfig cache_config;
        cache_config.on_compiled = [&](ByteBuffer blob) {
            cache_blob = move(blob);
        };
        Wasm::AbstractMachine machine;
        MUST(machine.validate(*module, move(cache_config)));
        expect_published_osr(module->code_section().functions().last().func().body().compiled_instructions);
    }
    EXPECT(!cache_blob.is_empty());

    auto module = parse_module();
    Wasm::CompileCacheConfig cache_config;
    cache_config.existing_blob = move(cache_blob);
    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, move(cache_config)));
    auto const& cached = module->code_section().functions().last().func().body().compiled_instructions;
    expect_direct_frontend(cached);
    EXPECT_EQ(Wasm::cranelift_osr_entry_acquire(cached), 0u);
    EXPECT_EQ(cached.cranelift_osr_code_start, 0u);
    EXPECT_EQ(cached.cranelift_osr_code_size, 0u);
    EXPECT_EQ(cached.cranelift_osr_traps, nullptr);
    EXPECT_EQ(cached.cranelift_osr_trap_count, 0u);
}

TEST_CASE(direct_osr_does_not_resume_interpreter_frame)
{
    auto file = MUST(Core::File::open("Fixtures/direct-osr-retention.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, {}, Wasm::CompileToNative::No));

    auto& compiled = module->code_section().functions()[0].func().body().compiled_instructions;
    EXPECT(compiled.has_tier_up_checkpoints);
    EXPECT(!compiled.cranelift_compiled);
    EXPECT_EQ(compiled.tier_up_checkpoints.size(), 1u);
    auto const& checkpoint = compiled.tier_up_checkpoints[0];
    EXPECT_EQ(checkpoint.checkpoint_id, Wasm::TierUpCheckpointIndex { 0 });
    EXPECT_EQ(compiled.dispatches[checkpoint.interpreter_dispatch_index.value()].instruction->opcode(), Wasm::Instructions::synthetic_tier_up);
    EXPECT_EQ(module->code_section().functions()[0].func().body().instructions()[checkpoint.parsed_loop_instruction_index.value()].opcode(), Wasm::Instructions::loop);
    EXPECT_EQ(checkpoint.live_local_indices.size(), 4u);
    EXPECT_EQ(checkpoint.live_local_indices[0], Wasm::LocalIndex { 0 });
    EXPECT_EQ(checkpoint.live_local_indices[1], Wasm::LocalIndex { 1 });
    EXPECT_EQ(checkpoint.live_local_indices[2], Wasm::LocalIndex { 2 });
    EXPECT_EQ(checkpoint.live_local_indices[3], Wasm::LocalIndex { 3 });

    size_t compilation_requests = 0;
    Wasm::FunctionType compile_type { {}, {} };
    auto compile = machine.store().allocate(Wasm::HostFunction {
        [&](Wasm::Configuration&, Span<Wasm::Value>) -> Wasm::Result {
            ++compilation_requests;
            if (!compiled.cranelift_compiled) {
                Wasm::start_cranelift_compilation(*module);
                expect_direct_frontend(compiled);
                EXPECT_NE(Wasm::cranelift_osr_entry_acquire(compiled), 0u);
            }
            return Wasm::Result { Vector<Wasm::Value> {} };
        },
        compile_type,
        "compile" });
    VERIFY(compile.has_value());

    auto instance = MUST(machine.instantiate(*module, { *compile }));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto const initial_tier_up_count = Wasm::tier_up_taken_count();
    auto osr_result = machine.invoke(*run, { Wasm::Value(static_cast<i32>(0)), Wasm::Value(static_cast<i32>(3)) });
    EXPECT_EQ(compilation_requests, 1u);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count + 1);
    EXPECT(!osr_result.is_trap());
    EXPECT_EQ(osr_result.values().size(), 2u);
    EXPECT_EQ(osr_result.values()[0].to<double>(), 3.0);
    EXPECT_EQ(osr_result.values()[1].to<i32>(), 43);

    auto fresh_result = machine.invoke(*run, { Wasm::Value(static_cast<i32>(0)), Wasm::Value(static_cast<i32>(3)) });
    EXPECT_EQ(compilation_requests, 2u);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count + 1);
    EXPECT(!fresh_result.is_trap());
    EXPECT_EQ(fresh_result.values().size(), 2u);
    EXPECT_EQ(fresh_result.values()[0].to<double>(), 6.0);
    EXPECT_EQ(fresh_result.values()[1].to<i32>(), 46);

    auto const fresh_entry = Wasm::cranelift_entry_acquire(compiled);
    Wasm::publish_cranelift_entry(compiled, 0);
    auto trapped = machine.invoke(*run, { Wasm::Value(static_cast<i32>(1)), Wasm::Value(static_cast<i32>(3)) });
    EXPECT_EQ(compilation_requests, 3u);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count + 2);
    EXPECT(trapped.is_trap());
    EXPECT_EQ(trapped.trap().format(), "unreachable executed"sv);
    Wasm::publish_cranelift_entry(compiled, fresh_entry);
}

TEST_CASE(direct_osr_publication_races_interpreter_execution)
{
    auto file = MUST(Core::File::open("Fixtures/direct-osr-retention.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, {}, Wasm::CompileToNative::No));

    RefPtr<Threading::Thread> compilation_thread;
    Wasm::FunctionType compile_type { {}, {} };
    auto compile = machine.store().allocate(Wasm::HostFunction {
        [&](Wasm::Configuration&, Span<Wasm::Value>) -> Wasm::Result {
            EXPECT(!compilation_thread);
            compilation_thread = Threading::Thread::construct("DirectWasmOSRCompiler"sv, [&module] {
                Wasm::start_cranelift_compilation(*module);
                return 0;
            });
            compilation_thread->start();
            return Wasm::Result { Vector<Wasm::Value> {} };
        },
        compile_type,
        "compile" });
    VERIFY(compile.has_value());

    auto instance = MUST(machine.instantiate(*module, { *compile }));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    constexpr i32 iteration_count = 5'000'000;
    auto const initial_tier_up_count = Wasm::tier_up_taken_count();
    auto result = machine.invoke(*run, { Wasm::Value(static_cast<i32>(0)), Wasm::Value(iteration_count) });
    VERIFY(compilation_thread);
    EXPECT(!compilation_thread->join().is_error());

    auto const& compiled = module->code_section().functions()[0].func().body().compiled_instructions;
    expect_direct_frontend(compiled);
    EXPECT_NE(Wasm::cranelift_osr_entry_acquire(compiled), 0u);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count + 1);
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 2u);
    EXPECT_EQ(result.values()[0].to<double>(), static_cast<double>(iteration_count));
    EXPECT_EQ(result.values()[1].to<i32>(), iteration_count + 40);
}

TEST_CASE(direct_frontend_fresh_entry_survives_cache_round_trip)
{
    auto const initial_tier_up_count = Wasm::tier_up_taken_count();
    auto parse_module = [] {
        auto bytes = make_direct_call_chain_module(1);
        FixedMemoryStream stream { bytes.span() };
        return MUST(Wasm::Module::parse(stream));
    };
    auto expect_frontend = [](Wasm::CompiledInstructions const& compiled) {
        expect_direct_frontend(compiled);
    };
    auto invoke = [](Wasm::AbstractMachine& machine, Wasm::ModuleInstance const& instance) {
        Optional<Wasm::FunctionAddress> run;
        for (auto const& export_ : instance.exports()) {
            if (export_.name() == "run"sv)
                run = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(run.has_value());
        return machine.invoke(*run, { Wasm::Value(static_cast<i32>(41)) });
    };

    ByteBuffer cache_blob;
    {
        auto module = parse_module();
        Wasm::CompileCacheConfig cache_config;
        cache_config.on_compiled = [&](ByteBuffer blob) {
            cache_blob = move(blob);
        };

        Wasm::AbstractMachine machine;
        MUST(machine.validate(*module, move(cache_config)));
        expect_frontend(module->code_section().functions()[0].func().body().compiled_instructions);
        auto instance = MUST(machine.instantiate(*module, {}));
        auto result = invoke(machine, *instance);
        EXPECT(!result.is_trap());
        EXPECT_EQ(result.values().size(), 1u);
        EXPECT_EQ(result.values()[0].to<i32>(), 42);
    }
    EXPECT(!cache_blob.is_empty());

    auto module = parse_module();
    bool produced_replacement_blob = false;
    Wasm::CompileCacheConfig cache_config;
    cache_config.existing_blob = move(cache_blob);
    cache_config.on_compiled = [&](ByteBuffer) {
        produced_replacement_blob = true;
    };

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, move(cache_config)));
    EXPECT(!produced_replacement_blob);
    expect_frontend(module->code_section().functions()[0].func().body().compiled_instructions);
    auto instance = MUST(machine.instantiate(*module, {}));
    auto result = invoke(machine, *instance);
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 42);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count);
}

TEST_CASE(interpreter_caller_reaches_direct_callee)
{
    auto bytes = make_direct_call_chain_module(2, 0);
    FixedMemoryStream stream { bytes.span() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));
    auto const& functions = module->code_section().functions();
    auto const& caller = functions[0].func().body().compiled_instructions;
    auto const& callee = functions[1].func().body().compiled_instructions;
    expect_interpreted(caller);
    expect_direct_frontend(callee);

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

TEST_CASE(direct_frontend_static_call_falls_back_to_interpreter)
{
    auto parse_module = [] {
        auto bytes = make_direct_call_chain_module(2, 1);
        FixedMemoryStream stream { bytes.span() };
        return MUST(Wasm::Module::parse(stream));
    };
    auto expect_frontends = [](Wasm::Module const& module) {
        auto const& functions = module.code_section().functions();
        auto const& caller = functions[0].func().body().compiled_instructions;
        auto const& callee = functions[1].func().body().compiled_instructions;
        expect_direct_frontend(caller);
        expect_interpreted(callee);
    };
    auto invoke = [](Wasm::AbstractMachine& machine, Wasm::ModuleInstance const& instance) {
        Optional<Wasm::FunctionAddress> run;
        for (auto const& export_ : instance.exports()) {
            if (export_.name() == "run"sv)
                run = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(run.has_value());
        return machine.invoke(*run, { Wasm::Value(static_cast<i32>(41)) });
    };

    ByteBuffer cache_blob;
    {
        auto module = parse_module();
        Wasm::CompileCacheConfig cache_config;
        cache_config.on_compiled = [&](ByteBuffer blob) {
            cache_blob = move(blob);
        };

        Wasm::AbstractMachine machine;
        MUST(machine.validate(*module, move(cache_config)));
        expect_frontends(*module);
        auto instance = MUST(machine.instantiate(*module, {}));
        auto result = invoke(machine, *instance);
        EXPECT(!result.is_trap());
        EXPECT_EQ(result.values()[0].to<i32>(), 42);
    }
    EXPECT(!cache_blob.is_empty());

    auto module = parse_module();
    Wasm::CompileCacheConfig cache_config;
    cache_config.existing_blob = move(cache_blob);

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, move(cache_config)));
    expect_frontends(*module);
    auto instance = MUST(machine.instantiate(*module, {}));
    auto result = invoke(machine, *instance);
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values()[0].to<i32>(), 42);
}

TEST_CASE(direct_frontend_links_mutually_recursive_group)
{
    auto bytes = make_mutually_recursive_module();
    FixedMemoryStream stream { bytes.span() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));
    for (auto const& function : module->code_section().functions())
        expect_direct_frontend(function.func().body().compiled_instructions);

    auto instance = MUST(machine.instantiate(*module, {}));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, { Wasm::Value(static_cast<i32>(6)) });
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 0);
}

TEST_CASE(direct_frontend_traps_native_stack_exhaustion)
{
    auto bytes = make_mutually_recursive_module();
    FixedMemoryStream stream { bytes.span() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));
    for (auto const& function : module->code_section().functions())
        expect_direct_frontend(function.func().body().compiled_instructions);

    auto instance = MUST(machine.instantiate(*module, {}));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, { Wasm::Value(NumericLimits<i32>::max()) });
    EXPECT(result.is_trap());
    EXPECT_EQ(result.trap().format(), Wasm::Constants::stack_exhaustion_message);
}

TEST_CASE(direct_frontend_executes_indirect_calls)
{
    auto bytes = make_call_indirect_callee_module();
    FixedMemoryStream stream { bytes.span() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));
    for (auto const& function : module->code_section().functions())
        expect_direct_frontend(function.func().body().compiled_instructions);

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

TEST_CASE(direct_frontend_explicit_trap_reaches_fault_recovery)
{
    Vector<u8> bytes {
        0x00,
        0x61,
        0x73,
        0x6d,
        0x01,
        0x00,
        0x00,
        0x00,
        0x01,
        0x04,
        0x01,
        0x60,
        0x00,
        0x00,
        0x03,
        0x02,
        0x01,
        0x00,
        0x07,
        0x07,
        0x01,
        0x03,
        'r',
        'u',
        'n',
        0x00,
        0x00,
        0x0a,
        0x05,
        0x01,
        0x03,
        0x00,
        0x00,
        0x0b,
    };
    FixedMemoryStream stream { bytes.span() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));
    auto const& compiled = module->code_section().functions()[0].func().body().compiled_instructions;
    expect_direct_frontend(compiled);

    auto instance = MUST(machine.instantiate(*module, {}));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());
    EXPECT(machine.invoke(*run, {}).is_trap());
}

TEST_CASE(tier_up_does_not_resume_interpreter_frame)
{
    auto file = MUST(Core::File::open("Fixtures/tier-up-one-way.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, {}, Wasm::CompileToNative::No));

    auto& compiled = module->code_section().functions()[0].func().body().compiled_instructions;
    EXPECT(compiled.has_tier_up_checkpoints);
    EXPECT(!compiled.cranelift_compiled);
    EXPECT_EQ(compiled.tier_up_checkpoints.size(), 1u);
    auto const& checkpoint = compiled.tier_up_checkpoints[0];
    EXPECT_EQ(checkpoint.checkpoint_id, Wasm::TierUpCheckpointIndex { 0 });
    EXPECT_EQ(compiled.dispatches[checkpoint.interpreter_dispatch_index.value()].instruction->opcode(), Wasm::Instructions::synthetic_tier_up);
    EXPECT_EQ(module->code_section().functions()[0].func().body().instructions()[checkpoint.parsed_loop_instruction_index.value()].opcode(), Wasm::Instructions::loop);
    EXPECT_EQ(checkpoint.live_local_indices.size(), 2u);
    EXPECT_EQ(checkpoint.live_local_indices[0], Wasm::LocalIndex { 0 });
    EXPECT_EQ(checkpoint.live_local_indices[1], Wasm::LocalIndex { 260 });

    size_t compilation_requests = 0;
    FlatPtr osr_entry = 0;
    bool publish_osr_during_call = false;
    Wasm::FunctionType compile_type { {}, {} };
    auto compile = machine.store().allocate(Wasm::HostFunction {
        [&](Wasm::Configuration&, Span<Wasm::Value>) -> Wasm::Result {
            ++compilation_requests;
            if (!compiled.cranelift_compiled) {
                Wasm::start_cranelift_compilation(*module);
                osr_entry = Wasm::cranelift_osr_entry_acquire(compiled);
                EXPECT_NE(osr_entry, 0u);
                Wasm::publish_cranelift_osr_entry(compiled, 0);
            } else if (publish_osr_during_call) {
                Wasm::publish_cranelift_osr_entry(compiled, osr_entry);
            }
            return Wasm::Result { Vector<Wasm::Value> {} };
        },
        compile_type,
        "compile" });
    VERIFY(compile.has_value());

    auto instance = MUST(machine.instantiate(*module, { *compile }));
    auto find_export = [&](StringView name) {
        Optional<Wasm::FunctionAddress> address;
        for (auto const& export_ : instance->exports()) {
            if (export_.name() == name)
                address = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(address.has_value());
        return *address;
    };
    auto run = find_export("run"sv);

    auto const initial_tier_up_count = Wasm::tier_up_taken_count();
    auto result = machine.invoke(run, {});
    EXPECT(compiled.cranelift_compiled);
    EXPECT_EQ(compilation_requests, 1u);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count);
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 43);

    auto const fresh_entry = Wasm::cranelift_entry_acquire(compiled);
    EXPECT_NE(fresh_entry, 0u);
    Wasm::publish_cranelift_entry(compiled, 0);
    publish_osr_during_call = true;
    auto osr_result = machine.invoke(run, {});
    EXPECT_EQ(compilation_requests, 2u);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count + 1);
    EXPECT(!osr_result.is_trap());
    EXPECT_EQ(osr_result.values().size(), 1u);
    EXPECT_EQ(osr_result.values()[0].to<i32>(), 46);

    Wasm::publish_cranelift_entry(compiled, fresh_entry);
    auto fresh_native_result = machine.invoke(run, {});
    EXPECT_EQ(compilation_requests, 3u);
    EXPECT_EQ(Wasm::tier_up_taken_count(), initial_tier_up_count + 1);
    EXPECT(!fresh_native_result.is_trap());
    EXPECT_EQ(fresh_native_result.values().size(), 1u);
    EXPECT_EQ(fresh_native_result.values()[0].to<i32>(), 49);

    Vector<Wasm::Value> native_arguments;
    for (i32 argument = 1; argument <= 11; ++argument)
        native_arguments.append(Wasm::Value(argument));
    auto native_many_locals_result = machine.invoke(find_export("native_many_locals"sv), move(native_arguments));
    EXPECT(!native_many_locals_result.is_trap());
    EXPECT_EQ(native_many_locals_result.values().size(), 1u);
    EXPECT_EQ(native_many_locals_result.values()[0].to<i32>(), 12);

    auto direct_many_locals_result = machine.invoke(find_export("direct_many_locals"sv), {});
    EXPECT(!direct_many_locals_result.is_trap());
    EXPECT_EQ(direct_many_locals_result.values().size(), 1u);
    EXPECT_EQ(direct_many_locals_result.values()[0].to<i32>(), 12);

    Vector<Wasm::Value> typed_arguments {
        Wasm::Value(static_cast<i32>(1)),
        Wasm::Value(static_cast<i64>(2)),
        Wasm::Value(3.5f),
        Wasm::Value(4.25),
    };
    auto native_typed_locals_result = machine.invoke(
        find_export("native_typed_locals"sv), move(typed_arguments));
    EXPECT(!native_typed_locals_result.is_trap());
    EXPECT_EQ(native_typed_locals_result.values().size(), 1u);
    EXPECT_EQ(native_typed_locals_result.values()[0].to<double>(), 20.75);

    auto direct_typed_locals_result = machine.invoke(find_export("direct_typed_locals"sv), {});
    EXPECT(!direct_typed_locals_result.is_trap());
    EXPECT_EQ(direct_typed_locals_result.values().size(), 1u);
    EXPECT_EQ(direct_typed_locals_result.values()[0].to<double>(), 20.75);

    auto invoke_with_i32 = [&](StringView name, i32 argument) {
        auto result = machine.invoke(find_export(name), { Wasm::Value(argument) });
        EXPECT(!result.is_trap());
        EXPECT_EQ(result.values().size(), 1u);
        return result.values()[0];
    };
    EXPECT_EQ(invoke_with_i32("ssa_typed_merge"sv, 1).to<double>(), 20.75);
    EXPECT_EQ(invoke_with_i32("ssa_typed_merge"sv, 0).to<double>(), 24.75);
    EXPECT_EQ(invoke_with_i32("ssa_partial_merge"sv, 1).to<i32>(), 20);
    EXPECT_EQ(invoke_with_i32("ssa_partial_merge"sv, 0).to<i32>(), 10);
    EXPECT_EQ(invoke_with_i32("ssa_loop_backedge"sv, 4).to<i64>(), 12);

    auto invoke_i32_bank_edges = [&](i32 value, i32 condition) {
        auto result = machine.invoke(find_export("i32_bank_edges"sv), { Wasm::Value(value), Wasm::Value(condition) });
        EXPECT(!result.is_trap());
        EXPECT_EQ(result.values().size(), 1u);
        return result.values()[0].to<i32>();
    };
    EXPECT_EQ(invoke_i32_bank_edges(-2147483647 - 1, 1), 0x40000008);
    EXPECT_EQ(invoke_i32_bank_edges(0x1234, 0), -53);

    EXPECT_EQ(invoke_with_i32("vstack_control_edges"sv, 0).to<double>(), 40.75);
    EXPECT_EQ(invoke_with_i32("vstack_control_edges"sv, 1).to<double>(), 40.75);
}

TEST_CASE(ineligible_function_has_no_tier_up_checkpoints)
{
    auto file = MUST(Core::File::open("Fixtures/tier-up-one-way.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, {}, Wasm::CompileToNative::No));

    auto const& ineligible = module->code_section().functions().last().func().body().compiled_instructions;
    EXPECT(!ineligible.cranelift_eligible);
    EXPECT(!ineligible.has_tier_up_checkpoints);
    for (auto const& dispatch : ineligible.dispatches)
        EXPECT(dispatch.instruction->opcode() != Wasm::Instructions::synthetic_tier_up);
}

TEST_CASE(native_control_flow_ignores_unreachable_merge_predecessors)
{
    auto file = MUST(Core::File::open("Fixtures/cranelift-unreachable-merge.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));
    for (auto const& function : module->code_section().functions())
        EXPECT(function.func().body().compiled_instructions.cranelift_compiled);

    auto instance = MUST(machine.instantiate(*module, {}));
    auto invoke = [&](StringView name, Vector<Wasm::Value> arguments = {}) {
        Optional<Wasm::FunctionAddress> address;
        for (auto const& export_ : instance->exports()) {
            if (export_.name() == name)
                address = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(address.has_value());
        return machine.invoke(*address, move(arguments));
    };

    auto unreachable_then = invoke("unreachable_then"sv, { Wasm::Value(static_cast<i32>(0)) });
    EXPECT(!unreachable_then.is_trap());
    EXPECT_EQ(unreachable_then.values()[0].to<i32>(), 42);

    auto unreachable_else = invoke("unreachable_else"sv, { Wasm::Value(static_cast<i32>(1)) });
    EXPECT(!unreachable_else.is_trap());
    EXPECT_EQ(unreachable_else.values()[0].to<double>(), 13.5);

    auto branched_result = invoke("branched_result"sv);
    EXPECT(!branched_result.is_trap());
    EXPECT_EQ(branched_result.values()[0].to<float>(), 7.25f);
}

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

TEST_CASE(native_direct_calls_survive_cranelift_cache_round_trip)
{
    auto parse_module = [] {
        auto file = MUST(Core::File::open("Fixtures/call-record-forward-inlined-locals.wasm"sv, Core::File::OpenMode::Read));
        auto bytes = MUST(file->read_until_eof());
        FixedMemoryStream stream { bytes.bytes() };
        return MUST(Wasm::Module::parse(stream));
    };

    ByteBuffer cache_blob;
    {
        auto module = parse_module();
        Wasm::CompileCacheConfig cache_config;
        cache_config.on_compiled = [&](ByteBuffer blob) {
            cache_blob = move(blob);
        };

        Wasm::AbstractMachine machine;
        MUST(machine.validate(*module, move(cache_config)));
    }
    EXPECT(!cache_blob.is_empty());

    auto module = parse_module();
    bool produced_replacement_blob = false;
    Wasm::CompileCacheConfig cache_config;
    cache_config.existing_blob = move(cache_blob);
    cache_config.on_compiled = [&](ByteBuffer) {
        produced_replacement_blob = true;
    };

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, move(cache_config)));
    EXPECT(!produced_replacement_blob);
    for (auto const& function : module->code_section().functions())
        EXPECT(function.func().body().compiled_instructions.cranelift_compiled);

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

TEST_CASE(compiled_function_table_applies_deferred_native_publications)
{
    auto file = MUST(Core::File::open("Fixtures/call-record-forward-inlined-locals.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module, {}, Wasm::CompileToNative::No));
    auto instance = MUST(machine.instantiate(*module, {}));

    auto const& interpreted_table = instance->compiled_fn_table(machine.store());
    EXPECT_EQ(interpreted_table.size(), 3u);
    for (auto const& entry : interpreted_table)
        EXPECT_EQ(entry.handler_ptr, 0u);

    Wasm::start_cranelift_compilation(*module);
    EXPECT_EQ(module->cranelift_publication_count(), 3u);

    auto const& native_table = instance->compiled_fn_table(machine.store());
    for (auto const& entry : native_table) {
        EXPECT(entry.handler_ptr != 0);
        EXPECT(entry.module != nullptr);
    }
}

TEST_CASE(native_direct_calls_cross_incremental_compilation_batches)
{
    auto bytes = make_direct_call_chain_module(513);
    FixedMemoryStream stream { bytes.span() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    MUST(machine.validate(*module));

    auto& functions = module->code_section().functions();
    EXPECT_EQ(functions.size(), 513u);
    for (size_t function_index = 0; function_index < functions.size(); ++function_index) {
        if (!functions[function_index].func().body().compiled_instructions.cranelift_compiled)
            warnln("Function {} was not compiled", function_index);
        EXPECT(functions[function_index].func().body().compiled_instructions.cranelift_compiled);
    }

    auto& first_batch_callee = functions[1].func().body().compiled_instructions;
    Wasm::publish_cranelift_entry(first_batch_callee, 0);
    Wasm::publish_cranelift_osr_entry(first_batch_callee, 0);
    Wasm::publish_cranelift_native_entry(first_batch_callee, 0);
    for (auto& dispatch : first_batch_callee.dispatches)
        dispatch.instruction_opcode = dispatch.instruction->opcode();
    first_batch_callee.direct = false;
    first_batch_callee.dispatches[0].instruction_opcode = Wasm::Instructions::unreachable;

    auto instance = MUST(machine.instantiate(*module, {}));
    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, { Wasm::Value(static_cast<i32>(0)) });
    EXPECT(!result.is_trap());
    if (result.is_trap()) {
        warnln("Cross-batch direct call trapped: {}", result.trap().format());
        return;
    }
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
    expect_direct_frontend(caller);

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
        expect_direct_frontend(function.func().body().compiled_instructions);

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
    expect_direct_frontend(sum4_compiled);
    EXPECT_NE(Wasm::cranelift_direct_native_entry_acquire(sum4_compiled), Wasm::cranelift_entry_acquire(sum4_compiled));

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
    EXPECT_EQ(functions.size(), 42u);
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
        expect_direct_frontend(compiled);
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
    EXPECT_EQ(invoke_value("run_null_typed_funcref"sv).to<i32>(), 1);
    EXPECT_EQ(invoke_value("run_non_null_typed_funcref"sv).to<i32>(), 0);

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
        expect_direct_frontend(function.func().body().compiled_instructions);

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
