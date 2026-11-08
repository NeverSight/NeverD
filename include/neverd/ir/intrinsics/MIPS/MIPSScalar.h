//===- MIPSScalar.h - Scalar memory and value contracts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owns the bounded merge-load access plan and scalar intrinsic admission.
/// Register values are numeric; memory byte order belongs to the target.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_MIPS_MIPSSCALAR_H
#define NEVERD_IR_INTRINSICS_MIPS_MIPSSCALAR_H

#include "neverd/ArchSupport.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace neverd::mips {

#define NEVERD_MIPS_SCALAR_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.def"

struct MergeLoad {
  Intrinsic Id;
  uint8_t Bytes;
  bool Left;
};

constexpr std::optional<MergeLoad> mergeLoad(Intrinsic Id) {
  switch (Id) {
#define NEVERD_MIPS_MERGE_LOAD(Id, Value, Opcode, Bytes, Left, CName)          \
  case Intrinsic::Id:                                                          \
    return MergeLoad{Intrinsic::Id, Bytes, Left};
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.def"
  default:
    return std::nullopt;
  }
}

constexpr std::optional<MergeLoad> mergeLoadOpcode(unsigned Opcode) {
  switch (Opcode) {
#define NEVERD_MIPS_MERGE_LOAD(Id, Value, Opcode, Bytes, Left, CName)          \
  case Opcode:                                                                 \
    return MergeLoad{Intrinsic::Id, Bytes, Left};
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.def"
  default:
    return std::nullopt;
  }
}

constexpr bool isDSPControlIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadDSPControl ||
         Id == Intrinsic::MIPSWriteDSPControl;
}

constexpr bool isScalarIntrinsic(Intrinsic Id) {
  return mergeLoad(Id).has_value() || Id == Intrinsic::MIPSUnpredictable ||
         Id == Intrinsic::MIPSRequireWord || Id == Intrinsic::MIPSLwrUpper ||
         isDSPControlIntrinsic(Id);
}

struct ScalarShape {
  Arch Target = Arch::Unknown;
  uint8_t Inputs = 0;
  bool IdIsValid = false;
  bool OutputWritable = false;
  uint16_t OutputBytes = 0;
  uint16_t FirstBytes = 0;
  uint16_t SecondBytes = 0;
  uint16_t ThirdBytes = 0;
  bool Ordinary = false;
};

constexpr bool scalarShapeIsValid(Intrinsic Id, const ScalarShape &S) {
  if (!S.IdIsValid || !S.Ordinary ||
      (S.Target != Arch::Unknown && !isMIPS(S.Target)))
    return false;
  if (Id == Intrinsic::MIPSRequireWord)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 8 &&
           (S.Target == Arch::Unknown || isMIPS64(S.Target));
  if (Id == Intrinsic::MIPSWriteDSPControl)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 4 &&
           !isMIPS64(S.Target);
  if (Id == Intrinsic::MIPSReadDSPControl)
    return S.Inputs == 1 && S.OutputWritable && S.OutputBytes == 4 &&
           !isMIPS64(S.Target);
  if (!S.OutputWritable || (S.OutputBytes != 4 && S.OutputBytes != 8))
    return false;
  if (Id == Intrinsic::MIPSUnpredictable)
    return S.Inputs == 1 && (S.Target == Arch::Unknown ||
                             S.OutputBytes == (isMIPS64(S.Target) ? 8 : 4));
  if (Id == Intrinsic::MIPSLwrUpper)
    return S.Inputs == 4 && S.OutputBytes == 4 && S.FirstBytes == 8 &&
           S.SecondBytes == 8 && S.ThirdBytes == 4 &&
           (S.Target == Arch::Unknown || isMIPS64(S.Target));
  const auto Load = mergeLoad(Id);
  return Load && S.Inputs == 3 && S.OutputBytes == Load->Bytes &&
         S.SecondBytes == Load->Bytes &&
         (S.FirstBytes == 4 || S.FirstBytes == 8) &&
         S.FirstBytes >= Load->Bytes &&
         (S.Target == Arch::Unknown ||
          S.FirstBytes == (isMIPS64(S.Target) ? 8 : 4));
}

/// Exact contiguous bytes within the aligned source word. These are ordinary
/// memory reads; this contract does not describe device bus transactions.
struct MergeAccess {
  uint64_t First;
  uint8_t Count;
  uint8_t DestinationByte;
};

constexpr MergeAccess mergeAccess(MergeLoad Load, uint64_t Address,
                                  bool LittleEndian) {
  const uint8_t Offset = Address & (Load.Bytes - 1);
  const bool Forward = Load.Left != LittleEndian;
  const uint8_t Count = Forward ? Load.Bytes - Offset : Offset + 1;
  return {Forward ? Address : Address - Offset, Count,
          uint8_t(Load.Left ? Load.Bytes - Count : 0)};
}

constexpr uint64_t signExtendWord(uint64_t Value) {
  const uint64_t Word = Value & UINT64_C(0xffffffff);
  return Word |
         ((Word & UINT64_C(0x80000000)) ? UINT64_C(0xffffffff00000000) : 0);
}

} // namespace neverd::mips

#endif // NEVERD_IR_INTRINSICS_MIPS_MIPSSCALAR_H
