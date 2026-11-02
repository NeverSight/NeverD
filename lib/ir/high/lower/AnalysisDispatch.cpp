//===- AnalysisDispatch.cpp - Tentative physical branch targets ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retain candidate bodies when native selector proof was refused. The
/// original memory read still selects the branch; no table slot is folded.
///
//===----------------------------------------------------------------------===//

#include "AnalysisDispatch.h"

#include "neverd/ir/med/MedAnalysisDispatch.h"

#include <set>
#include <utility>

namespace neverd::high_detail {

bool appendAnalysisDispatch(HighFunc &Function, const MedOp &Branch,
                            const JumpTable &Table, ExprPtr Target,
                            const MedVar &Snapshot) {
  if (!hasAnalysisDispatchShape(Function.AnalysisOnly, Branch, Table) ||
      Snapshot.Size != Branch.Inputs[0].Size || !Target)
    return false;

  const auto Type = NdType::makeInt(Snapshot.Size, false);
  auto Value = HighExpr::makeVar(Snapshot, Type);
  HighStmt Capture;
  Capture.Kind = StmtKind::Assign;
  Capture.Addr = Branch.Addr;
  Capture.Dst = Value;
  Capture.Val = std::move(Target);
  Function.Body.push_back(std::move(Capture));

  HighStmt Dispatch;
  Dispatch.Kind = StmtKind::Switch;
  Dispatch.Addr = Branch.Addr;
  Dispatch.SwitchExpr = Value;
  std::set<va_t> Seen;
  for (va_t Address : Table.Targets) {
    if (!Seen.insert(Address).second)
      continue;
    HighStmt Transfer;
    Transfer.Kind = StmtKind::Goto;
    Transfer.GotoTarget = Address;
    SwitchCase Case;
    Case.Value = Address;
    Case.Body.push_back(std::move(Transfer));
    Dispatch.Cases.push_back(std::move(Case));
  }
  HighStmt Unknown;
  Unknown.Kind = StmtKind::Call;
  Unknown.Addr = Branch.Addr;
  Unknown.CallExpr =
      HighExpr::makeCall("__neverd_analysis_indirect_jump", 0, {Value});
  Unknown.CallExpr->CallSiteAddr = Branch.Addr;
  Unknown.CallExpr->DoesNotReturn = true;
  Dispatch.DefaultBody.push_back(std::move(Unknown));
  Function.Body.push_back(std::move(Dispatch));
  // These keys describe physical addresses, not table positions. Do not
  // publish source selector labels or change the native refusal receipt.
  return true;
}

} // namespace neverd::high_detail
