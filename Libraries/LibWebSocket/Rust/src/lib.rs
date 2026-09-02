/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The websocket protocol is defined by RFC 6455, found at https://tools.ietf.org/html/rfc6455
// In this file, section numbers refer to RFC 6455.

use std::fmt;

pub mod ffi;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Role {
    Client,
    Server,
}

// As defined in section 5.2
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OpCode {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    ConnectionClose = 0x8,
    Ping = 0x9,
    Pong = 0xA,
}

impl OpCode {
    fn from_u8(value: u8) -> Option<Self> {
        match value {
            0x0 => Some(Self::Continuation),
            0x1 => Some(Self::Text),
            0x2 => Some(Self::Binary),
            0x8 => Some(Self::ConnectionClose),
            0x9 => Some(Self::Ping),
            0xA => Some(Self::Pong),
            _ => None,
        }
    }
}

#[derive(Debug, Eq, PartialEq)]
pub enum Frame {
    Text(Vec<u8>),
    Binary(Vec<u8>),
    Ping(Vec<u8>),
    Pong,
    Close { code: u16, reason: Vec<u8> },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProtocolError {
    UnmaskedFrame,
    UnknownOpCode(u8),
}

impl fmt::Display for ProtocolError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnmaskedFrame => write!(formatter, "Received an unmasked frame"),
            Self::UnknownOpCode(op_code) => write!(formatter, "Received unknown opcode {op_code:#x}"),
        }
    }
}

struct RawFrame {
    op_code: u8,
    is_final: bool,
    is_masked: bool,
    payload: Vec<u8>,
    encoded_length: usize,
}

// Returns None until the buffer holds a complete frame.
fn parse_frame(buffer: &[u8]) -> Option<RawFrame> {
    let mut cursor = 0usize;
    let mut take = |count: usize| -> Option<&[u8]> {
        let bytes = buffer.get(cursor..cursor.checked_add(count)?)?;
        cursor += count;
        Some(bytes)
    };

    let head = take(2)?;
    let op_code = head[0] & 0x0f;
    let is_final = head[0] & 0x80 != 0;
    let is_masked = head[1] & 0x80 != 0;

    // Parse the payload length.
    let payload_length = match head[1] & 0x7f {
        // A code of 127 means that the next 8 bytes contain the payload length.
        127 => {
            let length_bytes: [u8; 8] = take(8)?.try_into().ok()?;
            usize::try_from(u64::from_be_bytes(length_bytes)).ok()?
        }
        // A code of 126 means that the next 2 bytes contain the payload length.
        126 => {
            let length_bytes: [u8; 2] = take(2)?.try_into().ok()?;
            usize::from(u16::from_be_bytes(length_bytes))
        }
        length => usize::from(length),
    };

    // Parse the mask, if it exists.
    let masking_key: Option<[u8; 4]> = if is_masked {
        Some(take(4)?.try_into().ok()?)
    } else {
        None
    };

    let mut payload = take(payload_length)?.to_vec();
    if let Some(masking_key) = masking_key {
        mask_payload(&mut payload, masking_key);
    }

    Some(RawFrame {
        op_code,
        is_final,
        is_masked,
        payload,
        encoded_length: cursor,
    })
}

fn mask_payload(payload: &mut [u8], masking_key: [u8; 4]) {
    for (byte, key) in payload.iter_mut().zip(masking_key.iter().cycle()) {
        *byte ^= key;
    }
}

fn close_frame(payload: &[u8]) -> Frame {
    if payload.len() > 1 {
        Frame::Close {
            code: u16::from_be_bytes([payload[0], payload[1]]),
            reason: payload[2..].to_vec(),
        }
    } else {
        Frame::Close {
            code: 1000,
            reason: Vec::new(),
        }
    }
}

pub struct FrameReader {
    role: Role,
    buffered_data: Vec<u8>,
    fragmented_data_buffer: Vec<u8>,
    initial_fragment_op_code: u8,
}

impl FrameReader {
    pub fn new(role: Role) -> Self {
        Self {
            role,
            buffered_data: Vec::new(),
            fragmented_data_buffer: Vec::new(),
            initial_fragment_op_code: 0,
        }
    }

    pub fn feed(&mut self, bytes: &[u8]) {
        self.buffered_data.extend_from_slice(bytes);
    }

    // Returns None once the buffered data holds no further complete message.
    pub fn next_frame(&mut self) -> Result<Option<Frame>, ProtocolError> {
        loop {
            let Some(RawFrame {
                op_code: raw_op_code,
                is_final,
                is_masked,
                payload,
                encoded_length,
            }) = parse_frame(&self.buffered_data)
            else {
                return Ok(None);
            };
            self.buffered_data.drain(..encoded_length);

            // Section 5.1 :
            // > The server MUST close the connection upon receiving a frame that is not masked.
            // > A client MUST close a connection if it detects a masked frame.
            // Only the server half is enforced: receiving masked frames as a client doesn't cost much, so we support it
            // anyways.
            if self.role == Role::Server && !is_masked {
                return Err(ProtocolError::UnmaskedFrame);
            }

            let op_code = OpCode::from_u8(raw_op_code);
            match op_code {
                Some(OpCode::ConnectionClose) => return Ok(Some(close_frame(&payload))),
                Some(OpCode::Ping) => return Ok(Some(Frame::Ping(payload))),
                Some(OpCode::Pong) => return Ok(Some(Frame::Pong)),
                _ => {}
            }

            if !is_final {
                // First fragmented message
                if op_code != Some(OpCode::Continuation) {
                    self.initial_fragment_op_code = raw_op_code;
                }
                // First and next fragmented message
                self.fragmented_data_buffer.extend_from_slice(&payload);
                continue;
            }

            let (raw_op_code, payload) = if op_code == Some(OpCode::Continuation) {
                // Last fragmented message
                self.fragmented_data_buffer.extend_from_slice(&payload);
                (
                    self.initial_fragment_op_code,
                    std::mem::take(&mut self.fragmented_data_buffer),
                )
            } else {
                (raw_op_code, payload)
            };

            return match OpCode::from_u8(raw_op_code) {
                Some(OpCode::Text) => Ok(Some(Frame::Text(payload))),
                Some(OpCode::Binary) => Ok(Some(Frame::Binary(payload))),
                // Section 5.2 :
                // > If an unknown opcode is received, the receiving endpoint MUST _Fail the WebSocket Connection_.
                _ => Err(ProtocolError::UnknownOpCode(raw_op_code)),
            };
        }
    }
}

pub fn encode_frame(role: Role, op_code: OpCode, payload: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(1 + 9 + 4 + payload.len());
    frame.push(0x80 | op_code as u8);

    // Section 5.1 :
    // > a client MUST mask all frames that it sends to the server
    // > A server MUST NOT mask any frames that it sends to the client.
    let mask_flag = if role == Role::Client { 0x80 } else { 0x00 };
    if payload.len() > usize::from(u16::MAX) {
        // Send (the 'mask' flag + 127) + the 8-byte payload length
        frame.push(mask_flag | 127);
        frame.extend_from_slice(&(payload.len() as u64).to_be_bytes());
    } else if payload.len() >= 126 {
        // Send (the 'mask' flag + 126) + the 2-byte payload length
        frame.push(mask_flag | 126);
        frame.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    } else {
        // Send the mask flag + the payload length in a single byte
        frame.push(mask_flag | payload.len() as u8);
    }

    if role == Role::Client {
        // Section 10.3 :
        // > Clients MUST choose a new masking key for each frame, using an algorithm
        // > that cannot be predicted by end applications that provide data
        let mut masking_key = [0; 4];
        getrandom::fill(&mut masking_key).expect("failed to generate a masking key");
        frame.extend_from_slice(&masking_key);

        let payload_start = frame.len();
        frame.extend_from_slice(payload);
        mask_payload(&mut frame[payload_start..], masking_key);
    } else {
        frame.extend_from_slice(payload);
    }
    frame
}

pub fn encode_close_frame(role: Role, code: u16, reason: &[u8]) -> Vec<u8> {
    // Section 5.5.1:
    // > If there is a body, the first two bytes of the body MUST be a 2-byte unsigned integer (in network byte order)
    // > representing a status code with value /code/ defined in Section 7.4.
    let mut close_payload = Vec::with_capacity(2 + reason.len());
    close_payload.extend_from_slice(&code.to_be_bytes());
    close_payload.extend_from_slice(reason);
    encode_frame(role, OpCode::ConnectionClose, &close_payload)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unmasked_frame(op_code: u8, is_final: bool, payload: &[u8]) -> Vec<u8> {
        let mut frame = vec![if is_final { 0x80 } else { 0x00 } | op_code];
        if payload.len() > usize::from(u16::MAX) {
            frame.push(127);
            frame.extend_from_slice(&(payload.len() as u64).to_be_bytes());
        } else if payload.len() >= 126 {
            frame.push(126);
            frame.extend_from_slice(&(payload.len() as u16).to_be_bytes());
        } else {
            frame.push(payload.len() as u8);
        }
        frame.extend_from_slice(payload);
        frame
    }

    fn masked_frame(op_code: u8, payload: &[u8]) -> Vec<u8> {
        let masking_key = [0x11, 0x22, 0x33, 0x44];
        let mut frame = vec![0x80 | op_code, 0x80 | payload.len() as u8];
        frame.extend_from_slice(&masking_key);
        let mut payload = payload.to_vec();
        mask_payload(&mut payload, masking_key);
        frame.extend(payload);
        frame
    }

    fn read_all(role: Role, bytes: &[u8]) -> Result<Vec<Frame>, ProtocolError> {
        let mut reader = FrameReader::new(role);
        reader.feed(bytes);
        let mut frames = Vec::new();
        while let Some(frame) = reader.next_frame()? {
            frames.push(frame);
        }
        Ok(frames)
    }

    #[test]
    fn text_and_binary_messages() {
        let mut bytes = unmasked_frame(0x1, true, b"hello");
        bytes.extend(unmasked_frame(0x2, true, &[1, 2, 3]));
        assert_eq!(
            read_all(Role::Client, &bytes),
            Ok(vec![Frame::Text(b"hello".to_vec()), Frame::Binary(vec![1, 2, 3])])
        );
    }

    #[test]
    fn extended_payload_lengths() {
        let medium = vec![0xAB; 126];
        let large = vec![0xCD; usize::from(u16::MAX) + 1];
        let mut bytes = unmasked_frame(0x2, true, &medium);
        bytes.extend(unmasked_frame(0x2, true, &large));
        assert_eq!(
            read_all(Role::Client, &bytes),
            Ok(vec![Frame::Binary(medium), Frame::Binary(large)])
        );
    }

    #[test]
    fn client_accepts_masked_and_unmasked_frames() {
        let mut bytes = masked_frame(0x1, b"abc");
        bytes.extend(unmasked_frame(0x1, true, b"def"));
        assert_eq!(
            read_all(Role::Client, &bytes),
            Ok(vec![Frame::Text(b"abc".to_vec()), Frame::Text(b"def".to_vec())])
        );
    }

    #[test]
    fn server_requires_masked_frames() {
        assert_eq!(
            read_all(Role::Server, &masked_frame(0x1, b"abc")),
            Ok(vec![Frame::Text(b"abc".to_vec())])
        );
        assert_eq!(
            read_all(Role::Server, &unmasked_frame(0x1, true, b"abc")),
            Err(ProtocolError::UnmaskedFrame)
        );
    }

    #[test]
    fn fragmented_message_is_reassembled() {
        let mut bytes = unmasked_frame(0x1, false, b"one ");
        bytes.extend(unmasked_frame(0x0, false, b"two "));
        bytes.extend(unmasked_frame(0x0, true, b"three"));
        assert_eq!(
            read_all(Role::Client, &bytes),
            Ok(vec![Frame::Text(b"one two three".to_vec())])
        );
    }

    #[test]
    fn control_frames_interleave_with_fragments() {
        let mut bytes = unmasked_frame(0x2, false, &[1]);
        bytes.extend(unmasked_frame(0x9, true, b"ping"));
        bytes.extend(unmasked_frame(0x0, true, &[2]));
        assert_eq!(
            read_all(Role::Client, &bytes),
            Ok(vec![Frame::Ping(b"ping".to_vec()), Frame::Binary(vec![1, 2])])
        );
    }

    #[test]
    fn close_frame_with_and_without_status_code() {
        let mut bytes = unmasked_frame(0x8, true, &[0x03, 0xE9, b'b', b'y', b'e']);
        bytes.extend(unmasked_frame(0x8, true, &[]));
        assert_eq!(
            read_all(Role::Client, &bytes),
            Ok(vec![
                Frame::Close {
                    code: 1001,
                    reason: b"bye".to_vec()
                },
                Frame::Close {
                    code: 1000,
                    reason: Vec::new()
                }
            ])
        );
    }

    #[test]
    fn pong_and_unknown_op_codes() {
        assert_eq!(
            read_all(Role::Client, &unmasked_frame(0xA, true, b"pong")),
            Ok(vec![Frame::Pong])
        );
        assert_eq!(
            read_all(Role::Client, &unmasked_frame(0x3, true, b"?")),
            Err(ProtocolError::UnknownOpCode(0x3))
        );
    }

    #[test]
    fn frames_split_across_feeds() {
        let bytes = unmasked_frame(0x1, true, &vec![b'x'; 200]);
        let mut reader = FrameReader::new(Role::Client);
        for (index, byte) in bytes.iter().enumerate() {
            reader.feed(std::slice::from_ref(byte));
            if index + 1 < bytes.len() {
                assert_eq!(reader.next_frame(), Ok(None));
            }
        }
        assert_eq!(reader.next_frame(), Ok(Some(Frame::Text(vec![b'x'; 200]))));
        assert_eq!(reader.next_frame(), Ok(None));
    }

    #[test]
    fn truncated_length_fields_wait_for_more_data() {
        assert_eq!(read_all(Role::Client, &[0x82, 126, 0x01]), Ok(Vec::new()));
        assert_eq!(
            read_all(Role::Client, &[0x82, 127, 0, 0, 0, 0, 0, 0, 0]),
            Ok(Vec::new())
        );
        assert_eq!(read_all(Role::Client, &[0x82, 0x80 | 1, 0x11, 0x22]), Ok(Vec::new()));
    }

    #[test]
    fn client_frames_are_masked_and_round_trip_to_a_server() {
        for payload in [
            Vec::new(),
            vec![7; 125],
            vec![8; 126],
            vec![9; usize::from(u16::MAX) + 1],
        ] {
            let frame = encode_frame(Role::Client, OpCode::Binary, &payload);
            assert_eq!(frame[0], 0x82);
            assert_ne!(frame[1] & 0x80, 0);
            assert_eq!(read_all(Role::Server, &frame), Ok(vec![Frame::Binary(payload)]));
        }
    }

    #[test]
    fn server_frames_are_unmasked_and_round_trip_to_a_client() {
        let frame = encode_frame(Role::Server, OpCode::Text, b"hello");
        assert_eq!(frame, unmasked_frame(0x1, true, b"hello"));
        assert_eq!(read_all(Role::Client, &frame), Ok(vec![Frame::Text(b"hello".to_vec())]));
    }

    #[test]
    fn encoded_close_frame_carries_the_status_code() {
        let frame = encode_close_frame(Role::Client, 1002, b"protocol error");
        assert_eq!(
            read_all(Role::Server, &frame),
            Ok(vec![Frame::Close {
                code: 1002,
                reason: b"protocol error".to_vec()
            }])
        );
    }
}
