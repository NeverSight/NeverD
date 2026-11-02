//===- X86HleEncoding.h - Owned long-mode HLE byte grammar -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read the original prefix, operand and address without relying on a decoder's
/// normalized HLE fields. This is admission to an explicit native source step,
/// never a certificate for ordinary memory lowering.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86HLEENCODING_H
#define NEVERD_LIFT_X86_X86HLEENCODING_H

#include "neverd/ir/intrinsics/X86/X86Hle.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

namespace neverd {

struct X86HleEncoding {
  X86HleOperation Operation = X86HleOperation::Add;
  unsigned Bytes = 4;
  unsigned AddressBytes = 8;
  unsigned Segment = 0; // 0: flat, 1: original FS, 2: original GS.
  unsigned SourceOffset = 0;
  bool RegisterSource = false;
  uint64_t Immediate = 0;
  bool Acquire = false;
  std::optional<unsigned> Base;
  std::optional<unsigned> Index;
  unsigned Scale = 1;
  bool Relative = false;
  int64_t Displacement = 0;

  bool bitString() const {
    return Operation == X86HleOperation::Btc ||
           Operation == X86HleOperation::Btr ||
           Operation == X86HleOperation::Bts;
  }

  uint32_t control() const {
    const unsigned Width = Bytes == 8 ? 3 : Bytes == 4 ? 2 : Bytes == 2 ? 1 : 0;
    const unsigned Output =
        Operation == X86HleOperation::Xadd || Operation == X86HleOperation::Xchg
            ? SourceOffset
            : 0;
    return unsigned(Operation) | (Acquire ? X86HleAcquire : 0) |
           X86HleAbortBeforePublication | (Width << X86HleWidthShift) |
           (Output << X86HleSourceShift);
  }
};

inline std::optional<X86HleEncoding>
x86HleEncoding(llvm::ArrayRef<uint8_t> Bytes) {
  using O = X86HleOperation;
  if (Bytes.empty() || Bytes.size() > 15)
    return {};
  X86HleEncoding E;
  size_t P = 0;
  bool Operand16 = false, Lock = false, Segment = false;
  uint8_t Hint = 0;
  for (; P < Bytes.size(); ++P) {
    const auto B = Bytes[P];
    if (B == 0xf2 || B == 0xf3)
      Hint = B; // The last F2/F3 is the architectural HLE hint.
    else if (B == 0xf0 && !Lock)
      Lock = true;
    else if (B == 0x66 && !Operand16)
      Operand16 = true;
    else if (B == 0x67 && E.AddressBytes == 8)
      E.AddressBytes = 4;
    else if (!Segment && (B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e ||
                          B == 0x64 || B == 0x65)) {
      Segment = true;
      E.Segment = B == 0x64 ? 1 : B == 0x65 ? 2 : 0;
    } else
      break;
  }
  uint8_t Rex = 0;
  if (P < Bytes.size() && (Bytes[P] & 0xf0) == 0x40)
    Rex = Bytes[P++];
  if (!Hint || P == Bytes.size())
    return {};
  E.Acquire = Hint == 0xf2;
  E.Bytes = Rex & 8 ? 8 : Operand16 ? 2 : 4;
  unsigned Opcode = Bytes[P++];
  if (Opcode == 0x0f) {
    if (P == Bytes.size())
      return {};
    Opcode = 0x100 | Bytes[P++];
  }
  if (P == Bytes.size())
    return {};
  const uint8_t ModRM = Bytes[P++];
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7, Ext = (ModRM >> 3) & 7;
  if (Mod == 3)
    return {};
  const unsigned Reg = Ext + ((Rex & 4) ? 8 : 0);
  E.SourceOffset = Reg * 8;
  unsigned ImmediateBytes = 0;
  bool SignedImmediate = false, ByteOperand = false;
  const auto Arithmetic = [](unsigned I) -> std::optional<O> {
    constexpr O Ops[] = {O::Add, O::Or, O::Adc, O::Sbb, O::And, O::Sub, O::Xor};
    if (I >= std::size(Ops))
      return {};
    return Ops[I];
  };
  if (Opcode <= 0x31 && (Opcode & 6) == 0) {
    const auto Op = Arithmetic(Opcode >> 3);
    if (!Op)
      return {};
    E.Operation = *Op;
    E.RegisterSource = true;
    ByteOperand = !(Opcode & 1);
  } else if (Opcode == 0x80 || Opcode == 0x81 || Opcode == 0x83) {
    const auto Op = Arithmetic(Ext);
    if (!Op)
      return {};
    E.Operation = *Op;
    ByteOperand = Opcode == 0x80;
    ImmediateBytes = Opcode == 0x81 ? (E.Bytes == 2 ? 2 : 4) : 1;
    SignedImmediate = Opcode == 0x83 || E.Bytes == 8;
  } else if (Opcode == 0xfe || Opcode == 0xff || Opcode == 0xf6 ||
             Opcode == 0xf7) {
    const bool Increment = Opcode == 0xfe || Opcode == 0xff;
    if ((Increment && Ext > 1) || (!Increment && Ext != 2 && Ext != 3))
      return {};
    E.Operation =
        Increment ? (Ext == 0 ? O::Inc : O::Dec) : (Ext == 2 ? O::Not : O::Neg);
    ByteOperand = !(Opcode & 1);
  } else if (Opcode == 0x86 || Opcode == 0x87 || Opcode == 0x88 ||
             Opcode == 0x89 || Opcode == 0x1b0 || Opcode == 0x1b1 ||
             Opcode == 0x1c0 || Opcode == 0x1c1) {
    E.Operation = Opcode < 0x88    ? O::Xchg
                  : Opcode < 0x100 ? O::Mov
                  : Opcode < 0x1c0 ? O::Cmpxchg
                                   : O::Xadd;
    E.RegisterSource = true;
    ByteOperand = !(Opcode & 1);
  } else if (Opcode == 0xc6 || Opcode == 0xc7) {
    if (Ext)
      return {};
    E.Operation = O::Mov;
    ByteOperand = Opcode == 0xc6;
    ImmediateBytes = ByteOperand ? 1 : E.Bytes == 2 ? 2 : 4;
    SignedImmediate = E.Bytes == 8;
  } else if (Opcode == 0x1ab || Opcode == 0x1b3 || Opcode == 0x1bb ||
             Opcode == 0x1ba) {
    if (Opcode == 0x1ba) {
      if (Ext < 5)
        return {};
      E.Operation = Ext == 5 ? O::Bts : Ext == 6 ? O::Btr : O::Btc;
      ImmediateBytes = 1;
    } else {
      E.Operation = Opcode == 0x1ab   ? O::Bts
                    : Opcode == 0x1b3 ? O::Btr
                                      : O::Btc;
      E.RegisterSource = true;
    }
  } else if (Opcode == 0x1c7 && Ext == 1 && !(Rex & 8) && !Operand16) {
    E.Operation = O::Cmpxchg8b;
    E.Bytes = 8;
  } else {
    return {};
  }
  if (ByteOperand)
    E.Bytes = 1;
  if (E.RegisterSource && E.Bytes == 1 && !Rex && Reg >= 4)
    E.SourceOffset = (Reg - 4) * 8 + 1;
  if (E.Operation == O::Mov ? (Lock || E.Acquire)
                            : (!Lock && E.Operation != O::Xchg))
    return {};
  unsigned DispBytes = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
  if (RM == 4) {
    if (P == Bytes.size())
      return {};
    const uint8_t SIB = Bytes[P++];
    E.Scale = 1u << (SIB >> 6);
    const unsigned Index = (SIB >> 3) & 7, Base = SIB & 7;
    if (Index != 4 || (Rex & 2))
      E.Index = Index + ((Rex & 2) ? 8 : 0);
    if (Mod == 0 && Base == 5)
      DispBytes = 4;
    else
      E.Base = Base + ((Rex & 1) ? 8 : 0);
  } else if (Mod == 0 && RM == 5) {
    E.Relative = true;
    DispBytes = 4;
  } else {
    E.Base = RM + ((Rex & 1) ? 8 : 0);
  }
  if (P + DispBytes + ImmediateBytes != Bytes.size())
    return {};
  uint32_t Displacement = 0;
  for (unsigned I = 0; I != DispBytes; ++I)
    Displacement |= uint32_t(Bytes[P++]) << (I * 8);
  E.Displacement =
      DispBytes == 1 ? int8_t(Displacement) : int32_t(Displacement);
  for (unsigned I = 0; I != ImmediateBytes; ++I)
    E.Immediate |= uint64_t(Bytes[P++]) << (I * 8);
  if (SignedImmediate)
    E.Immediate = ImmediateBytes == 1   ? int8_t(E.Immediate)
                  : ImmediateBytes == 2 ? int16_t(E.Immediate)
                                        : int32_t(E.Immediate);
  return x86HlePlan(E.control()) ? std::optional(E) : std::nullopt;
}

} // namespace neverd

#endif // NEVERD_LIFT_X86_X86HLEENCODING_H
