/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

  #include "LLVMCompiler.h"
  #include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
  #include <llvm/IR/Verifier.h>
  #include <llvm/Support/TargetSelect.h>

namespace Wasm {

ErrorOr<NonnullOwnPtr<LLVMCompiler>> LLVMCompiler::create()
{
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  auto jit = llvm::orc::LLJITBuilder().create();
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

  // Wasm parameters
  for (auto const& param : type.parameters())
      param_types.append(wasm_type_to_llvm(param));

  // Return type (simplified: single return or void)
  llvm::Type* return_type;
  if (type.results().is_empty()) {
      return_type = llvm::Type::getVoidTy(*m_context);
  } else if (type.results().size() == 1) {
      return_type = wasm_type_to_llvm(type.results()[0]);
  } else {
      // Multiple returns: use a struct
      Vector<llvm::Type*> result_types;
      for (auto const& result : type.results())
          result_types.append(wasm_type_to_llvm(result));
      return_type = llvm::StructType::get(*m_context, { result_types.data(), result_types.size() });
  }

  return llvm::FunctionType::get(return_type, { param_types.data(), param_types.size() }, false);
}

ErrorOr<void*> LLVMCompiler::compile_module(Module const& module)
{
  std::unique_ptr<llvm::Module> llvm_module(new llvm::Module("wasm_module", *m_context));
  llvm::IRBuilder<> builder(*m_context);

  CompilationContext ctx {
      .module = module,
  };

    for (auto& memory : module.memory_section().memories()) {
        auto memory_address = ctx.store.allocate(memory.type());
        if (!memory_address.has_value())
            return Error::from_string_literal("Failed to allocate memory");

        ctx.memories.append(memory_address.release_value());
    }

  compile_global_section(*llvm_module, builder, ctx);
  compile_functions(*llvm_module, builder, ctx);

  // JIT compile
  auto tsm = llvm::orc::ThreadSafeModule(move(llvm_module), move(m_context));
  if (auto err = m_jit->addIRModule(move(tsm)))
      return Error::from_string_literal("Failed to add module to JIT");

  auto symbol = m_jit->lookup("wasm_func");
  if (!symbol)
      return Error::from_string_literal("Failed to find compiled function");

  return reinterpret_cast<void*>(symbol->getValue());
}

void LLVMCompiler::compile_functions(CodeSection const& code_section, FunctionSection const& function_section, TypeSection const& type_section, llvm::Module& llvm_module, llvm::IRBuilder<>& builder, CompilationContext& context)
{
    size_t code_index = 0;
    auto const& code_section = context.module.code_section();
    auto const& function_section = context.module.function_section();
    auto const& type_section = context.module.type_section();
    for (auto& code : code_section.functions()) {
        auto function_type_index = function_section.types()[code_index];
        auto& function_type = type_section.types()[function_type_index.value()];
        auto* llvm_function_type = wasm_func_type_to_llvm(function_type);
        auto* llvm_function = llvm::Function::Create(
            llvm_function_type,
            llvm::Function::ExternalLinkage,
            "wasm_func",
            llvm_module
        );

        // Set up compilation context
        context.function = llvm_function;

        // Entry block
        auto* entry = llvm::BasicBlock::Create(*m_context, "entry", llvm_function);
        builder.SetInsertPoint(entry);
        context.current_block = entry;

        // Allocate locals (parameters + local variables)
        size_t param_idx = 0;
        for (auto const& param_type : function_type.parameters()) {
            auto* alloca = builder.CreateAlloca(wasm_type_to_llvm(param_type));
            builder.CreateStore(llvm_function->getArg(param_idx++), alloca);
            context.locals.append(alloca);
        }
        for (auto const& local : code.func().locals()) {
            for (size_t local_index = 0; local_index < local.n(); local_index++) {
                auto* llvm_type = wasm_type_to_llvm(local.type());
                auto* alloca = builder.CreateAlloca(llvm_type);
                builder.CreateStore(llvm::Constant::getNullValue(llvm_type), alloca);
                context.locals.append(alloca);
            }
        }

        compile_expression(code.func().body(), builder, context);

        // Verify the function
        // if (llvm::verifyFunction(*function, &llvm::errs()))
        //     return Error::from_string_literal("LLVM function verification failed");
        ++code_index;
    }
}

void LLVMCompiler::compile_global_section(llvm::Module& llvm_module, llvm::IRBuilder<>& builder, CompilationContext& context)
{
    for (auto& global_entry : context.module.global_section().entries()) {
        compile_expression(global_entry.expression(), builder, context);
        auto* global_initial_value = context.stack.take_last();

        if (auto* constant = llvm::dyn_cast<llvm::Constant>(global_initial_value)) {
            auto* wasm_type = wasm_type_to_llvm(global_entry.type().type());
            context.globals.append(make<llvm::GlobalVariable>(llvm_module, wasm_type, !global_entry.type().is_mutable(), llvm::GlobalValue::InternalLinkage, constant));
        } else {
            VERIFY_NOT_REACHED();
        }
    }
}

void LLVMCompiler::compile_expression(Expression const& expression, llvm::IRBuilder<>& builder, CompilationContext& ctx)
{
    for (auto const& instruction : expression.instructions())
        compile_instruction(instruction, builder, ctx);
}

void LLVMCompiler::compile_instruction(Instruction const& insn, llvm::IRBuilder<>& builder, CompilationContext& ctx)
{
    switch (insn.opcode().value()) {

    // === Constants ===
    case Instructions::i32_const.value(): {
      auto value = insn.arguments().get<i32>();
      ctx.stack.append(builder.getInt32(value));
      break;
    }
    case Instructions::i64_const.value(): {
      auto value = insn.arguments().get<i64>();
      ctx.stack.append(builder.getInt64(value));
      break;
    }
    case Instructions::f32_const.value(): {
      auto value = insn.arguments().get<float>();
      ctx.stack.append(llvm::ConstantFP::get(builder.getFloatTy(), value));
      break;
    }
    case Instructions::f64_const.value(): {
      auto value = insn.arguments().get<double>();
      ctx.stack.append(llvm::ConstantFP::get(builder.getDoubleTy(), value));
      break;
    }

    // === Locals ===
    case Instructions::local_get.value(): {
      auto idx = insn.arguments().get<LocalIndex>().value();
      auto* value = builder.CreateLoad(ctx.locals[idx]->getAllocatedType(), ctx.locals[idx]);
      ctx.stack.append(value);
      break;
    }
    case Instructions::local_set.value(): {
      auto idx = insn.arguments().get<LocalIndex>().value();
      auto* value = ctx.stack.take_last();
      builder.CreateStore(value, ctx.locals[idx]);
      break;
    }
    case Instructions::local_tee.value(): {
      auto idx = insn.arguments().get<LocalIndex>().value();
      auto* value = ctx.stack.last();
      builder.CreateStore(value, ctx.locals[idx]);
      break;
    }

    case Instructions::global_get.value(): {
        auto global_index = insn.arguments().get<GlobalIndex>();
        auto* global = ctx.globals[global_index.value()].ptr();
        auto* value = builder.CreateLoad(global->getValueType(), global);
        ctx.stack.append(value);
        break;
    }

    // === Arithmetic (i32) ===
    case Instructions::i32_add.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      ctx.stack.append(builder.CreateAdd(lhs, rhs));
      break;
    }
    case Instructions::i32_sub.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      ctx.stack.append(builder.CreateSub(lhs, rhs));
      break;
    }
    case Instructions::i32_mul.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      ctx.stack.append(builder.CreateMul(lhs, rhs));
      break;
    }
    case Instructions::i32_divs.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      // TODO: Trap on division by zero
      ctx.stack.append(builder.CreateSDiv(lhs, rhs));
      break;
    }
    case Instructions::i32_divu.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      ctx.stack.append(builder.CreateUDiv(lhs, rhs));
      break;
    }
    case Instructions::i32_and.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      ctx.stack.append(builder.CreateAnd(lhs, rhs));
      break;
    }
    case Instructions::i32_or.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      ctx.stack.append(builder.CreateOr(lhs, rhs));
      break;
    }
    case Instructions::i32_xor.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      ctx.stack.append(builder.CreateXor(lhs, rhs));
      break;
    }
    case Instructions::i32_shl.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      // Wasm: shift amount is masked to 5 bits
      auto* masked = builder.CreateAnd(rhs, builder.getInt32(31));
      ctx.stack.append(builder.CreateShl(lhs, masked));
      break;
    }
    case Instructions::i32_shrs.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      auto* masked = builder.CreateAnd(rhs, builder.getInt32(31));
      ctx.stack.append(builder.CreateAShr(lhs, masked));
      break;
    }
    case Instructions::i32_shru.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      auto* masked = builder.CreateAnd(rhs, builder.getInt32(31));
      ctx.stack.append(builder.CreateLShr(lhs, masked));
      break;
    }

    // === Comparisons (i32) ===
    case Instructions::i32_eqz.value(): {
      auto* value = ctx.stack.take_last();
      auto* result = builder.CreateICmpEQ(value, builder.getInt32(0));
      ctx.stack.append(builder.CreateZExt(result, builder.getInt32Ty()));
      break;
    }
    case Instructions::i32_eq.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      auto* result = builder.CreateICmpEQ(lhs, rhs);
      ctx.stack.append(builder.CreateZExt(result, builder.getInt32Ty()));
      break;
    }
    case Instructions::i32_lts.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      auto* result = builder.CreateICmpSLT(lhs, rhs);
      ctx.stack.append(builder.CreateZExt(result, builder.getInt32Ty()));
      break;
    }
    case Instructions::i32_ltu.value(): {
      auto* rhs = ctx.stack.take_last();
      auto* lhs = ctx.stack.take_last();
      auto* result = builder.CreateICmpULT(lhs, rhs);
      ctx.stack.append(builder.CreateZExt(result, builder.getInt32Ty()));
      break;
    }

    // === Memory Operations ===
    case Instructions::i32_load.value(): {
      auto const& arg = insn.arguments().get<Instruction::MemoryArgument>();
        auto& address = ctx.memories.data()[arg.memory_index.value()];
        auto* memory = ctx.store.get(address);
      auto* offset_val = ctx.stack.take_last();
      auto* effective_addr = builder.CreateAdd(offset_val, builder.getInt64(arg.offset));
      auto* addr_i64 = builder.CreateZExt(effective_addr, builder.getInt64Ty());

      // Bounds check (simplified - should trap on OOB)
      // auto* in_bounds = builder.CreateICmpULT(addr_i64, ctx.memory_size);

        // Convert the C++ pointer to an LLVM constant pointer
        auto* memory_base_int = builder.getInt64(reinterpret_cast<uintptr_t>(memory->data()));
        auto* memory_base_ptr = builder.CreateIntToPtr(memory_base_int, builder.getPtrTy());

        memory->data();
      auto* ptr = builder.CreateGEP(builder.getInt8Ty(), memory->data(), addr_i64);
      auto* typed_ptr = builder.CreateBitCast(ptr, llvm::PointerType::getInt32Ty(*m_context));
      ctx.stack.append(builder.CreateLoad(builder.getInt32Ty(), typed_ptr));
      break;
    }
    case Instructions::i32_store.value(): {
      auto const& arg = insn.arguments().get<Instruction::MemoryArgument>();
        auto& address = ctx.memories.data()[arg.memory_index.value()];
        auto* memory = ctx.store.get(address);
      auto* value = ctx.stack.take_last();
      auto* offset_val = ctx.stack.take_last();
      auto* effective_addr = builder.CreateAdd(offset_val, builder.getInt32(arg.offset));
      auto* addr_i64 = builder.CreateZExt(effective_addr, builder.getInt64Ty());

      auto* ptr = builder.CreateGEP(builder.getInt8Ty(), memory->data(), addr_i64);
      auto* typed_ptr = builder.CreateBitCast(ptr, llvm::PointerType::getInt32Ty(*m_context));
      builder.CreateStore(value, typed_ptr);
      break;
    }

    // === Control Flow ===
    case Instructions::block.value(): {
      auto* continuation = llvm::BasicBlock::Create(*m_context, "block_end", ctx.function);

      ctx.control_stack.append({
          .continuation = continuation,
          .else_block = nullptr,
          .stack_height = ctx.stack.size(),
      });
      break;
    }
    case Instructions::loop.value(): {
      auto* loop_header = llvm::BasicBlock::Create(*m_context, "loop", ctx.function);

      builder.CreateBr(loop_header);
      builder.SetInsertPoint(loop_header);
      ctx.current_block = loop_header;

      ctx.control_stack.append({
          .continuation = loop_header,  // For loops, br targets the header
          .else_block = nullptr,
          .stack_height = ctx.stack.size(),
      });
      break;
    }
    case Instructions::if_.value(): {
      auto* condition = ctx.stack.take_last();
      auto* cond_bool = builder.CreateICmpNE(condition, builder.getInt32(0));

      auto* then_block = llvm::BasicBlock::Create(*m_context, "then", ctx.function);
      auto* else_block = llvm::BasicBlock::Create(*m_context, "else", ctx.function);
      auto* continuation = llvm::BasicBlock::Create(*m_context, "if_end", ctx.function);

      builder.CreateCondBr(cond_bool, then_block, else_block);
      builder.SetInsertPoint(then_block);
      ctx.current_block = then_block;

      ctx.control_stack.append({
          .continuation = continuation,
          .else_block = else_block,
          .stack_height = ctx.stack.size(),
      });
      break;
    }
    case Instructions::structured_else.value(): {
      auto& frame = ctx.control_stack.last();

      // Branch from end of then-block to continuation
      builder.CreateBr(frame.continuation);

      // Switch to else block
      builder.SetInsertPoint(frame.else_block);
      ctx.current_block = frame.else_block;

      // Reset stack to block entry height
      ctx.stack.shrink(frame.stack_height);
      break;
    }
    case Instructions::structured_end.value(): {
      if (ctx.control_stack.is_empty()) {
          // Function end - create return
          if (ctx.stack.is_empty()) {
              builder.CreateRetVoid();
          } else {
              builder.CreateRet(ctx.stack.take_last());
          }
      } else {
          auto frame = ctx.control_stack.take_last();

          // If there's an else block that was never used, fill it
          if (frame.else_block && frame.else_block->empty()) {
              auto* current = builder.GetInsertBlock();
              builder.SetInsertPoint(frame.else_block);
              builder.CreateBr(frame.continuation);
              builder.SetInsertPoint(current);
          }

          builder.CreateBr(frame.continuation);
          builder.SetInsertPoint(frame.continuation);
          ctx.current_block = frame.continuation;
      }
      break;
    }
    case Instructions::br.value(): {
      auto depth = insn.arguments().get<LabelIndex>().value();
      auto& frame = ctx.control_stack[ctx.control_stack.size() - 1 - depth];
      builder.CreateBr(frame.continuation);

      // Create unreachable block for subsequent instructions
      auto* unreachable = llvm::BasicBlock::Create(*m_context, "unreachable", ctx.function);
      builder.SetInsertPoint(unreachable);
      ctx.current_block = unreachable;
      break;
    }
    case Instructions::br_if.value(): {
      auto depth = insn.arguments().get<LabelIndex>().value();
      auto& frame = ctx.control_stack[ctx.control_stack.size() - 1 - depth];

      auto* condition = ctx.stack.take_last();
      auto* cond_bool = builder.CreateICmpNE(condition, builder.getInt32(0));

      auto* continue_block = llvm::BasicBlock::Create(*m_context, "br_if_continue", ctx.function);
      builder.CreateCondBr(cond_bool, frame.continuation, continue_block);
      builder.SetInsertPoint(continue_block);
      ctx.current_block = continue_block;
      break;
    }
    case Instructions::return_.value(): {
      if (ctx.stack.is_empty()) {
          builder.CreateRetVoid();
      } else {
          builder.CreateRet(ctx.stack.take_last());
      }

      auto* unreachable = llvm::BasicBlock::Create(*m_context, "unreachable", ctx.function);
      builder.SetInsertPoint(unreachable);
      ctx.current_block = unreachable;
      break;
    }

    // === Misc ===
    case Instructions::drop.value(): {
      ctx.stack.take_last();
      break;
    }
    case Instructions::select.value(): {
      auto* condition = ctx.stack.take_last();
      auto* val2 = ctx.stack.take_last();
      auto* val1 = ctx.stack.take_last();
      auto* cond_bool = builder.CreateICmpNE(condition, builder.getInt32(0));
      ctx.stack.append(builder.CreateSelect(cond_bool, val1, val2));
      break;
    }
    case Instructions::synthetic_end_expression.value():
        builder.CreateRetVoid();
        break;
    case Instructions::nop.value():
      break;
    case Instructions::unreachable.value():
      builder.CreateUnreachable();
      break;

    default:
      // TODO: Implement remaining instructions
      dbgln("Unimplemented instruction: {:#x}", insn.opcode().value());
      break;
    }
}

} // namespace Wasm
