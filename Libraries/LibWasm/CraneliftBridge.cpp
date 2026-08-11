/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/DistinctNumeric.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/Queue.h>
#include <AK/ScopeGuard.h>
#include <CraneliftFFI.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/CraneliftDirectInput.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>
#include <errno.h>
#include <stdlib.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <LibSync/Mutex.h>
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
extern "C" void* wasm_cl_current_interpreter();

namespace {

struct InputHeader {
    u32 format_version;
    u32 function_count;
    u32 function_type_count;
    u32 function_types_offset;
    u32 module_type_count;
    u32 module_types_offset;
    u32 global_type_count;
    u32 global_types_offset;
    u32 layout_offset;
    u64 output_size;
    u64 total_size;
};

struct InputFunctionEntry {
    u32 preferred_frontend;
    u32 insn_offset;
    u32 insn_count;
    u32 direct_insn_offset;
    u32 direct_insn_count;
    u32 direct_branch_targets_offset;
    u32 direct_branch_target_count;
    u32 direct_locals_offset;
    u32 direct_local_count;
    u32 direct_tier_up_checkpoints_offset;
    u32 direct_tier_up_checkpoint_count;
    u32 direct_tier_up_live_local_indices_offset;
    u32 direct_tier_up_live_local_index_count;
    u32 result_arity;
    u32 num_locals;
    u32 direct_num_locals;
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

struct InputModuleTypeEntry {
    u32 is_function;
    u32 parameters_offset;
    u32 parameter_count;
    u32 results_offset;
    u32 result_count;
};

static_assert(sizeof(InputHeader) == 56);
static_assert(sizeof(InputFunctionEntry) == 80);
static_assert(sizeof(InputFunctionTypeEntry) == 16);
static_assert(sizeof(InputModuleTypeEntry) == 20);

struct OutputHeader {
    u32 function_count;
    u32 _pad;
    u64 code_base_offset;
    u64 reloc_region_start;
    u64 total_size;
};

struct OutputCompiledArtifact {
    u64 code_offset;
    u64 reloc_offset;
    u64 trap_offset;
    u32 code_size;
    u32 reloc_count;
    u32 trap_count;
    u32 native_entry_offset;
    u32 compiled;
    u32 _padding;
};
static_assert(sizeof(OutputCompiledArtifact) == 48);

struct OutputFunctionEntry {
    OutputCompiledArtifact clean;
    OutputCompiledArtifact osr;
    u32 frontend;
    u32 _padding;
};
static_assert(sizeof(OutputFunctionEntry) == 104);

static_assert(sizeof(OutputHeader) == 32);
static_assert(offsetof(OutputCompiledArtifact, trap_offset) == 16);
static_assert(offsetof(OutputFunctionEntry, frontend) == 96);

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
    CraneliftFrontend frontend;
    OwnPtr<CodeMapping> mapping;
    Vector<CraneliftRelocation> relocs;
    size_t code_size;
    size_t native_entry_offset;
    size_t next_veneer_offset;
};

struct OutputCompiledArtifactView {
    ReadonlyBytes code;
    ReadonlySpan<CraneliftRelocation> relocs;
    ReadonlySpan<CraneliftTrap> traps;
    size_t native_entry_offset;
};

struct CompilerOutputRegions {
    Optional<OutputCompiledArtifactView> read_artifact(OutputCompiledArtifact const& output) const
    {
        if (!output.compiled)
            return {};

        auto const code_offset = static_cast<size_t>(output.code_offset);
        auto const code_size = static_cast<size_t>(output.code_size);
        if (code_offset > code_region_size || code_size > code_region_size - code_offset)
            return {};
        auto const code_start = code_base_offset + code_offset;
        if (code_start > total_size || code_size > total_size - code_start)
            return {};

        auto const reloc_offset = static_cast<size_t>(output.reloc_offset);
        auto const reloc_count = static_cast<size_t>(output.reloc_count);
        Checked<size_t> reloc_bytes { reloc_count };
        reloc_bytes *= sizeof(CraneliftRelocation);
        if (reloc_bytes.has_overflow() || reloc_offset > reloc_region_size || reloc_bytes.value() > reloc_region_size - reloc_offset)
            return {};
        auto const reloc_start = reloc_region_start + reloc_offset;
        if (reloc_start > total_size || reloc_bytes.value() > total_size - reloc_start)
            return {};
        if (reloc_count != 0 && bit_cast<FlatPtr>(base + reloc_start) % alignof(CraneliftRelocation) != 0)
            return {};

        auto const trap_offset = static_cast<size_t>(output.trap_offset);
        auto const trap_count = static_cast<size_t>(output.trap_count);
        Checked<size_t> trap_bytes { trap_count };
        trap_bytes *= sizeof(CraneliftTrap);
        if (trap_bytes.has_overflow() || trap_offset > reloc_region_size || trap_bytes.value() > reloc_region_size - trap_offset)
            return {};
        auto const trap_start = reloc_region_start + trap_offset;
        if (trap_start > total_size || trap_bytes.value() > total_size - trap_start)
            return {};
        if (trap_count != 0 && bit_cast<FlatPtr>(base + trap_start) % alignof(CraneliftTrap) != 0)
            return {};

        return OutputCompiledArtifactView {
            .code = { base + code_start, code_size },
            .relocs = reloc_count == 0
                ? ReadonlySpan<CraneliftRelocation> {}
                : ReadonlySpan<CraneliftRelocation> { reinterpret_cast<CraneliftRelocation const*>(base + reloc_start), reloc_count },
            .traps = trap_count == 0
                ? ReadonlySpan<CraneliftTrap> {}
                : ReadonlySpan<CraneliftTrap> { reinterpret_cast<CraneliftTrap const*>(base + trap_start), trap_count },
            .native_entry_offset = output.native_entry_offset,
        };
    }

    u8 const* base;
    size_t total_size;
    size_t code_base_offset;
    size_t code_region_size;
    size_t reloc_region_start;
    size_t reloc_region_size;
};

static constexpr size_t oop_code_region_min_size = 256 * KiB;
static constexpr size_t oop_code_bytes_per_insn = 256;
static constexpr size_t oop_reloc_region_min_size = 64 * KiB;
static constexpr size_t oop_reloc_bytes_per_insn = 128;
static constexpr size_t native_code_alignment = 16;
static constexpr size_t call_veneer_size = 16;

#if ARCH(AARCH64)
// https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85
// https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
//
// A64 instructions are four bytes wide. LDR (literal) encodes a signed imm19 scaled by four,
// BL encodes a signed imm26 scaled by four, and BR encodes its source in the five-bit Rn field.
// AAPCS64 permits linker-generated veneers to corrupt IP0 (x16), so use it to hold the absolute
// branch target without clobbering an argument or callee-saved register.
static constexpr u32 aarch64_instruction_size = sizeof(u32);
static constexpr u32 aarch64_register_field_width = 5;
static constexpr u32 aarch64_register_field_mask = (1u << aarch64_register_field_width) - 1;
static constexpr u32 aarch64_literal_immediate_width = 19;
static constexpr u32 aarch64_literal_immediate_mask = (1u << aarch64_literal_immediate_width) - 1;
static constexpr u32 aarch64_branch_immediate_width = 26;
static constexpr u32 aarch64_branch_immediate_mask = (1u << aarch64_branch_immediate_width) - 1;
static constexpr u32 aarch64_branch_opcode_mask = ~aarch64_branch_immediate_mask;
static constexpr u32 aarch64_64_bit_literal_load_opcode = 0x58000000;
static constexpr u32 aarch64_branch_to_register_opcode = 0xd61f0000;
static constexpr u32 aarch64_branch_with_link_opcode = 0x94000000;
static constexpr u32 aarch64_veneer_target_register = 16;
static constexpr size_t aarch64_veneer_target_offset = 2 * aarch64_instruction_size;
static constexpr i64 aarch64_literal_minimum_byte_delta = -(1LL << (aarch64_literal_immediate_width - 1)) * aarch64_instruction_size;
static constexpr i64 aarch64_literal_maximum_byte_delta = (1LL << (aarch64_literal_immediate_width - 1)) * aarch64_instruction_size;
static constexpr i64 aarch64_branch_minimum_byte_delta = -(1LL << (aarch64_branch_immediate_width - 1)) * aarch64_instruction_size;
static constexpr i64 aarch64_branch_maximum_byte_delta = (1LL << (aarch64_branch_immediate_width - 1)) * aarch64_instruction_size;

static_assert(aarch64_veneer_target_register <= aarch64_register_field_mask);
static_assert(aarch64_veneer_target_offset % aarch64_instruction_size == 0);
static_assert(static_cast<i64>(aarch64_veneer_target_offset) >= aarch64_literal_minimum_byte_delta);
static_assert(static_cast<i64>(aarch64_veneer_target_offset) < aarch64_literal_maximum_byte_delta);
static_assert(aarch64_veneer_target_offset + sizeof(FlatPtr) <= call_veneer_size);

static constexpr u32 encode_aarch64_64_bit_literal_load(u32 destination_register, i32 byte_delta)
{
    auto const immediate = static_cast<u32>(byte_delta / static_cast<i32>(aarch64_instruction_size))
        & aarch64_literal_immediate_mask;
    return aarch64_64_bit_literal_load_opcode | (immediate << aarch64_register_field_width) | destination_register;
}

static constexpr u32 encode_aarch64_branch_to_register(u32 source_register)
{
    return aarch64_branch_to_register_opcode | (source_register << aarch64_register_field_width);
}

// https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85
//
// 1. Require the instruction at the relocation to be BL.
// 2. Require its byte displacement to be four-byte aligned and representable by signed imm26.
// 3. Divide the displacement by four and replace BL's imm26 field with that value.
static Optional<u32> encode_aarch64_branch_with_link(u32 instruction, i64 byte_delta)
{
    if ((instruction & aarch64_branch_opcode_mask) != aarch64_branch_with_link_opcode)
        return {};
    if (byte_delta % aarch64_instruction_size != 0)
        return {};
    if (byte_delta < aarch64_branch_minimum_byte_delta || byte_delta >= aarch64_branch_maximum_byte_delta)
        return {};

    auto const immediate = static_cast<u32>(byte_delta / aarch64_instruction_size) & aarch64_branch_immediate_mask;
    return aarch64_branch_with_link_opcode | immediate;
}
#elif ARCH(X86_64)
// https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
//
// Intel SDM Volume 2 specifies E8 cd as CALL rel32. FF /4 is JMP r/m64; ModRM 00 100 101 selects
// a RIP-relative memory operand, and a zero disp32 places the absolute target immediately after the
// instruction.
static constexpr u8 x86_call_relative_opcode = 0xe8;
static constexpr u8 x86_group_5_opcode = 0xff;
static constexpr u8 x86_jump_rip_relative_modrm = 0x25;
static constexpr size_t x86_relative_displacement_size = sizeof(i32);
static constexpr u8 x86_indirect_jump_to_next_pointer[] = {
    x86_group_5_opcode,
    x86_jump_rip_relative_modrm,
    0,
    0,
    0,
    0,
};
static constexpr size_t x86_veneer_target_offset = sizeof(x86_indirect_jump_to_next_pointer);

static_assert(x86_veneer_target_offset + sizeof(FlatPtr) <= call_veneer_size);
#endif

static size_t align_up(size_t value, size_t alignment)
{
    VERIFY(alignment > 0);
    auto remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

static ErrorOr<size_t> try_align_up(size_t value, size_t alignment)
{
    VERIFY(alignment > 0);
    auto remainder = value % alignment;
    if (remainder == 0)
        return value;

    Checked<size_t> result = value;
    result += alignment - remainder;
    if (result.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");
    return result.value();
}

static ErrorOr<size_t> compute_output_buffer_size(size_t function_count, size_t instruction_count)
{
    Checked<size_t> output_entries_size = sizeof(OutputFunctionEntry);
    output_entries_size *= function_count;
    if (output_entries_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    Checked<size_t> output_size = sizeof(OutputHeader);
    output_size += output_entries_size.value();
    if (output_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size = TRY(try_align_up(output_size.value(), SERIALIZED_CODE_ALIGNMENT));

    Checked<size_t> maximum_code_size = instruction_count;
    maximum_code_size *= oop_code_bytes_per_insn;
    if (maximum_code_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size += max(oop_code_region_min_size, maximum_code_size.value());
    if (output_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size = TRY(try_align_up(output_size.value(), alignof(CraneliftRelocation)));

    Checked<size_t> maximum_reloc_size = instruction_count;
    maximum_reloc_size *= oop_reloc_bytes_per_insn;
    if (maximum_reloc_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size += max(oop_reloc_region_min_size, maximum_reloc_size.value());
    if (output_size.has_overflow())
        return Error::from_string_literal("Cranelift output is too large");

    return output_size.value();
}

struct BatchInput {
    Vector<CraneliftInsn> insns;
    Optional<DirectCompilerInput> direct_input;
    CraneliftFrontend preferred_frontend;
    u32 result_arity;
    u32 function_index;
    CompiledInstructions* target;
    u32 num_locals;
    u32 direct_num_locals;
    u32 num_params;
};

static size_t compiler_instruction_count(BatchInput const& input)
{
    auto const direct_instruction_count = input.direct_input.has_value() ? input.direct_input->instructions.size() : 0;
    return max(input.insns.size(), direct_instruction_count);
}

// Disk-cache blob format. Stable: cached files name format_version + layout_hash so
// any rebuild that changes those will simply miss the cache rather than try to
// execute incompatible bytes.
constexpr u64 cache_blob_magic = 0x4354494A4D534157ULL; // "WASMJITC" little-endian
constexpr u32 cache_blob_format_version = 32;

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
    u32 frontend;
};
static_assert(sizeof(CacheBlobFunctionEntry) == 24);

struct CacheRecord {
    u32 function_index;
    u32 native_entry_offset;
    CraneliftFrontend frontend;
    ByteBuffer unpatched_code;
    Vector<CraneliftRelocation> relocs;
    Vector<CraneliftTrap> traps;
};

struct PendingCachedFunction {
    u32 function_index;
    CompiledInstructions* target;
    CacheRecord record;
};

// On a cache miss we capture every successful compile so we can hand the blob to a
// store callback after validation finishes. On a cache hit we populate the install
// map up front; try_cranelift_compile associates each record with its completed
// dispatch table, and flush_cranelift_batch installs the records together so their
// cross-function relocations can resolve directly.
struct CacheCaptureState {
    bool capturing { false };
    Vector<CacheRecord> records;
};
struct PendingInstallState {
    bool active { false };
    HashMap<u32, CacheRecord> records;
    Vector<PendingCachedFunction> functions;
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

static CraneliftCompileCallback& cranelift_compile_callback()
{
    static NeverDestroyed<CraneliftCompileCallback> callback;
    return *callback;
}

static u64 compute_layout_hash(RuntimeLayout const& layout)
{
    auto fnv1a = [](u64 hash, u64 value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xff;
            hash *= 0x100000001b3ULL;
        }
        return hash;
    };
    u64 hash = 0xcbf29ce484222325ULL;
    hash = fnv1a(hash, layout.regs_offset);
    hash = fnv1a(hash, layout.value_size);
    hash = fnv1a(hash, layout.locals_base_offset);
    hash = fnv1a(hash, layout.table_instances_offset);
    hash = fnv1a(hash, layout.memory_instances_offset);
    hash = fnv1a(hash, layout.global_instances_offset);
    hash = fnv1a(hash, layout.global_instance_value_offset);
    hash = fnv1a(hash, layout.memory_instance_data_offset);
    hash = fnv1a(hash, layout.memory_buffer_storage_offset_offset);
    hash = fnv1a(hash, layout.compiled_call_result_scratch_offset);
    hash = fnv1a(hash, layout.value_stack_base_offset);
    hash = fnv1a(hash, layout.value_stack_top_offset);
    hash = fnv1a(hash, layout.call_record_base_offset);
    hash = fnv1a(hash, layout.call_record_stack_top_offset);
    hash = fnv1a(hash, layout.depth_offset);
    hash = fnv1a(hash, layout.current_compiled_fn_table_data_offset);
    hash = fnv1a(hash, layout.current_module_offset);
    hash = fnv1a(hash, layout.current_canonical_types_offset);
    hash = fnv1a(hash, layout.current_expression_offset);
    hash = fnv1a(hash, layout.compiled_function_entry_size);
    hash = fnv1a(hash, layout.compiled_function_entry_expression_offset);
    hash = fnv1a(hash, layout.table_instance_size_offset);
    hash = fnv1a(hash, layout.table_instance_callables_offset);
    hash = fnv1a(hash, layout.callable_defined_type_offset);
    hash = fnv1a(hash, layout.callable_module_offset);
    hash = fnv1a(hash, layout.callable_compiled_instructions_offset);
    hash = fnv1a(hash, layout.compiled_instructions_native_entry_offset);
    hash = fnv1a(hash, layout.compiled_instructions_direct_native_entry_offset);
    return hash;
}

using RuntimeHelperAddresses = Array<size_t, HELPER_COUNT>;
static_assert(HELPER_COUNT == 17);
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
    if (pending.next_veneer_offset > pending.mapping->size
        || call_veneer_size > pending.mapping->size - pending.next_veneer_offset)
        return {};

    auto* veneer = static_cast<u8*>(pending.mapping->mapping) + pending.next_veneer_offset;
#if ARCH(AARCH64)
    // https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85
    //
    // 1. Load the absolute target stored after these two instructions into IP0 (x16).
    // 2. Branch to the address in IP0 without changing the link register set by the caller's BL.
    // 3. Store the absolute target in the eight-byte literal read by the first instruction.
    constexpr u32 load_target = encode_aarch64_64_bit_literal_load(
        aarch64_veneer_target_register,
        static_cast<i32>(aarch64_veneer_target_offset));
    constexpr u32 branch_target = encode_aarch64_branch_to_register(aarch64_veneer_target_register);
    __builtin_memcpy(veneer, &load_target, sizeof(load_target));
    __builtin_memcpy(veneer + sizeof(load_target), &branch_target, sizeof(branch_target));
    __builtin_memcpy(veneer + aarch64_veneer_target_offset, &target, sizeof(target));
#elif ARCH(X86_64)
    // https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
    //
    // 1. Jump indirectly through the 64-bit pointer immediately following the instruction.
    // 2. Store the absolute target in that pointer.
    __builtin_memcpy(veneer, x86_indirect_jump_to_next_pointer, sizeof(x86_indirect_jump_to_next_pointer));
    __builtin_memcpy(veneer + x86_veneer_target_offset, &target, sizeof(target));
#else
    (void)target;
    return {};
#endif

    pending.next_veneer_offset += call_veneer_size;
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

    auto const patch_address = bit_cast<FlatPtr>(code_bytes + relocation.code_offset);
    auto delta = address_delta(resolved_target.value(), patch_address);
    if (!delta.has_value())
        return false;

    auto patched_instruction = encode_aarch64_branch_with_link(instruction, delta.value());
    if (!patched_instruction.has_value())
        return false;
    auto const encoded_instruction = patched_instruction.release_value();
    __builtin_memcpy(code_bytes + relocation.code_offset, &encoded_instruction, sizeof(encoded_instruction));
    return true;
#elif ARCH(X86_64)
    // https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
    //
    // 1. Require E8 immediately before Cranelift's rel32 relocation field.
    // 2. Require the resolved displacement from that field to fit signed rel32.
    // 3. Store the displacement; Cranelift's relocation addend accounts for RIP advancing past it.
    if (relocation.kind != CraneliftRelocationKind::X86CallPCRel4)
        return false;
    if (relocation.code_offset == 0
        || static_cast<size_t>(relocation.code_offset) + x86_relative_displacement_size > pending.code_size)
        return false;
    if (code_bytes[relocation.code_offset - 1] != x86_call_relative_opcode)
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

static bool apply_relocations(PendingCompiledFunction& pending, RuntimeHelperAddresses const& helper_addresses, HashMap<u32, FlatPtr> const& native_targets)
{
    auto* code_bytes = static_cast<u8*>(pending.mapping->mapping);
    for (auto const& relocation : pending.relocs) {
        if (relocation.target_kind == CraneliftRelocationTargetKind::Helper) {
            if (relocation.kind != CraneliftRelocationKind::Abs8 || relocation.target_index >= HELPER_COUNT)
                return false;
            if (static_cast<size_t>(relocation.code_offset) + sizeof(u64) > pending.code_size)
                return false;
            auto address = apply_addend(helper_addresses[relocation.target_index], relocation.addend);
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
            } else if (pending.frontend == CraneliftFrontend::AllocatedBytecode) {
                target = bit_cast<FlatPtr>(&wasm_cl_direct_call_with_record_fallback);
            } else {
                // The direct frontend's typed call ABI is not compatible with the allocated-
                // bytecode bridge. Direct functions are currently selected for publication only
                // when every Wasm-function relocation resolves to another direct body.
                return false;
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

    Checked<size_t> veneer_bytes { veneer_count };
    veneer_bytes *= call_veneer_size;
    Checked<size_t> writable_size { align_up(code_size, native_code_alignment) };
    writable_size += veneer_bytes;
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

static Optional<PendingCompiledFunction> prepare_compiled_function(u32 function_index, CompiledInstructions& target, CraneliftFrontend frontend, ReadonlyBytes code_bytes, size_t native_entry_offset, ReadonlySpan<CraneliftRelocation> relocs, ReadonlySpan<CraneliftTrap> traps)
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
        .frontend = frontend,
        .mapping = move(mapping),
        .relocs = move(copied_relocs),
        .code_size = code_bytes.size(),
        .native_entry_offset = native_entry_offset,
        .next_veneer_offset = align_up(code_bytes.size(), native_code_alignment),
    };
}

static bool link_compiled_function(PendingCompiledFunction& pending, RuntimeHelperAddresses const& helper_addresses, HashMap<u32, FlatPtr> const& native_targets)
{
#if defined(AK_OS_MACOS)
    pthread_jit_write_protect_np(0);
    ScopeGuard restore_write_protection = [] { pthread_jit_write_protect_np(1); };
#endif

    return apply_relocations(pending, helper_addresses, native_targets);
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

static void publish_compiled_function(PendingCompiledFunction&& pending, Module const& module)
{
    auto* handle = pending.mapping.leak_ptr();
    auto* func_ptr = static_cast<u8 const*>(handle->mapping);
    auto* native_func_ptr = func_ptr + pending.native_entry_offset;

    pending.target->cranelift_code_start = bit_cast<FlatPtr>(func_ptr);
    pending.target->cranelift_code_size = pending.code_size;
    pending.target->cranelift_traps = handle->traps.data();
    pending.target->cranelift_trap_count = handle->traps.size();
    pending.target->cranelift_compiled = true;
    module.retain_cranelift_code_handle(handle);
    if (pending.frontend == CraneliftFrontend::Direct) {
        publish_cranelift_direct_native_entry(*pending.target, bit_cast<FlatPtr>(native_func_ptr));
        publish_cranelift_entry(*pending.target, bit_cast<FlatPtr>(func_ptr));
    } else {
        publish_cranelift_native_entry(*pending.target, bit_cast<FlatPtr>(native_func_ptr));
        publish_cranelift_entry(*pending.target, bit_cast<FlatPtr>(func_ptr));
        publish_cranelift_osr_entry(*pending.target, bit_cast<FlatPtr>(func_ptr));
    }
}

static void retain_osr_compiled_function(PendingCompiledFunction&& pending, Module const& module)
{
    auto* handle = pending.mapping.leak_ptr();
    auto* function = static_cast<u8 const*>(handle->mapping);

    pending.target->cranelift_osr_code_start = bit_cast<FlatPtr>(function);
    pending.target->cranelift_osr_code_size = pending.code_size;
    pending.target->cranelift_osr_traps = handle->traps.data();
    pending.target->cranelift_osr_trap_count = handle->traps.size();
    module.retain_cranelift_code_handle(handle);
}

static size_t imported_function_count(Module const& module)
{
    size_t count = 0;
    for (auto const& import : module.import_section().imports()) {
        import.description().visit(
            [&](TypeIndex const& type_index) {
                auto const& types = module.type_section().types();
                if (type_index.value() < types.size() && types[type_index.value()].is_function())
                    ++count;
            },
            [&](FunctionType const&) { ++count; },
            [&](auto const&) {});
    }
    return count;
}

static void install_compiled_functions(Vector<PendingCompiledFunction>& pending_functions, Vector<PendingCompiledFunction>& pending_osr_functions, RuntimeHelperAddresses const& helper_addresses, Module const& module)
{
    HashMap<u32, FlatPtr> allocated_bytecode_targets;
    HashMap<u32, FlatPtr> direct_targets;
    auto function_index = imported_function_count(module);
    for (auto const& function : module.code_section().functions()) {
        auto const& compiled = function.func().body().compiled_instructions;
        if (auto native_entry = cranelift_native_entry_acquire(compiled); native_entry != 0)
            allocated_bytecode_targets.set(static_cast<u32>(function_index), native_entry);
        if (auto direct_native_entry = cranelift_direct_native_entry_acquire(compiled); direct_native_entry != 0)
            direct_targets.set(static_cast<u32>(function_index), direct_native_entry);
        ++function_index;
    }
    for (auto const& pending : pending_functions) {
        auto* native_entry = static_cast<u8*>(pending.mapping->mapping) + pending.native_entry_offset;
        auto& targets = pending.frontend == CraneliftFrontend::Direct ? direct_targets : allocated_bytecode_targets;
        targets.set(pending.function_index, bit_cast<FlatPtr>(native_entry));
    }

    for (auto& pending : pending_functions) {
        auto const& targets = pending.frontend == CraneliftFrontend::Direct ? direct_targets : allocated_bytecode_targets;
        if (!link_compiled_function(pending, helper_addresses, targets))
            return;
    }
    for (auto& pending : pending_functions) {
        if (!finalize_compiled_function(pending))
            return;
    }
    for (auto& pending : pending_osr_functions) {
        if (!link_compiled_function(pending, helper_addresses, direct_targets) || !finalize_compiled_function(pending))
            pending.mapping = nullptr;
    }

    Vector<FunctionIndex> published_functions;
    published_functions.ensure_capacity(pending_functions.size());
    for (auto& pending : pending_functions) {
        if (pending.mapping) {
            published_functions.unchecked_append(FunctionIndex { pending.function_index });
            publish_compiled_function(move(pending), module);
        }
    }
    for (auto& pending : pending_osr_functions) {
        if (pending.mapping)
            retain_osr_compiled_function(move(pending), module);
    }
    module.record_cranelift_publications(published_functions);
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

    if (callable->result_count <= 1) {
        auto parameter_count = callable->parameter_count;
        if (parameter_count > config.value_stack().size())
            return interpreter.set_trap(Trap::from_string("Insufficient arguments for indirect call"));

        auto arguments = config.value_stack().span().slice_from_end(parameter_count);
        config.value_stack().shrink(config.value_stack().size() - parameter_count);
        auto did_trap = wasm_cl_finish_call(interpreter, config, address, arguments.data(), arguments.size());
        if (!did_trap && callable->result_count != 0)
            config.value_stack().unchecked_append(config.compiled_call_result_scratch());
        return did_trap;
    }

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

static RuntimeHelperAddresses make_runtime_helper_addresses()
{
    RuntimeHelperAddresses addresses {};
    addresses[to_underlying(HelperId::call_function)] = bit_cast<uintptr_t>(&wasm_cl_call_function);
    addresses[to_underlying(HelperId::memory_size)] = bit_cast<uintptr_t>(&wasm_cl_memory_size);
    addresses[to_underlying(HelperId::memory_grow)] = bit_cast<uintptr_t>(&wasm_cl_memory_grow);
    addresses[to_underlying(HelperId::call_with_record)] = bit_cast<uintptr_t>(&wasm_cl_call_with_record);
    addresses[to_underlying(HelperId::direct_call_0)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_0);
    addresses[to_underlying(HelperId::direct_call_1)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_1);
    addresses[to_underlying(HelperId::direct_call_2)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_2);
    addresses[to_underlying(HelperId::direct_call_3)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_3);
    addresses[to_underlying(HelperId::call_indirect)] = bit_cast<uintptr_t>(&wasm_cl_call_indirect);
    addresses[to_underlying(HelperId::memory_copy)] = bit_cast<uintptr_t>(&wasm_cl_memory_copy);
    addresses[to_underlying(HelperId::memory_fill)] = bit_cast<uintptr_t>(&wasm_cl_memory_fill);
    addresses[to_underlying(HelperId::primitive_storage_cage_base)] = bit_cast<uintptr_t>(&js_primitive_storage_cage_base);
    addresses[to_underlying(HelperId::call_indirect_with_record)] = bit_cast<uintptr_t>(&wasm_cl_call_indirect_with_record);
    addresses[to_underlying(HelperId::stack_exhaustion)] = bit_cast<uintptr_t>(&wasm_cl_stack_exhaustion);
    addresses[to_underlying(HelperId::raise_trap)] = bit_cast<uintptr_t>(&wasm_cl_raise_trap);
    addresses[to_underlying(HelperId::check_indirect_type)] = bit_cast<uintptr_t>(&wasm_cl_check_indirect_type);
    addresses[to_underlying(HelperId::current_interpreter)] = bit_cast<uintptr_t>(&wasm_cl_current_interpreter);
    return addresses;
}

static RuntimeLayout make_runtime_layout()
{
    return RuntimeLayout {
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
        .compiled_instructions_direct_native_entry_offset = static_cast<u32>(offsetof(CompiledInstructions, cranelift_direct_native_entry)),
    };
}

static RuntimeLayout const& runtime_layout()
{
    static auto const layout = make_runtime_layout();
    return layout;
}

static u64 runtime_layout_hash()
{
    static auto const hash = compute_layout_hash(runtime_layout());
    return hash;
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

ByteString const& cranelift_compiler_path()
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

    return *s_path;
}

template<typename T>
static ErrorOr<T> read_cranelift_input(ReadonlyBytes input, size_t offset)
{
    Checked<size_t> end = offset;
    end += sizeof(T);
    if (end.has_overflow() || end.value() > input.size())
        return Error::from_string_literal("Cranelift input is truncated");

    T value;
    input.slice(offset, sizeof(T)).copy_to({ &value, sizeof(T) });
    return value;
}

static ErrorOr<Core::AnonymousBuffer> create_cranelift_output_buffer(ReadonlyBytes input)
{
    if (input.size() < sizeof(InputHeader))
        return Error::from_string_literal("Cranelift input header is truncated");

    auto header = TRY(read_cranelift_input<InputHeader>(input, 0));
    if (header.format_version != CRANELIFT_COMPILER_INPUT_FORMAT_VERSION)
        return Error::from_string_literal("Unsupported Cranelift compiler input format");
    if (header.total_size != input.size())
        return Error::from_string_literal("Cranelift input size does not match its header");

    Checked<size_t> entries_size = sizeof(InputFunctionEntry);
    entries_size *= header.function_count;
    if (entries_size.has_overflow())
        return Error::from_string_literal("Cranelift input entries are too large");

    Checked<size_t> entries_end = sizeof(InputHeader);
    entries_end += entries_size.value();
    if (entries_end.has_overflow() || entries_end.value() > input.size())
        return Error::from_string_literal("Cranelift input entries are truncated");

    auto function_types_offset = align_up(entries_end.value(), alignof(InputFunctionTypeEntry));
    Checked<size_t> function_types_size = sizeof(InputFunctionTypeEntry);
    function_types_size *= header.function_type_count;
    if (function_types_size.has_overflow())
        return Error::from_string_literal("Cranelift function type entries are too large");

    Checked<size_t> function_types_end = function_types_offset;
    function_types_end += function_types_size.value();
    if (function_types_end.has_overflow() || function_types_end.value() > input.size() || header.function_types_offset != function_types_offset)
        return Error::from_string_literal("Cranelift function type entries are not canonical");

    auto module_types_offset = align_up(function_types_end.value(), alignof(InputModuleTypeEntry));
    Checked<size_t> module_types_size = sizeof(InputModuleTypeEntry);
    module_types_size *= header.module_type_count;
    if (module_types_size.has_overflow())
        return Error::from_string_literal("Cranelift module type entries are too large");

    Checked<size_t> module_types_end = module_types_offset;
    module_types_end += module_types_size.value();
    if (module_types_end.has_overflow() || module_types_end.value() > input.size() || header.module_types_offset != module_types_offset)
        return Error::from_string_literal("Cranelift module type entries are not canonical");

    size_t instruction_count = 0;
    size_t direct_instruction_count = 0;
    size_t direct_osr_instruction_count = 0;
    size_t direct_branch_target_count = 0;
    size_t direct_local_count = 0;
    size_t direct_tier_up_checkpoint_count = 0;
    size_t direct_tier_up_live_local_index_count = 0;
    size_t locals_size = 0;
    size_t function_type_values_size = 0;
    size_t module_type_value_count = 0;
    for (size_t i = 0; i < header.function_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputFunctionEntry>(input, sizeof(InputHeader) + i * sizeof(InputFunctionEntry)));
        Checked<size_t> new_instruction_count = instruction_count;
        new_instruction_count += entry.insn_count;
        if (new_instruction_count.has_overflow())
            return Error::from_string_literal("Cranelift instruction count overflow");
        instruction_count = new_instruction_count.value();

        Checked<size_t> new_direct_instruction_count = direct_instruction_count;
        new_direct_instruction_count += entry.direct_insn_count;
        if (new_direct_instruction_count.has_overflow())
            return Error::from_string_literal("Cranelift direct instruction count overflow");
        direct_instruction_count = new_direct_instruction_count.value();

        Checked<size_t> new_direct_branch_target_count = direct_branch_target_count;
        new_direct_branch_target_count += entry.direct_branch_target_count;
        if (new_direct_branch_target_count.has_overflow())
            return Error::from_string_literal("Cranelift direct branch target count overflow");
        direct_branch_target_count = new_direct_branch_target_count.value();

        Checked<size_t> new_direct_local_count = direct_local_count;
        new_direct_local_count += entry.direct_local_count;
        if (new_direct_local_count.has_overflow())
            return Error::from_string_literal("Cranelift direct local count overflow");
        direct_local_count = new_direct_local_count.value();

        Checked<size_t> new_direct_tier_up_checkpoint_count = direct_tier_up_checkpoint_count;
        new_direct_tier_up_checkpoint_count += entry.direct_tier_up_checkpoint_count;
        if (new_direct_tier_up_checkpoint_count.has_overflow())
            return Error::from_string_literal("Cranelift direct tier-up checkpoint count overflow");
        direct_tier_up_checkpoint_count = new_direct_tier_up_checkpoint_count.value();

        if (entry.direct_tier_up_checkpoint_count != 0) {
            Checked<size_t> new_direct_osr_instruction_count = direct_osr_instruction_count;
            new_direct_osr_instruction_count += entry.direct_insn_count;
            if (new_direct_osr_instruction_count.has_overflow())
                return Error::from_string_literal("Cranelift direct OSR instruction count overflow");
            direct_osr_instruction_count = new_direct_osr_instruction_count.value();
        }

        Checked<size_t> new_direct_tier_up_live_local_index_count = direct_tier_up_live_local_index_count;
        new_direct_tier_up_live_local_index_count += entry.direct_tier_up_live_local_index_count;
        if (new_direct_tier_up_live_local_index_count.has_overflow())
            return Error::from_string_literal("Cranelift direct tier-up live-local count overflow");
        direct_tier_up_live_local_index_count = new_direct_tier_up_live_local_index_count.value();

        Checked<size_t> new_locals_size = locals_size;
        new_locals_size += entry.num_locals;
        if (new_locals_size.has_overflow())
            return Error::from_string_literal("Cranelift locals size overflow");
        locals_size = new_locals_size.value();
    }

    for (size_t i = 0; i < header.function_type_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputFunctionTypeEntry>(input, function_types_offset + i * sizeof(InputFunctionTypeEntry)));
        Checked<size_t> new_function_type_values_size = function_type_values_size;
        new_function_type_values_size += entry.parameter_count;
        new_function_type_values_size += entry.result_count;
        if (new_function_type_values_size.has_overflow())
            return Error::from_string_literal("Cranelift function type values are too large");
        function_type_values_size = new_function_type_values_size.value();
    }

    for (size_t i = 0; i < header.module_type_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputModuleTypeEntry>(input, module_types_offset + i * sizeof(InputModuleTypeEntry)));
        if (entry.is_function == 0) {
            if (entry.parameters_offset != 0 || entry.parameter_count != 0 || entry.results_offset != 0 || entry.result_count != 0)
                return Error::from_string_literal("Cranelift non-function module type is invalid");
            continue;
        }
        if (entry.is_function != 1)
            return Error::from_string_literal("Cranelift module type kind is invalid");
        Checked<size_t> new_module_type_value_count = module_type_value_count;
        new_module_type_value_count += entry.parameter_count;
        new_module_type_value_count += entry.result_count;
        if (new_module_type_value_count.has_overflow())
            return Error::from_string_literal("Cranelift module type values are too large");
        module_type_value_count = new_module_type_value_count.value();
    }

    auto insn_region_offset = align_up(module_types_end.value(), alignof(CraneliftInsn));
    Checked<size_t> insn_region_size = instruction_count;
    insn_region_size *= sizeof(CraneliftInsn);
    if (insn_region_size.has_overflow())
        return Error::from_string_literal("Cranelift instruction region is too large");

    Checked<size_t> direct_insn_region_offset = insn_region_offset;
    direct_insn_region_offset += insn_region_size.value();
    if (direct_insn_region_offset.has_overflow())
        return Error::from_string_literal("Cranelift direct instruction region offset overflow");
    auto aligned_direct_insn_region_offset = align_up(direct_insn_region_offset.value(), alignof(DirectInstruction));

    Checked<size_t> direct_insn_region_size = direct_instruction_count;
    direct_insn_region_size *= sizeof(DirectInstruction);
    if (direct_insn_region_size.has_overflow())
        return Error::from_string_literal("Cranelift direct instruction region is too large");

    Checked<size_t> direct_branch_targets_offset = aligned_direct_insn_region_offset;
    direct_branch_targets_offset += direct_insn_region_size.value();
    if (direct_branch_targets_offset.has_overflow())
        return Error::from_string_literal("Cranelift direct branch target region offset overflow");
    auto aligned_direct_branch_targets_offset = align_up(direct_branch_targets_offset.value(), alignof(u32));

    Checked<size_t> direct_branch_targets_size = direct_branch_target_count;
    direct_branch_targets_size *= sizeof(u32);
    if (direct_branch_targets_size.has_overflow())
        return Error::from_string_literal("Cranelift direct branch target region is too large");

    Checked<size_t> direct_locals_offset = aligned_direct_branch_targets_offset;
    direct_locals_offset += direct_branch_targets_size.value();
    if (direct_locals_offset.has_overflow())
        return Error::from_string_literal("Cranelift direct locals region offset overflow");
    auto aligned_direct_locals_offset = align_up(direct_locals_offset.value(), alignof(DirectValueType));

    Checked<size_t> direct_locals_size = direct_local_count;
    direct_locals_size *= sizeof(DirectValueType);
    if (direct_locals_size.has_overflow())
        return Error::from_string_literal("Cranelift direct locals region is too large");

    Checked<size_t> direct_tier_up_checkpoints_offset = aligned_direct_locals_offset;
    direct_tier_up_checkpoints_offset += direct_locals_size.value();
    if (direct_tier_up_checkpoints_offset.has_overflow())
        return Error::from_string_literal("Cranelift direct tier-up checkpoint region offset overflow");
    auto aligned_direct_tier_up_checkpoints_offset = align_up(direct_tier_up_checkpoints_offset.value(), alignof(DirectTierUpCheckpoint));

    Checked<size_t> direct_tier_up_checkpoints_size = direct_tier_up_checkpoint_count;
    direct_tier_up_checkpoints_size *= sizeof(DirectTierUpCheckpoint);
    if (direct_tier_up_checkpoints_size.has_overflow())
        return Error::from_string_literal("Cranelift direct tier-up checkpoint region is too large");

    Checked<size_t> direct_tier_up_live_local_indices_offset = aligned_direct_tier_up_checkpoints_offset;
    direct_tier_up_live_local_indices_offset += direct_tier_up_checkpoints_size.value();
    if (direct_tier_up_live_local_indices_offset.has_overflow())
        return Error::from_string_literal("Cranelift direct tier-up live-local region offset overflow");
    auto aligned_direct_tier_up_live_local_indices_offset = align_up(direct_tier_up_live_local_indices_offset.value(), alignof(u32));

    Checked<size_t> direct_tier_up_live_local_indices_size = direct_tier_up_live_local_index_count;
    direct_tier_up_live_local_indices_size *= sizeof(u32);
    if (direct_tier_up_live_local_indices_size.has_overflow())
        return Error::from_string_literal("Cranelift direct tier-up live-local region is too large");

    Checked<size_t> locals_region_offset = aligned_direct_tier_up_live_local_indices_offset;
    locals_region_offset += direct_tier_up_live_local_indices_size.value();
    if (locals_region_offset.has_overflow())
        return Error::from_string_literal("Cranelift locals region offset overflow");

    Checked<size_t> function_type_values_offset = locals_region_offset.value();
    function_type_values_offset += locals_size;
    if (function_type_values_offset.has_overflow())
        return Error::from_string_literal("Cranelift function type values offset overflow");

    Checked<size_t> module_type_values_offset = function_type_values_offset.value();
    module_type_values_offset += function_type_values_size;
    if (module_type_values_offset.has_overflow())
        return Error::from_string_literal("Cranelift module type values offset overflow");
    auto aligned_module_type_values_offset = align_up(module_type_values_offset.value(), alignof(DirectValueType));

    Checked<size_t> module_type_values_size = module_type_value_count;
    module_type_values_size *= sizeof(DirectValueType);
    if (module_type_values_size.has_overflow())
        return Error::from_string_literal("Cranelift module type values are too large");

    Checked<size_t> global_types_offset = aligned_module_type_values_offset;
    global_types_offset += module_type_values_size.value();
    if (global_types_offset.has_overflow())
        return Error::from_string_literal("Cranelift global types offset overflow");
    auto aligned_global_types_offset = align_up(global_types_offset.value(), alignof(DirectValueType));
    if (header.global_types_offset != aligned_global_types_offset)
        return Error::from_string_literal("Cranelift global types are not canonical");

    Checked<size_t> global_types_size = header.global_type_count;
    global_types_size *= sizeof(DirectValueType);
    if (global_types_size.has_overflow())
        return Error::from_string_literal("Cranelift global types are too large");

    Checked<size_t> layout_offset = aligned_global_types_offset;
    layout_offset += global_types_size.value();
    if (layout_offset.has_overflow())
        return Error::from_string_literal("Cranelift layout offset overflow");
    auto aligned_layout_offset = align_up(layout_offset.value(), alignof(RuntimeLayout));

    Checked<size_t> expected_input_size = aligned_layout_offset;
    expected_input_size += sizeof(RuntimeLayout);
    if (expected_input_size.has_overflow() || expected_input_size.value() != input.size() || header.layout_offset != aligned_layout_offset)
        return Error::from_string_literal("Cranelift input regions are not canonical");

    size_t insn_cursor = insn_region_offset;
    size_t direct_insn_cursor = aligned_direct_insn_region_offset;
    size_t direct_branch_targets_cursor = aligned_direct_branch_targets_offset;
    size_t direct_locals_cursor = aligned_direct_locals_offset;
    size_t direct_tier_up_checkpoints_cursor = aligned_direct_tier_up_checkpoints_offset;
    size_t direct_tier_up_live_local_indices_cursor = aligned_direct_tier_up_live_local_indices_offset;
    size_t locals_cursor = locals_region_offset.value();
    for (size_t i = 0; i < header.function_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputFunctionEntry>(input, sizeof(InputHeader) + i * sizeof(InputFunctionEntry)));
        if (entry.insn_offset != insn_cursor || entry.locals_offset != locals_cursor)
            return Error::from_string_literal("Cranelift input regions are not canonical");

        if ((entry.direct_insn_count == 0 && entry.direct_insn_offset != 0)
            || (entry.direct_insn_count != 0 && entry.direct_insn_offset != direct_insn_cursor)
            || (entry.direct_branch_target_count == 0 && entry.direct_branch_targets_offset != 0)
            || (entry.direct_branch_target_count != 0 && entry.direct_branch_targets_offset != direct_branch_targets_cursor)
            || (entry.direct_local_count == 0 && entry.direct_locals_offset != 0)
            || (entry.direct_local_count != 0 && entry.direct_locals_offset != direct_locals_cursor)
            || (entry.direct_tier_up_checkpoint_count == 0 && entry.direct_tier_up_checkpoints_offset != 0)
            || (entry.direct_tier_up_checkpoint_count != 0 && entry.direct_tier_up_checkpoints_offset != direct_tier_up_checkpoints_cursor)
            || (entry.direct_tier_up_live_local_index_count == 0 && entry.direct_tier_up_live_local_indices_offset != 0)
            || (entry.direct_tier_up_live_local_index_count != 0 && entry.direct_tier_up_live_local_indices_offset != direct_tier_up_live_local_indices_cursor))
            return Error::from_string_literal("Cranelift direct input regions are not canonical");

        insn_cursor += static_cast<size_t>(entry.insn_count) * sizeof(CraneliftInsn);
        direct_insn_cursor += static_cast<size_t>(entry.direct_insn_count) * sizeof(DirectInstruction);
        direct_branch_targets_cursor += static_cast<size_t>(entry.direct_branch_target_count) * sizeof(u32);
        direct_locals_cursor += static_cast<size_t>(entry.direct_local_count) * sizeof(DirectValueType);
        direct_tier_up_checkpoints_cursor += static_cast<size_t>(entry.direct_tier_up_checkpoint_count) * sizeof(DirectTierUpCheckpoint);
        direct_tier_up_live_local_indices_cursor += static_cast<size_t>(entry.direct_tier_up_live_local_index_count) * sizeof(u32);
        locals_cursor += entry.num_locals;
    }

    size_t function_type_values_cursor = function_type_values_offset.value();
    for (size_t i = 0; i < header.function_type_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputFunctionTypeEntry>(input, function_types_offset + i * sizeof(InputFunctionTypeEntry)));
        if (entry.parameters_offset != function_type_values_cursor)
            return Error::from_string_literal("Cranelift function type values are not canonical");
        function_type_values_cursor += entry.parameter_count;
        if (entry.results_offset != function_type_values_cursor)
            return Error::from_string_literal("Cranelift function type values are not canonical");
        function_type_values_cursor += entry.result_count;
    }
    if (align_up(function_type_values_cursor, alignof(DirectValueType)) != aligned_module_type_values_offset)
        return Error::from_string_literal("Cranelift function type values are not canonical");

    size_t module_type_values_cursor = aligned_module_type_values_offset;
    for (size_t i = 0; i < header.module_type_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputModuleTypeEntry>(input, module_types_offset + i * sizeof(InputModuleTypeEntry)));
        if (entry.is_function == 0)
            continue;
        if (entry.parameters_offset != module_type_values_cursor)
            return Error::from_string_literal("Cranelift module type values are not canonical");
        module_type_values_cursor += static_cast<size_t>(entry.parameter_count) * sizeof(DirectValueType);
        if (entry.results_offset != module_type_values_cursor)
            return Error::from_string_literal("Cranelift module type values are not canonical");
        module_type_values_cursor += static_cast<size_t>(entry.result_count) * sizeof(DirectValueType);
    }
    if (align_up(module_type_values_cursor, alignof(DirectValueType)) != aligned_global_types_offset)
        return Error::from_string_literal("Cranelift module type values are not canonical");

    Checked<size_t> compiler_instruction_count = instruction_count;
    compiler_instruction_count += direct_instruction_count;
    compiler_instruction_count += direct_osr_instruction_count;
    if (compiler_instruction_count.has_overflow())
        return Error::from_string_literal("Cranelift compiler instruction count overflow");
    auto output_size = TRY(compute_output_buffer_size(header.function_count, compiler_instruction_count.value()));
    if (header.output_size != output_size)
        return Error::from_string_literal("Cranelift output size does not match its input");

    return Core::AnonymousBuffer::create_with_size(output_size, Core::AnonymousBuffer::Sealability::Sealable);
}

static ErrorOr<Core::AnonymousBuffer> finalize_cranelift_output_buffer(Core::AnonymousBuffer const& output)
{
    if (output.size() < sizeof(OutputHeader))
        return Error::from_string_literal("Cranelift output header is truncated");

    auto const& header = *output.data<OutputHeader>();
    auto output_size = static_cast<size_t>(header.total_size);
    if (output_size < sizeof(OutputHeader) || output_size > output.size())
        return Error::from_string_literal("Cranelift output size is invalid");

    auto output_fd = TRY(Core::System::dup(output.fd()));
    return Core::AnonymousBuffer::create_from_anon_fd(output_fd, output_size);
}

ErrorOr<Core::AnonymousBuffer> compile_cranelift_buffer(Core::AnonymousBuffer const& input)
{
    auto output = TRY(create_cranelift_output_buffer(input.bytes()));

#if defined(AK_OS_WINDOWS)
    auto process = TRY([&]() -> ErrorOr<Core::Process> {
        // FIXME: Use Core::FileAction::DupFd once it is supported on Windows so spawning does not require temporarily
        //        inheritable handles.
        static Sync::Mutex spawn_mutex;
        Sync::MutexLocker locker(spawn_mutex);

        auto inherited_input_fd = TRY(Core::System::dup(input.fd()));
        ScopeGuard close_inherited_input_fd { [&]() { (void)Core::System::close(inherited_input_fd); } };
        TRY(Core::System::set_close_on_exec(inherited_input_fd, false));

        auto inherited_output_fd = TRY(Core::System::dup(output.fd()));
        ScopeGuard close_inherited_output_fd { [&]() { (void)Core::System::close(inherited_output_fd); } };
        TRY(Core::System::set_close_on_exec(inherited_output_fd, false));

        Vector<ByteString> arguments;
        arguments.append(ByteString::number(reinterpret_cast<uintptr_t>(to_handle(inherited_input_fd))));
        arguments.append(ByteString::number(input.size()));
        arguments.append(ByteString::number(reinterpret_cast<uintptr_t>(to_handle(inherited_output_fd))));
        arguments.append(ByteString::number(output.size()));

        return Core::Process::spawn({
            .name = "cranelift-compiler"sv,
            .executable = cranelift_compiler_path(),
            .die_with_parent = true,
            .arguments = arguments,
        });
    }());
#else
    // Reserve the child descriptor numbers with close-on-exec duplicates. This prevents concurrent spawns from claiming
    // or inheriting them before the child-side DupFd actions replace them and clear close-on-exec.
    auto compiler_input_fd = TRY(Core::System::fcntl(input.fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
    ScopeGuard close_compiler_input_fd { [&]() { (void)Core::System::close(compiler_input_fd); } };

    auto compiler_output_fd = TRY(Core::System::fcntl(output.fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
    ScopeGuard close_compiler_output_fd { [&]() { (void)Core::System::close(compiler_output_fd); } };

    Vector<ByteString> arguments;
    arguments.append(ByteString::number(compiler_input_fd));
    arguments.append(ByteString::number(input.size()));
    arguments.append(ByteString::number(compiler_output_fd));
    arguments.append(ByteString::number(output.size()));

    auto process = TRY(Core::Process::spawn({
        .name = "cranelift-compiler"sv,
        .executable = cranelift_compiler_path(),
        .die_with_parent = true,
        .arguments = arguments,
        .file_actions = {
            Core::FileAction::DupFd { .write_fd = input.fd(), .fd = compiler_input_fd },
            Core::FileAction::DupFd { .write_fd = output.fd(), .fd = compiler_output_fd },
        },
    }));
#endif

    if (TRY(process.wait_for_termination()) != 0)
        return Error::from_string_literal("Failed to compile a WebAssembly module");

    return finalize_cranelift_output_buffer(output);
}

static ErrorOr<void> try_cranelift_compile_batch(ReadonlySpan<BatchInput> batch, Module const& module)
{
    if (batch.is_empty())
        return {};

    static auto helper_addresses = make_runtime_helper_addresses();
    auto const& layout = runtime_layout();
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

    Vector<FunctionType const*> module_function_types;
    module_function_types.ensure_capacity(types.size());
    for (auto const& type : types)
        module_function_types.unchecked_append(type.is_function() ? &type.function() : nullptr);

    Vector<DirectValueType> global_types;
    for (auto const& import : module.import_section().imports()) {
        import.description().visit(
            [&](GlobalType const& global_type) {
                global_types.append(serialize_direct_value_type(global_type.type()));
            },
            [&](auto const&) {});
    }
    for (auto const& global : module.global_section().entries())
        global_types.append(serialize_direct_value_type(global.type().type()));

    auto const function_types_offset = align_up(entries_offset + entries_size, alignof(InputFunctionTypeEntry));
    auto const function_types_size = sizeof(InputFunctionTypeEntry) * function_types.size();
    auto const module_types_offset = align_up(function_types_offset + function_types_size, alignof(InputModuleTypeEntry));
    auto const module_types_size = sizeof(InputModuleTypeEntry) * module_function_types.size();

    size_t total_insn_count = 0;
    size_t total_direct_insn_count = 0;
    size_t total_direct_osr_insn_count = 0;
    size_t total_direct_branch_target_count = 0;
    size_t total_direct_local_count = 0;
    size_t total_direct_tier_up_checkpoint_count = 0;
    size_t total_direct_tier_up_live_local_index_count = 0;
    size_t total_locals_bytes = 0;
    size_t total_function_type_bytes = 0;
    size_t total_module_type_value_count = 0;
    for (auto const& entry : batch) {
        total_insn_count += entry.insns.size();
        if (entry.direct_input.has_value()) {
            total_direct_insn_count += entry.direct_input->instructions.size();
            if (!entry.direct_input->tier_up_checkpoints.is_empty())
                total_direct_osr_insn_count += entry.direct_input->instructions.size();
            total_direct_branch_target_count += entry.direct_input->branch_targets.size();
            total_direct_local_count += entry.direct_input->local_types.size();
            total_direct_tier_up_checkpoint_count += entry.direct_input->tier_up_checkpoints.size();
            total_direct_tier_up_live_local_index_count += entry.direct_input->tier_up_live_local_indices.size();
        }
        total_locals_bytes += entry.num_locals;
    }
    for (auto const* function_type : function_types)
        total_function_type_bytes += function_type->parameters().size() + function_type->results().size();
    for (auto const* function_type : module_function_types) {
        if (function_type)
            total_module_type_value_count += function_type->parameters().size() + function_type->results().size();
    }

    auto const insn_region_offset = align_up(module_types_offset + module_types_size, alignof(CraneliftInsn));
    auto const insn_bytes = total_insn_count * sizeof(CraneliftInsn);
    auto const direct_insn_region_offset = align_up(insn_region_offset + insn_bytes, alignof(DirectInstruction));
    auto const direct_insn_bytes = total_direct_insn_count * sizeof(DirectInstruction);
    auto const direct_branch_targets_offset = align_up(direct_insn_region_offset + direct_insn_bytes, alignof(u32));
    auto const direct_branch_targets_bytes = total_direct_branch_target_count * sizeof(u32);
    auto const direct_locals_offset = align_up(direct_branch_targets_offset + direct_branch_targets_bytes, alignof(DirectValueType));
    auto const direct_locals_bytes = total_direct_local_count * sizeof(DirectValueType);
    auto const direct_tier_up_checkpoints_offset = align_up(direct_locals_offset + direct_locals_bytes, alignof(DirectTierUpCheckpoint));
    auto const direct_tier_up_checkpoints_bytes = total_direct_tier_up_checkpoint_count * sizeof(DirectTierUpCheckpoint);
    auto const direct_tier_up_live_local_indices_offset = align_up(direct_tier_up_checkpoints_offset + direct_tier_up_checkpoints_bytes, alignof(u32));
    auto const direct_tier_up_live_local_indices_bytes = total_direct_tier_up_live_local_index_count * sizeof(u32);
    auto const locals_region_offset = direct_tier_up_live_local_indices_offset + direct_tier_up_live_local_indices_bytes; // u8, no alignment needed
    auto const function_type_values_offset = locals_region_offset + total_locals_bytes;                                   // u8, no alignment needed
    auto const module_type_values_offset = align_up(function_type_values_offset + total_function_type_bytes, alignof(DirectValueType));
    auto const module_type_values_bytes = total_module_type_value_count * sizeof(DirectValueType);
    auto const global_types_offset = align_up(module_type_values_offset + module_type_values_bytes, alignof(DirectValueType));
    auto const global_types_bytes = global_types.size() * sizeof(DirectValueType);
    auto const layout_offset = align_up(global_types_offset + global_types_bytes, alignof(RuntimeLayout));
    auto const total_size = layout_offset + sizeof(RuntimeLayout);
    auto const total_compiler_instruction_count = total_insn_count + total_direct_insn_count + total_direct_osr_insn_count;
    auto const output_size = TRY(compute_output_buffer_size(function_count, total_compiler_instruction_count));

    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(total_size, Core::AnonymousBuffer::Sealability::Sealable));
    auto* base = buffer.data<u8>();
    __builtin_memset(base, 0, total_size);

    auto* header = reinterpret_cast<InputHeader*>(base);
    *header = InputHeader {
        .format_version = CRANELIFT_COMPILER_INPUT_FORMAT_VERSION,
        .function_count = static_cast<u32>(function_count),
        .function_type_count = static_cast<u32>(function_types.size()),
        .function_types_offset = static_cast<u32>(function_types_offset),
        .module_type_count = static_cast<u32>(module_function_types.size()),
        .module_types_offset = static_cast<u32>(module_types_offset),
        .global_type_count = static_cast<u32>(global_types.size()),
        .global_types_offset = static_cast<u32>(global_types_offset),
        .layout_offset = static_cast<u32>(layout_offset),
        .output_size = output_size,
        .total_size = total_size,
    };

    size_t insn_cursor = insn_region_offset;
    size_t direct_insn_cursor = direct_insn_region_offset;
    size_t direct_branch_targets_cursor = direct_branch_targets_offset;
    size_t direct_locals_cursor = direct_locals_offset;
    size_t direct_tier_up_checkpoints_cursor = direct_tier_up_checkpoints_offset;
    size_t direct_tier_up_live_local_indices_cursor = direct_tier_up_live_local_indices_offset;
    size_t locals_cursor = locals_region_offset;
    for (size_t i = 0; i < function_count; ++i) {
        auto const& input = batch[i];
        u32 function_direct_insn_offset = 0;
        u32 function_direct_insn_count = 0;
        u32 function_direct_branch_targets_offset = 0;
        u32 function_direct_branch_target_count = 0;
        u32 function_direct_locals_offset = 0;
        u32 function_direct_local_count = 0;
        u32 function_direct_tier_up_checkpoints_offset = 0;
        u32 function_direct_tier_up_checkpoint_count = 0;
        u32 function_direct_tier_up_live_local_indices_offset = 0;
        u32 function_direct_tier_up_live_local_index_count = 0;
        if (input.direct_input.has_value()) {
            auto const& direct_input = input.direct_input.value();
            function_direct_insn_offset = direct_input.instructions.is_empty() ? 0 : static_cast<u32>(direct_insn_cursor);
            function_direct_insn_count = static_cast<u32>(direct_input.instructions.size());
            function_direct_branch_targets_offset = direct_input.branch_targets.is_empty() ? 0 : static_cast<u32>(direct_branch_targets_cursor);
            function_direct_branch_target_count = static_cast<u32>(direct_input.branch_targets.size());
            function_direct_locals_offset = direct_input.local_types.is_empty() ? 0 : static_cast<u32>(direct_locals_cursor);
            function_direct_local_count = static_cast<u32>(direct_input.local_types.size());
            function_direct_tier_up_checkpoints_offset = direct_input.tier_up_checkpoints.is_empty() ? 0 : static_cast<u32>(direct_tier_up_checkpoints_cursor);
            function_direct_tier_up_checkpoint_count = static_cast<u32>(direct_input.tier_up_checkpoints.size());
            function_direct_tier_up_live_local_indices_offset = direct_input.tier_up_live_local_indices.is_empty() ? 0 : static_cast<u32>(direct_tier_up_live_local_indices_cursor);
            function_direct_tier_up_live_local_index_count = static_cast<u32>(direct_input.tier_up_live_local_indices.size());

            if (!direct_input.instructions.is_empty())
                __builtin_memcpy(base + direct_insn_cursor, direct_input.instructions.data(), direct_input.instructions.size() * sizeof(DirectInstruction));
            direct_insn_cursor += direct_input.instructions.size() * sizeof(DirectInstruction);
            if (!direct_input.branch_targets.is_empty())
                __builtin_memcpy(base + direct_branch_targets_cursor, direct_input.branch_targets.data(), direct_input.branch_targets.size() * sizeof(u32));
            direct_branch_targets_cursor += direct_input.branch_targets.size() * sizeof(u32);
            if (!direct_input.local_types.is_empty())
                __builtin_memcpy(base + direct_locals_cursor, direct_input.local_types.data(), direct_input.local_types.size() * sizeof(DirectValueType));
            direct_locals_cursor += direct_input.local_types.size() * sizeof(DirectValueType);
            if (!direct_input.tier_up_checkpoints.is_empty())
                __builtin_memcpy(base + direct_tier_up_checkpoints_cursor, direct_input.tier_up_checkpoints.data(), direct_input.tier_up_checkpoints.size() * sizeof(DirectTierUpCheckpoint));
            direct_tier_up_checkpoints_cursor += direct_input.tier_up_checkpoints.size() * sizeof(DirectTierUpCheckpoint);
            if (!direct_input.tier_up_live_local_indices.is_empty())
                __builtin_memcpy(base + direct_tier_up_live_local_indices_cursor, direct_input.tier_up_live_local_indices.data(), direct_input.tier_up_live_local_indices.size() * sizeof(u32));
            direct_tier_up_live_local_indices_cursor += direct_input.tier_up_live_local_indices.size() * sizeof(u32);
        }

        auto* entry = reinterpret_cast<InputFunctionEntry*>(base + entries_offset + i * sizeof(InputFunctionEntry));
        *entry = InputFunctionEntry {
            .preferred_frontend = static_cast<u32>(input.preferred_frontend),
            .insn_offset = static_cast<u32>(insn_cursor),
            .insn_count = static_cast<u32>(input.insns.size()),
            .direct_insn_offset = function_direct_insn_offset,
            .direct_insn_count = function_direct_insn_count,
            .direct_branch_targets_offset = function_direct_branch_targets_offset,
            .direct_branch_target_count = function_direct_branch_target_count,
            .direct_locals_offset = function_direct_locals_offset,
            .direct_local_count = function_direct_local_count,
            .direct_tier_up_checkpoints_offset = function_direct_tier_up_checkpoints_offset,
            .direct_tier_up_checkpoint_count = function_direct_tier_up_checkpoint_count,
            .direct_tier_up_live_local_indices_offset = function_direct_tier_up_live_local_indices_offset,
            .direct_tier_up_live_local_index_count = function_direct_tier_up_live_local_index_count,
            .result_arity = input.result_arity,
            .num_locals = input.num_locals,
            .direct_num_locals = input.direct_num_locals,
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

    size_t module_type_values_cursor = module_type_values_offset;
    for (size_t i = 0; i < module_function_types.size(); ++i) {
        auto* entry = reinterpret_cast<InputModuleTypeEntry*>(base + module_types_offset + i * sizeof(InputModuleTypeEntry));
        auto const* function_type = module_function_types[i];
        if (!function_type) {
            *entry = InputModuleTypeEntry {};
            continue;
        }
        *entry = InputModuleTypeEntry {
            .is_function = 1,
            .parameters_offset = static_cast<u32>(module_type_values_cursor),
            .parameter_count = static_cast<u32>(function_type->parameters().size()),
            .results_offset = static_cast<u32>(module_type_values_cursor + function_type->parameters().size() * sizeof(DirectValueType)),
            .result_count = static_cast<u32>(function_type->results().size()),
        };
        for (auto const& parameter : function_type->parameters()) {
            *reinterpret_cast<DirectValueType*>(base + module_type_values_cursor) = serialize_direct_value_type(parameter);
            module_type_values_cursor += sizeof(DirectValueType);
        }
        for (auto const& result : function_type->results()) {
            *reinterpret_cast<DirectValueType*>(base + module_type_values_cursor) = serialize_direct_value_type(result);
            module_type_values_cursor += sizeof(DirectValueType);
        }
    }

    if (!global_types.is_empty())
        __builtin_memcpy(base + global_types_offset, global_types.data(), global_types_bytes);

    __builtin_memcpy(base + layout_offset, &layout, sizeof(layout));

    dbgln("Cranelift: submitting batch of {} functions ({} instructions)", function_count, total_compiler_instruction_count);
    auto output_buffer = TRY([&]() -> ErrorOr<Core::AnonymousBuffer> {
        if (auto& callback = cranelift_compile_callback()) {
            auto output = callback(buffer);
            return output.snapshot();
        }
        return compile_cranelift_buffer(buffer);
    }());
    if (getenv("LADYBIRD_CRANELIFT_SIMULATE_SLOW_COMPILATION")) {
        dbgln("Cranelift: simulating 3 seconds of compilation latency");
        (void)Core::System::sleep_ms(3'000);
    }
    if (!output_buffer.is_valid() || output_buffer.size() < sizeof(OutputHeader))
        return Error::from_string_literal("Failed to compile a WebAssembly module");
    dbgln("Cranelift: received batch of {} functions ({} instructions)", function_count, total_compiler_instruction_count);

    // Extract results for each function.
    auto const* output_base = output_buffer.data<u8>();
    auto const& output_header = *reinterpret_cast<OutputHeader const*>(output_base);
    auto const output_entries_offset = sizeof(OutputHeader);
    auto const output_entries_size = sizeof(OutputFunctionEntry) * function_count;
    auto const code_base_offset = static_cast<size_t>(output_header.code_base_offset);
    auto const reloc_region_start = static_cast<size_t>(output_header.reloc_region_start);
    auto const compact_output_size = static_cast<size_t>(output_header.total_size);
    if (output_header.function_count != function_count
        || compact_output_size != output_buffer.size()
        || code_base_offset < output_entries_offset + output_entries_size
        || code_base_offset % SERIALIZED_CODE_ALIGNMENT != 0
        || reloc_region_start < code_base_offset
        || reloc_region_start % alignof(CraneliftRelocation) != 0
        || reloc_region_start > compact_output_size)
        return Error::from_string_literal("Cranelift compiler returned an invalid buffer");

    auto const code_region_size = reloc_region_start - code_base_offset;
    auto const reloc_region_size = compact_output_size - reloc_region_start;
    CompilerOutputRegions output_regions {
        .base = output_base,
        .total_size = compact_output_size,
        .code_base_offset = code_base_offset,
        .code_region_size = code_region_size,
        .reloc_region_start = reloc_region_start,
        .reloc_region_size = reloc_region_size,
    };
    Vector<PendingCompiledFunction> pending_functions;
    Vector<PendingCompiledFunction> pending_osr_functions;

    for (size_t i = 0; i < function_count; ++i) {
        auto const& output = *reinterpret_cast<OutputFunctionEntry const*>(output_base + output_entries_offset + i * sizeof(OutputFunctionEntry));
        if (!output.clean.compiled)
            continue;
        if (output.frontend > static_cast<u32>(CraneliftFrontend::Direct))
            continue;
        auto const frontend = static_cast<CraneliftFrontend>(output.frontend);

        auto clean = output_regions.read_artifact(output.clean);
        if (!clean.has_value())
            continue;

        auto& capture = cranelift_cache_state().cache_capture;
        if (capture.capturing && batch[i].function_index != NumericLimits<u32>::max()) {
            if (auto copy = ByteBuffer::copy(clean->code.data(), clean->code.size()); !copy.is_error()) {
                CacheRecord rec;
                rec.function_index = batch[i].function_index;
                rec.native_entry_offset = clean->native_entry_offset;
                rec.frontend = frontend;
                rec.unpatched_code = copy.release_value();
                rec.relocs.ensure_capacity(clean->relocs.size());
                for (auto const& reloc : clean->relocs)
                    rec.relocs.unchecked_append(reloc);
                rec.traps.ensure_capacity(clean->traps.size());
                for (auto const& trap : clean->traps)
                    rec.traps.unchecked_append(trap);
                capture.records.append(move(rec));
            }
        }

        auto pending_clean = prepare_compiled_function(batch[i].function_index, *batch[i].target, frontend, clean->code, clean->native_entry_offset, clean->relocs, clean->traps);
        if (!pending_clean.has_value())
            continue;
        pending_functions.append(pending_clean.release_value());

        if (frontend != CraneliftFrontend::Direct || !output.osr.compiled)
            continue;
        auto osr = output_regions.read_artifact(output.osr);
        if (!osr.has_value())
            continue;
        if (auto pending = prepare_compiled_function(batch[i].function_index, *batch[i].target, frontend, osr->code, osr->native_entry_offset, osr->relocs, osr->traps); pending.has_value())
            pending_osr_functions.append(pending.release_value());
    }

    install_compiled_functions(pending_functions, pending_osr_functions, helper_addresses, module);
    return {};
}

bool try_cranelift_compile(CodeSection::Func const& function, u32 result_arity)
{
#if !WASM_COMPILED_FAULT_RECOVERY_SUPPORTED
    (void)function;
    (void)result_arity;
    return false;
#else
    auto& compiled = function.body().compiled_instructions;
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

    // Cache hit: associate the parsed record with the dispatch table that was just populated by
    // try_compile_instructions. The records are installed together by flush_cranelift_batch so
    // cross-function relocations resolve directly.
    if (cranelift_cache_state().pending_install.active && s_active_function_index != NumericLimits<u32>::max()) {
        auto record = cranelift_cache_state().pending_install.records.take(s_active_function_index);
        if (record.has_value()) {
            cranelift_cache_state().pending_install.functions.append({ s_active_function_index, &compiled, record.release_value() });
            return false;
        }
    }

    auto direct_input = serialize_direct_compiler_input(function);
    auto preferred_frontend = direct_input.has_value() ? CraneliftFrontend::Direct : CraneliftFrontend::AllocatedBytecode;

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

    u32 direct_num_locals = 0;
    if (direct_input.has_value()) {
        Checked<u32> direct_local_count = compiled.cranelift_param_count;
        direct_local_count += direct_input->local_types.size();
        if (direct_local_count.has_overflow())
            return false;
        direct_num_locals = direct_local_count.value();
    }

    cranelift_cache_state().pending_batch.append({
        move(flat),
        move(direct_input),
        preferred_frontend,
        result_arity,
        s_active_function_index,
        &compiled,
        compiled.cranelift_local_count,
        direct_num_locals,
        compiled.cranelift_param_count,
    });
    return false; // Not compiled yet, will be compiled in flush.
#endif
}

static bool is_native_direct_call_opcode(u32 opcode)
{
    return opcode == Instructions::call.value()
        || (opcode >= Instructions::synthetic_call_00.value() && opcode <= Instructions::synthetic_call_31.value())
        || opcode == Instructions::synthetic_call_with_record_0.value()
        || opcode == Instructions::synthetic_call_with_record_1.value();
}

static Optional<FunctionIndex> native_direct_call_target(CraneliftInsn const& instruction)
{
    if (!is_native_direct_call_opcode(instruction.opcode))
        return {};
    if (instruction.imm1 < 0 || !AK::is_within_range<u32>(instruction.imm1))
        return {};
    return FunctionIndex { static_cast<u32>(instruction.imm1) };
}

static Optional<FunctionIndex> native_direct_call_target(DirectInstruction const& instruction)
{
    if (instruction.opcode != Instructions::call.value())
        return {};
    return FunctionIndex { instruction.arguments.function_index };
}

// A component is keyed by the function index of the node it was discovered from.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, ComponentKey);

struct CompilationComponent {
    Vector<BatchInput> inputs;
    OrderedHashTable<ComponentKey> dependencies;
    OrderedHashTable<ComponentKey> dependents;
    size_t instruction_count { 0 };
};

class CompilationCallGraph {
public:
    explicit CompilationCallGraph(Vector<BatchInput>& inputs)
    {
        m_nodes.ensure_capacity(inputs.size());

        // The graph takes ownership of the inputs; they are handed back grouped into components.
        for (auto& input : inputs) {
            FunctionIndex function_index { input.function_index };
            auto& new_node = m_nodes.ensure(function_index);
            new_node.input = move(input);
        }

        // Record callee edges for direct calls within the batch; calls that leave it don't constrain ordering.
        for (auto& entry : m_nodes) {
            auto& current_node = entry.value;
            for (auto const& instruction : current_node.input.insns) {
                auto target = native_direct_call_target(instruction);
                if (!target.has_value())
                    continue;

                auto callee = target.value();
                if (m_nodes.contains(callee))
                    current_node.callees.set(callee);
            }
            if (current_node.input.direct_input.has_value()) {
                for (auto const& instruction : current_node.input.direct_input->instructions) {
                    auto target = native_direct_call_target(instruction);
                    if (!target.has_value())
                        continue;

                    auto callee = target.value();
                    if (m_nodes.contains(callee))
                        current_node.callees.set(callee);
                }
            }
        }

        // Mirror callee edges into caller edges: component discovery walks the transposed graph.
        for (auto& entry : m_nodes) {
            auto function_index = entry.key;
            for (auto callee : entry.value.callees) {
                auto& callee_node = node(callee);
                callee_node.callers.set(function_index);
            }
        }
    }

    // Kosaraju's algorithm: peel components off in reverse finishing order by walking the transposed (caller-edge) graph.
    OrderedHashMap<ComponentKey, CompilationComponent> strongly_connected_components()
    {
        auto finishing_order = depth_first_finishing_order();

        OrderedHashMap<ComponentKey, CompilationComponent> components;
        components.ensure_capacity(m_nodes.size());

        for (auto root : finishing_order.in_reverse()) {
            auto const& root_node = node(root);
            if (root_node.component.has_value())
                continue;

            ComponentKey const component_key { root.value() };
            auto& component = components.ensure(component_key);
            append_component(root, component_key, component);
        }

        connect_components(components);
        return components;
    }

private:
    struct Node {
        BatchInput input;
        OrderedHashTable<FunctionIndex> callees;
        OrderedHashTable<FunctionIndex> callers;
        Optional<ComponentKey> component;
    };

    enum class TraversalPhase : u8 {
        Enter,
        Leave,
    };

    struct TraversalStep {
        FunctionIndex function_index;
        TraversalPhase phase;
    };

    Node& node(FunctionIndex function_index)
    {
        return m_nodes.get(function_index).value();
    }

    Node const& node(FunctionIndex function_index) const
    {
        return m_nodes.get(function_index).value();
    }

    Vector<FunctionIndex> depth_first_finishing_order() const
    {
        HashTable<FunctionIndex> visited;
        visited.ensure_capacity(m_nodes.size());

        Vector<FunctionIndex> finishing_order;
        finishing_order.ensure_capacity(m_nodes.size());

        for (auto const& entry : m_nodes) {
            if (visited.contains(entry.key))
                continue;
            append_depth_first_order(entry.key, visited, finishing_order);
        }
        return finishing_order;
    }

    void append_depth_first_order(FunctionIndex root, HashTable<FunctionIndex>& visited, Vector<FunctionIndex>& finishing_order) const
    {
        Vector<TraversalStep> work_list;
        work_list.append({ root, TraversalPhase::Enter });

        while (!work_list.is_empty()) {
            auto step = work_list.take_last();
            if (step.phase == TraversalPhase::Leave) {
                finishing_order.append(step.function_index);
                continue;
            }

            if (visited.contains(step.function_index))
                continue;

            visited.set(step.function_index);
            work_list.append({ step.function_index, TraversalPhase::Leave });

            auto const& current_node = node(step.function_index);

            // Reversed so callees are visited in their original order despite the LIFO work list.
            for (auto callee : current_node.callees.in_reverse()) {
                if (!visited.contains(callee))
                    work_list.append({ callee, TraversalPhase::Enter });
            }
        }
    }

    void append_component(FunctionIndex root, ComponentKey component_key, CompilationComponent& component)
    {
        Vector<FunctionIndex> work_list;
        auto& root_node = node(root);
        root_node.component = component_key;
        work_list.append(root);

        while (!work_list.is_empty()) {
            auto function_index = work_list.take_last();
            auto& current_node = node(function_index);

            component.instruction_count += compiler_instruction_count(current_node.input);
            component.inputs.append(move(current_node.input));

            for (auto caller : current_node.callers) {
                auto& caller_node = node(caller);
                if (caller_node.component.has_value())
                    continue;

                caller_node.component = component_key;
                work_list.append(caller);
            }
        }
    }

    void connect_components(OrderedHashMap<ComponentKey, CompilationComponent>& components) const
    {
        // A call into another component makes that component a compilation dependency.
        for (auto const& entry : m_nodes) {
            auto const& current_node = entry.value;
            auto component_key = current_node.component.value();
            auto& component = components.get(component_key).value();

            for (auto callee : current_node.callees) {
                auto const& callee_node = node(callee);
                auto callee_component = callee_node.component.value();
                if (callee_component != component_key)
                    component.dependencies.set(callee_component);
            }
        }

        // Mirror dependency edges into dependent edges.
        for (auto& entry : components) {
            auto& component = entry.value;
            for (auto dependency : component.dependencies) {
                auto& dependency_component = components.get(dependency).value();
                dependency_component.dependents.set(entry.key);
            }
        }
    }

    OrderedHashMap<FunctionIndex, Node> m_nodes;
};

// Plans batches so that every function is compiled after the functions it calls. Mutual recursion permits
// no such order, so the planning unit is a whole strongly connected component of the call graph.
// Batches are drawn in dependency order using Kahn's algorithm, consuming edges as components are scheduled.
class IncrementalCompilationPlan {
public:
    explicit IncrementalCompilationPlan(Vector<BatchInput>& inputs)
    {
        CompilationCallGraph call_graph { inputs };
        m_components = call_graph.strongly_connected_components();
        initialize_ready_components();
    }

    // The component graph is acyclic, so the ready queue only runs dry once every component has been scheduled.
    bool is_complete() const { return m_ready_components.is_empty(); }

    Vector<BatchInput> take_batch()
    {
        auto selection = select_components();

        Vector<BatchInput> batch;
        batch.ensure_capacity(selection.function_count);

        for (auto component_key : selection.components) {
            auto& component = m_components.get(component_key).value();
            for (auto& input : component.inputs)
                batch.unchecked_append(move(input));
        }

        return batch;
    }

private:
    static constexpr size_t MAXIMUM_BATCH_FUNCTION_COUNT = 512;
    static constexpr size_t MAXIMUM_BATCH_INSTRUCTION_COUNT = 100'000;

    struct BatchSelection {
        Vector<ComponentKey> components;
        size_t function_count { 0 };
        size_t instruction_count { 0 };
    };

    void initialize_ready_components()
    {
        for (auto const& entry : m_components) {
            if (entry.value.dependencies.is_empty())
                m_ready_components.enqueue(entry.key);
        }
    }

    static bool exceeds_limit(size_t current, size_t addition, size_t maximum)
    {
        Checked<size_t> total = current;
        total += addition;
        return total.has_overflow() || total.value() > maximum;
    }

    bool fits_in_batch(CompilationComponent const& component, BatchSelection const& selection) const
    {
        // An oversized component still has to compile, so it gets a batch of its own.
        if (selection.components.is_empty())
            return true;

        if (exceeds_limit(selection.function_count, component.inputs.size(), MAXIMUM_BATCH_FUNCTION_COUNT))
            return false;
        return !exceeds_limit(selection.instruction_count, component.instruction_count, MAXIMUM_BATCH_INSTRUCTION_COUNT);
    }

    BatchSelection select_components()
    {
        VERIFY(!m_ready_components.is_empty());

        BatchSelection selection;
        while (!m_ready_components.is_empty()) {
            auto component_key = m_ready_components.head();
            auto const& component = m_components.get(component_key).value();
            if (!fits_in_batch(component, selection))
                break;

            m_ready_components.dequeue();
            selection.components.append(component_key);
            selection.function_count += component.inputs.size();
            selection.instruction_count += component.instruction_count;

            schedule(component_key);
        }

        return selection;
    }

    void schedule(ComponentKey component_key)
    {
        auto const& component = m_components.get(component_key).value();
        for (auto dependent : component.dependents) {
            auto& dependent_component = m_components.get(dependent).value();
            bool removed = dependent_component.dependencies.remove(component_key);
            VERIFY(removed);

            if (dependent_component.dependencies.is_empty())
                m_ready_components.enqueue(dependent);
        }
    }

    OrderedHashMap<ComponentKey, CompilationComponent> m_components;
    Queue<ComponentKey> m_ready_components;
};

static ErrorOr<void> compile_incremental_batches(Vector<BatchInput>& inputs, Module const& module)
{
    IncrementalCompilationPlan plan { inputs };
    while (!plan.is_complete()) {
        auto batch = plan.take_batch();
        TRY(try_cranelift_compile_batch(batch, module));
    }
    return {};
}

static void install_cached_functions(Vector<PendingCachedFunction>& functions, Module const& module)
{
    static auto helper_addresses = make_runtime_helper_addresses();
    Vector<PendingCompiledFunction> pending_functions;
    pending_functions.ensure_capacity(functions.size());

    for (auto& function : functions) {
        auto& record = function.record;
        auto pending = prepare_compiled_function(
            function.function_index,
            *function.target,
            record.frontend,
            record.unpatched_code.bytes(),
            record.native_entry_offset,
            record.relocs.span(),
            record.traps.span());
        if (pending.has_value())
            pending_functions.unchecked_append(pending.release_value());
    }

    Vector<PendingCompiledFunction> pending_osr_functions;
    install_compiled_functions(pending_functions, pending_osr_functions, helper_addresses, module);
}

void flush_cranelift_batch(Module const& module)
{
    auto& state = cranelift_cache_state();
    if (state.pending_batch.is_empty() && state.pending_install.functions.is_empty())
        return;

    if (!state.pending_batch.is_empty()) {
        auto result = compile_incremental_batches(state.pending_batch, module);
        if (result.is_error())
            warnln("Cranelift compilation failed: {}", result.error());
    }
    if (!state.pending_install.functions.is_empty())
        install_cached_functions(state.pending_install.functions, module);

    state.pending_batch.clear();
    state.pending_install.functions.clear();
}

void discard_cranelift_batch()
{
    cranelift_cache_state().pending_batch.clear();
}

void free_cranelift_code(void* handle)
{
    delete static_cast<CodeMapping*>(handle);
}

CraneliftCodeOwner::CraneliftCodeOwner(void* handle)
    : m_handle(handle)
{
}

CraneliftCodeOwner::CraneliftCodeOwner(CraneliftCodeOwner&& other)
    : m_handle(other.m_handle)
{
    other.m_handle = nullptr;
}

CraneliftCodeOwner& CraneliftCodeOwner::operator=(CraneliftCodeOwner&& other)
{
    if (this == &other)
        return *this;
    if (m_handle)
        free_cranelift_code(m_handle);
    m_handle = other.m_handle;
    other.m_handle = nullptr;
    return *this;
}

CraneliftCodeOwner::~CraneliftCodeOwner()
{
    if (m_handle)
        free_cranelift_code(m_handle);
}

Module::~Module() = default;

void Module::retain_cranelift_code_handle(void* handle) const
{
    Sync::MutexLocker locker(m_cranelift_code_handles_mutex);
    m_cranelift_code_handles.empend(handle);
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
    cranelift_cache_state().pending_install.functions.clear();
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

    size_t total_size = sizeof(CacheBlobHeader);
    for (auto const& r : capture.records) {
        total_size += sizeof(CacheBlobFunctionEntry);
        total_size += align_up(r.unpatched_code.size(), native_code_alignment);
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
    header->layout_hash = runtime_layout_hash();
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
        entry->frontend = static_cast<u32>(r.frontend);
        offset += sizeof(CacheBlobFunctionEntry);

        __builtin_memcpy(out + offset, r.unpatched_code.data(), r.unpatched_code.size());
        offset += align_up(r.unpatched_code.size(), native_code_alignment);

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

    if (header->layout_hash != runtime_layout_hash())
        return false;

    size_t offset = sizeof(CacheBlobHeader);
    for (u32 i = 0; i < header->function_count; ++i) {
        if (offset + sizeof(CacheBlobFunctionEntry) > blob.size())
            return false;
        auto const* entry = reinterpret_cast<CacheBlobFunctionEntry const*>(blob.data() + offset);
        offset += sizeof(CacheBlobFunctionEntry);
        if (entry->native_entry_offset >= entry->code_size)
            return false;
        if (entry->frontend > static_cast<u32>(CraneliftFrontend::Direct))
            return false;

        auto code_off = offset;
        auto aligned_code_size = align_up(entry->code_size, native_code_alignment);
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
        rec.frontend = static_cast<CraneliftFrontend>(entry->frontend);
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

void set_cranelift_compile_callback(CraneliftCompileCallback callback)
{
    auto& installed_callback = cranelift_compile_callback();
    VERIFY(!installed_callback);

    installed_callback = move(callback);
}

}
