//===- MedLLVMRegistrationCookieCall.cpp - Retained PE32 GS checks --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the original cookie check and its sole physical ECX argument.
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/med/X86/X86RegistrationCall.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Module.h"

namespace neverd {

llvm::Function *MedLLVMEmitter::prepareX86RegistrationCookieCall(
    const MedOp &Op, int BlockId, int OpIdx, llvm::IRBuilder<> &Builder,
    std::vector<llvm::Value *> &Arguments) {
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF || !Img ||
      !CurMedFunc || !CurMedFunc->RegistrationStates ||
      !CurMedFunc->RegistrationStates->cookieCheck(Op.Addr, Op.OriginSeq))
    return nullptr;
  auto Reject = [&]() -> llvm::Function * {
    syncError() << "med_llvm_emitter: retained PE32 cookie call lost its "
                   "current source or register ABI\n";
    FatalCodePointerResolution = true;
    return nullptr;
  };
  if (CurMedFunc->Blocks.size() > limits::kMaxRegistrationEHRecords)
    return Reject();
  const MedBlock *Block = nullptr;
  for (const auto &Candidate : CurMedFunc->Blocks)
    if (Candidate.Id == BlockId) {
      if (Block)
        return Reject();
      Block = &Candidate;
    }
  if (!Block || OpIdx < 0 || size_t(OpIdx) >= Block->Ops.size() ||
      &Block->Ops[OpIdx] != &Op || Op.Output.Size || !Op.PreservesCallerSaved ||
      Op.X87ArgumentInput || Op.X87ImportArguments)
    return Reject();
  const auto ABI = registrationCookieCallABI(*Img, *CurMedFunc, *Block, OpIdx);
  const auto *Info = CurMedFunc->findCall(BlockId, OpIdx);
  if (!ABI || !Info || Info->IsIndirect || Info->SourceCallHint ||
      Info->TargetAddr != ABI->CheckVA || Info->Args.size() != 1 ||
      Info->Args.front() != ABI->Argument ||
      Info->IntegerRegisterArgCount != 1 || Info->FloatingRegisterArgCount ||
      !Info->StackArgumentOffsets.empty())
    return Reject();
  const std::string Name =
      "__nd_registration_cookie_" + llvm::utohexstr(ABI->CheckVA);
  auto *Callee = Mod->getFunction(Name);
  if (Mod->getNamedValue(Name) &&
      (!Callee || !hasX86RegistrationSecurityCheckABI(*Callee)))
    return Reject();
  if (Callee) {
    auto Identity = rewrite_source::getOriginalVA(*Callee);
    if (!Identity) {
      llvm::consumeError(Identity.takeError());
      return Reject();
    }
    if (!*Identity || **Identity != ABI->CheckVA)
      return Reject();
  }
  auto *Argument = getVar(ABI->Argument, Builder);
  if (!Argument->getType()->isIntegerTy(32))
    return Reject();
  if (!Callee) {
    Callee = createX86RegistrationSecurityCheck(*Mod, Name);
    rewrite_source::setOriginalVA(*Callee, ABI->CheckVA);
  }
  Arguments = {Builder.CreateIntToPtr(Argument, Builder.getPtrTy())};
  return Callee;
}

} // namespace neverd
