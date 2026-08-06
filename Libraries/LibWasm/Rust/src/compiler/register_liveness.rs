/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::REG_COUNT;
use super::op;
use crate::CraneliftInsn;

#[cfg(test)]
use super::STACK_MARKER;

pub(super) type RegisterSet = u8;

#[derive(Clone, Copy, PartialEq, Eq)]
enum RegionKind {
    Block,
    Loop,
    If,
}

struct StructuredRegion {
    kind: RegionKind,
    instruction: usize,
    else_instruction: Option<usize>,
    end_instruction: Option<usize>,
}

#[derive(Default)]
struct ParsedInstruction {
    entered_region: Option<usize>,
    else_region: Option<usize>,
    ended_region: Option<usize>,
    branch_target_regions: Vec<usize>,
}

// The serialized branch immediates are structured-control label depths rather than instruction
// indices. Resolve them before building the ordinary predecessor/successor graph used by liveness.
struct ParsedControlFlow {
    regions: Vec<StructuredRegion>,
    instructions: Vec<ParsedInstruction>,
}

impl ParsedControlFlow {
    fn branch_table_labels(insns: &[CraneliftInsn], instruction: usize) -> Result<Vec<usize>, &'static str> {
        let insn = &insns[instruction];
        let inline_count = (insn.imm3 & 0xff) as usize;
        if inline_count == 0xff {
            return Err("br_table too large for inline encoding");
        }

        let mut labels = Vec::with_capacity(inline_count + 1);
        for index in 0..inline_count {
            let packed = if index < 4 { insn.imm1 as u64 } else { insn.imm2 as u64 };
            labels.push(((packed >> ((index % 4) * 16)) & 0xffff) as usize);
        }

        let mut continuation = instruction + 1;
        while continuation < insns.len() && insns[continuation].opcode == op::SYNTHETIC_BR_TABLE_CONT {
            let insn = &insns[continuation];
            let count = (insn.imm3 & 0xff) as usize;
            for index in 0..count {
                let packed = if index < 4 { insn.imm1 as u64 } else { insn.imm2 as u64 };
                labels.push(((packed >> ((index % 4) * 16)) & 0xffff) as usize);
            }
            continuation += 1;
        }

        labels.push(((insn.imm3 >> 8) & 0xffff) as usize);
        Ok(labels)
    }

    fn resolve_label(label: usize, control_stack: &[usize]) -> Result<Option<usize>, &'static str> {
        if label < control_stack.len() {
            Ok(Some(control_stack[control_stack.len() - label - 1]))
        } else if label == control_stack.len() {
            // The implicit outer label returns from the function and has no instruction target.
            Ok(None)
        } else {
            Err("branch label is outside the control stack")
        }
    }

    fn new(insns: &[CraneliftInsn]) -> Result<Self, &'static str> {
        let mut parsed = Self {
            regions: Vec::new(),
            instructions: (0..insns.len()).map(|_| ParsedInstruction::default()).collect(),
        };
        let mut control_stack = Vec::new();

        for (instruction, insn) in insns.iter().enumerate() {
            match insn.opcode {
                op::BLOCK | op::LOOP | op::IF => {
                    let kind = match insn.opcode {
                        op::BLOCK => RegionKind::Block,
                        op::LOOP => RegionKind::Loop,
                        op::IF => RegionKind::If,
                        _ => unreachable!(),
                    };
                    let region = parsed.regions.len();
                    parsed.regions.push(StructuredRegion {
                        kind,
                        instruction,
                        else_instruction: None,
                        end_instruction: None,
                    });
                    parsed.instructions[instruction].entered_region = Some(region);
                    control_stack.push(region);
                }
                op::ELSE => {
                    let region = *control_stack.last().ok_or("else without a control frame")?;
                    if parsed.regions[region].kind != RegionKind::If
                        || parsed.regions[region].else_instruction.is_some()
                    {
                        return Err("else outside an if instruction");
                    }
                    parsed.regions[region].else_instruction = Some(instruction);
                    parsed.instructions[instruction].else_region = Some(region);
                }
                op::END => {
                    if let Some(region) = control_stack.pop() {
                        parsed.regions[region].end_instruction = Some(instruction);
                        parsed.instructions[instruction].ended_region = Some(region);
                    }
                }
                op::BR | op::SYNTHETIC_BR_NOSTACK | op::BR_IF | op::SYNTHETIC_BR_IF_NOSTACK => {
                    let label = usize::try_from(insn.imm1).map_err(|_| "invalid branch label")?;
                    if let Some(region) = Self::resolve_label(label, &control_stack)? {
                        parsed.instructions[instruction].branch_target_regions.push(region);
                    }
                }
                op::BR_TABLE => {
                    for label in Self::branch_table_labels(insns, instruction)? {
                        if let Some(region) = Self::resolve_label(label, &control_stack)? {
                            parsed.instructions[instruction].branch_target_regions.push(region);
                        }
                    }
                }
                _ => {}
            }
        }

        if !control_stack.is_empty() {
            return Err("unterminated structured control instruction");
        }
        Ok(parsed)
    }

    fn branch_target(&self, region: usize) -> Result<usize, &'static str> {
        let region = &self.regions[region];
        if region.kind == RegionKind::Loop {
            Ok(region.instruction + 1)
        } else {
            Ok(region
                .end_instruction
                .ok_or("unterminated structured control instruction")?
                + 1)
        }
    }
}

#[derive(Default)]
struct InstructionEdges {
    successors: Vec<usize>,
}

struct InstructionControlFlow {
    instructions: Vec<InstructionEdges>,
    branch_target_instruction: Vec<Option<usize>>,
}

impl InstructionControlFlow {
    fn add_successor(instructions: &mut [InstructionEdges], instruction: usize, successor: usize) {
        if successor < instructions.len() && !instructions[instruction].successors.contains(&successor) {
            instructions[instruction].successors.push(successor);
        }
    }

    fn new(insns: &[CraneliftInsn]) -> Result<Self, &'static str> {
        let parsed = ParsedControlFlow::new(insns)?;
        let mut instructions = (0..insns.len())
            .map(|_| InstructionEdges::default())
            .collect::<Vec<_>>();

        for (instruction, insn) in insns.iter().enumerate() {
            match insn.opcode {
                op::UNREACHABLE | op::RETURN | op::SYNTHETIC_END_EXPRESSION | op::SYNTHETIC_BR_TABLE_CONT => {}
                op::BR | op::SYNTHETIC_BR_NOSTACK | op::BR_TABLE => {
                    for &region in &parsed.instructions[instruction].branch_target_regions {
                        Self::add_successor(&mut instructions, instruction, parsed.branch_target(region)?);
                    }
                }
                op::BR_IF | op::SYNTHETIC_BR_IF_NOSTACK => {
                    for &region in &parsed.instructions[instruction].branch_target_regions {
                        Self::add_successor(&mut instructions, instruction, parsed.branch_target(region)?);
                    }
                    Self::add_successor(&mut instructions, instruction, instruction + 1);
                }
                op::IF => {
                    let region = parsed.instructions[instruction]
                        .entered_region
                        .ok_or("missing if control region")?;
                    Self::add_successor(&mut instructions, instruction, instruction + 1);
                    let false_target = if let Some(else_instruction) = parsed.regions[region].else_instruction {
                        else_instruction + 1
                    } else {
                        parsed.branch_target(region)?
                    };
                    Self::add_successor(&mut instructions, instruction, false_target);
                }
                op::ELSE => {
                    let region = parsed.instructions[instruction]
                        .else_region
                        .ok_or("missing else control region")?;
                    Self::add_successor(&mut instructions, instruction, parsed.branch_target(region)?);
                }
                op::END if parsed.instructions[instruction].ended_region.is_none() => {}
                _ => Self::add_successor(&mut instructions, instruction, instruction + 1),
            }
        }

        let mut branch_target_instruction = vec![None; insns.len()];
        for (region_index, region) in parsed.regions.iter().enumerate() {
            branch_target_instruction[region.instruction] = Some(parsed.branch_target(region_index)?);
        }

        Ok(Self {
            instructions,
            branch_target_instruction,
        })
    }
}

pub(super) struct RegisterLiveness {
    branch_target_live: Vec<Option<RegisterSet>>,
}

impl RegisterLiveness {
    fn instruction_registers(insn: &CraneliftInsn) -> Result<(RegisterSet, RegisterSet), &'static str> {
        let (input_count, output_count) = op::operand_counts(insn.opcode).ok_or("missing opcode operand counts")?;
        let input_count = if input_count < 0 {
            // Raw calls consume their arguments from the virtual stack. The indirect-call table
            // index is the only dynamic-arity operand carried in the register-source array.
            if insn.opcode == op::CALL_INDIRECT { 1 } else { 0 }
        } else {
            (input_count as usize).min(insn.sources.len())
        };

        let mut uses = 0;
        for &source in &insn.sources[..input_count] {
            if usize::from(source) < REG_COUNT {
                uses |= 1 << source;
            }
        }

        let has_destination = output_count > 0 || (output_count < 0 && insn.call_result_count > 0);
        let definitions = if has_destination && usize::from(insn.destination) < REG_COUNT {
            1 << insn.destination
        } else {
            0
        };
        Ok((uses, definitions))
    }

    pub(super) fn analyze(insns: &[CraneliftInsn]) -> Result<Self, &'static str> {
        let control_flow = InstructionControlFlow::new(insns)?;

        let mut uses = Vec::with_capacity(insns.len());
        let mut definitions = Vec::with_capacity(insns.len());
        for insn in insns {
            let (instruction_uses, instruction_definitions) = Self::instruction_registers(insn)?;
            uses.push(instruction_uses);
            definitions.push(instruction_definitions);
        }

        // live-in = uses union (live-out - definitions). Iterate to a fixed point so loop
        // back-edges propagate their uses to the header before lowering chooses register banks.
        let mut live_before = vec![0; insns.len()];
        loop {
            let mut changed = false;
            for instruction in (0..insns.len()).rev() {
                let live_after = control_flow.instructions[instruction]
                    .successors
                    .iter()
                    .fold(0, |live, &successor| live | live_before[successor]);
                let new_live_before = uses[instruction] | (live_after & !definitions[instruction]);
                if live_before[instruction] != new_live_before {
                    live_before[instruction] = new_live_before;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }

        let branch_target_live = control_flow
            .branch_target_instruction
            .iter()
            .map(|target| target.map(|target| live_before.get(target).copied().unwrap_or_default()))
            .collect();

        Ok(Self { branch_target_live })
    }

    pub(super) fn branch_target_live(&self, instruction: usize) -> RegisterSet {
        self.branch_target_live
            .get(instruction)
            .copied()
            .flatten()
            .unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn instruction(opcode: u64) -> CraneliftInsn {
        CraneliftInsn {
            opcode,
            sources: [STACK_MARKER; 3],
            destination: STACK_MARKER,
            imm1: 0,
            imm2: 0,
            imm3: 0,
            call_result_count: 0,
            call_type_encoding: 0,
        }
    }

    fn register(index: u8) -> RegisterSet {
        1 << index
    }

    #[test]
    fn tracks_structured_control_flow() {
        let mut invariant = instruction(op::I32_CONST);
        invariant.destination = 0;

        let mut loop_temporary = instruction(op::I32_CONST);
        loop_temporary.destination = 1;

        let mut consume_temporary = instruction(op::I32_ADD);
        consume_temporary.sources = [1, 1, STACK_MARKER];
        consume_temporary.destination = 2;

        let mut back_edge = instruction(op::BR_IF);
        back_edge.sources[0] = 2;

        let mut consume_invariant = instruction(op::DROP);
        consume_invariant.sources[0] = 0;

        let liveness = RegisterLiveness::analyze(&[
            invariant,
            instruction(op::LOOP),
            loop_temporary,
            consume_temporary,
            back_edge,
            instruction(op::END),
            consume_invariant,
            instruction(op::SYNTHETIC_END_EXPRESSION),
        ])
        .unwrap();

        assert_eq!(liveness.branch_target_live(1), register(0));
    }

    #[test]
    fn tracks_if_merge_values() {
        let mut if_instruction = instruction(op::IF);
        if_instruction.sources[0] = 0;

        let mut then_value = instruction(op::I32_CONST);
        then_value.destination = 1;

        let mut else_value = instruction(op::I32_CONST);
        else_value.destination = 1;

        let mut consume_merged_value = instruction(op::DROP);
        consume_merged_value.sources[0] = 1;

        let liveness = RegisterLiveness::analyze(&[
            if_instruction,
            then_value,
            instruction(op::ELSE),
            else_value,
            instruction(op::END),
            consume_merged_value,
            instruction(op::SYNTHETIC_END_EXPRESSION),
        ])
        .unwrap();

        assert_eq!(liveness.branch_target_live(0), register(1));
    }

    #[test]
    fn reaches_a_loop_fixed_point() {
        let mut update = instruction(op::I32_ADD);
        update.sources = [0, 1, STACK_MARKER];
        update.destination = 0;

        let mut back_edge = instruction(op::BR_IF);
        back_edge.sources[0] = 0;

        let liveness = RegisterLiveness::analyze(&[
            instruction(op::LOOP),
            update,
            back_edge,
            instruction(op::END),
            instruction(op::SYNTHETIC_END_EXPRESSION),
        ])
        .unwrap();

        assert_eq!(liveness.branch_target_live(0), register(0) | register(1));
    }

    #[test]
    fn follows_br_table_continuations() {
        let mut branch_table = instruction(op::BR_TABLE);
        branch_table.sources[0] = 0;
        branch_table.imm3 = 8 | (1 << 8);

        let mut continuation = instruction(op::SYNTHETIC_BR_TABLE_CONT);
        continuation.imm3 = 1;

        let liveness = RegisterLiveness::analyze(&[
            instruction(op::LOOP),
            branch_table,
            continuation,
            instruction(op::END),
            instruction(op::SYNTHETIC_END_EXPRESSION),
        ])
        .unwrap();

        assert_eq!(liveness.branch_target_live(0), register(0));
    }
}
