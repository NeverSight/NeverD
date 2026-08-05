//===- NdTypes.h - NeverD IR type system --------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the NdType structure representing types in the NeverD IR,
/// including integer, float, pointer, array, struct, and function types.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_NDTYPES_H
#define NEVERD_IR_NDTYPES_H

#include "neverd/Common.h"

#include <memory>
#include <string>
#include <vector>

namespace neverd {

enum class NdTypeKind : uint8_t {
  Void,
  Int,
  Float,
  Ptr,
  Array,
  Struct,
  Func,
  Unknown
};

struct NdType {
  NdTypeKind Kind = NdTypeKind::Unknown;
  uint16_t Size = 0;
  bool IsSigned = false;

  /// For Ptr
  std::shared_ptr<NdType> Pointee;

  /// For Array
  uint32_t ArrayCount = 0;
  std::shared_ptr<NdType> ElemType;

  /// For Func
  std::shared_ptr<NdType> RetType;
  std::vector<std::shared_ptr<NdType>> ParamTypes;

  static std::shared_ptr<NdType> makeVoid() {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Void;
    T->Size = 0;
    return T;
  }
  static std::shared_ptr<NdType> makeInt(uint16_t Sz, bool S = true) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Int;
    T->Size = Sz;
    T->IsSigned = S;
    return T;
  }
  static std::shared_ptr<NdType> makeFloat(uint16_t Sz) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Float;
    T->Size = Sz;
    return T;
  }
  static std::shared_ptr<NdType> makePtr(std::shared_ptr<NdType> Pt = nullptr) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Ptr;
    T->Size = 8;
    T->Pointee = Pt ? Pt : makeInt(1, false);
    return T;
  }

  std::string str() const;
};

using TypeRef = std::shared_ptr<NdType>;

} // namespace neverd

#endif // NEVERD_IR_NDTYPES_H
