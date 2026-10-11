//===- X86RegistrationReturn.cpp - Catch resumes ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
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
      !Med.ExceptionMetadata->Registration->cxxRuntimeFrameOffset() ||
      !Med.ExceptionMetadata->Cxx || !Med.RegistrationStates ||
      !Med.RegistrationStates->CxxContinuationsComplete ||
      CurOp.NumInputs != 1 || CurOp.Inputs[0].Size != 4 || CurOp.OriginSeq < 0)
    return false;
  const auto *Resume =
      Med.RegistrationStates->cxxContinuation(CurOp.Addr, CurOp.OriginSeq);
  if (!Resume || Resume->EndAddress != CurBlock.EndAddr ||
      (Resume->SavedCallbackVA
           ? Resume->SavedStackOffset > 0
           : Resume->SavedStackOffset >
                 *Med.ExceptionMetadata->Registration->RegistrationOffset -
                     4) ||
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
  // An authenticated direct-frame return remains a runtime transfer even if
  // other state flow prevents structuring the surrounding region.
  const auto Frame = Med.ExceptionMetadata->Registration->hasCxxCallbackStack()
                         ? cxxRegistrationFrameCoordinate(
                               *Med.ExceptionMetadata, &*Med.RegistrationStates)
                         : std::optional(RegistrationFrameCoordinate{-4, 1, 0});
  if (!Frame || Med.Entry != Med.ExceptionMetadata->CodeRange.Begin)
    return false;
  auto Slot = x86RegistrationFrameAddress(
      *Frame, *Med.ExceptionMetadata->Registration->RegistrationOffset - 4);
  auto Saved = x86RegistrationFrameAddress(*Frame, Resume->SavedStackOffset);
  if (Resume->SavedCallbackVA) {
    MedVar SP;
    SP.Kind = MedVar::Reg;
    SP.TheArch = Arch::X86;
    SP.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
    SP.Size = 4;
    Saved = HighExpr::makeVar(SP, NdType::makeInt(4, false));
    Saved->Kind = ExprKind::EntryRegister;
    Saved->EntryFunctionVA = Med.Entry;
    Saved->EntryVA = Resume->SavedCallbackVA;
    if (Resume->SavedStackOffset)
      Saved = HighExpr::makeBinop(
          NdOp::INT_SUB, Saved,
          HighExpr::makeConst(-int64_t(Resume->SavedStackOffset), 4,
                              ConstantAddressProvenance::Scalar));
  }
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
