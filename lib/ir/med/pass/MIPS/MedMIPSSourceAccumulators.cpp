//===- MedMIPSSourceAccumulators.cpp - Defined HI/LO source inputs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse SSA and call-clobber identities for all MIPS instruction encodings.
/// A copied unknown accumulator is not evidence that a function returns void.
//===----------------------------------------------------------------------===//

#include "../../abi/MedABIPassDetail.h"

#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/med/MIPS/MIPSSourceAccumulators.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <utility>

namespace neverd {

void requireMIPSSourceAccumulators(const MedFunc &Function) {
  const auto IsAccumulator = [](const MedVar &Value) {
    return Value.Kind == MedVar::Reg && (mipsreg::isAccumulator(Value.RegOff) ||
                                         Value.RegOff == mipsreg::AC0Pending);
  };
  llvm::SmallVector<MedVar, 8> Pending;
  for (const auto &Block : Function.Blocks) {
    for (const auto &Op : Block.Ops) {
      // SSA's live-in definition is not itself an observation. A real use
      // below still reaches this entry copy and is rejected by the proof.
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          IsAccumulator(Op.Output) && Op.Output == Op.Inputs[0])
        continue;
      // This operation transports unknown bits; it does not observe them.
      if (mipsScalarMedShapeIsValid(Op, Op.Output.TheArch) &&
          static_cast<Intrinsic>(Op.Inputs[0].ConstVal) ==
              Intrinsic::MIPSAccumulatorAfterWrite)
        continue;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        if (IsAccumulator(Op.Inputs[I]))
          Pending.push_back(Op.Inputs[I]);
    }
  }
  if (Pending.empty())
    return;
  llvm::DenseSet<std::pair<int, int>> Clobbers;
  for (const auto &Clobber : Function.CallClobbers)
    if (IsAccumulator(Clobber.Value))
      Clobbers.insert({Clobber.Value.Id, Clobber.Value.SSAVer});
  MedDefSiteIndex Definitions(Function);
  llvm::DenseSet<std::pair<int, int>> Seen;
  while (!Pending.empty()) {
    const MedVar Value = Pending.pop_back_val();
    if (!Seen.insert({Value.Id, Value.SSAVer}).second)
      continue;
    if (Function.SkippedSSA)
      throw LowToMedConversionError("MIPS HI/LO source proof requires SSA");
    const auto Definition = Definitions.find(Value);
    // Version zero can also be the first real definition. Only the entry
    // self-copy (or a missing definition) denotes incoming machine state.
    const auto *Op = Definition.Op;
    const bool EntryCopy = Op && Op->Opcode == NdOp::COPY &&
                           Op->NumInputs == 1 && IsAccumulator(Op->Inputs[0]) &&
                           Op->Inputs[0].Id == Value.Id &&
                           Op->Inputs[0].SSAVer == Value.SSAVer;
    if (Definition.Ambiguous || (!Op && !Definition.Phi) || EntryCopy ||
        Clobbers.contains({Value.Id, Value.SSAVer}))
      throw LowToMedConversionError("MIPS incoming or call-clobbered HI/LO "
                                    "source state is not qualified");
    if (Definition.Phi)
      for (const auto &[Predecessor, Input] : Definition.Phi->Args) {
        (void)Predecessor;
        if (Input.Kind == MedVar::Unspecified)
          throw LowToMedConversionError(
              "MIPS HI/LO source has an undefined incoming edge");
        if (IsAccumulator(Input))
          Pending.push_back(Input);
      }
  }
}

} // namespace neverd
