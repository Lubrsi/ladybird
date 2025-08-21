/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Process.h>
#include <asmjit/core.h>
#include <asmjit/a64.h>

#include <LibMain/Main.h>

typedef int (*Func)(void);

template <typename... Args>
asmjit::FuncNode* compile_function(asmjit::CodeHolder& jit_code, asmjit::a64::Compiler& compiler, Function<void(asmjit::a64::Compiler&)> compile_body, asmjit::TypeId return_type, Args&&... arguments)
{
    asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl, asmjit::FuncSignature::kNoVarArgs, return_type, forward<Args>(arguments)...);
    asmjit::FuncNode* function_node = compiler.addFunc(signature);

    signature.addArg()
    compiler.pac

    compiler.endFunc();
    compiler.finalize();

    return function_node;
}

ErrorOr<int> ladybird_main(Main::Arguments)
{
    asmjit::JitRuntime jit_runtime;
    asmjit::CodeHolder jit_code;
    asmjit::a64::Compiler compiler;
    jit_code.init(jit_runtime.environment(), jit_runtime.cpuFeatures());
    jit_code.attach(&compiler);


    auto local_i32 = compiler.newReg(asmjit::TypeId::kInt32);

    asmjit::InvokeNode* invoke_node { nullptr };
    compiler.ret(local_i32.baseReg());
    compiler.invoke(&invoke_node, )
    compiler.endFunc();
    compiler.finalize();

    Func function;
    asmjit::Error err = jit_runtime.add(&function, &jit_code);

    if (err) {
        dbgln("AsmJit failed: {}", asmjit::DebugUtils::errorAsString(err));
        return 1;
    }

    dbgln("running...");
    int result = function();
    dbgln("{}", result);

    jit_runtime.release(function);

    return 0;
}
