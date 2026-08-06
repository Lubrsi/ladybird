#!/usr/bin/env python3

from __future__ import annotations

import argparse
import bisect
import collections
import dataclasses
import math
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from collections.abc import Sequence
from pathlib import Path
from typing import Any
from typing import NamedTuple

CACHE_MAGIC = 0x4354494A4D534157
CACHE_HEADER = struct.Struct("<QIIQ32sII")
CACHE_FUNCTION_ENTRY = struct.Struct("<IIIIII")
CRANELIFT_RELOCATION_SIZE = 32
CRANELIFT_TRAP_SIZE = 8


class RunInfo(NamedTuple):
    number: int
    duration_seconds: float
    pid: int | None
    process_name: str
    schemas: frozenset


class CacheRecord(NamedTuple):
    function_index: int
    code_size: int
    native_entry_offset: int
    relocation_count: int
    trap_count: int
    mapping_size: int


class Region(NamedTuple):
    start: int
    end: int


class NativeMapping(NamedTuple):
    start: int
    end: int
    record: CacheRecord


class Frame(NamedTuple):
    name: str
    address: int
    binary: str


class StackInfo(NamedTuple):
    leaf: Frame | None
    bridge_frames: frozenset


@dataclasses.dataclass
class VisualFrames:
    begins: list[int]
    ends: list[int]
    pairs: list[tuple[int, int]]

    @property
    def start_rate(self) -> float | None:
        if len(self.begins) < 2 or self.begins[-1] == self.begins[0]:
            return None
        return (len(self.begins) - 1) / ((self.begins[-1] - self.begins[0]) / 1e9)


@dataclasses.dataclass
class CPUAnalysis:
    main_weight: int = 0
    main_samples: int = 0
    minimum_time: int | None = None
    maximum_time: int | None = None
    categories: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    functions: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    function_samples: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    pcs: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    named_self: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    bridge_inclusive: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    frame_cycles: list[int] = dataclasses.field(default_factory=list)

    @property
    def sample_window_seconds(self) -> float:
        if self.minimum_time is None or self.maximum_time is None:
            return 0
        return (self.maximum_time - self.minimum_time) / 1e9


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    index = fraction * (len(ordered) - 1)
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    weight = index - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


def resolve(values: dict[int, Any], element: ET.Element, default=None):
    reference = element.get("ref")
    if reference is not None:
        return values.get(int(reference), default)
    identifier = element.get("id")
    if identifier is not None:
        return values.get(int(identifier), default)
    return default


def should_refresh(source: Path, output: Path, force: bool) -> bool:
    return force or not output.exists() or output.stat().st_mtime < source.stat().st_mtime


def export_trace_toc(trace: Path, output: Path, force: bool) -> None:
    if not should_refresh(trace, output, force):
        return
    output.unlink(missing_ok=True)
    subprocess.run(
        ["xcrun", "xctrace", "export", "--input", str(trace), "--toc", "--output", str(output)],
        check=True,
    )


def export_trace_table(trace: Path, run: int, schema: str, output: Path, force: bool) -> None:
    if not should_refresh(trace, output, force):
        return
    output.unlink(missing_ok=True)
    xpath = f'/trace-toc/run[@number="{run}"]/data/table[@schema="{schema}"]'
    subprocess.run(
        ["xcrun", "xctrace", "export", "--input", str(trace), "--xpath", xpath, "--output", str(output)],
        check=True,
    )


def parse_toc(path: Path) -> list[RunInfo]:
    root = ET.parse(path).getroot()
    runs = []
    for run in root.findall(".//run"):
        number_text = run.get("number")
        if number_text is None:
            continue
        number = int(number_text)
        duration_element = run.find("./info/summary/duration")
        duration = float(duration_element.text or 0) if duration_element is not None else 0
        target_process = run.find("./info/target/process")
        pid = None
        process_name = ""
        if target_process is not None:
            process_name = target_process.get("name", "")
            pid_text = target_process.get("pid")
            if pid_text:
                pid = int(pid_text)
        schemas = frozenset(table.get("schema") for table in run.findall("./data/table"))
        runs.append(RunInfo(number, duration, pid, process_name, schemas))
    return runs


def parse_cache(path: Path) -> tuple[dict, list[CacheRecord]]:
    blob = path.read_bytes()
    if len(blob) < CACHE_HEADER.size:
        raise ValueError(f"{path}: truncated Cranelift cache header")
    magic, version, helper_count, layout_hash, wasm_hash, function_count, _padding = CACHE_HEADER.unpack_from(blob)
    if magic != CACHE_MAGIC:
        raise ValueError(f"{path}: not a Ladybird Cranelift cache blob")

    page_size = os.sysconf("SC_PAGE_SIZE")
    records = []
    offset = CACHE_HEADER.size
    for _ in range(function_count):
        if offset + CACHE_FUNCTION_ENTRY.size > len(blob):
            raise ValueError(f"{path}: truncated function entry")
        function_index, code_size, native_entry_offset, relocation_count, trap_count, _padding = (
            CACHE_FUNCTION_ENTRY.unpack_from(blob, offset)
        )
        offset += CACHE_FUNCTION_ENTRY.size
        offset += align_up(code_size, 16)
        offset += relocation_count * CRANELIFT_RELOCATION_SIZE
        offset += trap_count * CRANELIFT_TRAP_SIZE
        if offset > len(blob):
            raise ValueError(f"{path}: truncated function payload")
        writable_size = align_up(code_size, 16) + relocation_count * 16
        records.append(
            CacheRecord(
                function_index,
                code_size,
                native_entry_offset,
                relocation_count,
                trap_count,
                align_up(writable_size, page_size),
            )
        )
    if offset != len(blob):
        raise ValueError(f"{path}: {len(blob) - offset} trailing bytes after cache records")
    metadata = {
        "version": version,
        "helper_count": helper_count,
        "layout_hash": layout_hash,
        "wasm_hash": wasm_hash.hex(),
        "function_count": function_count,
        "size": len(blob),
    }
    return metadata, records


def capture_vmmap(pid: int, output: Path) -> None:
    result = subprocess.run(["vmmap", "-wide", str(pid)], check=True, stdout=subprocess.PIPE, text=True)
    output.write_text(result.stdout)


def parse_executable_regions(path: Path) -> list[Region]:
    pattern = re.compile(r"^VM_ALLOCATE\s+([0-9a-f]+)-([0-9a-f]+).*\brwx/rwx\b")
    regions = []
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.match(line)
        if match:
            regions.append(Region(int(match.group(1), 16), int(match.group(2), 16)))
    return regions


def correlate_regions(records: Sequence[CacheRecord], regions: Sequence[Region]) -> list[NativeMapping]:
    if len(records) != len(regions):
        raise ValueError(f"cache has {len(records)} functions, but vmmap has {len(regions)} rwx VM_ALLOCATE regions")
    if collections.Counter(record.mapping_size for record in records) != collections.Counter(
        region.end - region.start for region in regions
    ):
        raise ValueError("cache allocation sizes do not match the live rwx mappings")

    assigned: list[Region | None] = [None] * len(records)
    expected = [record.mapping_size for record in records]
    actual = [region.end - region.start for region in regions]
    index = 0
    while index < len(records):
        if expected[index] == actual[index]:
            assigned[index] = regions[index]
            index += 1
            continue

        repaired = False
        search_end = min(len(records), index + 64)
        for candidate in range(index + 1, search_end):
            if actual[candidate] == expected[index] and actual[index:candidate] == expected[index + 1 : candidate + 1]:
                assigned[index] = regions[candidate]
                for shifted in range(index + 1, candidate + 1):
                    assigned[shifted] = regions[shifted - 1]
                index = candidate + 1
                repaired = True
                break
            if expected[candidate] == actual[index] and expected[index:candidate] == actual[index + 1 : candidate + 1]:
                assigned[candidate] = regions[index]
                for shifted in range(index, candidate):
                    assigned[shifted] = regions[shifted + 1]
                index = candidate + 1
                repaired = True
                break
        if not repaired:
            raise ValueError(f"could not correlate cache and vmmap allocation order at record {index}")

    if any(region is None for region in assigned):
        raise ValueError("internal error while correlating cache mappings")
    mappings = [NativeMapping(region.start, region.end, record) for record, region in zip(records, assigned) if region]
    if any(mapping.record.mapping_size != mapping.end - mapping.start for mapping in mappings):
        raise ValueError("internal error while correlating cache mappings")
    return sorted(mappings, key=lambda mapping: mapping.start)


class MappingLookup:
    def __init__(self, mappings: Sequence[NativeMapping]):
        self.mappings = list(mappings)
        self.starts = [mapping.start for mapping in self.mappings]

    def find(self, address: int) -> NativeMapping | None:
        index = bisect.bisect_right(self.starts, address) - 1
        if index >= 0 and address < self.mappings[index].end:
            return self.mappings[index]
        return None


def read_function_names(wasm: Path, wasm_objdump: str) -> dict[int, str]:
    result = subprocess.run([wasm_objdump, "-x", str(wasm)], check=True, stdout=subprocess.PIPE, text=True)
    names = {}
    pattern = re.compile(r" - func\[(\d+)\].*<(.+)>")
    for line in result.stdout.splitlines():
        match = pattern.match(line)
        if match:
            names.setdefault(int(match.group(1)), match.group(2))
    return names


def read_native_instructions(paths: Sequence[Path]) -> dict[tuple[int, int], str]:
    instructions = {}
    function_pattern = re.compile(r"^Function #(\d+) \(")
    instruction_pattern = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$")
    for path in paths:
        function_index = None
        for line in path.read_text(errors="replace").splitlines():
            function_match = function_pattern.match(line)
            if function_match:
                function_index = int(function_match.group(1))
                continue
            instruction_match = instruction_pattern.match(line)
            if function_index is not None and instruction_match:
                offset = int(instruction_match.group(1), 16)
                instructions[(function_index, offset)] = (
                    f"{instruction_match.group(2)}  {instruction_match.group(3).strip()}"
                )
    return instructions


def parse_visual_frames(path: Path) -> VisualFrames:
    values = {}
    begins = []
    ends = []
    for _event, element in ET.iterparse(path, events=("end",)):
        identifier = element.get("id")
        if element.tag in ("event-type", "signpost-name") and identifier:
            values[int(identifier)] = element.get("fmt", element.text)
        elif element.tag == "row":
            fields = list(element)
            if len(fields) >= 7:
                timestamp = int(fields[0].text)
                event_type = resolve(values, fields[3], fields[3].get("fmt"))
                name = resolve(values, fields[6], fields[6].get("fmt"))
                if name == "WebContent Visual Frame":
                    (begins if event_type == "Begin" else ends).append(timestamp)
            element.clear()

    pairs = []
    end_index = 0
    for begin in begins:
        while end_index < len(ends) and ends[end_index] <= begin:
            end_index += 1
        if end_index < len(ends):
            pairs.append((begin, ends[end_index]))
            end_index += 1
    return VisualFrames(begins, ends, pairs)


def is_bridge_frame(name: str) -> bool:
    return name.startswith("wasm_cl_") or "Wasm::BytecodeInterpreter::" in name


def classify_leaf(frame: Frame, mappings: MappingLookup | None) -> str:
    if mappings is not None and mappings.find(frame.address) is not None:
        return "Wasm JIT"
    if "liblagom-wasm" in frame.binary:
        if "BytecodeInterpreter" in frame.name:
            return "Wasm interpreter"
        return "Wasm runtime"
    if "liblagom-js" in frame.binary or frame.name.startswith("asm_handler_") or frame.name.startswith(".Lasm_"):
        return "JavaScript"
    if "liblagom-web" in frame.binary:
        return "LibWeb"
    return "Other/system"


def parse_cpu_profile(path: Path, mappings: MappingLookup | None, visual_frames: VisualFrames | None) -> CPUAnalysis:
    values: dict[int, Any] = {}
    analysis = CPUAnalysis()
    if visual_frames is not None:
        analysis.frame_cycles = [0] * len(visual_frames.pairs)
        frame_begins = [pair[0] for pair in visual_frames.pairs]
        frame_pairs = visual_frames.pairs
    else:
        frame_begins = []
        frame_pairs = []

    for _event, element in ET.iterparse(path, events=("end",)):
        tag = element.tag
        identifier = element.get("id")
        if tag in ("sample-time", "cycle-weight") and identifier:
            values[int(identifier)] = int(element.text or 0)
        elif tag == "thread" and identifier:
            values[int(identifier)] = element.get("fmt", "")
        elif tag == "binary" and identifier:
            values[int(identifier)] = element.get("name", "")
        elif tag == "frame" and identifier:
            binary = ""
            for child in element:
                if child.tag == "binary":
                    binary = str(resolve(values, child, "") or "")
                    break
            values[int(identifier)] = Frame(element.get("name", ""), int(element.get("addr", "0"), 16), binary)
        elif tag == "backtrace" and identifier:
            frames = [resolve(values, child) for child in element if child.tag == "frame"]
            frames = [frame for frame in frames if isinstance(frame, Frame)]
            values[int(identifier)] = StackInfo(
                frames[0] if frames else None,
                frozenset(frame.name for frame in frames if is_bridge_frame(frame.name)),
            )
        elif tag == "tagged-backtrace" and identifier:
            stack = StackInfo(None, frozenset())
            for child in element:
                if child.tag == "backtrace":
                    resolved_stack = resolve(values, child, stack)
                    if isinstance(resolved_stack, StackInfo):
                        stack = resolved_stack
                    break
            values[int(identifier)] = stack
        elif tag == "row":
            fields = list(element)
            if len(fields) >= 7:
                timestamp = int(resolve(values, fields[0], 0) or 0)
                thread = str(resolve(values, fields[1], "") or "")
                weight = int(resolve(values, fields[5], 0) or 0)
                resolved_stack = resolve(values, fields[6], StackInfo(None, frozenset()))
                stack = resolved_stack if isinstance(resolved_stack, StackInfo) else StackInfo(None, frozenset())
                if thread.startswith("Main Thread"):
                    analysis.main_weight += weight
                    analysis.main_samples += 1
                    analysis.minimum_time = (
                        timestamp if analysis.minimum_time is None else min(analysis.minimum_time, timestamp)
                    )
                    analysis.maximum_time = (
                        timestamp if analysis.maximum_time is None else max(analysis.maximum_time, timestamp)
                    )
                    if stack.leaf is not None:
                        analysis.categories[classify_leaf(stack.leaf, mappings)] += weight
                        mapping = mappings.find(stack.leaf.address) if mappings is not None else None
                        if mapping is not None:
                            function_index = mapping.record.function_index
                            analysis.functions[function_index] += weight
                            analysis.function_samples[function_index] += 1
                            instruction_offset = (stack.leaf.address - mapping.start) & ~1
                            analysis.pcs[(function_index, instruction_offset)] += weight
                        else:
                            analysis.named_self[stack.leaf.name] += weight
                    for bridge_frame in stack.bridge_frames:
                        analysis.bridge_inclusive[bridge_frame] += weight
                    if frame_begins:
                        frame_index = bisect.bisect_right(frame_begins, timestamp) - 1
                        if frame_index >= 0 and timestamp <= frame_pairs[frame_index][1]:
                            analysis.frame_cycles[frame_index] += weight
            element.clear()
    return analysis


def print_visual_frames(frames: VisualFrames) -> None:
    print(f"  visual frames: begins={len(frames.begins):,}, ends={len(frames.ends):,}, complete={len(frames.pairs):,}")
    if len(frames.begins) >= 2:
        intervals = [(right - left) / 1e6 for left, right in zip(frames.begins, frames.begins[1:])]
        span = (frames.begins[-1] - frames.begins[0]) / 1e9
        print(f"    marker span={span:.6f}s, update starts/s={frames.start_rate:.3f}")
        print(
            "    start interval: "
            f"mean={statistics.mean(intervals):.3f}ms median={statistics.median(intervals):.3f}ms "
            f"p90={percentile(intervals, 0.90):.3f}ms p95={percentile(intervals, 0.95):.3f}ms "
            f"p99={percentile(intervals, 0.99):.3f}ms max={max(intervals):.3f}ms"
        )
    if frames.pairs:
        work = [(end - begin) / 1e6 for begin, end in frames.pairs]
        over_8333 = sum(value > 8.333 for value in work)
        over_16667 = sum(value > 16.667 for value in work)
        print(
            "    update work: "
            f"mean={statistics.mean(work):.3f}ms median={statistics.median(work):.3f}ms "
            f"p90={percentile(work, 0.90):.3f}ms p95={percentile(work, 0.95):.3f}ms "
            f"p99={percentile(work, 0.99):.3f}ms max={max(work):.3f}ms"
        )
        print(
            f"    over 8.333ms={over_8333:,}/{len(work):,} ({over_8333 / len(work) * 100:.1f}%), "
            f"over 16.667ms={over_16667:,}/{len(work):,} ({over_16667 / len(work) * 100:.1f}%)"
        )


def correlation(left: Sequence[float], right: Sequence[float]) -> float | None:
    if len(left) != len(right) or len(left) < 2:
        return None
    left_mean = statistics.mean(left)
    right_mean = statistics.mean(right)
    covariance = sum((x - left_mean) * (y - right_mean) for x, y in zip(left, right))
    left_variance = sum((x - left_mean) ** 2 for x in left)
    right_variance = sum((y - right_mean) ** 2 for y in right)
    denominator = math.sqrt(left_variance * right_variance)
    return covariance / denominator if denominator else None


def print_cpu_analysis(
    analysis: CPUAnalysis,
    frames: VisualFrames | None,
    function_names: dict[int, str],
    native_instructions: dict[tuple[int, int], str],
    focus_functions: Sequence[int],
    top: int,
) -> None:
    print(
        f"  main-thread CPU: samples={analysis.main_samples:,}, cycle-weight={analysis.main_weight / 1e9:.3f}B, "
        f"sample window={analysis.sample_window_seconds:.6f}s"
    )
    estimated_updates = None
    if frames is not None and frames.start_rate is not None:
        estimated_updates = frames.start_rate * analysis.sample_window_seconds

    print("    leaf categories:")
    for name, weight in analysis.categories.most_common():
        per_update = ""
        if estimated_updates:
            per_update = f", {weight / estimated_updates / 1e6:.3f}M/update"
        print(f"      {weight / 1e9:8.3f}B {weight / analysis.main_weight * 100:6.2f}%{per_update}  {name}")

    if analysis.functions:
        print("    resolved Wasm functions:")
        for function_index, weight in analysis.functions.most_common(top):
            print(
                f"      {weight / 1e9:8.3f}B {weight / analysis.main_weight * 100:6.2f}% "
                f"{analysis.function_samples[function_index]:7,d}  [{function_index}] {function_names.get(function_index, '?')}"
            )

    print("    non-JIT leaf symbols:")
    for name, weight in analysis.named_self.most_common(top):
        print(f"      {weight / 1e9:8.3f}B {weight / analysis.main_weight * 100:6.2f}%  {name}")

    if analysis.bridge_inclusive:
        print("    inclusive Wasm bridge/interpreter frames:")
        for name, weight in analysis.bridge_inclusive.most_common(top):
            print(f"      {weight / 1e9:8.3f}B {weight / analysis.main_weight * 100:6.2f}%  {name}")

    for function_index in focus_functions:
        total = analysis.functions[function_index]
        print(
            f"    top PCs [{function_index}] {function_names.get(function_index, '?')}: "
            f"{total / 1e9:.3f}B ({total / analysis.main_weight * 100 if analysis.main_weight else 0:.2f}%)"
        )
        function_pcs = [
            (offset, weight)
            for (candidate_function, offset), weight in analysis.pcs.most_common()
            if candidate_function == function_index
        ]
        for offset, weight in function_pcs[:top]:
            share = weight / total * 100 if total else 0
            instruction = native_instructions.get((function_index, offset))
            suffix = f"  {instruction}" if instruction else ""
            print(f"      +0x{offset:05x} {weight / 1e9:7.3f}B {share:6.2f}%{suffix}")

    if frames is not None and analysis.frame_cycles:
        durations = [(end - begin) / 1e6 for begin, end in frames.pairs]
        sampled = [(duration, cycles) for duration, cycles in zip(durations, analysis.frame_cycles) if cycles]
        if sampled:
            coefficient = correlation([item[0] for item in sampled], [item[1] for item in sampled])
            if coefficient is not None:
                print(
                    f"    frame work/cycle-weight correlation={coefficient:.3f} across {len(sampled):,} sampled updates"
                )


def find_wasm_objdump(requested: str | None) -> str:
    if requested:
        return requested
    executable = shutil.which("wasm-objdump")
    if executable is None:
        raise RuntimeError("wasm-objdump is required with --wasm; pass --wasm-objdump")
    return executable


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze Ladybird Wasm JIT samples in an Instruments CPU Profiler trace"
    )
    parser.add_argument("trace", type=Path)
    parser.add_argument("--cache", type=Path, help="Ladybird .wasmjit cache blob used by the traced process")
    parser.add_argument("--wasm", type=Path, help="Named Wasm module corresponding to the cache")
    parser.add_argument("--wasm-objdump", help="Path to wabt's wasm-objdump")
    parser.add_argument(
        "--native-dump",
        type=Path,
        action="append",
        default=[],
        help="Native dump produced by wasm --dump-native (repeatable)",
    )
    parser.add_argument("--pid", type=int, help="Live WebContent PID; defaults to the trace target PID")
    parser.add_argument("--vmmap-file", type=Path, help="Previously captured 'vmmap -wide' output")
    parser.add_argument("--run", type=int, action="append", dest="runs", help="Run number to analyze (repeatable)")
    parser.add_argument(
        "--focus-function", type=int, action="append", default=[], help="Function index whose hot PCs should be printed"
    )
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--work-directory", type=Path)
    parser.add_argument("--refresh", action="store_true", help="Re-export Instruments XML tables")
    arguments = parser.parse_args()

    trace = arguments.trace.expanduser().resolve()
    if not trace.exists():
        parser.error(f"trace does not exist: {trace}")
    work_directory = arguments.work_directory
    if work_directory is None:
        work_directory = Path(tempfile.gettempdir()) / "ladybird-wasm-instruments" / trace.stem
    work_directory.mkdir(parents=True, exist_ok=True)

    toc_path = work_directory / "toc.xml"
    export_trace_toc(trace, toc_path, arguments.refresh)
    run_infos = parse_toc(toc_path)
    if arguments.runs:
        requested_runs = set(arguments.runs)
        run_infos = [run for run in run_infos if run.number in requested_runs]
        missing_runs = requested_runs - {run.number for run in run_infos}
        if missing_runs:
            parser.error(f"trace does not contain run(s): {', '.join(str(run) for run in sorted(missing_runs))}")

    cache_metadata = None
    records = None
    if arguments.cache:
        cache_metadata, records = parse_cache(arguments.cache.expanduser().resolve())
        print(
            f"cache: version={cache_metadata['version']}, helpers={cache_metadata['helper_count']}, "
            f"functions={cache_metadata['function_count']:,}, size={cache_metadata['size']:,} bytes"
        )
        print(f"  layout hash=0x{cache_metadata['layout_hash']:016x}")
        print(f"  Wasm SHA-256={cache_metadata['wasm_hash']}")

    function_names = {}
    if arguments.wasm:
        function_names = read_function_names(
            arguments.wasm.expanduser().resolve(), find_wasm_objdump(arguments.wasm_objdump)
        )
    native_instructions = read_native_instructions([path.expanduser().resolve() for path in arguments.native_dump])

    mappings = None
    if records is not None:
        vmmap_path = arguments.vmmap_file
        if vmmap_path is None:
            pid = arguments.pid
            if pid is None:
                pids = {run.pid for run in run_infos if run.pid is not None}
                if len(pids) == 1:
                    pid = pids.pop()
            if pid is not None:
                vmmap_path = work_directory / f"vmmap-{pid}.txt"
                capture_vmmap(pid, vmmap_path)
        if vmmap_path is not None:
            regions = parse_executable_regions(vmmap_path.expanduser().resolve())
            native_mappings = correlate_regions(records, regions)
            mappings = MappingLookup(native_mappings)
            print(f"  resolved {len(native_mappings):,} live executable mappings")
        else:
            print("warning: no live PID or --vmmap-file; raw JIT PCs cannot be resolved", file=sys.stderr)

    for run in run_infos:
        print(
            f"\nrun {run.number}: duration={run.duration_seconds:.6f}s, target={run.process_name or '?'} ({run.pid or '?'})"
        )
        visual_frames = None
        if "os-signpost" in run.schemas:
            signpost_path = work_directory / f"run-{run.number}-os-signpost.xml"
            export_trace_table(trace, run.number, "os-signpost", signpost_path, arguments.refresh)
            visual_frames = parse_visual_frames(signpost_path)
            print_visual_frames(visual_frames)
        if "cpu-profile" in run.schemas:
            cpu_path = work_directory / f"run-{run.number}-cpu-profile.xml"
            export_trace_table(trace, run.number, "cpu-profile", cpu_path, arguments.refresh)
            analysis = parse_cpu_profile(cpu_path, mappings, visual_frames)
            print_cpu_analysis(
                analysis,
                visual_frames,
                function_names,
                native_instructions,
                arguments.focus_function,
                arguments.top,
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
