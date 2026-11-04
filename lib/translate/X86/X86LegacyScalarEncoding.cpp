//===- X86LegacyScalarEncoding.cpp - Protected scalar grammar -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact scalar instruction extents with independent operand/address widths.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalar.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate::detail {

std::optional<X86LegacyScalarInstruction>
decodeLegacyScalarInstruction(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  using Op = X86LegacyScalarOperation;
  X86LegacyEncoding R(Bytes, Code32);
  if (R.Group1Prefix && R.Group1Prefix != 0xf0)
    return std::nullopt;
  X86LegacyScalarInstruction F;
  F.Bytes = F.SourceBytes = R.OperandBytes;
  const uint8_t B = R.byte();
  const auto Width = [&](bool Byte) {
    F.Bytes = F.SourceBytes = Byte ? 1 : R.OperandBytes;
  };
  const auto Immediate = [&](unsigned N, bool Signed = false) {
    uint32_t V = R.value(N);
    if (Signed)
      V = N == 1 ? int8_t(V) : int16_t(V);
    F.Source.Immediate = V;
  };
  const auto Binary = [&](bool Reverse) {
    F.Source.Register = R.modRM(F.Destination);
    if (Reverse)
      std::swap(F.Source, F.Destination);
  };
  constexpr Op Arithmetic[]{Op::Add, Op::Or,       Op::AddCarry, Op::SubBorrow,
                            Op::And, Op::Subtract, Op::Xor,      Op::Compare};
  if (B <= 0x3d && (B & 7) <= 5) {
    F.Operation = Arithmetic[B >> 3];
    Width(!(B & 1));
    if ((B & 7) < 4) {
      Binary(B & 2);
    } else {
      F.Destination.Register = 0;
      Immediate(F.Bytes);
    }
  } else if (B >= 0x80 && B <= 0x83 && B != 0x82) {
    Width(B == 0x80);
    F.Operation = Arithmetic[R.modRM(F.Destination)];
    Immediate(B == 0x83 ? 1 : F.Bytes, B == 0x83);
  } else if ((B >= 0x88 && B <= 0x8b) || B == 0x84 || B == 0x85) {
    Width(!(B & 1));
    F.Operation = B < 0x88 ? Op::Test : Op::Move;
    Binary(B >= 0x88 && (B & 2));
  } else if (B == 0x86 || B == 0x87) {
    Width(B == 0x86);
    F.Operation = Op::Exchange;
    Binary(false);
  } else if (B >= 0x90 && B <= 0x97) {
    // Keep the original unbound one-byte NOP interface in the step owner.
    if (B == 0x90 && Bytes.size() == 1)
      return std::nullopt;
    F.Operation = Op::Exchange;
    F.Destination.Register = 0;
    F.Source.Register = B & 7;
  } else if (B >= 0xb0 && B <= 0xbf) {
    Width(B < 0xb8);
    F.Destination.Register = B & 7;
    Immediate(F.Bytes);
  } else if (B == 0xc6 || B == 0xc7) {
    Width(B == 0xc6);
    if (R.modRM(F.Destination) != 0)
      return std::nullopt;
    Immediate(F.Bytes);
  } else if (B >= 0xa0 && B <= 0xa3) {
    Width(!(B & 1));
    F.Destination.AddressBytes = R.AddressBytes;
    F.Destination.MemorySegment = R.Segment.value_or(X86SegmentRegister::DS);
    F.Destination.Displacement = R.value(R.AddressBytes);
    F.Source.Register = 0;
    if (!(B & 2))
      std::swap(F.Source, F.Destination);
  } else if (B == 0xa8 || B == 0xa9) {
    Width(B == 0xa8);
    F.Operation = Op::Test;
    F.Destination.Register = 0;
    Immediate(F.Bytes);
  } else if (B == 0x8d) {
    F.Operation = Op::Address;
    F.Destination.Register = R.modRM(F.Source);
    if (F.Source.Register)
      return std::nullopt;
  } else if (B == 0xc0 || B == 0xc1 || (B >= 0xd0 && B <= 0xd3)) {
    Width(!(B & 1));
    const unsigned Group = R.modRM(F.Destination);
    if (Group == 6)
      return std::nullopt;
    constexpr Op Operations[]{Op::RotateLeft,      Op::RotateRight,
                              Op::RotateCarryLeft, Op::RotateCarryRight,
                              Op::ShiftLeft,       Op::ShiftRight,
                              Op::ShiftLeft,       Op::SignedShiftRight};
    F.Operation = Operations[Group];
    F.SourceBytes = 1;
    if (B == 0xc0 || B == 0xc1)
      Immediate(1);
    else if (B & 2)
      F.Source.Register = 1;
    else
      F.Source.Immediate = 1;
  } else if (B == 0xd7) {
    F.Operation = Op::TableLookup;
    Width(true);
    F.Source.Base = 3;
    F.Source.AddressBytes = R.AddressBytes;
    F.Source.MemorySegment = R.Segment.value_or(X86SegmentRegister::DS);
  } else if (B == 0xc9) {
    F.Operation = Op::Leave;
  } else if (B == 0x98 || B == 0x99) {
    F.Operation = B == 0x98 ? Op::MoveSigned : Op::SignHigh;
    F.Source.Register = 0;
    F.Destination.Register = B == 0x98 ? 0 : 2;
    if (B == 0x98)
      F.SourceBytes = F.Bytes / 2;
  } else if (B == 0x9e || B == 0x9f) {
    F.Operation = B == 0x9e ? Op::AHToFlags : Op::FlagsToAH;
  } else if (B == 0xf5 || B == 0xf8 || B == 0xf9 || B == 0xfc || B == 0xfd) {
    F.Operation = B == 0xf5 ? Op::ComplementFlag
                  : B & 1   ? Op::SetFlag
                            : Op::ClearFlag;
    F.Source.Immediate = B >= 0xfc ? 0x400 : 1;
  } else if (B >= 0x40 && B <= 0x4f) {
    F.Operation = B & 8 ? Op::Decrement : Op::Increment;
    F.Destination.Register = B & 7;
  } else if (B == 0xf6 || B == 0xf7) {
    Width(B == 0xf6);
    const unsigned Group = R.modRM(F.Destination);
    if (Group == 0) {
      F.Operation = Op::Test;
      Immediate(F.Bytes);
    } else if (Group == 2 || Group == 3) {
      F.Operation = Group == 2 ? Op::Complement : Op::Negate;
    } else if (Group >= 4) {
      F.Operation = Group == 4   ? Op::Multiply
                    : Group == 5 ? Op::SignedMultiply
                    : Group == 6 ? Op::Divide
                                 : Op::SignedDivide;
      std::swap(F.Source, F.Destination);
    } else {
      return std::nullopt;
    }
  } else if (B == 0xfe || B == 0xff) {
    Width(B == 0xfe);
    const unsigned Group = R.modRM(F.Destination);
    if (Group <= 1) {
      F.Operation = Group ? Op::Decrement : Op::Increment;
    } else if (B == 0xff && (Group == 2 || Group == 4 || Group == 6)) {
      F.Operation = Group == 2 ? Op::Call : Group == 4 ? Op::Jump : Op::Push;
      std::swap(F.Source, F.Destination);
    } else {
      return std::nullopt;
    }
  } else if (B >= 0x50 && B <= 0x5f) {
    F.Operation = B & 8 ? Op::Pop : Op::Push;
    (B & 8 ? F.Destination : F.Source).Register = B & 7;
  } else if (B == 0x68 || B == 0x6a) {
    F.Operation = Op::Push;
    Immediate(B == 0x6a ? 1 : F.Bytes, B == 0x6a);
  } else if (B == 0x69 || B == 0x6b) {
    F.Operation = Op::SignedMultiplyLow;
    F.Destination.Register = R.modRM(F.Source);
    const uint32_t V = R.value(B == 0x6b ? 1 : F.Bytes);
    F.MultiplyImmediate = B == 0x6b ? uint32_t(int8_t(V)) : V;
  } else if (B == 0x8f) {
    F.Operation = Op::Pop;
    if (R.modRM(F.Destination) != 0)
      return std::nullopt;
  } else if (B == 0xc2 || B == 0xc3) {
    F.Operation = Op::Return;
    if (B == 0xc2)
      F.Discard = R.value(2);
  } else if (B >= 0xe0 && B <= 0xe3) {
    F.Operation = B == 0xe3 ? Op::CounterZeroJump : Op::Loop;
    F.CounterBytes = R.AddressBytes;
    F.Condition = B & 3;
    Immediate(1, true);
  } else if (B == 0xe8 || B == 0xe9 || B == 0xeb || (B >= 0x70 && B <= 0x7f)) {
    F.Operation = B == 0xe8   ? Op::Call
                  : B >= 0xe8 ? Op::Jump
                              : Op::ConditionalJump;
    F.Relative = true;
    F.Condition = B & 15;
    Immediate(B == 0xeb || B < 0x80 ? 1 : F.Bytes,
              B == 0xeb || B < 0x80 || F.Bytes == 2);
  } else if (B == 0x0f) {
    const uint8_t Extended = R.byte();
    if (Extended == 0xb0 || Extended == 0xb1 || Extended == 0xc0 ||
        Extended == 0xc1) {
      Width(!(Extended & 1));
      F.Operation = Extended < 0xc0 ? Op::CompareExchange : Op::ExchangeAdd;
      Binary(false);
    } else if (Extended == 0xc7) {
      F.Operation = Op::CompareExchangePair;
      F.Bytes = F.SourceBytes = 8;
      if (R.modRM(F.Destination) != 1 || F.Destination.Register)
        return std::nullopt;
    } else if (Extended >= 0xc8 && Extended <= 0xcf) {
      // The architectural word form has no defined result.
      if (F.Bytes != 4)
        return std::nullopt;
      F.Operation = Op::ByteSwap;
      F.Destination.Register = Extended & 7;
    } else if (Extended >= 0x40 && Extended <= 0x4f) {
      F.Operation = Op::ConditionalMove;
      F.Condition = Extended & 15;
      F.Destination.Register = R.modRM(F.Source);
    } else if (Extended >= 0x90 && Extended <= 0x9f) {
      F.Operation = Op::SetCondition;
      F.Condition = Extended & 15;
      Width(true);
      R.modRM(F.Destination); // The reg field is ignored for SETcc.
    } else if (Extended >= 0x80 && Extended <= 0x8f) {
      F.Operation = Op::ConditionalJump;
      F.Condition = Extended & 15;
      F.Relative = true;
      Immediate(F.Bytes, F.Bytes == 2);
    } else if (Extended == 0xa4 || Extended == 0xa5 || Extended == 0xac ||
               Extended == 0xad) {
      F.Operation = Extended & 8 ? Op::DoubleShiftRight : Op::DoubleShiftLeft;
      F.Source.Register = R.modRM(F.Destination);
      if (!(Extended & 1))
        F.ShiftImmediate = R.byte();
    } else if (Extended == 0xa3 || Extended == 0xab || Extended == 0xb3 ||
               Extended == 0xbb || Extended == 0xba) {
      constexpr Op Operations[]{Op::BitTest, Op::BitSet, Op::BitReset,
                                Op::BitComplement};
      if (Extended == 0xba) {
        const unsigned Group = R.modRM(F.Destination);
        if (Group < 4)
          return std::nullopt;
        F.Operation = Operations[Group - 4];
        Immediate(1);
      } else {
        F.Operation = Operations[(Extended - 0xa3) >> 3];
        F.Source.Register = R.modRM(F.Destination);
      }
    } else if (Extended == 0xbc || Extended == 0xbd) {
      F.Operation = Extended & 1 ? Op::BitScanReverse : Op::BitScanForward;
      F.Destination.Register = R.modRM(F.Source);
    } else if (Extended == 0xaf) {
      F.Operation = Op::SignedMultiplyLow;
      F.Destination.Register = R.modRM(F.Source);
    } else if (Extended == 0xb6 || Extended == 0xb7 || Extended == 0xbe ||
               Extended == 0xbf) {
      F.Operation = Extended & 8 ? Op::MoveSigned : Op::Move;
      F.SourceBytes = Extended & 1 ? 2 : 1;
      F.Destination.Register = R.modRM(F.Source);
    } else {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
  if (!R.done())
    return std::nullopt;
  if (R.Group1Prefix && !F.lockable())
    return std::nullopt;
  F.Atomic = R.Group1Prefix ||
             (F.Operation == Op::Exchange && !F.Destination.Register);
  return F;
}

} // namespace neverd::translate::detail
