//===- X86DebugRegisterEncoding.h - Original MOV-DR bytes ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode the register-only debug move grammar, including faulting encodings.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIFT_X86_X86DEBUGREGISTERENCODING_H
#define NEVERD_LIFT_X86_X86DEBUGREGISTERENCODING_H

#include "neverd/ir/X86/X86DebugRegisterRules.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd {
struct X86DebugRegisterEncoding {
  X86DebugRegisterAccess Access;
  unsigned Size;
};

/// Reads the first instruction of a window. At most one prefix per legacy
/// group and one final REX are admitted. LOCK/REX.R are recognized #UD forms;
/// the ModR/M mod bits are ignored, never a memory addressing mode.
inline std::optional<X86DebugRegisterEncoding>
x86DebugRegisterEncoding(llvm::ArrayRef<uint8_t> Bytes, bool LongMode) {
  unsigned At = 0, Groups = 0;
  bool Lock = false;
  while (At < Bytes.size()) {
    const auto B = Bytes[At];
    const unsigned Group = B == 0xf0 || B == 0xf2 || B == 0xf3 ? 1
                           : B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e ||
                                   B == 0x64 || B == 0x65
                               ? 2
                           : B == 0x66 ? 4
                           : B == 0x67 ? 8
                                       : 0;
    if (!Group)
      break;
    if (Groups & Group)
      return std::nullopt;
    Groups |= Group;
    Lock |= B == 0xf0;
    ++At;
  }
  uint8_t Rex = 0;
  if (LongMode && At < Bytes.size() && Bytes[At] >= 0x40 && Bytes[At] <= 0x4f)
    Rex = Bytes[At++];
  if (At + 3 > Bytes.size() || At + 3 > 15 || Bytes[At] != 0x0f ||
      (Bytes[At + 1] != 0x21 && Bytes[At + 1] != 0x23))
    return std::nullopt;
  const auto ModRM = Bytes[At + 2];
  return X86DebugRegisterEncoding{
      {uint8_t((ModRM >> 3) & 7), uint8_t((ModRM & 7) | ((Rex & 1) << 3)),
       Bytes[At + 1] == 0x23, Lock || bool(Rex & 4)},
      At + 3};
}
} // namespace neverd
#endif // NEVERD_LIFT_X86_X86DEBUGREGISTERENCODING_H
