/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

use crate::Frame;
use crate::FrameReader;
use crate::OpCode;
use crate::Role;
use crate::encode_close_frame;
use crate::encode_frame;

pub type FfiBytesFn = unsafe extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize);

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WebSocketRustRole {
    Client,
    Server,
}

impl From<WebSocketRustRole> for Role {
    fn from(role: WebSocketRustRole) -> Self {
        match role {
            WebSocketRustRole::Client => Self::Client,
            WebSocketRustRole::Server => Self::Server,
        }
    }
}

pub struct WebSocketRustCodec {
    role: Role,
    reader: FrameReader,
    // The payload handed to C++ borrows from these, so they live until the next frame is read.
    frame: Option<Frame>,
    error_description: String,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WebSocketRustFrameKind {
    Text,
    Binary,
    Ping,
    Pong,
    Close,
    ProtocolError,
}

#[repr(C)]
pub struct WebSocketRustFrame {
    pub kind: WebSocketRustFrameKind,
    pub close_code: u16,
    pub payload: *const u8,
    pub payload_length: usize,
}

fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(_) => std::process::abort(),
    }
}

unsafe fn bytes_from_raw<'a>(bytes: *const u8, len: usize) -> &'a [u8] {
    if len == 0 {
        return &[];
    }
    // SAFETY: The caller guarantees `bytes` is valid for `len` bytes.
    unsafe { std::slice::from_raw_parts(bytes, len) }
}

fn frame_to_ffi(frame: &Frame) -> WebSocketRustFrame {
    const EMPTY: &[u8] = &[];
    let (kind, payload, close_code) = match frame {
        Frame::Text(payload) => (WebSocketRustFrameKind::Text, payload.as_slice(), 0),
        Frame::Binary(payload) => (WebSocketRustFrameKind::Binary, payload.as_slice(), 0),
        Frame::Ping(payload) => (WebSocketRustFrameKind::Ping, payload.as_slice(), 0),
        Frame::Pong => (WebSocketRustFrameKind::Pong, EMPTY, 0),
        Frame::Close { code, reason } => (WebSocketRustFrameKind::Close, reason.as_slice(), *code),
    };
    WebSocketRustFrame {
        kind,
        close_code,
        payload: payload.as_ptr(),
        payload_length: payload.len(),
    }
}

unsafe fn emit(frame: &[u8], ctx: *mut c_void, on_bytes: FfiBytesFn) {
    // SAFETY: The caller guarantees `on_bytes` accepts `ctx` and a borrowed byte slice.
    unsafe { on_bytes(ctx, frame.as_ptr(), frame.len()) }
}

#[unsafe(no_mangle)]
pub extern "C" fn websocket_rust_codec_new(role: WebSocketRustRole) -> *mut WebSocketRustCodec {
    abort_on_panic(|| {
        Box::into_raw(Box::new(WebSocketRustCodec {
            role: role.into(),
            reader: FrameReader::new(role.into()),
            frame: None,
            error_description: String::new(),
        }))
    })
}

/// # Safety
/// - `codec` must be null or a pointer returned by `websocket_rust_codec_new` that has not been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn websocket_rust_codec_free(codec: *mut WebSocketRustCodec) {
    if codec.is_null() {
        return;
    }
    // SAFETY: `codec` was allocated by `Box::into_raw` in `websocket_rust_codec_new`.
    drop(unsafe { Box::from_raw(codec) });
}

/// # Safety
/// - `codec` must be a live pointer returned by `websocket_rust_codec_new`.
/// - `data`/`len` must be a valid byte slice.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn websocket_rust_codec_feed(codec: *mut WebSocketRustCodec, data: *const u8, len: usize) {
    unsafe {
        abort_on_panic(|| {
            let codec = &mut *codec;
            codec.reader.feed(bytes_from_raw(data, len));
        });
    }
}

/// # Safety
/// - `codec` must be a live pointer returned by `websocket_rust_codec_new`.
/// - `out_frame` must be a valid writable pointer. Its payload stays valid until the next call on `codec`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn websocket_rust_codec_next_frame(
    codec: *mut WebSocketRustCodec,
    out_frame: *mut WebSocketRustFrame,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            let codec = &mut *codec;
            *out_frame = match codec.reader.next_frame() {
                Ok(Some(frame)) => frame_to_ffi(codec.frame.insert(frame)),
                Ok(None) => return false,
                Err(error) => {
                    codec.error_description = error.to_string();
                    WebSocketRustFrame {
                        kind: WebSocketRustFrameKind::ProtocolError,
                        close_code: 0,
                        payload: codec.error_description.as_ptr(),
                        payload_length: codec.error_description.len(),
                    }
                }
            };
            true
        })
    }
}

/// # Safety
/// - `codec` must be a live pointer returned by `websocket_rust_codec_new`.
/// - `payload`/`payload_len` must be a valid byte slice.
/// - `on_bytes` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn websocket_rust_codec_encode_message_frame(
    codec: *const WebSocketRustCodec,
    is_text: bool,
    payload: *const u8,
    payload_len: usize,
    ctx: *mut c_void,
    on_bytes: FfiBytesFn,
) {
    unsafe {
        abort_on_panic(|| {
            let op_code = if is_text { OpCode::Text } else { OpCode::Binary };
            let frame = encode_frame((*codec).role, op_code, bytes_from_raw(payload, payload_len));
            emit(&frame, ctx, on_bytes);
        });
    }
}

/// # Safety
/// - `codec` must be a live pointer returned by `websocket_rust_codec_new`.
/// - `payload`/`payload_len` must be a valid byte slice.
/// - `on_bytes` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn websocket_rust_codec_encode_pong_frame(
    codec: *const WebSocketRustCodec,
    payload: *const u8,
    payload_len: usize,
    ctx: *mut c_void,
    on_bytes: FfiBytesFn,
) {
    unsafe {
        abort_on_panic(|| {
            let frame = encode_frame((*codec).role, OpCode::Pong, bytes_from_raw(payload, payload_len));
            emit(&frame, ctx, on_bytes);
        });
    }
}

/// # Safety
/// - `codec` must be a live pointer returned by `websocket_rust_codec_new`.
/// - `reason`/`reason_len` must be a valid byte slice.
/// - `on_bytes` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn websocket_rust_codec_encode_close_frame(
    codec: *const WebSocketRustCodec,
    code: u16,
    reason: *const u8,
    reason_len: usize,
    ctx: *mut c_void,
    on_bytes: FfiBytesFn,
) {
    unsafe {
        abort_on_panic(|| {
            let frame = encode_close_frame((*codec).role, code, bytes_from_raw(reason, reason_len));
            emit(&frame, ctx, on_bytes);
        });
    }
}
