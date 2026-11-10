//===- MachOArm64eABI.h - Native source ABI contracts ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOARM64EABI_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOARM64EABI_H

#include <cstdint>

namespace neverd::macho_arm64e_abi {
#define MACHO_ARM64E_ABI_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define MACHO_ARM64E_ABI_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "MachOArm64eABI.def"
} // namespace neverd::macho_arm64e_abi

#endif
