test("V128 value plumbing remains interpreter-compatible", () => {
    const binary = readBinaryWasmFile("Fixtures/Modules/cranelift-v128-value-plumbing.wasm");
    const module = parseWebAssemblyModule(binary);

    expect(module.invoke(module.getExport("produce_low"))).toBe(0x0123456789abcdefn);
    expect(module.invoke(module.getExport("produce_high"))).toBe(0x76543210fedcba98n);
    expect(module.invoke(module.getExport("consume_high"))).toBe(0x1020304050607080n);
    expect(module.invoke(module.getExport("merge_high"), 1)).toBe(0x2222222222222222n);
    expect(module.invoke(module.getExport("merge_high"), 0)).toBe(0x4444444444444444n);
    expect(module.invoke(module.getExport("frame_local_high"))).toBe(0x0123456789abcdefn);
    expect(module.invoke(module.getExport("bitwise_and_high"))).toBe(0x030c50a00a500c30n);
    expect(module.invoke(module.getExport("bitwise_or_high"))).toBe(0x3fcff5faaff5cff3n);
    expect(module.invoke(module.getExport("bitwise_xor_high"))).toBe(0x3cc3a55aa5a5c3c3n);
    expect(module.invoke(module.getExport("bitwise_not_high"))).toBe(-0x0f0ff0f00ff00ff1n);
    expect(module.invoke(module.getExport("tiered_high"), 2)).toBe(0x6666666666666666n);
});
