//===- IndirectBranchPass.cpp - Indirect branch obfuscation -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Indirect-branch pass: rewrites two-way conditional branches into a
/// position-independent `indirectbr` driven by a PIC computed-goto offset
/// table.  Demo-level sample transform — see IndirectBranchPass.h.
///
/// For `br i1 %c, %T, %F` we emit, mirroring how LLVM lowers a PIC jump table:
///
///   base = ptrtoint(@F)                 ; PC-relative (AArch64 ADRP+ADD,
///   x86-64
///                                       ;   RIP-relative) — self-corrects
///                                       under ;   ASLR/PIE slide
///   off  = tbl[ zext(%c) ]              ; tbl = { (%F - @F), (%T - @F) }, i.e.
///                                       ;   same-section (.text) label diffs —
///                                       ;   slide-invariant constants, no
///                                       relocs
///   indirectbr inttoptr(base + sext off), [%T, %F]
///
/// Because the only address materialised is `@F` (PC-relative) and the table
/// holds nothing but text-internal label differences, the emitted image
/// contains *no absolute code pointers*: nothing needs a load-time rebase, so
/// the construct runs correctly under ASLR/PIE on a real machine.  (An earlier
/// design `select`ed between two `blockaddress` values; the backend pools a
/// `blockaddress` used as a value into an absolute `.quad`, which is correct at
/// a fixed VA — Unicorn — but SIGSEGVs once the image is slid because the
/// rewrite backend emits no rebase metadata.  See docs §15.2 P9a.)
///
/// The index is laundered through a volatile stack slot so the backend cannot
/// fold the one-of-two indirect branch back into a direct conditional branch
/// (which would erase the obfuscation).
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/obf/IndirectBranchPass.h"

#define DEBUG_TYPE "neverd-indirect-branch"
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

unsigned indirectBranchFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  llvm::Module &M = *F.getParent();
  llvm::LLVMContext &Ctx = F.getContext();
  auto *PtrTy = llvm::PointerType::getUnqual(Ctx);
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  // Pointer-width integer (i64 on 64-bit, i32 on 32-bit): a label difference
  // ptrtoint'd to a *wider* type is not a legal static-initializer relocation
  // on 32-bit targets, so the base arithmetic must use the native width.  Width
  // comes from the module's target triple, not its DataLayout: this pass runs
  // before compileForRewrite installs the target DataLayout, so getDataLayout()
  // would still report the host default (64-bit) here.
  const bool Is64 = !M.getTargetTriple().isArch32Bit();
  auto *IntPtrTy = Is64 ? llvm::Type::getInt64Ty(Ctx) : I32;
  // The offset table holds i32 entries (a function spans far less than 2 GiB),
  // matching how LLVM lays out PIC jump tables.  4-byte entries also keep the
  // table load (ldrsw / movsxd) 4-byte aligned, so inplace mode — where the
  // code sits at the original 4-byte-aligned VA — never hits the 8-byte
  // alignment requirement an i64 table's `ldr x` would impose.
  auto *ArrTy = llvm::ArrayType::get(I32, 2);

  // Collect the two-way conditional branches up front so the blocks/edges we
  // create are not themselves reprocessed.
  std::vector<llvm::CondBrInst *> CondBrs;
  for (llvm::BasicBlock &BB : F) {
    if (auto *BI = llvm::dyn_cast<llvm::CondBrInst>(BB.getTerminator()))
      CondBrs.push_back(BI);
  }
  if (CondBrs.empty())
    return 0;

  // Taking @F's own address must lower PC-relatively (AArch64 ADRP+ADD, x86-64
  // RIP-relative) rather than through the GOT — the rewrite backend creates no
  // GOT.  compileForRewrite marks external *declarations* dso_local for the
  // same reason; a defined function whose address we now take needs the same
  // treatment (it sits at a fixed VA in the final image, never preemptible).
  F.setDSOLocal(true);

  // One volatile i32 slot per function, placed at the top of the entry block (a
  // static alloca — dominates every use, and stays static so no Windows
  // __chkstk stack probe is emitted).  Routing the table index through it stops
  // the backend from folding the one-of-two indirect branch back into a direct
  // conditional branch.
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::AllocaInst *Slot = nullptr;
  {
    llvm::IRBuilder<> EB(&Entry, Entry.getFirstInsertionPt());
    Slot = EB.CreateAlloca(I32, nullptr, "nd_ibr_idx");
  }

  // base = ptrtoint(@F) as a constant — the backend materialises it
  // PC-relatively (no absolute pointer in the image).
  llvm::Constant *FInt = llvm::ConstantExpr::getPtrToInt(&F, IntPtrTy);

  // trunc(blockaddress(@F, BB) - @F) — a same-section (.text) label difference,
  // a slide-invariant constant the rewrite backend resolves without a reloc.
  auto blockOffset = [&](llvm::BasicBlock *BB) -> llvm::Constant * {
    llvm::Constant *BA = llvm::BlockAddress::get(&F, BB);
    llvm::Constant *BAInt = llvm::ConstantExpr::getPtrToInt(BA, IntPtrTy);
    llvm::Constant *Diff = llvm::ConstantExpr::getSub(BAInt, FInt);
    return Is64 ? llvm::ConstantExpr::getTrunc(Diff, I32) : Diff;
  };

  unsigned Count = 0;
  for (llvm::CondBrInst *BI : CondBrs) {
    llvm::Value *Cond = BI->getCondition();
    llvm::BasicBlock *T = BI->getSuccessor(0);  // taken when Cond is true
    llvm::BasicBlock *Fa = BI->getSuccessor(1); // taken when Cond is false

    // Offset table: index 0 (Cond false) -> Fa, index 1 (Cond true) -> T.
    llvm::Constant *Elems[2] = {blockOffset(Fa), blockOffset(T)};
    auto *Tbl = new llvm::GlobalVariable(
        M, ArrTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(ArrTy, Elems), "nd_ibr_tbl");
    Tbl->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    llvm::IRBuilder<> B(BI);
    // idx = zext(Cond), laundered through the volatile slot.
    llvm::Value *Idx = B.CreateZExt(Cond, I32);
    B.CreateStore(Idx, Slot, /*isVolatile=*/true);
    llvm::Value *RoutedIdx =
        B.CreateLoad(I32, Slot, /*isVolatile=*/true, "nd_ibr_i");

    // The i32 index is fine as a GEP subscript directly (GEP sign-extends it to
    // pointer width); idx is 0/1 so the sign is irrelevant.
    llvm::Value *GEP =
        B.CreateInBoundsGEP(ArrTy, Tbl, {B.getInt64(0), RoutedIdx});
    llvm::Value *Off32 = B.CreateLoad(I32, GEP, "nd_ibr_off");
    llvm::Value *Off = Is64 ? B.CreateSExt(Off32, IntPtrTy) : Off32;
    llvm::Value *Tgt = B.CreateAdd(FInt, Off, "nd_ibr_t");
    llvm::Value *Routed = B.CreateIntToPtr(Tgt, PtrTy, "nd_ibr_p");

    auto *IBr = B.CreateIndirectBr(Routed, 2);
    IBr->addDestination(T);
    IBr->addDestination(Fa);

    // The original block remains the predecessor of T/Fa, so successor PHIs are
    // unaffected.  Drop the old conditional branch.
    BI->eraseFromParent();
    ++Count;
  }

  LLVM_DEBUG(llvm::dbgs() << "neverd: indirect branch applied to " << Count
                          << " conditional branch(es) in " << F.getName()
                          << "\n");
  return Count;
}

unsigned indirectBranchModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += indirectBranchFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses IndirectBranchPass::run(llvm::Module &M,
                                                llvm::ModuleAnalysisManager &) {
  unsigned N = indirectBranchModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned IndirectBranchPass::inject(llvm::Module &M) {
  return indirectBranchModule(M);
}

} // namespace neverd
