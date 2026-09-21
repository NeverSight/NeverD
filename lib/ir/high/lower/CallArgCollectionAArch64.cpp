//===- CallArgCollectionAArch64.cpp - AArch64 call-argument ABI -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 (AAPCS64) stack-argument recovery, plus Darwin objc_msgSend
/// selector stubs that overwrite x1 before the real call.
///
//===----------------------------------------------------------------------===//

#include "CallArgCollectionDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"

namespace neverd {
namespace call_args_detail {

void collectCallArgsAArch64(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                            std::vector<ExprPtr> &Args) {
  const auto &Ops = *Scan.Ops;
  if (Scan.Image && Scan.CallIdx < Ops.size()) {
    const MedOp &Call = Ops[Scan.CallIdx];
    if (Call.Opcode == NdOp::CALL && Call.NumInputs >= 1 &&
        Call.Inputs[0].isConst() &&
        objcSelectorStubOverwritesCommand(*Scan.Image,
                                          Call.Inputs[0].ConstVal)) {
      if (Scan.MaxArgs > 1)
        Found[1] = HighExpr::makeConst(0, Scan.TRI->PointerSize);
    }
  }

  CallArgScan Spilled = Scan;
  int FirstStackSlot = 0;
  for (int K = 0; K < Scan.MaxArgs; ++K) {
    if (Found[K])
      FirstStackSlot = K + 1;
    else
      break;
  }
  Spilled.FirstStackSlot = FirstStackSlot;
  const BinaryFormat Format =
      Scan.Image ? Scan.Image->Format : BinaryFormat::Unknown;
  const auto ParamRegs = Scan.TRI->integerParamRegs(Format);
  if (FirstStackSlot == static_cast<int>(ParamRegs.size()))
    Spilled.StoreScanWindow = limits::kAArch64FullBankCallArgStoreScanWindow;
  collectSpilledStackArgs(Spilled, Found);
  (void)Args;
}

} // namespace call_args_detail
} // namespace neverd
