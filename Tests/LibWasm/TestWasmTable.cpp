/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/Constants.h>

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

TEST_CASE(table_elements_remain_stable_across_growth)
{
    Wasm::Store store;
    Wasm::TableType type {
        Wasm::ValueType(Wasm::ValueType::FunctionReference),
        Wasm::Limits(Wasm::AddressType::I32, 1, 4)
    };

    auto address = store.allocate(type);
    VERIFY(address.has_value());
    auto* instance = store.get(*address);
    VERIFY(instance);
    auto* elements = instance->elements().data();

    Wasm::Reference fill_value { Wasm::Reference::Null { type.element_type() } };
    EXPECT(instance->grow(3, fill_value));
    EXPECT_EQ(instance->elements().size(), 4u);
    EXPECT_EQ(instance->elements().data(), elements);
    EXPECT(!instance->grow(1, fill_value));
    EXPECT_EQ(instance->elements().size(), 4u);
    EXPECT_EQ(instance->elements().data(), elements);
}

TEST_CASE(table_growth_obeys_implementation_limit)
{
    Wasm::Store store;
    Wasm::TableType type {
        Wasm::ValueType(Wasm::ValueType::FunctionReference),
        Wasm::Limits(Wasm::AddressType::I32, 0)
    };

    auto address = store.allocate(type);
    VERIFY(address.has_value());
    auto* instance = store.get(*address);
    VERIFY(instance);

    Wasm::Reference fill_value { Wasm::Reference::Null { type.element_type() } };
    EXPECT(!instance->grow(static_cast<u64>(Wasm::Constants::max_allowed_table_size) + 1, fill_value));
    EXPECT_EQ(instance->elements().size(), 0u);

    Wasm::TableType oversized_type {
        Wasm::ValueType(Wasm::ValueType::FunctionReference),
        Wasm::Limits(Wasm::AddressType::I32, static_cast<u64>(Wasm::Constants::max_allowed_table_size) + 1)
    };
    EXPECT(!store.allocate(oversized_type).has_value());
}
