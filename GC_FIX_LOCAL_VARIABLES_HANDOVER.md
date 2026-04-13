# GC Root Container Local Variables - Handover Document

## Overview

This branch adds protection against two failure modes for GC-managed pointers held in containers:

1. **Plain heap-backed containers holding GC pointers** (e.g. `Vector<GC::Ref<T>>` as a local variable). Caught by a clang plugin `VisitVarDecl` check that flags `Vector`/`HashMap`/`HashTable`/`OwnPtr` of GC types when they're not registered as roots.
2. **Slicing a GC root container into its non-root base** (e.g. `Vector<T> v = root_vec` or `f(root_vec)` where `f` takes `Vector<T>` by value). Caught at **compile time** by a deleted constrained constructor + `operator=` template in `AK::Vector`, `AK::HashMap`, and `AK::HashTable`.

It also adds missing GC container types needed to fix the violations (`RootHashTable`, `ConservativeHashTable`, `ConservativeHashMap`, `HeapHashMap`), and fixes all LibJS and Utilities violations.

## Branch State

```
git log --oneline 094e4bacf8..HEAD
```

### Recent history operations

- **Autosquash rebase performed**: The `ConservativeHashMap` and `ConservativeHashTable` explicit copy/move/operator= changes were created as `fixup!` commits and then squashed into their respective `Introduce` commits via `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash 094e4bacf8`. This rewrote every commit from `3716ab5e71` (ConservativeHashMap) onwards. The branch is currently divergent from `origin/catch-non-visited-local-varibles` (30 vs 12), so a future push will need `--force-with-lease`.

- **Post-rebase commit hashes** for the rewritten commits (see numbered list below). The earlier commits (#1–#4) keep their original hashes because they precede the squash points:

  | # | Description | Hash |
  |---|---|---|
  | 1 | Trace keys in RootHashMap | `b54d1e635e` |
  | 2 | Add RootHashTable | `c791d8879e` |
  | 3 | Recognize RootHashMap and ConservativeVector in plugin | `48fcc43fdc` |
  | 4 | Add clang plugin VarDecl check | `b0e3f1bed0` |
  | 5 | Introduce ConservativeHashTable (squash target) | `af7361b93e` |
  | 6 | Introduce ConservativeHashMap (squash target) | `3716ab5e71` |
  | 7 | LibJS: Use GC-rooted containers | `a43810a643` |
  | 8 | Add static_assert for non-GC types in root containers | `2fd2274e18` |
  | 9 | Add tests for GC containers | `91a85d5116` |
  | 10 | Utilities+LibWeb: Fix violations outside LibJS | `7db93a866a` |
  | 11 | Introduce HeapHashMap | `7feedcc17f` |
  | 12 | WIP commit | `7b8e5b44b5` |
  | 13 | LibJS+LibWeb: Fix violations in LibJS/Runtime and LibWeb/Editing | `a2ff9a96f7` |

### Commits (in current order)

1. **LibGC: Trace keys in RootHashMap::gather_roots when they are GC types**
   - RootHashMap previously only traced values. Now traces keys too when they're convertible to `Cell*`.
   - Uses `IsConvertible<K, Cell const*>` guard so non-GC keys (int, FlyString) are skipped.
   - Added `static_assert` that at least one of key/value is a GC type (fires in `gather_roots` to handle forward-declared types).

2. **LibGC: Add RootHashTable for GC-rooted hash table storage**
   - New `RootHashTable<T>` — registers with Heap, precisely traces elements via `gather_roots()`.
   - Added `OrderedRootHashTable<T>` alias.
   - Added `IsConvertible<T, Cell const*>` guard + `static_assert` matching RootHashMap.
   - Plugin updated to recognize it.

3. **LibGC: Recognize RootHashMap and ConservativeVector in clang plugin**
   - Added `GC::RootHashMap`, `GC::RootHashMapBase`, `GC::ConservativeVector` to plugin exclusion lists that were missing.

4. **LibGC: Add clang plugin check for GC pointers in unrooted containers**
   - New `VisitVarDecl` in the plugin. Checks local and static/global variables.
   - Flags: `AK::Vector`, `AK::HashMap`, `AK::HashTable`, `AK::OrderedHashMap`, `AK::OrderedHashTable`, `AK::OwnPtr`, `AK::NonnullOwnPtr`.
   - Skips: references, parameters, direct GC types (GC::Ref/Ptr/Root), GC root containers.
   - **This commit should be reordered to the END of the branch** so the history reads "fix all issues, then enforce".
   - Includes tests: `Tests/ClangPlugins/LibJSGCTests/local_var_gc_in_container.cpp`, `root_container_non_gc_type.cpp`

5. **LibGC: Add ConservativeHashTable for conservatively-scanned hash tables** (`af7361b93e`)
   - New `ConservativeHashTable<T>` — for types like `PropertyKey` that store GC pointers in non-standard layouts (tagged pointers).
   - Uses `for_each_possible_value()` callback that iterates only live entries — dead/free buckets are not scanned.
   - Added `OrderedConservativeHashTable<T>` alias.
   - **Includes (squashed via fixup)** explicit copy ctor, move ctor, and copy `operator=` that bypass the noncopyable `ConservativeHashTableBase` and chain through `static_cast<HashTableBase const&>(other)`. Previously the class was implicitly non-copyable due to its noncopyable base — inconsistent with `ConservativeVector`. The static_cast path is also required to bypass `AK::HashTable`'s deleted derived ctor (added later, in the compile-time slicing block).

6. **LibGC: Add ConservativeHashMap for conservatively-scanned hash maps** (`3716ab5e71`)
   - New `ConservativeHashMap<K, V>` — same live-entry-only scanning approach for HashMap.
   - Also changed `ConservativeHashTable` from `possible_values()` (scanning raw bucket memory including freed buckets) to `for_each_possible_value()` (live entries only). This fixes a bug where stale pointers in deleted buckets could keep GC cells alive indefinitely.
   - Fixed `RootVector` and `RootHashTable` `gather_roots()` to handle const Cell pointer types via `const_cast`.
   - Added `OrderedConservativeHashMap<K, V>` alias.
   - **Includes (squashed via fixup)** explicit copy ctor, move ctor, and copy `operator=` for `ConservativeHashMap`, mirroring the `ConservativeHashTable` change above.

7. **LibJS: Use GC-rooted containers for local variables with GC types**
   - Fixed all 35 LibJS violations.
   - See "LibJS Fixes Summary" section below for details.

8. **LibGC: Add static_assert for non-GC types in root containers**
   - `RootVector`, `RootHashTable`, and `RootHashMap` now `static_assert` if instantiated with types not convertible to `Cell const*` or deriving from `NanBoxedValue`.
   - Asserts are inside `gather_roots()` (not the class body) to handle forward-declared types correctly.
   - Includes clang plugin test: `root_container_non_gc_type.cpp`

9. **LibGC: Add tests for GC containers**
   - `Tests/LibGC/TestGCContainers.cpp` — 26 unit tests covering:
     - Root containers: `gather_roots()` reports correct cells (RootVector, RootHashTable, RootHashMap)
     - Conservative containers: `for_each_possible_value()` reports correct values, stale pointer regression tests
     - Heap containers: `visit_edges()` traces cells correctly (HeapVector, HeapHashTable, HeapHashMap)
   - `TestVisitor` — simple visitor that records visited cells for testing `visit_edges()`

10. **Utilities+LibWeb: Fix GC root container violations outside LibJS**
    - `js.cpp` — IGNORE_GC on OwnPtr<ExecutionContext> (FIXME)
    - `wasm.cpp` — IGNORE_GC on OwnPtr<ExecutionContext> (FIXME), RootVector<Value>
    - `WebAssemblyModule.h/.cpp` — Updated `get_exported_names` signature to match Module virtual interface refactor

11. **LibGC: Add HeapHashMap for GC-allocated hash map storage**
    - New `HeapHashMap<K, V>` — GC-allocated Cell that owns a HashMap and traces via `visit_edges()`.
    - Mirrors existing `HeapVector` and `HeapHashTable` types.
    - GC_DECLARE/DEFINE_ALLOCATOR macros expanded manually (can't handle multi-param templates due to commas).
    - Plugin updated to recognize it.
    - Includes tests for `visit_edges()`.

12. **WIP commit (`7b8e5b44b5`, post-rebase)** — RootHashMap/RootHashTable copy/move, plugin record recursion fix
    - Added safe copy/move constructors and `assign_heap()` to `RootHashMap` and `RootHashTable` (matching `RootVector`'s pattern). Previously they were noncopyable/nonmovable.
    - Fixed plugin `type_has_unrooted_gc_container()` record recursion to skip `GC::` types that appear as instantiated `RecordType` (not `TemplateSpecializationType`). This was causing false positives on `GC::RootVector<JS::Value>` return types deduced via `auto`.
    - Updated clang plugin tests (`local_var_gc_in_container.cpp` now has cases for `Optional<HashTable<GC::Ptr>>` wrapped types and named wrapper structs).
    - **Note**: Originally `b09c8243e9` before the autosquash rebase. Hash changed because the rebase replayed every commit from `3716ab5e71` (ConservativeHashMap fixup target) onwards.

13. **LibJS+LibWeb: Fix GC root container violations in LibJS/Runtime and LibWeb/Editing**
    - LibJS/Runtime/JSONObject: `StringifyState::seen_objects` is now `GC::RootHashTable<GC::Ptr<Object>>`. Added heap-taking constructor and caller updated to `StringifyState { vm.heap() }`.
    - LibWeb/Editing/Commands.cpp: 17+ local `Vector<GC::Ref<DOM::Node>>` → `GC::RootVector`. Also `HashTable<DOM::Node*>` → `GC::RootHashTable`.
    - LibWeb/Editing/Internal/Algorithms.cpp: 16+ local `Vector<GC::Ref<DOM::Node>>` → `GC::RootVector`. `Vector<RecordedNodeValue>` → `GC::ConservativeVector` (struct transitively contains GC pointers).
    - LibWeb/Editing/ExecCommand.cpp: 1 site fixed.

## Outstanding Tasks

Live task list (mirrored from the in-session TaskList tool, lowest ID first):

| ID | Status | Subject |
|---|---|---|
| #4 | completed | Collect LibWeb violations from build |
| #5 | completed | Fix LibWeb/Editing violations |
| #6 | pending | Fix LibWeb/Layout violations (~49) |
| #7 | pending | Fix LibWeb/HTML violations (~36) |
| #8 | pending | Fix LibWeb/DOM violations (~33) |
| #9 | pending | Fix LibWeb/SVG violations (~23) |
| #10 | in_progress | Fix LibWeb/CSS violations (~12 — 5 done, see "CSS Progress") |
| #11 | pending | Fix remaining LibWeb violations (Crypto ~23, Bindings, IndexedDB, Animations, etc.) |
| #12 | pending | Reorder commits to put plugin enforcement at the end of the branch |
| #13 | completed | Add compile-time block for GC container downgrades |

Roughly in priority order, the next things to land are:

1. **Fix the four LibWeb sites the new compile-time block surfaced** (see "Newly surfaced violations from the compile-time block" above) — these block the branch from building. None of them are in the existing task list because they were only discovered after adding the slicing protection.
2. **Finish task #10 (CSS)** — already in progress, see "CSS Progress" section below for the remaining files.
3. **Continue tasks #6–#9 and #11** — fix the remaining LibWeb violation directories. Suggested order is by violation count (Layout → HTML → DOM → SVG → Crypto → smaller dirs) but they're independent and can be tackled in any order.
4. **Task #12 (commit reordering)** — once everything builds, rebase to put the plugin-enforcement commit at the end of the branch so the history reads "fix all issues, then enforce".

### Deferred (not in the task list, tracked here for the next session)

- **Refactor `resolve_export` recursion accumulator** so the deferred plugin parameter check can be re-enabled. See "Plugin extension attempted and reverted" below for context.
- **Re-enable the deferred plugin parameter check** after the `resolve_export` refactor. The compile-time slicing block already covers the most common case (a RootVector being passed by value), but a parameter check would still catch plain `Vector<GC::Ref<T>>` parameters that aren't fed from a Root container at any call site.
- **Possible bug: minimum Cell size not enforced at compile time** — see the section below.
- **Dangerous patterns not yet caught by tooling** — see the new section below for the full audit and concrete instance lists.

## Future work: dangerous patterns not yet caught by tooling

Research pass after the adopt_* pattern landed, hunting for GC-safety hazards that slip past the current plugin + compile-time checks. Categorised by expected fix path.

### 1. `AK::Function` with GC-containing captures (~15–20 visible sites, category is broader)

**The hazard.** `AK::Function<Sig>` is type-erased heap storage for a callable. When a lambda with captures is assigned into it, the captures live inside the `Function`'s heap-allocated callable storage. If any capture is `GC::Ptr<T>` / `GC::Ref<T>` / `JS::Value` / `Cell*` / a struct transitively holding a cell, it is **invisible to GC** — no matter who owns the `AK::Function`. The typical dangerous pattern is **callback registration**: class A constructs a lambda capturing some of A's GC-managed state, stores it in an `AK::Function` that class B accepts and holds. B later outlives the cells A captured, and when B fires the callback the captures are dangling.

This bites whether the `AK::Function` lives as:

- a member of another class (Cell or non-Cell)
- a local that escapes via registration
- a by-value parameter being forwarded into storage
- a return value being consumed by a caller who stores it

The common factor is that the captures have been copied into heap-invisible storage the moment the lambda was assigned into the `AK::Function`.

**Known-good replacement pattern.** `GC::Function<Sig>` is a `GC::Cell` that wraps a callable and participates in GC tracing. Use `GC::Ref<GC::Function<Sig>>` in every context where you'd otherwise reach for `AK::Function<Sig>` for a callback that needs to survive past its registration call — parameter type, member type, local that gets stored elsewhere, return type from a registration helper. The call sites in `EventLoop` / `Task` / `Promise` / `JobCallback` / `IDBDatabase` already use this correctly and are worth copying.

**Highest-impact concrete sites found so far** (members of Cell-derived classes that aren't visited today — the tip of the iceberg, not the whole category):

| File | Member | Why it's concerning |
|---|---|---|
| `Libraries/LibWeb/DOM/HTMLCollection.h:67-68` | `Function<bool(Element const&)> m_filter`, `m_sort` | DOM collection filters — captures frequently reference other nodes. |
| `Libraries/LibWeb/DOM/LiveNodeList.h:44` | `Function<bool(Node const&)> m_filter` | Same pattern; widespread in traversal code. |
| `Libraries/LibWeb/HTML/HTMLAllCollection.h:56` | `Function<bool(DOM::Element const&)> m_filter` | Same pattern. |
| `Libraries/LibWeb/HTML/HTMLScriptElement.h:152` | `Function<void()> m_steps_to_run_when_the_result_is_ready` | Async script completion; captures execution context. |
| `Libraries/LibWeb/HTML/Scripting/ModuleScript.h:61` | `Function<void(ModuleScript const*)> m_completed_fetch_internal_callback` | Module fetch continuation. |
| `Libraries/LibWeb/CSS/CSSRuleList.h:70` | `Function<void()> on_change` | Style change observer. |
| `Libraries/LibWeb/CSS/StyleValues/ImageStyleValue.h:62` | `mutable Function<void()> on_animate` | Has an explicit FIXME about this exact issue. |

Rough count across `Libraries/LibJS/` and `Libraries/LibWeb/` is ~15–20 such members in Cell-derived classes. The DOM collection filter cluster (`HTMLCollection`, `LiveNodeList`, `HTMLAllCollection`) is the highest priority because it's used pervasively by DOM traversal code and the same pattern is repeated three times. That scan did not cover the broader shape of the category — callback-by-parameter APIs, helper factory functions returning `AK::Function`, and locals that escape via a registration call would need a separate pass.

**Tooling opportunity.** The cleanest rule would be to scan lambda expressions assigned into (or constructing) an `AK::Function<Sig>` and flag any capture whose type transitively contains a GC pointer — that catches the problem at the point of introduction rather than at storage. A coarser alternative that is much easier to implement: flag every occurrence of `AK::Function<Sig>` in LibJS/LibWeb code and recommend `GC::Function<Sig>`, with an opt-out annotation for the small set of cases that are provably GC-free (pure formatter callbacks, debug printing, strictly no-capture lambdas). The coarser version is probably the right starting point — this codebase already seems to converge on `GC::Function` for anything GC-relevant, so the false-positive rate should be low.

### 2. By-value function parameters holding heap-backed containers (~70 sites, mixed difficulty)

**The hazard.** A parameter of type e.g. `Vector<GC::Ref<X>>` lives with its header on the callee's stack, but its backing allocation is on the heap and is not registered with the GC as a root. Any GC triggered inside the callee can collect the cells it holds. The current plugin `VisitVarDecl` check short-circuits on `ParmVarDecl` with a FIXME — this is the deferred parameter check.

**Rough breakdown** (honest count from a recursive grep over `Libraries/LibJS/` and `Libraries/LibWeb/`, excluding tests):

| Container type | By-value parameter count |
|---|---|
| `Vector<GC::Ref<T>>` | ~35 |
| `Vector<GC::Ptr<T>>` | ~12 |
| `Vector<GC::Root<T>>` | ~18 |
| `Vector<JS::Value>` | ~3 (but see recursion accumulator hazard below) |
| `HashMap<...>` / `HashTable<...>` with GC values | <5 |

**By difficulty:**

- **Trivial (~25–30 sites)**: constructors and setters that `move()` the parameter straight into a member. These can be rewritten as `GC::RootVector<T>&&` + `GC::adopt_root_vector(...)` in the member initializer — the pattern we just built. Clusters:
  - SVG list constructors (`SVGLengthList`, `SVGNumberList`, `SVGTransformList`, …)
  - `IDBTransaction`, `IDBObjectStore` initialization
  - CSS numeric/transform array creation
- **Non-trivial (~40 sites)**: spec algorithms that pass collections through recursive dispatch. Touching these risks spec-observable behavior changes. Clusters:
  - `Libraries/LibWeb/Editing/Internal/Algorithms.cpp` — ~8 instances (indent, split_the_parent, etc.)
  - `Libraries/LibWeb/HTML/Focus.cpp` — ~3 instances (focus chain traversal)
  - `Libraries/LibWeb/HTML/TraversableNavigable.cpp` — ~5 instances (navigation transitions)
  - `Libraries/LibWeb/HTML/Parser/HTMLParser.cpp:4947` — `parse_html_fragment(..., Vector<GC::Root<DOM::Node>>)`
- **Structurally hard (2–5 sites)**: recursion accumulators where the by-value pass *is* the deduplication mechanism. The canonical example is `Libraries/LibWeb/IndexedDB/Internal/Algorithms.cpp:255` — `convert_a_value_to_a_key(realm, value, Vector<JS::Value> seen)` — the `seen` vector is accumulated across recursion and the by-value pass ensures sibling branches get independent copies. Refactoring this needs a deliberate plan (callback-based traversal? explicit rooted accumulator on caller's stack with save/restore? heap-allocated accumulator?). Similar structure to `resolve_export` (already tracked above).

**Interesting individual sites for the "non-trivial" bucket:**

- `Libraries/LibWeb/Page/Page.cpp:839` — `Page::update_find_in_page_selection(Vector<GC::Root<DOM::Range>> matches)`
- `Libraries/LibWeb/XPath/XPathResult.cpp:58` — `XPathResult::set_node_set(Vector<GC::Ptr<DOM::Node>> node_set)`
- `Libraries/LibWeb/Editing/Internal/Algorithms.cpp:1607` — `indent(Vector<GC::Ref<DOM::Node>> node_list)`
- `Libraries/LibWeb/HTML/Focus.cpp:44` — `run_focus_update_steps(Vector<GC::Root<DOM::Node>> old_chain, Vector<GC::Root<DOM::Node>> new_chain, ...)`
- `Libraries/LibJS/Runtime/Intrinsics.cpp:206` — `parse_builtin_file(Vector<GC::Root<SharedFunctionInstanceData>>)`

**Recommended approach.** Fix the trivial constructor / setter cluster first, folding into the LibWeb violation-fixing passes as encountered. Document the spec-algorithm bucket for a dedicated pass. Defer recursion accumulators until there's a specific plan. Only re-enable the plugin parameter check once the trivial sites are cleared and the non-trivial ones are either fixed or explicitly annotated.

### 3. `visit_edges` correctness gaps (small residual surface)

The plugin already has thorough `visit_edges` validation (`Meta/Lagom/ClangPlugins/LibJSGCPluginAction.cpp:492-784`) — member presence enforcement, member access verification, substruct tracing, smart pointer unwrapping, `Base::visit_edges()` upcall via the `must_upcall` attribute, and Optional/Variant wrapper unwrapping. The remaining gaps are narrower than expected:

- **Conditional visits**: `if (cond) visitor.visit(m_ptr);` passes the "accessed somewhere in the function body" matcher but can skip marking in some paths. No test coverage. Would need control-flow analysis — expensive but small code surface.
- **"Field mentioned ≠ actually visited"**: documented FIXME in `Tests/ClangPlugins/LibJSGCTests/gc_allocated_member_is_accessed.cpp:19`. The current matcher (`LibJSGCPluginAction.cpp:720-722`) only checks that the field name appears somewhere in the function body, not that it's passed to `visitor.visit(...)`. Tightening this is the highest-value plugin improvement in the visit_edges area — small scope, catches a real "forgot to actually visit" footgun where the developer wrote e.g. `(void)m_foo;` or referenced the field in an assertion without visiting it.
- **Deeper non-Cell substruct chains**: a non-Cell struct `A` that composes another non-Cell struct `B` with its own `visit_edges()` — the plugin only checks the outermost level. `A::visit_edges()` is not required to call `B::visit_edges()` if `B` is one level deep inside `A`. Worth a test case to confirm and then a recursive fix.
- **Iterator visited instead of underlying container**: visiting `m_vec.begin()` instead of `m_vec` would currently pass the matcher. No test.
- **Const vs. non-const visitor paths**: untested — a class that has both a const-visitor and non-const-visitor path could plausibly drift.

**Recommended approach.** Land the "access must be an actual `visitor.visit(field)` call" tightening first (it subsumes the "field mentioned but not visited" gap and has the highest real-world bug-catching ratio). Add regression tests for conditional visits, deeper substruct chains, and iterator-visited patterns even without code changes, so the current behavior is pinned and future regressions are caught.

### 4. Non-Cell owners of `ExecutionContext` (transient locals + opaque custom data)

**The hazard, recap.** `JS::ExecutionContext` is not a `GC::Cell`, but it holds GC pointers (`realm`, `function`, environments, `script_or_module`, inline value span). It has a `visit_edges` method, and its Cell-held owners forward tracing through it manually: `GeneratorObject`, `AsyncGenerator`, `SourceTextModule`, `AsyncFunctionDriverWrapper`, `EnvironmentSettingsObject`, and the VM's raw execution-context stack (via `VM::gather_roots`). Those are correctly traced today.

The actual hazard is **stack-held `OwnPtr<JS::ExecutionContext>` transient locals**: between `ExecutionContext::create(...)` and `vm.push_execution_context(*ctx)`, nothing registers the inner GC pointers as roots. Any allocation in that window is a real use-after-GC risk. Currently annotated with `IGNORE_GC` + `FIXME: ExecutionContext should be GC-allocated so this is properly rooted.` at:

- `Libraries/LibJS/Runtime/ExecutionContext.cpp` (copy)
- `Libraries/LibJS/Runtime/Realm.cpp` (initialize_host_defined_realm)
- `Libraries/LibJS/Runtime/ECMAScriptFunctionObject.cpp` (async context copy)
- `Libraries/LibWeb/Bindings/MainThreadVM.cpp` (dummy execution context in host_enqueue_promise_job, script execution context in host_make_job_callback, and the `WebEngineCustomJobCallbackData` construction)

The `WebEngineCustomJobCallbackData` case is slightly different but related: `JobCallback` holds the custom data as `OwnPtr<CustomData>` and its `visit_edges` does not forward through `m_custom_data`, so `WebEngineCustomJobCallbackData::incumbent_settings` and `active_script_context` are not traced even though the `JobCallback` itself is a Cell.

**`static RefPtr<JS::VM> s_main_thread_vm`** in `MainThreadVM.cpp` is annotated with `IGNORE_GC` for the same structural reason as `VM.cpp`'s `s_vm`: the VM owns the GC heap and handles its own root gathering via `VM::gather_roots`.

**Options for a proper fix, in ascending order of scope:**

1. **Specialized `JS::RootedExecutionContext` wrapper for transient locals.** Introduce an RAII wrapper that owns a `NonnullOwnPtr<ExecutionContext>` inside it, registers itself with the heap on construction, and traces its held context via the existing `ExecutionContext::visit_edges`. Factory functions combine "allocate + register + populate" into one atomic step (the pool allocator is not the GC heap, so no GC fires during the allocation), eliminating the timing-window hazard. Replaces the five transient-local `IGNORE_GC` sites with a wrapper whose declared variable type is `JS::RootedExecutionContext`, so the plugin sees no `OwnPtr<ExecutionContext>` local to flag in the first place — no flow-sensitive plugin work needed. Leaves the hot path (VM execution context stack, generator/module member storage) untouched. Implementation plan spelled out below.
2. **Teach the plugin the "manually-traced owner" pattern.** Recognize that a class holding `OwnPtr<T>` / `NonnullOwnPtr<T>` where `T` has a `visit_edges` method, *and* whose own `visit_edges` calls `->visit_edges(visitor)` on that member, is not a violation. This is a separate plugin refinement — it's not needed for the migration below because `RootedExecutionContext` already dodges the plugin by keeping the `OwnPtr` as an internal member. But it would clean up the six *existing* Cell-held hand-written forwarders (`GeneratorObject`, `AsyncGenerator`, etc. — see list below), which aren't actually violations today but would be flagged if the plugin were stricter in the future.
3. **Make `ExecutionContext` a `GC::Cell`.** Fixes all of the above but pays the hot-path cost on every JS function call (push/pop of the VM stack becomes barrier traffic), loses the custom tail-sized pool allocator (`ExecutionContextAllocator` buckets by 4/16/64/128/256/512 Value slots), and cascades signature changes across ~130 files in LibJS and LibWeb (generators, modules, settings objects, and all creators). Probably not worth it until profiling shows GC pressure from contexts or the Cell allocator grows a pooled / variable-size variant.

**Recommended path:** option 1 — it's self-contained, solves every `IGNORE_GC` site in this branch (bucket B as well as bucket A, because the factory collapses the timing window), and requires no changes to the existing plugin or `GC::Cell::Visitor` API. Defer options 2 and 3 unless a deeper reason emerges.

#### Why specialized instead of a generic `GC::ScopedRoots`

An earlier version of this section proposed a generic `GC::ScopedRoots<Ts...>` variadic RAII helper plus `Cell::Visitor::visit(T&)` / `visit(OwnPtr<T>&)` overloads constrained on `visit_edges`. In principle it's more general, but for the actual cluster of sites in this branch, it carries costs that the specialized approach avoids entirely:

- **Timing-window hazard for populated locals.** `ScopedRoots` can't protect a variable retroactively; the "populated at declaration" cluster (e.g. `async_context = running_context.copy()`) either needs a `copy_to`-style refactor or a separate owning-wrapper primitive. `RootedExecutionContext::copy_from` wraps allocation and registration into one call and sidesteps this.
- **Flow-sensitive plugin work.** `ScopedRoots` leaves a bare `OwnPtr<ExecutionContext>` local in the source — the plugin's `VisitVarDecl` (`Meta/Lagom/ClangPlugins/LibJSGCPluginAction.cpp:978-1018`) flags it on type alone, so removing the `IGNORE_GC` requires a same-`CompoundStmt` lookahead in the plugin that verifies a paired `ScopedRoots` construction. That's a non-trivial extension with subtle correctness requirements around ordering and scope. `RootedExecutionContext` has no bare `OwnPtr` local — the plugin sees `JS::RootedExecutionContext ctx;` and a simple type-level allowlist entry is enough.
- **`Cell::Visitor` surface extension.** `ScopedRoots` requires `visit(T&)` / `visit(OwnPtr<T>&)` / `visit(NonnullOwnPtr<T>&)` overloads constrained on `visit_edges` across `LibGC/Cell.h:66-185`. `RootedExecutionContext::visit_edges` calls `m_ctx->visit_edges(visitor)` directly with no new overload needed.
- **No actual generic use case in the tree.** The other non-Cell holders-of-GC-pointers (`GeneratorObject::m_execution_context`, `SourceTextModule::m_execution_context`, settings-object contexts, etc.) are already members of `Cell`s and correctly traced by their owner's `visit_edges` (see "Step 5" below for the list). Only stack-held transient `ExecutionContext`s have this shape, and there's exactly one non-Cell type in the codebase that needs this treatment.

If another non-Cell type with `visit_edges` and a timing hazard ever appears, writing a second specialized wrapper is cheap — each one is ~50 lines.

#### Implementation plan for option 1 (`JS::RootedExecutionContext`)

Goal: an RAII wrapper that owns a `NonnullOwnPtr<ExecutionContext>`, registers with the heap on construction so the held context is traced via `ExecutionContext::visit_edges` from that point forward, and offers factory functions that collapse "allocate + register + populate" into one timing-window-free call. Declared variable type is `JS::RootedExecutionContext`, so the plugin sees nothing to flag.

**Target usage (covering all five `IGNORE_GC` transient-local sites):**

```cpp
// Allocate-and-register — direct construction. Mandatory copy elision (C++17)
// means no move is required even though the wrapper is non-movable.
JS::RootedExecutionContext ctx(vm, 0, ReadonlySpan<Value> {}, 0);
ctx->realm = &realm;
vm.push_execution_context(*ctx);

// Copy-and-register — second public ctor selected by parameter shape.
JS::RootedExecutionContext async_context(vm, running_context);

// Conditional population — Optional<T>::emplace calls the ctor in-place, so
// non-movability is fine. Do NOT use operator= assignment; that would need
// move-assignment, which is deleted. Note the double-dereference below:
// `*dummy_execution_context` yields the RootedExecutionContext, and its
// operator-> reaches the held ExecutionContext — `(*dummy)->script_or_module`
// is the access path, not `dummy->script_or_module`.
Optional<JS::RootedExecutionContext> dummy_execution_context;
if (!job_settings) {
    dummy_execution_context.emplace(vm, 0, ReadonlySpan<Value> {}, 0);
    (*dummy_execution_context)->script_or_module = script_or_module;
    vm.push_execution_context(**dummy_execution_context);
}
```

**The Optional constraint matters.** `RootedExecutionContext` has copy and move deleted (the intrusive list node pins the object to its registration address). Consequently:

- `auto ctx = ...;` works via C++17 mandatory copy elision only when the RHS is a prvalue of the same type — so `JS::RootedExecutionContext ctx(vm, ...);` (direct initialization) is fine, and `JS::RootedExecutionContext ctx = JS::RootedExecutionContext(vm, ...);` is also fine.
- `opt.emplace(vm, ...)` works — emplace forwards ctor args and constructs in-place.
- `opt = JS::RootedExecutionContext(vm, ...)` does **not** compile: this is move-assignment of the held value, and move-assign is deleted. Linter-style error.
- Named factory functions (`::create`, `::copy_from`) returning the wrapper by value would also work for simple locals via NRVO, but **cannot** be used as an argument to `emplace` (emplace forwards ctor args, not a constructed prvalue). For this reason the implementation should prefer direct public ctors over static factories — `::create` would be a usability trap.

If `AK::Optional` turns out not to support a non-movable `T` at all (some implementations require the type to be move-constructible even when `emplace` is used — worth actually checking `AK/Optional.h` before committing to the shape), two fallbacks are:

- `AK::Variant<Empty, JS::RootedExecutionContext>` — Variant uses in-place construction and doesn't require T to be movable.
- A tiny `JS::OptionalRootedExecutionContext` type that wraps a manual `alignas(T) std::byte[sizeof(T)]` with a live flag — trivial to write if needed.

Before committing to the shape, verify `AK::Optional<T>::emplace` compiles for a non-movable `T`.

**Step 1: new `Libraries/LibJS/Runtime/RootedExecutionContext.h` / `.cpp` next to `ExecutionContext.{h,cpp}`.**

```cpp
// RootedExecutionContext.h (sketch)
namespace JS {

class JS_API [[nodiscard]] RootedExecutionContext {
public:
    // Allocate-and-register.
    RootedExecutionContext(VM&, u32 registers_and_locals_count,
        ReadonlySpan<Value> constants, u32 arguments_count);

    // Copy-and-register. Distinguished from the allocate ctor by parameter shape.
    RootedExecutionContext(VM&, ExecutionContext const& source);

    ~RootedExecutionContext();

    RootedExecutionContext(RootedExecutionContext const&) = delete;
    RootedExecutionContext& operator=(RootedExecutionContext const&) = delete;
    RootedExecutionContext(RootedExecutionContext&&) = delete;
    RootedExecutionContext& operator=(RootedExecutionContext&&) = delete;

    ExecutionContext& operator*() { return *m_ctx; }
    ExecutionContext const& operator*() const { return *m_ctx; }
    ExecutionContext* operator->() { return m_ctx.ptr(); }
    ExecutionContext const* operator->() const { return m_ctx.ptr(); }
    ExecutionContext* ptr() { return m_ctx.ptr(); }

    void visit_edges(Cell::Visitor& visitor) { m_ctx->visit_edges(visitor); }

private:
    VM* m_vm { nullptr };
    NonnullOwnPtr<ExecutionContext> m_ctx;
    IntrusiveListNode<RootedExecutionContext> m_list_node;

public:
    using List = IntrusiveList<&RootedExecutionContext::m_list_node>;
};

}
```

```cpp
// RootedExecutionContext.cpp (sketch)
namespace JS {

RootedExecutionContext::RootedExecutionContext(VM& vm, u32 regs_locals,
    ReadonlySpan<Value> constants, u32 args)
    : m_vm(&vm)
    // ExecutionContext::create goes through ExecutionContextAllocator's pool,
    // NOT the GC heap — no GC can fire here, so no timing window.
    , m_ctx(ExecutionContext::create(regs_locals, constants, args))
{
    m_vm->did_create_rooted_execution_context({}, *this);
}

RootedExecutionContext::RootedExecutionContext(VM& vm, ExecutionContext const& source)
    : m_vm(&vm)
    // source.copy() allocates (pool) and does a sequence of GC-pointer copies
    // and a memcpy of the Value tail. None of these trigger GC. By the time
    // control reaches did_create_rooted_execution_context below, the
    // populated m_ctx is already accessible through *this, so any GC that
    // fires afterwards will trace its fields precisely.
    , m_ctx(source.copy())
{
    m_vm->did_create_rooted_execution_context({}, *this);
}

RootedExecutionContext::~RootedExecutionContext()
{
    m_vm->did_destroy_rooted_execution_context({}, *this);
}

}
```

Copy/move deleted intentionally — the intrusive list node pins the object to its registration slot; copying would double-register and moving mid-scope is the kind of thing we want the type system to forbid.

**Step 2: wire into `VM`, not `Heap`.** The architecture has a clean embedder-roots seam already: `Heap` takes an `AK::Function<void(HashMap<Cell*, HeapRoot>&)> gather_embedder_roots` in its ctor at `Libraries/LibGC/Heap.h:44`, and `JS::VM::gather_roots` (`Libraries/LibJS/Runtime/VM.cpp:277`) is what gets registered there. VM already owns the raw execution-context stack walk at `VM.cpp:311-325` — `RootedExecutionContext` slots in next to it. **No changes to LibGC headers or `Heap.cpp` are needed**; LibGC stays oblivious to `JS::RootedExecutionContext`.

- `Libraries/LibJS/Runtime/VM.h`: add `JS::RootedExecutionContext::List m_rooted_execution_contexts;` next to the existing `m_execution_context_stack` (around `VM.h:335`). Also add inline helpers:

  ```cpp
  inline void VM::did_create_rooted_execution_context(Badge<RootedExecutionContext>, RootedExecutionContext& rooted)
  {
      VERIFY(!m_rooted_execution_contexts.contains(rooted));
      m_rooted_execution_contexts.append(rooted);
  }
  inline void VM::did_destroy_rooted_execution_context(Badge<RootedExecutionContext>, RootedExecutionContext& rooted)
  {
      VERIFY(m_rooted_execution_contexts.contains(rooted));
      m_rooted_execution_contexts.remove(rooted);
  }
  ```

- `Libraries/LibJS/Runtime/VM.cpp:277-329`: in `VM::gather_roots`, after the existing `gather_roots_from_execution_context_stack` block (which already uses `ExecutionContextRootsCollector` to extract cells out of each frame), add:

  ```cpp
  for (auto& rooted : m_rooted_execution_contexts) {
      IGNORE_GC ExecutionContextRootsCollector visitor;
      rooted.visit_edges(visitor);
      for (auto cell : visitor.roots)
          roots.set(cell, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
  }
  ```

  Reuses the existing `ExecutionContextRootsCollector` visitor and the existing `HeapRoot::Type::VM` tag. No new `HeapRoot::Type` enumerator needed, and no corresponding switch updates in `Heap.cpp`'s dump path — the `VM` tag path is already handled at `Heap.cpp:250-252`.

**Step 3: plugin allowlist.** Add `JS::RootedExecutionContext` to the same allowlist that already carries `GC::RootVector`, `GC::RootHashMap`, `GC::ConservativeVector`, `GC::RootHashTable`, etc. Edit location: the recognition list in `Meta/Lagom/ClangPlugins/LibJSGCPluginAction.cpp`. See the "Recognize RootHashMap and ConservativeVector in clang plugin" commit in the branch history for the exact entry points.

No flow-sensitive analysis needed: the declared variable type at each call site is `JS::RootedExecutionContext`, not `OwnPtr<ExecutionContext>`, so `VisitVarDecl`'s type-level check passes trivially.

**Step 4: migrate the `IGNORE_GC` sites.** Five of the six current annotations become `RootedExecutionContext`:

| File | Current | Replacement |
|---|---|---|
| `Libraries/LibJS/Runtime/ExecutionContext.cpp:108` (in `copy()` itself) | `IGNORE_GC auto copy = create(...); copy->function = function; ...; return copy;` | This is the *body* of `ExecutionContext::copy()`, which the `RootedExecutionContext(VM&, ExecutionContext const&)` ctor wraps. The `IGNORE_GC` here can come off once the new ctor is the only caller of `copy()` — i.e. when all external users of `ExecutionContext::copy()` are migrated and `copy()` becomes an internal detail. If that's too aggressive for one commit, leave the annotation with a rewritten FIXME pointing at `RootedExecutionContext` as the intended public entry point. |
| `Libraries/LibJS/Runtime/Realm.cpp:42` | `IGNORE_GC auto new_context = ExecutionContext::create(0, {}, 0); new_context->function = nullptr; ...` | `JS::RootedExecutionContext new_context(vm, 0, ReadonlySpan<Value> {}, 0); new_context->function = nullptr; ...` |
| `Libraries/LibJS/Runtime/ECMAScriptFunctionObject.cpp:437` | `IGNORE_GC auto async_context = running_context.copy();` | `JS::RootedExecutionContext async_context(vm, running_context);` — this is the bucket-B site; the copy-ctor variant solves the timing window. |
| `Libraries/LibWeb/Bindings/MainThreadVM.cpp:287` (`dummy_execution_context`) | `IGNORE_GC OwnPtr<JS::ExecutionContext> dummy_execution_context;` + later conditional `dummy_execution_context = JS::ExecutionContext::create(...); ...` | `Optional<JS::RootedExecutionContext> dummy_execution_context;` + inside the `else` branch: `dummy_execution_context.emplace(vm, 0, ReadonlySpan<JS::Value> {}, 0); (*dummy_execution_context)->script_or_module = script_or_module; vm.push_execution_context(**dummy_execution_context);`. |
| `Libraries/LibWeb/Bindings/MainThreadVM.cpp:347` (`script_execution_context`) + `:374` (`host_defined`) | `IGNORE_GC OwnPtr<JS::ExecutionContext> script_execution_context;` + conditional populate + `move(script_execution_context)` into `WebEngineCustomJobCallbackData` | **These two sites must migrate together.** The current code builds `script_execution_context` as a stack local and then transfers it into `WebEngineCustomJobCallbackData` via `move(...)` at line 374. A non-movable `RootedExecutionContext` can't participate in that transfer, so there's no intermediate stack-local form to reach for. The fix folds both sites into one structural change (see bucket-C note below). |

**Bucket C (combined with `script_execution_context`).** The `WebEngineCustomJobCallbackData` hazard and the `script_execution_context` transfer hazard are the same problem in two halves. The fix has three parts:

1. **Change `WebEngineCustomJobCallbackData`'s storage** (`Libraries/LibWeb/Bindings/MainThreadVM.h:22-33`) from `OwnPtr<JS::ExecutionContext> active_script_context` to `NonnullOwnPtr<JS::ExecutionContext> active_script_context`, since by the point the struct is constructed the decision "should there be a script execution context?" has already been made in the caller (only constructed on the `if (script)` path). If the `Optional`-ness is actually needed at the callback site, keep `OwnPtr<ExecutionContext>` — what changes is how the struct *participates in tracing*.

2. **Give `JobCallback::CustomData` a virtual `visit_edges(Cell::Visitor&)` hook** (`Libraries/LibJS/Runtime/JobCallback.h`), default-empty. Override it in `WebEngineCustomJobCallbackData`:
   ```cpp
   virtual void visit_edges(JS::Cell::Visitor& visitor) override
   {
       visitor.visit(incumbent_settings);
       if (active_script_context)
           active_script_context->visit_edges(visitor);
   }
   ```
   Then update `JobCallback::visit_edges` (`Libraries/LibJS/Runtime/JobCallback.cpp:19-23`) to add `if (m_custom_data) m_custom_data->visit_edges(visitor);`. At that point the custom data's internal `OwnPtr<ExecutionContext>` is correctly traced through its `Cell` owner.

3. **Rewrite the construction path in `host_make_job_callback` to build the `ExecutionContext` directly inside the struct, not as a rooted stack local.** Since the struct now forwards tracing, the `ExecutionContext` is protected as soon as the struct is constructed. The window of concern is between `ExecutionContext::create(...)` returning a populated (well, initially-empty — see below) `NonnullOwnPtr` and the `WebEngineCustomJobCallbackData` ctor receiving it. That's the standard bucket-A shape: `ExecutionContext::create` goes through the pool allocator (no GC), and the field population happens on the struct's member. The cleanest form passes the ctor args through:
   ```cpp
   // In MainThreadVM.h, give WebEngineCustomJobCallbackData a ctor that
   // allocates the ExecutionContext internally:
   WebEngineCustomJobCallbackData(
       HTML::EnvironmentSettingsObject& incumbent_settings,
       u32 regs_locals, ReadonlySpan<JS::Value> constants, u32 args)
       : incumbent_settings(incumbent_settings)
       , active_script_context(JS::ExecutionContext::create(regs_locals, constants, args))
   {
   }
   ```
   And in `host_make_job_callback`:
   ```cpp
   OwnPtr<WebEngineCustomJobCallbackData> host_defined;
   if (script) {
       host_defined = make<WebEngineCustomJobCallbackData>(
           incumbent_settings, 0, ReadonlySpan<JS::Value>{}, 0);
       host_defined->active_script_context->function = nullptr;
       host_defined->active_script_context->realm = &script->settings_object().realm();
       // ... script_or_module population ...
   } else {
       host_defined = make<WebEngineCustomJobCallbackData>(incumbent_settings);
       // requires a second ctor that doesn't allocate an ExecutionContext —
       // or, if active_script_context becomes NonnullOwnPtr as in option 1
       // above, the no-script branch doesn't construct the struct at all
       // and threads a null custom_data through JobCallback::create.
   }
   return JS::JobCallback::create(*s_main_thread_vm, callable, move(host_defined));
   ```
   The `OwnPtr<WebEngineCustomJobCallbackData> host_defined` local itself is not a plugin violation — `WebEngineCustomJobCallbackData` contains GC pointers, but its `visit_edges` is now wired in, and the plugin's existing "OwnPtr to a Cell-adjacent thing with `visit_edges`" allowlist handling should cover it. If the plugin disagrees, the simplest workaround is to hoist the allocation into `JS::JobCallback::create` itself (`Libraries/LibJS/Runtime/JobCallback.cpp:26-30`) so the `OwnPtr` local lives inside `JobCallback` from the start — but that's a larger change and probably unnecessary.

After all three parts land, the five `IGNORE_GC` + `FIXME` annotations this branch added come off:

- `dummy_execution_context` → `Optional<RootedExecutionContext>` + `.emplace(...)` (bucket A, rows 4-ish of the table).
- `new_context` in `Realm.cpp` → direct `RootedExecutionContext` ctor (bucket A).
- `async_context` in `ECMAScriptFunctionObject.cpp` → direct `RootedExecutionContext` copy ctor (bucket B, solved by the ctor).
- The `ExecutionContext.cpp:108` annotation inside `copy()` → either leave with a rewritten FIXME (if `copy()` stays an internal call), or comes off once the `RootedExecutionContext` copy ctor is the only caller.
- `script_execution_context` + `host_defined` → merged bucket-C fix above.

**Step 5: internal-only forwarders (optional, related but separate).** Six sites currently hand-write `m_execution_context->visit_edges(visitor)` inside their Cell's own `visit_edges`:

```
Libraries/LibJS/Runtime/GeneratorObject.cpp:73
Libraries/LibJS/Runtime/AsyncGenerator.cpp:63
Libraries/LibJS/Runtime/AsyncFunctionDriverWrapper.cpp:239
Libraries/LibJS/SourceTextModule.cpp:57
Libraries/LibWeb/HTML/Scripting/Environments.cpp:67
Libraries/LibJS/Runtime/VM.cpp:318   (per-frame loop in gather_roots)
```

These aren't incorrect today — they're the existing "manually-traced owner" pattern and they work correctly. They also don't trigger the plugin, because each one's enclosing class is a `Cell` whose `visit_edges` the plugin trusts. Leaving them alone is fine. If consistency matters, one could either convert these classes to hold `RootedExecutionContext` members (not worth it — they already get free tracing from their Cell owner) or adopt option 2 (teach the plugin about manually-traced owners and leave the call shape as-is). No action required for the branch's `IGNORE_GC` cleanup.

**Step 6: tests.** Add `Tests/LibJS/TestRootedExecutionContext.cpp` (tests live in LibJS because `RootedExecutionContext` itself is a LibJS type). Construct a `RootedExecutionContext` with known GC pointer fields (e.g. a stub `FunctionObject*`), call `VM::gather_roots()` directly, and assert the inner cells show up in the roots map with `HeapRoot::Type::VM`. Also cover: destruction unregisters, the copy-ctor variant populates before the next GC sees the new wrapper, and `Optional<RootedExecutionContext>::emplace(...)` round-trips correctly. Use the `gather_roots`-direct pattern described in "Test Status" below rather than relying on GC/conservative-scanning to avoid the dangling-reference and conservative-stack issues documented there.

**Out of scope for option 1 (and for this doc):** the ~130-file ExecutionContext → Cell conversion.

### 5. Other gaps worth keeping in mind (not audited for concrete instances)

- **`std::vector` / `std::map` with GC pointers**: plugin only checks `AK::*` containers. If any part of the codebase uses `std::` containers with cell pointers, it slips through. Worth a one-time grep; these types are discouraged elsewhere in the codebase so the hit rate should be near zero.
- **Raw pointers to containers** (`Vector<GC::Ptr<T>>* p`): the variable type isn't a `TemplateSpecializationType` of a known container, so the recursive walker doesn't look through it. Rare but catastrophic if used.
- **Custom AK-adjacent containers**: `AK::CircularQueue`, `AK::RedBlackTree`, `AK::DoublyLinkedList`, `AK::Queue`, etc. are not on the `types_with_gc_invisible_storage` list. Any of those holding GC pointers would evade the check. `AK::IntrusiveList` is safe — it doesn't own storage.
- **`memcpy` / `bit_cast` of `GC::Ptr` / containers**: no tool catches raw bit moves of GC pointers. Unusual in this codebase but worth a note.

## Known Issues / In Progress

### Test Status

All 26 container tests pass in `Tests/LibGC/TestGCContainers.cpp`. The clang plugin lit suite reports 30 passing, 1 expected failure (`strong_root_fields_in_gc_allocated_types.cpp`, unrelated to this work), and 2 excluded out of 33 total — including the new `container_downgrade_blocked.cpp` test added for the compile-time slicing block.

Tests call `gather_roots()` / `for_each_possible_value()` / `visit_edges()` directly to verify root/value reporting, avoiding GC/conservative-scanning interference. This was chosen after discovering that:
- **Dangling `bool&` references**: Using `TestCell` with `bool& m_was_finalized` and a shared static heap caused cross-test finalization UB.
- **Conservative stack scanning**: `GC::Weak<T>` approach failed because stale stack pointers kept cells alive, making "should be collected" tests unreliable.
- **Minimum cell size**: Empty `TestCell` hit `VERIFY(cell_size >= sizeof(FreelistEntry))` — see below.

### Possible bug: minimum Cell size not enforced

`HeapBlock::create_with_cell_size` has `VERIFY(cell_size >= sizeof(FreelistEntry))`, which means a `Cell` subclass with no fields will fail to allocate if `sizeof(Cell) < sizeof(FreelistEntry)`. This was hit when `TestCell` had no members. Ideally this would be a compile-time check (e.g., a `static_assert` in `GC_CELL` or `GC_DECLARE_ALLOCATOR`) rather than a runtime `VERIFY` crash. The GC could also enforce the minimum internally (e.g., `max(cell_size, sizeof(FreelistEntry))` in the allocator). Filed as a separate concern from this branch's work.

### Remaining Violations (LibWeb)

After the rebase onto master and the editing/JSONObject fixes, the current violation count from a clean LibWeb build (captured in `/tmp/libweb_violations.txt`) is **246 entries**.

Violation counts by directory:

| Directory | Count |
|---|---|
| LibWeb/Layout | 49 |
| LibWeb/HTML | 36 |
| LibWeb/DOM | 33 |
| LibWeb/SVG | 23 |
| LibWeb/Crypto | 23 |
| LibWeb/Editing | 19 (pre-fix count; now ~0 editing-specific) |
| LibWeb/Editing/Internal | 18 (pre-fix count; now ~0 editing-specific) |
| LibWeb/CSS | 12 |
| LibWeb/Bindings | 5 |
| LibWeb/IndexedDB (+ Internal) | 5 |
| LibWeb/HTML/Scripting | 3 |
| LibWeb/CSS/Parser | 3 |
| LibWeb/WebAssembly | 2 |
| LibWeb/Painting | 2 |
| LibWeb/Animations | 2 |
| Others (XPath, XHR, WebIDL, ViewTransition, etc.) | 1 each |

Violation counts by type (top entries):

| Type | Count |
|---|---|
| `Vector<GC::Ref<DOM::Node>>` | 33 (pre-fix, now 0) |
| `OwnPtr<FormattingContext>` | 14 |
| `HashTable<const SVGGradientElement*>` | 14 |
| `Web::Crypto::NormalizedAlgorithmAndParameter` | 11 (struct) |
| `WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter>` | 10 |
| `HashTable<const SVGPatternElement*>` | 8 |
| `Vector<Slottable>` | 7 |
| `Vector<GC::Ref<DOM::Element>>` | 7 |
| `LayoutState` | 7 (struct transitively contains GC) |
| `Vector<JS::Value>` | 6 |
| `Vector<GC::Ref<Node>>` | 4 |
| `Vector<GC::Ref<MimeType>>` | 4 |
| `OrderedHashMap<FlyString, GC::Ref<Navigable>>` | 4 |
| `NonnullOwnPtr<FormattingContext>` | 4 |
| `Vector<GC::Ref<Plugin>>` | 3 |
| `OwnPtr<JS::ExecutionContext>` | 3 |
| `NonnullOwnPtr<ComputedValues>` | 3 |
| ...and many more single-occurrence patterns |

### CSS Progress (in progress)

Started fixing CSS violations. Completed:
- `LibWeb/CSS/ComputedValues.h:741` — `NonnullOwnPtr<ComputedValues> clone_inherited_values()` marked with `IGNORE_GC` + FIXME. Added `#include <LibGC/Cell.h>`.
- `LibWeb/CSS/CSSFontFeatureValuesMap.cpp:53` — `Vector<JS::Value>` → `GC::RootVector<JS::Value>`.
- `LibWeb/CSS/FontFaceSet.cpp:145` — `Vector<JS::Value>` → `GC::RootVector<JS::Value>`.
- `LibWeb/CSS/CSSTransformValue.cpp:36` — `Vector<GC::Ref<CSSTransformComponent>>` → `GC::RootVector`.
- `LibWeb/CSS/CSSUnparsedValue.cpp:22` — `Vector<CSSUnparsedSegment>` → `GC::ConservativeVector`.

Still to do in CSS:
- `ComputedProperties.cpp:1137` — `ContentData` struct (transitively GC)
- `ComputedProperties.cpp:1942` — `Vector<AnimationProperties>`
- `CountersSet.cpp:164` — `OwnPtr<CountersSet>`
- `Parser/Helpers.cpp:28,42` — `OwnPtr<JS::ExecutionContext>`, `NonnullOwnPtr<HostDefined>`
- `Parser/RuleParsing.cpp:1404` — `Vector<GC::Ref<CSSRule>>`
- `StyleComputer.cpp:1947,2127` — `Vector<DOM::AbstractElement>`, `WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>>`
- `StyleScope.cpp:215,535` — `Vector<MatchingRule>`, `HashTable<DOM::Element*>`

### Plugin extension attempted and reverted (superseded for slicing by the compile-time block)

During this session, I attempted to extend `VisitVarDecl` to also flag by-value function parameters (since `RootVector` passed by value slices to `Vector` on the callee stack, losing the Root property). This surfaced 4 violations in LibJS headers:

1. `Module.h:81` — `GraphLoadingState` constructor takes `HashTable<GC::Ptr<CyclicModule>>` by value. Fixed by changing to rvalue reference (`HashTable<GC::Ptr<CyclicModule>>&&`).
2. `Module.h:117` — `resolve_export(vm, name, Vector<ResolvedBinding> resolve_set = {})`. This is a spec-driven recursion accumulator pattern that's hard to refactor without changing semantics. Would require updating all callers and overrides in SourceTextModule.h, SyntheticModule.h, and the .cpp files.
3. `Executable.h:118` — `NonnullOwnPtr<PropertyKeyTable>` constructor parameter. Fixed by changing to rvalue reference.
4. `Executable.h:121` — `Vector<Value>` constructor parameter. Fixed by changing to rvalue reference.

**Status**: Reverted the plugin extension for now with a FIXME comment in the plugin code and test file. The `Executable.h` and `Module.h:81` rvalue reference fixes were also reverted (they were preparatory). The `resolve_export` refactor would need a dedicated pass to update all overrides.

**Partially superseded**: The slicing aspect of this concern (RootVector passed by value to a Vector parameter) is now caught at compile time by the deleted constrained ctor in `AK::Vector`/`AK::HashMap`/`AK::HashTable` — see "Compile-time block for GC container downgrades" above. The deferred parameter check is still useful for catching plain `Vector<GC::Ref<T>>` parameters that aren't fed from a Root container at any call site, but the most common slicing path is now covered without it.

Current `VisitVarDecl` behavior: skips `ParmVarDecl` entirely. Only checks local and static/global variables.

### Commit Reordering

Before merging, the plugin enforcement commit needs to be reordered to come AFTER all violations are fixed. Use:
```
git rebase -i 094e4bacf8
```
And move that commit to the end.

### Future LibGC tests (not directly related to this branch)

LibGC had no tests before this branch. The container tests are a start, but broader GC correctness tests would be valuable:

- **GC::Root** — basic root handle prevents collection; destroying it allows collection
- **GC::Weak** — becomes null after referent is collected, stays valid while alive
- **Cell tracing (visit_edges)** — a rooted cell referencing another cell keeps both alive; breaking the reference allows the child to be collected
- **Transitive tracing** — A → B → C chain; root A, all survive; unroot A, all collected
- **Cycle collection** — two cells referencing each other with no external root should both be collected (mark-and-sweep handles this)
- **DeferGC** — GC doesn't run while deferred, runs when the deferral ends
- **must_survive_garbage_collection** — cells that override this stay alive regardless of roots

Note: tests that verify collection happened are tricky due to conservative stack scanning keeping cells alive via stale stack pointers. The `NEVER_INLINE` helper function approach (allocate in a separate stack frame) works for this.

## Architecture: Container Type Decision Tree

```
Is the container owned by a Cell (member field)?
  YES -> Use Heap* variants (GC-allocated, traced via visit_edges):
  |      - HeapVector<T>
  |      - HeapHashTable<T>
  |      - HeapHashMap<K, V>
  |
  NO -> Is it a local/stack variable?
        |
        Does the element type convert directly to Cell*?
          (GC::Ref<T>, GC::Ptr<T>, T* where T : Cell, NanBoxedValue)
          |
          YES -> Use Root* variants (precise tracing, registers with Heap):
          |      - RootVector<GC::Ref<T>>
          |      - RootHashTable<GC::Ref<T>>
          |      - RootHashMap<K, GC::Ref<T>>
          |
          NO -> Does the element contain GC pointers in non-standard layout?
                (PropertyKey with tagged Symbol*, structs with GC members)
                |
                YES -> Use Conservative* variants (scans live entry bytes):
                |      - ConservativeVector<PropertyKey>
                |      - ConservativeHashTable<PropertyKey>
                |      - ConservativeHashMap<u32, ValueAndAttributes>
                |
                NO -> Not a GC concern, plain container is fine
```

## Compile-time block for GC container downgrades (slicing protection)

After exploring whether to add a plugin check for slicing GC root containers (e.g. `Vector<T> v = root_vec`), we landed on a stronger approach: **deleted constrained overloads in `AK::Vector`, `AK::HashMap`, and `AK::HashTable` that block construction and assignment from any strict descendant**. This protection runs in every translation unit regardless of whether the plugin is enabled, and it covers compilers other than clang.

### The shape of the protection

In each of `AK/Vector.h`, `AK/HashMap.h`, `AK/HashTable.h`:

```cpp
// Block slicing: any *strict* descendant of Vector cannot be passed to a
// Vector constructor or assignment operator.
template<typename Derived>
requires(IsBaseOf<Vector, RemoveCVReference<Derived>> && !IsSame<Vector, RemoveCVReference<Derived>>)
Vector(Derived&&) = delete;

template<typename Derived>
requires(IsBaseOf<Vector, RemoveCVReference<Derived>> && !IsSame<Vector, RemoveCVReference<Derived>>)
Vector& operator=(Derived&&) = delete;
```

The forwarding-reference signature `Derived&&` captures **every value category** via reference collapsing (`Derived` deduces to `T&` for lvalues, `T` for rvalues). The `RemoveCVReference` strip + `IsBaseOf` + `!IsSame` constraint then checks "is the source a strict descendant of this container?". If yes, the deleted overload wins overload resolution (identity match beats derived-to-base) and produces a compile error.

### What's blocked vs. what's still allowed

**Blocked at compile time** — every form of slicing construction from a Root/Conservative container:

- Lvalue copy: `Vector<T> v = root_vec;`
- Rvalue move: `Vector<T> v = move(root_vec);`
- Prvalue from a function returning a Root container by value
- Pass-by-value: `f(root_vec)` where `f` takes `Vector<T>` by value
- Return-by-value: `Vector<T> foo() { GC::RootVector<T> tmp(...); return tmp; }`
- Member-init slicing: `: m_vec(root_vec)` where `m_vec` is `Vector<T>`
- Assignment slicing: `vec = root_vec;` (via the deleted `operator=`)

**Still allowed** — these don't go through a constructor call, so the deleted overload isn't in the overload set:

- Reference binding to `Vector const&` parameter (no copy, just a base-class reference bind)
- Reference binding to `Vector&` parameter (mutating through the reference mutates the underlying RootVector storage, which is still rooted)
- `RootVector → RootVector` copy and move (the explicit copy ctor uses `static_cast<VectorBase const&>(other)` to bypass the deleted overload, and the destination is still registered with the heap)

### Why the static_cast plumbing is needed in LibGC

`RootVector` and friends inherit publicly from their AK counterparts and have explicit copy/move constructors that chain through the base. With the deleted overload in place, those chain-throughs need to manually cast to `Vector const&`/`Vector&` so the non-template overload (`Vector(Vector const&)`) is selected instead of the deleted derived overload. The pattern looks like:

```cpp
RootVector(RootVector const& other)
    : RootVectorBase(*other.m_heap)
    , Vector<T, inline_capacity>(static_cast<VectorBase const&>(other))  // bypass deleted overload
{
}
```

Updated in: `Libraries/LibGC/RootVector.h`, `Libraries/LibGC/RootHashMap.h`, `Libraries/LibGC/RootHashTable.h`, `Libraries/LibGC/ConservativeVector.h`, `Libraries/LibGC/ConservativeHashMap.h`, `Libraries/LibGC/ConservativeHashTable.h`.

`ConservativeHashMap` and `ConservativeHashTable` previously had no explicit copy/move ctors at all — and because their `*Base` classes are marked `AK_MAKE_NONCOPYABLE`, the implicit copy ctor was deleted by the noncopyable base, leaving the whole class effectively non-copyable. This was an inconsistency with `ConservativeVector` (which has always been copyable via explicit ctors that bypass the noncopyable base by directly calling `ConservativeVectorBase(*other.m_heap)`). They now follow the same pattern: explicit copy ctor, move ctor, and copy `operator=` that chain through `static_cast<HashMapBase const&>(other)` / `static_cast<HashTableBase const&>(other)` to bypass the deleted derived overload in `AK::HashMap`/`AK::HashTable`.

### Test coverage

`Tests/ClangPlugins/LibJSGCTests/container_downgrade_blocked.cpp` — compile-failure test using clang's `-verify` mode. Covers:

- **13 positive cases** (each must produce a `call to deleted constructor` or `overload resolution selected deleted operator '='` error):
  - Vector: lvalue copy, rvalue move, prvalue, return slicing, mem-init slicing, assignment slicing, ConservativeVector slicing
  - HashMap: pass-by-value, assignment slicing, ConservativeHashMap slicing
  - HashTable: pass-by-value, assignment slicing, ConservativeHashTable slicing
- **7 negative cases** (must compile cleanly):
  - Vector: `Vector const&` binding, `Vector&` binding, RootVector→RootVector copy, RootVector→RootVector move
  - HashMap: `HashMap const&` binding, RootHashMap→RootHashMap copy
  - HashTable: `HashTable const&` binding, RootHashTable→RootHashTable copy

The RUN line uses `-Xclang -verify -Xclang -verify-ignore-unexpected=note` because deleted-overload errors come bundled with `note: candidate function ... has been explicitly deleted` notes that would otherwise trip clang's strict verify mode.

### Newly surfaced violations from the compile-time block

Adding the deleted overloads immediately surfaced four real RootVector→Vector slicing sites in LibWeb that compile cleanly today on master but now fail to compile on this branch. These need to be fixed as part of getting the branch to build:

- `Libraries/LibWeb/CSS/CSSTransformValue.cpp:40` — `Vector<GC::Ref<CSSTransformComponent>>` constructed from a `RootVector`
- `Libraries/LibWeb/CSS/CSSUnparsedValue.cpp:29` — `Vector<CSSUnparsedSegment>` constructed from a `ConservativeVector`
- `Libraries/LibWeb/Editing/Internal/Algorithms.cpp:4275, 4292` — two `Vector<GC::Ref<DOM::Node>>` slicing sites

(No HashMap/HashTable downgrade sites surfaced from the LibGC build, suggesting downgrades of those container types are rarer in the codebase.)

### Relationship to the deferred parameter check

The earlier "Plugin extension attempted and reverted" section described an attempt to extend `VisitVarDecl` to flag by-value function parameters of `Vector<GC::Ref<T>>`. The compile-time block we added now covers the **slicing aspect** of that concern: a `RootVector` passed to a function taking `Vector<T>` by value now fails to compile at the call site. It does **not** cover the case where a function declares a `Vector<GC::Ref<T>>` parameter and is only called from sources that are themselves plain (already-flagged) `Vector<GC::Ref<T>>`. Those still need the parameter check (deferred — see below).

## IGNORE_GC Usage Policy

`IGNORE_GC` (macro for `[[clang::annotate("serenity::ignore_gc")]]`) should be used sparingly with justification:

**Acceptable:**
- Static variables that are manually managed within a call scope (e.g., `s_array_join_seen_objects`)
- `NonnullOwnPtr<T>` where T transitively contains GC pointers and there's no GC-allocated alternative yet (add FIXME)
- References to VM-owned data that outlives the scope (e.g., CalendarFieldData pointing to `vm.names.*`)
- Return type constraints where the container type can't be changed without interface changes
- Debug utilities with no heap access (e.g., `Print.cpp`)

**Not acceptable:**
- Any container that could use a Root*/Conservative*/Heap* variant
- Laziness — if a heap reference is available, use the proper type

## Files Changed Summary

### New files
- `Libraries/LibGC/RootHashTable.h` / `.cpp`
- `Libraries/LibGC/ConservativeHashTable.h` / `.cpp`
- `Libraries/LibGC/ConservativeHashMap.h` / `.cpp`
- `Libraries/LibGC/HeapHashMap.h`
- `Tests/LibGC/TestGCContainers.cpp` / `CMakeLists.txt`
- `Tests/ClangPlugins/LibJSGCTests/local_var_gc_in_container.cpp`
- `Tests/ClangPlugins/LibJSGCTests/root_container_non_gc_type.cpp`
- `Tests/ClangPlugins/LibJSGCTests/container_downgrade_blocked.cpp` — compile-failure tests for the slicing block

### Modified infrastructure
- `AK/Vector.h` — deleted constrained ctor and operator= blocking slicing from strict descendants
- `AK/HashMap.h` — same
- `AK/HashTable.h` — same
- `Libraries/LibGC/Heap.h` / `.cpp` — registration for new container types, scanning loops, debug dump
- `Libraries/LibGC/HeapRoot.h` — new HeapRoot::Type entries
- `Libraries/LibGC/Forward.h` — forward declarations
- `Libraries/LibGC/CMakeLists.txt` — new source files
- `Libraries/LibGC/RootVector.h` — static_assert for non-GC types; static_cast through VectorBase in copy ctor and copy operator= to bypass deleted overload
- `Libraries/LibGC/RootHashMap.h` — key tracing, IsConvertible guards, static_assert; static_cast through HashMapBase in copy ctor and copy operator=
- `Libraries/LibGC/RootHashTable.h` — IsConvertible guard, static_assert; static_cast through HashTableBase in copy ctor and copy operator=
- `Libraries/LibGC/ConservativeVector.h` — static_cast through Vector base in copy ctor and copy operator=
- `Libraries/LibGC/ConservativeHashMap.h` — added explicit copy ctor, move ctor, and copy operator= (previously implicitly non-copyable due to noncopyable base)
- `Libraries/LibGC/ConservativeHashTable.h` — same
- `Meta/Lagom/ClangPlugins/LibJSGCPluginAction.h` / `.cpp` — VisitVarDecl, exclusion lists, HeapHashMap recognition

### LibJS fixes (25 files)
- `Bytecode/Interpreter.cpp` — RootHashTable, ConservativeHashTable, ConservativeVector, RootVector
- `Console.cpp` — ConservativeHashTable (was HashMap<PropertyKey, bool> used as set)
- `CyclicModule.cpp` / `.h` — RootVector<GC::Ptr<Module>>, ConservativeVector<LoadedModuleRequest>
- `Module.cpp` / `.h` — virtual signatures updated to rooted types
- `SourceTextModule.cpp` / `.h` — RootHashTable<GC::Ref<Module const>>, ConservativeVector<FunctionToInitialize>
- `SyntheticModule.cpp` / `.h` — signature update
- `Runtime/ArrayPrototype.cpp` — IGNORE_GC on static s_array_join_seen_objects
- `Runtime/ECMAScriptFunctionObject.cpp` — IGNORE_GC on OwnPtr<ExecutionContext> (FIXME)
- `Runtime/ExecutionContext.cpp` — IGNORE_GC on OwnPtr<ExecutionContext> (FIXME)
- `Runtime/FunctionPrototype.cpp` — RootVector<Value>
- `Runtime/IndexedProperties.cpp` / `.h` — ConservativeHashMap, added Heap& parameter
- `Runtime/MathObject.cpp` — RootVector<Value> (3 instances)
- `Runtime/Object.cpp` — IGNORE_GC on static s_intrinsics, ConservativeVector<NameAndDescriptor>
- `Runtime/PrimitiveString.cpp` — RootVector<PrimitiveString const*>
- `Runtime/ProxyObject.cpp` — ConservativeHashTable<PropertyKey>
- `Runtime/Realm.cpp` — IGNORE_GC on OwnPtr<ExecutionContext> (FIXME)
- `Runtime/Shape.cpp` — RootVector<GC::Ref<Shape const>>, RootHashTable<Shape*>
- `Runtime/Temporal/Calendar.cpp` — IGNORE_GC on CalendarFieldData vectors (VM-owned refs)
- `RustIntegration.cpp` — IGNORE_GC on OwnPtr<PropertyKeyTable> (FIXME), RootVector<Value>
- `Print.cpp` — IGNORE_GC (no heap available)

### Utilities fixes
- `js.cpp` — IGNORE_GC on OwnPtr<ExecutionContext> (FIXME)
- `wasm.cpp` — IGNORE_GC on OwnPtr<ExecutionContext> (FIXME), RootVector<Value>

### LibWeb fixes
- `WebAssembly/WebAssemblyModule.h` / `.cpp` — Updated `get_exported_names` virtual signature

## Build Instructions

The clang plugin requires LLVM 21 (matches CI):

```bash
cmake --preset Release \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang++ \
  -DClang_DIR=/opt/homebrew/opt/llvm@21/lib/cmake/clang \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@21/lib/cmake/llvm \
  -DENABLE_CLANG_PLUGINS=ON

# Build just LibJS (currently clean):
ninja -C Build/release LibJS

# Build GC container tests:
ninja -C Build/release TestGCContainers

# Run GC tests:
./Meta/ladybird.py run TestGCContainers

# Run clang plugin tests:
cd Build/release && ctest -R TestClangPlugins

# Full build (will show remaining LibWeb violations):
ninja -C Build/release -k0
```
