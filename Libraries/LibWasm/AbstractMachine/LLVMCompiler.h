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

// Holds state during compilation of a single function
struct CompilationContext {
  llvm::Function* function;
  llvm::BasicBlock* current_block;

  // Wasm value stack -> LLVM values
  Vector<llvm::Value*> stack;

  // Local variables (as LLVM allocas)
  Vector<llvm::AllocaInst*> locals;
  Vector<NonnullOwnPtr<llvm::GlobalVariable>> globals;

    Module const& module;
    Vector<MemoryAddress> memories;
    Store store;

  // For structured control flow
  struct ControlFrame {
    llvm::BasicBlock* continuation;  // Where to branch on `end`
    llvm::BasicBlock* else_block;    // For `if` instructions
    size_t stack_height;             // Stack height at entry
  };
  Vector<ControlFrame> control_stack;
};

class LLVMCompiler {
public:
  static ErrorOr<NonnullOwnPtr<LLVMCompiler>> create();

  // Compile a Wasm module to native code
  ErrorOr<void*> compile_module(Module const&);

private:
  LLVMCompiler(std::unique_ptr<llvm::orc::LLJIT>);

  // Type conversion
  llvm::Type* wasm_type_to_llvm(ValueType);
  llvm::FunctionType* wasm_func_type_to_llvm(FunctionType const&);

  // Compilation helpers
  void compile_functions(llvm::Module&, llvm::IRBuilder<>&, CompilationContext&);
  void compile_global_section(llvm::Module&, llvm::IRBuilder<>&, CompilationContext&);
  void compile_expression(Expression const&, llvm::IRBuilder<>&, CompilationContext&);
  void compile_instruction(Instruction const&, llvm::IRBuilder<>&, CompilationContext&);

  std::unique_ptr<llvm::orc::LLJIT> m_jit;
  std::unique_ptr<llvm::LLVMContext> m_context;
};

}
