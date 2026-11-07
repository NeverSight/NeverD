//===- X64UserInterruptEncoding.h - UINTR state-step admission --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Canonical instruction identities shared by the binary and state owners.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64USERINTERRUPTENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64USERINTERRUPTENCODING_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd::analysis::detail {

enum class X64UserInterrupt { Return, Test, Clear, Set };

inline std::optional<X64UserInterrupt>
x64UserInterrupt(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() != 4 || Bytes[0] != 0xf3 || Bytes[1] != 0x0f ||
      Bytes[2] != 0x01)
    return std::nullopt;
  switch (Bytes[3]) {
  case 0xec:
    return X64UserInterrupt::Return;
  case 0xed:
    return X64UserInterrupt::Test;
  case 0xee:
    return X64UserInterrupt::Clear;
  case 0xef:
    return X64UserInterrupt::Set;
  default:
    return std::nullopt;
  }
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64USERINTERRUPTENCODING_H
