/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <CraneliftFFI.h>
#include <LibWasm/Types.h>

namespace Wasm {

struct DirectCompilerInput {
    Vector<Cranelift::DirectInstruction> instructions;
    Vector<u32> branch_targets;
    Vector<Cranelift::DirectValueType> local_types;
};

Cranelift::DirectValueType serialize_direct_value_type(ValueType const&);
Optional<DirectCompilerInput> serialize_direct_compiler_input(CodeSection::Func const&);

}
