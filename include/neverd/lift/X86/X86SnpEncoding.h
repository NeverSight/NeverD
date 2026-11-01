//===- X86SnpEncoding.h - SNP source admission -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Canonical SNP instructions admitted only with an explicit native
/// environment. The decoder may recognize more prefixes; they do not acquire
/// this contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86SNPENCODING_H
#define NEVERD_LIFT_X86_X86SNPENCODING_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct X86SnpEncoding {
  Intrinsic Id;
  uint32_t Control;
};

inline std::optional<X86SnpEncoding>
x86SnpEncoding(llvm::ArrayRef<uint8_t> Bytes) {
  const bool Address32 = Bytes.size() == 5 && Bytes.front() == 0x67;
  if (Address32)
    Bytes = Bytes.drop_front();
  if (Bytes.size() != 4 || Bytes[1] != 0x0f || Bytes[2] != 0x01)
    return {};
  if (!Address32 && Bytes[3] == 0xd9 && (Bytes[0] == 0xf2 || Bytes[0] == 0xf3))
    return X86SnpEncoding{Intrinsic::X86SevVmgexit, 0};
#define NEVERD_X86_SNP(Id, Name, Prefix, ModRM)                                \
  if (Bytes[0] == Prefix && Bytes[3] == ModRM &&                               \
      (!Address32 || Intrinsic::Id == Intrinsic::X86SnpPvalidate))             \
    return X86SnpEncoding{Intrinsic::Id, uint32_t(Address32)};
#include "neverd/ir/intrinsics/X86/X86Snp.def"
#undef NEVERD_X86_SNP
  return {};
}

} // namespace neverd

#endif // NEVERD_LIFT_X86_X86SNPENCODING_H
