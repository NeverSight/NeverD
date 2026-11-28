//===- MachOArm64eABI.h - Native source ABI contracts ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// ABI identities shared by native arm64e source emission and installation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOARM64EABI_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOARM64EABI_H

#include <cstdint>
#include <map>
#include <string>

namespace neverd::macho_arm64e_abi {
#define MACHO_ARM64E_ABI_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define MACHO_ARM64E_ABI_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "MachOArm64eABI.def"

/// LLVM's verbatim prefix names the exact dyld symbol. An unqualified source
/// name selects its C spelling if present, or the sole bare assembly import.
inline std::string
importName(const std::string &Symbol,
           const std::map<std::string, bool> &ObjectImports) {
  if (Symbol.starts_with(kVerbatimPrefix))
    return Symbol.substr(sizeof(kVerbatimPrefix) - 1);
  std::string CName = kSymbolPrefix + Symbol;
  if (!Symbol.starts_with(kSymbolPrefix) && ObjectImports.contains(Symbol) &&
      !ObjectImports.contains(CName))
    return Symbol;
  return CName;
}

enum class PAuthEncoding : uint32_t {
#define NEVERD_A64_PAUTH_HINT(Name, Word, Operation, Pointer, Modifier)        \
  Name = Word,
#define NEVERD_A64_PAUTH_BRANCH(Name, Mask, Word, Key, Link, Kind) Name = Word,
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.def"
};
} // namespace neverd::macho_arm64e_abi

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOARM64EABI_H
