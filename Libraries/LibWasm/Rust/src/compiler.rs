/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::CompiledFunction;
use crate::CraneliftInsn;
use crate::CraneliftRelocation;
use crate::CraneliftRelocationKind;
use crate::CraneliftRelocationTargetKind;
use crate::CraneliftTrap;
use crate::FunctionCompilationOptions;
use crate::HelperId;
use crate::RuntimeHelpers;

use cranelift_codegen::Context;
use cranelift_codegen::FinalizedRelocTarget;
use cranelift_codegen::binemit::Reloc;
use cranelift_codegen::ir::AbiParam;
use cranelift_codegen::ir::Block;
use cranelift_codegen::ir::ExtFuncData;
use cranelift_codegen::ir::ExternalName;
use cranelift_codegen::ir::Function;
use cranelift_codegen::ir::InstBuilder;
use cranelift_codegen::ir::MemFlags;
use cranelift_codegen::ir::Signature;
use cranelift_codegen::ir::StackSlotData;
use cranelift_codegen::ir::StackSlotKind;
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

// Opcode constants generated from Opcode.h (see build.rs.)
#[allow(dead_code)]
mod op {
    include!(concat!(env!("OUT_DIR"), "/opcodes.rs"));
}

const REG_COUNT: usize = 8;
const STACK_MARKER: u8 = 8;
const CALLREC_BASE: u8 = 9;
const HELPER_EXTERNAL_NAMESPACE: u32 = 0;
const WASM_FUNCTION_EXTERNAL_NAMESPACE: u32 = 1;

struct CompiledCodeParts {
    code: Vec<u8>,
    relocs: Vec<CraneliftRelocation>,
    traps: Vec<CraneliftTrap>,
}

/// The `Int` bank is always defined.
/// The `F64` bank is trusted only until the next control-flow merge, where it may be undefined on an incoming edge.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Bank {
    Int,
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
    /// Real value-stack size at block entry, minus this block's param count.
    /// Only meaningful (and only set) when vstack is disabled (max_stack_depth == 0).
    entry_real_depth_var: Option<Variable>,
    bank_snapshot: Option<([Bank; REG_COUNT], Vec<Bank>)>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ControlKind {
    Block,
    Loop,
    If,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct LocalSet {
    words: Vec<u64>,
}

impl LocalSet {
    fn new(local_count: usize) -> Self {
        Self {
            words: vec![0; local_count.div_ceil(u64::BITS as usize)],
        }
    }

    fn contains(&self, local_index: usize) -> bool {
        let Some(word) = self.words.get(local_index / u64::BITS as usize) else {
            return false;
        };
        word & (1 << (local_index % u64::BITS as usize)) != 0
    }

    fn insert(&mut self, local_index: usize) {
        let Some(word) = self.words.get_mut(local_index / u64::BITS as usize) else {
            return;
        };
        *word |= 1 << (local_index % u64::BITS as usize);
    }

    fn union_with(&mut self, other: &Self) {
        for (word, other_word) in self.words.iter_mut().zip(&other.words) {
            *word |= other_word;
        }
    }

    fn union_without(&mut self, included: &Self, excluded: &Self) {
        for ((word, included_word), excluded_word) in self.words.iter_mut().zip(&included.words).zip(&excluded.words) {
            *word |= included_word & !excluded_word;
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct LocalAccesses {
    reads: [Option<usize>; 2],
    write: Option<usize>,
}

struct LocalLivenessBlock {
    start: usize,
    end: usize,
    is_loop_header: bool,
    has_branch_table_successors: bool,
    predecessors: Vec<usize>,
    successors: Vec<usize>,
    uses: LocalSet,
    definitions: LocalSet,
    live_in: LocalSet,
    live_out: LocalSet,
}

struct LocalLiveness {
    blocks: Vec<LocalLivenessBlock>,
    instruction_blocks: Vec<usize>,
}

impl LocalLiveness {
    fn block_at(&self, instruction_index: usize) -> &LocalLivenessBlock {
        &self.blocks[self.instruction_blocks[instruction_index]]
    }

    fn block_index_at(&self, instruction_index: usize) -> usize {
        self.instruction_blocks[instruction_index]
    }
}

pub struct CraneliftCompiler;

impl CraneliftCompiler {
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
            addend,
        })
    }

    fn compile_function(isa: &dyn TargetIsa, function: Function) -> Result<CompiledCodeParts, &'static str> {
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
            relocs.push(Self::serialize_relocation(
                relocation.kind,
                relocation.offset,
                relocation.addend,
                name,
            )?);
        }

        Ok(CompiledCodeParts { code, relocs, traps })
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

    fn analyze_local_liveness(insns: &[CraneliftInsn], num_locals: usize) -> Result<LocalLiveness, &'static str> {
        if insns.is_empty() {
            return Ok(LocalLiveness {
                blocks: Vec::new(),
                instruction_blocks: Vec::new(),
            });
        }

        let mut matching_else = vec![None; insns.len()];
        let mut matching_end = vec![None; insns.len()];
        let mut control_stack = Vec::new();
        for (instruction_index, insn) in insns.iter().enumerate() {
            match insn.opcode {
                op::BLOCK | op::LOOP | op::IF => control_stack.push(instruction_index),
                op::ELSE => {
                    let Some(&start) = control_stack.last() else {
                        return Err("else without matching if");
                    };
                    if insns[start].opcode != op::IF || matching_else[start].is_some() {
                        return Err("else without matching if");
                    }
                    matching_else[start] = Some(instruction_index);
                }
                op::END => {
                    if let Some(start) = control_stack.pop() {
                        matching_end[start] = Some(instruction_index);
                    }
                }
                _ => {}
            }
        }
        if !control_stack.is_empty() {
            return Err("unterminated structured control instruction");
        }

        let branch_target = |label_index: usize, active_controls: &[usize]| -> Result<Option<usize>, &'static str> {
            if label_index >= active_controls.len() {
                return Ok(None);
            }
            let control_start = active_controls[active_controls.len() - 1 - label_index];
            let target = if insns[control_start].opcode == op::LOOP {
                control_start + 1
            } else {
                matching_end[control_start].ok_or("structured control instruction without matching end")? + 1
            };
            Ok((target < insns.len()).then_some(target))
        };

        let mut successors = vec![Vec::new(); insns.len()];
        let mut active_controls = Vec::new();
        for (instruction_index, insn) in insns.iter().enumerate() {
            let fallthrough = (instruction_index + 1 < insns.len()).then_some(instruction_index + 1);
            match insn.opcode {
                op::BLOCK | op::LOOP => {
                    if let Some(fallthrough) = fallthrough {
                        successors[instruction_index].push(fallthrough);
                    }
                    active_controls.push(instruction_index);
                }
                op::IF => {
                    if let Some(then_target) = fallthrough {
                        successors[instruction_index].push(then_target);
                    }
                    let false_target = if let Some(else_index) = matching_else[instruction_index] {
                        else_index + 1
                    } else {
                        matching_end[instruction_index].ok_or("if without matching end")? + 1
                    };
                    if false_target < insns.len() {
                        successors[instruction_index].push(false_target);
                    }
                    active_controls.push(instruction_index);
                }
                op::ELSE => {
                    let Some(&if_start) = active_controls.last() else {
                        return Err("else without active if");
                    };
                    let after_if = matching_end[if_start].ok_or("if without matching end")? + 1;
                    if after_if < insns.len() {
                        successors[instruction_index].push(after_if);
                    }
                }
                op::END => {
                    if active_controls.pop().is_some()
                        && let Some(fallthrough) = fallthrough
                    {
                        successors[instruction_index].push(fallthrough);
                    }
                }
                op::BR | op::SYNTHETIC_BR_NOSTACK => {
                    let label_index = usize::try_from(insn.imm1).map_err(|_| "invalid branch label")?;
                    if let Some(target) = branch_target(label_index, &active_controls)? {
                        successors[instruction_index].push(target);
                    }
                }
                op::BR_IF | op::SYNTHETIC_BR_IF_NOSTACK => {
                    let label_index = usize::try_from(insn.imm1).map_err(|_| "invalid branch label")?;
                    if let Some(target) = branch_target(label_index, &active_controls)? {
                        successors[instruction_index].push(target);
                    }
                    if let Some(fallthrough) = fallthrough {
                        successors[instruction_index].push(fallthrough);
                    }
                }
                op::BR_TABLE => {
                    let inline_count = (insn.imm3 & 0xff) as usize;
                    if inline_count == 0xff {
                        return Err("br_table too large for inline encoding");
                    }

                    let mut labels = Vec::with_capacity(inline_count + 1);
                    for label_index in 0..inline_count {
                        let packed = if label_index < 4 {
                            insn.imm1 as u64
                        } else {
                            insn.imm2 as u64
                        };
                        labels.push(((packed >> ((label_index % 4) * 16)) & 0xffff) as usize);
                    }
                    let mut continuation_index = instruction_index + 1;
                    while continuation_index < insns.len()
                        && insns[continuation_index].opcode == op::SYNTHETIC_BR_TABLE_CONT
                    {
                        let continuation = &insns[continuation_index];
                        let count = (continuation.imm3 & 0xff) as usize;
                        for label_index in 0..count {
                            let packed = if label_index < 4 {
                                continuation.imm1 as u64
                            } else {
                                continuation.imm2 as u64
                            };
                            labels.push(((packed >> ((label_index % 4) * 16)) & 0xffff) as usize);
                        }
                        continuation_index += 1;
                    }
                    labels.push(((insn.imm3 >> 8) & 0xffff) as usize);

                    for label_index in labels {
                        if let Some(target) = branch_target(label_index, &active_controls)?
                            && !successors[instruction_index].contains(&target)
                        {
                            successors[instruction_index].push(target);
                        }
                    }
                }
                op::UNREACHABLE | op::RETURN | op::SYNTHETIC_END_EXPRESSION | op::SYNTHETIC_BR_TABLE_CONT => {}
                _ => {
                    if let Some(fallthrough) = fallthrough {
                        successors[instruction_index].push(fallthrough);
                    }
                }
            }
        }

        let mut is_block_start = vec![false; insns.len()];
        is_block_start[0] = true;
        for (instruction_index, instruction_successors) in successors.iter().enumerate() {
            for &successor in instruction_successors {
                if successor != instruction_index + 1 {
                    is_block_start[successor] = true;
                }
            }
            let next_instruction = instruction_index + 1;
            if next_instruction < insns.len()
                && (matches!(insns[instruction_index].opcode, op::LOOP | op::END)
                    || instruction_successors.len() != 1
                    || instruction_successors[0] != next_instruction)
            {
                is_block_start[next_instruction] = true;
            }
        }

        let block_starts: Vec<usize> = is_block_start
            .iter()
            .enumerate()
            .filter_map(|(instruction_index, &is_start)| is_start.then_some(instruction_index))
            .collect();
        let mut instruction_blocks = vec![0; insns.len()];
        let mut blocks = Vec::with_capacity(block_starts.len());
        for (block_index, &start) in block_starts.iter().enumerate() {
            let end = block_starts.get(block_index + 1).copied().unwrap_or(insns.len());
            instruction_blocks[start..end].fill(block_index);
            blocks.push(LocalLivenessBlock {
                start,
                end,
                is_loop_header: start > 0 && insns[start - 1].opcode == op::LOOP,
                has_branch_table_successors: insns[end - 1].opcode == op::BR_TABLE,
                predecessors: Vec::new(),
                successors: Vec::new(),
                uses: LocalSet::new(num_locals),
                definitions: LocalSet::new(num_locals),
                live_in: LocalSet::new(num_locals),
                live_out: LocalSet::new(num_locals),
            });
        }

        for block in &mut blocks {
            for &successor in &successors[block.end - 1] {
                let successor_block = instruction_blocks[successor];
                if !block.successors.contains(&successor_block) {
                    block.successors.push(successor_block);
                }
            }
            for insn in &insns[block.start..block.end] {
                let accesses = Self::local_accesses(insn);
                for local_index in accesses.reads.into_iter().flatten() {
                    if !block.definitions.contains(local_index) {
                        block.uses.insert(local_index);
                    }
                }
                if let Some(local_index) = accesses.write {
                    block.definitions.insert(local_index);
                }
            }
        }
        for predecessor in 0..blocks.len() {
            let block_successors = blocks[predecessor].successors.clone();
            for successor in block_successors {
                blocks[successor].predecessors.push(predecessor);
            }
        }

        loop {
            let previous_live_ins: Vec<LocalSet> = blocks.iter().map(|block| block.live_in.clone()).collect();
            let mut changed = false;
            for block in &mut blocks {
                let mut live_out = LocalSet::new(num_locals);
                for &successor in &block.successors {
                    live_out.union_with(&previous_live_ins[successor]);
                }
                let mut live_in = block.uses.clone();
                live_in.union_without(&live_out, &block.definitions);
                changed |= live_in != block.live_in || live_out != block.live_out;
                block.live_in = live_in;
                block.live_out = live_out;
            }
            if !changed {
                break;
            }
        }

        Ok(LocalLiveness {
            blocks,
            instruction_blocks,
        })
    }

    fn select_locals_for_edge_cache(local_liveness: &LocalLiveness, block_cached_locals: &[bool]) -> Vec<Vec<bool>> {
        let mut incoming_locals = vec![vec![false; block_cached_locals.len()]; local_liveness.blocks.len()];

        for (successor_index, successor) in local_liveness.blocks.iter().enumerate() {
            if successor.predecessors.is_empty()
                || successor.is_loop_header
                || successor
                    .predecessors
                    .iter()
                    .any(|&predecessor_index| predecessor_index >= successor_index)
            {
                continue;
            }

            if successor
                .predecessors
                .iter()
                .any(|&predecessor_index| local_liveness.blocks[predecessor_index].has_branch_table_successors)
            {
                continue;
            }
            for (local_index, &cached) in block_cached_locals.iter().enumerate() {
                incoming_locals[successor_index][local_index] = cached
                    && successor.live_in.contains(local_index)
                    && successor.predecessors.iter().all(|&predecessor_index| {
                        local_liveness.blocks[predecessor_index]
                            .definitions
                            .contains(local_index)
                    });
            }
        }

        incoming_locals
    }

    fn select_locals_for_promotion(insns: &[CraneliftInsn], num_locals: usize, budget: usize) -> Vec<bool> {
        if budget >= num_locals {
            return vec![true; num_locals];
        }

        let mut read_counts = vec![0u32; num_locals];
        let mut write_counts = vec![0u32; num_locals];
        let record_access = |counts: &mut [u32], local_index: usize| {
            let Some(access_count) = counts.get_mut(local_index) else {
                return;
            };
            *access_count = access_count.saturating_add(1);
        };

        for insn in insns {
            let accesses = Self::local_accesses(insn);
            for local_index in accesses.reads.into_iter().flatten() {
                record_access(&mut read_counts, local_index);
            }
            if let Some(local_index) = accesses.write {
                record_access(&mut write_counts, local_index);
            }
        }

        // Each mutation of a promoted local introduces another SSA definition. Promoting every
        // local in functions with many mutations can therefore create enough simultaneous live
        // values at control-flow merges to cause excessive register-allocation work and spilling.
        const MAX_UNCONDITIONAL_LOCAL_SSA_DEFINITIONS: usize = 256;
        let estimated_ssa_definitions =
            num_locals.saturating_add(write_counts.iter().map(|&count| count as usize).sum::<usize>());
        if estimated_ssa_definitions <= MAX_UNCONDITIONAL_LOCAL_SSA_DEFINITIONS {
            return vec![true; num_locals];
        }

        // Prefer locals with fewer definitions, then use read frequency to choose between equally
        // stable locals. Locals outside the budget remain in their canonical frame slots.
        let mut candidates: Vec<usize> = (0..num_locals)
            .filter(|&index| read_counts[index] > 0 || write_counts[index] > 0)
            .collect();
        candidates.sort_unstable_by(|&lhs, &rhs| {
            write_counts[lhs]
                .cmp(&write_counts[rhs])
                .then_with(|| read_counts[rhs].cmp(&read_counts[lhs]))
                .then_with(|| lhs.cmp(&rhs))
        });

        let mut promoted = vec![false; num_locals];
        for index in candidates.into_iter().take(budget) {
            promoted[index] = true;
        }
        promoted
    }

    fn select_locals_for_block_cache(insns: &[CraneliftInsn], promoted: &[bool], budget: usize) -> Vec<bool> {
        let mut read_counts = vec![0u32; promoted.len()];
        let mut write_counts = vec![0u32; promoted.len()];
        for insn in insns {
            let accesses = Self::local_accesses(insn);
            for local_index in accesses.reads.into_iter().flatten() {
                if let Some(count) = read_counts.get_mut(local_index) {
                    *count = count.saturating_add(1);
                }
            }
            if let Some(local_index) = accesses.write
                && let Some(count) = write_counts.get_mut(local_index)
            {
                *count = count.saturating_add(1);
            }
        }

        // Function-wide promotion favors stable locals. Within a block, repeatedly mutated locals
        // are more valuable because intermediate definitions can stay cached without crossing a
        // control-flow edge.
        let mut candidates: Vec<usize> = (0..promoted.len())
            .filter(|&local_index| {
                !promoted[local_index] && (read_counts[local_index] > 0 || write_counts[local_index] > 0)
            })
            .collect();
        candidates.sort_unstable_by(|&lhs, &rhs| {
            write_counts[rhs]
                .cmp(&write_counts[lhs])
                .then_with(|| read_counts[rhs].cmp(&read_counts[lhs]))
                .then_with(|| lhs.cmp(&rhs))
        });

        let mut cached = vec![false; promoted.len()];
        for local_index in candidates.into_iter().take(budget) {
            cached[local_index] = true;
        }
        cached
    }

    pub fn compile_to_bytes(
        insns: &[CraneliftInsn],
        helpers: &RuntimeHelpers,
        options: FunctionCompilationOptions,
        local_types: &[u8],
    ) -> Result<CompiledFunction, &'static str> {
        let FunctionCompilationOptions {
            outcome_return_value,
            result_arity,
            num_locals,
            num_params,
            function_index,
            max_call_rec_size,
        } = options;

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

        let mut flag_builder = settings::builder();
        flag_builder.set("opt_level", "speed").unwrap();
        flag_builder.set("is_pic", "false").unwrap();
        let flags = settings::Flags::new(flag_builder);
        let isa = cranelift_native::builder()
            .map_err(|_| "unsupported host architecture")?
            .finish(flags)
            .map_err(|_| "failed to build ISA")?;

        // Function signature matches handler_ptr:
        //   u64 fn(void* interpreter, void* configuration, void* insn, u32 short_ip, void* cc, void* addrs)
        let ptr_type = isa.pointer_type();
        let host_cc = isa.default_call_conv();
        let mut sig = Signature::new(host_cc);
        sig.params.push(AbiParam::new(ptr_type)); // interpreter
        sig.params.push(AbiParam::new(ptr_type)); // configuration
        sig.params.push(AbiParam::new(ptr_type)); // instruction (unused)
        sig.params.push(AbiParam::new(types::I32)); // short_ip (unused)
        sig.params.push(AbiParam::new(ptr_type)); // cc (unused)
        sig.params.push(AbiParam::new(ptr_type)); // addresses_ptr (unused)
        sig.returns.push(AbiParam::new(types::I64)); // Outcome

        let handler_signature = sig.clone();
        let mut func = Function::with_name_signature(UserFuncName::user(0, function_index), sig);
        let mut builder_ctx = FunctionBuilderContext::new();
        let mut builder = FunctionBuilder::new(&mut func, &mut builder_ctx);

        // Declare variables for virtual registers R0-R7.
        // We store everything as i64 and bitcast for floats.
        let reg_vars: [Variable; REG_COUNT] = std::array::from_fn(|i| Variable::from_u32(i as u32));
        for var in &reg_vars {
            builder.declare_var(*var, types::I64);
        }

        let entry_block = builder.create_block();
        builder.append_block_params_for_function_params(entry_block);
        builder.switch_to_block(entry_block);
        builder.seal_block(entry_block);

        let interpreter_val = builder.block_params(entry_block)[0];
        let configuration_val = builder.block_params(entry_block)[1];

        // Load regs[0..7] from configuration. regs is at offset `regs_offset` from Configuration*.
        // Each Value is `value_size` bytes; the low 8 bytes are the i64 payload.
        let regs_offset = helpers.regs_offset as i32;
        let value_size = helpers.value_size as i32;
        for (i, var) in reg_vars.iter().enumerate() {
            let offset = regs_offset + (i as i32) * value_size;
            let val = builder
                .ins()
                .load(types::I64, MemFlags::trusted(), configuration_val, offset);
            builder.def_var(*var, val);
        }

        let epilogue_block = builder.create_block();
        let trap_block = builder.create_block();

        // Build helper call signatures. We import them as indirect calls via function pointers.
        macro_rules! sig {
            (@ty ptr) => { ptr_type };
            (@ty i32) => { types::I32 };
            (@ty i64) => { types::I64 };
            (@def $name:ident : void fn($($param:ident),*)) => {
                let $name = {
                    let mut s = Signature::new(host_cc);
                    $(s.params.push(AbiParam::new(sig!(@ty $param)));)*
                    builder.import_signature(s)
                };
            };
            (@def $name:ident : $ret:ident fn($($param:ident),*)) => {
                let $name = {
                    let mut s = Signature::new(host_cc);
                    $(s.params.push(AbiParam::new(sig!(@ty $param)));)*
                    s.returns.push(AbiParam::new(sig!(@ty $ret)));
                    builder.import_signature(s)
                };
            };
            ($($name:ident : $ret:ident fn($($param:ident),*);)*) => { $(sig!(@def $name : $ret fn($($param),*));)* }
        }

        sig! {
            call_fn_sig:       i32 fn(ptr, ptr, i32);
            call_fn1_sig:      i32 fn(ptr, ptr, i32, i64);
            call_fn2_sig:      i32 fn(ptr, ptr, i32, i64, i64);
            call_fn3_sig:      i32 fn(ptr, ptr, i32, i64, i64, i64);
            call_indirect_sig: i32 fn(ptr, ptr, i32, i32, i32);
            memory_copy_sig:   i32 fn(ptr, ptr, i32, i32, i32, i32, i32);
            memory_fill_sig:   i32 fn(ptr, ptr, i32, i32, i32, i32);
            cage_base_sig:     i64 fn();
            mem_size_sig:      i64 fn(ptr, i32);
            mem_grow_sig:      i32 fn(ptr, i32, i32);
            set_trap_sig:      void fn(ptr, ptr, i32);
            stack_exhaustion_sig: void fn(ptr);
        }

        // Declare each runtime helper as an imported external function. At every use site
        // we emit `func_addr` which lowers (with is_pic=false) to a load from an inline
        // 8-byte literal marked with a `Reloc::Abs8` relocation -- the cache install path
        // walks these and rewrites the 8 bytes with the current process's helper address.
        macro_rules! decl_helper {
            ($sig:expr, $id:expr) => {{
                let user_ref = builder.func.declare_imported_user_function(UserExternalName {
                    namespace: HELPER_EXTERNAL_NAMESPACE,
                    index: $id as u32,
                });
                builder.func.import_function(ExtFuncData {
                    name: ExternalName::user(user_ref),
                    signature: $sig,
                    colocated: false,
                })
            }};
        }
        let h_call_fn = decl_helper!(call_fn_sig, HelperId::call_function);
        let h_direct_call_0 = decl_helper!(call_fn_sig, HelperId::direct_call_0);
        let h_direct_call_1 = decl_helper!(call_fn1_sig, HelperId::direct_call_1);
        let h_direct_call_2 = decl_helper!(call_fn2_sig, HelperId::direct_call_2);
        let h_direct_call_3 = decl_helper!(call_fn3_sig, HelperId::direct_call_3);
        let h_set_trap = decl_helper!(set_trap_sig, HelperId::set_trap);
        let h_mem_size = decl_helper!(mem_size_sig, HelperId::memory_size);
        let h_mem_grow = decl_helper!(mem_grow_sig, HelperId::memory_grow);
        let h_call_indirect = decl_helper!(call_indirect_sig, HelperId::call_indirect);
        let h_call_indirect_wr = decl_helper!(call_indirect_sig, HelperId::call_indirect_with_record);
        let h_memory_copy = decl_helper!(memory_copy_sig, HelperId::memory_copy);
        let h_memory_fill = decl_helper!(memory_fill_sig, HelperId::memory_fill);
        let h_primitive_storage_cage_base = decl_helper!(cage_base_sig, HelperId::primitive_storage_cage_base);
        let h_stack_exhaustion = decl_helper!(stack_exhaustion_sig, HelperId::stack_exhaustion);
        let direct_call_sig = builder.import_signature(handler_signature.clone());
        let mut direct_call_targets = HashMap::new();
        for insn in insns.iter().filter(|insn| {
            matches!(
                insn.opcode,
                op::SYNTHETIC_CALL_WITH_RECORD_0 | op::SYNTHETIC_CALL_WITH_RECORD_1
            )
        }) {
            let target_index = u32::try_from(insn.imm1).map_err(|_| "invalid direct-call target")?;
            direct_call_targets.entry(target_index).or_insert_with(|| {
                let user_ref = builder.func.declare_imported_user_function(UserExternalName {
                    namespace: WASM_FUNCTION_EXTERNAL_NAMESPACE,
                    index: target_index,
                });
                builder.func.import_function(ExtFuncData {
                    name: ExternalName::user(user_ref),
                    signature: direct_call_sig,
                    colocated: true,
                })
            });
        }
        let locals_base_offset = helpers.locals_base_offset as i32;
        let memory_instances_offset = helpers.memory_instances_offset as i32;
        let global_instances_offset = helpers.global_instances_offset as i32;
        let global_instance_value_offset = helpers.global_instance_value_offset as i32;
        let memory_instance_data_offset = helpers.memory_instance_data_offset as i32;
        let memory_buffer_storage_offset_offset = helpers.memory_buffer_storage_offset_offset as i32;
        let compiled_call_result_scratch_offset = helpers.compiled_call_result_scratch_offset as i32;
        let value_stack_base_offset = helpers.value_stack_base_offset as i32;
        let value_stack_top_offset = helpers.value_stack_top_offset as i32;
        let call_record_base_offset = helpers.call_record_base_offset as i32;
        let call_record_stack_top_offset = helpers.call_record_stack_top_offset as i32;
        let depth_offset = helpers.depth_offset as i32;
        let current_compiled_fn_table_data_offset = helpers.current_compiled_fn_table_data_offset as i32;
        let current_expression_offset = helpers.current_expression_offset as i32;
        let compiled_function_entry_size = i64::from(helpers.compiled_function_entry_size);
        let compiled_function_entry_expression_offset = helpers.compiled_function_entry_expression_offset as i32;
        // Accesses to memory32 are unchecked and may fault; the fault handler turns
        // faults inside a memory's guarded reservation into wasm traps.
        let wasm_memory_flags = MemFlags::new();
        let interp_var = Variable::from_u32(8);
        builder.declare_var(interp_var, ptr_type);
        builder.def_var(interp_var, interpreter_val);
        let config_var = Variable::from_u32(9);
        builder.declare_var(config_var, ptr_type);
        builder.def_var(config_var, configuration_val);

        let direct_call_mode_var = Variable::from_u32(11);
        builder.declare_var(direct_call_mode_var, types::I8);
        let entry_instruction = builder.block_params(entry_block)[2];
        let direct_call_mode = builder.ins().icmp_imm(IntCC::Equal, entry_instruction, 0);
        builder.def_var(direct_call_mode_var, direct_call_mode);

        let saved_locals_base_var = Variable::from_u32(12);
        let saved_call_record_base_var = Variable::from_u32(13);
        let saved_call_record_top_var = Variable::from_u32(14);
        let saved_expression_var = Variable::from_u32(15);
        let saved_depth_var = Variable::from_u32(16);
        for var in [
            saved_locals_base_var,
            saved_call_record_base_var,
            saved_call_record_top_var,
            saved_expression_var,
            saved_depth_var,
        ] {
            builder.declare_var(var, ptr_type);
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
        let saved_locals = builder
            .ins()
            .load(ptr_type, MemFlags::trusted(), cfg, locals_base_offset);
        let saved_call_record = builder
            .ins()
            .load(ptr_type, MemFlags::trusted(), cfg, call_record_base_offset);
        let saved_call_record_top =
            builder
                .ins()
                .load(ptr_type, MemFlags::trusted(), cfg, call_record_stack_top_offset);
        let saved_expression = builder
            .ins()
            .load(ptr_type, MemFlags::trusted(), cfg, current_expression_offset);
        let saved_depth = builder.ins().load(ptr_type, MemFlags::trusted(), cfg, depth_offset);
        builder.def_var(saved_locals_base_var, saved_locals);
        builder.def_var(saved_call_record_base_var, saved_call_record);
        builder.def_var(saved_call_record_top_var, saved_call_record_top);
        builder.def_var(saved_expression_var, saved_expression);
        builder.def_var(saved_depth_var, saved_depth);

        let direct_setup_body = builder.create_block();
        let direct_stack_exhausted = builder.create_block();
        let depth_exhausted = builder.ins().icmp_imm(IntCC::UnsignedGreaterThan, saved_depth, 500);
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
        builder
            .ins()
            .store(MemFlags::trusted(), saved_call_record, cfg, locals_base_offset);
        let table_data = builder.ins().load(
            ptr_type,
            MemFlags::trusted(),
            cfg,
            current_compiled_fn_table_data_offset,
        );
        let entry_function_index = builder.block_params(entry_block)[3];
        let target_index = if ptr_type == types::I64 {
            builder.ins().uextend(types::I64, entry_function_index)
        } else {
            entry_function_index
        };
        let entry_offset = builder.ins().imul_imm(target_index, compiled_function_entry_size);
        let entry = builder.ins().iadd(table_data, entry_offset);
        let expression = builder.ins().load(
            ptr_type,
            MemFlags::trusted(),
            entry,
            compiled_function_entry_expression_offset,
        );
        builder
            .ins()
            .store(MemFlags::trusted(), expression, cfg, current_expression_offset);

        if max_call_rec_size > 0 {
            builder
                .ins()
                .store(MemFlags::trusted(), saved_call_record_top, cfg, call_record_base_offset);
            let next_call_record_top = builder.ins().iadd_imm(
                saved_call_record_top,
                i64::from(max_call_rec_size) * i64::from(value_size),
            );
            builder.ins().store(
                MemFlags::trusted(),
                next_call_record_top,
                cfg,
                call_record_stack_top_offset,
            );
        } else {
            let null = builder.ins().iconst(ptr_type, 0);
            builder
                .ins()
                .store(MemFlags::trusted(), null, cfg, call_record_base_offset);
        }
        let next_depth = builder.ins().iadd_imm(saved_depth, 1);
        builder.ins().store(MemFlags::trusted(), next_depth, cfg, depth_offset);
        builder.ins().jump(setup_done, &[]);

        builder.switch_to_block(normal_entry);
        builder.seal_block(normal_entry);
        builder.ins().jump(setup_done, &[]);

        builder.switch_to_block(setup_done);
        builder.seal_block(setup_done);
        let locals_base_var = Variable::from_u32(10);
        builder.declare_var(locals_base_var, ptr_type);
        let initial_locals_base =
            builder
                .ins()
                .load(ptr_type, MemFlags::trusted(), configuration_val, locals_base_offset);
        builder.def_var(locals_base_var, initial_locals_base);
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
            let cage_base = builder.ins().load(ptr_type, MemFlags::trusted(), cage_base_storage, 0);
            let memory_instances = builder.ins().load(
                ptr_type,
                MemFlags::trusted(),
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
                    .load(ptr_type, MemFlags::trusted(), memory_pointer_address, 0);
                let storage_offset = builder.ins().load(
                    types::I64,
                    MemFlags::trusted(),
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
                MemFlags::trusted(),
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
                    .load(ptr_type, MemFlags::trusted(), global_pointer_address, 0);
                global_instances.push((global_index, global));
            }
        }

        let mut control_stack: Vec<ControlFrame> = Vec::new();

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
        const VSTACK_VAR_BASE: u32 = 17;

        for i in 0..max_stack_depth {
            let var = Variable::from_u32(VSTACK_VAR_BASE + i as u32);
            builder.declare_var(var, types::I64);
            let zero = builder.ins().iconst(types::I64, 0);
            builder.def_var(var, zero);
            stack_vars.push(var);
        }
        let mut sp: usize = 0;

        // If we allocate something on the stack, make sure to restore the stack at the end.
        let initial_stack_size_var = Variable::from_u32(VSTACK_VAR_BASE + max_stack_depth as u32);
        builder.declare_var(initial_stack_size_var, types::I64);
        let mut next_var_id: u32 = VSTACK_VAR_BASE + max_stack_depth as u32 + 1;

        let reg_vars_f64: [Variable; REG_COUNT] = std::array::from_fn(|_| {
            let v = Variable::from_u32(next_var_id);
            next_var_id += 1;
            builder.declare_var(v, types::F64);
            v
        });
        let stack_vars_f64: Vec<Variable> = (0..max_stack_depth)
            .map(|_| {
                let v = Variable::from_u32(next_var_id);
                next_var_id += 1;
                builder.declare_var(v, types::F64);
                v
            })
            .collect();
        let reg_vars_f32: [Variable; REG_COUNT] = std::array::from_fn(|_| {
            let v = Variable::from_u32(next_var_id);
            next_var_id += 1;
            builder.declare_var(v, types::F32);
            v
        });
        let stack_vars_f32: Vec<Variable> = (0..max_stack_depth)
            .map(|_| {
                let v = Variable::from_u32(next_var_id);
                next_var_id += 1;
                builder.declare_var(v, types::F32);
                v
            })
            .collect();
        let mut reg_ty = [Bank::Int; REG_COUNT];
        let mut stack_ty = vec![Bank::Int; max_stack_depth];

        const F32_KIND: u8 = 2;
        const F64_KIND: u8 = 3;
        let num_locals = num_locals as usize;
        let num_params = num_params as usize;
        let local_is_f64: Vec<bool> = (0..num_locals)
            .map(|i| local_types.get(i).copied() == Some(F64_KIND))
            .collect();
        let local_is_f32: Vec<bool> = (0..num_locals)
            .map(|i| local_types.get(i).copied() == Some(F32_KIND))
            .collect();

        let selective_promotion_budget = if cfg!(target_arch = "aarch64") { 10 } else { 8 };
        let promoted_locals = Self::select_locals_for_promotion(insns, num_locals, selective_promotion_budget);
        // Keep this budget below the function-wide promotion budget. Caching more locals in large
        // blocks recreates the same register-pressure cliff that selective promotion avoids.
        let block_local_cache_budget = if cfg!(target_arch = "aarch64") {
            6
        } else if cfg!(target_arch = "x86_64") {
            2
        } else {
            0
        };
        let block_cached_locals =
            Self::select_locals_for_block_cache(insns, &promoted_locals, block_local_cache_budget);
        let block_local_cache_enabled = block_cached_locals.iter().any(|&cached| cached);
        let local_liveness = if block_local_cache_enabled {
            Some(Self::analyze_local_liveness(insns, num_locals)?)
        } else {
            None
        };
        let edge_cached_locals = local_liveness
            .as_ref()
            .map(|local_liveness| Self::select_locals_for_edge_cache(local_liveness, &block_cached_locals))
            .unwrap_or_default();
        let local_vars: Vec<Option<Variable>> = promoted_locals
            .iter()
            .map(|&promoted| {
                if !promoted {
                    return None;
                }
                let v = Variable::from_u32(next_var_id);
                next_var_id += 1;
                Some(v)
            })
            .collect();
        let mut dirty_locals = vec![false; num_locals];
        for (i, var) in local_vars.iter().enumerate() {
            let Some(var) = var else {
                continue;
            };
            let ty = if local_is_f64[i] {
                types::F64
            } else if local_is_f32[i] {
                types::F32
            } else {
                types::I64
            };
            builder.declare_var(*var, ty);
        }
        let edge_cache_vars: Vec<Vec<Option<Variable>>> = edge_cached_locals
            .iter()
            .map(|incoming_locals| {
                incoming_locals
                    .iter()
                    .map(|&cached| {
                        if !cached {
                            return None;
                        }
                        let variable = Variable::from_u32(next_var_id);
                        next_var_id += 1;
                        builder.declare_var(variable, types::I64);
                        Some(variable)
                    })
                    .collect()
            })
            .collect();
        let mut local_cache: Vec<Option<Value>> = vec![None; num_locals];
        let mut local_cache_dirty = vec![false; num_locals];

        // set_frame_lightweight verifies the stack-usage hint before these unchecked operations.
        macro_rules! emit_stack_push {
            ($builder:expr, $val:expr) => {{
                let v = $val;
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, MemFlags::trusted(), cfg, value_stack_top_offset);
                $builder.ins().store(MemFlags::trusted(), v, top, 0);
                let zero_tag = $builder.ins().iconst(types::I64, 0);
                $builder.ins().store(MemFlags::trusted(), zero_tag, top, 8);
                let new_top = $builder.ins().iadd_imm(top, i64::from(value_size));
                $builder
                    .ins()
                    .store(MemFlags::trusted(), new_top, cfg, value_stack_top_offset);
            }};
        }
        macro_rules! emit_stack_pop {
            ($builder:expr) => {{
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, MemFlags::trusted(), cfg, value_stack_top_offset);
                let new_top = $builder.ins().iadd_imm(top, -i64::from(value_size));
                $builder
                    .ins()
                    .store(MemFlags::trusted(), new_top, cfg, value_stack_top_offset);
                $builder.ins().load(types::I64, MemFlags::trusted(), new_top, 0)
            }};
        }
        macro_rules! emit_stack_size {
            ($builder:expr) => {{
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, MemFlags::trusted(), cfg, value_stack_top_offset);
                let base = $builder
                    .ins()
                    .load(ptr_type, MemFlags::trusted(), cfg, value_stack_base_offset);
                let bytes = $builder.ins().isub(top, base);
                $builder.ins().ushr_imm(bytes, 4)
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
                    .load(ptr_type, MemFlags::trusted(), cfg, value_stack_base_offset);
                let target_bytes = $builder.ins().ishl_imm($target_size, 4);
                let trimmed_top = $builder.ins().iadd(base, target_bytes);
                let new_top = if $arity as usize > 0 {
                    let top = $builder
                        .ins()
                        .load(ptr_type, MemFlags::trusted(), cfg, value_stack_top_offset);
                    let bits = $builder
                        .ins()
                        .load(types::I64, MemFlags::trusted(), top, -value_size);
                    let tag = $builder
                        .ins()
                        .load(types::I64, MemFlags::trusted(), top, -value_size + 8);
                    $builder.ins().store(MemFlags::trusted(), bits, trimmed_top, 0);
                    $builder.ins().store(MemFlags::trusted(), tag, trimmed_top, 8);
                    $builder.ins().iadd_imm(trimmed_top, i64::from(value_size))
                } else {
                    trimmed_top
                };
                $builder
                    .ins()
                    .store(MemFlags::trusted(), new_top, cfg, value_stack_top_offset);
            }};
        }

        if has_raw_call {
            let initial_stack_size = emit_stack_size!(builder);
            builder.def_var(initial_stack_size_var, initial_stack_size);
        } else {
            let zero = builder.ins().iconst(types::I64, 0);
            builder.def_var(initial_stack_size_var, zero);
        }

        // Read a value from a source location (register, virtual stack, or call record)
        macro_rules! read_src {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    $builder.use_var(reg_vars[src as usize])
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        // we have vstack, so just allocate on the native stack.
                        sp -= 1;
                        $builder.use_var(stack_vars[sp])
                    } else {
                        emit_stack_pop!($builder)
                    }
                } else {
                    // Frame entry allocated the record eagerly, so the read is a plain load.
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, MemFlags::trusted(), cfg, call_record_base_offset);
                    let off = i32::from(src - CALLREC_BASE) * value_size;
                    $builder.ins().load(types::I64, MemFlags::trusted(), base, off)
                }
            }};
        }

        // Temporarily materialize virtual stack slots 0..sp for an opaque runtime call. The saved
        // top remains valid because ValueStack storage cannot move while a frame is active.
        macro_rules! materialize_vstack_to_real {
            ($builder:expr) => {{
                let cfg = $builder.use_var(config_var);
                let top = $builder
                    .ins()
                    .load(ptr_type, MemFlags::trusted(), cfg, value_stack_top_offset);
                if sp > 0 {
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    for i in 0..sp {
                        let val = $builder.use_var(stack_vars[i]);
                        let offset = (i as i32) * value_size;
                        $builder.ins().store(MemFlags::trusted(), val, top, offset);
                        $builder
                            .ins()
                            .store(MemFlags::trusted(), zero_tag, top, offset + 8);
                    }
                    let new_top = $builder.ins().iadd_imm(top, i64::from(sp as i32 * value_size));
                    $builder
                        .ins()
                        .store(MemFlags::trusted(), new_top, cfg, value_stack_top_offset);
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
                    .load(ptr_type, MemFlags::trusted(), cfg, value_stack_top_offset);
                let result_bytes = (result_count as i64) * i64::from(value_size);
                let mut result_address = $builder.ins().iadd_imm(helper_top, -result_bytes);

                sp = stack_base;
                if destination == STACK_MARKER {
                    for i in 0..result_count {
                        let result = $builder
                            .ins()
                            .load(types::I64, MemFlags::trusted(), result_address, 0);
                        $builder.def_var(stack_vars[stack_base + i], result);
                        stack_ty[stack_base + i] = Bank::Int;
                        result_address = $builder.ins().iadd_imm(result_address, i64::from(value_size));
                    }
                    sp += result_count;
                } else {
                    debug_assert!(result_count <= 1);
                    if result_count == 1 {
                        let result = $builder
                            .ins()
                            .load(types::I64, MemFlags::trusted(), result_address, 0);
                        write_dst!($builder, destination, result);
                    }
                }

                $builder
                    .ins()
                    .store(MemFlags::trusted(), $original_top, cfg, value_stack_top_offset);
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
                        .load(ptr_type, MemFlags::trusted(), cfg, value_stack_top_offset);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    for i in 0..n {
                        let val = $builder.use_var(stack_vars[sp - n + i]);
                        let offset = (i as i32) * value_size;
                        $builder.ins().store(MemFlags::trusted(), val, top, offset);
                        $builder
                            .ins()
                            .store(MemFlags::trusted(), zero_tag, top, offset + 8);
                    }
                    let new_top = $builder.ins().iadd_imm(top, i64::from(n as i32 * value_size));
                    $builder
                        .ins()
                        .store(MemFlags::trusted(), new_top, cfg, value_stack_top_offset);
                }
            }};
        }

        macro_rules! write_dst {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars[dst as usize], val);
                    reg_ty[dst as usize] = Bank::Int;
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars[sp], val);
                        stack_ty[sp] = Bank::Int;
                        sp += 1;
                    } else {
                        emit_stack_push!($builder, val);
                    }
                } else {
                    // Frame entry allocated the record eagerly, so the write is two plain stores.
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, MemFlags::trusted(), cfg, call_record_base_offset);
                    let off = i32::from(dst - CALLREC_BASE) * value_size;
                    $builder.ins().store(MemFlags::trusted(), val, base, off);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    $builder.ins().store(MemFlags::trusted(), zero_tag, base, off + 8);
                }
            }};
        }

        macro_rules! read_src_f64 {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    if reg_ty[src as usize] == Bank::F64 {
                        $builder.use_var(reg_vars_f64[src as usize])
                    } else {
                        let raw = $builder.use_var(reg_vars[src as usize]);
                        $builder.ins().bitcast(types::F64, MemFlags::new(), raw)
                    }
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        sp -= 1;
                        if stack_ty[sp] == Bank::F64 {
                            $builder.use_var(stack_vars_f64[sp])
                        } else {
                            let raw = $builder.use_var(stack_vars[sp]);
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
                        .load(ptr_type, MemFlags::trusted(), cfg, call_record_base_offset);
                    let off = i32::from(src - CALLREC_BASE) * value_size;
                    $builder.ins().load(types::F64, MemFlags::trusted(), base, off)
                }
            }};
        }

        macro_rules! write_dst_f64 {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                let bits = $builder.ins().bitcast(types::I64, MemFlags::new(), val);
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars_f64[dst as usize], val);
                    $builder.def_var(reg_vars[dst as usize], bits);
                    reg_ty[dst as usize] = Bank::F64;
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars_f64[sp], val);
                        $builder.def_var(stack_vars[sp], bits);
                        stack_ty[sp] = Bank::F64;
                        sp += 1;
                    } else {
                        emit_stack_push!($builder, bits);
                    }
                } else {
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, MemFlags::trusted(), cfg, call_record_base_offset);
                    let off = i32::from(dst - CALLREC_BASE) * value_size;
                    $builder.ins().store(MemFlags::trusted(), bits, base, off);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    $builder.ins().store(MemFlags::trusted(), zero_tag, base, off + 8);
                }
            }};
        }

        macro_rules! read_src_f32 {
            ($builder:expr, $src:expr) => {{
                let src = $src;
                if src < STACK_MARKER {
                    if reg_ty[src as usize] == Bank::F32 {
                        $builder.use_var(reg_vars_f32[src as usize])
                    } else {
                        let raw = $builder.use_var(reg_vars[src as usize]);
                        let raw32 = $builder.ins().ireduce(types::I32, raw);
                        $builder.ins().bitcast(types::F32, MemFlags::new(), raw32)
                    }
                } else if src == STACK_MARKER {
                    if max_stack_depth > 0 && sp > 0 {
                        sp -= 1;
                        if stack_ty[sp] == Bank::F32 {
                            $builder.use_var(stack_vars_f32[sp])
                        } else {
                            let raw = $builder.use_var(stack_vars[sp]);
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
                        .load(ptr_type, MemFlags::trusted(), cfg, call_record_base_offset);
                    let off = i32::from(src - CALLREC_BASE) * value_size;
                    $builder.ins().load(types::F32, MemFlags::trusted(), base, off)
                }
            }};
        }

        macro_rules! write_dst_f32 {
            ($builder:expr, $dst:expr, $val:expr) => {{
                let dst = $dst;
                let val = $val;
                // Keep the always-valid i64 copy in sync for bank-agnostic consumers: an f32 is 4
                // bytes, so bitcast to i32 and sign-extend into the boxed slot.
                let bits32 = $builder.ins().bitcast(types::I32, MemFlags::new(), val);
                let bits = $builder.ins().sextend(types::I64, bits32);
                if dst < STACK_MARKER {
                    $builder.def_var(reg_vars_f32[dst as usize], val);
                    $builder.def_var(reg_vars[dst as usize], bits);
                    reg_ty[dst as usize] = Bank::F32;
                    dirty_regs[dst as usize] = true;
                } else if dst == STACK_MARKER {
                    if max_stack_depth > 0 {
                        $builder.def_var(stack_vars_f32[sp], val);
                        $builder.def_var(stack_vars[sp], bits);
                        stack_ty[sp] = Bank::F32;
                        sp += 1;
                    } else {
                        emit_stack_push!($builder, bits);
                    }
                } else {
                    let cfg = $builder.use_var(config_var);
                    let base = $builder
                        .ins()
                        .load(ptr_type, MemFlags::trusted(), cfg, call_record_base_offset);
                    let off = i32::from(dst - CALLREC_BASE) * value_size;
                    $builder.ins().store(MemFlags::trusted(), bits, base, off);
                    let zero_tag = $builder.ins().iconst(types::I64, 0);
                    $builder.ins().store(MemFlags::trusted(), zero_tag, base, off + 8);
                }
            }};
        }

        macro_rules! reset_banks {
            () => {{
                for t in reg_ty.iter_mut() {
                    *t = Bank::Int;
                }
                for t in stack_ty.iter_mut() {
                    *t = Bank::Int;
                }
            }};
        }

        // Note that all reads from sources have to be in order (sources[0] before sources[1])
        macro_rules! i32_binop {
            ($builder:expr, $insn:expr, $op:ident) => {{
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs = $builder.ins().ireduce(types::I32, lhs_raw);
                let rhs = $builder.ins().ireduce(types::I32, rhs_raw);
                let result = $builder.ins().$op(lhs, rhs);
                let result = $builder.ins().sextend(types::I64, result);
                write_dst!($builder, $insn.destination, result);
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
                let src_raw = read_src!($builder, $insn.sources[0]);
                let src = $builder.ins().ireduce(types::I32, src_raw);
                let result = $builder.ins().$op(src);
                let result = $builder.ins().sextend(types::I64, result);
                write_dst!($builder, $insn.destination, result);
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
                let rhs_raw = read_src!($builder, $insn.sources[0]);
                let lhs_raw = read_src!($builder, $insn.sources[1]);
                let lhs = $builder.ins().ireduce(types::I32, lhs_raw);
                let rhs = $builder.ins().ireduce(types::I32, rhs_raw);
                let cmp = $builder.ins().icmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! i64_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs = read_src!($builder, $insn.sources[0]);
                let lhs = read_src!($builder, $insn.sources[1]);
                let cmp = $builder.ins().icmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
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
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
            }};
        }
        macro_rules! f64_cmp {
            ($builder:expr, $insn:expr, $cc:expr) => {{
                let rhs = read_src_f64!($builder, $insn.sources[0]);
                let lhs = read_src_f64!($builder, $insn.sources[1]);
                let cmp = $builder.ins().fcmp($cc, lhs, rhs);
                let result = $builder.ins().uextend(types::I64, cmp);
                write_dst!($builder, $insn.destination, result);
            }};
        }

        macro_rules! read_local_inline {
            ($builder:expr, $idx_imm:expr) => {{
                let idx = ($idx_imm) as usize;
                if let Some(var) = local_vars.get(idx).copied().flatten() {
                    let v = $builder.use_var(var);
                    if local_is_f64[idx] {
                        $builder.ins().bitcast(types::I64, MemFlags::new(), v)
                    } else if local_is_f32[idx] {
                        let bits32 = $builder.ins().bitcast(types::I32, MemFlags::new(), v);
                        $builder.ins().sextend(types::I64, bits32)
                    } else {
                        v
                    }
                } else if block_cached_locals[idx] {
                    if let Some(value) = local_cache[idx] {
                        value
                    } else {
                        let lb = $builder.use_var(locals_base_var);
                        let value = $builder
                            .ins()
                            .load(types::I64, MemFlags::trusted(), lb, (idx as i32) * value_size);
                        local_cache[idx] = Some(value);
                        value
                    }
                } else {
                    let lb = $builder.use_var(locals_base_var);
                    $builder
                        .ins()
                        .load(types::I64, MemFlags::trusted(), lb, (idx as i32) * value_size)
                }
            }};
        }
        macro_rules! read_local_f64 {
            ($builder:expr, $idx_imm:expr) => {{
                let idx = ($idx_imm) as usize;
                if let Some(var) = local_vars.get(idx).copied().flatten() {
                    if local_is_f64[idx] {
                        $builder.use_var(var)
                    } else {
                        let v = $builder.use_var(var);
                        $builder.ins().bitcast(types::F64, MemFlags::new(), v)
                    }
                } else if block_cached_locals[idx] {
                    let bits = read_local_inline!($builder, $idx_imm);
                    $builder.ins().bitcast(types::F64, MemFlags::new(), bits)
                } else {
                    let lb = $builder.use_var(locals_base_var);
                    $builder
                        .ins()
                        .load(types::F64, MemFlags::trusted(), lb, (idx as i32) * value_size)
                }
            }};
        }
        macro_rules! read_local_f32 {
            ($builder:expr, $idx_imm:expr) => {{
                let idx = ($idx_imm) as usize;
                if let Some(var) = local_vars.get(idx).copied().flatten() {
                    if local_is_f32[idx] {
                        $builder.use_var(var)
                    } else {
                        let v = $builder.use_var(var);
                        let v32 = $builder.ins().ireduce(types::I32, v);
                        $builder.ins().bitcast(types::F32, MemFlags::new(), v32)
                    }
                } else if block_cached_locals[idx] {
                    let bits = read_local_inline!($builder, $idx_imm);
                    let bits32 = $builder.ins().ireduce(types::I32, bits);
                    $builder.ins().bitcast(types::F32, MemFlags::new(), bits32)
                } else {
                    let lb = $builder.use_var(locals_base_var);
                    $builder
                        .ins()
                        .load(types::F32, MemFlags::trusted(), lb, (idx as i32) * value_size)
                }
            }};
        }
        macro_rules! write_local_inline {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let idx = ($idx_imm) as usize;
                let v = $val;
                if let Some(var) = local_vars.get(idx).copied().flatten() {
                    let stored = if local_is_f64[idx] {
                        $builder.ins().bitcast(types::F64, MemFlags::new(), v)
                    } else if local_is_f32[idx] {
                        let v32 = $builder.ins().ireduce(types::I32, v);
                        $builder.ins().bitcast(types::F32, MemFlags::new(), v32)
                    } else {
                        v
                    };
                    $builder.def_var(var, stored);
                    dirty_locals[idx] = true;
                } else if block_cached_locals[idx] {
                    local_cache[idx] = Some(v);
                    local_cache_dirty[idx] = true;
                } else {
                    let lb = $builder.use_var(locals_base_var);
                    let offset = (idx as i32) * value_size;
                    $builder.ins().store(MemFlags::trusted(), v, lb, offset);
                    let zero = $builder.ins().iconst(types::I64, 0);
                    $builder.ins().store(MemFlags::trusted(), zero, lb, offset + 8);
                }
            }};
        }
        macro_rules! write_local_f64 {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let idx = ($idx_imm) as usize;
                let v = $val;
                if let Some(var) = local_vars.get(idx).copied().flatten() {
                    if local_is_f64[idx] {
                        $builder.def_var(var, v);
                    } else {
                        let bits = $builder.ins().bitcast(types::I64, MemFlags::new(), v);
                        $builder.def_var(var, bits);
                    }
                    dirty_locals[idx] = true;
                } else if block_cached_locals[idx] {
                    let bits = $builder.ins().bitcast(types::I64, MemFlags::new(), v);
                    local_cache[idx] = Some(bits);
                    local_cache_dirty[idx] = true;
                } else {
                    let lb = $builder.use_var(locals_base_var);
                    let offset = (idx as i32) * value_size;
                    $builder.ins().store(MemFlags::trusted(), v, lb, offset);
                    let zero = $builder.ins().iconst(types::I64, 0);
                    $builder.ins().store(MemFlags::trusted(), zero, lb, offset + 8);
                }
            }};
        }
        macro_rules! write_local_f32 {
            ($builder:expr, $idx_imm:expr, $val:expr) => {{
                let idx = ($idx_imm) as usize;
                let v = $val;
                if local_vars.get(idx).is_some_and(Option::is_some) && local_is_f32[idx] {
                    $builder.def_var(local_vars[idx].expect("promoted local"), v);
                    dirty_locals[idx] = true;
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
                let is_promoted = local_vars.get(idx).is_some_and(Option::is_some);
                if is_promoted && local_is_f64[idx] {
                    let result = read_local_f64!($builder, $idx_imm);
                    write_dst_f64!($builder, $dst, result);
                } else if is_promoted && local_is_f32[idx] {
                    let result = read_local_f32!($builder, $idx_imm);
                    write_dst_f32!($builder, $dst, result);
                } else {
                    let result = read_local_inline!($builder, $idx_imm);
                    write_dst!($builder, $dst, result);
                }
            }};
        }
        macro_rules! local_set {
            ($builder:expr, $idx_imm:expr, $src:expr) => {{
                let idx = ($idx_imm) as usize;
                let is_promoted = local_vars.get(idx).is_some_and(Option::is_some);
                if is_promoted && local_is_f64[idx] {
                    let val = read_src_f64!($builder, $src);
                    write_local_f64!($builder, $idx_imm, val);
                } else if is_promoted && local_is_f32[idx] {
                    let val = read_src_f32!($builder, $src);
                    write_local_f32!($builder, $idx_imm, val);
                } else {
                    let val = read_src!($builder, $src);
                    write_local_inline!($builder, $idx_imm, val);
                }
            }};
        }
        macro_rules! flush_locals {
            ($builder:expr) => {{
                let lb = $builder.use_var(locals_base_var);
                for i in 0..local_vars.len() {
                    if !dirty_locals[i] {
                        continue;
                    }
                    let var = local_vars[i].expect("dirty local must be promoted");
                    let v = $builder.use_var(var);
                    let stored = if local_is_f32[i] {
                        let bits32 = $builder.ins().bitcast(types::I32, MemFlags::new(), v);
                        $builder.ins().sextend(types::I64, bits32)
                    } else {
                        v
                    };
                    let offset = (i as i32) * value_size;
                    $builder.ins().store(MemFlags::trusted(), stored, lb, offset);
                    let zero = $builder.ins().iconst(types::I64, 0);
                    $builder.ins().store(MemFlags::trusted(), zero, lb, offset + 8);
                }
            }};
        }
        macro_rules! begin_local_cache_block {
            ($builder:expr, $instruction_index:expr) => {{
                if block_local_cache_enabled {
                    let local_liveness = local_liveness
                        .as_ref()
                        .expect("block-local caching requires local liveness");
                    let block_index = local_liveness.block_index_at($instruction_index);
                    let block = &local_liveness.blocks[block_index];
                    debug_assert_eq!(block.start, $instruction_index);

                    for local_index in 0..num_locals {
                        let Some(variable) = edge_cache_vars[block_index][local_index] else {
                            continue;
                        };
                        let value = $builder.use_var(variable);
                        local_cache[local_index] = Some(value);

                        local_cache_dirty[local_index] = block.predecessors.iter().any(|&predecessor_index| {
                            let predecessor = &local_liveness.blocks[predecessor_index];
                            !predecessor.successors.iter().any(|&successor_index| {
                                local_liveness.blocks[successor_index]
                                    .live_in
                                    .contains(local_index)
                                    && edge_cache_vars[successor_index][local_index].is_none()
                            })
                        });
                    }
                }
            }};
        }
        macro_rules! finish_local_cache_block {
            ($builder:expr, $instruction_index:expr) => {{
                if block_local_cache_enabled {
                    let local_liveness = local_liveness
                        .as_ref()
                        .expect("block-local caching requires local liveness");
                    let block = local_liveness.block_at($instruction_index);
                    debug_assert_eq!(block.end, $instruction_index + 1);

                    for local_index in 0..num_locals {
                        if !local_cache_dirty[local_index] || !block.live_out.contains(local_index) {
                            continue;
                        }
                        let value = local_cache[local_index].expect("dirty local must be cached");
                        for &successor_index in &block.successors {
                            let Some(variable) = edge_cache_vars[successor_index][local_index] else {
                                continue;
                            };
                            $builder.def_var(variable, value);
                        }
                    }

                    let local_needs_store = |local_index| {
                        block.successors.iter().any(|&successor_index| {
                            local_liveness.blocks[successor_index]
                                .live_in
                                .contains(local_index)
                                && edge_cache_vars[successor_index][local_index].is_none()
                        })
                    };
                    if local_cache_dirty.iter().enumerate().any(|(local_index, &dirty)| {
                        dirty && block.live_out.contains(local_index) && local_needs_store(local_index)
                    }) {
                        let lb = $builder.use_var(locals_base_var);
                        let zero = $builder.ins().iconst(types::I64, 0);
                        for local_index in 0..num_locals {
                            if !local_cache_dirty[local_index]
                                || !block.live_out.contains(local_index)
                                || !local_needs_store(local_index)
                            {
                                continue;
                            }
                            let value = local_cache[local_index].expect("dirty local must be cached");
                            let offset = (local_index as i32) * value_size;
                            $builder.ins().store(MemFlags::trusted(), value, lb, offset);
                            $builder.ins().store(MemFlags::trusted(), zero, lb, offset + 8);
                        }
                    }

                    local_cache.fill(None);
                    local_cache_dirty.fill(false);
                }
            }};
        }

        // Call + trap-check macro for helper calls that do not consume caller register state.
        // The callee gets arguments explicitly (register immediates, stack, or call record), and the caller's virtual registers stay live in SSA across the call.
        macro_rules! do_call_and_check {
            ($builder:expr, $sig:expr, $ptr:expr, $args:expr) => {{
                let call = $builder.ins().call_indirect($sig, $ptr, $args);
                let trapped = $builder.inst_results(call)[0];
                let is_trap = $builder.ins().icmp_imm(IntCC::NotEqual, trapped, 0);
                let cont = $builder.create_block();
                $builder.ins().brif(is_trap, trap_block, &[], cont, &[]);
                $builder.switch_to_block(cont);
                $builder.seal_block(cont);
                // Reload locals_base; frame_stack may have reallocated.
                let _cfg_for_lb = $builder.use_var(config_var);
                let new_lb = $builder
                    .ins()
                    .load(ptr_type, MemFlags::trusted(), _cfg_for_lb, locals_base_offset);
                $builder.def_var(locals_base_var, new_lb);
            }};
        }
        macro_rules! set_trap {
            ($builder:expr, $msg:expr) => {{
                let msg = $msg.as_bytes();
                let ss = $builder.create_sized_stack_slot(StackSlotData::new(
                    StackSlotKind::ExplicitSlot,
                    msg.len() as u32,
                    0,
                ));
                for (i, &byte) in msg.iter().enumerate() {
                    let b = $builder.ins().iconst(types::I8, i64::from(byte));
                    $builder.ins().stack_store(b, ss, i as i32);
                }
                let msg_ptr = $builder.ins().stack_addr(ptr_type, ss, 0);
                let msg_len = $builder.ins().iconst(types::I32, msg.len() as i64);
                let st_ptr = $builder.ins().func_addr(ptr_type, h_set_trap);
                let interp = $builder.use_var(interp_var);
                $builder
                    .ins()
                    .call_indirect(set_trap_sig, st_ptr, &[interp, msg_ptr, msg_len]);
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

        // On a fresh call only the parameters are initialized by the caller.
        macro_rules! init_locals_fresh {
            ($builder:expr) => {{
                let lb = $builder.use_var(locals_base_var);
                let memory_zero = if local_vars[num_params..].iter().any(Option::is_none) {
                    Some($builder.ins().iconst(types::I64, 0))
                } else {
                    None
                };
                for (i, var) in local_vars.iter().enumerate() {
                    if let Some(var) = var {
                        if i < num_params {
                            let ty = if local_is_f64[i] {
                                types::F64
                            } else if local_is_f32[i] {
                                types::F32
                            } else {
                                types::I64
                            };
                            let offset = (i as i32) * value_size;
                            let val = $builder.ins().load(ty, MemFlags::trusted(), lb, offset);
                            $builder.def_var(*var, val);
                        } else if local_is_f64[i] {
                            let zero = $builder.ins().f64const(0.0);
                            $builder.def_var(*var, zero);
                        } else if local_is_f32[i] {
                            let zero = $builder.ins().f32const(0.0);
                            $builder.def_var(*var, zero);
                        } else {
                            let zero = $builder.ins().iconst(types::I64, 0);
                            $builder.def_var(*var, zero);
                        }
                    } else if i >= num_params {
                        let zero = memory_zero.expect("unpromoted non-parameter local needs memory initialization");
                        let offset = (i as i32) * value_size;
                        $builder.ins().store(MemFlags::trusted(), zero, lb, offset);
                        $builder.ins().store(MemFlags::trusted(), zero, lb, offset + 8);
                    }
                }
            }};
        }
        macro_rules! init_locals_resume {
            ($builder:expr) => {{
                let lb = $builder.use_var(locals_base_var);
                for (i, var) in local_vars.iter().enumerate() {
                    let Some(var) = var else {
                        continue;
                    };
                    let ty = if local_is_f64[i] {
                        types::F64
                    } else if local_is_f32[i] {
                        types::F32
                    } else {
                        types::I64
                    };
                    let offset = (i as i32) * value_size;
                    let val = $builder.ins().load(ty, MemFlags::trusted(), lb, offset);
                    $builder.def_var(*var, val);
                }
            }};
        }

        // If we have any tier-up checkpoints, the interpreter will eventually need to jump to some point in the function other than the entry block, so prepare dispatch blocks for that.
        // Note that the initial block will already have the correct register state loaded, so we don't need to sync registers for the tier-up dispatch targets.
        let has_tier_up = insns.iter().any(|i| i.opcode == op::SYNTHETIC_TIER_UP);
        let tier_up_target_ip = builder.block_params(entry_block)[3];
        let mut tier_up_dispatch_tail: Option<Block> = None;
        let tier_up_body_start: Option<Block> = if has_tier_up {
            let body_start = builder.create_block();
            let dispatch = builder.create_block();
            let fresh = builder.create_block();
            let resume = builder.create_block();
            let has_tier_up_target = builder.ins().icmp_imm(IntCC::NotEqual, tier_up_target_ip, 0);
            let direct_call_mode = builder.use_var(direct_call_mode_var);
            let is_normal_entry = builder.ins().icmp_imm(IntCC::Equal, direct_call_mode, 0);
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
            if block_local_cache_enabled
                && local_liveness
                    .as_ref()
                    .expect("block-local caching requires local liveness")
                    .block_at(ip)
                    .start
                    == ip
            {
                begin_local_cache_block!(builder, ip);
            }

            let insn = &insns[ip];
            let opc = insn.opcode;

            match opc {
                op::NOP => {}

                op::UNREACHABLE => {
                    finish_local_cache_block!(builder, ip);
                    Self::sync_regs_to_config(
                        &mut builder,
                        &reg_vars,
                        config_var,
                        regs_offset,
                        value_size,
                        &dirty_regs,
                    );
                    flush_locals!(builder);
                    set_trap!(builder, "unreachable executed");
                    builder.ins().jump(trap_block, &[]);
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
                        let var = Variable::from_u32(next_var_id);
                        next_var_id += 1;
                        builder.declare_var(var, types::I64);
                        let cur = emit_stack_size!(builder);
                        let entry = builder.ins().iadd_imm(cur, -(param_count as i64));
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
                        entry_real_depth_var,
                        bank_snapshot: None,
                    });
                }

                op::LOOP => {
                    finish_local_cache_block!(builder, ip);
                    let arity = insn.imm3 & 0xffff;
                    let param_count = (insn.imm3 >> 16) as usize;
                    let header = builder.create_block();
                    let after = builder.create_block();
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = Variable::from_u32(next_var_id);
                        next_var_id += 1;
                        builder.declare_var(var, types::I64);
                        let cur = emit_stack_size!(builder);
                        let entry = builder.ins().iadd_imm(cur, -(param_count as i64));
                        builder.def_var(var, entry);
                        Some(var)
                    } else {
                        None
                    };
                    builder.ins().jump(header, &[]);
                    builder.switch_to_block(header);
                    control_stack.push(ControlFrame {
                        kind: ControlKind::Loop,
                        branch_target: header,
                        after_block: after,
                        arity,
                        param_count,
                        stack_depth_at_entry: (sp - param_count) as i32,
                        entry_real_depth_var,
                        bank_snapshot: None,
                    });
                    // Loop header is a merge point (entry edge + back-edges + any tier-up dispatch).
                    reset_banks!();
                }

                op::IF => {
                    finish_local_cache_block!(builder, ip);
                    let arity = insn.imm3 & 0xffff;
                    let _param_count = (insn.imm3 >> 16) as usize;
                    let has_else = insn.imm2 >= 0;
                    let then_block = builder.create_block();
                    let else_block = builder.create_block();
                    let after = builder.create_block();

                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let cond = builder.ins().icmp_imm(IntCC::NotEqual, cond_raw, 0);
                    let entry_real_depth_var = if max_stack_depth == 0 {
                        let var = Variable::from_u32(next_var_id);
                        next_var_id += 1;
                        builder.declare_var(var, types::I64);
                        let cur = emit_stack_size!(builder);
                        let entry = builder.ins().iadd_imm(cur, -(_param_count as i64));
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
                        entry_real_depth_var,
                        bank_snapshot: Some((reg_ty, stack_ty.clone())),
                    });
                }

                op::ELSE => {
                    finish_local_cache_block!(builder, ip);
                    if let Some(frame) = control_stack.last() {
                        let else_block = frame.after_block;
                        let after = frame.branch_target;
                        let entry_depth = frame.stack_depth_at_entry;
                        let pc = frame.param_count;
                        let snapshot = frame.bank_snapshot.clone();
                        builder.ins().jump(after, &[]);
                        builder.switch_to_block(else_block);
                        builder.seal_block(else_block);
                        // Reset sp to entry depth + param_count (else branch inherits params).
                        sp = (entry_depth as usize) + pc;
                        if let Some((saved_reg_ty, saved_stack_ty)) = snapshot {
                            reg_ty = saved_reg_ty;
                            stack_ty = saved_stack_ty;
                        }
                        if let Some(frame) = control_stack.last_mut() {
                            frame.after_block = after;
                        }
                    }
                }

                op::END | op::SYNTHETIC_END_EXPRESSION => {
                    finish_local_cache_block!(builder, ip);
                    if let Some(frame) = control_stack.pop() {
                        let after = if frame.kind == ControlKind::If && frame.after_block != frame.branch_target {
                            // If without else: the after_block is the branch_target.
                            frame.branch_target
                        } else {
                            frame.after_block
                        };

                        builder.ins().jump(after, &[]);
                        builder.switch_to_block(after);
                        is_unreachable = false;
                        // `after` merges the block body with any branches to it.
                        reset_banks!();

                        // After end of block, sp = entry depth + arity.
                        sp = (frame.stack_depth_at_entry + frame.arity as i32) as usize;

                        if frame.kind == ControlKind::Loop {
                            builder.seal_block(frame.branch_target); // loop header
                        }
                        builder.seal_block(after);
                    } else if !is_unreachable {
                        push_top_n_to_real!(builder, result_arity);
                        builder.ins().jump(epilogue_block, &[]);
                        let dead = builder.create_block();
                        builder.switch_to_block(dead);
                        builder.seal_block(dead);
                    }
                }

                op::BR | op::SYNTHETIC_BR_NOSTACK => {
                    finish_local_cache_block!(builder, ip);
                    let label_idx = insn.imm1 as usize;
                    if label_idx < control_stack.len() {
                        let target_idx = control_stack.len() - 1 - label_idx;
                        let frame = &control_stack[target_idx];
                        let target = frame.branch_target;
                        let arity = if frame.kind == ControlKind::Loop {
                            0
                        } else {
                            frame.arity
                        };
                        let entry = frame.stack_depth_at_entry as usize;
                        if max_stack_depth > 0 {
                            // vstack enabled: move top arity values to entry position.
                            if arity > 0 {
                                let result = if sp > 0 {
                                    builder.use_var(stack_vars[sp - 1])
                                } else {
                                    emit_stack_pop!(builder)
                                };
                                builder.def_var(stack_vars[entry], result);
                            }
                        } else {
                            // vstack disabled: trim the real value stack down to the target label's entry depth + arity, preserving the top arity values.
                            let entry_depth_var = frame
                                .entry_real_depth_var
                                .expect("entry_real_depth_var must be set when vstack is disabled");
                            let target_size = builder.use_var(entry_depth_var);
                            emit_stack_cleanup!(builder, target_size, arity);
                        }
                        builder.ins().jump(target, &[]);
                    } else {
                        // br to function label = return.
                        push_top_n_to_real!(builder, result_arity);
                        builder.ins().jump(epilogue_block, &[]);
                    }
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::BR_IF | op::SYNTHETIC_BR_IF_NOSTACK => {
                    finish_local_cache_block!(builder, ip);
                    let label_idx = insn.imm1 as usize;
                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let cond = builder.ins().icmp_imm(IntCC::NotEqual, cond_raw, 0);

                    if label_idx < control_stack.len() {
                        let target_idx = control_stack.len() - 1 - label_idx;
                        let frame = &control_stack[target_idx];
                        let target = frame.branch_target;
                        let arity = if frame.kind == ControlKind::Loop {
                            0
                        } else {
                            frame.arity
                        };
                        let entry = frame.stack_depth_at_entry as usize;
                        let extras = (sp as i32 - entry as i32 - arity as i32).max(0);
                        if max_stack_depth == 0 {
                            // not vstack: real value stack may have extras between the target label's entry depth and the result on top.
                            // On the taken path, trim using the saved entry-depth variable.
                            let entry_depth_var = frame
                                .entry_real_depth_var
                                .expect("entry_real_depth_var must be set when vstack is disabled");
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            let target_size = builder.use_var(entry_depth_var);
                            emit_stack_cleanup!(builder, target_size, arity);
                            builder.ins().jump(target, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else if extras > 0 {
                            let taken_block = builder.create_block();
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, taken_block, &[], fallthrough, &[]);
                            builder.switch_to_block(taken_block);
                            builder.seal_block(taken_block);
                            if arity > 0 {
                                let result = builder.use_var(stack_vars[sp - 1]);
                                builder.def_var(stack_vars[entry], result);
                            }
                            // Note: we don't change sp here since fallthrough needs the original sp.
                            builder.ins().jump(target, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else {
                            let fallthrough = builder.create_block();
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
                            builder.ins().jump(epilogue_block, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        } else {
                            let fallthrough = builder.create_block();
                            builder.ins().brif(cond, epilogue_block, &[], fallthrough, &[]);
                            builder.switch_to_block(fallthrough);
                            builder.seal_block(fallthrough);
                        }
                    }
                }

                op::RETURN => {
                    finish_local_cache_block!(builder, ip);
                    push_top_n_to_real!(builder, result_arity);
                    builder.ins().jump(epilogue_block, &[]);
                    sp = 0;
                    is_unreachable = true;
                    let dead = builder.create_block();
                    builder.switch_to_block(dead);
                    builder.seal_block(dead);
                }

                op::I32_CONST | op::I64_CONST => {
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
                    let is_promoted = local_vars.get(idx).is_some_and(Option::is_some);
                    if is_promoted && local_is_f64[idx] {
                        let val = read_src_f64!(builder, insn.sources[0]);
                        write_local_f64!(builder, insn.imm1, val);
                        write_dst_f64!(builder, insn.destination, val);
                    } else if is_promoted && local_is_f32[idx] {
                        let val = read_src_f32!(builder, insn.sources[0]);
                        write_local_f32!(builder, insn.imm1, val);
                        write_dst_f32!(builder, insn.destination, val);
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
                    let val = read_local_inline!(builder, insn.imm1);
                    write_local_inline!(builder, insn.imm2, val);
                }

                op::GLOBAL_GET => {
                    let global = inline_global_instance!(insn.imm1 as u32);
                    let result =
                        builder
                            .ins()
                            .load(types::I64, MemFlags::trusted(), global, global_instance_value_offset);
                    write_dst!(builder, insn.destination, result);
                }
                op::GLOBAL_SET => {
                    let val = read_src!(builder, insn.sources[0]);
                    let global = inline_global_instance!(insn.imm1 as u32);
                    builder
                        .ins()
                        .store(MemFlags::trusted(), val, global, global_instance_value_offset);
                    let zero = builder.ins().iconst(types::I64, 0);
                    builder
                        .ins()
                        .store(MemFlags::trusted(), zero, global, global_instance_value_offset + 8);
                }

                op::DROP => {
                    if insn.sources[0] == STACK_MARKER {
                        read_src!(builder, insn.sources[0]);
                    }
                    // No need to do anything if it's not on the real stack.
                }

                op::SELECT | op::SELECT_TYPED => {
                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let rhs = read_src!(builder, insn.sources[1]);
                    let lhs = read_src!(builder, insn.sources[2]);
                    let cond = builder.ins().icmp_imm(IntCC::NotEqual, cond_raw, 0);
                    let result = builder.ins().select(cond, lhs, rhs);
                    write_dst!(builder, insn.destination, result);
                }

                op::BR_TABLE => {
                    finish_local_cache_block!(builder, ip);
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

                    let cond_raw = read_src!(builder, insn.sources[0]);
                    let cond = builder.ins().ireduce(types::I32, cond_raw);

                    let branch_to_label = |builder: &mut FunctionBuilder, label_idx: usize| {
                        if label_idx < control_stack.len() {
                            let target_idx = control_stack.len() - 1 - label_idx;
                            let frame = &control_stack[target_idx];
                            let target = frame.branch_target;
                            let arity = if frame.kind == ControlKind::Loop {
                                0
                            } else {
                                frame.arity
                            };
                            let entry = frame.stack_depth_at_entry as usize;
                            if max_stack_depth > 0 && arity > 0 {
                                let result = if sp > 0 {
                                    builder.use_var(stack_vars[sp - 1])
                                } else {
                                    emit_stack_pop!(builder)
                                };
                                builder.def_var(stack_vars[entry], result);
                            } else if max_stack_depth == 0 {
                                let entry_depth_var = frame
                                    .entry_real_depth_var
                                    .expect("entry_real_depth_var must be set when vstack is disabled");
                                let target_size = builder.use_var(entry_depth_var);
                                emit_stack_cleanup!(builder, target_size, arity);
                            }
                            builder.ins().jump(target, &[]);
                        } else {
                            push_top_n_to_real!(builder, result_arity);
                            builder.ins().jump(epilogue_block, &[]);
                        }
                    };

                    for (i, &label) in all_labels.iter().enumerate() {
                        let case_block = builder.create_block();
                        let next_fallthrough = builder.create_block();
                        let compare = builder.ins().icmp_imm(IntCC::Equal, cond, i as i64);
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
                    let src_raw = read_src!(builder, insn.sources[0]);
                    let src = builder.ins().ireduce(types::I32, src_raw);
                    let r = builder.ins().icmp_imm(IntCC::Equal, src, 0);
                    let result = builder.ins().uextend(types::I64, r);
                    write_dst!(builder, insn.destination, result);
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
                    let r = builder.ins().icmp_imm(IntCC::Equal, src, 0);
                    let result = builder.ins().uextend(types::I64, r);
                    write_dst!(builder, insn.destination, result);
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
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND_SI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().sextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I64_EXTEND_UI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I32, src);
                    let result = builder.ins().uextend(types::I64, narrowed);
                    write_dst!(builder, insn.destination, result);
                }
                op::I32_EXTEND8_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I8, src);
                    let extended = builder.ins().sextend(types::I32, narrowed);
                    let result = builder.ins().sextend(types::I64, extended);
                    write_dst!(builder, insn.destination, result);
                }
                op::I32_EXTEND16_S => {
                    let src = read_src!(builder, insn.sources[0]);
                    let narrowed = builder.ins().ireduce(types::I16, src);
                    let extended = builder.ins().sextend(types::I32, narrowed);
                    let result = builder.ins().sextend(types::I64, extended);
                    write_dst!(builder, insn.destination, result);
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
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f32_val = builder.ins().fcvt_from_sint(types::F32, i32_val);
                    write_dst_f32!(builder, insn.destination, f32_val);
                }
                op::F32_CONVERT_UI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f32_val = builder.ins().fcvt_from_uint(types::F32, i32_val);
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
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f64_val = builder.ins().fcvt_from_sint(types::F64, i32_val);
                    write_dst_f64!(builder, insn.destination, f64_val);
                }
                op::F64_CONVERT_UI32 => {
                    let src = read_src!(builder, insn.sources[0]);
                    let i32_val = builder.ins().ireduce(types::I32, src);
                    let f64_val = builder.ins().fcvt_from_uint(types::F64, i32_val);
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
                op::I32_REINTERPRET_F32
                | op::F32_REINTERPRET_I32
                | op::I64_REINTERPRET_F64
                | op::F64_REINTERPRET_I64 => {
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

                    let result = if is_i32_dst {
                        builder.ins().sextend(types::I64, int_val)
                    } else {
                        int_val
                    };
                    write_dst!(builder, insn.destination, result);
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

                    let result = if is_i32_dst {
                        builder.ins().sextend(types::I64, int_val)
                    } else {
                        int_val
                    };
                    write_dst!(builder, insn.destination, result);
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
                    let base_raw = read_src!(builder, insn.sources[0]);
                    let base_u32 = builder.ins().ireduce(types::I32, base_raw);
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
                    } else {
                        let result = match opc {
                            op::I32_LOAD | op::I64_LOAD32_U => {
                                let value = builder.ins().load(types::I32, wasm_memory_flags, address, 0);
                                builder.ins().uextend(types::I64, value)
                            }
                            op::I64_LOAD => builder.ins().load(types::I64, wasm_memory_flags, address, 0),
                            op::I32_LOAD8_S | op::I64_LOAD8_S => {
                                let value = builder.ins().load(types::I8, wasm_memory_flags, address, 0);
                                builder.ins().sextend(types::I64, value)
                            }
                            op::I32_LOAD8_U | op::I64_LOAD8_U => {
                                let value = builder.ins().load(types::I8, wasm_memory_flags, address, 0);
                                builder.ins().uextend(types::I64, value)
                            }
                            op::I32_LOAD16_S | op::I64_LOAD16_S => {
                                let value = builder.ins().load(types::I16, wasm_memory_flags, address, 0);
                                builder.ins().sextend(types::I64, value)
                            }
                            op::I32_LOAD16_U | op::I64_LOAD16_U => {
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
                    let val = if is_f32 {
                        read_src_f32!(builder, insn.sources[0])
                    } else {
                        read_src!(builder, insn.sources[0])
                    };
                    let base_raw = read_src!(builder, insn.sources[1]);
                    let base_u32 = builder.ins().ireduce(types::I32, base_raw);
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
                    let value = if is_f32 {
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
                    write_dst!(builder, insn.destination, result);
                }

                op::MEMORY_GROW => {
                    let pages = read_src!(builder, insn.sources[0]);
                    let pages_i32 = builder.ins().ireduce(types::I32, pages);
                    let mem_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let _xv_config_var = builder.use_var(config_var);
                    let _xc_0 = builder.ins().func_addr(ptr_type, h_mem_grow);
                    let call = builder
                        .ins()
                        .call_indirect(mem_grow_sig, _xc_0, &[_xv_config_var, mem_idx, pages_i32]);
                    let result = builder.inst_results(call)[0];
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                op::MEMORY_COPY => {
                    // imm1 = dst_mem, imm2 = src_mem
                    // sources: [0]=count, [1]=src_offset, [2]=dst_offset
                    let count = read_src!(builder, insn.sources[0]);
                    let src_offset = read_src!(builder, insn.sources[1]);
                    let dst_offset = read_src!(builder, insn.sources[2]);
                    let count_i32 = builder.ins().ireduce(types::I32, count);
                    let src_i32 = builder.ins().ireduce(types::I32, src_offset);
                    let dst_i32 = builder.ins().ireduce(types::I32, dst_offset);
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
                    let count = read_src!(builder, insn.sources[0]);
                    let value = read_src!(builder, insn.sources[1]);
                    let offset = read_src!(builder, insn.sources[2]);
                    let count_i32 = builder.ins().ireduce(types::I32, count);
                    let value_i32 = builder.ins().ireduce(types::I32, value);
                    let offset_i32 = builder.ins().ireduce(types::I32, offset);
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

                // Raw calls use the interpreter stack ABI for arguments and results. Materialize
                // the live vstack only for the duration of the call, then resume using it.
                op::CALL => {
                    let original_top = materialize_vstack_to_real!(builder);
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

                op::CALL_INDIRECT => {
                    let original_top = materialize_vstack_to_real!(builder);
                    let element_index = if insn.sources[0] == STACK_MARKER {
                        debug_assert!(is_unreachable || sp > 0);
                        sp = sp.saturating_sub(1);
                        emit_stack_pop!(builder)
                    } else {
                        read_src!(builder, insn.sources[0])
                    };
                    debug_assert!(is_unreachable || sp >= insn.imm3 as usize);
                    let stack_base = sp.saturating_sub(insn.imm3 as usize);
                    let element_index = builder.ins().ireduce(types::I32, element_index);
                    let type_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let table_idx = builder.ins().iconst(types::I32, insn.imm2);
                    let cfp = builder.ins().func_addr(ptr_type, h_call_indirect);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
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

                opc if (op::SYNTHETIC_CALL_00..=op::SYNTHETIC_CALL_31).contains(&opc) => {
                    let variant = (opc - op::SYNTHETIC_CALL_00) as usize;
                    let param_count = variant / 2;
                    let result_count = variant % 2;
                    let func_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    match param_count {
                        0 => {
                            let cfp = builder.ins().func_addr(ptr_type, h_direct_call_0);
                            do_call_and_check!(builder, call_fn_sig, cfp, &[iv, cv, func_idx]);
                        }
                        1 => {
                            let arg0 = read_src!(builder, insn.sources[0]);
                            let cfp = builder.ins().func_addr(ptr_type, h_direct_call_1);
                            do_call_and_check!(builder, call_fn1_sig, cfp, &[iv, cv, func_idx, arg0]);
                        }
                        2 => {
                            let s0 = read_src!(builder, insn.sources[0]); // last param (top)
                            let s1 = read_src!(builder, insn.sources[1]); // first param
                            let cfp = builder.ins().func_addr(ptr_type, h_direct_call_2);
                            do_call_and_check!(builder, call_fn2_sig, cfp, &[iv, cv, func_idx, s1, s0]);
                        }
                        3 => {
                            let s0 = read_src!(builder, insn.sources[0]); // last param (top)
                            let s1 = read_src!(builder, insn.sources[1]); // middle param
                            let s2 = read_src!(builder, insn.sources[2]); // first param
                            let cfp = builder.ins().func_addr(ptr_type, h_direct_call_3);
                            do_call_and_check!(builder, call_fn3_sig, cfp, &[iv, cv, func_idx, s2, s1, s0]);
                        }
                        _ => unreachable!(),
                    }

                    if result_count > 0 {
                        let cv = builder.use_var(config_var);
                        let result = builder.ins().load(
                            types::I64,
                            MemFlags::trusted(),
                            cv,
                            compiled_call_result_scratch_offset,
                        );
                        write_dst!(builder, insn.destination, result);
                    }
                }

                op::SYNTHETIC_CALL_WITH_RECORD_0 | op::SYNTHETIC_CALL_WITH_RECORD_1 => {
                    let func_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    let null = builder.ins().iconst(ptr_type, 0);
                    let result_count = if opc == op::SYNTHETIC_CALL_WITH_RECORD_1 { 1 } else { 0 };
                    let result_count_arg = builder.ins().iconst(ptr_type, result_count);
                    let target = direct_call_targets
                        .get(&(insn.imm1 as u32))
                        .copied()
                        .expect("direct-call target must be declared");
                    let call = builder
                        .ins()
                        .call(target, &[iv, cv, null, func_idx, result_count_arg, null]);
                    let status = builder.inst_results(call)[0];
                    let trapped = builder.ins().icmp_imm(IntCC::NotEqual, status, 0);
                    let cont = builder.create_block();
                    builder.ins().brif(trapped, trap_block, &[], cont, &[]);
                    builder.switch_to_block(cont);
                    builder.seal_block(cont);
                    if opc == op::SYNTHETIC_CALL_WITH_RECORD_1 {
                        let result = emit_stack_pop!(builder);
                        write_dst!(builder, insn.destination, result);
                    }
                }

                op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_0 | op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_1 => {
                    let element_index = read_src!(builder, insn.sources[0]);
                    let element_index = builder.ins().ireduce(types::I32, element_index);
                    let type_idx = builder.ins().iconst(types::I32, insn.imm1);
                    let table_idx = builder.ins().iconst(types::I32, insn.imm2);
                    let cwp = builder.ins().func_addr(ptr_type, h_call_indirect_wr);
                    let iv = builder.use_var(interp_var);
                    let cv = builder.use_var(config_var);
                    do_call_and_check!(
                        builder,
                        call_indirect_sig,
                        cwp,
                        &[iv, cv, table_idx, type_idx, element_index]
                    );
                    if opc == op::SYNTHETIC_CALL_INDIRECT_WITH_RECORD_1 {
                        let cv2 = builder.use_var(config_var);
                        let result = builder.ins().load(
                            types::I64,
                            MemFlags::trusted(),
                            cv2,
                            compiled_call_result_scratch_offset,
                        );
                        write_dst!(builder, insn.destination, result);
                    }
                }

                op::SYNTHETIC_LOCAL_SETI32_CONST | op::SYNTHETIC_LOCAL_SETI64_CONST => {
                    let val = builder.ins().iconst(types::I64, insn.imm1);
                    write_local_inline!(builder, insn.imm2, val);
                }

                op::SYNTHETIC_I32_ADD2LOCAL => {
                    let r1 = read_local_inline!(builder, insn.imm1);
                    let v1 = builder.ins().ireduce(types::I32, r1);
                    let r2 = read_local_inline!(builder, insn.imm2);
                    let v2 = builder.ins().ireduce(types::I32, r2);
                    let result = builder.ins().iadd(v1, v2);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_ADDCONSTLOCAL => {
                    let r = read_local_inline!(builder, insn.imm2);
                    let v = builder.ins().ireduce(types::I32, r);
                    let k = builder.ins().iconst(types::I32, insn.imm1);
                    let result = builder.ins().iadd(v, k);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                op::SYNTHETIC_I32_ANDCONSTLOCAL => {
                    let r = read_local_inline!(builder, insn.imm2);
                    let v = builder.ins().ireduce(types::I32, r);
                    let k = builder.ins().iconst(types::I32, insn.imm1);
                    let result = builder.ins().band(v, k);
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
                }

                opc if (op::SYNTHETIC_I32_SUB2LOCAL..=op::SYNTHETIC_I32_SHRS2LOCAL).contains(&opc) => {
                    let r1 = read_local_inline!(builder, insn.imm1);
                    let v1 = builder.ins().ireduce(types::I32, r1);
                    let r2 = read_local_inline!(builder, insn.imm2);
                    let v2 = builder.ins().ireduce(types::I32, r2);
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
                    let result = builder.ins().sextend(types::I64, result);
                    write_dst!(builder, insn.destination, result);
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
                    let base_raw = read_src!(builder, insn.sources[0]);
                    let val = read_local_inline!(builder, insn.imm2);
                    let base_u32 = builder.ins().ireduce(types::I32, base_raw);
                    let base_u64 = builder.ins().uextend(types::I64, base_u32);
                    let offset = builder.ins().iconst(types::I64, insn.imm1);
                    let addr = builder.ins().iadd(base_u64, offset);

                    let mem_idx = insn.imm3;
                    let address = inline_memory_address!(builder, mem_idx, addr);
                    let value = if opc == op::SYNTHETIC_I32_STORELOCAL {
                        builder.ins().ireduce(types::I32, val)
                    } else {
                        val
                    };
                    builder.ins().store(wasm_memory_flags, value, address, 0);
                }

                op::SYNTHETIC_TIER_UP => {
                    if let Some(tail) = tier_up_dispatch_tail {
                        let header = control_stack
                            .last()
                            .expect("tier-up checkpoint must be inside a loop")
                            .branch_target;
                        let next_tail = builder.create_block();
                        builder.switch_to_block(tail);
                        let matches = builder.ins().icmp_imm(IntCC::Equal, tier_up_target_ip, insn.imm1);
                        builder.ins().brif(matches, header, &[], next_tail, &[]);
                        builder.seal_block(tail);
                        tier_up_dispatch_tail = Some(next_tail);
                        builder.switch_to_block(header);
                        // The tier-up dispatch jumps straight into this header with regs loaded from config (i64 bank),
                        // so we can't trust any F32/F64 vars to survive across this jump.
                        reset_banks!();
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
                let saved_locals = $builder.use_var(saved_locals_base_var);
                let saved_call_record = $builder.use_var(saved_call_record_base_var);
                let saved_call_record_top = $builder.use_var(saved_call_record_top_var);
                let saved_expression = $builder.use_var(saved_expression_var);
                let saved_depth = $builder.use_var(saved_depth_var);
                $builder
                    .ins()
                    .store(MemFlags::trusted(), saved_locals, cfg, locals_base_offset);
                $builder.ins().store(
                    MemFlags::trusted(),
                    saved_call_record,
                    cfg,
                    call_record_base_offset,
                );
                $builder.ins().store(
                    MemFlags::trusted(),
                    saved_call_record_top,
                    cfg,
                    call_record_stack_top_offset,
                );
                $builder.ins().store(
                    MemFlags::trusted(),
                    saved_expression,
                    cfg,
                    current_expression_offset,
                );
                $builder
                    .ins()
                    .store(MemFlags::trusted(), saved_depth, cfg, depth_offset);
            }};
        }

        builder.switch_to_block(trap_block);
        builder.seal_block(trap_block);
        // The helper already set the trap for us.
        let direct_trap_return = builder.create_block();
        let handler_trap_return = builder.create_block();
        let direct_call_mode = builder.use_var(direct_call_mode_var);
        builder
            .ins()
            .brif(direct_call_mode, direct_trap_return, &[], handler_trap_return, &[]);

        builder.switch_to_block(direct_trap_return);
        builder.seal_block(direct_trap_return);
        restore_direct_call_context!(builder);
        let direct_trap_ret = builder.ins().iconst(types::I64, 1);
        builder.ins().return_(&[direct_trap_ret]);

        builder.switch_to_block(handler_trap_return);
        builder.seal_block(handler_trap_return);
        let handler_trap_ret = builder.ins().iconst(types::I64, outcome_return_value as i64);
        builder.ins().return_(&[handler_trap_ret]);

        builder.switch_to_block(epilogue_block);
        builder.seal_block(epilogue_block);
        // Clean up excess values on the real stack (e.g. from BR out of nested blocks); nothing to touch if we have vstack info.
        if has_raw_call {
            let init_size = builder.use_var(initial_stack_size_var);
            emit_stack_cleanup!(builder, init_size, result_arity);
        }
        Self::sync_regs_to_config(
            &mut builder,
            &reg_vars,
            config_var,
            regs_offset,
            value_size,
            &dirty_regs,
        );
        flush_locals!(builder);
        let direct_return = builder.create_block();
        let handler_return = builder.create_block();
        let direct_call_mode = builder.use_var(direct_call_mode_var);
        builder
            .ins()
            .brif(direct_call_mode, direct_return, &[], handler_return, &[]);

        builder.switch_to_block(direct_return);
        builder.seal_block(direct_return);
        restore_direct_call_context!(builder);
        let direct_ret = builder.ins().iconst(types::I64, 0);
        builder.ins().return_(&[direct_ret]);

        builder.switch_to_block(handler_return);
        builder.seal_block(handler_return);
        let handler_ret = builder.ins().iconst(types::I64, outcome_return_value as i64);
        builder.ins().return_(&[handler_ret]);

        builder.finalize();

        let body = Self::compile_function(&*isa, func)?;

        let mut adapter =
            Function::with_name_signature(UserFuncName::user(1, function_index), handler_signature.clone());
        let mut adapter_builder_context = FunctionBuilderContext::new();
        let mut adapter_builder = FunctionBuilder::new(&mut adapter, &mut adapter_builder_context);
        let adapter_entry = adapter_builder.create_block();
        adapter_builder.append_block_params_for_function_params(adapter_entry);
        adapter_builder.switch_to_block(adapter_entry);
        adapter_builder.seal_block(adapter_entry);
        let adapter_params = adapter_builder.block_params(adapter_entry).to_vec();
        let body_signature = adapter_builder.import_signature(handler_signature);
        let body_name = adapter_builder.func.declare_imported_user_function(UserExternalName {
            namespace: WASM_FUNCTION_EXTERNAL_NAMESPACE,
            index: function_index,
        });
        let body_function = adapter_builder.func.import_function(ExtFuncData {
            name: ExternalName::user(body_name),
            signature: body_signature,
            colocated: true,
        });
        let call = adapter_builder.ins().call(body_function, &adapter_params);
        let results = adapter_builder.inst_results(call).to_vec();
        adapter_builder.ins().return_(&results);
        adapter_builder.finalize();
        let adapter = Self::compile_function(&*isa, adapter)?;

        let native_entry_offset = adapter.code.len().div_ceil(16) * 16;
        let native_entry_offset = u32::try_from(native_entry_offset).map_err(|_| "native entry offset overflow")?;
        let mut code = adapter.code;
        code.resize(native_entry_offset as usize, 0);
        code.extend_from_slice(&body.code);

        let mut relocs = adapter.relocs;
        relocs.reserve(body.relocs.len());
        for mut relocation in body.relocs {
            relocation.code_offset = relocation
                .code_offset
                .checked_add(native_entry_offset)
                .ok_or("relocation offset overflow")?;
            relocs.push(relocation);
        }

        let mut traps = adapter.traps;
        traps.reserve(body.traps.len());
        for mut trap in body.traps {
            trap.offset = trap
                .offset
                .checked_add(native_entry_offset)
                .ok_or("trap offset overflow")?;
            traps.push(trap);
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

    fn sync_regs_to_config(
        builder: &mut FunctionBuilder,
        reg_vars: &[Variable; REG_COUNT],
        config_var: Variable,
        regs_offset: i32,
        value_size: i32,
        dirty: &[bool; REG_COUNT],
    ) {
        let config = builder.use_var(config_var);
        for i in 0..REG_COUNT {
            if !dirty[i] {
                continue;
            }
            let val = builder.use_var(reg_vars[i]);
            let offset = regs_offset + (i as i32) * value_size;
            builder.ins().store(MemFlags::trusted(), val, config, offset);
            let zero = builder.ins().iconst(types::I64, 0);
            builder.ins().store(MemFlags::trusted(), zero, config, offset + 8);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn insn(opcode: u64) -> CraneliftInsn {
        CraneliftInsn {
            opcode,
            sources: [0; 3],
            destination: 0,
            imm1: 0,
            imm2: 0,
            imm3: 0,
            call_result_count: 0,
        }
    }

    fn local_insn(opcode: u64, local_index: i64) -> CraneliftInsn {
        let mut insn = insn(opcode);
        insn.imm1 = local_index;
        insn
    }

    #[test]
    fn serializes_helper_and_wasm_function_relocations() {
        let helper = UserExternalName {
            namespace: HELPER_EXTERNAL_NAMESPACE,
            index: HelperId::memory_size as u32,
        };
        let helper_relocation = CraneliftCompiler::serialize_relocation(Reloc::Abs8, 12, -4, &helper).unwrap();
        assert_eq!(helper_relocation.code_offset, 12);
        assert_eq!(helper_relocation.kind, CraneliftRelocationKind::Abs8);
        assert_eq!(helper_relocation.target_kind, CraneliftRelocationTargetKind::Helper);
        assert_eq!(helper_relocation.target_index, HelperId::memory_size as u32);
        assert_eq!(helper_relocation.addend, -4);

        let wasm_function = UserExternalName {
            namespace: WASM_FUNCTION_EXTERNAL_NAMESPACE,
            index: 42,
        };
        let direct_call = CraneliftCompiler::serialize_relocation(Reloc::Arm64Call, 24, 0, &wasm_function).unwrap();
        assert_eq!(direct_call.kind, CraneliftRelocationKind::Arm64Call);
        assert_eq!(direct_call.target_kind, CraneliftRelocationTargetKind::WasmFunction);
        assert_eq!(direct_call.target_index, 42);

        let x86_direct_call =
            CraneliftCompiler::serialize_relocation(Reloc::X86CallPCRel4, 32, -4, &wasm_function).unwrap();
        assert_eq!(x86_direct_call.kind, CraneliftRelocationKind::X86CallPCRel4);
        assert_eq!(x86_direct_call.addend, -4);

        assert!(CraneliftCompiler::serialize_relocation(Reloc::Arm64Call, 0, 0, &helper).is_err());
        assert!(
            CraneliftCompiler::serialize_relocation(
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
        assert!(
            CraneliftCompiler::serialize_relocation(Reloc::Abs8, 0, 0, &UserExternalName { namespace: 2, index: 0 },)
                .is_err()
        );
    }

    #[test]
    fn local_liveness_merges_if_branches() {
        let insns = [
            insn(op::IF),
            local_insn(op::LOCAL_SET, 0),
            insn(op::ELSE),
            local_insn(op::LOCAL_SET, 1),
            insn(op::END),
            local_insn(op::LOCAL_GET, 0),
            local_insn(op::LOCAL_GET, 1),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 2).unwrap();
        let then_block = liveness.block_at(1);
        assert!(!then_block.live_in.contains(0));
        assert!(then_block.live_in.contains(1));
        assert!(then_block.live_out.contains(0));
        assert!(then_block.live_out.contains(1));

        let else_block = liveness.block_at(3);
        assert!(else_block.live_in.contains(0));
        assert!(!else_block.live_in.contains(1));
        assert!(else_block.live_out.contains(0));
        assert!(else_block.live_out.contains(1));
    }

    #[test]
    fn local_liveness_reaches_loop_backedge() {
        let mut branch = insn(op::BR_IF);
        branch.imm1 = 0;
        let insns = [
            local_insn(op::LOCAL_SET, 0),
            insn(op::LOOP),
            local_insn(op::LOCAL_GET, 0),
            local_insn(op::LOCAL_SET, 0),
            branch,
            insn(op::END),
            local_insn(op::LOCAL_GET, 0),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 1).unwrap();
        let loop_block = liveness.block_at(2);
        assert!(loop_block.live_in.contains(0));
        assert!(loop_block.live_out.contains(0));
    }

    #[test]
    fn local_liveness_reaches_outer_branch_target() {
        let mut branch = insn(op::BR);
        branch.imm1 = 1;
        let insns = [
            insn(op::BLOCK),
            local_insn(op::LOCAL_SET, 0),
            insn(op::LOOP),
            branch,
            insn(op::END),
            insn(op::END),
            local_insn(op::LOCAL_GET, 0),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 1).unwrap();
        assert!(liveness.block_at(3).live_out.contains(0));
    }

    #[test]
    fn local_liveness_unions_branch_table_targets() {
        let mut branch_table = insn(op::BR_TABLE);
        branch_table.imm1 = 0;
        branch_table.imm3 = 1 | (1 << 8);
        let insns = [
            insn(op::BLOCK),
            insn(op::LOOP),
            local_insn(op::LOCAL_SET, 0),
            branch_table,
            insn(op::END),
            insn(op::END),
            local_insn(op::LOCAL_GET, 0),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 1).unwrap();
        let branch_table_block = liveness.block_at(3);
        assert!(branch_table_block.live_out.contains(0));
        assert!(branch_table_block.has_branch_table_successors);
    }

    #[test]
    fn edge_cache_selects_single_predecessor_forward_edges() {
        let insns = [
            local_insn(op::LOCAL_SET, 0),
            insn(op::IF),
            local_insn(op::LOCAL_GET, 0),
            insn(op::ELSE),
            local_insn(op::LOCAL_GET, 0),
            insn(op::END),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 1).unwrap();
        let edge_cached_locals = CraneliftCompiler::select_locals_for_edge_cache(&liveness, &[true]);
        assert!(edge_cached_locals[liveness.block_index_at(2)][0]);
        assert!(edge_cached_locals[liveness.block_index_at(4)][0]);
    }

    #[test]
    fn edge_cache_selects_fully_defined_control_flow_joins() {
        let insns = [
            insn(op::IF),
            local_insn(op::LOCAL_SET, 0),
            insn(op::ELSE),
            local_insn(op::LOCAL_SET, 0),
            insn(op::END),
            local_insn(op::LOCAL_GET, 0),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 1).unwrap();
        let edge_cached_locals = CraneliftCompiler::select_locals_for_edge_cache(&liveness, &[true]);
        assert!(edge_cached_locals[liveness.block_index_at(5)][0]);
    }

    #[test]
    fn edge_cache_rejects_partially_defined_control_flow_joins() {
        let insns = [
            insn(op::IF),
            local_insn(op::LOCAL_SET, 0),
            insn(op::ELSE),
            insn(op::NOP),
            insn(op::END),
            local_insn(op::LOCAL_GET, 0),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 1).unwrap();
        let edge_cached_locals = CraneliftCompiler::select_locals_for_edge_cache(&liveness, &[true]);
        assert!(!edge_cached_locals[liveness.block_index_at(5)][0]);
    }

    #[test]
    fn edge_cache_rejects_loop_headers() {
        let insns = [
            local_insn(op::LOCAL_SET, 0),
            insn(op::LOOP),
            local_insn(op::LOCAL_GET, 0),
            insn(op::END),
            insn(op::SYNTHETIC_END_EXPRESSION),
        ];

        let liveness = CraneliftCompiler::analyze_local_liveness(&insns, 1).unwrap();
        let edge_cached_locals = CraneliftCompiler::select_locals_for_edge_cache(&liveness, &[true]);
        assert!(!edge_cached_locals[liveness.block_index_at(2)][0]);
    }
}
