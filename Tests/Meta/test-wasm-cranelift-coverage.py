#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import os
import runpy
import sys
import tempfile
import unittest

from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

SCRIPT = Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Meta" / "collect-wasm-cranelift-coverage.py"
MODULE = runpy.run_path(str(SCRIPT), run_name="collect_wasm_cranelift_coverage")


class TestWasmCraneliftCoverage(unittest.TestCase):
    def test_parses_and_aggregates_compiler_batches(self):
        trace = """direct compilation of function 6 failed: instruction 26 (v128_load, opcode 0xfd000000): unsupported direct instruction; trying allocated-bytecode frontend
allocated-bytecode compilation of function 6 also failed: unsupported allocated instruction; leaving the function interpreted
direct compilation of function 7 failed: instruction 4 (table_get, opcode 0x25): unsupported direct instruction; trying allocated-bytecode frontend
direct OSR compilation of function 9 failed: invalid checkpoint; retaining only the clean body
direct compilation summary: attempted=10 succeeded=8 allocated_fallbacks=1 uncompiled=1; osr_requested=4 osr_succeeded=3 osr_unavailable=1
direct compilation summary: attempted=5 succeeded=5 allocated_fallbacks=0 uncompiled=0; osr_requested=2 osr_succeeded=2 osr_unavailable=0
"""
        coverage = MODULE["parse_trace"](Path("fixture.wasm"), trace)

        self.assertEqual(coverage.summary_count, 2)
        self.assertEqual(
            coverage.counts,
            MODULE["CoverageCounts"](
                attempted=15,
                succeeded=13,
                allocated_fallbacks=1,
                uncompiled=1,
                osr_requested=6,
                osr_succeeded=5,
                osr_unavailable=1,
            ),
        )
        self.assertEqual(len(coverage.failures), 4)
        self.assertEqual(coverage.failures[0].stage, "direct")
        self.assertEqual(coverage.failures[0].function_index, 6)
        self.assertEqual(coverage.failures[0].instruction_index, 26)
        self.assertEqual(coverage.failures[0].opcode_name, "v128_load")
        self.assertEqual(coverage.failures[0].opcode, 0xFD000000)
        self.assertEqual(coverage.failures[0].reason, "unsupported direct instruction")
        successful_fallbacks = MODULE["successful_allocated_fallbacks"](coverage)
        self.assertEqual(len(successful_fallbacks), 1)
        self.assertEqual(successful_fallbacks[0].function_index, 7)
        self.assertEqual(successful_fallbacks[0].opcode_name, "table_get")

    def test_discovers_wasm_modules_recursively(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.wasm"
            second = root / "nested" / "second.wasm"
            ignored = root / "ignored.wat"
            second.parent.mkdir()
            first.touch()
            second.touch()
            ignored.touch()

            top_level_modules = MODULE["discover_modules"]([root])
            recursive_modules = MODULE["discover_modules"]([root], recursive=True)

        self.assertEqual(top_level_modules, [first.resolve()])
        self.assertEqual(recursive_modules, sorted([first.resolve(), second.resolve()]))

    def test_limits_printed_failure_locations(self):
        trace = "\n".join(
            f"direct compilation of function {index} failed: instruction 2 (v128_load, opcode 0xfd000000): unsupported direct instruction; trying allocated-bytecode frontend"
            for index in range(4)
        )
        coverage = MODULE["parse_trace"](Path("fixture.wasm"), trace)
        output = StringIO()

        with redirect_stdout(output):
            MODULE["print_failure_groups"]([coverage], 2)

        self.assertIn("4 x direct v128_load", output.getvalue())
        self.assertIn("fixture.wasm:0@2, fixture.wasm:1@2, ... 2 more", output.getvalue())

    def test_collects_a_phase_aware_test_command(self):
        trace = (
            "direct compilation summary: attempted=3 succeeded=2 allocated_fallbacks=0 "
            "uncompiled=1; osr_requested=1 osr_succeeded=1 osr_unavailable=0"
        )
        coverage = MODULE["run_traced_command"](
            Path("specification-tests"),
            [sys.executable, "-c", f"import sys; print({trace!r}, file=sys.stderr)"],
            10,
        )

        self.assertIsNone(coverage.invocation_error)
        self.assertEqual(coverage.summary_count, 1)
        self.assertEqual(coverage.counts.attempted, 3)
        self.assertEqual(coverage.counts.succeeded, 2)
        self.assertEqual(coverage.counts.uncompiled, 1)

    def test_substitutes_phase_aware_test_inputs(self):
        input_path = Path("Spec/address.js")

        command = MODULE["substitute_trace_input"](
            ["test-wasm", "--filter", "/{name}", "--fixture={input}"],
            input_path,
        )

        self.assertEqual(
            command,
            ["test-wasm", "--filter", "/address.js", "--fixture=Spec/address.js"],
        )


if __name__ == "__main__":
    unittest.main()
