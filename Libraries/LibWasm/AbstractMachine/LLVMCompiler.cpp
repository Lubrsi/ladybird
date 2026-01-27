/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LLVMCompiler.h"

#include <AK/Enumerate.h>
#include <LibCore/ElapsedTimer.h>
#include <LibWasm/Printer/Printer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

namespace Wasm {

// Helper function for memory.grow - called from JIT code
static i32 wasm_memory_grow_helper(MemoryInstance* mem, i32 pages)
{
    auto old_page_count = static_cast<i32>(mem->size() / Constants::page_size);
    if (mem->grow(static_cast<u64>(pages) * Constants::page_size, MemoryInstance::GrowType::No))
        return old_page_count;
    return -1;
}

// === LLVMCompiler implementation ===

ErrorOr<NonnullOwnPtr<LLVMCompiler>> LLVMCompiler::create()
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(
            llvm::orc::JITTargetMachineBuilder(llvm::Triple(llvm::sys::getProcessTriple())).setCodeGenOptLevel(llvm::CodeGenOptLevel::None))
        .create();
    if (!jit)
        return Error::from_string_literal("Failed to create LLJIT");

    return adopt_nonnull_own_or_enomem(new LLVMCompiler(std::move(*jit)));
}

LLVMCompiler::LLVMCompiler(std::unique_ptr<llvm::orc::LLJIT> jit)
    : m_jit(move(jit))
    , m_context(new llvm::LLVMContext())
{
}

llvm::Type* LLVMCompiler::wasm_type_to_llvm(ValueType type)
{
    switch (type.kind()) {
    case ValueType::I32:
        return llvm::Type::getInt32Ty(*m_context);
    case ValueType::I64:
        return llvm::Type::getInt64Ty(*m_context);
    case ValueType::F32:
        return llvm::Type::getFloatTy(*m_context);
    case ValueType::F64:
        return llvm::Type::getDoubleTy(*m_context);
    case ValueType::V128:
        return llvm::VectorType::get(llvm::Type::getInt8Ty(*m_context), 16, false);
    case ValueType::FunctionReference:
    case ValueType::ExternReference:
        return llvm::Type::getInt64Ty(*m_context); // Opaque pointer as i64
    default:
        VERIFY_NOT_REACHED();
    }
}

llvm::FunctionType* LLVMCompiler::wasm_func_type_to_llvm(FunctionType const& type)
{
    Vector<llvm::Type*> param_types;

    for (auto const& param : type.parameters())
        param_types.append(wasm_type_to_llvm(param));

    llvm::Type* return_type;
    if (type.results().is_empty()) {
        return_type = llvm::Type::getVoidTy(*m_context);
    } else if (type.results().size() == 1) {
        return_type = wasm_type_to_llvm(type.results()[0]);
    } else {
        Vector<llvm::Type*> result_types;
        for (auto const& result : type.results())
            result_types.append(wasm_type_to_llvm(result));
        return_type = llvm::StructType::get(*m_context, { result_types.data(), result_types.size() });
    }

    return llvm::FunctionType::get(return_type, { param_types.data(), param_types.size() }, false);
}

u64 LLVMCompiler::evaluate_constant_expression(Expression const& expression)
{
    u64 result = 0;
    for (auto const& instruction : expression.instructions()) {
        switch (instruction.opcode().value()) {
        case Instructions::i32_const.value():
            result = static_cast<u32>(instruction.arguments().get<i32>());
            break;
        case Instructions::i64_const.value():
            result = instruction.arguments().get<i64>();
            break;
        case Instructions::global_get.value(): {
            auto global_index = instruction.arguments().get<GlobalIndex>();
            auto* g = m_globals[global_index.value()].ptr();
            auto* initializer = g->getInitializer();
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(initializer))
                result = ci->getZExtValue();
            else
                VERIFY_NOT_REACHED();
            break;
        }
        case Instructions::structured_end.value():
        case Instructions::synthetic_end_expression.value():
            // End markers, ignore
            break;
        default:
            dbgln("Unsupported constant expression instruction: {:#x}", instruction.opcode().value());
            VERIFY_NOT_REACHED();
        }
    }
    return result;
}

ErrorOr<void*> LLVMCompiler::compile_module(Module const& module)
{
    // Allocate memories first - must happen before compilation so pointers are stable
    for (auto const& memory : module.memory_section().memories()) {
        auto memory_address = m_store.allocate(memory.type());
        if (!memory_address.has_value())
            return Error::from_string_literal("Failed to allocate memory");
        m_memories.append(memory_address.release_value());
    }

    std::unique_ptr<llvm::Module> llvm_module(new llvm::Module("wasm_module", *m_context));

    compile_function_declarations(*llvm_module, module);
    compile_global_section(*llvm_module, module);
    compile_table_section(module);
    compile_element_section(module);
    compile_data_section(module);
    compile_function_bodies(*llvm_module, module);

    if (llvm::verifyModule(*llvm_module, &llvm::errs()))
        VERIFY_NOT_REACHED();

    // JIT compile
    auto tsm = llvm::orc::ThreadSafeModule(move(llvm_module), move(m_context));
    if (auto err = m_jit->addIRModule(move(tsm)))
        return Error::from_string_literal("Failed to add module to JIT");

    for (size_t index = 0; index < m_function_declarations.size(); ++index) {
        auto name = MUST(String::formatted("wasm_func_{}", index));
        auto sv = name.bytes_as_string_view();
        auto symbol = m_jit->lookup(llvm::StringRef(sv.characters_without_null_termination(), sv.length()));
        if (!symbol)
            return Error::from_string_literal("Failed to find compiled function");

        dbgln("{}: {:p}", index, symbol->getValue());
    }

    auto symbol = m_jit->lookup("wasm_func_1");
    if (!symbol)
        VERIFY_NOT_REACHED();

    auto* ptr = symbol->toPtr<void()>();
    auto elapsed_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    ptr();
    auto ms = elapsed_timer.elapsed_milliseconds();
    dbgln("took {} ms to run!", ms);
    return reinterpret_cast<void*>(ptr);
}

void LLVMCompiler::compile_global_section(llvm::Module& llvm_module, Module const& module)
{
    // For global initializers, we need a temporary stack to evaluate constant expressions
    Vector<llvm::Value*> init_stack;

    for (auto const& global_entry : module.global_section().entries()) {
        // Evaluate the initializer expression (must be constant)
        for (auto const& instruction : global_entry.expression().instructions()) {
            switch (instruction.opcode().value()) {
            case Instructions::i32_const.value():
                init_stack.append(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*m_context), instruction.arguments().get<i32>()));
                break;
            case Instructions::i64_const.value():
                init_stack.append(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*m_context), instruction.arguments().get<i64>()));
                break;
            case Instructions::f32_const.value():
                init_stack.append(llvm::ConstantFP::get(llvm::Type::getFloatTy(*m_context), instruction.arguments().get<float>()));
                break;
            case Instructions::f64_const.value():
                init_stack.append(llvm::ConstantFP::get(llvm::Type::getDoubleTy(*m_context), instruction.arguments().get<double>()));
                break;
            case Instructions::global_get.value(): {
                auto global_index = instruction.arguments().get<GlobalIndex>();
                auto* g = m_globals[global_index.value()].ptr();
                // For constant initializers, we need the initializer value, not a load
                init_stack.append(g->getInitializer());
                break;
            }
            case Instructions::structured_end.value():
            case Instructions::synthetic_end_expression.value():
                // End markers, ignore
                break;
            default:
                dbgln("Unsupported global initializer instruction: {:#x}", instruction.opcode().value());
                VERIFY_NOT_REACHED();
            }
        }

        auto* global_initial_value = init_stack.take_last();

        if (auto* constant = llvm::dyn_cast<llvm::Constant>(global_initial_value)) {
            auto* wasm_type = wasm_type_to_llvm(global_entry.type().type());
            m_globals.append(make<llvm::GlobalVariable>(llvm_module, wasm_type, !global_entry.type().is_mutable(), llvm::GlobalValue::InternalLinkage, constant));
        } else {
            VERIFY_NOT_REACHED();
        }
    }
}

void LLVMCompiler::compile_data_section(Module const& module)
{
    for (auto const& segment : module.data_section().data()) {
        segment.value().visit(
            [&](DataSection::Data::Active const& data) {
                auto offset = evaluate_constant_expression(data.offset);

                // Copy data into memory
                auto* memory_instance = m_store.get(m_memories[data.index.value()]);
                VERIFY(offset + data.init.size() <= memory_instance->size());
                if (!data.init.is_empty())
                    AK::TypedTransfer<u8>::copy(memory_instance->data() + offset, data.init.data(), data.init.size());
            },
            [&](DataSection::Data::Passive const& passive) {
                // Store for later use with memory.init
                auto buffer = MUST(ByteBuffer::copy(passive.init));
                m_passive_data_segments.append(move(buffer));
            });
    }
}

void LLVMCompiler::compile_table_section(Module const& module)
{
    for (auto const& table : module.table_section().tables()) {
        auto table_address = m_store.allocate(table.type());
        VERIFY(table_address.has_value());
        m_tables.append(table_address.release_value());
    }
}

void LLVMCompiler::compile_element_section(Module const& module)
{
    for (auto const& segment : module.element_section().segments()) {
        // First, evaluate all init expressions to get the references
        Vector<Reference> references;
        for (auto const& init_expr : segment.init) {
            for (auto const& instruction : init_expr.instructions()) {
                switch (instruction.opcode().value()) {
                case Instructions::ref_func.value(): {
                    auto function_index = instruction.arguments().get<FunctionIndex>();
                    references.append(Reference { Reference::Func { FunctionAddress { function_index.value() }, &module } });
                    break;
                }
                case Instructions::ref_null.value():
                    references.append(Reference { Reference::Null { segment.type } });
                    break;
                case Instructions::global_get.value():
                    // FIXME: Handle global.get for reference globals
                    dbgln("FIXME: global.get in element init expression");
                    break;
                case Instructions::structured_end.value():
                case Instructions::synthetic_end_expression.value():
                    break;
                default:
                    dbgln("Unsupported element init instruction: {:#x}", instruction.opcode().value());
                    break;
                }
            }
        }

        segment.mode.visit(
            [&](ElementSection::Active const& active) {
                auto offset = evaluate_constant_expression(active.expression);

                // Copy references into table
                auto* table_instance = m_store.get(m_tables[active.index.value()]);
                for (size_t i = 0; i < references.size(); ++i) {
                    VERIFY(offset + i < table_instance->elements().size());
                    table_instance->elements()[offset + i] = references[i];
                }
            },
            [&](ElementSection::Passive const&) {
                // FIXME: Store for later use with table.init
            },
            [&](ElementSection::Declarative const&) {
                // Declarative segments are dropped after validation, nothing to do
            });
    }
}

void LLVMCompiler::compile_function_declarations(llvm::Module& llvm_module, Module const& module)
{
    auto const& code_section = module.code_section();
    auto const& function_section = module.function_section();
    auto const& type_section = module.type_section();

    size_t function_number = 0;
    auto create_function_declaration = [&](TypeIndex function_type_index, CodeSection::Code const* code = nullptr) {
        auto const& function_type = type_section.types()[function_type_index.value()];
        auto* llvm_function_type = wasm_func_type_to_llvm(function_type);

        auto function_name = MUST(String::formatted("wasm_func_{}", function_number));
        auto function_name_view = function_name.bytes_as_string_view();

        auto* llvm_function = llvm::Function::Create(
            llvm_function_type,
            llvm::Function::ExternalLinkage,
            llvm::StringRef(function_name_view.characters_without_null_termination(), function_name_view.length()),
            llvm_module);
        m_function_declarations.append(FunctionDeclaration {
            .llvm_function = *llvm_function,
            .wasm_function_type = function_type,
            .wasm_code = code,
        });

        ++function_number;
    };

    for (auto const& import_ : module.import_section().imports()) {
        auto const* type_index = import_.description().get_pointer<TypeIndex>();
        if (!type_index) {
            dbgln("FIXME: Non-function import");
            continue;
        }

        create_function_declaration(*type_index);
    }

    for (auto [code_index, code] : enumerate(code_section.functions())) {
        create_function_declaration(function_section.types()[code_index], &code);
    }

    // Create function pointer table for call_indirect
    if (!m_function_declarations.is_empty()) {
        auto* ptr_type = llvm::PointerType::get(*m_context, 0);
        auto* array_type = llvm::ArrayType::get(ptr_type, m_function_declarations.size());

        Vector<llvm::Constant*> func_ptrs;
        for (auto const& decl : m_function_declarations)
            func_ptrs.append(&decl.llvm_function);

        auto* initializer = llvm::ConstantArray::get(array_type, { func_ptrs.data(), func_ptrs.size() });
        m_function_pointer_table = new llvm::GlobalVariable(
            llvm_module,
            array_type,
            true, // isConstant
            llvm::GlobalValue::InternalLinkage,
            initializer,
            "wasm_func_ptr_table");
    }
}

void LLVMCompiler::compile_function_bodies(llvm::Module& llvm_module, Module const& module)
{
    for (auto const& function_declaration : m_function_declarations) {
        if (!function_declaration.wasm_code) {
            dbgln("FIXME: Imported function");
            continue;
        }

        LLVMFunctionGenerator generator(*this, llvm_module, function_declaration.llvm_function, module);

        // Entry block
        auto entry = generator.make_block("entry"sv);
        generator.switch_to_basic_block(entry);

        // Allocate locals (parameters + local variables)
        size_t param_idx = 0;
        for (auto const& param_type : function_declaration.wasm_function_type.parameters()) {
            auto* alloca = generator.builder().CreateAlloca(generator.wasm_type_to_llvm(param_type));
            generator.builder().CreateStore(function_declaration.llvm_function.getArg(param_idx++), alloca);
            generator.append_local(alloca);
        }
        for (auto const& local : function_declaration.wasm_code->func().locals()) {
            for (size_t local_index = 0; local_index < local.n(); local_index++) {
                auto* llvm_type = generator.wasm_type_to_llvm(local.type());
                auto* alloca = generator.builder().CreateAlloca(llvm_type);
                generator.builder().CreateStore(llvm::Constant::getNullValue(llvm_type), alloca);
                generator.append_local(alloca);
            }
        }

        generator.compile_expression(function_declaration.wasm_code->func().body());

        // Verify the function
        if (llvm::verifyFunction(function_declaration.llvm_function, &llvm::errs()))
            VERIFY_NOT_REACHED();
    }
}

// === LLVMFunctionGenerator implementation ===

LLVMFunctionGenerator::LLVMFunctionGenerator(LLVMCompiler& compiler, llvm::Module& module, llvm::Function& function, Module const& wasm_module)
    : m_compiler(compiler)
    , m_module(module)
    , m_function(function)
    , m_builder(compiler.context())
    , m_wasm_module(wasm_module)
{
}

llvm::LLVMContext& LLVMFunctionGenerator::context()
{
    return m_compiler.context();
}

WasmBasicBlock LLVMFunctionGenerator::make_block(StringView name)
{
    String block_name;
    if (name.is_empty())
        block_name = String::number(m_next_block++);
    else
        block_name = MUST(String::formatted("{}_{}", name, m_next_block++));

    auto sv = block_name.bytes_as_string_view();
    auto* block = llvm::BasicBlock::Create(context(), llvm::StringRef(sv.characters_without_null_termination(), sv.length()), &m_function);
    return WasmBasicBlock {
        .llvm_basic_block = block,
        .stack = current_block().stack,
    };
}

void LLVMFunctionGenerator::switch_to_basic_block(WasmBasicBlock block)
{
    m_current_block = move(block);
    m_builder.SetInsertPoint(block.llvm_basic_block);
}

void LLVMFunctionGenerator::switch_to_unreachable_block()
{
    switch_to_basic_block(make_block("unreachable"sv));
    m_builder.CreateUnreachable();
}

bool LLVMFunctionGenerator::is_current_block_terminated() const
{
    return m_current_block.llvm_basic_block->getTerminator() != nullptr;
}

void LLVMFunctionGenerator::push_control_frame(WasmBasicBlock branch_target, WasmBasicBlock end_block, Optional<WasmBasicBlock> else_block)
{
    m_control_stack.append({
        .branch_target = move(branch_target),
        .end_block = move(end_block),
        .else_block = move(else_block),
    });
}

ControlFrame LLVMFunctionGenerator::pop_control_frame()
{
    return m_control_stack.take_last();
}

ControlFrame& LLVMFunctionGenerator::control_frame_at_depth(size_t depth)
{
    return m_control_stack[m_control_stack.size() - 1 - depth];
}

FunctionDeclaration const& LLVMFunctionGenerator::function_declaration(size_t index)
{
    return m_compiler.m_function_declarations[index];
}

llvm::GlobalVariable* LLVMFunctionGenerator::global(size_t index)
{
    return m_compiler.m_globals[index].ptr();
}

MemoryInstance* LLVMFunctionGenerator::memory(size_t index)
{
    return m_compiler.m_store.get(m_compiler.m_memories[index]);
}

TableInstance* LLVMFunctionGenerator::table(size_t index)
{
    return m_compiler.m_store.get(m_compiler.m_tables[index]);
}

llvm::GlobalVariable* LLVMFunctionGenerator::function_pointer_table()
{
    return m_compiler.m_function_pointer_table;
}

llvm::Type* LLVMFunctionGenerator::wasm_type_to_llvm(ValueType type)
{
    return m_compiler.wasm_type_to_llvm(type);
}

llvm::FunctionType* LLVMFunctionGenerator::wasm_func_type_to_llvm(FunctionType const& type)
{
    return m_compiler.wasm_func_type_to_llvm(type);
}

void LLVMFunctionGenerator::compile_expression(Expression const& expression)
{
    dbgln("== expression");
    for (auto const& instruction : expression.instructions())
        compile_instruction(instruction);

    dbgln("== end expression");
}

void LLVMFunctionGenerator::compile_instruction(Instruction const& insn)
{
    auto& b = builder();

    dbgln("{}", instruction_name(insn.opcode()));

    switch (insn.opcode().value()) {

    // === Constants ===
    case Instructions::i32_const.value(): {
        auto value = insn.arguments().get<i32>();
        push(b.getInt32(value));
        break;
    }
    case Instructions::i64_const.value(): {
        auto value = insn.arguments().get<i64>();
        push(b.getInt64(value));
        break;
    }
    case Instructions::f32_const.value(): {
        auto value = insn.arguments().get<float>();
        push(llvm::ConstantFP::get(b.getFloatTy(), value));
        break;
    }
    case Instructions::f64_const.value(): {
        auto value = insn.arguments().get<double>();
        push(llvm::ConstantFP::get(b.getDoubleTy(), value));
        break;
    }

    // === Locals ===
    case Instructions::local_get.value(): {
        auto idx = insn.local_index().value();
        auto* alloca = local(idx);
        auto* value = b.CreateLoad(alloca->getAllocatedType(), alloca);
        push(value);
        break;
    }
    case Instructions::local_set.value(): {
        auto idx = insn.local_index().value();
        auto* value = pop();
        b.CreateStore(value, local(idx));
        break;
    }
    case Instructions::local_tee.value(): {
        auto idx = insn.local_index().value();
        auto* value = peek();
        b.CreateStore(value, local(idx));
        break;
    }

    case Instructions::global_get.value(): {
        auto global_index = insn.arguments().get<GlobalIndex>();
        auto* g = global(global_index.value());
        auto* value = b.CreateLoad(g->getValueType(), g);
        push(value);
        break;
    }

    case Instructions::global_set.value(): {
        auto global_index = insn.arguments().get<GlobalIndex>();
        auto* g = global(global_index.value());
        auto* value = pop();
        b.CreateStore(value, g);
        break;
    }

    // === Arithmetic (i32) ===
    case Instructions::i32_add.value():
    case Instructions::i64_add.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateAdd(lhs, rhs));
        break;
    }
    case Instructions::i32_sub.value():
    case Instructions::i64_sub.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateSub(lhs, rhs));
        break;
    }
    case Instructions::i32_mul.value():
    case Instructions::i64_mul.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateMul(lhs, rhs));
        break;
    }
    case Instructions::i32_divs.value():
    case Instructions::i64_divs.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        // TODO: Trap on division by zero
        push(b.CreateSDiv(lhs, rhs));
        break;
    }
    case Instructions::i32_divu.value():
    case Instructions::i64_divu.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateUDiv(lhs, rhs));
        break;
    }
    case Instructions::i32_rems.value():
    case Instructions::i64_rems.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateSRem(lhs, rhs));
        break;
    }
    case Instructions::i32_remu.value():
    case Instructions::i64_remu.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateURem(lhs, rhs));
        break;
    }
    case Instructions::i32_and.value():
    case Instructions::i64_and.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateAnd(lhs, rhs));
        break;
    }
    case Instructions::i32_or.value():
    case Instructions::i64_or.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateOr(lhs, rhs));
        break;
    }
    case Instructions::i32_xor.value():
    case Instructions::i64_xor.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateXor(lhs, rhs));
        break;
    }
    case Instructions::i32_shl.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        // Wasm: shift amount is masked to 5 bits
        auto* masked = b.CreateAnd(rhs, b.getInt32(sizeof(i32) * 8 - 1));
        push(b.CreateShl(lhs, masked));
        break;
    }
    case Instructions::i64_shl.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        // Wasm: shift amount is masked to 5 bits
        auto* masked = b.CreateAnd(rhs, b.getInt64(sizeof(i64) * 8 - 1));
        push(b.CreateShl(lhs, masked));
        break;
    }
    case Instructions::i32_shrs.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* masked = b.CreateAnd(rhs, b.getInt32(sizeof(i32) * 8 - 1));
        push(b.CreateAShr(lhs, masked));
        break;
    }
    case Instructions::i32_shru.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* masked = b.CreateAnd(rhs, b.getInt32(sizeof(i32) * 8 - 1));
        push(b.CreateLShr(lhs, masked));
        break;
    }
    case Instructions::i32_rotl.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateIntrinsic(llvm::Intrinsic::fshl, { b.getInt32Ty() }, { lhs, lhs, rhs }));
        break;
    }
    case Instructions::i32_rotr.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        push(b.CreateIntrinsic(llvm::Intrinsic::fshr, { b.getInt32Ty() }, { lhs, lhs, rhs }));
        break;
    }
    case Instructions::i32_clz.value(): {
        auto* value = pop();
        push(b.CreateIntrinsic(llvm::Intrinsic::ctlz, { b.getInt32Ty() }, { value, /* is_zero_poison= */ b.getFalse() }));
        break;
    }
    case Instructions::i32_ctz.value(): {
        auto* value = pop();
        push(b.CreateIntrinsic(llvm::Intrinsic::cttz, { b.getInt32Ty() }, { value, /* is_zero_poison= */ b.getFalse() }));
        break;
    }
    case Instructions::i64_shru.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* masked = b.CreateAnd(rhs, b.getInt64(sizeof(i64) * 8 - 1));
        push(b.CreateLShr(lhs, masked));
        break;
    }

    case Instructions::i32_wrap_i64.value(): {
        auto* i64_value = pop();
        push(b.CreateTrunc(i64_value, b.getInt32Ty()));
        break;
    }
    case Instructions::i64_extend_ui32.value(): {
        auto* i32_value = pop();
        push(b.CreateZExt(i32_value, b.getInt64Ty()));
        break;
    }

    // === Comparisons ===
    case Instructions::i32_eqz.value(): {
        auto* value = pop();
        auto* result = b.CreateICmpEQ(value, b.getInt32(0));
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i64_eqz.value(): {
        auto* value = pop();
        auto* result = b.CreateICmpEQ(value, b.getInt64(0));
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_eq.value():
    case Instructions::i64_eq.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpEQ(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_ne.value():
    case Instructions::i64_ne.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpNE(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_lts.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpSLT(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_ltu.value():
    case Instructions::i64_ltu.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpULT(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_gts.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpSGT(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_gtu.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpUGT(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_les.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpSLE(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_leu.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpULE(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_ges.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpSGE(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_geu.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpUGE(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_load8_s.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt8Ty(), ptr);
        push(b.CreateSExt(loaded_value, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_load8_u.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt8Ty(), ptr);
        push(b.CreateZExt(loaded_value, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_load16_s.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt16Ty(), ptr);
        push(b.CreateSExt(loaded_value, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_load16_u.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt16Ty(), ptr);
        push(b.CreateZExt(loaded_value, b.getInt32Ty()));
        break;
    }
    case Instructions::i64_load8_s.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt8Ty(), ptr);
        push(b.CreateSExt(loaded_value, b.getInt64Ty()));
        break;
    }
    case Instructions::i64_load8_u.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt8Ty(), ptr);
        push(b.CreateZExt(loaded_value, b.getInt64Ty()));
        break;
    }
    case Instructions::i64_load16_s.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt16Ty(), ptr);
        push(b.CreateSExt(loaded_value, b.getInt64Ty()));
        break;
    }
    case Instructions::i64_load16_u.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt16Ty(), ptr);
        push(b.CreateZExt(loaded_value, b.getInt64Ty()));
        break;
    }
    case Instructions::i64_load32_s.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt32Ty(), ptr);
        push(b.CreateSExt(loaded_value, b.getInt64Ty()));
        break;
    }
    case Instructions::i64_load32_u.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        auto loaded_value = b.CreateLoad(b.getInt32Ty(), ptr);
        push(b.CreateZExt(loaded_value, b.getInt64Ty()));
        break;
    }
    // === Memory Operations ===
    case Instructions::i32_load.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        push(b.CreateLoad(b.getInt32Ty(), ptr));
        break;
    }
    case Instructions::i64_load.value(): {
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        push(b.CreateLoad(b.getInt64Ty(), ptr));
        break;
    }
    case Instructions::i32_store.value():
    case Instructions::i64_store.value(): {
        auto* value = pop();
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        b.CreateStore(value, ptr);
        break;
    }
    case Instructions::i32_store8.value():
    case Instructions::i64_store8.value(): {
        auto* value = pop();
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        b.CreateStore(b.CreateTrunc(value, b.getInt8Ty()), ptr);
        break;
    }
    case Instructions::i32_store16.value():
    case Instructions::i64_store16.value(): {
        auto* value = pop();
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        b.CreateStore(b.CreateTrunc(value, b.getInt16Ty()), ptr);
        break;
    }
    case Instructions::i64_store32.value(): {
        auto* value = pop();
        auto* base = pop();
        auto* ptr = get_memory_pointer(insn.arguments().get<Instruction::MemoryArgument>(), base);
        b.CreateStore(b.CreateTrunc(value, b.getInt32Ty()), ptr);
        break;
    }

    case Instructions::memory_copy.value(): {
        auto& args = insn.arguments().get<Instruction::MemoryCopyArgs>();

        auto* count = pop();

        auto* source_offset = pop();
        auto* source = get_memory_pointer(args.src_index, source_offset);

        auto* destination_offset = pop();
        auto* destination = get_memory_pointer(args.src_index, destination_offset);

        b.CreateMemCpy(destination, llvm::MaybeAlign(), source, llvm::MaybeAlign(), count);
        break;
    }

    case Instructions::memory_fill.value(): {
        auto& memory_index_arg = insn.arguments().get<Instruction::MemoryIndexArgument>();

        auto* count = pop();
        auto* value = b.CreateTrunc(pop(), b.getInt8Ty());

        auto* destination_offset = pop();
        auto* destination = get_memory_pointer(memory_index_arg.memory_index, destination_offset);

        b.CreateMemSet(destination, value, count, llvm::MaybeAlign());
        break;
    }

    case Instructions::memory_size.value(): {
        auto memory_index = insn.arguments().get<Instruction::MemoryIndexArgument>().memory_index;
        auto* mem = memory(memory_index.value());

        // Embed pointer to the size field and load at runtime
        auto* size_ptr_int = b.getInt64(reinterpret_cast<FlatPtr>(mem->size_ptr()));
        auto* size_ptr = b.CreateIntToPtr(size_ptr_int, b.getPtrTy());
        auto* size_bytes = b.CreateLoad(b.getInt64Ty(), size_ptr);

        // Convert bytes to pages (divide by 65536)
        auto* page_count = b.CreateLShr(size_bytes, 16);
        push(b.CreateTrunc(page_count, b.getInt32Ty()));
        break;
    }

    case Instructions::memory_grow.value(): {
        auto memory_index = insn.arguments().get<Instruction::MemoryIndexArgument>().memory_index;
        auto* mem = memory(memory_index.value());

        auto* pages_to_grow = pop();

        // Call the helper function: i32 wasm_memory_grow_helper(MemoryInstance*, i32)
        auto* helper_func_ptr = b.getInt64(reinterpret_cast<FlatPtr>(&wasm_memory_grow_helper));
        auto* helper_func = b.CreateIntToPtr(helper_func_ptr, b.getPtrTy());

        auto* mem_ptr = b.CreateIntToPtr(b.getInt64(reinterpret_cast<FlatPtr>(mem)), b.getPtrTy());

        // Create the function type: i32 (ptr, i32)
        auto* func_type = llvm::FunctionType::get(b.getInt32Ty(), { b.getPtrTy(), b.getInt32Ty() }, false);

        auto* result = b.CreateCall(func_type, helper_func, { mem_ptr, pages_to_grow });
        push(result);
        break;
    }

    // === Control Flow ===
    case Instructions::block.value(): {
        auto end_block = make_block("block_end"sv);
        push_control_frame(end_block, end_block);
        break;
    }
    case Instructions::loop.value(): {
        auto loop_header = make_block("loop"sv);
        auto loop_end = make_block("loop_end"sv);

        b.CreateBr(loop_header.llvm_basic_block);
        switch_to_basic_block(loop_header);

        push_control_frame(move(loop_header), move(loop_end));
        break;
    }
    case Instructions::if_.value(): {
        auto* condition = pop();
        auto* cond_bool = b.CreateICmpNE(condition, b.getInt32(0));

        auto then_block = make_block("then"sv);
        auto else_block = make_block("else"sv);
        auto end_block = make_block("if_end"sv);

        b.CreateCondBr(cond_bool, then_block.llvm_basic_block, else_block.llvm_basic_block);
        switch_to_basic_block(then_block);

        push_control_frame(end_block, end_block, move(else_block));
        break;
    }
    case Instructions::structured_else.value(): {
        auto& frame = control_frame_at_depth(0);

        // Branch from end of then-block to end (unless already terminated)
        if (!is_current_block_terminated())
            b.CreateBr(frame.end_block.llvm_basic_block);

        // Switch to else block
        switch_to_basic_block(frame.else_block.value());
        break;
    }
    case Instructions::structured_end.value(): {
        if (control_stack_size() == 0) {
            // Function end - create return (unless already terminated by return/br/unreachable)
            if (!is_current_block_terminated()) {
                if (stack_is_empty()) {
                    b.CreateRetVoid();
                } else {
                    b.CreateRet(pop());
                }
            }
        } else {
            auto frame = pop_control_frame();

            // If there's an else block that was never used, fill it
            if (frame.else_block.has_value() && frame.else_block->llvm_basic_block->empty()) {
                auto* current = &current_block();
                switch_to_basic_block(*frame.else_block);
                b.CreateBr(frame.end_block.llvm_basic_block);
                switch_to_basic_block(*current);
            }

            // Branch to end block (unless already terminated by br/return/unreachable)
            if (!is_current_block_terminated())
                b.CreateBr(frame.end_block.llvm_basic_block);
            switch_to_basic_block(frame.end_block);
        }
        break;
    }
    case Instructions::br.value(): {
        auto depth = insn.arguments().get<Instruction::BranchArgs>().label;
        auto& frame = control_frame_at_depth(depth.value());
        b.CreateBr(frame.branch_target.llvm_basic_block);
        switch_to_unreachable_block();
        break;
    }
    case Instructions::br_if.value(): {
        auto depth = insn.arguments().get<Instruction::BranchArgs>().label;
        auto& frame = control_frame_at_depth(depth.value());

        auto* condition = pop();
        auto* cond_bool = b.CreateICmpNE(condition, b.getInt32(0));

        auto continue_block = make_block("br_if_continue"sv);
        b.CreateCondBr(cond_bool, frame.branch_target.llvm_basic_block, continue_block.llvm_basic_block);
        switch_to_basic_block(move(continue_block));
        break;
    }
    case Instructions::br_table.value(): {
        auto& args = insn.arguments().get<Instruction::TableBranchArgs>();
        auto* value = pop();

        auto& default_block = control_frame_at_depth(args.default_.value());
        auto switch_ = b.CreateSwitch(value, default_block.branch_target.llvm_basic_block, args.labels.size());

        for (size_t i = 0; i < args.labels.size(); ++i) {
            auto& frame = control_frame_at_depth(args.labels[i].value());
            switch_->addCase(b.getInt32(i), frame.branch_target.llvm_basic_block);
        }

        switch_to_unreachable_block();
        break;
    }
    case Instructions::return_.value(): {
        if (stack_is_empty()) {
            b.CreateRetVoid();
        } else {
            b.CreateRet(pop());
        }
        switch_to_unreachable_block();
        break;
    }

    // === Misc ===
    case Instructions::drop.value(): {
        (void)pop();
        break;
    }
    case Instructions::select.value(): {
        auto* condition = pop();
        auto* val2 = pop();
        auto* val1 = pop();
        auto* cond_bool = b.CreateICmpNE(condition, b.getInt32(0));
        push(b.CreateSelect(cond_bool, val1, val2));
        break;
    }
    case Instructions::synthetic_end_expression.value():
        // Don't add a return if we're in an unreachable block (e.g., after return/br/unreachable)
        if (!is_current_block_terminated()) {
            if (stack_is_empty()) {
                b.CreateRetVoid();
            } else {
                b.CreateRet(pop());
            }
        }
        break;
    case Instructions::nop.value():
        break;
    case Instructions::unreachable.value():
        b.CreateUnreachable();
        switch_to_unreachable_block();
        break;
    case Instructions::call.value(): {
        auto function_index = insn.arguments().get<FunctionIndex>();
        auto& function_to_call = function_declaration(function_index.value());

        Vector<llvm::Value*> arguments;
        for (size_t parameter_index = 0; parameter_index < function_to_call.wasm_function_type.parameters().size(); ++parameter_index)
            arguments.append(pop());

        llvm::FunctionCallee function_callee(function_to_call.llvm_function.getFunctionType(), &function_to_call.llvm_function);
        llvm::ArrayRef arguments_as_llvm_array { arguments.data(), arguments.size() };
        auto* call = b.CreateCall(function_callee, arguments_as_llvm_array);

        auto const& results = function_to_call.wasm_function_type.results();
        if (results.size() == 1) {
            push(call);
        } else if (results.size() > 1) {
            for (size_t result_index = 0; result_index < results.size(); ++result_index)
                push(b.CreateExtractValue(call, result_index));
        }
        break;
    }
    case Instructions::call_indirect.value(): {
        auto const& args = insn.arguments().get<Instruction::IndirectCallArgs>();
        auto* table_instance = table(args.table.value());
        auto* table_index = pop();

        // FIXME: trap if index is out of bounds
        // FIXME: trap if reference is null
        // FIXME: trap if actual type is not equal to expected type

        auto const& function_type = wasm_module().type_section().types()[args.type.value()];
        auto* llvm_function_type = wasm_func_type_to_llvm(function_type);

        // The table stores Reference objects. Reference::Func has FunctionAddress at offset 0.
        // We need to:
        // 1. Compute pointer to the Reference in the table
        // 2. Load the FunctionAddress (u64) from offset 0
        // 3. Use that to index into our function pointer table
        // 4. Call through the function pointer

        // Compute pointer to table element: table_base + index * sizeof(Reference)
        auto* table_base_int = b.getInt64(reinterpret_cast<FlatPtr>(table_instance->elements().data()));
        auto* table_base_ptr = b.CreateIntToPtr(table_base_int, b.getPtrTy());
        auto* index_i64 = b.CreateZExt(table_index, b.getInt64Ty());
        auto* reference_size = b.getInt64(sizeof(Reference));
        auto* byte_offset = b.CreateMul(index_i64, reference_size);
        auto* element_ptr = b.CreateGEP(b.getInt8Ty(), table_base_ptr, byte_offset);

        // Load FunctionAddress (u64) from offset 0 of the Reference
        auto* func_addr = b.CreateLoad(b.getInt64Ty(), element_ptr);

        // Index into function pointer table and load the function pointer
        auto* func_ptr_table = function_pointer_table();
        auto* zero = b.getInt64(0);
        auto* gep = b.CreateGEP(func_ptr_table->getValueType(), func_ptr_table, { zero, func_addr });
        auto* func_ptr = b.CreateLoad(b.getPtrTy(), gep);

        // Pop arguments (in reverse order, same as direct call)
        Vector<llvm::Value*> call_arguments;
        for (size_t i = 0; i < function_type.parameters().size(); ++i)
            call_arguments.append(pop());

        // Call through the function pointer
        llvm::FunctionCallee callee(llvm_function_type, func_ptr);
        llvm::ArrayRef call_arguments_array { call_arguments.data(), call_arguments.size() };
        auto* call_result = b.CreateCall(callee, call_arguments_array);

        // Handle return values
        auto const& results = function_type.results();
        if (results.size() == 1) {
            push(call_result);
        } else if (results.size() > 1) {
            for (size_t result_index = 0; result_index < results.size(); ++result_index)
                push(b.CreateExtractValue(call_result, result_index));
        }
        break;
    }
    default:
        dbgln("Unimplemented instruction: {:#x}", insn.opcode().value());
        break;
    }
}

llvm::Value* LLVMFunctionGenerator::get_memory_pointer(Instruction::MemoryArgument const& memory_argument, llvm::Value* base)
{
    auto& b = builder();

    auto* mem = memory(memory_argument.memory_index.value());

    dbgln("memory at {}", mem->data());

    llvm::Value* memory_base_ptr = nullptr;

    if (!mem->backed_by_virtual_memory()) {
        auto* ptr_to_data_ptr_int = b.getInt64(reinterpret_cast<FlatPtr>(mem->ptr_to_data()));
        auto* ptr_to_data_ptr = b.CreateIntToPtr(ptr_to_data_ptr_int, b.getPtrTy());
        memory_base_ptr = b.CreateIntToPtr(b.CreateLoad(b.getInt64Ty(), ptr_to_data_ptr), b.getPtrTy());
    } else {
        auto* memory_base_int = b.getInt64(reinterpret_cast<FlatPtr>(mem->data()));
        memory_base_ptr = b.CreateIntToPtr(memory_base_int, b.getPtrTy());
    }

    VERIFY(memory_base_ptr);

    // FIXME: trap if address is out of bounds (this is done by the MMU when using VM backing)
    auto* effective_addr = b.CreateAdd(base, mem->type().limits().address_type() == AddressType::I32 ? b.getInt32(memory_argument.offset) : b.getInt64(memory_argument.offset));
    auto* addr_i64 = b.CreateZExt(effective_addr, b.getInt64Ty());
    return b.CreateGEP(b.getInt8Ty(), memory_base_ptr, addr_i64);
}

llvm::Value* LLVMFunctionGenerator::get_memory_pointer(MemoryIndex memory_index, llvm::Value* pointer)
{
    auto& b = builder();

    auto* mem = memory(memory_index.value());

    dbgln("memory at {}", mem->data());

    llvm::Value* memory_base_ptr = nullptr;

    if (!mem->backed_by_virtual_memory()) {
        auto* ptr_to_data_ptr_int = b.getInt64(reinterpret_cast<FlatPtr>(mem->ptr_to_data()));
        auto* ptr_to_data_ptr = b.CreateIntToPtr(ptr_to_data_ptr_int, b.getPtrTy());
        memory_base_ptr = b.CreateIntToPtr(b.CreateLoad(b.getInt64Ty(), ptr_to_data_ptr), b.getPtrTy());
    } else {
        auto* memory_base_int = b.getInt64(reinterpret_cast<FlatPtr>(mem->data()));
        memory_base_ptr = b.CreateIntToPtr(memory_base_int, b.getPtrTy());
    }

    VERIFY(memory_base_ptr);

    // FIXME: trap if address is out of bounds (this is done by the MMU when using VM backing)
    auto* effective_addr = b.getInt64(reinterpret_cast<FlatPtr>(pointer));
    auto* addr_i64 = b.CreateZExt(effective_addr, b.getInt64Ty());
    return b.CreateGEP(b.getInt8Ty(), memory_base_ptr, addr_i64);
}

} // namespace Wasm
