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
#include "neverd/ir/low/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/low/NdOpEmulator.h"

namespace neverd {

bool NdOpEmulator::executeMIPSScalar(const LowOp &Op) {
  if (!mipsScalarLowShapeIsValid(Op, Img.Arch))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::MIPSUnpredictable) {
    Registers.erase(Op.Output.Offset);
    WideRegisters.erase(Op.Output.Offset);
    UnpredictableValues.insert(Op.Output.Offset);
    return true;
  }
  if (!knownOperandBytes(Op.Inputs[1], 0, Op.Inputs[1].Size))
    return false;
  const uint64_t First = readOperand(Op.Inputs[1]);
  if (Id == Intrinsic::MIPSRequireWord)
    return First == mips::signExtendWord(First);
  if (Id == Intrinsic::MIPSLwrUpper) {
    const auto Load = *mips::mergeLoad(Intrinsic::MIPSLwr);
    const auto Access =
        mips::mergeAccess(Load, First, archLittleEndian(Img.Arch));
    if (!knownOperandBytes(Op.Inputs[3], 0, 4))
      return false;
    if (Access.Count != 4 &&
        (!knownOperandBytes(Op.Inputs[2], 0, 8) ||
         readOperand(Op.Inputs[2]) !=
             mips::signExtendWord(readOperand(Op.Inputs[2]))))
      return false;
    writeOutput(Op.Output,
                mips::signExtendWord(readOperand(Op.Inputs[3])) >> 32);
    return true;
  }
  const auto Load = *mips::mergeLoad(Id);
  const bool Little = archLittleEndian(Img.Arch);
  const auto Access = mips::mergeAccess(Load, First, Little);
  // A complete load does not observe the incoming destination register.
  if (Access.Count != Load.Bytes &&
      !knownOperandBytes(Op.Inputs[2], 0, Op.Inputs[2].Size))
    return false;
  uint64_t Result = readOperand(Op.Inputs[2]);
  for (unsigned I = 0; I != Access.Count; ++I) {
    const auto Byte = loadMemory(Access.First + I, 1);
    if (!Byte)
      return false;
    if (CollectLoads && int(LoadLog.size()) < limits::kMaxLoadRecords)
      LoadLog.push_back({Access.First + I, 1});
    const unsigned Lane =
        Access.DestinationByte + (Little ? I : Access.Count - 1 - I);
    const unsigned Shift = Lane * 8;
    Result = (Result & ~(UINT64_C(0xff) << Shift)) | (*Byte << Shift);
  }
  writeOutput(Op.Output, Result);
  return true;
}

} // namespace neverd
