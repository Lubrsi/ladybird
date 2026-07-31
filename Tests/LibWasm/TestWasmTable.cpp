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
    EXPECT(instance->grow(store, 3, fill_value));
    EXPECT_EQ(instance->elements().size(), 4u);
    EXPECT_EQ(instance->elements().data(), elements);
    EXPECT(!instance->grow(store, 1, fill_value));
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
    EXPECT(!instance->grow(store, static_cast<u64>(Wasm::Constants::max_allowed_table_size) + 1, fill_value));
    EXPECT_EQ(instance->elements().size(), 0u);

    Wasm::TableType oversized_type {
        Wasm::ValueType(Wasm::ValueType::FunctionReference),
        Wasm::Limits(Wasm::AddressType::I32, static_cast<u64>(Wasm::Constants::max_allowed_table_size) + 1)
    };
    EXPECT(!store.allocate(oversized_type).has_value());
}

TEST_CASE(table_tracks_callable_metadata)
{
    Wasm::Store store;
    Wasm::FunctionType empty_function_type { {}, {} };
    auto empty_function_address = store.allocate(Wasm::HostFunction {
        [](Wasm::Configuration&, Span<Wasm::Value>) -> Wasm::Result {
            return Wasm::Result { Vector<Wasm::Value> {} };
        },
        empty_function_type,
        "empty" });
    VERIFY(empty_function_address.has_value());

    Wasm::FunctionType typed_function_type {
        { Wasm::ValueType(Wasm::ValueType::I32) },
        { Wasm::ValueType(Wasm::ValueType::I64) }
    };
    auto typed_function_address = store.allocate(Wasm::HostFunction {
        [](Wasm::Configuration&, Span<Wasm::Value>) -> Wasm::Result {
            return Wasm::Result { Vector<Wasm::Value> { Wasm::Value(i64 { 0 }) } };
        },
        typed_function_type,
        "typed" });
    VERIFY(typed_function_address.has_value());

    Wasm::TableType table_type {
        Wasm::ValueType(Wasm::ValueType::FunctionReference),
        Wasm::Limits(Wasm::AddressType::I32, 2, 3)
    };
    auto table_address = store.allocate(table_type);
    VERIFY(table_address.has_value());
    auto* table = store.get(*table_address);
    VERIFY(table);

    Wasm::Reference empty_function_reference { Wasm::Reference::Func { *empty_function_address, nullptr } };
    Wasm::Reference typed_function_reference { Wasm::Reference::Func { *typed_function_address, nullptr } };
    table->set_element(store, 0, empty_function_reference);
    table->set_element(store, 1, typed_function_reference);

    EXPECT_EQ(table->callable_at(0), store.get_callable(*empty_function_address));
    EXPECT_EQ(table->callable_at(0)->address, *empty_function_address);
    EXPECT_EQ(table->callable_at(0)->defined_type, store.get_callable(*empty_function_address)->defined_type);
    EXPECT_EQ(table->callable_at(0)->parameter_count, 0u);
    EXPECT_EQ(table->callable_at(0)->result_count, 0u);

    EXPECT_EQ(table->callable_at(1), store.get_callable(*typed_function_address));
    EXPECT_EQ(table->callable_at(1)->address, *typed_function_address);
    EXPECT_EQ(table->callable_at(1)->defined_type, store.get_callable(*typed_function_address)->defined_type);
    EXPECT_EQ(table->callable_at(1)->parameter_count, 1u);
    EXPECT_EQ(table->callable_at(1)->result_count, 1u);
    EXPECT_NE(table->callable_at(0)->defined_type, table->callable_at(1)->defined_type);

    EXPECT(table->grow(store, 1, empty_function_reference));
    EXPECT_EQ(table->callable_at(2), store.get_callable(*empty_function_address));

    table->set_element(store, 0, Wasm::Reference { Wasm::Reference::Null { table_type.element_type() } });
    EXPECT_EQ(table->callable_at(0), nullptr);
}
