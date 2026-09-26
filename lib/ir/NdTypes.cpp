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

#include <algorithm>
#include <cstring>
#include <string>

namespace neverd {

namespace {
void abandonSmashedVector(void *Storage, size_t Bytes) {
  auto *BytesPtr = static_cast<unsigned char *>(Storage);
  bool BeginNull = true;
  bool EndNull = true;
  for (size_t I = 0; I < sizeof(void *); ++I) {
    if (BytesPtr[I])
      BeginNull = false;
    if (BytesPtr[sizeof(void *) + I])
      EndNull = false;
  }
  if (BeginNull != EndNull)
    std::memset(BytesPtr, 0, Bytes);
}
} // namespace

NdType::~NdType() {
  if (Kind != NdTypeKind::Int && Kind != NdTypeKind::Float &&
      Kind != NdTypeKind::Void && Kind != NdTypeKind::Unknown)
    return;
  abandonSmashedVector(&FieldDisplayNames, sizeof(FieldDisplayNames));
  abandonSmashedVector(&FieldDisplayOffsets, sizeof(FieldDisplayOffsets));
  abandonSmashedVector(&FieldDisplayTypes, sizeof(FieldDisplayTypes));
}

namespace {
bool layout(const NdType &T, uint16_t &Size, uint16_t &Alignment,
            std::vector<uint16_t> &Offsets, unsigned Depth, unsigned &Budget,
            bool Constructing = false) {
  if (!Budget || Depth > 16)
    return false;
  --Budget;
  if (T.Kind != NdTypeKind::Struct) {
    const bool Scalar =
        (T.Kind == NdTypeKind::Int &&
         (T.Size == 1 || T.Size == 2 || T.Size == 4 || T.Size == 8)) ||
        (T.Kind == NdTypeKind::Float && (T.Size == 4 || T.Size == 8)) ||
        (T.Kind == NdTypeKind::Ptr && T.Size == 8 && T.Pointee);
    Size = Alignment = T.Size;
    return Scalar;
  }
  if (T.Fields.empty() || T.Fields.size() > 64)
    return false;
  uint32_t End = 0;
  Alignment = 1;
  for (const auto &Field : T.Fields) {
    uint16_t Bytes, Align;
    std::vector<uint16_t> Nested;
    if (!Field || !layout(*Field, Bytes, Align, Nested, Depth + 1, Budget))
      return false;
    Alignment = std::max(Alignment, Align);
    End = (End + Align - 1) & -uint32_t(Align);
    Offsets.push_back(static_cast<uint16_t>(End));
    End += Bytes;
    if (End > 4096)
      return false;
  }
  Size = static_cast<uint16_t>((End + Alignment - 1) & -uint32_t(Alignment));
  return Constructing || (T.Size == Size && T.Alignment == Alignment &&
                          T.FieldOffsets == Offsets);
}
} // namespace

std::shared_ptr<NdType>
NdType::makeStruct(std::vector<std::shared_ptr<NdType>> Fields) {
  auto T = std::make_shared<NdType>();
  T->Kind = NdTypeKind::Struct;
  T->Fields = std::move(Fields);
  unsigned Budget = 4096;
  if (!layout(*T, T->Size, T->Alignment, T->FieldOffsets, 0, Budget, true))
    return nullptr;
  return T;
}

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
  case NdTypeKind::Struct: {
    // Validate before recursive printing, including cycles and stale offsets.
    unsigned Budget = 4096;
    uint16_t Bytes, Align;
    std::vector<uint16_t> Offsets;
    if (!layout(*this, Bytes, Align, Offsets, 0, Budget))
      return "invalid_record";
    std::string Result = "record{";
    for (size_t I = 0; I < Fields.size(); ++I) {
      if (I)
        Result += ",";
      Result += std::to_string(FieldOffsets[I]) + ":" +
                (Fields[I]->Kind == NdTypeKind::Ptr ? "ptr" : Fields[I]->str());
    }
    return Result + "}";
  }
  case NdTypeKind::Unknown:
    return Size > 0 ? "unk" + std::to_string(Size * 8) : "unk";
  default:
    return "?";
  }
}

} // namespace neverd
