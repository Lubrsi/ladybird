/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/GenericShorthands.h>
#include <LibWasm/AbstractMachine/TierUpCheckpointLiveness.h>
#include <LibWasm/Types.h>

namespace Wasm {

namespace {

enum class RegionKind {
    Block,
    Loop,
    If,
};

struct StructuredRegion {
    RegionKind kind;
    size_t instruction_index;
    Optional<size_t> else_instruction_index;
    Optional<size_t> end_instruction_index;
};

struct ParsedInstruction {
    Optional<size_t> entered_region;
    Optional<size_t> else_region;
    Vector<size_t, 2> branch_target_regions;
};

class ParsedControlFlow {
public:
    static Optional<ParsedControlFlow> create(Expression const& expression)
    {
        ParsedControlFlow control_flow;
        control_flow.m_instructions.resize(expression.instructions().size());
        Vector<size_t> control_stack;

        for (size_t instruction_index = 0; instruction_index < expression.instructions().size(); ++instruction_index) {
            auto const& instruction = expression.instructions()[instruction_index];
            auto const opcode = instruction.opcode();

            if (opcode == Instructions::try_table)
                return {};

            if (first_is_one_of(opcode, Instructions::block, Instructions::loop, Instructions::if_)) {
                auto kind = RegionKind::If;
                if (opcode == Instructions::block)
                    kind = RegionKind::Block;
                else if (opcode == Instructions::loop)
                    kind = RegionKind::Loop;
                auto const region_index = control_flow.m_regions.size();
                control_flow.m_regions.append({ kind, instruction_index, {}, {} });
                control_flow.m_instructions[instruction_index].entered_region = region_index;
                control_stack.append(region_index);
                continue;
            }

            if (opcode == Instructions::structured_else) {
                VERIFY(!control_stack.is_empty());
                auto const region_index = control_stack.last();
                VERIFY(control_flow.m_regions[region_index].kind == RegionKind::If);
                VERIFY(!control_flow.m_regions[region_index].else_instruction_index.has_value());
                control_flow.m_regions[region_index].else_instruction_index = instruction_index;
                control_flow.m_instructions[instruction_index].else_region = region_index;
                continue;
            }

            if (opcode == Instructions::structured_end) {
                VERIFY(!control_stack.is_empty());
                auto const region_index = control_stack.take_last();
                control_flow.m_regions[region_index].end_instruction_index = instruction_index;
                continue;
            }

            auto append_branch_target = [&](LabelIndex label) {
                if (label.value() == control_stack.size())
                    return;
                VERIFY(label.value() < control_stack.size());
                auto const region_index = control_stack[control_stack.size() - label.value() - 1];
                control_flow.m_instructions[instruction_index].branch_target_regions.append(region_index);
            };

            if (first_is_one_of(opcode, Instructions::br, Instructions::br_if, Instructions::br_on_null, Instructions::br_on_non_null)) {
                append_branch_target(instruction.arguments().get<Instruction::BranchArgs>().label);
            } else if (first_is_one_of(opcode, Instructions::br_on_cast, Instructions::br_on_cast_fail)) {
                append_branch_target(instruction.arguments().get<Instruction::BranchOnCastArgs>().branch.label);
            } else if (opcode == Instructions::br_table) {
                auto const& arguments = instruction.arguments().get<Instruction::TableBranchArgs>();
                for (auto label : arguments.labels)
                    append_branch_target(label);
                append_branch_target(arguments.default_);
            }
        }

        VERIFY(control_stack.is_empty());
        return control_flow;
    }

    size_t branch_target(size_t region_index) const
    {
        auto const& region = m_regions[region_index];
        if (region.kind == RegionKind::Loop)
            return region.instruction_index + 1;
        return region.end_instruction_index.value() + 1;
    }

    ParsedInstruction const& instruction(size_t instruction_index) const
    {
        return m_instructions[instruction_index];
    }

    StructuredRegion const& region(size_t region_index) const
    {
        return m_regions[region_index];
    }

private:
    Vector<StructuredRegion> m_regions;
    Vector<ParsedInstruction> m_instructions;
};

struct InstructionEdges {
    Vector<size_t, 2> successors;
};

class InstructionControlFlow {
public:
    static Optional<InstructionControlFlow> create(Expression const& expression)
    {
        auto parsed = ParsedControlFlow::create(expression);
        if (!parsed.has_value())
            return {};

        InstructionControlFlow control_flow;
        control_flow.m_instructions.resize(expression.instructions().size());

        auto add_successor = [&](size_t instruction_index, size_t successor) {
            if (successor >= control_flow.m_instructions.size())
                return;
            auto& successors = control_flow.m_instructions[instruction_index].successors;
            if (!successors.contains_slow(successor))
                successors.append(successor);
        };

        for (size_t instruction_index = 0; instruction_index < expression.instructions().size(); ++instruction_index) {
            auto const& instruction = expression.instructions()[instruction_index];
            auto const opcode = instruction.opcode();
            auto const& parsed_instruction = parsed->instruction(instruction_index);

            if (first_is_one_of(opcode,
                    Instructions::unreachable,
                    Instructions::return_,
                    Instructions::return_call,
                    Instructions::return_call_indirect,
                    Instructions::return_call_ref,
                    Instructions::throw_,
                    Instructions::throw_ref,
                    Instructions::synthetic_end_expression)) {
                continue;
            }

            if (first_is_one_of(opcode, Instructions::br, Instructions::br_table)) {
                for (auto region_index : parsed_instruction.branch_target_regions)
                    add_successor(instruction_index, parsed->branch_target(region_index));
                continue;
            }

            if (first_is_one_of(opcode,
                    Instructions::br_if,
                    Instructions::br_on_null,
                    Instructions::br_on_non_null,
                    Instructions::br_on_cast,
                    Instructions::br_on_cast_fail)) {
                for (auto region_index : parsed_instruction.branch_target_regions)
                    add_successor(instruction_index, parsed->branch_target(region_index));
                add_successor(instruction_index, instruction_index + 1);
                continue;
            }

            if (opcode == Instructions::if_) {
                auto const region_index = parsed_instruction.entered_region.value();
                auto const& region = parsed->region(region_index);
                add_successor(instruction_index, instruction_index + 1);
                add_successor(
                    instruction_index,
                    region.else_instruction_index.has_value()
                        ? region.else_instruction_index.value() + 1
                        : parsed->branch_target(region_index));
                continue;
            }

            if (opcode == Instructions::structured_else) {
                add_successor(instruction_index, parsed->branch_target(parsed_instruction.else_region.value()));
                continue;
            }

            add_successor(instruction_index, instruction_index + 1);
        }

        return control_flow;
    }

    ReadonlySpan<size_t> successors(size_t instruction_index) const
    {
        return m_instructions[instruction_index].successors.span();
    }

private:
    Vector<InstructionEdges> m_instructions;
};

}

ErrorOr<bool> populate_tier_up_checkpoint_live_locals(Expression const& expression, size_t local_count)
{
    auto& checkpoints = expression.compiled_instructions.tier_up_checkpoints;
    if (checkpoints.is_empty())
        return true;

    auto control_flow = InstructionControlFlow::create(expression);
    if (!control_flow.has_value())
        return false;

    if (local_count == 0)
        return true;

    auto const word_count = ceil_div(local_count, static_cast<size_t>(64));
    Checked<size_t> total_word_count { expression.instructions().size() };
    total_word_count *= word_count;
    if (total_word_count.has_overflow())
        return Error::from_string_literal("tier-up local liveness size overflow");

    Vector<u64> live_before;
    TRY(live_before.try_resize(total_word_count.value()));
    live_before.fill(0);

    bool changed;
    do {
        changed = false;
        for (size_t instruction_index = expression.instructions().size(); instruction_index-- > 0;) {
            auto const& instruction = expression.instructions()[instruction_index];
            auto const local_index = first_is_one_of(instruction.opcode(), Instructions::local_get, Instructions::local_set, Instructions::local_tee)
                ? Optional<u32> { instruction.local_index().value() }
                : Optional<u32> {};
            if (local_index.has_value())
                VERIFY(local_index.value() < local_count);

            for (size_t word_index = 0; word_index < word_count; ++word_index) {
                u64 live = 0;
                for (auto successor : control_flow->successors(instruction_index))
                    live |= live_before[successor * word_count + word_index];

                if (local_index.has_value() && local_index.value() / 64 == word_index) {
                    auto const mask = static_cast<u64>(1) << (local_index.value() % 64);
                    if (instruction.opcode() == Instructions::local_get)
                        live |= mask;
                    else
                        live &= ~mask;
                }

                auto& previous_live = live_before[instruction_index * word_count + word_index];
                if (previous_live != live) {
                    previous_live = live;
                    changed = true;
                }
            }
        }
    } while (changed);

    for (auto& checkpoint : checkpoints) {
        auto const loop_body_instruction_index = static_cast<size_t>(checkpoint.parsed_loop_instruction_index.value()) + 1;
        VERIFY(loop_body_instruction_index < expression.instructions().size());
        checkpoint.live_local_indices.clear();
        for (size_t local_index = 0; local_index < local_count; ++local_index) {
            auto const word = live_before[loop_body_instruction_index * word_count + local_index / 64];
            if ((word & (static_cast<u64>(1) << (local_index % 64))) != 0)
                checkpoint.live_local_indices.append(LocalIndex { static_cast<u32>(local_index) });
        }
    }

    return true;
}

}
