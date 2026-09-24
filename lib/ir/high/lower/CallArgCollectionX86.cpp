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

namespace neverd {
namespace call_args_detail {

void collectCallArgsX86(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args) {
  collectSpilledStackArgs(Scan, Found);
  for (int K = 0; K < Scan.MaxArgs; ++K) {
    if (!Found[K])
      break;
    Args.push_back(Found[K]);
  }
  if (Scan.TheArch != Arch::X86)
    return;

  // Every i386 `push` stores at the current ESP, so the spilled-slot scan
  // sees StackOff 0 for each of them and keeps only the last push (arg0).
  // Walk STORE values backward (last push is arg0). Call-only IP-map splits
  // leave the earlier pushes in ExtraWindows.
  std::vector<ExprPtr> Pushed;
  auto walkPushes = [&](const std::vector<MedOp> &Ops, int Before) {
    for (int J = Before; J >= 0; --J) {
      const MedOp &Prev = Ops[static_cast<size_t>(J)];
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
        return;
    }
  };
  if (Scan.Ops)
    walkPushes(*Scan.Ops, static_cast<int>(Scan.CallIdx) - 1);
  if (static_cast<int>(Pushed.size()) < Scan.MaxArgs)
    for (const auto &W : Scan.ExtraWindows)
      if (W.Ops) {
        walkPushes(*W.Ops, W.Before);
        if (static_cast<int>(Pushed.size()) == Scan.MaxArgs)
          break;
      }
  if (Pushed.size() <= Args.size())
    return;
  for (size_t K = 0; K < Pushed.size() && K < Found.size(); ++K)
    Found[K] = Pushed[K];
  Args = std::move(Pushed);
}

} // namespace call_args_detail
} // namespace neverd
