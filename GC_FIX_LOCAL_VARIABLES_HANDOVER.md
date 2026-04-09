# GC Root Container Local Variables - Handover Document

## Overview

This branch adds a clang plugin check that detects local/static variables using heap-allocating containers (Vector, HashMap, HashTable, OwnPtr) that contain GC-managed pointers but are not registered as GC roots. It also adds missing GC container types needed to fix the violations, and fixes all LibJS and Utilities violations.

## Branch State

```
git log --oneline 094e4bacf8..HEAD
```

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

5. **LibGC: Add ConservativeHashTable for conservatively-scanned hash tables**
   - New `ConservativeHashTable<T>` — for types like `PropertyKey` that store GC pointers in non-standard layouts (tagged pointers).
   - Uses `for_each_possible_value()` callback that iterates only live entries — dead/free buckets are not scanned.
   - Added `OrderedConservativeHashTable<T>` alias.

6. **LibGC: Add ConservativeHashMap for conservatively-scanned hash maps**
   - New `ConservativeHashMap<K, V>` — same live-entry-only scanning approach for HashMap.
   - Also changed `ConservativeHashTable` from `possible_values()` (scanning raw bucket memory including freed buckets) to `for_each_possible_value()` (live entries only). This fixes a bug where stale pointers in deleted buckets could keep GC cells alive indefinitely.
   - Fixed `RootVector` and `RootHashTable` `gather_roots()` to handle const Cell pointer types via `const_cast`.
   - Added `OrderedConservativeHashMap<K, V>` alias.

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

## Known Issues / In Progress

### Test Status

All 26 container tests pass in `Tests/LibGC/TestGCContainers.cpp`, 32 clang plugin tests pass.

Tests call `gather_roots()` / `for_each_possible_value()` / `visit_edges()` directly to verify root/value reporting, avoiding GC/conservative-scanning interference. This was chosen after discovering that:
- **Dangling `bool&` references**: Using `TestCell` with `bool& m_was_finalized` and a shared static heap caused cross-test finalization UB.
- **Conservative stack scanning**: `GC::Weak<T>` approach failed because stale stack pointers kept cells alive, making "should be collected" tests unreliable.
- **Minimum cell size**: Empty `TestCell` hit `VERIFY(cell_size >= sizeof(FreelistEntry))` — see below.

### Possible bug: minimum Cell size not enforced

`HeapBlock::create_with_cell_size` has `VERIFY(cell_size >= sizeof(FreelistEntry))`, which means a `Cell` subclass with no fields will fail to allocate if `sizeof(Cell) < sizeof(FreelistEntry)`. This was hit when `TestCell` had no members. Ideally this would be a compile-time check (e.g., a `static_assert` in `GC_CELL` or `GC_DECLARE_ALLOCATOR`) rather than a runtime `VERIFY` crash. The GC could also enforce the minimum internally (e.g., `max(cell_size, sizeof(FreelistEntry))` in the allocator). Filed as a separate concern from this branch's work.

### Remaining Violations (LibWeb)

The full build shows 8 unique violation patterns in LibWeb (multiplied across TUs due to header-level issues):

| Violation | Recommended fix |
|---|---|
| `Vector<GC::Ref<Animation>>` (Animatable.cpp) | `GC::RootVector` — local, contents already rooted but future-proofing |
| `OwnPtr<JS::ExecutionContext>` (MainThreadVM.cpp x2) | `IGNORE_GC` + FIXME — no GC OwnPtr exists |
| `NonnullOwnPtr<WebEngineCustomJobCallbackData>` (MainThreadVM.cpp) | `IGNORE_GC` + FIXME — wraps OwnPtr<ExecutionContext> |
| `HashMap<JS::PropertyKey, JS::Value>` (MainThreadVM.cpp) | `IGNORE_GC` — return type is `HashMap`, can't change without callback signature change |
| `NonnullOwnPtr<ComputedValues>` (ComputedValues.h) | `IGNORE_GC` + FIXME — transitively contains GC pointers |
| `HashTable<SVGGradientElement const*>` (SVGGradientElement.h) | `GC::RootHashTable` — raw Cell pointer, `this->heap()` available |
| `HashMap<GC::Ptr<JS::Object>, WebAssemblyCache>` (WebAssembly.h) | `IGNORE_GC` + FIXME — global registry, needs restructuring |

Beyond headers, the full ~195 violation count from LibWeb includes:
- `Vector<GC::Ref<DOM::Node>>` (33 instances, mostly in Editing) → `GC::RootVector`
- `OwnPtr<FormattingContext>` (18 in Layout) → `IGNORE_GC` + FIXME or restructuring
- `Vector<Slottable>` (7 in DOM) → `GC::ConservativeVector<Slottable>` (Slottable is a Variant containing GC types)
- `OrderedHashMap<FlyString, GC::Ref<Navigable>>` (4 in HTML) → `GC::OrderedRootHashMap`

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

### Modified infrastructure
- `Libraries/LibGC/Heap.h` / `.cpp` — registration for new container types, scanning loops, debug dump
- `Libraries/LibGC/HeapRoot.h` — new HeapRoot::Type entries
- `Libraries/LibGC/Forward.h` — forward declarations
- `Libraries/LibGC/CMakeLists.txt` — new source files
- `Libraries/LibGC/RootVector.h` — static_assert for non-GC types
- `Libraries/LibGC/RootHashMap.h` — key tracing, IsConvertible guards, static_assert
- `Libraries/LibGC/RootHashTable.h` — IsConvertible guard, static_assert
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
