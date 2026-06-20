// A caller operand the allocator keeps live in a register across a call must survive the call.

test("store address held in a register across two calls is preserved", () => {
    const bin = readBinaryWasmFile("Fixtures/Modules/register-clobber-across-calls.wasm");
    // Pin the bytecode interpreter: the callee reuses the register file, and only the interpreter
    // (not Cranelift-compiled code) was failing to preserve the caller's registers across the call.
    const module = parseWebAssemblyModuleInterpreted(bin);
    const fn = module.getExport("store_value");

    // The fixture stores 1230 through a constant address held live across two register-heavy calls,
    // then reads it back. A clobbered address makes the readback wrong or traps out of bounds.
    expect(module.invoke(fn)).toBe(1230);
});
