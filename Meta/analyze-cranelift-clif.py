#!/usr/bin/env python3

"""Compare structural metrics from optimized Cranelift IR and native-code dumps."""

from __future__ import annotations

import argparse
import collections
import dataclasses
import pathlib
import re

BLOCK_RE = re.compile(r"^\s*(?:cold\s+)?block\d+(?:\((.*)\))?:\s*$")
PARAM_RE = re.compile(r"\bv\d+\s*:")
PARAM_TYPE_RE = re.compile(r"\bv\d+\s*:\s*([a-zA-Z0-9]+)")
SOURCE_LOCATION_RE = re.compile(r"^@[0-9a-fA-F]+\s+")
RESULT_RE = re.compile(r"^v\d+(?:\s*,\s*v\d+)*\s*=\s*")
NATIVE_INSTRUCTION_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+(?:(?:[0-9a-fA-F]{2}){4}|[0-9a-fA-F]{8}|(?:[0-9a-fA-F]{2}\s+){4})\s+([a-z][a-z0-9.]*)\s*(.*)$"
)
NATIVE_FUNCTION_RE = re.compile(r"^Function #\d+.*\((\d+) bytes\):$")


@dataclasses.dataclass
class ClifMetrics:
    bytes: int = 0
    blocks: int = 0
    instructions: int = 0
    values: int = 0
    block_parameters: int = 0
    parameterized_blocks: int = 0
    maximum_block_parameters: int = 0
    memory_operations: int = 0
    loads: int = 0
    stores: int = 0
    stack_loads: int = 0
    stack_stores: int = 0
    calls: int = 0
    branches: int = 0
    integer_reductions: int = 0
    zero_extensions: int = 0
    sign_extensions: int = 0
    explicit_stack_slots: int = 0
    explicit_stack_slot_bytes: int = 0
    readonly_operations: int = 0
    movable_operations: int = 0
    region_operations: int = 0
    i32_block_parameters: int = 0
    i64_block_parameters: int = 0
    f32_block_parameters: int = 0
    f64_block_parameters: int = 0
    opcodes: collections.Counter[str] = dataclasses.field(default_factory=collections.Counter)


@dataclasses.dataclass
class NativeMetrics:
    code_bytes: int = 0
    instructions: int = 0
    loads: int = 0
    stores: int = 0
    memory_operations: int = 0
    stack_memory_operations: int = 0
    direct_x28_memory_operations: int = 0
    branches: int = 0
    calls: int = 0


def parse_named_path(value: str) -> tuple[str, pathlib.Path]:
    try:
        name, path = value.split("=", 1)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected NAME=PATH") from error
    if not name:
        raise argparse.ArgumentTypeError("NAME must not be empty")
    parsed_path = pathlib.Path(path)
    if parsed_path != pathlib.Path("/dev/stdin") and not parsed_path.is_file():
        raise argparse.ArgumentTypeError(f"input file does not exist: {parsed_path}")
    return name, parsed_path


def parse_native_range(value: str) -> tuple[str, pathlib.Path, int, int]:
    try:
        named_path, address_range = value.rsplit("@", 1)
        name, path = parse_named_path(named_path)
        start_text, end_text = address_range.split("-", 1)
        start = int(start_text, 0)
        end = int(end_text, 0)
    except (ValueError, argparse.ArgumentTypeError) as error:
        raise argparse.ArgumentTypeError("expected NAME=PATH@START-END") from error
    if end < start:
        raise argparse.ArgumentTypeError("native range end precedes its start")
    return name, path, start, end


def instruction_opcode(line: str) -> tuple[str, int] | None:
    line = SOURCE_LOCATION_RE.sub("", line.strip())
    line = line.split(";", 1)[0].strip()
    result_match = RESULT_RE.match(line)
    result_count = 0
    if result_match:
        result_count = len(re.findall(r"\bv\d+\b", result_match.group(0)))
        line = line[result_match.end() :]
    if not line:
        return None
    opcode = line.split(maxsplit=1)[0]
    if not re.fullmatch(r"[a-z][a-z0-9_.]*", opcode):
        return None
    return opcode, result_count


def parse_clif(path: pathlib.Path) -> ClifMetrics:
    text = path.read_text()
    metrics = ClifMetrics(bytes=len(text.encode()))
    in_body = False

    for line in text.splitlines():
        stripped = line.strip()
        stack_slot = re.match(r"ss\d+\s*=\s*explicit_slot\s+(\d+)", stripped)
        if stack_slot:
            metrics.explicit_stack_slots += 1
            metrics.explicit_stack_slot_bytes += int(stack_slot.group(1))

        block_match = BLOCK_RE.match(line)
        if block_match:
            in_body = True
            parameter_count = len(PARAM_RE.findall(block_match.group(1) or ""))
            metrics.blocks += 1
            metrics.block_parameters += parameter_count
            metrics.parameterized_blocks += parameter_count != 0
            metrics.maximum_block_parameters = max(metrics.maximum_block_parameters, parameter_count)
            parameter_types = collections.Counter(PARAM_TYPE_RE.findall(block_match.group(1) or ""))
            metrics.i32_block_parameters += parameter_types["i32"]
            metrics.i64_block_parameters += parameter_types["i64"]
            metrics.f32_block_parameters += parameter_types["f32"]
            metrics.f64_block_parameters += parameter_types["f64"]
            continue
        if not in_body or stripped == "}" or not stripped or stripped.startswith(";;"):
            continue

        instruction = instruction_opcode(stripped)
        if instruction is None:
            continue
        opcode, result_count = instruction
        metrics.instructions += 1
        metrics.values += result_count
        metrics.opcodes[opcode] += 1

        base_opcode = opcode.split(".", 1)[0]
        if base_opcode in {"load", "store", "atomic_load", "atomic_store", "stack_load", "stack_store"}:
            metrics.memory_operations += 1
        metrics.loads += base_opcode in {"load", "atomic_load"}
        metrics.stores += base_opcode in {"store", "atomic_store"}
        metrics.stack_loads += base_opcode == "stack_load"
        metrics.stack_stores += base_opcode == "stack_store"
        if base_opcode in {"call", "call_indirect"}:
            metrics.calls += 1
        if base_opcode in {"brif", "br_table", "jump"}:
            metrics.branches += 1
        if base_opcode == "ireduce":
            metrics.integer_reductions += 1
        if base_opcode == "uextend":
            metrics.zero_extensions += 1
        if base_opcode == "sextend":
            metrics.sign_extensions += 1
        metrics.readonly_operations += " readonly" in f" {stripped}"
        metrics.movable_operations += " can_move" in f" {stripped}"
        metrics.region_operations += re.search(r"\bregion\d+\b", stripped) is not None

    return metrics


def parse_native(path: pathlib.Path, start: int, end: int) -> NativeMetrics:
    metrics = NativeMetrics()
    for line in path.read_text().splitlines():
        function = NATIVE_FUNCTION_RE.match(line)
        if function is not None:
            metrics.code_bytes = int(function.group(1))
            continue
        match = NATIVE_INSTRUCTION_RE.match(line)
        if match is None:
            continue
        address = int(match.group(1), 16)
        if not start <= address <= end:
            continue
        mnemonic = match.group(2)
        operands = match.group(3)
        is_load = mnemonic.startswith(("ld", "cas", "swp"))
        is_store = mnemonic.startswith(("st", "cas", "swp"))
        is_memory = is_load or is_store
        metrics.instructions += 1
        metrics.loads += is_load
        metrics.stores += is_store
        metrics.memory_operations += is_memory
        metrics.stack_memory_operations += is_memory and "[sp" in operands
        metrics.direct_x28_memory_operations += is_memory and "[x28" in operands
        metrics.calls += mnemonic in {"bl", "blr"}
        metrics.branches += mnemonic.startswith(("b", "cb", "tb")) and mnemonic not in {"bl", "blr"}
    return metrics


def print_table(results: list[tuple[str, ClifMetrics]]) -> None:
    fields = [
        ("Bytes", "bytes"),
        ("Blocks", "blocks"),
        ("Instructions", "instructions"),
        ("Values", "values"),
        ("Memory operations", "memory_operations"),
        ("Loads", "loads"),
        ("Stores", "stores"),
        ("Stack loads", "stack_loads"),
        ("Stack stores", "stack_stores"),
        ("Calls", "calls"),
        ("Branches", "branches"),
        ("Block parameters", "block_parameters"),
        ("Parameterized blocks", "parameterized_blocks"),
        ("Maximum block parameters", "maximum_block_parameters"),
        ("i32 block parameters", "i32_block_parameters"),
        ("i64 block parameters", "i64_block_parameters"),
        ("f32 block parameters", "f32_block_parameters"),
        ("f64 block parameters", "f64_block_parameters"),
        ("Integer reductions", "integer_reductions"),
        ("Zero extensions", "zero_extensions"),
        ("Sign extensions", "sign_extensions"),
        ("Explicit stack slots", "explicit_stack_slots"),
        ("Explicit stack-slot bytes", "explicit_stack_slot_bytes"),
        ("Readonly operations", "readonly_operations"),
        ("Movable operations", "movable_operations"),
        ("Region operations", "region_operations"),
    ]
    name_width = max(len("Metric"), *(len(label) for label, _ in fields))
    value_widths = [
        max(len(name), *(len(f"{getattr(metrics, field):,}") for _, field in fields)) for name, metrics in results
    ]
    print(f"{'Metric':<{name_width}}", end="")
    for (name, _), width in zip(results, value_widths):
        print(f"  {name:>{width}}", end="")
    print()
    for label, field in fields:
        print(f"{label:<{name_width}}", end="")
        for (_, metrics), width in zip(results, value_widths):
            print(f"  {getattr(metrics, field):>{width},}", end="")
        print()


def print_native_table(results: list[tuple[str, NativeMetrics]]) -> None:
    fields = [
        ("Code bytes", "code_bytes"),
        ("Instructions", "instructions"),
        ("Memory operations", "memory_operations"),
        ("Loads", "loads"),
        ("Stores", "stores"),
        ("Stack-memory operations", "stack_memory_operations"),
        ("Direct x28-memory operations", "direct_x28_memory_operations"),
        ("Branches", "branches"),
        ("Calls", "calls"),
    ]
    name_width = max(len("Metric"), *(len(label) for label, _ in fields))
    value_widths = [
        max(len(name), *(len(f"{getattr(metrics, field):,}") for _, field in fields)) for name, metrics in results
    ]
    print(f"{'Metric':<{name_width}}", end="")
    for (name, _), width in zip(results, value_widths):
        print(f"  {name:>{width}}", end="")
    print()
    for label, field in fields:
        print(f"{label:<{name_width}}", end="")
        for (_, metrics), width in zip(results, value_widths):
            print(f"  {getattr(metrics, field):>{width},}", end="")
        print()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--clif",
        action="append",
        type=parse_named_path,
        metavar="NAME=PATH",
        help="Optimized CLIF dump to analyze (repeatable)",
    )
    parser.add_argument(
        "--native-range",
        action="append",
        type=parse_native_range,
        metavar="NAME=PATH@START-END",
        help="Inclusive native instruction range to analyze (repeatable)",
    )
    args = parser.parse_args()
    if not args.clif and not args.native_range:
        parser.error("at least one --clif or --native-range is required")
    if args.clif:
        print_table([(name, parse_clif(path)) for name, path in args.clif])
    if args.clif and args.native_range:
        print()
    if args.native_range:
        print_native_table([(name, parse_native(path, start, end)) for name, path, start, end in args.native_range])


if __name__ == "__main__":
    main()
