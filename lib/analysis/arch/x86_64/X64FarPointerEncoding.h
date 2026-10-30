//===- X64FarPointerEncoding.h - Explicit far-pointer operands -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Closed byte grammar for the descriptor-owning LFS/LGS continuation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FARPOINTERENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FARPOINTERENCODING_H

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

struct X64FarPointerLoad {
  unsigned Destination = 0;
  unsigned Bytes = 4;
  unsigned AddressBytes = 8;
  bool GS = false;
  // Zero for ordinary addressing, 1 for FS, 2 for GS.
  unsigned Segment = 0;
  std::optional<unsigned> Base;
  std::optional<unsigned> Index;
  unsigned Scale = 1;
  bool Relative = false;
  int64_t Displacement = 0;
};

/// Decode raw bytes rather than Capstone's normalized prefix/operand fields.
/// Only one prefix per group, an optional final REX, and a complete memory
/// operand are admitted. This is not an ordinary LowIR lifting certificate.
inline std::optional<X64FarPointerLoad>
x64FarPointerLoad(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.empty() || Bytes.size() > 15)
    return std::nullopt;
  X64FarPointerLoad F;
  size_t P = 0;
  bool Operand16 = false, Address32 = false;
  while (P < Bytes.size()) {
    if (Bytes[P] == 0x66 && !Operand16)
      Operand16 = true;
    else if (Bytes[P] == 0x67 && !Address32)
      Address32 = true;
    else if ((Bytes[P] == 0x64 || Bytes[P] == 0x65) && !F.Segment)
      F.Segment = Bytes[P] - 0x63;
    else
      break;
    ++P;
  }
  uint8_t Rex = 0;
  if (P < Bytes.size() && (Bytes[P] & 0xf0) == 0x40)
    Rex = Bytes[P++];
  if (P + 3 > Bytes.size() || Bytes[P] != 0x0f ||
      (Bytes[P + 1] != 0xb4 && Bytes[P + 1] != 0xb5))
    return std::nullopt;
  F.GS = Bytes[P + 1] == 0xb5;
  F.Bytes = Rex & 8 ? 8 : Operand16 ? 2 : 4;
  F.AddressBytes = Address32 ? 4 : 8;
  const uint8_t ModRM = Bytes[P + 2];
  P += 3;
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7;
  if (Mod == 3)
    return std::nullopt;
  F.Destination = ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0);
  unsigned DispBytes = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
  if (RM == 4) {
    if (P == Bytes.size())
      return std::nullopt;
    const uint8_t SIB = Bytes[P++];
    F.Scale = 1u << (SIB >> 6);
    const unsigned Index = (SIB >> 3) & 7, Base = SIB & 7;
    if (Index != 4 || (Rex & 2))
      F.Index = Index + ((Rex & 2) ? 8 : 0);
    if (Mod == 0 && Base == 5)
      DispBytes = 4;
    else
      F.Base = Base + ((Rex & 1) ? 8 : 0);
  } else if (Mod == 0 && RM == 5) {
    F.Relative = true;
    DispBytes = 4;
  } else {
    F.Base = RM + ((Rex & 1) ? 8 : 0);
  }
  if (P + DispBytes != Bytes.size())
    return std::nullopt;
  uint32_t Displacement = 0;
  for (unsigned I = 0; I != DispBytes; ++I)
    Displacement |= uint32_t(Bytes[P + I]) << (I * 8);
  F.Displacement =
      DispBytes == 1 ? int8_t(Displacement) : int32_t(Displacement);
  return F;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FARPOINTERENCODING_H
