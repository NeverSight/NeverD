//===- IndirectCallPass.cpp - Indirect call obfuscation ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Indirect-call pass: rewrites direct calls to defined functions into a
/// position-independent indirect call through an opaque function pointer.
/// Demo-level sample transform — see IndirectCallPass.h.
///
/// For `call @g(args)` (g defined, non-intrinsic) we emit:
///
///   addr = ptrtoint(@g)                 ; PC-relative (AArch64 ADRP+ADD,
///                                       ;   x86-64 RIP-relative) —
///                                       self-corrects ;   under ASLR/PIE
///                                       slide, no GOT
///   store volatile addr -> slot         ; one i-ptr slot in the caller's entry
///   fp = inttoptr(load volatile slot)   ; opaque pointer the backend cannot
///                                       ;   prove equals @g
///   call <FTy> fp(args)                 ; was: call @g(args)
///
/// Only the callee's address is materialised (PC-relative), so the emitted
/// image contains no absolute code pointer: nothing needs a load-time rebase,
/// and the construct runs correctly under ASLR/PIE on a real machine — the same
/// design IndirectBranchPass uses for its PIC base.  The volatile slot stops
/// the backend from folding the indirect call back into a direct one (which
/// would erase the obfuscation).
///
/// Scope: direct calls whose callee is a *defined*, non-intrinsic function (so
/// `ptrtoint(@g)` is an in-image, PC-relative reference the rewrite backend's
/// address model resolves without a GOT).  Intrinsics, inline asm, already
/// indirect calls and `musttail` calls are left untouched.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/IndirectCallPass.h"

#define DEBUG_TYPE "neverd-indirect-call"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace neverd {

namespace {

unsigned indirectCallFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  llvm::Module &M = *F.getParent();
  llvm::LLVMContext &Ctx = F.getContext();
  auto *PtrTy = llvm::PointerType::getUnqual(Ctx);
  // Pointer-width integer (i64 on 64-bit, i32 on 32-bit).  Width comes from the
  // module's target triple, not its DataLayout: this pass runs before
  // compileForRewrite installs the target DataLayout, so getDataLayout() would
  // still report the host default (64-bit) here.
  const bool Is64 = !M.getTargetTriple().isArch32Bit();
  auto *IntPtrTy =
      Is64 ? llvm::Type::getInt64Ty(Ctx) : llvm::Type::getInt32Ty(Ctx);

  // Collect the direct calls up front (mutating call operands while iterating
  // is fine, but collecting keeps the intent clear).  Only convert calls whose
  // callee is a defined, non-intrinsic function: that guarantees the
  // materialised `ptrtoint(@g)` is an in-image, PC-relative reference the
  // address model resolves without a GOT.
  std::vector<llvm::CallInst *> Calls;
  for (llvm::BasicBlock &BB : F) {
    for (llvm::Instruction &I : BB) {
      auto *CI = llvm::dyn_cast<llvm::CallInst>(&I);
      if (!CI)
        continue;
      if (CI->isInlineAsm())
        continue;
      if (CI->isMustTailCall())
        continue; // musttail has strict signature/ABI constraints
      if (CI->getIntrinsicID() != llvm::Intrinsic::not_intrinsic)
        continue; // intrinsics must stay direct
      llvm::Function *Callee = CI->getCalledFunction();
      if (!Callee)
        continue; // already indirect
      if (Callee->isIntrinsic())
        continue;
      if (Callee->isDeclaration())
        continue; // external/import: keep direct (its address is a stub)
      Calls.push_back(CI);
    }
  }
  if (Calls.empty())
    return 0;

  // One volatile pointer-width slot at the top of the entry block (a static
  // alloca — dominates every use, stays static so no Windows __chkstk probe is
  // emitted).  Reused across all call sites in this function.
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::AllocaInst *Slot = nullptr;
  {
    llvm::IRBuilder<> EB(&Entry, Entry.getFirstInsertionPt());
    Slot = EB.CreateAlloca(IntPtrTy, nullptr, "nd_icall_slot");
  }

  unsigned Count = 0;
  for (llvm::CallInst *CI : Calls) {
    llvm::Function *Callee = CI->getCalledFunction();

    // Taking @g's address must lower PC-relatively (no GOT — the rewrite
    // backend creates none).  Mark the callee dso_local, mirroring how
    // IndirectBranchPass treats the function whose address it takes (a defined
    // function sits at a fixed VA in the final image, never preemptible).
    Callee->setDSOLocal(true);

    llvm::IRBuilder<> B(CI);
    // addr = ptrtoint(@g) as a constant — materialised PC-relatively.
    llvm::Constant *FInt = llvm::ConstantExpr::getPtrToInt(Callee, IntPtrTy);
    B.CreateStore(FInt, Slot, /*isVolatile=*/true);
    llvm::Value *Routed =
        B.CreateLoad(IntPtrTy, Slot, /*isVolatile=*/true, "nd_icall_a");
    llvm::Value *FP = B.CreateIntToPtr(Routed, PtrTy, "nd_icall_fp");

    // Turn the direct call into an indirect call: keep the original
    // FunctionType / arguments, only swap the called operand.
    CI->setCalledOperand(FP);
    ++Count;
  }

  LLVM_DEBUG(llvm::dbgs() << "neverd: indirect call applied to " << Count
                          << " call(s) in " << F.getName() << "\n");
  return Count;
}

unsigned indirectCallModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += indirectCallFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses IndirectCallPass::run(llvm::Module &M,
                                              llvm::ModuleAnalysisManager &) {
  unsigned N = indirectCallModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned IndirectCallPass::inject(llvm::Module &M) {
  return indirectCallModule(M);
}

} // namespace neverd
