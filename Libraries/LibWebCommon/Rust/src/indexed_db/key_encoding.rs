/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The encoding of keys, https://w3c.github.io/IndexedDB/#key-construct
//!
//! An encoded key is a string of bytes that sorts bytewise as the keys compare
//! (https://w3c.github.io/IndexedDB/#compare-two-keys), so that a database orders and matches keys without decoding
//! them. It starts with a tag for the type of the key, and the tags rise in the order of the types:
//!
//! | Type   | Bytes                                                                                               |
//! | ------ | --------------------------------------------------------------------------------------------------- |
//! | number | `0x10`, then the 8 bytes of the double, most significant first, with the sign bit set if it is not  |
//! |        | negative and every bit flipped if it is                                                             |
//! | date   | `0x20`, then the same 8 bytes for its time value                                                    |
//! | string | `0x30`, then each code unit as 2 bytes, most significant first, then `0x0000`                       |
//! | binary | `0x40`, then the bytes, then `0x00`                                                                 |
//! | array  | `0x50`, then each element encoded in full, then `0x00`                                              |
//!
//! Inside a string, the code unit `0x0000` is written `0x0001 0x0001` and `0x0001` is written `0x0001 0x0002`. Inside
//! binary, the byte `0x00` is written `0x01 0x01` and `0x01` is written `0x01 0x02`. That keeps the end of a string,
//! of binary and of an array below everything that can follow it in a longer one, so a prefix sorts first.
//!
//! A key has two forms. The exact form keeps the sign of -0, which a key is read back with. The sort form writes -0
//! as 0, since the two are the same key, and is the one that is compared.
//!
//! Databases on disk hold these bytes, so a change to them is a change of format.

use std::collections::TryReserveError;

use ak::Utf16StringUnits;

const NUMBER_TAG: u8 = 0x10;
const DATE_TAG: u8 = 0x20;
const STRING_TAG: u8 = 0x30;
const BINARY_TAG: u8 = 0x40;
const ARRAY_TAG: u8 = 0x50;

/// Stands inside an index key that is on its way to the database for the key that the database generates and injects
/// into the value of the record, and the database writes that key in its place. No stored key holds it.
const GENERATED_KEY_MARKER: u8 = 0x11;

/// Ends binary and an array as a byte, and a string as a code unit.
const TERMINATOR: u8 = 0x00;

/// Starts the pair that stands for a `TERMINATOR` or an `ESCAPE` of the key's own.
const ESCAPE: u8 = 0x01;

const SIGN_BIT: u64 = 1 << 63;

/// https://tc39.es/ecma262/#sec-time-values-and-time-range
const MAXIMUM_TIME_VALUE: f64 = 8.64e15;

/// Why bytes are not the encoding of a key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InvalidKey {
    Truncated,
    UnknownTag,
    NotANumber,
    NotATimeValue,
    InvalidEscape,
    TrailingBytes,
    UnexpectedMarker,
}

fn encode_double(value: f64) -> [u8; 8] {
    let bits = value.to_bits();

    // A negative double grows in size as it falls in value, so every bit is flipped to turn its order around.
    let ordered_bits = if bits & SIGN_BIT == 0 { bits | SIGN_BIT } else { !bits };
    ordered_bits.to_be_bytes()
}

fn decode_double(bytes: [u8; 8]) -> f64 {
    let ordered_bits = u64::from_be_bytes(bytes);

    let bits = if ordered_bits & SIGN_BIT == 0 {
        !ordered_bits
    } else {
        ordered_bits & !SIGN_BIT
    };
    f64::from_bits(bits)
}

/// Whether the second half of an escape pair stands for a `TERMINATOR` or an `ESCAPE`.
fn is_escaped(value: u16) -> bool {
    value == u16::from(TERMINATOR) + 1 || value == u16::from(ESCAPE) + 1
}

/// Writes the exact form of a key, part by part. An array is what lies between `begin_array` and `end_array`.
#[derive(Default)]
pub struct KeyEncoder {
    bytes: Vec<u8>,
}

impl KeyEncoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn encode_number(&mut self, value: f64) {
        self.bytes.push(NUMBER_TAG);
        self.bytes.extend_from_slice(&encode_double(value));
    }

    pub fn encode_date(&mut self, value: f64) {
        self.bytes.push(DATE_TAG);
        self.bytes.extend_from_slice(&encode_double(value));
    }

    pub fn encode_string(&mut self, string: Utf16StringUnits<'_>) {
        self.bytes.push(STRING_TAG);

        match string {
            Utf16StringUnits::Ascii(characters) | Utf16StringUnits::Latin1(characters) => {
                for &character in characters {
                    self.append_code_unit(u16::from(character));
                }
            }
            Utf16StringUnits::Utf16(code_units) => {
                for &code_unit in code_units {
                    self.append_code_unit(code_unit);
                }
            }
        }

        self.bytes.extend_from_slice(&[TERMINATOR, TERMINATOR]);
    }

    pub fn encode_binary(&mut self, bytes: &[u8]) {
        self.bytes.push(BINARY_TAG);

        for &byte in bytes {
            if byte == TERMINATOR || byte == ESCAPE {
                self.bytes.extend_from_slice(&[ESCAPE, byte + 1]);
            } else {
                self.bytes.push(byte);
            }
        }

        self.bytes.push(TERMINATOR);
    }

    pub fn begin_array(&mut self) {
        self.bytes.push(ARRAY_TAG);
    }

    pub fn end_array(&mut self) {
        self.bytes.push(TERMINATOR);
    }

    pub fn encode_generated_key_marker(&mut self) {
        self.bytes.push(GENERATED_KEY_MARKER);
    }

    pub fn finish(self) -> Vec<u8> {
        self.bytes
    }

    fn append_code_unit(&mut self, code_unit: u16) {
        if code_unit == u16::from(TERMINATOR) || code_unit == u16::from(ESCAPE) {
            self.bytes.extend_from_slice(&u16::from(ESCAPE).to_be_bytes());
            self.bytes.extend_from_slice(&(code_unit + 1).to_be_bytes());
        } else {
            self.bytes.extend_from_slice(&code_unit.to_be_bytes());
        }
    }
}

/// A part of an encoded key that has been checked and is still encoded.
enum EncodedPart<'a> {
    Number(f64),
    Date(f64),
    /// The code units of a string without the one that ends it.
    String(&'a [u8]),
    /// The bytes of binary without the one that ends it.
    Binary(&'a [u8]),
    ArrayStart,
    ArrayEnd,
    GeneratedKeyMarker,
}

/// Walks the parts of one encoded key, and refuses bytes that `KeyEncoder` cannot have written. It keeps count of the
/// arrays that are open and never recurses, so a key may nest as deep as it likes.
struct Scanner<'a> {
    bytes: &'a [u8],
    position: usize,
    open_arrays: usize,
    is_complete: bool,
}

impl<'a> Scanner<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self {
            bytes,
            position: 0,
            open_arrays: 0,
            is_complete: false,
        }
    }

    /// The next part of the key, or `None` once the key is complete. The last part of a key is only given when nothing
    /// follows it.
    fn next_part(&mut self) -> Result<Option<EncodedPart<'a>>, InvalidKey> {
        if self.is_complete {
            return Ok(None);
        }

        let part = match self.take_byte()? {
            NUMBER_TAG => EncodedPart::Number(self.take_double()?),
            DATE_TAG => EncodedPart::Date(self.take_time_value()?),
            STRING_TAG => EncodedPart::String(self.take_string()?),
            BINARY_TAG => EncodedPart::Binary(self.take_binary()?),
            ARRAY_TAG => {
                self.open_arrays += 1;
                return Ok(Some(EncodedPart::ArrayStart));
            }
            TERMINATOR if self.open_arrays > 0 => {
                self.open_arrays -= 1;
                EncodedPart::ArrayEnd
            }
            GENERATED_KEY_MARKER => EncodedPart::GeneratedKeyMarker,
            _ => return Err(InvalidKey::UnknownTag),
        };

        if self.open_arrays == 0 {
            if self.position != self.bytes.len() {
                return Err(InvalidKey::TrailingBytes);
            }
            self.is_complete = true;
        }
        Ok(Some(part))
    }

    fn take_byte(&mut self) -> Result<u8, InvalidKey> {
        let byte = *self.bytes.get(self.position).ok_or(InvalidKey::Truncated)?;
        self.position += 1;
        Ok(byte)
    }

    fn take_code_unit(&mut self) -> Result<u16, InvalidKey> {
        let bytes = self.bytes[self.position..].first_chunk().ok_or(InvalidKey::Truncated)?;
        self.position += bytes.len();
        Ok(u16::from_be_bytes(*bytes))
    }

    fn take_double(&mut self) -> Result<f64, InvalidKey> {
        let bytes = self.bytes[self.position..].first_chunk().ok_or(InvalidKey::Truncated)?;
        self.position += bytes.len();

        let value = decode_double(*bytes);
        if value.is_nan() {
            return Err(InvalidKey::NotANumber);
        }
        Ok(value)
    }

    /// https://tc39.es/ecma262/#sec-timeclip
    fn take_time_value(&mut self) -> Result<f64, InvalidKey> {
        let value = self.take_double()?;

        // A date key is the time value of a Date, which is a whole number of milliseconds within the time range.
        if value.trunc() != value || value.abs() > MAXIMUM_TIME_VALUE {
            return Err(InvalidKey::NotATimeValue);
        }
        Ok(value)
    }

    fn take_string(&mut self) -> Result<&'a [u8], InvalidKey> {
        let start = self.position;

        loop {
            let end = self.position;
            let code_unit = self.take_code_unit()?;

            if code_unit == u16::from(TERMINATOR) {
                return Ok(&self.bytes[start..end]);
            }
            if code_unit == u16::from(ESCAPE) && !is_escaped(self.take_code_unit()?) {
                return Err(InvalidKey::InvalidEscape);
            }
        }
    }

    fn take_binary(&mut self) -> Result<&'a [u8], InvalidKey> {
        let start = self.position;

        loop {
            let end = self.position;
            let byte = self.take_byte()?;

            if byte == TERMINATOR {
                return Ok(&self.bytes[start..end]);
            }
            if byte == ESCAPE && !is_escaped(u16::from(self.take_byte()?)) {
                return Err(InvalidKey::InvalidEscape);
            }
        }
    }
}

/// A part of a key as `KeyDecoder` reads it. An array is what lies between `ArrayStart` and `ArrayEnd`.
#[derive(Clone, Debug, PartialEq)]
pub enum KeyPart {
    Number(f64),
    Date(f64),
    String(Vec<u16>),
    Binary(Vec<u8>),
    ArrayStart,
    ArrayEnd,
}

/// Reads a stored key part by part.
pub struct KeyDecoder<'a> {
    scanner: Scanner<'a>,
}

impl<'a> KeyDecoder<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        Self {
            scanner: Scanner::new(bytes),
        }
    }

    /// The next part of the key, or `None` at its end.
    pub fn next_part(&mut self) -> Result<Option<KeyPart>, InvalidKey> {
        let Some(part) = self.scanner.next_part()? else {
            return Ok(None);
        };

        Ok(Some(match part {
            EncodedPart::Number(value) => KeyPart::Number(value),
            EncodedPart::Date(value) => KeyPart::Date(value),
            EncodedPart::String(encoded) => KeyPart::String(decode_string(encoded)),
            EncodedPart::Binary(encoded) => KeyPart::Binary(decode_binary(encoded)),
            EncodedPart::ArrayStart => KeyPart::ArrayStart,
            EncodedPart::ArrayEnd => KeyPart::ArrayEnd,
            EncodedPart::GeneratedKeyMarker => return Err(InvalidKey::UnexpectedMarker),
        }))
    }
}

fn decode_string(encoded: &[u8]) -> Vec<u16> {
    let (encoded_code_units, _) = encoded.as_chunks();
    let mut code_units = Vec::with_capacity(encoded_code_units.len());
    let mut encoded_code_units = encoded_code_units.iter().map(|bytes| u16::from_be_bytes(*bytes));

    while let Some(code_unit) = encoded_code_units.next() {
        if code_unit == u16::from(ESCAPE) {
            let escaped = encoded_code_units.next().expect("the scanner checked the escape");
            code_units.push(escaped - 1);
        } else {
            code_units.push(code_unit);
        }
    }

    code_units
}

fn decode_binary(encoded: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(encoded.len());
    let mut encoded_bytes = encoded.iter().copied();

    while let Some(byte) = encoded_bytes.next() {
        if byte == ESCAPE {
            let escaped = encoded_bytes.next().expect("the scanner checked the escape");
            bytes.push(escaped - 1);
        } else {
            bytes.push(byte);
        }
    }

    bytes
}

/// A key that another process sent, in the forms that a database stores.
pub struct CheckedKey {
    /// Compares bytewise as the key compares.
    pub sort_form: Vec<u8>,
    /// The key as it is read back, for a key that holds a -0.
    pub exact_form: Option<Vec<u8>>,
}

/// A key that a key generator made, https://w3c.github.io/IndexedDB/#key-generator-construct
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GeneratedKey(f64);

impl GeneratedKey {
    /// A key generator counts up from 1, and makes no key once it has passed 2^53.
    pub fn new(number: u64) -> Option<Self> {
        const LARGEST_NUMBER: u64 = 1 << 53;
        (1..=LARGEST_NUMBER).contains(&number).then_some(Self(number as f64))
    }
}

/// Why a key that another process sent was not accepted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyCheckError {
    Invalid(InvalidKey),
    OutOfMemory,
}

impl From<InvalidKey> for KeyCheckError {
    fn from(reason: InvalidKey) -> Self {
        Self::Invalid(reason)
    }
}

impl From<TryReserveError> for KeyCheckError {
    fn from(_: TryReserveError) -> Self {
        Self::OutOfMemory
    }
}

fn try_append(bytes: &mut Vec<u8>, appended: &[u8]) -> Result<(), KeyCheckError> {
    bytes.try_reserve(appended.len())?;
    bytes.extend_from_slice(appended);
    Ok(())
}

/// Checks the exact form of a key that another process sent. The sender decides how long the key is, so no allocation
/// here may abort the process.
///
/// `injected_key` is the key that was generated for a record and injected into its value, when the bytes are an index
/// key of that record, and takes the place of every marker. A write that injects no key refuses a marker.
pub fn check_received_key(bytes: &[u8], injected_key: Option<GeneratedKey>) -> Result<CheckedKey, KeyCheckError> {
    let mut exact_form = Vec::new();
    exact_form.try_reserve_exact(bytes.len())?;
    let mut negative_zero_offsets = Vec::new();

    let mut append_double = |exact_form: &mut Vec<u8>, tag: u8, value: f64| -> Result<(), KeyCheckError> {
        try_append(exact_form, &[tag])?;
        if value.to_bits() == (-0.0_f64).to_bits() {
            negative_zero_offsets.try_reserve(1)?;
            negative_zero_offsets.push(exact_form.len());
        }
        try_append(exact_form, &encode_double(value))
    };

    let mut scanner = Scanner::new(bytes);
    while let Some(part) = scanner.next_part()? {
        match part {
            EncodedPart::Number(value) => append_double(&mut exact_form, NUMBER_TAG, value)?,
            EncodedPart::Date(value) => append_double(&mut exact_form, DATE_TAG, value)?,
            EncodedPart::String(encoded) => {
                try_append(&mut exact_form, &[STRING_TAG])?;
                try_append(&mut exact_form, encoded)?;
                try_append(&mut exact_form, &[TERMINATOR, TERMINATOR])?;
            }
            EncodedPart::Binary(encoded) => {
                try_append(&mut exact_form, &[BINARY_TAG])?;
                try_append(&mut exact_form, encoded)?;
                try_append(&mut exact_form, &[TERMINATOR])?;
            }
            EncodedPart::ArrayStart => try_append(&mut exact_form, &[ARRAY_TAG])?,
            EncodedPart::ArrayEnd => try_append(&mut exact_form, &[TERMINATOR])?,
            EncodedPart::GeneratedKeyMarker => {
                let Some(GeneratedKey(injected_key)) = injected_key else {
                    return Err(InvalidKey::UnexpectedMarker.into());
                };
                append_double(&mut exact_form, NUMBER_TAG, injected_key)?;
            }
        }
    }

    if negative_zero_offsets.is_empty() {
        return Ok(CheckedKey {
            sort_form: exact_form,
            exact_form: None,
        });
    }

    let mut sort_form = Vec::new();
    sort_form.try_reserve_exact(exact_form.len())?;
    sort_form.extend_from_slice(&exact_form);

    let zero = encode_double(0.0);
    for offset in negative_zero_offsets {
        sort_form[offset..offset + zero.len()].copy_from_slice(&zero);
    }

    Ok(CheckedKey {
        sort_form,
        exact_form: Some(exact_form),
    })
}

#[cfg(test)]
mod tests {
    use core::cmp::Ordering;

    use super::*;

    #[derive(Clone, Debug)]
    enum Key {
        Number(f64),
        Date(f64),
        String(Vec<u16>),
        Binary(Vec<u8>),
        Array(Vec<Key>),
    }

    // https://w3c.github.io/IndexedDB/#compare-two-keys
    fn compare_two_keys(a: &Key, b: &Key) -> Ordering {
        fn rank_of_type(key: &Key) -> u8 {
            match key {
                Key::Number(_) => 0,
                Key::Date(_) => 1,
                Key::String(_) => 2,
                Key::Binary(_) => 3,
                Key::Array(_) => 4,
            }
        }

        match (a, b) {
            (Key::Number(a), Key::Number(b)) | (Key::Date(a), Key::Date(b)) => {
                a.partial_cmp(b).expect("a key is never NaN")
            }
            // A slice compares as "code unit less than" and "byte less than" do: a proper prefix is less, and
            // otherwise the first element that differs decides.
            (Key::String(a), Key::String(b)) => a.cmp(b),
            (Key::Binary(a), Key::Binary(b)) => a.cmp(b),
            (Key::Array(a), Key::Array(b)) => {
                for (a, b) in a.iter().zip(b) {
                    let comparison = compare_two_keys(a, b);
                    if comparison != Ordering::Equal {
                        return comparison;
                    }
                }
                a.len().cmp(&b.len())
            }
            _ => rank_of_type(a).cmp(&rank_of_type(b)),
        }
    }

    fn encode_into(encoder: &mut KeyEncoder, key: &Key) {
        match key {
            Key::Number(value) => encoder.encode_number(*value),
            Key::Date(value) => encoder.encode_date(*value),
            Key::String(code_units) => encoder.encode_string(Utf16StringUnits::Utf16(code_units)),
            Key::Binary(bytes) => encoder.encode_binary(bytes),
            Key::Array(elements) => {
                encoder.begin_array();
                for element in elements {
                    encode_into(encoder, element);
                }
                encoder.end_array();
            }
        }
    }

    fn exact_form(key: &Key) -> Vec<u8> {
        let mut encoder = KeyEncoder::new();
        encode_into(&mut encoder, key);
        encoder.finish()
    }

    fn sort_form(key: &Key) -> Vec<u8> {
        check_received_key(&exact_form(key), None)
            .expect("an encoded key is valid")
            .sort_form
    }

    fn decode(bytes: &[u8]) -> Result<Key, InvalidKey> {
        let mut decoder = KeyDecoder::new(bytes);
        let mut open_arrays: Vec<Vec<Key>> = Vec::new();
        let mut decoded = None;

        while let Some(part) = decoder.next_part()? {
            let key = match part {
                KeyPart::Number(value) => Key::Number(value),
                KeyPart::Date(value) => Key::Date(value),
                KeyPart::String(code_units) => Key::String(code_units),
                KeyPart::Binary(bytes) => Key::Binary(bytes),
                KeyPart::ArrayStart => {
                    open_arrays.push(Vec::new());
                    continue;
                }
                KeyPart::ArrayEnd => Key::Array(open_arrays.pop().expect("an array ends after it starts")),
            };

            match open_arrays.last_mut() {
                Some(elements) => elements.push(key),
                None => decoded = Some(key),
            }
        }

        Ok(decoded.expect("a complete encoding holds a key"))
    }

    /// Equality that tells -0 from 0.
    fn is_same_key_exactly(a: &Key, b: &Key) -> bool {
        match (a, b) {
            (Key::Number(a), Key::Number(b)) | (Key::Date(a), Key::Date(b)) => a.to_bits() == b.to_bits(),
            (Key::String(a), Key::String(b)) => a == b,
            (Key::Binary(a), Key::Binary(b)) => a == b,
            (Key::Array(a), Key::Array(b)) => {
                a.len() == b.len() && a.iter().zip(b).all(|(a, b)| is_same_key_exactly(a, b))
            }
            _ => false,
        }
    }

    fn without_negative_zero(key: &Key) -> Key {
        match key {
            Key::Number(value) if *value == 0.0 => Key::Number(0.0),
            Key::Date(value) if *value == 0.0 => Key::Date(0.0),
            Key::Array(elements) => Key::Array(elements.iter().map(without_negative_zero).collect()),
            _ => key.clone(),
        }
    }

    const SPECIAL_DOUBLES: [f64; 20] = [
        f64::NEG_INFINITY,
        f64::MIN,
        -2.0,
        -1.000_000_000_000_000_2,
        -1.0,
        -0.5,
        -f64::MIN_POSITIVE,
        -5e-324,
        -0.0,
        0.0,
        5e-324,
        f64::MIN_POSITIVE,
        0.5,
        1.0,
        1.000_000_000_000_000_2,
        2.0,
        9_007_199_254_740_991.0,
        9_007_199_254_740_992.0,
        f64::MAX,
        f64::INFINITY,
    ];
    const SPECIAL_TIME_VALUES: [f64; 10] = [
        -MAXIMUM_TIME_VALUE,
        -8_639_999_999_999_999.0,
        -2.0,
        -1.0,
        -0.0,
        0.0,
        1.0,
        2.0,
        8_639_999_999_999_999.0,
        MAXIMUM_TIME_VALUE,
    ];
    const INTERESTING_CODE_UNITS: [u16; 12] = [
        0x0000, 0x0001, 0x0002, 0x0003, 0x0061, 0x0062, 0x00ff, 0x0100, 0xd800, 0xdfff, 0xfffe, 0xffff,
    ];
    const INTERESTING_BYTES: [u8; 10] = [0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x50, 0x61, 0xfe, 0xff];

    /// Keys that share prefixes, sit next to each other, or hold what the encoding escapes.
    fn edge_keys() -> Vec<Key> {
        let mut keys = Vec::new();
        keys.extend(SPECIAL_DOUBLES.map(Key::Number));
        keys.extend(SPECIAL_TIME_VALUES.map(Key::Date));

        let strings: [&[u16]; 16] = [
            &[],
            &[0],
            &[0, 0],
            &[0, 1],
            &[1],
            &[1, 0],
            &[1, 1],
            &[2],
            &[0x61],
            &[0x61, 0],
            &[0x61, 0x62],
            &[0x62],
            &[0xd800],
            &[0xdfff],
            &[0xffff],
            &[0xffff, 0],
        ];
        keys.extend(strings.map(|code_units| Key::String(code_units.to_vec())));

        let binaries: [&[u8]; 13] = [
            &[],
            &[0x00],
            &[0x00, 0x00],
            &[0x00, 0x01],
            &[0x01],
            &[0x01, 0x00],
            &[0x01, 0x01],
            &[0x02],
            &[0x10],
            &[0x11],
            &[0x50],
            &[0xff],
            &[0xff, 0x00],
        ];
        keys.extend(binaries.map(|bytes| Key::Binary(bytes.to_vec())));

        let one = Key::Number(1.0);
        let a = Key::String(vec![0x61]);
        let empty = Key::Array(Vec::new());
        let arrays = [
            vec![],
            vec![empty.clone()],
            vec![empty.clone(), empty],
            vec![one.clone()],
            vec![one.clone(), one.clone()],
            vec![one.clone(), a.clone()],
            vec![a.clone()],
            vec![a, one.clone()],
            vec![Key::Array(vec![one.clone()])],
            vec![Key::Array(vec![one.clone()]), one],
            vec![Key::Binary(vec![])],
            vec![Key::Binary(vec![0x00])],
            vec![Key::String(vec![])],
            vec![Key::String(vec![0])],
            vec![Key::Number(-0.0)],
            vec![Key::Number(0.0)],
            vec![Key::Date(1.0)],
        ];
        keys.extend(arrays.map(Key::Array));

        keys
    }

    /// xorshift64*, which gives the same keys on every run.
    struct Random(u64);

    impl Random {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 >> 12;
            self.0 ^= self.0 << 25;
            self.0 ^= self.0 >> 27;
            self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
        }

        fn below(&mut self, bound: usize) -> usize {
            (self.next() % bound as u64) as usize
        }

        fn has_chance(&mut self, percent: usize) -> bool {
            self.below(100) < percent
        }

        fn pick<T: Copy>(&mut self, items: &[T]) -> T {
            items[self.below(items.len())]
        }

        fn double(&mut self) -> f64 {
            match self.below(10) {
                0..3 => self.pick(&SPECIAL_DOUBLES),
                3..6 => self.below(2001) as f64 - 1000.0,
                _ => loop {
                    let value = f64::from_bits(self.next());
                    if !value.is_nan() {
                        return value;
                    }
                },
            }
        }

        fn time_value(&mut self) -> f64 {
            if self.has_chance(30) {
                return self.pick(&SPECIAL_TIME_VALUES);
            }

            let magnitude = (self.next() % (MAXIMUM_TIME_VALUE as u64 + 1)) as f64;
            if self.has_chance(50) { magnitude } else { -magnitude }
        }

        fn code_unit(&mut self) -> u16 {
            if self.has_chance(70) {
                self.pick(&INTERESTING_CODE_UNITS)
            } else {
                self.next() as u16
            }
        }

        fn byte(&mut self) -> u8 {
            if self.has_chance(70) {
                self.pick(&INTERESTING_BYTES)
            } else {
                self.next() as u8
            }
        }

        fn key(&mut self, depth: usize) -> Key {
            let kinds = if depth < 3 { 6 } else { 4 };
            match self.below(kinds) {
                0 => Key::Number(self.double()),
                1 => Key::Date(self.time_value()),
                2 => Key::String((0..self.below(7)).map(|_| self.code_unit()).collect()),
                3 => Key::Binary((0..self.below(7)).map(|_| self.byte()).collect()),
                _ => Key::Array((0..self.below(5)).map(|_| self.key(depth + 1)).collect()),
            }
        }

        fn neighbouring_double(&mut self, value: f64) -> f64 {
            let neighbour = f64::from_bits(value.to_bits() ^ (1 << self.below(64)));
            if neighbour.is_nan() { value } else { neighbour }
        }

        fn neighbouring_time_value(&mut self, value: f64) -> f64 {
            let neighbour = if self.has_chance(50) { value + 1.0 } else { value - 1.0 };
            if neighbour.abs() > MAXIMUM_TIME_VALUE {
                value
            } else {
                neighbour
            }
        }

        fn neighbouring_sequence<T: Clone>(
            &mut self,
            items: &[T],
            mut make_item: impl FnMut(&mut Self) -> T,
        ) -> Vec<T> {
            let mut items = items.to_vec();
            let choice = self.below(10);

            if choice < 4 || items.is_empty() {
                items.push(make_item(self));
            } else if choice < 7 {
                items.pop();
            } else {
                let index = self.below(items.len());
                items[index] = make_item(self);
            }

            items
        }

        /// A key that shares a prefix with the given one, which is where an encoding is most likely to go wrong.
        fn neighbour(&mut self, key: &Key) -> Key {
            match key {
                Key::Number(value) => Key::Number(self.neighbouring_double(*value)),
                Key::Date(value) => Key::Date(self.neighbouring_time_value(*value)),
                Key::String(code_units) => Key::String(self.neighbouring_sequence(code_units, Self::code_unit)),
                Key::Binary(bytes) => Key::Binary(self.neighbouring_sequence(bytes, Self::byte)),
                Key::Array(elements) if !elements.is_empty() && self.has_chance(50) => {
                    let mut elements = elements.clone();
                    let index = self.below(elements.len());
                    elements[index] = self.neighbour(&elements[index]);
                    Key::Array(elements)
                }
                Key::Array(elements) => Key::Array(self.neighbouring_sequence(elements, |random| random.key(3))),
            }
        }
    }

    /// Pairs of keys, half of them neighbours.
    fn generated_pairs() -> Vec<(Key, Key)> {
        let mut random = Random(0x9e37_79b9_7f4a_7c15);

        (0..200_000)
            .map(|_| {
                let a = random.key(0);
                let b = if random.has_chance(50) {
                    random.neighbour(&a)
                } else {
                    random.key(0)
                };
                (a, b)
            })
            .collect()
    }

    fn assert_sorts_as_the_keys_compare(a: &Key, b: &Key) {
        let bytewise = sort_form(a).cmp(&sort_form(b));
        assert_eq!(bytewise, compare_two_keys(a, b), "a = {a:?}, b = {b:?}");
    }

    fn assert_round_trips(key: &Key) {
        let exact = decode(&exact_form(key)).expect("an encoded key is valid");
        assert!(is_same_key_exactly(&exact, key), "{key:?} came back as {exact:?}");

        let sorted = decode(&sort_form(key)).expect("the sort form of a key is valid");
        assert!(
            is_same_key_exactly(&sorted, &without_negative_zero(key)),
            "{key:?} came back as {sorted:?}"
        );
    }

    #[test]
    fn keys_have_these_bytes() {
        let one = [0xbf, 0xf0, 0, 0, 0, 0, 0, 0];

        assert_eq!(exact_form(&Key::Number(1.0)), [&[0x10][..], &one].concat());
        assert_eq!(exact_form(&Key::Number(0.0)), [0x10, 0x80, 0, 0, 0, 0, 0, 0, 0]);
        assert_eq!(
            exact_form(&Key::Number(-0.0)),
            [0x10, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]
        );
        assert_eq!(sort_form(&Key::Number(-0.0)), [0x10, 0x80, 0, 0, 0, 0, 0, 0, 0]);
        assert_eq!(
            exact_form(&Key::Number(-1.0)),
            [0x10, 0x40, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]
        );
        assert_eq!(
            exact_form(&Key::Number(f64::INFINITY)),
            [0x10, 0xff, 0xf0, 0, 0, 0, 0, 0, 0]
        );
        assert_eq!(
            exact_form(&Key::Number(f64::NEG_INFINITY)),
            [0x10, 0x00, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]
        );
        assert_eq!(exact_form(&Key::Date(1.0)), [&[0x20][..], &one].concat());

        assert_eq!(exact_form(&Key::String(vec![])), [0x30, 0x00, 0x00]);
        assert_eq!(exact_form(&Key::String(vec![0x61])), [0x30, 0x00, 0x61, 0x00, 0x00]);
        assert_eq!(
            exact_form(&Key::String(vec![0x0000])),
            [0x30, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00]
        );
        assert_eq!(
            exact_form(&Key::String(vec![0x0001])),
            [0x30, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00]
        );
        assert_eq!(
            exact_form(&Key::String(vec![0xd800, 0x1234])),
            [0x30, 0xd8, 0x00, 0x12, 0x34, 0x00, 0x00]
        );

        assert_eq!(exact_form(&Key::Binary(vec![])), [0x40, 0x00]);
        assert_eq!(
            exact_form(&Key::Binary(vec![0x00, 0x01, 0x02])),
            [0x40, 0x01, 0x01, 0x01, 0x02, 0x02, 0x00]
        );

        assert_eq!(exact_form(&Key::Array(vec![])), [0x50, 0x00]);
        assert_eq!(
            exact_form(&Key::Array(vec![Key::Number(1.0), Key::String(vec![0x61])])),
            [&[0x50, 0x10][..], &one, &[0x30, 0x00, 0x61, 0x00, 0x00, 0x00]].concat()
        );
    }

    #[test]
    fn a_string_has_the_same_bytes_from_either_storage() {
        let from_ascii = {
            let mut encoder = KeyEncoder::new();
            encoder.encode_string(Utf16StringUnits::Ascii(b"a\0\x01"));
            encoder.finish()
        };
        let from_latin1 = {
            let mut encoder = KeyEncoder::new();
            encoder.encode_string(Utf16StringUnits::Latin1(&[0xe9, 0x00]));
            encoder.finish()
        };

        assert_eq!(from_ascii, exact_form(&Key::String(vec![0x61, 0x00, 0x01])));
        assert_eq!(from_latin1, exact_form(&Key::String(vec![0xe9, 0x00])));
    }

    #[test]
    fn edge_keys_sort_as_the_keys_compare() {
        let keys = edge_keys();

        for a in &keys {
            for b in &keys {
                assert_sorts_as_the_keys_compare(a, b);
            }
        }
    }

    #[test]
    fn generated_keys_sort_as_the_keys_compare() {
        for (a, b) in generated_pairs() {
            assert_sorts_as_the_keys_compare(&a, &b);
        }
    }

    #[test]
    fn keys_round_trip() {
        for key in edge_keys() {
            assert_round_trips(&key);
        }
        for (key, _) in generated_pairs() {
            assert_round_trips(&key);
        }
    }

    #[test]
    fn only_a_key_with_a_negative_zero_has_an_exact_form_of_its_own() {
        let with_negative_zero = Key::Array(vec![Key::Number(1.0), Key::Date(-0.0)]);
        let checked = check_received_key(&exact_form(&with_negative_zero), None).expect("the key is valid");
        assert_eq!(checked.exact_form, Some(exact_form(&with_negative_zero)));
        assert_eq!(
            checked.sort_form,
            exact_form(&Key::Array(vec![Key::Number(1.0), Key::Date(0.0)]))
        );

        let without = Key::Array(vec![Key::Number(1.0), Key::Date(0.0)]);
        let checked = check_received_key(&exact_form(&without), None).expect("the key is valid");
        assert_eq!(checked.exact_form, None);
        assert_eq!(checked.sort_form, exact_form(&without));
    }

    #[test]
    fn malformed_encodings_are_refused() {
        let positive_nan = [0x10, 0xff, 0xf8, 0, 0, 0, 0, 0, 0];
        let negative_nan = [0x10, 0x00, 0x07, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff];
        let one_and_a_byte = [0x10, 0xbf, 0xf0, 0, 0, 0, 0, 0, 0, 0x00];

        let malformed: [(&[u8], InvalidKey); 18] = [
            (&[], InvalidKey::Truncated),
            (&[0x00], InvalidKey::UnknownTag),
            (&[0x60], InvalidKey::UnknownTag),
            (&positive_nan, InvalidKey::NotANumber),
            (&negative_nan, InvalidKey::NotANumber),
            (&[0x20, 0xff, 0xf8, 0, 0, 0, 0, 0, 0], InvalidKey::NotANumber),
            (&[0x10, 0x80, 0x00], InvalidKey::Truncated),
            (&[0x30, 0x00, 0x61], InvalidKey::Truncated),
            (&[0x30, 0x00, 0x61, 0x00], InvalidKey::Truncated),
            (&[0x30, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00], InvalidKey::InvalidEscape),
            (&[0x30, 0x00, 0x01], InvalidKey::Truncated),
            (&[0x40, 0x61], InvalidKey::Truncated),
            (&[0x40, 0x01, 0x03, 0x00], InvalidKey::InvalidEscape),
            (&[0x40, 0x01], InvalidKey::Truncated),
            (&[0x50], InvalidKey::Truncated),
            (&[0x50, 0x10], InvalidKey::Truncated),
            (&[0x50, 0x00, 0x00], InvalidKey::TrailingBytes),
            (&one_and_a_byte, InvalidKey::TrailingBytes),
        ];

        for (bytes, reason) in malformed {
            let refusal = Some(KeyCheckError::Invalid(reason));
            assert_eq!(decode(bytes).err(), Some(reason), "{bytes:x?}");
            assert_eq!(check_received_key(bytes, None).err(), refusal, "{bytes:x?}");
            assert_eq!(
                check_received_key(bytes, GeneratedKey::new(1)).err(),
                refusal,
                "{bytes:x?}"
            );
        }
    }

    #[test]
    fn a_date_that_no_date_object_holds_is_refused() {
        let time_values = [
            0.5,
            -0.5,
            f64::INFINITY,
            f64::NEG_INFINITY,
            8_640_000_000_000_001.0,
            -8_640_000_000_000_001.0,
        ];

        for time_value in time_values {
            let bytes = exact_form(&Key::Date(time_value));
            assert_eq!(decode(&bytes).err(), Some(InvalidKey::NotATimeValue), "{time_value}");
            assert_eq!(
                check_received_key(&bytes, None).err(),
                Some(KeyCheckError::Invalid(InvalidKey::NotATimeValue)),
                "{time_value}"
            );
        }
    }

    #[test]
    fn bytes_after_a_key_are_refused_before_its_last_part() {
        let mut after_binary = KeyDecoder::new(&[0x40, 0x00, 0x61]);
        assert_eq!(after_binary.next_part(), Err(InvalidKey::TrailingBytes));

        let mut after_an_array = KeyDecoder::new(&[0x50, 0x00, 0x00]);
        assert_eq!(after_an_array.next_part(), Ok(Some(KeyPart::ArrayStart)));
        assert_eq!(after_an_array.next_part(), Err(InvalidKey::TrailingBytes));
    }

    #[test]
    fn a_generated_key_is_a_whole_number_from_1_to_2_to_the_53() {
        let largest = 1 << 53;

        assert_eq!(GeneratedKey::new(0), None);
        assert_eq!(GeneratedKey::new(largest + 1), None);

        let first = check_received_key(&[GENERATED_KEY_MARKER], GeneratedKey::new(1)).expect("1 is generated");
        assert_eq!(first.sort_form, exact_form(&Key::Number(1.0)));

        let last = check_received_key(&[GENERATED_KEY_MARKER], GeneratedKey::new(largest)).expect("2^53 is generated");
        assert_eq!(last.sort_form, exact_form(&Key::Number(9_007_199_254_740_992.0)));
    }

    #[test]
    fn a_marker_takes_the_generated_key() {
        // An index on ["id", "id", "delta", "payload"] of a record whose "id" is generated.
        let payload = [0x11, 0x00, 0x11];
        let sent = {
            let mut encoder = KeyEncoder::new();
            encoder.begin_array();
            encoder.encode_generated_key_marker();
            encoder.encode_generated_key_marker();
            encoder.encode_number(-0.0);
            encoder.encode_binary(&payload);
            encoder.end_array();
            encoder.finish()
        };

        let checked = check_received_key(&sent, GeneratedKey::new(7)).expect("the request generates a key");

        let with_delta = |delta| {
            Key::Array(vec![
                Key::Number(7.0),
                Key::Number(7.0),
                Key::Number(delta),
                Key::Binary(payload.to_vec()),
            ])
        };
        assert_eq!(checked.exact_form, Some(exact_form(&with_delta(-0.0))));
        assert_eq!(checked.sort_form, exact_form(&with_delta(0.0)));

        let alone =
            check_received_key(&[GENERATED_KEY_MARKER], GeneratedKey::new(7)).expect("the request generates a key");
        assert_eq!(alone.sort_form, exact_form(&Key::Number(7.0)));
        assert_eq!(alone.exact_form, None);
    }

    #[test]
    fn a_marker_is_refused_without_a_generated_key() {
        let markers: [&[u8]; 3] = [
            &[0x11],
            &[0x50, 0x11, 0x00],
            &[0x50, 0x50, 0x50, 0x11, 0x00, 0x00, 0x00],
        ];

        for bytes in markers {
            assert_eq!(
                check_received_key(bytes, None).err(),
                Some(KeyCheckError::Invalid(InvalidKey::UnexpectedMarker)),
                "{bytes:x?}"
            );
        }
    }

    #[test]
    fn a_stored_key_holds_no_marker() {
        let markers: [&[u8]; 3] = [
            &[0x11],
            &[0x50, 0x11, 0x00],
            &[0x50, 0x50, 0x50, 0x11, 0x00, 0x00, 0x00],
        ];

        for bytes in markers {
            assert_eq!(decode(bytes).err(), Some(InvalidKey::UnexpectedMarker), "{bytes:x?}");
        }
    }

    #[test]
    fn nesting_has_no_limit() {
        let depth = 100_000;
        let mut encoder = KeyEncoder::new();
        for _ in 0..depth {
            encoder.begin_array();
        }
        for _ in 0..depth {
            encoder.end_array();
        }
        let nested = encoder.finish();

        let checked = check_received_key(&nested, None).expect("the key is valid");
        assert_eq!(checked.sort_form, nested);

        let mut decoder = KeyDecoder::new(&nested);
        let mut parts = 0;
        while decoder.next_part().expect("the key is valid").is_some() {
            parts += 1;
        }
        assert_eq!(parts, 2 * depth);
    }
}
