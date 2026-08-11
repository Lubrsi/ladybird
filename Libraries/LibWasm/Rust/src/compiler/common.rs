/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::CraneliftRelocation;
use crate::CraneliftRelocationKind;
use crate::CraneliftRelocationTargetKind;
use crate::CraneliftTrap;
use crate::CraneliftUserTrapCode;
use crate::HelperId;
use crate::RuntimeLayout as SerializedRuntimeLayout;

use cranelift_codegen::Context;
use cranelift_codegen::FinalizedRelocTarget;
use cranelift_codegen::binemit::Reloc;
use cranelift_codegen::ir::AbiParam;
use cranelift_codegen::ir::AliasRegionData;
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
use cranelift_codegen::ir::TrapCode;
use cranelift_codegen::ir::Type;
use cranelift_codegen::ir::UserExternalName;
use cranelift_codegen::ir::Value;
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::types;
use cranelift_codegen::isa::CallConv;
use cranelift_codegen::isa::TargetIsa;
use cranelift_frontend::FunctionBuilder;

pub(super) const HELPER_EXTERNAL_NAMESPACE: u32 = 0;
pub(super) const WASM_FUNCTION_EXTERNAL_NAMESPACE: u32 = 1;
pub(super) const NATIVE_CODE_ALIGNMENT: usize = 16;
pub(super) const I32_KIND: u8 = 0;
pub(super) const I64_KIND: u8 = 1;
pub(super) const F32_KIND: u8 = 2;
pub(super) const F64_KIND: u8 = 3;
const NO_FALLBACK_OFFSET: u32 = u32::MAX;

pub(super) struct CompiledCodeParts {
    pub(super) code: Vec<u8>,
    pub(super) relocs: Vec<CraneliftRelocation>,
    pub(super) traps: Vec<CraneliftTrap>,
}

#[derive(Clone, Copy)]
pub(super) struct WasmMemoryFlags {
    pub(super) activation: MemFlags,
    pub(super) configuration: MemFlags,
    pub(super) globals: MemFlags,
    pub(super) linear_memory: MemFlags,
    pub(super) readonly_runtime_metadata: MemFlags,
    pub(super) runtime_metadata: MemFlags,
    pub(super) tables: MemFlags,
}

impl WasmMemoryFlags {
    pub(super) fn new(function: &mut Function) -> Self {
        // Regions describe disjoint runtime storage, not module indices. Keep all memories,
        // tables, and globals in broad regions because imported instances may be aliased.
        let mut insert_region = |user_id, description: &'static str| {
            function.dfg.alias_regions.insert(AliasRegionData {
                user_id,
                description: description.into(),
            })
        };
        let activation = insert_region(0, "Wasm activation values");
        let configuration = insert_region(1, "Wasm Configuration fields");
        let globals = insert_region(2, "Wasm global state");
        let linear_memory = insert_region(3, "Wasm linear memory");
        let runtime_metadata = insert_region(4, "Wasm runtime metadata");
        let tables = insert_region(5, "Wasm table state");

        Self {
            activation: MemFlags::trusted().with_alias_region(Some(activation)),
            configuration: MemFlags::trusted().with_alias_region(Some(configuration)),
            globals: MemFlags::trusted().with_alias_region(Some(globals)),
            linear_memory: MemFlags::new().with_alias_region(Some(linear_memory)),
            // CallableMetadata and canonical type-table entries are initialized before execution
            // and remain immutable. Other runtime metadata includes fields published while code
            // is running and must not use these flags.
            readonly_runtime_metadata: MemFlags::trusted()
                .with_alias_region(Some(runtime_metadata))
                .with_readonly(),
            runtime_metadata: MemFlags::trusted().with_alias_region(Some(runtime_metadata)),
            tables: MemFlags::trusted().with_alias_region(Some(tables)),
        }
    }
}

pub(super) struct RuntimeLayout {
    pub(super) regs_offset: i32,
    pub(super) value_size: i32,
    pub(super) locals_base_offset: i32,
    pub(super) table_instances_offset: i32,
    pub(super) memory_instances_offset: i32,
    pub(super) global_instances_offset: i32,
    pub(super) global_instance_value_offset: i32,
    pub(super) memory_instance_data_offset: i32,
    pub(super) memory_buffer_storage_offset_offset: i32,
    pub(super) compiled_call_result_scratch_offset: i32,
    pub(super) value_stack_base_offset: i32,
    pub(super) value_stack_top_offset: i32,
    pub(super) call_record_base_offset: i32,
    pub(super) call_record_stack_top_offset: i32,
    pub(super) depth_offset: i32,
    pub(super) current_compiled_fn_table_data_offset: i32,
    pub(super) current_module_offset: i32,
    pub(super) current_canonical_types_offset: i32,
    pub(super) current_expression_offset: i32,
    pub(super) compiled_function_entry_size: i64,
    pub(super) compiled_function_entry_expression_offset: i32,
    pub(super) table_instance_size_offset: i32,
    pub(super) table_instance_callables_offset: i32,
    pub(super) callable_defined_type_offset: i32,
    pub(super) callable_module_offset: i32,
    pub(super) callable_compiled_instructions_offset: i32,
    pub(super) compiled_instructions_native_entry_offset: i32,
    pub(super) compiled_instructions_direct_native_entry_offset: i32,
}

impl RuntimeLayout {
    pub(super) fn new(layout: &SerializedRuntimeLayout) -> Self {
        Self {
            regs_offset: layout.regs_offset as i32,
            value_size: layout.value_size as i32,
            locals_base_offset: layout.locals_base_offset as i32,
            table_instances_offset: layout.table_instances_offset as i32,
            memory_instances_offset: layout.memory_instances_offset as i32,
            global_instances_offset: layout.global_instances_offset as i32,
            global_instance_value_offset: layout.global_instance_value_offset as i32,
            memory_instance_data_offset: layout.memory_instance_data_offset as i32,
            memory_buffer_storage_offset_offset: layout.memory_buffer_storage_offset_offset as i32,
            compiled_call_result_scratch_offset: layout.compiled_call_result_scratch_offset as i32,
            value_stack_base_offset: layout.value_stack_base_offset as i32,
            value_stack_top_offset: layout.value_stack_top_offset as i32,
            call_record_base_offset: layout.call_record_base_offset as i32,
            call_record_stack_top_offset: layout.call_record_stack_top_offset as i32,
            depth_offset: layout.depth_offset as i32,
            current_compiled_fn_table_data_offset: layout.current_compiled_fn_table_data_offset as i32,
            current_module_offset: layout.current_module_offset as i32,
            current_canonical_types_offset: layout.current_canonical_types_offset as i32,
            current_expression_offset: layout.current_expression_offset as i32,
            compiled_function_entry_size: i64::from(layout.compiled_function_entry_size),
            compiled_function_entry_expression_offset: layout.compiled_function_entry_expression_offset as i32,
            table_instance_size_offset: layout.table_instance_size_offset as i32,
            table_instance_callables_offset: layout.table_instance_callables_offset as i32,
            callable_defined_type_offset: layout.callable_defined_type_offset as i32,
            callable_module_offset: layout.callable_module_offset as i32,
            callable_compiled_instructions_offset: layout.callable_compiled_instructions_offset as i32,
            compiled_instructions_native_entry_offset: layout.compiled_instructions_native_entry_offset as i32,
            compiled_instructions_direct_native_entry_offset: layout.compiled_instructions_direct_native_entry_offset
                as i32,
        }
    }
}

#[derive(Clone, Copy)]
pub(super) struct NativeIndirectCallLayout {
    pub(super) table_instances: i32,
    pub(super) current_module: i32,
    pub(super) current_canonical_types: i32,
    pub(super) table_instance_size: i32,
    pub(super) table_instance_callables: i32,
    pub(super) callable_defined_type: i32,
    pub(super) callable_module: i32,
    pub(super) callable_compiled_instructions: i32,
    pub(super) compiled_instructions_native_entry: i32,
}

pub(super) struct NativeIndirectCallTarget {
    pub(super) native_call: Block,
    pub(super) fallback_call: Block,
}

#[derive(Clone, Copy)]
pub(super) struct NativeIndirectCallContext {
    pub(super) pointer_type: Type,
    pub(super) check_type_signature: SigRef,
    pub(super) check_type_helper: FuncRef,
    pub(super) layout: NativeIndirectCallLayout,
    pub(super) memory_flags: WasmMemoryFlags,
}

pub(super) fn emit_native_indirect_call_target(
    builder: &mut FunctionBuilder<'_>,
    table_index: u32,
    type_index: u32,
    configuration: Value,
    element_index: Value,
    context: NativeIndirectCallContext,
) -> Result<NativeIndirectCallTarget, &'static str> {
    let pointer_type = context.pointer_type;
    let layout = context.layout;
    let memory_flags = context.memory_flags;
    let exact_type = builder.create_block();
    let subtype_check = builder.create_block();
    let native_entry_check = builder.create_block();
    let native_call = builder.create_block();
    let fallback_call = builder.create_block();
    builder.set_cold_block(subtype_check);
    builder.set_cold_block(fallback_call);
    builder.append_block_param(native_call, pointer_type);

    let table_instances = builder.ins().load(
        pointer_type,
        memory_flags.configuration,
        configuration,
        layout.table_instances,
    );
    let table_offset = i64::from(table_index)
        .checked_mul(i64::from(pointer_type.bytes()))
        .ok_or("table index offset overflow")?;
    let table_offset = builder.ins().iconst(pointer_type, table_offset);
    let table_address = builder.ins().iadd(table_instances, table_offset);
    let table = builder.ins().load(pointer_type, memory_flags.tables, table_address, 0);
    let table_size = builder
        .ins()
        .load(pointer_type, memory_flags.tables, table, layout.table_instance_size);
    let table_size = if pointer_type == types::I64 {
        table_size
    } else {
        builder.ins().uextend(types::I64, table_size)
    };
    let in_bounds = builder.ins().icmp(IntCC::UnsignedLessThan, element_index, table_size);
    builder
        .ins()
        .trapz(in_bounds, user_trap_code(CraneliftUserTrapCode::TableOutOfBounds));
    let callables = builder.ins().load(
        pointer_type,
        memory_flags.tables,
        table,
        layout.table_instance_callables,
    );
    let element_offset = if pointer_type == types::I64 {
        builder.ins().imul_imm_s(element_index, i64::from(pointer_type.bytes()))
    } else {
        let element_index = builder.ins().ireduce(types::I32, element_index);
        builder.ins().imul_imm_s(element_index, i64::from(pointer_type.bytes()))
    };
    let callable_address = builder.ins().iadd(callables, element_offset);
    let callable = builder
        .ins()
        .load(pointer_type, memory_flags.tables, callable_address, 0);
    let is_callable = builder.ins().icmp_imm_s(IntCC::NotEqual, callable, 0);
    builder
        .ins()
        .trapz(is_callable, user_trap_code(CraneliftUserTrapCode::IndirectCallNull));
    let actual_type = builder.ins().load(
        pointer_type,
        memory_flags.readonly_runtime_metadata,
        callable,
        layout.callable_defined_type,
    );
    let canonical_types = builder.ins().load(
        pointer_type,
        memory_flags.configuration,
        configuration,
        layout.current_canonical_types,
    );
    let type_offset = i64::from(type_index)
        .checked_mul(i64::from(pointer_type.bytes()))
        .ok_or("type index offset overflow")?;
    let type_offset = builder.ins().iconst(pointer_type, type_offset);
    let expected_type_address = builder.ins().iadd(canonical_types, type_offset);
    let expected_type = builder.ins().load(
        pointer_type,
        memory_flags.readonly_runtime_metadata,
        expected_type_address,
        0,
    );
    let is_exact_type = builder.ins().icmp(IntCC::Equal, actual_type, expected_type);
    builder.ins().brif(is_exact_type, exact_type, &[], subtype_check, &[]);

    builder.switch_to_block(subtype_check);
    builder.seal_block(subtype_check);
    let type_check = builder.ins().func_addr(pointer_type, context.check_type_helper);
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
        pointer_type,
        memory_flags.readonly_runtime_metadata,
        callable,
        layout.callable_module,
    );
    let current_module = builder.ins().load(
        pointer_type,
        memory_flags.configuration,
        configuration,
        layout.current_module,
    );
    let is_same_module = builder.ins().icmp(IntCC::Equal, callable_module, current_module);
    builder
        .ins()
        .brif(is_same_module, native_entry_check, &[], fallback_call, &[]);

    builder.switch_to_block(native_entry_check);
    builder.seal_block(native_entry_check);
    let compiled_instructions = builder.ins().load(
        pointer_type,
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
        .atomic_load(pointer_type, memory_flags.runtime_metadata, native_entry_address);
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

pub(super) fn declare_helper(builder: &mut FunctionBuilder<'_>, signature: SigRef, id: HelperId) -> FuncRef {
    let user_ref = builder.func.declare_imported_user_function(UserExternalName {
        namespace: HELPER_EXTERNAL_NAMESPACE,
        index: id as u32,
    });
    builder.func.import_function(ExtFuncData {
        name: ExternalName::user(user_ref),
        signature,
        colocated: false,
        patchable: false,
    })
}

// The allocated-bytecode frontend imports this complete set in a stable order. The direct
// frontend should import only the helpers its generated body actually uses.
pub(super) struct LegacyImportedHelpers {
    pub(super) call_function_signature: SigRef,
    pub(super) call_indirect_signature: SigRef,
    pub(super) memory_copy_signature: SigRef,
    pub(super) memory_fill_signature: SigRef,
    pub(super) memory_size_signature: SigRef,
    pub(super) memory_grow_signature: SigRef,
    pub(super) stack_exhaustion_signature: SigRef,
    pub(super) check_indirect_type_signature: SigRef,
    pub(super) raise_trap_signature: SigRef,
    pub(super) call_function: FuncRef,
    pub(super) memory_size: FuncRef,
    pub(super) memory_grow: FuncRef,
    pub(super) call_indirect: FuncRef,
    pub(super) call_indirect_with_record: FuncRef,
    pub(super) memory_copy: FuncRef,
    pub(super) memory_fill: FuncRef,
    pub(super) primitive_storage_cage_base: FuncRef,
    pub(super) stack_exhaustion: FuncRef,
    pub(super) raise_trap: FuncRef,
    pub(super) check_indirect_type: FuncRef,
}

impl LegacyImportedHelpers {
    pub(super) fn new(builder: &mut FunctionBuilder<'_>, pointer_type: Type, host_cc: CallConv) -> Self {
        macro_rules! signature {
            (@type ptr) => { pointer_type };
            (@type i32) => { types::I32 };
            (@type i64) => { types::I64 };
            (@define $name:ident : void fn($($parameter:ident),*)) => {
                let $name = {
                    let mut signature = Signature::new(host_cc);
                    $(signature.params.push(AbiParam::new(signature!(@type $parameter)));)*
                    builder.import_signature(signature)
                };
            };
            (@define $name:ident : $result:ident fn($($parameter:ident),*)) => {
                let $name = {
                    let mut signature = Signature::new(host_cc);
                    $(signature.params.push(AbiParam::new(signature!(@type $parameter)));)*
                    signature.returns.push(AbiParam::new(signature!(@type $result)));
                    builder.import_signature(signature)
                };
            };
            ($($name:ident : $result:ident fn($($parameter:ident),*);)*) => {
                $(signature!(@define $name : $result fn($($parameter),*));)*
            };
        }

        signature! {
            call_fn_sig:       i32 fn(ptr, ptr, i32);
            call_indirect_sig: i32 fn(ptr, ptr, i32, i32, i64);
            memory_copy_sig:   i32 fn(ptr, ptr, i32, i32, i32, i32, i32);
            memory_fill_sig:   i32 fn(ptr, ptr, i32, i32, i32, i32);
            cage_base_sig:     i64 fn();
            mem_size_sig:      i64 fn(ptr, i32);
            mem_grow_sig:      i32 fn(ptr, i32, i32);
            stack_exhaustion_sig: void fn(ptr);
            check_indirect_type_sig: i32 fn(ptr, ptr);
        }
        let raise_trap_sig = builder.import_signature(Signature::new(host_cc));

        let h_call_fn = declare_helper(builder, call_fn_sig, HelperId::call_function);
        let h_mem_size = declare_helper(builder, mem_size_sig, HelperId::memory_size);
        let h_mem_grow = declare_helper(builder, mem_grow_sig, HelperId::memory_grow);
        let h_call_indirect = declare_helper(builder, call_indirect_sig, HelperId::call_indirect);
        let h_call_indirect_wr = declare_helper(builder, call_indirect_sig, HelperId::call_indirect_with_record);
        let h_memory_copy = declare_helper(builder, memory_copy_sig, HelperId::memory_copy);
        let h_memory_fill = declare_helper(builder, memory_fill_sig, HelperId::memory_fill);
        let h_primitive_storage_cage_base =
            declare_helper(builder, cage_base_sig, HelperId::primitive_storage_cage_base);
        let h_stack_exhaustion = declare_helper(builder, stack_exhaustion_sig, HelperId::stack_exhaustion);
        let h_raise_trap = declare_helper(builder, raise_trap_sig, HelperId::raise_trap);
        let h_check_indirect_type = declare_helper(builder, check_indirect_type_sig, HelperId::check_indirect_type);

        Self {
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
        }
    }
}

pub(super) fn wasm_abi_type(kind: u8) -> Result<Type, &'static str> {
    match kind {
        I32_KIND => Ok(types::I32),
        I64_KIND => Ok(types::I64),
        F32_KIND => Ok(types::F32),
        F64_KIND => Ok(types::F64),
        _ => Err("unsupported native Wasm ABI type"),
    }
}

pub(super) fn interpreter_handler_signature(isa: &dyn TargetIsa) -> Signature {
    // void fn(void* interpreter, void* configuration, void* instruction,
    //     u32 short_ip, void* dispatch, void* addresses)
    let pointer_type = isa.pointer_type();
    let mut signature = Signature::new(isa.default_call_conv());
    signature.params.push(AbiParam::new(pointer_type));
    signature.params.push(AbiParam::new(pointer_type));
    signature.params.push(AbiParam::new(pointer_type));
    signature.params.push(AbiParam::new(types::I32));
    signature.params.push(AbiParam::new(pointer_type));
    signature.params.push(AbiParam::new(pointer_type));
    signature
}

pub(super) fn value_to_payload(
    builder: &mut FunctionBuilder<'_>,
    value: Value,
    kind: u8,
) -> Result<Value, &'static str> {
    match kind {
        I32_KIND => Ok(builder.ins().sextend(types::I64, value)),
        I64_KIND => Ok(value),
        F32_KIND => {
            let bits = builder.ins().bitcast(types::I32, MemFlags::new(), value);
            Ok(builder.ins().sextend(types::I64, bits))
        }
        F64_KIND => Ok(builder.ins().bitcast(types::I64, MemFlags::new(), value)),
        _ => Err("unsupported native Wasm ABI type"),
    }
}

pub(super) fn payload_to_value(
    builder: &mut FunctionBuilder<'_>,
    payload: Value,
    kind: u8,
) -> Result<Value, &'static str> {
    match kind {
        I32_KIND => Ok(builder.ins().ireduce(types::I32, payload)),
        I64_KIND => Ok(payload),
        F32_KIND => {
            let bits = builder.ins().ireduce(types::I32, payload);
            Ok(builder.ins().bitcast(types::F32, MemFlags::new(), bits))
        }
        F64_KIND => Ok(builder.ins().bitcast(types::F64, MemFlags::new(), payload)),
        _ => Err("unsupported native Wasm ABI type"),
    }
}

pub(super) fn user_trap_code(code: CraneliftUserTrapCode) -> TrapCode {
    TrapCode::unwrap_user(code as u8)
}

fn serialize_relocation(
    kind: Reloc,
    code_offset: u32,
    addend: i64,
    target: &UserExternalName,
) -> Result<CraneliftRelocation, &'static str> {
    let kind = match kind {
        Reloc::Abs8 => CraneliftRelocationKind::Abs8,
        Reloc::Arm64Call => CraneliftRelocationKind::Arm64Call,
        Reloc::X86CallPCRel4 => CraneliftRelocationKind::X86CallPCRel4,
        _ => return Err("unsupported relocation kind"),
    };
    let target_kind = match target.namespace {
        HELPER_EXTERNAL_NAMESPACE => {
            if target.index >= crate::HELPER_COUNT {
                return Err("relocation refers to an unknown helper id");
            }
            if kind != CraneliftRelocationKind::Abs8 {
                return Err("helper relocation is not Abs8");
            }
            CraneliftRelocationTargetKind::Helper
        }
        WASM_FUNCTION_EXTERNAL_NAMESPACE => CraneliftRelocationTargetKind::WasmFunction,
        _ => return Err("relocation refers to an unknown target namespace"),
    };
    Ok(CraneliftRelocation {
        code_offset,
        kind,
        target_kind,
        target_index: target.index,
        fallback_offset: NO_FALLBACK_OFFSET,
        _padding: 0,
        addend,
    })
}

pub(super) fn compile_function(isa: &dyn TargetIsa, function: Function) -> Result<CompiledCodeParts, &'static str> {
    let mut context = Context::for_function(function);
    let (code, raw_relocations, traps) = {
        let compiled_code = context
            .compile(isa, &mut Default::default())
            .map_err(|_| "cranelift compilation failed")?;
        let traps = compiled_code
            .buffer
            .traps()
            .iter()
            .map(|trap| CraneliftTrap {
                offset: trap.offset,
                code: trap.code.as_raw().get(),
                _padding: [0; 3],
            })
            .collect();
        (
            compiled_code.code_buffer().to_vec(),
            compiled_code.buffer.relocs().to_vec(),
            traps,
        )
    };
    let mut relocs = Vec::with_capacity(raw_relocations.len());
    let user_names = context.func.params.user_named_funcs();
    for relocation in &raw_relocations {
        let name = match &relocation.target {
            FinalizedRelocTarget::ExternalName(ExternalName::User(user_ref)) => &user_names[*user_ref],
            _ => return Err("unexpected relocation target"),
        };
        relocs.push(serialize_relocation(
            relocation.kind,
            relocation.offset,
            relocation.addend,
            name,
        )?);
    }

    Ok(CompiledCodeParts { code, relocs, traps })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::HelperId;

    #[test]
    fn serializes_helper_and_wasm_function_relocations() {
        let helper = UserExternalName {
            namespace: HELPER_EXTERNAL_NAMESPACE,
            index: HelperId::memory_size as u32,
        };
        let helper_relocation = serialize_relocation(Reloc::Abs8, 12, -4, &helper).unwrap();
        assert_eq!(helper_relocation.code_offset, 12);
        assert_eq!(helper_relocation.kind, CraneliftRelocationKind::Abs8);
        assert_eq!(helper_relocation.target_kind, CraneliftRelocationTargetKind::Helper);
        assert_eq!(helper_relocation.target_index, HelperId::memory_size as u32);
        assert_eq!(helper_relocation.addend, -4);

        let wasm_function = UserExternalName {
            namespace: WASM_FUNCTION_EXTERNAL_NAMESPACE,
            index: 42,
        };
        let direct_call = serialize_relocation(Reloc::Arm64Call, 24, 0, &wasm_function).unwrap();
        assert_eq!(direct_call.kind, CraneliftRelocationKind::Arm64Call);
        assert_eq!(direct_call.target_kind, CraneliftRelocationTargetKind::WasmFunction);
        assert_eq!(direct_call.target_index, 42);

        let x86_direct_call = serialize_relocation(Reloc::X86CallPCRel4, 32, -4, &wasm_function).unwrap();
        assert_eq!(x86_direct_call.kind, CraneliftRelocationKind::X86CallPCRel4);
        assert_eq!(x86_direct_call.addend, -4);

        assert!(serialize_relocation(Reloc::Arm64Call, 0, 0, &helper).is_err());
        assert!(
            serialize_relocation(
                Reloc::Abs8,
                0,
                0,
                &UserExternalName {
                    namespace: HELPER_EXTERNAL_NAMESPACE,
                    index: crate::HELPER_COUNT,
                },
            )
            .is_err()
        );
        assert!(serialize_relocation(Reloc::Abs8, 0, 0, &UserExternalName { namespace: 2, index: 0 },).is_err());
    }
}
