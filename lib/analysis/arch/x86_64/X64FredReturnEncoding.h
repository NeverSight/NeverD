//===- X64FredReturnEncoding.h - Canonical FRED returns --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original bytes that bind the explicit FRED return-state owner.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FREDRETURNENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FREDRETURNENCODING_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

/// True selects ERETU, false ERETS. Ignored prefixes are not admitted here.
inline std::optional<bool> x64FredReturn(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() != 4 || Bytes[1] != 0x0f || Bytes[2] != 0x01 ||
      Bytes[3] != 0xca || (Bytes[0] != 0xf2 && Bytes[0] != 0xf3))
    return std::nullopt;
  return Bytes[0] == 0xf3;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64FREDRETURNENCODING_H
