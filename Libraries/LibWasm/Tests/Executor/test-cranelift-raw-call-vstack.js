test("Cranelift preserves the virtual stack across raw calls", () => {
    const binary = readBinaryWasmFile("Fixtures/Modules/cranelift-raw-call-vstack.wasm");
    const module = parseWebAssemblyModule(binary);
    const functions = [
        "direct",
        "void_call",
        "indirect",
        "indirect_pair",
        "drive",
        "indirect_index",
        "indirect_void",
    ].map(name => [name, module.getExport(name)]);

    if (functions.every(([, fn]) => !isCraneliftEligible(fn))) return;

    for (const [name, fn] of functions) {
        if (!isCraneliftEligible(fn)) throw new Error(`${name} is not Cranelift-eligible`);
        if (!isCraneliftCompiled(fn)) throw new Error(`${name} did not compile with Cranelift`);
    }

    expect(module.invoke(functions[0][1], 1, 5)).toBe(1125);
    expect(module.invoke(functions[0][1], 0, 5)).toBe(1007);
    expect(module.invoke(functions[1][1], 42)).toBe(77);
    expect(module.invoke(module.getExport("read_last"))).toBe(42);
    expect(module.invoke(functions[2][1], 21)).toBe(1042);
    expect(module.invoke(functions[3][1], 21)).toBe(1043);
    expect(module.invoke(functions[4][1], 25)).toBe(3000);
    expect(module.invoke(functions[5][1], 21, 0)).toBe(42);
    expect(() => module.invoke(functions[5][1], 21, 1)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Indirect call type mismatch"
    );
    expect(() => module.invoke(functions[5][1], 21, 2)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Table element is not a function reference"
    );
    expect(() => module.invoke(functions[5][1], 21, 4)).toThrowWithMessage(
        TypeError,
        "Execution trapped: Table index out of bounds"
    );
    expect(module.invoke(functions[5][1], 21, 0)).toBe(42);
    expect(module.invoke(functions[6][1], 1234)).toBe(1234);
});
