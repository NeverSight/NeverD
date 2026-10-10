//===- X86RegistrationCall.cpp - Checked source call arguments -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/X86RegistrationCall.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

namespace neverd {

std::optional<std::vector<ExprPtr>>
MedToHighConverter::collectRegistrationCallArgs(const MedBlock &Block,
                                                size_t CallIdx) {
  if (TargetArch != Arch::X86 || !CurMed || CallIdx >= Block.Ops.size())
    return std::nullopt;
  const auto ABI = registrationCallABI(*CurMed, Block, Block.Ops[CallIdx]);
  if (!ABI)
    return std::nullopt;
  std::vector<ExprPtr> Args;
  if (ABI->RuntimeThrow) {
    const auto *Call = CurMed->findCall(Block.Id, CallIdx);
    if (!Call || Call->Args.size() != 2)
      return std::nullopt;
    for (const auto &Argument : Call->Args)
      Args.push_back(medvarToExpr(Argument));
    return Args;
  }
  if (!ABI->BorrowsECX)
    return Args;

  const auto ECX = getTargetRegInfo(Arch::X86).IntParamRegs.front();
  // The receipt owns the callee ABI. Keep the argument's current SSA value,
  // including edits, rather than substituting a historical frame offset.
  ExprPtr Value;
  for (size_t I = CallIdx; I != 0; --I) {
    const auto &Op = Block.Ops[I - 1];
    if (Op.Dead || Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != ECX ||
        !Op.Output.Size)
      continue;
    Value =
        Op.Output.Size == 4 ? medvarToExpr(Op.Output) : HighExpr::makeUndef(4);
    break;
  }
  if (!Value) {
    MedVar Incoming;
    if (reachingRegAtBlockEntry(Block, ECX, Incoming) && Incoming.Size == 4)
      Value = medvarToExpr(Incoming);
  }
  Args.push_back(Value ? std::move(Value) : HighExpr::makeUndef(4));
  return Args;
}

} // namespace neverd
