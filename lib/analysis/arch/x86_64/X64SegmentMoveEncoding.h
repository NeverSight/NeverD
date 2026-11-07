//===- X64SegmentMoveEncoding.h - Selector transfer bytes -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate long-mode selector moves and FS/GS stack transfers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64SEGMENTMOVEENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64SEGMENTMOVEENCODING_H

#include "X64SegmentEncoding.h"

#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

struct X64SegmentMove : X64SegmentMemoryOperand {
  X86SegmentRegister SegmentRegister = X86SegmentRegister::ES;
  std::optional<unsigned> Register;
  unsigned Bytes = 2;
  bool Read = false;
  bool Pop = false;
  bool Push = false;
};

inline std::optional<X64SegmentMove>
x64SegmentMove(llvm::ArrayRef<uint8_t> Bytes) {
  size_t P = 0;
  X64SegmentPrefixes Prefix;
  if (!x64SegmentPrefixes(Bytes, P, Prefix))
    return std::nullopt;
  X64SegmentMove F;
  if (P + 2 == Bytes.size() && Bytes[P] == 0x0f &&
      (Bytes[P + 1] == 0xa0 || Bytes[P + 1] == 0xa1 || Bytes[P + 1] == 0xa8 ||
       Bytes[P + 1] == 0xa9)) {
    F.Pop = Bytes[P + 1] & 1;
    F.Read = F.Push = !F.Pop;
    F.SegmentRegister =
        Bytes[P + 1] & 8 ? X86SegmentRegister::GS : X86SegmentRegister::FS;
    F.Base = 4;
    F.Bytes = Prefix.Rex & 8 ? 8 : Prefix.Operand16 ? 2 : 8;
    return F;
  }
  if (P + 2 > Bytes.size() || (Bytes[P] != 0x8c && Bytes[P] != 0x8e))
    return std::nullopt;
  F.Read = Bytes[P] == 0x8c;
  const uint8_t ModRM = Bytes[P + 1];
  const unsigned Segment = (ModRM >> 3) & 7;
  if (!x86SegmentRegisterName(Segment) || (!F.Read && Segment == 1))
    return std::nullopt;
  F.SegmentRegister = static_cast<X86SegmentRegister>(Segment);
  P += 2;
  if ((ModRM >> 6) == 3) {
    if (P != Bytes.size())
      return std::nullopt;
    F.Register = (ModRM & 7) + ((Prefix.Rex & 1) ? 8 : 0);
    if (F.Read)
      F.Bytes = Prefix.Rex & 8 ? 8 : Prefix.Operand16 ? 2 : 4;
  } else {
    F.Segment = Prefix.Segment;
    F.AddressBytes = Prefix.AddressBytes;
    if (!x64SegmentMemoryOperand(Bytes, P, ModRM, Prefix.Rex, F))
      return std::nullopt;
  }
  return F;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64SEGMENTMOVEENCODING_H
