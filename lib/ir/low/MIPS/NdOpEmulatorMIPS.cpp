//===- NdOpEmulatorMIPS.cpp - MIPS scalar execution ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact ordinary-memory merge loads and explicit unpredictable definitions.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.h"
#include "neverd/ir/low/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/Support/MathExtras.h"

namespace neverd {

bool NdOpEmulator::executeMIPSScalar(const LowOp &Op) {
  if (!mipsScalarLowShapeIsValid(Op, Img.Arch))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::MIPSReadDSPControl) {
    const auto State = NdVar::reg(mipsreg::DSPControl, 4);
    if (!knownOperandBytes(State, 0, 4))
      return false;
    writeOutput(Op.Output, readOperand(State) & mips::DSPControlBits);
    return true;
  }
  if (Id == Intrinsic::MIPSUnpredictable) {
    writeOutput(Op.Output, 0,
                llvm::maskTrailingOnes<uint64_t>(Op.Output.Size * 8));
    return true;
  }
  if (!knownOperandBytes(Op.Inputs[1], 0, Op.Inputs[1].Size))
    return false;
  const uint64_t First = readOperand(Op.Inputs[1]);
  if (mips::isWordDivision(Id)) {
    if (!knownOperandBytes(Op.Inputs[2], 0, 4))
      return false;
    const uint32_t Second = readOperand(Op.Inputs[2]);
    if (!Second) {
      writeOutput(Op.Output, 0, UINT64_MAX);
      return true;
    }
    uint32_t Quotient, Remainder;
    if (Id == Intrinsic::MIPSDivWord) {
      const auto SignedWord = [](uint64_t V) -> int64_t {
        return (V & UINT64_C(0x80000000)) ? int64_t(V) - (INT64_C(1) << 32)
                                          : int64_t(V);
      };
      const int64_t A = SignedWord(First), B = SignedWord(Second);
      Quotient = uint32_t(A / B);
      Remainder = uint32_t(A % B);
    } else {
      Quotient = uint32_t(First) / Second;
      Remainder = uint32_t(First) % Second;
    }
    writeOutput(Op.Output, (uint64_t(Remainder) << 32) | Quotient);
    return true;
  }
  if (Id == Intrinsic::MIPSWriteDSPControl) {
    writeOutput(NdVar::reg(mipsreg::DSPControl, 4),
                First & mips::DSPControlBits);
    return true;
  }
  if (Id == Intrinsic::MIPSRequireWord)
    return First == mips::signExtendWord(First);
  if (Id == Intrinsic::MIPSLwrUpper) {
    const auto Load = *mips::mergeLoad(Intrinsic::MIPSLwr);
    const auto Access =
        mips::mergeAccess(Load, First, archLittleEndian(Img.Arch));
    if (!availableOperandBytes(Op.Inputs[3], 0, 4))
      return false;
    if (Access.Count == 4) {
      const uint64_t Sign =
          mips::signExtendWord(readOperand(Op.Inputs[3])) >> 32;
      const uint64_t Unknown =
          unknownOperandBits(Op.Inputs[3]) & UINT64_C(0x80000000)
              ? UINT64_C(0xffffffff)
              : 0;
      writeOutput(Op.Output, Sign, Unknown);
      return true;
    }
    if (!availableOperandBytes(Op.Inputs[2], 0, 8))
      return false;
    const uint64_t Old = readOperand(Op.Inputs[2]);
    const uint64_t OldUnknown = unknownOperandBits(Op.Inputs[2]);
    const uint64_t High = Old >> 32;
    const uint64_t Sign = mips::signExtendWord(Old) >> 32;
    // r2 allows the old upper word or the old sign bit repeated. Only their
    // common known bits are concrete; no CPU policy is chosen here.
    const uint64_t Unknown =
        (OldUnknown >> 32) | (High ^ Sign) |
        ((OldUnknown & UINT64_C(0x80000000)) ? UINT64_C(0xffffffff) : 0);
    writeOutput(Op.Output, High, Unknown);
    return true;
  }
  if (const auto Store = mips::mergeStore(Id)) {
    const bool Little = archLittleEndian(Img.Arch);
    const auto Access = mips::mergeAccess(*Store, First, Little);
    if (!knownOperandBytes(Op.Inputs[2], Access.RegisterByte, Access.Count))
      return false;
    const uint64_t Value = readOperand(Op.Inputs[2]);
    std::array<uint8_t, 8> Bytes{};
    for (unsigned I = 0; I != Access.Count; ++I) {
      const unsigned Lane =
          Access.RegisterByte + (Little ? I : Access.Count - 1 - I);
      Bytes[I] = uint8_t(Value >> (Lane * 8));
    }
    // Validate the complete span before committing any byte. In particular a
    // three-byte partial store must not become a whole-word read/modify/write.
    return storeMemoryBytes(Access.First,
                            llvm::ArrayRef(Bytes).take_front(Access.Count));
  }
  const auto Load = *mips::mergeLoad(Id);
  const bool Little = archLittleEndian(Img.Arch);
  const auto Access = mips::mergeAccess(Load, First, Little);
  // A complete load does not observe the incoming destination register.
  if (Access.Count != Load.Bytes &&
      !availableOperandBytes(Op.Inputs[2], 0, Op.Inputs[2].Size))
    return false;
  uint64_t Result = readOperand(Op.Inputs[2]);
  uint64_t Unknown = unknownOperandBits(Op.Inputs[2]);
  for (unsigned I = 0; I != Access.Count; ++I) {
    const auto Byte = loadMemory(Access.First + I, 1);
    if (!Byte)
      return false;
    if (CollectLoads && int(LoadLog.size()) < limits::kMaxLoadRecords)
      LoadLog.push_back({Access.First + I, 1});
    const unsigned Lane =
        Access.RegisterByte + (Little ? I : Access.Count - 1 - I);
    const unsigned Shift = Lane * 8;
    Result = (Result & ~(UINT64_C(0xff) << Shift)) | (*Byte << Shift);
    Unknown &= ~(UINT64_C(0xff) << Shift);
  }
  writeOutput(Op.Output, Result, Unknown);
  return true;
}

} // namespace neverd
