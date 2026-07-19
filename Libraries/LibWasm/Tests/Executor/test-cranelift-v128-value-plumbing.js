test("compiled V128 values remain compatible with interpreter storage", () => {
    const binary = readBinaryWasmFile("Fixtures/Modules/cranelift-v128-value-plumbing.wasm");
    const module = parseWebAssemblyModule(binary);
    const produce = module.getExport("produce");
    const consume = module.getExport("consume");
    const mergeHigh = module.getExport("merge_high");
    const frameLocalHigh = module.getExport("frame_local_high");
    const bitwiseAndHigh = module.getExport("bitwise_and_high");
    const bitwiseOrHigh = module.getExport("bitwise_or_high");
    const bitwiseXorHigh = module.getExport("bitwise_xor_high");
    const bitwiseNotHigh = module.getExport("bitwise_not_high");
    const tieredHigh = module.getExport("tiered_high");

    const compiledFunctions = [
        produce,
        consume,
        mergeHigh,
        frameLocalHigh,
        bitwiseAndHigh,
        bitwiseOrHigh,
        bitwiseXorHigh,
        bitwiseNotHigh,
        tieredHigh,
    ];

    if (!compiledFunctions.some(isCraneliftEligible)) return;

    for (const fn of compiledFunctions) {
        expect(isCraneliftEligible(fn)).toBe(true);
        expect(isCraneliftCompiled(fn)).toBe(true);
    }

    expect(module.invoke(module.getExport("produce_low"))).toBe(0x0123456789abcdefn);
    expect(module.invoke(module.getExport("produce_high"))).toBe(0x76543210fedcba98n);
    expect(module.invoke(module.getExport("consume_high"))).toBe(0x1020304050607080n);
    expect(module.invoke(mergeHigh, 1)).toBe(0x2222222222222222n);
    expect(module.invoke(mergeHigh, 0)).toBe(0x4444444444444444n);
    expect(module.invoke(frameLocalHigh)).toBe(0x0123456789abcdefn);
    expect(module.invoke(bitwiseAndHigh)).toBe(0x030c50a00a500c30n);
    expect(module.invoke(bitwiseOrHigh)).toBe(0x3fcff5faaff5cff3n);
    expect(module.invoke(bitwiseXorHigh)).toBe(0x3cc3a55aa5a5c3c3n);
    expect(module.invoke(bitwiseNotHigh)).toBe(-0x0f0ff0f00ff00ff1n);
    expect(module.invoke(tieredHigh, 2)).toBe(0x6666666666666666n);
});
