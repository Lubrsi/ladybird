This file records measured Cranelift optimization experiments and the resulting work order.

## Where the branch stands

The typed-state sequence is committed through lazy vstack payloads (`fa08d1c5c60`). Trap codes
(`186803b57f8`), alias regions (`1dd273bb638`), immutable runtime metadata (`c366284cdb8`), cold OSR
dispatch (`b62e9941a0f`), and exact R0-R7 edge liveness (`39bd4085916`) are also complete.

The state-model experiments established a representation ceiling: native lowering currently
consumes storage locations allocated for the bytecode interpreter, then reconstructs typed SSA
from those reused locations. Banks, location liveness, preheaders, and pressure policies can reduce
individual symptoms but cannot restore the original typed Wasm value flow. The primary direction
is therefore direct typed lowering from the retained validated `Expression`. The allocated-bytecode
frontend remains a correctness fallback and is the natural temporary OSR-variant compiler.

## 1. Replace trap-string materialization with trap codes — completed

The baseline `emit_trap_message` implementation created an explicit stack slot per trap site and
emitted one `iconst` plus `stack_store` per message byte. That accounted for 101 slots, 3,320 bytes,
and at least 6,640 CLIF instructions in `AddActiveInteraction()` alone.

Commit `186803b57f8` replaced that machinery with Cranelift user trap codes and `trap`, `trapz`, or
`trapnz`. Fault recovery translates the compact code to the existing diagnostic only after a trap.
This lowering and its tests transfer unchanged to the direct frontend.

## 2. Alias-region classification — completed

The baseline supplied no alias regions. Cranelift 0.134.3 uses frontend-defined
`AliasRegionData` identities rather than the older fixed `Heap`, `Table`, and `Vmctx` categories.

Commit `1dd273bb638` classified activation values, configuration fields, globals, linear memory,
runtime metadata, and tables after auditing every explicit load and store. Commit `c366284cdb8`
separately marked only complete-activation immutable call metadata as `readonly`. These facts belong
to per-instruction lowering and transfer to the direct frontend; uncertain or mutable storage
remains conservative.

## 3. Local/register duplication in the allocated-bytecode frontend — frozen

The lazy-bank checkpoint deliberately established the single-representation invariant so this becomes possible, and the failed equivalent-parameter GFP pass proved the duplicate state must be *avoided during lowering*, not cleaned up after. Two mechanisms were identified, gated by the doc's own proposed diagnostic (attribute `unzReadCurrentFile()`'s 6,806 `i32` params to locals vs bytecode registers):

- **Register liveness.** This is now implemented without extending the compiler protocol: the Rust frontend reconstructs the structured bytecode CFG, resolves branch depths, and computes backwards liveness for R0-R7. Edge normalization and OSR resume materialize only registers live at the destination. Against the exact cold-OSR baseline, this reduced `unzReadCurrentFile()` native code by 4.4%, native instructions by 4.7%, and stack-memory operations by 7.5%; `AddActiveInteraction()` fell by 2.5%, 2.5%, and 2.0% respectively. SHA-512 execution was neutral/slightly positive and d3wasm compilation did not regress. This proves stale interpreter-register state was a real cost, but also bounds it: eight registers cannot by themselves explain the remaining thousands of parameters or the multi-kilobyte native frame.
- **Fuse `local.get` into consumers.** This could remove another location-level copy, but it is now
  intentionally deferred. The direct frontend preserves the producer/consumer relationship rather
  than recovering it from interpreter allocation.

Do not add further state-model refinements here unless required for correctness or for the
temporary OSR fallback. The measured register-liveness checkpoint remains useful while this
frontend exists.

### Architectural finding: do not use interpreter allocation locations as native value identity

R0-R7 are an optimization of the bytecode interpreter, not part of Wasm and not a requirement of Cranelift. The bytecode-generation pass first tracks semantic `ValueID`s, then allocates those values to R0-R7, `ValueStack`, or call-record slots. The persistent bytecode discards the original typed value identity and retains the chosen interpreter locations. The Cranelift frontend consequently receives reused, effectively untyped locations and has to reconstruct a typed value graph using separate `i32`, `i64`, `f32`, and `f64` Variable banks.

This reverses work that was already done before interpreter register allocation:

```text
typed Wasm values
    -> interpreter register allocation
    -> reused R0-R7 locations
    -> bank selection and Cranelift SSA reconstruction
```

It causes unrelated definitions of the same interpreter register to share a mutable frontend identity, makes CFG joins operate on storage locations rather than original Wasm values, requires bank normalization when predecessors assign different types to a reused location, and carries interpreter-oriented destructive-source/destination constraints into native lowering. Cranelift can eliminate some of the resulting copies, but it cannot recover semantic information that the frontend no longer supplies.

Two scope qualifications keep the finding precise:

- The temporary `ValueID` graph is not complete CFG-aware Wasm SSA. Structured and variable-arity
  instructions sink all active values to the stack and clear the allocator's linear value stack, so
  the graph preserves genuine per-definition identity only within regions. It could not simply be
  handed to Cranelift unchanged.
- Call-record slots are not part of the block-parameter mass. Cranelift lowers them as
  activation-memory loads and stores; they cost memory traffic and constrain interpreter
  allocation, but the state that becomes `FunctionBuilder` edge state — and therefore block
  parameters — is R0-R7, virtual-stack variables, and locals.

Ladybird retains the representation from which typed value flow can be constructed: `Expression`
(Types.h:1286) keeps the original parsed, validated instruction stream, with the interpreter
bytecode living separately in `compiled_instructions` (Types.h:1302). Operand identities and the
validator's stack state are not stored, so a direct frontend must rebuild them with a typed
operand/control-stack walk — but direct lowering requires neither keeping nor re-decoding the raw
binary. The intended long-term split is:

```text
parsed, validated Wasm
    ├── interpreter bytecode + R0-R7 allocation
    └── typed Cranelift SSA lowering
```

The interpreter should keep R0-R7: they avoid repeated `ValueStack` push/pop traffic. For native
code there are two paths:

- **Primary: direct typed lowering from the retained `Expression`.** A typed operand/control-stack
  lowering over the original instructions, sharing the module context with the interpreter path
  but nothing of its allocation. OSR then needs an explicit side table mapping the live typed Wasm
  values at each checkpoint to their interpreter locations; only the resume adapter imports those
  locations, and the shared native body never adopts R0-R7 as persistent identities.
- **Incremental: preserve or reconstruct `ValueID` metadata alongside the bytecode**, so the
  existing frontend recovers per-definition identity without changing its input. Reconstruction
  means reaching definitions across every reused interpreter location and permanently inherits
  bytecode-shaped analysis surface (synthetic ops, argument markers, fused instructions).

Direct lowering is not free — it needs the typed operand/control-stack lowering itself, serialized
module context for the compile subprocess, the OSR maps, and differential testing — but it may be
less work than a faithful reconstructor and produces the better endpoint. The compiler-disabled
mode supplies the interpreter oracle; a harness that executes both modes and compares results,
traps, and side effects still has to be built.

The defensible scope of this finding is that it is the central cause of the frontend's excessive
SSA reconstruction and location-liveness problems — spurious joins from location reuse, bank
normalization, and the failure of coarse location-count and write-set proxies (exact R0-R7 edge
liveness, by contrast, measurably helped). It is not the single cause of the native-code gap. Trap
lowering and alias information were independent costs and have already been addressed; call
machinery remains a separate workstream. The next step is therefore not removing R0-R7 from the
interpreter, but prototyping direct typed lowering from `Expression` for native code while retaining
an explicit live-location map for OSR.

## 4. Restore dominance at OSR checkpoint headers — measured negative and frozen

Tier-up predecessors add ~20% block parameters (17,752 → 22,072; 9,171 → 11,558), and the mechanism is specific: the resume arm makes the checkpointed loop header's third predecessor, so *every* live local needs a header parameter — including locals never mutated in the loop, whose definitions above the loop would otherwise dominate the header.

The first implementation routed the fresh-entry and resume-dispatch edges through a synthesized merge block before every checkpointed loop header, while leaving back-edges aimed at the real header. This did restore dominance locally, but Cranelift did not coalesce away the merge block's SSA parameters. In `unzReadCurrentFile()`, 35 merge blocks carried 1,492 parameters before their target headers carried another 361; in `AddActiveInteraction()`, 50 merge blocks carried 3,657 parameters before their headers carried another 1,907. Total optimized block parameters increased from 4,681 to 5,036 and from 12,396 to 14,299 respectively. Native code grew by 7.8% and 4.6%, and SHA-512 regressed by about 1.0% in the measured run.

A selective version based on function-wide accessed locals versus locals written in the structured loop body was also ineffective. Those sets are not the actual state live on each header edge, and they omit bytecode registers and runtime state kept live by the frontend. It still increased native code by 8.1% and 4.5%, while SHA-512 was noisy/neutral.

Real edge liveness could select a less harmful subset, but the direct-frontend decision removes the
reason to keep reshaping the persistent cached body around interpreter entry state. Do not retry
preheader selection in the allocated-bytecode frontend. The cold-only part is independently useful
and has been retained: the resume block, initial dispatch block, and dispatch-chain tails are
marked cold, with unchanged native stack-memory traffic and only 16 bytes of code growth in each
probe function.

There is also an orthogonal compilation-policy experiment: compile a persistent entry-only version for fresh calls and the native-code cache, plus a complete temporary OSR-enabled version for already-running interpreter activations. The temporary version may use the existing all-checkpoint resume machinery unchanged; retire it after a transferred activation returns, or discard it if no activation still needs it. This removes the OSR tax entirely from normal and cached code, at the cost of demand-aware scheduling, safe temporary-code reclamation, and sometimes compiling the same function twice during cold startup.

## 5. Regional pressure-aware homes — frozen with the old state model

Peak-live, loop-live, and pressure-split experiments were neutral or too small to justify their
complexity. Do not tune another home-selection policy against interpreter-location pressure. If
the direct frontend later exposes genuine semantic-value pressure, make that decision from its
typed SSA graph instead.

## 6. Call-ABI follow-ups — reassess after direct lowering

- **Retry the cold interpreter pointer.** The earlier experiment regressed JSON 9.4% because `call_indirect` still crossed helper boundaries; that precondition is gone — indirect calls are native now. Freeing `x0` for a Wasm argument register is a bounded, already-designed experiment whose stated prerequisite is satisfied.
- **Tiny-callee inlining.** Direct call targets are statically known module-wide before lowering. Ladybird's call boundary is more expensive than Wasmtime's (context save/restore), so inlining leaf callees of a few instructions pays disproportionately — `idVertexCache::Position()` at ~1.2% of main-thread cycles is the poster child. Neither engine gets this from Cranelift; it would be a frontend transform.
- **Native cross-module indirect targets** — lower value for single-module d3wasm; note and defer.

## 7. SIMD lowering coverage — direct frontend only

Blake3 spends ~27.6 s dominated by *interpreted* SIMD, and several other Rust workloads have fallback
functions. Cranelift 0.134 has mature i8x16-f64x2 support, but adding a `V128` bank would extend the
state model being retired. The direct frontend's type model should include `V128` from the start;
individual SIMD instructions can use the old frontend or interpreter while coverage is partial.

One more observation while verifying: the compile subprocess already parallelizes across threads (cranelift-compiler.rs:306), so parallel compilation is not on the table as a remaining win — good, that removes one obvious suspect for the load-time story.

## Revised order after the frontend architecture pivot

1. **Preserve frontend-independent work.** Trap codes, alias regions, native calls, relocations,
   caching, incremental publication, and trap recovery remain shared infrastructure. Items 1 and 2
   are already complete.
2. **Build the differential harness.** Run the same workload with native compilation enabled and
   disabled, then compare results, traps, and observable side effects. The compiler-disabled
   interpreter oracle exists; the comparison harness does not.
3. **Add direct typed lowering from `Expression`.** Rebuild typed operand and control stacks from
   the retained validated instructions, and emit CLIF without interpreter register allocation or
   synthetic bytecode. Control-flow construction is the bulk of this frontend, not per-op emission.
4. **Use three-tier eligibility during migration.** Prefer the direct frontend, fall back to the
   allocated-bytecode compiler, then interpret functions unsupported by either compiler. Add SIMD
   to the direct frontend rather than introducing a `V128` bank into the old one.
5. **Separate persistent and OSR code.** Cache and use a clean direct-lowered version for fresh
   calls. Request the existing allocated-bytecode flavor only as a temporary OSR-enabled version
   for a live interpreter activation, then retire or discard it safely.
6. **Re-profile before further policy work.** Only after direct lowering should pressure policy,
   native inlining, and remaining ABI work be ranked against the new generated-code baseline.
