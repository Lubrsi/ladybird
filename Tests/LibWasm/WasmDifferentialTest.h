/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>

namespace Wasm::TestSupport {

enum class ExecutionMode {
    Interpreter,
    Native,
};

struct Invocation {
    ByteString export_name;
    Vector<Value> arguments;
};

struct InvocationOutcome {
    Optional<ByteString> trap;
    Vector<u128> values;
};

struct FunctionCompilationState {
    bool eligible { false };
    bool compiled { false };
    bool has_entry { false };
};

struct ExecutionSnapshot {
    Vector<FunctionCompilationState> functions;
    Vector<InvocationOutcome> invocations;
    Vector<Vector<u8>> memories;
    Vector<u128> globals;
};

ExecutionSnapshot execute(ReadonlyBytes wasm, ExecutionMode, ReadonlySpan<Invocation>);
Optional<ByteString> find_first_difference(ExecutionSnapshot const&, ExecutionSnapshot const&);

}
