/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWasm/AbstractMachine/AbstractMachine.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#pragma GCC diagnostic pop

namespace Wasm {

class LLVMCompiler;

// For structured control flow
struct WasmBasicBlock {
    llvm::BasicBlock* llvm_basic_block { nullptr };
    Vector<llvm::Value*> stack;
};

struct ControlFrame {
    WasmBasicBlock branch_target;        // Where `br` jumps to (loop header for loops, end block for blocks)
    WasmBasicBlock end_block;            // Where to go after the construct ends
    Optional<WasmBasicBlock> else_block; // For `if` instructions
};

struct FunctionDeclaration {
    llvm::Function& llvm_function;
    FunctionType const& wasm_function_type;
    CodeSection::Code const& wasm_code;
};

// Generator-style class for compiling a single Wasm function to LLVM IR
class LLVMFunctionGenerator {
public:
    LLVMFunctionGenerator(LLVMCompiler&, llvm::Module&, llvm::Function&, Module const&);

    // Basic block management (Generator-style API)
    WasmBasicBlock make_block(StringView name = {});
    void switch_to_basic_block(WasmBasicBlock block);
    [[nodiscard]] WasmBasicBlock& current_block() { return m_current_block; }
    [[nodiscard]] WasmBasicBlock const& current_block() const { return m_current_block; }
    [[nodiscard]] bool is_current_block_terminated() const;

    // Control flow helpers
    void push_control_frame(WasmBasicBlock branch_target, WasmBasicBlock end_block, Optional<WasmBasicBlock> else_block = OptionalNone {});
    ControlFrame pop_control_frame();
    [[nodiscard]] ControlFrame& control_frame_at_depth(size_t depth);
    [[nodiscard]] size_t control_stack_size() const { return m_control_stack.size(); }

    // Stack management
    void push(llvm::Value* value) { current_block().stack.append(value); }
    [[nodiscard]] llvm::Value* pop() { return current_block().stack.take_last(); }
    [[nodiscard]] llvm::Value* peek() { return current_block().stack.last(); }
    [[nodiscard]] bool stack_is_empty() const { return current_block().stack.is_empty(); }

    // Local variable access
    [[nodiscard]] llvm::AllocaInst* local(size_t index) { return m_locals[index]; }
    void append_local(llvm::AllocaInst* alloca) { m_locals.append(alloca); }

    [[nodiscard]] FunctionDeclaration const& function_declaration(size_t index);
    [[nodiscard]] llvm::GlobalVariable* global(size_t index);

    // Memory access
    [[nodiscard]] MemoryInstance* memory(size_t index);

    // IR Builder access
    [[nodiscard]] llvm::IRBuilder<>& builder() { return m_builder; }
    [[nodiscard]] llvm::LLVMContext& context();
    [[nodiscard]] llvm::Function& function() { return m_function; }
    [[nodiscard]] Module const& wasm_module() const { return m_wasm_module; }

    // Type conversion
    [[nodiscard]] llvm::Type* wasm_type_to_llvm(ValueType type);

    // Expression/instruction compilation
    void compile_expression(Expression const& expression);
    void compile_instruction(Instruction const& instruction);

private:
    LLVMCompiler& m_compiler;
    llvm::Module& m_module;
    llvm::Function& m_function;
    llvm::IRBuilder<> m_builder;
    Module const& m_wasm_module;

    WasmBasicBlock m_current_block;
    Vector<llvm::AllocaInst*> m_locals;
    Vector<ControlFrame> m_control_stack;

    size_t m_next_block { 0 };

    llvm::Value* get_memory_pointer(Instruction::MemoryArgument const& memory_argument, llvm::Value* base);
};

class LLVMCompiler {
public:
    static ErrorOr<NonnullOwnPtr<LLVMCompiler>> create();

    // Compile a Wasm module to native code
    ErrorOr<void*> compile_module(Module const&);

    [[nodiscard]] llvm::LLVMContext& context() { return *m_context; }

private:
    friend class LLVMFunctionGenerator;

    LLVMCompiler(std::unique_ptr<llvm::orc::LLJIT>);

    // Type conversion
    llvm::Type* wasm_type_to_llvm(ValueType);
    llvm::FunctionType* wasm_func_type_to_llvm(FunctionType const&);

    // Compilation phases
    void compile_global_section(llvm::Module&, Module const&);
    void compile_functions(llvm::Module&, Module const&);

    Vector<FunctionDeclaration> m_function_declarations;
    Vector<NonnullOwnPtr<llvm::GlobalVariable>> m_globals;

    // Memory - must outlive JIT code execution
    Vector<MemoryAddress> m_memories;
    Store m_store;

    std::unique_ptr<llvm::orc::LLJIT> m_jit;
    std::unique_ptr<llvm::LLVMContext> m_context;
};

}
