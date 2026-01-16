find_package(LLVM CONFIG REQUIRED)

# Find the libraries that correspond to the LLVM components that we wish to use
llvm_map_components_to_libnames(llvm_libs Support Core IRReader OrcJIT AArch64)

set(SOURCES
    AbstractMachine/AbstractMachine.cpp
    AbstractMachine/BytecodeInterpreter.cpp
    AbstractMachine/Configuration.cpp
    AbstractMachine/LLVMCompiler.cpp
    AbstractMachine/Validator.cpp
    Parser/Parser.cpp
    Printer/Printer.cpp
)

# FIXME: Add Windows support
if (NOT WIN32)
    list(APPEND SOURCES WASI/Wasi.cpp)
endif()

ladybird_lib(LibWasm wasm EXPLICIT_SYMBOL_EXPORT)
target_include_directories(LibWasm PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(LibWasm PRIVATE LibCore ${llvm_libs})

set_source_files_properties(AbstractMachine/BytecodeInterpreter.cpp PROPERTIES COMPILE_OPTIONS "-fomit-frame-pointer")
#target_compile_options(LibWasm PRIVATE -ggdb3)

include(wasm_spec_tests)
