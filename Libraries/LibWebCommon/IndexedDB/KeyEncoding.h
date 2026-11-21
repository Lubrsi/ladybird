/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibWebCommon/Export.h>

namespace Web::IndexedDB {

// A key that another process sent, in the forms that a database stores.
struct CheckedKey {
    // Compares bytewise as the key compares.
    ByteBuffer sort_form;
    // The key as it is read back, for a key that holds a -0.
    Optional<ByteBuffer> exact_form;
};

enum class KeyCheckError : u8 {
    InvalidKey,
    OutOfMemory,
};

WEBCOMMON_API ErrorOr<CheckedKey, KeyCheckError> check_received_key(ReadonlyBytes);

// https://w3c.github.io/IndexedDB/#key-generator-construct
// The key that a key generator makes of the number, as a record's key is stored.
WEBCOMMON_API Optional<Array<u8, 9>> encoded_generated_key(u64 number);

WEBCOMMON_API Optional<double> number_of_key(ReadonlyBytes checked_key);

}
