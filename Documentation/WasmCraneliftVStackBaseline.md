# Wasm Cranelift vstack baseline

Recorded on 2026-07-29 at 20:19 BST, before changing how raw Wasm calls interact with the Cranelift virtual stack.

## Environment

- Ladybird revision: `d513595f2fe01f299d7d5d79d014e621cf06a560`
- `js-benchmarks` revision: `c66654a218740e2e685b97e39de6554e53e781c5`
- Host: macOS 26.6 (25G72), arm64, Darwin 25.6.0
- Build: `Build/release`
- `ENABLE_CRANELIFT_JIT=ON`
- `WASM_CRANELIFT_DEBUG=ON`
- `Build/release/bin/wasm` SHA-256: `4de60f201a20d059e63f47917d7688b701bb1d66cfa1e64bfd2a12545a091794`
- Iterations per benchmark: 3
- Warm-up iterations: 0

`ninja -C Build/release -n wasm` reported no work after rebuilding the executable.

The benchmark repository had a modified `run.py` and untracked Wasm-to-JS microbenchmarks. The direct-call, CoreMark, and Rust Wasm inputs used below were otherwise taken from the revision listed above.

## Method and Cranelift verification

The benchmarks were run with:

```sh
python3 run.py \
    --wasm-executable /Users/lukewilde/Repositories/ladybird/Build/release/bin/wasm \
    --iterations 3
```

The direct-call microbenchmarks were run as a filtered `WasmMicroBench` suite containing only `call-[0-9][0-9]-args.wasm`. CoreMark and the Rust workloads were run with:

```sh
python3 run.py \
    --wasm-executable /Users/lukewilde/Repositories/ladybird/Build/release/bin/wasm \
    --suites WasmCoremark,WasmRustBench \
    --iterations 3
```

The normal benchmark processes were run outside the command sandbox. Inside the sandbox, Cranelift's out-of-process compiler could not use its POSIX shared-memory path, silently leaving execution in the bytecode interpreter. All numbers from those sandboxed runs were discarded.

At the time this baseline was recorded, the `wasm` utility did not supply a
`CompileCacheConfig`, and every iteration started a new process. Two explicit probes both
reported compiling all four functions in `call-00-args.wasm`. No cached Cranelift blob was
installed, so the recorded wall-clock times include parsing, validation, fresh Cranelift
compilation, instantiation, and execution.

Native dumps confirmed that `run_microbench`, CoreMark's `run`, and every Rust `_start` export had installed native bodies. Complete module coverage was:

| Module                      | Native defined functions | Total defined functions | Notes                                                 |
| --------------------------- | -----------------------: | ----------------------: | ----------------------------------------------------- |
| Direct-call microbenchmarks |                      All |                     All | Clean Cranelift measurement                           |
| `coremark-minimal.wasm`     |                       15 |                      15 | Clean Cranelift measurement                           |
| `base64-bench.wasm`         |                      188 |                     192 | A few support functions fall back                     |
| `blake3-bench.wasm`         |                      156 |                     171 | Benchmark `main` and SIMD hashing functions fall back |
| `json-bench.wasm`           |                      252 |                     256 | A few support functions fall back                     |
| `regex-match-bench.wasm`    |                     1534 |                    1550 | Several strategy/support functions fall back          |
| `sha512-bench.wasm`         |                      154 |                     158 | A few support functions fall back                     |

The Rust results are therefore mixed-mode measurements. In particular, the Blake3 result must not be treated as a Cranelift performance baseline: its hot SIMD implementation is not compiled by the current backend.

## Direct-call microbenchmarks

Times are process wall-clock seconds; lower is better.

| Benchmark           |    Run 1 |    Run 2 |    Run 3 |     Mean | Standard deviation |
| ------------------- | -------: | -------: | -------: | -------: | -----------------: |
| `call-00-args.wasm` | 0.077851 | 0.081667 | 0.075921 | 0.078479 |           0.002924 |
| `call-01-args.wasm` | 0.077181 | 0.131182 | 0.077007 | 0.095124 |           0.031228 |
| `call-02-args.wasm` | 0.077459 | 0.077800 | 0.077987 | 0.077748 |           0.000268 |
| `call-03-args.wasm` | 0.086427 | 0.085615 | 0.084654 | 0.085565 |           0.000888 |
| `call-04-args.wasm` | 0.078503 | 0.079689 | 0.079176 | 0.079123 |           0.000595 |
| `call-16-args.wasm` | 1.385108 | 1.385586 | 1.372802 | 1.381165 |           0.007247 |
| `call-32-args.wasm` | 1.695452 | 1.718998 | 1.696257 | 1.703569 |           0.013368 |

`call-01-args.wasm` had one clear 0.131-second outlier; its other two runs were both approximately 0.077 seconds.

## CoreMark

CoreMark reports a score; higher is better.

| Benchmark               |     Run 1 |     Run 2 |     Run 3 |      Mean | Standard deviation |
| ----------------------- | --------: | --------: | --------: | --------: | -----------------: |
| `coremark-minimal.wasm` | 18940.590 | 19043.992 | 18682.277 | 18888.953 |            186.304 |

## Rust workloads

Times are process wall-clock seconds; lower is better. These are mixed-mode measurements as described above.

| Benchmark                |     Run 1 |     Run 2 |     Run 3 |      Mean | Standard deviation |
| ------------------------ | --------: | --------: | --------: | --------: | -----------------: |
| `base64-bench.wasm`      |  0.795540 |  0.794689 |  0.789203 |  0.793144 |           0.003439 |
| `blake3-bench.wasm`      | 27.817997 | 27.801773 | 27.329195 | 27.649655 |           0.277645 |
| `json-bench.wasm`        |  1.984113 |  1.993740 |  2.005927 |  1.994593 |           0.010932 |
| `regex-match-bench.wasm` |  1.043201 |  1.050732 |  1.032674 |  1.042202 |           0.009070 |
| `sha512-bench.wasm`      |  0.883118 |  0.881966 |  0.884127 |  0.883071 |           0.001081 |

## Wasm-to-JS microbenchmarks

No baseline was recorded. Each benchmark performs 50 million calls to an imported JavaScript function. The first `call-wasm-to-js-00-args.wasm` iteration had not completed after roughly three minutes, so the suite was stopped rather than blocking the remaining measurements.

## Raw results

- `/private/tmp/ladybird-wasm-direct-call-baseline-before-vstack.json`
    - SHA-256: `cd3cac72a3b8bc719b4f2f12aa7144c9d03f3721bd94b08bb1fe97a56dd00236`
- `/private/tmp/ladybird-wasm-coremark-rust-baseline-before-vstack.json`
    - SHA-256: `201a3c279713829a4b69aa8bb19474235e421eefa9e6921dd96baf00f14bc67a`

For an after-change comparison, rebuild `Build/release/bin/wasm`, confirm `ninja -C Build/release -n wasm` reports no work, run outside the command sandbox, retain fresh compilation with no `CompileCacheConfig`, and use the same three-iteration commands.

## Phase 1 comparison

The first phase keeps vstack enabled for functions containing raw calls. At each raw-call boundary it materializes the complete live vstack for the interpreter stack ABI, then reconstructs the vstack from the helper's results and restores the original real-stack top.

The comparison used the same build and three-iteration method. The local Python installation did not contain the benchmark runner's `brotli` and `tabulate` dependencies, so no-op local modules supplied those two imports; neither code path participates in running or measuring these already-present benchmark files.

| Benchmark               | Baseline mean | Phase 1 mean | Change |
| ----------------------- | ------------: | -----------: | -----: |
| `call-16-args.wasm`     |      1.381165 |     1.408148 |  +2.0% |
| `call-32-args.wasm`     |      1.703569 |     1.707041 |  +0.2% |
| `coremark-minimal.wasm` |     18888.953 |    18986.092 |  +0.5% |

The direct-call times are lower-is-better; the CoreMark score is higher-is-better. These differences are small enough to treat phase 1 as neutral on these workloads. That is expected for the call-only microbenchmarks: phase 1 restores vstack use between boundaries, but it still writes every live argument to the real stack at every raw call. Spilling only the required argument suffix is the phase that can remove that boundary traffic.

Phase 1 raw results:

- `/private/tmp/ladybird-wasm-direct-call-after-vstack-phase1.json`
    - SHA-256: `65e22ae955a2feb23fcb465c504d4c18dd48fe4c3fda266ddbac1c6e257b397b`
- `/private/tmp/ladybird-wasm-coremark-after-vstack-phase1.json`
    - SHA-256: `61d1575a04c4eb452baf647f5a046f0c77e0b48c21f75fc81e066fa166e917d7`

## Local promotion cutoff experiment

The architecture-specific all-or-nothing cutoff for promoting Wasm locals to Cranelift
variables was removed. A fresh cutoff-enabled baseline and cutoff-disabled comparison were
recorded from the same phase 1 worktree and build configuration.

| Benchmark                | Cutoff enabled | Cutoff disabled | Change |
| ------------------------ | -------------: | --------------: | -----: |
| `coremark-minimal.wasm`  |      19178.833 |       18959.578 |  -1.1% |
| `base64-bench.wasm`      |       0.791530 |        0.617097 | -22.0% |
| `blake3-bench.wasm`      |      27.348217 |       27.318625 |  -0.1% |
| `json-bench.wasm`        |       1.990488 |        1.988430 |  -0.1% |
| `regex-match-bench.wasm` |       0.664592 |        0.945974 | +42.3% |
| `sha512-bench.wasm`      |       0.886174 |        1.028967 | +16.1% |

CoreMark is a score, so higher is better. The Rust workload values are process wall-clock
seconds, so lower is better. Regex peak RSS increased from 245.6 MB to 398.0 MB, and SHA-512
peak RSS increased from 35.2 MB to 43.3 MB. Blake3 remains a mixed-mode measurement whose hot
SIMD implementation is not compiled by Cranelift.

The correct d3wasm module was also instantiated without executing an export:

```sh
/usr/bin/time -lp \
    Build/release/bin/wasm \
    --instantiate \
    --export-noop \
    /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm
```

Each invocation was a new process and compiled without a `CompileCacheConfig`. The cutoff-enabled
runs took 4.27 and 3.91 seconds (mean 4.09 seconds); the cutoff-disabled runs took 6.68 and
7.42 seconds (mean 7.05 seconds), a 72.4% load-time regression. Mean user CPU time increased
from 18.62 to 25.07 seconds.

Raw benchmark results:

- `/private/tmp/ladybird-wasm-locals-cutoff-on.json`
    - SHA-256: `caa1848702565d0fa1f778568c948ac817aa0fe033e1f804abadaf9d4e7c6d39`
- `/private/tmp/ladybird-wasm-locals-cutoff-off.json`
    - SHA-256: `668a813623cc0db06db9a798f342b341005a32065c24c3b564194715cfd20279`

## Internal phase timing experiment

`wasm --benchmark-timings` reports separate arrays of parse, validation, native compilation,
instantiation, and exported-function execution times. These measurements used normal uncached
compilation in a new process for every iteration.

### Local promotion cutoff

Each configuration was measured for three iterations. Times are seconds; lower is better. Change
is the cutoff-removed value relative to the cutoff-enabled value.

| Benchmark                | Compile enabled | Compile removed | Compile change | Execute enabled | Execute removed | Execute change |
| ------------------------ | --------------: | --------------: | -------------: | --------------: | --------------: | -------------: |
| `coremark-minimal.wasm`  |        0.010708 |        0.011697 |          +9.2% |       22.408484 |       21.551793 |          -3.8% |
| `base64-bench.wasm`      |        0.026835 |        0.025240 |          -5.9% |        0.758194 |        0.564606 |         -25.5% |
| `blake3-bench.wasm`      |        0.030202 |        0.033540 |         +11.1% |       27.967244 |       27.604140 |          -1.3% |
| `json-bench.wasm`        |        0.043118 |        0.045881 |          +6.4% |        1.965731 |        1.959169 |          -0.3% |
| `regex-match-bench.wasm` |        0.303308 |        0.678926 |        +123.8% |        0.256007 |        0.239010 |          -6.6% |
| `sha512-bench.wasm`      |        0.027842 |        0.067856 |        +143.7% |        0.870341 |        0.942563 |          +8.3% |

The phase split changes the interpretation of the earlier process-time comparison:

- Base64's improvement is execution performance; its native compilation time is effectively
  unchanged.
- Regex executes 6.6% faster without the cutoff, but takes 2.24 times as long to compile. The
  earlier process-time regression was compilation dominating a short-running benchmark.
- SHA-512 regresses in both compilation and execution.
- JSON execution is neutral.
- Blake3 remains unsuitable for judging Cranelift-generated code because its hot SIMD path runs in
  the interpreter.

CoreMark's reported score increased from 18330.112 with the cutoff to 19069.177 without it. Its
three sequential runs were noisier than the other workloads, so the magnitude should not be treated
as precise.

### Raw-call vstack

The 16- and 32-argument direct-call tests were measured for five iterations after a discarded
compiler warm-up. The disabled configuration temporarily restored the old zero-vstack-depth choice
for functions containing raw calls while retaining the current raw-call implementation.

| Benchmark           | Vstack disabled | Standard deviation | Vstack enabled | Standard deviation | Change |
| ------------------- | --------------: | -----------------: | -------------: | -----------------: | -----: |
| `call-16-args.wasm` |        1.396044 |           0.023166 |       1.444800 |           0.021299 |  +3.5% |
| `call-32-args.wasm` |        1.766255 |           0.022906 |       1.786138 |           0.032695 |  +1.1% |

These call-only microbenchmarks still materialize every live argument at every raw call in phase 1,
so they do not exercise the intended selective-spill improvement.

Phase timing raw results:

- `/private/tmp/wasm-phase-current-no-cutoff.json`
    - SHA-256: `37f2b2ef9cc7e6dddfc354afa57652d1de554132d25869f1181ad155efd6cb2c`
- `/private/tmp/wasm-phase-cutoff-on.json`
    - SHA-256: `137b6269eb8f92e231a738e19c7f9b03a6e6d342ae7ff7fe52563a8658e7951a`
- `/private/tmp/wasm-phase-vstack-disabled.json`
    - SHA-256: `8b0b947803156cf887b088f2bed87a7372026fbe7cbfd69a2fe1998b8a71bb54`
- `/private/tmp/wasm-phase-vstack-enabled.json`
    - SHA-256: `7a32b1a7e529fca708a07c60358ce4ff1711632f247e3a0d848584e714e6bf25`

## SHA-512 local-promotion investigation

The local-promotion cutoff experiment showed that `sha512-bench.wasm` regressed in both native
compilation and execution after removing the all-or-nothing local cutoff. A follow-up investigation
isolated the hot compression function, measured its native spill traffic, swept the number of
promoted locals, and tested whether synthetic tier-up control-flow edges were responsible.

All commands in this section were run outside the command sandbox so that Cranelift's
out-of-process compiler could use its POSIX shared-memory path. Every measurement used a new
`wasm` process with no `CompileCacheConfig`.

The phase measurements used:

```sh
Build/release/bin/wasm \
    --benchmark-timings \
    --execute _start \
    --wasi \
    /Users/lukewilde/Repositories/js-benchmarks/WasmRustBench/sha512-bench.wasm
```

Native code measurements used:

```sh
Build/release/bin/wasm \
    --benchmark-timings \
    --dump-native \
    --print-function 16 \
    --wasi \
    /Users/lukewilde/Repositories/js-benchmarks/WasmRustBench/sha512-bench.wasm
```

### Hot function

Function 16 is:

```text
sha2::sha512::soft::compress
```

Its Wasm body is 11,203 bytes. It has three parameters and 35 declared `i64` locals, for 38
Cranelift locals in total. The Wasm body contains 2,315 local operations and approximately 350
`local.set`/`local.tee` mutations. Locals 11 through 37 account for most of those operations and
remain active throughout the unrolled SHA-512 compression rounds.

With the old AArch64 cutoff, a function with more than 24 locals promoted none of them. Removing
the cutoff promotes all 38.

### Matched cutoff comparison

A fresh five-run comparison produced the following phase means:

| Metric                   | Cutoff enabled | Cutoff removed |  Change |
| ------------------------ | -------------: | -------------: | ------: |
| Native compilation       |       0.023452 |       0.065645 | +179.9% |
| Execution                |       0.823861 |       0.918128 |  +11.4% |
| Native code size         |   22,188 bytes |   38,592 bytes |  +73.9% |
| Native instructions      |          5,547 |          9,648 |  +73.9% |
| Stack-pointer references |             12 |          3,473 |       — |
| Stack-based loads        |              6 |          2,636 |       — |
| Stack-based stores       |              6 |            837 |       — |
| Maximum stack offset     |       16 bytes |    6,208 bytes |       — |

The cutoff-enabled stack references are the callee-save prologue and epilogue. The cutoff-removed
function has a roughly 6 KiB native frame and thousands of allocator-generated spill operations.
The increase in compilation plus execution time accounts for almost all of the earlier process
wall-clock regression.

### Promotion-count sweep

The Rust compiler was temporarily changed to accept a promotion-count environment variable. This
allowed the same compiler binary to promote a prefix of 0 through 38 locals without rebuilding
between points. Each intermediate phase value below is a two-run mean; the five-run endpoint
comparison above is the more reliable measurement.

| Promoted locals | Native bytes | Stack references | Stack loads | Stack stores | Max stack offset | Compile seconds | Execute seconds |
| --------------: | -----------: | ---------------: | ----------: | -----------: | ---------------: | --------------: | --------------: |
|               0 |       22,188 |               12 |           6 |            6 |         16 bytes |        0.025306 |        0.829462 |
|               4 |       21,740 |               12 |           6 |            6 |         16 bytes |        0.025402 |        0.797308 |
|               8 |       21,740 |               33 |          18 |           15 |         24 bytes |        0.026800 |        0.798261 |
|              12 |       21,568 |               92 |          56 |           36 |         80 bytes |        0.020878 |        0.803516 |
|              16 |       23,324 |              594 |         411 |          183 |      1,184 bytes |        0.022773 |        0.872962 |
|              20 |       24,984 |            1,063 |         750 |          313 |      2,224 bytes |        0.025053 |        0.961659 |
|              24 |       28,124 |            1,672 |       1,236 |          436 |      3,176 bytes |        0.031887 |        0.983126 |
|              28 |       31,672 |            2,249 |       1,692 |          557 |      3,992 bytes |        0.041041 |        1.025240 |
|              32 |       37,368 |            3,207 |       2,426 |          781 |      5,720 bytes |        0.053050 |        1.022172 |
|              38 |       38,592 |            3,473 |       2,636 |          837 |      6,208 bytes |        0.065593 |        0.907927 |

Promoting 4 through 12 locals improved execution relative to promoting none. The spill cliff
appears between 12 and 16 promoted locals: four additional promoted locals increase stack
references from 92 to 594. This is not a linear cost per additional local. Once the live set
exceeds the available register budget, reload temporaries displace other live values and create a
spill cascade.

The 32-to-38 execution result is non-monotonic. The final six locals are themselves heavily used,
so promoting them removes some explicit local-frame traffic even while increasing spills. The
intermediate points used only two runs and should guide policy experiments rather than establish a
fixed architecture threshold.

All temporary promotion controls were removed after the sweep.

### SSA variables and `def_var`

Cranelift instructions already produce SSA values. `def_var` binds one of those existing values as
the current definition of a mutable frontend `Variable`; it does not directly emit a copy or
allocate a stack slot:

```text
%result = i64.add %lhs, %rhs
def_var(local, %result)
```

For promoted Wasm locals, the bindings are created by `write_local_inline!`, `write_local_f32!`,
and `write_local_f64!`. `local.set`, `local.tee`, and fused synthetic local-writing operations
reach those macros. When a local is not promoted, the same macros instead load from or store to its
canonical slot under `locals_base`.

`write_dst!` also uses `def_var` for bytecode registers and vstack slots. That behavior remained
constant throughout the promotion sweep, so it supplies background register pressure but does not
explain the cutoff-dependent delta.

At a control-flow merge, different reaching definitions of a frontend `Variable` become a
phi-like Cranelift block parameter. A loop-carried local therefore has a value live into the loop
header and another definition feeding the backedge. Repeated mutation creates many SSA versions,
but it does not imply one native stack slot per mutation:

- Sequential versions can reuse the same register.
- Non-overlapping spills can reuse stack slots.
- One SSA value can be split into several live intervals.
- One spilled value can require several reloads.

The 6 KiB native frame includes spills for local SSA versions, vstack values, intermediate
expressions, and allocator temporaries. It is not simply 38 locals multiplied by eight bytes.

### Native frame and spill-slot accounting

A temporary diagnostic read the `CompiledCode` frame metadata and counted active CLIF
instructions, values, and block parameters before and after optimization. The function creates no
explicit sized or dynamic Cranelift stack slots in either configuration:

| Metric                        | 0 locals promoted | 38 locals promoted |
| ----------------------------- | ----------------: | -----------------: |
| Frontend active instructions  |            10,209 |              7,348 |
| Frontend active SSA values    |             9,424 |              7,374 |
| Frontend block parameters     |                26 |                141 |
| Optimized active instructions |             5,568 |              5,787 |
| Optimized active SSA values   |             4,780 |              5,813 |
| Optimized block parameters    |                18 |                136 |
| Cranelift-reported frame size |          80 bytes |        6,304 bytes |
| Explicit sized stack slots    |                 0 |                  0 |
| Explicit dynamic stack slots  |                 0 |                  0 |

The promoted prologue saves `x29`/`x30`, saves five additional register pairs, and subtracts 6,224
bytes from `sp`. Cranelift's 6,304-byte reported frame is the 80 bytes of additional callee saves
plus that 6,224-byte fixed allocation; the `x29`/`x30` setup pair is separate. The unpromoted
function reports only the 80 bytes of callee saves and has no fixed allocation. The entire
6,224-byte difference is therefore allocator spill storage and alignment, not a frontend local
array. Its highest observed spill reference is `[sp, #6208]`, which reaches logical 8-byte
position 776 before final frame alignment.

This is the qualified version of “one stack slot per mutation”: each mutation supplies a new SSA
definition, and loop merges add phi-like block parameters, but the native allocation unit is a
regalloc2 spill set rather than a Wasm local or mutation. The promoted form has 1,033 more active
SSA values and 118 more block parameters after optimization. Spills also include the bytecode
registers, vstack values, and arithmetic intermediates, so the number of candidate spill ranges is
much larger than either 38 locals or approximately 350 local mutations.

Cranelift 0.116.1 uses regalloc2 0.11.2's Ion allocator for the default `backtracking` setting.
regalloc2 attempts to reuse a spill slot when spill-set ranges do not overlap, but two heuristics
limit that reuse:

- A spill set stores one aggregate range for all live ranges in its bundle. regalloc2's own
  comment calls this pessimistic for fragmented bundles with substantial gaps.
- The spill-slot allocator probes at most 10 existing slots before allocating a new one.

A temporary build changed only that 10-probe limit to exhaustive probing. It reduced the reported
frame from 6,304 to 3,088 bytes and the fixed allocation from 6,224 to 3,008 bytes. Native code
size remained effectively unchanged at 38,616 bytes instead of 38,592. This shows that roughly
half of the dramatic frame size comes from deliberately bounded spill-slot coloring rather than
values that must all occupy the stack simultaneously. The remaining roughly 3 KiB still reflects
the large and conservatively represented set of overlapping live ranges. No runtime conclusion
was drawn from this diagnostic-only allocator experiment.

The SHA-512 cliff is primarily a liveness problem. Many hash-state and message-schedule values are
simultaneously live across the outer loop backedge. Once the register budget is exceeded, values
that are not used in a particular block may still be spilled because they are live through that
block and needed later. A truly dead value does not need preservation.

The frontend's `flush_locals!` is a separate conservative mechanism. It writes every promoted
local whose compile-time `dirty_locals` flag was ever set when synchronizing at the epilogue or
unreachable trap path. Those writes target `locals_base`, not the native stack, and occur at only
two sites. They do not account for the thousands of stack-pointer-relative operations in function 16.

### Why the existing locals frame can be faster

The memory-backed representation gives every Wasm local one stable canonical slot. A local is
loaded only when the Wasm program reads it, and the resulting temporary can die immediately after
use. A mutation overwrites the same slot instead of extending a value across future control-flow
edges.

Both the existing local frame and native spill frame are expected to remain in L1 cache for this
workload. The important differences are the number and placement of transfers, the length of live
ranges, and instruction-cache pressure. The cutoff-enabled function is 42.5% smaller than the
all-promoted function.

Cranelift accounts for register pressure by spilling, but only after the frontend has committed to
SSA values. At that point it does not know that a value originated from a Wasm local with a
preferred existing home. It can split and spill the live range, but it cannot choose to return the
value to `locals_base` as a higher-level representation decision.

An LLVM backend would have the same finite-register problem. LLVM's optimizer and register
allocator might reduce the severity through more mature live-range splitting, spill placement,
spill-slot reuse, rematerialization, and machine scheduling. It would not make an inherently large
live set fit in registers. A Wasm-to-LLVM frontend would also be reconstructing SSA after the
original source compiler had already lowered its representation to Wasm locals and stack
operations.

### Control-flow edge metadata

Wasm local types are not lost at control-flow edges. Local types are immutable, are passed through
`local_types`, and determine the type of each declared Cranelift `Variable`. Cranelift's
`def_var`/`use_var` machinery reconstructs reaching definitions across sealed blocks.

The operand-stack edge model is more conservative:

- `ControlFrame` records stack depth, block parameters/results, and a limited bank snapshot.
- The `F32` and `F64` bank information is reset to the always-valid integer representation at
  merges.
- Maximum vstack depth is estimated by counting stack destinations rather than tracking exact
  depth over every edge.

That conservatism can increase background IR size and register pressure, causing a spill threshold
to arrive earlier. It does not directly explain the SHA-512 regression because function 16's
declared locals are all `i64`, and the operand-stack behavior was identical at every promotion
point.

More precise edge metadata would still help a promotion policy determine which locals and operand
stack values are genuinely live across each edge.

### Synthetic tier-up experiment

Function 16 contains one synthetic tier-up checkpoint immediately after its outer loop header.
Checkpoint generation was first disabled with a temporary threshold change. A second experiment
used a temporary environment gate and ten alternating enabled/disabled process pairs from the same
binary to avoid rebuild-order drift.

| Metric              | Tier-up enabled | Tier-up disabled |  Change |
| ------------------- | --------------: | ---------------: | ------: |
| Native code size    |    38,592 bytes |     38,236 bytes |   -0.9% |
| Native instructions |           9,648 |            9,559 |   -0.9% |
| Stack references    |           3,473 |            3,451 |   -0.6% |
| Stack loads         |           2,636 |            2,626 |     -10 |
| Stack stores        |             837 |              825 |     -12 |
| Compilation median  |        0.065154 |         0.065411 | Neutral |
| Execution median    |        0.925287 |         0.910549 |   -1.5% |

The paired median execution improvement was 13.6 ms. The paired compilation difference was
0.064 ms and is noise. The tier-up dispatch edge therefore has a small measurable execution cost,
but removing it eliminates less than 1% of the spill traffic and does not explain the
local-promotion cliff.

All temporary tier-up controls were removed after the measurement.

### Composable optimization directions

If a local is needed after a control-flow edge, its value must exist either as an SSA value
crossing the edge or in memory. Avoiding a long SSA live range therefore requires deliberately
materializing the value to memory and reloading it later.

The following approaches are complementary rather than exclusive:

1. **Selective function-wide promotion**

    Promote only locals whose expected benefit fits within a register-pressure budget. Supporting
    arbitrary local selection requires replacing the current prefix-shaped `Vec<Variable>` test
    with a representation such as `Vec<Option<Variable>>`.

2. **Block-local caching**

    Treat `locals_base` as canonical storage. Load a local on first use in a block, retain its
    current raw Cranelift `Value` for subsequent operations in that block, and store its final value
    only if it is dirty and live-out.

3. **Explicit SSA edge values**

    Pass selected high-value locals as block arguments across low-pressure edges. Explicit block
    parameters do not by themselves remove liveness; they provide control over exactly which
    values cross each edge.

4. **Pressure-aware edge splitting**

    Keep a local in SSA through several blocks, but materialize it before a loop backedge or other
    edge where the estimated live set exceeds the available register budget. Reload it lazily in
    the successor.

Ordinary backward local liveness can drive these decisions:

```text
live_out(block) = union(live_in(successor))
live_in(block)  = use(block) union (live_out(block) - def(block))
```

For each local at each edge:

- A hot, affordable value can cross as SSA.
- A block-local value can remain a raw Cranelift `Value` and then die.
- A dirty, live-out value that is too expensive to carry can be stored to `locals_base`.
- A value overwritten before its next read needs neither an edge store nor a successor load.

The lowering needs an explicit coherence state for each cached local:

- `clean`: the SSA/cache value agrees with memory.
- `dirty`: the SSA/cache value is newer than memory.
- `memory-only`: no cached value is valid.

Interpreter reentry, tier-up, trap, and other synchronization boundaries must materialize the
required dirty state.

A practical implementation sequence is:

1. Add per-local function-wide promotion.
2. Build local use/def and live-in/live-out sets over the reconstructed CFG.
3. Cache unpromoted locals within blocks.
4. Carry selected live values explicitly across edges.
5. Split lower-priority values at high-pressure edges.

Cranelift's register allocator remains responsible for residual pressure. These frontend policies
would prevent it from receiving a function where every mutable Wasm local becomes a long-lived
competitor for physical registers.

### Phase 1: per-local function-wide promotion

The first phase retains unconditional promotion for functions estimated to produce at most 256
local SSA definitions. Above that threshold, it promotes up to ten locals on AArch64 and eight on
other targets. Locals with fewer writes are selected first to limit SSA definitions and merge
pressure; read frequency breaks ties.

The benchmark comparison used alternating all-promoted and selective-promoted processes from the
same build. Five pairs were recorded for every benchmark except Blake3, where one pair was used
because the hot SIMD implementation remains interpreted.

| Benchmark                | All promoted total | Selective total | All promoted execution | Selective execution |
| ------------------------ | -----------------: | --------------: | ---------------------: | ------------------: |
| `base64-bench.wasm`      |           0.626385 |        0.618173 |               0.561719 |            0.563739 |
| `blake3-bench.wasm`      |          27.560796 |       27.511792 |              27.495595 |           27.441105 |
| `json-bench.wasm`        |           2.016359 |        2.012090 |               1.928137 |            1.926216 |
| `regex-match-bench.wasm` |           0.941846 |        0.697705 |               0.232192 |            0.251194 |
| `sha512-bench.wasm`      |           1.006389 |        0.878776 |               0.910165 |            0.818717 |

The policy preserves the Base64 and JSON execution results that regressed when the budget was
applied indiscriminately. SHA-512 execution improves by 10.0%, while its native compilation time
falls from 0.062499 to 0.026550 seconds. Regex execution increases by 8.2%, but native compilation
falls from 0.548764 to 0.284487 seconds and reduces total time by 25.9%.

For SHA-512 function 16, selective promotion reduces native code from 38,592 to 21,988 bytes and
the Cranelift stack allocation from 6,224 to 48 bytes.

Three alternating uncached instantiation pairs were also recorded for
`build-wasm-full/d3wasm.wasm`:

| Metric                      | All promoted | Selective | Change |
| --------------------------- | -----------: | --------: | -----: |
| Process wall-clock mean     |     6.805495 |  4.452559 | -34.6% |
| Native compilation-time sum |     5.457815 |  3.102976 | -43.1% |

Two inverse selection orders were tested from the same binary with temporary environment
switches:

- Choosing the least-read locals first and using most-written as a tie-breaker improved SHA-512
  execution from 0.811697 to 0.806747 seconds, but increased d3wasm mean wall time from 4.309831
  to 4.366828 seconds and native compilation from 2.994081 to 3.058516 seconds.
- Strictly reversing the production comparator—most-written first, then least-read—regressed
  SHA-512 mean execution from 0.820722 to 0.840214 seconds. Median d3wasm wall time increased from
  4.233302 to 4.338819 seconds, and median native compilation increased from 2.947562 to 3.056547
  seconds.

The temporary switches were removed and the low-mutation-first policy retained.

### Peak live-local cutoff experiment

Ali proposed replacing the declared-local cutoff with the maximum number of simultaneously live
locals across the function's control-flow graph. A temporary implementation reconstructed basic
blocks from the structured Wasm control instructions, computed block-local use and definition
sets, solved live-in/live-out sets to a fixed point, and scanned each block backwards to find the
peak live-local count. Targeted tests covered an `if` merge and a loop backedge.

The first experiment reproduced the old all-or-nothing policy, but compared the peak live-local
count with the AArch64 cutoff of 24 instead of comparing the total declared-local count. This
correctly classified SHA-512's function 16 as high pressure: 36 of its 38 locals are
simultaneously live. It consequently promoted none of them.

Five alternating process pairs gave:

| SHA-512 metric | Selective promotion | Peak-live cutoff 24 | Change |
| -------------- | ------------------: | ------------------: | -----: |
| Compilation    |            0.025464 |            0.025308 |  -0.6% |
| Execution      |            0.811722 |            0.823441 |  +1.4% |
| Process wall   |            0.866914 |            0.879758 |  +1.5% |

The live cutoff identifies the pressure correctly, but the all-or-nothing response is worse than
retaining ten selectively chosen locals.

The same cutoff improved uncached d3wasm instantiation, but caused a large Base64 regression:

| Benchmark                | Selective wall | Live-24 wall | Change |
| ------------------------ | -------------: | -----------: | -----: |
| `base64-bench.wasm`      |       0.626259 |     0.800165 | +27.8% |
| `json-bench.wasm`        |       1.999931 |     2.004015 |  +0.2% |
| `regex-match-bench.wasm` |       0.686277 |     0.674980 |  -1.6% |
| `sha512-bench.wasm`      |       0.866914 |     0.879758 |  +1.5% |
| `blake3-bench.wasm`      |      27.431528 |    27.518689 |  +0.3% |
| d3wasm instantiation     |       4.333171 |     3.955255 |  -8.7% |

Base64's two large functions have 24 live locals out of 26 and 26 live locals out of 31. The
cutoff of 24 therefore promotes the first function and disables promotion for the second. Raising
the cutoff to 26 restores Base64.

A two-run exploratory cutoff sweep showed that d3wasm has another sharp pressure cliff:

| Policy         | Native compilation | Process wall |
| -------------- | -----------------: | -----------: |
| Selective      |           2.987318 |     4.276450 |
| Live cutoff 24 |           2.644118 |     3.926515 |
| Live cutoff 26 |           2.641726 |     3.941492 |
| Live cutoff 28 |           2.783227 |     4.094531 |
| Live cutoff 32 |           4.298777 |     5.581831 |

Cutoff 26 was then compared with selective promotion using five alternating pairs for the Rust
benchmarks:

| Benchmark                | Selective compile | Live-26 compile | Selective execution | Live-26 execution | Selective wall | Live-26 wall |
| ------------------------ | ----------------: | --------------: | ------------------: | ----------------: | -------------: | -----------: |
| `base64-bench.wasm`      |          0.023012 |        0.023988 |            0.565127 |          0.563442 |       0.617891 |     0.617114 |
| `json-bench.wasm`        |          0.042042 |        0.045626 |            1.926690 |          1.915300 |       2.003214 |     1.995992 |
| `regex-match-bench.wasm` |          0.280727 |        0.278412 |            0.252706 |          0.246642 |       0.684081 |     0.675915 |
| `sha512-bench.wasm`      |          0.025104 |        0.023710 |            0.819217 |          0.836000 |       0.874814 |     0.889665 |

Three paired d3wasm runs contained one slow first selective sample, so medians are more
representative. Cutoff 26 reduced median native compilation from 2.919339 to 2.677616 seconds
(-8.3%) and median process wall time from 4.204276 to 3.960854 seconds (-5.8%).

Two alternating CoreMark pairs were neutral: the mean score changed from 18,983.142 to
18,927.577 (-0.3%).

Peak liveness is therefore useful pressure information, but a hard all-or-nothing cutoff remains
fragile:

- It improves d3wasm compilation and modestly improves Regex.
- It preserves Base64 only after tuning the cutoff from 24 to 26.
- It regresses SHA-512 because zero promoted locals are worse than a small selected set.
- Increasing the cutoff from 28 to 32 crosses another severe d3wasm compilation cliff.

The peak-live cutoff is not being adopted as the production policy. The liveness information is
instead a candidate input for block-local caching and pressure-aware edge splitting, where
high-mutation locals can be cached without becoming function-wide SSA values.

### Phase 2: bounded block-local caching

The first block-cache implementation cached every unpromoted local within each basic block. Dirty
values were stored to canonical local storage only when live-out, and the cache was cleared at
control-flow boundaries. On SHA-512 this recreated the original pressure problem within large
blocks:

| SHA-512 metric | Selective promotion | Unbounded block cache | Change |
| -------------- | ------------------: | --------------------: | -----: |
| Compilation    |            0.025387 |              0.061524 |  +142% |
| Execution      |            0.808194 |              1.018577 |   +26% |
| Process wall   |            0.863678 |              1.109103 |   +28% |

The block cache was then bounded and populated with the most frequently written unpromoted locals.
Function-wide promotion continues to select stable locals, while the block cache targets mutable
locals without carrying them across edges. A single-run SHA-512 budget sweep gave:

| Cached locals | Compilation | Execution | Process wall |
| ------------: | ----------: | --------: | -----------: |
|             0 |    0.287084 |  0.827984 |     1.146871 |
|             1 |    0.026042 |  0.802783 |     0.861645 |
|             2 |    0.024032 |  0.771703 |     0.827981 |
|             4 |    0.022899 |  0.774618 |     0.826074 |
|             6 |    0.024076 |  0.732416 |     0.786097 |
|             8 |    0.025677 |  0.797574 |     0.852870 |
|            12 |    0.040642 |  0.910408 |     0.981276 |

The zero-budget compilation sample contains a one-time cold-start outlier. Execution shows another
clear pressure cliff above six cached locals.

Seven alternating process pairs validated a budget of six on AArch64:

| SHA-512 metric | Selective promotion |  Cache 6 | Change |
| -------------- | ------------------: | -------: | -----: |
| Compilation    |            0.025835 | 0.023866 |  -7.6% |
| Execution      |            0.812384 | 0.729885 | -10.2% |
| Process wall   |            0.867998 | 0.783777 |  -9.7% |

Five alternating pairs on the other Rust workloads gave:

| Benchmark                | Selective compile | Cache-6 compile | Selective execution | Cache-6 execution | Selective wall | Cache-6 wall |
| ------------------------ | ----------------: | --------------: | ------------------: | ----------------: | -------------: | -----------: |
| `base64-bench.wasm`      |          0.022711 |        0.024033 |            0.574065 |          0.565459 |       0.626723 |     0.619547 |
| `json-bench.wasm`        |          0.044266 |        0.046767 |            1.918517 |          1.918964 |       1.997637 |     2.000149 |
| `regex-match-bench.wasm` |          0.297918 |        0.293452 |            0.256043 |          0.251537 |       0.707452 |     0.700115 |

One Blake3 pair was neutral within normal variation. Two alternating CoreMark pairs were also
neutral: the mean score changed from 18,867.609 to 18,873.617.

Three d3wasm instantiation pairs contained one slow selective sample. Median native compilation
changed from 3.064813 to 3.083184 seconds (+0.6%), while median process wall time changed from
4.420398 to 4.370380 seconds (-1.1%). The bounded cache therefore does not materially affect
d3wasm load time.

For SHA-512 function 16, the bounded cache trades more allocator spill traffic for fewer canonical
local accesses:

| Native-code metric | Selective promotion |      Cache 6 |
| ------------------ | ------------------: | -----------: |
| Code size          |        21,988 bytes | 23,268 bytes |
| Instructions       |               5,497 |        5,817 |
| Stack allocation   |            48 bytes |  1,392 bytes |
| Stack references   |                  49 |          686 |
| Stack loads        |                  28 |          484 |
| Stack stores       |                  21 |          202 |

Despite the additional allocator frame, execution improves because a bounded set of highly
mutable locals is reused within blocks rather than repeatedly reading and writing the
16-byte canonical `Value` slots. The fixed policy caches six unpromoted locals on AArch64, two on
x86-64, and none on other targets. All temporary benchmark controls were removed.

### Joint function-wide and block-local budget sweep

Function-wide promoted locals and block-local cached values both become Cranelift SSA values and
can therefore compete for physical registers while their live ranges overlap. A two-dimensional
budget sweep tested whether reducing function-wide promotion allowed a larger block cache.

The initial sweep used one process per point and is exploratory:

| Function-wide | Block-local | SHA compile | SHA execute | d3wasm compile | d3wasm wall |
| ------------: | ----------: | ----------: | ----------: | -------------: | ----------: |
|            10 |           0 |    0.024979 |    0.807890 |       3.095916 |    4.378176 |
|            10 |           2 |    0.023419 |    0.769705 |       3.029119 |    4.343476 |
|            10 |           4 |    0.021052 |    0.777999 |       3.081369 |    4.400769 |
|            10 |           6 |    0.026861 |    0.734656 |       3.000489 |    4.307794 |
|            10 |           8 |    0.025666 |    0.800888 |       3.041622 |    4.359677 |
|             8 |           2 |    0.022232 |    0.776381 |       3.265489 |    4.593884 |
|             8 |           4 |    0.026082 |    0.795399 |       3.200668 |    4.590661 |
|             8 |           6 |    0.024902 |    0.734652 |       2.908113 |    4.221727 |
|             8 |           8 |    0.029388 |    0.834982 |       2.911780 |    4.236584 |
|             6 |           4 |    0.022786 |    0.780048 |       2.979516 |    4.267861 |
|             6 |           6 |    0.023643 |    0.746802 |       2.933660 |    4.256898 |
|             6 |           8 |    0.025236 |    0.829076 |       2.974663 |    4.301100 |
|             4 |           6 |    0.031061 |    0.762119 |       2.887232 |    4.237435 |
|             4 |           8 |    0.025369 |    0.819387 |       2.870448 |    4.205351 |
|             4 |          10 |    0.031080 |    0.841323 |       2.841813 |    4.169885 |
|             0 |           6 |    0.022902 |    0.739386 |       2.624699 |    3.945415 |
|             0 |           8 |    0.024131 |    0.839299 |       2.633010 |    3.956186 |
|             0 |          10 |    0.034667 |    0.818726 |       2.964410 |    4.322630 |
|             0 |          12 |    0.041701 |    0.935830 |       2.869665 |    4.263926 |

The cache-size cliff remains when function-wide promotion is completely disabled: SHA-512
execution increases from 0.739386 seconds with six cached locals to 0.839299 seconds with eight.
The cliff is therefore intrinsic to the additional block-local SSA pressure or to the identities
of the two additional cached locals, rather than being caused only by competition with
function-wide locals.

Five forward/reverse rounds then compared the cache-six column and a zero-promotion,
zero-cache control. d3wasm only instantiated the module, so its results measure compilation and
loading; they provide no generated-code execution measurement.

| Function-wide | Block-local | SHA compile | SHA execute | d3wasm compile | d3wasm wall |
| ------------: | ----------: | ----------: | ----------: | -------------: | ----------: |
|            10 |           6 |    0.024102 |    0.740678 |       3.140609 |    4.459124 |
|             8 |           6 |    0.024173 |    0.732466 |       3.012156 |    4.333232 |
|             4 |           6 |    0.023346 |    0.763619 |       2.876236 |    4.209173 |
|             0 |           6 |    0.022835 |    0.738977 |       2.675062 |    4.002713 |
|             0 |           0 |    0.023330 |    0.837569 |       2.584650 |    3.900492 |

Reducing function-wide promotion lowers d3wasm compilation time, demonstrating compilation-side
competition. The SHA execution result is not monotonic, however: eight promoted locals are fastest,
four are slowest among the cache-six configurations, and zero approximately matches ten. Selection
identity and live-range shape therefore matter in addition to the total SSA-value count.

Five additional forward/reverse rounds compared the production `10 + 6` split with `8 + 6`:

| Benchmark | 10 + 6 compile | 8 + 6 compile | 10 + 6 execute | 8 + 6 execute |
| --------- | -------------: | ------------: | -------------: | ------------: |
| Base64    |       0.022815 |      0.023262 |       0.566487 |      0.563900 |
| JSON      |       0.042205 |      0.042705 |       1.920411 |      1.918935 |
| Regex     |       0.280893 |      0.279393 |       0.248952 |      0.248936 |

These execution results are neutral. Three CoreMark rounds were also neutral: the reported score
changed from 18,866.734 with `10 + 6` to 18,855.838 with `8 + 6` (-0.06%), while its measured
execution time changed from 21.785060 to 21.770094 seconds (-0.07%).

Temporary selection diagnostics confirmed that SHA-512 function 16 uses the same block-cache set
under both policies: locals 13, 17, 22, 25, 30, and 32. Reducing the function-wide budget removes
only locals 4 and 8 from the promoted set. Its native-code metrics change accordingly:

| Native-code metric |       10 + 6 |        8 + 6 |
| ------------------ | -----------: | -----------: |
| Code size          | 23,268 bytes | 23,208 bytes |
| Stack allocation   |  1,392 bytes |  1,360 bytes |
| Instructions       |        5,816 |        5,801 |
| Stack references   |          686 |          660 |
| Stack loads        |          484 |          467 |
| Stack stores       |          202 |          193 |

The comparison therefore isolates lower function-wide SSA pressure rather than a change in which
high-mutation locals receive block-local caching.

The `8 + 6` split was the best synthetic candidate from this sweep. Relative to `10 + 6`, it
reduced mean d3wasm native compilation by 4.1% and mean loading wall time by 2.8%, improved
SHA-512 execution by 1.1%, and was neutral on the other measured execution workloads. The
temporary budget controls were removed after measurement.

### d3wasm runtime validation of `8 + 6`

The `8 + 6` candidate was tested interactively in Ladybird and recorded in runs 18 through 20 of:

```text
/Users/lukewilde/Documents/d3wasm_4.trace
```

Run 18 records loading, run 19 records the cutscene transition into gameplay, and run 20 records a
heavy gameplay scene. Run 17 is the closest loading-shaped recording made immediately before the
candidate and is used only as a cautious baseline because the trace does not record which
out-of-process Cranelift compiler binary produced already-installed native code.

| Loading metric          |   Run 17 |   Run 18 |  Change |
| ----------------------- | -------: | -------: | ------: |
| Recording duration      | 13.024 s | 13.075 s |   +0.4% |
| Sampled main-thread CPU | 10.821 s | 10.888 s |   +0.6% |
| Sampled all-thread CPU  | 11.442 s | 11.415 s |   -0.2% |
| JIT share               |    66.0% |    64.6% | -1.4 pp |
| Wasm runtime share      |    17.9% |    18.8% | +0.9 pp |
| JavaScript share        |    10.0% |    10.5% | +0.5 pp |

The opposing subcategory movements are consistent with sampling noise. Loading is neutral rather
than confirming the synthetic 2.8% wall-time improvement.

The gameplay recordings have the same broad profile as the earlier gameplay-shaped run 15:

| Recording | Scenario               | Main CPU |  All CPU |   JIT | Wasm runtime | JavaScript | LibWeb | Interpreter |
| --------- | ---------------------- | -------: | -------: | ----: | -----------: | ---------: | -----: | ----------: |
| Run 15    | Earlier gameplay       | 59.953 s | 79.605 s | 37.7% |        26.6% |      14.5% |   9.0% |        1.6% |
| Run 19    | Cutscene into gameplay | 48.903 s | 66.987 s | 36.9% |        26.3% |      15.1% |   9.4% |        1.6% |
| Run 20    | Heavy gameplay         | 24.250 s | 30.004 s | 37.4% |        25.8% |      15.1% |  10.0% |        1.5% |

The scenarios and durations differ, so absolute CPU time is not comparable. The stable category
shares, unchanged dominant JIT-PC ordering between runs 19 and 20, and unchanged observed loading
time and frame rate make the candidate runtime-neutral in d3wasm. The AArch64 function-wide
promotion budget remains ten; the synthetic improvement is not sufficient evidence to change the
production policy to eight.

The profile does identify a separate opportunity: roughly 26% of heavy-scene main-thread samples
are in Wasm runtime and call-boundary code, while only roughly 1.5% are in the bytecode
interpreter. Frequent self-time appears in `call_address`, `Configuration::set_frame_lightweight`,
`Vector` capacity/append helpers, and `_platform_memmove`. This supports returning to the
compiled-to-compiled raw-call path after the local-cache edge work.

### Phase 3 plan: explicit cache values on control-flow edges

The current block cache always stores a dirty, live-out value to `locals_base` at the end of a
basic block, clears the cache, and reloads it on demand in the successor. The first explicit-edge
phase will remove that store/reload pair only where the CFG gives one unambiguous reaching value:

1. Record predecessor information for each reconstructed basic block.
2. Select an edge only when its successor has one predecessor and the edge is not a loop
   backedge.
3. Carry only selected block-cache locals that are dirty in the predecessor and live into the
   successor. `live_in` already means the successor reads the current value before redefining it.
4. Pass those values as Cranelift block arguments and seed the successor's cache from the block
   parameters.
5. Keep the canonical-memory store/reload path for joins, loop backedges, `br_table`, and any edge
   whose lowering cannot supply the exact arguments.
6. Preserve explicit materialization at interpreter reentry, tier-up, trap, and other runtime
   synchronization boundaries.

This intentionally begins with single-predecessor forward edges. It establishes the cache
coherence and block-argument machinery without introducing phi-like joins or extending values
around loops. Later phases can add loop backedges and joins where measured pressure permits, then
split lower-priority values at high-pressure edges.

Every comparison must report native compilation and execution separately. d3wasm instantiation
measures loading and compilation only; the Rust workloads, CoreMark, and interactive gameplay
measure generated-code execution.

### Phase 3: single-predecessor forward-edge cache values

The first explicit-edge phase records each reconstructed basic block's predecessors and identifies
loop headers and `br_table` terminators. For every selected block-cache local, it creates an
edge-specific Cranelift `Variable` only when:

- the successor has exactly one predecessor;
- the predecessor precedes the successor in reconstructed block order;
- the successor is not a loop header;
- the predecessor is not terminated by `br_table`;
- the predecessor defines the local; and
- the local is live into the successor.

Defining and using that edge-specific variable makes Cranelift pass the value as a block parameter
without promoting the local throughout the function. A dirty value is written to `locals_base`
only when at least one live successor edge cannot carry it. The successor seeds its block cache
from the edge value and records whether canonical memory was also updated. Joins, loops,
`br_table`, and uncertain paths retain the phase-2 store/reload behavior.

The comparison used separately built baseline and candidate compiler executables from the same
source state. The baseline disabled only edge selection. Each compiler swap was followed by a
discarded warm-up because the first process after replacing the compiler executable added roughly
35 ms of unrelated startup time to `native_compilation_time`.

The exact final implementation, including the `br_table` fallback, produced:

| Workload | Baseline compile | Edge compile | Change | Baseline execute | Edge execute | Change |
| -------- | ---------------: | -----------: | -----: | ---------------: | -----------: | -----: |
| SHA-512  |       0.024709 s |   0.024559 s |  -0.6% |       0.730719 s |   0.732943 s |  +0.3% |
| Base64   |       0.023170 s |   0.023125 s |  -0.2% |       0.561430 s |   0.562163 s |  +0.1% |
| JSON     |       0.042453 s |   0.045019 s |  +6.0% |       1.910143 s |   1.906699 s |  -0.2% |
| Regex    |       0.282863 s |   0.287251 s |  +1.6% |       0.247279 s |   0.236171 s |  -4.5% |

SHA-512, Base64, and JSON used two forward/reverse measurements per configuration. Regex used four
per configuration because its initial comparison showed a material improvement. JSON's 2.6 ms
compilation increase is visible in the small sample, but execution is neutral. Regex consistently
retains the improvement after excluding `br_table` edges.

Two forward/reverse d3wasm instantiation measurements gave:

| d3wasm metric                                    |   Baseline | Edge values | Change |
| ------------------------------------------------ | ---------: | ----------: | -----: |
| Native compilation                               | 3.029141 s |  3.046455 s |  +0.6% |
| Parse + validation + compilation + instantiation | 4.256449 s |  4.279615 s |  +0.5% |

d3wasm loading is therefore neutral, and this command does not execute generated application code.

Two forward/reverse CoreMark measurements were also neutral:

| CoreMark metric    |    Baseline | Edge values | Change |
| ------------------ | ----------: | ----------: | -----: |
| Native compilation |  0.007220 s |  0.007525 s |  +4.2% |
| Execution          | 21.495078 s | 21.567261 s |  +0.3% |
| Score              |  19,127.774 |  19,056.706 |  -0.4% |

The compilation percentage is 0.3 ms in absolute terms. An earlier candidate run took 23.36
seconds and reported a score of 17,592 while all surrounding runs were in the 21.5–21.9 second and
18,800–19,100 ranges; it was treated as a system outlier and replaced by the exact-source
forward/reverse comparison above.

This phase is execution-positive for Regex and otherwise neutral in the measured suite. It also
establishes the per-edge coherence machinery needed for the next experiments. Loop backedges and
joins should be added separately and measured for register-pressure regressions rather than
enabled as part of this conservative phase.

### Phase 4: fully-covered acyclic joins

The next phase allows an acyclic successor to have more than one predecessor when every
predecessor:

- precedes the successor in reconstructed block order;
- is not terminated by `br_table`; and
- defines the selected cached local.

The successor must still be outside a loop header and read the value before redefining it. Every
predecessor defines the same edge-specific Cranelift `Variable`, so Cranelift constructs a
phi-like block parameter at the join. A partially defined join remains on canonical memory.

Cache dirtiness at the successor is conservative across paths. If any predecessor reaches the
join without storing the value, the joined cache value is considered dirty. A predecessor that
also has a live fallback successor stores before its branch, so all paths can be considered clean
only when every predecessor performs such a store.

This phase was compared directly with the committed single-predecessor phase, not with the
phase-2 block-cache baseline:

| Workload | Forward-edge compile | Join compile | Change | Forward-edge execute | Join execute | Change |
| -------- | -------------------: | -----------: | -----: | -------------------: | -----------: | -----: |
| SHA-512  |           0.024315 s |   0.024001 s |  -1.3% |           0.731433 s |   0.730569 s |  -0.1% |
| Base64   |           0.023944 s |   0.023510 s |  -1.8% |           0.560248 s |   0.562235 s |  +0.4% |
| JSON     |           0.043031 s |   0.043745 s |  +1.7% |           1.905659 s |   1.906351 s |  +0.0% |
| Regex    |           0.282455 s |   0.290938 s |  +3.0% |           0.237202 s |   0.234586 s |  -1.1% |

SHA-512 used four forward/reverse measurements per configuration, Regex used six, and Base64 and
JSON used two. One Regex join-compilation sample was 0.320497 seconds; the other five averaged
0.285026 seconds, only 0.9% above the forward-edge mean. Execution is neutral except for a modest
additional Regex improvement.

Two d3wasm instantiation measurements moved native compilation from 3.231777 to 3.174570 seconds
and the summed parse, validation, compilation, and instantiation phases from 4.479655 to 4.408664
seconds. The first forward-edge sample was slower than its reverse-order sample, so the apparent
1.6% loading improvement is treated as noise rather than a win.

Two CoreMark measurements changed execution from 21.563255 to 21.651176 seconds (+0.4%) and score
from 19,062.156 to 18,969.467 (-0.5%). Native compilation remained approximately 8 ms. This is
also treated as neutral at the observed run-to-run variance.

Fully-covered acyclic joins are retained as a separate implementation phase. They add a small
Regex execution improvement without a demonstrated regression and establish the phi-like join
path needed before any pressure-aware loop experiment. Loop headers and backedges remain excluded.

### Loop-backedge experiment

A temporary loop extension selected the most frequently written block-cache locals and allowed
them to cross a loop header when the reconstructed CFG contained a backedge. Clean entry-edge
values could be loaded from canonical memory, dirty backedge values could be passed directly, and
tier-up reentry loaded each selected value from canonical memory before entering the dispatch
chain.

A first AArch64 sweep tested loop budgets of one, two, four, and six against the acyclic-join
baseline. Two forward/reverse SHA-512 and d3wasm measurements at each point showed no monotonic
execution, compilation, or loading change. SHA-512 remained in the same approximately
0.73–0.74-second execution range.

Native dumps of SHA-512 function 16 were identical at loop budgets zero and six after ignoring the
relocated helper address. A temporary selection diagnostic explained why:

```text
loop header 2: predecessors=[1, 2]
block-cache locals=[13, 17, 22, 25, 30, 32]
loop-edge locals=[]

entry predecessor definitions=[2]
backedge predecessor definitions=[1, 3, 4, ..., 37]
```

All six high-mutation block-cache locals are defined on the backedge, but none is live into the
outer loop header: each is overwritten before its first read in the next iteration. They benefit
from reuse inside the very large loop block, which phase 2 already provides, but carrying their
old values across the backedge cannot remove a needed load.

This is a useful distinction from “high mutation in a loop.” Mutation frequency identifies values
worth caching within the loop body, but a loop-carried SSA value also requires the previous
iteration's definition to reach a read in the next iteration. The existing liveness test correctly
rejects the SHA candidates.

The loop machinery and temporary diagnostics were removed rather than committed. A future loop
experiment would need to rank locals that are both live-in at a particular loop header and
profitable enough to displace one of the current six block-cache locals. That is a separate
per-loop cache-selection policy, not a simple extension of the current global mutation ranking.

#### One-way tier-up and canonical local state

`synthetic_tier_up` is a one-way interpreter-to-native OSR entry. The interpreter reaches an
eligible loop header with an empty operand stack and current locals in canonical `Value` storage;
the cold native resume path can load the selected live locals from that storage once. Native loop
backedges remain native and should carry those values in SSA or ordinary Cranelift spill slots
without writing the canonical `Value` representation on every iteration.

Calling an uncompiled function is not tier-down of the caller. The callee runs in the bytecode
interpreter and returns to the suspended native caller. Traps and exceptions leave or unwind the
current activation rather than resuming it at an arbitrary bytecode instruction. Ladybird has no
current synthetic native-to-interpreter tier-down path and Cranelift code does not depend on
speculative assumptions that require deoptimization.

An arbitrary future tier-down facility would need one of:

- explicit safepoints that materialize the required locals and operand-stack suffix;
- metadata that reconstructs interpreter-visible state from native registers and spill slots; or
- continuously synchronized canonical state.

The last option would penalize every native path for a feature that does not currently exist. Loop
cache policy should therefore preserve the existing cold tier-up entry but must not keep canonical
locals synchronized merely to support hypothetical tier-down. Debugging, suspension, code
invalidation, or a speculative higher JIT tier can add explicit reconstruction points if they
later require them.

This behavior predates the current native-call work. Commit `08221aebab5` (`LibWasm: Start
execution immediately after validation`) introduced synthetic checkpoints, the interpreter-handler
entry ABI, and the instruction-index dispatch to eligible loop headers. Despite its fragment-like
shape, compilation and publication have always been function-wide: Cranelift emits one complete
function body, and the complete mapping is linked, finalized, and published before any checkpoint
can enter it. The checkpoint index selects an OSR entry within that body; it does not identify an
independently compiled block.

The committed compiler already has one cold resume block per function. `init_locals_resume!` loads
all function-wide promoted locals once, then the shared checkpoint dispatch selects the target
header. The interpreter-facing adapter always returns the constant `Outcome::Return`, and bridge
callers reject any other outcome. Native execution therefore already runs to the function
epilogue or a trap in practice, but the handler-compatible return type does not encode that
one-way invariant.

The canonical local area is separate from the vstack. It is the function's authoritative array of
16-byte `Value` objects understood by the bytecode interpreter and runtime helpers. Numeric local
writes currently store an eight-byte payload and a zero high half. Vstack materialization instead
uses `Configuration::value_stack_top`, writes only the required operand suffix, and advances that
top pointer. An ordinary eight-byte access to a native `sp` offset is a Cranelift spill, not a
materialized Wasm `Value`.

A renewed prototype carried mutated, live-in block-cache locals through leaf loops. Because these
additional locals were not part of the existing function-wide promoted set, it added a separate
resume block at every selected checkpoint. In d3wasm function 2445,
`idInteraction::AddActiveInteraction()`, the complete native body grew from 245,196 to 247,564
bytes (+1.0%), while its Cranelift frame fell from 7,648 to 7,504 bytes. The identified
turbo-shadow silhouette loop shrank from 440 to 368 bytes, and its approximate canonical-local
traffic fell from 43 to 25 operations per iteration.

The hot-loop result did not justify the permanent specialization cost. Five alternating d3wasm
instantiation pairs measured native compilation at 5.301 seconds for the prototype and 4.962
seconds for the committed baseline (+6.8%; medians +7.1%). Four-pair execution measurements were
neutral for the local-edge pressure fixture and SHA-512, noisy for Base64, and about 1.5% faster
for Regex. The prototype's per-loop resume and natural-loop machinery is therefore being removed
rather than refined.

The revised sequence is:

1. Make the one-way handoff explicit by giving the interpreter-facing native adapter a `void`
   return ABI. `run_native_entry()` should likewise return `void`, and a successful synthetic
   tier-up should unconditionally terminate interpreter dispatch for that activation.
2. Add focused coverage proving that an activation does not resume bytecode dispatch after native
   execution returns.
3. First test the bounded high-mutation local set through the existing single function-wide resume
   path. Loading extra state once is cold; duplicating materialization at every loop is permanent.
4. Consider per-loop subsets only if the function-wide policy causes a measured SSA-pressure or
   transition-cost problem that outweighs the additional native code and compilation work.

The first checkpoint implements steps 1 and 2. The interpreter-facing adapter now returns `void`;
the out-of-process compiler protocol no longer carries the numeric value of `Outcome::Return`;
`run_native_entry()` also returns `void`; and `synthetic_tier_up` unconditionally returns
`Outcome::Return` to terminate interpreter dispatch after native completion. The JIT cache format
version advanced to 24 because cached adapters use the changed ABI.

The focused regression begins the function in the interpreter, invokes native compilation through
an imported callback, and reaches a synthetic checkpoint in the same activation. The native loop
increments a global three times. Resuming interpreter dispatch after native completion would run
the loop body once more and return four; the test returns three. All ten `TestWasmExecution` cases
and all nine Rust unit tests pass.

Five alternating uncached d3wasm compilation pairs were neutral within normal variance: the
one-way adapter averaged 4.656 seconds and an OOP-protocol-compatible compiler built from the
committed source averaged 4.588 seconds (+1.5%). Individual pairs ranged from -4.5% to +11%, and
the native body is otherwise unchanged.

The first function-wide local experiment then replaced the production `10 function-wide + 6
block-local` split with up to `16 function-wide + 0 block-local`. It selected the same bounded six
high-mutation locals, but promoted them for the complete function so the existing shared resume
block initialized them once and no loop-specific resume or edge variables were needed.

The topology is simpler but the live ranges are too broad:

| Workload | Baseline native compile | Function-wide native compile | Baseline execute | Function-wide execute |
| -------- | ----------------------: | ---------------------------: | ---------------: | --------------------: |
| d3wasm   |                4.8564 s |                     5.0559 s |                — |                     — |
| SHA-512  |                0.0311 s |                     0.0277 s |         0.7198 s |              0.7383 s |
| Pressure |                0.0035 s |                     0.0037 s |         0.7728 s |              0.7725 s |
| Base64   |                0.0316 s |                     0.0339 s |         0.3991 s |              0.3918 s |
| Regex    |                0.3766 s |                     0.4070 s |         0.1744 s |              0.1751 s |

d3wasm compilation regressed by 4.1% by mean and 7.0% by median. SHA-512 execution regressed by
2.6%; the pressure fixture and Regex execution were neutral; and Base64's noisy 1.8% execution
improvement came with 7.3% slower compilation. Regex compilation regressed by 8.1%.

For `idInteraction::AddActiveInteraction()`, function-wide promotion increased native code from
245,196 to 246,972 bytes (+0.7%) and the Cranelift frame from 7,648 to 8,224 bytes (+7.5%). This is
less code growth than the per-loop resume prototype but worse whole-function pressure. The
function-wide experiment was removed rather than committed. A single shared resume remains the
right entry topology; the remaining problem is limiting hot SSA live ranges without duplicating
cold resume code at each loop.

A narrower follow-up promoted a block-cache candidate function-wide only when the existing local
liveness analysis found it live into a loop header with a backedge. This still used the single
shared function resume block and left all other candidates in the existing block-local cache. The
rule did not narrow `idInteraction::AddActiveInteraction()`: all six candidates were live into at
least one such header, so its 246,972-byte body and 8,224-byte frame were identical to the rejected
16-local function-wide experiment.

Five alternating uncached d3wasm samples showed the same whole-function pressure cost. Native
compilation increased from 4.4232 to 5.0282 seconds by mean (+13.7%) and from 4.4393 to 4.9701
seconds by median (+12.0%). Four alternating samples of the focused workloads measured:

| Workload | Baseline native compile | Loop-live native compile | Baseline execute | Loop-live execute |
| -------- | ----------------------: | -----------------------: | ---------------: | ----------------: |
| SHA-512  |                0.0303 s |                 0.0278 s |         0.7212 s |          0.7217 s |
| Pressure |                0.0034 s |                 0.0034 s |         0.7568 s |          0.7618 s |
| Base64   |                0.0320 s |                 0.0299 s |         0.3861 s |          0.3994 s |
| Regex    |                0.3490 s |                 0.3813 s |         0.1735 s |          0.1753 s |

SHA-512 execution remained neutral, while the pressure fixture, Base64, and Regex moved in the
wrong direction. Loop-header `live_in` is therefore too broad to decide which locals deserve
function-wide SSA live ranges. The follow-up was removed rather than committed. A future policy
needs to shorten those live ranges at selected pressure points; merely selecting a subset of loop
headers does not solve the whole-function lifetime imposed by function-wide Cranelift variables.

#### Separate cold OSR state from native loop state

The tier-up dispatcher already resumes at the native loop header. Bytecode construction inserts a
`synthetic_tier_up` immediately after each eligible `loop`. When the interpreter reaches that
checkpoint, its `short_ip` identifies the synthetic instruction. The native adapter encodes that
index as `short_ip + 1`, reserving entry token zero for compiled-to-compiled calls. The native body
subtracts one, compares the resulting checkpoint index in the shared dispatch chain, and branches
to the enclosing loop's native header. The structural `loop` and synthetic checkpoint have no
native runtime work, so the first real loop-body instruction runs next and no completed work is
repeated.

The remaining problem is the representation of local state on the incoming edges. The cold OSR
edge starts with canonical `Value` locals, while a native backedge already has current typed SSA
values. Loop headers are currently excluded from the block-local edge cache, so the native edge is
forced through the cold edge's representation: dirty values are stored as payload/tag pairs and
loaded again on the next iteration.

The intended invariant is:

- canonical local storage is authoritative when entering native code from the interpreter;
- typed SSA values or ordinary Cranelift spill slots are authoritative while the activation runs
  natively;
- a native loop backedge must not materialize canonical `Value` locals solely because the loop is
  also an OSR target;
- an exit stores a dirty local only when a successor cannot consume its typed value directly; and
- traps or any future native-to-interpreter continuation need an explicit reconstruction point
  rather than continuously synchronized canonical state.

The implementation should give each selected loop header typed block parameters for the locals
live there. Normal entry edges and native backedges pass their current values directly. The one
shared resume block loads the bounded union of selected locals from canonical storage once, and
each checkpoint arm passes only the subset required by its target header. In schematic form:

```text
shared cold resume:
    local1 = load canonical local 1 payload
    local4 = load canonical local 4 payload
    checkpoint A -> loop_A(local1, local4)

normal native entry -> loop_A(current_local1, current_local4)

loop_A(local1, local4):
    ...
    continue -> loop_A(updated_local1, updated_local4)
    exit     -> successor(updated_local1, updated_local4)
```

The header parameters are phi-like values. Cranelift remains free to keep them in registers or
spill their native-width representation. The design does not add a resume block per loop and does
not promote the selected locals across the complete function. It extends the existing edge-cache
coherence model to loop entry and backedges while retaining the existing single resume dispatcher.

The first implementation applied this mechanism to every globally selected block-cache local that
was live into any loop header with a real backedge. A strengthened tier-up regression keeps its
loop-carried local outside the ten function-wide promotions. Its first invocation compiles during
interpreter execution and enters the native loop through OSR; its second invocation begins in
native code and reaches the same header through the ordinary preheader. Both paths produce the
expected result.

The regression's AArch64 body confirms the representation change. Baseline code loads the local
payload from its canonical slot, stores the updated payload, and stores a zero tag on every
backedge. The candidate keeps the value in a native register around the backedge and performs the
payload/tag store once on loop exit. A 200-million-iteration purpose benchmark nevertheless had
neutral throughput: 0.095430 seconds at baseline and 0.095809 seconds with the loop cache (+0.4%).
Its increment/compare/branch dependency, rather than the eliminated stack-local traffic, limits
this deliberately small loop.

Applying the mechanism broadly costs too much CFG and SSA work:

| Workload | Baseline native compile | Broad loop-cache compile | Baseline execute | Broad loop-cache execute |
| -------- | ----------------------: | -----------------------: | ---------------: | -----------------------: |
| d3wasm   |                4.5050 s |                 4.8086 s |                — |                        — |
| SHA-512  |                0.0305 s |                 0.0306 s |         0.7210 s |                 0.7209 s |
| Pressure |                0.0034 s |                 0.0035 s |         0.7590 s |                 0.7607 s |
| Base64   |                0.0306 s |                 0.0290 s |         0.3834 s |                 0.3913 s |
| Regex    |                0.3588 s |                 0.3601 s |         0.1752 s |                 0.1821 s |

Five alternating d3wasm pairs regressed native compilation by 6.7% by mean and 7.3% by median.
Four alternating workload pairs left SHA-512 neutral, moved the pressure fixture by only 0.2%,
regressed Base64 execution by 2.1% in a noisy sample, and consistently regressed Regex execution by
3.9%. `idInteraction::AddActiveInteraction()` grew from 245,196 to 248,812 bytes (+1.5%), although
its native frame fell from 7,648 to 7,568 bytes (-1.0%).

The edge mechanism is therefore retained only as an experiment while selection is narrowed. The
next policy requires a local to be live into and mutated within a leaf natural loop. Read-only
values do not need a backedge definition, and carrying values through outer loops as well as nested
inner loops duplicates live ranges without removing additional canonical mutation traffic.

That narrower policy preserves the focused transformation while substantially reducing its scope.
Unit coverage rejects loop-shaped blocks without backedges, read-only loop locals, and outer loops
containing a nested loop; it selects a mutated, live-in local for the inner leaf loop. The native
tier-up regression covers both the shared OSR edge and a subsequent fresh native invocation.

For `idInteraction::AddActiveInteraction()`, the narrowed body is 244,876 bytes, 320 bytes smaller
than the 245,196-byte baseline (-0.1%). Its native frame falls from 7,648 to 7,616 bytes. Five
alternating d3wasm compilation pairs and four alternating workload pairs measured:

| Workload | Baseline native compile | Leaf-loop cache compile | Baseline execute | Leaf-loop cache execute |
| -------- | ----------------------: | ----------------------: | ---------------: | ----------------------: |
| d3wasm   |                4.6110 s |                4.7944 s |                — |                       — |
| SHA-512  |                0.0288 s |                0.0297 s |         0.7330 s |                0.7291 s |
| Pressure |                0.0038 s |                0.0036 s |         0.7700 s |                0.7677 s |
| Base64   |                0.0315 s |                0.0351 s |         0.3931 s |                0.3928 s |
| Regex    |                0.3776 s |                0.3994 s |         0.1768 s |                0.1739 s |

d3wasm native compilation regressed by 4.0% by mean and 2.0% by median. Regex execution improved
consistently by 1.6% while its compilation regressed by 5.8%. SHA-512, the pressure fixture, and
Base64 execution were neutral. The purpose loop was also neutral after discarding a first-sample
compiler outlier. This is a generated-code-versus-compilation trade-off rather than a universal
benchmark win. The installed candidate remains suitable for an application-level d3wasm trace,
where interpreter-first tiering, cached subsequent use, and frame-time behavior can be evaluated
separately from uncached native compilation. Cache blob format version 25 prevents older generated
code from masking that comparison.

#### High-frequency d3wasm profile

Runs 1 through 3 of `/Users/lukewilde/Documents/d3wasm_9.trace` used the CPU Profiler
instrument's High Frequency setting in deferred recording mode. Run 1 captured loading. Runs 2
and 3 captured the same heavy gameplay viewpoint in two separate WebContent processes. The cache
blob installed identical code at different randomized addresses, so matching live code bytes
against cache records, while ignoring only relocation slots, gave unambiguous function and
offset attribution.

The two heavy-scene captures are closely reproducible:

| Main-thread cycle share                 |  Run 2 |  Run 3 |
| --------------------------------------- | -----: | -----: |
| Wasm JIT                                | 57.67% | 57.77% |
| JavaScript                              | 16.81% | 16.80% |
| LibWeb                                  | 11.16% | 10.90% |
| Other/system                            |  8.61% |  8.76% |
| Wasm runtime                            |  5.63% |  5.65% |
| Wasm interpreter                        |  0.12% |  0.12% |
| `idInteraction::AddActiveInteraction()` |  8.43% |  8.54% |
| `R_CreateLightTris()`                   |  2.97% |  2.98% |
| `idSIMD_Generic::Dot(plane, drawvert)`  |  2.28% |  2.22% |
| `idSIMD_Generic::CmpLT()`               |  1.23% |  1.19% |
| `idVertexCache::Position()`             |  1.15% |  1.20% |
| `idSIMD_Generic::Dot(vec3, plane)`      |  0.92% |  0.88% |
| `idInteraction::UnlinkAndFree()`        |  0.82% |  0.81% |
| `idSIMD_Generic::CmpGE()`               |  0.33% |  0.34% |

The narrowed loop-cache policy does not reach the important `AddActiveInteraction()` loops in
this scene. The unchanged native loop at offsets `0x21a6c` through `0x21c20` accounts for 1.81%
and 1.87% of all main-thread cycles, or 21.5% and 21.9% of the function. The unchanged loop at
`0x21680` through `0x217b4` accounts for another 1.04% and 1.03%, or approximately 12% of the
function. The four leaf loops changed by the candidate, at `0xd7f4`, `0xed20`, `0xf0fc`, and
`0xf48c`, received no samples in either heavy-scene capture. Loading run 1 spent only 0.012% of
main-thread cycles in the complete function and did not sample those loops either.

The dominant loop retains canonical local traffic. Its most frequently sampled offsets include
the payload loads at `0x21af0` and `0x21c08` and the tag store at `0x21adc`. The second loop's
frequently sampled offsets include the payload load at `0x21778` and tag stores at `0x216a8` and
`0x217a8`. Instruction-level cycle samples can skid or phase-lock within a tight loop, so these
individual percentages are evidence for where execution remains concentrated, not isolated
latency measurements for one load or store. The function and loop-range totals are the more
reliable result.

The non-JIT samples also separate costs that an ordinary 1 ms profile grouped together:

- `_platform_memmove` has 1.98% direct self share. Its largest resolved callers are WebGL command
  serialization in `write_payload` at 0.77% and LibWeb buffer `move_from` work at 0.30%; this is
  not primarily Wasm local or vstack materialization.
- `wasm_cl_call_indirect_with_record` has 0.53% direct self share. `size()` beneath that helper
  adds 0.26%, and argument-allocation work beneath `wasm_cl_finish_call` adds another 0.26%.
- `Web::WebAssembly::Detail::to_js_value()` has 0.70% direct self share. The disabled
  `WebIDL::log_trace()` check still has 0.46%, alongside separately visible JS call and execution
  context setup.
- `wasm_cl_finish_call` appears on 36.6% of main-thread stacks as an inclusive ancestor. That is
  not 36.6% direct overhead: execution of native callees and JS imports remains nested under the
  helper-mediated call until it returns.

Runs 1 through 3 do not contain visual frame markers. They establish repeatable aggregate cycle
shares but cannot associate a CPU burst with one rendered frame, distinguish frames that exceed
the 8.33 ms budget from frames that do not, or directly correlate a sampled interval with a
dropped presentation.

##### Visual-frame correlation

Run 4 added a macOS Points of Interest animation interval named `WebContent Visual Frame` around
`EventLoop::update_the_rendering()`. The interval covers input processing, animation-frame
callbacks, layout, canvas and WebGL flushing, and painting. It therefore measures the WebContent
work for one rendering update, not the separate compositor presentation. It can identify an
over-budget application frame but cannot prove that the compositor presented that update.

The run captured the heavy opening-area view while repeatedly firing a weapon. This is an
intentionally demanding but relevant case: looking at an uncomplicated wall reaches the 120 Hz
display target, while Chrome also reaches 120 FPS in the same heavy scene. The latter is an
observational comparison rather than a controlled cross-engine benchmark, but it establishes that
the content is capable of meeting an 8.33 ms frame budget on the same machine.

The leaf-loop candidate had been stashed before run 4. Rebuilding installed the baseline compiler,
and matching the live JIT mappings against the current cache confirmed cache format version 24,
6,482 compiled functions, and unambiguous matches for the hot functions. Run 4 is therefore a
baseline generated-code capture rather than another leaf-loop-candidate measurement.

The signposts contain 1,319 complete rendering updates:

| Rendering-update metric           |            Run 4 |
| --------------------------------- | ---------------: |
| Mean                              |        15.802 ms |
| Median                            |        15.790 ms |
| 90th percentile                   |        17.965 ms |
| 95th percentile                   |        18.646 ms |
| 99th percentile                   |        20.665 ms |
| Maximum                           |        36.729 ms |
| Updates over 8.333 ms             |    1,316 (99.8%) |
| Updates over 16.667 ms            |      365 (27.7%) |
| Updates over 20 ms                |        20 (1.5%) |
| Mean update-start rate            |          62.73/s |
| Mean / median gap between updates | 0.138 / 0.123 ms |

The main thread is consequently running rendering updates almost continuously in this workload;
the low update rate is not caused by waiting for the next rendering opportunity. These absolute
numbers include High Frequency profiler overhead and must not be treated as unprofiled game-wide
FPS. A signpost-only recording would be the appropriate lower-overhead measurement of absolute
frame time.

Within the overlap between the signpost and CPU-sample capture windows, grouping frames by their
measured duration gives the following mean main-thread cycle counts:

| Mean cycles per frame   | <= 16.667 ms | 16.667-20 ms |   > 20 ms |
| ----------------------- | -----------: | -----------: | --------: |
| Frames with CPU samples |          888 |          333 |        19 |
| Mean duration           |    15.009 ms |    17.695 ms | 22.048 ms |
| All main-thread work    |      42.746M |      49.960M |   57.721M |
| Wasm JIT                |      24.561M |      29.726M |   34.939M |
| JavaScript              |       6.912M |       8.072M |    9.625M |
| LibWeb                  |       4.973M |       5.216M |    5.601M |
| Other/system            |       3.802M |       4.181M |    4.609M |
| Wasm runtime            |       2.441M |       2.704M |    2.847M |
| Wasm interpreter        |       0.056M |       0.061M |    0.100M |

Frame duration and sampled main-thread cycles have a Pearson correlation of 0.854. Compared with
the frames at or below 16.667 ms, the 16.667-20 ms group uses another 7.214 million cycles per
frame. Wasm JIT code accounts for 5.165 million, or 71.6%, of that increase. The greater-than-20 ms
group uses another 14.975 million cycles, of which Wasm JIT code accounts for 10.378 million, or
69.3%. The slow tail is primarily more native Wasm game work rather than a distinct LibWeb,
interpreter, or helper cliff.

The largest resolved self-cycle increases per 16.667-20 ms frame, relative to the faster group,
are consistent with the weapon workload:

| Function                                      | Extra cycles/frame |
| --------------------------------------------- | -----------------: |
| `idInteraction::AddActiveInteraction()`       |             0.556M |
| `decode_packed_entry_number()`                |             0.243M |
| `R_CullLocalBox()`                            |             0.156M |
| `idSoundWorldLocal::AddChannelContribution()` |             0.152M |
| `R_CreateLightTris()`                         |             0.120M |
| `idSIMD_Generic::Dot(plane, drawvert)`        |             0.103M |
| `idInteraction::UnlinkAndFree()`              |             0.084M |
| `floor1_inverse2()`                           |             0.060M |

The greater-than-20 ms tail increases the lighting functions further, while
`decode_packed_entry_number()`, `idSoundWorldLocal::AddChannelContribution()`, and
`floor1_inverse2()` grow especially strongly. Firing therefore adds both dynamic-light geometry
and sound decoding or mixing work; it does not expose one new Wasm-to-JavaScript boundary stall.

#### Run 4 hot-source mapping

The Wasm name attached to a native body does not necessarily identify the source routine that
contains its hottest instructions. LLVM inlined a substantial amount of renderer code into
`idInteraction::AddActiveInteraction()`: the native body is 245,196 bytes, and its dominant range
at offsets `0x21b68` through `0x21d20` maps to the silhouette-edge loop in
`neo/renderer/tr_turboshadow.cpp`, not to a simple loop written directly in
`AddActiveInteraction()`. For each silhouette edge, the loop loads the facing state of the two
adjacent faces. If the states differ, it constructs two triangles by writing six shadow indices.
The loop is intrinsically high-volume, and the native instructions also repeatedly move loop
values through the canonical Wasm-local area instead of retaining all of them in registers.

The other resolved functions contain the following concentrated work:

| Function                                             | Dominant work and native-code observation                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `R_CreateLightTris()`                                | The loop at `neo/renderer/Interaction.cpp:376` visits every triangle, loads its three indices and cull bytes, rejects triangles outside the light frustum, and writes three indices for each accepted triangle. Approximately 74% of this function's run-wide samples fall in that loop. Its native body also spills and reloads substantial loop-carried state through Cranelift's stack frame.                                                                                           |
| `R_CullLocalBox()`                                   | The fast radius test can fall through to the loop at `neo/renderer/tr_main.cpp:640`, which transforms eight box corners and tests up to eight corner-to-plane distances for each of five or six frustum planes. The hottest native range is the inner corner/plane dot-product loop. Repeated calls for interaction surfaces magnify this otherwise small routine.                                                                                                                         |
| `idSIMD_Generic::Dot(plane, drawvert)` and `CmpLT()` | `R_CalcInteractionCullBits()` runs these as two passes over all vertices for each relevant one of six light planes. `Dot()` performs three scalar multiplies and a dependent scalar add chain, then stores one float; approximately 88% of its samples are in that loop. `CmpLT()` makes a second scalar pass that compares four values at a time in source-level unrolling and read-modify-writes the cull bytes. This non-SIMD Wasm build contains no vector operations for either pass. |
| `idSoundWorldLocal::AddChannelContribution()`        | The concentrated native range at `0x2980` through `0x2af8` maps to the streaming loop at `neo/sound/snd_world.cpp:1643`. It clamps each decoded float sample to the signed 16-bit range, converts it, and stores it into the streaming buffer. The loop is scalar and branchy, and its generated body performs canonical Wasm-local payload and tag stores within the iteration.                                                                                                           |
| `decode_packed_entry_number()`                       | The Vorbis decoder first attempts a short codebook-table lookup, reverses the available bits on the fallback path, and then bisects an ordered codeword list. Native offsets around `0x518` through `0x570` are the bisection loop. Another sampled range around `0x680` through `0x728` contains a large canonical-state materialization sequence around an exit path, so its cost is not exclusively intrinsic codebook search work.                                                     |
| `floor1_inverse2()`                                  | The hot native loop at offsets `0x774` through `0x834` maps to `render_line()` in Vorbis `floor1.c`. It advances an integer line interpolator and multiplies each spectral bin by a decibel lookup-table value. It is another scalar, per-element loop rather than a call-boundary cost.                                                                                                                                                                                                   |
| `idInteraction::UnlinkAndFree()`                     | There is no comparable arithmetic kernel. It removes an interaction from the entity and light lists, walks its surfaces to release light and shadow triangles and cull data, frees its area references, and returns the interaction to an allocator. Its samples are distributed through that cleanup and its entry/exit machinery, which indicates increased transient-light interaction churn rather than one expensive operation.                                                       |

The slow-frame growth therefore has two layers. Firing legitimately increases the trip counts of
dynamic-light, shadow-volume, culling, Vorbis decode, and audio conversion loops. Generated-code
quality amplifies that work through canonical local payload/tag traffic, Cranelift stack spills,
and scalar execution in the non-SIMD build. This is broader than the Wasm-to-JavaScript boundary:
removing boundary overhead remains useful, but it would not remove the dominant work inside these
native Wasm loops.

#### Cranelift 0.122 and Wasmtime 35 comparison

Ladybird was temporarily updated from Cranelift 0.116.1 to exactly 0.122.0, the Cranelift version
used by Wasmtime 35.0.0. The comparison compiled the same named d3wasm module from
`build-wasm-full/d3wasm.wasm` on the same AArch64 machine. Ladybird retained its existing lowering,
settings, runtime ABI, trap implementation, and local-promotion policy; only the Cranelift crates
and API adaptations changed. Wasmtime necessarily used its own lowering and runtime ABI, so the
whole-function comparison is directional. The matching silhouette-edge loop is the stronger
controlled comparison because its six emitted indices make its boundaries unambiguous.

The Ladybird dumps were produced with:

```sh
Build/release/bin/wasm \
    --dump-native \
    --print-function 2445 \
    --export-noop \
    /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm
```

The Wasmtime 35 dump was produced with:

```sh
/Users/lukewilde/.wasmtime/bin/wasmtime compile \
    -C compiler=cranelift \
    -C cache=n \
    -C parallel-compilation=n \
    -O opt-level=2 \
    -o /tmp/d3wasm-wasmtime35.cwasm \
    /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm

/Users/lukewilde/.wasmtime/bin/wasmtime objdump \
    --addresses \
    --funcs wasm \
    --filter AddActiveInteraction \
    /tmp/d3wasm-wasmtime35.cwasm
```

The complete native bodies and frames changed as follows:

| Compiler and lowering          |                Native body | Stack frame |
| ------------------------------ | -------------------------: | ----------: |
| Ladybird, Cranelift 0.116.1    |              245,196 bytes | 7,648 bytes |
| Ladybird, Cranelift 0.122.0    |              237,980 bytes | 6,960 bytes |
| Wasmtime 35, Cranelift 0.122.0 | approximately 95,856 bytes | 2,992 bytes |

Updating Ladybird's backend alone therefore reduced the complete body by 2.9% and its stack frame
by 9.0%. The remaining Ladybird body is 2.48 times the size of Wasmtime's and its frame is 2.33
times as large. Some of that whole-function gap can come from different stack-limit, bounds-check,
trap, and ABI lowering and must not be attributed exclusively to local handling.

The exact source loop at `neo/renderer/tr_turboshadow.cpp:116` gives a more specific result:

| Compiler and lowering          | Bytes | Instructions | Memory operations | Canonical-local operations | Ordinary stack operations |
| ------------------------------ | ----: | -----------: | ----------------: | -------------------------: | ------------------------: |
| Ladybird, Cranelift 0.116.1    |   440 |          110 |                62 |                         43 |                         7 |
| Ladybird, Cranelift 0.122.0    |   412 |          103 |                56 |                         43 |                         1 |
| Wasmtime 35, Cranelift 0.122.0 |   212 |           53 |                20 |                          0 |                         8 |

Both 0.122 loops contain the same twelve linear-memory operations required to read the facing
states and emit six indices. Wasmtime adds eight ordinary frame accesses. Ladybird adds one
ordinary frame access and 43 accesses to its canonical 16-byte Wasm-local payload and tag slots.
The newer backend removes six Ladybird spills and makes the loop 6.4% smaller, but it cannot remove
canonical traffic that Ladybird's frontend explicitly emits. With the backend version held equal,
Ladybird's loop remains 1.94 times the instruction count and 2.8 times the memory-operation count
of Wasmtime's loop. This isolates canonical-local lowering, rather than the old Cranelift version,
as the dominant remaining code-quality difference in this measured hotspot.

#### Canonical-free native activations

Canonical locals should remain an interpreter and OSR representation, but they do not need to be
the backing store of a running native activation. The bytecode interpreter requires its array of
16-byte `Value` locals, and `synthetic_tier_up` requires the current interpreter values as cold
incoming state. The interpreter-facing adapter likewise needs to load parameters from the current
frame. These requirements justify a one-time conversion at native entry; they do not justify
writing canonical payload and tag pairs throughout native execution.

The current compiler gives canonical locals several additional roles:

- locals outside the function-wide promotion set use canonical storage as their permanent home;
- the block-local cache stores dirty values there when a control-flow edge cannot carry a typed
  value directly;
- a direct native callee allocates a 16-byte slot for every local and installs that array as
  `Configuration::locals_base`;
- helper calls reload `locals_base` because an interpreted callee may have moved the interpreter's
  frame storage; and
- `flush_locals!` writes every dirty promoted local back at successful return and at the explicit
  `unreachable` trap path.

Those are consequences of the current lowering rather than requirements of the execution model.
Tier-up is one-way: after taking a synthetic checkpoint, the activation runs natively until it
returns or traps. Calling an uncompiled function is not tier-down of the caller; the callee gets a
separate interpreter frame, returns, and the suspended native caller continues. Host and imported
calls similarly need argument and result marshalling at their boundaries, not a continuously
synchronized copy of every caller local. A trap unwinds the native activation rather than resuming
it at a bytecode instruction.

The intended representation boundary is therefore:

```text
interpreter Value locals
        |
        | one cold conversion at function entry or synthetic tier-up
        v
native typed locals: SSA values, registers, or native stack slots
        |
        | no canonical-local synchronization
        v
return or trap
```

A bounded first implementation should proceed as follows:

1. Retain canonical `Value` locals in interpreter frames and retain the interpreter-facing adapter.
2. Give every local in a compiled activation a native representation. Keep the selected hot locals
   as typed Cranelift variables and put the remaining locals in compact typed Cranelift stack slots
   rather than canonical `Value` slots.
3. On fresh native entry, initialize the native representation from typed ABI parameters and Wasm
   zero values. On synthetic tier-up, use the existing single per-function resume block to copy the
   interpreter state once before dispatching to the requested loop header. This must not create a
   resume block per loop.
4. Make native local reads, writes, block-cache exits, and loop backedges operate only on the native
   representation. Remove `flush_locals!` after focused coverage confirms that successful return
   and every trap path terminate the native activation.
5. Remove the direct native callee's canonical `direct_locals` array and make helper context
   restoration independent of a valid native `locals_base`. Canonical call records may remain at
   high-arity or interpreter/host boundaries; they are argument transport rather than persistent
   native local storage.
6. Measure the result before extending SSA promotion. Later liveness-guided block parameters can
   move profitable native-slot locals into registers without recreating the earlier function-wide
   SSA pressure cliff.

This first phase removes `Value` tag stores, halves the nominal memory allocated for an i64 local,
and gives Cranelift backend-visible native stack slots. It does not by itself guarantee
Wasmtime-shaped code: a local assigned a native stack slot can still generate loads and stores.
Matching Wasmtime's hot loop also requires keeping its profitable loop-carried values in SSA or
registers, as shown by Wasmtime folding `v1 << 1` directly into the `eor` that emits a shadow
index. The separation is still valuable because it makes register promotion a native code-quality
decision instead of a correctness requirement for avoiding canonical `Value` traffic.

The implementation must cover every local type accepted by native compilation. The present typed
native ABI supports numeric types; if native reference locals are added later, their native
representation must preserve reference lifetime and rooting rather than treating the payload as
an untracked integer. Functions outside the supported native type set should continue to use the
interpreter rather than receive a reproduction-specific partial conversion.

The first implementation allocates one dense eight-byte payload slot for each accessed,
unpromoted local. Unused declared locals allocate no native storage and generate no initialization
or tier-up copying. Fresh entry initializes the slots from the typed native ABI or the high-arity
incoming `Value` array; the shared cold resume block copies the accessed interpreter locals once.
Native reads, writes, block-cache exits, and loop backedges never update canonical locals, and a
direct native callee no longer installs a canonical locals array in `Configuration`.

Using Cranelift `stack_load` and `stack_store` directly on the large explicit slot produced poor
AArch64 address lowering: the backend materialized many constant-offset slot addresses and then
spilled those addresses. On the Cranelift 0.122 `AddActiveInteraction()` comparison, that first
form generated 240,476 bytes of code with a 6,320-byte frame, versus the canonical baseline's
237,980 bytes and 6,960-byte frame. Materializing one native-locals base address and using ordinary
constant-offset loads and stores instead generated 210,620 bytes with a 5,552-byte frame. This is
why the implementation uses a single base even though the source-level representations are
otherwise equivalent.

In the matched silhouette-edge loop, the canonical baseline has 103 instructions and 56 memory
operations: 43 canonical-local payload or tag accesses, twelve linear-memory operations, and one
ordinary Cranelift stack access. The native-locals form has 90 instructions and 43 memory
operations: thirty native payload accesses, the same twelve linear-memory operations, and one
ordinary stack access. The 13 removed operations are canonical representation overhead; the
remaining gap from Wasmtime's 53 instructions and 20 memory operations is principally local
values that still live in native memory rather than SSA. Eliminating canonical locals therefore
completes the representation fix, but does not replace the later liveness-guided promotion work.

##### Remaining Wasmtime gap

The matched silhouette-edge loop isolates the remaining difference more precisely. Both engines
perform the same twelve required linear-memory operations. Ladybird performs thirty accesses
through its native-locals base and one ordinary Cranelift stack access; Wasmtime performs eight
ordinary stack accesses and no separate local-home accesses. The difference between 31 and eight
is exactly the remaining 23-memory-operation gap:

| Additional state traffic           | Ladybird | Wasmtime |
| ---------------------------------- | -------: | -------: |
| Native-local accesses              |       30 |        0 |
| Backend stack accesses             |        1 |        8 |
| Total additional memory operations |       31 |        8 |

Ladybird still emits explicit local-home operations around arithmetic:

```asm
ldr x14, [x27, #0x40]
ldr x15, [x27, #0x58]
eor w14, w14, w15
```

Wasmtime keeps the corresponding values in SSA and lets instruction selection fold the shift into
the consuming operation:

```asm
eor w4, w13, w3, lsl #1
```

This is a frontend state-representation difference rather than a Cranelift-version difference.
For a large function, Ladybird selects ten locals for function-wide promotion. Its six-local block
cache is also selected across the complete function rather than separately for each loop. Loop
headers are excluded from edge caching because a synthetic tier-up checkpoint adds a cold
interpreter predecessor whose local values are not currently supplied as block arguments. The
remaining locals therefore become explicit loads and stores through the native-locals base.
Cranelift cannot reliably reconstruct local SSA after the frontend has represented those values as
ordinary memory operations.

##### Local homes and the three execution stacks

The explicit native-local slot is not required by WebAssembly, and it is not an independent Wasm
stack-exhaustion mechanism. It is the current lowering's authoritative home for every accessed
local that was not selected for function-wide promotion. Without complete local SSA across the
control-flow graph, a later `local.get` still needs somewhere from which to recover the latest value
after a `local.set` or control-flow merge. Replacing canonical `Value` locals with dense native
slots removed representation and tag traffic, but retained that fallback memory model as an
incremental correctness step.

Three distinct kinds of stack state must not be conflated:

1. The Cranelift vstack and SSA state are compiler abstractions. A value may remain in an SSA value
   or machine register, or Cranelift's register allocator may spill it when actual register
   pressure requires that. This state does not need a persistent memory representation merely
   because the Wasm instruction model is stack-based.
2. `Configuration::m_value_stack` and `m_call_record_stack` are fixed 64 MiB virtual-memory
   reservations used for interpreter operand values and call-record storage. `ValueStack` replaced
   a `Vector<Value>` so push and pop become pointer bumps and compiled code can retain stable
   pointers into the storage. It remains useful for interpreter execution and representation
   boundaries, but it is not the machine call stack.
3. Cranelift-generated code uses the executing OS thread's native stack. It contains return
   addresses, saved registers, outgoing ABI state, frontend-requested explicit slots, and
   backend-selected spills. The current dense native-local allocation is one such explicit slot,
   so it consumes native stack rather than providing a separate Wasm stack.

Complete liveness-guided SSA can remove the requirement for that explicit local home. Fresh native
entry can seed the entry block's live locals from typed parameters and Wasm zero values. Normal
forward edges, merges, and loop backedges can pass only live locals as typed block arguments. A
synthetic tier-up entry needs a cold target-specific shim that reads only the target loop header's
live-in locals from the interpreter's canonical `Value` array and supplies those block arguments.
Native calls cannot modify their caller's locals, and a trap or return ends the native activation.
Because the current tiering model has no native-to-interpreter continuation for the same frame,
ordinary native execution does not need to write locals back continuously.

```text
fresh entry: typed parameters + zero values
                         |
                         v
                  SSA block arguments <----- normal edges and loop backedges
                         ^
                         |
OSR entry: load only the target loop header's live-in interpreter locals
```

The local WebAssembly specification's `Implementation Limitations` appendix says that restrictions
may be imposed on "the number of frames", "the number of labels", and "the number of values" on
the stack. If a runtime limit is exceeded, "it may terminate that computation and report an
embedder-specific error to the invoking code"; concrete limits may depend on implementation or
embedder circumstances. This does not require an implementation to represent the abstract Wasm
stack as a separate physical stack. The core `call.wast` test additionally says that every call
must consume an abstract resource towards a finite limit so infinitely recursive tests reliably
trap in finite time. Tail-call instructions have the separate specification guarantee that a
sequence using only those instructions cannot exhaust the active-call limit.

Ladybird currently has different mechanisms for these limits. Interpreter calls inspect the
actual thread stack through `StackInfo`, while direct compiled-to-compiled calls use a coarse
`Configuration::depth() > 500` limit. On macOS, `StackInfo` records that the default main-thread
stack is 8 MiB while other threads default to 512 KiB, so a fixed call-depth threshold is not a
frame-size-aware guarantee and can behave poorly across threads. The 64 MiB `ValueStack` does not
solve this: exhausting it currently reaches capacity assertions, and compiled frames are consuming
the native stack independently.

Cranelift cannot avoid the native stack for ordinary ABI-compatible generated functions, but the
frontend can avoid requesting unnecessary explicit local slots. Cranelift 0.122 also supports a
per-function stack-limit global and stack probes; Ladybird currently configures only `opt_level`
and `is_pic` and uses neither facility. A separate stack-exhaustion workstream should therefore
make compiled prologues account for their actual finalized frame sizes and the executing thread's
stack limit, with probing where needed to prevent large frames from skipping a guard page.

Running Wasm on a separately allocated native stack is technically possible by switching stacks
before native entry. That could normalize available stack space across main and worker threads,
but it would complicate JS and C++ transitions, unwinding, traps, signal handling, and GC. It is not
needed to remove explicit local homes: the immediate code-quality path is complete CFG liveness
and SSA, leaving only Cranelift-selected spills on the ordinary native stack, followed separately
by precise native-stack-limit handling.

The effect repeats across the complete function. The native-locals Ladybird dump contains
approximately 52,609 instructions and 23,898 memory operations, including 6,504 accesses through
the native-locals base. The Wasmtime dump contains approximately 23,964 instructions and 10,784
memory operations. These whole-function counts remain directional because the runtime ABIs and
lowerings differ, but they show that the loop is not an isolated instance of the same state
traffic.

Tier-up dispatch, the interpreter adapter, runtime-context maintenance, call-record fallback, and
the interpreter-oriented bytecode and vstack representation also contribute to the complete-body
gap. They do not explain the matched loop gap because none of that machinery executes inside the
loop. The demonstrated first target is therefore liveness-guided loop and block SSA rather than
removing one of those boundaries.

The intended shape is:

```text
cold fresh/OSR edge: load selected live locals once
                              |
                              v
                  loop header(local0, local4, ...)
                              |
                              v
                  loop body uses typed SSA values
                              |
                              +---- backedge passes current values ----+
                              ^                                        |
                              +----------------------------------------+
```

The shared function-wide resume block should continue to materialize interpreter state into native
slots once. A loop entry can then load only its selected live-in locals and pass them as block
arguments, while the hot backedge passes the current SSA values directly. Native slots remain the
cold and fallback home; they should be updated only on an edge that actually requires memory
state. This avoids both continuous local traffic and the earlier experiment's duplicated
per-loop materialization of all locals.

Focused execution coverage forces selective promotion with more than 256 declared locals, then
checks an unpromoted loop counter after synthetic tier-up, an unpromoted eleventh parameter on
fresh adapter entry, and the same high-arity signature through a compiled-to-compiled call. It
also exercises unpromoted i32, i64, f32, and f64 parameters through both the typed interpreter
adapter and a typed compiled-to-compiled call. Finally, it invokes the tiered function again after
compilation to prove that fresh native entry does not depend on a surviving interpreter locals
array.

Three uncached d3wasm instantiations of the final Cranelift 0.122 candidate report native
compilation times of 3.470, 3.417, and 3.506 seconds: a 3.464-second mean, 0.045-second sample
standard deviation, and 3.417-3.506-second range. Parse, validation, and instantiation remain
separate phases. No pre-change Cranelift 0.122 compiler binary was retained, so these numbers are a
candidate baseline, not a controlled compilation-time improvement claim; the assembly comparison
above is the controlled before/after result.

An unsubmitted experiment on Ali's separate `alimpfard/wasm-simd-fasterer` branch provides useful
context but is not part of the production design's history. WIP commit `94ec25165ff` extracted
straight-line, call-free runs of unsupported SIMD and selected GC, table, and bulk-memory
instructions into `synthetic_interp_region` operations. Native code flushed the operand stack,
called a helper that ran those copied dispatches in the bytecode interpreter, and then resumed
native execution. The experiment was discussed while being tried, was never proposed for merging,
and must not be treated as evidence that current canonical locals intentionally support tier-down.

It does demonstrate what a future mixed-mode facility would require. Such a facility should use an
explicit transition safepoint that materializes only the live locals and operand-stack values the
interpreted region consumes, then reloads only the state it can modify. It must not make ordinary
native blocks continuously maintain interpreter-format state. Until such metadata exists, a
function containing unsupported native operations should remain in the interpreter, or those
operations should be implemented by the native backend.

Across run 4 as a whole, Wasm JIT code accounts for 57.66% of main-thread cycles, JavaScript
16.51%, LibWeb 11.31%, other or system code 8.80%, the Wasm runtime 5.58%, and the Wasm interpreter
0.13%. This reproduces the broad split in runs 2 and 3. The leading resolved Wasm functions are
`AddActiveInteraction()` at 8.18%, `R_CreateLightTris()` at 3.04%, `R_CullLocalBox()` at 2.36%,
and `idSIMD_Generic::Dot(plane, drawvert)` at 2.26%. In the restored baseline code, the dominant
`AddActiveInteraction()` lighting loop is at offsets `0x21b68` through `0x21d20`; it accounts for
1.70% of all main-thread cycles and 20.8% of the function.

An ordinary frame's 42.746 million cycles over 15.009 ms corresponds to an observed rate of about
2.85 billion cycles per second. At that rate, its 24.561 million Wasm JIT cycles are approximately
8.6 ms of cycle-equivalent work. This is an inference from sampled cycles rather than a separately
timed phase, but it means generated Wasm code alone is approximately the complete 8.33 ms target.
Optimizing native-to-native and Wasm-to-JavaScript boundaries remains useful, but boundary-only
work cannot close the complete gap to Chrome in this scene; broad generated-code quality remains
necessary as well.

The 36.729 ms maximum is a different kind of outlier. It contains 49.224 million sampled cycles,
less than several 21-24 ms frames and less than the slow-tail mean. That is consistent with a
temporary scheduling or preemption delay rather than 36 ms of additional game work, and it does
not explain the sustained frame-time deficit.

For d3wasm, the application profile therefore does not justify retaining the narrowed leaf-loop
candidate: it regresses uncached native compilation, its synthetic execution results are mostly
neutral, and its selected loops are cold in the representative heavy scene. A future canonical
traffic experiment must target the two measured hot loop ranges rather than infer importance from
leaf-loop shape alone.

The earlier broader LibWasm failure in the multi-result raw `call_indirect` case was caused by its
stack-element special case decrementing virtual depth and then popping the real `ValueStack`.
The exact-width typed-source change fixes that mismatch, and the broader harness now passes all
fifteen suites and 21 enabled tests.

### Pressure-aware splitting

The 256-definition shortcut can still promote every local in a large function whose initial
values have extensive overlapping live ranges. A diagnostic pass found 100 d3wasm functions with
more than 26 simultaneously live promoted locals. Ninety-nine had at least 256 Cranelift input
instructions. `SDL_Blit_Slow`, function 6570, had 40 locals, 1,737 instructions, 111 reconstructed
blocks, and a peak of 39 simultaneously live locals.

A focused `local-edge-pressure.wasm` microbenchmark was added to `WasmMicroBench`. It has 33
loop-carried locals and 145 Cranelift input instructions. Applying the existing `10 + 6`
selective-promotion policy to every function above the live-local limit reduced its warm native
compilation from approximately 3.1 ms to 2.9 ms, but increased execution from a 0.703459-second
mean to 0.905301 seconds (+28.7%). This established that a live-local limit alone would trade
away generated-code performance in small hot loops.

A bounded candidate applied only on AArch64 and only when all of these conditions held:

- the existing definition estimate is at most 256 and would otherwise promote every local;
- the function has at least 256 Cranelift input instructions;
- more than 26 locals are simultaneously live.

It retains the normal ten stable function-wide promoted locals and six-local block cache.
Lower-priority locals remain in canonical storage. The 145-instruction pressure microbenchmark
remains fully promoted, and its baseline and candidate native dumps are byte-for-byte identical.

Two less bounded variants were rejected:

- Promoting 20 to 26 locals preserved the pressure microbenchmark, but increased d3wasm native
  compilation well above both the ten-local path and the original baseline.
- Making every demoted local eligible for the block cache was neutral to slightly negative for
  d3wasm compilation. It grew function 6570 to 8,588 bytes and a 128-byte frame without improving
  representative execution.

The final bounded candidate changes function 6570 as follows:

| Native-code metric |    Baseline | Pressure split |
| ------------------ | ----------: | -------------: |
| Code size          | 8,032 bytes |    8,428 bytes |
| Instructions       |       2,008 |          2,107 |
| Stack allocation   |   352 bytes |       80 bytes |
| Stack references   |         307 |             81 |

Four alternating exact-binary d3wasm comparisons used a saved pre-change `wasm` executable,
LibWasm dylib, and Cranelift compiler:

| d3wasm metric                |   Baseline | Pressure split | Change |
| ---------------------------- | ---------: | -------------: | -----: |
| Mean native compilation      | 3.314236 s |     3.268507 s |  -1.4% |
| Median native compilation    | 3.281592 s |     3.172354 s |  -3.3% |
| Mean summed loading phases   | 4.635568 s |     4.587863 s |  -1.0% |
| Median summed loading phases | 4.605012 s |     4.483256 s |  -2.6% |

The improvement was modest enough to call compilation neutral-to-positive rather than a strong
loading win. Exact saved-baseline execution comparisons were neutral:

| Workload | Baseline execution | Pressure-split execution | Change |
| -------- | -----------------: | -----------------------: | -----: |
| Base64   |         0.486728 s |               0.486303 s |  -0.1% |
| JSON     |         1.600396 s |               1.590354 s |  -0.6% |
| Regex    |         0.176757 s |               0.177513 s |  +0.4% |
| SHA-512  |         0.737837 s |               0.734060 s |  -0.5% |

CoreMark's baseline and candidate native dumps had identical function sizes and instructions after
ignoring relocated helper addresses, so the pressure policy does not affect its generated code.
Its observed score variation is unrelated to this change.

Runs 1–5 of `/Users/lukewilde/Documents/d3wasm_5.trace` compared the bounded candidate with the
saved baseline compiler while retaining the same `call_indirect` call-record implementation.
Runs 1 and 3 captured loading:

| Loading metric     | Pressure split, run 1 | Baseline, run 3 |
| ------------------ | --------------------: | --------------: |
| Recording duration |           12.123917 s |     11.776818 s |
| Main-thread CPU    |               9.643 s |         9.622 s |
| JIT-code CPU       |               6.955 s |         6.922 s |

Instruments was attached to WebContent rather than the external compiler process, so its CPU
samples do not directly measure native compilation. The manually stopped recording duration also
favored the baseline, while sampled WebContent work was identical.

Gameplay runs 2 and 4 had comparable recording durations of 99.850 and 97.771 seconds. Normalized
by recording duration, the pressure candidate used 5.6% more main-thread CPU and 10.0% more
JIT-code CPU. That could represent either more work per frame or more frames completed; the trace
has no frame counter, and no corresponding frame-rate improvement was observed. Baseline run 5
captured a shorter, heavier scene and was not averaged with runs 2 and 4.

The candidate was removed rather than committed. It added a live-local analysis and two empirical
thresholds without a repeatable loading or execution improvement. The diagnostic results and
focused microbenchmark remain available if further compilation-time squeezing is needed later.

### Raw-call argument-suffix experiment

Phase 1 materializes every live vstack value before a raw call, even though the stack ABI consumes
only the call's argument suffix. A temporary follow-up passed the known parameter count to the
Cranelift subprocess and emitted stores only for that suffix. `call_indirect` consumed its table
element index from vstack first, then materialized the remaining arguments.

A purpose-built benchmark kept 16 unrelated values live below a 16-argument raw call for 50
million iterations. Three forward/reverse measurements produced:

| Metric             | Full live vstack | Argument suffix | Change |
| ------------------ | ---------------: | --------------: | -----: |
| Native compilation |       0.002946 s |      0.002854 s |  -3.1% |
| Execution          |       1.048182 s |      1.040085 s |  -0.8% |

The first full-vstack execution sample was 1.075056 seconds; the other two were 1.037006 and
1.032485 seconds. Excluding that cold outlier reverses the execution comparison to a 0.5%
regression. The compilation difference is 0.09 ms and is also below the observed variation.

More importantly, native dumps of the benchmark's hot function were identical after ignoring the
relocated helper address: both were 696 bytes and contained stores only for the 16 call arguments.
Cranelift had already eliminated the unused stores for the 16 values below the argument suffix.
The ordinary 16- and 32-argument call controls were likewise neutral.

The argument-suffix change was removed rather than committed. Although it reduces the frontend IR
constructed for some raw calls, it did not improve generated code or establish a measurable
compilation win. The next call-boundary phase should instead reuse the earlier frameless
compiled-to-compiled call work, which removes bridge-side frame management and is independent of
phase 1's vstack handling for raw fallback calls.

### Frameless compiled-to-compiled calls

The earlier `wasm-opt-stuff` sequence established the right prerequisites for removing the
remaining compiled-to-compiled bridge overhead:

1. Cache the active module and compiled-function table in `Configuration` scalars so bridge
   helpers do not have to find them through the top interpreter `Frame`.
2. Run a compiled callee without pushing a lightweight `Frame`. Save the caller's scalar context
   on the native stack and restore it when the bridge helper returns.
3. Track the innermost compiled expression separately so signal faults use the callee's trap table
   rather than the nearest interpreter frame's expression.
4. Distinguish calls originating in compiled code when propagating exceptions. A compiled caller
   has no interpreter label stack to search.

The current candidate ports those pieces together rather than recreating the known interval in
which frameless calls could not recover a fault in an inner compiled callee. The callee still
updates depth and reserves its call-record area, but it saves and restores the call-record mark and
base directly instead of using frame-stack unwind. A regression test traps on integer division by
zero in a compiled callee and then calls the instance again to verify that both fault lookup and
context restoration are intact.

The baseline and candidate binaries were built from the same source state, with the baseline
removing only the staged frameless-call change. Each binary swap was followed by a discarded
direct-call warm-up. The direct-call sweep used three alternating measurements per configuration:

| Workload            | Baseline compile | Frameless compile | Baseline execute | Frameless execute | Execute change |
| ------------------- | ---------------: | ----------------: | ---------------: | ----------------: | -------------: |
| `call-00-args.wasm` |       0.002780 s |        0.002786 s |       0.050877 s |        0.050932 s |          +0.1% |
| `call-01-args.wasm` |       0.002586 s |        0.002941 s |       0.053002 s |        0.052706 s |          -0.6% |
| `call-02-args.wasm` |       0.003011 s |        0.002873 s |       0.054800 s |        0.054788 s |          -0.0% |
| `call-03-args.wasm` |       0.003042 s |        0.003067 s |       0.063571 s |        0.063062 s |          -0.8% |
| `call-04-args.wasm` |       0.003052 s |        0.002899 s |       0.055939 s |        0.056386 s |          +0.8% |
| `call-16-args.wasm` |       0.002931 s |        0.002706 s |       1.379522 s |        1.262393 s |          -8.5% |

The 0–4-argument cases are neutral. The 16-argument call loop improves by 8.5%, demonstrating that
removing the frame operation remains material on a call-record-heavy path. The apparent direct
microbenchmark compilation differences are fractions of a millisecond and are not systematic.

Two forward/reverse measurements per configuration on the broader suite produced:

| Workload | Baseline compile | Frameless compile | Change | Baseline execute | Frameless execute | Change |
| -------- | ---------------: | ----------------: | -----: | ---------------: | ----------------: | -----: |
| Base64   |       0.025432 s |        0.026170 s |  +2.9% |       0.572903 s |        0.489895 s | -14.5% |
| JSON     |       0.046109 s |        0.047819 s |  +3.7% |       1.948033 s |        1.630440 s | -16.3% |
| Regex    |       0.321878 s |        0.328726 s |  +2.1% |       0.237736 s |        0.241844 s |  +1.7% |
| SHA-512  |       0.024952 s |        0.025098 s |  +0.6% |       0.746371 s |        0.740782 s |  -0.7% |
| CoreMark |       0.008128 s |        0.008330 s |  +2.5% |      22.158504 s |       19.987250 s |  -9.8% |

CoreMark's primary result is its returned score rather than wall-clock execution time:

| CoreMark metric |    Baseline |   Frameless | Change |
| --------------- | ----------: | ----------: | -----: |
| Score           |  18,559.246 |  20,575.113 | +10.9% |
| Execution       | 22.158504 s | 19.987250 s |  -9.8% |

The individual baseline scores were 18,591.969 and 18,526.523; the frameless scores were
20,424.836 and 20,725.389.

Base64 and JSON show substantial execution improvements, and CoreMark's score improves by 10.9%.
SHA-512 is neutral. The two Regex candidate samples were 0.247513 and 0.236176 seconds, spanning
the 0.237736-second baseline, so its reported +1.7% mean is also treated as neutral. Native
compilation is unaffected by the runtime frame change; the displayed percentage changes correspond
to approximately 0.1–1.7 ms and are within the variation already seen in these workloads.

This validates the first part of the earlier call-boundary sequence on top of the new local-cache
and raw-vstack work. Routing `call_indirect` through call records remains a complementary next
step rather than a prerequisite for direct frameless calls.

### Indirect calls through call records

The next phase applies the existing call-record allocation to eligible `call_indirect` sites.
The table element index remains an ordinary dispatch operand while only the function arguments
receive call-record slots. A dedicated bridge helper performs the table bounds, reference-kind,
and defined-type checks, then reuses the frameless compiled-call path. Calls with more arguments
than the record can hold, more than one result, or a polymorphic operand stack retain the raw
interpreter-stack ABI.

The baseline and candidate were built from the same source state, and the harness swapped the Wasm
CLI, Cranelift compiler, and LibWasm dynamic library together. The CLI does not supply a
`CompileCacheConfig`, so these measurements do not install cached JIT blobs. Each swap was followed
by a discarded module warm-up.

Two 50-million-iteration purpose benchmarks used a two-argument indirect call. The second also
contained a conditional branch and merge inside the loop. Three alternating measurements per
configuration produced:

| Workload                   | Baseline compile | Call-record compile | Baseline execute | Call-record execute | Execute change |
| -------------------------- | ---------------: | ------------------: | ---------------: | ------------------: | -------------: |
| Indirect loop              |       0.002228 s |          0.002265 s |       3.261486 s |          0.561801 s |         -82.8% |
| Branch-heavy indirect loop |       0.002314 s |          0.002387 s |       3.340271 s |          0.566601 s |         -83.0% |

The approximately 0.04–0.07 ms compilation differences are noise. The result is consistent with
the old branch's reported 79% and 65% instruction-count reductions and confirms that this remains
valuable after preserving the vstack across raw calls.

The representative suite produced:

| Workload | Baseline compile | Call-record compile | Change | Baseline execute | Call-record execute | Change |
| -------- | ---------------: | ------------------: | -----: | ---------------: | ------------------: | -----: |
| Base64   |       0.022926 s |          0.022584 s |  -1.5% |       0.498198 s |          0.477600 s |  -4.1% |
| JSON     |       0.043920 s |          0.042516 s |  -3.2% |       1.596868 s |          1.578384 s |  -1.2% |
| Regex    |       0.283715 s |          0.291767 s |  +2.8% |       0.232092 s |          0.177463 s | -23.5% |
| SHA-512  |       0.034324 s |          0.023678 s | -31.0% |       0.730484 s |          0.726114 s |  -0.6% |

Regex uses three additional alternating measurements per configuration and shows a stable 23.5%
execution improvement. Base64's first baseline sample was 0.518711 seconds while its other
baseline and both candidate samples were approximately 0.477–0.478 seconds, so the displayed
4.1% mean is treated as a cold-run artefact. JSON and SHA-512 are neutral. SHA-512's compilation
percentage is likewise caused by one 0.042596-second baseline sample; the other baseline and both
candidate samples were approximately 0.023–0.026 seconds.

CoreMark is neutral:

| CoreMark metric    |    Baseline | Call records | Change |
| ------------------ | ----------: | -----------: | -----: |
| Score              |  21,095.568 |   21,029.020 |  -0.3% |
| Execution          | 19.504230 s |  19.590562 s |  +0.4% |
| Native compilation |  0.007836 s |   0.007728 s |  -1.4% |

Three alternating d3wasm measurements per configuration gave:

| d3wasm metric                                    |   Baseline | Call records | Change |
| ------------------------------------------------ | ---------: | -----------: | -----: |
| Native compilation                               | 3.188896 s |   3.003643 s |  -5.8% |
| Parse + validation + compilation + instantiation | 4.439803 s |   4.272733 s |  -3.8% |

One baseline native-compilation sample was 3.377885 seconds, compared with 3.104530 and 3.084272
seconds around it. Excluding that outlier gives a more conservative 2.9% native-compilation
improvement and a 1.2% improvement in the summed loading phases. The earlier two-sample suite
comparison independently moved native compilation from 3.205628 to 3.060576 seconds. The candidate
therefore appears modestly compilation-positive rather than merely neutral, but interactive
loading remains the more representative confirmation.

The candidate rewrites all 16,020 `call_indirect` sites printed for d3wasm; none remains on the raw
path. The permanent fixture covers zero-result and one-result record calls, the multi-result raw
fallback, type mismatch, null table elements, table bounds, and successful reuse after trapping.

### Direct native-to-native calls

Before the direct-relocation work, the compiled-call fast path was native only at its endpoints. A
Cranelift caller still called `wasm_cl_direct_call_N` or a call-record helper, which allocated and
initialized callee storage, replaced scalar state in `Configuration`, invoked the callee's native
handler, restored the caller context, and transferred a result through scratch storage. A direct
call therefore crossed two native ABIs:

```text
Cranelift caller -> C++ bridge helper -> Cranelift callee
```

In baseline gameplay run 4 of `/Users/lukewilde/Documents/d3wasm_5.trace`, bridge helpers account
for roughly 7% of main-thread CPU as direct self-time. This is a lower bound. Argument
serialization, call-record access, result loads, status branches, and spills and reloads around
the opaque helper call are attributed to the surrounding generated-code frames. A conventional
native call still clobbers caller-saved registers, but it would remove one ABI boundary and allow
Cranelift to use the callee's actual signature.

#### Existing relocation and cache support

At the start of this work, the cache already stored unpatched machine code and a relocation table.
Cranelift emitted an eight-byte absolute relocation for every runtime-helper address. The
subprocess returned those records with each function, cache blobs preserved them, and the installer
applied live current-process addresses while the JIT mapping was writable and before publishing
the executable entry point.

That machinery was deliberately specialized rather than absent:

- `HelperReloc` identified a code offset, helper ID, and addend.
- The Rust compiler accepted only `Reloc::Abs8`.
- The installer resolved only helper IDs.
- Each function received an independent code mapping and was published immediately
  after its own relocations are applied.

Cranelift also supports the direct-call relocations needed here: `Arm64Call` on AArch64 and
`X86CallPCRel4` on x86-64. A direct Wasm call already contains its module function index. The Rust
compiler can preserve that index as a distinct external-name namespace, and the cache relocation
format can generalize its target from only `Helper(id)` to `Helper(id) | WasmFunction(index)`.
Cache-format versioning already makes such a layout change a clean cache miss.

#### Function eligibility is known module-wide

Validation finishes every defined function before `compile_module_to_native()` begins. It records
`cranelift_eligible` after checking the bytecode shape, local and result types, memory address
types, and direct-call result shapes. Native compilation then walks the entire code section,
assigns final module function indices, queues every eligible function, and flushes one batch.
This gives the parent a complete module-wide set of functions eligible to be attempted.

The validation flag is not the final success result. `compile_to_bytes()` can still reject an
unsupported instruction or block shape, a debug filter can omit a function, and output capacity
or code generation can fail. However, the subprocess finishes the whole batch and writes a
`compiled` flag and code size for every successful function before returning. The parent can
therefore scan the complete output and know the exact successful set before installing any code.
On a cache hit, the blob already enumerates the exact function indices for which native code was
captured.

This supports a two-phase module linker:

1. Scan all fresh-batch outputs or cache records and collect the successful functions and sizes.
2. Allocate code mappings and assign every native address without publishing an entry.
3. Apply helper relocations and resolve direct-call relocations by module function index.
4. Publish all fully linked entries with the existing release-store protocol.

Allocating one module code region also keeps ordinary AArch64 branches within their signed
26-bit, four-byte-scaled range for modules smaller than 128 MiB in either direction. A general
implementation still needs a veneer when a target falls outside that range. Self-recursion and
mutual recursion do not require special runtime lookup because every successful function receives
an address before relocation.

Calls from a successfully compiled caller to another successfully compiled defined function can
then become an ordinary direct `bl` or `call`; they do not need a function-table load. Imports,
functions rejected by Cranelift, and failed compilations retain a semantics-preserving bridge
fallback. `call_indirect` remains genuinely dynamic after its table and defined-type checks, but
can eventually dispatch through a native entry record using the same call ABI.

#### Interpreter interoperability and tiering

Direct native calls must preserve the existing interpreter-to-native tier-up paths. The bytecode
interpreter currently observes the release-published `cranelift_entry` at function entry. Large
eligible functions also contain `synthetic_tier_up` checkpoints immediately after suitable loop
headers. A checkpoint can enter native code at the corresponding bytecode IP because the operand
stack is empty there and live locals have canonical storage from which the native resume path can
reload them.

A compiled function should consequently expose two entry paths:

1. A signature-aware native-call entry for compiled callers.
2. An interpreter-compatible entry and resume adapter that retains the current handler inputs,
   including the checkpoint IP.

Both paths can enter the same generated body after performing their respective setup. Publishing
the interpreter-compatible entry only after the module's native code and relocations are fully
installed preserves the current behavior: execution continues in bytecode while compilation is
pending, then a function entry or loop checkpoint can tier up safely.

Calling a function that did not compile is the corresponding native-to-interpreter transition.
Because final compilation success is known before linking, each direct call can be resolved
without a runtime target-table lookup:

```text
compiled caller -> compiled callee: direct native branch
compiled caller -> uncompiled callee: direct branch to fallback adapter
```

The fallback adapter materializes the known arguments into an interpreter call frame, invokes the
bytecode callee, converts its results back to the native ABI, propagates traps and exceptions, and
returns to the suspended compiled caller. Only the callee is interpreted; successful return
resumes the caller in native code. Imported and host functions use the same fallback category.

This does not require arbitrary deoptimization of the currently executing native function.
Ladybird does not presently resume an arbitrary native activation in the bytecode interpreter or
unpublish installed native code while its module remains alive. If future tiers require code
invalidation or general on-stack tier-down, they will need safepoints, reconstruction of
interpreter-visible state, and either patched direct-call sites or stable thunks. That is separate
from preserving the current entry/checkpoint tier-up and native-call fallback behavior.

The native-to-interpreter fallback is primarily a correctness path, not an initial performance
target. Entering the bytecode interpreter inherently requires materializing arguments and results,
establishing interpreter-visible frame state, and propagating traps and exceptions. The first
implementation should reuse the existing bridge where practical and avoid complicating or
weakening the native-to-native ABI to optimize this comparatively cold boundary. Native-to-native
calls and fallback calls should be benchmarked separately.

#### Initial full-ABI direction

The initial design direction was a native Wasm call ABI that no longer switched shared
caller/callee state through `Configuration`. Such a complete ABI would need:

- an explicit native activation or context containing module state, locals and call-record
  storage, parent state, and trap propagation;
- signature-aware argument and result passing, including multi-value results;
- stack-exhaustion and recursion accounting;
- a bridge adapter with the same semantics for interpreter, host-function, imported, and
  otherwise non-native targets;
- correct synchronization with tier-up publication and cached-code installation.

The first implementation deliberately stopped short of that full ABI. The existing call-record
layout already gives calls with arbitrary parameter counts stable callee storage. Reusing it made
it possible to remove the C++ boundary first while retaining the established handler ABI and
interpreter fallback. Signature-specific register argument passing and a separate activation
object remain possible later optimizations rather than prerequisites.

#### Implementation status on 2026-07-31

The first direct native-call checkpoint is complete on `wasm-opt-stuff2`. The implementation is
split across four commits:

1. `3a944eb1e23` generalizes helper relocations into `CraneliftRelocation`, with separate helper
   and Wasm-function target namespaces and AArch64 and x86-64 direct-call relocation kinds.
2. `036631cbbc4` separates code installation into allocate, link, finalize, and publish phases so
   all successful fresh-batch function addresses exist before any direct edge is resolved.
3. `3815457e04c` exposes the call-record, compiled-function-table, expression, and depth offsets
   required by generated callees. It also sizes caller records after all functions have been
   validated, including forward callees' inlined locals.
4. `e95b7ec0784` lowers eligible call-record direct calls to native call relocations, installs
   veneers and fallbacks, and adds end-to-end tests.

The implemented scope is intentionally narrower than every Wasm call:

- `synthetic_call_with_record_0` and `synthetic_call_with_record_1` can branch directly to a
  successfully compiled defined callee.
- The existing zero-to-three-argument specialized calls still use `wasm_cl_direct_call_N`.
- `call_indirect` still performs dynamic table/type checks and enters through its call-record
  helper; it cannot be statically relocated to one function.
- Multi-result and other raw-call fallbacks are unchanged.

The direct path reuses the six-argument native handler signature. A null instruction pointer marks
a compiled-caller entry; the existing shortened-IP argument carries the module function index and
the fifth argument carries the zero-or-one result count. The callee then:

1. Saves `locals_base`, `call_record_base`, the call-record stack top, `current_expression`, and
   `depth` in its native activation.
2. Uses the caller's current call record as its parameter/local storage.
3. Loads its expression from the current compiled-function table, allocates its own outgoing
   call-record area, increments depth, and enters the normal generated body.
4. Leaves a successful result on the real value stack and returns status zero, or returns status
   one after a trap.
5. Restores all saved scalar context on either exit.

Normal interpreter-compatible entry is retained. It still receives a non-null instruction and
returns `Outcome::Return`. A direct entry cannot be mistaken for a synthetic tier-up resume: tier
dispatch is considered only for normal entries. Entry publication also remains the final
release-store after linking and executable finalization, so the bytecode interpreter continues
running while native code is unavailable and can tier up at the existing entry/checkpoint sites.

Fresh compilation now resolves each Wasm-function relocation against the complete successful
batch. AArch64 `Arm64Call` and x86-64 `X86CallPCRel4` edges are patched directly when in range. Each
mapping reserves a 16-byte veneer per relocation; an out-of-range edge uses a local veneer that
jumps to the actual callee. If the target is an import, was rejected, was filtered out, or failed
compilation, the same mechanism targets `wasm_cl_direct_call_with_record_fallback`. That adapter
reuses `wasm_cl_call_with_record`, propagates traps, and places a single scratch result back on the
real value stack so the caller has one result convention for both paths.

Stack-depth accounting remains equivalent to the previous bridge path. A direct callee checks the
existing 500-level limit, calls a small runtime helper to set the standard stack-exhaustion trap,
and restores its caller context. Using a helper avoids embedding and constructing the trap string
in every generated function.

The fresh-batch installer is all-or-nothing after direct edges are introduced. It allocates every
mapping, links every mapping against the complete target map, finalizes every mapping, and only
then publishes entries. This prevents a published caller from retaining an edge to a mapping that
was discarded after a later link or finalization failure.

Focused `TestWasmExecution` coverage now verifies:

- a four-argument compiled caller directly calls a forward compiled callee whose inlined locals
  are held in the caller's record;
- a compiled call-record caller falls back correctly to an imported host function and receives
  its result;
- a trap in a directly called compiled callee restores context sufficiently for a later direct
  call on the same instance to succeed.

Validation completed before the checkpoint commit:

```sh
cargo clippy --all-targets -- -D clippy::all
cargo test
cmake --build Build/release --target TestWasmExecution -j8
../../Build/release/bin/TestWasmExecution
```

The six C++ execution tests and nine Rust tests passed, and the commit hook passed the complete CI
lint set. The execution tests must run outside the command sandbox because the out-of-process
Cranelift compiler uses POSIX shared memory; a sandboxed run silently remains in the bytecode
interpreter and is not a valid native-path test.

#### First checkpoint benchmark comparison

The first comparison used `3815457e04c` as the baseline and `e95b7ec0784` as the candidate. Both
were release builds. Every sample used a fresh `wasm` process without a `CompileCacheConfig`, so no
cached JIT blob was installed. A discarded compiler invocation preceded each configuration. The
focused direct-call tests used five iterations. The broader CoreMark, JSON, and Regex comparison
used three baseline-first iterations followed by two candidate-first iterations to check ordering;
Base64 and SHA-512 used three baseline-first iterations. Native compilation and execution are
reported separately and do not contribute independently to the existing benchmark score.

The focused call tests show that the implemented path is effective when it is exercised heavily:

| Test                | Baseline total | Candidate total | Change |
| ------------------- | -------------: | --------------: | -----: |
| `call-00-args.wasm` |     0.073656 s |      0.075700 s |  +2.8% |
| `call-01-args.wasm` |     0.076950 s |      0.078986 s |  +2.6% |
| `call-02-args.wasm` |     0.077345 s |      0.079675 s |  +3.0% |
| `call-03-args.wasm` |     0.084408 s |      0.084697 s |  +0.3% |
| `call-04-args.wasm` |     0.078299 s |      0.078117 s |  -0.2% |
| `call-16-args.wasm` |     1.257411 s |      0.909147 s | -27.7% |
| `call-32-args.wasm` |     1.552710 s |      1.103989 s | -28.9% |

The phase table independently measured 28.2% and 29.3% execution-time improvements for the 16-
and 32-argument cases. The zero-to-four-argument totals are dominated by process startup and are
effectively neutral. This is also the expected scope boundary: those calls still use
`wasm_cl_direct_call_N`, while the two high-arity fixtures use call records and therefore reach the
new relocation path.

The representative results were mixed. The first three rows below combine the forward and reverse
ordering runs. Phase values are approximate because the runner prints them to millisecond
precision:

| Workload | Baseline native compile | Candidate native compile | Baseline execute | Candidate execute | Execution change |
| -------- | ----------------------: | -----------------------: | ---------------: | ----------------: | ---------------: |
| CoreMark |                0.0122 s |                 0.0126 s |         19.537 s |          19.801 s |            +1.4% |
| JSON     |                0.0414 s |                 0.0532 s |          1.609 s |           1.663 s |            +3.4% |
| Regex    |                0.2874 s |                 0.3436 s |         0.1746 s |          0.1670 s |            -4.4% |
| Base64   |                 0.023 s |                  0.028 s |          0.487 s |           0.490 s |            +0.6% |
| SHA-512  |                 0.024 s |                  0.028 s |          0.727 s |           0.727 s |             0.0% |

CoreMark's reported score moved from 21049.696 to 20749.095 (-1.4%). The combined JSON process
time moved from 1.684735 to 1.751357 seconds (+4.0%), while Regex moved from 0.614440 to 0.664143
seconds (+8.1%) despite its 4.4% execution improvement because its native-compilation time grew by
about 20%. Base64 and SHA-512 execution were neutral. The reverse-order samples repeated the
CoreMark and JSON regressions and the Regex execution improvement, so the broad shape is not only
the original baseline-first ordering.

The uncached d3wasm CLI instantiation surrogate used
`/Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm` for three fresh processes per
configuration:

| d3wasm metric                                    |   Baseline |  Candidate | Change |
| ------------------------------------------------ | ---------: | ---------: | -----: |
| Native compilation                               | 2.966909 s | 3.430964 s | +15.6% |
| Parse + validation + compilation + instantiation | 4.212397 s | 4.686639 s | +11.3% |

Interactive testing likewise found no d3wasm load-time improvement, but did find an in-game FPS
improvement. Instruments still attributed substantial time to the direct-call bridge and
`call_indirect_with_record`. That is consistent with the implementation rather than evidence that
the relocated calls are ineffective: only `synthetic_call_with_record_0` and
`synthetic_call_with_record_1` bypass the C++ bridge at this checkpoint. Ordinary zero-to-three-
argument direct calls and all indirect calls still use bridge helpers.

The candidate also emits the direct-entry setup and return paths in every compiled function,
including functions that no compiled caller targets. The focused execution win therefore arrives
with module-wide generated-code and compilation costs. The checkpoint is useful evidence for
native-to-native calls, but it should be refined before being treated as the final form.

The immediate next checkpoint should extend native dispatch to every eligible compiled direct
call, while emitting a native-call entry only for functions that can be native call targets. The
longer-term endpoint is to remove compiled-to-compiled invocation machinery from
`CraneliftBridge`: direct calls use native relocations, and indirect calls perform their dynamic
table and type checks before invoking a compatible native entry. Bridge helpers remain for real
transitions to the bytecode interpreter, imports, host functions, and compilation failures.

Cached code remains a necessary implementation checkpoint. Cache records are currently installed
one function at a time. A cached self-call can resolve directly, but a cross-function relocation
does not yet see the other cached mappings and therefore resolves to the correct bridge fallback.
A later change should collect all cache records and their `CompiledInstructions` targets during the
module walk, allocate/link/finalize them as one batch, and publish only after the complete cache
target map is available. Fresh compilation and execution time must remain distinct when that work
is measured; the cache is not needed merely to separate those phases.

The native-disassembly testing changes remain in `stash@{0}` (`Wasm JIT dump testing
infrastructure`) and were not included in the implementation commits.

#### Typed native-call ABI draft

The next uncommitted checkpoint replaces the first checkpoint's handler-shaped native ABI with a
signature-aware ABI for direct calls. The mapping still begins with an interpreter adapter at
offset zero, while `native_entry_offset` identifies the separately compiled native body. The
interpreter calls the adapter; relocated compiled callers branch directly to the body.

For functions with at most eight parameters, the body uses the platform ABI for the four numeric
Wasm types. On AArch64 the current argument assignment is:

```text
x0  BytecodeInterpreter*
x1  Configuration*
w2  entry token: zero for a compiled call, interpreter resume IP plus one otherwise
x3… integer Wasm parameters
v0… floating-point Wasm parameters
```

The result is returned in the platform ABI result register. A successful native call therefore
does not return a trap status and does not place its result in the interpreter `ValueStack`.
Explicit traps and helper-reported traps call `wasm_cl_raise_trap`, which performs the same
non-local escape as native memory and Cranelift traps. Each possibly-native `interpret()` entry
installs a nested recovery context so a compiled-to-interpreted-to-compiled call catches the trap
at the correct interpreter activation rather than jumping across C++ cleanup.

Every direct target also has a cold signature-matched fallback stub in the caller's mapping. If
the target is an import, unsupported, filtered out, or not successfully compiled, relocation
targets that stub. It marshals parameters to the interpreter value stack, calls the ordinary
function machinery, returns a numeric result through the native result register, and raises an
existing trap only after the C++ call context has unwound.

Passing every parameter through the platform ABI produced a clear high-arity cliff. The initial
draft measured 0.997 seconds of execution for `call-16-args.wasm` and 1.879 seconds for
`call-32-args.wasm`; the first call-record-native checkpoint had total process times of 0.909 and
1.104 seconds respectively. Values beyond the available argument registers were copied to the
native stack even though the bytecode compiler had already placed them in stable call-record
storage.

The refined ABI retains the signature-aware value ABI through eight parameters, but passes one
pointer to the existing call record for larger signatures. The callee uses that record directly as
its locals storage and still returns its result in the native result register. Three final samples
measured:

| Benchmark           | Total time | Execution time |
| ------------------- | ---------: | -------------: |
| `call-16-args.wasm` |    0.778 s |        0.753 s |
| `call-32-args.wasm` |    1.117 s |        1.090 s |

This removes 24.5% and 42.0% of the execution time of the all-value-ABI draft. The 16-argument
case is also 14.4% faster in total than the first call-record-native checkpoint; the 32-argument
case is within 1.2%. End-to-end tests cover i32, i64, f32, and f64 value-ABI calls, a four-argument
raw call, a nine-argument record-ABI call, interpreted fallbacks for both ABI forms, and explicit
trap propagation.

The final three-iteration representative measurements were:

| Workload | Total or score | Native compile |  Execute | Change from first native-call checkpoint |
| -------- | -------------: | -------------: | -------: | ---------------------------------------: |
| CoreMark |      23776.645 |        0.009 s | 17.270 s |           score +14.6%; execution -12.8% |
| Base64   |        0.468 s |        0.029 s |  0.410 s |                         execution -16.3% |
| JSON     |        1.463 s |        0.054 s |  1.373 s |           total -16.5%; execution -17.4% |
| Regex    |        0.678 s |        0.349 s |  0.177 s |             total +2.1%; execution +6.0% |
| SHA-512  |        0.781 s |        0.027 s |  0.722 s |                          execution -0.7% |

Blake3 remained dominated by its interpreted SIMD path and is not useful evidence for this
change. The result file is `/private/tmp/ladybird-wasm-native-abi-refined.json`, with SHA-256
`267e6563b44c579fbb55268a4e35dafe392de580a207fae45198f57e422034a0`.

Three uncached d3wasm instantiation samples averaged 3.071212 seconds of native compilation and
4.343605 seconds for parsing, validation, native compilation, and instantiation. Relative to the
first native-call checkpoint, those are improvements of 10.5% and 7.3% respectively.

##### Native-code inspection

`wasm --print-compiled` was used only to verify bytecode lowering: in particular, that the test's
outer four-argument call remained a raw `call` instead of being inlined or assigned a call record.
`wasm --dump-native --print-function` was then used for machine-code inspection.

The AArch64 dump confirms the intended entry topology and call conventions:

- Function 13's four-argument raw outer call places its live values directly in `w3` through
  `w6` and uses a direct `bl` to function 16's body. It does not materialize a call record.
- Four-argument calls that were already selected for call records load the four values into
  `w3` through `w6`, then use the same direct `bl` target.
- Function 14's nine-argument call loads the existing call-record address into `x3`, sets the
  direct-call token to zero, and directly branches to function 17's body. It does not reload and
  copy the nine values at the native boundary.
- The adapters remain at mapping offset zero. In this dump, function 16's body begins at offset
  `0x70` and function 17's body at `0x60`; relocated native calls target those body entries rather
  than the adapters.
- A trailing high-arity fallback stub performs the call-record-to-`ValueStack` copy only when the
  compiled target is unavailable. It is not on the successful native-to-native path.

The first dump exposed two generated-code problems. The four-argument body stored `w3` through
`w6` into its 64-byte canonical locals area and then reloaded them. The nine-argument body eagerly
loaded all nine parameters even though the test uses only parameters zero and eight.

The first problem is now fixed. Parameters chosen for function-wide SSA promotion are initialized
directly from the typed entry ABI. The interpreter adapter loads the canonical parameter values
into the same ABI registers before calling the body, while a native caller supplies them directly.
The body therefore consumes one representation regardless of how it was entered. A tier-up entry
also receives the current parameter values through the adapter; only non-parameter locals need to
be recovered from canonical storage when resuming in the middle of a function.

The new function 16 dump is 436 bytes, down from 484 bytes before this change. Its adapter loads
the four parameters once into `w3` through `w6`. At the body entry at offset `0x70`, the relevant
code uses those values directly:

```text
  80: mov x14, x3
  ...
 10c: mov x8, x14
 110: add w14, w8, w4
 114: add w0, w5, w6
 118: add w14, w14, w0
```

There are no canonical parameter stores in the direct setup and no parameter reloads in the body.
The stores later in the dump synchronize interpreter dispatch registers and are not parameter-local
materialization.

An initial version also stopped eagerly clearing memory slots for promoted non-parameter locals.
That is a separate valid optimization, because a promoted local can start as an SSA zero and only
needs a canonical store if it is later materialized. It was reverted here to isolate parameter
passing from local initialization. Unpromoted non-parameter locals still require their canonical
slots to be zeroed, and the scoped change preserves the previous zeroing behavior for every
non-parameter local. The unscoped version moved `call-32-args.wasm` execution from 1.090 to
1.158 seconds; restoring the old zeroing behavior returned it to 1.076 seconds.

Three scoped samples measured:

| Workload                | Refined ABI | Promoted parameters | Change |
| ----------------------- | ----------: | ------------------: | -----: |
| CoreMark reported score |   23776.645 |           24836.823 |  +4.5% |
| Base64 execution        |     0.410 s |             0.378 s |  -7.8% |
| JSON execution          |     1.373 s |             1.282 s |  -6.6% |
| Regex execution         |     0.177 s |             0.173 s |  -2.3% |
| SHA-512 execution       |     0.722 s |             0.722 s |   0.0% |
| Local-edge execution    |     0.759 s |             0.756 s |  -0.4% |
| 16-argument execution   |     0.753 s |             0.746 s |  -0.9% |
| 32-argument execution   |     1.090 s |             1.076 s |  -1.3% |

CoreMark's printed score is the primary metric; its separately reported execution phase was noisy
and moved in the opposite direction. The zero-to-four-argument microbenchmarks are largely inlined,
so they do not isolate this native callee-entry improvement. The instruction dump is the direct
evidence for eliminating the parameter memory round trip, while the representative workloads show
no scoped regression.

The result file is
`/private/tmp/ladybird-wasm-native-abi-promoted-params-scoped.json`, with SHA-256
`f4d5fe8c90ccd57cb33090878fb6bb8f7b84047d90838cea9860119a19f4ef5b`.

Three uncached d3wasm samples averaged 3.221210 seconds of native compilation and 4.499370 seconds
for parsing, validation, native compilation, and instantiation. These are 4.9% and 3.6% slower than
the immediately preceding three-sample result, so this small sample does not establish a d3wasm
compilation-time improvement. That result is kept separate from the execution measurements.

Avoiding eager loads of unused parameters for the high-arity call-record ABI remains open.

##### Native context arguments

The three context arguments are not equally fundamental for compiled-to-compiled calls:

- `Configuration*` in `x1` is actively used on the normal path for locals, memories, globals,
  call-record allocation, call depth, and compiled-function metadata. Some equivalent context is
  required. A future custom calling convention could pin it in a reserved callee-saved register,
  but the current platform ABI must pass it.
- `BytecodeInterpreter*` in `x0` is needed only by cold paths and helpers, including trap creation,
  stack exhaustion, and transitions to interpreted or host functions. Ordinary compiled
  execution does not use it. Storing it in or making it recoverable from `Configuration` would
  free `x0` for a Wasm integer parameter and load the interpreter only on cold paths.
- The `w2` entry token is not semantically needed by a compiled-to-compiled call; every such call
  passes zero. It exists because the adapter and native callers currently enter the same
  Cranelift body. A nonzero value carries the interpreter resume IP plus one, allowing the body to
  enter at a tier-up checkpoint rather than at the function start.

Removing `w2` cleanly is therefore coupled to genuinely separate native-only and interpreter-
resumable entries. Merely changing `native_entry_offset` to an internal block is unsafe: the
ordinary function prologue establishes the native frame and moves ABI parameters before that
block. Making the native body always start from the function entry would also lose mid-function
interpreter-to-native tier-up unless a separate resume path were retained. Removing or making
`x0` cold is the more self-contained ABI improvement; `x1` should remain until there is a deliberate
fixed-register context convention.

##### Cold interpreter-pointer experiment

A follow-up experiment tested that apparently self-contained `x0` change before optimizing
`call_indirect`. The active `BytecodeInterpreter*` was stored in `Configuration`, removed from the
native body signature, and loaded only immediately before helpers and trap transitions. The body
signature became `(Configuration*, entry token, Wasm arguments...)`. In function 16, the four
integer parameters moved from `w3`–`w6` to `w2`–`w5`, and the ordinary arithmetic path contained no
interpreter load. The cold stack-exhaustion and fallback paths loaded it from `Configuration`.

The result demonstrated that those paths are not all cold yet:

| Workload                | Typed ABI checkpoint | Cold interpreter | Change |
| ----------------------- | -------------------: | ---------------: | -----: |
| CoreMark reported score |            24836.823 |        24678.878 |  -0.6% |
| Base64 execution        |              0.378 s |          0.380 s |  +0.5% |
| JSON execution          |              1.282 s |          1.402 s |  +9.4% |
| Regex native compile    |              0.345 s |          0.410 s | +18.8% |
| Regex execution         |              0.173 s |          0.175 s |  +1.2% |
| SHA-512 execution       |              0.722 s |          0.727 s |  +0.7% |
| 16-argument execution   |              0.746 s |          0.754 s |  +1.1% |
| 32-argument execution   |              1.076 s |          1.099 s |  +2.1% |

The interpreter load moved out of ordinary arithmetic, but it was added to every existing
`call_indirect`, interpreted fallback, bulk-memory, and explicit-trap helper boundary. JSON in
particular shows that treating all of those boundaries as cold is premature. The result file is
`/private/tmp/ladybird-wasm-native-abi-cold-interpreter.json`, with SHA-256
`94c1ab2be179bd2a66166e678ee57372f395bed35d6ffb46ecf55ddb12639fe6`.

This experiment was not committed. The implementation order should instead be:

1. Give successful compiled `call_indirect` operations a native path that performs their dynamic
   table and type checks and then invokes the compatible native body directly.
2. Keep the bridge for imports, uncompiled functions, and other genuine interpreter transitions.
3. Revisit making the interpreter pointer cold once the common indirect-call path no longer needs
   it.

Moving the resume token after the Wasm parameters is also still available as an independent ABI
layout experiment, but it does not address the measured `call_indirect_with_record` hotspot.

##### Native `call_indirect` design notes

Specification references:

- <https://webassembly.github.io/spec/core/syntax/types.html#limits>
- <https://webassembly.github.io/spec/core/exec/runtime.html#table-instances>
- <https://webassembly.github.io/spec/core/exec/instructions.html#exec-table-grow>
- <https://webassembly.github.io/spec/core/appendix/implementation.html#execution>

The local specification sources establish the storage constraints that the implementation must
preserve. `spec/document/core/syntax/types.rst` says, "If no maximum is present, then the
respective storage can grow to any valid size." The `table.grow` execution note says that failure
must occur when a declared maximum would be exceeded, but can occur in other cases depending on
the resources available to the embedder. The implementation-limit appendix explicitly permits a
runtime restriction on "the size of a table instance" and does not require that restriction to be
a concrete, fixed number. The reference validator accepts at most `2^32 - 1` entries for an i32
table and `2^64 - 1` entries for an i64 table.

Ladybird already chooses a much smaller implementation limit:
`Constants::max_allowed_table_size` is 1,048,576 entries. Initial table allocation rejects a
declared minimum above that value. `TableInstance::grow()` currently checks the table's declared
maximum and the u32 limit, but does not enforce `max_allowed_table_size`; consequently a table
whose initial size was accepted can grow past Ladybird's stated implementation limit. Stable
table storage should first make allocation and growth use the same effective capacity. It must
also avoid baking in the interpreter's current u32-only table operation handling, because the
type parser and validator accept table64 limits.

There are two independent relocation hazards in the current runtime representation:

1. `Store::m_tables` is a `Vector<TableInstance>`, so allocating another table can relocate the
   `TableInstance` objects. Memories and globals already avoid the analogous problem by storing
   owned pointers.
2. Each `TableInstance` has a `Vector<Reference>` and a parallel
   `Vector<RefPtr<ModuleInstance const>>`. `table.grow` can relocate either backing buffer. The
   second vector pins the defining module instance for function references, so stabilizing only
   the reference vector would be incomplete.

The 1,048,576-entry implementation limit makes ValueStack-like stable backing practical without
reserving the format's theoretical i32 or i64 maximum. The effective capacity can be the smaller
of a declared maximum and Ladybird's implementation limit, or the implementation limit when no
smaller maximum is declared. Merely using a fixed-capacity raw `ValueStack` is not sufficient:
`Reference` and `RefPtr` have construction, destruction, and ownership semantics. A table-specific
storage type can reserve stable capacity while constructing only live slots.

An initial implementation reserved the effective capacity in both existing vectors. It was
discarded before commit: `Vector` still permits operations that replace its data pointer, so base
stability would have remained a convention rather than an invariant of the type. It also asks the
allocator for the complete backing allocation at table creation. The replacement is a non-copyable,
non-movable `TableInstance::Storage`. It uses `GC::PrimitiveStorage` to reserve fixed virtual ranges
for references and module anchors, caches their base pointers once, commits pages only while
growing, placement-constructs only live entries, and destroys them before releasing the ranges.
The storage API has no operation that can increase capacity or replace either base pointer.

The stable-storage checkpoint also makes the existing implementation limit consistent:
allocation and growth both use the smaller of the declared maximum and
`max_allowed_table_size`. The low-level growth API accepts a u64 count so the storage does not add
another table64 restriction, even though the current bytecode table instructions still narrow
their operands to u32. Tests retain a `Reference*` across growth and check that the table size and
base are unchanged after growth past the implementation limit is rejected.

The focused table, memory, native execution, and specification suites passed. Three uncached
d3wasm instantiations averaged 3.245423 seconds of native compilation and 4.496715 seconds for
parsing, validation, native compilation, and instantiation. The previously recorded typed-ABI
checkpoint averaged 3.221210 and 4.499370 seconds respectively, so the storage change is neutral
within this small sample. It does not alter generated code or execution speed.

Stable backing is not strictly required if every compiled indirect call reloads a possibly changed
vector data pointer from a stable table object. It becomes valuable when generated code is to keep
a fixed table base or use a compact resolved-entry array without updating compiled metadata after
every `table.grow`. Table mutations still have to update whatever resolved representation the JIT
reads for `table.set`, `table.fill`, `table.copy`, and `table.init`.

The function reference itself currently does not provide the metadata needed by an efficient
native indirect call. `Reference::Func` stores a `FunctionAddress` and a
`RefPtr<Module const> source_module`. The source-module field is packed into and reconstructed from
the high half of a 128-bit `Value`, but is not otherwise used to select or invoke the target.
Dispatch looks up the function address in `Store`, inspects the `FunctionInstance` variant, checks
its canonical defined type, rediscovers its module instance, and enters the bridge. The table's
separate module anchor provides the strong lifetime pin.

A JIT-friendly function-table entry therefore needs a stable callable descriptor, or equivalent
stable resolved fields, that can provide:

- the canonical defined type used for the `call_indirect` signature check;
- whether the function is Wasm, host, interpreted, or has a published native body;
- the callee's module/configuration context, including cross-module table references;
- the native body entry that skips the interpreter adapter; and
- the existing interpreter/host fallback information.

The existing per-module `compiled_fn_table` is not by itself sufficient because a table may contain
a function imported from another module. Reusing the currently redundant `source_module` half of
`Reference::Func` for a descriptor pointer may avoid growing the 128-bit `Value`, but that requires
a complete lifetime, tagging, GC, and cross-module audit first. A literal C++ virtual interface is
not required; a compact data descriptor keeps the successful path as loads, checks, and one typed
indirect native call.

The successful native path must retain the WebAssembly `call_indirect` semantics in this order:

1. Check the table index against the current table size and trap when it is out of bounds.
2. Load the current table element and trap when it is null.
3. Check the element's canonical function type against the instruction's expected type and trap
   on a mismatch.
4. When a compatible native body is available, establish the callee's context and invoke it with
   the existing typed ABI: values through the platform ABI for signatures of at most eight
   parameters and the existing call-record pointer for larger signatures.
5. Otherwise enter the existing host/interpreter machinery without changing tiering behavior.

The non-local native trap escape remains part of the ABI. A successful call must not return a trap
status on its hot path. Explicit `call_indirect` failures raise the same traps as the existing
bridge, while traps raised inside a native callee continue to escape through the installed recovery
context. Calling an uncompiled function remains an ordinary compiled-to-interpreter transition;
after it is compiled and safely published, later table calls may select its native body.

A checkpointed implementation sequence is:

1. Give `TableInstance` stable ownership in `Store`, then test allocation of multiple tables and
   references retained across allocations.
2. Introduce stable, capped table-element storage; enforce the same implementation capacity during
   allocation and growth; preserve reference/module-anchor lifetime; and test every table mutation.
3. Introduce stable callable metadata with canonical type, callee context, native entry, and
   fallback state. Cover same-module, imported, cross-module, host, uncompiled, and subsequently
   tiered-up functions.
4. Lower `call_indirect` and `call_indirect_with_record` to the bounds/null/type checks followed by
   a typed native indirect call when the descriptor permits it. Keep the existing bridge only as
   the fallback.
5. Test out-of-bounds, null, type-mismatch, explicit and callee traps, table mutation after
   compilation, cache relocation, and interpreter-to-native and native-to-interpreter transitions.
6. Re-measure the representative Wasm suite and d3wasm before revisiting the cold
   `BytecodeInterpreter*` ABI experiment.

This is analogous to the cost shape of C++ virtual dispatch: stable metadata supplies a function
pointer for an indirect branch, while a statically known direct call can avoid the lookup and may
be inlined. WebAssembly adds bounds, null, canonical-signature, context, and tier-state checks, so
the goal is not to make `call_indirect` identical to a direct `call`; it is to reduce the successful
compiled case to the necessary checks and one well-predicted native indirect branch.

#### First native `call_indirect` checkpoint

The first lowering checkpoint handles numeric `synthetic_call_indirect_with_record` instructions.
Generated code performs the table bounds, null, and canonical defined-type checks directly. Exact
canonical type pointers take the common path; only subtype-compatible non-identical types enter a
cold C++ type helper. A same-module Wasm target atomically loads its published native body and calls
it with the existing typed or high-arity call-record ABI. Host, cross-module, and not-yet-compiled
targets retain the existing bridge fallback, so interpreter tiering behavior is unchanged. The
bridge element index is now u64, preserving table64 indices instead of truncating them to i32.

The focused fixture covers i32, i64, f32, and f64 results; the nine-argument call-record ABI; a
same-module interpreted fallback; void calls; native callee traps; bounds, null, and type-mismatch
traps; and replacing a table element after compilation. All eight `TestWasmExecution` cases pass,
as do `TestWasmTable` and `TestWasmMemory`.

Three representative iterations measured:

| Workload | Total or score | Native compile |  Execute | Change from promoted-parameter checkpoint |
| -------- | -------------: | -------------: | -------: | ----------------------------------------: |
| CoreMark |      24821.027 |        0.010 s | 17.868 s |                               score -0.1% |
| Base64   |        0.450 s |        0.030 s |  0.389 s |                           execution +2.9% |
| JSON     |        1.403 s |        0.056 s |  1.309 s |                           execution +2.1% |
| Regex    |        0.708 s |        0.370 s |  0.176 s |                           execution +1.7% |
| SHA-512  |        0.783 s |        0.029 s |  0.721 s |                           execution -0.1% |

These small differences are neutral at three samples. The suite has no isolated indirect-call
microbenchmark, and d3wasm has not yet been measured for this checkpoint. Blake3 remained dominated
by interpreted SIMD and is omitted from the comparison. The complete result is
`/private/tmp/ladybird-wasm-native-indirect.json`, with SHA-256
`3071c729816287c4044e1938ee4ade28a6c116dd844f754581a4384be89af04c`.

#### Indirect-call correctness follow-ups

Two correctness checkpoints followed before extending native lowering to raw indirect calls:

1. `d9a14207287` keeps `cranelift_indirect_calls` metadata aligned while serializing bytecode.
   `br_table` inserts synthetic continuation instructions into the flattened Cranelift input, so
   applying original bytecode instruction indexes to that expanded vector can attach the next
   indirect call's arity and type encoding to the wrong instruction. Metadata is now consumed at
   the corresponding original dispatch before any continuations are appended. The fixture places
   `call_indirect` after a large `br_table` and also covers mixed integer and floating-point
   signatures.
2. `280c1e1184b` makes `CompiledCallerContext` restore the caller's table-instance array and
   canonical-type table as well as its existing module, compiled-function, expression, memory,
   global, and locals context. A cross-module interpreted fallback changes those fields to the
   callee's module; failing to restore them leaves subsequent generated indirect-call checks using
   incompatible module state. A two-module regression calls an imported provider through the
   fallback and then resumes compiled execution in the caller.

The canonical-type table has a named `CanonicalTypeTable` alias rather than spelling the
`DefinedType const* const*` representation at every use. The first `const` protects each pointed-to
`DefinedType`; the second protects the pointers stored in the table. The table itself remains a
copyable pointer value so caller context can save and restore it.

#### Raw native `call_indirect`

`cfaa7c35724` extends the native indirect-call lowering from
`synthetic_call_indirect_with_record` to raw `CALL_INDIRECT`. A raw call is not a different Wasm
operation: it is the bytecode representation used when overlapping call lifetimes prevent the
shared call record from representing every call. Nested indirect calls provide the focused case:
the inner call owns the record and the outer call remains raw.

For a decoded numeric signature with at most eight parameters and at most one result, the raw path
now performs the same table bounds, null-reference, canonical-type, same-module, and published
native-entry checks as the call-record path. On success it makes a typed native indirect call and
places the result back in the vstack representation. Cross-module, host, uncompiled, unsupported,
and multi-result cases continue through `wasm_cl_call_indirect`. This preserves interpreter
tiering: a target without a published native body is interpreted, while later calls may use the
body after its release publication.

The focused fixture covers raw i32, i64, f32, and f64 results, a void call, an uncompiled fallback,
and nested call-record/raw lifetimes. The raw-call microbenchmark performs 50 million nested
two-argument indirect calls. Against `280c1e1184b`, three execution samples measured:

| Configuration            | Mean execution | Standard deviation | Change |
| ------------------------ | -------------: | -----------------: | -----: |
| Bridge raw indirect call |    12.037357 s |         0.074569 s |      — |
| Native raw indirect call |     0.354379 s |         0.004832 s | -97.1% |

That is a 33.97-times speedup for the isolated raw indirect-call loop. Native compilation remained
in the low milliseconds and is not included in the execution figures. A three-sample
representative-suite comparison was much less conclusive: CoreMark's reported score improved by
0.5%, while total process times moved by +0.6% for Base64, +2.1% for JSON, +1.4% for Regex, and
+0.8% for SHA-512. Blake3 moved by +2.3% but remains dominated by interpreted SIMD. Those totals
combine startup, compilation, and execution and should be treated as neutral/noisy rather than as
generated-code regressions.

Raw representative results:

- `/private/tmp/ladybird-native-raw-indirect-baseline.json`
    - SHA-256: `dc0469df5761c78b40230dc971173cac52589a7dfe9989dccf330d21d1627de4`
- `/private/tmp/ladybird-native-raw-indirect-current.json`
    - SHA-256: `80d2b500888ccdc1ab8572d791b0a15f1fbcd63629884014eb5786c4198b66f5`

#### Argument-suffix materialization

The first raw native implementation materialized the complete live vstack before deciding whether
the call would use the native or bridge edge. `e1bcb843477` makes those edges asymmetric:

- the native raw-indirect edge reads each typed argument directly from its Cranelift vstack value
  and does not write the Wasm `ValueStack`;
- the bridge fallback writes exactly the top `parameter_count` values, including their payload and
  zero tag, advances the real stack top by exactly that suffix, and restores the saved top after
  the helper returns.

The saved top is safe because `ValueStack` storage cannot move while an activation is live.
Unrelated values below the argument suffix therefore remain in SSA/native storage across the
boundary. The fallback regression keeps such a lower value live and uses it after an interpreted
indirect call.

The AArch64 dump for the two-argument microbenchmark confirms the distinction. The native edge at
offsets `0x8b4` through `0x8cc` loads its arguments from native state and executes `blr` without a
Wasm-stack store. The fallback edge at `0x84c` through `0x86c` writes exactly two payload/tag pairs
and advances the top by `0x20` bytes.

Three samples comparing the initially materialized native implementation with the suffix-only
implementation measured:

| Configuration                   | Mean execution | Standard deviation | Change |
| ------------------------------- | -------------: | -----------------: | -----: |
| Complete-vstack materialization |     0.355524 s |         0.009330 s |      — |
| No native-edge materialization  |     0.334752 s |         0.004147 s |  -5.8% |

`4ea8f166980` applies the same contract to raw direct-call fallback. Register-ABI direct calls
already consume their vstack values without materializing them; a raw call that must use
`wasm_cl_call_function`, including an overlapping high-arity call, now writes only its known
argument suffix instead of every live vstack slot. This made the old whole-vstack materialization
macro unused, so the checkpoint removes it.

The direct-call regression gives an inner nine-argument call the shared call record, leaving an
outer nine-argument call raw, and keeps an unrelated value live below the outer arguments for use
after the call. Its initial timing was neutral: the high-arity bridge work dominates this small
store reduction, and the available paired samples were too noisy to claim a runtime change. The
change is retained because the stack ABI consumes only the argument suffix and the regression
proves that lower live values need not be materialized.

Cache blob format versions advanced from 20 through 23 across the metadata, raw-indirect, and
materialization checkpoints so older generated code cannot be installed with the changed lowering
and layout assumptions. After `4ea8f166980`, Rust formatting and type checking pass, repository
lint passes, and all nine `TestWasmExecution` cases pass outside the command sandbox.

### Unconditional local SSA and the remaining Wasmtime gap

An uncommitted experiment removed the compact native-local payload array and represented every
accessed numeric local as a Cranelift frontend `Variable`. `FunctionBuilder` then constructed SSA
block parameters at control-flow merges and Cranelift chose any required spill slots. This tested
the upper bound of removing explicit local homes; it was not intended to become a checkpoint
without representative measurements.

The comparison used two compiler executables against the same named
`build-wasm-full/d3wasm.wasm` module:

- an exact-HEAD compiler containing the compact native-local implementation; and
- the uncommitted all-SSA compiler containing no explicit native local homes.

The native dumps for `unzReadCurrentFile`, Wasm function 1574, were produced with:

```sh
LADYBIRD_CRANELIFT_COMPILER=/path/to/native-local/cranelift-compiler \
    Build/release/bin/wasm \
    --dump-native \
    --print-function 1574 \
    --export-noop \
    /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm

LADYBIRD_CRANELIFT_COMPILER=/path/to/all-ssa/cranelift-compiler \
    Build/release/bin/wasm \
    --dump-native \
    --print-function 1574 \
    --export-noop \
    /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm
```

These commands must run outside the restricted command sandbox so the CLI can create its shared
mapping and spawn the selected Cranelift compiler. A sandboxed run silently leaves the function
uncompiled and produces an empty native dump.

Wasmtime 35 used Cranelift 0.122.0, the same backend version as the Ladybird comparison. Its dump
was produced with:

```sh
/Users/lukewilde/.wasmtime/bin/wasmtime compile \
    -C compiler=cranelift \
    -C cache=n \
    -C parallel-compilation=n \
    -O opt-level=2 \
    -o /tmp/d3wasm-wasmtime35.cwasm \
    /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm

/Users/lukewilde/.wasmtime/bin/wasmtime objdump \
    --addresses \
    --funcs wasm \
    --filter unzReadCurrentFile \
    /tmp/d3wasm-wasmtime35.cwasm
```

#### Loading significance

Loading run 1 of `/Users/lukewilde/Documents/d3wasm_8.trace` recorded 9.231 seconds of main-thread
CPU over 11.193 seconds elapsed. Samples whose leaf PC was inside the known
`unzReadCurrentFile()` JIT mapping totalled 1.994 seconds, or 21.6% of the recorded main-thread
work. That recording predates the matched native-local/all-SSA comparison, so it establishes the
function's importance but is not a paired runtime measurement of the two lowerings.

The function contains much more than the small wrapper visible in the C++ source. LLVM inlined a
large part of zlib's inflate implementation into it: its Wasm body is 13,654 bytes, has three
parameters, 46 additional `i32` locals, two `i64` locals, and direct calls to `call_zseek64`,
`crc32_z`, `adler32`, and `inflate_table`. This makes it both representative of loading and a
high-pressure counterexample to the small `AddActiveInteraction()` silhouette loop.

#### Native-code comparison

The complete generated objects measured:

| Lowering                         | Native code | Instructions | Memory operations | Stack-pointer accesses | Branches | Stack frame |
| -------------------------------- | ----------: | -----------: | ----------------: | ---------------------: | -------: | ----------: |
| Ladybird compact native locals   |    40,120 B |       10,023 |             4,774 |                    941 |      760 |       784 B |
| Ladybird unconditional local SSA |    43,656 B |       10,910 |             4,773 |                  3,467 |      806 |       928 B |
| Wasmtime 35                      |    21,696 B |        5,424 |             1,858 |                  1,041 |      537 |       288 B |

The Ladybird totals include its 96-byte interpreter-facing adapter and four per-target interpreter
fallback bodies. Those pieces occupy 792 bytes in either Ladybird object. Excluding them leaves a
39,328-byte native-local body and a 42,864-byte all-SSA body, still respectively 1.81 and 1.98
times Wasmtime's complete function.

The native-local body contains 2,222 accesses through the stable local-base register and 941
ordinary stack-pointer accesses. All SSA removes the former but increases stack-pointer accesses
to 3,467, an additional 2,526 backend stack operations. Total memory traffic is consequently
unchanged to within one instruction. Code size and instruction count both regress by 8.8%, the
branch count regresses by 6.1%, and the fixed frame grows by 18.4%. Cranelift has not lost track of
the values; it has been given a graph whose simultaneous live ranges make spilling more expensive
than the explicit homes.

This is the opposite of the matched silhouette-edge loop in `AddActiveInteraction()`. In the
current native-local build that loop has 91 instructions and 49 memory operations, including 31
native-local accesses, five backend stack accesses, and thirteen linear-memory accesses. The
all-SSA form has 62 instructions and 24 memory operations: it removes all 31 local-home accesses,
adds six backend stack accesses, and leaves the thirteen required linear-memory operations
unchanged. Its short path falls from 29 instructions and fourteen memory operations to seventeen
and three; its full path falls from 89 and 48 to 59 and 23. Wasmtime's previously measured form is
53 instructions and twenty memory operations. Local SSA is therefore close to Wasmtime for this
bounded low-pressure loop even though it is poor for the complete function and for
`unzReadCurrentFile()`.

SHA-512 provides another high-pressure result with an isolated execution measurement. Three
alternating post-warmup samples measured:

| SHA-512 lowering                | Mean compilation | Mean execution |
| ------------------------------- | ---------------: | -------------: |
| Compact native locals           |       0.030513 s |     0.477956 s |
| Unconditional function-wide SSA |       0.055266 s |     0.632384 s |

The all-SSA form regresses compilation by 81.1% and execution by 32.3%. In its fully unrolled
compression loop, static instructions rise from 4,750 to 6,948 and memory operations from 1,169
to 2,169. The fixed frame grows from 1,184 to 3,424 bytes and the number of distinct backend stack
offsets rises from 111 to 419. A 123-byte SHA-512 input requires two compression blocks, so the
benchmark's 999,999 digests execute approximately 1,999,998 such iterations; the static delta
therefore represents roughly 4.4 billion additional dynamic instructions and two billion
additional memory operations.

#### Optimized Cranelift IR comparison

To distinguish frontend lowering from backend register allocation, the Ladybird compiler was
temporarily instrumented to print `context.func.display()` immediately after
`Context::compile()`. The instrumentation existed only in a temporary compiler workspace and is
not part of the branch. Wasmtime's corresponding optimized CLIF was captured with:

```sh
/Users/lukewilde/.wasmtime/bin/wasmtime compile \
    -C compiler=cranelift \
    -C cache=n \
    -C parallel-compilation=n \
    -O opt-level=2 \
    --emit-clif /tmp/d3wasm-wasmtime35-clif \
    -o /tmp/d3wasm-wasmtime35-clif.cwasm \
    /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm
```

The optimized native bodies supplied to the same Cranelift backend measured:

| Optimized CLIF metric            | Native locals | All local SSA | Wasmtime 35 |
| -------------------------------- | ------------: | ------------: | ----------: |
| Blocks                           |           914 |           914 |         761 |
| IR instructions                  |        11,810 |         9,231 |       5,627 |
| Memory-like operations           |         3,777 |         1,250 |         812 |
| Calls                            |            40 |            40 |          31 |
| Total block parameters           |         2,855 |        11,618 |       1,432 |
| Blocks with parameters           |           350 |           358 |         195 |
| Maximum parameters on one block  |            17 |            56 |          30 |
| Integer reduce/extend operations |         2,842 |         2,468 |         552 |

This separates the two Ladybird failure modes:

1. Compact local homes make frontend memory traffic explicit. They add 2,527 memory-like CLIF
   operations relative to all SSA, and Cranelift cannot reconstruct SSA values after those loads
   and stores have been requested.
2. Unconditional local SSA makes edge pressure explicit. It increases the total number of block
   parameters from 2,855 to 11,618. Many blocks carry thirty to fifty-six parameters, while
   Wasmtime has 1,432 parameters over the complete function. The resulting backend spill traffic
   replaces the frontend local traffic almost one for one in native code.

This is not principally an old-Cranelift or register-allocator problem. Before instruction
selection, Ladybird's all-SSA IR is already 64% larger than Wasmtime's and contains over eight
times as many block parameters. The shared per-function synthetic-tier-up resume dispatch and the
custom bytecode CFG may amplify this pressure by adding cold predecessors to loop headers, but the
measurement does not yet apportion the excess parameters between those causes.

#### Integer-width erasure

Ladybird currently represents the integer vstack as `I64`. It declares every integer stack
variable as `types::I64`, and its local representation distinguishes `f32` and `f64` but uses
`types::I64` for both Wasm `i32` and `i64`. `unzReadCurrentFile()` is overwhelmingly an `i32`
function, so loads, local mutations, branch conditions, and address calculations repeatedly cross
between an `I64` payload representation and real `i32` operations.

In the optimized CLIF this produces 1,312 integer reductions, 1,049 zero extensions, and 481 sign
extensions with native local homes. All SSA still contains 986 reductions, 850 zero extensions,
and 632 sign extensions. Wasmtime contains four reductions, 548 zero extensions, and no sign
extensions. Some extensions are required for 64-bit host addresses and cannot be removed, but the
approximately 1,900 to 2,300 excess conversions show that exact-width integer state is a separate
large frontend opportunity. Removing local homes alone does not address it.

#### Exact-width `i32` bank experiment

The first exact-width experiment was implemented on top of unconditional local SSA. Accessed
`i32` locals are declared as `types::I32`, and native virtual registers and vstack positions have
an `I32` bank alongside the existing `I64`, `F32`, and `F64` banks. `i32` arithmetic,
comparisons, conditions, loads, stores, conversions, typed calls, and the local synthetic
instructions consume and produce `I32` values directly.

The `I64` payload bank remains defined for every virtual register and vstack position. An `i32`
write also defines that payload with a zero extension, which Cranelift removes when native code
does not need it. This is required by the current conservative control-flow implementation:
ordinary merge points invalidate typed-bank metadata and use the payload definition common to
all incoming edges. It also supplies the representation needed when a value reaches the
interpreter `ValueStack`, a call record, register synchronization, or an opaque helper. Carrying
typed bank information on individual edges is a later refinement; this experiment does not make
an `I32` definition available on an edge where none was supplied.

Raw `call_indirect` now consumes its table element through the same typed source path. The broader
LibWasm suite exposed that the old stack-element special case decremented virtual depth but popped
the real `ValueStack`; an element index of zero passed accidentally, while the multi-result test's
index of one called table element zero and trapped on the resulting type mismatch. The typed read
uses the current virtual `I32` value when present and falls back to the canonical stack only when
the operand is genuinely there. Globals remain payload typed because the current compiler input
does not include global value types.

The generated object for `unzReadCurrentFile()` changed as follows relative to the same all-SSA
compiler without the `I32` bank:

| `unzReadCurrentFile()` metric | Untyped all SSA | `I32` bank | Change |
| ----------------------------- | --------------: | ---------: | -----: |
| Native code                   |        43,656 B |   41,544 B |  -4.8% |
| Instructions                  |          10,910 |     10,382 |  -4.8% |
| Memory operations             |           4,773 |      4,684 |  -1.9% |
| Stack-pointer accesses        |           3,467 |      3,378 |  -2.6% |
| Branches                      |             806 |        808 |  +0.2% |
| Fixed stack frame             |           928 B |      944 B |  +1.7% |

An optimized-CLIF dump from a temporary instrumented compiler explains the code-size change:

| Optimized `unzReadCurrentFile()` CLIF metric | Untyped all SSA | `I32` bank |
| -------------------------------------------- | --------------: | ---------: |
| Blocks                                       |             914 |        914 |
| IR instructions                              |           9,231 |      7,793 |
| Memory-like operations                       |           1,250 |      1,250 |
| Calls                                        |              40 |         40 |
| Total block parameters                       |          11,618 |     11,558 |
| Blocks with parameters                       |             358 |        358 |
| Maximum parameters on one block              |              56 |         56 |
| Integer reduce/extend operations             |           2,468 |        998 |

The bank removes 1,438 optimized IR instructions and 1,470 integer width conversions before
instruction selection. Reductions fall from 986 to 78 and sign extensions from 632 to zero;
zero extensions rise from 850 to 920 because the conservative `I64` shadows remain available at
merges and representation boundaries. Memory-like IR operations and the high block-parameter
count do not change, confirming that this experiment fixes width erasure without fixing all-SSA
edge pressure.

Exact widths recover a meaningful part of the static all-SSA regression, but do not solve its
edge pressure. Relative to compact native locals, the typed all-SSA function still has 3.6% more
instructions, over three times as many stack-pointer accesses, and a 20.4% larger frame. The
remaining high-pressure problem therefore still requires regional SSA or pressure-aware homes.

For `AddActiveInteraction()`, the complete all-SSA object shrank from 261,100 to 259,964 bytes,
from 65,229 to 64,942 instructions, and from 36,441 to 36,348 memory operations. The whole
function is large enough that those changes are only 0.3-0.4%. The hot silhouette backedge is a
stronger result: using the exact backedge-delimited range in the paired Ladybird dumps, it falls
from 62 to 55 instructions while retaining the same twelve required linear-memory operations and
four backend spill stores. The removed per-iteration work is width repair and related register
movement, not memory traffic moved out of the loop.

SHA-512 is predominantly `i64`, so it is a guardrail rather than a target for this experiment.
Its hot function changes by only 44 bytes, eleven instructions, nine memory operations, and 16
bytes of frame size. Four alternating fresh-compilation runs measured:

| SHA-512 phase      | Untyped all SSA | `I32` bank | Change |
| ------------------ | --------------: | ---------: | -----: |
| Native compilation |      0.052910 s | 0.051721 s |  -2.2% |
| Execution          |      0.636050 s | 0.625038 s |  -1.7% |

The execution result is modest but, importantly, does not reproduce the previous high-pressure
cliff. Four alternating d3wasm compilation runs were noisier: mean native compilation changed
from 3.9773 to 4.0778 seconds (+2.5%), while the median changed from 3.9661 to 3.9984 seconds
(+0.8%). One `I32`-bank sample took 4.3425 seconds; the other three were between 3.9719 and
4.0172 seconds. This is best treated as neutral to a possible small compilation regression until
more samples are collected, separately from the demonstrated execution-code improvement.

The focused execution fixture also covers high-bit `i32` values through select, an `if` merge,
logical shift, sign extension, and typed locals. Rust formatting, checking, and unit tests pass,
and all ten `TestWasmExecution` cases pass with native compilation enabled outside the sandbox.
The broader LibWasm harness passes all fifteen suites and 21 enabled tests; 38 specification-parser
tests remain skipped as before.

#### `d3wasm_10` runtime validation

`/Users/lukewilde/Documents/d3wasm_10.trace` records the exact-width compiler in a live
d3wasm process. The loaded cache is format version 26, contains all 6,482 compiled functions, and
has the same SHA-256 hash as `build-wasm-full/d3wasm.wasm`. Matching live executable mappings
against that cache resolved `unzReadCurrentFile()`, `AddActiveInteraction()`, and the other known
renderer functions unambiguously. The cache file predates run 1 by approximately 31 seconds, so
this is cached native-code execution rather than a native-compilation-time measurement.

Run 1 is the loading capture. Its trace recording lasts 8.483 seconds and its CPU sample window
lasts 8.139 seconds. Native Wasm accounts for 74.62% of main-thread cycles;
`unzReadCurrentFile()` alone accounts for 21.71%. This confirms the current approximately
eight-second load and that decompression remains the largest resolved loading function. The
reported change from approximately ten to eight seconds is encouraging, but the trace contains
only the post-change run and therefore cannot by itself establish a controlled two-second delta.

Run 2 uses the heavy opening-area scene defined in the earlier visual-frame section. A trace does
not encode camera position or input, so equivalence of the firing pattern remains part of the
manual test procedure rather than something Instruments can verify. Compared with the earlier
run 4 capture, its visual-frame signposts measure:

| Rendering-update metric | Earlier run 4 | `d3wasm_10` run 2 | Change |
| ----------------------- | ------------: | -----------------: | -----: |
| Complete updates        |         1,319 |              1,617 |        |
| Signpost span           |      21.025 s |           20.660 s |        |
| Mean update-start rate  |       62.73/s |            78.27/s | +24.8% |
| Mean work               |     15.802 ms |          12.650 ms | -19.9% |
| Median work             |     15.790 ms |          13.363 ms | -15.4% |
| 90th percentile         |     17.968 ms |          14.750 ms | -17.9% |
| 95th percentile         |     18.648 ms |          15.662 ms | -16.0% |
| 99th percentile         |     20.662 ms |          17.168 ms | -16.9% |
| Maximum                 |     36.729 ms |          46.050 ms | +25.4% |
| Work over 8.333 ms      |         99.8% |              87.6% |        |
| Work over 16.667 ms     |         27.7% |               2.2% |        |

The maximum is a single worse outlier, but the median and the complete 90th through 99th
percentile range improve substantially. High Frequency profiling still adds overhead, so the
signposts should be read as a controlled profiled comparison rather than absolute unprofiled FPS.

The main thread remains saturated in both captures: the matched CPU windows contain 57.535 and
56.804 billion main-thread cycle-weight units over 20.587 and 20.491 seconds respectively. It is
doing more frames with nearly the same total CPU budget. Normalizing the CPU categories by each
capture's update-start rate gives the following estimates:

| Estimated million cycle-weight units/update | Earlier run 4 | `d3wasm_10` run 2 | Change |
| ------------------------------------------- | ------------: | -----------------: | -----: |
| All main-thread work                        |        44.549 |             35.420 | -20.5% |
| Wasm JIT                                    |        25.687 |             17.490 | -31.9% |
| JavaScript                                  |         7.355 |              7.109 |  -3.3% |
| LibWeb                                      |         5.038 |              4.714 |  -6.4% |
| Other/system                                |         3.920 |              3.574 |  -8.8% |
| Wasm runtime                                |         2.486 |              2.476 |  -0.4% |
| Wasm interpreter                            |         0.058 |              0.057 |  -2.1% |

The estimated per-update saving is therefore overwhelmingly inside generated Wasm rather than
the interpreter, runtime helpers, JavaScript, or LibWeb. The resolved function shares agree:
rate-normalized cycles per update fall by approximately 43% in `AddActiveInteraction()`, 39% in
`R_CreateLightTris()`, 39% in `R_CullLocalBox()`, 26% in `CmpLT()`, and 44% in
`idVertexCache::Position()`. Trip counts can vary between manually played captures, so individual
function percentages are directional; the broad category and frame-time changes are stronger
evidence.

This comparison measures the cumulative current compiler against the older run 4 compiler. It
includes the native-call, canonical-local, all-SSA, Cranelift-version, and exact-width work and
must not be presented as an isolated `i32`-bank result. The user's immediately adjacent ten- to
eight-second and approximately 70-80 FPS observations are the available before/after indication
for the bank itself; retaining a pre-bank all-SSA compiler for an alternating trace pair would be
needed for strict attribution.

#### No-synthetic-tier-up experiment

A temporary experiment disabled insertion of every `synthetic_tier_up` instruction by setting
`tier_up_instruction_threshold` to `NumericLimits<size_t>::max()`. This disables checkpoint
generation in the bytecode construction step rather than merely ignoring checkpoints in the
Cranelift lowering. It is an experiment, not a proposed permanent configuration.

The structural effect is substantial. Removing the synthetic resume predecessors reduces both
the block-parameter graph supplied to Cranelift and the final native object:

| Function and metric                            | Tier-up enabled | Tier-up disabled | Change |
| ---------------------------------------------- | --------------: | ---------------: | -----: |
| `unzReadCurrentFile()` optimized blocks        |             914 |              875 |  -4.3% |
| `unzReadCurrentFile()` block parameters        |          11,558 |            9,171 | -20.7% |
| `unzReadCurrentFile()` local parameters        |           9,081 |            6,994 | -23.0% |
| `unzReadCurrentFile()` complete native code    |        41,544 B |         38,472 B |  -7.4% |
| `AddActiveInteraction()` optimized blocks      |           2,337 |            2,283 |  -2.3% |
| `AddActiveInteraction()` block parameters      |          22,072 |           17,752 | -19.6% |
| `AddActiveInteraction()` local parameters      |          18,974 |           15,116 | -20.3% |
| `AddActiveInteraction()` complete native code  |       259,964 B |        228,956 B | -11.9% |

Three immediate d3wasm native-compilation runs with tier-up enabled took 4.551154, 4.372921, and
4.214035 seconds. Three with checkpoint generation disabled took 4.072525, 4.163619, and 4.196828
seconds. The mean falls from 4.379370 to 4.144324 seconds (-5.4%) and the median from 4.372921 to
4.163619 seconds (-4.8%). CoreMark improved by 3.5% in the existing Wasm benchmark run, while the
other execution results were small, mixed, or noisy. Compilation latency is therefore a plausible
benefit, but it must be evaluated separately from steady-state generated-code execution.

`/Users/lukewilde/Documents/d3wasm_11.trace` records the disabled-checkpoint compiler. Run 1 is
loading and run 2 is the manually reproduced heavy opening-area scene. The following checks
confirm that these are valid no-synthetic-tier-up runtime captures rather than runs which reused
an old tier-enabled cache blob:

- The cache was written at 14:52:32, approximately 44 seconds before run 1 began. It is format
  version 26 and contains all 6,482 compiled functions.
- Its Wasm SHA-256 is
  `9927bc955b0455621eb47bb7755e6febff0d30f909c0832a019e4c0b119e3829`, exactly matching
  `build-wasm-full/d3wasm.wasm`.
- All 6,482 executable mappings in the still-running WebContent process correlate with the cache
  record allocation sizes after accounting for relocation-veneer capacity and two adjacent
  install-order swaps. The resolved cache entries contain the 38,472-byte
  `unzReadCurrentFile()` and 228,956-byte `AddActiveInteraction()` objects shown above, rather
  than their tier-enabled forms.
- Printing the compiled bytecode for `unzReadCurrentFile()` reports zero
  `synthetic:tier_up` instructions. The threshold change makes the same true module-wide.
- The Cranelift compiler only creates `init_locals_resume`, the resume block, and the checkpoint
  dispatch cascade when its scan finds a `SYNTHETIC_TIER_UP` instruction. That branch is not taken
  for these objects.

The native entry still contains `cmp w2, #0`. This is not residual synthetic-tier-up dispatch.
The shared native body uses `w2` to distinguish a compiled-to-compiled call from entry through the
interpreter-facing adapter. That split performs the direct-call context setup required by the
current native call ABI. A tier-enabled function has an additional resume-local initialization
path and checkpoint-target dispatch after this common entry setup; those pieces are absent from
the measured objects.

Run 1 does not show a loading execution improvement relative to `d3wasm_10` run 1:

| Loading metric                       | `d3wasm_10` | `d3wasm_11` | Change |
| ------------------------------------ | -----------: | -----------: | -----: |
| Trace duration                       |      8.483 s |      8.603 s |  +1.4% |
| CPU sample window                    |      8.139 s |      8.229 s |  +1.1% |
| Main-thread cycle-weight units       |      23.674B |      23.334B |  -1.4% |
| Native Wasm share                    |       74.62% |       74.81% |        |
| `unzReadCurrentFile()` share         |       21.71% |       22.49% |        |
| `unzReadCurrentFile()` cycle-weight  |       5.139B |       5.248B |  +2.1% |

The overall CPU total moves slightly in the favourable direction while the primary loading
function moves slightly in the unfavourable direction. These small opposing changes are best
treated as neutral measurement variation. In particular, the 7.4% smaller static
`unzReadCurrentFile()` object does not translate into less sampled work in that function.

Run 2's visual-frame markers likewise show no gameplay improvement. Measured update duration is
the time from each `WebContent Visual Frame` begin marker to its end marker. The update-start rate
uses the interval between consecutive begin markers:

| Rendering-update metric | `d3wasm_10` run 2 | `d3wasm_11` run 2 | Change |
| ----------------------- | -----------------: | -----------------: | -----: |
| Complete updates        |              1,617 |              1,585 |        |
| Signpost span           |           20.660 s |           20.823 s |        |
| Mean update-start rate  |            78.27/s |            76.07/s |  -2.8% |
| Mean work               |          12.650 ms |          13.020 ms |  +2.9% |
| Median work             |          13.363 ms |          13.574 ms |  +1.6% |
| 90th percentile         |          14.750 ms |          15.295 ms |  +3.7% |
| 95th percentile         |          15.662 ms |          16.244 ms |  +3.7% |
| 99th percentile         |          17.168 ms |          18.628 ms |  +8.5% |
| Maximum                 |          46.050 ms |          42.109 ms |  -8.6% |
| Work over 8.333 ms      |              87.6% |              90.8% |        |
| Work over 16.667 ms     |               2.2% |               3.2% |        |

The lower isolated maximum does not offset the small regression throughout the median and 90th
through 99th percentile range. Because the camera position and firing pattern are reproduced
manually, this is not a deterministic paired benchmark. It is nevertheless inconsistent with a
material gameplay improvement.

Normalizing the high-frequency CPU samples by the measured update-start rate separates generated
Wasm execution from other changes in the capture:

| Estimated million cycle-weight units/update | `d3wasm_10` run 2 | `d3wasm_11` run 2 | Change |
| ------------------------------------------- | -----------------: | -----------------: | -----: |
| All main-thread work                        |             35.418 |             35.741 |  +0.9% |
| Wasm JIT                                    |             17.489 |             17.474 |  -0.1% |
| JavaScript                                  |              7.109 |              6.963 |  -2.1% |
| LibWeb                                      |              4.714 |              4.812 |  +2.1% |
| Other/system                                |              3.574 |              3.824 |  +7.0% |
| Wasm runtime                                |              2.476 |              2.616 |  +5.7% |
| Wasm interpreter                            |              0.057 |              0.054 |  -5.3% |

Generated Wasm work per update is effectively identical. Resolved functions also move in both
directions: `AddActiveInteraction()` falls from 5.88% to 5.45% of main-thread cycles, while
`R_CreateLightTris()` rises from 2.31% to 2.73%; `R_CullLocalBox()` remains approximately 1.8%.
That mixture is consistent with manually varying renderer trip counts rather than a broad lowering
improvement.

Disabling synthetic tier-up therefore removes substantial cold resume/dispatch structure,
reduces Cranelift graph size, shrinks large native objects, and improves the measured d3wasm native
compilation time. It does not reduce loading work in `unzReadCurrentFile()` or steady-state native
Wasm work per rendered update. The experiment should not be retained as a d3wasm runtime
optimization. Faster native readiness remains a useful separate workstream, but it does not
justify removing interpreter-to-native tiering or treating static cold-path removal as a hot-code
execution win.

#### Cranelift 0.134.3 update

The Cranelift backend was authored on 22 April 2026 and landed on `master` as `a33e14833980` on
10 May 2026. Its initial Cargo manifest selected Cranelift 0.116, whose 0.116.0 and 0.116.1
releases date to 20 and 21 January 2025. The backend therefore started approximately fifteen
months behind the released Cranelift series. This branch had already moved to 0.122.0 for the
matched Wasmtime 35 comparison above.

The backend was updated from 0.122.0 to the released Cranelift 0.134.3 family, matching the
Cranelift version shipped by the locally installed Wasmtime 47.0.3. The source adaptations are
API migrations rather than lowering-policy changes:

- `MemFlagsData` is imported under the existing local `MemFlags` name, preserving every memory
  operation's previous flags;
- `stack_store` is given the target pointer type;
- imported functions explicitly use `patchable: false`;
- `FunctionBuilder::finalize` receives `isa.frontend_config()`; and
- deprecated immediate builders are replaced by their explicit-extension equivalents. Addition,
  multiplication, and comparison use the old sign-extending semantics; shifts use the old
  zero-extending semantics.

The compiled-code cache format advances from 26 to 27. The cache header otherwise keys code on
the Wasm hash and runtime layout, not the Cranelift version, so retaining version 26 would keep
installing valid but older 0.122-generated code and make both the performance update and a new
profile appear ineffective.

The controlled comparison retained the final pre-update release compiler executable, which
contains the same no-synthetic-tier-up frontend, and selected it through
`LADYBIRD_CRANELIFT_COMPILER`. The current release compiler differs only in the Cranelift update
and required API migration. Neither path used a compiled-code cache.

Three fresh d3wasm instantiations report:

| Native compilation | Cranelift 0.122.0 | Cranelift 0.134.3 | Change |
| ------------------ | ----------------: | ----------------: | -----: |
| Mean               |        4.010448 s |        3.776771 s |  -5.8% |
| Median             |        3.992744 s |        3.778097 s |  -5.4% |

The native dumps were classified with the same mnemonic-based method. Instruction totals exclude
literal data and `udf` padding; memory totals count scalar AArch64 loads and stores; stack-memory
totals are those operations whose address uses `sp`. The complete generated objects include the
interpreter adapter and per-target fallback bodies:

| Function and metric                         | Cranelift 0.122.0 | Cranelift 0.134.3 | Change |
| ------------------------------------------- | ----------------: | ----------------: | -----: |
| `unzReadCurrentFile()` bytes                |          38,472 B |          37,284 B |  -3.1% |
| `unzReadCurrentFile()` instructions         |             9,563 |             9,260 |  -3.2% |
| `unzReadCurrentFile()` memory operations    |             4,298 |             4,244 |  -1.3% |
| `unzReadCurrentFile()` stack-memory accesses|             3,019 |             3,117 |  +3.2% |
| `unzReadCurrentFile()` body frame           |             816 B |             832 B |  +2.0% |
| `AddActiveInteraction()` bytes              |         228,956 B |         214,200 B |  -6.4% |
| `AddActiveInteraction()` instructions       |            56,593 |            53,176 |  -6.0% |
| `AddActiveInteraction()` memory operations  |            30,367 |            30,843 |  +1.6% |
| `AddActiveInteraction()` stack-memory accesses |         19,741 |            23,518 | +19.1% |
| `AddActiveInteraction()` body frame         |           8,352 B |           8,320 B |  -0.4% |

The update therefore removes substantial static code without uniformly reducing spills. In
particular, whole-function `AddActiveInteraction()` stack traffic increases even while its code
shrinks. This is another reason not to infer runtime improvement from whole-function byte count.
Only the instructions executed at the sampled hot PCs are relevant to the heavy scene.

SHA-512 provides a much stronger positive high-pressure result. Three alternating executions and
the native dump of its unrolled compression function report:

| SHA-512 metric                         | Cranelift 0.122.0 | Cranelift 0.134.3 | Change |
| -------------------------------------- | ----------------: | ----------------: | -----: |
| Mean native compilation               |        0.054560 s |        0.021265 s | -61.0% |
| Mean execution                        |        0.635589 s |        0.342056 s | -46.2% |
| Function 16 bytes                     |          28,912 B |          13,868 B | -52.0% |
| Function 16 instructions              |             7,222 |             3,464 | -52.0% |
| Function 16 memory operations         |             2,340 |               290 | -87.6% |
| Function 16 stack-memory accesses     |             2,260 |               210 | -90.7% |
| Function 16 body frame                |           3,456 B |             336 B | -90.3% |
| Function 16 branches / calls          |            11 / 3 |            11 / 3 | unchanged |

This is repeatable and too large to be measurement noise. It proves that the newer backend can
handle at least one of Ladybird's high-pressure SSA graphs dramatically better. It does not yet
identify the responsible pass. Both releases select regalloc2's backtracking allocator by
default; the dependency moves from regalloc2 0.12.2 to 0.15.2, while Cranelift's optimizer,
AArch64 lowering, and stack addressing also changed. Attributing the result specifically to the
allocator would therefore be premature.

##### Repeatable Instruments analysis

The earlier trace analyses accumulated several one-off Python programs under `/private/tmp`.
Those programs separately resolved xctrace XML references, summarized visual-frame signposts,
classified CPU samples, parsed `.wasmjit` cache records, matched live mappings, and ranked PCs.
Some also hard-coded the JIT bases for one specific process. Reconstructing or editing those
programs for every trace made the procedure difficult to repeat and easy to classify differently.

`Meta/analyze-wasm-instruments-trace.py` now combines that workflow. It:

- exports and reuses the trace table of contents, `cpu-profile`, and `os-signpost` XML;
- parses `WebContent Visual Frame` intervals and reports start-rate and work-time distributions;
- parses the current Cranelift cache format and validates its complete record layout;
- captures `vmmap -wide` from the trace target PID, or accepts retained output through
  `--vmmap-file`;
- correlates every executable mapping with its cache record by allocation size and install order,
  without trace-specific JIT bases;
- reads function names from the named Wasm module with WABT's `wasm-objdump`;
- reports main-thread categories, cycle-weight per update, named functions, inclusive bridge
  frames, and hot function-relative PCs; and
- optionally annotates those PCs with a native dump produced by `wasm --dump-native`.

The `d3wasm_12` analysis was reproduced with:

```sh
Meta/analyze-wasm-instruments-trace.py \
    /Users/lukewilde/Documents/d3wasm_12.trace \
    --cache /Users/lukewilde/Library/Caches/Ladybird/Profiles/default/Cache/c8024fa087c5860d.wasmjit \
    --wasm /Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm \
    --wasm-objdump /Users/lukewilde/Repositories/wabt/Build/wasm-objdump \
    --native-dump /private/tmp/d3wasm-addactive-cranelift-0.134.3.txt \
    --focus-function 1574 \
    --focus-function 2445
```

The process was still alive, so the tool obtained its mappings from PID 18582. A retained mapping
can instead be supplied with `--vmmap-file`. Instruments export and live-process inspection need
to run outside the restricted command sandbox. The exported XML is retained in a temporary work
directory and reused unless `--refresh` is passed; the hundreds of megabytes of profiler XML do
not belong in the repository.

The cache used by the trace is format 27, has seventeen runtime helpers and 6,482 compiled
functions, and is 49,504,224 bytes. Its layout hash is `0x45f757f4257eea23`. Its embedded Wasm
SHA-256 is
`9927bc955b0455621eb47bb7755e6febff0d30f909c0832a019e4c0b119e3829`, exactly matching
`build-wasm-full/d3wasm.wasm`. All 6,482 live executable mappings correlate with the cache. One
large allocation was sorted by `vmmap` after four following allocations; the allocation-size
sequence resolves that local address-order rotation without hard-coded addresses.

The tool is validated by Ruff formatting and linting, repository Python type checking, Python
bytecode compilation, and a complete analysis of both trace runs. The old `/private/tmp` programs
remain as historical scratch data but are superseded by the repository tool.

##### `d3wasm_12` runtime validation

Run 1 is the loading capture. It contains 23.544 billion main-thread cycle-weight units over an
8.381-second CPU sample window. Native Wasm accounts for 74.85%; `unzReadCurrentFile()` accounts
for 5.371 billion units, or 22.81% of all main-thread work. Compared with the disabled-tier-up
`d3wasm_11` loading run, the trace duration rises from 8.603 to 8.843 seconds, total main-thread
work rises by 0.9%, and `unzReadCurrentFile()` rises from 5.248 to 5.371 billion units (+2.3%).
The opposing static code-size improvement therefore still does not produce a loading execution
win; these small movements remain consistent with neutral run-to-run variation.

Run 2 is the manually reproduced heavy opening-area scene. The signposts contain 1,670 complete
rendering updates over a 20.662-second marker span:

| Rendering-update metric | `d3wasm_10` | `d3wasm_11` | `d3wasm_12` |
| ----------------------- | -----------: | -----------: | -----------: |
| Complete updates        |        1,617 |        1,585 |        1,670 |
| Mean update-start rate  |      78.27/s |      76.07/s |      80.83/s |
| Mean work               |    12.650 ms |    13.020 ms |    12.253 ms |
| Median work             |    13.363 ms |    13.574 ms |    12.972 ms |
| 90th percentile         |    14.750 ms |    15.295 ms |    14.312 ms |
| 95th percentile         |    15.662 ms |    16.244 ms |    15.138 ms |
| 99th percentile         |    17.168 ms |    18.628 ms |    16.352 ms |
| Maximum                 |    46.050 ms |    42.109 ms |    25.261 ms |
| Work over 8.333 ms      |        87.6% |        90.8% |        86.2% |
| Work over 16.667 ms     |         2.2% |         3.2% |         0.5% |

The isolated maximum is not a reliable optimization metric, but the median and complete 90th
through 99th percentile range improve relative to both preceding manual captures. High Frequency
profiling overhead still makes these profiled rendering-update times, not absolute unprofiled FPS.

The CPU window contains 56.576 billion main-thread cycle-weight units over 20.498 seconds.
Normalizing by the measured update-start rate gives 16.455 million generated-Wasm units per
update, down from 17.474 million in `d3wasm_11` (-5.8%). JavaScript is effectively unchanged at
6.968 versus 6.963 million units per update. This makes the generated-Wasm movement more credible
than a raw FPS comparison: the adjacent engine work does not indicate a materially lighter JS
workload. Camera position, visibility, and firing remain manually reproduced, so this is evidence
of a modest gameplay improvement rather than strict attribution to Cranelift 0.134.3.

`AddActiveInteraction()` remains the largest resolved gameplay function at 3.007 billion units,
or 5.31% of the main thread. `R_CreateLightTris()` follows at 2.31%, the draw-vertex `Dot()` at
2.25%, `R_CullLocalBox()` at 1.84%, and `R_RenderView()` at 1.16%. The correlation between
rendering-update work duration and main-thread cycle weight is 0.966 across 1,582 updates with CPU
samples, confirming that main-thread saturation explains the slower updates in this capture.

Annotating `AddActiveInteraction()`'s top PCs with the exact 214,200-byte Cranelift 0.134.3 native
dump identifies three repeatedly sampled loops around `0x163ec-0x1650c`, `0x16868-0x16970`, and
`0x16ce0-0x16db0`. The hottest individual PCs are:

| Function offset | Share of function | Instruction |
| --------------: | ----------------: | ----------- |
|       `0x168b8` |             7.44% | `add x4, x5, #0xc` |
|       `0x168a4` |             4.89% | `str x4, [sp, #0x10c8]` |
|       `0x16960` |             2.89% | `ldr x5, [sp, #0x10c0]` |
|       `0x16400` |             2.86% | `ldr x5, [sp, #0xff8]` |
|       `0x16d8c` |             2.61% | `ldr x6, [sp, #0x1110]` |
|       `0x164f4` |             2.42% | `ldr x12, [sp, #0xff8]` |
|       `0x1640c` |             2.19% | `ldr x3, [sp, #0xff8]` |
|       `0x16d88` |             2.10% | `add w12, w5, #0x3` |

The loops also contain required loads and stores through the linear-memory base in `x28`, but
several of the most frequently sampled instructions access Cranelift's native stack frame. In the
`0x16868-0x16970` loop, for example, the loop-carried value at `sp + 0x10c0` is loaded repeatedly
and stored on the backedge. The all-SSA frontend no longer maintains explicit canonical local
homes, so these ordinary `sp` slots in the hot loop are backend spill or lowered stack slots,
not the removed native-local array. Cold interpreter-fallback paths elsewhere in the function do
materialize more state, but they are not the sampled loop.

Instruction-level active-cycle samples can skid and phase-lock within a tight loop. The 7.44% at
the address calculation must not be read as that `add` having exceptional standalone latency.
The clustered loop range and repeated stack accesses are the stronger result. They also explain
why the newer backend can improve measured work while whole-function stack-memory instructions
increase: static totals include cold paths and say nothing about the executed path or its trip
count.

The next diagnostic sequence is now:

1. Match the three sampled `AddActiveInteraction()` ranges and the loading run's hot
   `unzReadCurrentFile()` ranges between Cranelift 0.122.0 and 0.134.3. Compare range-local
   instructions, linear-memory operations, stack operations, and branches rather than complete
   object totals.
2. Temporarily capture optimized CLIF, pre-register-allocation VCode, and regalloc output metrics
   for SHA-512 function 16 and the sampled d3wasm loops. Comparing IR instruction count, VReg
   count, spill edits, spill slots, and final stack accesses will separate mid-end simplification,
   instruction lowering, and register allocation.
3. Use SHA-512 as a fast discriminator to bisect released Cranelift versions from 0.123 through
   0.134. Once the first large improvement is located, inspect only that release interval's
   upstream optimizer, AArch64, and regalloc changes. Feed captured equivalent CLIF to both
   backends where possible so frontend-builder API changes do not confound the backend result.
4. Add per-pass Cranelift timing only to a temporary diagnostic compiler. The public timing module
   separates egraph optimization, VCode lowering, register allocation, and emission, which can
   explain the 61% SHA-512 compilation improvement and show which stage dominates d3wasm.

The update is validated by Rust formatting, `cargo check`, the Rust unit test, and the LibWasm
JavaScript/spec harness (15 suites, 21 tests passed and 38 skipped). Nine of ten focused native
execution tests pass. The remaining test expects tier-up checkpoints and fails because the
separate worktree experiment still sets `tier_up_instruction_threshold` to
`NumericLimits<size_t>::max()`; it is not an update regression.

#### Apples-to-apples Wasmtime 47 gap diagnosis

Wasmtime 47.0.3 and the updated Ladybird compiler both use Cranelift 0.134.3. Wasmtime's optimized
CLIF was emitted while compiling the same named `d3wasm.wasm`, and Ladybird's optimized CLIF was
captured after `Context::compile()` from a temporary diagnostic compiler. The dump hook was then
removed and the ordinary compiler rebuilt. `Meta/analyze-cranelift-clif.py` retains the structural
CLIF and inclusive native-range comparison so these measurements do not depend on new one-off
scripts.

For `AddActiveInteraction()`, the whole optimized functions differ before instruction selection:

| Optimized CLIF metric | Ladybird | Wasmtime 47 |
| --------------------- | -------: | -----------: |
| Blocks | 2,283 | 1,726 |
| Instructions | 34,367 | 18,372 |
| Memory operations | 9,937 | 3,505 |
| Block parameters | 17,752 | 6,464 |
| `i32` block parameters | 11,595 | 4,497 |
| `i64` block parameters | 2,304 | 52 |
| `f32` block parameters | 3,853 | 1,915 |
| Explicit stack slots | 101 | 0 |
| Explicit stack-slot bytes | 3,320 | 0 |
| Operations with an alias region | 0 | 3,574 |

The explicit slots expose a large cold-path problem. The function has fifty `call_indirect`
sites. Ladybird independently materializes the 25-byte `Table index out of bounds` and 41-byte
`Table element is not a function reference` messages at every site, plus a 20-byte unreachable
message. That accounts for all 101 slots and all 3,320 explicit bytes. Each message byte requires
at least a `stack_addr` and store in optimized CLIF, so those cold strings account for at least
6,640 instructions, 41.5% of the complete 15,995-instruction CLIF gap. Their 3,320 stores also
account for 71.0% of the store-count difference. This inflates compilation work, code size, and
the fixed native frame, but is not the direct cause of the sampled hot-loop traffic. A later trap
ABI should carry a compact trap reason and construct the text only after a trap is taken.

The three sampled native loops map exactly to Wasmtime by their Wasm address maps and operation
sequences. Their loop-header state is:

| Loop | Ladybird header | Ladybird parameters | Wasmtime header | Wasmtime parameters |
| ---- | ---------------- | ------------------: | --------------- | ------------------: |
| `0x163ec-0x1650c` | `block1378` | 74: 45 `i32`, 9 `i64`, 20 `f32` | `block913` | 62: 42 `i32`, 20 `f32` |
| `0x16868-0x16970` | `block1406` | 72: 43 `i32`, 9 `i64`, 20 `f32` | `block933` | 63: 43 `i32`, 20 `f32` |
| `0x16ce0-0x16db0` | `block1420` | 72: 44 `i32`, 10 `i64`, 18 `f32` | `block944` | 62: 44 `i32`, 18 `f32` |

The typed `i32` and `f32` parameter counts match exactly in the second and third loops. The
remaining structural difference there is Ladybird's nine or ten `i64` payload shadows. These are
not the removed canonical local homes. `reg_vars` still maintains an always-defined payload for
the bytecode virtual registers, and every typed write defines both the typed bank and that payload.
`reset_banks!()` selects the payload after a loop or ordinary merge because bank metadata is not
yet merged per incoming edge. The payload is also the representation required by an actual
interpreter, helper, call-record, or `ValueStack` boundary, but continuously carrying it through
normal native control flow is unnecessary when the edge's exact Wasm type is known.

The matching native ranges quantify the consequence:

| Metric | Ladybird loop 1 | Wasmtime loop 1 | Ladybird loop 2 | Wasmtime loop 2 | Ladybird loop 3 | Wasmtime loop 3 |
| ------ | ----------------: | --------------: | ----------------: | --------------: | ----------------: | --------------: |
| Instructions | 73 | 52 | 67 | 50 | 53 | 50 |
| Memory operations | 39 | 29 | 29 | 19 | 23 | 23 |
| Native stack operations | 30 | 20 | 17 | 7 | 12 | 12 |

All ten additional memory operations in each of the first two loops are native stack accesses.
In the second loop, Wasmtime keeps the loop bound in `w22`, while Ladybird repeatedly reloads its
counter or bound around `sp + 0x10c0`; these include the PCs sampled by Instruments. The third
loop is an important guardrail: despite ten extra `i64` header values, its spill count matches
Wasmtime. Block-parameter count is therefore a pressure indicator rather than a direct spill
formula. Exact interference and instruction-local temporaries determine whether a loop crosses
the allocator's cliff.

`unzReadCurrentFile()` exposes a broader local/CFG problem:

| Optimized CLIF metric | Ladybird | Wasmtime 47 |
| --------------------- | -------: | -----------: |
| Blocks | 875 | 761 |
| Instructions | 7,675 | 5,440 |
| Memory operations | 1,198 | 542 |
| Block parameters | 9,171 | 1,432 |
| `i32` block parameters | 6,806 | 1,354 |
| `i64` block parameters | 2,365 | 78 |
| Maximum parameters on one block | 41 | 30 |

The payload shadow is visible in the 2,287 excess `i64` parameters, but removing it cannot close
the 5,452-parameter `i32` gap. Both frontends represent Wasm locals with Cranelift `Variable`s;
the difference is the CFG across which those definitions are requested. Ladybird lowers its
interpreter bytecode and runtime compatibility branches, while Wasmtime translates the validated
Wasm control and operand stacks directly. Each additional join can make `FunctionBuilder` thread
many still-live local definitions through another block even when the joined code does not use
them immediately. This is the same high-pressure failure previously seen when all locals were
promoted, now measured against the same backend version.

A controlled diagnostic compile set `max_stack_depth` to zero, forcing operand-stack values back
through the interpreter `ValueStack`. It changed total block parameters only from 9,171 to 9,154,
`i32` parameters from 6,806 to 6,805, and `i64` parameters from 2,365 to 2,349. At the same time,
optimized memory operations rose from 1,198 to 8,210 and the complete native object rose from
37,284 to 78,100 bytes. The experiment was reverted immediately and the normal compiler rebuilt
to its original hash. The vstack is therefore a large win and is not the source of the local-edge
pressure; almost all of the `i64` parameter excess remains in the canonical register payload bank.

The resulting next experiments are separate and independently measurable:

1. Carry exact register-bank types on normal control-flow edges and materialize an `i64` payload
   only at real interpreter/helper/call-record/`ValueStack` boundaries. Recompare the three
   `AddActiveInteraction()` loops first; they isolate this issue cleanly.
2. Attribute `unzReadCurrentFile()`'s `i32` parameters to locals versus bytecode registers, then
   reduce CFG joins or apply regional pressure-aware local homes. Disabling the vstack is ruled
   out, and a function-wide return to native local homes would restore known per-access traffic.
3. Replace per-site trap strings with compact trap reasons. This primarily targets compilation,
   memory, cold code size, and fixed frame size rather than the sampled hot loops.
4. Audit and add correct alias regions separately. Wasmtime supplies materially more alias
   information, but alias flags cannot remove frontend-requested duplicate live state.

#### Alias information and runtime CFG

Correction after checking the exact Cranelift 0.134.3 source and the retained Wasmtime 47 CLIF:
`heap` and `table` are not named `MemFlagsData` flags in this version. Cranelift 0.122 had the fixed
`AliasRegion::{Heap, Table, Vmctx}` enum, which is the source of the stale terminology. Cranelift
0.134.3 instead lets each frontend insert arbitrary `AliasRegionData { user_id, description }`
entries into `function.dfg.alias_regions`, then attach one with
`MemFlagsData::with_alias_region()`. `readonly` and `can_move` remain real, independent memory
flags.

Wasmtime 47 uses the general interface. Its `AddActiveInteraction()` CLIF declares regions such as
`PublicMemory`, `DefinedTable`, individual defined globals, table and memory metadata fields,
canonical type entries, and function-reference fields. Instructions print the opaque identity,
for example `region6`; the declaration's description is diagnostic and has no semantic meaning to
Cranelift. Ladybird currently emits no alias regions, `readonly`, or `can_move`. Its unchecked
guarded-memory accesses use otherwise empty `MemFlagsData`, while configuration, global, table,
local, and helper-related accesses use ordinary trusted loads and stores.

An alias region supplies one fact: operations in different regions cannot access the same storage.
Operations in the same region may still alias and need ordinary address analysis, while an
unclassified operation remains conservative. This can let Cranelift retain or reuse a loaded
configuration, local, global, or table value across unrelated linear-memory stores. More generally
it can enable redundant-load elimination, store-to-load forwarding, dead-store elimination,
reordering of independent memory operations, and load motion when the separate trapping and
`can_move` requirements are also satisfied. It does not itself remove frontend-requested duplicate
SSA state.

Adding regions therefore requires a general correctness audit rather than renaming empty flags.
The frontend can define broad Ladybird linear-memory, table, configuration, activation, and global
regions, or finer field-specific regions where the runtime representation proves them disjoint.
Two imported memory or table indices may resolve to the same runtime instance, so they cannot be
given distinct regions merely because their module indices differ. Opaque calls remain broad
memory barriers unless their effects are represented, and memory-growing or table-mutating calls
must continue to invalidate affected state. `readonly` is stronger still: it is valid only when the
referenced storage cannot change for the entire native function activation. `can_move` separately
asserts that code motion is safe and is not implied by non-aliasing.

Ladybird and Wasmtime emit the same 22 direct calls to the four Wasm callees. Ladybird's remaining
eighteen call instructions include stack-exhaustion, trap, type-check, and native/fallback paths
for dynamic calls; Wasmtime has nine additional calls. Ladybird also has 153 more blocks. Adapter,
OSR, trap, and interpreter-fallback paths account for some static size and are mostly cold, while
the direct-call context updates and dynamic-call paths can execute at runtime. They are secondary
to the measured state-representation gap but explain why Wasmtime remains smaller even after a
profitable local loop approaches its code quality.

#### Resulting sequence

The measurements support a composable sequence rather than either native homes or all SSA as a
function-wide policy:

1. Preserve exact Wasm integer widths in native vstack and local state. Measure a typed `i32`/`i64`
   experiment first against `unzReadCurrentFile()`, SHA-512, and the existing representative suite.
2. Retain native homes for genuinely high-pressure regions while carrying profitable low-pressure
   loop values in SSA. The decision needs regional live pressure and edge information rather than
   the function's declared-local count.
3. Reduce unnecessary block parameters and determine how much pressure comes from the bytecode CFG
   versus synthetic-tier-up resume predecessors. Tier-up must remain a one-way, cold handoff, but
   its compatibility edge should not make normal native edges carry unrelated locals.
4. Define and apply linear-memory, table, VM-context, and activation alias regions through
   Cranelift's general `AliasRegionData` interface after a correctness audit.
5. Continue trimming interpreter-transition, trap, and indirect-call scaffolding as a separate
   runtime-ABI workstream. These paths do not justify retaining hot local traffic, but removing
   them does not replace typed state and pressure-aware SSA.

Compilation time and compiler memory remain important guardrails, particularly because native
readiness competes with interpreter execution during a cold load. Tiered execution and persistent
compiled-code caching make generated execution quality the first-pass priority, however. Compiler
latency and peak memory should receive their own second pass rather than preserving demonstrably
poor generated code to improve them. Peak compiler memory was not measured in this experiment.

### Repeatable native-execution test workflow

Run Rust formatting and type checking from the Ladybird repository root:

```sh
cargo fmt --manifest-path Libraries/LibWasm/Rust/Cargo.toml
cargo check --manifest-path Libraries/LibWasm/Rust/Cargo.toml
```

`cargo check` does not compile representative Wasm input, so it cannot detect invalid Cranelift IR
produced by the lowering. The C++ execution tests are required as well. Build them from the
repository root. In a restricted Codex sandbox, redirect both ccache directories to writable
temporary storage:

```sh
env CCACHE_DIR=/tmp/ladybird-ccache \
    CCACHE_TEMPDIR=/tmp/ladybird-ccache-tmp \
    cmake --build Build/release --target TestWasmExecution -j8
```

Without those variables, ccache fails while creating files below
`~/Library/Caches/ccache/tmp`; this is a sandbox failure, not a compiler error. The CMake target
rebuilds the Rust compiler when needed. Its explicit target name is
`cranelift-compiler-build`, not `cranelift-compiler`.

Run the executable with `Tests/LibWasm` as its working directory because fixture paths are relative:

```sh
cd Tests/LibWasm
../../Build/release/bin/TestWasmExecution
```

Running it from the repository root fails to open `Fixtures/*.wasm`. More importantly, the native
tests must run outside the restricted macOS sandbox: LibWasm creates a POSIX shared-memory object
and spawns `cranelift-compiler` to compile through that mapping. When the sandbox blocks this IPC,
native compilation is silently unavailable and the test output misleadingly reports every
`cranelift_compiled` expectation as false while interpreter-only cases still pass. Confirm a native
compiler regression only after repeating the test outside the sandbox.

The broader LibWasm JavaScript/spec harness also needs `LADYBIRD_SOURCE_DIR`, even when an explicit
Wasm test root is passed, because it separately resolves `Tests/LibJS/Runtime/test-common.js`:

```sh
LADYBIRD_SOURCE_DIR=/Users/lukewilde/Repositories/ladybird \
    Build/release/bin/test-wasm \
    --show-progress=false \
    /Users/lukewilde/Repositories/ladybird/Libraries/LibWasm/Tests
```

Without that variable it misleadingly says “No test root given”. Run this harness outside the
sandbox too when native compilation behavior is under test. The first native `call_indirect`
checkpoint passed 15 suites: 21 tests passed and 38 were skipped.

After editing the focused text fixture, regenerate its binary with the locally built WABT tool:

```sh
/Users/lukewilde/Repositories/wabt/Build/wat2wasm \
    Tests/LibWasm/Fixtures/native-call-abi.wat \
    -o Tests/LibWasm/Fixtures/native-call-abi.wasm

/Users/lukewilde/Repositories/wabt/Build/wat2wasm \
    Tests/LibWasm/Fixtures/native-call-indirect-abi.wat \
    -o Tests/LibWasm/Fixtures/native-call-indirect-abi.wasm
```

### Separate future workstream: Wasm-JavaScript boundaries

Optimizing calls from Wasm to JavaScript and from JavaScript to Wasm is a separate future
workstream. Its boundary costs should not be conflated with module compilation, generated Wasm
execution, or compiled-to-compiled Wasm calls. No investigation or baseline has been performed for
this workstream yet.

### Edge-typed register banks and lazy canonical payloads

The first follow-up to the Wasmtime gap diagnosis tracked the selected register bank at structured
control-flow targets. The first incoming edge establishes each target register's representation;
later `br`, `br_if`, `br_table`, block fallthrough, and loop-backedge predecessors convert only
registers whose selected representations differ. Function returns use the same rule. Synthetic
tier-up dispatch starts with canonical interpreter payloads and reconstructs the representation
expected by the particular loop header before taking its resume edge.

Merely preserving the selected typed bank was the wrong intermediate implementation. Typed writes
still eagerly defined their canonical `i64` shadows, and edge conversion read those shadows. With
normal tier-up enabled, this increased `AddActiveInteraction()` from 22,072 to 22,246 block
parameters and `unzReadCurrentFile()` from 11,558 to 11,620. Every added parameter was `i64`.

The refined implementation gives each bytecode register one selected authoritative bank. An
`i32`, `f32`, or `f64` register write no longer also defines `reg_vars`. A consumer that genuinely
needs the canonical payload reconstructs its bits from the selected bank. Control-flow edges
convert directly between their source and target banks, and the merged return block synchronizes
the final selected values to the interpreter-visible configuration once. Virtual operand-stack
entries still retain canonical payloads; stack-bank edge tracking is deliberately outside this
register experiment.

The focused execution fixture already covers all four scalar types through `if` merges and loop
backedges. With the normal tier-up threshold restored, all ten `TestWasmExecution` cases pass,
including the one-way tier-up resume test. Rust formatting and type checking pass. The temporary
post-compilation CLIF dump hook was removed before rebuilding the ordinary compiler.

#### Structural result

The same pre-change and candidate compilers were compared with synthetic tier-up both disabled and
enabled. The enabled configuration is the real one; disabling it only keeps the original block IDs
stable and isolates ordinary Wasm edges.

| Function and optimized-CLIF metric | Baseline, tier-up off | Lazy banks, tier-up off | Baseline, tier-up on | Lazy banks, tier-up on |
| ---------------------------------- | --------------------: | ----------------------: | --------------------: | ----------------------: |
| `AddActiveInteraction()` total parameters | 17,752 | 17,927 | 22,072 | 22,255 |
| `AddActiveInteraction()` `i32` parameters | 11,595 | 12,651 | 14,278 | 15,477 |
| `AddActiveInteraction()` `i64` parameters | 2,304 | 1,267 | 2,681 | 1,500 |
| `AddActiveInteraction()` native bytes | 214,200 | 214,520 | 247,640 | 249,208 |
| `unzReadCurrentFile()` total parameters | 9,171 | 9,251 | 11,558 | 11,639 |
| `unzReadCurrentFile()` `i32` parameters | 6,806 | 7,604 | 8,834 | 9,722 |
| `unzReadCurrentFile()` `i64` parameters | 2,365 | 1,647 | 2,724 | 1,917 |
| `unzReadCurrentFile()` native bytes | 37,284 | 37,700 | 40,196 | 39,876 |

The intended `i64` reduction materializes, but total SSA pressure does not yet fall. Much of the
removed payload state becomes typed `i32` state because a Wasm local and the bytecode virtual
register receiving `local.get` can remain separately live across the same edge. This is the next
duplication to remove; the current change establishes the single-representation invariant needed
to do so without falling back to canonical payloads.

The original three tier-up-disabled hot loops retain the same total header size but change their
representation mix:

| Loop header | Baseline parameters (`i32` / `i64` / `f32`) | Lazy-bank parameters (`i32` / `i64` / `f32`) |
| ----------- | ------------------------------------------: | -------------------------------------------: |
| `block1378` | 45 / 9 / 20 | 49 / 5 / 20 |
| `block1406` | 43 / 9 / 20 | 47 / 5 / 20 |
| `block1420` | 44 / 10 / 18 | 48 / 5 / 19 |

The matched native ranges are correspondingly mixed:

| Hot range | Baseline instructions | Lazy-bank instructions | Baseline stack accesses | Lazy-bank stack accesses |
| --------- | --------------------: | ---------------------: | ----------------------: | -----------------------: |
| Loop 1 | 73 | 66 | 30 | 26 |
| Loop 2 | 67 | 67 | 17 | 19 |
| Loop 3 | 53 | 52 | 12 | 12 |

This is a pressure-redistribution prerequisite, not a standalone generated-code win. It improves
one allocator cliff, worsens another, and leaves the third neutral.

#### Compilation and execution guardrails

Both diagnostic compilers contain the identical inactive CLIF-dump environment check, so their
compilation overhead is matched. Three alternating fresh, uncached d3wasm instantiations with
tier-up enabled report:

| d3wasm phase | Baseline | Lazy banks | Change |
| ------------ | -------: | ---------: | -----: |
| Native compilation | 3.994684 s | 3.969891 s | -0.6% |
| Summed parse, validation, compilation, and instantiation | 5.246131 s | 5.229759 s | -0.3% |

The prerequisite therefore does not impose a cold-load penalty in this sample. Three benchmark
iterations per compiler produced these execution-phase results; Blake3 remains predominantly
interpreted and is not evidence about Cranelift-generated execution:

| Workload | Baseline execution | Lazy-bank execution | Change |
| -------- | -----------------: | ------------------: | -----: |
| Base64 | 0.299 s | 0.297 s | -0.7% |
| Blake3 | 27.521 s | 27.669 s | +0.5% |
| JSON | 0.952 s | 0.899 s | -5.6% |
| Regex | 0.123 s | 0.124 s | +0.8% |
| SHA-512 | 0.337 s | 0.341 s | +1.2% |

CoreMark's primary metric is its printed score. A baseline-first three-run round favored the
baseline, 32,849 versus 32,291 (-1.7%). A reverse-order three-run round favored the candidate,
32,630 versus 32,609 (+0.1%). The disagreement is retained rather than averaged away; it does not
establish a repeatable standalone CoreMark regression.

The acceptance criterion for this sequence is therefore compositional. A correctness-complete
checkpoint can be retained when it establishes a necessary invariant, has no serious measured
regression, and unlocks a concrete next pass. It need not be independently benchmark-positive or
fully cleaned up. The combined local/register-duplication result should be measured before
simplifying the mechanism and judging the total diff.

#### Equivalent block-parameter cleanup experiment

A follow-up prototype attempted to remove two same-typed Cranelift block parameters when every
incoming edge supplied either the same value or another recursively equivalent parameter pair.
The greatest-fixed-point formulation correctly handled loop phis in a focused unit test, but it
did not describe the d3wasm state inflation seen above.

With synthetic tier-up enabled, both `AddActiveInteraction()` and `unzReadCurrentFile()` removed
zero parameters. Their post-optimization CLIF was byte-for-byte and structurally identical to the
checkpoint compiler: 22,255 parameters remained in `AddActiveInteraction()` and 11,639 remained
in `unzReadCurrentFile()`. Across the complete module, only 69 of approximately 6,500 functions
changed, removing 84 parameters in total; each affected function lost between one and four.

Matched diagnostic compilers contained the same CLIF-dump hook. The CLI's internal native
compilation timer, rather than command wall time that could include sandbox-approval latency,
reported:

| Run | Checkpoint | Equivalent-parameter pass |
| --- | ---------: | ------------------------: |
| 1 | 3.963116 s | 5.732834 s |
| 2 | 4.180392 s | 5.784498 s |
| Mean | 4.071754 s | 5.758666 s |

The prototype therefore increased d3wasm native compilation by 41.4% for negligible structural
effect and no effect in either primary function. This also refines the earlier diagnosis: the
local and bytecode-register values are usually distinct live state, not duplicate phi columns
that happen to carry the same value around every edge. A useful next experiment must avoid
creating or keeping unnecessary state during lowering or SSA construction, rather than relying on
a generic pairwise cleanup after `FunctionBuilder::finalize()`. The prototype, its test, and its
diagnostic hooks were removed; they are not part of the branch checkpoint.

### Lazy canonical virtual-stack payloads

The next checkpoint was deliberately restricted to the remaining canonical virtual-stack shadow.
During native execution, each bytecode register and virtual operand-stack entry now has one
authoritative selected bank. An `i32`, `f32`, or `f64` stack write defines only that typed bank;
it no longer also defines an `i64` payload. The payload is reconstructed only at a boundary that
actually consumes the interpreter representation: a real `ValueStack` push, a raw helper call,
a call-record store, or a function result returned through the interpreter adapter. A genuine
Wasm `i64` value naturally remains in the `i64` bank and is not a redundant canonical shadow.

Normal native edges do not canonicalize the stack. Each structured target records the bank vector
established by its first incoming edge. Later block fallthroughs, loop backedges, `br`, `br_if`,
and `br_table` edges convert only entries whose selected banks differ. Branch results are copied
in their selected bank. Synthetic tier-up remains a cold interpreter-to-native boundary. The
validator only marks a loop tier-up eligible when it has no parameters and the complete operand
stack is empty, so there is no live virtual-stack payload to reconstruct at those resume points.

No block-parameter cleanup, pressure heuristic, CFG transformation, or call-ABI change is part of
this checkpoint.

#### Focused correctness coverage

`vstack_control_edges` was added to `tier-up-one-way.wat`. Its compiled bytecode places `i32`,
`i64`, `f32`, and `f64` values on the virtual stack, keeps all four live across an `if`, a loop
backedge, a `br_if` with an extra value to discard, and a two-target `br_table`, then consumes each
value in its original type. Both branch choices return `40.75` through a fresh native invocation.

The ordinary, non-diagnostic compiler passes:

- Rust formatting, unit tests, and Clippy with `-D clippy::all`;
- all ten `TestWasmExecution` cases outside the restricted sandbox; and
- the broader LibWasm harness: 15 suites passed, 21 tests passed, and 38 unsupported tests skipped.

#### Structural result

The committed register-bank checkpoint and the lazy-stack candidate were compiled with synthetic
tier-up enabled. The optimized CLIF shows fewer canonical conversion instructions without changing
loads, stores, calls, branches, or CFG block count:

| Optimized-CLIF metric | `unzReadCurrentFile()` baseline | Lazy stack | `AddActiveInteraction()` baseline | Lazy stack |
| --------------------- | ------------------------------: | ---------: | ---------------------------------: | ---------: |
| Instructions | 7,451 | 7,408 | 34,059 | 33,937 |
| Values | 5,915 | 5,872 | 25,645 | 25,523 |
| Block parameters | 11,639 | 11,682 | 22,255 | 22,296 |
| `i32` block parameters | 9,722 | 9,781 | 15,477 | 15,543 |
| `i64` block parameters | 1,917 | 1,901 | 1,500 | 1,463 |
| Integer reductions | 97 | 73 | 514 | 479 |
| Zero extensions | 785 | 762 | 2,031 | 2,000 |
| Sign extensions | 0 | 0 | 69 | 49 |

Typed stack state adds 41--43 block parameters, but removes 43 instructions from
`unzReadCurrentFile()` and 122 from `AddActiveInteraction()`. Cranelift's later spilling response
is mixed:

| Native metric | `unzReadCurrentFile()` baseline | Lazy stack | `AddActiveInteraction()` baseline | Lazy stack |
| ------------- | ------------------------------: | ---------: | ---------------------------------: | ---------: |
| Code bytes | 39,876 | 39,956 | 249,208 | 247,592 |
| Instructions | 9,938 | 9,955 | 62,194 | 61,740 |
| Loads | 2,635 | 2,680 | 19,871 | 19,532 |
| Stores | 2,129 | 2,146 | 17,850 | 17,916 |
| Stack-memory operations | 3,585 | 3,647 | 30,288 | 30,015 |

The primary gameplay function improves while the primary loading function regresses slightly.
That is consistent with pressure redistribution after removing the shadow, rather than evidence
that canonical traffic remains in the frontend lowering.

#### Compilation and execution guardrails

Six alternating fresh d3wasm compilations used the CLI's internal native-compilation timer. The
baseline samples were 4.649554, 4.255521, and 4.184782 seconds; the candidate samples were
4.097448, 4.184390, and 4.155687 seconds. Their raw means are 4.363286 and 4.145842 seconds
respectively, but the first baseline sample is sufficiently high that this establishes only the
absence of a compilation regression, not a 5% speedup.

Three-iteration baseline-first and candidate-first benchmark rounds disagreed in the small places
where they moved. Isolated execution phases, not process totals, were compared:

| Workload | Baseline-first result | Candidate-first result |
| -------- | --------------------: | ---------------------: |
| CoreMark reported score | 32,168.590 baseline / 32,097.275 candidate (-0.2%) | 32,103.861 baseline / 32,145.088 candidate (+0.1%) |
| Base64 execution | 0.288 s baseline / 0.299 s candidate (+3.8%) | 0.298 s baseline / 0.288 s candidate (-3.4%) |
| JSON execution | 0.933 s baseline / 0.928 s candidate (-0.5%) | 0.924 s baseline / 0.933 s candidate (+1.0%) |
| Regex execution | 0.124 s baseline / 0.125 s candidate (+0.8%) | 0.124 s baseline / 0.125 s candidate (+0.8%) |
| SHA-512 execution | 0.344 s baseline / 0.350 s candidate (+1.7%) | 0.343 s baseline / 0.343 s candidate (neutral) |

The complete first pass also left predominantly interpreted Blake3 neutral at 27.994 versus
28.002 seconds. These rounds do not establish a repeatable execution regression. Raw process
results are retained in `/private/tmp/ladybird-lazy-vstack-{baseline,candidate}.json` and the
reverse-order focused results in
`/private/tmp/ladybird-lazy-vstack-{baseline,candidate}-reverse.json`.

The temporary CLIF dump hook was removed, and the ordinary compiler was rebuilt before final
validation.

### Cranelift user trap codes

The next checkpoint removes per-site trap-string construction from compiled code. Ladybird now
assigns compact Cranelift user trap codes to `unreachable`, an out-of-bounds table index, a null
indirect-call target, and an indirect-call type mismatch. `trap`, `trapz`, and `trapnz` record the
reason in Cranelift's existing trap table. The compiled-fault recovery path translates the code to
the existing diagnostic string only after the cold trap is taken.

The inlined `call_indirect` path consequently no longer creates separate bounds-failure,
bounds-success, null-failure, or callable-success blocks. Its bounds and callable checks remain on
the normal path as conditional traps. A non-exact canonical type still calls
`wasm_cl_check_indirect_type`, but that helper is now a pure predicate and the caller conditionally
traps on its result. The obsolete `wasm_cl_set_trap` helper, its runtime-helper field, and its
relocation ID were removed. The helper count changed from seventeen to sixteen, and the cache blob
format changed from 27 to 28 so old generated code cannot be installed with the new helper layout or
trap-code interpretation.

The POSIX compiled-fault handler already recovered Cranelift's trap instructions through the trap
table. The Windows vectored exception handler previously handled only guarded-memory access
violations despite Windows being marked as supporting compiled fault recovery. It now also looks up
Cranelift trap-table entries for illegal-instruction, integer-division, and integer-overflow
exceptions before redirecting to the existing recovery trampoline.

#### Correctness and structural result

The existing `native_indirect_call_uses_typed_abi` coverage exercises all four affected outcomes
and checks their exact messages: a native `unreachable`, an indirect type mismatch, a null table
element, and an out-of-bounds table index. All ten `TestWasmExecution` cases pass. Rust formatting,
unit tests, and Clippy with `-D clippy::all` pass, as does the broader LibWasm harness: 15 suites and
21 tests passed, with 38 unsupported tests skipped.

A temporary, inactive-unless-requested diagnostic hook captured post-optimization CLIF with
synthetic tier-up enabled. The baseline is the immediately preceding lazy-vstack checkpoint:

| Optimized-CLIF metric | `unzReadCurrentFile()` baseline | Trap codes | `AddActiveInteraction()` baseline | Trap codes |
| --------------------- | ------------------------------: | ---------: | ---------------------------------: | ---------: |
| Blocks | 914 | 905 | 2,337 | 2,137 |
| Instructions | 7,408 | 6,985 | 33,937 | 25,222 |
| Values | 5,872 | 5,624 | 25,523 | 20,258 |
| Memory operations | 1,250 | 1,082 | 10,045 | 6,745 |
| Loads | 636 | 636 | 4,263 | 4,263 |
| Stores | 614 | 446 | 5,782 | 2,482 |
| Calls | 40 | 35 | 436 | 336 |
| Branches | 912 | 903 | 2,335 | 2,135 |
| Block parameters | 11,682 | 11,675 | 22,296 | 22,296 |
| Explicit stack slots | 5 | 0 | 101 | 0 |
| Explicit stack-slot bytes | 152 | 0 | 3,320 | 0 |

This removes 423 optimized-CLIF instructions from `unzReadCurrentFile()` and 8,715 from
`AddActiveInteraction()`. All 106 explicit message slots disappear. The gameplay function's 3,300
removed stores exactly match the message-byte count, while removing the success/failure blocks and
helper calls accounts for the additional CFG and instruction reduction. Block-parameter pressure
is unchanged in `AddActiveInteraction()`, keeping this result separate from the local/register and
OSR-edge workstreams.

Native lowering retains the reduction:

| Native metric | `unzReadCurrentFile()` baseline | Trap codes | `AddActiveInteraction()` baseline | Trap codes |
| ------------- | ------------------------------: | ---------: | ---------------------------------: | ---------: |
| Code bytes | 39,956 | 38,708 | 247,592 | 191,400 |
| Instructions | 9,955 | 9,649 | 61,740 | 47,685 |
| Loads | 2,680 | 2,655 | 19,532 | 18,139 |
| Stores | 2,146 | 1,976 | 17,916 | 13,539 |
| Stack-memory operations | 3,647 | 3,473 | 30,015 | 24,437 |

That is a 3.1% native-code reduction for the loading function and a 22.7% reduction for the
gameplay function. `AddActiveInteraction()` also loses 18.6% of its statically generated
stack-memory operations. These whole-function counts include cold code and do not by themselves
claim an equivalent runtime improvement.

#### Compilation and execution guardrails

Three fresh candidate d3wasm processes reported native compilation times of 3.909595, 3.422784,
and 3.359499 seconds, a 3.563959-second mean. The immediately preceding checkpoint measured
4.097448, 4.184390, and 4.155687 seconds, a 4.145842-second mean. The raw sequential comparison is
14.0% lower, which is consistent with the frontend and native-code reductions, but it was not an
interleaved A/B and should not be treated as a precise speedup.

A fresh three-iteration execution guardrail produced a CoreMark score of
31,365.690 +/- 665.846. Isolated execution phases were 0.290 seconds for Base64, 28.536 for the
predominantly interpreted Blake3 case, 0.971 for JSON, 0.128 for Regex, and 0.352 for SHA-512. The
entire round was slower than the preceding run, including workloads whose generated trap paths are
not executed; without an interleaved compatible baseline this does not identify a trap-code
execution regression. The raw candidate results are in
`/private/tmp/ladybird-trap-codes-candidate.json`.

The diagnostic compiler is retained at
`/private/tmp/cranelift-compiler-0.134.3-trap-codes-diagnostic`, and the optimized CLIF is retained
as `/private/tmp/{unz,addactive}-ladybird-trap-codes.clif`. The temporary dump hook was removed and
the ordinary compiler rebuilt before the timing and final validation runs.

### Cranelift alias-region classification

Commit `1dd273bb638` classifies every explicit Cranelift load and store emitted by Ladybird's main
functions, interpreter adapters, and fallback thunks. Each generated Cranelift function has six
frontend-defined regions:

- activation values;
- `Configuration` fields;
- global state;
- linear memory;
- runtime metadata; and
- table state.

A region identity says only that accesses in different regions cannot alias. It does not say that
two accesses within one region are disjoint. Imported memories, tables, and globals can resolve to
the same runtime instance through different module indices, so each category deliberately uses one
broad region rather than creating an unsound region per index. Calls remain complete memory
barriers. Linear-memory accesses retain their trapping flags, while runtime-owned accesses retain
their existing `trusted` property. This checkpoint adds neither `readonly` nor `can_move`.

The cache blob format changed from 28 to 29 so existing cache entries cannot hide the new lowering
during measurement.

#### Structural result

The immediately preceding trap-code checkpoint and the alias-region candidate were compiled with
matched diagnostic compilers and synthetic tier-up enabled:

| Optimized-CLIF metric | `unzReadCurrentFile()` baseline | Alias regions | `AddActiveInteraction()` baseline | Alias regions |
| --------------------- | ------------------------------: | ------------: | ---------------------------------: | ------------: |
| Blocks | 905 | 905 | 2,137 | 2,137 |
| Instructions | 6,985 | 6,927 | 25,222 | 24,584 |
| Values | 5,624 | 5,566 | 20,258 | 19,620 |
| Memory operations | 1,082 | 1,043 | 6,745 | 6,132 |
| Loads | 636 | 597 | 4,263 | 3,650 |
| Stores | 446 | 446 | 2,482 | 2,482 |
| Block parameters | 11,675 | 11,675 | 22,296 | 22,296 |

The regions let Cranelift eliminate 39 loads from `unzReadCurrentFile()` and 613 loads from
`AddActiveInteraction()` without changing CFG or block-parameter pressure. The native result
retains most of that improvement:

| Native metric | `unzReadCurrentFile()` baseline | Alias regions | `AddActiveInteraction()` baseline | Alias regions |
| ------------- | ------------------------------: | ------------: | ---------------------------------: | ------------: |
| Code bytes | 38,708 | 38,516 | 191,400 | 185,624 |
| Instructions | 9,649 | 9,601 | 47,685 | 46,245 |
| Loads | 2,655 | 2,579 | 18,139 | 16,701 |
| Stores | 1,976 | 2,003 | 13,539 | 13,555 |
| Stack-memory operations | 3,473 | 3,463 | 24,437 | 23,628 |

The whole-function store counts rise slightly, but `AddActiveInteraction()` removes 1,438 native
loads, 1,440 instructions, 5,776 code bytes, and 809 stack-memory operations. This is generated-code
evidence rather than a claim that runtime improves by the same proportion.

Two three-iteration focused benchmark orders put Base64's execution phase at approximately 0.295
and 0.290 seconds for the baseline and 0.286 and 0.284 seconds with regions, a combined result about
2.6% in favor of the candidate. SHA-512 remained approximately 0.338--0.339 seconds in both
configurations. CoreMark's two-sample means were approximately 32,049 for the baseline and 31,742
for the candidate, a 1.0% difference treated as noise rather than an established regression. JSON
and Regex were neutral, and the predominantly interpreted Blake3 case was excluded from the
focused round.

Two fresh d3wasm native-compilation samples averaged 3.005841 seconds for the baseline and
3.081508 seconds for the candidate, a raw 2.5% increase. The sample is too small and noisy to
establish an alias-analysis compilation regression. Rust formatting, checking, and Clippy passed,
as did all ten focused native execution tests and the broader 15-suite LibWasm harness.

The diagnostic compiler is retained at
`/private/tmp/cranelift-compiler-0.134.3-alias-regions-diagnostic`. Its optimized CLIF and native
dumps are retained as `/private/tmp/{unz,addactive}-ladybird-alias-regions.clif` and
`/private/tmp/{unz,addactive}-ladybird-alias-regions-native.txt`.

#### Immutable runtime metadata

Commit `c366284cdb8` performs the separate `readonly` audit on top of the region checkpoint.
Cranelift defines a `readonly` load as having no memory dependencies and requires the dereferenced
storage not to be mutated at any time between function entry and exit. The audit therefore marks
only storage whose lifetime satisfies that complete-function rule:

- `CallableMetadata::defined_type`, `CallableMetadata::module`, and
  `CallableMetadata::compiled_instructions` are initialized when the `Store` allocates the callable
  and are never subsequently assigned; and
- each module instance's canonical type-table entries are installed during instantiation and remain
  stable during execution.

The `CompiledInstructions` pointer field is immutable, but its pointee is not: the nested native
entry remains an ordinary atomic load because background compilation publishes it while execution
is possible. The audit also deliberately excludes:

- table size, table storage, and callable elements, which `table.grow` and `table.set` can change;
- incrementally rebuilt compiled-function-table entries;
- `Configuration` fields, which native and interpreter transitions save, replace, and restore;
- globals and linear memory; and
- other runtime metadata whose complete-activation immutability was not proven.

`WasmMemoryFlags` consequently has a `readonly_runtime_metadata` variant using the same runtime
metadata alias region. The inlined `call_indirect` path applies it to four loads: the callable's
defined type, the expected canonical type, the callable's module, and its `CompiledInstructions`
pointer. It does not apply `can_move`. In particular, callable-field loads are valid only after the
table entry's null check, and their operands do not encode that control dependency; allowing
arbitrary motion could hoist them above the check. Current Wasmtime uses the same `readonly`
without `can_move` shape for comparable function-reference metadata loads.

The cache blob format changed from 29 to 30.

#### Structural and timing result

The candidate emits eight readonly loads in `unzReadCurrentFile()` and 200 in
`AddActiveInteraction()`, corresponding to two and fifty inlined indirect-call sites respectively.
No other structural metric changes:

| Optimized-CLIF metric | `unzReadCurrentFile()` baseline | Readonly metadata | `AddActiveInteraction()` baseline | Readonly metadata |
| --------------------- | ------------------------------: | ----------------: | ---------------------------------: | ----------------: |
| Blocks | 905 | 905 | 2,137 | 2,137 |
| Instructions | 6,927 | 6,927 | 24,584 | 24,584 |
| Values | 5,566 | 5,566 | 19,620 | 19,620 |
| Memory operations | 1,043 | 1,043 | 6,132 | 6,132 |
| Loads | 597 | 597 | 3,650 | 3,650 |
| Stores | 446 | 446 | 2,482 | 2,482 |
| Block parameters | 11,675 | 11,675 | 22,296 | 22,296 |
| Readonly operations | 0 | 8 | 0 | 200 |

After removing only the printed `readonly` tokens, each optimized CLIF dump is byte-for-byte equal
to its baseline apart from the diagnostic dumper's final newline. Native code bytes, load and store
counts, and stack-memory-operation counts are also unchanged. This checkpoint supplies correct
optimizer metadata but does not claim a standalone execution improvement.

Three alternating fresh d3wasm compilations per compiler reported:

| Sample | Alias-region baseline | Readonly metadata |
| -----: | --------------------: | ----------------: |
| 1 | 3.055390 s | 3.179620 s |
| 2 | 3.094838 s | 3.470931 s |
| 3 | 3.039338 s | 3.026863 s |
| Raw mean | 3.063189 s | 3.225805 s |

The candidate's 3.027--3.471-second spread and the final reversed result do not establish either a
compilation improvement or a regression. No execution benchmark is interpreted as evidence here
because the sampled functions' emitted native structure is unchanged.

Rust formatting, unit tests, and Clippy with `-D clippy::all` pass. All ten
`TestWasmExecution` cases pass with the ordinary compiler outside the sandbox, and the broader
LibWasm harness passes all 15 suites with 21 tests passed and 38 unsupported tests skipped. The
temporary dump hook was removed before the ordinary compiler was rebuilt and retested.

The diagnostic compiler is retained at
`/private/tmp/cranelift-compiler-0.134.3-readonly-metadata-diagnostic`. Candidate CLIF and native
dumps are retained as `/private/tmp/{unz,addactive}-ladybird-readonly-metadata.clif` and
`/private/tmp/{unz,addactive}-ladybird-readonly-metadata-native.txt`.

The next primary workstream is the local/register attribution diagnostic from optimization idea 3:
measure which frontend state accounts for the excess block parameters before changing lowering.
Any `can_move` annotation remains an independent experiment requiring its own address-validity and
control-dependency proof.

### Optimized block-parameter attribution

A temporary diagnostic attributed final, post-optimization Cranelift block parameters back to the
frontend `Variable` that caused `cranelift_frontend`'s SSA builder to create them. This tracks the
SSA builder's final variable-definition map rather than counting `use_var()` calls: block sealing
can recursively create, alias, and remove parameters, so use-site counts do not describe the IR
that reaches code generation. When one surviving parameter represented equal frontend state from
multiple variables, the report classified it as shared rather than counting it more than once.

With synthetic tier-up enabled, the attribution is:

| Optimized block-parameter source | `unzReadCurrentFile()` | `AddActiveInteraction()` |
| -------------------------------- | ---------------------: | -----------------------: |
| Explicit function/block parameters | 172 | 95 |
| Runtime state | 1,236 | 1,200 |
| Wasm locals | 9,093 | 19,019 |
| Bytecode registers | 947 | 1,621 |
| Vstack entries | 35 | 24 |
| Shared frontend state | 192 | 337 |
| **Total** | **11,675** | **22,296** |

The shared rows are mostly local/register and local/vstack equality. Cranelift is already using a
single block parameter for those equal values; splitting the row between its contributing
categories would double-count it. The non-shared local rows include 8,490 `i32` and 603 `i64`
parameters in `unzReadCurrentFile()`, and 13,993 `i32` and 5,026 `f32` parameters in
`AddActiveInteraction()`. The register/vstack duplication targeted by local-read fusion is real,
but it is much smaller than the Wasm-local component.

For a control measurement, the bytecode tier-up threshold was temporarily set so no synthetic
tier-up instructions were generated. Everything else, including native lowering and attribution,
remained unchanged:

| Optimized block-parameter source | `unzReadCurrentFile()` tier-up | No tier-up | Delta | `AddActiveInteraction()` tier-up | No tier-up | Delta |
| -------------------------------- | ------------------------------: | ---------: | ----: | ---------------------------------: | ---------: | ----: |
| Explicit function/block parameters | 172 | 168 | -4 | 95 | 92 | -3 |
| Runtime state | 1,236 | 1,052 | -184 | 1,200 | 996 | -204 |
| Wasm locals | 9,093 | 7,013 | -2,080 | 19,019 | 15,161 | -3,858 |
| Bytecode registers | 947 | 857 | -90 | 1,621 | 1,440 | -181 |
| Vstack entries | 35 | 21 | -14 | 24 | 24 | 0 |
| Shared frontend state | 192 | 169 | -23 | 337 | 255 | -82 |
| **Total** | **11,675** | **9,280** | **-2,395** | **22,296** | **17,968** | **-4,328** |

Disabling tier-up removes 20.5% of `unzReadCurrentFile()`'s parameters and 19.4% of
`AddActiveInteraction()`'s. Wasm locals account for 86.8% and 89.1% of those respective
reductions. This confirms that OSR predecessors substantially exacerbate local SSA pressure.
However, the no-tier-up functions still contain 7,013 and 15,161 non-shared local parameters, so
OSR is not the root cause of the whole gap.

This changes the ordering proposed in optimization idea 3. Fusing `local.get` into its consumer can
remove the smaller register/vstack copy of a value, but cannot address the dominant local values.
The next lowering experiment should first restore dominance around tier-up resume edges while
preserving interpreter-to-native tiering. After that, a fresh attribution run can distinguish the
remaining genuinely loop-carried locals from locals kept live across regions where a memory home
or frontend liveness boundary would be cheaper.

The tier-up-enabled reports and optimized CLIF are retained under
`/private/tmp/ladybird-param-attribution-v2.2sL8ZR`; the no-tier-up control is under
`/private/tmp/ladybird-param-attribution-no-tier-up.dpMQ8H`. The temporary frontend dependency,
diagnostic code, and disabled tier-up threshold were removed after collection.

### End-to-end synthetic tier-up audit

The block-parameter attribution establishes the generated-code cost of the current OSR entries,
but does not establish how often the entries rescue an already-running interpreter activation.
The complete tiering path narrows that benefit:

- WebAssembly validation returns without waiting for Cranelift and submits eager module-wide
  compilation to the thread pool.
- Fresh compilation sends every eligible function to one out-of-process compiler batch. The
  bridge waits for that batch, prepares and links the returned mappings, and only then publishes
  the function entries.
- Every new Wasm-to-Wasm call enters `BytecodeInterpreter::interpret()`, which checks the callee's
  native entry before dispatching bytecode. Once an entry is published, a new nested call therefore
  starts natively even when its caller began in the interpreter.
- A synthetic loop checkpoint only benefits code physically remaining in an activation that
  started before publication. Repeated worker calls and event-loop-driven frame callbacks already
  tier at ordinary function entry.
- Cache records contain the native body generated from the checkpointed bytecode. A warm load has
  a much shorter publication window but continues to pay the multi-entry native-code cost.

This makes d3wasm useful but not representative on its own. It downloads PK4 data and displays a
startup video while native compilation proceeds, so its heavy decompression and frame work may
begin only after native entries are ready. The counterexample is a site that immediately invokes
one long-running decompressor, decoder, crypto routine, or initialization function after
instantiation. If that activation overlaps publication, loop OSR is its only transition to native
code before returning.

#### Eligibility ordering

Commit `511ca8761f4` removes an independent bug before measuring policy. Previously,
`try_compile_instructions()` inserted checkpoints before the validator decided whether the
function was a Cranelift candidate. A sufficiently large function with an eligible loop therefore
received permanent checkpoint dispatches even when reference or SIMD locals/results, multi-value
results, a direct call returning multiple values, or memory64 made native compilation impossible.
`has_tier_up_checkpoints` also caused every invocation to install compiled fault recovery despite
there being no native entry that could ever be published.

The validator now computes the existing Cranelift type and shape candidate first and passes that
single decision into bytecode lowering. Checkpoints additionally require direct-threading support.
The final `cranelift_eligible` flag still requires the lowered stream to be direct, so the eligibility
rules remain in one place rather than being duplicated for checkpoint insertion.

The regression fixture adds a function with a `v128` local, more than 32 lowered dispatches, and an
otherwise eligible loop. Validation confirms that the function is not a Cranelift candidate and
that its dispatch stream contains no `synthetic_tier_up`. The existing eligible function in the
same fixture retains its checkpoint and successfully tiers.

The current non-SIMD d3wasm module contains 11,858 checkpoint instructions both with and without
the eligibility gate. Its checkpointed functions all pass the existing C++ candidate test, so this
is a correctness and general-workload improvement rather than a d3wasm optimization. The count was
obtained structurally with native compilation disabled by selecting `/usr/bin/false` as the
compiler and counting opcode `0xfe00003d` in `wasm --print-compiled` output.

#### Successful-transfer counter

Commit `6eafe4b9999` adds a relaxed process-wide atomic that increments only after a checkpoint
observes a published native entry and is about to hand over its activation. It deliberately does
not count every checkpoint poll: an additional atomic read-modify-write on each interpreted loop
iteration would perturb the workload being measured. `dump-wasm-stats` reports the cumulative
transfer count.

For checkpoint attribution, set `LADYBIRD_WASM_TIER_UP_TRACE=1` in the environment inherited by
WebContent. Each successful transfer prints its module function index and checkpoint dispatch IP:

```text
wasm-tier-up: function=1 checkpoint=22
```

The one-way tier-up test snapshots the count, proves that its interpreter activation increments it
exactly once, and proves that the following fresh native invocation does not increment it. The
trace produces the expected function and checkpoint identity. All 11 `TestWasmExecution` cases
pass, as do `TestWasmMemory`, `TestWasmTable`, and the broader JS Wasm harness.

The next measurement should cover cache misses and hits across distinct workload shapes: immediate
single-activation compute, repeated short calls, asynchronous/network-gated startup such as
d3wasm, and both small and large modules. A transfer count says whether OSR is used; a controlled
wall-clock comparison is still required to say whether the work it rescues justifies the permanent
cached-code cost. If it does, the next compiler experiment remains redirecting each resume edge to
an empty synthesized loop preheader so the fresh/resume merge is not carried on every hot
backedge.

### Follow-up: preheader result and native-frontend pivot (2026-08-06)

The preheader experiment proposed above was performed and rejected. Redirecting fresh entry and
OSR resume through a synthesized merge block does not remove the merge: the two edges provide
different definitions for every live-in value, so those values inherently become merge-block
parameters. Loop-carried values then also remain parameters of the real header because backedges
still target it. The transform therefore duplicates the carried set and can only win dynamically
when a hot loop has substantially more invariant live-ins than carried values.

The all-loop implementation created 35 merge blocks carrying 1,492 parameters before headers
carrying another 361 in `unzReadCurrentFile()`. It created 50 merge blocks carrying 3,657
parameters before headers carrying another 1,907 in `AddActiveInteraction()`. Total optimized
block parameters rose from 4,681 to 5,036 and from 12,396 to 14,299 respectively. Native code grew
by 7.8% and 4.6%. A selective implementation based on function-wide local accesses and loop-local
writes also grew native code by 8.1% and 4.5%; those declared-local proxies did not describe the
actual values live on each header edge. Both implementations were removed. The independently
useful cold markings on the resume block, initial dispatch block, and dispatch-chain tails were
retained in `b62e9941a0f`.

Commit `39bd4085916` then reconstructed the structured bytecode CFG and computed exact backwards
liveness for R0-R7. Normal edges and OSR resume now normalize only registers live at the target.
This reduced `unzReadCurrentFile()` native code by 4.4%, native instructions by 4.7%, and
stack-memory operations by 7.5%. `AddActiveInteraction()` fell by 2.5%, 2.5%, and 2.0%
respectively. The win proves false interpreter-location liveness is a real cost, while its bounded
size shows that eight registers cannot explain the remaining local and native-frame pressure.

Together with the typed-bank, local-home, pressure, no-tier-up, and Cranelift-version experiments,
these results identify a representation ceiling. The native frontend consumes storage locations
allocated for interpreter dispatch, including reused R0-R7 and virtual-stack locations, then
reconstructs typed SSA from them. Further local fusion, preheader selection, and pressure-aware
home policies would continue optimizing inside that state model rather than restore typed Wasm
value identity.

The primary direction is now direct typed lowering from the retained parsed and validated
`Expression`. A typed operand/control-stack walk will construct native SSA without consuming
interpreter register allocation or synthetic bytecode. R0-R7 and interpreter superinstructions
remain useful to the bytecode interpreter. The existing allocated-bytecode compiler remains a
correctness fallback during migration and is the natural compiler for a temporary OSR-enabled
variant: normal and cached calls use a clean direct-lowered entry-only body, while an activation
already running in the interpreter may transfer into temporary code that understands its location
state.

Before migrating instruction families, build a differential harness that runs native-enabled and
compiler-disabled modes and compares results, traps, and observable side effects. During partial
coverage, per-function eligibility falls through from the direct frontend to the allocated-bytecode
frontend and finally the interpreter. Trap codes, alias regions, native calls, relocations, caching,
incremental publication, and trap recovery remain shared infrastructure; state-model refinements
to the old frontend are frozen unless required for correctness.
