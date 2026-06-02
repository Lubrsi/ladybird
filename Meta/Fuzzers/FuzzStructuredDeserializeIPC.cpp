/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StructuredDeserializeFuzzerCommon.h"
#include <LibIPC/Message.h>
#include <LibWeb/HTML/StructuredSerialize.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);

    auto& realm = StructuredDeserializeFuzzer::realm();
    auto& vm = realm.vm();

    IPC::MessageDataType message;
    message.append(data, size);
    Web::HTML::IPCSerializationRecord record { move(message) };
    Web::HTML::StructuredSerializeReader reader { record };
    Web::HTML::DeserializationMemory memory;
    (void)Web::HTML::structured_deserialize_internal(vm, reader, realm, memory);

    StructuredDeserializeFuzzer::collect_garbage();
    return 0;
}
