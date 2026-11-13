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
#include "neverd/ir/intrinsics/MIPS/MIPSR6FPOperations.h"

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

constexpr bool isDSPHalfCompare(Intrinsic Id) {
  return Id >= Intrinsic::MIPSCmpEqPH && Id <= Intrinsic::MIPSCmpLePH;
}
constexpr bool isDSPPartialControl(Intrinsic Id) {
  return isDSPHalfCompare(Id) || Id == Intrinsic::MIPSMthlipPosition;
}
constexpr bool isDSPControlIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadDSPControl ||
         Id == Intrinsic::MIPSWriteDSPControl || isDSPPartialControl(Id);
}

constexpr bool isMSAControl(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadMSAControl ||
         Id == Intrinsic::MIPSWriteMSAControl;
}

constexpr bool isMSAFloat(Intrinsic Id) {
  return Id >= Intrinsic::MIPSMSAFADD_W && Id <= Intrinsic::MIPSMSAFTRUNC_U_D;
}

constexpr bool isMSADivision(Intrinsic Id) {
  return Id >= Intrinsic::MIPSMSADIV_S_B && Id <= Intrinsic::MIPSMSAMOD_U_D;
}
constexpr bool isMSANativeVector(Intrinsic Id) {
  return isMSAFloat(Id) || isMSADivision(Id);
}
struct MSANativeOperation {
  uint32_t Encoding;
  unsigned Arity;
  const char *Mnemonic;
};
inline constexpr MSANativeOperation MSANativeOperations[] = {
#define NEVERD_MSA_FLOAT(Name, Value, Encoding, Arity, Mnemonic)               \
  {Encoding, Arity, Mnemonic},
#include "neverd/ir/intrinsics/MIPS/MIPSMSAFloat.def"
#define NEVERD_MSA_DIVISION(Name, Value, Encoding, Mnemonic)                   \
  {Encoding, 2, Mnemonic},
#include "neverd/ir/intrinsics/MIPS/MIPSMSADivision.def"
};
constexpr const MSANativeOperation &msaNativeOperation(Intrinsic Id) {
  return MSANativeOperations[unsigned(Id) - unsigned(Intrinsic::MIPSMSAFADD_W)];
}

constexpr bool isWordDivision(Intrinsic Id) {
  return Id == Intrinsic::MIPSDivWord || Id == Intrinsic::MIPSDivUnsignedWord;
}

/// No memory/control effect. A fresh unpredictable result is still not
/// CSE-able.
constexpr bool isPureScalarValue(Intrinsic Id) {
  return Id == Intrinsic::MIPSUnpredictable || Id == Intrinsic::MIPSLwrUpper ||
         isWordDivision(Id) || Id == Intrinsic::MIPSAccumulatorAfterWrite ||
         Id == Intrinsic::MIPSExtractResult ||
         Id == Intrinsic::MIPSRelease6DivideResult;
}

/// Ordering effects remain observable, but neither operation writes data.
/// This is not a purity/CSE contract.
constexpr bool isBarrier(Intrinsic Id) {
  return Id == Intrinsic::MIPSExecutionHazardBarrier ||
         Id == Intrinsic::MIPSSync;
}

struct ReservationTransfer {
  Intrinsic Id;
  uint16_t ElementBytes;
  bool Paired;
  bool Store;
  constexpr uint16_t bytes() const {
    return Paired ? 2 * ElementBytes : ElementBytes;
  }
};

constexpr std::optional<ReservationTransfer> reservationTransfer(Intrinsic Id) {
  switch (Id) {
#define NEVERD_MIPS_RESERVATION(Code, Width, Pair, Load, LID, LM, Store, SID,  \
                                SM, CSLoad, CSStore)                           \
  case Intrinsic::Load:                                                        \
    return ReservationTransfer{Id, Width, Pair, false};                        \
  case Intrinsic::Store:                                                       \
    return ReservationTransfer{Id, Width, Pair, true};
#include "neverd/ir/intrinsics/MIPS/MIPSReservation.def"
  default:
    return std::nullopt;
  }
}

constexpr std::optional<ReservationTransfer> r6Reservation(uint32_t Word) {
  if ((Word >> 26) != 31 || ((Word & 64) && (Word & 0x780)))
    return std::nullopt;
  switch (Word & 127) {
#define NEVERD_MIPS_RESERVATION(Code, Width, Pair, Load, LID, LM, Store, SID,  \
                                SM, CSLoad, CSStore)                           \
  case Code:                                                                   \
    return ReservationTransfer{Intrinsic::Load, Width, Pair, false};           \
  case Code ^ 16:                                                              \
    return ReservationTransfer{Intrinsic::Store, Width, Pair, true};
#include "neverd/ir/intrinsics/MIPS/MIPSReservation.def"
  default:
    return std::nullopt;
  }
}
constexpr bool isMTIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSDisableMT || Id == Intrinsic::MIPSEnableMT ||
         Id == Intrinsic::MIPSDisableVPE || Id == Intrinsic::MIPSEnableVPE;
}
constexpr bool isCacheIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSCache || Id == Intrinsic::MIPSPrefetch;
}
struct CP0Transfer {
  Intrinsic Id;
  unsigned RS;
  uint16_t Bytes;
  bool Write;
};

constexpr std::optional<CP0Transfer> cp0Transfer(Intrinsic Id) {
  switch (Id) {
#define NEVERD_MIPS_CP0_TRANSFER(RS, Bytes, Write, Name, Value, Mnemonic)      \
  case Intrinsic::Name:                                                        \
    return CP0Transfer{Id, RS, Bytes, Write};
#include "neverd/ir/intrinsics/MIPS/MIPSCP0Transfer.def"
  default:
    return std::nullopt;
  }
}

constexpr std::optional<CP0Transfer> cp0TransferEncoding(unsigned Rs) {
  switch (Rs) {
#define NEVERD_MIPS_CP0_TRANSFER(RS, Bytes, Write, Name, Value, Mnemonic)      \
  case RS:                                                                     \
    return CP0Transfer{Intrinsic::Name, RS, Bytes, Write};
#include "neverd/ir/intrinsics/MIPS/MIPSCP0Transfer.def"
  default:
    return std::nullopt;
  }
}

constexpr bool isSystemIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadHardwareRegister ||
         Id == Intrinsic::MIPSDisableInterrupts ||
         Id == Intrinsic::MIPSEnableInterrupts ||
         Id == Intrinsic::MIPSReadR6HardwareRegister ||
         cp0Transfer(Id).has_value() || isMTIntrinsic(Id);
}
constexpr bool scalarMayWriteMemory(Intrinsic Id) {
  if (const auto R = reservationTransfer(Id))
    return R->Store;
  return mergeStore(Id).has_value() || isCacheIntrinsic(Id);
}
constexpr bool isStatefulScalar(Intrinsic Id) {
  return isR6FPArithmetic(Id) || isR6FPControl(Id) ||
         reservationTransfer(Id).has_value() || isSystemIntrinsic(Id) ||
         Id == Intrinsic::MIPSPause || isCacheIntrinsic(Id);
}
constexpr bool localCP0Word(uint64_t Index, bool Write) {
  switch (Index) {
#define NEVERD_MIPS_CP0(Index, Selector, Name, Writable)                       \
  case ((Selector << 5) | Index):                                              \
    return !Write || Writable;
#include "neverd/ir/intrinsics/MIPS/MIPSCP0.def"
  default:
    return false;
  }
}
constexpr bool cp0RequiresMT(uint64_t Key) {
  const unsigned Reg = Key & 31, Sel = Key >> 5;
  // MVP, VPE, TC and TCOpt views in MT Volume IV-f, Table 6.1.
  return (Reg == 0 && Sel >= 1 && Sel <= 3) ||
         ((Reg == 1 || Reg == 2) && Sel >= 1 && Sel <= 7) ||
         (Reg == 3 && Sel == 7);
}
constexpr bool supportedHardwareRegister(uint64_t Index) {
  return Index < 4 || (Index >= 29 && Index <= 31);
}

constexpr bool supportedR6HardwareRegister(uint64_t Key) {
  return Key <= UINT8_MAX &&
         ((Key & 31) == 4 || Key == 5 || supportedHardwareRegister(Key));
}

constexpr bool isScalarIntrinsic(Intrinsic Id) {
  return mergeMemory(Id).has_value() || Id == Intrinsic::MIPSUnpredictable ||
         Id == Intrinsic::MIPSRequireWord ||
         Id == Intrinsic::MIPSRequireTakenBranch ||
         Id == Intrinsic::MIPSLwrUpper || isDSPControlIntrinsic(Id) ||
         isMSAControl(Id) || isMSANativeVector(Id) || isWordDivision(Id) ||
         isBarrier(Id) || isStatefulScalar(Id) ||
         Id == Intrinsic::MIPSAccumulatorAfterWrite ||
         Id == Intrinsic::MIPSExtractResult ||
         Id == Intrinsic::MIPSRelease6DivideResult;
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
  if (isR6FPControl(Id)) {
    const bool Write = Id == Intrinsic::MIPSWriteFPControl;
    return S.FirstBytes == 1 && S.FirstConstant && *S.FirstConstant < 32 &&
           r6FPControlSelector(*S.FirstConstant, Write) &&
           (Write ? S.Inputs == 3 && S.SecondBytes == 4 && !S.OutputBytes
                  : S.Inputs == 2 && S.OutputWritable && S.OutputBytes == 4);
  }
  if (isR6FPArithmetic(Id)) {
    const auto &F = r6FPOperation(Id);
    return S.OutputWritable && S.OutputBytes == F.OutputBytes &&
           S.Inputs == F.Arity + 1 && S.FirstBytes == F.InputBytes &&
           (F.Arity < 2 || S.SecondBytes == F.InputBytes) &&
           (F.Arity < 3 || S.ThirdBytes == F.InputBytes);
  }
  if (isMSANativeVector(Id)) {
    const unsigned N = msaNativeOperation(Id).Arity;
    return S.OutputWritable && S.OutputBytes == 16 && S.Inputs == N + 1 &&
           S.FirstBytes == 16 && (N < 2 || S.SecondBytes == 16) &&
           (N < 3 || S.ThirdBytes == 16);
  }
  if (isMSAControl(Id)) {
    if (!S.FirstConstant || *S.FirstConstant >= 32 || S.FirstBytes != 1)
      return false;
    return Id == Intrinsic::MIPSReadMSAControl
               ? S.Inputs == 2 && S.OutputWritable && S.OutputBytes == 4
               : S.Inputs == 3 && S.SecondBytes == 4 && !S.OutputBytes;
  }
  if (const auto R = reservationTransfer(Id)) {
    const auto GPR = S.FirstBytes;
    if ((GPR != 4 && GPR != 8) || GPR < R->ElementBytes ||
        (S.Target != Arch::Unknown && GPR != (isMIPS64(S.Target) ? 8 : 4)) ||
        !S.OutputWritable)
      return false;
    if (!R->Store)
      return S.Inputs == 2 && S.OutputBytes == R->bytes();
    return S.Inputs == (R->Paired ? 4 : 3) && S.OutputBytes == 4 &&
           S.SecondBytes == GPR && (!R->Paired || S.ThirdBytes == GPR);
  }
  if (isStatefulScalar(Id)) {
    if (isCacheIntrinsic(Id))
      return S.Inputs == 3 && !S.OutputBytes && S.FirstBytes == 1 &&
             (S.SecondBytes == 4 || S.SecondBytes == 8) &&
             (S.Target == Arch::Unknown ||
              S.SecondBytes == (isMIPS64(S.Target) ? 8 : 4)) &&
             S.FirstConstant && *S.FirstConstant < 32;
    if (Id == Intrinsic::MIPSPause)
      return S.Inputs == 1 && !S.OutputBytes;
    if (const auto C = cp0Transfer(Id)) {
      if (!S.FirstConstant || *S.FirstConstant > UINT8_MAX ||
          S.FirstBytes != 1 ||
          (C->Bytes == 8 && S.Target != Arch::Unknown && !isMIPS64(S.Target)))
        return false;
      return C->Write
                 ? S.Inputs == 3 && !S.OutputBytes && S.SecondBytes == C->Bytes
                 : S.Inputs == 2 && S.OutputWritable &&
                       S.OutputBytes == C->Bytes;
    }
    if (Id == Intrinsic::MIPSReadR6HardwareRegister)
      return S.Inputs == 2 && S.OutputWritable && S.FirstBytes == 1 &&
             S.FirstConstant && supportedR6HardwareRegister(*S.FirstConstant) &&
             (S.OutputBytes == 4 || S.OutputBytes == 8) &&
             (S.Target == Arch::Unknown ||
              S.OutputBytes == (isMIPS64(S.Target) ? 8 : 4));
    if (!S.OutputWritable || S.OutputBytes != 4 ||
        (isMIPS64(S.Target) && Id != Intrinsic::MIPSDisableInterrupts &&
         Id != Intrinsic::MIPSEnableInterrupts))
      return false;
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
  if (Id == Intrinsic::MIPSRequireTakenBranch)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 1;
  if (Id == Intrinsic::MIPSRequireWord)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 8 &&
           (S.Target == Arch::Unknown || isMIPS64(S.Target));
  if (isDSPPartialControl(Id))
    return !S.OutputBytes &&
           (isDSPHalfCompare(Id)
                ? S.Inputs == 3 && S.FirstBytes == 4 && S.SecondBytes == 4
                : S.Inputs == 1);
  if (Id == Intrinsic::MIPSWriteDSPControl)
    return S.Inputs == 2 && !S.OutputBytes && S.FirstBytes == 4;
  if (Id == Intrinsic::MIPSReadDSPControl)
    return S.Inputs == 1 && S.OutputWritable && S.OutputBytes == 4;
  if (const auto Store = mergeStore(Id))
    return S.Inputs == 3 && !S.OutputBytes && S.SecondBytes == Store->Bytes &&
           (S.FirstBytes == 4 || S.FirstBytes == 8) &&
           S.FirstBytes >= Store->Bytes &&
           (S.Target == Arch::Unknown ||
            S.FirstBytes == (isMIPS64(S.Target) ? 8 : 4));
  if (!S.OutputWritable || (S.OutputBytes != 4 && S.OutputBytes != 8))
    return false;
  if (Id == Intrinsic::MIPSRelease6DivideResult)
    return S.Inputs == 3 && S.OutputBytes == 8 && S.FirstBytes == 1 &&
           S.SecondBytes == 8;
  if (Id == Intrinsic::MIPSExtractResult)
    return S.Inputs == 3 && S.OutputBytes == 4 && S.FirstBytes == 1 &&
           S.SecondBytes == 4;
  if (Id == Intrinsic::MIPSAccumulatorAfterWrite)
    return S.Inputs == 3 && S.FirstBytes == 1 &&
           S.SecondBytes == S.OutputBytes &&
           (S.Target == Arch::Unknown ||
            S.OutputBytes == (isMIPS64(S.Target) ? 8 : 4));
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
