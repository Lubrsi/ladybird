/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::DirectBlockType as SerializedBlockType;
use crate::DirectBlockTypeKind;
use crate::DirectInstruction;
use crate::DirectMemoryInstructionArguments;
use crate::DirectValueType as SerializedValueType;
use crate::DirectValueTypeKind;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum ValueTypeKind {
    I32,
    I64,
    F32,
    F64,
    V128,
    I8,
    I16,
    FunctionReference,
    NoFunctionReference,
    ExternReference,
    NoExternReference,
    AnyReference,
    EqReference,
    I31Reference,
    StructReference,
    ArrayReference,
    NoneReference,
    ExceptionReference,
    NoExceptionReference,
    TypeUseReference,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct ValueType {
    pub(super) kind: ValueTypeKind,
    pub(super) type_index: u32,
    pub(super) nullable: bool,
}

impl ValueType {
    fn decode(value_type: SerializedValueType) -> Result<Self, &'static str> {
        let kind = match value_type.kind {
            value if value == DirectValueTypeKind::I32 as u32 => ValueTypeKind::I32,
            value if value == DirectValueTypeKind::I64 as u32 => ValueTypeKind::I64,
            value if value == DirectValueTypeKind::F32 as u32 => ValueTypeKind::F32,
            value if value == DirectValueTypeKind::F64 as u32 => ValueTypeKind::F64,
            value if value == DirectValueTypeKind::V128 as u32 => ValueTypeKind::V128,
            value if value == DirectValueTypeKind::I8 as u32 => ValueTypeKind::I8,
            value if value == DirectValueTypeKind::I16 as u32 => ValueTypeKind::I16,
            value if value == DirectValueTypeKind::FunctionReference as u32 => ValueTypeKind::FunctionReference,
            value if value == DirectValueTypeKind::NoFunctionReference as u32 => ValueTypeKind::NoFunctionReference,
            value if value == DirectValueTypeKind::ExternReference as u32 => ValueTypeKind::ExternReference,
            value if value == DirectValueTypeKind::NoExternReference as u32 => ValueTypeKind::NoExternReference,
            value if value == DirectValueTypeKind::AnyReference as u32 => ValueTypeKind::AnyReference,
            value if value == DirectValueTypeKind::EqReference as u32 => ValueTypeKind::EqReference,
            value if value == DirectValueTypeKind::I31Reference as u32 => ValueTypeKind::I31Reference,
            value if value == DirectValueTypeKind::StructReference as u32 => ValueTypeKind::StructReference,
            value if value == DirectValueTypeKind::ArrayReference as u32 => ValueTypeKind::ArrayReference,
            value if value == DirectValueTypeKind::NoneReference as u32 => ValueTypeKind::NoneReference,
            value if value == DirectValueTypeKind::ExceptionReference as u32 => ValueTypeKind::ExceptionReference,
            value if value == DirectValueTypeKind::NoExceptionReference as u32 => ValueTypeKind::NoExceptionReference,
            value if value == DirectValueTypeKind::TypeUseReference as u32 => ValueTypeKind::TypeUseReference,
            _ => return Err("invalid serialized direct value type"),
        };
        let nullable = match value_type.nullable {
            0 => false,
            1 => true,
            _ => return Err("invalid serialized direct nullability"),
        };
        if kind != ValueTypeKind::TypeUseReference && value_type.type_index != 0 {
            return Err("unexpected direct type index");
        }
        Ok(Self {
            kind,
            type_index: value_type.type_index,
            nullable,
        })
    }
}

impl SerializedValueType {
    pub(super) fn checked(self) -> Result<ValueType, &'static str> {
        ValueType::decode(self)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum BlockType {
    Empty,
    Value(ValueType),
    TypeIndex(u32),
}

impl BlockType {
    fn decode(block_type: SerializedBlockType) -> Result<Self, &'static str> {
        match block_type.kind {
            value if value == DirectBlockTypeKind::Empty as u32 => Ok(Self::Empty),
            value if value == DirectBlockTypeKind::ValueType as u32 => {
                Ok(Self::Value(ValueType::decode(block_type.value_type)?))
            }
            value if value == DirectBlockTypeKind::TypeIndex as u32 => Ok(Self::TypeIndex(block_type.type_index)),
            _ => Err("invalid serialized direct block type"),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct StructuredInstruction {
    pub(super) block_type: BlockType,
    pub(super) end_ip: u32,
    pub(super) else_ip: Option<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct MemoryArgument {
    pub(super) align: u32,
    pub(super) memory_index: u32,
    pub(super) offset: u64,
}

impl From<DirectMemoryInstructionArguments> for MemoryArgument {
    fn from(argument: DirectMemoryInstructionArguments) -> Self {
        Self {
            align: argument.align,
            memory_index: argument.memory_index,
            offset: argument.offset,
        }
    }
}

impl DirectInstruction {
    pub(super) fn structured(&self) -> Result<StructuredInstruction, &'static str> {
        // The parent serializer owns the opcode/payload invariant. These wire structs contain only
        // integers and integer-discriminated value types, so every bit pattern is valid Rust data;
        // the discriminants are validated before lowering uses them.
        let arguments = unsafe { self.arguments.structured };
        Ok(StructuredInstruction {
            block_type: BlockType::decode(arguments.block_type)?,
            end_ip: arguments.end_ip,
            else_ip: (arguments.else_ip != u32::MAX).then_some(arguments.else_ip),
        })
    }

    pub(super) fn label_depth(&self) -> Result<usize, &'static str> {
        // u32 accepts every bit pattern; the parent serializer owns the opcode/payload invariant.
        usize::try_from(unsafe { self.arguments.label_index }).map_err(|_| "direct label depth overflow")
    }

    pub(super) fn i32_constant(&self) -> i32 {
        // i32 accepts every bit pattern; the parent serializer owns the opcode/payload invariant.
        unsafe { self.arguments.i32_constant }
    }

    pub(super) fn i64_constant(&self) -> i64 {
        // i64 accepts every bit pattern; the parent serializer owns the opcode/payload invariant.
        unsafe { self.arguments.i64_constant }
    }

    pub(super) fn local_index(&self) -> Result<usize, &'static str> {
        // u32 accepts every bit pattern; the parent serializer owns the opcode/payload invariant.
        usize::try_from(unsafe { self.arguments.local_index }).map_err(|_| "direct local index overflow")
    }

    pub(super) fn memory_argument(&self) -> MemoryArgument {
        // This wire struct contains only integers, so every bit pattern is valid Rust data. The
        // parent serializer owns the opcode/payload invariant.
        MemoryArgument::from(unsafe { self.arguments.memory })
    }
}
