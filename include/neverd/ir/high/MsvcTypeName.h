//===- MsvcTypeName.h - Fundamental Windows C++ spellings -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Display spellings for fundamental Microsoft C++ exception types.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_HIGH_MSVCTYPENAME_H
#define NEVERD_IR_HIGH_MSVCTYPENAME_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace neverd::msvc_type_name {
struct FundamentalType {
  llvm::StringLiteral Encoding;
  llvm::StringLiteral Spelling;
  uint16_t Size;
  bool Signed;
  bool Floating;
};
// PE32 fundamental identities are distinct even when their widths agree.
// This display mapping never supplies object extents to the native proof.
inline constexpr FundamentalType FundamentalTypes[] = {
    {".C", "signed char", 1, true, false},
    {".D", "char", 1, true, false},
    {".E", "unsigned char", 1, false, false},
    {".F", "short", 2, true, false},
    {".G", "unsigned short", 2, false, false},
    {".H", "int", 4, true, false},
    {".I", "unsigned int", 4, false, false},
    {".J", "long", 4, true, false},
    {".K", "unsigned long", 4, false, false},
    {".M", "float", 4, true, true},
    {".N", "double", 8, true, true},
    {"._J", "long long", 8, true, false},
    {"._K", "unsigned long long", 8, false, false},
    {"._N", "bool", 1, false, false},
    {"._W", "wchar_t", 2, false, false},
    {"._S", "char16_t", 2, false, false},
    {"._U", "char32_t", 4, false, false}};

inline std::optional<FundamentalType> fundamental(llvm::StringRef Name) {
  for (const auto &Type : FundamentalTypes)
    if (Name == Type.Encoding)
      return Type;
  return std::nullopt;
}

inline bool isFundamentalSpelling(llvm::StringRef Name) {
  for (const auto &Type : FundamentalTypes)
    if (Name == Type.Spelling)
      return true;
  return false;
}
} // namespace neverd::msvc_type_name
#endif
