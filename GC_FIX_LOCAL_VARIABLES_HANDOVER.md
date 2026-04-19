# GC Root Container Local Variables - Handover Document

> **This file is a temporary working doc and should be deleted before
> the branch is opened as a PR.** Its sole purpose is to carry context
> across sessions while the branch is in flight — none of its content
> belongs in master.

## Overview

This branch adds protection against two failure modes for GC-managed pointers held in containers:

1. **Plain heap-backed containers holding GC pointers** (e.g. `Vector<GC::Ref<T>>` as a local variable). Caught by a clang plugin `VisitVarDecl` check that flags `Vector`/`HashMap`/`HashTable`/`OwnPtr` of GC types when they're not registered as roots.
2. **Slicing a GC root container into its non-root base** (e.g. `Vector<T> v = root_vec` or `f(root_vec)` where `f` takes `Vector<T>` by value). Caught at **compile time** by a deleted constrained constructor + `operator=` template in `AK::Vector`, `AK::HashMap`, and `AK::HashTable`.

It also adds missing GC container types needed to fix the violations (`RootHashTable`, `ConservativeHashTable`, `ConservativeHashMap`, `HeapHashMap`), and fixes all LibJS and Utilities violations.

## Branch State

> **All commit SHAs below are stale.** The branch has been rebased onto master
> multiple times since these tables were last written, so the 7-character SHAs
> here will not resolve against the current tree. When you need the actual
> commit, search by the commit subject line (each entry has one) and get the
> new SHA from `git log --oneline` on the branch. Do not spend time mechanically
> updating SHAs in this doc — it is temporary and will be deleted before PR.

```
git log --oneline 094e4bacf8..HEAD
```

### Recent history operations

- **Rebase onto current master**: Branch rebased from `094e4bacf8` onto `236c0b41a5`. Two merge conflicts resolved in `Libraries/LibJS/Bytecode/Interpreter.cpp` (master had renamed `interpreter.get(...)` → `vm.get(...)`) and `Libraries/LibJS/Runtime/VM.cpp` (master refactored `gather_roots_from_execution_context_stack` to use the `for_each_execution_context_top_to_bottom` callback). The earlier post-autosquash hash table in the next sub-section is pre-rebase; live hashes are in the `git log --oneline master..HEAD` output.
- **Autosquash rebase performed**: The `ConservativeHashMap` and `ConservativeHashTable` explicit copy/move/operator= changes were created as `fixup!` commits and then squashed into their respective `Introduce` commits via `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash 094e4bacf8`. This rewrote every commit from `3716ab5e71` (ConservativeHashMap) onwards. The branch is currently divergent from `origin/catch-non-visited-local-varibles`, so a future push will need `--force-with-lease`.

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

### Recent commits since the documented baseline (chronological)

This section tracks work since commit #13 above. Run `git log --oneline 094e4bacf8..HEAD` for the full live list — entries here highlight what shipped, not full per-commit detail.

| Commit | Summary |
|---|---|
| `1963ac15df` | AK+LibGC: Block GC root container slicing at compile time (the deleted constrained ctor; see "Compile-time block" section) |
| `c16bbf5320` | LibWeb/CSS: Use GC-rooted containers for some local variables |
| `e1964d13fc` | LibGC: Add `adopt_*` free functions for root/conservative containers |
| `96364690bd` | LibJS+LibWeb: Use `adopt_*` for moving root container storage into traced members |
| `fe63cdaac7` | LibJSGCPlugin: Restrict `adopt_*` calls to traced member contexts |
| `815298a8ca` | LibJS: Annotate VM singleton and root-gather visitor locals with IGNORE_GC |
| `612c2b96c6` | LibWeb/SVG: Root seen-element hash tables in gradient/pattern recursion |
| `02bdb7476d` | LibJS+LibWeb: Return `GC::RootHashMap` from `host_get_import_meta_properties` |
| `81f3ead348` | LibWeb/Bindings: Annotate MainThreadVM locals and singleton with IGNORE_GC |
| `eb55880066` | **LibJS+LibWeb: Promote `JobCallback::CustomData` to `GC::Cell`** (executes section-4 bucket-C plan; `host_defined` IGNORE_GC removed) |
| `9757bd5b07` | LibJS/Temporal: Store `Utf16FlyString` by value in `CalendarFieldData` |
| `e65d011746` | LibWeb/CSS: Move-adopt RootVector storage in `CSSNumericArray::create` |
| `f39bd44929` | LibWeb: Return `GC::RootVector` from `Animatable::get_animations` and friends |
| `7cfe01c097` | LibWeb/HTML: Root form-related GC containers in the submission flow (`get_submittable_elements`, `Vector<SourcedName>`, `NavigateParams::form_data_entry_list`) |
| `b6aa990731` | LibWeb/WebAssembly: Annotate `s_caches` with IGNORE_GC + tracing rationale |
| `95107490eb` | LibWeb/Animations: Root `AnimationUpdateContext` via inlined `ConservativeHashMap` |
| `a0da2f382b` | Handover: document plugin gap for unreached GC pointers behind indirection (section 7) |
| `e33704e2ef` | LibWeb/CSS: Promote `CountersSet` to `GC::Cell` |
| `cd2763dad2` | LibWeb/CSS: Return `GC::ConservativeVector` from `ComputedProperties::animations` |
| `321f943fdc` | LibWeb/CSS: Promote `ContentData` to `GC::Cell` |
| `17d9765b09` | LibTest+Utilities: Annotate VM singleton holders with IGNORE_GC (post-rebase fallout; 5 new `RefPtr<JS::VM>` / `NonnullRefPtr<JS::VM>` sites outside LibJS that the plugin commit now catches) |
| `41053664b3` | LibGC: Relax container visitors and add a Variant visit helper — `GC::Cell::Visitor`'s template helpers for `Vector`/`Span`/`HashTable`/`Optional` gained `if constexpr (requires { visit(value); })` guards and a new `visit(Variant<Ts...>)` overload. `visitor.visit(container)` now compiles for any element type. See the section 3 follow-up for the silent-no-op risk this introduces. |
| `7391fabfce` | **LibWeb/Crypto: Promote `AlgorithmMethods`/`AlgorithmParams` to `GC::Cell`** — 49 subclasses total (24 methods + 25 params) plus `NormalizedAlgorithmAndParameter`; `normalize_an_algorithm` returns `GC::Ref<NormalizedAlgorithmAndParameter>`, `from_value` / `create` factories return `GC::Ref`. Also flips `AlgorithmIdentifier` to `Variant<GC::Ref<JS::Object>, String>` and adds `visit_edges` on every `*Params` subclass that holds a `HashAlgorithmIdentifier`. Closes the largest remaining violation cluster. |
| `dea519a1d9` | LibJS+LibWeb: Promote `Realm::HostDefined` to `GC::Cell` — includes the `Web::Bindings::HostDefined` and `Web::Bindings::PrincipalHostDefined` subclasses; `Realm::m_host_defined` becomes `GC::Ptr`, `set_host_defined` takes `GC::Ptr`. Removes the `OwnPtr`-forwarding in `Realm::visit_edges`. |
| `3653c2d69d` | LibWeb/CSS: Root transient CSSRule vectors via `GC::RootVector` — `CSSRuleList::set_rules` now takes `GC::RootVector<GC::Ref<CSSRule>>&&` and `adopt_root_vector`s its storage. |
| `4ce9404b16` | LibJS+LibWeb: Use the unified Visitor for Variant-holding members — collapses ~18 hand-rolled `member.visit([&](GC::Ref<T>) {...}, ...)` dispatchers. |
| `54e7098b08` | LibWeb/CSS: Root the last StyleScope and StyleComputer locals — closes CSS at zero flagged variables. |
| `80156f841d` | LibWeb/DOM: Adopt `GC::WeakHashSet` for `Range::live_ranges`, replacing the raw `HashTable<Range*>` the other registries (`all_message_ports`, `all_windows`, etc.) already outgrew. |
| `fb328cea06` | LibWeb/DOM: Root `Node::queue_mutation_record`'s observer map; drops the `GC::DeferGC` workaround that had been parked on top. |
| `73421cf517` | LibWeb/DOM: Root `normalize` and `compare_document_position` ancestors. |
| `f00cfe7e11` | LibWeb/DOM: Root `Range::{extract,clone_the_contents}` contained_children. |
| `c010fe28b2` | LibWeb/Editing: Take `GC::RootVector` in `clear_the_value`, `indent`, and `wrap`. |
| `74bfbf934b` | LibWeb/DOM: Root transient locals across `Document` methods. |
| `9806e92902` | LibWeb/DOM: Root `scrolling_boxes` in `scroll_an_element_into_view`. |
| `fca31f95ca` | LibWeb/DOM: Root touch target list in `EventDispatcher::dispatch`. |
| `01cb088340` | LibWeb/DOM: Avoid copying PaintableFragment list in `Range::get_client_rects`. |
| `45597c04f4` | LibWeb/Bindings: Use Vector storage for GC::Root sequences (generator storage_type change that caused the PointerEvent slicing regression). |
| `2168d5f8cf` | WebContent: Root the dump-all-resolved-styles BFS traversal. |
| `3237c53692` | WebWorker: Annotate realm execution context local with IGNORE_GC. |
| `e0c7b803f7` | LibWeb/Page: Include `LibJS/Runtime/Value.h` directly (avoid relying on a transitive include broken by the SelectItem refactor). |
| `9d3ba87586` | LibWeb/HTML: Drop `SelectItemOption::option_element` (replaced with the 0-based `m_picker_options` side table on `HTMLSelectElement`). |
| `2cc3417a0c` | Tests/LibJS: Annotate TestVM fixture locals with `IGNORE_GC`. |
| `bd0e5a7e6c` | LibWeb/ViewTransition: Root `captureElements` with `GC::RootVector`. |

Session `2026-04-16` batch (post-rebase, continued in the same branch):

| Commit | Summary |
|---|---|
| `f4385c472b` | LibWeb/DOM: Use `GC::Ptr` access in content invalidation check (rebase leftover from the `ContentData` Cell promotion). |
| `3bc59d7b89` | LibWeb/HTML: Include `HTMLOptionElement.h` at users of the type (transitive include broken by the `SelectItem` refactor). |
| `537b2c61b3` | LibWeb/Crypto: Use the `GC::Ref` alternative of `AlgorithmIdentifier` (adapt callers to the `Ref`-flavoured Variant in `CryptoAlgorithms.h`). |
| `93052b289d` | LibWeb/Bindings: Emit `GC::Ref<JS::Object>` for the IDL `object` type (union expansion now matches the hand-written `AlgorithmIdentifier`). |
| `45f13da46f` | LibWeb/Bindings: Pass sequence parameters by value (generator-side `move()` + `CSSTransformValue::construct_impl` / `Clipboard::write` destinations updated to `Vector<GC::Root<T>>`). |
| `8d72bd6114` | LibWeb/Geometry: Root the `DOMRectList::create` transient vector via `GC::RootVector`. |
| `5420044d12` | LibWeb/HTML: Root the `disposedNHEs` local in `Navigation`. |
| `2ee226b77d` | LibWeb/HTML: Return `pdf_viewer_{plugin,mime_type}_objects` by reference. |
| `c76b246de4` | LibWeb/DOM: Root transient `Slottable` lists via `GC::ConservativeVector` (`find_slottables`, `find_flattened_slottables`, `Slot::set_assigned_nodes`, `HTMLSlotElement::assign`). |
| `b02e07eeed` | LibWeb/HTML: Adopt `GC::WeakHashSet` for the BCG group set. |
| `5afc4f7336` | LibWeb/DOM: Port `Text::split_text` to the `WeakHashSet` iterator shape (missed in `b8ba96d602`). |
| `0df12535b4` | LibWeb/DOM: Root the `whole_text` contiguous-nodes list. |
| `e5b65137c9` | LibWeb/DOM: Store scrolling boxes as `GC::Ref<Node>` in scroll-into-view. |
| `0c1f4f928e` | LibGC: Add `WeakHashMap` (cell-typed key/value slots stored as `Weak<T>`). |
| `26b50eb587` | LibWeb/DOM: Adopt `GC::WeakHashMap` for the node directory. |
| `d133f5fee5` / `d0d84047d4` / `acc12db37b` / `b3af725b16` | Handover: note `WeakHashMap` iterator / prune-on-read / JS `WeakMap` semantic difference / commit-restructure before PR. |
| `fc0cd418c8` | LibWeb/HTML: Root the `popover_positions` map in `topmost_popover_ancestor`. |
| `fc00c0bc1d` | LibWeb/HTML: Root the document index map in `EventLoop` sort. |
| `9d52d0adf4` | LibWeb/HTML: Adopt `GC::WeakHashSet` for `NavigableContainer::all_instances` (Document.cpp iteration syntax adapted in the same commit). |
| `8d0d2f3904` | LibWeb/HTML: Include `StructuredSerializeOptions.h` in `DedicatedWorkerGlobalScope`. |
| `b8e02ce0da` | LibWeb/HTML: Root the Window named-property-set collections (`OrderedRootHashMap` return type + `NamedObjects(Heap&)` constructor; forces `Navigable.h` into `Window.h`). |
| `dcf91b16af` | LibWeb/HTML: Bind `pdf_viewer_mime_type_objects` result by reference in `Plugin`. |
| `bd9c1f817b` | LibWeb/XPath: Root the `XPathResult` node-set transient via `RootVector`. |
| `31323fa7fc` | LibWeb/IndexedDB: Root transient transaction-list locals (`block_on_conflicting_transactions` + `IDBTransaction::create` scope). |
| `63d5969c6a` | LibWeb/HTML: Root `StructuredSerialize` Map/Set `copied_list` transients. |
| `d4c749f5a9` | LibWeb/WebGL: Root the `getActiveUniforms` `params_as_values` local. |
| `76504adfca` | LibWeb/IndexedDB: Root seen list in `convert_a_value_to_a_key` (split into two overloads; the 3-arg impl takes `GC::RootVector<JS::Value>&`). |
| `bff32c105b` | LibWeb/HTML: Adopt `GC::WeakHashSet` for the top-level traversable set (silently drops insertion order — see Deferred). |
| `371c936fc5` | LibWeb/HTML: Root performance-observer notify list + entry copies (`OrderedRootHashTable` populated by loop; `PerformanceObserverEntryList` constructor takes `RootVector&&` + adopts). |

Session `2026-04-17` batch (continues on the same branch):

| Commit | Summary |
|---|---|
| `4e225d3a80` | LibWeb/XHR: Adopt `GC::ConservativeVector` in `FormData` construction (fixes the `call to deleted constructor` compile error from the slicing block). |
| `87038481a7` | LibWeb/Painting: Root `cell_boxes` list and `cell_coordinates_to_box` map in `paint_table_borders` (`GC::RootVector<GC::Ref<PaintableBox const>>` + `GC::RootHashMap<CellCoordinates, GC::Ptr<PaintableBox const>>`). |
| `0990cdd88a` | LibWeb/SVG: Take `GC::RootVector<T>&&` in `SVGList` / `SVGNumberList` / `SVGLengthList` / `SVGTransformList` constructors and adopt in `SVGList`'s own member init list (closes the deferred base-class adopt pattern from the previous session). |
| `d82803839b` | LibWeb/Layout: Root the `absolute_boxes` list in `InlineFormattingContext::run`. |
| `854e7f77ac` | LibWeb/Layout: Root the throwaway cells/rows locals in the 2-arg `TableGrid::calculate_row_column_grid` overload. |
| `0f889f38c0` | LibWeb/Layout: Return `GC::ConservativeVector<ConflictingEdge>` from `BorderConflictFinder::conflicting_edges`. |
| `6f85b10fff` | LibWeb/Layout: Root `seen_content_elements` in `TreeBuilder`'s SVG-pattern recursion guard. |
| `6b30e81b9e` | LibWeb/Layout: Root paint-tree rebuild inline/text/paintable sets in `LayoutState::commit` (three `HashTable<T*>` → `GC::RootHashTable<GC::Ptr<T>>`). |

Session `2026-04-18` batch (Layout cluster Cell-promotion):

| Commit | Summary |
|---|---|
| `dd7cb757ad` | LibWeb/Painting: Include `HTMLElement.h` in `PaintableWithLines.cpp` (pre-existing latent missing include surfaced by header reshuffling). |
| `e43f3e5fb0` | LibWeb/SVG: Include `EventNames.h` in `SVGImageElement.cpp` (same category). |
| `aab894d8b7` | LibWeb/HTML: Move form data entry list when creating `FormData` (pre-existing lvalue→rvalue mismatch; `construct_impl` wants `&&`). |
| `2e70fce45f` | LibWeb/Layout: Promote `LayoutState` to a `GC::Cell`. `FormattingContext::m_state` becomes `GC::Ref<LayoutState>`; `UsedValues` / `PagedStore` / `LineBox` / `LineBoxFragment` each own their own `visit_edges` (encapsulation pattern). |
| `51ff7b92e1` | LibWeb/Layout: Promote `FormattingContext` hierarchy to `GC::Cell` — base + 7 header subclasses + the two anon-namespace `InternalReplaced` / `InternalDummy` shims. `OwnPtr<FormattingContext>` / `NonnullOwnPtr<FormattingContext>` → `GC::Ptr<FormattingContext>` / `GC::Ref<FormattingContext>`. `FlexItem` / `GridItem` / `TableGrid::Cell` / `TableGrid::Row` / `BFC::FloatingBox` each got their own `visit_edges`. BFC's destructor moved to `finalize()` + `OVERRIDES_FINALIZE = true`. |
| `2ae1cca093` | LibWeb/CSS: Promote `ComputedValues` to a `GC::Cell`. `MutableComputedValues` / `ImmutableComputedValues` subclasses get their own allocators; `clone_inherited_values()` allocates via `heap()` (no arg — inherits from the Cell base). `Layout::NodeWithStyle::m_computed_values` becomes `GC::Ref<CSS::ComputedValues>`; constructors across ~6 Layout subclasses updated from `NonnullOwnPtr<ComputedValues>` to `GC::Ref<ComputedValues>`. Created `ComputedValues.cpp` (was header-only) for the three `GC_DEFINE_ALLOCATOR` lines + out-of-line `visit_edges` and `clone_inherited_values`. |
| `8ad7f8e326` | LibWeb/Layout: Bind BFC root state by reference in multi-column check (was copying `UsedValues` into a local; reference closes a one-off violation). |
| `3f87464baa` | LibWeb/Layout: Promote `BorderConflictFinder` to a `GC::Cell`. Name-collision fix: `Cell` inside the nested class now refers to `GC::Cell` via inheritance, so signatures explicitly spell `TableGrid::Cell`. `RowGroupInfo` gets its own `visit_edges`. |
| `cca6c6fca2` | LibWeb/Layout: Promote `InlineLevelIterator` to a `GC::Cell`. `InlineLevelIterator::Item` gets its own `visit_edges`. `InlineFormattingContext::run` heap-allocates the iterator via `heap().allocate<InlineLevelIterator>(...)`. |

Session `2026-04-19` batch (post-rebase conflict cleanup + TreeBuilder/Agent + IndexedDB Cell-promotion + three fixup groups):

Note: hashes below are from before the session-closing autosquash rebase that folded three `--fixup=` commits into their origin commits. After that rebase, every commit from `790b401929` onwards got a new hash. Use `git log --grep=` / the subject to locate a commit on the live branch.

| Commit | Summary |
|---|---|
| `30626e5bdc` (was `ad719c4d0d`) | LibWeb/Layout: Promote `TreeBuilder` to a `GC::Cell`. One-site promotion at `Document.cpp:1537`; `visit_edges` traces `m_layout_root` and `m_ancestor_stack`. |
| `cd01770e48` (was `888cddb486`) | LibWeb/HTML: Store the structured-transfer list in a `GC::RootVector`. Aligned `StructuredSerializeOptions::transfer` and every `post_message` / `structured_serialize_with_transfer` signature on `GC::RootVector<GC::Ref<JS::Object>>`, and taught the dictionary generator to emit `{ vm.heap() }` init when any member's sequence-storage is `RootVector`. **Fixup folded in:** `Streams/AbstractOperations.cpp` `{ .transfer = {} }` → `{ realm.heap() }`. |
| `8352b0820d` (was `b34fb4de9a`) | LibJS+LibWeb: Promote `Agent` to a `GC::Cell`. `JS::Agent` + `Web::HTML::Agent` + `SimilarOriginWindowAgent` + `WorkerAgent`. `VM::m_agent` switches to `GC::Ptr<Agent>` rooted via `gather_roots`. The flagged `HashMap` key (`GC::Ref<JS::FunctionObject>`) is now traced through the owner's `visit_edges`; value simplified from `GC::Root` to `GC::Ref`. **Fixup folded in:** generator dropped the stale `registry_for_constructor->is_null()` check. |
| `264ce33012` | LibWeb/IndexedDB: Root MutationLog record-deletion transients. `RecordsDeleted` / `IndexRecordsDeleted` variant alternatives hold `GC::ConservativeVector<T>`; the four `Index` / `ObjectStore` call sites construct with `heap()` from the start. |
| Fixup into `790b401929` (object → `GC::Ref`) | `WebAssembly::instantiate` / `instantiate_streaming` / `Instance::construct_impl` take `GC::Ptr<JS::Object>` directly, matching the generator's nullable-`object?` emission. |
| `b63fa3fd7c` | LibJS: Add `RootedExecutionContext` for rooting transient locals. New type + VM `m_rooted_execution_contexts` list walked in `gather_roots` + plugin allowlist entries in both `type_contains_gc_ptr` and `type_has_unrooted_gc_container` + `Tests/LibJS/test-rooted-execution-context.cpp`. |
| `0d05ebe7a3` | LibWeb: Root realm execution context across Window setup. `WindowEnvironmentSettingsObject::setup` + ctor take `JS::RootedExecutionContext&&`; release lands in `EnvironmentSettingsObject`'s base member-init. Document/BrowsingContext construct the wrapper inline around `create_a_new_javascript_realm`. Closes 9 → 7 violations. |
| `5338e75b36` | LibJS+LibWeb: Drop four `IGNORE_GC` ExecutionContext sites — `Realm::initialize_host_defined_realm`, `ECMAScriptFunctionObject` async_context copy, `MainThreadVM` dummy + script_execution_context. `WebEngineCustomJobCallbackData`'s ctor now takes `Optional<JS::RootedExecutionContext>&&` so the last one's release lands in the Cell's own member-init. |

### Plugin behavior cheat-sheet (documented this session, from source read)

The clang plugin at `Meta/Lagom/ClangPlugins/LibJSGCPluginAction.cpp`:
- Short-circuits on `ParmVarDecl` — **function parameters are never checked**. `Foo const&` / `Foo*` parameters where `Foo` is a Cell pass silently.
- Short-circuits on reference-typed **local** variables. `auto& x = get_cell()` is fine.
- **Flags** owning containers (`NonnullOwnPtr<Cell>`, `Vector<Cell*>`, `HashMap<K, Cell*>`, etc.) when used as locals/globals.
- **Flags** member fields of type raw `Cell&` / `Cell*` on `GC::Cell` subclasses — these must be wrapped in `GC::Ref` / `GC::Ptr` (or explicitly opted out via `GC::RawRef` / `GC::RawPtr`).

Implication: when promoting a type to `GC::Cell`, most function signatures that take the type by reference can stay unchanged. Only owning-by-value (`NonnullOwnPtr<T>` / `Vector<T>` locals) and member fields need to change to `GC::Ref` / `GC::Ptr`. Applied throughout the Layout cluster promotion to minimise cascade.

### Encapsulation pattern for non-Cell structs that hold GC pointers

When a struct is a member of a `GC::Cell` but not a Cell itself (e.g. `FlexItem`, `GridItem`, `TableGrid::Cell`, `BFC::FloatingBox`, `LayoutState::UsedValues`, `LineBox`, `LineBoxFragment`, `InlineLevelIterator::Item`, `BorderConflictFinder::RowGroupInfo`), the struct gets its own non-virtual `visit_edges(GC::Cell::Visitor&)` method. The owning Cell's `visit_edges` iterates and calls `entry.visit_edges(visitor)` rather than reaching into the struct's members directly. This keeps the visit list next to the data definition so future members can't silently escape tracing.

For container types holding these structs, the pattern extends one more layer: `PagedStore<T>::visit_edges` calls `entry.visit_edges(visitor)` so the owning cell just does `m_used_values_store.visit_edges(visitor)`.

Section-4 bucket A is now closed: `JS::RootedExecutionContext` landed this session (commits `b63fa3fd7c` / `0d05ebe7a3` / `5338e75b36`), replacing the five transient-local `IGNORE_GC` sites (`Realm`, `ECMAScriptFunctionObject`, `MainThreadVM` × 2) and the two Document/BrowsingContext flagged locals. Bucket-C `CustomData` cell-promotion landed previously. The only remaining §4 annotation is `ExecutionContext.cpp:108`'s in-body `IGNORE_GC`, left as-is because making `copy()` private would require migrating six external callers. Plugin enforcement reorder (task #12) is still pending.

## Session summaries

One paragraph per working session, newest first. The per-commit table above is the raw log; these summaries are the "what shifted" narrative and exist so a future session can pick up without replaying every commit. Each entry should close with the violation count at end of session so the trajectory is legible.

### 2026-04-19 — Post-rebase conflict cleanup + `TreeBuilder` / `Agent` Cell-promotion

Resumed on a mid-rebase branch (54/124 commits in). Two merge
conflicts resolved: `DOM/Range.cpp` kept the updated camelCase spec
comment while preserving the branch's `GC::RootVector` fix, and
`BindingsGenerator/IDLGenerators.cpp` picked up master's refactor of
inline scalar/union/buffer-source conversions into helper functions
(`generate_object_to_cpp`, `generate_buffer_source_to_cpp`) while
re-applying the branch's `GC::Ref<JS::Object>` emission for the IDL
`object` scalar and the `includes_object` union branches.

The `StructuredSerializeOptions::transfer` field was the hidden knock-on
from the `object`-as-`GC::Ref` switch: sequences of `object` now emit
`GC::RootVector<GC::Ref<JS::Object>>` locals, but the hand-written dict
field (and the cascading `post_message` / `structured_serialize_with
_transfer` signatures) was still `Vector<GC::Root<JS::Object>>`. Aligned
the full chain on `GC::RootVector<GC::Ref<JS::Object>>` (dict field gets
a `Heap&` constructor; `Window`, `MessagePort`, `Worker`,
`DedicatedWorkerGlobalScope`, and the `Stream` transfer-out sites
follow). Taught the dictionary generator to detect members whose
sequence-storage is `RootVector` and emit `{ vm.heap() }` init for
those dicts only — others keep `{}` aggregate init.

`Layout::TreeBuilder` Cell-promoted (one-site local at
`Document.cpp:1537`; `visit_edges` traces `m_layout_root` and
`m_ancestor_stack`).

`JS::Agent` and the `Web::HTML::Agent` intermediate Cell-promoted,
taking `SimilarOriginWindowAgent` and `WorkerAgent` with them.
`VM::m_agent` switched from `OwnPtr<Agent>` to `GC::Ptr<Agent>` rooted
via `gather_roots` (matching the existing pattern for other cached
cells). `create()` factories now return `GC::Ref<...>` via
`heap.allocate<...>()`. The flagged `HashMap<GC::Ref<JS::FunctionObject>,
GC::Root<CustomElementRegistry>>` key became traced through the
owner's `visit_edges` instead (value dropped from `GC::Root` to
`GC::Ref` — simpler now that the owner is a Cell). Intermediate
`Web::HTML::Agent::visit_edges` is `protected` so the concrete
subclasses can chain via `Base::visit_edges`; the concrete subclasses
keep `visit_edges` private. Follow-up call-site adjustments in
`Element.cpp` / `ElementFactory.cpp` dereference `GC::Ptr<Registry>`
into the map's `GC::Ref` value.

**Closing violation count: 13 → 11**. Remaining 11 are all in the
originally-deferred buckets: `NonnullOwnPtr<JS::ExecutionContext>` (2),
MutationLog transients (2), nested-container indirection (5), and
`Vector<Variant<cell,...>>` returns (2).

Continuation of the same date: closed the IndexedDB MutationLog
transients. `RecordsDeleted` and `IndexRecordsDeleted` variant
alternatives now hold `GC::ConservativeVector<T>` directly; the
four call sites in `Index::{clear_records, remove_records_with_value_in_range}`
and `ObjectStore::{remove_records_in_range, clear_records}` construct a
`GC::ConservativeVector` with `heap()` from the start. This is the
clean-fix shape the previous deferred note described — the variant
field carries the heap registration itself via move-construction, no
`adopt_conservative_vector` needed.

A full rebuild that exercised previously-cached generated files
surfaced three latent build breakages from earlier branch commits,
each folded back into its origin via `--fixup` + autosquash:

- `--fixup=b34fb4de9a` (Agent → Cell): the HTML-constructor generator
  template in `IDLGenerators.cpp` still called
  `registry_for_constructor->is_null()` on the map value, but
  promoting `SimilarOriginWindowAgent` changed the map from
  `HashMap<..., GC::Root<Registry>>` to
  `HashMap<..., GC::Ref<Registry>>` — `GC::Ref` has no `is_null()`.
  Dropped the check (existence in the map now guarantees non-null).
- `--fixup=888cddb486` (StructuredSerializeOptions → `GC::RootVector`):
  `Streams/AbstractOperations.cpp:129` used designated-init
  `{ .transfer = {} }`, which stopped compiling once the struct got a
  non-trivial constructor. Rewrote as `{ realm.heap() }`.
- `--fixup=790b401929` (bindings `object` → `GC::Ref<JS::Object>`):
  nullable `object?` started emitting `GC::Ptr<JS::Object>` at that
  commit, but the WebAssembly C++ signatures still took
  `Optional<GC::Root<JS::Object>>&`. The mismatch was masked by ccache.
  Updated `WebAssembly::instantiate` (two overloads),
  `WebAssembly::instantiate_streaming`, and
  `Instance::construct_impl` to take `GC::Ptr<JS::Object>` directly —
  the old conversion block inside each callee becomes a no-op.

**Updated closing violation count: 11 → 9**. Remaining 9: ExecutionContext
(2), nested-container indirection (5), `Vector<Variant<cell,...>>` returns
(2). All in deferred buckets that need either the `JS::RootedExecutionContext`
wrapper, the plugin's nested-container extension (section 7), or the
bindings-generator extension for Variant returns.

Continuation (same date): implemented the `JS::RootedExecutionContext`
wrapper end-to-end per the §4 plan. Three atomic commits:

- `JS::RootedExecutionContext` type + VM integration + plugin allowlist +
  test: new files `Libraries/LibJS/Runtime/RootedExecutionContext.{h,cpp}`,
  a `RootedExecutionContext::List m_rooted_execution_contexts` member on
  `VM` walked inside the existing `gather_roots` next to the
  `m_execution_context_stack` walk (same `ExecutionContextRootsCollector` +
  `HeapRoot::Type::VM` plumbing), and a `Tests/LibJS/test-rooted-execution
  -context.cpp` that exercises `gather_roots` directly. The plugin needed
  two allowlist entries — one in `type_contains_gc_ptr`'s
  `gc_infrastructure_types` (for member-field walks) and one in
  `type_has_unrooted_gc_container`'s template + record paths (the codepath
  `VisitVarDecl` actually uses). `GC::` types already go through the
  record branch's `starts_with("GC::")` exemption; `JS::RootedExecutionContext`
  needed an explicit entry alongside.
- `WindowEnvironmentSettingsObject::setup` + ctor signatures now take
  `JS::RootedExecutionContext&&`, with `release()` landing in the base
  `EnvironmentSettingsObject`'s member-init (atomic against GC because
  the ctor body runs inside `Heap::allocate`'s `defer_gc` bracket). Both
  call sites (`Document.cpp:334`, `BrowsingContext.cpp:173`) construct
  the wrapper locally and `move(...)` into `setup`. `EnvironmentSettingsObject`'s
  base ctor stays as `NonnullOwnPtr<ExecutionContext>` so the `Worker`
  path stays untouched. **Closes 9 → 7 violations.**
- Dropped four `IGNORE_GC` annotations via wrapper adoption: `Realm.cpp:43`
  (`new_context`), `ECMAScriptFunctionObject.cpp:428` (`async_context` —
  copy-ctor variant), `MainThreadVM.cpp:293` (`dummy_execution_context` —
  `Optional<RootedExecutionContext>`), and `MainThreadVM.cpp:353`
  (`script_execution_context`). For the last site, extended
  `WebEngineCustomJobCallbackData`'s ctor to take `Optional<RootedExecutionContext>&&`
  so the release-and-store happens inside its ctor (same defer_gc
  argument) — cleaner than wrapping the single caller in an explicit
  `DeferGC` block.

**Post-wrapper closing violation count: 9 → 7.** Remaining 7:
nested-container indirection (5), `Vector<Variant<cell,...>>` returns (2).
Both still blocked on infrastructure (section-7 plugin extension,
bindings generator extension).

API shape notes, in case a future session touches this:
- `RootedExecutionContext::release()` is **not** rvalue-ref qualified —
  matches AK convention (`OwnPtr::release_nonnull`, `NonnullOwnPtr::leak_ptr`)
  and keeps call sites clean (`rc.release()` / `rc->release()` rather
  than `move(rc).release()`). Misuse is caught by `VERIFY` on the
  wrapper's accessors after release.
- The wrapper is non-copyable and non-movable (the intrusive list node
  pins the address), so handoff across function boundaries goes by
  rvalue-ref parameter — never by value and never via a move ctor.
- `AK::Optional<RootedExecutionContext>` works because `Optional::emplace`
  uses `construct_at` with perfect forwarding and doesn't require the
  payload to be movable.
- `ExecutionContext::copy()` stays public and keeps its in-body
  `IGNORE_GC auto copy = create(...)` annotation. Making it private
  would require migrating six external callers (`IteratorHelper`,
  `AsyncFunctionDriverWrapper`, `ECMAScriptFunctionObject` generator
  path, `NativeJavaScriptBackedFunction`, etc.) — scope creep we
  deliberately skipped.

### 2026-04-18 — Layout cluster Cell-promotion (LayoutState + FormattingContext + ComputedValues)

Closed the big deferred Layout cluster via three back-to-back Cell
promotions plus per-struct cleanups, in the order the plan dictated
(`LayoutState` first because `FormattingContext::m_state` needed to
become `GC::Ref<LayoutState>`; then the `FormattingContext` hierarchy;
then `ComputedValues`).

`LayoutState` became a `GC::Cell` with `visit_edges` that iterates its
`PagedStore<UsedValues>` and delegates per-entry. Rather than reaching
into `UsedValues` directly, `UsedValues`/`PagedStore`/`LineBox`/
`LineBoxFragment` each got their own `visit_edges` — this is the
encapsulation pattern we settled on this session for non-Cell structs
that hold GC pointers, so future edits to the struct's GC members can't
silently escape tracing. The initial commit missed that `UsedValues::
line_boxes` contained `LineBoxFragment::m_layout_node` (a `GC::Ref<Node
const>`) and would have left those refs silently unrooted — that fix
was squashed into the LayoutState commit so the tree builds cleanly at
every point in the series.

`FormattingContext` and all nine subclasses (7 header + 2
anon-namespace shims) became Cells. Per-struct `visit_edges` on
`FlexItem`, `GridItem`, `TableGrid::Cell`, `TableGrid::Row`, and
`BFC::FloatingBox`. `OwnPtr<FormattingContext>` / `NonnullOwnPtr` →
`GC::Ptr` / `GC::Ref` throughout. BFC's destructor (which calls a
pseudo-virtual as a late-hook) moved to `finalize()` with
`OVERRIDES_FINALIZE = true` per the plugin's Cell-destructor rule.
During the same commit, `FormattingContext::m_state` switched to
`GC::Ref<LayoutState>` — the user pointed out this made the ownership
explicit and matched how `m_context_box` was already held, which
prompted renaming 163 `m_state.` accesses across 8 files to
`m_state->`. `LayoutState&` function parameters became
`GC::Ref<LayoutState>`.

`ComputedValues` became a Cell, along with `MutableComputedValues` and
`ImmutableComputedValues` (each needs its own `GC_DEFINE_ALLOCATOR`).
Created `Libraries/LibWeb/CSS/ComputedValues.cpp` (was header-only) for
the three allocator definitions + out-of-line `visit_edges` and
`clone_inherited_values`. The `IGNORE_GC` / FIXME on
`clone_inherited_values` dropped — it returns `GC::Ref<ComputedValues>`
now and allocates via `heap()` (no arg; inherited from the Cell base).
All four call sites updated (`TreeBuilder.cpp:1164,1281,1312` +
`Node.cpp:1121`); the `static_cast<MutableComputedValues&>(*clone)
.set_foo()` mutation idiom continues to work after promotion because
the subclasses are still `final : public ComputedValues`. The
refactor stayed minimal-cascade thanks to the plugin-behaviour
cheat-sheet (see above): accessor returns (`computed_values() const&`,
`mutable_computed_values() &`) stay unchanged because the plugin
exempts references; only owning sites (the `m_computed_values` member
and the handful of constructors taking `NonnullOwnPtr<ComputedValues>`)
needed to change.

Three per-struct cleanups landed after the big promotions:
`BlockFormattingContext.cpp:118` switched a `UsedValues` value-copy to
a `const&` (trivial one-liner); `BorderConflictFinder` became a Cell
with `RowGroupInfo` owning its own `visit_edges`; `InlineLevelIterator`
became a Cell with `InlineLevelIterator::Item` owning its own
`visit_edges`. The `BorderConflictFinder` promotion hit a name-
collision wrinkle: the nested class's `public GC::Cell` inheritance
shadows the enclosing `using Cell = TableGrid::Cell;` alias inside the
nested-class scope, so signatures on BorderConflictFinder methods had
to explicitly spell `TableGrid::Cell` (worth remembering for future
cell-promotion of any nested type that uses a similarly-named alias).

Three pre-existing build breakages showed up under the new header
include graph and were committed as separate small commits before the
Cell-promotion batch: `SVGImageElement.cpp` needed `EventNames.h`,
`PaintableWithLines.cpp` needed `HTMLElement.h`, and `Navigation.cpp`
needed a `move()` around `release_value()` since `FormData::
construct_impl` takes an rvalue (the Navigation call site was missed
when `construct_impl`'s signature changed on this branch).

**Closing violation count: 19 → 13** (session started at 19 after
Steps 1–2, closed 3 `ComputedValues` + 3 per-struct = 6). Remaining 13
are all still in the deferred buckets:
`NonnullOwnPtr<JS::ExecutionContext>` (3), IndexedDB MutationLog
transients (2), nested-container indirection (5: Flex/Grid nested
`HashMap<int, Vector<T>>`, `ContainedBoxesMap`, Viewport
TextPosition/TextBlock), `Vector<Variant<cell,...>>` returns (2:
WebIDL OverloadResolution, XHR FormData), and `Document.cpp:1537`
`Layout::TreeBuilder` local (a small Cell promotion, deferrable).

### 2026-04-17 — SVG base-class adopt + Painting/Layout one-offs

Picked off the tractable loose ends that didn't need the bigger Layout /
`RootedExecutionContext` / Cell-promotion refactors.

Fixed the `FormData` `call to deleted constructor` site: switched the
`construct_impl` / `create` / ctor signatures to take
`GC::ConservativeVector<FormDataEntry>&&` and adopted in the member init
list. Implemented the **SVGList base-class adopt pattern** that the
previous session had filed as deferred — `SVGList<T>::SVGList` now
takes `GC::RootVector<T>&&` and adopts in its own member init, so the
three `SVGNumberList`/`SVGLengthList`/`SVGTransformList` subclass ctors
just forward the rvalue reference without invoking adopt from a
derived-class context (which the plugin rejects).

Rooted 8 more transient locals: `absolute_boxes` in inline layout,
TableGrid's throwaway cells/rows locals, `BorderConflictFinder`'s result
vector (return type flipped to `GC::ConservativeVector`), TreeBuilder's
SVG-pattern recursion guard, LayoutState's paint-tree rebuild
inline/text/paintable sets, and `TableBordersPainting`'s
`Vector<PaintableBox const&>` + `HashMap<CellCoordinates, PaintableBox
const*>` (the latter switched to `GC::RootVector<GC::Ref<...>>` and
`GC::RootHashMap<CellCoordinates, GC::Ptr<...>>`; the inner loops had
to move from `cell_box.method()` to `cell_box->method()` since the
iterator now yields `GC::Ref<PaintableBox const>`).

Deferred items unchanged: `ContainedBoxesMap` (nested-container
indirection — needs section-7 plugin extension), `Vector<TextBlock>` in
`Viewport::update_text_blocks` (struct-nested `Vector<TextPosition>` would
need TextBlock redefinition to adopt), FlexFormattingContext and
GridFormattingContext `HashMap<int, Vector<T>>` (same nested-container
issue).

**Closing violation count: 62 → 50** (all remaining are Layout cluster,
`ExecutionContext`, `SimilarOriginWindowAgent`, `MutationLog` transients,
and the `Vector<Variant<cell,...>>` return-type category — every one
still blocked on a wider refactor).

### 2026-04-16 — Post-rebase DOM/HTML/IndexedDB batch + `GC::WeakHashMap`

Resolved post-rebase fallout (three files): `Element.cpp` content-invalidation access, transitive-include repairs broken by the earlier `SelectItem` refactor, and a generator/`AlgorithmIdentifier` mismatch.

Rooted ~19 transient locals across `LibWeb/DOM`, `LibWeb/HTML`, `LibWeb/Geometry`, `LibWeb/XPath`, `LibWeb/IndexedDB`, `LibWeb/WebGL`, and `LibWeb/PerformanceTimeline`. Switched three global cell registries (`BrowsingContextGroup` BCG set, `NavigableContainer::all_instances`, `TraversableNavigable` top-level set) from raw `HashTable<T*>` / `OrderedHashTable<GC::Ref<T>>` to `GC::WeakHashSet<T>`, matching the existing `MessagePort` / `Window` / `Range` pattern. Returned two `pdf_viewer_*` lists by `const&` instead of by value so callers stop copying traced vectors into unrooted locals.

Built and shipped **`GC::WeakHashMap<K, V>`** alongside four unit tests in `TestGCContainers.cpp`. It handles the three cell/non-cell mixes (non-cell key + cell value, cell key + non-cell value, both cell) and verifies entry eviction on `collect_garbage`. Applied it to `Node::s_node_directory`, eliminating an `IGNORE_GC` workaround.

Completed the (b)+(c) generator cleanup from the previous session: IDL `object` now emits `GC::Ref<JS::Object>`, sequence arguments move into by-value destinations, and `CSSTransformValue` / `Clipboard` destinations accept `Vector<GC::Root<T>>` directly.

Added to the handover: the commit-structure-rewrite-before-PR TODO, the "handover doc is temporary and should be deleted before PR" warning at the top, the `WeakHashMap` vs JS `WeakMap` semantic difference (not an ephemeron map), the prune-on-read trade-off, the `user_agent_top_level_traversable_set` losing its `Ordered` marker, and five new deferred items (SVGList base-class adopt, IndexedDB `MutationLog` transients, `SimilarOriginWindowAgent` ownership, generator-facing `Vector<Variant<cell,...>>` returns, missing `WeakHashMap` iterator).

**Closing violation count: 80 → 61** (Layout cluster now the only large remainder; everything else is in deferred buckets).

## Outstanding Tasks

Live task list (mirrored from the in-session TaskList tool, lowest ID first):

| ID | Status | Subject |
|---|---|---|
| #4 | completed | Collect LibWeb violations from build |
| #5 | completed | Fix LibWeb/Editing violations |
| #6 | pending | Fix LibWeb/Layout violations (47 — biggest remaining cluster; mostly `OwnPtr<FormattingContext>` + `LayoutState`) |
| #7 | pending | Fix LibWeb/HTML violations (32 — Window/NamedObjects, Plugin+MimeType, Select, Scripting, StructuredSerialize) |
| #8 | pending | Fix LibWeb/DOM violations (12 — Slottable, Document, Node, Text) |
| #9 | completed | Fix LibWeb/SVG violations (done — `SVGList`-family ctors now take `GC::RootVector&&` and adopt in `SVGList` member init) |
| #10 | completed | Fix LibWeb/CSS violations (done — StyleScope and StyleComputer closed out in the in-flight changes) |
| #11 | in_progress | Fix remaining LibWeb violations (Crypto/Painting/XHR-slicing/Geometry done; MutationLog=2, WebIDL/XHR-Variant returns=2 remain as deferred categories) |
| #12 | pending | Reorder commits to put plugin enforcement at the end of the branch |
| #13 | completed | Add compile-time block for GC container downgrades |
| #14 | completed | Polish adopt pattern: doc comments, runtime tests, plugin cleanup |
| #15 | completed | Promote JobCallback::CustomData to GC::Cell |

Roughly in priority order, the next things to land are:

1. **Close the remaining 7 violations** — all deferred-bucket: nested-container indirection (5 sites: Flex/Grid `HashMap<int, Vector<T>>`, `ContainedBoxesMap`, Viewport `Vector<TextPosition>`/`Vector<TextBlock>`) and generator-facing `Vector<Variant<cell,…>>` returns (2 sites: WebIDL `OverloadResolution`, XHR `FormData`). Nested-container needs a section-7 plugin extension or per-struct redefinitions; Variant returns need a bindings-generator extension or caller reshape.
2. **Task #12 (commit reordering)** — once everything builds, rebase to put the plugin-enforcement commit at the end of the branch so the history reads "fix all issues, then enforce".

Landed this branch (status note):
- `JS::RootedExecutionContext` — added + adopted across Document/BrowsingContext/Realm/ECMAScriptFunctionObject/MainThreadVM (see 2026-04-19 session). Section-4 bucket A is closed.
- `JobCallback::CustomData` Cell-promotion — landed (bucket C).
- `LibWeb/CSS/Parser/Helpers.cpp` `execution_context` process-static — intentionally kept as `IGNORE_GC` (one-time `HostDefined` factory helper, outlives the parser; refactoring it would need its own tiny Cell-promotion pass).

### Deferred (not in the task list, tracked here for the next session)

- **`Vector<Variant<...>>` returns with a cell inside the Variant**: two concrete sites — `WebIDL::resolve_overload` returning `ResolvedOverload::arguments` (`Vector<Variant<JS::Value, Missing>>`) and `XHR::FormData::get_all` returning `Vector<FormDataEntryValue>` (`Vector<Variant<GC::Ref<FileAPI::File>, String>>`). The natural fix is to make both returns a `GC::ConservativeVector`, but the callers are generated bindings that pass the result to the IDL-to-JS conversion layer — that layer currently expects a plain `Vector`. Unblocking it cleanly requires either (a) extending the bindings generator so a `GC::ConservativeVector<...>` return is accepted the same way a `Vector<...>` is, or (b) reshaping each caller to take a `Heap&` out-parameter. Leaving both with the plugin violation for now; the wider refactor belongs with the other generator-facing return-type work.
- **Refactor `resolve_export` recursion accumulator** so the deferred plugin parameter check can be re-enabled. See "Plugin extension attempted and reverted" below for context.
- **Re-enable the deferred plugin parameter check** after the `resolve_export` refactor. The compile-time slicing block already covers the most common case (a RootVector being passed by value), but a parameter check would still catch plain `Vector<GC::Ref<T>>` parameters that aren't fed from a Root container at any call site.
- **Possible bug: minimum Cell size not enforced at compile time** — see the section below.
- **Dangerous patterns not yet caught by tooling** — see the new section below for the full audit and concrete instance lists.
- **EventInit dictionary convention**: Hand-written EventInit/Options structs in LibWeb (`PointerEventInit::coalesced_events`, `FontFaceSetLoadEventInit::fontfaces`, `MessageEventInit::ports`, `StructuredSerializeOptions::transfer`) hold sequences as `Vector<GC::Root<T>>` — each element self-roots, no heap parameter needed at struct construction. The minimal generator fix in `IDLGenerators.cpp` (`SequenceStorageType::RootVector` → `Vector` for `GC::Root<T>` element types) eliminated double-rooting and brought the iteration variable's type in line with this convention. The cleaner-in-principle alternative would be to switch the convention to `RootVector<GC::Ref<T>>` everywhere, dropping the `GC::Root<T>` element wrapper. Costs of the alternative: ~17 dict-field construction sites need to thread a `Heap&` through (`MessageEventInit init {};` → `MessageEventInit init { heap };`), and the `Vector<GC::Root<T>>` shape also appears in ~50 method return/parameter signatures across LibWeb that benefit from default-constructibility and return-by-value — those would need the same heap-threading. Not worth doing on this branch; the minimal fix preserves the existing ergonomic convention.
- **`GC::WeakHashMap` iterator support**: the initial version ships with `set` / `get` / `remove` / `contains` / `is_empty` / `clear` but no `begin()` / `end()`. None of the current callers iterate. When a caller needs iteration, mirror the `WeakHashSet` shape but expose a proxy in `operator*` so cell-typed slots come out unwrapped (`it->key()` / `it->value()`) regardless of whether the slot is stored as `Weak<T>` or plain `T`.
- **Prune on read for `GC::WeakHashSet` / `GC::WeakHashMap`**: both only call `maybe_prune()` on mutation (`set` / `remove`). In a read-heavy workload with no writes, dead entries linger until the next mutation. Could be changed to prune on `contains` / `get` / iteration too. Pros: bounded memory in pathological read-only cases. Cons: forces `mutable` state and makes const queries occasionally O(N), which is surprising at the call site. Not observed to matter in practice — every current caller has mutations mixed in — but worth reconsidering if a pure-read regression shows up.
- **`user_agent_top_level_traversable_set` lost its `Ordered` marker**: the conversion from `OrderedHashTable<TraversableNavigable*>` to `GC::WeakHashSet<TraversableNavigable>` drops the insertion-order guarantee. No caller iterates today, so the change is behaviour-preserving now, but the spec concept is an ordered set — if a future caller iterates in spec order, add `GC::OrderedWeakHashSet` (mirror of `OrderedRootHashMap`) and switch back.
- **Commit structure needs a rewrite before PR**: the per-violation commit granularity (one commit per local rooted, one per include added, etc.) is too fine — the branch currently has a long tail of one- and two-line commits that would be easier to review bundled. Before PR, walk the branch with `git rebase -i` and fold simple violations into themed commits ("LibWeb/HTML: Root transient vectors in Window named-object helpers", etc.). Big structural commits (plugin enforcement, container additions, destinations redesigns) should stay separate. Come up with the bundling strategy before the rebase so the final shape is defensible to a reviewer who hasn't followed the branch in flight. Related: `#12` — the plugin-enforcement commit still needs to land at the end of the series.
- **`GC::WeakHashMap` is not an ephemeron map**: both key and value slots are independently `Weak<T>` when they are cell types, so a value with no other live references can collect *even while its key is still alive*. JavaScript's `WeakMap` has ephemeron semantics — the value is kept alive as long as the key is alive, and only the key is held weakly. The current caller set (`node_directory` — non-cell key, cell value; and similar) does not need JS-style semantics; each use wants the entry to vanish as soon as the cell slot dies. If a future caller needs JS-`WeakMap`-equivalent behavior (typically a cache keyed by object where the value should outlive its last external reference only as long as the key survives), the container must participate in tracing rather than rely on `Weak`: either promote it to a `Cell` with a custom `visit_edges` that walks live keys and visits their values, or register it with the heap so `gather_roots` can do the same. That's meaningfully more plumbing than the current shape and should be built as a sibling template rather than retrofitted.

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
- **Silently-dropped `visit(container)` calls after the Visitor SFINAE uplift**: the template container helpers in `GC::Cell::Visitor` (`visit(Vector<T>)`, `visit(HashTable<T>)`, `visit(Span<T>)`, `visit(Optional<T>)`, and the new `visit(Variant<Ts...>)`) were guarded with `if constexpr (requires { visit(value); })` so they become no-ops when the element type isn't traceable. This makes `visitor.visit(member)` uniformly callable but creates a silent-failure hazard: if a future refactor renames a Cell type or changes a `GC::Ref<Cell>` member to a non-Cell type, the `visit(member)` call stops tracing without a compile error. The risk is sharpest for `Variant<...>` with mixed alternatives, where dropping one alternative from traceable to non-traceable silently skips that branch. Mitigations to consider: (a) a plugin check that `visit_edges` on a Cell covers every traceable member (the complement to the "field mentioned ≠ actually visited" gap above, but for the "member never mentioned at all" case); (b) keeping the container helpers strict and requiring an explicit `visit_ignored(...)` opt-in for the non-traceable case, so the unsafe path is loud at the callsite. No immediate fix needed, but this should be tracked before more `visit_edges` implementations rely on the uniform-call ergonomics.
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

**Decision framework for each site:** for every non-Cell type that holds GC pointers, prefer Cell-promotion — make the type a `GC::Cell`. The plugin stops inspecting the type's fields once `record_inherits_from_cell` returns true (`LibJSGCPluginAction.cpp:953-954`), the Cell allocator eliminates every timing window (`Heap::allocate<T>` wraps construction in `defer_gc()` / `undefer_gc()` at `Heap.h:53-55`), and `visit_edges` gets called by the tracer unconditionally. Fall back to specialized non-Cell wrappers (like the `JS::RootedExecutionContext` sketched below) only when Cell-promotion is too costly — i.e. the allocation is on a genuinely hot path that profiling shows can't absorb a small per-allocation overhead.

**Site-by-site recommendation:**

1. **`WebEngineCustomJobCallbackData` → `GC::Cell`-derived.** We're already allocating this via bare `new` on every `host_make_job_callback` call (which fires on every `.then(...)`, `await` continuation, HTML event microtask, `FinalizationRegistry` cleanup, thenable resolve). Routing the allocation through the GC heap costs roughly nothing compared to `malloc`, and it collapses the bucket-C fix structurally — no more `IGNORE_GC` on `host_defined`, no contingent safety argument, no plugin flow-analysis work. Details under "Cell-promotion plan for `CustomData`" below.
2. **`ExecutionContext` stays a non-Cell + specialized wrapper.** Push/pop on every JS function call is genuinely hot. The pool allocator (`ExecutionContextAllocator`, 4/16/64/128/256/512 Value slots) is a meaningful optimisation, and `ExecutionContext` has a variable-length Value tail that the GC heap doesn't currently bucket for. The five transient-local `IGNORE_GC` sites become `JS::RootedExecutionContext` — an RAII wrapper that owns a `NonnullOwnPtr<ExecutionContext>`, registers with VM on construction, and forwards `visit_edges`. Direct public ctors collapse "allocate + populate" (and "copy + register") into one step, eliminating the timing window inside the wrapper's construction. Details under "Implementation plan for `JS::RootedExecutionContext`" below.
3. **The existing `OwnPtr<ExecutionContext>` members on `GeneratorObject`, `SourceTextModule`, `AsyncGenerator`, `AsyncFunctionDriverWrapper`, `EnvironmentSettingsObject`** are not a hazard today. Their owners are Cells whose `visit_edges` hand-forwards through the `OwnPtr`. The plugin already trusts Cells, so no migration needed. A future plugin refinement (recognizing "manually-traced `OwnPtr<T-with-visit_edges>`" structurally) would be nice but isn't required for this branch.
4. **Promoting `ExecutionContext` to `GC::Cell` directly** — deferred. It's the cleanest fix in principle but pays the hot-path cost, loses the pool allocator, and cascades signature changes across ~130 files. Revisit only if profiling shows GC pressure from contexts or if the GC heap grows a variable-size / pooled allocator variant.

#### Cell-promotion plan for `JobCallback::CustomData`

**Status: completed in `eb55880066` (LibJS+LibWeb: Promote `JobCallback::CustomData` to `GC::Cell`).** The `host_defined` IGNORE_GC came off; `WebEngineCustomJobCallbackData::active_script_context` is still an `OwnPtr<JS::ExecutionContext>` traced through the Cell's hand-written `visit_edges`, matching the `GeneratorObject` / `SourceTextModule` pattern. Plan retained below for historical reference.

Changes to land together (should all fit in one commit or a small sequence):

1. **`JobCallback::CustomData`** (`Libraries/LibJS/Runtime/JobCallback.h:24-27`): change from bare struct to Cell.
   ```cpp
   class CustomData : public Cell {
       GC_CELL(CustomData, Cell);
   public:
       virtual ~CustomData() override = default;
   };
   ```
2. **`JobCallback::m_custom_data`** (`JobCallback.h:42-43`): change from `OwnPtr<CustomData>` to `GC::Ptr<CustomData>`. Update `JobCallback::JobCallback` ctor (`JobCallback.h:30-34`) to take `GC::Ptr<CustomData>` and `visit(m_custom_data)` in `JobCallback::visit_edges` (`JobCallback.cpp:19-23`).
3. **`JobCallback::create`** (`JobCallback.h:28`, `JobCallback.cpp:14`): signature becomes `static GC::Ref<JobCallback> create(JS::VM&, FunctionObject&, GC::Ptr<CustomData>)`. `make_job_callback` (`JobCallback.cpp:26-30`) passes `{}` for custom_data — unchanged semantically.
4. **`JobCallback::custom_data()`** accessor (`JobCallback.h:39`): returns `GC::Ptr<CustomData>` instead of `CustomData*`. Call site at `MainThreadVM.cpp:209` (`as<WebEngineCustomJobCallbackData>(*callback.custom_data())`) continues to work with `*` dereferencing the `GC::Ptr`.
5. **`WebEngineCustomJobCallbackData`** (`Libraries/LibWeb/Bindings/MainThreadVM.h:22-33`): derive from `JS::JobCallback::CustomData`, add `GC_CELL(WebEngineCustomJobCallbackData, JS::JobCallback::CustomData)` and `GC_DECLARE_ALLOCATOR(WebEngineCustomJobCallbackData)` (plus matching `GC_DEFINE_ALLOCATOR` in a `.cpp`). Change `incumbent_settings` from raw `GC::Ref<HTML::EnvironmentSettingsObject>` to `GC::Ref<HTML::EnvironmentSettingsObject>` (no change — already a GC ref). Add a `visit_edges` override:
   ```cpp
   virtual void visit_edges(JS::Cell::Visitor& visitor) override
   {
       Base::visit_edges(visitor);
       visitor.visit(incumbent_settings);
       if (active_script_context)
           active_script_context->visit_edges(visitor);
   }
   ```
   The `OwnPtr<JS::ExecutionContext> active_script_context` member is fine as-is — once `WebEngineCustomJobCallbackData` is a Cell, the plugin stops walking its fields, and the hand-written `active_script_context->visit_edges(visitor)` forward is the same pattern `GeneratorObject`, `SourceTextModule`, etc. already use.
6. **`host_make_job_callback` construction path** (`MainThreadVM.cpp:338-376`): replace the bare `new` + `adopt_own` + `move(script_execution_context)` sequence with `vm.heap().allocate<WebEngineCustomJobCallbackData>(incumbent_settings, ...)`. Concretely:
   ```cpp
   auto host_defined = vm.heap().allocate<WebEngineCustomJobCallbackData>(incumbent_settings);
   if (script) {
       host_defined->active_script_context = JS::ExecutionContext::create(0, ReadonlySpan<JS::Value> {}, 0);
       host_defined->active_script_context->function = nullptr;
       host_defined->active_script_context->realm = &script->settings_object().realm();
       // ... script_or_module population unchanged ...
   }
   return JS::JobCallback::create(*s_main_thread_vm, callable, host_defined);
   ```
   `host_defined` is now `GC::Ref<WebEngineCustomJobCallbackData>` — the plugin recognizes `GC::Ref<T>` natively (no allowlist entry needed), no `IGNORE_GC`, no FIXME. The window between `allocate<WebEngineCustomJobCallbackData>` returning and `host_defined->active_script_context = create(...)` running is inside `Heap::allocate`'s `defer_gc()` / `undefer_gc()` brackets — the ctor runs with GC deferred. After the allocation returns, `host_defined` is a stack-local `GC::Ref` — strictly speaking this is *not* an exact root in the `GC::Root<T>` sense; the cell stays reachable because the `GC::Ref` on the stack is visible to `gather_conservative_roots` (`Libraries/LibGC/Heap.cpp:482`). That's the same protection every other stack-held `GC::Ref<T>` local relies on in this codebase, so there's no new category of hazard here. Subsequent `active_script_context = create(...)` and field assignments happen while the cell is kept alive through conservative stack scanning, and its own `visit_edges` wires up `active_script_context` precisely. No timing hazard remains.

After step 6, the `script_execution_context` and `host_defined` `IGNORE_GC` annotations both come off structurally.

#### Why `ExecutionContext` stays a non-Cell with a specialized wrapper

Cell-promotion is the default answer unless the hot-path cost is prohibitive. For `ExecutionContext` specifically, it is:

- **Push/pop hot path.** `VM::m_execution_context_stack` is modified on every JS function call, every generator yield, every module evaluation. A `Vector<GC::Ptr<ExecutionContext>>` would add atomic-barrier traffic to the most performance-sensitive codepath in the interpreter.
- **Pool allocator.** `ExecutionContextAllocator` (`ExecutionContext.cpp:97-101`) buckets allocations by tail size (4/16/64/128/256/512 Value slots). The GC heap's size-class-based allocator doesn't currently know about the flexible Value tail, and re-expressing that as a GC-friendly layout is a separate project.
- **Variable-length Value tail.** `ExecutionContext` overlays a `Value[]` tail after its base struct (`ExecutionContext.h:85-88`). The GC heap allocator's size classes are fixed per type; supporting variable-size cells is possible but would require new infrastructure.
- **Cross-library reach.** ~130 files in LibJS and LibWeb hold `OwnPtr<ExecutionContext>` / `NonnullOwnPtr<ExecutionContext>` members (generators, modules, settings objects, driver wrappers, etc.). Converting all of them in one go is a cross-cutting refactor that deserves its own branch and its own profiling.

So the branch keeps `ExecutionContext` as-is and introduces `JS::RootedExecutionContext` for the transient-local cluster. If profiling ever shows a reason to Cell-promote, the wrapper's `visit_edges` forwarder already describes the needed tracing contract and the migration is largely a find-and-replace.

#### Implementation plan for option 1 (`JS::RootedExecutionContext`)

**Status: landed (2026-04-19).** Commits `b63fa3fd7c` (type + wiring + test), `0d05ebe7a3` (Window setup migration — closes two violations), `5338e75b36` (four `IGNORE_GC` sites migrated). The live shape differs from the sketch below in two ways worth knowing: (a) `release()` is **not** rvalue-ref qualified (matches AK convention for `OwnPtr::release_nonnull` / `NonnullOwnPtr::leak_ptr`; call sites are `rc.release()` not `move(rc).release()`); (b) the `MainThreadVM` `script_execution_context` site pushes the release into `WebEngineCustomJobCallbackData`'s own ctor (ctor takes `Optional<JS::RootedExecutionContext>&&`, releases in member-init inside `Heap::allocate`'s defer_gc) rather than using an explicit `DeferGC` block at the caller — cleaner since there was a single construction site. The rest of the plan below is preserved for historical reference.

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
| `Libraries/LibWeb/Bindings/MainThreadVM.cpp:347` (`script_execution_context`) + `:374` (`host_defined`) | `IGNORE_GC OwnPtr<JS::ExecutionContext> script_execution_context;` + conditional populate + `move(script_execution_context)` into `WebEngineCustomJobCallbackData` | Handled by the Cell-promotion of `WebEngineCustomJobCallbackData` (see "Cell-promotion plan for `JobCallback::CustomData`" above in the decision framework). Once `WebEngineCustomJobCallbackData` is a `GC::Cell` allocated via `vm.heap().allocate<...>`, `host_defined` becomes a `GC::Ref<WebEngineCustomJobCallbackData>` local, and `active_script_context` lives as an `OwnPtr<ExecutionContext>` member of a Cell (traced via the Cell's `visit_edges`, same pattern as `GeneratorObject::m_execution_context`). Both `IGNORE_GC` annotations come off structurally. |

After all parts land, every one of the six `IGNORE_GC` + `FIXME` annotations this branch added comes off structurally:

- `dummy_execution_context` (MainThreadVM.cpp:287) → `Optional<RootedExecutionContext>` + `.emplace(...)` (bucket A).
- `new_context` (Realm.cpp:42) → direct `RootedExecutionContext` ctor (bucket A).
- `async_context` (ECMAScriptFunctionObject.cpp:437) → direct `RootedExecutionContext` copy ctor (bucket B, solved by the ctor).
- `copy` (ExecutionContext.cpp:108, inside `copy()` itself) → either leave with a rewritten FIXME (if `copy()` stays an internal call), or comes off once the `RootedExecutionContext` copy ctor is the only caller.
- `script_execution_context` (MainThreadVM.cpp:347) → disappears as a named local; the `ExecutionContext` is built as a member of the Cell-promoted `WebEngineCustomJobCallbackData`.
- `host_defined` (MainThreadVM.cpp:374) → becomes `GC::Ref<WebEngineCustomJobCallbackData>`; the plugin recognizes `GC::Ref<T>` natively.

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

### 6. Plugin can't see indirect rooting via per-object `visit_edges` hooks

`Web::WebAssembly::Detail::s_caches` (`Libraries/LibWeb/WebAssembly/WebAssembly.h:116`) is a namespace-scope `HashMap<GC::Ptr<JS::Object>, WebAssemblyCache>` that the plugin flags as an unrooted GC container. It's actually rooted indirectly: each entry's `WebAssemblyCache` is reached and traced through the per-object `WebAssembly::visit_edges(JS::Object&, ...)` hook (`Libraries/LibWeb/WebAssembly/WebAssembly.cpp:58`), which the global object's own `visit_edges` invokes — so the cache for any live global is visited as part of that global's edge walk, while caches for collected globals are dropped via the `finalize` hook.

The current pattern is sound but invisible to the plugin. Two options for closing the gap:

1. **Annotate the site with `IGNORE_GC` + comment**: keeps the existing pattern, costs ~5 lines, but adds one more `IGNORE_GC` we'd ideally not have.
2. **Teach the plugin about indirect rooting**: extend it to recognize containers that are visited from a sibling `visit_edges` overload taking the key type as its first argument. Concretely, for `HashMap<GC::Ptr<K>, V>` declared at namespace scope, look for a same-namespace `visit_edges(K&, Cell::Visitor&)` (or similar) and treat the map as rooted if found. This would also cover any future per-object cache patterns of the same shape.

Option 2 is the better long-term answer since the pattern is generic enough that it could appear elsewhere as the codebase grows — but it requires a non-trivial plugin extension. Defer to a separate piece of work; in the interim, the site is a known plugin false-positive that hasn't been silenced yet.

### 7. Plugin trusts the rooting mechanism without checking it actually reaches every GC pointer

Concrete example from this branch — `Web::Animations::AnimationUpdateContext::elements` (`Libraries/LibWeb/Animations/AnimationEffect.h`) before the fix:

```cpp
struct AnimationUpdateContext {
    struct ElementData {
        HashMap<CSS::PropertyID, NonnullRefPtr<CSS::StyleValue const>> animated_properties_before_update;
        GC::Ptr<CSS::ComputedProperties> target_style; // <- GC pointer
    };

    // Plain HashMap: plugin flags this — sees through NonnullOwnPtr to the GC::Ptr inside ElementData.
    HashMap<DOM::AbstractElement, NonnullOwnPtr<ElementData>> elements;
};
```

A naive "fix" would be to swap the outer container for `GC::ConservativeHashMap`:

```cpp
GC::ConservativeHashMap<DOM::AbstractElement, NonnullOwnPtr<ElementData>> elements;
//                                            ^^^^^^^^^^^^^^^^^^^^^^^^^
// Plugin reports clean — but for_each_possible_value only scans `sizeof(NonnullOwnPtr)`
// bytes per entry. Those bytes are just a pointer to ElementData on the heap; the actual
// `GC::Ptr<ComputedProperties> target_style` lives at a different address that the
// conservative scan never visits, so the GC can still free target_style mid-update.
```

The actual fix was to inline `ElementData` so its bytes (including `target_style`) sit inside what `for_each_possible_value` scans:

```cpp
GC::ConservativeHashMap<DOM::AbstractElement, ElementData> elements;
```

It's worth noting *why* this was an OwnPtr in the first place — the original commit (`92221f0c57`) added the struct as part of an animation-correctness fix, not for any specific pointer-stability requirement. Inspecting the call sites, nothing keeps a reference to an entry across map mutations, so the indirection wasn't load-bearing. Two reasons authors reach for `OwnPtr<Heavy>` inside maps even when they don't need to:

1. **Reflex.** "Big struct in a map → wrap in OwnPtr." Often the only heavy thing is some heap-backed member (here, the inner `HashMap`'s storage), which is allocated separately either way — wrapping the *outer* struct just adds an extra hop.
2. **Defensive against rehash invalidation.** If anything holds a `T&` or `T*` to an entry across an insertion that triggers rehash, the reference dangles. `OwnPtr` stabilises the pointee. But you only need this when references actually *do* survive across mutations.

Both motivations are reasonable in isolation, and neither author would normally think "this also affects GC rooting". That's exactly what the plugin extension below would catch — so the rooting hole isn't gated on whether the original choice of `OwnPtr` was justified.

The same trap can hide inside a Cell's manually written `visit_edges`:

```cpp
struct Helper {
    GC::Ptr<Foo> m_foo; // <- GC pointer hidden behind OwnPtr indirection
    // (no visit_edges)
};

class MyCell : public Cell {
    GC_CELL(MyCell, Cell);
    NonnullOwnPtr<Helper> m_helper;

    void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_helper); // <- no-op! Visitor doesn't know to traverse OwnPtr's pointee.
                                 //    m_helper->m_foo is never visited.
    }
};
```

This is one symptom of a broader gap: **the plugin treats every rooting mechanism (Conservative scan, `gather_roots`, Cell `visit_edges`) as a black box that it trusts to handle all GC pointers in the value type, but it never verifies that the mechanism actually reaches them when there's indirect storage in the way.** Each rooting mechanism has different reach:

- **Conservative scan** (`Conservative{Vector,HashMap,HashTable}::for_each_possible_value`): walks the *direct bytes* of each entry. Any GC pointer hidden behind a smart-pointer or raw-pointer hop into a separate heap allocation is invisible.
- **Exact gather_roots** (`Root{Vector,HashMap,HashTable}::gather_roots`): only handles entry types that are directly Cell-convertible or `NanBoxedValue` (enforced by static_assert) — but says nothing about whether those entries' fields contain further indirect storage. In practice the static_assert means most root containers' values can't even be wrappers, so this gap is narrower here.
- **Cell `visit_edges`** (manually written): visits the fields the author lists. If the author writes `visitor.visit(m_owned_struct)` where `m_owned_struct` is a `NonnullOwnPtr<NonCellStruct>` with internal GC pointers, the visitor does nothing useful — `NonnullOwnPtr` isn't a GC type, and the visitor has no way to know the pointee carries GC pointers. The Cell field needs an explicit `m_owned_struct->visit_edges(visitor)` (and `NonCellStruct` needs that method). Today nothing forces the author to do this.

A first cut at closing the gap: extend the plugin's existing transitive GC-pointer check to validate, *per rooting mechanism*, that every reachable `GC::Ptr` / `GC::Ref` / `Cell*` is actually traversed.

- For `Conservative*` sites, walk the value type's field types and reject if any field is a known indirect-storage wrapper (`OwnPtr<U>`, `NonnullOwnPtr<U>`, `RefPtr<U>`, `NonnullRefPtr<U>`, raw `U*`, `Vector<U>`, `HashMap<…, U>`, …) where `U` transitively contains a GC pointer.
- For Cell `visit_edges` bodies, verify that every field whose type transitively contains a GC pointer is either visited directly (when the field type is itself a GC pointer the visitor handles) or routed through an explicit `field.visit_edges(visitor)` call (when the field type is a non-Cell struct with its own `visit_edges`).
- For `Root*` sites, the static_assert already rules out most of the dangerous cases at the entry-type level, but the same recursive walk would tighten the residual surface.

Done well, this would have caught the `AnimationUpdateContext::elements` regression at compile time. The same machinery would also catch the symmetric bug in any future Cell that adds an `OwnPtr`-wrapped helper struct holding GC pointers and forgets to forward `visit_edges` through it.

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

After the in-flight DOM/CSS/Crypto-follow-up work (uncommitted at time of writing), a keep-going build (`./Meta/ladybird.py build -- -k0`) surfaces **105** `error: Variable with type ... not a GC root` entries plus 1 `call to deleted constructor` entry.

Session `2026-04-16` reduced this to **61** violations. Breakdown of the remainder (see the "Session closing state" sub-section below for the file/type breakdown) is dominated by the Layout cluster (~46) and four other small deferred categories. Work done this session:

- **Small transient locals rooted** via `GC::RootVector` / `GC::ConservativeVector`: `DOMRectList::create`, `Navigation::disposedNHEs`, `Element::scrolling_boxes` (also switched to `GC::Ref<Node>`), `Text::whole_text` contiguous-nodes list, `Slottable::find_slottables` / `find_flattened_slottables`, `HTMLElement::topmost_popover_ancestor` map, `EventLoop::doc_to_index`, `Window::named_objects` (via `NamedObjects(Heap&)` ctor), `Window::document_tree_child_navigable_target_name_property_set`, `XPath` node-set handoff, IndexedDB transaction scope + blocking list + `convert_a_value_to_a_key` seen, StructuredSerialize map/set `copied_list`, WebGL `params_as_values`, `WindowOrWorkerGlobalScope` performance-observer notify list + entry handoff, Node directory (now `GC::WeakHashMap`).
- **Global registries converted to `GC::WeakHashSet`**: `BrowsingContextGroup::user_agent_browsing_context_group_set`, `NavigableContainer::all_instances`, `TraversableNavigable::user_agent_top_level_traversable_set` (dropped `Ordered` — see note in Deferred).
- **Return-by-const-reference instead of unrooted copy**: `Window::pdf_viewer_{plugin,mime_type}_objects` (callers in `MimeType` / `MimeTypeArray` / `PluginArray` / `Plugin` updated to `auto const&`).
- **New container**: `GC::WeakHashMap<K, V>` in `Libraries/LibGC/WeakHashMap.h`, with the four test cases in `Tests/LibGC/TestGCContainers.cpp` (non-cell key + cell value, cell key + non-cell value, both cell, and collection-clears-entry). Applied to the `node_directory` in `LibWeb/DOM/Node.cpp`.
- **Generator-side fallout** carried over from the prior session: `IDLGenerators.cpp` now emits `GC::Ref<JS::Object>` for the IDL `object` type and moves sequence arguments into by-value destinations; hand-written destinations (`CSSTransformValue::construct_impl`, `Clipboard::write`) updated accordingly.
- **Include/iterator repairs** unrelated to rooting: `StructuredSerializeOptions` include in `DedicatedWorkerGlobalScope.cpp`, `HTMLOptionElement` direct includes in `SelectorEngine.cpp` / `HTMLOptGroupElement.cpp` / `HTMLSelectedContentElement.cpp`, `Text::split_text` ported to `WeakHashSet::Iterator::operator*` returning `Range&` rather than `Range*`.

The remaining failures fall almost entirely into the big deferred clusters (Layout `OwnPtr<FormattingContext>` / `LayoutState`, `NonnullOwnPtr<JS::ExecutionContext>`, SVGList `Vector<GC::Ref<Number>>` needing the base-class-adopt pattern). See the per-directory notes below and the `Future work` section for the remaining categories.

#### Session closing state (2026-04-19)

`ninja -k0 -C Build/release LibWeb 2>&1 | grep "not a GC root" | sort -u | wc -l` → **7**.

Directory breakdown:

| Directory | Count |
|---|---|
| LibWeb/Layout (nested `HashMap<int, Vector<T>>` in Flex/Grid, `ContainedBoxesMap`, `Viewport::update_text_blocks`) | 5 |
| LibWeb/WebIDL / XHR | 1 each (both are `Vector<Variant<cell,...>>` returns — deferred category) |

All 7 remaining violations are in the originally-deferred buckets: nested-container indirection (section 7 plugin extension) and generator-facing `Vector<Variant<cell,...>>` returns. The `JS::RootedExecutionContext` wrapper landed this session and closed both ExecutionContext sites.

#### Session closing state (2026-04-18)

`ninja -k0 -C Build/release LibWeb 2>&1 | grep "not a GC root" | sort -u | wc -l` → **13**.

Directory breakdown:

| Directory | Count |
|---|---|
| LibWeb/DOM (`Document.cpp`: 1× `NonnullOwnPtr<JS::ExecutionContext>`, 1× `Layout::TreeBuilder`) | 2 |
| LibWeb/HTML | 3 (1× `NonnullOwnPtr<JS::ExecutionContext>` in `BrowsingContext`, 1× `NonnullOwnPtr<SimilarOriginWindowAgent>`, plus `Document.cpp`'s ExecutionContext) |
| LibWeb/IndexedDB (Index/ObjectStore MutationLog transients) | 2 |
| LibWeb/Layout (nested `HashMap<int, Vector<T>>` in Flex/Grid, `ContainedBoxesMap`, `Viewport::update_text_blocks`) | 5 |
| LibWeb/WebIDL / XHR | 1 each (both are `Vector<Variant<cell,...>>` returns — deferred category) |

All 13 remaining violations are in the originally-deferred buckets — every one still blocked on a wider refactor (`JS::RootedExecutionContext` wrapper, `SimilarOriginWindowAgent` Cell promotion, MutationLog Variant alternative, nested-container plugin extension per section 7, bindings generator extension for `Vector<Variant<cell,...>>` returns, and a small `Layout::TreeBuilder` Cell promotion that can happen any time).

#### Session closing state (2026-04-17)

`ninja -k0 -C Build/release LibWeb 2>&1 | grep "not a GC root" | sort -u | wc -l` → **50**.

Directory breakdown:

| Directory | Count |
|---|---|
| LibWeb/Layout | 35 |
| LibWeb/DOM (`Document.cpp` only) | 5 |
| LibWeb/HTML | 3 (2× `NonnullOwnPtr<JS::ExecutionContext>`, 1× `NonnullOwnPtr<SimilarOriginWindowAgent>`) |
| LibWeb/IndexedDB (Index/ObjectStore MutationLog transients) | 2 |
| LibWeb/WebIDL / XHR | 1 each (both are `Vector<Variant<cell,...>>` returns — deferred category) |

Layout breakdown (all known, covered by the big deferred cluster):
`OwnPtr<FormattingContext>` / `NonnullOwnPtr<FormattingContext>` /
`LayoutState` / `NonnullOwnPtr<ComputedValues>` / `BorderConflictFinder`
/ `InlineLevelIterator` / `UsedValues` / `ContainedBoxesMap` (nested
indirection — section 7) / `HashMap<int, Vector<FlexItem>>` and
`HashMap<int, Vector<GC::Ref<Box const>>>` (same nested-container
issue) / `Vector<TextPosition>` + `Vector<TextBlock>` in
`Viewport::update_text_blocks` (needs `TextBlock` redefinition to
adopt).

#### Session closing state (2026-04-16, post-rebase)

`ninja -k0 -C Build/release LibWeb 2>&1 | grep "not a GC root" | sort -u | wc -l` → **61**.

Directory breakdown:

| Directory | Count |
|---|---|
| LibWeb/Layout | 46 |
| LibWeb/DOM (`Document.cpp` only) | 5 |
| LibWeb/HTML | 3 (2× `NonnullOwnPtr<JS::ExecutionContext>`, 1× `NonnullOwnPtr<SimilarOriginWindowAgent>`) |
| LibWeb/IndexedDB (Index/ObjectStore MutationLog transients) | 2 |
| LibWeb/Painting | 2 |
| LibWeb/SVG / WebIDL / XHR | 1 each |

Type breakdown for Layout (the only remaining large cluster): `OwnPtr<FormattingContext>` / `NonnullOwnPtr<FormattingContext>` / `LayoutState` / `NonnullOwnPtr<ComputedValues>` / small per-struct sites (`InlineLevelIterator`, `BorderConflictFinder`, `UsedValues`, `Vector<Cell>` / `Vector<Row>` in `TableGrid`). All of these are known and discussed in the per-directory notes.

#### Historical (pre-session) subsystem counts

These were the counts documented before session `2026-04-16`; kept for context so a reviewer can see the trajectory. Live counts are in the "Session closing state" block above.

| Directory | Count |
|---|---|
| LibWeb/Layout | 47 |
| LibWeb/HTML | 32 |
| LibWeb/DOM | 12 |
| LibWeb/IndexedDB (+ Internal) | 5 |
| LibWeb/Painting | 2 |
| LibWeb/Geometry / SVG / XPath / XHR / WebIDL / WebGL / ViewTransition | 1 each |

CSS/Bindings/Crypto are at zero. The Crypto cluster was closed by `7391fabfce` (SHA stale — commit subject is *LibWeb/Crypto: Promote AlgorithmMethods/AlgorithmParams to GC::Cell*) plus the in-flight visit_edges/allocator follow-up.

Historical type breakdown (pre-session):

| Type | Count |
|---|---|
| `OwnPtr<FormattingContext>` / `NonnullOwnPtr<FormattingContext>` | 14 + 4 |
| `Vector<Slottable>` (aka `Vector<Variant<GC::Ref<Element>, GC::Ref<Text>>>`) | 8 |
| `LayoutState` (struct transitively contains GC) | 7 |
| `Vector<JS::Value>` | 4 |
| `Vector<GC::Ref<MimeType>>` | 4 |
| `OrderedHashMap<FlyString, GC::Ref<Navigable>>` | 4 |
| `Vector<GC::Ref<Plugin>>` | 3 |
| `NonnullOwnPtr<ComputedValues>` | 3 |
| `NonnullOwnPtr<JS::ExecutionContext>` | 2 |
| `Web::HTML::SelectItemOptionGroup` | 2 |
| `NamedObjects` | 2 |
| `Layout::BlockFormattingContext` | 2 |
| `Vector<Text *>`, `Vector<SelectItemOption>`, `Vector<TextBlock>`, `Vector<TextPosition>`, plus many single-occurrence patterns | 1 each |

### CSS Progress (in progress)

Started fixing CSS violations. Completed:
- `LibWeb/CSS/ComputedValues.h:741` — `NonnullOwnPtr<ComputedValues> clone_inherited_values()` marked with `IGNORE_GC` + FIXME. Added `#include <LibGC/Cell.h>`.
- `LibWeb/CSS/CSSFontFeatureValuesMap.cpp:53` — `Vector<JS::Value>` → `GC::RootVector<JS::Value>`.
- `LibWeb/CSS/FontFaceSet.cpp:145` — `Vector<JS::Value>` → `GC::RootVector<JS::Value>`.
- `LibWeb/CSS/CSSTransformValue.cpp:36` — `Vector<GC::Ref<CSSTransformComponent>>` → `GC::RootVector`.
- `LibWeb/CSS/CSSUnparsedValue.cpp:22` — `Vector<CSSUnparsedSegment>` → `GC::ConservativeVector`.
- `LibWeb/CSS/CSSMath{Sum,Product,Min,Max}.cpp` + `CSSNumericArray` — `RootVector<...>&&` + `adopt_root_vector` (commit `e65d011746`).
- `LibWeb/CSS/ComputedProperties.cpp:1137` — `ContentData` promoted to `GC::Cell` (commit `321f943fdc`). Cascaded through `ContentDataAndQuoteNestingLevel`, `ComputedValues::m_noninherited.content`, `TreeBuilder.cpp`, `Node.cpp`.
- `LibWeb/CSS/ComputedProperties.cpp:1942` — `Vector<AnimationProperties>` → `GC::ConservativeVector` (commit `cd2763dad2`).
- `LibWeb/CSS/CountersSet.cpp:164` + `CountersSet.h` — promoted `CountersSet` to `GC::Cell` (commit `e33704e2ef`); cascaded through `Element` / `PseudoElement` / `AbstractElement`.
- `LibWeb/CSS/Parser/Helpers.cpp:42` — was `NonnullOwnPtr<HostDefined>`; now allocated via `realm->heap().allocate<Bindings::HostDefined>` after the `Realm::HostDefined` Cell promotion (commit `dea519a1d9`).
- `LibWeb/CSS/Parser/Helpers.cpp:28` — `OwnPtr<JS::ExecutionContext>` process-static; annotated `IGNORE_GC` with rationale pointing to section-4 bucket A. Same disposition as MainThreadVM sites.
- `LibWeb/CSS/CSSRuleList.h` + `CSSStyleSheet.cpp:230,268` + `Parser/RuleParsing.cpp:1404` — transient `Vector<GC::Ref<CSSRule>>` locals replaced with `GC::RootVector<GC::Ref<CSSRule>>`; `set_rules` takes the root vector by rvalue and adopts its storage (commit `3653c2d69d`).

Still to do in CSS:
- `StyleComputer.cpp:1947,2127` — `Vector<DOM::AbstractElement>`, `WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>>`
- `StyleScope.cpp:215,535` — `Vector<MatchingRule>`, `HashTable<DOM::Element*>`

### Animations Progress (added)

- `Libraries/LibWeb/Animations/Animatable.cpp:96` + cascading return-type changes — `Vector<GC::Ref<Animation>>` → `GC::RootVector<GC::Ref<Animation>>` across `Animatable::get_animations[_internal]`, `Document::get_animations`, `ShadowRoot::get_animations`, `calculate_get_animations<T>` (commit `f39bd44929`).
- `Libraries/LibWeb/Animations/Animation.cpp:97` + `AnimationEffect.h` — `AnimationUpdateContext` now constructs with a `GC::Heap&` and uses `GC::ConservativeHashMap<DOM::AbstractElement, ElementData>` with `ElementData` inlined into the entry value (commit `95107490eb`). See section 7 below for the gap this exposed in the plugin.

### Form Submission Progress (added)

- `Libraries/LibWeb/HTML/HTMLFormElement::get_submittable_elements` — return type `Vector<GC::Ref<DOM::Element>>` → `GC::RootVector<GC::Ref<DOM::Element>>` (commit `7cfe01c097`).
- `Libraries/LibWeb/HTML/HTMLFormElement::supported_property_names` — local `Vector<SourcedName>` → `GC::ConservativeVector<SourcedName>`.
- `Libraries/LibWeb/HTML/Navigable::NavigateParams::form_data_entry_list` — `Optional<Vector<XHR::FormDataEntry>>` → `Optional<GC::ConservativeVector<XHR::FormDataEntry>>` (matches the actual `construct_entry_list` producer; the slicing block had been blocking the assignment).

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
