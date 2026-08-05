/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/ScopeGuard.h>
#include <CraneliftFFI.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>
#include <errno.h>
#include <stdlib.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#else
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <unistd.h>
#endif

#if defined(AK_OS_MACOS)
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#endif

using namespace Wasm;
using namespace Cranelift;

extern "C" u64 wasm_cl_direct_call_with_record_fallback(void*, void*, void const*, u32, void const*, void const*);
extern "C" [[noreturn]] void wasm_cl_raise_trap();

namespace {

struct InputHeader {
    u32 function_count;
    u32 function_type_count;
    u32 function_types_offset;
    u32 helpers_offset;
    u64 code_region_start;
    u64 reloc_region_start;
    u64 total_size;
};

struct InputFunctionEntry {
    u32 insn_offset;
    u32 insn_count;
    u32 result_arity;
    u32 num_locals;
    u32 locals_offset;
    u32 num_params;
    u32 function_index;
    u32 max_call_rec_size;
};

struct InputFunctionTypeEntry {
    u32 parameters_offset;
    u32 parameter_count;
    u32 results_offset;
    u32 result_count;
};

struct OutputFunctionEntry {
    u64 code_offset;
    u32 code_size;
    u32 compiled;
    // Offset (relative to the start of the reloc region) and count of
    // `CraneliftRelocation` entries describing process-specific code targets.
    u64 reloc_offset;
    u32 reloc_count;
    u64 trap_offset;
    u32 trap_count;
    u32 native_entry_offset;
};

struct CodeMapping {
    CodeMapping(void* mapping, size_t size)
        : mapping(mapping)
        , size(size)
    {
    }

    ~CodeMapping()
    {
#if defined(AK_OS_WINDOWS)
        VirtualFree(mapping, 0, MEM_RELEASE);
#else
        munmap(mapping, size);
#endif
    }

    void* mapping;
    size_t size;
    Vector<CraneliftTrap> traps;
};

struct PendingCompiledFunction {
    u32 function_index;
    CompiledInstructions* target;
    OwnPtr<CodeMapping> mapping;
    Vector<CraneliftRelocation> relocs;
    size_t code_size;
    size_t native_entry_offset;
    size_t next_veneer_offset;
};

static constexpr size_t oop_code_region_min_size = 256 * KiB;
static constexpr size_t oop_code_bytes_per_insn = 256;
static constexpr size_t oop_reloc_region_min_size = 64 * KiB;
static constexpr size_t oop_reloc_bytes_per_insn = 128;

static size_t align_up(size_t value, size_t alignment)
{
    VERIFY(alignment > 0);
    auto remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

struct BatchInput {
    Vector<CraneliftInsn> insns;
    u32 result_arity;
    u32 function_index;
    CompiledInstructions* target;
    u32 num_locals;
    u32 num_params;
};

// Disk-cache blob format. Stable: cached files name format_version + layout_hash so
// any rebuild that changes those will simply miss the cache rather than try to
// execute incompatible bytes.
constexpr u64 cache_blob_magic = 0x4354494A4D534157ULL; // "WASMJITC" little-endian
constexpr u32 cache_blob_format_version = 30;

struct CacheBlobHeader {
    u64 magic;
    u32 format_version;
    u32 helper_count;
    u64 layout_hash;
    u8 wasm_hash[32];
    u32 function_count;
    u32 _pad;
};
static_assert(sizeof(CacheBlobHeader) == 64);

struct CacheBlobFunctionEntry {
    u32 function_index;
    u32 code_size;
    u32 native_entry_offset;
    u32 reloc_count;
    u32 trap_count;
    u32 _pad;
};
static_assert(sizeof(CacheBlobFunctionEntry) == 24);

struct CacheRecord {
    u32 function_index;
    u32 native_entry_offset;
    ByteBuffer unpatched_code;
    Vector<CraneliftRelocation> relocs;
    Vector<CraneliftTrap> traps;
};

// On a cache miss we capture every successful compile so we can hand the blob to a
// store callback after validation finishes. On a cache hit we populate the install
// map up front; the per-function lookup happens inside try_cranelift_compile when
// the dispatch table for that function has just been built and is ready to receive
// a handler_ptr.
struct CacheCaptureState {
    bool capturing { false };
    Vector<CacheRecord> records;
};
struct PendingInstallState {
    bool active { false };
    HashMap<u32, CacheRecord> records;
};
struct CacheState {
    CacheCaptureState cache_capture;
    PendingInstallState pending_install;
    Vector<BatchInput> pending_batch;
};

static thread_local u32 s_active_function_index = NumericLimits<u32>::max();

static CacheState& cranelift_cache_state()
{
    static thread_local auto* state = new CacheState;
    return *state;
}

static u64 compute_layout_hash(RuntimeHelpers const& h)
{
    auto fnv1a = [](u64 hash, u64 value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xff;
            hash *= 0x100000001b3ULL;
        }
        return hash;
    };
    u64 hash = 0xcbf29ce484222325ULL;
    hash = fnv1a(hash, h.regs_offset);
    hash = fnv1a(hash, h.value_size);
    hash = fnv1a(hash, h.locals_base_offset);
    hash = fnv1a(hash, h.table_instances_offset);
    hash = fnv1a(hash, h.memory_instances_offset);
    hash = fnv1a(hash, h.global_instances_offset);
    hash = fnv1a(hash, h.global_instance_value_offset);
    hash = fnv1a(hash, h.memory_instance_data_offset);
    hash = fnv1a(hash, h.memory_buffer_storage_offset_offset);
    hash = fnv1a(hash, h.compiled_call_result_scratch_offset);
    hash = fnv1a(hash, h.value_stack_base_offset);
    hash = fnv1a(hash, h.value_stack_top_offset);
    hash = fnv1a(hash, h.call_record_base_offset);
    hash = fnv1a(hash, h.call_record_stack_top_offset);
    hash = fnv1a(hash, h.depth_offset);
    hash = fnv1a(hash, h.current_compiled_fn_table_data_offset);
    hash = fnv1a(hash, h.current_module_offset);
    hash = fnv1a(hash, h.current_canonical_types_offset);
    hash = fnv1a(hash, h.current_expression_offset);
    hash = fnv1a(hash, h.compiled_function_entry_size);
    hash = fnv1a(hash, h.compiled_function_entry_expression_offset);
    hash = fnv1a(hash, h.table_instance_size_offset);
    hash = fnv1a(hash, h.table_instance_callables_offset);
    hash = fnv1a(hash, h.callable_defined_type_offset);
    hash = fnv1a(hash, h.callable_module_offset);
    hash = fnv1a(hash, h.callable_compiled_instructions_offset);
    hash = fnv1a(hash, h.compiled_instructions_native_entry_offset);
    return hash;
}

// `HelperId` values are assigned in lockstep with the field order of `RuntimeHelpers`,
// so the helper address for id N is simply the N-th `size_t` field of the struct.
static_assert(offsetof(RuntimeHelpers, call_function) == 0);
static_assert(offsetof(RuntimeHelpers, memory_fill) == sizeof(size_t) * 10);
static_assert(offsetof(RuntimeHelpers, primitive_storage_cage_base) == sizeof(size_t) * 11);
static_assert(offsetof(RuntimeHelpers, call_indirect_with_record) == sizeof(size_t) * 12);
static_assert(offsetof(RuntimeHelpers, stack_exhaustion) == sizeof(size_t) * 13);
static_assert(offsetof(RuntimeHelpers, raise_trap) == sizeof(size_t) * 14);
static_assert(offsetof(RuntimeHelpers, check_indirect_type) == sizeof(size_t) * 15);
static_assert(HELPER_COUNT == 16);
static_assert(sizeof(CraneliftRelocation) == 32);

static Optional<FlatPtr> apply_addend(FlatPtr target, i64 addend)
{
    Checked<FlatPtr> address { target };
    if (addend >= 0) {
        address += static_cast<FlatPtr>(addend);
    } else {
        auto const magnitude = static_cast<u64>(-(addend + 1)) + 1;
        if (!AK::is_within_range<FlatPtr>(magnitude))
            return {};
        address -= static_cast<FlatPtr>(magnitude);
    }
    if (address.has_overflow())
        return {};
    return address.value();
}

static Optional<i64> address_delta(FlatPtr target, FlatPtr source)
{
    if (target >= source) {
        auto const delta = target - source;
        if (delta > static_cast<FlatPtr>(NumericLimits<i64>::max()))
            return {};
        return static_cast<i64>(delta);
    }

    auto const magnitude = source - target;
    auto const i64_min_magnitude = static_cast<FlatPtr>(NumericLimits<i64>::max()) + 1;
    if (magnitude > i64_min_magnitude)
        return {};
    if (magnitude == i64_min_magnitude)
        return NumericLimits<i64>::min();
    return -static_cast<i64>(magnitude);
}

static Optional<FlatPtr> append_call_veneer(PendingCompiledFunction& pending, FlatPtr target)
{
    constexpr size_t veneer_size = 16;
    if (pending.next_veneer_offset > pending.mapping->size || veneer_size > pending.mapping->size - pending.next_veneer_offset)
        return {};

    auto* veneer = static_cast<u8*>(pending.mapping->mapping) + pending.next_veneer_offset;
#if ARCH(AARCH64)
    // ldr x16, #8; br x16; .quad target
    constexpr u32 load_target = 0x58000050;
    constexpr u32 branch_target = 0xD61F0200;
    __builtin_memcpy(veneer, &load_target, sizeof(load_target));
    __builtin_memcpy(veneer + sizeof(load_target), &branch_target, sizeof(branch_target));
    __builtin_memcpy(veneer + 8, &target, sizeof(target));
#elif ARCH(X86_64)
    // jmp qword ptr [rip]; .quad target
    constexpr u8 jump_target[] = { 0xff, 0x25, 0, 0, 0, 0 };
    __builtin_memcpy(veneer, jump_target, sizeof(jump_target));
    __builtin_memcpy(veneer + sizeof(jump_target), &target, sizeof(target));
#else
    (void)target;
    return {};
#endif

    pending.next_veneer_offset += veneer_size;
    return bit_cast<FlatPtr>(veneer);
}

static bool patch_direct_call(PendingCompiledFunction& pending, CraneliftRelocation const& relocation, FlatPtr target, bool apply_relocation_addend)
{
    auto* code_bytes = static_cast<u8*>(pending.mapping->mapping);
    auto resolved_target = apply_relocation_addend ? apply_addend(target, relocation.addend) : Optional<FlatPtr> { target };
    if (!resolved_target.has_value())
        return false;

#if ARCH(AARCH64)
    if (relocation.kind != CraneliftRelocationKind::Arm64Call)
        return false;
    if (static_cast<size_t>(relocation.code_offset) + sizeof(u32) > pending.code_size)
        return false;

    u32 instruction;
    __builtin_memcpy(&instruction, code_bytes + relocation.code_offset, sizeof(instruction));
    if ((instruction & 0xfc000000) != 0x94000000)
        return false;

    auto const patch_address = bit_cast<FlatPtr>(code_bytes + relocation.code_offset);
    auto delta = address_delta(resolved_target.value(), patch_address);
    if (!delta.has_value() || delta.value() % 4 != 0 || delta.value() < -(1 << 27) || delta.value() >= (1 << 27))
        return false;

    auto const immediate = static_cast<u32>(delta.value() >> 2) & 0x03ffffff;
    instruction = (instruction & 0xfc000000) | immediate;
    __builtin_memcpy(code_bytes + relocation.code_offset, &instruction, sizeof(instruction));
    return true;
#elif ARCH(X86_64)
    if (relocation.kind != CraneliftRelocationKind::X86CallPCRel4)
        return false;
    if (relocation.code_offset == 0 || static_cast<size_t>(relocation.code_offset) + sizeof(i32) > pending.code_size)
        return false;
    if (code_bytes[relocation.code_offset - 1] != 0xe8)
        return false;

    auto const patch_address = bit_cast<FlatPtr>(code_bytes + relocation.code_offset);
    auto delta = address_delta(resolved_target.value(), patch_address);
    if (!delta.has_value() || !AK::is_within_range<i32>(delta.value()))
        return false;

    auto const displacement = static_cast<i32>(delta.value());
    __builtin_memcpy(code_bytes + relocation.code_offset, &displacement, sizeof(displacement));
    return true;
#else
    (void)pending;
    (void)relocation;
    (void)target;
    (void)apply_relocation_addend;
    return false;
#endif
}

static bool apply_relocations(PendingCompiledFunction& pending, RuntimeHelpers const& helpers, HashMap<u32, FlatPtr> const& native_targets)
{
    auto* code_bytes = static_cast<u8*>(pending.mapping->mapping);
    auto const* helper_table = reinterpret_cast<size_t const*>(&helpers);
    for (auto const& relocation : pending.relocs) {
        if (relocation.target_kind == CraneliftRelocationTargetKind::Helper) {
            if (relocation.kind != CraneliftRelocationKind::Abs8 || relocation.target_index >= HELPER_COUNT)
                return false;
            if (static_cast<size_t>(relocation.code_offset) + sizeof(u64) > pending.code_size)
                return false;
            auto address = apply_addend(helper_table[relocation.target_index], relocation.addend);
            if (!address.has_value())
                return false;
            auto const resolved_address = address.value();
            __builtin_memcpy(code_bytes + relocation.code_offset, &resolved_address, sizeof(resolved_address));
            continue;
        }

        if (relocation.target_kind != CraneliftRelocationTargetKind::WasmFunction)
            return false;

        FlatPtr target;
        if (auto native_target = native_targets.get(relocation.target_index); native_target.has_value()) {
            target = native_target.value();
        } else {
            if (relocation.fallback_offset != NumericLimits<u32>::max()) {
                if (relocation.fallback_offset >= pending.code_size)
                    return false;
                target = bit_cast<FlatPtr>(code_bytes + relocation.fallback_offset);
            } else {
                target = bit_cast<FlatPtr>(&wasm_cl_direct_call_with_record_fallback);
            }
        }
        if (patch_direct_call(pending, relocation, target, true))
            continue;

        auto veneer = append_call_veneer(pending, target);
        if (!veneer.has_value() || !patch_direct_call(pending, relocation, veneer.value(), true))
            return false;
    }
    return true;
}

static OwnPtr<CodeMapping> allocate_code_mapping(ReadonlyBytes code_bytes, size_t veneer_count)
{
    auto const code_size = code_bytes.size();
    if (code_size == 0)
        return {};

    Checked<size_t> veneer_size { veneer_count };
    veneer_size *= 16;
    Checked<size_t> writable_size { align_up(code_size, 16) };
    writable_size += veneer_size;
    if (writable_size.has_overflow())
        return {};

#if defined(AK_OS_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto const page_size = static_cast<size_t>(si.dwPageSize);
    auto const rx_aligned_size = align_up(writable_size.value(), page_size);
    auto* jit_mem = VirtualAlloc(nullptr, rx_aligned_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!jit_mem)
        return {};
    __builtin_memcpy(jit_mem, code_bytes.data(), code_size);
    return make<CodeMapping>(jit_mem, rx_aligned_size);
#elif defined(AK_OS_MACOS)
    auto const page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto const rx_aligned_size = align_up(writable_size.value(), page_size);
    auto* jit_mapping = mmap(nullptr, rx_aligned_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit_mapping == MAP_FAILED)
        return {};

    pthread_jit_write_protect_np(0);
    ScopeGuard restore_write_protection = [] { pthread_jit_write_protect_np(1); };
    __builtin_memcpy(jit_mapping, code_bytes.data(), code_size);
    return make<CodeMapping>(jit_mapping, rx_aligned_size);
#else
    auto const page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto const rx_aligned_size = align_up(writable_size.value(), page_size);
    auto* rw_mapping = mmap(nullptr, rx_aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rw_mapping == MAP_FAILED)
        return {};
    __builtin_memcpy(rw_mapping, code_bytes.data(), code_size);
    return make<CodeMapping>(rw_mapping, rx_aligned_size);
#endif
}

static Optional<PendingCompiledFunction> prepare_compiled_function(u32 function_index, CompiledInstructions& target, ReadonlyBytes code_bytes, size_t native_entry_offset, ReadonlySpan<CraneliftRelocation> relocs, ReadonlySpan<CraneliftTrap> traps)
{
    if (target.dispatches.is_empty())
        return {};
    if (native_entry_offset >= code_bytes.size())
        return {};

    auto mapping = allocate_code_mapping(code_bytes, relocs.size());
    if (!mapping)
        return {};

    mapping->traps.ensure_capacity(traps.size());
    for (auto const& trap : traps)
        mapping->traps.unchecked_append(trap);

    Vector<CraneliftRelocation> copied_relocs;
    copied_relocs.ensure_capacity(relocs.size());
    for (auto const& reloc : relocs)
        copied_relocs.unchecked_append(reloc);

    return PendingCompiledFunction {
        .function_index = function_index,
        .target = &target,
        .mapping = move(mapping),
        .relocs = move(copied_relocs),
        .code_size = code_bytes.size(),
        .native_entry_offset = native_entry_offset,
        .next_veneer_offset = align_up(code_bytes.size(), 16),
    };
}

static bool link_compiled_function(PendingCompiledFunction& pending, RuntimeHelpers const& helpers, HashMap<u32, FlatPtr> const& native_targets)
{
#if defined(AK_OS_MACOS)
    pthread_jit_write_protect_np(0);
    ScopeGuard restore_write_protection = [] { pthread_jit_write_protect_np(1); };
#endif

    return apply_relocations(pending, helpers, native_targets);
}

static bool finalize_compiled_function(PendingCompiledFunction& pending)
{
#if defined(AK_OS_WINDOWS)
    DWORD old_protect;
    if (!VirtualProtect(pending.mapping->mapping, pending.mapping->size, PAGE_EXECUTE_READ, &old_protect))
        return false;
    FlushInstructionCache(GetCurrentProcess(), pending.mapping->mapping, pending.mapping->size);
#elif defined(AK_OS_MACOS)
    sys_icache_invalidate(pending.mapping->mapping, pending.mapping->size);
#else
    if (mprotect(pending.mapping->mapping, pending.mapping->size, PROT_READ | PROT_EXEC) != 0)
        return false;
    __builtin___clear_cache(static_cast<char*>(pending.mapping->mapping), static_cast<char*>(pending.mapping->mapping) + pending.mapping->size);
#endif
    return true;
}

static void publish_compiled_function(PendingCompiledFunction&& pending)
{
    auto* handle = pending.mapping.leak_ptr();
    auto* func_ptr = static_cast<u8 const*>(handle->mapping);
    auto* native_func_ptr = func_ptr + pending.native_entry_offset;

    pending.target->cranelift_code_handle = handle;
    pending.target->cranelift_code_size = pending.code_size;
    pending.target->cranelift_traps = handle->traps.data();
    pending.target->cranelift_trap_count = handle->traps.size();
    pending.target->cranelift_compiled = true;
    publish_cranelift_native_entry(*pending.target, bit_cast<FlatPtr>(native_func_ptr));
    publish_cranelift_entry(*pending.target, bit_cast<FlatPtr>(func_ptr));
}

static void install_compiled_functions(Vector<PendingCompiledFunction>& pending_functions, RuntimeHelpers const& helpers)
{
    HashMap<u32, FlatPtr> native_targets;
    for (auto const& pending : pending_functions) {
        auto* native_entry = static_cast<u8*>(pending.mapping->mapping) + pending.native_entry_offset;
        native_targets.set(pending.function_index, bit_cast<FlatPtr>(native_entry));
    }

    for (auto& pending : pending_functions) {
        if (!link_compiled_function(pending, helpers, native_targets))
            return;
    }

    for (auto& pending : pending_functions) {
        if (!finalize_compiled_function(pending))
            return;
    }

    for (auto& pending : pending_functions) {
        if (pending.mapping)
            publish_compiled_function(move(pending));
    }
}

// Used by the cache-install path. Freshly compiled functions are prepared as a batch so every
// native address exists before any relocations are applied.
static bool install_compiled_function(u32 function_index, CompiledInstructions& target, ReadonlyBytes code_bytes, size_t native_entry_offset, ReadonlySpan<CraneliftRelocation> relocs, ReadonlySpan<CraneliftTrap> traps, RuntimeHelpers const& helpers)
{
    auto pending = prepare_compiled_function(function_index, target, code_bytes, native_entry_offset, relocs, traps);
    if (!pending.has_value())
        return false;

    Vector<PendingCompiledFunction> pending_functions;
    pending_functions.append(pending.release_value());
    install_compiled_functions(pending_functions, helpers);
    return target.cranelift_compiled;
}

}

// Compiled functions have no Frame on the frame stack; their context lives in Configuration
// scalars that callees and interpreter unwinds clobber. Every bridge helper that can run
// another function restores the caller's context before returning to its code.
// The call record and depth are excluded: paths that change them restore them directly.
class CompiledCallerContext {
    AK_MAKE_NONCOPYABLE(CompiledCallerContext);
    AK_MAKE_NONMOVABLE(CompiledCallerContext);

public:
    explicit CompiledCallerContext(Configuration& config)
        : m_config(config)
        , m_locals_base(config.locals_base())
        , m_current_module(config.current_module())
        , m_current_compiled_fn_table(config.current_compiled_fn_table())
        , m_current_compiled_fn_table_data(config.current_compiled_fn_table_data())
        , m_current_expression(config.current_expression())
        , m_table_instances(config.m_table_instances)
        , m_memory_instances(config.m_memory_instances)
        , m_global_instances(config.m_global_instances)
        , m_current_canonical_types(config.m_current_canonical_types)
    {
    }

    ~CompiledCallerContext()
    {
        m_config.m_locals_base = m_locals_base;
        m_config.m_current_module = m_current_module;
        m_config.m_current_compiled_fn_table = m_current_compiled_fn_table;
        m_config.m_current_compiled_fn_table_data = m_current_compiled_fn_table_data;
        m_config.m_current_expression = m_current_expression;
        m_config.m_table_instances = m_table_instances;
        m_config.m_memory_instances = m_memory_instances;
        m_config.m_global_instances = m_global_instances;
        m_config.m_current_canonical_types = m_current_canonical_types;
    }

private:
    Configuration& m_config;
    Value* m_locals_base;
    ModuleInstance const* m_current_module;
    Vector<CompiledFunctionEntry> const* m_current_compiled_fn_table;
    CompiledFunctionEntry const* m_current_compiled_fn_table_data;
    Expression const* m_current_expression;
    TableInstanceTable m_table_instances;
    MemoryInstanceTable m_memory_instances;
    GlobalInstanceTable m_global_instances;
    CanonicalTypeTable m_current_canonical_types;
};

extern "C" {

static ALWAYS_INLINE i32 wasm_cl_run_compiled(BytecodeInterpreter& interpreter, Configuration& config, CompiledFunctionEntry const& entry, Value* callee_locals)
{
    auto* caller_record_base = config.call_record_base();
    auto* caller_record_mark = config.m_call_record_stack.mark();
    config.depth()++;
    config.set_callee_context(*entry.module, callee_locals, *entry.expression, entry.max_call_rec_size);
    config.ip() = 0;

    interpreter.clear_trap();
    using HandlerFn = void (*)(BytecodeInterpreter&, Configuration&, Instruction const*, u32, Dispatch const*, SourcesAndDestination const*);
    auto const handler = bit_cast<HandlerFn>(entry.handler_ptr);
    handler(interpreter, config, entry.first_insn, 0, bit_cast<Dispatch const*>(entry.dispatches_ptr), bit_cast<SourcesAndDestination const*>(entry.src_dst_ptr));

    config.m_call_record_stack.release_to(caller_record_mark);
    config.set_call_record_base(caller_record_base);
    config.depth()--;

    if (interpreter.did_trap())
        return 1;
    if (entry.arity == 1)
        config.compiled_call_result_scratch() = config.value_stack().unsafe_take_last();
    return 0;
}

static NEVER_INLINE COLD i32 wasm_cl_run_compiled_with_heap_locals(BytecodeInterpreter& interpreter, Configuration& config, CompiledFunctionEntry const& entry, Value const* args, size_t arg_count)
{
    auto total = arg_count + entry.total_local_count;
    Vector<Value, ArgumentsStaticSize> heap_locals;
    heap_locals.ensure_capacity(total);
    heap_locals.resize_and_keep_capacity(total);
    auto* callee_locals = heap_locals.data();
    for (size_t i = 0; i < arg_count; i++)
        callee_locals[i] = args[i];
    // The non-argument slots are left uninitialized; the compiled entry block zeroes its own locals.
    return wasm_cl_run_compiled(interpreter, config, entry, callee_locals);
}

static ALWAYS_INLINE i32 wasm_cl_finish_call(BytecodeInterpreter& interpreter, Configuration& config, FunctionAddress address, Value const* args, size_t arg_count)
{
    if (interpreter.trap_if_insufficient_native_stack_space())
        return 1;

    auto* instance = config.store().unsafe_get(address);

    if (auto* wasm_function = instance->get_pointer<WasmFunction>(); wasm_function
        && !config.should_limit_instruction_count()
        && cranelift_entry_acquire(wasm_function->code().func().body().compiled_instructions) != 0) {

        // Fast compiled-to-compiled call: stack-allocate locals + non-owning frame.
        auto& func = wasm_function->code().func();
        auto& ci = func.body().compiled_instructions;
        CompiledFunctionEntry const entry {
            .handler_ptr = cranelift_entry_acquire(ci),
            .dispatches_ptr = bit_cast<FlatPtr>(ci.dispatches.data()),
            .src_dst_ptr = bit_cast<FlatPtr>(ci.src_dst_mappings.data()),
            .first_insn = ci.dispatches[0].instruction,
            .expression = &func.body(),
            .module = &wasm_function->module(),
            // Match the direct-call table's sizing: the JIT frame is grown by the inlined-callee
            // locals too, so the buffer must cover them or the callee scribbles past its end.
            .total_local_count = static_cast<u32>(func.total_local_count()) + ci.cranelift_inlined_locals,
            .arity = static_cast<u32>(wasm_function->type().results().size()),
            .max_call_rec_size = static_cast<u32>(ci.max_call_rec_size),
        };

        if (arg_count + entry.total_local_count > 64) [[unlikely]]
            return wasm_cl_run_compiled_with_heap_locals(interpreter, config, entry, args, arg_count);

        // Opt out of -ftrivial-auto-var-init: only the argument slots are written below, and the
        // compiled entry block initializes its own locals, so nothing reads the rest.
        __attribute__((uninitialized)) Value callee_locals[64];
        for (size_t i = 0; i < arg_count; i++)
            callee_locals[i] = args[i];
        return wasm_cl_run_compiled(interpreter, config, entry, callee_locals);
    }

    Vector<Value, ArgumentsStaticSize> args_vec;
    config.get_arguments_allocation_if_possible(args_vec, arg_count);
    args_vec.ensure_capacity(arg_count);
    for (size_t i = 0; i < arg_count; i++)
        args_vec.unchecked_append(args[i]);

    // direct-threaded interpreter path:
    if (auto* wasm_function = instance->get_pointer<WasmFunction>(); wasm_function && !config.should_limit_instruction_count() && wasm_function->code().func().body().compiled_instructions.direct) {
        BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
        if (auto prepare_result = config.prepare_wasm_call(*wasm_function, args_vec); prepare_result.is_error()) {
            interpreter.set_trap(prepare_result.release_error());
            return 1;
        }
        config.ip() = 0;
        auto outcome = interpreter.run_compiled_function_direct(config);
        if (outcome != Outcome::Return) {
            interpreter.set_trap("Compiled function returned unexpectedly"sv);
            return 1;
        }
        if (interpreter.did_trap())
            return 1;
        if (config.frame().arity() == 1)
            config.compiled_call_result_scratch() = config.value_stack().unsafe_take_last();
        if (config.label_stack().size() > config.frame().label_index())
            config.label_stack().shrink(config.frame().label_index(), true);
        return 0;
    }

    // non-compiled call (interpreter or host function)
    Wasm::Result result { Vector<Value> {} };
    if (instance->has<WasmFunction>()) {
        BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
        result = config.call(interpreter, address, args_vec);
    } else {
        result = config.call(interpreter, address, args_vec);
        config.release_arguments_allocation(args_vec);
    }

    if (result.is_trap()) {
        interpreter.set_trap(move(result.trap()));
        return 1;
    }

    if (!result.values().is_empty())
        config.compiled_call_result_scratch() = result.values().first();
    return 0;
}

i32 wasm_cl_call_function(void* interp_ptr, void* config_ptr, i32 func_index);
i32 wasm_cl_call_function(void* interp_ptr, void* config_ptr, i32 func_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };

    auto const& module = *config.current_module();
    auto const& functions = module.functions();
    if (static_cast<size_t>(func_index) >= functions.size())
        return 1;

    auto address = functions[func_index];

    SourcesAndDestination addrs {};
    addrs.sources[0] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[1] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[2] = Dispatch::RegisterOrStack::Stack;
    addrs.destination = Dispatch::RegisterOrStack::Stack;

    auto outcome = interpreter.call_address(config, address, addrs,
        BytecodeInterpreter::CallAddressSource::CompiledDirectCall,
        BytecodeInterpreter::CallType::UsingStack);

    return outcome == Outcome::Return && interpreter.did_trap() ? 1 : 0;
}

static inline MemoryInstance* wasm_cl_get_memory(void* config_ptr, u32 mem_idx)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    return config.memory_instance(mem_idx);
}

i64 wasm_cl_memory_size(void* config_ptr, u32 mem_idx);
i64 wasm_cl_memory_size(void* config_ptr, u32 mem_idx)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    return static_cast<i64>(memory->size() / Constants::page_size);
}

i32 wasm_cl_memory_grow(void* config_ptr, u32 mem_idx, i32 pages);
i32 wasm_cl_memory_grow(void* config_ptr, u32 mem_idx, i32 pages)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto old_pages = memory->size() / Constants::page_size;
    if (!memory->grow(pages * Constants::page_size))
        return -1;
    return static_cast<i32>(old_pages);
}

i32 wasm_cl_call_indirect(void* interp_ptr, void* config_ptr, i32 table_idx, i32 type_idx, u64 element_index);
i32 wasm_cl_call_indirect(void* interp_ptr, void* config_ptr, i32 table_idx, i32 type_idx, u64 element_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };

    auto const& module = *config.current_module();
    auto table_address = module.tables()[table_idx];
    auto* table_instance = config.store().get(table_address);
    if (!table_instance || element_index >= table_instance->elements().size())
        return interpreter.set_trap(Trap::from_string("Table index out of bounds"));

    auto const& element = table_instance->elements()[element_index];
    if (!element.ref().has<Reference::Func>())
        return interpreter.set_trap(Trap::from_string("Table element is not a function reference"));

    auto const* callable = table_instance->callable_at(element_index);
    if (!callable)
        return interpreter.set_trap(Trap::from_string("Indirect call to freed function"));

    auto address = callable->address;
    // https://webassembly.github.io/spec/core/exec/instructions.html#xref-syntax-instructions-syntax-instr-control-mathsf-call-indirect-x-y
    // call_indirect's runtime check is a defined-type match (a downcast), not structural equality.
    auto const* type_expected = module.canonical_types()[type_idx];
    if (!callable->defined_type || !matches_defined_type(*callable->defined_type, *type_expected))
        return interpreter.set_trap(Trap::from_string("Indirect call type mismatch"));

    SourcesAndDestination addrs {};
    addrs.sources[0] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[1] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[2] = Dispatch::RegisterOrStack::Stack;
    addrs.destination = Dispatch::RegisterOrStack::Stack;

    auto outcome = interpreter.call_address(config, address, addrs, BytecodeInterpreter::CallAddressSource::CompiledIndirectCall, BytecodeInterpreter::CallType::UsingStack);

    return outcome == Outcome::Return && interpreter.did_trap() ? 1 : 0;
}

i32 wasm_cl_memory_copy(void* interp_ptr, void* config_ptr, u32 dst_mem, u32 src_mem, i32 dst_offset, i32 src_offset, i32 count);
i32 wasm_cl_memory_copy(void* interp_ptr, void* config_ptr, u32 dst_mem, u32 src_mem, i32 dst_offset, i32 src_offset, i32 count)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto* src_instance = wasm_cl_get_memory(config_ptr, src_mem);
    auto* dst_instance = wasm_cl_get_memory(config_ptr, dst_mem);

    auto src_end = static_cast<u64>(static_cast<u32>(src_offset)) + static_cast<u32>(count);
    auto dst_end = static_cast<u64>(static_cast<u32>(dst_offset)) + static_cast<u32>(count);
    if (src_end > src_instance->size() || dst_end > dst_instance->size())
        return interpreter.set_trap(Trap::from_string("Memory access out of bounds"));

    if (count > 0)
        dst_instance->data().copy_from(src_instance->data(), static_cast<u32>(src_offset), static_cast<u32>(dst_offset), static_cast<u32>(count));

    return 0;
}

i32 wasm_cl_memory_fill(void* interp_ptr, void* config_ptr, u32 mem_idx, i32 offset, i32 value, i32 count);
i32 wasm_cl_memory_fill(void* interp_ptr, void* config_ptr, u32 mem_idx, i32 offset, i32 value, i32 count)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto* instance = wasm_cl_get_memory(config_ptr, mem_idx);

    auto end = static_cast<u64>(static_cast<u32>(offset)) + static_cast<u32>(count);
    if (end > instance->size())
        return interpreter.set_trap(Trap::from_string("Memory access out of bounds"));

    if (count > 0)
        instance->data().fill(static_cast<u32>(offset), static_cast<u8>(value), static_cast<u32>(count));

    return 0;
}

i32 wasm_cl_call_with_record(void* interp_ptr, void* config_ptr, i32 func_index);
i32 wasm_cl_call_with_record(void* interp_ptr, void* config_ptr, i32 func_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };

    auto const& module = *config.current_module();
    auto const& functions = module.functions();
    if (static_cast<size_t>(func_index) >= functions.size())
        return 1;

    auto address = functions[func_index];
    auto* instance = config.store().get(address);
    if (!instance) {
        interpreter.set_trap("Attempt to call nonexistent function by address"sv);
        return 1;
    }

    FunctionType const* type { nullptr };
    instance->visit([&](auto const& function) { type = &function.type(); });

    return wasm_cl_finish_call(interpreter, config, address, config.call_record_base(), type->parameters().size());
}

void wasm_cl_stack_exhaustion(void* interp_ptr);
void wasm_cl_stack_exhaustion(void* interp_ptr)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    interpreter.set_trap(Constants::stack_exhaustion_message);
}

u64 wasm_cl_direct_call_with_record_fallback(void* interp_ptr, void* config_ptr, void const*, u32 func_index, void const* result_count, void const*)
{
    auto status = wasm_cl_call_with_record(interp_ptr, config_ptr, static_cast<i32>(func_index));
    if (status == 0 && bit_cast<FlatPtr>(result_count) != 0) {
        auto& config = *static_cast<Configuration*>(config_ptr);
        config.value_stack().unchecked_append(config.compiled_call_result_scratch());
    }
    return static_cast<u64>(status);
}

i32 wasm_cl_call_indirect_with_record(void* interp_ptr, void* config_ptr, i32 table_idx, i32 type_idx, u64 element_index);
i32 wasm_cl_call_indirect_with_record(void* interp_ptr, void* config_ptr, i32 table_idx, i32 type_idx, u64 element_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };

    auto const& module = *config.current_module();
    auto table_address = module.tables()[table_idx];
    auto* table_instance = config.store().get(table_address);
    if (!table_instance || element_index >= table_instance->elements().size())
        return interpreter.set_trap(Trap::from_string("Table index out of bounds"));

    auto const& element = table_instance->elements()[element_index];
    if (!element.ref().has<Reference::Func>())
        return interpreter.set_trap(Trap::from_string("Table element is not a function reference"));

    auto const* callable = table_instance->callable_at(element_index);
    if (!callable)
        return interpreter.set_trap(Trap::from_string("Indirect call to freed function"));

    // https://webassembly.github.io/spec/core/exec/instructions.html#xref-syntax-instructions-syntax-instr-control-mathsf-call-indirect-x-y
    // call_indirect's runtime check is a defined-type match (a downcast), not structural equality.
    auto const* type_expected = module.canonical_types()[type_idx];
    if (!callable->defined_type || !matches_defined_type(*callable->defined_type, *type_expected))
        return interpreter.set_trap(Trap::from_string("Indirect call type mismatch"));

    return wasm_cl_finish_call(interpreter, config, callable->address, config.call_record_base(), callable->parameter_count);
}

i32 wasm_cl_check_indirect_type(void const* actual_type_ptr, void const* expected_type_ptr);
i32 wasm_cl_check_indirect_type(void const* actual_type_ptr, void const* expected_type_ptr)
{
    auto const& actual_type = *static_cast<DefinedType const*>(actual_type_ptr);
    auto const& expected_type = *static_cast<DefinedType const*>(expected_type_ptr);
    return matches_defined_type(actual_type, expected_type) ? 0 : 1;
}

static NEVER_INLINE COLD i32 wasm_cl_direct_call_fallback(BytecodeInterpreter& interpreter, Configuration& config, i32 func_index, Value const* args, size_t arg_count)
{
    return wasm_cl_finish_call(interpreter, config, config.current_module()->functions()[func_index], args, arg_count);
}

// Direct compiled-to-compiled call. Falls back to wasm_cl_finish_call for non-compiled targets.
static ALWAYS_INLINE i32 wasm_cl_direct_call_impl(BytecodeInterpreter& interpreter, Configuration& config, i32 func_index, Value* args, size_t arg_count)
{
    auto const* table = config.current_compiled_fn_table();
    auto index = static_cast<size_t>(func_index);
    if (!table || index >= table->size() || !(*table)[index].module) [[unlikely]]
        return wasm_cl_direct_call_fallback(interpreter, config, func_index, args, arg_count);

    auto const& entry = (*table)[index];

    if (config.depth() > 500) [[unlikely]] {
        interpreter.set_trap(Constants::stack_exhaustion_message);
        return 1;
    }

    if (arg_count + entry.total_local_count > 64) [[unlikely]]
        return wasm_cl_run_compiled_with_heap_locals(interpreter, config, entry, args, arg_count);

    // Opt out of -ftrivial-auto-var-init as in wasm_cl_finish_call: only the argument slots are
    // written, and the compiled entry block initializes its own locals.
    __attribute__((uninitialized)) Value callee_locals[64];
    for (size_t i = 0; i < arg_count; i++)
        callee_locals[i] = args[i];
    return wasm_cl_run_compiled(interpreter, config, entry, callee_locals);
}

i32 wasm_cl_direct_call_0(void* interp_ptr, void* config_ptr, i32 func_index);
i32 wasm_cl_direct_call_0(void* interp_ptr, void* config_ptr, i32 func_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, nullptr, 0);
}

i32 wasm_cl_direct_call_1(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0);
i32 wasm_cl_direct_call_1(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };
    Value args[] = { Value(arg0) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 1);
}

i32 wasm_cl_direct_call_2(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1);
i32 wasm_cl_direct_call_2(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };
    Value args[] = { Value(arg0), Value(arg1) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 2);
}

i32 wasm_cl_direct_call_3(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1, i64 arg2);
i32 wasm_cl_direct_call_3(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1, i64 arg2)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    CompiledCallerContext caller_context { config };
    Value args[] = { Value(arg0), Value(arg1), Value(arg2) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 3);
}
}

namespace Wasm {

static RuntimeHelpers make_runtime_helpers()
{
    return RuntimeHelpers {
        .call_function = bit_cast<uintptr_t>(&wasm_cl_call_function),
        .memory_size = bit_cast<uintptr_t>(&wasm_cl_memory_size),
        .memory_grow = bit_cast<uintptr_t>(&wasm_cl_memory_grow),
        .call_with_record = bit_cast<uintptr_t>(&wasm_cl_call_with_record),
        .direct_call_0 = bit_cast<uintptr_t>(&wasm_cl_direct_call_0),
        .direct_call_1 = bit_cast<uintptr_t>(&wasm_cl_direct_call_1),
        .direct_call_2 = bit_cast<uintptr_t>(&wasm_cl_direct_call_2),
        .direct_call_3 = bit_cast<uintptr_t>(&wasm_cl_direct_call_3),
        .call_indirect = bit_cast<uintptr_t>(&wasm_cl_call_indirect),
        .memory_copy = bit_cast<uintptr_t>(&wasm_cl_memory_copy),
        .memory_fill = bit_cast<uintptr_t>(&wasm_cl_memory_fill),
        .primitive_storage_cage_base = bit_cast<uintptr_t>(&js_primitive_storage_cage_base),
        .call_indirect_with_record = bit_cast<uintptr_t>(&wasm_cl_call_indirect_with_record),
        .stack_exhaustion = bit_cast<uintptr_t>(&wasm_cl_stack_exhaustion),
        .raise_trap = bit_cast<uintptr_t>(&wasm_cl_raise_trap),
        .check_indirect_type = bit_cast<uintptr_t>(&wasm_cl_check_indirect_type),
        .regs_offset = static_cast<u32>(offsetof(Configuration, regs)),
        .value_size = static_cast<u32>(sizeof(Value)),
        .locals_base_offset = static_cast<u32>(Configuration::locals_base_offset()),
        .table_instances_offset = static_cast<u32>(Configuration::table_instances_offset()),
        .memory_instances_offset = static_cast<u32>(Configuration::memory_instances_offset()),
        .global_instances_offset = static_cast<u32>(Configuration::global_instances_offset()),
        .global_instance_value_offset = static_cast<u32>(GlobalInstance::value_offset()),
        .memory_instance_data_offset = static_cast<u32>(MemoryInstance::data_offset()),
        .memory_buffer_storage_offset_offset = static_cast<u32>(MemoryBuffer::storage_offset_offset()),
        .compiled_call_result_scratch_offset = static_cast<u32>(Configuration::compiled_call_result_scratch_offset()),
        .value_stack_base_offset = static_cast<u32>(Configuration::value_stack_base_offset()),
        .value_stack_top_offset = static_cast<u32>(Configuration::value_stack_top_offset()),
        .call_record_base_offset = static_cast<u32>(Configuration::call_record_base_offset()),
        .call_record_stack_top_offset = static_cast<u32>(Configuration::call_record_stack_top_offset()),
        .depth_offset = static_cast<u32>(Configuration::depth_offset()),
        .current_compiled_fn_table_data_offset = static_cast<u32>(Configuration::current_compiled_fn_table_data_offset()),
        .current_module_offset = static_cast<u32>(Configuration::current_module_offset()),
        .current_canonical_types_offset = static_cast<u32>(Configuration::current_canonical_types_offset()),
        .current_expression_offset = static_cast<u32>(Configuration::current_expression_offset()),
        .compiled_function_entry_size = static_cast<u32>(sizeof(CompiledFunctionEntry)),
        .compiled_function_entry_expression_offset = static_cast<u32>(offsetof(CompiledFunctionEntry, expression)),
        .table_instance_size_offset = static_cast<u32>(TableInstance::size_offset()),
        .table_instance_callables_offset = static_cast<u32>(TableInstance::callables_offset()),
        .callable_defined_type_offset = static_cast<u32>(CallableMetadata::defined_type_offset()),
        .callable_module_offset = static_cast<u32>(CallableMetadata::module_offset()),
        .callable_compiled_instructions_offset = static_cast<u32>(CallableMetadata::compiled_instructions_offset()),
        .compiled_instructions_native_entry_offset = static_cast<u32>(offsetof(CompiledInstructions, cranelift_native_entry)),
    };
}

static CraneliftInsn serialize_insn(Dispatch const& dispatch, SourcesAndDestination const& addr)
{
    CraneliftInsn out {};
    auto const* insn = dispatch.instruction;
    out.opcode = insn->opcode().value();
    out.sources[0] = addr.sources[0];
    out.sources[1] = addr.sources[1];
    out.sources[2] = addr.sources[2];
    out.destination = addr.destination;
    out.imm1 = 0;
    out.imm2 = 0;
    out.imm3 = 0;

    auto const& args = insn->arguments();
    u32 opc = out.opcode;

    if (opc == Instructions::i32_const.value()) {
        out.imm1 = static_cast<i64>(args.get<i32>());
    } else if (opc == Instructions::i64_const.value()) {
        out.imm1 = args.get<i64>();
    } else if (opc == Instructions::f32_const.value()) {
        out.imm1 = static_cast<i64>(bit_cast<i32>(args.get<float>()));
    } else if (opc == Instructions::f64_const.value()) {
        out.imm1 = bit_cast<i64>(args.get<double>());
    } else if (opc == Instructions::local_get.value() || opc == Instructions::local_set.value() || opc == Instructions::local_tee.value()) {
        out.imm1 = static_cast<i64>(insn->local_index().value());
    } else if (opc == Instructions::global_get.value() || opc == Instructions::global_set.value()) {
        out.imm1 = static_cast<i64>(args.get<GlobalIndex>().value());
    } else if (opc == Instructions::br.value() || opc == Instructions::br_if.value()) {
        auto const& br_args = args.get<Instruction::BranchArgs>();
        out.imm1 = static_cast<i64>(br_args.label.value());
    } else if (opc == Instructions::block.value() || opc == Instructions::loop.value() || opc == Instructions::if_.value()) {
        auto const& struct_args = args.get<Instruction::StructuredInstructionArgs>();
        out.imm1 = static_cast<i64>(struct_args.end_ip.value());
        out.imm2 = struct_args.else_ip().has_value()
            ? static_cast<i64>(struct_args.else_ip()->value())
            : -1;
        u32 arity = struct_args.meta.arity;
        u32 param_count = struct_args.meta.parameter_count;
        out.imm3 = arity | (param_count << 16);
    } else if (opc == Instructions::br_table.value()) {
        auto const& table_args = args.get<Instruction::TableBranchArgs>();
        if (table_args.default_.value() > NumericLimits<u16>::max()) {
            out.imm3 = 0xff;
            return out;
        }
        for (auto const& label : table_args.labels) {
            if (label.value() > NumericLimits<u16>::max()) {
                out.imm3 = 0xff;
                return out;
            }
        }

        // Pack: imm3 low byte = min(label_count, 8), upper bits = default label.
        // First 4 labels in imm1, next 4 in imm2 (16 bits each).
        // If more than 8 labels, continuation instructions carry the rest.
        auto const total_labels = table_args.labels.size();
        auto const inline_count = min(total_labels, static_cast<size_t>(8));
        out.imm3 = static_cast<u32>(inline_count) | (static_cast<u32>(table_args.default_.value()) << 8);
        for (size_t i = 0; i < inline_count; ++i) {
            auto const encoded = static_cast<u64>(table_args.labels[i].value()) << ((i % 4) * 16);
            if (i < 4)
                out.imm1 |= static_cast<i64>(encoded);
            else
                out.imm2 |= static_cast<i64>(encoded);
        }
    } else if (opc == Instructions::call.value()) {
        out.imm1 = static_cast<i64>(args.get<FunctionIndex>().value());
    } else if (opc == Instructions::call_indirect.value()) {
        auto const& indirect_args = args.get<Instruction::IndirectCallArgs>();
        out.imm1 = static_cast<i64>(indirect_args.type.value());
        out.imm2 = static_cast<i64>(indirect_args.table.value());
    } else if (opc == Instructions::memory_copy.value()) {
        auto const& copy_args = args.get<Instruction::MemoryCopyArgs>();
        out.imm1 = static_cast<i64>(copy_args.dst_index.value());
        out.imm2 = static_cast<i64>(copy_args.src_index.value());
    } else if (opc == Instructions::memory_fill.value()) {
        auto const& fill_args = args.get<Instruction::MemoryIndexArgument>();
        out.imm1 = static_cast<i64>(fill_args.memory_index.value());
    } else if (opc >= Instructions::i32_load.value() && opc <= Instructions::i64_store32.value()) {
        auto const& mem_arg = args.get<Instruction::MemoryArgument>();
        out.imm1 = static_cast<i64>(mem_arg.offset);
        out.imm3 = static_cast<u32>(mem_arg.memory_index.value());
    } else if (opc == Instructions::memory_size.value()
        || opc == Instructions::memory_grow.value()) {
        auto const& mem_idx_arg = args.get<Instruction::MemoryIndexArgument>();
        out.imm1 = static_cast<i64>(mem_idx_arg.memory_index.value());
    }

    auto is_syn = [opc](OpCode op) { return opc == op.value(); };
    auto syn_between = [opc](OpCode lo, OpCode hi) { return opc >= lo.value() && opc <= hi.value(); };

    if (opc >= Instructions::SyntheticInstructionBase.value()) {
        if (syn_between(Instructions::synthetic_call_00, Instructions::synthetic_call_31)) {
            out.imm1 = static_cast<i64>(args.get<FunctionIndex>().value());
        } else if (is_syn(Instructions::synthetic_call_with_record_0) || is_syn(Instructions::synthetic_call_with_record_1)) {
            out.imm1 = static_cast<i64>(args.get<FunctionIndex>().value());
        } else if (is_syn(Instructions::synthetic_call_indirect_with_record_0) || is_syn(Instructions::synthetic_call_indirect_with_record_1)) {
            auto const& indirect_args = args.get<Instruction::IndirectCallArgs>();
            out.imm1 = static_cast<i64>(indirect_args.type.value());
            out.imm2 = static_cast<i64>(indirect_args.table.value());
        } else if (is_syn(Instructions::synthetic_br_nostack) || is_syn(Instructions::synthetic_br_if_nostack)) {
            auto const& br_args = args.get<Instruction::BranchArgs>();
            out.imm1 = static_cast<i64>(br_args.label.value());
        } else if (is_syn(Instructions::synthetic_local_copy)) {
            out.imm1 = static_cast<i64>(insn->local_index().value());
            out.imm2 = static_cast<i64>(args.get<LocalIndex>().value());
        } else if (is_syn(Instructions::synthetic_i32_add2local)
            || syn_between(Instructions::synthetic_i32_sub2local, Instructions::synthetic_i32_shrs2local)
            || is_syn(Instructions::synthetic_i64_add2local)
            || syn_between(Instructions::synthetic_i64_sub2local, Instructions::synthetic_i64_shrs2local)) {
            out.imm1 = static_cast<i64>(insn->local_index().value());
            out.imm2 = static_cast<i64>(args.get<LocalIndex>().value());
        } else if (is_syn(Instructions::synthetic_i32_addconstlocal) || is_syn(Instructions::synthetic_i32_andconstlocal)) {
            out.imm1 = static_cast<i64>(args.get<i32>());
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (is_syn(Instructions::synthetic_i64_addconstlocal) || is_syn(Instructions::synthetic_i64_andconstlocal)) {
            out.imm1 = args.get<i64>();
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (is_syn(Instructions::synthetic_i32_storelocal) || is_syn(Instructions::synthetic_i64_storelocal)) {
            auto const& mem_arg = args.get<Instruction::MemoryArgument>();
            out.imm1 = static_cast<i64>(mem_arg.offset);
            out.imm2 = static_cast<i64>(insn->local_index().value());
            out.imm3 = static_cast<u32>(mem_arg.memory_index.value());
        } else if (is_syn(Instructions::synthetic_local_seti32_const)) {
            out.imm1 = static_cast<i64>(args.get<i32>());
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (is_syn(Instructions::synthetic_local_seti64_const)) {
            out.imm1 = args.get<i64>();
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (syn_between(Instructions::synthetic_argument_get, Instructions::synthetic_argument_tee)) {
            out.imm1 = static_cast<i64>(insn->local_index().value());
        }
    }

    return out;
}

static StringView resolve_cranelift_compiler_path()
{
    // Lookup order: LADYBIRD_CRANELIFT_COMPILER, compile-time path, sibling-of-self.
    static NeverDestroyed<ByteString> s_path = []() -> ByteString {
        auto file_exists = [](ByteString const& path) {
            return FileSystem::exists(path);
        };

        if (auto const* env = getenv("LADYBIRD_CRANELIFT_COMPILER"); env && *env) {
            if (file_exists(env))
                return ByteString { env };
        }

        if (file_exists(WASM_CRANELIFT_COMPILER_PATH))
            return WASM_CRANELIFT_COMPILER_PATH;

        if (auto self_path = Core::System::current_executable_path(); !self_path.is_error()) {
            auto sibling = LexicalPath::join(LexicalPath::dirname(self_path.value()), "cranelift-compiler"sv).string();
            if (file_exists(sibling))
                return sibling;
        }

        return WASM_CRANELIFT_COMPILER_PATH;
    }();
    return s_path->view();
}

static void try_cranelift_compile_batch(Vector<BatchInput>& batch, Module const& module)
{
    if (batch.is_empty())
        return;

    static auto helpers = make_runtime_helpers();
    size_t function_count = batch.size();
    auto const entries_offset = sizeof(InputHeader);
    auto const entries_size = sizeof(InputFunctionEntry) * function_count;

    Vector<FunctionType const*> function_types;
    auto const& types = module.type_section().types();
    for (auto const& import : module.import_section().imports()) {
        import.description().visit(
            [&](TypeIndex const& type_index) {
                VERIFY(type_index.value() < types.size());
                if (types[type_index.value()].is_function())
                    function_types.append(&types[type_index.value()].function());
            },
            [&](FunctionType const& function_type) {
                function_types.append(&function_type);
            },
            [&](auto const&) {});
    }
    for (auto const& type_index : module.function_section().types()) {
        VERIFY(type_index.value() < types.size());
        VERIFY(types[type_index.value()].is_function());
        function_types.append(&types[type_index.value()].function());
    }

    auto const function_types_offset = align_up(entries_offset + entries_size, alignof(InputFunctionTypeEntry));
    auto const function_types_size = sizeof(InputFunctionTypeEntry) * function_types.size();

    size_t total_insn_count = 0;
    size_t total_locals_bytes = 0;
    size_t total_function_type_bytes = 0;
    for (auto& entry : batch) {
        total_insn_count += entry.insns.size();
        total_locals_bytes += entry.num_locals;
    }
    for (auto const* function_type : function_types)
        total_function_type_bytes += function_type->parameters().size() + function_type->results().size();

    auto const insn_region_offset = align_up(function_types_offset + function_types_size, alignof(CraneliftInsn));
    auto const insn_bytes = total_insn_count * sizeof(CraneliftInsn);
    auto const locals_region_offset = insn_region_offset + insn_bytes;                  // u8, no alignment needed
    auto const function_type_values_offset = locals_region_offset + total_locals_bytes; // u8, no alignment needed
    auto const helpers_offset = align_up(function_type_values_offset + total_function_type_bytes, alignof(RuntimeHelpers));
    auto const code_region_start = align_up(helpers_offset + sizeof(RuntimeHelpers), alignof(OutputFunctionEntry));
    auto const code_region_size = max(oop_code_region_min_size, total_insn_count * oop_code_bytes_per_insn);
    auto const reloc_region_start = align_up(code_region_start + sizeof(OutputFunctionEntry) * function_count + code_region_size, alignof(CraneliftRelocation));
    auto const reloc_region_size = max(oop_reloc_region_min_size, total_insn_count * oop_reloc_bytes_per_insn);
    auto const total_size = reloc_region_start + reloc_region_size;

#if defined(AK_OS_WINDOWS)
    DWORD size_hi = static_cast<DWORD>(static_cast<u64>(total_size) >> 32);
    DWORD size_lo = static_cast<DWORD>(total_size & 0xFFFFFFFF);
    HANDLE section_handle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, size_hi, size_lo, NULL);
    if (!section_handle)
        return;
    SetHandleInformation(section_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    ScopeGuard close_handle = [section_handle] { CloseHandle(section_handle); };

    auto* mapping = MapViewOfFile(section_handle, FILE_MAP_ALL_ACCESS, 0, 0, total_size);
    if (!mapping)
        return;
    ScopeGuard unmap = [mapping] { UnmapViewOfFile(mapping); };
#elif defined(AK_OS_MACOS)
    // macOS lacks memfd_create; use shm_open + shm_unlink for an anonymous fd.
    char shm_name[] = "/libwasm-cranelift-XXXXXX";
    constexpr size_t shm_suffix_offset = sizeof("/libwasm-cranelift-") - 1;
    arc4random_buf(shm_name + shm_suffix_offset, 6);
    for (size_t i = shm_suffix_offset; i < shm_suffix_offset + 6; ++i)
        shm_name[i] = 'A' + (static_cast<unsigned char>(shm_name[i]) % 26);
    int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return;
    shm_unlink(shm_name);
    // POSIX shm_open sets FD_CLOEXEC on the returned fd, which would close it
    // in the spawned cranelift-compiler child. Clear it so the child inherits.
    if (auto flags = fcntl(fd, F_GETFD); flags >= 0)
        fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
    ScopeGuard close_fd = [fd] { close(fd); };
    if (ftruncate(fd, static_cast<off_t>(total_size)) < 0)
        return;

    auto* mapping = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return;
    ScopeGuard unmap = [mapping, total_size] { munmap(mapping, total_size); };
#else
    int fd = memfd_create("libwasm-cranelift", 0);
    if (fd < 0)
        return;
    ScopeGuard close_fd = [fd] { close(fd); };
    if (ftruncate(fd, static_cast<off_t>(total_size)) < 0)
        return;

    auto* mapping = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return;
    ScopeGuard unmap = [mapping, total_size] { munmap(mapping, total_size); };
#endif

    auto* base = static_cast<u8*>(mapping);
    __builtin_memset(base, 0, total_size);

    auto* header = reinterpret_cast<InputHeader*>(base);
    *header = InputHeader {
        .function_count = static_cast<u32>(function_count),
        .function_type_count = static_cast<u32>(function_types.size()),
        .function_types_offset = static_cast<u32>(function_types_offset),
        .helpers_offset = static_cast<u32>(helpers_offset),
        .code_region_start = code_region_start,
        .reloc_region_start = reloc_region_start,
        .total_size = total_size,
    };

    size_t insn_cursor = insn_region_offset;
    size_t locals_cursor = locals_region_offset;
    for (size_t i = 0; i < function_count; ++i) {
        auto& input = batch[i];
        auto* entry = reinterpret_cast<InputFunctionEntry*>(base + entries_offset + i * sizeof(InputFunctionEntry));
        *entry = InputFunctionEntry {
            .insn_offset = static_cast<u32>(insn_cursor),
            .insn_count = static_cast<u32>(input.insns.size()),
            .result_arity = input.result_arity,
            .num_locals = input.num_locals,
            .locals_offset = static_cast<u32>(locals_cursor),
            .num_params = input.num_params,
            .function_index = input.function_index,
            .max_call_rec_size = static_cast<u32>(input.target->max_call_rec_size),
        };
        __builtin_memcpy(base + insn_cursor, input.insns.data(), input.insns.size() * sizeof(CraneliftInsn));
        insn_cursor += input.insns.size() * sizeof(CraneliftInsn);

        auto const& local_types = input.target->cranelift_local_types;
        for (u32 l = 0; l < input.num_locals; ++l)
            base[locals_cursor + l] = l < local_types.size() ? local_types[l] : static_cast<u8>(ValueType::I64);
        locals_cursor += input.num_locals;
    }

    size_t function_type_values_cursor = function_type_values_offset;
    for (size_t i = 0; i < function_types.size(); ++i) {
        auto const& function_type = *function_types[i];
        auto* entry = reinterpret_cast<InputFunctionTypeEntry*>(base + function_types_offset + i * sizeof(InputFunctionTypeEntry));
        *entry = InputFunctionTypeEntry {
            .parameters_offset = static_cast<u32>(function_type_values_cursor),
            .parameter_count = static_cast<u32>(function_type.parameters().size()),
            .results_offset = static_cast<u32>(function_type_values_cursor + function_type.parameters().size()),
            .result_count = static_cast<u32>(function_type.results().size()),
        };
        for (auto const& parameter : function_type.parameters())
            base[function_type_values_cursor++] = static_cast<u8>(parameter.kind());
        for (auto const& result : function_type.results())
            base[function_type_values_cursor++] = static_cast<u8>(result.kind());
    }

    __builtin_memcpy(base + helpers_offset, &helpers, sizeof(helpers));

    Vector<ByteString> arguments;
#if defined(AK_OS_WINDOWS)
    arguments.append(ByteString::number(reinterpret_cast<uintptr_t>(section_handle)));
#else
    arguments.append(ByteString::number(fd));
#endif

    auto process_result = Core::Process::spawn({
        .name = "cranelift-compiler"sv,
        .executable = resolve_cranelift_compiler_path(),
        .arguments = arguments,
    });
    if (process_result.is_error())
        return;
    auto status_result = process_result.release_value().wait_for_termination();
    if (status_result.is_error() || status_result.value() != 0)
        return;

    // Extract results for each function.
    auto const code_base_offset = code_region_start + sizeof(OutputFunctionEntry) * function_count;
    Vector<PendingCompiledFunction> pending_functions;

    for (size_t i = 0; i < function_count; ++i) {
        auto const* output = reinterpret_cast<OutputFunctionEntry const*>(base + code_region_start + i * sizeof(OutputFunctionEntry));
        if (!output->compiled)
            continue;

        auto code_offset = static_cast<size_t>(output->code_offset);
        auto code_size = static_cast<size_t>(output->code_size);
        if (code_offset > code_region_size || code_size > code_region_size - code_offset)
            continue;
        auto code_start = code_base_offset + code_offset;
        if (code_start + code_size > total_size)
            continue;

        auto const reloc_offset = static_cast<size_t>(output->reloc_offset);
        auto const reloc_count = static_cast<size_t>(output->reloc_count);
        auto const reloc_bytes = reloc_count * sizeof(CraneliftRelocation);
        if (reloc_count != 0 && reloc_bytes / sizeof(CraneliftRelocation) != reloc_count)
            continue;
        if (reloc_offset > reloc_region_size || reloc_bytes > reloc_region_size - reloc_offset)
            continue;
        if (reloc_region_start + reloc_offset + reloc_bytes > total_size)
            continue;

        auto const trap_offset = static_cast<size_t>(output->trap_offset);
        auto const trap_count = static_cast<size_t>(output->trap_count);
        auto const trap_bytes = trap_count * sizeof(CraneliftTrap);
        if (trap_count != 0 && trap_bytes / sizeof(CraneliftTrap) != trap_count)
            continue;
        if (trap_offset > reloc_region_size || trap_bytes > reloc_region_size - trap_offset)
            continue;
        if (reloc_region_start + trap_offset + trap_bytes > total_size)
            continue;

        auto code_bytes = ReadonlyBytes { base + code_start, code_size };
        auto relocs = reloc_count == 0
            ? ReadonlySpan<CraneliftRelocation> {}
            : ReadonlySpan<CraneliftRelocation> { reinterpret_cast<CraneliftRelocation const*>(base + reloc_region_start + reloc_offset), reloc_count };
        auto traps = trap_count == 0
            ? ReadonlySpan<CraneliftTrap> {}
            : ReadonlySpan<CraneliftTrap> { reinterpret_cast<CraneliftTrap const*>(base + reloc_region_start + trap_offset), trap_count };

        auto& capture = cranelift_cache_state().cache_capture;
        if (capture.capturing && batch[i].function_index != NumericLimits<u32>::max()) {
            if (auto copy = ByteBuffer::copy(code_bytes.data(), code_bytes.size()); !copy.is_error()) {
                CacheRecord rec;
                rec.function_index = batch[i].function_index;
                rec.native_entry_offset = output->native_entry_offset;
                rec.unpatched_code = copy.release_value();
                rec.relocs.ensure_capacity(reloc_count);
                for (size_t j = 0; j < reloc_count; ++j)
                    rec.relocs.unchecked_append(relocs[j]);
                rec.traps.ensure_capacity(trap_count);
                for (size_t j = 0; j < trap_count; ++j)
                    rec.traps.unchecked_append(traps[j]);
                capture.records.append(move(rec));
            }
        }

        if (auto pending = prepare_compiled_function(batch[i].function_index, *batch[i].target, code_bytes, output->native_entry_offset, relocs, traps); pending.has_value())
            pending_functions.append(pending.release_value());
    }

    install_compiled_functions(pending_functions, helpers);
}

bool try_cranelift_compile(CompiledInstructions& compiled, u32 result_arity)
{
#if !WASM_COMPILED_FAULT_RECOVERY_SUPPORTED
    (void)compiled;
    (void)result_arity;
    return false;
#else
    auto const& dispatches = compiled.dispatches;
    auto const& addresses = compiled.src_dst_mappings;

    if (dispatches.is_empty())
        return false;

    // Already installed (either from a prior compile or a previous cache install in this
    // same validation pass) -- nothing to do.
    if (compiled.cranelift_compiled)
        return true;

    if (s_active_function_index == NumericLimits<u32>::max())
        return false;

    // Cache hit: install from the parsed blob instead of going through cranelift.
    //            dispatches[] has just been populated by try_compile_instructions, so handler_ptr is ready to be set.
    if (cranelift_cache_state().pending_install.active && s_active_function_index != NumericLimits<u32>::max()) {
        auto record = cranelift_cache_state().pending_install.records.take(s_active_function_index);
        if (record.has_value()) {
            static auto cache_install_helpers = make_runtime_helpers();
            if (install_compiled_function(
                    s_active_function_index,
                    compiled,
                    record->unpatched_code.bytes(),
                    record->native_entry_offset,
                    record->relocs.span(),
                    record->traps.span(),
                    cache_install_helpers)) {
                return true;
            }
            // Put it back so we can try later.
            cranelift_cache_state().pending_install.records.set(s_active_function_index, record.release_value());
        }
    }

    if constexpr (WASM_CRANELIFT_DEBUG) {
        // CRANELIFT_MAX_INSNS=N       skip functions with more than N dispatches.
        // CRANELIFT_MIN_INSNS=N       skip functions with fewer than N dispatches.
        // CRANELIFT_MIN_FN=N          skip functions with id < N.
        // CRANELIFT_MAX_FN=N          skip functions with id > N.
        // CRANELIFT_SKIP_FN=a,b,c     skip listed function ids.
        // CRANELIFT_ONLY_FN=a,b,c     only compile listed function ids.
        // CRANELIFT_TRACE=1           log a line per compiled function.
        static auto const read_size_env = [](char const* name, size_t fallback) {
            if (auto* env = getenv(name))
                return static_cast<size_t>(atol(env));
            return fallback;
        };
        static auto const read_set_env = [](char const* name) {
            HashTable<size_t> out;
            auto* env = getenv(name);
            if (!env || !*env)
                return out;
            StringView view { env, strlen(env) };
            view.for_each_split_view(',', SplitBehavior::Nothing, [&](auto part) {
                if (auto n = part.template to_number<size_t>(); n.has_value())
                    out.set(n.value());
            });
            return out;
        };
        static size_t s_max_insns = read_size_env("CRANELIFT_MAX_INSNS", NumericLimits<size_t>::max());
        static size_t s_min_insns = read_size_env("CRANELIFT_MIN_INSNS", 0);
        static size_t s_min_fn = read_size_env("CRANELIFT_MIN_FN", 0);
        static size_t s_max_fn = read_size_env("CRANELIFT_MAX_FN", NumericLimits<size_t>::max());
        static auto& s_skip_fn = *new HashTable<size_t>(read_set_env("CRANELIFT_SKIP_FN"));
        static auto& s_only_fn = *new HashTable<size_t>(read_set_env("CRANELIFT_ONLY_FN"));
        static auto& s_dump_fn = *new HashTable<size_t>(read_set_env("CRANELIFT_DUMP_FN"));
        static bool s_trace = getenv("CRANELIFT_TRACE") != nullptr;

        static size_t s_func_counter = 0;
        size_t func_id = s_func_counter++;

        if (dispatches.size() > s_max_insns || dispatches.size() < s_min_insns)
            return false;
        if (func_id < s_min_fn || func_id > s_max_fn)
            return false;
        if (s_skip_fn.contains(func_id))
            return false;
        if (!s_only_fn.is_empty() && !s_only_fn.contains(func_id))
            return false;
        if (s_trace)
            warnln("cranelift: compiling fn#{} ({} dispatches)", func_id, dispatches.size());

        if (s_dump_fn.contains(func_id)) {
            warnln("cranelift: dump fn#{} ({} dispatches)", func_id, dispatches.size());
            auto reg_name = [](Dispatch::RegisterOrStack reg) -> ByteString {
                if (reg == Dispatch::RegisterOrStack::Stack)
                    return "stack";
                if (reg >= Dispatch::RegisterOrStack::CallRecord)
                    return ByteString::formatted("cr{}", to_underlying(reg) - to_underlying(Dispatch::RegisterOrStack::CallRecord));
                return ByteString::formatted("reg{}", to_underlying(reg));
            };
            for (size_t ip = 0; ip < dispatches.size(); ++ip) {
                auto const& dispatch = dispatches[ip];
                auto const& addr = addresses[ip];
                ssize_t in_count = 0;
                ssize_t out_count = 0;
#    define M(name, _, ins, outs)    \
    case Instructions::name.value(): \
        in_count = ins;              \
        out_count = outs;            \
        break;
                switch (dispatch.instruction->opcode().value()) {
                    ENUMERATE_WASM_OPCODES(M)
                }
#    undef M
                StringBuilder regs;
                regs.append('(');
                for (ssize_t j = 0; j < (in_count < 0 ? 3 : in_count); ++j) {
                    if (j > 0)
                        regs.append(", "sv);
                    regs.append(reg_name(addr.sources[j]));
                }
                regs.append(')');
                if (out_count > 0) {
                    regs.appendff(" -> {}", reg_name(addr.destination));
                }
                warnln("  [{:>03}] {} {} dst={}", ip, instruction_name(dispatch.instruction->opcode()), regs.to_byte_string(), reg_name(addr.destination));
            }
        }
    }

    Vector<CraneliftInsn> flat;
    flat.ensure_capacity(dispatches.size());
    size_t raw_call_index = 0;
    size_t indirect_call_index = 0;
    for (size_t i = 0; i < dispatches.size(); ++i) {
        flat.append(serialize_insn(dispatches[i], addresses[i]));

        auto opcode = dispatches[i].instruction->opcode();
        if (opcode == Instructions::call || opcode == Instructions::call_indirect) {
            VERIFY(raw_call_index < compiled.cranelift_raw_calls.size());
            auto const& metadata = compiled.cranelift_raw_calls[raw_call_index++];
            VERIFY(metadata.instruction_index == i);
            flat.last().imm3 = metadata.parameter_count;
            flat.last().call_result_count = metadata.result_count;
        }

        if (indirect_call_index < compiled.cranelift_indirect_calls.size()
            && compiled.cranelift_indirect_calls[indirect_call_index].instruction_index == i) {
            auto const& metadata = compiled.cranelift_indirect_calls[indirect_call_index++];
            flat.last().imm3 = metadata.parameter_count;
            flat.last().call_result_count = metadata.result_count;
            flat.last().call_type_encoding = metadata.type_encoding;
        }

        if (dispatches[i].instruction->opcode().value() == Instructions::synthetic_tier_up.value())
            flat.last().imm1 = static_cast<i64>(i);

        if (dispatches[i].instruction->opcode().value() == Instructions::br_table.value()) {
            auto const& table_args = dispatches[i].instruction->arguments().get<Instruction::TableBranchArgs>();
            auto const total = table_args.labels.size();
            for (size_t base = 8; base < total; base += 8) {
                CraneliftInsn cont {};
                cont.opcode = Instructions::synthetic_br_table_cont.value();
                auto const chunk = min(total - base, static_cast<size_t>(8));
                cont.imm3 = static_cast<u32>(chunk);
                for (size_t j = 0; j < chunk; ++j) {
                    auto const encoded = static_cast<u64>(table_args.labels[base + j].value()) << ((j % 4) * 16);
                    if (j < 4)
                        cont.imm1 |= static_cast<i64>(encoded);
                    else
                        cont.imm2 |= static_cast<i64>(encoded);
                }
                flat.append(cont);
            }
        }
    }
    VERIFY(raw_call_index == compiled.cranelift_raw_calls.size());
    VERIFY(indirect_call_index == compiled.cranelift_indirect_calls.size());

    cranelift_cache_state().pending_batch.append({ move(flat), result_arity, s_active_function_index, &compiled, compiled.cranelift_local_count, compiled.cranelift_param_count });
    return false; // Not compiled yet, will be compiled in flush.
#endif
}

void flush_cranelift_batch(Module const& module)
{
    if (cranelift_cache_state().pending_batch.is_empty())
        return;
    try_cranelift_compile_batch(cranelift_cache_state().pending_batch, module);
    cranelift_cache_state().pending_batch.clear();
}

void discard_cranelift_batch()
{
    cranelift_cache_state().pending_batch.clear();
}

void free_cranelift_code(void* handle)
{
    delete static_cast<CodeMapping*>(handle);
}

void set_cranelift_active_function_index(u32 function_index)
{
    s_active_function_index = function_index;
}

void begin_cranelift_cache_capture()
{
    cranelift_cache_state().cache_capture.capturing = true;
    cranelift_cache_state().cache_capture.records.clear();
}

void abort_cranelift_cache_capture()
{
    cranelift_cache_state().cache_capture.capturing = false;
    cranelift_cache_state().cache_capture.records.clear();
}

void abort_cranelift_cache_install()
{
    cranelift_cache_state().pending_install.active = false;
    cranelift_cache_state().pending_install.records.clear();
}

Optional<ByteBuffer> serialize_cranelift_cache_blob(ReadonlyBytes wasm_hash)
{
    ScopeGuard reset = [] {
        cranelift_cache_state().cache_capture.capturing = false;
        cranelift_cache_state().cache_capture.records.clear();
    };

    auto const& capture = cranelift_cache_state().cache_capture;

    if (!capture.capturing || capture.records.is_empty())
        return {};
    if (wasm_hash.size() != 32)
        return {};

    static auto helpers = make_runtime_helpers();

    size_t total_size = sizeof(CacheBlobHeader);
    for (auto const& r : capture.records) {
        total_size += sizeof(CacheBlobFunctionEntry);
        total_size += align_up(r.unpatched_code.size(), 16);
        total_size += r.relocs.size() * sizeof(CraneliftRelocation);
        total_size += r.traps.size() * sizeof(CraneliftTrap);
    }

    auto blob_or_error = ByteBuffer::create_zeroed(total_size);
    if (blob_or_error.is_error())
        return {};
    auto blob = blob_or_error.release_value();
    auto* out = blob.data();

    auto* header = reinterpret_cast<CacheBlobHeader*>(out);
    header->magic = cache_blob_magic;
    header->format_version = cache_blob_format_version;
    header->helper_count = HELPER_COUNT;
    header->layout_hash = compute_layout_hash(helpers);
    __builtin_memcpy(header->wasm_hash, wasm_hash.data(), 32);
    header->function_count = static_cast<u32>(capture.records.size());

    size_t offset = sizeof(CacheBlobHeader);
    for (auto const& r : capture.records) {
        auto* entry = reinterpret_cast<CacheBlobFunctionEntry*>(out + offset);
        entry->function_index = r.function_index;
        entry->code_size = static_cast<u32>(r.unpatched_code.size());
        entry->native_entry_offset = r.native_entry_offset;
        entry->reloc_count = static_cast<u32>(r.relocs.size());
        entry->trap_count = static_cast<u32>(r.traps.size());
        offset += sizeof(CacheBlobFunctionEntry);

        __builtin_memcpy(out + offset, r.unpatched_code.data(), r.unpatched_code.size());
        offset += align_up(r.unpatched_code.size(), 16);

        auto reloc_bytes = r.relocs.size() * sizeof(CraneliftRelocation);
        if (reloc_bytes > 0)
            __builtin_memcpy(out + offset, r.relocs.data(), reloc_bytes);
        offset += reloc_bytes;

        auto trap_bytes = r.traps.size() * sizeof(CraneliftTrap);
        if (trap_bytes > 0)
            __builtin_memcpy(out + offset, r.traps.data(), trap_bytes);
        offset += trap_bytes;
    }

    return blob;
}

bool try_install_cranelift_cache_blob(ReadonlyBytes expected_wasm_hash, ReadonlyBytes blob)
{
    abort_cranelift_cache_install();

    if (expected_wasm_hash.size() != 32 || blob.size() < sizeof(CacheBlobHeader))
        return false;

    auto const* header = reinterpret_cast<CacheBlobHeader const*>(blob.data());
    if (header->magic != cache_blob_magic)
        return false;
    if (header->format_version != cache_blob_format_version)
        return false;
    if (header->helper_count != HELPER_COUNT)
        return false;
    if (__builtin_memcmp(header->wasm_hash, expected_wasm_hash.data(), 32) != 0)
        return false;

    static auto helpers = make_runtime_helpers();
    if (header->layout_hash != compute_layout_hash(helpers))
        return false;

    size_t offset = sizeof(CacheBlobHeader);
    for (u32 i = 0; i < header->function_count; ++i) {
        if (offset + sizeof(CacheBlobFunctionEntry) > blob.size())
            return false;
        auto const* entry = reinterpret_cast<CacheBlobFunctionEntry const*>(blob.data() + offset);
        offset += sizeof(CacheBlobFunctionEntry);
        if (entry->native_entry_offset >= entry->code_size)
            return false;

        auto code_off = offset;
        auto aligned_code_size = align_up(entry->code_size, 16);
        if (code_off + aligned_code_size > blob.size())
            return false;
        offset += aligned_code_size;

        auto reloc_off = offset;
        auto reloc_bytes = static_cast<size_t>(entry->reloc_count) * sizeof(CraneliftRelocation);
        if (reloc_off + reloc_bytes > blob.size())
            return false;
        offset += reloc_bytes;

        auto trap_off = offset;
        auto trap_bytes = static_cast<size_t>(entry->trap_count) * sizeof(CraneliftTrap);
        if (trap_off + trap_bytes > blob.size())
            return false;
        offset += trap_bytes;

        auto code_copy = ByteBuffer::copy(blob.data() + code_off, entry->code_size);
        if (code_copy.is_error())
            return false;

        CacheRecord rec;
        rec.function_index = entry->function_index;
        rec.native_entry_offset = entry->native_entry_offset;
        rec.unpatched_code = code_copy.release_value();
        rec.relocs.ensure_capacity(entry->reloc_count);
        for (u32 j = 0; j < entry->reloc_count; ++j) {
            CraneliftRelocation reloc;
            __builtin_memcpy(&reloc, blob.data() + reloc_off + j * sizeof(CraneliftRelocation), sizeof(CraneliftRelocation));
            rec.relocs.unchecked_append(reloc);
        }
        rec.traps.ensure_capacity(entry->trap_count);
        for (u32 j = 0; j < entry->trap_count; ++j) {
            CraneliftTrap trap;
            __builtin_memcpy(&trap, blob.data() + trap_off + j * sizeof(CraneliftTrap), sizeof(CraneliftTrap));
            rec.traps.unchecked_append(trap);
        }
        cranelift_cache_state().pending_install.records.set(entry->function_index, move(rec));
    }

    cranelift_cache_state().pending_install.active = true;
    return true;
}

}
