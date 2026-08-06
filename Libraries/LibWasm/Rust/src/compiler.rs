/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::CompiledFunction;
use crate::CraneliftInsn;
use crate::CraneliftRelocationTargetKind;
use crate::CraneliftUserTrapCode;
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
use cranelift_codegen::{self};
use cranelift_frontend::FunctionBuilder;
use cranelift_frontend::FunctionBuilderContext;
use cranelift_frontend::Variable;
use cranelift_native;
use std::collections::HashMap;

mod common;
pub(crate) mod direct;
mod direct_input;
mod register_liveness;
use common::CompiledCodeParts;
use common::F32_KIND;
use common::F64_KIND;
use common::HELPER_EXTERNAL_NAMESPACE;
use common::I32_KIND;
use common::I64_KIND;
use common::LegacyImportedHelpers;
use common::RuntimeLayout;
use common::WASM_FUNCTION_EXTERNAL_NAMESPACE;
use common::WasmMemoryFlags;
use common::compile_function;
use common::payload_to_value;
use common::user_trap_code;
use common::value_to_payload;
use common::wasm_abi_type;
use register_liveness::RegisterLiveness;
use register_liveness::RegisterSet;

// Opcode constants generated from Opcode.h (see build.rs.)
#[allow(dead_code)]
mod op {
    include!(concat!(env!("OUT_DIR"), "/opcodes.rs"));
}

const REG_COUNT: usize = 8;
const STACK_MARKER: u8 = 8;
const CALLREC_BASE: u8 = 9;
const INDIRECT_CALL_RESULT_TYPE_SHIFT: u32 = 16;
const INDIRECT_CALL_TABLE64: u32 = 1 << 18;
const INDIRECT_CALL_TYPE_VALID: u32 = 1 << 19;

#[derive(Clone, Copy)]
struct NativeIndirectCallLayout {
    table_instances: i32,
    current_module: i32,
    current_canonical_types: i32,
    table_instance_size: i32,
    table_instance_callables: i32,
    callable_defined_type: i32,
    callable_module: i32,
    callable_compiled_instructions: i32,
    compiled_instructions_native_entry: i32,
}

struct NativeIndirectCallTarget {
    native_call: Block,
    fallback_call: Block,
}

#[derive(Clone, Copy)]
struct IndirectCallLoweringContext {
    ptr_type: Type,
    bridge_signature: SigRef,
    bridge_helper: FuncRef,
    check_type_signature: SigRef,
    check_type_helper: FuncRef,
    trap_block: Block,
    result_scratch_offset: i32,
    call_record_base_offset: i32,
    value_size: i32,
    native_layout: NativeIndirectCallLayout,
    memory_flags: WasmMemoryFlags,
}

struct IndirectCallResult {
    payload: Value,
    kind: Option<u8>,
}

struct IndirectCallType {
    parameters: Vec<u8>,
    results: Vec<u8>,
}

#[derive(Clone, Copy)]
struct IndirectCallOperands {
    interpreter: Value,
    configuration: Value,
    table_index: Value,
    type_index: Value,
    element_index: Value,
}

/// The `I64` payload bank is always defined. Typed banks are trusted only until the next
/// control-flow merge, where they may be undefined on an incoming edge.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Bank {
    I32,
    I64,
    F32,
    F64,
}

/// Control flow frame tracking for structured control flow.
struct ControlFrame {
    kind: ControlKind,
    /// The cranelift block to branch to (for `br` targeting this frame).
    /// For blocks: the merge/after block.
    /// For loops: the loop header block.
    branch_target: cranelift_codegen::ir::Block,
    /// Where execution continues after this construct ends.
    after_block: cranelift_codegen::ir::Block,
    /// Number of result values.
    arity: u32,
    /// Number of parameters consumed from outer stack.
    param_count: usize,
    /// Stack depth at block entry, tracked for br*.
    stack_depth_at_entry: i32,
    /// Whether this construct was entered from unreachable code.
    is_unreachable_at_entry: bool,
    /// Real value-stack size at block entry, minus this block's param count.
    /// Only meaningful (and only set) when vstack is disabled (max_stack_depth == 0).
    entry_real_depth_var: Option<Variable>,
    bank_snapshot: Option<([Bank; REG_COUNT], Vec<Bank>)>,
    branch_target_reg_ty: Option<[Bank; REG_COUNT]>,
    branch_target_stack_ty: Option<Vec<Bank>>,
    branch_target_live_registers: RegisterSet,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ControlKind {
    Block,
    Loop,
    If,
}

#[derive(Clone, Copy, Debug, Default)]
struct LocalAccesses {
    reads: [Option<usize>; 2],
    write: Option<usize>,
}

pub struct CraneliftCompiler;

impl CraneliftCompiler {
    // At higher arities, platform-ABI stack marshalling duplicates values that are already in
    // call-record storage and produces a clear benchmark cliff.
    const NATIVE_REGISTER_ABI_PARAMETER_LIMIT: usize = 8;

    fn uses_register_native_abi(function_type: WasmFunctionType<'_>) -> bool {
        function_type.parameters.len() <= Self::NATIVE_REGISTER_ABI_PARAMETER_LIMIT
    }

    fn native_signature(isa: &dyn TargetIsa, function_type: WasmFunctionType<'_>) -> Result<Signature, &'static str> {
        if function_type.results.len() > 1 {
            return Err("multi-value native Wasm ABI is not supported");
        }

        let mut signature = Signature::new(isa.default_call_conv());
        signature.params.push(AbiParam::new(isa.pointer_type())); // interpreter
        signature.params.push(AbiParam::new(isa.pointer_type())); // configuration
        signature.params.push(AbiParam::new(types::I32)); // interpreter resume IP plus one, or zero for native calls
        if Self::uses_register_native_abi(function_type) {
            for &parameter in function_type.parameters {
                signature.params.push(AbiParam::new(wasm_abi_type(parameter)?));
            }
        } else {
            signature.params.push(AbiParam::new(isa.pointer_type())); // call-record locals
        }
        for &result in function_type.results {
            signature.returns.push(AbiParam::new(wasm_abi_type(result)?));
        }
        Ok(signature)
    }

    fn decode_indirect_call_type(insn: &CraneliftInsn) -> Result<Option<IndirectCallType>, &'static str> {
        if insn.call_type_encoding & INDIRECT_CALL_TYPE_VALID == 0 {
            return Ok(None);
        }

        let result_count = insn.call_result_count as usize;
        if result_count > 1 {
            return Err("multi-value native indirect call is not supported");
        }

        let parameter_types = (0..insn.imm3 as usize)
            .map(|index| {
                if index < Self::NATIVE_REGISTER_ABI_PARAMETER_LIMIT {
                    ((insn.call_type_encoding >> (index * 2)) & 0x3) as u8
                } else {
                    I64_KIND
                }
            })
            .collect();
        let result_types = if result_count == 1 {
            vec![((insn.call_type_encoding >> INDIRECT_CALL_RESULT_TYPE_SHIFT) & 0x3) as u8]
        } else {
            Vec::new()
        };

        Ok(Some(IndirectCallType {
            parameters: parameter_types,
            results: result_types,
        }))
    }

    fn emit_native_indirect_call_target(
        builder: &mut FunctionBuilder<'_>,
        insn: &CraneliftInsn,
        operands: IndirectCallOperands,
        context: IndirectCallLoweringContext,
    ) -> Result<NativeIndirectCallTarget, &'static str> {
        let ptr_type = context.ptr_type;
        let layout = context.native_layout;
        let memory_flags = context.memory_flags;
        let exact_type = builder.create_block();
        let subtype_check = builder.create_block();
        let native_entry_check = builder.create_block();
        let native_call = builder.create_block();
        let fallback_call = builder.create_block();
        builder.append_block_param(native_call, ptr_type);

        let table_instances = builder.ins().load(
            ptr_type,
            memory_flags.configuration,
            operands.configuration,
            layout.table_instances,
        );
        let table_offset = insn
            .imm2
            .checked_mul(i64::from(ptr_type.bytes()))
            .ok_or("table index offset overflow")?;
        let table_offset = builder.ins().iconst(ptr_type, table_offset);
        let table_address = builder.ins().iadd(table_instances, table_offset);
        let table = builder.ins().load(ptr_type, memory_flags.tables, table_address, 0);
        let table_size = builder
            .ins()
            .load(ptr_type, memory_flags.tables, table, layout.table_instance_size);
        let table_size = if ptr_type == types::I64 {
            table_size
        } else {
            builder.ins().uextend(types::I64, table_size)
        };
        let in_bounds = builder
            .ins()
            .icmp(IntCC::UnsignedLessThan, operands.element_index, table_size);
        builder
            .ins()
            .trapz(in_bounds, user_trap_code(CraneliftUserTrapCode::TableOutOfBounds));
        let callables = builder
            .ins()
            .load(ptr_type, memory_flags.tables, table, layout.table_instance_callables);
        let element_offset = if ptr_type == types::I64 {
            builder
                .ins()
                .imul_imm_s(operands.element_index, i64::from(ptr_type.bytes()))
        } else {
            let element_index = builder.ins().ireduce(types::I32, operands.element_index);
            builder.ins().imul_imm_s(element_index, i64::from(ptr_type.bytes()))
        };
        let callable_address = builder.ins().iadd(callables, element_offset);
        let callable = builder.ins().load(ptr_type, memory_flags.tables, callable_address, 0);
        let is_callable = builder.ins().icmp_imm_s(IntCC::NotEqual, callable, 0);
        builder
            .ins()
            .trapz(is_callable, user_trap_code(CraneliftUserTrapCode::IndirectCallNull));
        let actual_type = builder.ins().load(
            ptr_type,
            memory_flags.readonly_runtime_metadata,
            callable,
            layout.callable_defined_type,
        );
        let canonical_types = builder.ins().load(
            ptr_type,
            memory_flags.configuration,
            operands.configuration,
            layout.current_canonical_types,
        );
        let type_offset = insn
            .imm1
            .checked_mul(i64::from(ptr_type.bytes()))
            .ok_or("type index offset overflow")?;
        let type_offset = builder.ins().iconst(ptr_type, type_offset);
        let expected_type_address = builder.ins().iadd(canonical_types, type_offset);
        let expected_type = builder.ins().load(
            ptr_type,
            memory_flags.readonly_runtime_metadata,
            expected_type_address,
            0,
        );
        let is_exact_type = builder.ins().icmp(IntCC::Equal, actual_type, expected_type);
        builder.ins().brif(is_exact_type, exact_type, &[], subtype_check, &[]);

        builder.switch_to_block(subtype_check);
        builder.seal_block(subtype_check);
        let type_check = builder.ins().func_addr(ptr_type, context.check_type_helper);
        let type_check_call =
            builder
                .ins()
                .call_indirect(context.check_type_signature, type_check, &[actual_type, expected_type]);
        let type_mismatch = builder.inst_results(type_check_call)[0];
        builder.ins().trapnz(
            type_mismatch,
            user_trap_code(CraneliftUserTrapCode::IndirectCallTypeMismatch),
        );
        builder.ins().jump(exact_type, &[]);

        builder.switch_to_block(exact_type);
        builder.seal_block(exact_type);
        let callable_module = builder.ins().load(
            ptr_type,
            memory_flags.readonly_runtime_metadata,
            callable,
            layout.callable_module,
        );
        let current_module = builder.ins().load(
            ptr_type,
            memory_flags.configuration,
            operands.configuration,
            layout.current_module,
        );
        let is_same_module = builder.ins().icmp(IntCC::Equal, callable_module, current_module);
        builder
            .ins()
            .brif(is_same_module, native_entry_check, &[], fallback_call, &[]);

        builder.switch_to_block(native_entry_check);
        builder.seal_block(native_entry_check);
        let compiled_instructions = builder.ins().load(
            ptr_type,
            memory_flags.readonly_runtime_metadata,
            callable,
            layout.callable_compiled_instructions,
        );
        let native_entry_address = builder.ins().iadd_imm_s(
            compiled_instructions,
            i64::from(layout.compiled_instructions_native_entry),
        );
        let native_entry = builder
            .ins()
            .atomic_load(ptr_type, memory_flags.runtime_metadata, native_entry_address);
        let has_native_entry = builder.ins().icmp_imm_s(IntCC::NotEqual, native_entry, 0);
        builder.ins().brif(
            has_native_entry,
            native_call,
            &[BlockArg::Value(native_entry)],
            fallback_call,
            &[],
        );

        builder.seal_block(fallback_call);
        builder.switch_to_block(native_call);
        builder.seal_block(native_call);

        Ok(NativeIndirectCallTarget {
            native_call,
            fallback_call,
        })
    }

    fn emit_native_call_from_value_array(
        builder: &mut FunctionBuilder<'_>,
        native_signature: SigRef,
        native_entry: Value,
        operands: IndirectCallOperands,
        argument_base: Value,
        target_type: WasmFunctionType<'_>,
        context: IndirectCallLoweringContext,
    ) -> Result<Option<Value>, &'static str> {
        let entry_token = builder.ins().iconst(types::I32, 0);
        let uses_register_native_abi = Self::uses_register_native_abi(target_type);
        let mut arguments = Vec::with_capacity(if uses_register_native_abi {
            3 + target_type.parameters.len()
        } else {
            4
        });
        arguments.extend([operands.interpreter, operands.configuration, entry_token]);
        if uses_register_native_abi {
            for (parameter_index, &parameter_kind) in target_type.parameters.iter().enumerate() {
                let offset = i32::try_from(parameter_index * context.value_size as usize)
                    .map_err(|_| "call-record argument offset overflow")?;
                let argument_type = wasm_abi_type(parameter_kind)?;
                let argument =
                    builder
                        .ins()
                        .load(argument_type, context.memory_flags.activation, argument_base, offset);
                arguments.push(argument);
            }
        } else {
            arguments.push(argument_base);
        }

        let call = builder.ins().call_indirect(native_signature, native_entry, &arguments);
        let Some(&result_kind) = target_type.results.first() else {
            return Ok(None);
        };
        let result = builder.inst_results(call)[0];
        Ok(Some(value_to_payload(builder, result, result_kind)?))
    }

    fn emit_indirect_bridge_call(
        builder: &mut FunctionBuilder<'_>,
        operands: IndirectCallOperands,
        context: IndirectCallLoweringContext,
        has_result: bool,
    ) -> Option<Value> {
        let call_helper = builder.ins().func_addr(context.ptr_type, context.bridge_helper);
        let call = builder.ins().call_indirect(
            context.bridge_signature,
            call_helper,
            &[
                operands.interpreter,
                operands.configuration,
                operands.table_index,
                operands.type_index,
                operands.element_index,
            ],
        );
        let status = builder.inst_results(call)[0];
        let trapped = builder.ins().icmp_imm_s(IntCC::NotEqual, status, 0);
        let continuation = builder.create_block();
        builder.ins().brif(trapped, context.trap_block, &[], continuation, &[]);
        builder.switch_to_block(continuation);
        builder.seal_block(continuation);

        has_result.then(|| {
            builder.ins().load(
                types::I64,
                context.memory_flags.configuration,
                operands.configuration,
                context.result_scratch_offset,
            )
        })
    }

    fn emit_indirect_call_with_record(
        builder: &mut FunctionBuilder<'_>,
        isa: &dyn TargetIsa,
        insn: &CraneliftInsn,
        interpreter: Value,
        configuration: Value,
        element_index: Value,
        context: IndirectCallLoweringContext,
    ) -> Result<Option<IndirectCallResult>, &'static str> {
        let type_index = builder.ins().iconst(types::I32, insn.imm1);
        let table_index = builder.ins().iconst(types::I32, insn.imm2);
        let has_result = insn.opcode == op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_1;
        let operands = IndirectCallOperands {
            interpreter,
            configuration,
            table_index,
            type_index,
            element_index,
        };

        let Some(call_type) = Self::decode_indirect_call_type(insn)? else {
            let result = Self::emit_indirect_bridge_call(builder, operands, context, has_result);
            return Ok(result.map(|payload| IndirectCallResult { payload, kind: None }));
        };

        if call_type.results.len() != usize::from(has_result) {
            return Err("call-record call does not match target type");
        }

        let target_type = WasmFunctionType {
            parameters: &call_type.parameters,
            results: &call_type.results,
        };
        let native_signature = builder.import_signature(Self::native_signature(isa, target_type)?);
        let continuation = builder.create_block();
        if has_result {
            builder.append_block_param(continuation, types::I64);
        }

        // https://webassembly.github.io/spec/core/exec/instructions.html#exec-call-indirect
        // (CALL_INDIRECT x yy) ~> (TABLE.GET x) (REF.CAST (REF NULL yy)) (CALL_REF yy)
        // (TABLE.GET x)
        // (REF.CAST (REF NULL yy))
        let target = Self::emit_native_indirect_call_target(builder, insn, operands, context)?;

        // https://webassembly.github.io/spec/core/exec/instructions.html#exec-call-indirect
        // (CALL_REF yy)
        let native_entry = builder.block_params(target.native_call)[0];
        let call_record = builder.ins().load(
            context.ptr_type,
            context.memory_flags.configuration,
            operands.configuration,
            context.call_record_base_offset,
        );
        let native_result = Self::emit_native_call_from_value_array(
            builder,
            native_signature,
            native_entry,
            operands,
            call_record,
            target_type,
            context,
        )?;
        if let Some(result) = native_result {
            builder.ins().jump(continuation, &[BlockArg::Value(result)]);
        } else {
            builder.ins().jump(continuation, &[]);
        }

        builder.switch_to_block(target.fallback_call);
        let fallback_result = Self::emit_indirect_bridge_call(builder, operands, context, has_result);
        if let Some(result) = fallback_result {
            builder.ins().jump(continuation, &[BlockArg::Value(result)]);
        } else {
            builder.ins().jump(continuation, &[]);
        }

        builder.switch_to_block(continuation);
        builder.seal_block(continuation);
        Ok(call_type.results.first().map(|&kind| IndirectCallResult {
            payload: builder.block_params(continuation)[0],
            kind: Some(kind),
        }))
    }

    fn compile_interpreter_fallback(
        isa: &dyn TargetIsa,
        function_index: u32,
        function_type: WasmFunctionType<'_>,
        layout: &SerializedRuntimeLayout,
    ) -> Result<CompiledCodeParts, &'static str> {
        let ptr_type = isa.pointer_type();
        let host_cc = isa.default_call_conv();
        let signature = Self::native_signature(isa, function_type)?;
        let mut function = Function::with_name_signature(UserFuncName::user(2, function_index), signature);
        let memory_flags = WasmMemoryFlags::new(&mut function);
        let runtime_layout = RuntimeLayout::new(layout);
        let mut builder_context = FunctionBuilderContext::new();
        let mut builder = FunctionBuilder::new(&mut function, &mut builder_context);
        let entry = builder.create_block();
        builder.append_block_params_for_function_params(entry);
        builder.switch_to_block(entry);
        builder.seal_block(entry);

        let interpreter = builder.block_params(entry)[0];
        let configuration = builder.block_params(entry)[1];
        let original_top = builder.ins().load(
            ptr_type,
            memory_flags.configuration,
            configuration,
            runtime_layout.value_stack_top_offset,
        );
        let zero_tag = builder.ins().iconst(types::I64, 0);
        let uses_register_native_abi = Self::uses_register_native_abi(function_type);
        for (index, &kind) in function_type.parameters.iter().enumerate() {
            let offset =
                i32::try_from(index * runtime_layout.value_size as usize).map_err(|_| "argument offset overflow")?;
            let payload = if uses_register_native_abi {
                let value = builder.block_params(entry)[3 + index];
                value_to_payload(&mut builder, value, kind)?
            } else {
                let locals = builder.block_params(entry)[3];
                builder.ins().load(types::I64, memory_flags.activation, locals, offset)
            };
            builder
                .ins()
                .store(memory_flags.activation, payload, original_top, offset);
            builder
                .ins()
                .store(memory_flags.activation, zero_tag, original_top, offset + 8);
        }
        let arguments_size = i64::try_from(function_type.parameters.len() * runtime_layout.value_size as usize)
            .map_err(|_| "argument size overflow")?;
        let arguments_top = builder.ins().iadd_imm_s(original_top, arguments_size);
        builder.ins().store(
            memory_flags.configuration,
            arguments_top,
            configuration,
            runtime_layout.value_stack_top_offset,
        );

        let mut call_signature = Signature::new(host_cc);
        call_signature.params.push(AbiParam::new(ptr_type));
        call_signature.params.push(AbiParam::new(ptr_type));
        call_signature.params.push(AbiParam::new(types::I32));
        call_signature.returns.push(AbiParam::new(types::I32));
        let call_signature = builder.import_signature(call_signature);
        let call_name = builder.func.declare_imported_user_function(UserExternalName {
            namespace: HELPER_EXTERNAL_NAMESPACE,
            index: HelperId::call_function as u32,
        });
        let call_function = builder.func.import_function(ExtFuncData {
            name: ExternalName::user(call_name),
            signature: call_signature,
            colocated: false,
            patchable: false,
        });
        let call_address = builder.ins().func_addr(ptr_type, call_function);
        let target_index = builder.ins().iconst(types::I32, i64::from(function_index));
        let call = builder.ins().call_indirect(
            call_signature,
            call_address,
            &[interpreter, configuration, target_index],
        );
        let status = builder.inst_results(call)[0];
        let trapped = builder.ins().icmp_imm_s(IntCC::NotEqual, status, 0);
        let trap_block = builder.create_block();
        let return_block = builder.create_block();
        builder.ins().brif(trapped, trap_block, &[], return_block, &[]);

        builder.switch_to_block(trap_block);
        builder.seal_block(trap_block);
        let raise_signature = builder.import_signature(Signature::new(host_cc));
        let raise_name = builder.func.declare_imported_user_function(UserExternalName {
            namespace: HELPER_EXTERNAL_NAMESPACE,
            index: HelperId::raise_trap as u32,
        });
        let raise_function = builder.func.import_function(ExtFuncData {
            name: ExternalName::user(raise_name),
            signature: raise_signature,
            colocated: false,
            patchable: false,
        });
        let raise_address = builder.ins().func_addr(ptr_type, raise_function);
        builder.ins().call_indirect(raise_signature, raise_address, &[]);
        if let Some(&result_kind) = function_type.results.first() {
            let zero = builder.ins().iconst(types::I64, 0);
            let zero = payload_to_value(&mut builder, zero, result_kind)?;
            builder.ins().return_(&[zero]);
        } else {
            builder.ins().return_(&[]);
        }

        builder.switch_to_block(return_block);
        builder.seal_block(return_block);
        builder.ins().store(
            memory_flags.configuration,
            original_top,
            configuration,
            runtime_layout.value_stack_top_offset,
        );
        if let Some(&result_kind) = function_type.results.first() {
            let result = builder.ins().load(types::I64, memory_flags.activation, original_top, 0);
            let result = payload_to_value(&mut builder, result, result_kind)?;
            builder.ins().return_(&[result]);
        } else {
            builder.ins().return_(&[]);
        }

        builder.finalize(isa.frontend_config());
        compile_function(isa, function)
    }

    fn local_accesses(insn: &CraneliftInsn) -> LocalAccesses {
        let local_index = |index| usize::try_from(index).ok();
        match insn.opcode {
            op::LOCAL_GET | op::SYNTHETIC_ARGUMENT_GET => LocalAccesses {
                reads: [local_index(insn.imm1), None],
                write: None,
            },
            op::LOCAL_SET | op::LOCAL_TEE | op::SYNTHETIC_ARGUMENT_SET | op::SYNTHETIC_ARGUMENT_TEE => LocalAccesses {
                reads: [None, None],
                write: local_index(insn.imm1),
            },
            opcode if (op::SYNTHETIC_LOCAL_GET_0..=op::SYNTHETIC_LOCAL_GET_7).contains(&opcode) => LocalAccesses {
                reads: [Some((opcode - op::SYNTHETIC_LOCAL_GET_0) as usize), None],
                write: None,
            },
            opcode if (op::SYNTHETIC_LOCAL_SET_0..=op::SYNTHETIC_LOCAL_SET_7).contains(&opcode) => LocalAccesses {
                reads: [None, None],
                write: Some((opcode - op::SYNTHETIC_LOCAL_SET_0) as usize),
            },
            op::SYNTHETIC_LOCAL_COPY => LocalAccesses {
                reads: [local_index(insn.imm1), None],
                write: local_index(insn.imm2),
            },
            op::SYNTHETIC_LOCAL_SETI32_CONST | op::SYNTHETIC_LOCAL_SETI64_CONST => LocalAccesses {
                reads: [None, None],
                write: local_index(insn.imm2),
            },
            op::SYNTHETIC_I32_ADD2LOCAL | op::SYNTHETIC_I64_ADD2LOCAL => LocalAccesses {
                reads: [local_index(insn.imm1), local_index(insn.imm2)],
                write: None,
            },
            op::SYNTHETIC_I32_ADDCONSTLOCAL
            | op::SYNTHETIC_I32_ANDCONSTLOCAL
            | op::SYNTHETIC_I64_ADDCONSTLOCAL
            | op::SYNTHETIC_I64_ANDCONSTLOCAL
            | op::SYNTHETIC_I32_STORELOCAL
            | op::SYNTHETIC_I64_STORELOCAL => LocalAccesses {
                reads: [local_index(insn.imm2), None],
                write: None,
            },
            opcode
                if (op::SYNTHETIC_I32_SUB2LOCAL..=op::SYNTHETIC_I32_SHRS2LOCAL).contains(&opcode)
                    || (op::SYNTHETIC_I64_SUB2LOCAL..=op::SYNTHETIC_I64_SHRS2LOCAL).contains(&opcode) =>
            {
                LocalAccesses {
                    reads: [local_index(insn.imm1), local_index(insn.imm2)],
                    write: None,
                }
            }
            _ => LocalAccesses::default(),
        }
    }

    pub fn compile_to_bytes(
        insns: &[CraneliftInsn],
        layout: &SerializedRuntimeLayout,
        options: FunctionCompilationOptions,
        local_types: &[u8],
        function_types: &[WasmFunctionType<'_>],
    ) -> Result<CompiledFunction, &'static str> {
        let FunctionCompilationOptions {
            result_arity,
            num_locals,
            num_params,
            function_index,
            max_call_rec_size,
        } = options;

        let function_type = *function_types
            .get(function_index as usize)
            .ok_or("missing function type")?;
        if function_type.parameters.len() != num_params as usize || function_type.results.len() != result_arity as usize
        {
            return Err("function type does not match compilation options");
        }

        for insn in insns {
            if !Self::is_supported(insn) {
                return Err("unsupported instruction");
            }
            if matches!(insn.opcode, op::BLOCK | op::LOOP | op::IF) {
                let arity = insn.imm3 & 0xffff;
                if arity > 1 {
                    return Err("multi-value blocks not supported");
                }
            }
            // Note: op::CALL is used for multi-value returns but also for some
            // single-return calls. We handle it via flush_vstack_to_real before the call.
        }
        let register_liveness = RegisterLiveness::analyze(insns)?;

        let mut flag_builder = settings::builder();
        flag_builder.set("opt_level", "speed").unwrap();
        flag_builder.set("is_pic", "false").unwrap();
        let flags = settings::Flags::new(flag_builder);
        let isa = cranelift_native::builder()
            .map_err(|_| "unsupported host architecture")?
            .finish(flags)
            .map_err(|_| "failed to build ISA")?;

        // Function signature uses the handler_ptr parameters, but has no return value: entering
        // through this adapter transfers the current activation to native code until completion.
        //   void fn(void* interpreter, void* configuration, void* insn, u32 short_ip, void* cc, void* addrs)
        let ptr_type = isa.pointer_type();
        let host_cc = isa.default_call_conv();
        let mut sig = Signature::new(host_cc);
        sig.params.push(AbiParam::new(ptr_type)); // interpreter
        sig.params.push(AbiParam::new(ptr_type)); // configuration
        sig.params.push(AbiParam::new(ptr_type)); // instruction (unused)
        sig.params.push(AbiParam::new(types::I32)); // short_ip (unused)
        sig.params.push(AbiParam::new(ptr_type)); // cc (unused)
        sig.params.push(AbiParam::new(ptr_type)); // addresses_ptr (unused)
        let handler_signature = sig;
        let native_signature = Self::native_signature(&*isa, function_type)?;
        let mut func = Function::with_name_signature(UserFuncName::user(0, function_index), native_signature.clone());
        let memory_flags = WasmMemoryFlags::new(&mut func);
        let mut builder_ctx = FunctionBuilderContext::new();
        let mut builder = FunctionBuilder::new(&mut func, &mut builder_ctx);

        // Declare variables for virtual registers R0-R7. Each register has one selected bank at
        // a time; canonical i64 payloads are reconstructed only when a consumer requires one.
        let reg_vars: [Variable; REG_COUNT] = std::array::from_fn(|_| builder.declare_var(types::I64));
        let reg_vars_i32: [Variable; REG_COUNT] = std::array::from_fn(|_| builder.declare_var(types::I32));

        let entry_block = builder.create_block();
        builder.append_block_params_for_function_params(entry_block);
        builder.switch_to_block(entry_block);
        builder.seal_block(entry_block);

        let interpreter_val = builder.block_params(entry_block)[0];
        let configuration_val = builder.block_params(entry_block)[1];

        // Load regs[0..7] from configuration. regs is at offset `regs_offset` from Configuration*.
        // Each Value is `value_size` bytes; the low 8 bytes are the i64 payload.
        let runtime_layout = RuntimeLayout::new(layout);
        let regs_offset = runtime_layout.regs_offset;
        let value_size = runtime_layout.value_size;
        for (i, var) in reg_vars.iter().enumerate() {
            let offset = regs_offset + (i as i32) * value_size;
            let val = builder
                .ins()
                .load(types::I64, memory_flags.configuration, configuration_val, offset);
            builder.def_var(*var, val);
            let val_i32 = builder.ins().ireduce(types::I32, val);
            builder.def_var(reg_vars_i32[i], val_i32);
        }

        let epilogue_block = builder.create_block();
        let trap_block = builder.create_block();

        let LegacyImportedHelpers {
            call_function_signature: call_fn_sig,
            call_indirect_signature: call_indirect_sig,
            memory_copy_signature: memory_copy_sig,
            memory_fill_signature: memory_fill_sig,
            memory_size_signature: mem_size_sig,
            memory_grow_signature: mem_grow_sig,
            stack_exhaustion_signature: stack_exhaustion_sig,
            check_indirect_type_signature: check_indirect_type_sig,
            raise_trap_signature: raise_trap_sig,
            call_function: h_call_fn,
            memory_size: h_mem_size,
            memory_grow: h_mem_grow,
            call_indirect: h_call_indirect,
            call_indirect_with_record: h_call_indirect_wr,
            memory_copy: h_memory_copy,
            memory_fill: h_memory_fill,
            primitive_storage_cage_base: h_primitive_storage_cage_base,
            stack_exhaustion: h_stack_exhaustion,
            raise_trap: h_raise_trap,
            check_indirect_type: h_check_indirect_type,
        } = LegacyImportedHelpers::new(&mut builder, ptr_type, host_cc);
        let mut direct_call_targets = HashMap::new();
        for insn in insns.iter().filter(|insn| {
            insn.opcode == op::CALL
                || (op::SYNTHETIC_CALL_00..=op::SYNTHETIC_CALL_31).contains(&insn.opcode)
                || matches!(
                    insn.opcode,
                    op::SYNTHETIC_CALL_WITH_RECORD_0 | op::SYNTHETIC_CALL_WITH_RECORD_1
                )
        }) {
            let target_index = u32::try_from(insn.imm1).map_err(|_| "invalid direct-call target")?;
            if let std::collections::hash_map::Entry::Vacant(entry) = direct_call_targets.entry(target_index) {
                let target_type = *function_types
                    .get(target_index as usize)
                    .ok_or("missing direct-call function type")?;
                if insn.opcode == op::CALL && !Self::uses_register_native_abi(target_type) {
                    continue;
                }
                let target_signature = match Self::native_signature(&*isa, target_type) {
                    Ok(signature) => signature,
                    Err(_) if insn.opcode == op::CALL => continue,
                    Err(error) => return Err(error),
                };
                let target_signature = builder.import_signature(target_signature);
                let user_ref = builder.func.declare_imported_user_function(UserExternalName {
                    namespace: WASM_FUNCTION_EXTERNAL_NAMESPACE,
                    index: target_index,
                });
                let target = builder.func.import_function(ExtFuncData {
                    name: ExternalName::user(user_ref),
                    signature: target_signature,
                    colocated: true,
                    patchable: false,
                });
                entry.insert(target);
            }
        }
        let RuntimeLayout {
            locals_base_offset,
            table_instances_offset,
            memory_instances_offset,
            global_instances_offset,
            global_instance_value_offset,
            memory_instance_data_offset,
            memory_buffer_storage_offset_offset,
            compiled_call_result_scratch_offset,
            value_stack_base_offset,
            value_stack_top_offset,
            call_record_base_offset,
            call_record_stack_top_offset,
            depth_offset,
            current_compiled_fn_table_data_offset,
            current_module_offset,
            current_canonical_types_offset,
            current_expression_offset,
            compiled_function_entry_size,
            compiled_function_entry_expression_offset,
            table_instance_size_offset,
            table_instance_callables_offset,
            callable_defined_type_offset,
            callable_module_offset,
            callable_compiled_instructions_offset,
            compiled_instructions_native_entry_offset,
            ..
        } = runtime_layout;
        let native_indirect_call_layout = NativeIndirectCallLayout {
            table_instances: table_instances_offset,
            current_module: current_module_offset,
            current_canonical_types: current_canonical_types_offset,
            table_instance_size: table_instance_size_offset,
            table_instance_callables: table_instance_callables_offset,
            callable_defined_type: callable_defined_type_offset,
            callable_module: callable_module_offset,
            callable_compiled_instructions: callable_compiled_instructions_offset,
            compiled_instructions_native_entry: compiled_instructions_native_entry_offset,
        };
        let num_locals = num_locals as usize;
        let num_params = num_params as usize;
        let local_is_f64: Vec<bool> = (0..num_locals)
            .map(|i| local_types.get(i).copied() == Some(F64_KIND))
            .collect();
        let local_is_f32: Vec<bool> = (0..num_locals)
            .map(|i| local_types.get(i).copied() == Some(F32_KIND))
            .collect();
        let local_is_i32: Vec<bool> = (0..num_locals)
            .map(|i| local_types.get(i).copied() == Some(I32_KIND))
            .collect();
        let mut local_is_accessed = vec![false; num_locals];
        for insn in insns {
            let accesses = Self::local_accesses(insn);
            for local_index in accesses.reads.into_iter().flatten() {
                if let Some(accessed) = local_is_accessed.get_mut(local_index) {
                    *accessed = true;
                }
            }
            if let Some(local_index) = accesses.write
                && let Some(accessed) = local_is_accessed.get_mut(local_index)
            {
                *accessed = true;
            }
        }
        // Accesses to memory32 are unchecked and may fault; the fault handler turns
        // faults inside a memory's guarded reservation into wasm traps.
        let wasm_memory_flags = memory_flags.linear_memory;
        let interp_var = builder.declare_var(ptr_type);
        builder.def_var(interp_var, interpreter_val);
        let config_var = builder.declare_var(ptr_type);
        builder.def_var(config_var, configuration_val);

        let direct_call_mode_var = builder.declare_var(types::I8);
        let entry_token = builder.block_params(entry_block)[2];
        let direct_call_mode = builder.ins().icmp_imm_s(IntCC::Equal, entry_token, 0);
        builder.def_var(direct_call_mode_var, direct_call_mode);

        let saved_call_record_base_var = builder.declare_var(ptr_type);
        let saved_call_record_top_var = builder.declare_var(ptr_type);
        let saved_expression_var = builder.declare_var(ptr_type);
        let saved_depth_var = builder.declare_var(ptr_type);
        for var in [
            saved_call_record_base_var,
            saved_call_record_top_var,
            saved_expression_var,
            saved_depth_var,
        ] {
            let zero = builder.ins().iconst(ptr_type, 0);
            builder.def_var(var, zero);
        }

        let direct_setup = builder.create_block();
        let normal_entry = builder.create_block();
        let setup_done = builder.create_block();
        builder
            .ins()
            .brif(direct_call_mode, direct_setup, &[], normal_entry, &[]);

        builder.switch_to_block(direct_setup);
        builder.seal_block(direct_setup);
        let cfg = builder.use_var(config_var);
        let saved_call_record = builder
            .ins()
            .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
        let saved_call_record_top =
            builder
                .ins()
                .load(ptr_type, memory_flags.configuration, cfg, call_record_stack_top_offset);
        let saved_expression = builder
            .ins()
            .load(ptr_type, memory_flags.configuration, cfg, current_expression_offset);
        let saved_depth = builder
            .ins()
            .load(ptr_type, memory_flags.configuration, cfg, depth_offset);
        builder.def_var(saved_call_record_base_var, saved_call_record);
        builder.def_var(saved_call_record_top_var, saved_call_record_top);
        builder.def_var(saved_expression_var, saved_expression);
        builder.def_var(saved_depth_var, saved_depth);

        let direct_setup_body = builder.create_block();
        let direct_stack_exhausted = builder.create_block();
        let depth_exhausted = builder.ins().icmp_imm_s(IntCC::UnsignedGreaterThan, saved_depth, 500);
        builder
            .ins()
            .brif(depth_exhausted, direct_stack_exhausted, &[], direct_setup_body, &[]);

        builder.switch_to_block(direct_stack_exhausted);
        builder.seal_block(direct_stack_exhausted);
        let stack_exhaustion = builder.ins().func_addr(ptr_type, h_stack_exhaustion);
        let interpreter = builder.use_var(interp_var);
        builder
            .ins()
            .call_indirect(stack_exhaustion_sig, stack_exhaustion, &[interpreter]);
        builder.ins().jump(trap_block, &[]);

        builder.switch_to_block(direct_setup_body);
        builder.seal_block(direct_setup_body);
        let uses_register_native_abi = Self::uses_register_native_abi(function_type);
        let table_data = builder.ins().load(
            ptr_type,
            memory_flags.configuration,
            cfg,
            current_compiled_fn_table_data_offset,
        );
        let entry_function_index = builder.ins().iconst(types::I32, i64::from(function_index));
        let target_index = if ptr_type == types::I64 {
            builder.ins().uextend(types::I64, entry_function_index)
        } else {
            entry_function_index
        };
        let entry_offset = builder.ins().imul_imm_s(target_index, compiled_function_entry_size);
        let entry = builder.ins().iadd(table_data, entry_offset);
        let expression = builder.ins().load(
            ptr_type,
            memory_flags.runtime_metadata,
            entry,
            compiled_function_entry_expression_offset,
        );
        builder
            .ins()
            .store(memory_flags.configuration, expression, cfg, current_expression_offset);

        if max_call_rec_size > 0 {
            builder.ins().store(
                memory_flags.configuration,
                saved_call_record_top,
                cfg,
                call_record_base_offset,
            );
            let next_call_record_top = builder.ins().iadd_imm_s(
                saved_call_record_top,
                i64::from(max_call_rec_size) * i64::from(value_size),
            );
            builder.ins().store(
                memory_flags.configuration,
                next_call_record_top,
                cfg,
                call_record_stack_top_offset,
            );
        } else {
            let null = builder.ins().iconst(ptr_type, 0);
            builder
                .ins()
                .store(memory_flags.configuration, null, cfg, call_record_base_offset);
        }
        let next_depth = builder.ins().iadd_imm_s(saved_depth, 1);
        builder
            .ins()
            .store(memory_flags.configuration, next_depth, cfg, depth_offset);
        builder.ins().jump(setup_done, &[]);

        builder.switch_to_block(normal_entry);
        builder.seal_block(normal_entry);
        builder.ins().jump(setup_done, &[]);

        builder.switch_to_block(setup_done);
        builder.seal_block(setup_done);
        let indirect_call_lowering_context = IndirectCallLoweringContext {
            ptr_type,
            bridge_signature: call_indirect_sig,
            bridge_helper: h_call_indirect_wr,
            check_type_signature: check_indirect_type_sig,
            check_type_helper: h_check_indirect_type,
            trap_block,
            result_scratch_offset: compiled_call_result_scratch_offset,
            call_record_base_offset,
            value_size,
            native_layout: native_indirect_call_layout,
            memory_flags,
        };
        let is_scalar_memory_access = |opcode: u64| {
            matches!(
                opcode,
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
                    | op::I64_LOAD32_U
                    | op::I32_STORE
                    | op::I64_STORE
                    | op::F32_STORE
                    | op::F64_STORE
                    | op::I32_STORE8
                    | op::I32_STORE16
                    | op::I64_STORE8
                    | op::I64_STORE16
                    | op::I64_STORE32
                    | op::SYNTHETIC_I32_STORELOCAL
                    | op::SYNTHETIC_I64_STORELOCAL
            )
        };
        let mut used_memory_indices: Vec<u32> = insns
            .iter()
            .filter(|insn| is_scalar_memory_access(insn.opcode))
            .map(|insn| insn.imm3)
            .collect();
        used_memory_indices.sort_unstable();
        used_memory_indices.dedup();

        let mut memory_bases = Vec::with_capacity(used_memory_indices.len());
        if !used_memory_indices.is_empty() {
            // Each base address is stable for the whole call: memory32 storage reserves its
            // maximum (plus guard) up front, so growing commits in place and never moves it.
            let cage_base_storage = builder.ins().func_addr(ptr_type, h_primitive_storage_cage_base);
            let cage_base = builder
                .ins()
                .load(ptr_type, memory_flags.runtime_metadata, cage_base_storage, 0);
            let memory_instances = builder.ins().load(
                ptr_type,
                memory_flags.configuration,
                configuration_val,
                memory_instances_offset,
            );
            for memory_index in used_memory_indices {
                let memory_pointer_offset = builder
                    .ins()
                    .iconst(ptr_type, i64::from(memory_index) * i64::from(ptr_type.bytes()));
                let memory_pointer_address = builder.ins().iadd(memory_instances, memory_pointer_offset);
                let memory = builder
                    .ins()
                    .load(ptr_type, memory_flags.runtime_metadata, memory_pointer_address, 0);
                let storage_offset = builder.ins().load(
                    types::I64,
                    memory_flags.runtime_metadata,
                    memory,
                    memory_instance_data_offset + memory_buffer_storage_offset_offset,
                );
                let storage_offset = if ptr_type == types::I64 {
                    storage_offset
                } else {
                    builder.ins().ireduce(ptr_type, storage_offset)
                };
                let memory_base = builder.ins().iadd(cage_base, storage_offset);
                memory_bases.push((memory_index, memory_base));
            }
        }

        let mut used_global_indices: Vec<u32> = insns
            .iter()
            .filter(|insn| matches!(insn.opcode, op::GLOBAL_GET | op::GLOBAL_SET))
            .map(|insn| insn.imm1 as u32)
            .collect();
        used_global_indices.sort_unstable();
        used_global_indices.dedup();

        let mut global_instances = Vec::with_capacity(used_global_indices.len());
        if !used_global_indices.is_empty() {
            let globals = builder.ins().load(
                ptr_type,
                memory_flags.configuration,
                configuration_val,
                global_instances_offset,
            );
            for global_index in used_global_indices {
                let global_pointer_offset = builder
                    .ins()
                    .iconst(ptr_type, i64::from(global_index) * i64::from(ptr_type.bytes()));
                let global_pointer_address = builder.ins().iadd(globals, global_pointer_offset);
                let global = builder
                    .ins()
                    .load(ptr_type, memory_flags.globals, global_pointer_address, 0);
                global_instances.push((global_index, global));
            }
        }

        let mut control_stack: Vec<ControlFrame> = Vec::new();
        let mut epilogue_reg_ty: Option<[Bank; REG_COUNT]> = None;

        // Virtual stack, to avoid touching the interpreter-side stack as much as possible.
        let has_raw_call = insns
            .iter()
            .any(|i| i.opcode == op::CALL || i.opcode == op::CALL_INDIRECT);
        // We can't easily track across control flow merges, so count destinations instead. Raw
        // calls are variadic in the bytecode representation and may produce multiple stack slots.
        let raw_call_result_slots = insns
            .iter()
            .filter(|i| i.opcode == op::CALL || i.opcode == op::CALL_INDIRECT)
            .map(|i| i.call_result_count as usize)
            .sum::<usize>();
        let max_stack_depth = insns
            .iter()
            .filter(|i| i.destination == STACK_MARKER)
            .count()
            .saturating_add(raw_call_result_slots)
            .max(16);
        let mut is_unreachable = false;
        let mut dirty_regs = [false; REG_COUNT];
        let mut stack_vars: Vec<Variable> = Vec::with_capacity(max_stack_depth);
        let mut stack_vars_i32: Vec<Variable> = Vec::with_capacity(max_stack_depth);
        for _ in 0..max_stack_depth {
            let var = builder.declare_var(types::I64);
            let zero = builder.ins().iconst(types::I64, 0);
            builder.def_var(var, zero);
            stack_vars.push(var);

            let var_i32 = builder.declare_var(types::I32);
            let zero_i32 = builder.ins().iconst(types::I32, 0);
            builder.def_var(var_i32, zero_i32);
            stack_vars_i32.push(var_i32);
        }
        let mut sp: usize = 0;

        // If we allocate something on the stack, make sure to restore the stack at the end.
        let initial_stack_size_var = builder.declare_var(types::I64);

        let reg_vars_f64: [Variable; REG_COUNT] = std::array::from_fn(|_| builder.declare_var(types::F64));
        let stack_vars_f64: Vec<Variable> = (0..max_stack_depth).map(|_| builder.declare_var(types::F64)).collect();
        let reg_vars_f32: [Variable; REG_COUNT] = std::array::from_fn(|_| builder.declare_var(types::F32));
        let stack_vars_f32: Vec<Variable> = (0..max_stack_depth).map(|_| builder.declare_var(types::F32)).collect();
        let mut reg_ty = [Bank::I64; REG_COUNT];
        let mut stack_ty = vec![Bank::I64; max_stack_depth];

        // FunctionBuilder's SSA construction adds block parameters only where a later use needs
        // to merge definitions. Locals therefore remain typed SSA across the CFG without a
        // permanent frontend-requested stack slot; Cranelift may still spill under real pressure.
        let local_vars: Vec<Option<Variable>> = local_is_accessed
            .iter()
            .enumerate()
            .map(|(i, &accessed)| {
                if !accessed {
                    return None;
                }
                let ty = if local_is_f64[i] {
                    types::F64
                } else if local_is_f32[i] {
                    types::F32
                } else if local_is_i32[i] {
                    types::I32
                } else {
                    types::I64
                };
                Some(builder.declare_var(ty))
            })
            .collect();

        // set_frame_lightweight verifies the stack-usage hint before these unchecked operations.
        macro_rules! emit_stack_push {
            ($builder:expr, $val:expr) => {{
                let v = $val;
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, memory_flags.configuration, cfg, value_stack_top_offset);
                $builder.ins().store(memory_flags.activation, v, top, 0);
                let zero_tag = $builder.ins().iconst(types::I64, 0);
                $builder.ins().store(memory_flags.activation, zero_tag, top, 8);
                let new_top = $builder.ins().iadd_imm_s(top, i64::from(value_size));
                $builder
                    .ins()
                    .store(memory_flags.configuration, new_top, cfg, value_stack_top_offset);
            }};
        }
        macro_rules! emit_stack_pop {
            ($builder:expr) => {{
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, memory_flags.configuration, cfg, value_stack_top_offset);
                let new_top = $builder.ins().iadd_imm_s(top, -i64::from(value_size));
                $builder
                    .ins()
                    .store(memory_flags.configuration, new_top, cfg, value_stack_top_offset);
                $builder
                    .ins()
                    .load(types::I64, memory_flags.activation, new_top, 0)
            }};
        }
        macro_rules! emit_stack_size {
            ($builder:expr) => {{
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, memory_flags.configuration, cfg, value_stack_top_offset);
                let base = $builder
                    .ins()
                    .load(ptr_type, memory_flags.configuration, cfg, value_stack_base_offset);
                let bytes = $builder.ins().isub(top, base);
                $builder.ins().ushr_imm_u(bytes, 4)
            }};
        }
        // Trim the real stack to `target_size + arity` values, keeping the top `arity` values
        // verbatim (helper-pushed call results carry real tags). Validation guarantees at least
        // that many values are present -- the vstack branch move relies on the same invariant.
        macro_rules! emit_stack_cleanup {
            ($builder:expr, $target_size:expr, $arity:expr) => {{
                debug_assert!(($arity as usize) <= 1);
                let cfg = $builder.use_var(config_var);
                let base = $builder
                    .ins()
                    .load(ptr_type, memory_flags.configuration, cfg, value_stack_base_offset);
                let target_bytes = $builder.ins().ishl_imm_u($target_size, 4);
                let trimmed_top = $builder.ins().iadd(base, target_bytes);
                let new_top = if $arity as usize > 0 {
                    let top = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, value_stack_top_offset);
                    let bits = $builder
                        .ins()
                        .load(types::I64, memory_flags.activation, top, -value_size);
                    let tag = $builder
                        .ins()
                        .load(types::I64, memory_flags.activation, top, -value_size + 8);
                    $builder
                        .ins()
                        .store(memory_flags.activation, bits, trimmed_top, 0);
                    $builder.ins().store(memory_flags.activation, tag, trimmed_top, 8);
                    $builder.ins().iadd_imm_s(trimmed_top, i64::from(value_size))
                } else {
                    trimmed_top
                };
                $builder
                    .ins()
                    .store(memory_flags.configuration, new_top, cfg, value_stack_top_offset);
            }};
        }

        if has_raw_call {
            let initial_stack_size = emit_stack_size!(builder);
            builder.def_var(initial_stack_size_var, initial_stack_size);
        } else {
            let zero = builder.ins().iconst(types::I64, 0);
            builder.def_var(initial_stack_size_var, zero);
        }

        macro_rules! register_payload {
            ($builder:expr, $index:expr, $bank:expr) => {{
                let index = $index;
                match $bank {
                    Bank::I32 => {
                        let value = $builder.use_var(reg_vars_i32[index]);
                        $builder.ins().uextend(types::I64, value)
                    }
                    Bank::I64 => $builder.use_var(reg_vars[index]),
                    Bank::F32 => {
                        let value = $builder.use_var(reg_vars_f32[index]);
                        let bits = $builder.ins().bitcast(types::I32, MemFlags::new(), value);
                        $builder.ins().sextend(types::I64, bits)
                    }
                    Bank::F64 => {
                        let value = $builder.use_var(reg_vars_f64[index]);
                        $builder.ins().bitcast(types::I64, MemFlags::new(), value)
                    }
                }
            }};
        }

        macro_rules! stack_payload {
            ($builder:expr, $index:expr, $bank:expr) => {{
                let index = $index;
                match $bank {
                    Bank::I32 => {
                        let value = $builder.use_var(stack_vars_i32[index]);
                        $builder.ins().uextend(types::I64, value)
                    }
                    Bank::I64 => $builder.use_var(stack_vars[index]),
                    Bank::F32 => {
                        let value = $builder.use_var(stack_vars_f32[index]);
                        let bits = $builder.ins().bitcast(types::I32, MemFlags::new(), value);
                        $builder.ins().sextend(types::I64, bits)
                    }
                    Bank::F64 => {
                        let value = $builder.use_var(stack_vars_f64[index]);
                        $builder.ins().bitcast(types::I64, MemFlags::new(), value)
                    }
                }
            }};
        }

        macro_rules! stack_value_as_bank {
            ($builder:expr, $index:expr, $target_bank:expr) => {{
                let index = $index;
                let target_bank = $target_bank;
                if stack_ty[index] == target_bank {
                    match target_bank {
                        Bank::I32 => $builder.use_var(stack_vars_i32[index]),
                        Bank::I64 => $builder.use_var(stack_vars[index]),
                        Bank::F32 => $builder.use_var(stack_vars_f32[index]),
                        Bank::F64 => $builder.use_var(stack_vars_f64[index]),
                    }
                } else {
                    let payload = stack_payload!($builder, index, stack_ty[index]);
                    match target_bank {
                        Bank::I32 => $builder.ins().ireduce(types::I32, payload),
                        Bank::I64 => payload,
                        Bank::F32 => {
                            let bits = $builder.ins().ireduce(types::I32, payload);
                            $builder.ins().bitcast(types::F32, MemFlags::new(), bits)
                        }
                        Bank::F64 => $builder.ins().bitcast(types::F64, MemFlags::new(), payload),
                    }
                }
            }};
        }

        // Read a value from a source location (register, virtual stack, or call record)
        macro_rules! read_src {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    let index = src as usize;
                    let payload = register_payload!($builder, index, reg_ty[index]);
                    if reg_ty[index] != Bank::I64 {
                        $builder.def_var(reg_vars[index], payload);
                        reg_ty[index] = Bank::I64;
                    }
                    payload
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        // we have vstack, so just allocate on the native stack.
                        sp -= 1;
                        stack_payload!($builder, sp, stack_ty[sp])
                    } else {
                        emit_stack_pop!($builder)
                    }
                } else {
                    // Frame entry allocated the record eagerly, so the read is a plain load.
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(src - CALLREC_BASE) * value_size;
                    $builder
                        .ins()
                        .load(types::I64, memory_flags.activation, base, off)
                }
            }};
        }

        macro_rules! read_src_i32 {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    let index = src as usize;
                    if reg_ty[index] == Bank::I32 {
                        $builder.use_var(reg_vars_i32[index])
                    } else {
                        let payload = register_payload!($builder, index, reg_ty[index]);
                        let value = $builder.ins().ireduce(types::I32, payload);
                        $builder.def_var(reg_vars_i32[index], value);
                        reg_ty[index] = Bank::I32;
                        value
                    }
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        sp -= 1;
                        if stack_ty[sp] == Bank::I32 {
                            $builder.use_var(stack_vars_i32[sp])
                        } else {
                            let payload = stack_payload!($builder, sp, stack_ty[sp]);
                            $builder.ins().ireduce(types::I32, payload)
                        }
                    } else {
                        let payload = emit_stack_pop!($builder);
                        $builder.ins().ireduce(types::I32, payload)
                    }
                } else {
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(src - CALLREC_BASE) * value_size;
                    $builder
                        .ins()
                        .load(types::I32, memory_flags.activation, base, off)
                }
            }};
        }

        macro_rules! src_is_i32 {
            ($src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    reg_ty[src as usize] == Bank::I32
                } else if src == STACK_MARKER {
                    max_stack_depth > 0 && sp > 0 && stack_ty[sp - 1] == Bank::I32
                } else {
                    false
                }
            }};
        }

        // Temporarily materialize only the top `count` virtual values for a runtime call whose
        // stack ABI consumes exactly that argument suffix. The saved top remains valid because
        // ValueStack storage cannot move while a frame is active.
        macro_rules! materialize_vstack_suffix_to_real {
            ($builder:expr, $count:expr) => {{
                let count = $count as usize;
                debug_assert!(sp >= count);
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, memory_flags.configuration, cfg, value_stack_top_offset);
                if count > 0 {
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    for i in 0..count {
                        let index = sp - count + i;
                        let val = stack_payload!($builder, index, stack_ty[index]);
                        let offset = (i as i32) * value_size;
                        $builder.ins().store(memory_flags.activation, val, top, offset);
                        $builder
                            .ins()
                            .store(memory_flags.activation, zero_tag, top, offset + 8);
                    }
                    let new_top = $builder
                        .ins()
                        .iadd_imm_s(top, i64::from(count as i32 * value_size));
                    $builder
                        .ins()
                        .store(memory_flags.configuration, new_top, cfg, value_stack_top_offset);
                }
                top
            }};
        }
        // Rebuild the virtual stack from the results left by an opaque runtime call, then discard
        // the temporary real-stack materialization.
        macro_rules! restore_vstack_after_raw_call {
            ($builder:expr, $original_top:expr, $stack_base:expr, $result_count:expr, $destination:expr) => {{
                let stack_base = $stack_base;
                let result_count = $result_count;
                let destination = $destination;
                debug_assert!(stack_base + result_count <= max_stack_depth);

                let cfg = $builder.use_var(config_var);
                let helper_top = $builder
                    .ins()
                    .load(ptr_type, memory_flags.configuration, cfg, value_stack_top_offset);
                let result_bytes = (result_count as i64) * i64::from(value_size);
                let mut result_address = $builder.ins().iadd_imm_s(helper_top, -result_bytes);

                sp = stack_base;
                if destination == STACK_MARKER {
                    for i in 0..result_count {
                        let result = $builder
                            .ins()
                            .load(types::I64, memory_flags.activation, result_address, 0);
                        $builder.def_var(stack_vars[stack_base + i], result);
                        stack_ty[stack_base + i] = Bank::I64;
                        result_address = $builder.ins().iadd_imm_s(result_address, i64::from(value_size));
                    }
                    sp += result_count;
                } else {
                    debug_assert!(result_count <= 1);
                    if result_count == 1 {
                        let result = $builder
                            .ins()
                            .load(types::I64, memory_flags.activation, result_address, 0);
                        write_dst!($builder, destination, result);
                    }
                }

                $builder.ins().store(
                    memory_flags.configuration,
                    $original_top,
                    cfg,
                    value_stack_top_offset,
                );
            }};
        }
        // push only the top n values from vstack to real stack
        macro_rules! push_top_n_to_real {
            ($builder:expr, $n:expr) => {{
                let n = $n as usize;
                if max_stack_depth > 0 && sp >= n && n > 0 {
                    let cfg = $builder.use_var(config_var);
                    let top = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, value_stack_top_offset);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    for i in 0..n {
                        let index = sp - n + i;
                        let val = stack_payload!($builder, index, stack_ty[index]);
                        let offset = (i as i32) * value_size;
                        $builder.ins().store(memory_flags.activation, val, top, offset);
                        $builder
                            .ins()
                            .store(memory_flags.activation, zero_tag, top, offset + 8);
                    }
                    let new_top = $builder.ins().iadd_imm_s(top, i64::from(n as i32 * value_size));
                    $builder
                        .ins()
                        .store(memory_flags.configuration, new_top, cfg, value_stack_top_offset);
                }
            }};
        }

        macro_rules! write_dst {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars[dst as usize], val);
                    reg_ty[dst as usize] = Bank::I64;
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars[sp], val);
                        stack_ty[sp] = Bank::I64;
                        sp += 1;
                    } else {
                        emit_stack_push!($builder, val);
                    }
                } else {
                    // Frame entry allocated the record eagerly, so the write is two plain stores.
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(dst - CALLREC_BASE) * value_size;
                    $builder.ins().store(memory_flags.activation, val, base, off);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    $builder
                        .ins()
                        .store(memory_flags.activation, zero_tag, base, off + 8);
                }
            }};
        }

        macro_rules! write_dst_i32 {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars_i32[dst as usize], val);
                    reg_ty[dst as usize] = Bank::I32;
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars_i32[sp], val);
                        stack_ty[sp] = Bank::I32;
                        sp += 1;
                    } else {
                        let payload = $builder.ins().uextend(types::I64, val);
                        emit_stack_push!($builder, payload);
                    }
                } else {
                    let payload = $builder.ins().uextend(types::I64, val);
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(dst - CALLREC_BASE) * value_size;
                    $builder.ins().store(memory_flags.activation, payload, base, off);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    $builder
                        .ins()
                        .store(memory_flags.activation, zero_tag, base, off + 8);
                }
            }};
        }

        macro_rules! read_src_f64 {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    let index = src as usize;
                    if reg_ty[index] == Bank::F64 {
                        $builder.use_var(reg_vars_f64[index])
                    } else {
                        let payload = register_payload!($builder, index, reg_ty[index]);
                        let value = $builder.ins().bitcast(types::F64, MemFlags::new(), payload);
                        $builder.def_var(reg_vars_f64[index], value);
                        reg_ty[index] = Bank::F64;
                        value
                    }
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        sp -= 1;
                        if stack_ty[sp] == Bank::F64 {
                            $builder.use_var(stack_vars_f64[sp])
                        } else {
                            let raw = stack_payload!($builder, sp, stack_ty[sp]);
                            $builder.ins().bitcast(types::F64, MemFlags::new(), raw)
                        }
                    } else {
                        let raw = emit_stack_pop!($builder);
                        $builder.ins().bitcast(types::F64, MemFlags::new(), raw)
                    }
                } else {
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(src - CALLREC_BASE) * value_size;
                    $builder
                        .ins()
                        .load(types::F64, memory_flags.activation, base, off)
                }
            }};
        }

        macro_rules! write_dst_f64 {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars_f64[dst as usize], val);
                    reg_ty[dst as usize] = Bank::F64;
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars_f64[sp], val);
                        stack_ty[sp] = Bank::F64;
                        sp += 1;
                    } else {
                        let bits = $builder.ins().bitcast(types::I64, MemFlags::new(), val);
                        emit_stack_push!($builder, bits);
                    }
                } else {
                    let bits = $builder.ins().bitcast(types::I64, MemFlags::new(), val);
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(dst - CALLREC_BASE) * value_size;
                    $builder.ins().store(memory_flags.activation, bits, base, off);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    $builder
                        .ins()
                        .store(memory_flags.activation, zero_tag, base, off + 8);
                }
            }};
        }

        macro_rules! read_src_f32 {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    let index = src as usize;
                    if reg_ty[index] == Bank::F32 {
                        $builder.use_var(reg_vars_f32[index])
                    } else {
                        let payload = register_payload!($builder, index, reg_ty[index]);
                        let bits = $builder.ins().ireduce(types::I32, payload);
                        let value = $builder.ins().bitcast(types::F32, MemFlags::new(), bits);
                        $builder.def_var(reg_vars_f32[index], value);
                        reg_ty[index] = Bank::F32;
                        value
                    }
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        sp -= 1;
                        if stack_ty[sp] == Bank::F32 {
                            $builder.use_var(stack_vars_f32[sp])
                        } else {
                            let raw = stack_payload!($builder, sp, stack_ty[sp]);
                            let raw32 = $builder.ins().ireduce(types::I32, raw);
                            $builder.ins().bitcast(types::F32, MemFlags::new(), raw32)
                        }
                    } else {
                        let raw = emit_stack_pop!($builder);
                        let raw32 = $builder.ins().ireduce(types::I32, raw);
                        $builder.ins().bitcast(types::F32, MemFlags::new(), raw32)
                    }
                } else {
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(src - CALLREC_BASE) * value_size;
                    $builder
                        .ins()
                        .load(types::F32, memory_flags.activation, base, off)
                }
            }};
        }

        macro_rules! write_dst_f32 {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars_f32[dst as usize], val);
                    reg_ty[dst as usize] = Bank::F32;
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars_f32[sp], val);
                        stack_ty[sp] = Bank::F32;
                        sp += 1;
                    } else {
                        let bits32 = $builder.ins().bitcast(types::I32, MemFlags::new(), val);
                        let bits = $builder.ins().sextend(types::I64, bits32);
                        emit_stack_push!($builder, bits);
                    }
                } else {
                    let bits32 = $builder.ins().bitcast(types::I32, MemFlags::new(), val);
                    let bits = $builder.ins().sextend(types::I64, bits32);
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, call_record_base_offset);
                    let off = i32::from(dst - CALLREC_BASE) * value_size;
                    $builder.ins().store(memory_flags.activation, bits, base, off);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    $builder
                        .ins()
                        .store(memory_flags.activation, zero_tag, base, off + 8);
                }
            }};
        }

        macro_rules! sync_registers_to_config {
            ($builder:expr) => {{
                let config = $builder.use_var(config_var);
                for index in 0..REG_COUNT {
                    if !dirty_regs[index] {
                        continue;
                    }
                    let payload = register_payload!($builder, index, reg_ty[index]);
                    let offset = regs_offset + (index as i32) * value_size;
                    $builder
                        .ins()
                        .store(memory_flags.configuration, payload, config, offset);
                    let zero = $builder.ins().iconst(types::I64, 0);
                    $builder
                        .ins()
                        .store(memory_flags.configuration, zero, config, offset + 8);
                }
            }};
        }

        macro_rules! materialize_register_bank {
            ($builder:expr, $index:expr, $source_bank:expr, $target_bank:expr) => {{
                let index = $index;
                let payload = register_payload!($builder, index, $source_bank);
                match $target_bank {
                    Bank::I32 => {
                        let value = $builder.ins().ireduce(types::I32, payload);
                        $builder.def_var(reg_vars_i32[index], value);
                    }
                    Bank::I64 => {
                        $builder.def_var(reg_vars[index], payload);
                    }
                    Bank::F32 => {
                        let bits = $builder.ins().ireduce(types::I32, payload);
                        let value = $builder.ins().bitcast(types::F32, MemFlags::new(), bits);
                        $builder.def_var(reg_vars_f32[index], value);
                    }
                    Bank::F64 => {
                        let value = $builder.ins().bitcast(types::F64, MemFlags::new(), payload);
                        $builder.def_var(reg_vars_f64[index], value);
                    }
                }
            }};
        }

        macro_rules! materialize_stack_bank {
            ($builder:expr, $index:expr, $source_bank:expr, $target_bank:expr) => {{
                let index = $index;
                let payload = stack_payload!($builder, index, $source_bank);
                match $target_bank {
                    Bank::I32 => {
                        let value = $builder.ins().ireduce(types::I32, payload);
                        $builder.def_var(stack_vars_i32[index], value);
                    }
                    Bank::I64 => {
                        $builder.def_var(stack_vars[index], payload);
                    }
                    Bank::F32 => {
                        let bits = $builder.ins().ireduce(types::I32, payload);
                        let value = $builder.ins().bitcast(types::F32, MemFlags::new(), bits);
                        $builder.def_var(stack_vars_f32[index], value);
                    }
                    Bank::F64 => {
                        let value = $builder.ins().bitcast(types::F64, MemFlags::new(), payload);
                        $builder.def_var(stack_vars_f64[index], value);
                    }
                }
            }};
        }

        macro_rules! normalize_stack_banks_for_edge {
            ($builder:expr, $source_stack_ty:expr, $target_stack_ty:expr) => {{
                let source_stack_ty = $source_stack_ty;
                let target_stack_ty = $target_stack_ty;
                debug_assert_eq!(source_stack_ty.len(), target_stack_ty.len());
                for index in 0..source_stack_ty.len() {
                    if source_stack_ty[index] != target_stack_ty[index] {
                        materialize_stack_bank!($builder, index, source_stack_ty[index], target_stack_ty[index]);
                    }
                }
            }};
        }

        macro_rules! copy_stack_value {
            ($builder:expr, $source:expr, $destination:expr) => {{
                let source = $source;
                let destination = $destination;
                let bank = stack_ty[source];
                match bank {
                    Bank::I32 => {
                        let value = $builder.use_var(stack_vars_i32[source]);
                        $builder.def_var(stack_vars_i32[destination], value);
                    }
                    Bank::I64 => {
                        let value = $builder.use_var(stack_vars[source]);
                        $builder.def_var(stack_vars[destination], value);
                    }
                    Bank::F32 => {
                        let value = $builder.use_var(stack_vars_f32[source]);
                        $builder.def_var(stack_vars_f32[destination], value);
                    }
                    Bank::F64 => {
                        let value = $builder.use_var(stack_vars_f64[source]);
                        $builder.def_var(stack_vars_f64[destination], value);
                    }
                }
                stack_ty[destination] = bank;
            }};
        }

        macro_rules! normalize_register_banks_for_edge {
            ($builder:expr, $target_reg_ty:expr, $live_registers:expr) => {{
                let target_reg_ty = $target_reg_ty;
                let live_registers = $live_registers;
                for index in 0..REG_COUNT {
                    if live_registers & (1 << index) == 0 {
                        continue;
                    }
                    if reg_ty[index] != target_reg_ty[index] {
                        materialize_register_bank!($builder, index, reg_ty[index], target_reg_ty[index]);
                    }
                }
            }};
        }

        macro_rules! prepare_register_banks_for_branch {
            ($builder:expr, $target_index:expr) => {{
                if !is_unreachable {
                    let target_index = $target_index;
                    let live_registers = control_stack[target_index].branch_target_live_registers;
                    let target_reg_ty = match control_stack[target_index].branch_target_reg_ty {
                        Some(target_reg_ty) => target_reg_ty,
                        None => {
                            let target_reg_ty = reg_ty;
                            control_stack[target_index].branch_target_reg_ty = Some(target_reg_ty);
                            target_reg_ty
                        }
                    };
                    normalize_register_banks_for_edge!($builder, target_reg_ty, live_registers);
                }
            }};
        }

        macro_rules! prepare_stack_banks_for_branch {
            ($builder:expr, $target_index:expr, $depth:expr) => {{
                if !is_unreachable {
                    let target_index = $target_index;
                    let depth = $depth;
                    let source_stack_ty = stack_ty[..depth].to_vec();
                    let target_stack_ty = match control_stack[target_index].branch_target_stack_ty.clone() {
                        Some(target_stack_ty) => target_stack_ty,
                        None => {
                            control_stack[target_index].branch_target_stack_ty = Some(source_stack_ty.clone());
                            source_stack_ty.clone()
                        }
                    };
                    normalize_stack_banks_for_edge!($builder, &source_stack_ty, &target_stack_ty);
                }
            }};
        }

        macro_rules! prepare_register_banks_for_epilogue {
            ($builder:expr) => {{
                let target_reg_ty = match epilogue_reg_ty {
                    Some(target_reg_ty) => target_reg_ty,
                    None => {
                        let target_reg_ty = reg_ty;
                        epilogue_reg_ty = Some(target_reg_ty);
                        target_reg_ty
                    }
                };
                normalize_register_banks_for_edge!($builder, target_reg_ty, RegisterSet::MAX);
            }};
        }

        // Note that all reads from sources have to be in order (sources[0] before sources[1])
        macro_rules! i32_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs = read_src_i32!($builder, $insn.sources[0]);
                let lhs = read_src_i32!($builder, $insn.sources[1]);
                let result = $builder.ins().$op(lhs, rhs);
                write_dst_i32!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i64_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs = read_src!($builder, $insn.sources[0]);
                let lhs = read_src!($builder, $insn.sources[1]);
                let result = $builder.ins().$op(lhs, rhs);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i32_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src = read_src_i32!($builder, $insn.sources[0]);
                let result = $builder.ins().$op(src);
                write_dst_i32!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i64_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src = read_src!($builder, $insn.sources[0]);
                let result = $builder.ins().$op(src);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i32_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs = read_src_i32!($builder, $insn.sources[0]);
                let lhs = read_src_i32!($builder, $insn.sources[1]);
                let cmp = $builder.ins().icmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I32, cmp);
                write_dst_i32!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i64_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs = read_src!($builder, $insn.sources[0]);
                let lhs = read_src!($builder, $insn.sources[1]);
                let cmp = $builder.ins().icmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I32, cmp);
                write_dst_i32!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f32_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs = read_src_f32!($builder, $insn.sources[0]);
                let lhs = read_src_f32!($builder, $insn.sources[1]);
                let result = $builder.ins().$op(lhs, rhs);
                write_dst_f32!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f64_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs = read_src_f64!($builder, $insn.sources[0]);
                let lhs = read_src_f64!($builder, $insn.sources[1]);
                let result = $builder.ins().$op(lhs, rhs);
                write_dst_f64!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f32_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src = read_src_f32!($builder, $insn.sources[0]);
                let result = $builder.ins().$op(src);
                write_dst_f32!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f64_unop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let src = read_src_f64!($builder, $insn.sources[0]);
                let result = $builder.ins().$op(src);
                write_dst_f64!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f32_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs = read_src_f32!($builder, $insn.sources[0]);
                let lhs = read_src_f32!($builder, $insn.sources[1]);
                let cmp = $builder.ins().fcmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I32, cmp);
                write_dst_i32!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f64_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs = read_src_f64!($builder, $insn.sources[0]);
                let lhs = read_src_f64!($builder, $insn.sources[1]);
                let cmp = $builder.ins().fcmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I32, cmp);
                write_dst_i32!($builder, $insn.destination, result);
            }};
        }

        macro_rules! read_local_inline {
            ($builder:expr, $idx_imm:expr) => {{
                let idx = ($idx_imm) as usize;
                let var = local_vars[idx].expect("accessed local must have an SSA variable");
                let v = $builder.use_var(var);
                if local_is_f64[idx] {
                    $builder.ins().bitcast(types::I64, MemFlags::new(), v)
                } else if local_is_f32[idx] {
                    let bits32 = $builder.ins().bitcast(types::I32, MemFlags::new(), v);
                    $builder.ins().sextend(types::I64, bits32)
                } else if local_is_i32[idx] {
                    $builder.ins().uextend(types::I64, v)
                } else {
                    v
                }
            }};
        }
        macro_rules! read_local_i32 {
            ($builder:expr, $idx_imm:expr) => {{
                let idx = ($idx_imm) as usize;
                let var = local_vars[idx].expect("accessed local must have an SSA variable");
                if local_is_i32[idx] {
                    $builder.use_var(var)
                } else {
                    let value = $builder.use_var(var);
                    $builder.ins().ireduce(types::I32, value)
                }
            }};
        }
        macro_rules! read_local_f64 {
            ($builder:expr, $idx_imm:expr) => {{
                let idx = ($idx_imm) as usize;
                let var = local_vars[idx].expect("accessed local must have an SSA variable");
                if local_is_f64[idx] {
                    $builder.use_var(var)
                } else {
                    let v = $builder.use_var(var);
                    $builder.ins().bitcast(types::F64, MemFlags::new(), v)
                }
            }};
        }
        macro_rules! read_local_f32 {
            ($builder:expr, $idx_imm:expr) => {{
                let idx = ($idx_imm) as usize;
                let var = local_vars[idx].expect("accessed local must have an SSA variable");
                if local_is_f32[idx] {
                    $builder.use_var(var)
                } else {
                    let v = $builder.use_var(var);
                    let v32 = $builder.ins().ireduce(types::I32, v);
                    $builder.ins().bitcast(types::F32, MemFlags::new(), v32)
                }
            }};
        }
        macro_rules! write_local_inline {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let idx = ($idx_imm) as usize;
                let v = $val;
                let var = local_vars[idx].expect("accessed local must have an SSA variable");
                let stored = if local_is_f64[idx] {
                    $builder.ins().bitcast(types::F64, MemFlags::new(), v)
                } else if local_is_f32[idx] {
                    let v32 = $builder.ins().ireduce(types::I32, v);
                    $builder.ins().bitcast(types::F32, MemFlags::new(), v32)
                } else if local_is_i32[idx] {
                    $builder.ins().ireduce(types::I32, v)
                } else {
                    v
                };
                $builder.def_var(var, stored);
            }};
        }
        macro_rules! write_local_i32 {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let idx = ($idx_imm) as usize;
                let value = $val;
                let var = local_vars[idx].expect("accessed local must have an SSA variable");
                if local_is_i32[idx] {
                    $builder.def_var(var, value);
                } else {
                    let payload = $builder.ins().uextend(types::I64, value);
                    $builder.def_var(var, payload);
                }
            }};
        }
        macro_rules! write_local_f64 {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let idx = ($idx_imm) as usize;
                let v = $val;
                let var = local_vars[idx].expect("accessed local must have an SSA variable");
                if local_is_f64[idx] {
                    $builder.def_var(var, v);
                } else {
                    let bits = $builder.ins().bitcast(types::I64, MemFlags::new(), v);
                    $builder.def_var(var, bits);
                }
            }};
        }
        macro_rules! write_local_f32 {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let idx = ($idx_imm) as usize;
                let v = $val;
                if local_is_f32[idx] {
                    $builder.def_var(
                        local_vars[idx].expect("accessed local must have an SSA variable"),
                        v,
                    );
                } else {
                    let bits32 = $builder.ins().bitcast(types::I32, MemFlags::new(), v);
                    let bits = $builder.ins().sextend(types::I64, bits32);
                    write_local_inline!($builder, $idx_imm, bits);
                }
            }};
        }
        macro_rules! local_get {
            ($builder:expr, $idx_imm:expr, $dst:expr) => {{
                let idx = ($idx_imm) as usize;
                if local_is_f64[idx] {
                    let result = read_local_f64!($builder, $idx_imm);
                    write_dst_f64!($builder, $dst, result);
                } else if local_is_f32[idx] {
                    let result = read_local_f32!($builder, $idx_imm);
                    write_dst_f32!($builder, $dst, result);
                } else if local_is_i32[idx] {
                    let result = read_local_i32!($builder, $idx_imm);
                    write_dst_i32!($builder, $dst, result);
                } else {
                    let result = read_local_inline!($builder, $idx_imm);
                    write_dst!($builder, $dst, result);
                }
            }};
        }
        macro_rules! local_set {
            ($builder:expr, $idx_imm:expr, $src:expr) => {{
                let idx = ($idx_imm) as usize;
                if local_is_f64[idx] {
                    let val = read_src_f64!($builder, $src);
                    write_local_f64!($builder, $idx_imm, val);
                } else if local_is_f32[idx] {
                    let val = read_src_f32!($builder, $src);
                    write_local_f32!($builder, $idx_imm, val);
                } else if local_is_i32[idx] {
                    let val = read_src_i32!($builder, $src);
                    write_local_i32!($builder, $idx_imm, val);
                } else {
                    let val = read_src!($builder, $src);
                    write_local_inline!($builder, $idx_imm, val);
                }
            }};
        }
        // Call + trap-check macro for helper calls that do not consume caller register state.
        // The callee gets arguments explicitly (register immediates, stack, or call record), and the caller's virtual registers stay live in SSA across the call.
        macro_rules! do_call_and_check {
            ($builder:expr, $sig:expr, $ptr:expr, $args:expr) => {{
                let call = $builder.ins().call_indirect($sig, $ptr, $args);
                let trapped = $builder.inst_results(call)[0];
                let is_trap = $builder.ins().icmp_imm_s(IntCC::NotEqual, trapped, 0);
                let cont = $builder.create_block();
                $builder.ins().brif(is_trap, trap_block, &[], cont, &[]);
                $builder.switch_to_block(cont);
                $builder.seal_block(cont);
            }};
        }
        // No bounds checks: the memory reserves the full base (u32) + offset (u32) span, so any
        // out-of-bounds access faults on an uncommitted page and unwinds as a wasm trap.
        macro_rules! inline_memory_address {
            ($builder:expr, $memory_index:expr, $addr:expr) => {{
                let memory_base = memory_bases
                    .iter()
                    .find_map(|(index, base)| (*index == $memory_index).then_some(*base))
                    .expect("memory access must have a resolved base");
                let addr_offset = if ptr_type == types::I64 {
                    $addr
                } else {
                    $builder.ins().ireduce(ptr_type, $addr)
                };
                $builder.ins().iadd(memory_base, addr_offset)
            }};
        }
        macro_rules! inline_global_instance {
            ($global_index:expr) => {{
                global_instances
                    .iter()
                    .find_map(|(index, global)| (*index == $global_index).then_some(*global))
                    .expect("global access must have a resolved instance")
            }};
        }

        // The adapter has already loaded parameters into the native ABI before calling the body.
        // A direct native caller supplies the same values without going through locals memory.
        macro_rules! init_locals_fresh {
            ($builder:expr) => {{
                for (i, var) in local_vars.iter().enumerate() {
                    if !local_is_accessed[i] {
                        continue;
                    }
                    let var = var.expect("accessed local must have an SSA variable");
                    if i < num_params {
                        let val = if uses_register_native_abi {
                            let parameter = $builder.block_params(entry_block)[3 + i];
                            if local_is_f64[i] || local_is_f32[i] || local_is_i32[i] {
                                parameter
                            } else {
                                value_to_payload(&mut $builder, parameter, function_type.parameters[i])?
                            }
                        } else {
                            let incoming_locals = $builder.block_params(entry_block)[3];
                            let ty = if local_is_f64[i] {
                                types::F64
                            } else if local_is_f32[i] {
                                types::F32
                            } else if local_is_i32[i] {
                                types::I32
                            } else {
                                types::I64
                            };
                            let offset = (i as i32) * value_size;
                            $builder
                                .ins()
                                .load(ty, memory_flags.activation, incoming_locals, offset)
                        };
                        $builder.def_var(var, val);
                    } else if local_is_f64[i] {
                        let zero = $builder.ins().f64const(0.0);
                        $builder.def_var(var, zero);
                    } else if local_is_f32[i] {
                        let zero = $builder.ins().f32const(0.0);
                        $builder.def_var(var, zero);
                    } else if local_is_i32[i] {
                        let zero = $builder.ins().iconst(types::I32, 0);
                        $builder.def_var(var, zero);
                    } else {
                        let zero = $builder.ins().iconst(types::I64, 0);
                        $builder.def_var(var, zero);
                    }
                }
            }};
        }
        macro_rules! init_locals_resume {
            ($builder:expr) => {{
                let cfg = $builder.use_var(config_var);
                let canonical_locals =
                    $builder
                        .ins()
                        .load(ptr_type, memory_flags.configuration, cfg, locals_base_offset);
                for (i, var) in local_vars.iter().enumerate() {
                    if !local_is_accessed[i] {
                        continue;
                    }
                    let canonical_offset = (i as i32) * value_size;
                    let var = var.expect("accessed local must have an SSA variable");
                    let ty = if local_is_f64[i] {
                        types::F64
                    } else if local_is_f32[i] {
                        types::F32
                    } else if local_is_i32[i] {
                        types::I32
                    } else {
                        types::I64
                    };
                    let value = $builder
                        .ins()
                        .load(ty, memory_flags.activation, canonical_locals, canonical_offset);
                    $builder.def_var(var, value);
                }
            }};
        }

        // If we have any tier-up checkpoints, the interpreter will eventually need to jump to some point in the function other than the entry block, so prepare dispatch blocks for that.
        // Note that the initial block will already have the correct register state loaded, so we don't need to sync registers for the tier-up dispatch targets.
        let has_tier_up = insns.iter().any(|i| i.opcode == op::SYNTHETIC_TIER_UP);
        let tier_up_target_ip = builder.ins().iadd_imm_s(entry_token, -1);
        let mut tier_up_dispatch_tail: Option<Block> = None;
        let tier_up_body_start: Option<Block> = if has_tier_up {
            let body_start = builder.create_block();
            let dispatch = builder.create_block();
            let fresh = builder.create_block();
            let resume = builder.create_block();
            builder.set_cold_block(dispatch);
            builder.set_cold_block(resume);
            let has_tier_up_target = builder.ins().icmp_imm_s(IntCC::NotEqual, tier_up_target_ip, 0);
            let direct_call_mode = builder.use_var(direct_call_mode_var);
            let is_normal_entry = builder.ins().icmp_imm_s(IntCC::Equal, direct_call_mode, 0);
            let is_tier_up = builder.ins().band(has_tier_up_target, is_normal_entry);
            builder.ins().brif(is_tier_up, resume, &[], fresh, &[]);

            builder.switch_to_block(resume);
            builder.seal_block(resume);
            init_locals_resume!(builder);
            builder.ins().jump(dispatch, &[]);

            builder.switch_to_block(fresh);
            builder.seal_block(fresh);
            init_locals_fresh!(builder);
            builder.ins().jump(body_start, &[]);

            builder.switch_to_block(body_start);
            tier_up_dispatch_tail = Some(dispatch);
            Some(body_start)
        } else {
            init_locals_fresh!(builder);
            None
        };

        let mut ip = 0usize;
        while ip < insns.len() {
            let insn = &insns[ip];
            let opc = insn.opcode;

            match opc {
                op::NOP => {}

                op::UNREACHABLE => {
                    builder.ins().trap(user_trap_code(CraneliftUserTrapCode::Unreachable));
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::BLOCK => {
                    let arity = insn.imm3 & 0xffff;
                    let param_count = (insn.imm3 >> 16) as usize;
                    let after = builder.create_block();
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = builder.declare_var(types::I64);
                        let cur = emit_stack_size!(builder);
                        let entry = builder.ins().iadd_imm_s(cur, -(param_count as i64));
                        builder.def_var(var, entry);
                        Some(var)
                    } else {
                        None
                    };
                    control_stack.push(ControlFrame {
                        kind: ControlKind::Block,
                        branch_target: after,
                        after_block: after,
                        arity,
                        param_count,
                        stack_depth_at_entry: (sp - param_count) as i32,
                        is_unreachable_at_entry: is_unreachable,
                        entry_real_depth_var,
                        bank_snapshot: None,
                        branch_target_reg_ty: None,
                        branch_target_stack_ty: None,
                        branch_target_live_registers: register_liveness.branch_target_live(ip),
                    });
                }

                op::LOOP => {
                    let arity = insn.imm3 & 0xffff;
                    let param_count = (insn.imm3 >> 16) as usize;
                    let header = builder.create_block();
                    let after = builder.create_block();
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = builder.declare_var(types::I64);
                        let cur = emit_stack_size!(builder);
                        let entry = builder.ins().iadd_imm_s(cur, -(param_count as i64));
                        builder.def_var(var, entry);
                        Some(var)
                    } else {
                        None
                    };
                    builder.ins().jump(header, &[]);
                    builder.switch_to_block(header);
                    let header_reg_ty = reg_ty;
                    control_stack.push(ControlFrame {
                        kind: ControlKind::Loop,
                        branch_target: header,
                        after_block: after,
                        arity,
                        param_count,
                        stack_depth_at_entry: (sp - param_count) as i32,
                        is_unreachable_at_entry: is_unreachable,
                        entry_real_depth_var,
                        bank_snapshot: None,
                        branch_target_reg_ty: Some(header_reg_ty),
                        branch_target_stack_ty: Some(stack_ty[..sp].to_vec()),
                        branch_target_live_registers: register_liveness.branch_target_live(ip),
                    });
                }

                op::IF => {
                    let arity = insn.imm3 & 0xffff;
                    let _param_count = (insn.imm3 >> 16) as usize;
                    let has_else = insn.imm2 >= 0;
                    let then_block = builder.create_block();
                    let else_block = builder.create_block();
                    let after = builder.create_block();

                    let cond = read_src_i32!(builder, insn.sources[0]);
                    let cond = builder.ins().icmp_imm_s(IntCC::NotEqual, cond, 0);
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = builder.declare_var(types::I64);
                        let cur = emit_stack_size!(builder);
                        let entry = builder.ins().iadd_imm_s(cur, -(_param_count as i64));
                        builder.def_var(var, entry);
                        Some(var)
                    } else {
                        None
                    };
                    if has_else {
                        builder.ins().brif(cond, then_block, &[], else_block, &[]);
                    } else {
                        builder.ins().brif(cond, then_block, &[], after, &[]);
                    }

                    builder.switch_to_block(then_block);
                    builder.seal_block(then_block);

                    control_stack.push(ControlFrame {
                        kind: ControlKind::If,
                        branch_target: after,
                        after_block: if has_else { else_block } else { after },
                        arity,
                        param_count: _param_count,
                        stack_depth_at_entry: (sp - _param_count) as i32,
                        is_unreachable_at_entry: is_unreachable,
                        entry_real_depth_var,
                        bank_snapshot: Some((reg_ty, stack_ty.clone())),
                        branch_target_reg_ty: if has_else || is_unreachable { None } else { Some(reg_ty) },
                        branch_target_stack_ty: if has_else || is_unreachable {
                            None
                        } else {
                            Some(stack_ty[..sp].to_vec())
                        },
                        branch_target_live_registers: register_liveness.branch_target_live(ip),
                    });
                }

                op::ELSE => {
                    if !control_stack.is_empty() {
                        let frame_index = control_stack.len() - 1;
                        let (else_block, after, entry_depth, pc, snapshot, is_unreachable_at_entry) = {
                            let frame = &control_stack[frame_index];
                            (
                                frame.after_block,
                                frame.branch_target,
                                frame.stack_depth_at_entry,
                                frame.param_count,
                                frame.bank_snapshot.clone(),
                                frame.is_unreachable_at_entry,
                            )
                        };
                        if !is_unreachable {
                            prepare_register_banks_for_branch!(builder, frame_index);
                            prepare_stack_banks_for_branch!(builder, frame_index, sp);
                            builder.ins().jump(after, &[]);
                        }
                        builder.switch_to_block(else_block);
                        builder.seal_block(else_block);
                        // Reset sp to entry depth + param_count (else branch inherits params).
                        sp = (entry_depth as usize) + pc;
                        if let Some((saved_reg_ty, saved_stack_ty)) = snapshot {
                            reg_ty = saved_reg_ty;
                            stack_ty = saved_stack_ty;
                        }
                        is_unreachable = is_unreachable_at_entry;
                        control_stack[frame_index].after_block = after;
                    }
                }

                op::END | op::SYNTHETIC_END_EXPRESSION => {
                    if let Some(frame) = control_stack.pop() {
                        let after = if frame.kind == ControlKind::If && frame.after_block != frame.branch_target {
                            // If without else: the after_block is the branch_target.
                            frame.branch_target
                        } else {
                            frame.after_block
                        };

                        let fallthrough_is_reachable = !is_unreachable;
                        let branch_target_is_reachable =
                            frame.kind != ControlKind::Loop && frame.branch_target_reg_ty.is_some();
                        let after_is_reachable = fallthrough_is_reachable || branch_target_is_reachable;
                        let target_depth = (frame.stack_depth_at_entry + frame.arity as i32) as usize;
                        let source_stack_ty = stack_ty[..target_depth].to_vec();
                        let after_reg_ty = if branch_target_is_reachable {
                            frame.branch_target_reg_ty.unwrap()
                        } else {
                            reg_ty
                        };
                        let after_stack_ty = if branch_target_is_reachable {
                            frame.branch_target_stack_ty.unwrap()
                        } else {
                            source_stack_ty.clone()
                        };
                        if fallthrough_is_reachable {
                            normalize_register_banks_for_edge!(
                                builder,
                                after_reg_ty,
                                frame.branch_target_live_registers
                            );
                            normalize_stack_banks_for_edge!(builder, &source_stack_ty, &after_stack_ty);
                            builder.ins().jump(after, &[]);
                        }
                        builder.switch_to_block(after);
                        is_unreachable = !after_is_reachable;
                        reg_ty = after_reg_ty;

                        // After end of block, sp = entry depth + arity.
                        sp = target_depth;
                        stack_ty[..sp].copy_from_slice(&after_stack_ty);

                        if frame.kind == ControlKind::Loop {
                            builder.seal_block(frame.branch_target); // loop header
                        }
                        builder.seal_block(after);
                    } else if !is_unreachable {
                        push_top_n_to_real!(builder, result_arity);
                        prepare_register_banks_for_epilogue!(builder);
                        builder.ins().jump(epilogue_block, &[]);
                        let dead = builder.create_block();
                        builder.switch_to_block(dead);
                        builder.seal_block(dead);
                    }
                }

                op::BR | op::SYNTHETIC_BR_NOSTACK => {
                    let label_idx = insn.imm1 as usize;
                    if label_idx < control_stack.len() {
                        let target_idx = control_stack.len() - 1 - label_idx;
                        let (target, arity, entry, entry_real_depth_var) = {
                            let frame = &control_stack[target_idx];
                            (
                                frame.branch_target,
                                if frame.kind == ControlKind::Loop {
                                    0
                                } else {
                                    frame.arity
                                },
                                frame.stack_depth_at_entry as usize,
                                frame.entry_real_depth_var,
                            )
                        };
                        if max_stack_depth > 0 {
                            // vstack enabled: move top arity values to entry position.
                            if arity > 0 {
                                if sp > 0 {
                                    copy_stack_value!(builder, sp - 1, entry);
                                } else {
                                    let result = emit_stack_pop!(builder);
                                    builder.def_var(stack_vars[entry], result);
                                    stack_ty[entry] = Bank::I64;
                                }
                            }
                        } else {
                            // vstack disabled: trim the real value stack down to the target label's entry depth + arity, preserving the top arity values.
                            let entry_depth_var =
                                entry_real_depth_var.expect("entry_real_depth_var must be set when vstack is disabled");
                            let target_size = builder.use_var(entry_depth_var);
                            emit_stack_cleanup!(builder, target_size, arity);
                        }
                        prepare_register_banks_for_branch!(builder, target_idx);
                        prepare_stack_banks_for_branch!(builder, target_idx, entry + arity as usize);
                        builder.ins().jump(target, &[]);
                    } else {
                        // br to function label = return.
                        push_top_n_to_real!(builder, result_arity);
                        prepare_register_banks_for_epilogue!(builder);
                        builder.ins().jump(epilogue_block, &[]);
                    }
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::BR_IF | op::SYNTHETIC_BR_IF_NOSTACK => {
                    let label_idx = insn.imm1 as usize;
                    let cond = read_src_i32!(builder, insn.sources[0]);
                    let cond = builder.ins().icmp_imm_s(IntCC::NotEqual, cond, 0);

                    if label_idx < control_stack.len() {
                        let target_idx = control_stack.len() - 1 - label_idx;
                        let (target, arity, entry, entry_real_depth_var) = {
                            let frame = &control_stack[target_idx];
                            (
                                frame.branch_target,
                                if frame.kind == ControlKind::Loop {
                                    0
                                } else {
                                    frame.arity
                                },
                                frame.stack_depth_at_entry as usize,
                                frame.entry_real_depth_var,
                            )
                        };
                        let extras = (sp as i32 - entry as i32 - arity as i32).max(0);
                        if max_stack_depth == 0 {
                            // not vstack: real value stack may have extras between the target label's entry depth and the result on top.
                            // On the taken path, trim using the saved entry-depth variable.
                            let entry_depth_var =
                                entry_real_depth_var.expect("entry_real_depth_var must be set when vstack is disabled");
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            let target_size = builder.use_var(entry_depth_var);
                            emit_stack_cleanup!(builder, target_size, arity);
                            prepare_register_banks_for_branch!(builder, target_idx);
                            builder.ins().jump(target, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else if extras > 0 {
                            let fallthrough_stack_ty = stack_ty.clone();
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            if arity > 0 {
                                copy_stack_value!(builder, sp - 1, entry);
                            }
                            // Note: we don't change sp here since fallthrough needs the original sp.
                            prepare_register_banks_for_branch!(builder, target_idx);
                            prepare_stack_banks_for_branch!(builder, target_idx, entry + arity as usize);
                            builder.ins().jump(target, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                            stack_ty = fallthrough_stack_ty;
                        } else {
                            let fallthrough = builder.create_block();
                            prepare_register_banks_for_branch!(builder, target_idx);
                            prepare_stack_banks_for_branch!(builder, target_idx, entry + arity as usize);
                            builder.ins().brif(cond, target, &[], fallthrough, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        }
                    } else {
                        if sp > 0 {
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            push_top_n_to_real!(builder, result_arity);
                            prepare_register_banks_for_epilogue!(builder);
                            builder.ins().jump(epilogue_block, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else {
                            let fallthrough = builder.create_block();
                            prepare_register_banks_for_epilogue!(builder);
                            builder.ins().brif(cond, epilogue_block, &[], fallthrough, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        }
                    }
                }

                op::RETURN => {
                    push_top_n_to_real!(builder, result_arity);
                    prepare_register_banks_for_epilogue!(builder);
                    builder.ins().jump(epilogue_block, &[]);
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::I32_CONST => {
                    let val = builder.ins().iconst(types::I32, insn.imm1);
                    write_dst_i32!(builder, insn.destination, val);
                }
                op::I64_CONST => {
                    let val = builder.ins().iconst(types::I64, insn.imm1);
                    write_dst!(builder, insn.destination, val);
                }
                op::F32_CONST => {
                    let val = builder.ins().f32const(Ieee32::with_bits(insn.imm1 as u32));
                    write_dst_f32!(builder, insn.destination, val);
                }
                op::F64_CONST => {
                    let val = builder.ins().f64const(Ieee64::with_bits(insn.imm1 as u64));
                    write_dst_f64!(builder, insn.destination, val);
                }

                op::LOCAL_GET | op::SYNTHETIC_ARGUMENT_GET => {
                    local_get!(builder, insn.imm1, insn.destination);
                }
                op::LOCAL_SET | op::SYNTHETIC_ARGUMENT_SET => {
                    local_set!(builder, insn.imm1, insn.sources[0]);
                }
                op::LOCAL_TEE | op::SYNTHETIC_ARGUMENT_TEE => {
                    let idx = insn.imm1 as usize;
                    if local_is_f64[idx] {
                        let val = read_src_f64!(builder, insn.sources[0]);
                        write_local_f64!(builder, insn.imm1, val);
                        write_dst_f64!(builder, insn.destination, val);
                    } else if local_is_f32[idx] {
                        let val = read_src_f32!(builder, insn.sources[0]);
                        write_local_f32!(builder, insn.imm1, val);
                        write_dst_f32!(builder, insn.destination, val);
                    } else if local_is_i32[idx] {
                        let val = read_src_i32!(builder, insn.sources[0]);
                        write_local_i32!(builder, insn.imm1, val);
                        write_dst_i32!(builder, insn.destination, val);
                    } else {
                        let val = read_src!(builder, insn.sources[0]);
                        write_local_inline!(builder, insn.imm1, val);
                        write_dst!(builder, insn.destination, val);
                    }
                }

                opc if (op::SYNTHETIC_LOCAL_GET_0..=op::SYNTHETIC_LOCAL_GET_7).contains(&opc) => {
                    let local_idx = (opc - op::SYNTHETIC_LOCAL_GET_0) as i64;
                    local_get!(builder, local_idx, insn.destination);
                }
                opc if (op::SYNTHETIC_LOCAL_SET_0..=op::SYNTHETIC_LOCAL_SET_7).contains(&opc) => {
                    let local_idx = (opc - op::SYNTHETIC_LOCAL_SET_0) as i64;
                    local_set!(builder, local_idx, insn.sources[0]);
                }
                op::SYNTHETIC_LOCAL_COPY => {
                    let destination = insn.imm2 as usize;
                    if local_is_f64[destination] {
                        let val = read_local_f64!(builder, insn.imm1);
                        write_local_f64!(builder, insn.imm2, val);
                    } else if local_is_f32[destination] {
                        let val = read_local_f32!(builder, insn.imm1);
                        write_local_f32!(builder, insn.imm2, val);
                    } else if local_is_i32[destination] {
                        let val = read_local_i32!(builder, insn.imm1);
                        write_local_i32!(builder, insn.imm2, val);
                    } else {
                        let val = read_local_inline!(builder, insn.imm1);
                        write_local_inline!(builder, insn.imm2, val);
                    }
                }

                op::GLOBAL_GET => {
                    let global = inline_global_instance!(insn.imm1 as u32);
                    let result =
                        builder
                            .ins()
                            .load(types::I64, memory_flags.globals, global, global_instance_value_offset);
                    write_dst!(builder, insn.destination, result);
                }
                op::GLOBAL_SET => {
                    let val = read_src!(builder, insn.sources[0]);
                    let global = inline_global_instance!(insn.imm1 as u32);
                    builder
                        .ins()
                        .store(memory_flags.globals, val, global, global_instance_value_offset);
                    let zero = builder.ins().iconst(types::I64, 0);
                    builder
                        .ins()
                        .store(memory_flags.globals, zero, global, global_instance_value_offset + 8);
                }

                op::DROP => {
                    if insn.sources[0] == STACK_MARKER {
                        read_src!(builder, insn.sources[0]);
                    }
                    // No need to do anything if it's not on the real stack.
                }

                op::SELECT | op::SELECT_TYPED => {
                    let cond_raw = read_src_i32!(builder, insn.sources[0]);
                    let cond = builder.ins().icmp_imm_s(IntCC::NotEqual, cond_raw, 0);
                    let rhs_is_i32 = src_is_i32!(insn.sources[1]);
                    let rhs = if rhs_is_i32 {
                        read_src_i32!(builder, insn.sources[1])
                    } else {
                        read_src!(builder, insn.sources[1])
                    };
                    let lhs_is_i32 = src_is_i32!(insn.sources[2]);
                    let lhs = if lhs_is_i32 {
                        read_src_i32!(builder, insn.sources[2])
                    } else {
                        read_src!(builder, insn.sources[2])
                    };
                    if lhs_is_i32 && rhs_is_i32 {
                        let result = builder.ins().select(cond, lhs, rhs);
                        write_dst_i32!(builder, insn.destination, result);
                    } else {
                        let lhs = if lhs_is_i32 {
                            builder.ins().uextend(types::I64, lhs)
                        } else {
                            lhs
                        };
                        let rhs = if rhs_is_i32 {
                            builder.ins().uextend(types::I64, rhs)
                        } else {
                            rhs
                        };
                        let result = builder.ins().select(cond, lhs, rhs);
                        write_dst!(builder, insn.destination, result);
                    }
                }

                op::BR_TABLE => {
                    let inline_count = (insn.imm3 & 0xff) as usize;
                    if inline_count == 0xff {
                        return Err("br_table too large for inline encoding");
                    }

                    let default_label = ((insn.imm3 >> 8) & 0xffff) as usize;

                    // Collect all labels; first 8 from this instruction, rest from continuations.
                    let mut all_labels: Vec<usize> = Vec::with_capacity(inline_count);
                    for i in 0..inline_count {
                        let packed = if i < 4 { insn.imm1 as u64 } else { insn.imm2 as u64 };
                        all_labels.push(((packed >> ((i % 4) * 16)) & 0xffff) as usize);
                    }
                    while ip + 1 < insns.len() && insns[ip + 1].opcode == op::SYNTHETIC_BR_TABLE_CONT {
                        ip += 1;
                        let cont = &insns[ip];
                        let chunk = (cont.imm3 & 0xff) as usize;
                        for j in 0..chunk {
                            let packed = if j < 4 { cont.imm1 as u64 } else { cont.imm2 as u64 };
                            all_labels.push(((packed >> ((j % 4) * 16)) & 0xffff) as usize);
                        }
                    }

                    let cond = read_src_i32!(builder, insn.sources[0]);

                    let mut branch_to_label = |builder: &mut FunctionBuilder, label_idx: usize| {
                        let fallthrough_stack_ty = stack_ty.clone();
                        if label_idx < control_stack.len() {
                            let target_idx = control_stack.len() - 1 - label_idx;
                            let (target, arity, entry, entry_real_depth_var) = {
                                let frame = &control_stack[target_idx];
                                (
                                    frame.branch_target,
                                    if frame.kind == ControlKind::Loop {
                                        0
                                    } else {
                                        frame.arity
                                    },
                                    frame.stack_depth_at_entry as usize,
                                    frame.entry_real_depth_var,
                                )
                            };
                            if max_stack_depth > 0 && arity > 0 {
                                if sp > 0 {
                                    copy_stack_value!(builder, sp - 1, entry);
                                } else {
                                    let result = emit_stack_pop!(builder);
                                    builder.def_var(stack_vars[entry], result);
                                    stack_ty[entry] = Bank::I64;
                                }
                            } else if max_stack_depth == 0 {
                                let entry_depth_var = entry_real_depth_var
                                    .expect("entry_real_depth_var must be set when vstack is disabled");
                                let target_size = builder.use_var(entry_depth_var);
                                emit_stack_cleanup!(builder, target_size, arity);
                            }
                            prepare_register_banks_for_branch!(builder, target_idx);
                            prepare_stack_banks_for_branch!(builder, target_idx, entry + arity as usize);
                            builder.ins().jump(target, &[]);
                        } else {
                            push_top_n_to_real!(builder, result_arity);
                            prepare_register_banks_for_epilogue!(builder);
                            builder.ins().jump(epilogue_block, &[]);
                        }
                        stack_ty = fallthrough_stack_ty;
                    };

                    for (i, &label) in all_labels.iter().enumerate() {
                        let case_block = builder.create_block();
                        let next_fallthrough = builder.create_block();
                        let compare = builder.ins().icmp_imm_s(IntCC::Equal, cond, i as i64);
                        builder.ins().brif(compare, case_block, &[], next_fallthrough, &[]);

                        builder.switch_to_block(case_block);
                        builder.seal_block(case_block);
                        branch_to_label(&mut builder, label);

                        builder.switch_to_block(next_fallthrough);
                        builder.seal_block(next_fallthrough);
                    }

                    branch_to_label(&mut builder, default_label);
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::I32_ADD => i32_binop!(builder, insn, iadd),
                op::I32_SUB => i32_binop!(builder, insn, isub),
                op::I32_MUL => i32_binop!(builder, insn, imul),
                op::I32_AND => i32_binop!(builder, insn, band),
                op::I32_OR => i32_binop!(builder, insn, bor),
                op::I32_XOR => i32_binop!(builder, insn, bxor),
                op::I32_SHL => i32_binop!(builder, insn, ishl),
                op::I32_SHRS => i32_binop!(builder, insn, sshr),
                op::I32_SHRU => i32_binop!(builder, insn, ushr),
                op::I32_ROTL => i32_binop!(builder, insn, rotl),
                op::I32_ROTR => i32_binop!(builder, insn, rotr),
                op::I32_DIVS => i32_binop!(builder, insn, sdiv),
                op::I32_DIVU => i32_binop!(builder, insn, udiv),
                op::I32_REMS => i32_binop!(builder, insn, srem),
                op::I32_REMU => i32_binop!(builder, insn, urem),
                op::I32_CLZ => i32_unop!(builder, insn, clz),
                op::I32_CTZ => i32_unop!(builder, insn, ctz),
                op::I32_POPCNT => i32_unop!(builder, insn, popcnt),

                op::I64_ADD => i64_binop!(builder, insn, iadd),
                op::I64_SUB => i64_binop!(builder, insn, isub),
                op::I64_MUL => i64_binop!(builder, insn, imul),
                op::I64_AND => i64_binop!(builder, insn, band),
                op::I64_OR => i64_binop!(builder, insn, bor),
                op::I64_XOR => i64_binop!(builder, insn, bxor),
                op::I64_SHL => i64_binop!(builder, insn, ishl),
                op::I64_SHRS => i64_binop!(builder, insn, sshr),
                op::I64_SHRU => i64_binop!(builder, insn, ushr),
                op::I64_ROTL => i64_binop!(builder, insn, rotl),
                op::I64_ROTR => i64_binop!(builder, insn, rotr),
                op::I64_DIVS => i64_binop!(builder, insn, sdiv),
                op::I64_DIVU => i64_binop!(builder, insn, udiv),
                op::I64_REMS => i64_binop!(builder, insn, srem),
                op::I64_REMU => i64_binop!(builder, insn, urem),
                op::I64_CLZ => i64_unop!(builder, insn, clz),
                op::I64_CTZ => i64_unop!(builder, insn, ctz),
                op::I64_POPCNT => i64_unop!(builder, insn, popcnt),

                op::I32_EQZ => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let r = builder.ins().icmp_imm_s(IntCC::Equal, src, 0);
                    let result = builder.ins().uextend(types::I32, r);
                    write_dst_i32!(builder, insn.destination, result);
                }
                op::I32_EQ => i32_cmp!(builder, insn, IntCC::Equal),
                op::I32_NE => i32_cmp!(builder, insn, IntCC::NotEqual),
                op::I32_LTS => i32_cmp!(builder, insn, IntCC::SignedLessThan),
                op::I32_LTU => i32_cmp!(builder, insn, IntCC::UnsignedLessThan),
                op::I32_GTS => i32_cmp!(builder, insn, IntCC::SignedGreaterThan),
                op::I32_GTU => i32_cmp!(builder, insn, IntCC::UnsignedGreaterThan),
                op::I32_LES => i32_cmp!(builder, insn, IntCC::SignedLessThanOrEqual),
                op::I32_LEU => i32_cmp!(builder, insn, IntCC::UnsignedLessThanOrEqual),
                op::I32_GES => i32_cmp!(builder, insn, IntCC::SignedGreaterThanOrEqual),
                op::I32_GEU => i32_cmp!(builder, insn, IntCC::UnsignedGreaterThanOrEqual),

                op::I64_EQZ => {
                    let src = read_src!(builder, insn.sources[0]);
                    let r = builder.ins().icmp_imm_s(IntCC::Equal, src, 0);
                    let result = builder.ins().uextend(types::I32, r);
                    write_dst_i32!(builder, insn.destination, result);
                }
                op::I64_EQ => i64_cmp!(builder, insn, IntCC::Equal),
                op::I64_NE => i64_cmp!(builder, insn, IntCC::NotEqual),
                op::I64_LTS => i64_cmp!(builder, insn, IntCC::SignedLessThan),
                op::I64_LTU => i64_cmp!(builder, insn, IntCC::UnsignedLessThan),
                op::I64_GTS => i64_cmp!(builder, insn, IntCC::SignedGreaterThan),
                op::I64_GTU => i64_cmp!(builder, insn, IntCC::UnsignedGreaterThan),
                op::I64_LES => i64_cmp!(builder, insn, IntCC::SignedLessThanOrEqual),
                op::I64_LEU => i64_cmp!(builder, insn, IntCC::UnsignedLessThanOrEqual),
                op::I64_GES => i64_cmp!(builder, insn, IntCC::SignedGreaterThanOrEqual),
                op::I64_GEU => i64_cmp!(builder, insn, IntCC::UnsignedGreaterThanOrEqual),

                op::F32_ADD => f32_binop!(builder, insn, fadd),
                op::F32_SUB => f32_binop!(builder, insn, fsub),
                op::F32_MUL => f32_binop!(builder, insn, fmul),
                op::F32_DIV => f32_binop!(builder, insn, fdiv),
                op::F32_MIN => f32_binop!(builder, insn, fmin),
                op::F32_MAX => f32_binop!(builder, insn, fmax),
                op::F32_COPYSIGN => f32_binop!(builder, insn, fcopysign),
                op::F32_ABS => f32_unop!(builder, insn, fabs),
                op::F32_NEG => f32_unop!(builder, insn, fneg),
                op::F32_CEIL => f32_unop!(builder, insn, ceil),
                op::F32_FLOOR => f32_unop!(builder, insn, floor),
                op::F32_TRUNC => f32_unop!(builder, insn, trunc),
                op::F32_NEAREST => f32_unop!(builder, insn, nearest),
                op::F32_SQRT => f32_unop!(builder, insn, sqrt),

                op::F64_ADD => f64_binop!(builder, insn, fadd),
                op::F64_SUB => f64_binop!(builder, insn, fsub),
                op::F64_MUL => f64_binop!(builder, insn, fmul),
                op::F64_DIV => f64_binop!(builder, insn, fdiv),
                op::F64_MIN => f64_binop!(builder, insn, fmin),
                op::F64_MAX => f64_binop!(builder, insn, fmax),
                op::F64_COPYSIGN => f64_binop!(builder, insn, fcopysign),
                op::F64_ABS => f64_unop!(builder, insn, fabs),
                op::F64_NEG => f64_unop!(builder, insn, fneg),
                op::F64_CEIL => f64_unop!(builder, insn, ceil),
                op::F64_FLOOR => f64_unop!(builder, insn, floor),
                op::F64_TRUNC => f64_unop!(builder, insn, trunc),
                op::F64_NEAREST => f64_unop!(builder, insn, nearest),
                op::F64_SQRT => f64_unop!(builder, insn, sqrt),

                op::F32_EQ => f32_cmp!(builder, insn, FloatCC::Equal),
                op::F32_NE => f32_cmp!(builder, insn, FloatCC::NotEqual),
                op::F32_LT => f32_cmp!(builder, insn, FloatCC::LessThan),
                op::F32_GT => f32_cmp!(builder, insn, FloatCC::GreaterThan),
                op::F32_LE => f32_cmp!(builder, insn, FloatCC::LessThanOrEqual),
                op::F32_GE => f32_cmp!(builder, insn, FloatCC::GreaterThanOrEqual),

                op::F64_EQ => f64_cmp!(builder, insn, FloatCC::Equal),
                op::F64_NE => f64_cmp!(builder, insn, FloatCC::NotEqual),
                op::F64_LT => f64_cmp!(builder, insn, FloatCC::LessThan),
                op::F64_GT => f64_cmp!(builder, insn, FloatCC::GreaterThan),
                op::F64_LE => f64_cmp!(builder, insn, FloatCC::LessThanOrEqual),
                op::F64_GE => f64_cmp!(builder, insn, FloatCC::GreaterThanOrEqual),

                op::I32_WRAP_I64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let result = builder.ins().ireduce(types::I32, src);
                    write_dst_i32!(builder, insn.destination, result);
                }
                op::I64_EXTEND_SI32 => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let result = builder.ins().sextend(types::I64, src);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND_UI32 => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let result = builder.ins().uextend(types::I64, src);
                    write_dst!(builder, insn.destination, result);
                }
                op::I32_EXTEND8_S => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I8, src);
                    let result = builder.ins().sextend(types::I32, narrowed);
                    write_dst_i32!(builder, insn.destination, result);
                }
                op::I32_EXTEND16_S => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I16, src);
                    let result = builder.ins().sextend(types::I32, narrowed);
                    write_dst_i32!(builder, insn.destination, result);
                }
                op::I64_EXTEND8_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I8, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND16_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I16, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND32_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                // Float-int conversions
                op::F32_CONVERT_SI32 => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let f32_val = builder.ins().fcvt_from_sint(types::F32, src);
                    write_dst_f32!(builder, insn.destination, f32_val);
                }
                op::F32_CONVERT_UI32 => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let f32_val = builder.ins().fcvt_from_uint(types::F32, src);
                    write_dst_f32!(builder, insn.destination, f32_val);
                }
                op::F32_CONVERT_SI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f32_val = builder.ins().fcvt_from_sint(types::F32, src);
                    write_dst_f32!(builder, insn.destination, f32_val);
                }
                op::F32_CONVERT_UI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f32_val = builder.ins().fcvt_from_uint(types::F32, src);
                    write_dst_f32!(builder, insn.destination, f32_val);
                }
                op::F64_CONVERT_SI32 => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let f64_val = builder.ins().fcvt_from_sint(types::F64, src);
                    write_dst_f64!(builder, insn.destination, f64_val);
                }
                op::F64_CONVERT_UI32 => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let f64_val = builder.ins().fcvt_from_uint(types::F64, src);
                    write_dst_f64!(builder, insn.destination, f64_val);
                }
                op::F64_CONVERT_SI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f64_val = builder.ins().fcvt_from_sint(types::F64, src);
                    write_dst_f64!(builder, insn.destination, f64_val);
                }
                op::F64_CONVERT_UI64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let f64_val = builder.ins().fcvt_from_uint(types::F64, src);
                    write_dst_f64!(builder, insn.destination, f64_val);
                }
                op::I32_REINTERPRET_F32 => {
                    let src = read_src_f32!(builder, insn.sources[0]);
                    let result = builder.ins().bitcast(types::I32, MemFlags::new(), src);
                    write_dst_i32!(builder, insn.destination, result);
                }
                op::F32_REINTERPRET_I32 => {
                    let src = read_src_i32!(builder, insn.sources[0]);
                    let result = builder.ins().bitcast(types::F32, MemFlags::new(), src);
                    write_dst_f32!(builder, insn.destination, result);
                }
                op::I64_REINTERPRET_F64 | op::F64_REINTERPRET_I64 => {
                    let src = read_src!(builder, insn.sources[0]);
                    write_dst!(builder, insn.destination, src);
                }
                op::F32_DEMOTE_F64 => {
                    let f64_val = read_src_f64!(builder, insn.sources[0]);
                    let f32_val = builder.ins().fdemote(types::F32, f64_val);
                    write_dst_f32!(builder, insn.destination, f32_val);
                }
                op::F64_PROMOTE_F32 => {
                    let f32_val = read_src_f32!(builder, insn.sources[0]);
                    let f64_val = builder.ins().fpromote(types::F64, f32_val);
                    write_dst_f64!(builder, insn.destination, f64_val);
                }
                // Truncation conversions (these can trap in wasm).
                // Cranelift's fcvt_to_sint/fcvt_to_uint trap on overflow/NaN; the handler then maps the resulting "BadConversionToInteger" trap back to a wasm trap.
                op::I32_TRUNC_SF32
                | op::I32_TRUNC_UF32
                | op::I32_TRUNC_SF64
                | op::I32_TRUNC_UF64
                | op::I64_TRUNC_SF32
                | op::I64_TRUNC_UF32
                | op::I64_TRUNC_SF64
                | op::I64_TRUNC_UF64 => {
                    let is_f32_src = matches!(
                        opc,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_UF32 | op::I64_TRUNC_SF32 | op::I64_TRUNC_UF32
                    );
                    let is_i32_dst = matches!(
                        opc,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_UF32 | op::I32_TRUNC_SF64 | op::I32_TRUNC_UF64
                    );
                    let is_signed = matches!(
                        opc,
                        op::I32_TRUNC_SF32 | op::I32_TRUNC_SF64 | op::I64_TRUNC_SF32 | op::I64_TRUNC_SF64
                    );

                    let float_val = if is_f32_src {
                        read_src_f32!(builder, insn.sources[0])
                    } else {
                        read_src_f64!(builder, insn.sources[0])
                    };

                    let int_type = if is_i32_dst { types::I32 } else { types::I64 };
                    let int_val = if is_signed {
                        builder.ins().fcvt_to_sint(int_type, float_val)
                    } else {
                        builder.ins().fcvt_to_uint(int_type, float_val)
                    };

                    if is_i32_dst {
                        write_dst_i32!(builder, insn.destination, int_val);
                    } else {
                        write_dst!(builder, insn.destination, int_val);
                    }
                }

                op::I32_TRUNC_SAT_F32_S
                | op::I32_TRUNC_SAT_F32_U
                | op::I32_TRUNC_SAT_F64_S
                | op::I32_TRUNC_SAT_F64_U
                | op::I64_TRUNC_SAT_F32_S
                | op::I64_TRUNC_SAT_F32_U
                | op::I64_TRUNC_SAT_F64_S
                | op::I64_TRUNC_SAT_F64_U => {
                    let is_f32_src = matches!(
                        opc,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F32_U
                            | op::I64_TRUNC_SAT_F32_S
                            | op::I64_TRUNC_SAT_F32_U
                    );
                    let is_i32_dst = matches!(
                        opc,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F32_U
                            | op::I32_TRUNC_SAT_F64_S
                            | op::I32_TRUNC_SAT_F64_U
                    );
                    let is_signed = matches!(
                        opc,
                        op::I32_TRUNC_SAT_F32_S
                            | op::I32_TRUNC_SAT_F64_S
                            | op::I64_TRUNC_SAT_F32_S
                            | op::I64_TRUNC_SAT_F64_S
                    );

                    let float_val = if is_f32_src {
                        read_src_f32!(builder, insn.sources[0])
                    } else {
                        read_src_f64!(builder, insn.sources[0])
                    };

                    let int_type = if is_i32_dst { types::I32 } else { types::I64 };
                    let int_val = if is_signed {
                        builder.ins().fcvt_to_sint_sat(int_type, float_val)
                    } else {
                        builder.ins().fcvt_to_uint_sat(int_type, float_val)
                    };

                    if is_i32_dst {
                        write_dst_i32!(builder, insn.destination, int_val);
                    } else {
                        write_dst!(builder, insn.destination, int_val);
                    }
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
                    // addr = base (from source reg as u32) + offset.
                    let base_u32 = read_src_i32!(builder, insn.sources[0]);
                    let base_u64 = builder.ins().uextend(types::I64, base_u32);
                    let offset = builder.ins().iconst(types::I64, insn.imm1);
                    let addr = builder.ins().iadd(base_u64, offset);

                    let mem_idx = insn.imm3;
                    let address = inline_memory_address!(builder, mem_idx, addr);
                    if opc == op::F64_LOAD {
                        let result = builder.ins().load(types::F64, wasm_memory_flags, address, 0);
                        write_dst_f64!(builder, insn.destination, result);
                    } else if opc == op::F32_LOAD {
                        let result = builder.ins().load(types::F32, wasm_memory_flags, address, 0);
                        write_dst_f32!(builder, insn.destination, result);
                    } else if matches!(
                        opc,
                        op::I32_LOAD | op::I32_LOAD8_S | op::I32_LOAD8_U | op::I32_LOAD16_S | op::I32_LOAD16_U
                    ) {
                        let result = match opc {
                            op::I32_LOAD => builder.ins().load(types::I32, wasm_memory_flags, address, 0),
                            op::I32_LOAD8_S => {
                                let value = builder.ins().load(types::I8, wasm_memory_flags, address, 0);
                                builder.ins().sextend(types::I32, value)
                            }
                            op::I32_LOAD8_U => {
                                let value = builder.ins().load(types::I8, wasm_memory_flags, address, 0);
                                builder.ins().uextend(types::I32, value)
                            }
                            op::I32_LOAD16_S => {
                                let value = builder.ins().load(types::I16, wasm_memory_flags, address, 0);
                                builder.ins().sextend(types::I32, value)
                            }
                            op::I32_LOAD16_U => {
                                let value = builder.ins().load(types::I16, wasm_memory_flags, address, 0);
                                builder.ins().uextend(types::I32, value)
                            }
                            _ => unreachable!(),
                        };
                        write_dst_i32!(builder, insn.destination, result);
                    } else {
                        let result = match opc {
                            op::I64_LOAD32_U => {
                                let value = builder.ins().load(types::I32, wasm_memory_flags, address, 0);
                                builder.ins().uextend(types::I64, value)
                            }
                            op::I64_LOAD => builder.ins().load(types::I64, wasm_memory_flags, address, 0),
                            op::I64_LOAD8_S => {
                                let value = builder.ins().load(types::I8, wasm_memory_flags, address, 0);
                                builder.ins().sextend(types::I64, value)
                            }
                            op::I64_LOAD8_U => {
                                let value = builder.ins().load(types::I8, wasm_memory_flags, address, 0);
                                builder.ins().uextend(types::I64, value)
                            }
                            op::I64_LOAD16_S => {
                                let value = builder.ins().load(types::I16, wasm_memory_flags, address, 0);
                                builder.ins().sextend(types::I64, value)
                            }
                            op::I64_LOAD16_U => {
                                let value = builder.ins().load(types::I16, wasm_memory_flags, address, 0);
                                builder.ins().uextend(types::I64, value)
                            }
                            op::I64_LOAD32_S => {
                                let value = builder.ins().load(types::I32, wasm_memory_flags, address, 0);
                                builder.ins().sextend(types::I64, value)
                            }
                            _ => unreachable!(),
                        };
                        write_dst!(builder, insn.destination, result);
                    }
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
                    let mem_idx = insn.imm3;
                    let is_f32 = opc == op::F32_STORE;
                    let is_i32 = matches!(opc, op::I32_STORE | op::I32_STORE8 | op::I32_STORE16);
                    let val = if is_f32 {
                        read_src_f32!(builder, insn.sources[0])
                    } else if is_i32 {
                        read_src_i32!(builder, insn.sources[0])
                    } else {
                        read_src!(builder, insn.sources[0])
                    };
                    let base_u32 = read_src_i32!(builder, insn.sources[1]);
                    let base_u64 = builder.ins().uextend(types::I64, base_u32);
                    let offset = builder.ins().iconst(types::I64, insn.imm1);
                    let addr = builder.ins().iadd(base_u64, offset);

                    let access_size = match opc {
                        op::I32_STORE8 | op::I64_STORE8 => 1,
                        op::I32_STORE16 | op::I64_STORE16 => 2,
                        op::I32_STORE | op::F32_STORE | op::I64_STORE32 => 4,
                        op::I64_STORE | op::F64_STORE => 8,
                        _ => unreachable!(),
                    };
                    let address = inline_memory_address!(builder, mem_idx, addr);
                    let value = if is_f32 || (is_i32 && access_size == 4) {
                        val
                    } else {
                        match access_size {
                            1 => builder.ins().ireduce(types::I8, val),
                            2 => builder.ins().ireduce(types::I16, val),
                            4 => builder.ins().ireduce(types::I32, val),
                            8 => val,
                            _ => unreachable!(),
                        }
                    };
                    builder.ins().store(wasm_memory_flags, value, address, 0);
                }

                op::MEMORY_SIZE => {
                    let mem_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let _xv_config_var = builder.use_var(config_var);
                    let _xc_0 = builder.ins().func_addr(ptr_type, h_mem_size);
                    let call = builder
                        .ins()
                        .call_indirect(mem_size_sig, _xc_0, &[_xv_config_var, mem_idx]);
                    let result = builder.inst_results(call)[0];
                    let result = builder.ins().ireduce(types::I32, result);
                    write_dst_i32!(builder, insn.destination, result);
                }

                op::MEMORY_GROW => {
                    let pages_i32 = read_src_i32!(builder, insn.sources[0]);
                    let mem_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let _xv_config_var = builder.use_var(config_var);
                    let _xc_0 = builder.ins().func_addr(ptr_type, h_mem_grow);
                    let call = builder
                        .ins()
                        .call_indirect(mem_grow_sig, _xc_0, &[_xv_config_var, mem_idx, pages_i32]);
                    let result = builder.inst_results(call)[0];
                    write_dst_i32!(builder, insn.destination, result);
                }

                op::MEMORY_COPY => {
                    // imm1 = dst_mem, imm2 = src_mem
                    // sources: [0]=count, [1]=src_offset, [2]=dst_offset
                    let count_i32 = read_src_i32!(builder, insn.sources[0]);
                    let src_i32 = read_src_i32!(builder, insn.sources[1]);
                    let dst_i32 = read_src_i32!(builder, insn.sources[2]);
                    let dst_mem = builder.ins().iconst(types::I32, insn.imm1);
                    let src_mem = builder.ins().iconst(types::I32, insn.imm2);
                    let cfp = builder.ins().func_addr(ptr_type, h_memory_copy);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(
                        builder,
                        memory_copy_sig,
                        cfp,
                        &[iv, cv, dst_mem, src_mem, dst_i32, src_i32, count_i32]
                    );
                }

                op::MEMORY_FILL => {
                    // imm1 = mem_idx
                    // sources: [0]=count, [1]=value, [2]=offset
                    let count_i32 = read_src_i32!(builder, insn.sources[0]);
                    let value_i32 = read_src_i32!(builder, insn.sources[1]);
                    let offset_i32 = read_src_i32!(builder, insn.sources[2]);
                    let mem_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let cfp = builder.ins().func_addr(ptr_type, h_memory_fill);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(
                        builder,
                        memory_fill_sig,
                        cfp,
                        &[iv, cv, mem_idx, offset_i32, value_i32, count_i32]
                    );
                }

                // Use the typed native ABI for supported direct calls. Otherwise, materialize the
                // live vstack only for the duration of the opaque runtime call, then resume using it.
                op::CALL => {
                    let target_index = u32::try_from(insn.imm1).map_err(|_| "invalid direct-call target")?;
                    let target_type = *function_types
                        .get(target_index as usize)
                        .ok_or("missing direct-call function type")?;
                    if Self::uses_register_native_abi(target_type)
                        && let Some(&target) = direct_call_targets.get(&target_index)
                    {
                        let param_count = insn.imm3 as usize;
                        if target_type.parameters.len() != param_count
                            || target_type.results.len() != insn.call_result_count as usize
                        {
                            return Err("raw call does not match target type");
                        }
                        let mut reversed_arguments = Vec::with_capacity(param_count);
                        for source_index in 0..param_count {
                            let parameter_index = param_count - source_index - 1;
                            let argument = match target_type.parameters[parameter_index] {
                                I32_KIND => read_src_i32!(builder, STACK_MARKER),
                                I64_KIND => read_src!(builder, STACK_MARKER),
                                F32_KIND => read_src_f32!(builder, STACK_MARKER),
                                F64_KIND => read_src_f64!(builder, STACK_MARKER),
                                _ => return Err("unsupported native Wasm ABI type"),
                            };
                            reversed_arguments.push(argument);
                        }
                        let iv = builder.use_var(interp_var);
                        let cv = builder.use_var(config_var);
                        let entry_token = builder.ins().iconst(types::I32, 0);
                        let mut arguments = Vec::with_capacity(3 + param_count);
                        arguments.extend([iv, cv, entry_token]);
                        arguments.extend(reversed_arguments.into_iter().rev());
                        let call = builder.ins().call(target, &arguments);
                        if let Some(&result_kind) = target_type.results.first() {
                            let result = builder.inst_results(call)[0];
                            match result_kind {
                                I32_KIND => write_dst_i32!(builder, insn.destination, result),
                                I64_KIND => write_dst!(builder, insn.destination, result),
                                F32_KIND => write_dst_f32!(builder, insn.destination, result),
                                F64_KIND => write_dst_f64!(builder, insn.destination, result),
                                _ => return Err("unsupported native Wasm ABI type"),
                            }
                        }
                    } else {
                        let original_top = materialize_vstack_suffix_to_real!(builder, insn.imm3);
                        debug_assert!(is_unreachable || sp >= insn.imm3 as usize);
                        let stack_base = sp.saturating_sub(insn.imm3 as usize);
                        let func_idx = builder.ins().iconst(types::I32, insn.imm1);
                        let cfp = builder.ins().func_addr(ptr_type, h_call_fn);
                        let iv = builder.use_var(interp_var);
                        let cv = builder.use_var(config_var);
                        do_call_and_check!(builder, call_fn_sig, cfp, &[iv, cv, func_idx]);
                        restore_vstack_after_raw_call!(
                            builder,
                            original_top,
                            stack_base,
                            insn.call_result_count as usize,
                            insn.destination
                        );
                    }
                }

                op::CALL_INDIRECT => {
                    let element_index = if insn.call_type_encoding & INDIRECT_CALL_TABLE64 != 0 {
                        read_src!(builder, insn.sources[0])
                    } else {
                        let element_index = read_src_i32!(builder, insn.sources[0]);
                        builder.ins().uextend(types::I64, element_index)
                    };
                    debug_assert!(is_unreachable || sp >= insn.imm3 as usize);
                    let stack_base = sp.saturating_sub(insn.imm3 as usize);
                    let type_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let table_idx = builder.ins().iconst(types::I32, insn.imm2);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    let operands = IndirectCallOperands {
                        interpreter: iv,
                        configuration: cv,
                        table_index: table_idx,
                        type_index: type_idx,
                        element_index,
                    };
                    let call_type = if insn.call_result_count <= 1 {
                        Self::decode_indirect_call_type(insn)?
                    } else {
                        None
                    };

                    let mut emitted_native_call = false;
                    if let Some(call_type) = call_type {
                        let target_type = WasmFunctionType {
                            parameters: &call_type.parameters,
                            results: &call_type.results,
                        };
                        if target_type.parameters.len() != insn.imm3 as usize
                            || target_type.results.len() != insn.call_result_count as usize
                        {
                            return Err("raw indirect call does not match target type");
                        }

                        if Self::uses_register_native_abi(target_type) {
                            let mut wasm_arguments = Vec::with_capacity(target_type.parameters.len());
                            for (parameter_index, &parameter_kind) in target_type.parameters.iter().enumerate() {
                                let stack_index = stack_base + parameter_index;
                                let argument = match parameter_kind {
                                    I32_KIND => stack_value_as_bank!(builder, stack_index, Bank::I32),
                                    I64_KIND => stack_value_as_bank!(builder, stack_index, Bank::I64),
                                    F32_KIND => stack_value_as_bank!(builder, stack_index, Bank::F32),
                                    F64_KIND => stack_value_as_bank!(builder, stack_index, Bank::F64),
                                    _ => return Err("unsupported native Wasm ABI type"),
                                };
                                wasm_arguments.push(argument);
                            }
                            let native_signature =
                                builder.import_signature(Self::native_signature(&*isa, target_type)?);
                            let continuation = builder.create_block();
                            if !target_type.results.is_empty() {
                                builder.append_block_param(continuation, types::I64);
                            }

                            let target = Self::emit_native_indirect_call_target(
                                &mut builder,
                                insn,
                                operands,
                                indirect_call_lowering_context,
                            )?;

                            let native_entry = builder.block_params(target.native_call)[0];
                            let entry_token = builder.ins().iconst(types::I32, 0);
                            let mut native_arguments = Vec::with_capacity(3 + wasm_arguments.len());
                            native_arguments.extend([iv, cv, entry_token]);
                            native_arguments.extend(wasm_arguments);
                            let native_call =
                                builder
                                    .ins()
                                    .call_indirect(native_signature, native_entry, &native_arguments);
                            let native_result = if let Some(&result_kind) = target_type.results.first() {
                                let result = builder.inst_results(native_call)[0];
                                Some(value_to_payload(&mut builder, result, result_kind)?)
                            } else {
                                None
                            };
                            if let Some(result) = native_result {
                                builder.ins().jump(continuation, &[BlockArg::Value(result)]);
                            } else {
                                builder.ins().jump(continuation, &[]);
                            }

                            builder.switch_to_block(target.fallback_call);
                            let original_top = materialize_vstack_suffix_to_real!(builder, insn.imm3);
                            let cfp = builder.ins().func_addr(ptr_type, h_call_indirect);
                            do_call_and_check!(
                                builder,
                                call_indirect_sig,
                                cfp,
                                &[iv, cv, table_idx, type_idx, element_index]
                            );
                            let fallback_result = if target_type.results.is_empty() {
                                None
                            } else {
                                let helper_top = builder.ins().load(
                                    ptr_type,
                                    memory_flags.configuration,
                                    cv,
                                    value_stack_top_offset,
                                );
                                let result =
                                    builder
                                        .ins()
                                        .load(types::I64, memory_flags.activation, helper_top, -value_size);
                                Some(result)
                            };
                            builder
                                .ins()
                                .store(memory_flags.configuration, original_top, cv, value_stack_top_offset);
                            if let Some(result) = fallback_result {
                                builder.ins().jump(continuation, &[BlockArg::Value(result)]);
                            } else {
                                builder.ins().jump(continuation, &[]);
                            }

                            builder.switch_to_block(continuation);
                            builder.seal_block(continuation);
                            sp = stack_base;
                            if let Some(&result_kind) = target_type.results.first() {
                                let payload = builder.block_params(continuation)[0];
                                match result_kind {
                                    I32_KIND => {
                                        let value = payload_to_value(&mut builder, payload, I32_KIND)?;
                                        write_dst_i32!(builder, insn.destination, value);
                                    }
                                    I64_KIND => write_dst!(builder, insn.destination, payload),
                                    F32_KIND => {
                                        let value = payload_to_value(&mut builder, payload, F32_KIND)?;
                                        write_dst_f32!(builder, insn.destination, value);
                                    }
                                    F64_KIND => {
                                        let value = payload_to_value(&mut builder, payload, F64_KIND)?;
                                        write_dst_f64!(builder, insn.destination, value);
                                    }
                                    _ => return Err("unsupported native Wasm ABI type"),
                                }
                            }
                            emitted_native_call = true;
                        }
                    }

                    if !emitted_native_call {
                        let original_top = materialize_vstack_suffix_to_real!(builder, insn.imm3);
                        let cfp = builder.ins().func_addr(ptr_type, h_call_indirect);
                        do_call_and_check!(
                            builder,
                            call_indirect_sig,
                            cfp,
                            &[iv, cv, table_idx, type_idx, element_index]
                        );
                        restore_vstack_after_raw_call!(
                            builder,
                            original_top,
                            stack_base,
                            insn.call_result_count as usize,
                            insn.destination
                        );
                    }
                }

                opc if (op::SYNTHETIC_CALL_00..=op::SYNTHETIC_CALL_31).contains(&opc) => {
                    let variant = (opc - op::SYNTHETIC_CALL_00) as usize;
                    let param_count = variant / 2;
                    let result_count = variant % 2;
                    let target_index = u32::try_from(insn.imm1).map_err(|_| "invalid direct-call target")?;
                    let target_type = *function_types
                        .get(target_index as usize)
                        .ok_or("missing direct-call function type")?;
                    if target_type.parameters.len() != param_count || target_type.results.len() != result_count {
                        return Err("synthetic call does not match target type");
                    }
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    let entry_token = builder.ins().iconst(types::I32, 0);
                    let mut reversed_arguments = Vec::with_capacity(param_count);
                    for source_index in 0..param_count {
                        let parameter_index = param_count - source_index - 1;
                        let source = insn.sources[source_index];
                        let argument = match target_type.parameters[parameter_index] {
                            I32_KIND => read_src_i32!(builder, source),
                            I64_KIND => read_src!(builder, source),
                            F32_KIND => read_src_f32!(builder, source),
                            F64_KIND => read_src_f64!(builder, source),
                            _ => return Err("unsupported native Wasm ABI type"),
                        };
                        reversed_arguments.push(argument);
                    }
                    let mut arguments = Vec::with_capacity(3 + param_count);
                    arguments.extend([iv, cv, entry_token]);
                    arguments.extend(reversed_arguments.into_iter().rev());
                    let target = direct_call_targets
                        .get(&target_index)
                        .copied()
                        .expect("direct-call target must be declared");
                    let call = builder.ins().call(target, &arguments);

                    if let Some(&result_kind) = target_type.results.first() {
                        let result = builder.inst_results(call)[0];
                        match result_kind {
                            I32_KIND => write_dst_i32!(builder, insn.destination, result),
                            I64_KIND => write_dst!(builder, insn.destination, result),
                            F32_KIND => write_dst_f32!(builder, insn.destination, result),
                            F64_KIND => write_dst_f64!(builder, insn.destination, result),
                            _ => return Err("unsupported native Wasm ABI type"),
                        }
                    }
                }

                op::SYNTHETIC_CALL_WITH_RECORD_0 | op::SYNTHETIC_CALL_WITH_RECORD_1 => {
                    let target_index = u32::try_from(insn.imm1).map_err(|_| "invalid direct-call target")?;
                    let target_type = *function_types
                        .get(target_index as usize)
                        .ok_or("missing direct-call function type")?;
                    let result_count = if opc == op::SYNTHETIC_CALL_WITH_RECORD_1 { 1 } else { 0 };
                    if target_type.results.len() != result_count {
                        return Err("call-record call does not match target type");
                    }
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    let entry_token = builder.ins().iconst(types::I32, 0);
                    let call_record =
                        builder
                            .ins()
                            .load(ptr_type, memory_flags.configuration, cv, call_record_base_offset);
                    let uses_register_native_abi = Self::uses_register_native_abi(target_type);
                    let mut arguments = Vec::with_capacity(if uses_register_native_abi {
                        3 + target_type.parameters.len()
                    } else {
                        4
                    });
                    arguments.extend([iv, cv, entry_token]);
                    if uses_register_native_abi {
                        for (parameter_index, &parameter_kind) in target_type.parameters.iter().enumerate() {
                            let offset = i32::try_from(parameter_index * value_size as usize)
                                .map_err(|_| "call-record argument offset overflow")?;
                            let argument_type = wasm_abi_type(parameter_kind)?;
                            let argument =
                                builder
                                    .ins()
                                    .load(argument_type, memory_flags.activation, call_record, offset);
                            arguments.push(argument);
                        }
                    } else {
                        arguments.push(call_record);
                    }
                    let target = direct_call_targets
                        .get(&target_index)
                        .copied()
                        .expect("direct-call target must be declared");
                    let call = builder.ins().call(target, &arguments);
                    if let Some(&result_kind) = target_type.results.first() {
                        let result = builder.inst_results(call)[0];
                        match result_kind {
                            I32_KIND => write_dst_i32!(builder, insn.destination, result),
                            I64_KIND => write_dst!(builder, insn.destination, result),
                            F32_KIND => write_dst_f32!(builder, insn.destination, result),
                            F64_KIND => write_dst_f64!(builder, insn.destination, result),
                            _ => return Err("unsupported native Wasm ABI type"),
                        }
                    }
                }

                op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_0 | op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_1 => {
                    let element_index = if insn.call_type_encoding & INDIRECT_CALL_TABLE64 != 0 {
                        read_src!(builder, insn.sources[0])
                    } else {
                        let element_index = read_src_i32!(builder, insn.sources[0]);
                        builder.ins().uextend(types::I64, element_index)
                    };
                    let interpreter = builder.use_var(interp_var);
                    let configuration = builder.use_var(config_var);

                    let result = Self::emit_indirect_call_with_record(
                        &mut builder,
                        &*isa,
                        insn,
                        interpreter,
                        configuration,
                        element_index,
                        indirect_call_lowering_context,
                    )?;
                    if let Some(result) = result {
                        match result.kind {
                            Some(I32_KIND) => {
                                let value = payload_to_value(&mut builder, result.payload, I32_KIND)?;
                                write_dst_i32!(builder, insn.destination, value);
                            }
                            Some(I64_KIND) | None => write_dst!(builder, insn.destination, result.payload),
                            Some(F32_KIND) => {
                                let value = payload_to_value(&mut builder, result.payload, F32_KIND)?;
                                write_dst_f32!(builder, insn.destination, value);
                            }
                            Some(F64_KIND) => {
                                let value = payload_to_value(&mut builder, result.payload, F64_KIND)?;
                                write_dst_f64!(builder, insn.destination, value);
                            }
                            _ => return Err("unsupported native Wasm ABI type"),
                        }
                    }
                }

                op::SYNTHETIC_LOCAL_SETI32_CONST => {
                    let val = builder.ins().iconst(types::I32, insn.imm1);
                    write_local_i32!(builder, insn.imm2, val);
                }
                op::SYNTHETIC_LOCAL_SETI64_CONST => {
                    let val = builder.ins().iconst(types::I64, insn.imm1);
                    write_local_inline!(builder, insn.imm2, val);
                }

                op::SYNTHETIC_I32_ADD2LOCAL => {
                    let v1 = read_local_i32!(builder, insn.imm1);
                    let v2 = read_local_i32!(builder, insn.imm2);
                    let result = builder.ins().iadd(v1, v2);
                    write_dst_i32!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_ADDCONSTLOCAL => {
                    let v = read_local_i32!(builder, insn.imm2);
                    let k = builder.ins().iconst(types::I32, insn.imm1);
                    let result = builder.ins().iadd(v, k);
                    write_dst_i32!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_ANDCONSTLOCAL => {
                    let v = read_local_i32!(builder, insn.imm2);
                    let k = builder.ins().iconst(types::I32, insn.imm1);
                    let result = builder.ins().band(v, k);
                    write_dst_i32!(builder, insn.destination, result);
                }

                opc if (op::SYNTHETIC_I32_SUB2LOCAL..=op::SYNTHETIC_I32_SHRS2LOCAL).contains(&opc) => {
                    let v1 = read_local_i32!(builder, insn.imm1);
                    let v2 = read_local_i32!(builder, insn.imm2);
                    let result = match opc {
                        op::SYNTHETIC_I32_SUB2LOCAL => builder.ins().isub(v1, v2),
                        op::SYNTHETIC_I32_MUL2LOCAL => builder.ins().imul(v1, v2),
                        op::SYNTHETIC_I32_AND2LOCAL => builder.ins().band(v1, v2),
                        op::SYNTHETIC_I32_OR2LOCAL => builder.ins().bor(v1, v2),
                        op::SYNTHETIC_I32_XOR2LOCAL => builder.ins().bxor(v1, v2),
                        op::SYNTHETIC_I32_SHL2LOCAL => builder.ins().ishl(v1, v2),
                        op::SYNTHETIC_I32_SHRU2LOCAL => builder.ins().ushr(v1, v2),
                        op::SYNTHETIC_I32_SHRS2LOCAL => builder.ins().sshr(v1, v2),
                        _ => unreachable!(),
                    };
                    write_dst_i32!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I64_ADD2LOCAL => {
                    let v1 = read_local_inline!(builder, insn.imm1);
                    let v2 = read_local_inline!(builder, insn.imm2);
                    let result = builder.ins().iadd(v1, v2);
                    write_dst!(builder, insn.destination, result);
                }
                op::SYNTHETIC_I64_ADDCONSTLOCAL => {
                    let v = read_local_inline!(builder, insn.imm2);
                    let k = builder.ins().iconst(types::I64, insn.imm1);
                    let result = builder.ins().iadd(v, k);
                    write_dst!(builder, insn.destination, result);
                }
                op::SYNTHETIC_I64_ANDCONSTLOCAL => {
                    let v = read_local_inline!(builder, insn.imm2);
                    let k = builder.ins().iconst(types::I64, insn.imm1);
                    let result = builder.ins().band(v, k);
                    write_dst!(builder, insn.destination, result);
                }
                opc if (op::SYNTHETIC_I64_SUB2LOCAL..=op::SYNTHETIC_I64_SHRS2LOCAL).contains(&opc) => {
                    let v1 = read_local_inline!(builder, insn.imm1);
                    let v2 = read_local_inline!(builder, insn.imm2);
                    let result = match opc {
                        op::SYNTHETIC_I64_SUB2LOCAL => builder.ins().isub(v1, v2),
                        op::SYNTHETIC_I64_MUL2LOCAL => builder.ins().imul(v1, v2),
                        op::SYNTHETIC_I64_AND2LOCAL => builder.ins().band(v1, v2),
                        op::SYNTHETIC_I64_OR2LOCAL => builder.ins().bor(v1, v2),
                        op::SYNTHETIC_I64_XOR2LOCAL => builder.ins().bxor(v1, v2),
                        op::SYNTHETIC_I64_SHL2LOCAL => builder.ins().ishl(v1, v2),
                        op::SYNTHETIC_I64_SHRU2LOCAL => builder.ins().ushr(v1, v2),
                        op::SYNTHETIC_I64_SHRS2LOCAL => builder.ins().sshr(v1, v2),
                        _ => unreachable!(),
                    };
                    write_dst!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_STORELOCAL | op::SYNTHETIC_I64_STORELOCAL => {
                    let base_u32 = read_src_i32!(builder, insn.sources[0]);
                    let base_u64 = builder.ins().uextend(types::I64, base_u32);
                    let offset = builder.ins().iconst(types::I64, insn.imm1);
                    let addr = builder.ins().iadd(base_u64, offset);

                    let mem_idx = insn.imm3;
                    let address = inline_memory_address!(builder, mem_idx, addr);
                    let value = if opc == op::SYNTHETIC_I32_STORELOCAL {
                        read_local_i32!(builder, insn.imm2)
                    } else {
                        read_local_inline!(builder, insn.imm2)
                    };
                    builder.ins().store(wasm_memory_flags, value, address, 0);
                }

                op::SYNTHETIC_TIER_UP => {
                    if let Some(tail) = tier_up_dispatch_tail {
                        let (header, header_reg_ty, header_live_registers) = {
                            let frame = control_stack.last().expect("tier-up checkpoint must be inside a loop");
                            debug_assert!(
                                frame
                                    .branch_target_stack_ty
                                    .as_ref()
                                    .is_some_and(|stack_ty| stack_ty.is_empty())
                            );
                            (
                                frame.branch_target,
                                frame
                                    .branch_target_reg_ty
                                    .expect("loop header register banks must be initialized"),
                                frame.branch_target_live_registers,
                            )
                        };
                        let next_tail = builder.create_block();
                        builder.set_cold_block(next_tail);
                        builder.switch_to_block(tail);
                        // Tier-up dispatch starts with canonical payloads loaded from the
                        // interpreter configuration. Recreate the typed banks required by this
                        // particular loop header before taking its resume edge.
                        for (index, &bank) in header_reg_ty.iter().enumerate() {
                            if header_live_registers & (1 << index) == 0 {
                                continue;
                            }
                            if bank != Bank::I64 {
                                materialize_register_bank!(builder, index, Bank::I64, bank);
                            }
                        }
                        let matches = builder.ins().icmp_imm_s(IntCC::Equal, tier_up_target_ip, insn.imm1);
                        builder.ins().brif(matches, header, &[], next_tail, &[]);
                        builder.seal_block(tail);
                        tier_up_dispatch_tail = Some(next_tail);
                        builder.switch_to_block(header);
                        reg_ty = header_reg_ty;
                    }
                }

                _ => {
                    return Err("unsupported instruction during codegen");
                }
            }

            ip += 1;
        }

        // If we fell through without hitting end, sync all results and head to the epilogue.
        if !is_unreachable {
            push_top_n_to_real!(builder, result_arity);
            prepare_register_banks_for_epilogue!(builder);
            builder.ins().jump(epilogue_block, &[]);
        }

        if let Some(tail) = tier_up_dispatch_tail {
            let body_start = tier_up_body_start.expect("tier_up_body_start set when dispatch tail exists");
            builder.switch_to_block(tail);
            builder.ins().jump(body_start, &[]);
            builder.seal_block(tail);
            builder.seal_block(body_start);
        }

        macro_rules! restore_direct_call_context {
            ($builder:expr) => {{
                let cfg = $builder.use_var(config_var);
                let saved_call_record = $builder.use_var(saved_call_record_base_var);
                let saved_call_record_top = $builder.use_var(saved_call_record_top_var);
                let saved_expression = $builder.use_var(saved_expression_var);
                let saved_depth = $builder.use_var(saved_depth_var);
                $builder.ins().store(
                    memory_flags.configuration,
                    saved_call_record,
                    cfg,
                    call_record_base_offset,
                );
                $builder.ins().store(
                    memory_flags.configuration,
                    saved_call_record_top,
                    cfg,
                    call_record_stack_top_offset,
                );
                $builder.ins().store(
                    memory_flags.configuration,
                    saved_expression,
                    cfg,
                    current_expression_offset,
                );
                $builder
                    .ins()
                    .store(memory_flags.configuration, saved_depth, cfg, depth_offset);
            }};
        }

        builder.switch_to_block(trap_block);
        builder.seal_block(trap_block);
        // The helper already stored the trap on the interpreter. Propagate it after the helper's
        // C++ cleanup has completed, without adding a status result to the native Wasm ABI.
        let raise_trap = builder.ins().func_addr(ptr_type, h_raise_trap);
        builder.ins().call_indirect(raise_trap_sig, raise_trap, &[]);
        if let Some(&result_kind) = function_type.results.first() {
            let zero = builder.ins().iconst(types::I64, 0);
            let zero = payload_to_value(&mut builder, zero, result_kind)?;
            builder.ins().return_(&[zero]);
        } else {
            builder.ins().return_(&[]);
        }

        builder.switch_to_block(epilogue_block);
        builder.seal_block(epilogue_block);
        reg_ty = epilogue_reg_ty.unwrap_or([Bank::I64; REG_COUNT]);
        // Clean up excess values on the real stack (e.g. from BR out of nested blocks); nothing to touch if we have vstack info.
        if has_raw_call {
            let init_size = builder.use_var(initial_stack_size_var);
            emit_stack_cleanup!(builder, init_size, result_arity);
        }
        let native_result = if let Some(&result_kind) = function_type.results.first() {
            let payload = emit_stack_pop!(builder);
            Some(payload_to_value(&mut builder, payload, result_kind)?)
        } else {
            None
        };
        sync_registers_to_config!(builder);
        let direct_return = builder.create_block();
        let native_return = builder.create_block();
        let direct_call_mode = builder.use_var(direct_call_mode_var);
        builder
            .ins()
            .brif(direct_call_mode, direct_return, &[], native_return, &[]);

        builder.switch_to_block(direct_return);
        builder.seal_block(direct_return);
        restore_direct_call_context!(builder);
        builder.ins().jump(native_return, &[]);

        builder.switch_to_block(native_return);
        builder.seal_block(native_return);
        if let Some(result) = native_result {
            builder.ins().return_(&[result]);
        } else {
            builder.ins().return_(&[]);
        }

        builder.finalize(isa.frontend_config());

        let body = compile_function(&*isa, func)?;
        let mut fallback_target_indices: Vec<u32> = direct_call_targets.keys().copied().collect();
        fallback_target_indices.sort_unstable();
        let mut fallback_functions = Vec::with_capacity(fallback_target_indices.len());
        for target_index in fallback_target_indices {
            let target_type = *function_types
                .get(target_index as usize)
                .ok_or("missing fallback function type")?;
            let fallback = Self::compile_interpreter_fallback(&*isa, target_index, target_type, layout)?;
            fallback_functions.push((target_index, fallback));
        }

        let mut adapter = Function::with_name_signature(UserFuncName::user(1, function_index), handler_signature);
        let adapter_memory_flags = WasmMemoryFlags::new(&mut adapter);
        let mut adapter_builder_context = FunctionBuilderContext::new();
        let mut adapter_builder = FunctionBuilder::new(&mut adapter, &mut adapter_builder_context);
        let adapter_entry = adapter_builder.create_block();
        adapter_builder.append_block_params_for_function_params(adapter_entry);
        adapter_builder.switch_to_block(adapter_entry);
        adapter_builder.seal_block(adapter_entry);
        let adapter_params = adapter_builder.block_params(adapter_entry).to_vec();
        let locals_base = adapter_builder.ins().load(
            ptr_type,
            adapter_memory_flags.configuration,
            adapter_params[1],
            locals_base_offset,
        );
        let entry_token = adapter_builder.ins().iadd_imm_s(adapter_params[3], 1);
        let uses_register_native_abi = Self::uses_register_native_abi(function_type);
        let mut body_arguments = Vec::with_capacity(if uses_register_native_abi {
            3 + function_type.parameters.len()
        } else {
            4
        });
        body_arguments.extend([adapter_params[0], adapter_params[1], entry_token]);
        if uses_register_native_abi {
            for (parameter_index, &parameter_kind) in function_type.parameters.iter().enumerate() {
                let offset = i32::try_from(parameter_index * value_size as usize)
                    .map_err(|_| "adapter parameter offset overflow")?;
                let parameter_type = wasm_abi_type(parameter_kind)?;
                let parameter =
                    adapter_builder
                        .ins()
                        .load(parameter_type, adapter_memory_flags.activation, locals_base, offset);
                body_arguments.push(parameter);
            }
        } else {
            body_arguments.push(locals_base);
        }
        let body_signature = adapter_builder.import_signature(native_signature);
        let body_name = adapter_builder.func.declare_imported_user_function(UserExternalName {
            namespace: WASM_FUNCTION_EXTERNAL_NAMESPACE,
            index: function_index,
        });
        let body_function = adapter_builder.func.import_function(ExtFuncData {
            name: ExternalName::user(body_name),
            signature: body_signature,
            colocated: true,
            patchable: false,
        });
        let call = adapter_builder.ins().call(body_function, &body_arguments);
        if let Some(&result_kind) = function_type.results.first() {
            let result = adapter_builder.inst_results(call)[0];
            let payload = value_to_payload(&mut adapter_builder, result, result_kind)?;
            let top = adapter_builder.ins().load(
                ptr_type,
                adapter_memory_flags.configuration,
                adapter_params[1],
                value_stack_top_offset,
            );
            adapter_builder
                .ins()
                .store(adapter_memory_flags.activation, payload, top, 0);
            let zero_tag = adapter_builder.ins().iconst(types::I64, 0);
            adapter_builder
                .ins()
                .store(adapter_memory_flags.activation, zero_tag, top, 8);
            let new_top = adapter_builder.ins().iadd_imm_s(top, i64::from(value_size));
            adapter_builder.ins().store(
                adapter_memory_flags.configuration,
                new_top,
                adapter_params[1],
                value_stack_top_offset,
            );
        }
        adapter_builder.ins().return_(&[]);
        adapter_builder.finalize(isa.frontend_config());
        let adapter = compile_function(&*isa, adapter)?;

        let native_entry_offset = adapter.code.len().div_ceil(16) * 16;
        let native_entry_offset = u32::try_from(native_entry_offset).map_err(|_| "native entry offset overflow")?;
        let mut code = adapter.code;
        code.resize(native_entry_offset as usize, 0);
        code.extend_from_slice(&body.code);

        let mut positioned_fallbacks = Vec::with_capacity(fallback_functions.len());
        let mut fallback_offsets = HashMap::with_capacity(fallback_functions.len());
        for (target_index, fallback) in fallback_functions {
            let fallback_offset = code.len().div_ceil(16) * 16;
            code.resize(fallback_offset, 0);
            let fallback_offset = u32::try_from(fallback_offset).map_err(|_| "fallback entry offset overflow")?;
            fallback_offsets.insert(target_index, fallback_offset);
            code.extend_from_slice(&fallback.code);
            positioned_fallbacks.push((fallback_offset, fallback));
        }

        let mut relocs = adapter.relocs;
        let fallback_reloc_count: usize = positioned_fallbacks
            .iter()
            .map(|(_, fallback)| fallback.relocs.len())
            .sum();
        relocs.reserve(body.relocs.len() + fallback_reloc_count);
        for mut relocation in body.relocs {
            relocation.code_offset = relocation
                .code_offset
                .checked_add(native_entry_offset)
                .ok_or("relocation offset overflow")?;
            if relocation.target_kind == CraneliftRelocationTargetKind::WasmFunction {
                relocation.fallback_offset = *fallback_offsets
                    .get(&relocation.target_index)
                    .ok_or("missing direct-call fallback")?;
            }
            relocs.push(relocation);
        }
        for (fallback_offset, fallback) in &positioned_fallbacks {
            for mut relocation in fallback.relocs.iter().copied() {
                relocation.code_offset = relocation
                    .code_offset
                    .checked_add(*fallback_offset)
                    .ok_or("fallback relocation offset overflow")?;
                relocs.push(relocation);
            }
        }

        let mut traps = adapter.traps;
        let fallback_trap_count: usize = positioned_fallbacks
            .iter()
            .map(|(_, fallback)| fallback.traps.len())
            .sum();
        traps.reserve(body.traps.len() + fallback_trap_count);
        for mut trap in body.traps {
            trap.offset = trap
                .offset
                .checked_add(native_entry_offset)
                .ok_or("trap offset overflow")?;
            traps.push(trap);
        }
        for (fallback_offset, fallback) in positioned_fallbacks {
            for mut trap in fallback.traps {
                trap.offset = trap
                    .offset
                    .checked_add(fallback_offset)
                    .ok_or("fallback trap offset overflow")?;
                traps.push(trap);
            }
        }

        Ok(CompiledFunction {
            code,
            native_entry_offset,
            relocs,
            traps,
        })
    }

    fn is_supported(insn: &CraneliftInsn) -> bool {
        let opc = insn.opcode;
        matches!(
            opc,
            op::NOP
                | op::UNREACHABLE
                | op::BLOCK
                | op::LOOP
                | op::IF
                | op::ELSE
                | op::END
                | op::BR
                | op::BR_IF
                | op::BR_TABLE
                | op::RETURN
                | op::CALL
                | op::DROP
                | op::SELECT
                | op::SELECT_TYPED
                | op::LOCAL_GET
                | op::LOCAL_SET
                | op::LOCAL_TEE
                | op::GLOBAL_GET
                | op::GLOBAL_SET
                | op::I32_CONST
                | op::I64_CONST
                | op::F32_CONST
                | op::F64_CONST
                | op::I32_EQZ..=op::I32_GEU
                | op::I64_EQZ..=op::I64_GEU
                | op::F32_EQ..=op::F64_GE
                | op::I32_CLZ..=op::I32_ROTR
                | op::I64_CLZ..=op::I64_ROTR
                | op::F32_ABS..=op::F32_COPYSIGN
                | op::F64_ABS..=op::F64_COPYSIGN
                | op::I32_WRAP_I64..=op::F64_REINTERPRET_I64
                | op::I32_EXTEND8_S..=op::I64_EXTEND32_S
                | op::I32_LOAD..=op::I64_STORE32
                | op::MEMORY_SIZE
                | op::MEMORY_GROW
                | op::CALL_INDIRECT
                | op::I32_TRUNC_SAT_F32_S..=op::I64_TRUNC_SAT_F64_U
                | op::MEMORY_COPY
                | op::MEMORY_FILL
                | op::SYNTHETIC_END_EXPRESSION
                | op::SYNTHETIC_LOCAL_GET_0..=op::SYNTHETIC_LOCAL_GET_7
                | op::SYNTHETIC_LOCAL_SET_0..=op::SYNTHETIC_LOCAL_SET_7
                | op::SYNTHETIC_LOCAL_COPY
                | op::SYNTHETIC_BR_NOSTACK
                | op::SYNTHETIC_BR_IF_NOSTACK
                | op::SYNTHETIC_CALL_00..=op::SYNTHETIC_CALL_31
                | op::SYNTHETIC_I32_ADD2LOCAL..=op::SYNTHETIC_I32_ANDCONSTLOCAL
                | op::SYNTHETIC_I32_STORELOCAL
                | op::SYNTHETIC_LOCAL_SETI32_CONST
                | op::SYNTHETIC_CALL_WITH_RECORD_0
                | op::SYNTHETIC_CALL_WITH_RECORD_1
                | op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_0
                | op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_1
                | op::SYNTHETIC_ARGUMENT_GET
                | op::SYNTHETIC_ARGUMENT_SET
                | op::SYNTHETIC_ARGUMENT_TEE
                | op::SYNTHETIC_I32_SUB2LOCAL..=op::SYNTHETIC_I32_SHRS2LOCAL
                | op::SYNTHETIC_I64_ADD2LOCAL..=op::SYNTHETIC_LOCAL_SETI64_CONST
                | op::SYNTHETIC_BR_TABLE_CONT
                | op::SYNTHETIC_TIER_UP
        )
    }
}
