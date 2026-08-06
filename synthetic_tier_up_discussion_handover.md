# Synthetic tier-up mechanism — discussion handover

Discussion (2026-08-05, branch `wasm-opt-stuff2`) about whether the current synthetic tier-up
(interpreter→native OSR at loop back-edges) is worth its cost, and what to do instead. Luke's
starting position: neat idea, but it doesn't account for compilation speed and permanently
punishes native code — especially cached native code — to fund a one-time jump. The code and the
measurements in `Documentation/WasmCraneliftVStackBaseline.md` ("Optimized block-parameter
attribution", ~line 4380) back that position up. Line numbers below are as of this discussion.

Updated same day with an empirical Ruffle measurement from a parallel session (zero OSR firings
on a real cold load) and a resulting reordering: publication latency first, OSR verdict second.

## How the mechanism works today

- `try_compile_instructions` inserts a `synthetic_tier_up` op after every eligible loop header
  (`tier_up_eligible` = loop with no params and empty stack at the header, set in
  `Validator.cpp:2427`) in any function with ≥ 32 dispatches
  (`BytecodeInterpreter.cpp:7690–7758`, threshold at 7692). The back-edge hits the checkpoint
  every iteration.
- Interpreter handler (`BytecodeInterpreter.cpp:2065`): atomic load-acquire of the cranelift
  entry; if published, hands the activation to native code (target checkpoint recovered from
  `short_ip`; contract is empty operand stack + canonical locals) and the frame completes in
  native. If not published, falls through — so cost is one extra dispatch + atomic per loop
  iteration while interpreting.
- Native side (`Rust/src/compiler.rs:2574–2606`, `4145–4179`): when a function has checkpoints it
  becomes multi-entry — an entry-token compare, a fresh path (zero-init locals) vs a resume path
  (`init_locals_resume!` loads every local from canonical memory), and a linear dispatch chain
  whose edges jump straight into each loop header. `imm1` on the checkpoint insn carries the
  dispatch index (`CraneliftBridge.cpp:1632`).
- Compilation still targets all eligible functions by policy, but commit `6dd79f92633` submits
  dependency-ordered batches to the out-of-process `cranelift-compiler` and publishes each batch as
  it returns. The interpreter can therefore enter already-published functions while later batches
  are still compiling. The OSR window for a function is now its own batch's publication latency on
  a cold start and approximately blob-install time on a warm start.
- Cache: blob fetched from HTTP cache *before* `validate()` runs (`WebAssembly.cpp:490–515`);
  installed per-function inside `try_cranelift_compile` (`CraneliftBridge.cpp:1502`). The blob
  contains code compiled *from the instrumented dispatches* with resume indices baked in.

## Costs — established in discussion

1. **Measured codegen tax (the big one).** The attribution section of
   `WasmCraneliftVStackBaseline.md` shows disabling tier-up removes ~20% of final optimized block
   parameters (2,395/11,675 in `unzReadCurrentFile()`, 4,328/22,296 in `AddActiveInteraction()`),
   ~87–89% of the reduction attributed to Wasm locals. Cause: the resume edges land on loop
   headers, so every local live at a header becomes a loop-carried phi (invariant locals
   included), raising register pressure exactly at the hottest blocks and blocking
   LICM/GVN across the entry edge. Also: `body_start` merges fresh/resume so zero-init locals no
   longer constant-fold; header register banks are pinned to a canonical-materializable shape
   (`reg_ty = header_reg_ty`, compiler.rs:4177); every normal entry pays the entry-token branch.
   **Important correction from the control run:** OSR is the *marginal* tax, not the base rate —
   with tier-up off the probe functions still carry 7,013 / 15,161 non-shared local params. The
   dominant local-pressure problem is the baseline lowering keeping every local a frontend
   `Variable` merged at every join, independent of tier-up. (This reorders optimization idea 3:
   local-read fusion attacks the small register/vstack component.)
2. **Eligibility-ordering bug.** Checkpoints are inserted at `Validator.cpp:5132` but
   `cranelift_eligible` is computed *afterwards* at 5142–5180 and never consulted. Functions that
   can never compile (v128/ref locals, multi-value, i64 memories, calls to multi-value) are
   instrumented forever: per-iteration checkpoint that can never fire, plus — because
   `may_run_native` keys off `has_tier_up_checkpoints` alone (`BytecodeInterpreter.cpp:482`) — a
   setjmp + fault-recovery TLS install on every host call. Same for functions the Rust side
   rejects, or when compilation is disabled.
3. **Cache lock-in.** Warm starts (OSR window ≈ 0) re-pay the full multi-entry tax every run,
   because the blob bakes in the instrumented structure. Changing instrumentation policy
   invalidates blobs (dispatch indices shift), so any "skip instrumentation when cached" scheme
   needs blob versioning / two flavors.
4. **No accounting for compile speed.** The ≥32-dispatch threshold instruments essentially every
   function with a loop, identically for a module whose batch compiles in milliseconds and one
   that takes seconds. Nothing measures the window the instrumentation exists to cover.

## Key mechanical finding: nested calls already tier at entry

Verified: every wasm→wasm call from an interpreted frame goes `call_address`
(`BytecodeInterpreter.cpp:6785`) → `Configuration::call` → `execute` → `interpret()`, and
`interpret()` checks `cranelift_entry_acquire` at the top (`BytecodeInterpreter.cpp:480–506`;
intent confirmed by the comment in `prepare_wasm_call`, Configuration.cpp). So once the batch
finishes, all *new* calls — including callees of interpreted frames — run native. Without OSR,
the only residual interpreted work is the code physically inside frames already live when compile
finished (the loop shell + whatever inlining pulled in-frame).

This splits the workloads OSR serves:

- **Driver loops** (game main loop, event pump, per-item worker): OSR ≈ worthless. Callees tier
  at entry; emscripten-style main loops return to the event loop per frame anyway.
- **In-frame compute entered once during the compile window** (decompression, image/PDF decode,
  crypto — one call, one big loop, work in-frame): OSR is the only rescue. Without it the whole
  activation runs interpreted, potentially for seconds, as a cold-start-visible cliff ("site
  hangs in Ladybird, not in Chrome"). Note the doc's probe function is `unzReadCurrentFile()` —
  decompress-assets-right-after-instantiation is exactly this shape and happens exactly when
  native code isn't ready.

## Empirical: Ruffle cold load — zero OSR firings (parallel session, 2026-08-05)

A separate session added tier-up transition prints and measured a real Ruffle cold load:
9,973 checkpoints across 3,109 functions (insertion works), a focused test confirming a live
interpreter frame *can* transition mid-function, batch compile ≈ 2.4 s — and **zero** tier-up
transitions in actual use. Ruffle's work is short JS/rAF-driven calls that return before
publication; after publication, new calls enter native at entry and OSR never gets a chance.

How to read this (the batching-vs-workload untangling):

- The zero count is a property of Ruffle's **short-call shape**, not primarily of batching.
  With streamed per-function publication, Ruffle's OSR count would still be ~zero — calls would
  just start entering native *at entry* sooner. Ruffle is the class OSR was never going to help.
- What the 2.4 s monolithic batch *does* cost is that every call in the window runs interpreted —
  an **entry-tiering latency** problem. Chunked/streamed publication (ideally demand-prioritized:
  the interpreter bumps functions it's actually executing to the front of the compile queue)
  shrinks that for exactly this workload class, and is worth doing regardless of OSR's fate.
  Plausibly worth more than everything else in this discussion combined.
- **Streaming and OSR are complementary, not substitutes.** Streaming can't rescue an in-frame
  long call (a 5 s decompress activation runs 5 s interpreted under entry-only tiering even with
  instant publication — the activation never returns to be re-entered). OSR can't shrink the
  window for short-call workloads. If both exist, streaming makes each OSR firing *more*
  valuable (the long function's code publishes in tens of ms, so the jump lands seconds earlier).
- The parallel session's conclusion ("evidence increasingly favours entry-only") is ahead of the
  evidence: it tested the class where OSR's expected value was zero and measured zero. The
  decisive experiment is the other class — cold-cache instantiate-then-immediately-call-big-
  decompress (zip-heavy site, wasm PDF viewer, game unpacking assets at boot). There OSR fires by
  construction; the open question is how much real web content has that shape and how many
  seconds each firing saves. If real sites never show it, entry-only + delete is right.

## Empirical: delayed incremental publication exposes repeated function-table scans (2026-08-05)

Incremental dependency-ordered batches were tested with a temporary one-second, then three-second,
delay after submitting each out-of-process Cranelift compiler job. Temporary entry tracing showed
interpreted and native function entries interleaved with batch submission and completion, confirming
that WebContent can execute the module while compilation is in progress. However, Ruffle did not
reach its logo until every delayed batch had returned, and increasing the artificial delay made the
stall proportionally worse. This was much slower than known interpreter-only loading, so the result
could not be explained by interpreter throughput alone.

High-frequency CPU profile: `~/Documents/ruffle_batching.trace`, run 1. Disable the per-entry
`LADYBIRD_WASM_ENTRY_TRACE` diagnostic for timing because synchronous output on every Wasm entry is
itself highly perturbing.

- Trace duration: 128.215 s.
- Largest interval between visual-frame starts: 102.641 s, closely matching the accumulated
  three-second per-batch delays.
- The WebContent main thread was consuming CPU rather than blocked. 97.33% of its sampled
  cycle-weight was attributed to Wasm runtime leaves.
- The dominant named leaves were `deref_base` (47.11%), `compare_exchange_strong` (27.00%), and
  `ModuleInstance::compiled_fn_table(Store&) const` itself (12.16%). The first two are the atomic
  reference-count operations reached through `WasmFunction::module_ref()` in the table scan; the
  surrounding `WeakPtr` helpers account for additional samples.
- `BytecodeInterpreter::interpret()` was present in 96.99% of sampled stacks.
  `wasm_cl_call_function`, `BytecodeInterpreter::call_address()`, and
  `BytecodeInterpreter::run_native_entry()` were each present in about 79.6%, showing that mixed-tier
  calls repeatedly drove the expensive frame setup path.

The mechanism is in `ModuleInstance::compiled_fn_table()`. Every Wasm frame created by
`Configuration::set_frame()` requests the table. Until every source module reports that its
Cranelift attempt has finished, `m_compiled_fn_table_built` remains false, so every request scans
every function in the instance. For each Wasm function the scan resolves its weak `Module`
reference, checks compilation state, atomically reads its native entry, and possibly rebuilds the
table entry. Ruffle has roughly 3,109 functions, turning the extended mixed-tier window into an
accidental O(functions × Wasm calls) cost. The start function runs during instantiation, so the
entire interval appears as one visual-frame hang even though interpreter and native execution are
both making progress.

An interpreter-only run that lets the native compilation attempt fail or finish immediately can
freeze the empty compiled-function table after its first completed-state scan. It therefore avoids
the repeated-scan tax, explaining why pure interpretation can load faster than the artificially
delayed mixed-tier run.

The required fix is to make compiled-function-table refreshes publication-aware:

1. Associate native-code publication with a monotonically increasing generation (preferably per
   source module rather than a process-wide approximation).
2. Have each `ModuleInstance` remember the generations represented by its compiled-function table.
3. Reuse the table without scanning functions while those generations are unchanged.
4. After a batch publishes, refresh the table once on its next use so newly native functions become
   visible to compiled callers.
5. When compilation finishes, perform one final refresh and mark the table permanently built.

This is a prerequisite for evaluating incremental batch sizes or scheduling: without it, extending
the publication window measures repeated table reconstruction and mixed-tier bridge amplification,
not the underlying interpreter-versus-native trade-off.

## Empirical: d3wasm visibly benefits from incremental publication and OSR (2026-08-05)

After replacing the repeated compiled-function-table scan with publication deltas, d3wasm provided
a direct qualitative demonstration of both tiering mechanisms working during real execution:

- Loading initially progressed slowly in the interpreter.
- After a native batch published, a `wasm-tier-up` message appeared and collision-model loading
  visibly accelerated, observed through the increased rate of the game's collision-model console
  messages. This proves that an activation already executing during loading transferred to native
  code and then made materially faster progress; it is the long-lived in-frame workload for which
  entry-only tiering cannot help.
- In the heavy gameplay scene, frame rate rose in stages from approximately 47 FPS to 80 FPS and
  then 100 FPS while batches continued to publish.
- At least one `wasm-tier-up` message appeared during that FPS staircase. The staircase therefore
  was not solely new calls finding native entries: a live gameplay activation also transferred to
  native code. The available observation does not attribute each individual step between OSR and
  newly published callees, so retain the combined interpretation rather than assigning unsupported
  percentages.

This changes the synthetic-tier-up verdict. Ruffle's zero-transition cold load showed that OSR is
irrelevant to its short-call workload, but d3wasm demonstrates the complementary workload in which
it is effective. Incremental publication shortens the time until useful native functions exist;
synthetic tier-up then rescues long-running activations that entered before their native code was
available. One does not replace the other.

The next synthetic-tier-up workstream is therefore code-quality containment, not removal. The
mechanism is one-way: an interpreted activation may enter native code, and native completion ends
that frame; there is no supported native-to-interpreter tier-down path whose state must be
continuously maintained. The desired generated-code shape is:

1. Keep normal fresh native entry on the clean hot path.
2. Put OSR entry and checkpoint dispatch in cold resume blocks.
3. Import interpreter locals once when crossing the OSR boundary instead of continuously
   maintaining interpreter-compatible canonical local state during native execution.
4. Once actual per-edge liveness is available, route resume entries through preheaders only when
   doing so removes materially more hot-header state than it duplicates in the merge block. Do not
   use function-wide accessed/written-local counts as a proxy for header-edge liveness.
5. Keep normal native loop bodies identical, or as close as possible, to an entry-only build.
6. Measure enabled-versus-disabled native code and execution for `unzReadCurrentFile`,
   `AddActiveInteraction`, and SHA-512, including cached-code runs that will never use OSR but still
   execute the generated code.

The success criterion is to preserve the observed cold-loading and gameplay transitions while
removing the permanent native-code penalty from fresh and cached execution.

## Empirical: naïve OSR preheaders duplicate carried state (2026-08-06)

The first dominance-restoration implementation synthesized a merge block before every checkpointed
loop header. Fresh entry and the corresponding resume-dispatch arm entered that merge block, while
normal loop back-edges continued to target the real header. This gave definitions above the loop a
common dominator again and reduced the number of parameters on individual headers, but it did not
remove the state from the function: Cranelift retained the merge block's parameters as a second SSA
layer in front of the genuinely loop-carried header parameters.

The comparison used the exact current branch as the baseline, Cranelift 0.134.3, synthetic tier-up
enabled, and the same d3wasm functions on both sides:

| Metric | `unzReadCurrentFile()` baseline | All preheaders | `AddActiveInteraction()` baseline | All preheaders |
| --- | ---: | ---: | ---: | ---: |
| Optimized CLIF blocks | 905 | 940 | 2,137 | 2,187 |
| Optimized CLIF instructions | 6,825 | 7,253 | 24,422 | 25,766 |
| Optimized CLIF block parameters | 4,681 | 5,036 | 12,396 | 14,299 |
| Native code bytes | 31,268 | 33,700 | 155,000 | 162,152 |
| Native stack-memory instructions | 2,110 | 2,592 | 16,090 | 17,126 |

The 35 new `unzReadCurrentFile()` merge blocks carried 1,492 parameters before their target headers
carried another 361. The 50 new `AddActiveInteraction()` merge blocks carried 3,657 parameters
before the target headers carried another 1,907. The sampled hot `AddActiveInteraction()` headers
lost only about four, one, and three parameters respectively, so their small local pressure
reductions could not repay the additional merge blocks and live ranges. Native code grew by 7.8%
and 4.6%. Three SHA-512 runs averaged 0.346083 s at baseline and 0.349513 s with all preheaders,
approximately a 1.0% execution-time regression.

A second implementation synthesized a preheader only when the number of locals written within the
structured loop body was less than half the function-wide accessed-local count. This was intended
to select loops with many invariant locals, but those two sets are not the state actually live on
each header edge. They also fail to describe bytecode registers, runtime variables, and values
over-lived by the frontend SSA graph. The selective version therefore remained structurally
negative:

| Metric | `unzReadCurrentFile()` baseline | Selective preheaders | `AddActiveInteraction()` baseline | Selective preheaders |
| --- | ---: | ---: | ---: | ---: |
| Optimized CLIF block parameters | 4,681 | 4,965 | 12,396 | 14,220 |
| Native code bytes | 31,268 | 33,812 | 155,000 | 162,040 |
| Native stack-memory instructions | 2,110 | 2,566 | 16,090 | 17,065 |

Its three SHA-512 runs averaged 0.346006 s at baseline and 0.347034 s with selective preheaders, a
noisy result close to neutral. Both preheader implementations were removed.

The safe first part of the intended layout was retained: Cranelift now marks the resume block, the
initial checkpoint-dispatch block, and every dispatch-chain tail as cold. It does not alter the
loop-header topology. Native stack-memory instruction counts were unchanged, each probe function
grew by only 16 bytes, and three SHA-512 runs averaged 0.346078 s at baseline versus 0.345931 s with
the cold marking, effectively neutral. The one-way tier-up test was also strengthened so a local
defined before compilation remains invariant through the loop and is consumed after OSR; the first
OSR call and the following fresh native call both retain the correct value.

The revised dependency is important: idea 4 cannot profitably precede real header-edge liveness.
Idea 3's register-death/local-read work, or an equivalent liveness representation, must first tell
the compiler exactly which values each normal and resume edge needs. Only then should a selective
preheader move resume-only state off a hot header. Declared-local counts, function-wide access
sets, and structured-loop write sets are not sufficient selection signals.

Temporary diagnostic artifacts were written under `/private/tmp`:

- Baseline compiler: `/private/tmp/cranelift-compiler-osr-header-baseline-diagnostic`
- All-preheader compiler: `/private/tmp/cranelift-compiler-osr-preheader-diagnostic`
- Selective compiler: `/private/tmp/cranelift-compiler-osr-selective-preheader-diagnostic`
- Function dumps: `/private/tmp/osr-{header-baseline,preheader-candidate,selective}-{unz,addactive}*`

These paths are on a temporary volume and may not survive a reboot. Diagnostic dump hooks and both
preheader experiments were removed after measurement.

## Separate experiment: persistent clean code plus an ephemeral OSR variant

The preheader work assumes one persistent native version must serve both fresh entry and OSR. A
separate experiment is to compile two complete versions of a function with different lifetimes:

1. Compile a clean entry-only version without native OSR resume entry, checkpoint dispatch, or the
   extra resume predecessors on loop headers. Publish this version for all fresh calls and store
   only this version in the native-code cache.
2. Separately compile an OSR-enabled version of the same function using the existing multi-entry
   resume machinery. This version may support all eligible checkpoints in the function; it does
   not need to be specialized to one checkpoint.
3. Use the OSR-enabled version only to rescue interpreter activations that were already running
   while native compilation was in progress. Fresh calls use the clean version even while the OSR
   version exists.
4. If an activation transfers into the OSR version, retain its code until that activation has
   returned and native code reclamation is safe, then retire it. If no live interpreted activation
   can use it by the time it is ready, discard it without publishing it as the normal entry.

This isolates the permanent and cached native code from the OSR code-quality cost. Its price is
temporary duplicate code and potentially compiling the same function twice on a cold start. It
therefore belongs with compilation policy rather than the preheader/liveness sequence. The policy
should request the second version only for a function with a live interpreted activation that can
benefit, prioritize it soon enough to rescue that activation, suppress duplicate requests from
repeated checkpoints, and cancel or ignore work whose interested activations have finished. A warm
cache hit should normally need only the clean version and never request the OSR version.

The useful experiment is to keep the present OSR lowering unchanged for the temporary version and
compare it with a tier-up-disabled clean version of the same function. Measure clean native code
against the entry-only baseline, successful and unused OSR compilations, temporary code memory,
extra compiler CPU/time, and the loading or frame-time saved by successful transfers. This is
orthogonal to improving a single multi-entry function with edge liveness: the dual-version policy
buys complete hot-code isolation in exchange for cold-start compilation work.

## Options discussed, and where we landed

- **Entry-only tiering (drop OSR entirely, wasmtime's position).** Compilation still eventually
  targets the whole module, and incremental publication shortens the cold-start window, but this
  concedes the demonstrated in-frame-compute acceleration in d3wasm. The entry-only experiment is
  useful as an upper bound on the generated-code tax, not as the current preferred policy.
- **Preheader redirect (measured; frozen).** An empty synthesized
  preheader does restore dominance at its target header, but the all-loop experiment duplicated
  carried state between the preheader and header, grew both probe functions, and regressed SHA-512.
  A function-wide accessed/written-local heuristic did not identify profitable loops. Exact edge
  liveness could choose a less harmful subset, but the frontend pivot removes the reason to reshape
  persistent cached code around interpreter locations. Do not continue this state-model work in
  the allocated-bytecode frontend.
- **Dual clean and ephemeral OSR versions.** Compile a persistent entry-only version for fresh
  calls and caching, plus a complete OSR-enabled version when a live interpreted activation needs
  rescue. The latter may retain the existing all-checkpoint resume machinery; retire it after its
  interested activation has completed, or discard it unused. This completely isolates normal code
  quality, but needs demand-aware scheduling, temporary dual code ownership, safe reclamation, and
  accounting for duplicate cold-start compilation.
- **Cheap independent fixes regardless of direction:** eligibility gating is complete in
  `511ca8761f4`. Further checkpoint placement policy belongs to the temporary OSR flavor rather
  than the persistent direct-lowered body.
- **Recompile-for-cache (superseded).** The earlier shelf option compiled checkpointed code first,
  then rebuilt a clean cache flavor. Direct entry-only code plus an on-demand temporary OSR flavor
  gives the same permanent-code isolation without first publishing interpreter-shaped code for
  normal calls. Do not pursue the older sequence independently.

## Frontend architecture pivot

The code-quality experiments now identify interpreter-bytecode allocation as the wrong native
frontend boundary. The existing compiler consumes R0-R7, virtual-stack locations, call-record
choices, and synthetic instructions selected for interpreter dispatch, then reconstructs typed
SSA. Typed banks, exact register liveness, preheaders, and pressure policies can reduce individual
symptoms but cannot recover the original typed Wasm value flow.

Ladybird retains the parsed and validated `Expression` beside its compiled interpreter bytecode.
The persistent native compiler should lower that instruction stream directly with typed operand
and control stacks. It should cache and publish an entry-only body that never adopts interpreter
locations as persistent identities. The allocated-bytecode compiler remains useful precisely
where that representation fits: producing a temporary OSR-enabled version for an interpreter
activation already in progress. Its resume adapter imports live interpreter locations, and the
temporary code is retired after interested activations finish.

This synthesis removes the OSR predecessor and checkpoint-dispatch cost from normal and cached
code without giving up the demonstrated incremental interpreter-to-native transition. It also
freezes further local-fusion, preheader-selection, and pressure-home work in the old state model.
Trap codes, alias information, call lowering, relocations, caching, and incremental publication
remain frontend-independent and transfer to direct lowering.

The migration must start with a differential harness. Compiler-disabled execution already supplies
an interpreter oracle, but a runner still needs to execute both modes and compare results, traps,
and observable side effects. During partial instruction coverage, eligibility becomes direct
frontend, then allocated-bytecode frontend, then interpreter. SIMD belongs in the direct frontend;
adding a `V128` bank to the old frontend would extend the state model being retired.

## Recommended next steps (updated after the frontend architecture pivot)

1. **Incremental publication — completed in `6dd79f92633`.** Dependency-ordered batches now publish
   while later batches compile, and publication generations prevent repeated full function-table
   scans during the mixed-tier interval.
2. **OSR mechanism validation — completed.** Commit `6eafe4b9999` counts successful transfers, and
   d3wasm visibly accelerated both long-running loading and gameplay activations after tier-up.
   Broader browsing telemetry is still useful for policy, but removal is no longer the default
   conclusion from Ruffle's short-call result.
3. **Entry-only wall-clock A/B — completed for d3wasm.** Disabling synthetic tier-up did not
   materially improve the `d3wasm_11` runtime profile, so generated-code comparisons remain the
   more sensitive way to isolate its structural cost.
4. **Eligibility gating — completed in `511ca8761f4`.** Checkpoints are now added only to native
   candidates.
5. **Exact R0-R7 edge liveness — completed in `39bd4085916`.** This measurably reduced generated
   code and proved false interpreter-location liveness is costly, but the bounded result also
   helped establish that state-model refinements cannot close the frontend gap.
6. **Differential harness.** Compare native-enabled and compiler-disabled executions across
   results, traps, and side effects before migrating lowering instruction families.
7. **Direct typed frontend.** Lower the retained validated `Expression` with typed operand and
   control stacks. Initially fall back per function to the old compiler, then the interpreter,
   while complete and SIMD coverage grows.
8. **Clean persistent plus temporary OSR flavors.** Cache and publish the direct entry-only body
   for fresh calls. Request the allocated-bytecode OSR flavor only for a live interpreted
   activation, and add safe retirement or unused-result disposal before enabling that policy.

## Loose ends / cautions

- Attribution artifacts live under `/private/tmp/ladybird-param-attribution-*` — tmp volume, does
  not survive reboot; move if still needed. The diagnostic code and disabled threshold were
  removed after collection (per the doc), so re-running attribution means re-adding the temporary
  frontend dependency.
- Block-parameter counts ≠ cycles; don't credit a fix with a percentage until the wall-clock A/B
  exists.
- Any instrumentation-policy change shifts dispatch indices and invalidates cache blobs; version
  accordingly if flavors ever diverge.
- CRANELIFT_* env knobs are dead in release builds; force interpreter-only with
  `LADYBIRD_CRANELIFT_COMPILER=/usr/bin/false` when comparing.
