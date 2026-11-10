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

struct MergeTransfer {
  Intrinsic Id;
  uint8_t Bytes;
  bool Left;
};

constexpr std::optional<MergeTransfer> mergeLoad(Intrinsic Id) {
  switch (Id) {
#define NEVERD_MIPS_MERGE_LOAD(Id, Value, Opcode, Bytes, Left, CName)          \
  case Intrinsic::Id:                                                          \
    return MergeTransfer{Intrinsic::Id, Bytes, Left};
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.def"
  default:
    return std::nullopt;
  }
}

constexpr std::optional<MergeTransfer> mergeLoadOpcode(unsigned Opcode) {
  switch (Opcode) {
#define NEVERD_MIPS_MERGE_LOAD(Id, Value, Opcode, Bytes, Left, CName)          \
  case Opcode:                                                                 \
    return MergeTransfer{Intrinsic::Id, Bytes, Left};
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.def"
  default:
    return std::nullopt;
  }
}

constexpr std::optional<MergeTransfer> mergeStore(Intrinsic Id) {
  switch (Id) {
#define NEVERD_MIPS_MERGE_STORE(Id, Value, Opcode, Bytes, Left, CName)         \
  case Intrinsic::Id:                                                          \
    return MergeTransfer{Intrinsic::Id, Bytes, Left};
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.def"
  default:
    return std::nullopt;
  }
}

constexpr std::optional<MergeTransfer> mergeStoreOpcode(unsigned Opcode) {
  switch (Opcode) {
#define NEVERD_MIPS_MERGE_STORE(Id, Value, Opcode, Bytes, Left, CName)         \
  case Opcode:                                                                 \
    return MergeTransfer{Intrinsic::Id, Bytes, Left};
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.def"
  default:
    return std::nullopt;
  }
}

constexpr std::optional<MergeTransfer> mergeMemory(Intrinsic Id) {
  const auto Load = mergeLoad(Id);
  return Load ? Load : mergeStore(Id);
}

constexpr bool isDSPControlIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadDSPControl ||
         Id == Intrinsic::MIPSWriteDSPControl;
}

constexpr bool isWordDivision(Intrinsic Id) {
  return Id == Intrinsic::MIPSDivWord || Id == Intrinsic::MIPSDivUnsignedWord;
}

/// No memory/control effect. A fresh unpredictable result is still not
/// CSE-able.
constexpr bool isPureScalarValue(Intrinsic Id) {
  return Id == Intrinsic::MIPSUnpredictable || Id == Intrinsic::MIPSLwrUpper ||
         isWordDivision(Id);
}

/// Ordering effects remain observable, but neither operation writes data.
/// This is not a purity/CSE contract.
constexpr bool isBarrier(Intrinsic Id) {
  return Id == Intrinsic::MIPSExecutionHazardBarrier ||
         Id == Intrinsic::MIPSSync;
}

constexpr bool isReservationIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSLoadLinkedWord ||
         Id == Intrinsic::MIPSStoreConditionalWord;
}
constexpr bool isSystemIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadHardwareRegister ||
         Id == Intrinsic::MIPSDisableInterrupts ||
         Id == Intrinsic::MIPSEnableInterrupts;
}
constexpr bool scalarMayWriteMemory(Intrinsic Id) {
  return mergeStore(Id).has_value() ||
         Id == Intrinsic::MIPSStoreConditionalWord;
}
constexpr bool isStatefulScalar(Intrinsic Id) {
  return isReservationIntrinsic(Id) || isSystemIntrinsic(Id);
}
constexpr bool supportedHardwareRegister(uint64_t Index) {
  return Index < 4 || Index == 29;
}

constexpr bool isScalarIntrinsic(Intrinsic Id) {
  return mergeMemory(Id).has_value() || Id == Intrinsic::MIPSUnpredictable ||
         Id == Intrinsic::MIPSRequireWord || Id == Intrinsic::MIPSLwrUpper ||
         isDSPControlIntrinsic(Id) || isWordDivision(Id) || isBarrier(Id) ||
         isStatefulScalar(Id);
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
  std::optional<uint64_t> FirstConstant;
};

constexpr bool scalarShapeIsValid(Intrinsic Id, const ScalarShape &S) {
  if (!S.IdIsValid || !S.Ordinary ||
      (S.Target != Arch::Unknown && !isMIPS(S.Target)))
    return false;
  if (isStatefulScalar(Id)) {
    if (isMIPS64(S.Target) || !S.OutputWritable || S.OutputBytes != 4)
      return false;
    if (Id == Intrinsic::MIPSLoadLinkedWord)
      return S.Inputs == 2 && S.FirstBytes == 4;
    if (Id == Intrinsic::MIPSStoreConditionalWord)
      return S.Inputs == 3 && S.FirstBytes == 4 && S.SecondBytes == 4;
    if (Id == Intrinsic::MIPSReadHardwareRegister)
      return S.Inputs == 2 && S.FirstBytes == 1 && S.FirstConstant &&
             supportedHardwareRegister(*S.FirstConstant);
    return S.Inputs == 1;
  }
  if (Id == Intrinsic::MIPSExecutionHazardBarrier)
    return S.Inputs == 1 && !S.OutputBytes;
  if (Id == Intrinsic::MIPSSync)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 1 &&
           S.FirstConstant && *S.FirstConstant < 32;
  if (Id == Intrinsic::MIPSRequireWord)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 8 &&
           (S.Target == Arch::Unknown || isMIPS64(S.Target));
  if (Id == Intrinsic::MIPSWriteDSPControl)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 4 &&
           !isMIPS64(S.Target);
  if (Id == Intrinsic::MIPSReadDSPControl)
    return S.Inputs == 1 && S.OutputWritable && S.OutputBytes == 4 &&
           !isMIPS64(S.Target);
  if (const auto Store = mergeStore(Id))
    return S.Inputs == 3 && !S.OutputBytes && S.SecondBytes == Store->Bytes &&
           (S.FirstBytes == 4 || S.FirstBytes == 8) &&
           (S.Target == Arch::Unknown ||
            S.FirstBytes == (isMIPS64(S.Target) ? 8 : 4));
  if (!S.OutputWritable || (S.OutputBytes != 4 && S.OutputBytes != 8))
    return false;
  if (Id == Intrinsic::MIPSUnpredictable)
    return S.Inputs == 1 && (S.Target == Arch::Unknown ||
                             S.OutputBytes == (isMIPS64(S.Target) ? 8 : 4));
  if (isWordDivision(Id))
    return S.Inputs == 3 && S.OutputBytes == 8 && S.FirstBytes == 4 &&
           S.SecondBytes == 4;
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

/// Exact contiguous bytes within the aligned memory word. RegisterByte is
/// the first numeric register lane read by a store or written by a load.
/// This ordinary-memory contract does not describe device bus transactions.
struct MergeAccess {
  uint64_t First;
  uint8_t Count;
  uint8_t RegisterByte;
};

constexpr MergeAccess mergeAccess(MergeTransfer Load, uint64_t Address,
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
