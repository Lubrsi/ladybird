test("compiled V128 values remain compatible with interpreter storage", () => {
    const binary = readBinaryWasmFile("Fixtures/Modules/cranelift-v128-value-plumbing.wasm");
    const module = parseWebAssemblyModule(binary);
    const produce = module.getExport("produce");
    const consume = module.getExport("consume");
    const mergeHigh = module.getExport("merge_high");
    const frameLocalHigh = module.getExport("frame_local_high");
    const tieredHigh = module.getExport("tiered_high");

    if (![produce, consume, mergeHigh, frameLocalHigh, tieredHigh].some(isCraneliftEligible)) return;

    for (const fn of [produce, consume, mergeHigh, frameLocalHigh, tieredHigh]) {
        expect(isCraneliftEligible(fn)).toBe(true);
        expect(isCraneliftCompiled(fn)).toBe(true);
    }

    expect(module.invoke(module.getExport("produce_low"))).toBe(0x0123456789abcdefn);
    expect(module.invoke(module.getExport("produce_high"))).toBe(0x76543210fedcba98n);
    expect(module.invoke(module.getExport("consume_high"))).toBe(0x1020304050607080n);
    expect(module.invoke(mergeHigh, 1)).toBe(0x2222222222222222n);
    expect(module.invoke(mergeHigh, 0)).toBe(0x4444444444444444n);
    expect(module.invoke(frameLocalHigh)).toBe(0x0123456789abcdefn);
    expect(module.invoke(tieredHigh, 2)).toBe(0x6666666666666666n);
});
