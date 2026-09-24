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
#include <optional>
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

  /// Natural record layout for source projection. Member names are not
  /// recovered; the ordered types and byte offsets define structural identity.
  std::vector<std::shared_ptr<NdType>> Fields;
  std::vector<uint16_t> FieldOffsets;
  uint16_t Alignment = 0;

  /// For Func
  std::shared_ptr<NdType> RetType;
  std::vector<std::shared_ptr<NdType>> ParamTypes;

  /// Display-only C++/C spelling from a validated type graph
  /// (`char`, `ProbeError`, `Ns::Outer::Inner`).  A SourceName-only
  /// struct is not an ABI layout and must not authorize extents.
  std::string SourceName;
  /// Display-only TPI member names.  Parallel to FieldDisplayOffsets
  /// and FieldDisplayTypes; never an ABI layout and must not authorize
  /// extents.  An empty name at an offset is a tombstone for an
  /// ambiguous union/bitfield slot.
  std::vector<std::string> FieldDisplayNames;
  std::vector<uint16_t> FieldDisplayOffsets;
  /// Display-only TPI member types, used to name nested slots such as
  /// `TPtr<T>::p` at `outer+8`.  Not an ABI layout.
  std::vector<std::shared_ptr<NdType>> FieldDisplayTypes;
  /// LF_ENUM records share the named-record encoding; they return in RAX.
  bool IsEnum = false;

  /// Unique display field at exactly \p Offset.  Ambiguous or unnamed
  /// slots return nullopt; do not guess a member.  A nonzero
  /// \p AccessSize keeps the unique overlay whose type width matches
  /// (CPtred `_m_nRef` vs `_m_pNext` at +8). Same-width overlays stay raw.
  std::optional<std::string> displayFieldNameAt(uint64_t Offset,
                                                uint16_t AccessSize = 0) const {
    if (Kind != NdTypeKind::Struct ||
        FieldDisplayNames.size() != FieldDisplayOffsets.size())
      return std::nullopt;
    const bool FilterSize =
        AccessSize != 0 &&
        FieldDisplayTypes.size() == FieldDisplayOffsets.size();
    const std::string *Found = nullptr;
    for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
      if (FieldDisplayOffsets[I] != Offset || FieldDisplayNames[I].empty())
        continue;
      if (FilterSize) {
        const auto &Ty = FieldDisplayTypes[I];
        if (Ty && Ty->Size != 0 && Ty->Size != AccessSize)
          continue;
      }
      if (Found && *Found != FieldDisplayNames[I])
        return std::nullopt;
      Found = &FieldDisplayNames[I];
    }
    if (!Found)
      return std::nullopt;
    return *Found;
  }

  std::shared_ptr<NdType> displayFieldTypeAt(uint64_t Offset,
                                             uint16_t AccessSize = 0) const {
    if (Kind != NdTypeKind::Struct ||
        FieldDisplayNames.size() != FieldDisplayOffsets.size() ||
        FieldDisplayTypes.size() != FieldDisplayOffsets.size())
      return nullptr;
    std::shared_ptr<NdType> ExactTy;
    const std::string *ExactName = nullptr;
    for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
      if (FieldDisplayOffsets[I] != Offset || FieldDisplayNames[I].empty())
        continue;
      if (AccessSize != 0) {
        const auto &Ty = FieldDisplayTypes[I];
        if (Ty && Ty->Size != 0 && Ty->Size != AccessSize)
          continue;
      }
      if (ExactName && *ExactName != FieldDisplayNames[I])
        return nullptr;
      ExactName = &FieldDisplayNames[I];
      ExactTy = FieldDisplayTypes[I];
    }
    if (ExactName) {
      if (ExactTy && ExactTy->Kind == NdTypeKind::Struct && !ExactTy->IsEnum)
        if (auto Inner = ExactTy->displayFieldTypeAt(0, AccessSize))
          return Inner;
      return ExactTy;
    }
    size_t Best = static_cast<size_t>(-1);
    for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
      if (FieldDisplayNames[I].empty() || !FieldDisplayTypes[I] ||
          FieldDisplayTypes[I]->Kind != NdTypeKind::Struct ||
          FieldDisplayOffsets[I] > Offset)
        continue;
      const uint64_t Rel =
          Offset - static_cast<uint64_t>(FieldDisplayOffsets[I]);
      if (Rel != 0 && (FieldDisplayTypes[I]->Size == 0 ||
                       Rel >= FieldDisplayTypes[I]->Size))
        continue;
      if (Best == static_cast<size_t>(-1) ||
          FieldDisplayOffsets[I] > FieldDisplayOffsets[Best]) {
        Best = I;
        continue;
      }
      if (FieldDisplayOffsets[I] == FieldDisplayOffsets[Best] &&
          FieldDisplayNames[I] != FieldDisplayNames[Best])
        return nullptr;
    }
    if (Best == static_cast<size_t>(-1))
      return nullptr;
    const uint64_t Rel =
        Offset - static_cast<uint64_t>(FieldDisplayOffsets[Best]);
    if (Rel == 0) {
      if (auto Inner =
              FieldDisplayTypes[Best]->displayFieldTypeAt(0, AccessSize))
        return Inner;
      return FieldDisplayTypes[Best];
    }
    return FieldDisplayTypes[Best]->displayFieldTypeAt(Rel, AccessSize);
  }

  /// Unique display path at \p Offset, including nested record steps
  /// (`holder.p`, `record.id`).  No exact slot and no unique
  /// containing record field stay raw. A nonzero \p AccessSize may
  /// pick one overlay at a shared offset; same-width overlays stay raw.
  std::optional<std::string> displayFieldPathAt(
      uint64_t Offset, uint16_t AccessSize = 0,
      bool EnterNestedAtZero = true) const {
    if (auto Exact = displayFieldNameAt(Offset, AccessSize)) {
      if (EnterNestedAtZero &&
          FieldDisplayTypes.size() == FieldDisplayOffsets.size()) {
        for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
          if (FieldDisplayOffsets[I] != Offset ||
              FieldDisplayNames[I] != *Exact || !FieldDisplayTypes[I] ||
              FieldDisplayTypes[I]->Kind != NdTypeKind::Struct ||
              FieldDisplayTypes[I]->IsEnum)
            continue;
          if (auto Inner = FieldDisplayTypes[I]->displayFieldPathAt(
                  0, AccessSize, EnterNestedAtZero))
            return *Exact + "." + *Inner;
        }
      }
      return Exact;
    }
    if (Kind != NdTypeKind::Struct ||
        FieldDisplayNames.size() != FieldDisplayOffsets.size() ||
        FieldDisplayTypes.size() != FieldDisplayOffsets.size())
      return std::nullopt;
    size_t Best = static_cast<size_t>(-1);
    for (size_t I = 0; I < FieldDisplayOffsets.size(); ++I) {
      if (FieldDisplayNames[I].empty() || !FieldDisplayTypes[I] ||
          FieldDisplayTypes[I]->Kind != NdTypeKind::Struct ||
          FieldDisplayOffsets[I] > Offset)
        continue;
      const uint64_t Rel =
          Offset - static_cast<uint64_t>(FieldDisplayOffsets[I]);
      if (Rel != 0 && (FieldDisplayTypes[I]->Size == 0 ||
                       Rel >= FieldDisplayTypes[I]->Size))
        continue;
      if (Best == static_cast<size_t>(-1) ||
          FieldDisplayOffsets[I] > FieldDisplayOffsets[Best]) {
        Best = I;
        continue;
      }
      if (FieldDisplayOffsets[I] == FieldDisplayOffsets[Best] &&
          FieldDisplayNames[I] != FieldDisplayNames[Best])
        return std::nullopt;
    }
    if (Best == static_cast<size_t>(-1))
      return std::nullopt;
    const uint64_t Rel =
        Offset - static_cast<uint64_t>(FieldDisplayOffsets[Best]);
    if (Rel == 0) {
      if (EnterNestedAtZero) {
        if (auto Inner = FieldDisplayTypes[Best]->displayFieldPathAt(
                0, AccessSize, EnterNestedAtZero))
          return FieldDisplayNames[Best] + "." + *Inner;
      }
      return FieldDisplayNames[Best];
    }
    auto Inner = FieldDisplayTypes[Best]->displayFieldPathAt(
        Rel, AccessSize, EnterNestedAtZero);
    if (!Inner)
      return std::nullopt;
    return FieldDisplayNames[Best] + "." + *Inner;
  }

  NdType() = default;
  NdType(const NdType &) = default;
  NdType(NdType &&) = default;
  NdType &operator=(const NdType &) = default;
  NdType &operator=(NdType &&) = default;
  /// Scalar factories never populate display fields. A smashed vector
  /// header there is leaked rather than walked during Session teardown.
  ~NdType();

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
    T->Pointee = Pt ? Pt : makeVoid();
    return T;
  }
  /// Construct a bounded naturally aligned record. Unsupported, cyclic or
  /// malformed member layouts return null rather than guessing padding.
  static std::shared_ptr<NdType>
  makeStruct(std::vector<std::shared_ptr<NdType>> Fields);
  static std::shared_ptr<NdType>
  makeNamedRecord(std::string Name, uint16_t Size = 0, bool Enum = false) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Struct;
    T->Size = Size;
    T->SourceName = std::move(Name);
    T->IsEnum = Enum;
    return T;
  }
  static std::shared_ptr<NdType>
  makeArray(std::shared_ptr<NdType> Elem, uint32_t Count, uint16_t Size) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Array;
    T->ElemType = std::move(Elem);
    T->ArrayCount = Count;
    T->Size = Size;
    return T;
  }
  /// A fixed ordinary C function type. Calling-convention-specific source
  /// declarations require separate ABI evidence and must not use this type.
  static std::shared_ptr<NdType>
  makeFunc(std::shared_ptr<NdType> Return,
           std::vector<std::shared_ptr<NdType>> Parameters = {}) {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Func;
    T->RetType = std::move(Return);
    T->ParamTypes = std::move(Parameters);
    return T;
  }

  std::string str() const;
};

using TypeRef = std::shared_ptr<NdType>;

} // namespace neverd

#endif // NEVERD_IR_NDTYPES_H
