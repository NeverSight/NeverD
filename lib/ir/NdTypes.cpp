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

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"

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
  abandonSmashedVector(&FieldDisplayBits, sizeof(FieldDisplayBits));
}

std::optional<std::string>
NdType::displayBitFieldPathAt(uint64_t BitOffset, uint32_t BitWidth) const {
  const auto Bytes = FieldDisplaySize ? FieldDisplaySize : Size;
  if (Kind != NdTypeKind::Struct || IsEnum || !BitWidth ||
      BitOffset >= uint64_t(Bytes) * 8 ||
      BitWidth > uint64_t(Bytes) * 8 - BitOffset)
    return std::nullopt;
  std::optional<std::string> Found;
  bool Ambiguous = false;
  const auto Add = [&](std::string Name) {
    if (Found && *Found != Name)
      Ambiguous = true;
    else
      Found = std::move(Name);
  };
  for (const auto &Field : FieldDisplayBits)
    if (!Field.IsSigned && Field.BitOffset == BitOffset &&
        Field.BitWidth == BitWidth && !Field.Name.empty())
      Add(Field.Name);
  for (size_t I = 0;
       I < FieldDisplayNames.size() && I < FieldDisplayOffsets.size() &&
       I < FieldDisplayTypes.size();
       ++I) {
    const auto &Type = FieldDisplayTypes[I];
    const uint64_t Base = uint64_t(FieldDisplayOffsets[I]) * 8;
    if (!Type || FieldDisplayNames[I].empty() || BitOffset < Base ||
        BitOffset - Base >= uint64_t(Type->FieldDisplaySize
                                         ? Type->FieldDisplaySize
                                         : Type->Size) *
                                8)
      continue;
    uint64_t Rel = BitOffset - Base;
    TypeRef Element = Type;
    std::string Path = FieldDisplayNames[I];
    while (Element->Kind == NdTypeKind::Array && Element->ElemType &&
           Element->ElemType->Size) {
      const uint64_t ElementBits = uint64_t(Element->ElemType->Size) * 8;
      Path += "[" + std::to_string(Rel / ElementBits) + "]";
      Rel %= ElementBits;
      Element = Element->ElemType;
    }
    if (auto Inner = Element->displayBitFieldPathAt(Rel, BitWidth))
      Add(Path + "." + *Inner);
  }
  return Ambiguous ? std::nullopt : Found;
}

namespace {
bool layout(const NdType &T, uint16_t &Size, uint16_t &Alignment,
            std::vector<uint16_t> &Offsets, unsigned Depth, unsigned &Budget,
            uint16_t PointerBytes, bool Constructing = false) {
  if (!Budget || Depth > ::neverd::limits::ir::kNdTypesLayoutDepthLimit)
    return false;
  --Budget;
  if (T.Kind == NdTypeKind::Array) {
    if (!T.ElemType || !T.ArrayCount)
      return false;
    std::vector<uint16_t> ElementOffsets;
    if (!layout(*T.ElemType, Size, Alignment, ElementOffsets, Depth + 1, Budget,
                PointerBytes))
      return false;
    const uint64_t Bytes = uint64_t(Size) * T.ArrayCount;
    if (Bytes != T.Size || Bytes > ::neverd::limits::ir::kMaxNaturalStructBytes)
      return false;
    Size = static_cast<uint16_t>(Bytes);
    return true;
  }
  if (T.Kind != NdTypeKind::Struct) {
    // The canonical short-vector spelling carries its natural size/alignment;
    // an ordinary 128-bit integer does not acquire vector classification.
    const bool Scalar =
        (T.Kind == NdTypeKind::Int &&
         (T.Size == 1 || T.Size == 2 || T.Size == 4 || T.Size == 8)) ||
        (T.Kind == NdTypeKind::Float && (T.Size == 4 || T.Size == 8)) ||
        (T.Kind == NdTypeKind::Ptr && (T.Size == 4 || T.Size == 8) &&
         (!PointerBytes || T.Size == PointerBytes) && T.Pointee);
    Size = Alignment = T.Size;
    return (Scalar && !sourceAArch64VectorBytes(&T)) ||
           isSourceAArch64VectorType(&T);
  }
  if (T.Fields.empty() ||
      T.Fields.size() > ::neverd::limits::ir::kNdTypesLayoutTFieldsSizeLimit)
    return false;
  uint32_t End = 0;
  Alignment = 1;
  for (const auto &Field : T.Fields) {
    uint16_t Bytes, Align;
    std::vector<uint16_t> Nested;
    if (!Field ||
        !layout(*Field, Bytes, Align, Nested, Depth + 1, Budget, PointerBytes))
      return false;
    Alignment = std::max(Alignment, Align);
    End = (End + Align - 1) & -uint32_t(Align);
    Offsets.push_back(static_cast<uint16_t>(End));
    End += Bytes;
    if (End > ::neverd::limits::ir::kMaxNaturalStructBytes)
      return false;
  }
  Size = static_cast<uint16_t>((End + Alignment - 1) & -uint32_t(Alignment));
  return Constructing || (T.Size == Size && T.Alignment == Alignment &&
                          T.FieldOffsets == Offsets);
}
} // namespace

std::shared_ptr<NdType>
NdType::makeStruct(std::vector<std::shared_ptr<NdType>> Fields,
                   uint16_t PointerBytes) {
  if (PointerBytes != 4 && PointerBytes != 8)
    return nullptr;
  auto T = std::make_shared<NdType>();
  T->Kind = NdTypeKind::Struct;
  T->Fields = std::move(Fields);
  unsigned Budget = ::neverd::limits::ir::kNdTypesNdTypeMakeStructBudget;
  if (!layout(*T, T->Size, T->Alignment, T->FieldOffsets, 0, Budget,
              PointerBytes, true))
    return nullptr;
  return T;
}

std::shared_ptr<NdType> NdType::makeComplex(uint16_t ComponentBytes) {
  if (ComponentBytes != 4 && ComponentBytes != 8)
    return nullptr;
  auto T = makeStruct({makeFloat(ComponentBytes), makeFloat(ComponentBytes)});
  T->Passing = NdRecordPassing::Complex;
  T->ScalarLeaves = {{0, ComponentBytes, true},
                     {ComponentBytes, ComponentBytes, true}};
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
  case NdTypeKind::Vector:
    return "vector<" + std::to_string(ArrayCount) + " x " +
           (ElemType && (ElemType->Kind == NdTypeKind::Int ||
                         ElemType->Kind == NdTypeKind::Float)
                ? ElemType->str()
                : "unknown") +
           ">";
  case NdTypeKind::Struct: {
    // Validate before recursive printing, including cycles and stale offsets.
    unsigned Budget = ::neverd::limits::ir::kNdTypesNdTypeStrBudget;
    uint16_t Bytes, Align;
    std::vector<uint16_t> Offsets;
    // Display the stored member widths. ABI classification separately checks
    // every pointer against its target; spelling never supplies that proof.
    if (!layout(*this, Bytes, Align, Offsets, 0, Budget, 0))
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
