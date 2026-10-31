//===- X86SvmEncoding.h - Original VMRUN bytes ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Canonical long-mode VMRUN encodings with explicit address width.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86SVMENCODING_H
#define NEVERD_LIFT_X86_X86SVMENCODING_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd {

inline std::optional<unsigned>
x64VmrunAddressWidth(llvm::ArrayRef<uint8_t> Bytes) {
  unsigned Width = 64;
  if (Bytes.size() == 4 && Bytes.front() == 0x67) {
    Width = 32;
    Bytes = Bytes.drop_front();
  }
  if (Bytes.size() == 3 && Bytes[0] == 0x0f && Bytes[1] == 0x01 &&
      Bytes[2] == 0xd8)
    return Width;
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_LIFT_X86_X86SVMENCODING_H
