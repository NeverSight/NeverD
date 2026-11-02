//===- X86CompressExpand.h - Compact memory contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve contiguous masked memory accesses and the original opcode family.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86COMPRESSEXPAND_H
#define NEVERD_IR_INTRINSICS_X86_X86COMPRESSEXPAND_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>

namespace neverd {
inline constexpr unsigned X86CompactFloat = 0x10;
inline constexpr unsigned X86CompactStackReference = 0x20;
inline constexpr unsigned X86CompactZero = 0x80;
// A two-byte control retains i386 CS/ES/SS offsets. Other controls keep their
// existing byte layout, including long-mode stack-reference provenance.
inline constexpr unsigned X86CompactSegmentShift = 8;
inline constexpr const char *X86CompactMalformed =
    "invalid x86 compress/expand memory contract";
constexpr bool isX86CompactMemory(Intrinsic Id) {
  return Id == Intrinsic::EVEXCompressStore || Id == Intrinsic::EVEXExpandLoad;
}
struct X86CompactControl {
  unsigned ElementBytes;
  unsigned Kind;
  bool Zero;
  bool StackReference = false;
  unsigned SegmentPrefix = 0;
};
struct X86CompactOperandUse {
  uint64_t LaneMask;
  bool Address;
  bool Data;
};
/// Operand observations after the instruction shape and lane count are checked.
constexpr X86CompactOperandUse
x86CompactOperandUse(unsigned Lanes, bool Expand, bool Zero,
                     std::optional<uint64_t> KnownMask = std::nullopt) {
  assert(Lanes && Lanes <= 64);
  const uint64_t All = Lanes == 64 ? UINT64_MAX : (UINT64_C(1) << Lanes) - 1;
  const bool Access = !KnownMask || (*KnownMask & All) != 0;
  const bool Data =
      Expand ? !Zero && (!KnownMask || (*KnownMask & All) != All) : Access;
  return {All, Access, Data};
}
constexpr std::optional<X86CompactControl> x86CompactControl(Intrinsic Id,
                                                             uint64_t C) {
  const unsigned Segment = C >> X86CompactSegmentShift;
  if (!isX86CompactMemory(Id) || C > UINT16_MAX ||
      (Segment && Segment != 0x26 && Segment != 0x2e && Segment != 0x36) ||
      (Segment && (C & X86CompactStackReference)) ||
      ((C & UINT8_MAX) &
       ~(X86CompactFloat | X86CompactStackReference | X86CompactZero | 15u)) ||
      (Id == Intrinsic::EVEXCompressStore && (C & X86CompactZero)))
    return {};
  const unsigned Bytes = C & 15;
  if (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8)
    return {};
  const bool FP = C & X86CompactFloat;
  if (FP && Bytes < 4)
    return {};
  return X86CompactControl{Bytes,
                           FP           ? (Bytes == 4 ? 4u : 5u)
                           : Bytes == 1 ? 0u
                           : Bytes == 2 ? 1u
                           : Bytes == 4 ? 2u
                                        : 3u,
                           bool(C & X86CompactZero),
                           bool(C & X86CompactStackReference), Segment};
}
struct X86CompactShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  unsigned OutputSize = 0;
  unsigned Arguments = 0;
  std::array<unsigned, 4> Sizes{};
  std::array<bool, 4> Scalars{};
  std::optional<uint64_t> Control;
};
constexpr bool x86CompactShapeIsValid(Intrinsic Id, const X86CompactShape &S) {
  if (!S.ValidId || !S.Control || !x86CompactControl(Id, *S.Control) ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X64 &&
       S.TargetArch != Arch::X86) ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      !isKnownMemoryAddressSpace(S.MemoryAddressSpace) || S.Arguments != 4 ||
      S.Sizes[0] != 8 || !S.Sizes[1] || S.Sizes[1] > 8 ||
      (S.Sizes[2] != 16 && S.Sizes[2] != 32 && S.Sizes[2] != 64))
    return false;
  const auto C = *x86CompactControl(Id, *S.Control);
  if (S.Sizes[3] != (C.SegmentPrefix ? 2u : 1u) ||
      (C.SegmentPrefix &&
       ((S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X86) ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default)))
    return false;
  for (bool Scalar : S.Scalars)
    if (!Scalar)
      return false;
  if (C.StackReference &&
      (S.TargetArch == Arch::X86 ||
       S.MemoryAddressSpace != NdMemoryAddressSpace::Default))
    return false;
  if (S.Sizes[1] * 8 < S.Sizes[2] / C.ElementBytes)
    return false;
  return Id == Intrinsic::EVEXExpandLoad
             ? S.WritableOutput && S.OutputSize == S.Sizes[2]
             : !S.OutputSize;
}
} // namespace neverd
#endif // NEVERD_IR_INTRINSICS_X86_X86COMPRESSEXPAND_H
