//===- X64StreamStore.cpp - Retained streaming store evaluation ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse ordinary store analysis without replacing the emitted instruction.
//===----------------------------------------------------------------------===//

#include "X64StreamStore.h"

namespace neverd::analysis {
namespace {
bool supportedDomain(const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64 && Options.ExplicitMachineState &&
         Options.NormalNonfaultingExecution &&
         Options.ByteOrder == llvm::endianness::little;
}
} // namespace

bool x64::streamstore::isOperation(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 3 ||
      !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Offset != uint64_t(Intrinsic::X86StreamStore128) ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !x86SystemStateShapeIsValid(Intrinsic::X86StreamStore128,
                                  x86SystemStateLowShape(Op, Arch::X64)))
    return false;
  for (unsigned I = 1; I != 3; ++I)
    if (!Op.Inputs[I].isConst() &&
        Op.Inputs[I].Offset > InvalidVA - (Op.Inputs[I].Size - 1))
      return false;
  return true;
}

bool x64::streamstore::isFence(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && !Op.Output.Size && Op.NumInputs == 1 &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == uint64_t(Intrinsic::Sfence) &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

bool x64::streamstore::supportsFence(const LowOp &Op,
                                     const SpecializationOptions &Options) {
  return supportedDomain(Options) && isFence(Op);
}

std::optional<LowOp>
retainedStoreEvaluation(const LowOp &Op, const SpecializationOptions &Options) {
  if (!supportedDomain(Options) || !x64::streamstore::isOperation(Op))
    return std::nullopt;
  LowOp Store = Op;
  Store.Opcode = NdOp::STORE;
  Store.Inputs[0] = NdVar::cst(0, 2);
  return Store;
}
} // namespace neverd::analysis
