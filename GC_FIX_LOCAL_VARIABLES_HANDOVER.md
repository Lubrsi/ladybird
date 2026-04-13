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

1. **Teach the plugin the "manually-traced owner" pattern.** Recognize that a class holding `OwnPtr<T>` / `NonnullOwnPtr<T>` where `T` has a `visit_edges` method, *and* whose own `visit_edges` calls `->visit_edges(visitor)` on that member, is not a violation. This would eliminate the six Cell-held `OwnPtr<ExecutionContext>` sites from the false-positive list and generalise to any future non-Cell struct that follows the pattern. Does **not** fix transient locals.
2. **General-purpose `GC::ScopedRoots` primitive for transient locals.** Introduce a variadic RAII helper that registers stack-held variables with the heap for the lifetime of an enclosing scope, and have it forward through `visit_edges` so it composes with the existing tracing machinery. Replaces the transient-local `IGNORE_GC` sites and any future ones. Implementation plan spelled out below. Leaves the hot path (VM execution context stack, generator/module member storage) untouched.
3. **Make `ExecutionContext` a `GC::Cell`.** Fixes all of the above but pays the hot-path cost on every JS function call (push/pop of the VM stack becomes barrier traffic), loses the custom tail-sized pool allocator (`ExecutionContextAllocator` buckets by 4/16/64/128/256/512 Value slots), and cascades signature changes across ~130 files in LibJS and LibWeb (generators, modules, settings objects, and all creators). Probably not worth it until profiling shows GC pressure from contexts or the Cell allocator grows a pooled / variable-size variant.

**Recommended path:** option 1 as a standalone plugin improvement (clears the Cell-held OwnPtr false positives for free), then option 2 as a targeted refactor for the transient-local cluster. Defer option 3 unless a deeper reason emerges.

#### Implementation plan for option 2 (`GC::ScopedRoots`)

Goal: a one-liner RAII helper that a caller can drop next to a stack-held `OwnPtr<ExecutionContext>` (or any other non-Cell holder-of-GC-pointers) to make the contents traced for the lifetime of the enclosing scope, with no heap allocation, no lambda storage, and no per-type wrapper class.

**Target usage:**
```cpp
OwnPtr<JS::ExecutionContext> dummy_execution_context;
GC::ScopedRoots scoped(vm.heap(), dummy_execution_context);
// ... code that may allocate; ctx's inner GC pointers are traced ...
```

Construction deduces `Ts...` from the arguments (C++17 CTAD); the helper stores raw pointers to the stack slots and unregisters on destruction. Because variables are tracked by pointer, reassignment mid-scope is fine — the GC always reads the current value at trace time.

**Timing rule (important).** `ScopedRoots` protects a tracked variable from the moment the helper is constructed onward, *not retroactively*. The variable must not contain GC-relevant state at the moment the helper registers. Two patterns to keep straight:

```cpp
// SAFE: variable is empty at registration; populated afterwards under protection.
OwnPtr<JS::ExecutionContext> ctx;
GC::ScopedRoots scoped(vm.heap(), ctx);
ctx = JS::ExecutionContext::create(...);
ctx->realm = realm;
```

```cpp
// UNSAFE: ctx already holds GC pointers when ScopedRoots registers — there is
// a window between the populated-return and the registration line.
auto ctx = running_execution_context.copy();   // copy() returns populated state
GC::ScopedRoots scoped(vm.heap(), ctx);        // too late
```

The boundary is "first GC-relevant write happens after registration", not "declaration before registration". An `auto x = ExecutionContext::create(...);` is fine because `create()` returns an `ExecutionContext` whose GC pointer fields are all default-null at that moment — the dangerous writes are the field assignments below it. An `auto x = source.copy();` is *not* fine because `copy()` populates all fields before returning.

**Sites where the populated-return pattern is mandatory** need either a different primitive (e.g. `GC::ScopedRoot<T>` — a singular owning wrapper that performs registration *before* the held value is initialized, sketched below) or a site-specific refactor (e.g. give `copy()` a destination parameter so the populated writes happen after the destination is registered). Don't try to retrofit `ScopedRoots` onto these sites; the protection genuinely doesn't extend backwards.

**Step 1: add overloads to `GC::Cell::Visitor` (`Libraries/LibGC/Cell.h:66-185`) for non-Cell holders-of-GC-pointers.**

The motivating type is `JS::ExecutionContext`, whose `visit_edges` is **non-const** (`Libraries/LibJS/Runtime/ExecutionContext.h:33`), and so are the existing `visit_edges` overrides on every `Cell`-derived class via `MUST_UPCALL virtual void visit_edges(Visitor&)` (`Cell.h:202`). The overloads must therefore take non-const references and constrain on the non-const method:

```cpp
// Non-Cell reference-to-object that has its own visit_edges method.
template<typename T>
void visit(T& obj)
requires requires(T& o, Visitor& v) { o.visit_edges(v); }
{
    obj.visit_edges(*this);
}

// Ownership wrappers around the above. The OwnPtr itself can be const
// (we only need the held T to be mutable), matching how OwnPtr<T>::operator->
// returns a non-const T* on a const OwnPtr<T>&.
template<typename T>
void visit(OwnPtr<T> const& ptr)
requires requires(T& t, Visitor& v) { t.visit_edges(v); }
{
    if (ptr) ptr->visit_edges(*this);
}

template<typename T>
void visit(NonnullOwnPtr<T> const& ptr)
requires requires(T& t, Visitor& v) { t.visit_edges(v); }
{
    ptr->visit_edges(*this);
}
```

A const overload (`visit(T const&) requires { o.visit_edges(v); }` with a const-callable `visit_edges`) is not added in this step because no current type needs it — `ExecutionContext::visit_edges` is non-const and so is every Cell's `visit_edges`. Add a const-receiver overload only if a future non-Cell type chooses to declare its `visit_edges` as `const`.

Overload resolution stays correct: the non-template `visit(Cell*)` / `visit(Cell&)` in `Cell.h:66-84` is more specific than a constrained template, so Cell-derived types retain their current "mark as root; tracer later drives visit_edges" semantics. Non-Cell types (like `JS::ExecutionContext`) only match the new constrained template, which forwards straight through `visit_edges`. Types with no `visit_edges` method fail the requires-clause and produce a compile error — same footgun surface as today, just expressed at the type-system level instead of hand-written forwarders.

**Step 1b (fold-in cleanup, optional but nice):** six sites currently hand-write the forwarding call. Replace them with plain `visitor.visit(m_foo)` once the overloads exist:

```
Libraries/LibJS/Runtime/GeneratorObject.cpp:73
Libraries/LibJS/Runtime/AsyncGenerator.cpp:63
Libraries/LibJS/Runtime/AsyncFunctionDriverWrapper.cpp:239
Libraries/LibJS/SourceTextModule.cpp:57
Libraries/LibWeb/HTML/Scripting/Environments.cpp:67
Libraries/LibJS/Runtime/VM.cpp:318   (per-frame loop in gather_roots)
```

Keeps the call shape consistent with the rest of `visit_edges` code across the codebase.

**Step 2: new `Libraries/LibGC/ScopedRoots.h` / `.cpp`, following the `RootVector` template.**

```cpp
// ScopedRoots.h (sketch)
namespace GC {

class GC_API ScopedRootsBase {
public:
    virtual void visit_edges(Cell::Visitor&) = 0;

protected:
    explicit ScopedRootsBase(Heap&);
    ~ScopedRootsBase();

    Heap* m_heap { nullptr };
    IntrusiveListNode<ScopedRootsBase> m_list_node;

public:
    using List = IntrusiveList<&ScopedRootsBase::m_list_node>;
};

template<typename... Ts>
class ScopedRoots final : public ScopedRootsBase {
public:
    explicit ScopedRoots(Heap& heap, Ts&... values)
        : ScopedRootsBase(heap)
        , m_refs(&values...)
    {
    }

    ScopedRoots(ScopedRoots const&) = delete;
    ScopedRoots(ScopedRoots&&) = delete;
    ScopedRoots& operator=(ScopedRoots const&) = delete;
    ScopedRoots& operator=(ScopedRoots&&) = delete;

    virtual void visit_edges(Cell::Visitor& v) override
    {
        AK::apply([&](auto*... p) { (v.visit(*p), ...); }, m_refs);
    }

private:
    AK::Tuple<Ts*...> m_refs;
};

}
```

```cpp
// ScopedRoots.cpp (sketch) — mirrors RootVector.cpp:13-33
ScopedRootsBase::ScopedRootsBase(Heap& heap) : m_heap(&heap)
{ m_heap->did_create_scoped_roots({}, *this); }

ScopedRootsBase::~ScopedRootsBase()
{ m_heap->did_destroy_scoped_roots({}, *this); }
```

Deleting copy/move intentionally — a `ScopedRoots` is a stack-local registration; copying would double-register and moving across scopes defeats the point.

**Step 3: wire into `Heap`, mirroring `m_root_vectors` exactly.**

- `Heap.h:177-182`: add `ScopedRootsBase::List m_scoped_roots;` next to `m_root_vectors` / `m_root_hash_maps` / `m_root_hash_tables`.
- `Heap.h:213-223`: add inline `did_create_scoped_roots({}, ScopedRootsBase&)` / `did_destroy_scoped_roots({}, ScopedRootsBase&)` with the same `VERIFY(!list.contains(...))` + `append` / `remove` shape.
- `Heap.cpp:444-451`: in `gather_roots`, after the existing `m_root_hash_tables` loop, add:

  ```cpp
  class ScopedRootsGatheringVisitor final : public Cell::Visitor {
  public:
      explicit ScopedRootsGatheringVisitor(HashMap<Cell*, HeapRoot>& roots) : m_roots(roots) {}
      virtual void visit_impl(Cell& cell) override
      { m_roots.set(&cell, HeapRoot { .type = HeapRoot::Type::ScopedRoots }); }
      virtual void visit_impl(ReadonlySpan<NanBoxedValue> values) override
      {
          for (auto& value : values)
              if (value.is_cell())
                  m_roots.set(&const_cast<NanBoxedValue&>(value).as_cell(),
                              HeapRoot { .type = HeapRoot::Type::ScopedRoots });
      }
      virtual void visit_possible_values(ReadonlyBytes) override {}
  private:
      HashMap<Cell*, HeapRoot>& m_roots;
  };

  ScopedRootsGatheringVisitor shim(roots);
  for (auto& scope : m_scoped_roots)
      scope.visit_edges(shim);
  ```

  This adapter is the same shape as `ExecutionContextRootsCollector` at `VM.cpp:317` and the anonymous visitors at `Heap.cpp:150` and `Heap.cpp:640`. It bridges the `visit_edges`-style API into the `gather_roots` map.

**Step 4: new enumerator in `Libraries/LibGC/HeapRoot.h:16-29`: `ScopedRoots`.** Used by the gathering visitor's tag. The enum is also exhaustively switched on (no `default:` arm) in the heap graph dump path at `Libraries/LibGC/Heap.cpp:212-253` — that switch needs a new `case HeapRoot::Type::ScopedRoots: node.set("root"sv, "ScopedRoots"sv); break;` arm to keep the build green. Search for any other exhaustive switch over `HeapRoot::Type` before landing the change (none today, but worth grepping in case a debug tool gets one before this lands).

**Step 5: plugin work — *not* a simple allowlist add.** This is the part that determines whether the `IGNORE_GC` sites can actually come off, and the answer is "only after a flow-sensitive plugin extension".

The current `LibJSGCVisitor::VisitVarDecl` (`Meta/Lagom/ClangPlugins/LibJSGCPluginAction.cpp:978-1018`) flags variables purely based on type — at line 1003 it reads `var->getType()` and at line 1009 calls `type_has_unrooted_gc_container(type)`. It never inspects the surrounding statements. So a sibling `GC::ScopedRoots scoped(heap, ctx);` line in the same scope does nothing for the diagnostic on `OwnPtr<JS::ExecutionContext> ctx;`. Allowlisting `GC::ScopedRoots` (the helper's own type) would only suppress diagnostics on the helper itself — which doesn't carry GC pointers in its visible type anyway — and would not unblock the surrounding `OwnPtr<...>` declarations.

Two viable approaches, in order of preference:

1. **Flow-sensitive same-scope check (preferred).** Extend `VisitVarDecl` so that when an unrooted GC container is detected, it walks the parent `CompoundStmt` (the enclosing block) looking for a later statement that constructs `GC::ScopedRoots` with a `DeclRefExpr` referencing this var in its argument list. If found, the diagnostic is suppressed. This mirrors the existing same-function-body matcher used for `visit_edges` field-access verification (the `gc_allocated_member_is_accessed.cpp` test path), so the plugin already has the AST-walking primitives. Caveats to call out in the implementation: must require the `ScopedRoots` construction to be in the *same* `CompoundStmt` as the declaration, not just anywhere in the function (otherwise a conditional registration would silently disarm the check); and must reject declarations that appear *after* their `ScopedRoots` (since the registration must outlive the variable to do anything useful — `ScopedRoots` is destroyed first, leaving the variable unrooted for the rest of its lifetime).

2. **Explicit annotation fallback (uglier, but trivial to implement).** Define a `GC_SCOPED_ROOTS_LOCAL` macro that expands to `[[clang::annotate("serenity::scoped_roots_local")]]`, and have `VisitVarDecl` add it to the existing `serenity::ignore_gc` check at line 1000-1001. Migration becomes:
   ```cpp
   GC_SCOPED_ROOTS_LOCAL OwnPtr<JS::ExecutionContext> dummy;
   GC::ScopedRoots scoped(vm.heap(), dummy);
   ```
   Two lines per site instead of (ideally) one, plus a noisy macro on every declaration. Worth keeping as an escape hatch even if (1) lands, for cases where the flow check can't statically prove the registration.

**Until step 5 lands, the `IGNORE_GC` annotations from commit `81f3ead348` cannot be removed.** Step 6 below assumes step 5 is in place.

**Step 6: migrate the `IGNORE_GC` sites in this branch.** Each site needs to be classified by its initialization shape against the timing rule. Three buckets:

**Bucket A — empty at registration, populated afterwards.** Sibling `ScopedRoots` works. The migration deletes one `IGNORE_GC` + `FIXME` line and inserts a `ScopedRoots` line right after the declaration, before any field assignment.

| File | Variable | Why bucket A |
|---|---|---|
| `Libraries/LibJS/Runtime/ExecutionContext.cpp:108` (copy) | `copy` | `create()` returns an `ExecutionContext` with all GC pointer fields default-null. The populated writes (`copy->function = function;` ...) start on the next line. Insert `ScopedRoots` between `create()` and the first field write. |
| `Libraries/LibJS/Runtime/Realm.cpp:42` (initialize_host_defined_realm) | `new_context` | Same shape — `create()` returns empty, fields populated below. |
| `Libraries/LibWeb/Bindings/MainThreadVM.cpp:287` (host_enqueue_promise_job) | `dummy_execution_context` | Declared as a default-constructed `OwnPtr` (empty), assigned later via `dummy_execution_context = JS::ExecutionContext::create(...)`. `ScopedRoots` immediately after the declaration is correct. |
| `Libraries/LibWeb/Bindings/MainThreadVM.cpp:347` (host_make_job_callback) | `script_execution_context` | Same shape — empty `OwnPtr` at declaration, populated below. |

**Bucket B — populated at the moment of declaration.** Sibling `ScopedRoots` cannot help; the variable already holds GC pointers before registration. These sites need either a refactor or a different primitive.

| File | Variable | Why bucket B |
|---|---|---|
| `Libraries/LibJS/Runtime/ECMAScriptFunctionObject.cpp:437` (async context copy) | `async_context` | Initialised from `running_context.copy()`, which populates *all* GC fields before returning. The returned `NonnullOwnPtr` already holds live GC pointers when control reaches the next line. |

Two ways forward for bucket B:

- *Refactor approach.* Give `ExecutionContext::copy()` a destination parameter (e.g. `void copy_to(ExecutionContext& destination) const;`). Caller does `OwnPtr<ExecutionContext> ac; GC::ScopedRoots scoped(heap, ac); ac = ExecutionContext::create(...); running_context.copy_to(*ac);` — every populated write happens under `ScopedRoots` protection. Mechanical change, one extra line at the call site, but touches `copy()`'s API.
- *Owning-wrapper primitive (`GC::ScopedRoot<T>`, singular).* A separate helper that performs registration *before* taking ownership of its held value. Out-of-scope for this implementation plan but worth sketching the shape so the bucket-B sites have an obvious target:
  ```cpp
  template<typename T>
  class ScopedRoot final : public ScopedRootsBase {
  public:
      template<typename Factory>
      ScopedRoot(Heap& heap, Factory&& factory)
          : ScopedRootsBase(heap)
          , m_held(/* default / empty */)
      {
          // Registration has already happened in ScopedRootsBase's ctor.
          m_held = AK::forward<Factory>(factory)();
      }
      void visit_edges(Cell::Visitor& v) override { v.visit(m_held); }
      T* operator->() { return m_held.ptr(); }
      T& operator*() { return *m_held; }
  private:
      OwnPtr<T> m_held;
  };
  // Usage:
  GC::ScopedRoot<JS::ExecutionContext> async_context(heap, [&] { return running_context.copy(); });
  ```
  Note the still-fiddly part: between the lambda returning the populated `NonnullOwnPtr` and the move-assign into `m_held`, there is a brief window. In practice no allocation happens between a function return and an assignment in C++, but if strictness matters, the only way to fully close that window is the refactor approach above.

**Bucket C — different fix entirely.** Sibling `ScopedRoots` doesn't apply because the hazard is structural, not scope-local.

| File | Variable | Fix |
|---|---|---|
| `Libraries/LibWeb/Bindings/MainThreadVM.cpp:368` (host_make_job_callback) | `host_defined` (`WebEngineCustomJobCallbackData`) | Already populated at construction. The proper fix is structural: give `JobCallback::CustomData` a virtual `visit_edges(Cell::Visitor&)` hook, override it in `WebEngineCustomJobCallbackData` to visit `incumbent_settings` and forward to `active_script_context->visit_edges`, then have `JobCallback::visit_edges` call `m_custom_data->visit_edges(visitor)`. The `host_defined` `IGNORE_GC` then comes off because the data is correctly traced through its `Cell` owner. No `ScopedRoots` needed. |

After step 6, all six `IGNORE_GC` + `FIXME` annotations from this branch can be removed: four via `ScopedRoots` (bucket A), one via `copy_to`-style refactor or the `ScopedRoot<T>` follow-up primitive (bucket B), and one via the `JobCallback::CustomData::visit_edges` hook (bucket C).

**Step 7: tests.** Add a `Tests/LibGC/TestScopedRoots.cpp` (or extend `TestGCContainers.cpp`) along the lines of the existing container tests: allocate a cell, put a pointer to it in a stack-held struct that the test drives with `ScopedRoots`, trigger `gather_roots`/`visit_edges` directly, assert the cell appears in the roots map. Use the `gather_roots`-direct pattern described in the "Test Status" section of this doc rather than relying on GC/conservative-scanning to avoid the dangling-reference and conservative-stack issues documented there.

**Out of scope for option 2 (and for this doc):** the ~130-file ExecutionContext → Cell conversion. If that ever happens, `ScopedRoots` is still the right primitive for the general "stack-held non-Cell holder-of-GC-pointers" case, because other non-Cell structs will keep showing up (e.g. `WebEngineCustomJobCallbackData` pre-fix, `NormalizedAlgorithmAndParameter`-shaped structs that currently trip the plugin, and the `Crypto/SubtleCrypto.cpp` cluster of ExceptionOr wrappers around GC-bearing structs).

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
