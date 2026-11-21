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

// A key in the forms that a database stores.
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

// A key generator's number is given with an index key of the record that the number was generated for and injected
// into. Such a key may hold markers, which the number takes the place of. The number has to be one that a key
// generator makes.
WEBCOMMON_API ErrorOr<CheckedKey, KeyCheckError> check_received_key(ReadonlyBytes, Optional<u64> injected_generated_key);

// https://w3c.github.io/IndexedDB/#key-generator-construct
// The key that a key generator makes of the number, as a record's key is stored.
WEBCOMMON_API Optional<Array<u8, 9>> encoded_generated_key(u64 number);

WEBCOMMON_API Optional<double> number_of_key(ReadonlyBytes checked_key);

}
