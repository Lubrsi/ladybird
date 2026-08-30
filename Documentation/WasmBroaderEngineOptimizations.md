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

## Wasm-to-JavaScript calls

### Current generic path

An imported JavaScript function is currently stored as a `Wasm::HostFunction`. A direct native
Wasm call that cannot resolve to another compiled Wasm body falls back through
`wasm_cl_finish_call()`. That path rebuilds an owned `Vector<Wasm::Value>`, calls through
`Configuration::call()`, and eventually invokes the host-function closure created by
`create_host_function()`.

The closure then builds a `GC::RootVector<JS::Value>`, calls `to_js_value()` for every argument,
enters the generic `JS::call()` operation, and converts its completion back into a
`Vector<Wasm::Value>`. Multi-value results additionally use the JavaScript iterator protocol, as
required by the JS API. In simplified form:

```text
typed values in a direct Wasm body
    -> interpreter-compatible Wasm argument storage
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

The first success criterion is not a direct branch instruction by itself. It is removal of generic
Wasm call machinery and intermediate containers while producing the same results, exceptions,
side effects, GC roots, and frame behavior through the normal JavaScript ABI.

## Suggested order

1. Add the callback-scoped typed-list borrowing path and benchmark uniform/matrix-heavy calls.
2. Measure WebGL command counts, logical bytes, padding, copies, and flush behavior per visual
   frame.
3. Evaluate a safe command-stream padding or fixed-record fast path from those measurements.
4. Document the exact external JS interpreter entry, inline-frame, raw-native return, exception,
   GC, and stack-walking contracts.
5. Prototype one typed Wasm-to-ordinary-JavaScript import adapter behind an opt-in selector, with
   the generic host-function boundary as fallback.
6. Re-run the Wasm import microbenchmarks and the d3wasm visual-frame capture before expanding
   signatures or callable kinds.
