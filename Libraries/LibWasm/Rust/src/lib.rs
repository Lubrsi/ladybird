/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod compiler;

use compiler::CraneliftCompiler;

/// Immediates:
///   constants:    imm1 = value (i32 sign-extended, i64, or f32/f64 bits)
///   local ops:    imm1 = local index
///   global ops:   imm1 = global index
///   branch:       imm1 = label index (from control stack)
///   block/loop:   imm1 = end_ip, imm2 = else_ip (-1 if none), imm3 = arity | (param_count << 16)
///   call:         imm1 = function index, imm3 = parameter count, call_result_count = result count
///   call_indirect: imm1 = type index, imm2 = table index, imm3 = parameter count, call_result_count = result count
///   memory ops:   imm1 = offset, imm3 = memory index
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CraneliftInsn {
    pub opcode: u64,
    pub sources: [u8; 3],
    pub destination: u8,
    pub imm1: i64,
    pub imm2: i64,
    pub imm3: u32,
    pub call_result_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RuntimeHelpers {
    // i32 fn(interp, config, func_index); returns 1 on trap
    pub call_function: usize,
    // void fn(interp, msg_ptr, msg_len)
    pub set_trap: usize,
    // i64 fn(config, mem_idx); returns size in pages
    pub memory_size: usize,
    // i32 fn(config, mem_idx, pages); returns old size or -1
    pub memory_grow: usize,
    // i32 fn(interp, config, func_index); call using call record args
    pub call_with_record: usize,
    // i32 fn(interp, config, func_index, ...args); direct call via compiled function table
    pub direct_call_0: usize,
    pub direct_call_1: usize,
    pub direct_call_2: usize,
    pub direct_call_3: usize,
    // i32 fn(interp, config, table_idx, type_idx, element_index)
    pub call_indirect: usize,
    // i32 fn(interp, config, dst_mem, src_mem, dst, src, count)
    pub memory_copy: usize,
    // i32 fn(interp, config, mem_idx, offset, value, count)
    pub memory_fill: usize,
    // Address of the process-global primitive storage cage base.
    pub primitive_storage_cage_base: usize,
    // i32 fn(interp, config, table_idx, type_idx, element_index); call using call record args
    pub call_indirect_with_record: usize,
    // void fn(interp); set the standard Wasm stack-exhaustion trap
    pub stack_exhaustion: usize,

    pub regs_offset: u32,
    pub value_size: u32,
    pub locals_base_offset: u32,
    pub memory_instances_offset: u32,
    pub global_instances_offset: u32,
    pub global_instance_value_offset: u32,
    pub memory_instance_data_offset: u32,
    pub memory_buffer_storage_offset_offset: u32,
    pub compiled_call_result_scratch_offset: u32,
    pub value_stack_base_offset: u32,
    pub value_stack_top_offset: u32,
    pub call_record_base_offset: u32,
    pub call_record_stack_top_offset: u32,
    pub depth_offset: u32,
    pub current_compiled_fn_table_data_offset: u32,
    pub current_expression_offset: u32,
    pub compiled_function_entry_size: u32,
    pub compiled_function_entry_expression_offset: u32,
}

/// Stable index assigned to each runtime helper. Embedded in cranelift `ExternalName`
/// entries so we can recover, post-codegen, which helper a given relocation targets.
/// The numeric values are part of the cache blob format -- do NOT reorder.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(non_camel_case_types)]
pub enum HelperId {
    call_function = 0,
    set_trap = 1,
    memory_size = 2,
    memory_grow = 3,
    call_with_record = 4,
    direct_call_0 = 5,
    direct_call_1 = 6,
    direct_call_2 = 7,
    direct_call_3 = 8,
    call_indirect = 9,
    memory_copy = 10,
    memory_fill = 11,
    primitive_storage_cage_base = 12,
    call_indirect_with_record = 13,
    stack_exhaustion = 14,
}

pub const HELPER_COUNT: u32 = 15;

/// Relocation kinds emitted for runtime helpers and direct calls between compiled Wasm functions.
/// The numeric values are part of the cache blob format.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CraneliftRelocationKind {
    Abs8 = 0,
    Arm64Call = 1,
    X86CallPCRel4 = 2,
}

/// The namespace of a relocation target. The numeric values are part of the cache blob format.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CraneliftRelocationTargetKind {
    Helper = 0,
    WasmFunction = 1,
}

/// One relocation slot in the generated machine code. Helper relocations are resolved to
/// current-process addresses. Wasm-function relocations are resolved by module function index
/// after native addresses have been assigned.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CraneliftRelocation {
    pub code_offset: u32,
    pub kind: CraneliftRelocationKind,
    pub target_kind: CraneliftRelocationTargetKind,
    pub target_index: u32,
    pub addend: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CraneliftTrap {
    pub offset: u32,
    pub code: u8,
    pub _padding: [u8; 3],
}

/// Output of `compile_to_bytes`: the machine code plus the patch table.
pub struct CompiledFunction {
    pub code: Vec<u8>,
    pub native_entry_offset: u32,
    pub relocs: Vec<CraneliftRelocation>,
    pub traps: Vec<CraneliftTrap>,
}

#[derive(Clone, Copy, Debug)]
pub struct FunctionCompilationOptions {
    pub outcome_return_value: u64,
    pub result_arity: u32,
    pub num_locals: u32,
    pub num_params: u32,
    pub function_index: u32,
    pub max_call_rec_size: u32,
}

pub fn compile_to_bytes(
    insns: &[CraneliftInsn],
    helpers: &RuntimeHelpers,
    options: FunctionCompilationOptions,
    local_types: &[u8],
) -> Result<CompiledFunction, &'static str> {
    CraneliftCompiler::compile_to_bytes(insns, helpers, options, local_types)
}
