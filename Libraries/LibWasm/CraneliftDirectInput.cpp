/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibWasm/CraneliftDirectInput.h>

namespace Wasm {

Cranelift::DirectValueType serialize_direct_value_type(ValueType const& type)
{
    auto kind = [&] {
        switch (type.kind()) {
        case ValueType::I32:
            return Cranelift::DirectValueTypeKind::I32;
        case ValueType::I64:
            return Cranelift::DirectValueTypeKind::I64;
        case ValueType::F32:
            return Cranelift::DirectValueTypeKind::F32;
        case ValueType::F64:
            return Cranelift::DirectValueTypeKind::F64;
        case ValueType::V128:
            return Cranelift::DirectValueTypeKind::V128;
        case ValueType::I8:
            return Cranelift::DirectValueTypeKind::I8;
        case ValueType::I16:
            return Cranelift::DirectValueTypeKind::I16;
        case ValueType::FunctionReference:
            return Cranelift::DirectValueTypeKind::FunctionReference;
        case ValueType::NoFunctionReference:
            return Cranelift::DirectValueTypeKind::NoFunctionReference;
        case ValueType::ExternReference:
            return Cranelift::DirectValueTypeKind::ExternReference;
        case ValueType::NoExternReference:
            return Cranelift::DirectValueTypeKind::NoExternReference;
        case ValueType::AnyReference:
            return Cranelift::DirectValueTypeKind::AnyReference;
        case ValueType::EqReference:
            return Cranelift::DirectValueTypeKind::EqReference;
        case ValueType::I31Reference:
            return Cranelift::DirectValueTypeKind::I31Reference;
        case ValueType::StructReference:
            return Cranelift::DirectValueTypeKind::StructReference;
        case ValueType::ArrayReference:
            return Cranelift::DirectValueTypeKind::ArrayReference;
        case ValueType::NoneReference:
            return Cranelift::DirectValueTypeKind::NoneReference;
        case ValueType::ExceptionReference:
            return Cranelift::DirectValueTypeKind::ExceptionReference;
        case ValueType::NoExceptionReference:
            return Cranelift::DirectValueTypeKind::NoExceptionReference;
        case ValueType::TypeUseReference:
            return Cranelift::DirectValueTypeKind::TypeUseReference;
        }
        VERIFY_NOT_REACHED();
    }();

    return {
        .kind = static_cast<u32>(kind),
        .type_index = type.is_typeuse() ? type.unsafe_typeindex().value() : 0,
        .nullable = type.is_nullable() ? 1u : 0u,
    };
}

static Cranelift::DirectBlockType serialize_block_type(BlockType const& block_type)
{
    switch (block_type.kind()) {
    case BlockType::Empty:
        return {
            .kind = static_cast<u32>(Cranelift::DirectBlockTypeKind::Empty),
            .value_type = serialize_direct_value_type(ValueType { ValueType::I32 }),
            .type_index = 0,
        };
    case BlockType::Type:
        return {
            .kind = static_cast<u32>(Cranelift::DirectBlockTypeKind::ValueType),
            .value_type = serialize_direct_value_type(block_type.value_type()),
            .type_index = 0,
        };
    case BlockType::Index:
        return {
            .kind = static_cast<u32>(Cranelift::DirectBlockTypeKind::TypeIndex),
            .value_type = serialize_direct_value_type(ValueType { ValueType::I32 }),
            .type_index = block_type.type_index().value(),
        };
    }
    VERIFY_NOT_REACHED();
}

static Optional<Cranelift::DirectInstruction> serialize_instruction(Instruction const& instruction, Vector<u32>& branch_targets)
{
    Cranelift::DirectInstruction output {};
    output.opcode = instruction.opcode().value();

    if (instruction.opcode() == Instructions::local_get
        || instruction.opcode() == Instructions::local_set
        || instruction.opcode() == Instructions::local_tee) {
        output.arguments.local_index = instruction.local_index().value();
        return output;
    }

    if (instruction.opcode() == Instructions::synthetic_end_expression) {
        output.opcode = Instructions::structured_end.value();
        return output;
    }

    bool supported = instruction.arguments().visit(
        [&](u8) {
            return true;
        },
        [&](i32 value) {
            output.arguments.i32_constant = value;
            return true;
        },
        [&](i64 value) {
            output.arguments.i64_constant = value;
            return true;
        },
        [&](float value) {
            output.arguments.f32_constant = value;
            return true;
        },
        [&](double value) {
            output.arguments.f64_constant = value;
            return true;
        },
        [&](u128 value) {
            output.arguments.vector_constant[0] = value.low();
            output.arguments.vector_constant[1] = value.high();
            return true;
        },
        [&](Instruction::StructuredInstructionArgs const& arguments) {
            output.arguments.structured = {
                .block_type = serialize_block_type(arguments.block_type),
                .end_ip = arguments.end_ip.value(),
                .else_ip = arguments.else_ip().value_or(InstructionPointer { NumericLimits<u32>::max() }).value(),
            };
            return true;
        },
        [&](Instruction::BranchArgs const& arguments) {
            output.arguments.label_index = arguments.label.value();
            return true;
        },
        [&](Instruction::TableBranchArgs const& arguments) {
            Checked<u32> target_count { branch_targets.size() };
            target_count += arguments.labels.size();
            if (target_count.has_overflow())
                return false;
            output.arguments.table_branch = {
                .targets_offset = static_cast<u32>(branch_targets.size()),
                .target_count = static_cast<u32>(arguments.labels.size()),
                .default_target = arguments.default_.value(),
            };
            for (auto label : arguments.labels)
                branch_targets.append(label.value());
            return true;
        },
        [&](Instruction::MemoryArgument const& arguments) {
            output.arguments.memory = {
                .align = arguments.align,
                .memory_index = arguments.memory_index.value(),
                .offset = arguments.offset,
            };
            return true;
        },
        [&](Instruction::MemoryCopyArgs const& arguments) {
            output.arguments.memory_copy = {
                .source_memory_index = arguments.src_index.value(),
                .destination_memory_index = arguments.dst_index.value(),
            };
            return true;
        },
        [&](Instruction::MemoryIndexArgument const& argument) {
            output.arguments.memory_index = argument.memory_index.value();
            return true;
        },
        [&](FunctionIndex const& index) {
            output.arguments.function_index = index.value();
            return true;
        },
        [&](GlobalIndex const& index) {
            output.arguments.global_index = index.value();
            return true;
        },
        [&](TypeIndex const& index) {
            output.arguments.type_index = index.value();
            return true;
        },
        [&](TableIndex const& index) {
            output.arguments.table_index = index.value();
            return true;
        },
        [&](TagIndex const& index) {
            output.arguments.tag_index = index.value();
            return true;
        },
        [&](DataIndex const& index) {
            output.arguments.data_index = index.value();
            return true;
        },
        [&](ElementIndex const& index) {
            output.arguments.element_index = index.value();
            return true;
        },
        [&](LabelIndex const& index) {
            output.arguments.label_index = index.value();
            return true;
        },
        [&](Instruction::IndirectCallArgs const& arguments) {
            output.arguments.indirect_call = {
                .type_index = arguments.type.value(),
                .table_index = arguments.table.value(),
            };
            return true;
        },
        [&](auto const&) {
            return false;
        });

    if (!supported)
        return {};
    return output;
}

Optional<DirectCompilerInput> serialize_direct_compiler_input(CodeSection::Func const& function)
{
    DirectCompilerInput output;
    output.instructions.ensure_capacity(function.body().instructions().size());
    for (auto const& instruction : function.body().instructions()) {
        auto serialized = serialize_instruction(instruction, output.branch_targets);
        if (!serialized.has_value())
            return {};
        output.instructions.unchecked_append(serialized.release_value());
    }

    output.local_types.ensure_capacity(function.total_local_count());
    for (auto const& local_group : function.locals()) {
        auto type = serialize_direct_value_type(local_group.type());
        for (u32 i = 0; i < local_group.n(); ++i)
            output.local_types.unchecked_append(type);
    }

    for (auto const& checkpoint : function.body().compiled_instructions.tier_up_checkpoints) {
        Checked<u32> live_local_index_count { output.tier_up_live_local_indices.size() };
        live_local_index_count += checkpoint.live_local_indices.size();
        if (live_local_index_count.has_overflow())
            return {};
        output.tier_up_checkpoints.append({
            .checkpoint_id = checkpoint.checkpoint_id.value(),
            .interpreter_dispatch_index = checkpoint.interpreter_dispatch_index.value(),
            .loop_instruction_index = checkpoint.parsed_loop_instruction_index.value(),
            .live_local_indices_offset = static_cast<u32>(output.tier_up_live_local_indices.size()),
            .live_local_index_count = static_cast<u32>(checkpoint.live_local_indices.size()),
        });
        for (auto local_index : checkpoint.live_local_indices)
            output.tier_up_live_local_indices.append(local_index.value());
    }

    return output;
}

}
