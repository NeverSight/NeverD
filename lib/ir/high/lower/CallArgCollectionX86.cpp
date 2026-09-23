//===- CallArgCollectionX86.cpp - x86 / x86-64 call-argument ABI ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 stack-argument recovery.  Both use the shared spilled-store
/// scan (slot width is TRI.PointerSize).  i386 cdecl/stdcall additionally
/// pushes each argument, so every STORE looks like slot 0; walk those pushes
/// backward (last push is arg0).
///
//===----------------------------------------------------------------------===//

#include "CallArgCollectionDetail.h"

#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>

namespace neverd {
namespace call_args_detail {

void collectCallArgsX86(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args) {
  collectSpilledStackArgs(Scan, Found);
  // A stack argument means all four register arguments are passed, even the
  // ones this block did not write (a pass-through of the caller's own).
  if (isWin64(Scan) && Scan.ReachingRegArg &&
      std::any_of(Found.begin() + std::min<size_t>(4, Found.size()),
                  Found.end(), [](const ExprPtr &E) { return E != nullptr; }))
    for (int K = 0; K < 4 && K < static_cast<int>(Found.size()); ++K)
      if (!Found[K]) {
        // A slot the callee does not read still occupies its position.
        Found[K] = Scan.ReachingRegArg(K);
        if (!Found[K])
          Found[K] = HighExpr::makeConst(0, 8);
      }
  for (int K = 0; K < Scan.MaxArgs; ++K) {
    if (!Found[K])
      break;
    Args.push_back(Found[K]);
  }
  if (Scan.TheArch != Arch::X86)
    return;

  const auto &Ops = *Scan.Ops;
  std::vector<ExprPtr> Pushed;
  for (int J = static_cast<int>(Scan.CallIdx) - 1; J >= 0; --J) {
    const MedOp &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC)
      break;
    if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
        Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    if (Scan.IsCalleeSave(Prev.Inputs[1]))
      continue;
    Pushed.push_back(Scan.ToExpr(Prev.Inputs[1]));
    if (static_cast<int>(Pushed.size()) == Scan.MaxArgs)
      break;
  }
  if (Pushed.size() > Args.size())
    Args = std::move(Pushed);
}

} // namespace call_args_detail
} // namespace neverd
