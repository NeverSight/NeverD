//===- X86RegistrationReturn.cpp - Catch resumes ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/high/X86RegistrationFrame.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

#include <algorithm>

namespace neverd {

bool MedToHighConverter::lowerX86RegistrationCatchReturn(
    HighFunc &Func, const MedBlock &CurBlock, const MedOp &CurOp,
    const MedFunc &Med) {
  if (TargetArch != Arch::X86 || !Med.ExceptionMetadata ||
      Med.ExceptionMetadata->Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      !Med.ExceptionMetadata->Registration ||
      Med.ExceptionMetadata->Registration->RegistrationOffset != -12 ||
      !Med.ExceptionMetadata->Cxx || !Med.RegistrationStates ||
      !Med.RegistrationStates->CxxContinuationsComplete ||
      CurOp.NumInputs != 1 || CurOp.Inputs[0].Size != 4 || CurOp.OriginSeq < 0)
    return false;
  const auto *Resume =
      Med.RegistrationStates->cxxContinuation(CurOp.Addr, CurOp.OriginSeq);
  if (!Resume || Resume->EndAddress != CurBlock.EndAddr ||
      Resume->SavedStackOffset > -16 ||
      !std::any_of(Med.Blocks.begin(), Med.Blocks.end(),
                   [&](const auto &Block) {
                     return Block.StartAddr == Resume->TargetVA;
                   }))
    return false;
  const auto Value = forceInlineExpr(medvarToExpr(CurOp.Inputs[0]));
  if (!Value || Value->Kind != ExprKind::Const ||
      Value->ConstVal != Resume->TargetVA)
    return false;
  // The catch may have overwritten both EBP and SavedESP. Use the same
  // entry-relative source coordinate as its runtime roots, including any
  // proved alignment, rather than observing those live callback values.
  RegistrationFrameCoordinate Frame{-4, 1, 0};
  if (Med.ExceptionMetadata->Registration->RealignedFrame) {
    const auto Realigned = realignedRegistrationFrameCoordinate(
        *Med.ExceptionMetadata, &*Med.RegistrationStates);
    if (!Realigned || Med.Entry != Med.ExceptionMetadata->CodeRange.Begin)
      return false;
    Frame = *Realigned;
  }
  auto Slot = x86RegistrationFrameAddress(Frame, -16);
  auto Saved = x86RegistrationFrameAddress(Frame, Resume->SavedStackOffset);
  if (!Slot || !Saved)
    return false;
  HighStmt Restore;
  Restore.Kind = StmtKind::Store;
  Restore.Addr = CurOp.Addr;
  Restore.StoreAddr = std::move(Slot);
  Restore.StoreVal = std::move(Saved);
  Func.Body.push_back(std::move(Restore));
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.Addr = CurOp.Addr;
  Jump.GotoTarget = Resume->TargetVA;
  Func.Body.push_back(std::move(Jump));
  return true;
}

} // namespace neverd
