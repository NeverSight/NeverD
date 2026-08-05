//===- IndirectGlobalPass.cpp - Indirect global-variable obfuscation ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Indirect global-variable pass: rewrites direct references to defined global
/// variables into a position-independent indirect address through an opaque
/// pointer.  Demo-level sample transform — see IndirectGlobalPass.h.
///
/// For an instruction operand that is a defined global `@g` we emit, right
/// before the using instruction:
///
///   addr = ptrtoint(@g)                 ; PC-relative (AArch64 ADRP+ADD,
///                                       ;   x86-64 RIP-relative) —
///                                       self-corrects ;   under ASLR/PIE
///                                       slide, no GOT
///   store volatile addr -> slot         ; one i-ptr slot in the function entry
///   p = inttoptr(load volatile slot)    ; opaque pointer the backend cannot
///                                       ;   prove equals @g
///   ...use p instead of @g...
///
/// Only the global's address is materialised (PC-relative), so the emitted
/// image contains no absolute data pointer: nothing needs a load-time rebase,
/// and the construct runs correctly under ASLR/PIE — the same design
/// IndirectCallPass uses for a callee's address.  The volatile slot stops the
/// backend from folding the indirect address back into a direct `@g` reference
/// (which would erase the obfuscation).
///
/// Scope: direct operand references whose target is a *defined*, non-TLS,
/// non-`llvm.*` global variable (so `ptrtoint(@g)` is an in-image, PC-relative
/// reference the rewrite backend's address model resolves without a GOT).
/// External declarations (their address is a GOT/stub), thread-local globals
/// (separate TLS addressing) and PHI operands (their incoming value must
/// dominate the predecessor terminator, so laundering code cannot be inserted
/// at the PHI) are left untouched.  References buried inside a ConstantExpr are
/// also left as-is — only top-level instruction operands are rewritten.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/IndirectGlobalPass.h"

#define DEBUG_TYPE "neverd-indirect-global"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace neverd {

namespace {

// A global is eligible only if its address is an in-image, PC-relative
// reference the rewrite backend resolves without a GOT.
bool isEligibleGlobal(llvm::GlobalVariable *GV) {
  if (!GV)
    return false;
  if (GV->isDeclaration())
    return false; // external/import: its address is a GOT/stub, keep direct
  if (GV->isThreadLocal())
    return false; // TLS has its own addressing sequence
  if (GV->getName().starts_with("llvm."))
    return false; // llvm.used / metadata-carrying globals must stay direct
  return true;
}

unsigned indirectGlobalFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  llvm::Module &M = *F.getParent();
  llvm::LLVMContext &Ctx = F.getContext();
  auto *PtrTy = llvm::PointerType::getUnqual(Ctx);
  // Pointer-width integer from the target triple, not the DataLayout: this pass
  // runs before compileForRewrite installs the target DataLayout, so
  // getDataLayout() would still report the host default (mirrors
  // IndirectCallPass).
  const bool Is64 = !M.getTargetTriple().isArch32Bit();
  auto *IntPtrTy =
      Is64 ? llvm::Type::getInt64Ty(Ctx) : llvm::Type::getInt32Ty(Ctx);

  // Snapshot the (instruction, operand-index, global) sites up front so the
  // freshly-created laundering instructions are never themselves rewritten.
  // PHI operands are skipped: an incoming value must dominate the end of its
  // predecessor, so laundering code cannot be inserted at the PHI itself.
  struct Site {
    llvm::Instruction *I;
    unsigned OpIdx;
    llvm::GlobalVariable *GV;
  };
  std::vector<Site> Sites;
  for (llvm::BasicBlock &BB : F) {
    for (llvm::Instruction &I : BB) {
      if (llvm::isa<llvm::PHINode>(&I))
        continue;
      for (unsigned Op = 0, E = I.getNumOperands(); Op < E; ++Op) {
        auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(I.getOperand(Op));
        if (isEligibleGlobal(GV))
          Sites.push_back({&I, Op, GV});
      }
    }
  }
  if (Sites.empty())
    return 0;

  // One volatile pointer-width slot at the top of the entry block (a static
  // alloca — dominates every use, stays static so no Windows __chkstk probe is
  // emitted).  Reused across all sites in this function (mirrors
  // IndirectCallPass).
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::AllocaInst *Slot = nullptr;
  {
    llvm::IRBuilder<> EB(&Entry, Entry.getFirstInsertionPt());
    Slot = EB.CreateAlloca(IntPtrTy, nullptr, "nd_igv_slot");
  }

  unsigned Count = 0;
  for (const Site &S : Sites) {
    // Taking @g's address must lower PC-relatively (no GOT — the rewrite
    // backend creates none).  Mark the global dso_local, mirroring how
    // IndirectCallPass treats the callee whose address it takes (a defined
    // global sits at a fixed VA in the final image, never preemptible).
    S.GV->setDSOLocal(true);

    // Insert the launder right before the using instruction; the slot (in the
    // entry block) dominates it.
    llvm::IRBuilder<> B(S.I);
    // addr = ptrtoint(@g) as a constant — materialised PC-relatively.
    llvm::Constant *GInt = llvm::ConstantExpr::getPtrToInt(S.GV, IntPtrTy);
    B.CreateStore(GInt, Slot, /*isVolatile=*/true);
    llvm::Value *Routed =
        B.CreateLoad(IntPtrTy, Slot, /*isVolatile=*/true, "nd_igv_a");
    llvm::Value *P = B.CreateIntToPtr(Routed, PtrTy, "nd_igv_p");
    S.I->setOperand(S.OpIdx, P);
    ++Count;
  }

  LLVM_DEBUG(llvm::dbgs() << "neverd: indirect global applied to " << Count
                          << " reference(s) in " << F.getName() << "\n");
  return Count;
}

unsigned indirectGlobalModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += indirectGlobalFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses IndirectGlobalPass::run(llvm::Module &M,
                                                llvm::ModuleAnalysisManager &) {
  unsigned N = indirectGlobalModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned IndirectGlobalPass::inject(llvm::Module &M) {
  return indirectGlobalModule(M);
}

} // namespace neverd
