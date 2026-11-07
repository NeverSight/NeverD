//===- X64FarPointerEncoding.h - Explicit far-pointer operands -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Closed byte grammar for descriptor-owning LSS/LFS/LGS continuations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FARPOINTERENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FARPOINTERENCODING_H

#include "X64SegmentEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

enum class X64FarPointerSegment : uint8_t { SS, FS, GS };

struct X64FarPointerLoad : X64SegmentMemoryOperand {
  unsigned Destination = 0;
  unsigned Bytes = 4;
  X64FarPointerSegment DestinationSegment = X64FarPointerSegment::FS;
};

/// Decode raw bytes rather than Capstone's normalized prefix/operand fields.
/// Only one prefix per group, an optional final REX, and a complete memory
/// operand are admitted. This is not an ordinary LowIR lifting certificate.
inline std::optional<X64FarPointerLoad>
x64FarPointerLoad(llvm::ArrayRef<uint8_t> Bytes) {
  X64FarPointerLoad F;
  size_t P = 0;
  X64SegmentPrefixes Prefix;
  if (!x64SegmentPrefixes(Bytes, P, Prefix))
    return std::nullopt;
  F.AddressBytes = Prefix.AddressBytes;
  F.Segment = Prefix.Segment;
  const uint8_t Rex = Prefix.Rex;
  if (P + 3 > Bytes.size() || Bytes[P] != 0x0f ||
      (Bytes[P + 1] != 0xb2 && Bytes[P + 1] != 0xb4 && Bytes[P + 1] != 0xb5))
    return std::nullopt;
  F.DestinationSegment = Bytes[P + 1] == 0xb2   ? X64FarPointerSegment::SS
                         : Bytes[P + 1] == 0xb4 ? X64FarPointerSegment::FS
                                                : X64FarPointerSegment::GS;
  F.Bytes = Rex & 8 ? 8 : Prefix.Operand16 ? 2 : 4;
  const uint8_t ModRM = Bytes[P + 2];
  P += 3;
  F.Destination = ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0);
  if (!x64SegmentMemoryOperand(Bytes, P, ModRM, Rex, F))
    return std::nullopt;
  return F;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FARPOINTERENCODING_H
