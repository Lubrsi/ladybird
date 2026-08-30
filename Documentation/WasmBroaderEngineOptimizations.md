# Broader engine optimization after the direct Wasm frontend

Status: first-pass investigation, updated 2026-08-29.

## Scope

The direct Wasm-to-Cranelift frontend has changed the shape of the remaining d3wasm performance
work. The generated native code now preserves typed Wasm value flow and no longer carries the
bytecode interpreter's allocated register and stack representation through ordinary execution.
The next investigation should therefore measure the engine work around the guest rather than
assuming that every remaining frame-time problem belongs to the Wasm compiler.

This does not mean minimizing the percentage attributed to native Wasm. Doom 3's simulation,
visibility, lighting, geometry, and gameplay are the useful work of the frame. A healthy profile
can, and probably should, have native Wasm as its largest category. The objective is to reduce
absolute work per visual frame, especially representation conversion and command transport that
Ladybird performs around the game.

The first two broader targets are:

1. WebGL binding and command-record construction; and
2. the Wasm-to-JavaScript and JavaScript-to-Wasm call boundary.

The WebGL work is smaller and has concrete copy and initialization sites. The language boundary
is a larger architectural opportunity enabled by controlling both the Wasm native ABI and the
JavaScript interpreter ABI.

## `d3wasm_new.trace` baseline

`/Users/lukewilde/Documents/d3wasm_new.trace` was captured with high-frequency CPU profiling and
`WebContent Visual Frame` signposts. PID 95495 remained alive after the capture, allowing its hot
JIT mappings to be read and compared with the native cache.

The module is `/Users/lukewilde/Repositories/d3wasm/build-wasm-full/d3wasm.wasm`. Its SHA-256 is
`9927bc955b0455621eb47bb7755e6febff0d30f909c0832a019e4c0b119e3829`, which matches the hash in
the version-33 cache. All 6,482 cache records have `CraneliftFrontend::Direct`; there are no
allocated-bytecode records. Unique live instruction sequences at hot PCs match the cached direct
artifacts for:

- function 1574, `unzReadCurrentFile`;
- function 2445, `idInteraction::AddActiveInteraction()`;
- function 2964, `R_CreateLightTris(...)`;
- function 3909, `idSIMD_Generic::Dot(...)`; and
- function 3930, `idSIMD_Generic::CmpLT(...)`.

The small hot loops use typed integer and floating-point registers and access guest memory
directly. They do not contain interpreter register-bank accesses, full-width `Wasm::Value`
payload/tag traffic, or continuously synchronized canonical locals. The direct frontend is
therefore present in both the cache metadata and the instructions that actually executed.

Run 1 is the loading capture. It contains 20.471 billion main-thread cycle-weight units, of which
`unzReadCurrentFile` accounts for 4.955 billion, or 24.20%. Only 0.093 billion units fall inside
the visual-frame work intervals. The apparent visual update rate during this run does not measure
the loading work, which occurs mainly between those intervals.

Run 2 is the gameplay capture:

| Measurement | Result |
| --- | ---: |
| Complete visual frames | 2,596 |
| Visual update starts | 84.67/s |
| Median update work | 12.349 ms |
| 95th-percentile update work | 15.120 ms |
| 99th-percentile update work | 17.051 ms |
| Maximum update work | 104.824 ms |
| Updates over 8.333 ms | 2,068 / 2,596, 79.7% |
| Updates over 16.667 ms | 35 / 2,596, 1.3% |
| Frame-work/cycle-weight correlation | 0.788 |

This is primarily a steady frame-cost problem rather than a tail of frequent large stalls. The
main-thread leaf distribution inside visual-frame work is:

| Category | Share |
| --- | ---: |
| Wasm JIT | 43.2% |
| JavaScript | 23.4% |
| LibWeb | 14.9% |
| Other/system | 9.8% |
| Wasm runtime | 8.5% |
| Wasm interpreter | 0.2% |

The remaining native share is not itself a regression. Five identified direct functions account
for 11.36% of all main-thread work: `AddActiveInteraction` 4.89%, `R_CreateLightTris` 2.28%, the
unnamed function 3009 2.22%, `CmpLT` 1.12%, and `Dot` 0.85%. These functions perform game and
renderer work and are expected to remain visible after surrounding overhead is reduced.

`wasm_cl_finish_call` appears on 49% of the inclusive stacks. This is ancestry for work performed
beneath the host-call boundary, not 49% self time in that helper. Continue to distinguish leaf
cost from inclusive boundary attribution.

## WebGL command construction

### Current path

The common command path already writes into a shared command buffer rather than constructing and
sending a new IPC buffer for every call:

```text
WebGL binding
    -> WebGLContextProxy method
    -> WebGLContextProxyBase::record()
    -> record_bytes()
    -> WebGLCommandList::write_record()
    -> shared command-buffer data region
    -> later flush to the Compositor
```

`WebGLCommandList::write_payload()` copies the fixed command structure, aligns and copies optional
inline data, zeroes alignment gaps, and zeroes trailing padding. `record_bytes()` also performs
cursor, capacity, flush, and shared-buffer-reuse checks for each record.

Within run 2's visual-frame work:

| Work | Inclusive or leaf share |
| --- | ---: |
| Any `WebGLRenderingContext` binding | 8.42% inclusive |
| `WebGLContextProxyBase::record_bytes` | 2.84% inclusive |
| `_platform_memmove` beneath `record_bytes` | 0.91% leaf |
| `record_bytes` itself | 0.56% leaf |
| `buffer_data` binding | 2.4% inclusive |
| `uniform4fv` overload | 0.9% inclusive |
| `record<Uniform4fv>` | 0.8% inclusive |

The `record_bytes` stack also contains measurable `memset`/`bzero`, alignment calculation,
`padded_record_size()`, and small-copy work. Its share is similar in ordinary 8.33--16.67 ms
updates and slightly smaller in the p99 tail. Command recording is a steady tax, not the cause of
the rare longest updates.

### First experiment: remove typed-list staging copies

Before this experiment, `WebGLRenderingContextBase::span_from_typed_array()` called
`ArrayBuffer::copy_to_byte_buffer()`. Uniform and matrix entry points therefore copied a typed
array into temporary owned storage, then the proxy copied that storage into the shared command
buffer.

The replacement is a callback-scoped typed-list borrowing helper, following the existing
`with_buffer_source_bytes()` design:

```text
current:
    JS Float32Array -> temporary ByteBuffer -> shared WebGL command buffer

candidate:
    borrowed JS Float32Array span ---------> shared WebGL command buffer
```

The borrowed view must not escape the callback, run script, or allocate on the JS heap while held.
The proxy records the inline data synchronously, so the required WebGL snapshot remains the one
copy into shared command memory. Sequence-valued inputs can continue to borrow their converted
`Vector` for the same bounded call.

This is applied generally to the float, signed-integer, and unsigned-integer list helpers rather
than special-casing `uniform4fv` for d3wasm. Focused tests cover direct ArrayBuffer borrowing,
sequence storage, offsets, length overrides, detached buffers, and out-of-bounds views.

On 2026-08-30, a release-build microbenchmark performed one million conversions of a 16-element
`Float32Array`, with ten repetitions:

| Typed-list conversion | Mean and deviation | Range | Approximate time per conversion |
| --- | ---: | ---: | ---: |
| Temporary `ByteBuffer` copy | 42.4 +/- 9.1 ms | 41--60 ms | 42.4 ns |
| Callback-scoped borrow | 21.6 +/- 9.3 ms | 19--43 ms | 21.6 ns |

The callback-scoped path reduced this focused cost by approximately 49%. A pointer-identity test
also verifies that the common contiguous-ArrayBuffer path views the original backing storage. The
underlying `ArrayBuffer::with_readonly_bytes()` abstraction retains its temporary-copy fallback
for a non-contiguous `MemoryBuffer`; the WebGL helper no longer imposes that copy unconditionally.
The final synchronous copy into shared WebGL command memory remains unchanged.

### Required copy versus removable work

`bufferData`, uniforms, and similar WebGL calls must capture their input before returning because
JavaScript can mutate the source immediately afterward. The copy into asynchronously consumed
command memory is therefore generally semantic work, not automatically redundant work.

Potentially removable work includes:

- a temporary typed-array copy before the command-buffer copy;
- initialization of alignment bytes the decoder never consumes;
- repeated dynamic layout calculation for fixed-size commands; and
- avoidable per-command capacity or publication bookkeeping.

Do not remove padding initialization merely because the decoder should ignore it. First establish
why the stream currently initializes every byte, whether uninitialized shared or IPC-visible bytes
would violate a security invariant, and whether a format carrying logical and padded sizes can
avoid exposing those bytes at all.

Before changing the command format, measure per visual frame:

- command count by `WebGLCommandType`;
- fixed payload, inline payload, internal alignment, and trailing-padding bytes;
- shared-buffer rewinds, flushes, waits, and oversized records; and
- bytes copied by source category, including typed-list staging and final command capture.

Add a command-writer microbenchmark for the frequent fixed-size, uniform, and buffer-upload shapes.
Measure the whole d3wasm frame path as well; a microbenchmark improvement in a 0.9% leaf cannot by
itself explain the approximately 29% reduction needed to move 84.7 updates/s to 120 updates/s.

### Command-stream measurement instrumentation

An opt-in command-stream profile now records the measurements above at the point where commands
enter the asynchronous transport. Enable it by setting `LADYBIRD_WEBGL_COMMAND_PROFILE` to the
number of canvas presentations to aggregate into each report. For example, this reports every 600
presentations while running the local d3wasm page:

```sh
LADYBIRD_WEBGL_COMMAND_PROFILE=600 Build/release/bin/Ladybird http://localhost:8080/d3wasm.html
```

Use `1` for one report per canvas presentation. An empty or invalid value selects 600, and `0`
reports only when the context is restored or destroyed. With the variable unset, no statistics
object is allocated and no report is produced; the ordinary shared-buffer path retains only the
null statistics-pointer check. The full layout breakdown is constructed only when statistics are
enabled.

Each report contains:

- presentation and command counts, including commands per presentation;
- record, header, fixed-payload, inline-payload, internal-padding, and trailing-padding bytes;
- command and byte totals broken down by `WebGLCommandType`;
- shared-buffer flush counts and bytes;
- opportunistic cursor rewinds, capacity wraps, waits, failed waits, and wait time; and
- out-of-line commands, flushes, bytes, and oversized records.

`payload` is the fixed command structure copied into the record. `inline` is source data captured
after that structure. `internal-padding` is alignment between the two, while `trailing-padding` is
the remainder needed to align the whole record. Their sum with the header is exactly `records`.
This distinguishes useful captured bytes from layout overhead without assuming that any padding is
safe to leave uninitialized.

These counters cover the asynchronous command stream written by
`WebGLContextProxyBase::record()`. They intentionally do not yet count synchronous WebGL
request and reply serialization. In `d3wasm_new.trace` run 4, almost all sampled `write_payload`
cost was beneath asynchronous command recording, so this scope captures the current target without
adding unrelated instrumentation. If a later profile makes synchronous serialization material, it
should receive separate counters rather than being mixed into command counts.

The first capture should use the same heavy d3wasm view as the visual-frame trace and report over a
long enough interval to smooth gameplay variation. The resulting command and byte distribution
decides whether the next experiment should target padding, fixed-record construction, buffer
uploads, or transport reuse; the counters themselves do not make that policy decision.

### Heavy-scene command distribution

The first command-profile capture used the same heavy d3wasm scene on 2026-08-30. Consecutive
600-presentation windows were stable. A representative window contained:

| Measurement | Result |
| --- | ---: |
| Commands | 14,071,482, or 23,452 per presentation |
| Record bytes | 1,321,591,456, or 2.20 MB per presentation |
| Shared-buffer flushes | 600 |
| Opportunistic rewinds | 600 |
| Capacity wraps or waits | 0 |
| Out-of-line or oversized records | 0 |
| Internal plus trailing padding | 79,133,888 bytes, or 6.0% of records |

The shared-buffer transport therefore completes and rewinds once per presentation without
backpressure. Buffer reuse, capacity, and out-of-line fallback are not current bottlenecks, and a
command-format redesign aimed only at padding has a low byte-saving ceiling.

The distribution is bimodal. `Uniform4fv`, `VertexAttribPointer`, `BindTexture`, `BindBuffer`, and
`ActiveTexture` account for 77.1% of commands, or approximately 18,081 tiny records per
presentation. Conversely, `BufferData` and `BufferSubData` are only 2.6% of commands but 61.3% of
record bytes. The former group is sensitive to per-record call and small-copy overhead; the latter
mostly measures the required bulk snapshot copy.

### Typed fixed-record writer

The original templated `record(Command const&)` immediately erased the command into a
`ReadonlyBytes` payload. The generic recorder and `write_payload()` therefore saw runtime byte
counts even though each fixed command structure has a compile-time type and size. This caused tiny
command structures and their padding to pass through generic copy and zeroing calls.

The typed writer retains `Command` through the normal shared-buffer path. Reservation, statistics,
capacity handling, and fallback remain centralized. Once reservation returns the destination, the
typed overload emits the header, fixed command, and padding with known-size stores, then performs
the same required inline-data snapshot copy. Out-of-line and oversized commands keep using the
generic serializer. The wire format and Compositor decoder are unchanged.

A release-build microbenchmark used a zero-instruction compiler barrier after every record so the
optimizer could not merge writes from separate simulated WebGL calls. Each result is the mean of
20 repetitions:

| Record shape | Generic writer | Typed writer | Change |
| --- | ---: | ---: | ---: |
| 2,000,000 `ActiveTexture` records | 10.9 ms | 2.1 ms | -81% |
| 2,000,000 `VertexAttribPointer` records | 11.1 ms | 2.5 ms | -77% |
| 2,000,000 `Uniform4fv` records | 15.7 ms | 4.3 ms | -73% |
| 1,000,000 `UniformMatrix4fv` records | 8.9 ms | 2.3 ms | -74% |
| 100,000 4 KiB `BufferData` records | 6.5 ms | 6.5 ms | effectively neutral |

The neutral bulk result is expected: the inline copy dominates a 4 KiB record. The fixed and
uniform results isolate the overhead visible in the command-count distribution. ARM64 inspection
also confirms that a production `ActiveTexture` record uses direct stores for its header, payload,
and padding rather than calling the generic payload copier. Inline uniform records use direct
stores for the fixed portion and retain only the required inline copy and trailing zeroing.

This benchmark measures the writer, not the whole WebGL binding. The application-level effect must
be measured without `LADYBIRD_WEBGL_COMMAND_PROFILE`, since its per-command counters intentionally
add work to the same hot path.

The first application capture after adding the typed writer was `d3wasm_new.trace` run 5. Relative
to run 4, sampled `memmove` work per visual update fell by approximately 30.5%, from 0.589 million
to 0.409 million cycle-weight units. However, `prepare_record_destination()` became the largest
named leaf at 0.647 million units per update. Visual update starts fell from 86.864/s to 81.409/s
and mean update work rose from 11.329 ms to 12.118 ms. The workload is variable enough that these
frame totals alone would be inconclusive, but inspection found a concrete implementation
regression in the new hot symbol: even with profiling disabled, record-size calculation called
`record_layout()`, returning and initializing a five-field structure solely to use its final field.
Run 5 therefore confirms that the generic fixed-payload copy was removed, but is not a valid
measurement of the typed writer without the accidental layout overhead.

The follow-up keeps full layout construction behind the enabled statistics branch and restores the
scalar record-size calculation. It also splits reservation into an inlined ordinary path and an
out-of-line `prepare_record_destination_slow()` path. Fixed-size commands carry a compile-time
record size. Inline-data commands calculate their aligned size directly. The ordinary path checks
that the context and shared buffer are valid, profiling is disabled, no rewind is pending, and the
record fits in the remaining region, then returns the destination without a function call. Context
loss, profiling, out-of-line transport, oversize, a just-flushed cursor, and capacity wrap retain
the centralized slow path and its existing behavior.

ARM64 inspection verifies both sides of the split. `ActiveTexture` has no runtime size calculation,
and `Uniform4fv` reduces its dynamic aligned record size to an add and mask. Neither calls the slow
reservation path during ordinary recording.

`d3wasm_new.trace` run 6 measured the split reservation path in the same heavy scene. The accidental
run 5 regression disappeared: visual update starts recovered from 81.409/s to 86.384/s, mean update
work fell from 12.118 ms to 11.413 ms, and the reservation function disappeared from the top 50
named leaves. Against the pre-typed-writer run 4, the whole-frame result is effectively neutral:
86.384 versus 86.864 updates/s and 11.413 versus 11.329 ms mean work. The distribution is also
mixed, with p90 work improving from 13.811 ms to 13.642 ms, while p99 rose from 16.500 ms to
16.944 ms. Updates exceeding the 8.333 ms 120 Hz budget fell from 79.1% to 77.3%.

The local WebGL result is clearer than the whole frame. LibWeb leaf work per sampled visual update
fell by approximately 3.5% from run 4, and sampled `memmove` work per completed update fell by
approximately 37.5%, from 0.589 million to 0.368 million cycle-weight units. Thus the typed writer
and reservation split removed the targeted serialization work, but that work was not large enough
to move total frame time reliably by itself. The largest remaining named WebGL leaf is
`WebGLRenderingContextImpl::bind_buffer()` at 0.63% of total sampled cycles; there is no replacement
command-serialization hotspot of comparable size to run 5's reservation regression.

The remaining leaf profile is broad rather than dominated by another WebGL command-writer
operation. The highest raw JIT leaf in the exported run-6 profile is `0x1765300d1`. Matching its
live instructions against the version-33 native cache identifies direct-frontend Wasm function
3009. Its hot loop performs floating-point plane/vector dot-product work matching the
`idSIMD_Generic::Dot` family. Useful guest computation appearing above individual serialization
helpers is the intended profile shape, not a new Wasm regression.

This distribution makes the Wasm-to-JavaScript boundary the next cross-cutting target. Much of its
cost is expected to be spread across Wasm call fallback, argument containers, value conversion,
JavaScript call setup, WebIDL conversion, and the eventual WebGL method rather than concentrated in
one leaf. A faster boundary can reduce the common prefix paid by many different WebGL calls, while
another command-specific optimization can affect only its own smaller leaf.

The command rate makes apparently small boundary costs material. The representative heavy-scene
window contains approximately 23,452 commands per presentation. If each command corresponded to
one Wasm-to-JavaScript crossing, saving 250 ns per crossing would save approximately 5.86 ms per
presentation, or 70% of the complete 8.33 ms budget at 120 Hz. At the measured approximately 86
presentations/s, the same cost consumes about half a CPU-second per second. Commands and boundary
crossings are not guaranteed to be one-to-one, so this is a scale illustration rather than an
attribution result. Even half as many crossings would retain a 2.93 ms-per-presentation ceiling.

## Wasm-to-JavaScript calls

### Current generic path

An imported JavaScript function is currently stored as a `Wasm::HostFunction`. A direct native
Wasm call that cannot resolve to another compiled Wasm body enters its generated fallback. The
fallback writes full-width `Wasm::Value` arguments to the `Configuration` value stack, calls
`wasm_cl_current_interpreter()`, and enters `wasm_cl_call_function()`. That helper routes through
`BytecodeInterpreter::call_address()`, which rebuilds an owned `Vector<Wasm::Value>`, calls through
`Configuration::call()`, and eventually invokes the host-function closure created by
`create_host_function()`.

The closure then builds a `GC::RootVector<JS::Value>`, calls `to_js_value()` for every argument,
enters the generic `JS::call()` operation, and converts its completion back into a
`Vector<Wasm::Value>`. Multi-value results additionally use the JavaScript iterator protocol, as
required by the JS API. In simplified form:

```text
typed values in a direct Wasm body
    -> interpreter-compatible Wasm argument storage
    -> wasm_cl_call_function()
    -> BytecodeInterpreter::call_address()
    -> owned Vector<Wasm::Value>
    -> Wasm::HostFunction
    -> rooted vector of JS::Value arguments
    -> generic JS::call()
    -> JavaScript execution frame and callee
    -> JS result/completion
    -> Vector<Wasm::Value> or Wasm trap
```

Some of those steps express required semantics, while others are consequences of entering
JavaScript through a representation-neutral host-function interface. The proposed work should
measure and remove the latter without constructing a second JavaScript execution model.

### Boundary model

The conceptual boundary is similar to the bytecode-interpreter-to-native Wasm boundary: translate
the arguments and result once, then follow the callee's established ABI. The important difference
is where frame construction ends.

A clean native Wasm callee uses the machine calling convention and its Cranelift-generated native
frame. It does not require an interpreter activation record. A JavaScript callee must still have a
JavaScript execution frame even if a native Wasm instruction performs the call. That includes an
`ExecutionContext`, its register/local/constant/argument slots, realm and environment state,
caller linkage, `this`, exception state, stack-limit checks, and GC-visible `JS::Value` roots.

The intended shape is therefore:

```text
typed direct-Wasm call site
    -> typed per-import adapter
    -> convert Wasm arguments directly into JS::Value argument slots
    -> construct the normal JavaScript frame
    -> enter the existing JavaScript callee ABI
    -> receive JS completion/result
    -> convert the result to the declared Wasm result type
```

This avoids using interpreter-oriented `Wasm::Value` vectors as an intermediate native-call ABI.
It does not avoid JavaScript semantics or frame construction.

### Relevant existing JavaScript machinery

Ladybird does not currently have a JavaScript JIT, but it also does not execute each bytecode
operation through an ordinary C++ virtual call. `Libraries/LibJS/Interpreter/interpreter.flap` is
compiled ahead of time into the native `js_interpreter` assembly routine and shared native handler
implementations. The bytecode remains bytecode; an opcode dispatches to its well-known generated
handler. This is an assembly interpreter, not per-function native compilation.

`VM::run_executable()` supplies `js_interpreter` with the bytecode pointer, entry byte offset,
value-slot pointer, and `VM*`. This is already an external native entry into JavaScript bytecode
execution.

The hot JavaScript call paths provide two closer precedents:

- Direct JavaScript-to-JavaScript bytecode calls allocate an inline `ExecutionContext` on the
  interpreter stack, copy arguments into its slots, establish caller linkage and environments,
  switch `VM::m_running_execution_context`, and branch to the callee's first bytecode handler. They
  do not leave the assembly interpreter and re-enter through the generic `JS::call()` path.
- Calls to `RawNativeFunction` allocate a lightweight `ExecutionContext`, populate its JS argument
  slots and caller metadata, select a native function pointer from the VM's native-function table,
  and issue `call_raw_native`. Its tagged native result distinguishes an ordinary `JS::Value` from
  an exception, after which the assembly interpreter restores the caller frame.

The second path proves that a JavaScript frame and a CPU-native call frame can coexist without
routing every call through the generic C++ entry. The first path shows the frame state required to
enter an ordinary ECMAScript bytecode function efficiently.

A Wasm-originating call is not identical to either path. It begins outside the live JavaScript
assembly-interpreter register state, even when the outermost Wasm invocation originally came from
JavaScript. An initial prototype should therefore use a native adapter that constructs the normal
frame and calls an external `js_interpreter` entry. A later dedicated external-entry trampoline
could restore the interpreter's pinned state and branch to the first handler if measurement shows
that the extra native call/entry prologue matters.

### Per-instance import entries

The compiled Wasm module is cacheable, while imported JavaScript function objects belong to a
particular instance and realm. Do not patch cached live code with instance-specific JS pointers.

A likely contract is a per-instance import-entry table. Each entry can describe:

- the captured callable and its realm;
- the callable kind and specialized entry, if any;
- the declared Wasm function type;
- adapter or signature metadata; and
- a generic fallback target.

The direct Wasm call site can load the entry by import index and perform an indirect native call.
Ordinary ECMAScript functions, raw native functions, and generic/exotic callables may use different
adapters while sharing the same typed Wasm-facing signature. The table keeps instance-specific
state out of cached code and avoids patching code after publication.

The first prototype should be deliberately narrow: a low-arity numeric signature calling a stable
ordinary ECMAScript function. It should convert register arguments directly into the new
`ExecutionContext`'s argument slots and use the existing JS bytecode entry. Unsupported signatures
or callable kinds continue through the current generic host-function path.

### Semantics that remain mandatory

Controlling both internal ABIs permits specialization; it does not permit skipping observable
language behavior. The adapter must preserve:

- Wasm-to-JS and JS-to-Wasm numeric conversions, including `i64`/`BigInt`;
- reference rooting and future GC-reference handling;
- the imported function's realm and environment;
- `this` and passed-versus-formal argument counts;
- JavaScript exceptions converted into Wasm host-call traps and vice versa;
- multi-value return iteration;
- native and interpreter stack-exhaustion checks;
- reentrant Wasm/JavaScript calls;
- debugger, profiler, and mixed-language stack visibility; and
- future suspension behavior such as JSPI.

An import may contain arbitrary JavaScript glue. Entering its bytecode directly does not justify
skipping that glue or the WebIDL/WebGL binding it calls. A future specialized path to a raw native
host function can bypass generic dispatch only when it preserves the captured callable's actual
semantics.

### Measurement plan

Use separate microbenchmarks for:

- a trivial ordinary JavaScript import at several numeric arities;
- an import returning a value;
- a throwing import;
- a raw native JavaScript function;
- a JavaScript wrapper that reaches a WebGL binding; and
- reentrant JavaScript-to-Wasm-to-JavaScript calls.

Measure calls per second and absolute nanoseconds per call, but retain the d3wasm visual-frame
profile as the application check. Attribute separately:

- argument/result conversion;
- `ExecutionContext` allocation and initialization;
- JavaScript entry and return;
- generic callable dispatch;
- WebIDL conversion; and
- WebGL command construction.

### Initial numeric-import baseline

The restored `WasmMicroBench` import matrix establishes the first direct-frontend baseline. Each
Wasm function calls a trivial ordinary JavaScript function 50 million times. Three executions per
case were run through the release `wasm` CLI with `--benchmark-timings`; every recorded execution
had a nonzero native-compilation phase and reported the single loop function being submitted to and
received from Cranelift. A sandboxed trial that could not launch Cranelift was discarded.

| Imported function signature | Mean execution | Time per call |
| --- | ---: | ---: |
| `() -> ()` | 2.312 s | 46.24 ns |
| `(i32) -> ()` | 2.741 s | 54.83 ns |
| `(i32, i32) -> ()` | 2.882 s | 57.65 ns |
| `(i32, i32, i32) -> ()` | 3.053 s | 61.06 ns |
| `(i32, i32, i32, i32) -> ()` | 3.327 s | 66.54 ns |
| `(8 * i32) -> ()` | 3.787 s | 75.73 ns |
| `(16 * i32) -> ()` | 4.972 s | 99.44 ns |
| `(32 * i32) -> ()` | 6.679 s | 133.57 ns |
| `(4 * f32) -> ()` | 3.323 s | 66.47 ns |
| `(4 * f64) -> ()` | 3.393 s | 67.86 ns |
| `(i32, f32, i32, f32) -> ()` | 3.282 s | 65.63 ns |
| `() -> i32` | 3.040 s | 60.79 ns |
| `() -> f64` | 2.979 s | 59.57 ns |
| `(4 * i32) -> i32` | 3.970 s | 79.39 ns |

Two controls make the fixed cost visible. The existing native Wasm-to-Wasm call benchmarks take
approximately 1.00 ns with no arguments and 1.25 ns with four `i32` arguments. The corresponding
JavaScript-to-JavaScript empty-call loops take approximately 8 ns per call including CLI wall-time
overhead. The zero-argument Wasm-to-JavaScript path therefore spends roughly 38 ns beyond an
ordinary JavaScript call and roughly 45 ns beyond a native Wasm call.

Argument cost grows with arity, but the four-argument results do not show a meaningful penalty for
`f32`, `f64`, or mixed numeric types relative to `i32`. A scalar result adds approximately 13 ns.
This first decomposition points at the fixed Wasm host-call path and result container/conversion
work before any type-specific numeric optimization. At the representative 23,452-crossing scale,
the measured 46.24 ns zero-argument intercept alone corresponds to approximately 1.08 ms per
presentation; the earlier 250 ns example is therefore a ceiling illustration, not the measured
current boundary cost.

### Zero-argument boundary profile

A command-line Time Profiler capture of the zero-argument case sampled the 2.31-second execution at
1 ms intervals. The persistent Instruments analyzer now accepts both the GUI CPU Profiler's
`cpu-profile`/`cycle-weight` schema and the command-line Time Profiler's equivalent
`time-profile`/`weight` schema, so the same attribution workflow applies to both captures.

The profile directly confirms that the fixed cost is primarily the generic interpreter-compatible
call boundary. `wasm_cl_call_function()` covers 90.6% of main-thread samples inclusively, and
`BytecodeInterpreter::call_address()` covers 70.4%, despite the call having no arguments and no
result. The leaf distribution includes:

- `BytecodeInterpreter::call_address()` at 5.18%;
- `Configuration::get_arguments_allocation_if_possible()` at 4.35%;
- `_platform_memset_pattern16` at 4.26%;
- `JS::VM::run_executable()` at 4.05%;
- `JS::call_impl()` and `ExecutionContext` construction at 2.80% each; and
- JS argument-vector capacity handling at 2.76%.

The exact leaf percentages are sampling estimates and the CLI's host closure is not identical to
LibWeb's rooted WebAssembly host closure. The two large inclusive frames are shared runtime code,
however, and make the first experiment unambiguous: bypass `call_address()` and the configuration
value stack for direct low-arity host-call fallbacks, while retaining the generic path for
unsupported signatures and semantics. Existing `wasm_cl_direct_call_0` through
`wasm_cl_direct_call_3` helpers already pass scalar payloads directly to `wasm_cl_finish_call()` on
an uncompiled or host target. Reusing them from the direct frontend is a bounded way to test the
attribution before designing the durable per-import JavaScript adapter.

The first success criterion is not a direct branch instruction by itself. It is removal of generic
Wasm call machinery and intermediate containers while producing the same results, exceptions,
side effects, GC roots, and frame behavior through the normal JavaScript ABI.

### Low-arity direct-host fallback experiment

The first implementation deliberately reused the existing `wasm_cl_direct_call_0` through
`wasm_cl_direct_call_3` helpers. Direct-frontend static-call fallbacks with zero through three
numeric arguments now pass their typed values as 64-bit payloads instead of materializing complete
16-byte `Wasm::Value` entries on `Configuration::value_stack()`. The existing value-stack path
remains the fallback for four or more arguments. Scalar results return through
`compiled_call_result_scratch`, which the direct-call helpers already maintain.

Reusing the helpers alone was a measured negative result. An alternating five-pair run of the
zero-argument benchmark produced 2.320 s for the old path and 2.374 s for the helper path: a 2.35%
regression. Matched Time Profiler captures explained why. The new route replaced
`BytecodeInterpreter::call_address()` with the Wasm-to-Wasm-oriented
`wasm_cl_direct_call_impl()`/`wasm_cl_finish_call()` chain. It still copied arguments into a
temporary `Vector<Wasm::Value>` and resolved the host target again through `Configuration::call()`;
`Configuration::get_arguments_allocation_if_possible()` consequently grew from 4.25% to 10.15% of
leaf samples. Removing one generic layer while retaining another was not sufficient.

The refined path recognizes an already-resolved `HostFunction` in the direct-call fallback and
invokes it over the direct helper's private argument array. This is safe specifically because those
arguments are copies owned by the helper. The general call-record and value-stack paths retain
their defensive copy: the internal `HostFunction` ABI takes a mutable `Span<Value>`, so exposing
interpreter-owned argument storage would be an observable semantic change. The fast path retains
the native-stack check, `CompiledCallerContext`, host trap propagation, and result scratch.

Alternating old/new runs of 50 million calls show the refined result:

| Imported function signature | Old path | Direct-host path | Change |
| --- | ---: | ---: | ---: |
| `() -> ()` | 2.319 s (46.37 ns/call) | 1.684 s (33.68 ns/call) | -27.4% |
| `(i32) -> ()` | 2.751 s (55.03 ns/call) | 1.898 s (37.95 ns/call) | -31.0% |
| `(i32, i32, i32) -> ()` | 2.995 s (59.91 ns/call) | 2.040 s (40.79 ns/call) | -31.9% |
| `() -> i32` | 3.070 s (61.39 ns/call) | 2.276 s (45.52 ns/call) | -25.9% |
| `(i32, i32, i32, i32) -> ()` | 3.275 s | 3.255 s | -0.6% (control noise) |

The compiler-only A/B switch used for these measurements was removed. Three final runs of the
retained zero-argument path averaged 1.674 s (33.47 ns/call), and three `f64`-result runs averaged
2.291 s. The four-argument control confirms that the result comes from the intended zero-to-three
argument path rather than a global runtime shift.

In the optimized zero-argument profile, `BytecodeInterpreter::call_address()` and
`Configuration::get_arguments_allocation_if_possible()` disappear from the leading leaf symbols.
Wasm-runtime leaf samples fall from 38.05% of a 2.37-second sample window to 23.72% of a 1.79-second
window, while JavaScript itself becomes the largest category at 46.28%. Inclusive direct-call
frames still enclose the host invocation, so their high inclusive percentages must not be read as
self cost.

This is still an intermediate boundary, not the proposed direct JavaScript adapter. Every call
loads the current interpreter, enters a generic direct-call helper, checks the compiled-function
table, resolves the module function, and invokes the generic `HostFunction` closure. The experiment
nevertheless proves that removing representation conversion and redundant host dispatch is worth
roughly 13--19 ns per low-arity call. At 23,452 calls per presentation, the zero-argument saving
alone would be approximately 0.30 ms if command count and boundary-crossing count were one-to-one.
The next boundary experiment should target those remaining helper and callable-dispatch layers,
while preserving the generic fallback for unsupported callable kinds and signatures.

Because this changes emitted fallback code while the cache key otherwise accepts the old native
bytes, the native-code cache format advances from version 33 to 34.

### Measurement strategy: microbenchmarks first

Further boundary work should use the import microbenchmarks as its primary iteration loop before
returning to d3wasm. They isolate one call shape, verify that the caller was compiled by the direct
frontend, and report execution separately from parsing and native compilation. Most importantly,
they distinguish removing a named frame from reducing the total cost: the helper-only experiment
removed `BytecodeInterpreter::call_address()` but regressed, whereas removing the argument
container and redundant host resolution improved the same benchmark by 26--32%.

The matrix should separate these increasingly complete boundaries:

1. a raw native `HostFunction` with no JavaScript execution;
2. a trivial ordinary JavaScript function;
3. a JavaScript wrapper which reaches a native or WebGL binding; and
4. the representative WebGL wrapper shapes used by d3wasm.

Each layer should cover zero through several numeric arguments, scalar results, mixed numeric
types, traps or JavaScript exceptions, and re-entrant JavaScript-to-Wasm calls. Native
Wasm-to-Wasm and JavaScript-to-JavaScript calls remain controls for the fixed costs on either side
of the boundary. Every Wasm run must continue to report nonzero native compilation and direct
frontend selection so an interpreter fallback cannot masquerade as a boundary result.

d3wasm remains the application-level correctness and relevance check, rather than the primary
signal for each small change. Re-profile it after a microbenchmark change removes a complete layer,
saves several nanoseconds across common signatures, or changes the expected hot-path shape. Its
visual-frame profile then answers whether the isolated saving occurs frequently enough in a real
Wasm-to-JavaScript-to-WebIDL-to-WebGL chain to move frame time. This separation also avoids treating
the measured WebGL command count as an exact Wasm-to-JavaScript crossing count; the two may be
correlated without being one-to-one.

## Suggested order

1. Separate raw native host functions, ordinary JavaScript functions, and WebGL-reaching wrappers
   in the microbenchmark matrix.
2. Document the exact external JS interpreter entry, inline-frame, raw-native return, exception,
   GC, and stack-walking contracts.
3. Prototype one cached typed Wasm-to-ordinary-JavaScript import adapter behind an opt-in selector,
   with the generic `HostFunction` boundary as fallback.
4. Re-run the Wasm import microbenchmarks and the d3wasm visual-frame capture before expanding
   signatures or callable kinds.
