/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/IndexedDB/KeyEncoding.h>
#include <LibWebCommon/RustFFI.h>

namespace Web::IndexedDB {

ErrorOr<CheckedKey, KeyCheckError> check_received_key(ReadonlyBytes bytes, Optional<u64> injected_generated_key)
{
    Optional<CheckedKey> checked_key;

    auto outcome = FFI::indexed_db_check_received_key(bytes, injected_generated_key.has_value(), injected_generated_key.value_or(0), &checked_key, [](void* ctx, ReadonlyBytes sort_form, bool has_exact_form, ReadonlyBytes exact_form) {
        auto sort_form_copy = ByteBuffer::copy(sort_form);
        if (sort_form_copy.is_error())
            return false;

        Optional<ByteBuffer> exact_form_copy;
        if (has_exact_form) {
            auto copy = ByteBuffer::copy(exact_form);
            if (copy.is_error())
                return false;
            exact_form_copy = copy.release_value();
        }

        *static_cast<Optional<CheckedKey>*>(ctx) = CheckedKey { sort_form_copy.release_value(), move(exact_form_copy) };
        return true;
    });

    switch (outcome) {
    case FFI::KeyCheckOutcome::Valid:
        return checked_key.release_value();
    case FFI::KeyCheckOutcome::Invalid:
        return KeyCheckError::InvalidKey;
    case FFI::KeyCheckOutcome::OutOfMemory:
        return KeyCheckError::OutOfMemory;
    }
    VERIFY_NOT_REACHED();
}

Optional<Array<u8, 9>> encoded_generated_key(u64 number)
{
    auto encoded = FFI::indexed_db_encode_generated_key(number);
    if (!encoded.is_a_generated_key)
        return {};

    Array<u8, 9> bytes;
    ReadonlyBytes { encoded.bytes, sizeof(encoded.bytes) }.copy_to(bytes.span());
    return bytes;
}

Optional<double> number_of_key(ReadonlyBytes checked_key)
{
    auto number = FFI::indexed_db_number_of_key(checked_key);
    if (!number.is_a_number)
        return {};
    return number.number;
}

}
