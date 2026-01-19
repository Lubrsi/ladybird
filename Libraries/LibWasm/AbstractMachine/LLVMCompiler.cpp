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

// === LLVMCompiler implementation ===

ErrorOr<NonnullOwnPtr<LLVMCompiler>> LLVMCompiler::create()
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(
            llvm::orc::JITTargetMachineBuilder(llvm::Triple(llvm::sys::getProcessTriple())).setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive))
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

    compile_global_section(*llvm_module, module);
    compile_functions(*llvm_module, module);

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

    auto symbol = m_jit->lookup("wasm_func_2");
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

void LLVMCompiler::compile_functions(llvm::Module& llvm_module, Module const& module)
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

    for (auto& import_ : module.import_section().imports()) {
        auto* type_index = import_.description().get_pointer<TypeIndex>();
        if (!type_index) {
            dbgln("FIXME: Non-function import");
            continue;
        }

        create_function_declaration(*type_index);
    }

    for (auto [code_index, code] : enumerate(code_section.functions())) {
        create_function_declaration(function_section.types()[code_index], &code);
    }

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

llvm::Type* LLVMFunctionGenerator::wasm_type_to_llvm(ValueType type)
{
    return m_compiler.wasm_type_to_llvm(type);
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
        auto* masked = b.CreateAnd(rhs, b.getInt32(31));
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

    // === Comparisons (i32) ===
    case Instructions::i32_eqz.value(): {
        auto* value = pop();
        auto* result = b.CreateICmpEQ(value, b.getInt32(0));
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_eq.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpEQ(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
        break;
    }
    case Instructions::i32_ne.value(): {
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
    case Instructions::i32_ltu.value(): {
        auto* rhs = pop();
        auto* lhs = pop();
        auto* result = b.CreateICmpULT(lhs, rhs);
        push(b.CreateZExt(result, b.getInt32Ty()));
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

        // Branch from end of then-block to end
        b.CreateBr(frame.end_block.llvm_basic_block);

        // Switch to else block
        switch_to_basic_block(frame.else_block.value());
        break;
    }
    case Instructions::structured_end.value(): {
        if (control_stack_size() == 0) {
            // Function end - create return
            if (stack_is_empty()) {
                b.CreateRetVoid();
            } else {
                b.CreateRet(pop());
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

            b.CreateBr(frame.end_block.llvm_basic_block);
            switch_to_basic_block(frame.end_block);
        }
        break;
    }
    case Instructions::br.value(): {
        auto depth = insn.arguments().get<LabelIndex>().value();
        auto& frame = control_frame_at_depth(depth);
        b.CreateBr(frame.branch_target.llvm_basic_block);
        switch_to_basic_block(make_block("unreachable"sv));
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
    case Instructions::return_.value(): {
        if (stack_is_empty()) {
            b.CreateRetVoid();
        } else {
            b.CreateRet(pop());
        }
        switch_to_basic_block(make_block("unreachable"sv));
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
        if (stack_is_empty()) {
            b.CreateRetVoid();
        } else {
            b.CreateRet(pop());
        }
        break;
    case Instructions::nop.value():
        break;
    case Instructions::unreachable.value():
        b.CreateUnreachable();
        switch_to_basic_block(make_block("unreachable"sv));
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

    auto* effective_addr = b.CreateAdd(base, mem->type().limits().address_type() == AddressType::I32 ? b.getInt32(memory_argument.offset) : b.getInt64(memory_argument.offset));
    auto* addr_i64 = b.CreateZExt(effective_addr, b.getInt64Ty());

    auto* memory_base_int = b.getInt64(reinterpret_cast<FlatPtr>(mem->data()));
    auto* memory_base_ptr = b.CreateIntToPtr(memory_base_int, b.getPtrTy());
    return b.CreateGEP(b.getInt8Ty(), memory_base_ptr, addr_i64);
}

} // namespace Wasm
