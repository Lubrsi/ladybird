/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::CompiledFunction;
use crate::DirectInstruction;
use crate::DirectValueType;
use crate::FunctionCompilationOptions;
use crate::HelperId;
use crate::RuntimeLayout as SerializedRuntimeLayout;
use crate::WasmFunctionType;

use cranelift_codegen::ir::AbiParam;
use cranelift_codegen::ir::Block;
use cranelift_codegen::ir::BlockArg;
use cranelift_codegen::ir::ExtFuncData;
use cranelift_codegen::ir::ExternalName;
use cranelift_codegen::ir::Function;
use cranelift_codegen::ir::InstBuilder;
use cranelift_codegen::ir::Signature;
use cranelift_codegen::ir::Type;
use cranelift_codegen::ir::UserExternalName;
use cranelift_codegen::ir::UserFuncName;
use cranelift_codegen::ir::Value;
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::types;
use cranelift_codegen::isa::TargetIsa;
use cranelift_codegen::settings::Configurable;
use cranelift_codegen::settings::{self};
use cranelift_frontend::FunctionBuilder;
use cranelift_frontend::FunctionBuilderContext;
use cranelift_frontend::Variable;

use super::common::HELPER_EXTERNAL_NAMESPACE;
use super::common::RuntimeLayout;
use super::common::WASM_FUNCTION_EXTERNAL_NAMESPACE;
use super::common::WasmMemoryFlags;
use super::common::compile_function;
use super::common::wasm_abi_type;
use super::direct_input::BlockType;
use super::direct_input::ValueType as CheckedValueType;
use super::direct_input::ValueTypeKind;
use super::op;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ControlKind {
    Block,
    Loop,
}

struct ControlFrame {
    kind: ControlKind,
    branch_target: Block,
    continuation: Block,
    entry_stack: Vec<Value>,
    branch_types: Vec<Type>,
    result_types: Vec<Type>,
    continuation_has_predecessor: bool,
}

pub(crate) struct DirectCompiler;

impl DirectCompiler {
    fn checked_direct_type(value_type: CheckedValueType) -> Result<Type, &'static str> {
        match value_type.kind {
            ValueTypeKind::I32 => Ok(types::I32),
            ValueTypeKind::I64 => Ok(types::I64),
            ValueTypeKind::F32 => Ok(types::F32),
            ValueTypeKind::F64 => Ok(types::F64),
            ValueTypeKind::V128 => Ok(types::I8X16),
            _ => Err("unsupported direct value type"),
        }
    }

    fn direct_type(value_type: DirectValueType) -> Result<Type, &'static str> {
        Self::checked_direct_type(value_type.checked()?)
    }

    fn block_results(block_type: BlockType) -> Result<Vec<Type>, &'static str> {
        match block_type {
            BlockType::Empty => Ok(Vec::new()),
            BlockType::Value(value_type) => Ok(vec![Self::checked_direct_type(value_type)?]),
            BlockType::TypeIndex(_) => Err("type-index block types are not yet supported"),
        }
    }

    fn clean_signature(isa: &dyn TargetIsa, function_type: WasmFunctionType<'_>) -> Result<Signature, &'static str> {
        let mut signature = Signature::new(isa.default_call_conv());
        signature.params.push(AbiParam::new(isa.pointer_type()));
        for &parameter in function_type.parameters {
            signature.params.push(AbiParam::new(wasm_abi_type(parameter)?));
        }
        for &result in function_type.results {
            signature.returns.push(AbiParam::new(wasm_abi_type(result)?));
        }
        Ok(signature)
    }

    fn value_type(builder: &FunctionBuilder<'_>, value: Value) -> Type {
        builder.func.dfg.value_type(value)
    }

    fn pop_expected(
        builder: &FunctionBuilder<'_>,
        operand_stack: &mut Vec<Value>,
        expected: Type,
    ) -> Result<Value, &'static str> {
        let value = operand_stack.pop().ok_or("direct operand stack underflow")?;
        if Self::value_type(builder, value) != expected {
            return Err("direct operand type mismatch");
        }
        Ok(value)
    }

    fn branch_arguments(
        builder: &FunctionBuilder<'_>,
        operand_stack: &[Value],
        expected: &[Type],
    ) -> Result<Vec<BlockArg>, &'static str> {
        let values = operand_stack
            .get(
                operand_stack
                    .len()
                    .checked_sub(expected.len())
                    .ok_or("direct branch stack underflow")?..,
            )
            .ok_or("direct branch stack underflow")?;
        values
            .iter()
            .zip(expected)
            .map(|(&value, &expected_type)| {
                if Self::value_type(builder, value) != expected_type {
                    return Err("direct branch argument type mismatch");
                }
                Ok(BlockArg::Value(value))
            })
            .collect()
    }

    fn result_arguments(
        builder: &FunctionBuilder<'_>,
        operand_stack: &mut Vec<Value>,
        expected: &[Type],
    ) -> Result<Vec<BlockArg>, &'static str> {
        let mut arguments = Vec::with_capacity(expected.len());
        for &expected_type in expected.iter().rev() {
            arguments.push(BlockArg::Value(Self::pop_expected(
                builder,
                operand_stack,
                expected_type,
            )?));
        }
        arguments.reverse();
        Ok(arguments)
    }

    fn zero(builder: &mut FunctionBuilder<'_>, ty: Type) -> Result<Value, &'static str> {
        match ty {
            types::I32 | types::I64 => Ok(builder.ins().iconst(ty, 0)),
            types::F32 => Ok(builder.ins().f32const(0.0)),
            types::F64 => Ok(builder.ins().f64const(0.0)),
            types::I8X16 => {
                let zero = builder.func.dfg.constants.insert(vec![0; 16].into());
                Ok(builder.ins().vconst(types::I8X16, zero))
            }
            _ => Err("unsupported direct local type"),
        }
    }

    fn declare_cage_base(
        builder: &mut FunctionBuilder<'_>,
        pointer_type: Type,
        memory_flags: WasmMemoryFlags,
    ) -> Value {
        let mut signature = Signature::new(builder.func.signature.call_conv);
        signature.returns.push(AbiParam::new(types::I64));
        let signature = builder.import_signature(signature);
        let name = builder.func.declare_imported_user_function(UserExternalName {
            namespace: HELPER_EXTERNAL_NAMESPACE,
            index: HelperId::primitive_storage_cage_base as u32,
        });
        let helper = builder.func.import_function(ExtFuncData {
            name: ExternalName::user(name),
            signature,
            colocated: false,
            patchable: false,
        });
        let storage = builder.ins().func_addr(pointer_type, helper);
        builder
            .ins()
            .load(pointer_type, memory_flags.runtime_metadata, storage, 0)
    }

    fn memory_bases(
        builder: &mut FunctionBuilder<'_>,
        instructions: &[DirectInstruction],
        configuration: Value,
        layout: &SerializedRuntimeLayout,
        memory_flags: WasmMemoryFlags,
        pointer_type: Type,
    ) -> Vec<(u32, Value)> {
        let mut indices = Vec::new();
        for instruction in instructions {
            if matches!(instruction.opcode, op::I64_LOAD | op::I64_STORE) {
                indices.push(instruction.memory_argument().memory_index);
            }
        }
        indices.sort_unstable();
        indices.dedup();
        if indices.is_empty() {
            return Vec::new();
        }

        let runtime_layout = RuntimeLayout::new(layout);
        let cage_base = Self::declare_cage_base(builder, pointer_type, memory_flags);
        let memory_instances = builder.ins().load(
            pointer_type,
            memory_flags.configuration,
            configuration,
            runtime_layout.memory_instances_offset,
        );
        let mut bases = Vec::with_capacity(indices.len());
        for memory_index in indices {
            let pointer_offset = builder
                .ins()
                .iconst(pointer_type, i64::from(memory_index) * i64::from(pointer_type.bytes()));
            let pointer_address = builder.ins().iadd(memory_instances, pointer_offset);
            let memory = builder
                .ins()
                .load(pointer_type, memory_flags.runtime_metadata, pointer_address, 0);
            let storage_offset = builder.ins().load(
                types::I64,
                memory_flags.runtime_metadata,
                memory,
                runtime_layout.memory_instance_data_offset + runtime_layout.memory_buffer_storage_offset_offset,
            );
            let storage_offset = if pointer_type == types::I64 {
                storage_offset
            } else {
                builder.ins().ireduce(pointer_type, storage_offset)
            };
            bases.push((memory_index, builder.ins().iadd(cage_base, storage_offset)));
        }
        bases
    }

    fn memory_address(
        builder: &mut FunctionBuilder<'_>,
        memory_bases: &[(u32, Value)],
        memory_index: u32,
        address: Value,
        offset: u64,
        pointer_type: Type,
    ) -> Result<Value, &'static str> {
        let base = memory_bases
            .iter()
            .find_map(|&(index, base)| (index == memory_index).then_some(base))
            .ok_or("missing direct memory base")?;
        let address = builder.ins().uextend(types::I64, address);
        let address = if pointer_type == types::I64 {
            address
        } else {
            builder.ins().ireduce(pointer_type, address)
        };
        let address = builder.ins().iadd(base, address);
        let offset = builder.ins().iconst(pointer_type, offset as i64);
        Ok(builder.ins().iadd(address, offset))
    }

    pub(crate) fn compile_to_bytes(
        instructions: &[DirectInstruction],
        _branch_targets: &[u32],
        layout: &SerializedRuntimeLayout,
        options: FunctionCompilationOptions,
        declared_local_types: &[DirectValueType],
        function_types: &[WasmFunctionType<'_>],
    ) -> Result<CompiledFunction, &'static str> {
        let function_index = usize::try_from(options.function_index).map_err(|_| "direct function index overflow")?;
        let num_params = usize::try_from(options.num_params).map_err(|_| "direct parameter count overflow")?;
        let num_locals = usize::try_from(options.num_locals).map_err(|_| "direct local count overflow")?;
        let result_arity = usize::try_from(options.result_arity).map_err(|_| "direct result count overflow")?;
        let function_type = *function_types
            .get(function_index)
            .ok_or("missing direct function type")?;
        if function_type.parameters.len() != num_params || function_type.results.len() != result_arity {
            return Err("direct function type does not match compilation options");
        }
        if function_type.parameters.len() + declared_local_types.len() != num_locals {
            return Err("direct local type count does not match compilation options");
        }

        let mut flag_builder = settings::builder();
        flag_builder.set("opt_level", "speed").unwrap();
        flag_builder.set("is_pic", "false").unwrap();
        let isa = cranelift_native::builder()
            .map_err(|_| "unsupported host architecture")?
            .finish(settings::Flags::new(flag_builder))
            .map_err(|_| "failed to build ISA")?;
        let pointer_type = isa.pointer_type();
        let signature = Self::clean_signature(&*isa, function_type)?;
        let mut function = Function::with_name_signature(
            UserFuncName::user(WASM_FUNCTION_EXTERNAL_NAMESPACE, options.function_index),
            signature,
        );
        let memory_flags = WasmMemoryFlags::new(&mut function);
        let mut builder_context = FunctionBuilderContext::new();
        let mut builder = FunctionBuilder::new(&mut function, &mut builder_context);
        let entry = builder.create_block();
        builder.append_block_params_for_function_params(entry);
        builder.switch_to_block(entry);
        builder.seal_block(entry);

        let entry_parameters = builder.block_params(entry).to_vec();
        let configuration = entry_parameters[0];
        let mut local_types = function_type
            .parameters
            .iter()
            .copied()
            .map(wasm_abi_type)
            .collect::<Result<Vec<_>, _>>()?;
        local_types.extend(
            declared_local_types
                .iter()
                .copied()
                .map(Self::direct_type)
                .collect::<Result<Vec<_>, _>>()?,
        );
        let local_variables = local_types
            .iter()
            .copied()
            .map(|ty| builder.declare_var(ty))
            .collect::<Vec<Variable>>();
        for (parameter_index, variable) in local_variables.iter().take(function_type.parameters.len()).enumerate() {
            builder.def_var(*variable, entry_parameters[parameter_index + 1]);
        }
        for (&variable, &ty) in local_variables
            .iter()
            .skip(function_type.parameters.len())
            .zip(local_types.iter().skip(function_type.parameters.len()))
        {
            let zero = Self::zero(&mut builder, ty)?;
            builder.def_var(variable, zero);
        }

        let memory_bases = Self::memory_bases(
            &mut builder,
            instructions,
            configuration,
            layout,
            memory_flags,
            pointer_type,
        );
        let mut operand_stack = Vec::new();
        let mut control_stack = Vec::new();
        let mut function_ended = false;

        for instruction in instructions {
            match instruction.opcode {
                op::BLOCK | op::LOOP => {
                    let arguments = instruction.structured()?;
                    let result_types = Self::block_results(arguments.block_type)?;
                    let continuation = builder.create_block();
                    for &result_type in &result_types {
                        builder.append_block_param(continuation, result_type);
                    }

                    let kind = if instruction.opcode == op::LOOP {
                        ControlKind::Loop
                    } else {
                        ControlKind::Block
                    };
                    let branch_target = if kind == ControlKind::Loop {
                        let header = builder.create_block();
                        builder.ins().jump(header, &[]);
                        builder.switch_to_block(header);
                        header
                    } else {
                        continuation
                    };
                    control_stack.push(ControlFrame {
                        kind,
                        branch_target,
                        continuation,
                        entry_stack: operand_stack.clone(),
                        branch_types: if kind == ControlKind::Loop {
                            Vec::new()
                        } else {
                            result_types.clone()
                        },
                        result_types,
                        continuation_has_predecessor: false,
                    });
                }
                op::END => {
                    if let Some(mut frame) = control_stack.pop() {
                        let arguments = Self::result_arguments(&builder, &mut operand_stack, &frame.result_types)?;
                        operand_stack = frame.entry_stack;
                        builder.ins().jump(frame.continuation, &arguments);
                        frame.continuation_has_predecessor = true;
                        if frame.kind == ControlKind::Loop {
                            builder.seal_block(frame.branch_target);
                        }
                        builder.switch_to_block(frame.continuation);
                        builder.seal_block(frame.continuation);
                        if frame.continuation_has_predecessor {
                            operand_stack.extend_from_slice(builder.block_params(frame.continuation));
                        }
                    } else {
                        let result_types = function_type
                            .results
                            .iter()
                            .copied()
                            .map(wasm_abi_type)
                            .collect::<Result<Vec<_>, _>>()?;
                        let results = Self::result_arguments(&builder, &mut operand_stack, &result_types)?;
                        let results = results
                            .into_iter()
                            .map(|argument| match argument {
                                BlockArg::Value(value) => Ok(value),
                                _ => Err("unsupported direct return block argument"),
                            })
                            .collect::<Result<Vec<_>, _>>()?;
                        builder.ins().return_(&results);
                        function_ended = true;
                    }
                }
                op::BR_IF => {
                    let condition = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let label_depth = instruction.label_depth()?;
                    let frame_index = control_stack
                        .len()
                        .checked_sub(label_depth + 1)
                        .ok_or("invalid direct branch label")?;
                    let arguments =
                        Self::branch_arguments(&builder, &operand_stack, &control_stack[frame_index].branch_types)?;
                    let target = control_stack[frame_index].branch_target;
                    if control_stack[frame_index].kind == ControlKind::Block {
                        control_stack[frame_index].continuation_has_predecessor = true;
                    }
                    let fallthrough = builder.create_block();
                    let condition = builder.ins().icmp_imm_s(IntCC::NotEqual, condition, 0);
                    builder.ins().brif(condition, target, &arguments, fallthrough, &[]);
                    builder.switch_to_block(fallthrough);
                    builder.seal_block(fallthrough);
                }
                op::I32_CONST => {
                    let immediate = instruction.i32_constant();
                    operand_stack.push(builder.ins().iconst(types::I32, i64::from(immediate)));
                }
                op::I64_CONST => {
                    let immediate = instruction.i64_constant();
                    operand_stack.push(builder.ins().iconst(types::I64, immediate));
                }
                op::LOCAL_GET => {
                    let index = instruction.local_index()?;
                    let variable = *local_variables.get(index).ok_or("invalid direct local index")?;
                    operand_stack.push(builder.use_var(variable));
                }
                op::LOCAL_SET | op::LOCAL_TEE => {
                    let index = instruction.local_index()?;
                    let variable = *local_variables.get(index).ok_or("invalid direct local index")?;
                    let expected_type = *local_types.get(index).ok_or("invalid direct local type")?;
                    let value = if instruction.opcode == op::LOCAL_TEE {
                        *operand_stack.last().ok_or("direct operand stack underflow")?
                    } else {
                        Self::pop_expected(&builder, &mut operand_stack, expected_type)?
                    };
                    if Self::value_type(&builder, value) != expected_type {
                        return Err("direct local type mismatch");
                    }
                    builder.def_var(variable, value);
                }
                op::I32_ADD | op::I32_SHL => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = if instruction.opcode == op::I32_ADD {
                        builder.ins().iadd(lhs, rhs)
                    } else {
                        builder.ins().ishl(lhs, rhs)
                    };
                    operand_stack.push(result);
                }
                op::I32_EQZ => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = builder.ins().icmp_imm_s(IntCC::Equal, value, 0);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::I32_NE => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = builder.ins().icmp(IntCC::NotEqual, lhs, rhs);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::I64_ADD | op::I64_AND | op::I64_OR | op::I64_XOR | op::I64_SHL | op::I64_SHRU | op::I64_ROTL => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let result = match instruction.opcode {
                        op::I64_ADD => builder.ins().iadd(lhs, rhs),
                        op::I64_AND => builder.ins().band(lhs, rhs),
                        op::I64_OR => builder.ins().bor(lhs, rhs),
                        op::I64_XOR => builder.ins().bxor(lhs, rhs),
                        op::I64_SHL => builder.ins().ishl(lhs, rhs),
                        op::I64_SHRU => builder.ins().ushr(lhs, rhs),
                        op::I64_ROTL => builder.ins().rotl(lhs, rhs),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::I64_LOAD => {
                    let address = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let memory = instruction.memory_argument();
                    let address = Self::memory_address(
                        &mut builder,
                        &memory_bases,
                        memory.memory_index,
                        address,
                        memory.offset,
                        pointer_type,
                    )?;
                    operand_stack.push(builder.ins().load(types::I64, memory_flags.linear_memory, address, 0));
                }
                op::I64_STORE => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let address = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let memory = instruction.memory_argument();
                    let address = Self::memory_address(
                        &mut builder,
                        &memory_bases,
                        memory.memory_index,
                        address,
                        memory.offset,
                        pointer_type,
                    )?;
                    builder.ins().store(memory_flags.linear_memory, value, address, 0);
                }
                _ => return Err("unsupported direct instruction"),
            }
        }

        if !function_ended || !control_stack.is_empty() {
            return Err("unterminated direct function");
        }
        builder.finalize(isa.frontend_config());
        let body = compile_function(&*isa, function)?;
        Ok(CompiledFunction {
            code: body.code,
            native_entry_offset: 0,
            relocs: body.relocs,
            traps: body.traps,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::common::I32_KIND;
    use super::*;
    use crate::DirectBlockType;
    use crate::DirectBlockTypeKind;
    use crate::DirectInstructionArguments;
    use crate::DirectMemoryInstructionArguments;
    use crate::DirectStructuredInstructionArguments;
    use crate::DirectValueTypeKind;

    fn instruction(opcode: u64, arguments: DirectInstructionArguments) -> DirectInstruction {
        DirectInstruction { opcode, arguments }
    }

    fn no_arguments(opcode: u64) -> DirectInstruction {
        instruction(opcode, DirectInstructionArguments { i64_constant: 0 })
    }

    fn local(opcode: u64, index: u32) -> DirectInstruction {
        instruction(opcode, DirectInstructionArguments { local_index: index })
    }

    fn i32_constant(value: i32) -> DirectInstruction {
        instruction(op::I32_CONST, DirectInstructionArguments { i32_constant: value })
    }

    fn structured(opcode: u64) -> DirectInstruction {
        instruction(
            opcode,
            DirectInstructionArguments {
                structured: DirectStructuredInstructionArguments {
                    block_type: DirectBlockType {
                        kind: DirectBlockTypeKind::Empty as u32,
                        value_type: DirectValueType {
                            kind: DirectValueTypeKind::I32 as u32,
                            type_index: 0,
                            nullable: 0,
                        },
                        type_index: 0,
                    },
                    end_ip: 0,
                    else_ip: u32::MAX,
                },
            },
        )
    }

    fn memory(opcode: u64) -> DirectInstruction {
        instruction(
            opcode,
            DirectInstructionArguments {
                memory: DirectMemoryInstructionArguments {
                    align: 3,
                    memory_index: 0,
                    offset: 0,
                },
            },
        )
    }

    #[test]
    fn compiles_typed_loop_locals_and_memory() {
        let instructions = [
            local(op::LOCAL_GET, 0),
            memory(op::I64_LOAD),
            local(op::LOCAL_SET, 1),
            structured(op::BLOCK),
            structured(op::LOOP),
            local(op::LOCAL_GET, 0),
            i32_constant(1),
            no_arguments(op::I32_ADD),
            local(op::LOCAL_TEE, 0),
            i32_constant(3),
            no_arguments(op::I32_NE),
            instruction(op::BR_IF, DirectInstructionArguments { label_index: 0 }),
            no_arguments(op::END),
            no_arguments(op::END),
            local(op::LOCAL_GET, 0),
            local(op::LOCAL_GET, 1),
            memory(op::I64_STORE),
            no_arguments(op::END),
        ];
        let declared_local_types = [DirectValueType {
            kind: DirectValueTypeKind::I64 as u32,
            type_index: 0,
            nullable: 0,
        }];
        let parameters = [I32_KIND];
        let function_types = [WasmFunctionType {
            parameters: &parameters,
            results: &[],
        }];
        let compiled = DirectCompiler::compile_to_bytes(
            &instructions,
            &[],
            &SerializedRuntimeLayout::default(),
            FunctionCompilationOptions {
                result_arity: 0,
                num_locals: 2,
                num_params: 1,
                function_index: 0,
                max_call_rec_size: 0,
            },
            &declared_local_types,
            &function_types,
        )
        .expect("direct compilation should succeed");

        assert!(!compiled.code.is_empty());
        assert!(!compiled.relocs.is_empty());
    }
}
