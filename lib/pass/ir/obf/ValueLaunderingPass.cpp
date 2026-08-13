//===- ValueLaunderingPass.cpp - Value laundering obfuscation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Value-laundering pass: routes integer (scalar / integer-vector) instruction
/// results through a volatile stack slot.  Demo-level sample transform — see
/// ValueLaunderingPass.h.
///
/// For an eligible instruction result `r` we emit, right after the defining
/// instruction:
///
///   store volatile r -> slot            ; one volatile slot per value type,
///                                       ;   reused, in the entry block
///   r' = load volatile slot
///   ...uses of r become uses of r'...   ; every use except the store above
///
/// The volatile accesses cannot be removed or reordered by the backend, so the
/// value genuinely detours through the stack and the emitted machine code
/// differs from the original.  The reloaded value equals the stored value, so
/// semantics are strictly preserved.
///
/// Scope: results of non-PHI, non-terminator instructions whose type is an
/// integer or a fixed integer vector and that have at least one use.  PHIs are
/// skipped (their result would need laundering after all PHIs, and PHI operands
/// must dominate the predecessor terminator); terminators define no launderable
/// value here (invoke results excluded for simplicity); pointer /
/// floating-point / aggregate results are left untouched so the slot stays a
/// plain integer cell.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/obf/ValueLaunderingPass.h"

#define DEBUG_TYPE "neverd-value-laundering"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace neverd {

namespace {

// A value is launderable only if it is an integer or a fixed integer-vector
// scalar — pointers, floats and aggregates are left untouched to keep the
// transform simple and the stack slot a plain integer cell.
bool isLaunderableType(llvm::Type *Ty) {
  if (Ty->isIntegerTy())
    return true;
  if (auto *VT = llvm::dyn_cast<llvm::FixedVectorType>(Ty))
    return VT->getElementType()->isIntegerTy();
  return false;
}

unsigned launderFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  // Snapshot launderable instructions up front so the freshly-created volatile
  // load/store (and the entry-block slots) are never themselves laundered.
  std::vector<llvm::Instruction *> Sites;
  for (llvm::BasicBlock &BB : F) {
    for (llvm::Instruction &I : BB) {
      if (llvm::isa<llvm::PHINode>(&I))
        continue; // a PHI result must be laundered after all PHIs
      if (I.isTerminator())
        continue; // terminators define no launderable value here
      if (I.use_empty())
        continue; // nothing to redirect
      if (!isLaunderableType(I.getType()))
        continue;
      Sites.push_back(&I);
    }
  }
  if (Sites.empty())
    return 0;

  // One volatile slot per value type at the top of the entry block (static
  // allocas — they dominate every use and stay static so no Windows __chkstk
  // probe is emitted).  Reused across sites of the same type: every launder is
  // a store immediately followed by a load, so a shared slot never returns a
  // stale value.
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::DenseMap<llvm::Type *, llvm::AllocaInst *> Slots;
  auto slotFor = [&](llvm::Type *Ty) -> llvm::AllocaInst * {
    auto It = Slots.find(Ty);
    if (It != Slots.end())
      return It->second;
    llvm::IRBuilder<> EB(&Entry, Entry.getFirstInsertionPt());
    auto *Slot = EB.CreateAlloca(Ty, nullptr, "nd_launder_slot");
    Slots[Ty] = Slot;
    return Slot;
  };

  unsigned Count = 0;
  for (llvm::Instruction *I : Sites) {
    llvm::Type *Ty = I->getType();
    llvm::AllocaInst *Slot = slotFor(Ty);

    // Insert the round-trip right after the defining instruction (a non-PHI,
    // non-terminator, so getNextNode() is at worst the block terminator).
    llvm::IRBuilder<> B(I->getNextNode());
    auto *St = B.CreateStore(I, Slot, /*isVolatile=*/true);
    llvm::Value *Routed =
        B.CreateLoad(Ty, Slot, /*isVolatile=*/true, "nd_launder_v");
    // Redirect every use of the value to the reloaded copy, except the store we
    // just created (which must keep feeding the original value into the slot).
    I->replaceUsesWithIf(Routed,
                         [&](llvm::Use &U) { return U.getUser() != St; });
    ++Count;
  }

  LLVM_DEBUG(llvm::dbgs() << "neverd: laundered " << Count << " value(s) in "
                          << F.getName() << "\n");
  return Count;
}

unsigned launderModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += launderFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses
ValueLaunderingPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  unsigned N = launderModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned ValueLaunderingPass::inject(llvm::Module &M) {
  return launderModule(M);
}

} // namespace neverd
