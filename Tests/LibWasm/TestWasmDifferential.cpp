/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WasmDifferentialTest.h"

#include <LibCore/File.h>
#include <LibTest/TestCase.h>

TEST_CASE(interpreter_and_native_execution_are_equivalent)
{
    auto file = MUST(Core::File::open("Fixtures/differential-state.wasm"sv, Core::File::OpenMode::Read));
    auto wasm = MUST(file->read_until_eof());

    Vector<Wasm::TestSupport::Invocation> invocations;
    invocations.append({ "step", { Wasm::Value(static_cast<i32>(3)) } });
    invocations.append({ "step", { Wasm::Value(static_cast<i32>(2)) } });
    invocations.append({ "fallback", {} });
    invocations.append({ "trap", {} });

    auto interpreter = Wasm::TestSupport::execute(
        wasm.bytes(),
        Wasm::TestSupport::ExecutionMode::Interpreter,
        invocations);
    auto native = Wasm::TestSupport::execute(
        wasm.bytes(),
        Wasm::TestSupport::ExecutionMode::Native,
        invocations);

    EXPECT_EQ(interpreter.functions.size(), 3u);
    EXPECT_EQ(native.functions.size(), 3u);
    for (auto const& function : interpreter.functions) {
        EXPECT(!function.compiled);
        EXPECT(!function.has_entry);
    }
    for (size_t function_index = 0; function_index < 2; ++function_index) {
        EXPECT(native.functions[function_index].eligible);
        EXPECT(native.functions[function_index].compiled);
        EXPECT(native.functions[function_index].has_entry);
    }
    EXPECT(!native.functions[2].eligible);
    EXPECT(!native.functions[2].compiled);
    EXPECT(!native.functions[2].has_entry);

    EXPECT_EQ(interpreter.invocations.size(), 4u);
    EXPECT(!interpreter.invocations[0].trap.has_value());
    EXPECT_EQ(interpreter.invocations[0].values.size(), 1u);
    EXPECT_EQ(interpreter.invocations[0].values[0].low(), 40u);
    EXPECT(!interpreter.invocations[1].trap.has_value());
    EXPECT_EQ(interpreter.invocations[1].values.size(), 1u);
    EXPECT_EQ(interpreter.invocations[1].values[0].low(), 364u);
    EXPECT(!interpreter.invocations[2].trap.has_value());
    EXPECT_EQ(interpreter.invocations[2].values.size(), 2u);
    EXPECT_EQ(interpreter.invocations[3].trap.value(), "Memory access out of bounds"sv);
    EXPECT_EQ(interpreter.globals.size(), 1u);
    EXPECT_EQ(interpreter.globals[0].low(), 364u);
    EXPECT_EQ(interpreter.memories.size(), 1u);
    EXPECT_EQ(interpreter.memories[0][8], 121u);
    EXPECT_EQ(interpreter.memories[0][16], 0x6cu);
    EXPECT_EQ(interpreter.memories[0][17], 0x01u);
    EXPECT_EQ(interpreter.memories[0][24], 40u);

    auto difference = Wasm::TestSupport::find_first_difference(interpreter, native);
    if (difference.has_value())
        warnln("Differential Wasm execution failed: {}", *difference);
    EXPECT(!difference.has_value());
}
