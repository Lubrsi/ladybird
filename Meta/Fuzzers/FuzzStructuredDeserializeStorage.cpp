/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StructuredDeserializeFuzzerCommon.h"
#include <AK/ByteBuffer.h>
#include <LibWeb/HTML/StructuredSerialize.h>

// The storage surface must consume a complete "LBSC" record.

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);

    auto& realm = StructuredDeserializeFuzzer::realm();
    auto& vm = realm.vm();

    if (auto buffer = ByteBuffer::copy({ data, size }); !buffer.is_error()) {
        Web::HTML::StorageSerializationRecord record { buffer.release_value() };
        Web::HTML::StructuredSerializeReader reader { record };
        Web::HTML::DeserializationMemory memory;
        (void)Web::HTML::structured_deserialize_internal(vm, reader, realm, memory, Web::HTML::CheckFullyConsumed::Yes);
    }

    StructuredDeserializeFuzzer::collect_garbage();
    return 0;
}
