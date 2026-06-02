/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StructuredDeserializeFuzzerCommon.h"
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Forward.h>
#include <LibWeb/HTML/StructuredSerialize.h>

// The transfer surface starts at the raw SerializedTransferRecord envelope.

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);

    auto& realm = StructuredDeserializeFuzzer::realm();

    FixedMemoryStream stream { ReadonlyBytes { data, size } };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    if (auto record = decoder.decode<Web::HTML::SerializedTransferRecord>(); !record.is_error()) {
        auto transfer_record = record.release_value();
        (void)Web::HTML::structured_deserialize_with_transfer(transfer_record, realm);
    }

    StructuredDeserializeFuzzer::collect_garbage();
    return 0;
}
