//===- ConstantPoolingPass.cpp - Constant pooling pass -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Constant-pooling pass: moves integer constant operands of binary operators
/// and integer comparisons into a pass-created read-only global pool, fetching
/// them at run time through an opaque index.  Demo-level sample transform — see
/// ConstantPoolingPass.h.
///
/// Identity (exact for two's-complement / modular integers):
///
///   C  ==  trunc_W( pool[i] )     where pool[i] == zext_64(C) and i is an
///                                 opaque run-time copy of the pool slot
///
/// The opaque index is produced with a volatile stack slot — a volatile store
/// of the slot number followed by a volatile load.  Volatile accesses may
/// neither be removed nor value-forwarded, so the backend cannot prove the
/// index is constant and the indexed load survives to machine code (rather than
/// being folded back to the original immediate).
///
/// Unlike the other L1 samples, this pass *creates a new global variable*
/// (the read-only pool), so it is the first transform to exercise the rewrite
/// backend's placement / relocation of pass-introduced read-only data and
/// run-time-indexed global addressing.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/ConstantPoolingPass.h"

#define DEBUG_TYPE "neverd-const-pool"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace neverd {

namespace {

// Only standard integer widths are pooled; odd widths (i1, i24, ...) are left
// untouched to keep the demo transform simple and robustly correct.
bool eligibleWidth(unsigned W) {
  return W == 8 || W == 16 || W == 32 || W == 64;
}

// Returns true if \p I is an instruction whose operands are plain integer
// values (not required immediates) — i.e. safe to replace with a run-time
// loaded value.  Binary operators and integer compares always accept value
// operands; everything else (GEP indices, switch cases, intrinsic immediates,
// ...) is left alone.  Mirrors ConstantEncryptionPass.
bool poolableInst(llvm::Instruction *I) {
  return llvm::isa<llvm::BinaryOperator>(I) || llvm::isa<llvm::ICmpInst>(I);
}

// Per-function pooler: collects the distinct scalar integer constants used by
// eligible instructions, parks them in one read-only global array, and rewrites
// every use to an opaque-indexed load from that array.  All loads are
// materialized in the entry block so they dominate every use.
class FnPooler {
public:
  explicit FnPooler(llvm::Function &F)
      : F(F), Ctx(F.getContext()),
        B(&F.getEntryBlock(), F.getEntryBlock().getFirstInsertionPt()) {}

  unsigned run() {
    // 1. Snapshot the eligible (instruction, operand) sites and assign each
    //    distinct constant a pool slot.  We mutate operands afterwards.
    struct Site {
      llvm::Instruction *I;
      unsigned OpNo;
      llvm::ConstantInt *C;
    };
    std::vector<Site> Sites;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      if (!poolableInst(&I))
        continue;
      for (unsigned OpNo = 0, E = I.getNumOperands(); OpNo < E; ++OpNo) {
        auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(OpNo));
        if (!CI || !eligibleWidth(CI->getBitWidth()))
          continue;
        if (!Slot.count(CI)) {
          Slot[CI] = static_cast<unsigned>(Pool.size());
          Pool.push_back(CI);
        }
        Sites.push_back({&I, OpNo, CI});
      }
    }
    if (Pool.empty())
      return 0;

    // 2. Build the private read-only pool: [N x i64], each slot the constant's
    //    bit pattern zero-extended to 64 bits.  trunc on load recovers the
    //    exact low-W bits regardless of sign.
    auto *I64 = llvm::Type::getInt64Ty(Ctx);
    ArrTy = llvm::ArrayType::get(I64, Pool.size());
    std::vector<llvm::Constant *> Init;
    Init.reserve(Pool.size());
    for (auto *CI : Pool)
      Init.push_back(
          llvm::ConstantInt::get(I64, CI->getValue().getZExtValue()));
    auto *ArrInit = llvm::ConstantArray::get(ArrTy, Init);
    GV = new llvm::GlobalVariable(*F.getParent(), ArrTy, /*isConstant=*/true,
                                  llvm::GlobalValue::InternalLinkage, ArrInit,
                                  "neverd_const_pool." + F.getName().str());
    GV->setDSOLocal(true);

    // 3. One volatile index slot in the entry block (static alloca at the very
    //    top so it never becomes a dynamic alloca / Windows __chkstk probe).
    IdxSlot = B.CreateAlloca(llvm::Type::getInt32Ty(Ctx), nullptr, "nd_cp_idx");

    // 4. Rewrite every site to the (cached) opaque-indexed load.
    unsigned Count = 0;
    for (const Site &S : Sites) {
      S.I->setOperand(S.OpNo, loadFor(S.C));
      ++Count;
    }
    return Count;
  }

private:
  // trunc_W(pool[opaque(slot)]) == C at run time; cached per distinct constant.
  llvm::Value *loadFor(llvm::ConstantInt *CI) {
    auto It = Cache.find(CI);
    if (It != Cache.end())
      return It->second;
    auto *I32 = llvm::Type::getInt32Ty(Ctx);
    auto *I64 = llvm::Type::getInt64Ty(Ctx);
    // Opaque index: store the slot number, then volatile-load it back so the
    // backend cannot fold the indexed load to the original immediate.
    B.CreateStore(llvm::ConstantInt::get(I32, Slot[CI]), IdxSlot,
                  /*isVolatile=*/true);
    llvm::Value *Idx =
        B.CreateLoad(I32, IdxSlot, /*isVolatile=*/true, "nd_cp_i");
    llvm::Value *Ptr = B.CreateInBoundsGEP(
        ArrTy, GV, {llvm::ConstantInt::get(I32, 0), Idx}, "nd_cp_p");
    llvm::Value *V = B.CreateLoad(I64, Ptr, "nd_cp_v");
    unsigned W = CI->getBitWidth();
    if (W != 64)
      V = B.CreateTrunc(V, CI->getType(), "nd_cp_t");
    Cache[CI] = V;
    return V;
  }

  llvm::Function &F;
  llvm::LLVMContext &Ctx;
  llvm::IRBuilder<> B;
  std::vector<llvm::ConstantInt *> Pool; // distinct constants, in order
  llvm::DenseMap<llvm::ConstantInt *, unsigned> Slot; // constant -> pool index
  llvm::DenseMap<llvm::ConstantInt *, llvm::Value *> Cache; // constant -> load
  llvm::ArrayType *ArrTy = nullptr;
  llvm::GlobalVariable *GV = nullptr;
  llvm::Value *IdxSlot = nullptr;
};

unsigned poolModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    FnPooler P(F);
    Total += P.run();
  }
  LLVM_DEBUG(llvm::dbgs() << "neverd: constant pooling moved " << Total
                          << " constant operand(s) into read-only pools\n");
  return Total;
}

} // namespace

llvm::PreservedAnalyses
ConstantPoolingPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  unsigned N = poolModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned ConstantPoolingPass::inject(llvm::Module &M) { return poolModule(M); }

} // namespace neverd
