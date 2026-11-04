//===- X86LegacyScalar.h - Protected scalar instructions -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticated scalar operands and staged ordinary-RAM execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYSCALAR_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYSCALAR_H

#include "X86LegacyEncoding.h"

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

enum class X86LegacyScalarOperation {
  Move,
  MoveSigned,
  ConditionalMove,
  SetCondition,
  SignHigh,
  ByteSwap,
  TableLookup,
  FlagsToAH,
  AHToFlags,
  ClearFlag,
  SetFlag,
  ComplementFlag,
  Address,
  Exchange,
  ExchangeAdd,
  CompareExchange,
  CompareExchangePair,
  Add,
  Or,
  AddCarry,
  SubBorrow,
  And,
  Subtract,
  Xor,
  Compare,
  Test,
  Increment,
  Decrement,
  Negate,
  Complement,
  ShiftLeft,
  ShiftRight,
  SignedShiftRight,
  RotateLeft,
  RotateRight,
  RotateCarryLeft,
  RotateCarryRight,
  DoubleShiftLeft,
  DoubleShiftRight,
  BitTest,
  BitSet,
  BitReset,
  BitComplement,
  BitScanForward,
  BitScanReverse,
  Divide,
  SignedDivide,
  Multiply,
  SignedMultiply,
  SignedMultiplyLow,
  Push,
  Pop,
  Leave,
  Call,
  Jump,
  ConditionalJump,
  Loop,
  CounterZeroJump,
  Return,
};

struct X86LegacyScalarOperand : X86LegacyAddress {
  std::optional<uint32_t> Immediate;
};

struct X86LegacyScalarInstruction {
  X86LegacyScalarOperation Operation = X86LegacyScalarOperation::Move;
  X86LegacyScalarOperand Destination, Source;
  std::optional<uint32_t> MultiplyImmediate;
  std::optional<uint8_t> ShiftImmediate;
  unsigned Bytes = 4, SourceBytes = 4, Condition = 0;
  unsigned CounterBytes = 0;
  uint16_t Discard = 0;
  bool Relative = false;
  bool Atomic = false;

  bool lockable() const {
    using Op = X86LegacyScalarOperation;
    if (Destination.Register)
      return false;
    switch (Operation) {
    case Op::Add:
    case Op::Or:
    case Op::AddCarry:
    case Op::SubBorrow:
    case Op::And:
    case Op::Subtract:
    case Op::Xor:
    case Op::Increment:
    case Op::Decrement:
    case Op::Negate:
    case Op::Complement:
    case Op::BitSet:
    case Op::BitReset:
    case Op::BitComplement:
    case Op::Exchange:
    case Op::ExchangeAdd:
    case Op::CompareExchange:
    case Op::CompareExchangePair:
      return true;
    default:
      return false;
    }
  }

  bool division() const {
    return Operation == X86LegacyScalarOperation::Divide ||
           Operation == X86LegacyScalarOperation::SignedDivide;
  }

  bool multiplication() const {
    return Operation == X86LegacyScalarOperation::Multiply ||
           Operation == X86LegacyScalarOperation::SignedMultiply ||
           Operation == X86LegacyScalarOperation::SignedMultiplyLow;
  }

  bool logical() const {
    return Operation == X86LegacyScalarOperation::Or ||
           Operation == X86LegacyScalarOperation::And ||
           Operation == X86LegacyScalarOperation::Xor ||
           Operation == X86LegacyScalarOperation::Test;
  }

  bool shifting() const {
    using Op = X86LegacyScalarOperation;
    return Operation == Op::ShiftLeft || Operation == Op::ShiftRight ||
           Operation == Op::SignedShiftRight || Operation == Op::RotateLeft ||
           Operation == Op::RotateRight || Operation == Op::RotateCarryLeft ||
           Operation == Op::RotateCarryRight;
  }

  bool doubleShifting() const {
    return Operation == X86LegacyScalarOperation::DoubleShiftLeft ||
           Operation == X86LegacyScalarOperation::DoubleShiftRight;
  }

  bool bitTesting() const {
    using Op = X86LegacyScalarOperation;
    return Operation == Op::BitTest || Operation == Op::BitSet ||
           Operation == Op::BitReset || Operation == Op::BitComplement;
  }

  bool bitScanning() const {
    return Operation == X86LegacyScalarOperation::BitScanForward ||
           Operation == X86LegacyScalarOperation::BitScanReverse;
  }
};

std::optional<X86LegacyScalarInstruction>
decodeLegacyScalarInstruction(llvm::ArrayRef<uint8_t> Bytes, bool Code32);

/// Caller discards the staged state on a fault; earlier page-table effects
/// remain. A final store uses the same plan as its operand read.
llvm::Expected<X86MemoryAccessResult>
executeLegacyScalar(X86LegacyExecutionState &Staged, X86PagedMemory &Memory,
                    const X86LegacyScalarInstruction &Instruction,
                    uint32_t Fallthrough,
                    const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYSCALAR_H
