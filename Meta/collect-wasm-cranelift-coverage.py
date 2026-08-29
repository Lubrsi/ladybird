#!/usr/bin/env python3

"""Collect direct Cranelift frontend coverage from WebAssembly modules."""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import os
import re
import subprocess
import sys

from pathlib import Path

DIRECT_FAILURE_RE = re.compile(
    r"^direct compilation of function (\d+) failed: (.*); "
    r"(trying allocated-bytecode frontend|leaving the function interpreted)$"
)
ALLOCATED_FAILURE_RE = re.compile(
    r"^allocated-bytecode compilation of function (\d+) also failed: (.*); leaving the function interpreted$"
)
OSR_FAILURE_RE = re.compile(r"^direct OSR compilation of function (\d+) failed: (.*); retaining only the clean body$")
INSTRUCTION_FAILURE_RE = re.compile(r"^instruction (\d+) \(([^,]+), opcode (0x[0-9a-fA-F]+)\): (.*)$")
SUMMARY_RE = re.compile(
    r"^direct compilation summary: attempted=(\d+) succeeded=(\d+) allocated_fallbacks=(\d+) "
    r"uncompiled=(\d+); osr_requested=(\d+) osr_succeeded=(\d+) osr_unavailable=(\d+)$"
)


@dataclasses.dataclass
class CoverageCounts:
    attempted: int = 0
    succeeded: int = 0
    allocated_fallbacks: int = 0
    uncompiled: int = 0
    osr_requested: int = 0
    osr_succeeded: int = 0
    osr_unavailable: int = 0

    def add(self, other: CoverageCounts) -> None:
        for field in dataclasses.fields(self):
            setattr(self, field.name, getattr(self, field.name) + getattr(other, field.name))


@dataclasses.dataclass(frozen=True)
class CompilationFailure:
    stage: str
    function_index: int
    reason: str
    instruction_index: int | None = None
    opcode_name: str | None = None
    opcode: int | None = None


@dataclasses.dataclass
class ModuleCoverage:
    path: Path
    counts: CoverageCounts = dataclasses.field(default_factory=CoverageCounts)
    failures: list[CompilationFailure] = dataclasses.field(default_factory=list)
    summary_count: int = 0
    invocation_error: str | None = None


def parse_failure(stage: str, function_index: str, reason: str) -> CompilationFailure:
    instruction_match = INSTRUCTION_FAILURE_RE.match(reason)
    if instruction_match is None:
        return CompilationFailure(stage, int(function_index), reason)
    instruction_index, opcode_name, opcode, specific_reason = instruction_match.groups()
    return CompilationFailure(
        stage,
        int(function_index),
        specific_reason,
        int(instruction_index),
        opcode_name,
        int(opcode, 0),
    )


def parse_trace(path: Path, trace: str) -> ModuleCoverage:
    coverage = ModuleCoverage(path)
    for line in trace.splitlines():
        if match := DIRECT_FAILURE_RE.match(line):
            function_index, reason, outcome = match.groups()
            stage = "direct-to-allocated" if outcome == "trying allocated-bytecode frontend" else "direct"
            coverage.failures.append(parse_failure(stage, function_index, reason))
            continue
        if match := ALLOCATED_FAILURE_RE.match(line):
            coverage.failures.append(parse_failure("allocated-bytecode", *match.groups()))
            continue
        if match := OSR_FAILURE_RE.match(line):
            coverage.failures.append(parse_failure("direct-osr", *match.groups()))
            continue
        if match := SUMMARY_RE.match(line):
            values = [int(value) for value in match.groups()]
            coverage.counts.add(CoverageCounts(*values))
            coverage.summary_count += 1
    return coverage


def discover_modules(inputs: list[Path], recursive: bool = False, suffix: str = ".wasm") -> list[Path]:
    modules: set[Path] = set()
    for path in inputs:
        if path.is_file():
            if path.suffix != suffix:
                raise ValueError(f"input is not a {suffix} file: {path}")
            modules.add(path.resolve())
            continue
        if path.is_dir():
            pattern = f"**/*{suffix}" if recursive else f"*{suffix}"
            modules.update(module.resolve() for module in path.glob(pattern))
            continue
        raise ValueError(f"input does not exist: {path}")
    return sorted(modules)


def run_traced_command(
    label: Path,
    command: list[str],
    timeout: float,
    require_summary: bool = True,
) -> ModuleCoverage:
    environment = os.environ.copy()
    environment["CRANELIFT_TRACE_DIRECT_FALLBACK"] = "1"
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            check=False,
            text=True,
            timeout=timeout,
            env=environment,
        )
    except subprocess.TimeoutExpired as error:
        trace = error.stderr if isinstance(error.stderr, str) else ""
        coverage = parse_trace(label, trace)
        coverage.invocation_error = f"timed out after {timeout:g} seconds"
        return coverage

    coverage = parse_trace(label, result.stderr)
    if result.returncode != 0:
        coverage.invocation_error = f"command exited with status {result.returncode}"
    elif require_summary and coverage.summary_count == 0:
        coverage.invocation_error = "no direct compilation summary was emitted"
    return coverage


def run_module(
    executable: Path,
    module: Path,
    wasm_arguments: list[str],
    timeout: float,
) -> ModuleCoverage:
    command = [str(executable), "--instantiate", *wasm_arguments, str(module)]
    return run_traced_command(module, command, timeout)


def module_labels(results: list[ModuleCoverage]) -> dict[Path, str]:
    name_counts = collections.Counter(result.path.name for result in results)
    return {
        result.path: result.path.name if name_counts[result.path.name] == 1 else str(result.path) for result in results
    }


def print_table(results: list[ModuleCoverage], summary_only: bool = False) -> None:
    labels = module_labels(results)
    rows = []
    if not summary_only:
        rows = [
            (
                labels[result.path],
                result.counts.attempted,
                result.counts.succeeded,
                result.counts.allocated_fallbacks,
                result.counts.uncompiled,
                f"{result.counts.osr_succeeded}/{result.counts.osr_requested}",
            )
            for result in results
        ]
    total = CoverageCounts()
    for result in results:
        total.add(result.counts)
    rows.append(
        (
            "Total",
            total.attempted,
            total.succeeded,
            total.allocated_fallbacks,
            total.uncompiled,
            f"{total.osr_succeeded}/{total.osr_requested}",
        )
    )

    headers = ("Module", "Attempted", "Direct", "Allocated", "Uncompiled", "OSR")
    widths = [
        max(len(str(value)) for value in (header, *(row[index] for row in rows)))
        for index, header in enumerate(headers)
    ]
    print(f"{headers[0]:<{widths[0]}}", end="")
    for index, header in enumerate(headers[1:], 1):
        print(f"  {header:>{widths[index]}}", end="")
    print()
    for row in rows:
        print(f"{row[0]:<{widths[0]}}", end="")
        for index, value in enumerate(row[1:], 1):
            print(f"  {value:>{widths[index]}}", end="")
        print()


def print_failure_groups(
    results: list[ModuleCoverage],
    location_limit: int,
    heading: str | None = "Failures:",
) -> None:
    labels = module_labels(results)
    grouped: dict[tuple[str, str | None, int | None, str], list[str]] = collections.defaultdict(list)
    for result in results:
        for failure in result.failures:
            key = (failure.stage, failure.opcode_name, failure.opcode, failure.reason)
            location = f"{labels[result.path]}:{failure.function_index}"
            if failure.instruction_index is not None:
                location += f"@{failure.instruction_index}"
            grouped[key].append(location)

    if not grouped:
        return
    if heading is not None:
        print(f"\n{heading}")

    def failure_sort_key(item):
        stage, opcode_name, opcode, reason = item[0]
        return stage, opcode_name or "", opcode if opcode is not None else -1, reason

    for (stage, opcode_name, opcode, reason), locations in sorted(grouped.items(), key=failure_sort_key):
        instruction = ""
        if opcode_name is not None and opcode is not None:
            instruction = f" {opcode_name} ({opcode:#x})"
        print(f"  {len(locations)} x {stage}{instruction}: {reason}")
        locations = sorted(locations)
        displayed_locations = locations[:location_limit]
        if displayed_locations:
            suffix = ""
            if len(locations) > len(displayed_locations):
                suffix = f", ... {len(locations) - len(displayed_locations)} more"
            print(f"    {', '.join(displayed_locations)}{suffix}")


def successful_allocated_fallbacks(result: ModuleCoverage) -> list[CompilationFailure]:
    failed_fallbacks = collections.Counter(
        failure.function_index for failure in result.failures if failure.stage == "allocated-bytecode"
    )
    successful_fallbacks = []
    for failure in result.failures:
        if failure.stage != "direct-to-allocated":
            continue
        if failed_fallbacks[failure.function_index] > 0:
            failed_fallbacks[failure.function_index] -= 1
        else:
            successful_fallbacks.append(failure)
    return successful_fallbacks


def print_successful_fallback_groups(results: list[ModuleCoverage], location_limit: int) -> None:
    fallback_results = []
    for result in results:
        fallback_results.append(
            ModuleCoverage(
                path=result.path,
                failures=[
                    dataclasses.replace(failure, stage="direct-to-allocated")
                    for failure in successful_allocated_fallbacks(result)
                ],
            )
        )
    if any(result.failures for result in fallback_results):
        print_failure_groups(
            fallback_results,
            location_limit,
            heading="Successful allocated-bytecode fallbacks:",
        )


def json_result(results: list[ModuleCoverage]) -> dict:
    total = CoverageCounts()
    modules = []
    for result in results:
        total.add(result.counts)
        modules.append(
            {
                "path": str(result.path),
                "counts": dataclasses.asdict(result.counts),
                "failures": [dataclasses.asdict(failure) for failure in result.failures],
                "successful_allocated_fallbacks": [
                    dataclasses.asdict(failure) for failure in successful_allocated_fallbacks(result)
                ],
                "summary_count": result.summary_count,
                "invocation_error": result.invocation_error,
            }
        )
    return {"modules": modules, "total": dataclasses.asdict(total)}


def substitute_trace_input(command: list[str], path: Path) -> list[str]:
    return [argument.replace("{input}", str(path)).replace("{name}", path.name) for argument in command]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", type=Path, help=".wasm files or directories to scan")
    parser.add_argument("--wasm-executable", type=Path)
    parser.add_argument("--wasi", action="store_true", help="enable WASI imports")
    parser.add_argument("--export-noop", action="store_true", help="provide no-op functions for imports")
    parser.add_argument("--recursive", action="store_true", help="scan input directories recursively")
    parser.add_argument("--wasm-argument", action="append", default=[], metavar="ARG", help="additional wasm argument")
    parser.add_argument(
        "--trace-label",
        default="traced-command",
        help="workload label used with --trace-command",
    )
    parser.add_argument(
        "--trace-input-directory",
        type=Path,
        help="run --trace-command once per matching file, replacing {input} and {name}",
    )
    parser.add_argument(
        "--trace-input-suffix",
        default=".js",
        help="file suffix selected by --trace-input-directory",
    )
    parser.add_argument("--timeout", type=float, default=300, help="per-module timeout in seconds")
    parser.add_argument(
        "--failure-location-limit",
        type=int,
        default=10,
        metavar="COUNT",
        help="maximum locations shown for each failure group; JSON output always retains every failure",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    parser.add_argument("--summary-only", action="store_true", help="omit per-input rows from the table")
    parser.add_argument(
        "--trace-command",
        nargs=argparse.REMAINDER,
        help="run a phase-aware test command and collect all compiler summaries it emits",
    )
    arguments = parser.parse_args()

    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if arguments.failure_location_limit < 0:
        parser.error("--failure-location-limit must not be negative")

    if arguments.trace_command:
        if arguments.inputs:
            parser.error("module inputs cannot be combined with --trace-command")
        if arguments.wasm_executable is not None or arguments.wasi or arguments.export_noop or arguments.wasm_argument:
            parser.error("standalone wasm options cannot be combined with --trace-command")
        if arguments.trace_input_directory is None:
            results = [run_traced_command(Path(arguments.trace_label), arguments.trace_command, arguments.timeout)]
        else:
            if not arguments.trace_input_suffix.startswith("."):
                parser.error("--trace-input-suffix must start with a period")
            try:
                trace_inputs = discover_modules(
                    [arguments.trace_input_directory],
                    arguments.recursive,
                    suffix=arguments.trace_input_suffix,
                )
            except ValueError as error:
                parser.error(str(error))
            if not trace_inputs:
                parser.error("no trace input files found")
            results = [
                run_traced_command(
                    trace_input,
                    substitute_trace_input(arguments.trace_command, trace_input),
                    arguments.timeout,
                    require_summary=False,
                )
                for trace_input in trace_inputs
            ]
    else:
        if arguments.trace_input_directory is not None:
            parser.error("--trace-input-directory requires --trace-command")
        if arguments.wasm_executable is None:
            parser.error("--wasm-executable is required for module inputs")
        if not arguments.wasm_executable.is_file():
            parser.error(f"wasm executable does not exist: {arguments.wasm_executable}")
        try:
            modules = discover_modules(arguments.inputs, arguments.recursive)
        except ValueError as error:
            parser.error(str(error))
        if not modules:
            parser.error("no .wasm modules found")

        wasm_arguments = []
        if arguments.wasi:
            wasm_arguments.append("--wasi")
        if arguments.export_noop:
            wasm_arguments.append("--export-noop")
        wasm_arguments.extend(arguments.wasm_argument)

        results = [
            run_module(arguments.wasm_executable, module, wasm_arguments, arguments.timeout) for module in modules
        ]
    if arguments.json:
        json.dump(json_result(results), sys.stdout, indent=2)
        print()
    else:
        print_table(results, arguments.summary_only)
        print_successful_fallback_groups(results, arguments.failure_location_limit)
        print_failure_groups(results, arguments.failure_location_limit)

    errors = [result for result in results if result.invocation_error is not None]
    for result in errors:
        print(f"{result.path}: {result.invocation_error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
