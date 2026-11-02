//===- CDisplayFrameRecord.h - C record byte views --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share the C writers' declaration checks for a complete record view. This
/// is display evidence only: it grants no ABI, native extent or memory effect.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_CDISPLAYFRAMERECORD_H
#define NEVERD_LIB_BACKEND_C_CDISPLAYFRAMERECORD_H

#include "neverd/ArchSupport.h"
#include "neverd/backend/c/render/CTypeFormat.h"

namespace neverd {

/// C declaration alignment, kept distinct from native ABI classification.
inline uint16_t cDisplayFrameRecordAlignment(const TypeRef &Type) {
  return Type ? (Type->FieldDisplayAlignment ? Type->FieldDisplayAlignment
                                             : Type->Alignment)
              : 0;
}

/// A record whose declaration can name a view of independently bounded frame
/// bytes. Consumers must also obtain its current definition, keep its complete
/// byte range disjoint from other members, and preserve its alignment. Frame
/// storage is declared with the synthetic stack alignment, and C rejects that
/// declaration for a member that needs more.
inline bool isCDisplayFrameRecord(const TypeRef &Type) {
  const auto Alignment = cDisplayFrameRecordAlignment(Type);
  if (!Type || Type->Kind != NdTypeKind::Struct || Type->IsEnum ||
      !Type->Size || !Alignment || (Alignment & (Alignment - 1)) ||
      Type->Size % Alignment || Alignment > kSyntheticStackAlignment)
    return false;
  if (hasCValueLayout(Type))
    return true;
  // A current type-library definition supplies the declaration independently
  // of the optional per-field debug types. Some field typedefs cannot be
  // converted to an NdType; that does not make the validated C definition
  // incomplete. Consumers still require that definition and bound the bytes.
  if (Type->FieldDisplayAlignment && !Type->SourceName.empty())
    return true;
  if (Type->SourceName.empty() || Type->FieldDisplayTypes.empty() ||
      Type->FieldDisplayTypes.size() != Type->FieldDisplayOffsets.size() ||
      Type->FieldDisplayTypes.size() != Type->FieldDisplayNames.size())
    return false;
  for (size_t I = 0; I < Type->FieldDisplayTypes.size(); ++I) {
    const auto &Field = Type->FieldDisplayTypes[I];
    if (!Field || !Field->Size || Type->FieldDisplayOffsets[I] > Type->Size ||
        Field->Size > Type->Size - Type->FieldDisplayOffsets[I])
      return false;
  }
  return true;
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_CDISPLAYFRAMERECORD_H
