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
