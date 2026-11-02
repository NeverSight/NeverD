//===- X64ReadPrefetch.cpp - Retained read cache hints -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scalar-state interpretation of retained default-space read hints.
//===----------------------------------------------------------------------===//

#include "X64ControlTransitions.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd::analysis::x64::readprefetch {

bool isOperation(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.Output.Size || Op.NumInputs != 2 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.Inputs[1].Size != 8 ||
      !(Op.Inputs[1].isConst() ||
        ((Op.Inputs[1].isReg() || Op.Inputs[1].isTemp()) &&
         Op.Inputs[1].Offset <= InvalidVA - 7)))
    return false;
  switch (Op.Inputs[0].Offset) {
  case uint64_t(Intrinsic::PrefetchNta):
  case uint64_t(Intrinsic::PrefetchT0):
  case uint64_t(Intrinsic::PrefetchT1):
  case uint64_t(Intrinsic::PrefetchT2):
    return true;
  default:
    return false;
  }
}

bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64 && Options.ExplicitMachineState &&
         Options.NormalNonfaultingExecution &&
         Options.ByteOrder == llvm::endianness::little && isOperation(Op);
}

} // namespace neverd::analysis::x64::readprefetch
