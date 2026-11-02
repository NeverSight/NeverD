//===- AnalysisCall.cpp - Display of machine-only call conventions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit x87 call expressions for tentative source views. Keep the target
/// and raw operands visible without substituting a host floating-point ABI.
///
//===----------------------------------------------------------------------===//

#include "AnalysisCall.h"

#include "neverd/lift/X86Regs.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace neverd::high_detail {

bool hasAnalysisX87CallShape(const MedOp &Operation) {
  if ((Operation.Opcode != NdOp::CALL &&
       Operation.Opcode != NdOp::INDIR_CALL) ||
      !Operation.X87ImportArguments || Operation.X87ArgumentInput != 1 ||
      unsigned(Operation.X87ImportArguments) + 1 != Operation.NumInputs ||
      Operation.NumInputs > Operation.Inputs.size())
    return false;
  for (unsigned I = 1; I < Operation.NumInputs; ++I)
    if (Operation.Inputs[I].Size != x86reg::FPURegSize)
      return false;
  return Operation.Output.Id < 0 || Operation.Output.Size == 0 ||
         Operation.Output.Size == x86reg::FPURegSize;
}

bool appendAnalysisX87Call(
    HighFunc &Function, const MedOp &Operation,
    llvm::function_ref<ExprPtr(const MedVar &)> Expression) {
  if (!Function.AnalysisOnly || !Operation.X87ImportArguments)
    return false;
  if (!hasAnalysisX87CallShape(Operation))
    throw std::runtime_error("malformed analysis x87 call operands");
  std::vector<ExprPtr> Inputs;
  Inputs.reserve(Operation.NumInputs);
  for (unsigned I = 0; I < Operation.NumInputs; ++I)
    Inputs.push_back(Expression(Operation.Inputs[I]));
  // A constant INDIR_CALL operand denotes the original pointer slot. Keep
  // that distinction explicit instead of treating the slot as a function.
  const char *Name =
      Operation.Opcode == NdOp::CALL  ? "__neverd_analysis_x87_call"
      : Operation.Inputs[0].isConst() ? "__neverd_analysis_x87_call_slot"
                                      : "__neverd_analysis_x87_call_indirect";
  auto Call = HighExpr::makeCall(Name, 0, std::move(Inputs));
  Call->CallSiteAddr = Operation.Addr;
  Call->DoesNotReturn = Operation.DoesNotReturn;
  HighStmt Statement;
  Statement.Addr = Operation.Addr;
  if (Operation.Output.Id >= 0 && Operation.Output.Size) {
    Call->Type = NdType::makeInt(Operation.Output.Size, false);
    Statement.Kind = StmtKind::Assign;
    Statement.Dst = HighExpr::makeVar(Operation.Output, Call->Type);
    Statement.Val = std::move(Call);
  } else {
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr = std::move(Call);
  }
  Function.Body.push_back(std::move(Statement));
  return true;
}

} // namespace neverd::high_detail
