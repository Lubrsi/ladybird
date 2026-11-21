/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWebCommon's IndexedDB/KeyEncoding reaches the key encoding.

use core::ffi::c_void;

use ak::Span;

use super::key_encoding::{GeneratedKey, KeyCheckError, check_received_key, number_of_key};
use crate::rust_panic::abort_on_panic;

#[repr(u8)]
pub enum KeyCheckOutcome {
    Valid,
    Invalid,
    OutOfMemory,
}

/// Takes the forms of a checked key, of which the exact form is only lent with `has_exact_form`. Returns whether it
/// could keep them.
type OnCheckedKey = unsafe extern "C" fn(
    ctx: *mut c_void,
    sort_form: Span<'_, u8>,
    has_exact_form: bool,
    exact_form: Span<'_, u8>,
) -> bool;

/// Checks a key that another process sent, and calls `on_checked_key` with its forms if it is one.
///
/// # Safety
///
/// `bytes` must be lent for the call, and `on_checked_key` must be a callback for `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn indexed_db_check_received_key(
    bytes: Span<'_, u8>,
    ctx: *mut c_void,
    on_checked_key: OnCheckedKey,
) -> KeyCheckOutcome {
    abort_on_panic(|| {
        let checked_key = match check_received_key(bytes.as_slice(), None) {
            Ok(checked_key) => checked_key,
            Err(KeyCheckError::Invalid(_)) => return KeyCheckOutcome::Invalid,
            Err(KeyCheckError::OutOfMemory) => return KeyCheckOutcome::OutOfMemory,
        };

        let exact_form = checked_key.exact_form.as_deref();
        // SAFETY: The caller passes a callback for `ctx`.
        let was_kept = unsafe {
            on_checked_key(
                ctx,
                Span::new(&checked_key.sort_form),
                exact_form.is_some(),
                Span::new(exact_form.unwrap_or_default()),
            )
        };
        if was_kept {
            KeyCheckOutcome::Valid
        } else {
            KeyCheckOutcome::OutOfMemory
        }
    })
}

/// A generated key as a record's key is stored, if a key generator can make the number.
#[repr(C)]
pub struct EncodedGeneratedKey {
    pub is_a_generated_key: bool,
    pub bytes: [u8; 9],
}

#[unsafe(no_mangle)]
pub extern "C" fn indexed_db_encode_generated_key(number: u64) -> EncodedGeneratedKey {
    abort_on_panic(|| match GeneratedKey::new(number) {
        Some(generated_key) => EncodedGeneratedKey {
            is_a_generated_key: true,
            bytes: generated_key.encoded(),
        },
        None => EncodedGeneratedKey {
            is_a_generated_key: false,
            bytes: [0; 9],
        },
    })
}

/// The number that a checked key holds, if the key is a number.
#[repr(C)]
pub struct NumberOfKey {
    pub is_a_number: bool,
    pub number: f64,
}

/// # Safety
///
/// `checked_key` must be lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn indexed_db_number_of_key(checked_key: Span<'_, u8>) -> NumberOfKey {
    abort_on_panic(|| match number_of_key(checked_key.as_slice()) {
        Some(number) => NumberOfKey {
            is_a_number: true,
            number,
        },
        None => NumberOfKey {
            is_a_number: false,
            number: 0.0,
        },
    })
}
