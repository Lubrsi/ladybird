/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::CompiledFunction;
use crate::CraneliftUserTrapCode;
use crate::DirectCompilerInput;
use crate::DirectFunctionType;
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
use cranelift_codegen::ir::FuncRef;
use cranelift_codegen::ir::Function;
use cranelift_codegen::ir::InstBuilder;
use cranelift_codegen::ir::JumpTableData;
use cranelift_codegen::ir::MemFlagsData as MemFlags;
use cranelift_codegen::ir::SigRef;
use cranelift_codegen::ir::Signature;
use cranelift_codegen::ir::Type;
use cranelift_codegen::ir::UserExternalName;
use cranelift_codegen::ir::UserFuncName;
use cranelift_codegen::ir::Value;
use cranelift_codegen::ir::condcodes::FloatCC;
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::immediates::Ieee32;
use cranelift_codegen::ir::immediates::Ieee64;
use cranelift_codegen::ir::types;
use cranelift_codegen::isa::TargetIsa;
use cranelift_codegen::settings::Configurable;
use cranelift_codegen::settings::{self};
use cranelift_frontend::FunctionBuilder;
use cranelift_frontend::FunctionBuilderContext;
use cranelift_frontend::Variable;
use std::collections::HashMap;

use super::common::HELPER_EXTERNAL_NAMESPACE;
use super::common::NativeIndirectCallContext;
use super::common::NativeIndirectCallLayout;
use super::common::RuntimeLayout;
use super::common::WASM_FUNCTION_EXTERNAL_NAMESPACE;
use super::common::WasmMemoryFlags;
use super::common::compile_function;
use super::common::declare_helper;
use super::common::emit_native_indirect_call_target;
use super::common::user_trap_code;
use super::common::wasm_abi_type;
use super::direct_input::BlockType;
use super::direct_input::ValueType as CheckedValueType;
use super::direct_input::ValueTypeKind;
use super::op;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ControlKind {
    Block,
    Loop,
    If,
}

struct ControlFrame {
    kind: ControlKind,
    branch_target: Block,
    continuation: Block,
    else_block: Option<Block>,
    entry_stack: Vec<Value>,
    branch_types: Vec<Type>,
    result_types: Vec<Type>,
    continuation_has_predecessor: bool,
    entry_is_reachable: bool,
    else_was_seen: bool,
}

struct DirectIndirectCallHelpers {
    check_type_signature: SigRef,
    check_type: FuncRef,
    fallback_signature: SigRef,
    fallback: FuncRef,
}

struct DirectRuntimeFallbackHelpers {
    current_interpreter_signature: SigRef,
    current_interpreter: FuncRef,
    raise_trap_signature: SigRef,
    raise_trap: FuncRef,
}

struct DirectBulkMemoryHelpers {
    memory_copy_signature: SigRef,
    memory_copy: FuncRef,
    memory_fill_signature: SigRef,
    memory_fill: FuncRef,
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

    fn clean_direct_signature(
        isa: &dyn TargetIsa,
        function_type: DirectFunctionType<'_>,
    ) -> Result<Signature, &'static str> {
        let mut signature = Signature::new(isa.default_call_conv());
        signature.params.push(AbiParam::new(isa.pointer_type()));
        for &parameter in function_type.parameters {
            signature.params.push(AbiParam::new(Self::direct_type(parameter)?));
        }
        for &result in function_type.results {
            signature.returns.push(AbiParam::new(Self::direct_type(result)?));
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

    fn result_values(
        builder: &FunctionBuilder<'_>,
        operand_stack: &mut Vec<Value>,
        expected: &[Type],
    ) -> Result<Vec<Value>, &'static str> {
        Self::result_arguments(builder, operand_stack, expected)?
            .into_iter()
            .map(|argument| match argument {
                BlockArg::Value(value) => Ok(value),
                _ => Err("unsupported direct return block argument"),
            })
            .collect()
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
            if (op::I32_LOAD..=op::I64_STORE32).contains(&instruction.opcode) {
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

    fn global_instances(
        builder: &mut FunctionBuilder<'_>,
        instructions: &[DirectInstruction],
        global_types: &[DirectValueType],
        configuration: Value,
        layout: &SerializedRuntimeLayout,
        memory_flags: WasmMemoryFlags,
        pointer_type: Type,
    ) -> Result<Vec<(usize, Value, Type)>, &'static str> {
        let mut indices = Vec::new();
        for instruction in instructions {
            if matches!(instruction.opcode, op::GLOBAL_GET | op::GLOBAL_SET) {
                indices.push(instruction.global_index()?);
            }
        }
        indices.sort_unstable();
        indices.dedup();
        if indices.is_empty() {
            return Ok(Vec::new());
        }

        let runtime_layout = RuntimeLayout::new(layout);
        let globals = builder.ins().load(
            pointer_type,
            memory_flags.configuration,
            configuration,
            runtime_layout.global_instances_offset,
        );
        let mut instances = Vec::with_capacity(indices.len());
        for index in indices {
            let global_type = global_types.get(index).ok_or("missing direct global type")?;
            let global_type = Self::direct_type(*global_type)?;
            let pointer_offset = builder
                .ins()
                .iconst(pointer_type, index as i64 * i64::from(pointer_type.bytes()));
            let pointer_address = builder.ins().iadd(globals, pointer_offset);
            let global = builder
                .ins()
                .load(pointer_type, memory_flags.globals, pointer_address, 0);
            instances.push((index, global, global_type));
        }
        Ok(instances)
    }

    fn global_instance(
        global_instances: &[(usize, Value, Type)],
        global_index: usize,
    ) -> Result<(Value, Type), &'static str> {
        global_instances
            .iter()
            .find_map(|&(index, instance, ty)| (index == global_index).then_some((instance, ty)))
            .ok_or("missing direct global instance")
    }

    fn value_payload(builder: &mut FunctionBuilder<'_>, value: Value, ty: Type) -> Result<Value, &'static str> {
        match ty {
            types::I32 => Ok(builder.ins().uextend(types::I64, value)),
            types::I64 => Ok(value),
            types::F32 => {
                let bits = builder.ins().bitcast(types::I32, MemFlags::new(), value);
                Ok(builder.ins().uextend(types::I64, bits))
            }
            types::F64 => Ok(builder.ins().bitcast(types::I64, MemFlags::new(), value)),
            _ => Err("unsupported direct global type"),
        }
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
        input: DirectCompilerInput<'_>,
        layout: &SerializedRuntimeLayout,
        options: FunctionCompilationOptions,
    ) -> Result<CompiledFunction, &'static str> {
        let DirectCompilerInput {
            instructions,
            branch_targets,
            local_types: declared_local_types,
            function_types,
            module_types,
            global_types,
        } = input;
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
        let global_instances = Self::global_instances(
            &mut builder,
            instructions,
            global_types,
            configuration,
            layout,
            memory_flags,
            pointer_type,
        )?;
        let has_indirect_calls = instructions
            .iter()
            .any(|instruction| instruction.opcode == op::CALL_INDIRECT);
        let has_bulk_memory = instructions
            .iter()
            .any(|instruction| matches!(instruction.opcode, op::MEMORY_COPY | op::MEMORY_FILL));
        let runtime_fallback_helpers = if has_indirect_calls || has_bulk_memory {
            let mut current_interpreter_signature = Signature::new(isa.default_call_conv());
            current_interpreter_signature.returns.push(AbiParam::new(pointer_type));
            let current_interpreter_signature = builder.import_signature(current_interpreter_signature);
            let raise_trap_signature = builder.import_signature(Signature::new(isa.default_call_conv()));
            Some(DirectRuntimeFallbackHelpers {
                current_interpreter_signature,
                current_interpreter: declare_helper(
                    &mut builder,
                    current_interpreter_signature,
                    HelperId::current_interpreter,
                ),
                raise_trap_signature,
                raise_trap: declare_helper(&mut builder, raise_trap_signature, HelperId::raise_trap),
            })
        } else {
            None
        };
        let indirect_call_helpers = if has_indirect_calls {
            let mut check_type_signature = Signature::new(isa.default_call_conv());
            check_type_signature.params.push(AbiParam::new(pointer_type));
            check_type_signature.params.push(AbiParam::new(pointer_type));
            check_type_signature.returns.push(AbiParam::new(types::I32));
            let check_type_signature = builder.import_signature(check_type_signature);

            let mut fallback_signature = Signature::new(isa.default_call_conv());
            fallback_signature.params.push(AbiParam::new(pointer_type));
            fallback_signature.params.push(AbiParam::new(pointer_type));
            fallback_signature.params.push(AbiParam::new(types::I32));
            fallback_signature.params.push(AbiParam::new(types::I32));
            fallback_signature.params.push(AbiParam::new(types::I64));
            fallback_signature.returns.push(AbiParam::new(types::I32));
            let fallback_signature = builder.import_signature(fallback_signature);

            Some(DirectIndirectCallHelpers {
                check_type_signature,
                check_type: declare_helper(&mut builder, check_type_signature, HelperId::check_indirect_type),
                fallback_signature,
                fallback: declare_helper(&mut builder, fallback_signature, HelperId::call_indirect_with_record),
            })
        } else {
            None
        };
        let bulk_memory_helpers = if has_bulk_memory {
            let mut memory_copy_signature = Signature::new(isa.default_call_conv());
            memory_copy_signature.params.push(AbiParam::new(pointer_type));
            memory_copy_signature.params.push(AbiParam::new(pointer_type));
            memory_copy_signature.params.push(AbiParam::new(types::I32));
            memory_copy_signature.params.push(AbiParam::new(types::I32));
            memory_copy_signature.params.push(AbiParam::new(types::I32));
            memory_copy_signature.params.push(AbiParam::new(types::I32));
            memory_copy_signature.params.push(AbiParam::new(types::I32));
            memory_copy_signature.returns.push(AbiParam::new(types::I32));
            let memory_copy_signature = builder.import_signature(memory_copy_signature);

            let mut memory_fill_signature = Signature::new(isa.default_call_conv());
            memory_fill_signature.params.push(AbiParam::new(pointer_type));
            memory_fill_signature.params.push(AbiParam::new(pointer_type));
            memory_fill_signature.params.push(AbiParam::new(types::I32));
            memory_fill_signature.params.push(AbiParam::new(types::I32));
            memory_fill_signature.params.push(AbiParam::new(types::I32));
            memory_fill_signature.params.push(AbiParam::new(types::I32));
            memory_fill_signature.returns.push(AbiParam::new(types::I32));
            let memory_fill_signature = builder.import_signature(memory_fill_signature);

            Some(DirectBulkMemoryHelpers {
                memory_copy_signature,
                memory_copy: declare_helper(&mut builder, memory_copy_signature, HelperId::memory_copy),
                memory_fill_signature,
                memory_fill: declare_helper(&mut builder, memory_fill_signature, HelperId::memory_fill),
            })
        } else {
            None
        };
        let mut indirect_call_signatures = HashMap::new();
        for instruction in instructions
            .iter()
            .filter(|instruction| instruction.opcode == op::CALL_INDIRECT)
        {
            let argument = instruction.indirect_call_argument()?;
            if indirect_call_signatures.contains_key(&argument.type_index) {
                continue;
            }
            let target_type = module_types
                .get(argument.type_index)
                .copied()
                .flatten()
                .ok_or("direct indirect-call type is not a function type")?;
            let signature = builder.import_signature(Self::clean_direct_signature(&*isa, target_type)?);
            indirect_call_signatures.insert(argument.type_index, signature);
        }
        let mut direct_call_targets = HashMap::new();
        for instruction in instructions.iter().filter(|instruction| instruction.opcode == op::CALL) {
            let target_index = instruction.function_index()?;
            if direct_call_targets.contains_key(&target_index) {
                continue;
            }
            let target_type = *function_types
                .get(target_index)
                .ok_or("missing direct-call function type")?;
            let target_signature = builder.import_signature(Self::clean_signature(&*isa, target_type)?);
            let target_index_u32 = u32::try_from(target_index).map_err(|_| "direct-call function index overflow")?;
            let user_ref = builder.func.declare_imported_user_function(UserExternalName {
                namespace: WASM_FUNCTION_EXTERNAL_NAMESPACE,
                index: target_index_u32,
            });
            let target = builder.func.import_function(ExtFuncData {
                name: ExternalName::user(user_ref),
                signature: target_signature,
                colocated: true,
                patchable: false,
            });
            direct_call_targets.insert(target_index, target);
        }
        let mut operand_stack = Vec::new();
        let mut control_stack = Vec::new();
        let mut function_ended = false;
        let mut is_reachable = true;

        for instruction in instructions {
            if !is_reachable && !matches!(instruction.opcode, op::BLOCK | op::LOOP | op::IF | op::ELSE | op::END) {
                continue;
            }
            match instruction.opcode {
                op::BLOCK | op::LOOP | op::IF => {
                    let arguments = instruction.structured()?;
                    let result_types = Self::block_results(arguments.block_type)?;
                    let continuation = builder.create_block();
                    for &result_type in &result_types {
                        builder.append_block_param(continuation, result_type);
                    }

                    let kind = match instruction.opcode {
                        op::BLOCK => ControlKind::Block,
                        op::LOOP => ControlKind::Loop,
                        op::IF => ControlKind::If,
                        _ => unreachable!(),
                    };
                    let mut else_block = None;
                    let branch_target = match kind {
                        ControlKind::Block => continuation,
                        ControlKind::Loop => {
                            let header = builder.create_block();
                            if is_reachable {
                                builder.ins().jump(header, &[]);
                                builder.switch_to_block(header);
                            }
                            header
                        }
                        ControlKind::If => {
                            let condition = if is_reachable {
                                Some(Self::pop_expected(&builder, &mut operand_stack, types::I32)?)
                            } else {
                                None
                            };
                            let then_block = builder.create_block();
                            let false_target = if arguments.else_ip.is_some() {
                                let block = builder.create_block();
                                else_block = Some(block);
                                block
                            } else {
                                continuation
                            };
                            if let Some(condition) = condition {
                                let condition = builder.ins().icmp_imm_s(IntCC::NotEqual, condition, 0);
                                builder.ins().brif(condition, then_block, &[], false_target, &[]);
                                builder.switch_to_block(then_block);
                                builder.seal_block(then_block);
                            }
                            continuation
                        }
                    };
                    let continuation_has_predecessor =
                        kind == ControlKind::If && arguments.else_ip.is_none() && is_reachable;
                    control_stack.push(ControlFrame {
                        kind,
                        branch_target,
                        continuation,
                        else_block,
                        entry_stack: operand_stack.clone(),
                        branch_types: if kind == ControlKind::Loop {
                            Vec::new()
                        } else {
                            result_types.clone()
                        },
                        result_types,
                        continuation_has_predecessor,
                        entry_is_reachable: is_reachable,
                        else_was_seen: false,
                    });
                }
                op::ELSE => {
                    let frame = control_stack.last_mut().ok_or("direct else without control frame")?;
                    if frame.kind != ControlKind::If || frame.else_was_seen {
                        return Err("invalid direct else");
                    }
                    if is_reachable {
                        let arguments = Self::result_arguments(&builder, &mut operand_stack, &frame.result_types)?;
                        builder.ins().jump(frame.continuation, &arguments);
                        frame.continuation_has_predecessor = true;
                    }
                    operand_stack = frame.entry_stack.clone();
                    is_reachable = frame.entry_is_reachable;
                    if is_reachable {
                        let else_block = frame.else_block.ok_or("missing direct else block")?;
                        builder.switch_to_block(else_block);
                        builder.seal_block(else_block);
                    }
                    frame.else_was_seen = true;
                }
                op::END => {
                    if let Some(mut frame) = control_stack.pop() {
                        if frame.else_block.is_some() && !frame.else_was_seen {
                            return Err("direct if is missing else");
                        }
                        if is_reachable {
                            let arguments = Self::result_arguments(&builder, &mut operand_stack, &frame.result_types)?;
                            builder.ins().jump(frame.continuation, &arguments);
                            frame.continuation_has_predecessor = true;
                        }
                        operand_stack = frame.entry_stack;
                        if frame.kind == ControlKind::Loop {
                            builder.seal_block(frame.branch_target);
                        }
                        builder.seal_block(frame.continuation);
                        if frame.continuation_has_predecessor {
                            builder.switch_to_block(frame.continuation);
                            operand_stack.extend_from_slice(builder.block_params(frame.continuation));
                        }
                        is_reachable = frame.continuation_has_predecessor;
                    } else {
                        if is_reachable {
                            let result_types = function_type
                                .results
                                .iter()
                                .copied()
                                .map(wasm_abi_type)
                                .collect::<Result<Vec<_>, _>>()?;
                            let results = Self::result_values(&builder, &mut operand_stack, &result_types)?;
                            builder.ins().return_(&results);
                        }
                        function_ended = true;
                    }
                }
                op::BR => {
                    if !is_reachable {
                        continue;
                    }
                    let label_depth = instruction.label_depth()?;
                    if let Some(frame_index) = control_stack.len().checked_sub(label_depth + 1) {
                        let arguments =
                            Self::branch_arguments(&builder, &operand_stack, &control_stack[frame_index].branch_types)?;
                        let frame = &mut control_stack[frame_index];
                        if frame.kind != ControlKind::Loop {
                            frame.continuation_has_predecessor = true;
                        }
                        builder.ins().jump(frame.branch_target, &arguments);
                    } else if label_depth == control_stack.len() {
                        let result_types = function_type
                            .results
                            .iter()
                            .copied()
                            .map(wasm_abi_type)
                            .collect::<Result<Vec<_>, _>>()?;
                        let results = Self::result_values(&builder, &mut operand_stack, &result_types)?;
                        builder.ins().return_(&results);
                    } else {
                        return Err("invalid direct branch label");
                    }
                    is_reachable = false;
                }
                op::BR_IF => {
                    if !is_reachable {
                        continue;
                    }
                    let condition = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let label_depth = instruction.label_depth()?;
                    let condition = builder.ins().icmp_imm_s(IntCC::NotEqual, condition, 0);
                    let fallthrough = builder.create_block();
                    if let Some(frame_index) = control_stack.len().checked_sub(label_depth + 1) {
                        let arguments =
                            Self::branch_arguments(&builder, &operand_stack, &control_stack[frame_index].branch_types)?;
                        let frame = &mut control_stack[frame_index];
                        if frame.kind != ControlKind::Loop {
                            frame.continuation_has_predecessor = true;
                        }
                        builder
                            .ins()
                            .brif(condition, frame.branch_target, &arguments, fallthrough, &[]);
                    } else if label_depth == control_stack.len() {
                        let taken = builder.create_block();
                        builder.ins().brif(condition, taken, &[], fallthrough, &[]);
                        builder.switch_to_block(taken);
                        builder.seal_block(taken);
                        let result_types = function_type
                            .results
                            .iter()
                            .copied()
                            .map(wasm_abi_type)
                            .collect::<Result<Vec<_>, _>>()?;
                        let mut return_stack = operand_stack.clone();
                        let results = Self::result_values(&builder, &mut return_stack, &result_types)?;
                        builder.ins().return_(&results);
                    } else {
                        return Err("invalid direct branch label");
                    }
                    builder.switch_to_block(fallthrough);
                    builder.seal_block(fallthrough);
                }
                op::BR_TABLE => {
                    if !is_reachable {
                        continue;
                    }
                    let selector = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let argument = instruction.table_branch_argument(branch_targets)?;
                    let mut return_block = None;
                    let mut return_values = None;

                    let mut make_destination = |label_depth: usize,
                                                builder: &mut FunctionBuilder<'_>,
                                                control_stack: &mut Vec<ControlFrame>|
                     -> Result<_, &'static str> {
                        if let Some(frame_index) = control_stack.len().checked_sub(label_depth + 1) {
                            let arguments = Self::branch_arguments(
                                builder,
                                &operand_stack,
                                &control_stack[frame_index].branch_types,
                            )?;
                            let frame = &mut control_stack[frame_index];
                            if frame.kind != ControlKind::Loop {
                                frame.continuation_has_predecessor = true;
                            }
                            return Ok(builder.func.dfg.block_call(frame.branch_target, &arguments));
                        }
                        if label_depth != control_stack.len() {
                            return Err("invalid direct branch-table label");
                        }

                        let block = *return_block.get_or_insert_with(|| builder.create_block());
                        if return_values.is_none() {
                            let result_types = function_type
                                .results
                                .iter()
                                .copied()
                                .map(wasm_abi_type)
                                .collect::<Result<Vec<_>, _>>()?;
                            let mut stack = operand_stack.clone();
                            return_values = Some(Self::result_values(builder, &mut stack, &result_types)?);
                        }
                        Ok(builder.func.dfg.block_call(block, &[]))
                    };

                    let default_destination =
                        make_destination(argument.default_target, &mut builder, &mut control_stack)?;
                    let destinations = argument
                        .targets
                        .iter()
                        .map(|&label_depth| {
                            let label_depth =
                                usize::try_from(label_depth).map_err(|_| "direct branch-table label depth overflow")?;
                            make_destination(label_depth, &mut builder, &mut control_stack)
                        })
                        .collect::<Result<Vec<_>, _>>()?;
                    let jump_table = builder.create_jump_table(JumpTableData::new(default_destination, &destinations));
                    builder.ins().br_table(selector, jump_table);

                    if let Some(return_block) = return_block {
                        builder.switch_to_block(return_block);
                        builder.seal_block(return_block);
                        builder
                            .ins()
                            .return_(&return_values.ok_or("missing direct branch-table return values")?);
                    }
                    is_reachable = false;
                }
                op::RETURN => {
                    if !is_reachable {
                        continue;
                    }
                    let result_types = function_type
                        .results
                        .iter()
                        .copied()
                        .map(wasm_abi_type)
                        .collect::<Result<Vec<_>, _>>()?;
                    let results = Self::result_values(&builder, &mut operand_stack, &result_types)?;
                    builder.ins().return_(&results);
                    is_reachable = false;
                }
                op::UNREACHABLE => {
                    if is_reachable {
                        builder.ins().trap(user_trap_code(CraneliftUserTrapCode::Unreachable));
                        is_reachable = false;
                    }
                }
                op::NOP => {}
                op::DROP => {
                    operand_stack.pop().ok_or("direct operand stack underflow")?;
                }
                op::SELECT | op::SELECT_TYPED => {
                    let condition = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let rhs = operand_stack.pop().ok_or("direct operand stack underflow")?;
                    let lhs = operand_stack.pop().ok_or("direct operand stack underflow")?;
                    if Self::value_type(&builder, lhs) != Self::value_type(&builder, rhs) {
                        return Err("direct select operand type mismatch");
                    }
                    let condition = builder.ins().icmp_imm_s(IntCC::NotEqual, condition, 0);
                    operand_stack.push(builder.ins().select(condition, lhs, rhs));
                }
                op::I32_CONST => {
                    let immediate = instruction.i32_constant();
                    operand_stack.push(builder.ins().iconst(types::I32, i64::from(immediate)));
                }
                op::I64_CONST => {
                    let immediate = instruction.i64_constant();
                    operand_stack.push(builder.ins().iconst(types::I64, immediate));
                }
                op::F32_CONST => {
                    let immediate = Ieee32::with_bits(instruction.f32_constant_bits());
                    operand_stack.push(builder.ins().f32const(immediate));
                }
                op::F64_CONST => {
                    let immediate = Ieee64::with_bits(instruction.f64_constant_bits());
                    operand_stack.push(builder.ins().f64const(immediate));
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
                op::GLOBAL_GET => {
                    let global_index = instruction.global_index()?;
                    let (global, global_type) = Self::global_instance(&global_instances, global_index)?;
                    let runtime_layout = RuntimeLayout::new(layout);
                    operand_stack.push(builder.ins().load(
                        global_type,
                        memory_flags.globals,
                        global,
                        runtime_layout.global_instance_value_offset,
                    ));
                }
                op::GLOBAL_SET => {
                    let global_index = instruction.global_index()?;
                    let (global, global_type) = Self::global_instance(&global_instances, global_index)?;
                    let value = Self::pop_expected(&builder, &mut operand_stack, global_type)?;
                    let runtime_layout = RuntimeLayout::new(layout);
                    let stored_value = if global_type == types::I8X16 {
                        value
                    } else {
                        Self::value_payload(&mut builder, value, global_type)?
                    };
                    builder.ins().store(
                        memory_flags.globals,
                        stored_value,
                        global,
                        runtime_layout.global_instance_value_offset,
                    );
                    if global_type != types::I8X16 {
                        let zero = builder.ins().iconst(types::I64, 0);
                        builder.ins().store(
                            memory_flags.globals,
                            zero,
                            global,
                            runtime_layout.global_instance_value_offset + 8,
                        );
                    }
                }
                op::CALL => {
                    let target_index = instruction.function_index()?;
                    let target_type = *function_types
                        .get(target_index)
                        .ok_or("missing direct-call function type")?;
                    let parameter_types = target_type
                        .parameters
                        .iter()
                        .copied()
                        .map(wasm_abi_type)
                        .collect::<Result<Vec<_>, _>>()?;
                    let mut arguments = Self::result_values(&builder, &mut operand_stack, &parameter_types)?;
                    arguments.insert(0, configuration);
                    let target = *direct_call_targets
                        .get(&target_index)
                        .ok_or("missing direct-call target")?;
                    let call = builder.ins().call(target, &arguments);
                    operand_stack.extend_from_slice(builder.inst_results(call));
                }
                op::CALL_INDIRECT => {
                    let call_argument = instruction.indirect_call_argument()?;
                    let target_type = module_types
                        .get(call_argument.type_index)
                        .copied()
                        .flatten()
                        .ok_or("direct indirect-call type is not a function type")?;
                    if target_type.results.len() > 1 {
                        return Err("multi-value direct indirect-call fallback is not yet supported");
                    }

                    let raw_element_index = operand_stack.pop().ok_or("direct operand stack underflow")?;
                    let element_index = match Self::value_type(&builder, raw_element_index) {
                        types::I32 => builder.ins().uextend(types::I64, raw_element_index),
                        types::I64 => raw_element_index,
                        _ => return Err("direct indirect-call table index type mismatch"),
                    };
                    let parameter_types = target_type
                        .parameters
                        .iter()
                        .copied()
                        .map(Self::direct_type)
                        .collect::<Result<Vec<_>, _>>()?;
                    let arguments = Self::result_values(&builder, &mut operand_stack, &parameter_types)?;
                    let result_types = target_type
                        .results
                        .iter()
                        .copied()
                        .map(Self::direct_type)
                        .collect::<Result<Vec<_>, _>>()?;
                    let continuation = builder.create_block();
                    for &result_type in &result_types {
                        builder.append_block_param(continuation, result_type);
                    }

                    let runtime_layout = RuntimeLayout::new(layout);
                    let indirect_helpers = indirect_call_helpers
                        .as_ref()
                        .ok_or("missing direct indirect-call helpers")?;
                    let fallback_helpers = runtime_fallback_helpers
                        .as_ref()
                        .ok_or("missing direct runtime fallback helpers")?;
                    let target = emit_native_indirect_call_target(
                        &mut builder,
                        call_argument.table_index,
                        u32::try_from(call_argument.type_index).map_err(|_| "direct type index overflow")?,
                        configuration,
                        element_index,
                        NativeIndirectCallContext {
                            pointer_type,
                            check_type_signature: indirect_helpers.check_type_signature,
                            check_type_helper: indirect_helpers.check_type,
                            layout: NativeIndirectCallLayout {
                                table_instances: runtime_layout.table_instances_offset,
                                current_module: runtime_layout.current_module_offset,
                                current_canonical_types: runtime_layout.current_canonical_types_offset,
                                table_instance_size: runtime_layout.table_instance_size_offset,
                                table_instance_callables: runtime_layout.table_instance_callables_offset,
                                callable_defined_type: runtime_layout.callable_defined_type_offset,
                                callable_module: runtime_layout.callable_module_offset,
                                callable_compiled_instructions: runtime_layout.callable_compiled_instructions_offset,
                                compiled_instructions_native_entry: runtime_layout
                                    .compiled_instructions_direct_native_entry_offset,
                            },
                            memory_flags,
                        },
                    )?;

                    let native_entry = builder.block_params(target.native_call)[0];
                    let mut native_arguments = Vec::with_capacity(arguments.len() + 1);
                    native_arguments.push(configuration);
                    native_arguments.extend_from_slice(&arguments);
                    let signature = *indirect_call_signatures
                        .get(&call_argument.type_index)
                        .ok_or("missing direct indirect-call signature")?;
                    let call = builder.ins().call_indirect(signature, native_entry, &native_arguments);
                    let native_results = builder.inst_results(call).to_vec();
                    builder.ins().jump(
                        continuation,
                        &native_results.iter().copied().map(BlockArg::Value).collect::<Vec<_>>(),
                    );

                    builder.switch_to_block(target.fallback_call);
                    let call_record = builder.ins().load(
                        pointer_type,
                        memory_flags.configuration,
                        configuration,
                        runtime_layout.call_record_base_offset,
                    );
                    for (index, (&argument, &argument_type)) in arguments.iter().zip(&parameter_types).enumerate() {
                        let offset = i32::try_from(index * runtime_layout.value_size as usize)
                            .map_err(|_| "direct indirect-call argument offset overflow")?;
                        if argument_type == types::I8X16 {
                            builder
                                .ins()
                                .store(memory_flags.activation, argument, call_record, offset);
                        } else {
                            let payload = Self::value_payload(&mut builder, argument, argument_type)?;
                            builder
                                .ins()
                                .store(memory_flags.activation, payload, call_record, offset);
                            let zero = builder.ins().iconst(types::I64, 0);
                            builder
                                .ins()
                                .store(memory_flags.activation, zero, call_record, offset + 8);
                        }
                    }
                    let current_interpreter_address = builder
                        .ins()
                        .func_addr(pointer_type, fallback_helpers.current_interpreter);
                    let current_interpreter_call = builder.ins().call_indirect(
                        fallback_helpers.current_interpreter_signature,
                        current_interpreter_address,
                        &[],
                    );
                    let interpreter = builder.inst_results(current_interpreter_call)[0];
                    let fallback_address = builder.ins().func_addr(pointer_type, indirect_helpers.fallback);
                    let table_index = builder.ins().iconst(types::I32, i64::from(call_argument.table_index));
                    let type_index = builder.ins().iconst(
                        types::I32,
                        i64::try_from(call_argument.type_index).map_err(|_| "direct type index overflow")?,
                    );
                    let fallback_call = builder.ins().call_indirect(
                        indirect_helpers.fallback_signature,
                        fallback_address,
                        &[interpreter, configuration, table_index, type_index, element_index],
                    );
                    let status = builder.inst_results(fallback_call)[0];
                    let trapped = builder.ins().icmp_imm_s(IntCC::NotEqual, status, 0);
                    let trap_block = builder.create_block();
                    let fallback_return = builder.create_block();
                    builder.set_cold_block(trap_block);
                    builder.ins().brif(trapped, trap_block, &[], fallback_return, &[]);

                    builder.switch_to_block(trap_block);
                    builder.seal_block(trap_block);
                    let raise_trap_address = builder.ins().func_addr(pointer_type, fallback_helpers.raise_trap);
                    builder
                        .ins()
                        .call_indirect(fallback_helpers.raise_trap_signature, raise_trap_address, &[]);
                    builder.ins().trap(user_trap_code(CraneliftUserTrapCode::Unreachable));

                    builder.switch_to_block(fallback_return);
                    builder.seal_block(fallback_return);
                    let fallback_results = if let Some(&result_type) = result_types.first() {
                        vec![builder.ins().load(
                            result_type,
                            memory_flags.configuration,
                            configuration,
                            runtime_layout.compiled_call_result_scratch_offset,
                        )]
                    } else {
                        Vec::new()
                    };
                    builder.ins().jump(
                        continuation,
                        &fallback_results
                            .iter()
                            .copied()
                            .map(BlockArg::Value)
                            .collect::<Vec<_>>(),
                    );

                    builder.switch_to_block(continuation);
                    builder.seal_block(continuation);
                    operand_stack.extend_from_slice(builder.block_params(continuation));
                }
                op::I32_ADD
                | op::I32_SUB
                | op::I32_MUL
                | op::I32_AND
                | op::I32_OR
                | op::I32_XOR
                | op::I32_SHL
                | op::I32_SHRS
                | op::I32_SHRU
                | op::I32_ROTL
                | op::I32_ROTR
                | op::I32_DIVS
                | op::I32_DIVU
                | op::I32_REMS
                | op::I32_REMU => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = match instruction.opcode {
                        op::I32_ADD => builder.ins().iadd(lhs, rhs),
                        op::I32_SUB => builder.ins().isub(lhs, rhs),
                        op::I32_MUL => builder.ins().imul(lhs, rhs),
                        op::I32_AND => builder.ins().band(lhs, rhs),
                        op::I32_OR => builder.ins().bor(lhs, rhs),
                        op::I32_XOR => builder.ins().bxor(lhs, rhs),
                        op::I32_SHL => builder.ins().ishl(lhs, rhs),
                        op::I32_SHRS => builder.ins().sshr(lhs, rhs),
                        op::I32_SHRU => builder.ins().ushr(lhs, rhs),
                        op::I32_ROTL => builder.ins().rotl(lhs, rhs),
                        op::I32_ROTR => builder.ins().rotr(lhs, rhs),
                        op::I32_DIVS => builder.ins().sdiv(lhs, rhs),
                        op::I32_DIVU => builder.ins().udiv(lhs, rhs),
                        op::I32_REMS => builder.ins().srem(lhs, rhs),
                        op::I32_REMU => builder.ins().urem(lhs, rhs),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::I32_CLZ | op::I32_CTZ | op::I32_POPCNT => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = match instruction.opcode {
                        op::I32_CLZ => builder.ins().clz(value),
                        op::I32_CTZ => builder.ins().ctz(value),
                        op::I32_POPCNT => builder.ins().popcnt(value),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::I32_EQZ => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = builder.ins().icmp_imm_s(IntCC::Equal, value, 0);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::I32_EQ
                | op::I32_NE
                | op::I32_LTS
                | op::I32_LTU
                | op::I32_GTS
                | op::I32_GTU
                | op::I32_LES
                | op::I32_LEU
                | op::I32_GES
                | op::I32_GEU => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let condition = match instruction.opcode {
                        op::I32_EQ => IntCC::Equal,
                        op::I32_NE => IntCC::NotEqual,
                        op::I32_LTS => IntCC::SignedLessThan,
                        op::I32_LTU => IntCC::UnsignedLessThan,
                        op::I32_GTS => IntCC::SignedGreaterThan,
                        op::I32_GTU => IntCC::UnsignedGreaterThan,
                        op::I32_LES => IntCC::SignedLessThanOrEqual,
                        op::I32_LEU => IntCC::UnsignedLessThanOrEqual,
                        op::I32_GES => IntCC::SignedGreaterThanOrEqual,
                        op::I32_GEU => IntCC::UnsignedGreaterThanOrEqual,
                        _ => unreachable!(),
                    };
                    let result = builder.ins().icmp(condition, lhs, rhs);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::I64_ADD
                | op::I64_SUB
                | op::I64_MUL
                | op::I64_AND
                | op::I64_OR
                | op::I64_XOR
                | op::I64_SHL
                | op::I64_SHRS
                | op::I64_SHRU
                | op::I64_ROTL
                | op::I64_ROTR
                | op::I64_DIVS
                | op::I64_DIVU
                | op::I64_REMS
                | op::I64_REMU => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let result = match instruction.opcode {
                        op::I64_ADD => builder.ins().iadd(lhs, rhs),
                        op::I64_SUB => builder.ins().isub(lhs, rhs),
                        op::I64_MUL => builder.ins().imul(lhs, rhs),
                        op::I64_AND => builder.ins().band(lhs, rhs),
                        op::I64_OR => builder.ins().bor(lhs, rhs),
                        op::I64_XOR => builder.ins().bxor(lhs, rhs),
                        op::I64_SHL => builder.ins().ishl(lhs, rhs),
                        op::I64_SHRS => builder.ins().sshr(lhs, rhs),
                        op::I64_SHRU => builder.ins().ushr(lhs, rhs),
                        op::I64_ROTL => builder.ins().rotl(lhs, rhs),
                        op::I64_ROTR => builder.ins().rotr(lhs, rhs),
                        op::I64_DIVS => builder.ins().sdiv(lhs, rhs),
                        op::I64_DIVU => builder.ins().udiv(lhs, rhs),
                        op::I64_REMS => builder.ins().srem(lhs, rhs),
                        op::I64_REMU => builder.ins().urem(lhs, rhs),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::I64_CLZ | op::I64_CTZ | op::I64_POPCNT => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let result = match instruction.opcode {
                        op::I64_CLZ => builder.ins().clz(value),
                        op::I64_CTZ => builder.ins().ctz(value),
                        op::I64_POPCNT => builder.ins().popcnt(value),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::I64_EQZ => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let result = builder.ins().icmp_imm_s(IntCC::Equal, value, 0);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::I64_EQ
                | op::I64_NE
                | op::I64_LTS
                | op::I64_LTU
                | op::I64_GTS
                | op::I64_GTU
                | op::I64_LES
                | op::I64_LEU
                | op::I64_GES
                | op::I64_GEU => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let condition = match instruction.opcode {
                        op::I64_EQ => IntCC::Equal,
                        op::I64_NE => IntCC::NotEqual,
                        op::I64_LTS => IntCC::SignedLessThan,
                        op::I64_LTU => IntCC::UnsignedLessThan,
                        op::I64_GTS => IntCC::SignedGreaterThan,
                        op::I64_GTU => IntCC::UnsignedGreaterThan,
                        op::I64_LES => IntCC::SignedLessThanOrEqual,
                        op::I64_LEU => IntCC::UnsignedLessThanOrEqual,
                        op::I64_GES => IntCC::SignedGreaterThanOrEqual,
                        op::I64_GEU => IntCC::UnsignedGreaterThanOrEqual,
                        _ => unreachable!(),
                    };
                    let result = builder.ins().icmp(condition, lhs, rhs);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::I32_WRAP_I64 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    operand_stack.push(builder.ins().ireduce(types::I32, value));
                }
                op::I64_EXTEND_SI32 | op::I64_EXTEND_UI32 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = if instruction.opcode == op::I64_EXTEND_SI32 {
                        builder.ins().sextend(types::I64, value)
                    } else {
                        builder.ins().uextend(types::I64, value)
                    };
                    operand_stack.push(result);
                }
                op::I32_EXTEND8_S | op::I32_EXTEND16_S => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let narrowed_type = if instruction.opcode == op::I32_EXTEND8_S {
                        types::I8
                    } else {
                        types::I16
                    };
                    let narrowed = builder.ins().ireduce(narrowed_type, value);
                    operand_stack.push(builder.ins().sextend(types::I32, narrowed));
                }
                op::I64_EXTEND8_S | op::I64_EXTEND16_S | op::I64_EXTEND32_S => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let narrowed_type = match instruction.opcode {
                        op::I64_EXTEND8_S => types::I8,
                        op::I64_EXTEND16_S => types::I16,
                        op::I64_EXTEND32_S => types::I32,
                        _ => unreachable!(),
                    };
                    let narrowed = builder.ins().ireduce(narrowed_type, value);
                    operand_stack.push(builder.ins().sextend(types::I64, narrowed));
                }
                op::F32_ADD
                | op::F32_SUB
                | op::F32_MUL
                | op::F32_DIV
                | op::F32_MIN
                | op::F32_MAX
                | op::F32_COPYSIGN => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::F32)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::F32)?;
                    let result = match instruction.opcode {
                        op::F32_ADD => builder.ins().fadd(lhs, rhs),
                        op::F32_SUB => builder.ins().fsub(lhs, rhs),
                        op::F32_MUL => builder.ins().fmul(lhs, rhs),
                        op::F32_DIV => builder.ins().fdiv(lhs, rhs),
                        op::F32_MIN => builder.ins().fmin(lhs, rhs),
                        op::F32_MAX => builder.ins().fmax(lhs, rhs),
                        op::F32_COPYSIGN => builder.ins().fcopysign(lhs, rhs),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::F32_ABS
                | op::F32_NEG
                | op::F32_CEIL
                | op::F32_FLOOR
                | op::F32_TRUNC
                | op::F32_NEAREST
                | op::F32_SQRT => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::F32)?;
                    let result = match instruction.opcode {
                        op::F32_ABS => builder.ins().fabs(value),
                        op::F32_NEG => builder.ins().fneg(value),
                        op::F32_CEIL => builder.ins().ceil(value),
                        op::F32_FLOOR => builder.ins().floor(value),
                        op::F32_TRUNC => builder.ins().trunc(value),
                        op::F32_NEAREST => builder.ins().nearest(value),
                        op::F32_SQRT => builder.ins().sqrt(value),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::F64_ADD
                | op::F64_SUB
                | op::F64_MUL
                | op::F64_DIV
                | op::F64_MIN
                | op::F64_MAX
                | op::F64_COPYSIGN => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::F64)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::F64)?;
                    let result = match instruction.opcode {
                        op::F64_ADD => builder.ins().fadd(lhs, rhs),
                        op::F64_SUB => builder.ins().fsub(lhs, rhs),
                        op::F64_MUL => builder.ins().fmul(lhs, rhs),
                        op::F64_DIV => builder.ins().fdiv(lhs, rhs),
                        op::F64_MIN => builder.ins().fmin(lhs, rhs),
                        op::F64_MAX => builder.ins().fmax(lhs, rhs),
                        op::F64_COPYSIGN => builder.ins().fcopysign(lhs, rhs),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::F64_ABS
                | op::F64_NEG
                | op::F64_CEIL
                | op::F64_FLOOR
                | op::F64_TRUNC
                | op::F64_NEAREST
                | op::F64_SQRT => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::F64)?;
                    let result = match instruction.opcode {
                        op::F64_ABS => builder.ins().fabs(value),
                        op::F64_NEG => builder.ins().fneg(value),
                        op::F64_CEIL => builder.ins().ceil(value),
                        op::F64_FLOOR => builder.ins().floor(value),
                        op::F64_TRUNC => builder.ins().trunc(value),
                        op::F64_NEAREST => builder.ins().nearest(value),
                        op::F64_SQRT => builder.ins().sqrt(value),
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::F32_EQ | op::F32_NE | op::F32_LT | op::F32_GT | op::F32_LE | op::F32_GE => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::F32)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::F32)?;
                    let condition = match instruction.opcode {
                        op::F32_EQ => FloatCC::Equal,
                        op::F32_NE => FloatCC::NotEqual,
                        op::F32_LT => FloatCC::LessThan,
                        op::F32_GT => FloatCC::GreaterThan,
                        op::F32_LE => FloatCC::LessThanOrEqual,
                        op::F32_GE => FloatCC::GreaterThanOrEqual,
                        _ => unreachable!(),
                    };
                    let result = builder.ins().fcmp(condition, lhs, rhs);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::F64_EQ | op::F64_NE | op::F64_LT | op::F64_GT | op::F64_LE | op::F64_GE => {
                    let rhs = Self::pop_expected(&builder, &mut operand_stack, types::F64)?;
                    let lhs = Self::pop_expected(&builder, &mut operand_stack, types::F64)?;
                    let condition = match instruction.opcode {
                        op::F64_EQ => FloatCC::Equal,
                        op::F64_NE => FloatCC::NotEqual,
                        op::F64_LT => FloatCC::LessThan,
                        op::F64_GT => FloatCC::GreaterThan,
                        op::F64_LE => FloatCC::LessThanOrEqual,
                        op::F64_GE => FloatCC::GreaterThanOrEqual,
                        _ => unreachable!(),
                    };
                    let result = builder.ins().fcmp(condition, lhs, rhs);
                    operand_stack.push(builder.ins().uextend(types::I32, result));
                }
                op::F32_CONVERT_SI32 | op::F32_CONVERT_UI32 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = if instruction.opcode == op::F32_CONVERT_SI32 {
                        builder.ins().fcvt_from_sint(types::F32, value)
                    } else {
                        builder.ins().fcvt_from_uint(types::F32, value)
                    };
                    operand_stack.push(result);
                }
                op::F32_CONVERT_SI64 | op::F32_CONVERT_UI64 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let result = if instruction.opcode == op::F32_CONVERT_SI64 {
                        builder.ins().fcvt_from_sint(types::F32, value)
                    } else {
                        builder.ins().fcvt_from_uint(types::F32, value)
                    };
                    operand_stack.push(result);
                }
                op::F64_CONVERT_SI32 | op::F64_CONVERT_UI32 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    let result = if instruction.opcode == op::F64_CONVERT_SI32 {
                        builder.ins().fcvt_from_sint(types::F64, value)
                    } else {
                        builder.ins().fcvt_from_uint(types::F64, value)
                    };
                    operand_stack.push(result);
                }
                op::F64_CONVERT_SI64 | op::F64_CONVERT_UI64 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    let result = if instruction.opcode == op::F64_CONVERT_SI64 {
                        builder.ins().fcvt_from_sint(types::F64, value)
                    } else {
                        builder.ins().fcvt_from_uint(types::F64, value)
                    };
                    operand_stack.push(result);
                }
                op::F32_DEMOTE_F64 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::F64)?;
                    operand_stack.push(builder.ins().fdemote(types::F32, value));
                }
                op::F64_PROMOTE_F32 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::F32)?;
                    operand_stack.push(builder.ins().fpromote(types::F64, value));
                }
                op::I32_REINTERPRET_F32 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::F32)?;
                    operand_stack.push(builder.ins().bitcast(types::I32, MemFlags::new(), value));
                }
                op::I64_REINTERPRET_F64 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::F64)?;
                    operand_stack.push(builder.ins().bitcast(types::I64, MemFlags::new(), value));
                }
                op::F32_REINTERPRET_I32 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                    operand_stack.push(builder.ins().bitcast(types::F32, MemFlags::new(), value));
                }
                op::F64_REINTERPRET_I64 => {
                    let value = Self::pop_expected(&builder, &mut operand_stack, types::I64)?;
                    operand_stack.push(builder.ins().bitcast(types::F64, MemFlags::new(), value));
                }
                op::I32_TRUNC_SF32
                | op::I32_TRUNC_UF32
                | op::I32_TRUNC_SF64
                | op::I32_TRUNC_UF64
                | op::I64_TRUNC_SF32
                | op::I64_TRUNC_UF32
                | op::I64_TRUNC_SF64
                | op::I64_TRUNC_UF64 => {
                    let source_type = if matches!(
                        instruction.opcode,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_UF32 | op::I64_TRUNC_SF32 | op::I64_TRUNC_UF32
                    ) {
                        types::F32
                    } else {
                        types::F64
                    };
                    let result_type = if matches!(
                        instruction.opcode,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_UF32 | op::I32_TRUNC_SF64 | op::I32_TRUNC_UF64
                    ) {
                        types::I32
                    } else {
                        types::I64
                    };
                    let is_signed = matches!(
                        instruction.opcode,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_SF64 | op::I64_TRUNC_SF32 | op::I64_TRUNC_SF64
                    );
                    let value = Self::pop_expected(&builder, &mut operand_stack, source_type)?;
                    let result = if is_signed {
                        builder.ins().fcvt_to_sint(result_type, value)
                    } else {
                        builder.ins().fcvt_to_uint(result_type, value)
                    };
                    operand_stack.push(result);
                }
                op::I32_TRUNC_SAT_F32_S
                | op::I32_TRUNC_SAT_F32_U
                | op::I32_TRUNC_SAT_F64_S
                | op::I32_TRUNC_SAT_F64_U
                | op::I64_TRUNC_SAT_F32_S
                | op::I64_TRUNC_SAT_F32_U
                | op::I64_TRUNC_SAT_F64_S
                | op::I64_TRUNC_SAT_F64_U => {
                    let source_type = if matches!(
                        instruction.opcode,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F32_U
                            | op::I64_TRUNC_SAT_F32_S
                            | op::I64_TRUNC_SAT_F32_U
                    ) {
                        types::F32
                    } else {
                        types::F64
                    };
                    let result_type = if matches!(
                        instruction.opcode,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F32_U
                            | op::I32_TRUNC_SAT_F64_S
                            | op::I32_TRUNC_SAT_F64_U
                    ) {
                        types::I32
                    } else {
                        types::I64
                    };
                    let is_signed = matches!(
                        instruction.opcode,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F64_S
                            | op::I64_TRUNC_SAT_F32_S
                            | op::I64_TRUNC_SAT_F64_S
                    );
                    let value = Self::pop_expected(&builder, &mut operand_stack, source_type)?;
                    let result = if is_signed {
                        builder.ins().fcvt_to_sint_sat(result_type, value)
                    } else {
                        builder.ins().fcvt_to_uint_sat(result_type, value)
                    };
                    operand_stack.push(result);
                }
                op::MEMORY_COPY | op::MEMORY_FILL => {
                    let bulk_helpers = bulk_memory_helpers
                        .as_ref()
                        .ok_or("missing direct bulk-memory helpers")?;
                    let fallback_helpers = runtime_fallback_helpers
                        .as_ref()
                        .ok_or("missing direct runtime fallback helpers")?;
                    let current_interpreter_address = builder
                        .ins()
                        .func_addr(pointer_type, fallback_helpers.current_interpreter);
                    let current_interpreter_call = builder.ins().call_indirect(
                        fallback_helpers.current_interpreter_signature,
                        current_interpreter_address,
                        &[],
                    );
                    let interpreter = builder.inst_results(current_interpreter_call)[0];

                    let (signature, helper, arguments) = if instruction.opcode == op::MEMORY_COPY {
                        let count = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                        let source_offset = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                        let destination_offset = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                        let memory = instruction.memory_copy_argument();
                        let destination_memory = builder
                            .ins()
                            .iconst(types::I32, i64::from(memory.destination_memory_index));
                        let source_memory = builder.ins().iconst(types::I32, i64::from(memory.source_memory_index));
                        (
                            bulk_helpers.memory_copy_signature,
                            bulk_helpers.memory_copy,
                            vec![
                                interpreter,
                                configuration,
                                destination_memory,
                                source_memory,
                                destination_offset,
                                source_offset,
                                count,
                            ],
                        )
                    } else {
                        let count = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                        let value = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                        let offset = Self::pop_expected(&builder, &mut operand_stack, types::I32)?;
                        let memory = builder.ins().iconst(types::I32, i64::from(instruction.memory_index()));
                        (
                            bulk_helpers.memory_fill_signature,
                            bulk_helpers.memory_fill,
                            vec![interpreter, configuration, memory, offset, value, count],
                        )
                    };

                    let helper_address = builder.ins().func_addr(pointer_type, helper);
                    let call = builder.ins().call_indirect(signature, helper_address, &arguments);
                    let status = builder.inst_results(call)[0];
                    let trapped = builder.ins().icmp_imm_s(IntCC::NotEqual, status, 0);
                    let trap_block = builder.create_block();
                    let continuation = builder.create_block();
                    builder.set_cold_block(trap_block);
                    builder.ins().brif(trapped, trap_block, &[], continuation, &[]);

                    builder.switch_to_block(trap_block);
                    builder.seal_block(trap_block);
                    let raise_trap_address = builder.ins().func_addr(pointer_type, fallback_helpers.raise_trap);
                    builder
                        .ins()
                        .call_indirect(fallback_helpers.raise_trap_signature, raise_trap_address, &[]);
                    builder.ins().trap(user_trap_code(CraneliftUserTrapCode::Unreachable));

                    builder.switch_to_block(continuation);
                    builder.seal_block(continuation);
                }
                op::I32_LOAD
                | op::I64_LOAD
                | op::F32_LOAD
                | op::F64_LOAD
                | op::I32_LOAD8_S
                | op::I32_LOAD8_U
                | op::I32_LOAD16_S
                | op::I32_LOAD16_U
                | op::I64_LOAD8_S
                | op::I64_LOAD8_U
                | op::I64_LOAD16_S
                | op::I64_LOAD16_U
                | op::I64_LOAD32_S
                | op::I64_LOAD32_U => {
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
                    let result = match instruction.opcode {
                        op::I32_LOAD => builder.ins().load(types::I32, memory_flags.linear_memory, address, 0),
                        op::I64_LOAD => builder.ins().load(types::I64, memory_flags.linear_memory, address, 0),
                        op::F32_LOAD => builder.ins().load(types::F32, memory_flags.linear_memory, address, 0),
                        op::F64_LOAD => builder.ins().load(types::F64, memory_flags.linear_memory, address, 0),
                        op::I32_LOAD8_S => {
                            let value = builder.ins().load(types::I8, memory_flags.linear_memory, address, 0);
                            builder.ins().sextend(types::I32, value)
                        }
                        op::I32_LOAD8_U => {
                            let value = builder.ins().load(types::I8, memory_flags.linear_memory, address, 0);
                            builder.ins().uextend(types::I32, value)
                        }
                        op::I32_LOAD16_S => {
                            let value = builder.ins().load(types::I16, memory_flags.linear_memory, address, 0);
                            builder.ins().sextend(types::I32, value)
                        }
                        op::I32_LOAD16_U => {
                            let value = builder.ins().load(types::I16, memory_flags.linear_memory, address, 0);
                            builder.ins().uextend(types::I32, value)
                        }
                        op::I64_LOAD8_S => {
                            let value = builder.ins().load(types::I8, memory_flags.linear_memory, address, 0);
                            builder.ins().sextend(types::I64, value)
                        }
                        op::I64_LOAD8_U => {
                            let value = builder.ins().load(types::I8, memory_flags.linear_memory, address, 0);
                            builder.ins().uextend(types::I64, value)
                        }
                        op::I64_LOAD16_S => {
                            let value = builder.ins().load(types::I16, memory_flags.linear_memory, address, 0);
                            builder.ins().sextend(types::I64, value)
                        }
                        op::I64_LOAD16_U => {
                            let value = builder.ins().load(types::I16, memory_flags.linear_memory, address, 0);
                            builder.ins().uextend(types::I64, value)
                        }
                        op::I64_LOAD32_S => {
                            let value = builder.ins().load(types::I32, memory_flags.linear_memory, address, 0);
                            builder.ins().sextend(types::I64, value)
                        }
                        op::I64_LOAD32_U => {
                            let value = builder.ins().load(types::I32, memory_flags.linear_memory, address, 0);
                            builder.ins().uextend(types::I64, value)
                        }
                        _ => unreachable!(),
                    };
                    operand_stack.push(result);
                }
                op::I32_STORE
                | op::I64_STORE
                | op::F32_STORE
                | op::F64_STORE
                | op::I32_STORE8
                | op::I32_STORE16
                | op::I64_STORE8
                | op::I64_STORE16
                | op::I64_STORE32 => {
                    let value_type = match instruction.opcode {
                        op::I32_STORE | op::I32_STORE8 | op::I32_STORE16 => types::I32,
                        op::I64_STORE | op::I64_STORE8 | op::I64_STORE16 | op::I64_STORE32 => types::I64,
                        op::F32_STORE => types::F32,
                        op::F64_STORE => types::F64,
                        _ => unreachable!(),
                    };
                    let value = Self::pop_expected(&builder, &mut operand_stack, value_type)?;
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
                    let value = match instruction.opcode {
                        op::I32_STORE8 | op::I64_STORE8 => builder.ins().ireduce(types::I8, value),
                        op::I32_STORE16 | op::I64_STORE16 => builder.ins().ireduce(types::I16, value),
                        op::I64_STORE32 => builder.ins().ireduce(types::I32, value),
                        _ => value,
                    };
                    builder.ins().store(memory_flags.linear_memory, value, address, 0);
                }
                _ => {
                    return Err("unsupported direct instruction");
                }
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
    use crate::DirectMemoryCopyInstructionArguments;
    use crate::DirectMemoryInstructionArguments;
    use crate::DirectStructuredInstructionArguments;
    use crate::DirectTableBranchInstructionArguments;
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

    fn global(opcode: u64, index: u32) -> DirectInstruction {
        instruction(opcode, DirectInstructionArguments { global_index: index })
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
            DirectCompilerInput {
                instructions: &instructions,
                branch_targets: &[],
                local_types: &declared_local_types,
                function_types: &function_types,
                module_types: &[],
                global_types: &[],
            },
            &SerializedRuntimeLayout::default(),
            FunctionCompilationOptions {
                result_arity: 0,
                num_locals: 2,
                num_params: 1,
                function_index: 0,
                max_call_rec_size: 0,
            },
        )
        .expect("direct compilation should succeed");

        assert!(!compiled.code.is_empty());
        assert!(!compiled.relocs.is_empty());
    }

    #[test]
    fn compiles_v128_global_access() {
        let instructions = [
            local(op::LOCAL_GET, 0),
            global(op::GLOBAL_SET, 0),
            global(op::GLOBAL_GET, 0),
            local(op::LOCAL_SET, 0),
            no_arguments(op::END),
        ];
        let v128_type = DirectValueType {
            kind: DirectValueTypeKind::V128 as u32,
            type_index: 0,
            nullable: 0,
        };
        let function_types = [WasmFunctionType {
            parameters: &[],
            results: &[],
        }];
        let compiled = DirectCompiler::compile_to_bytes(
            DirectCompilerInput {
                instructions: &instructions,
                branch_targets: &[],
                local_types: &[v128_type],
                function_types: &function_types,
                module_types: &[],
                global_types: &[v128_type],
            },
            &SerializedRuntimeLayout::default(),
            FunctionCompilationOptions {
                result_arity: 0,
                num_locals: 1,
                num_params: 0,
                function_index: 0,
                max_call_rec_size: 0,
            },
        )
        .expect("direct compilation should support v128 globals");

        assert!(!compiled.code.is_empty());
    }

    #[test]
    fn compiles_branch_table() {
        let instructions = [
            structured(op::BLOCK),
            structured(op::BLOCK),
            i32_constant(0),
            instruction(
                op::BR_TABLE,
                DirectInstructionArguments {
                    table_branch: DirectTableBranchInstructionArguments {
                        targets_offset: 0,
                        target_count: 1,
                        default_target: 1,
                    },
                },
            ),
            no_arguments(op::END),
            no_arguments(op::END),
            no_arguments(op::END),
        ];
        let function_types = [WasmFunctionType {
            parameters: &[],
            results: &[],
        }];
        let compiled = DirectCompiler::compile_to_bytes(
            DirectCompilerInput {
                instructions: &instructions,
                branch_targets: &[0],
                local_types: &[],
                function_types: &function_types,
                module_types: &[],
                global_types: &[],
            },
            &SerializedRuntimeLayout::default(),
            FunctionCompilationOptions {
                result_arity: 0,
                num_locals: 0,
                num_params: 0,
                function_index: 0,
                max_call_rec_size: 0,
            },
        )
        .expect("direct compilation should support branch tables");

        assert!(!compiled.code.is_empty());
    }

    #[test]
    fn compiles_bulk_memory_helpers() {
        let instructions = [
            i32_constant(0),
            i32_constant(0),
            i32_constant(4),
            instruction(
                op::MEMORY_COPY,
                DirectInstructionArguments {
                    memory_copy: DirectMemoryCopyInstructionArguments {
                        source_memory_index: 0,
                        destination_memory_index: 0,
                    },
                },
            ),
            i32_constant(0),
            i32_constant(1),
            i32_constant(4),
            instruction(op::MEMORY_FILL, DirectInstructionArguments { memory_index: 0 }),
            no_arguments(op::END),
        ];
        let function_types = [WasmFunctionType {
            parameters: &[],
            results: &[],
        }];
        let compiled = DirectCompiler::compile_to_bytes(
            DirectCompilerInput {
                instructions: &instructions,
                branch_targets: &[],
                local_types: &[],
                function_types: &function_types,
                module_types: &[],
                global_types: &[],
            },
            &SerializedRuntimeLayout::default(),
            FunctionCompilationOptions {
                result_arity: 0,
                num_locals: 0,
                num_params: 0,
                function_index: 0,
                max_call_rec_size: 0,
            },
        )
        .expect("direct compilation should support bulk-memory helpers");

        assert!(!compiled.code.is_empty());
        assert!(
            compiled
                .relocs
                .iter()
                .any(|relocation| relocation.target_index == HelperId::memory_copy as u32)
        );
        assert!(
            compiled
                .relocs
                .iter()
                .any(|relocation| relocation.target_index == HelperId::memory_fill as u32)
        );
    }
}
