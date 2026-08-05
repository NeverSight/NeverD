//===- NdTypes.cpp - NeverD IR type system -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NeverD type system implementation.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/NdTypes.h"

#include "llvm/Support/raw_ostream.h"

#include <string>

namespace neverd {

std::string NdType::str() const {
  switch (Kind) {
  case NdTypeKind::Void:
    return "void";
  case NdTypeKind::Int:
    if (IsSigned) {
      switch (Size) {
      case 1:
        return "i8";
      case 2:
        return "i16";
      case 4:
        return "i32";
      case 8:
        return "i64";
      default:
        return "i" + std::to_string(Size * 8);
      }
    } else {
      switch (Size) {
      case 1:
        return "u8";
      case 2:
        return "u16";
      case 4:
        return "u32";
      case 8:
        return "u64";
      default:
        return "u" + std::to_string(Size * 8);
      }
    }
  case NdTypeKind::Float:
    return Size == 4 ? "float" : "double";
  case NdTypeKind::Ptr:
    return Pointee ? Pointee->str() + "*" : "void*";
  case NdTypeKind::Unknown:
    return Size > 0 ? "unk" + std::to_string(Size * 8) : "unk";
  default:
    return "?";
  }
}

} // namespace neverd
