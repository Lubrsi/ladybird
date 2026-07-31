/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>

TEST_CASE(table_instance_address_remains_stable)
{
    Wasm::Store store;
    Wasm::TableType type {
        Wasm::ValueType(Wasm::ValueType::FunctionReference),
        Wasm::Limits(Wasm::AddressType::I32, 0, 1)
    };

    auto first_address = store.allocate(type);
    VERIFY(first_address.has_value());
    auto* first_instance = store.get(*first_address);
    VERIFY(first_instance);

    for (size_t i = 0; i < 1024; ++i)
        VERIFY(store.allocate(type).has_value());

    EXPECT_EQ(store.get(*first_address), first_instance);
}
