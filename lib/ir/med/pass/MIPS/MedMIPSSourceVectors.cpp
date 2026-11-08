//===- MedMIPSSourceVectors.cpp - Scalar MSA leaf source contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The first MSA source slice uses initialized volatile vectors within one
/// ordinary leaf block. Vector ABI, calls and saved FPR restoration need their
/// own proofs; a scalar result alone does not establish those contracts.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSSourceVectors.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"

#include <bitset>

namespace neverd {

void requireMIPSSourceVectors(const LowFunc &Function, Arch Target,
                              const BinaryImage *Image) {
  const auto IsVector = [](const NdVar &V) {
    return V.isReg() && mipsreg::isVector(V.Offset);
  };
  bool HasVectors = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      HasVectors |= IsVector(Op.Output);
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        HasVectors |= IsVector(Op.Inputs[I]);
    }
  if (!HasVectors)
    return;
  if (Target != Arch::MIPS32 && Target != Arch::MIPS32EL)
    throw LowToMedConversionError("MSA source requires MIPS32");
  const bool FP64 = Image && Image->MIPS && Image->MIPS->HasMSA &&
                    Image->MIPS->ISARevision == 5 && Image->MIPS->FPABI == 6;
  if (Image && !FP64)
    throw LowToMedConversionError("MSA source requires explicit FP64 profile");
  if (Function.Blocks.size() != 1 || !Function.Blocks[0].Succs.empty() ||
      !Function.Blocks[0].ExceptionalSuccs.empty() ||
      !Function.Blocks[0].ExceptionalPreds.empty())
    throw LowToMedConversionError("MSA source requires a single leaf block");
  std::bitset<mipsreg::VectorCount> Initialized;
  for (const auto &Op : Function.Blocks[0].Ops) {
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
      throw LowToMedConversionError("MSA source call ABI is not qualified");
    for (unsigned I = 0; I != Op.NumInputs; ++I)
      if (IsVector(Op.Inputs[I]) &&
          !Initialized[(Op.Inputs[I].Offset - mipsreg::VectorBase) /
                       mipsreg::VectorBytes])
        throw LowToMedConversionError(
            "MSA incoming vector source ABI is not qualified");
    if (!IsVector(Op.Output))
      continue;
    const unsigned Index =
        (Op.Output.Offset - mipsreg::VectorBase) / mipsreg::VectorBytes;
    // o32 FP64 preserves the low64 F20,F22,...,F30 views, not whole W
    // registers. Do not silently discard their writes through vector DCE.
    // Without an Image, preserve every possible o32 FPABI save view: FP32
    // and FPXX can also own the odd scalar registers in this bank.
    if (Index >= 20 && (!FP64 || Index % 2 == 0))
      throw LowToMedConversionError(
          "MSA source saved FPR restoration is not qualified");
    Initialized[Index] = Op.Output.Size == mipsreg::VectorBytes;
  }
}

} // namespace neverd
