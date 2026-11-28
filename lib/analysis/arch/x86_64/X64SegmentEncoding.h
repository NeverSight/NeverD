//===- X64SegmentEncoding.h - Segment-transition operand bytes -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared prefix and memory grammar for descriptor-owning segment transfers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64SEGMENTENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64SEGMENTENCODING_H

#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

struct X64SegmentPrefixes {
  bool Operand16 = false;
  unsigned AddressBytes = 8;
  // Long mode ignores CS/DS/ES/SS overrides; FS=1, GS=2.
  unsigned Segment = 0;
  uint8_t Rex = 0;
};

struct X64SegmentMemoryOperand {
  unsigned AddressBytes = 8;
  unsigned Segment = 0;
  std::optional<unsigned> Base;
  std::optional<unsigned> Index;
  unsigned Scale = 1;
  bool Relative = false;
  int64_t Displacement = 0;
};

/// One prefix per group, followed by at most one final REX. A caller that
/// handles a LOCK/REP encoding fault can request its presence explicitly.
inline bool x64SegmentPrefixes(llvm::ArrayRef<uint8_t> Bytes, size_t &P,
                               X64SegmentPrefixes &F,
                               bool *SawLockOrRep = nullptr) {
  if (Bytes.empty() || Bytes.size() > x86::kMaxInstructionLen)
    return false;
  if (SawLockOrRep)
    *SawLockOrRep = false;
  bool Segment = false;
  while (P < Bytes.size()) {
    if (SawLockOrRep && !*SawLockOrRep &&
        (Bytes[P] == 0xf0 || Bytes[P] == 0xf2 || Bytes[P] == 0xf3))
      *SawLockOrRep = true;
    else if (Bytes[P] == 0x66 && !F.Operand16)
      F.Operand16 = true;
    else if (Bytes[P] == 0x67 && F.AddressBytes == 8)
      F.AddressBytes = 4;
    else if ((Bytes[P] == 0x26 || Bytes[P] == 0x2e || Bytes[P] == 0x36 ||
              Bytes[P] == 0x3e || Bytes[P] == 0x64 || Bytes[P] == 0x65) &&
             !Segment) {
      Segment = true;
      F.Segment = Bytes[P] >= 0x64 ? Bytes[P] - 0x63 : 0;
    } else
      break;
    ++P;
  }
  if (P < Bytes.size() && (Bytes[P] & 0xf0) == 0x40)
    F.Rex = Bytes[P++];
  return P < Bytes.size();
}

inline bool x64SegmentMemoryOperand(llvm::ArrayRef<uint8_t> Bytes, size_t P,
                                    uint8_t ModRM, uint8_t Rex,
                                    X64SegmentMemoryOperand &F) {
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7;
  if (Mod == 3)
    return false;
  unsigned DispBytes = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
  if (RM == 4) {
    if (P == Bytes.size())
      return false;
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
    return false;
  uint32_t Displacement = 0;
  for (unsigned I = 0; I != DispBytes; ++I)
    Displacement |= uint32_t(Bytes[P + I]) << (I * 8);
  F.Displacement =
      DispBytes == 1 ? int8_t(Displacement) : int32_t(Displacement);
  return true;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64SEGMENTENCODING_H
