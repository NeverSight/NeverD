//===- BitMaskingPass.cpp - Bit-masking value obfuscation -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bit-masking pass: replaces an eligible integer (scalar / integer-vector)
/// result with the bitwise identity `(x & m) | (x & nm)`.  Demo-level sample
/// transform — see BitMaskingPass.h.
///
/// Once per function (and per value type) we materialise, at the top of the
/// entry block:
///
///   m  = load volatile m_slot           ; m_slot  initialised to K   (in
///   entry) nm = load volatile nm_slot          ; nm_slot initialised to ~K (in
///   entry)
///
/// and then, right after each eligible defining instruction `x`, emit:
///
///   x' = (x & m) | (x & nm)
///   ...uses of x become uses of x'...   ; the two `& x` operands keep using x
///
/// `m` and `nm` are loaded from two *independent* volatile slots, so the
/// backend cannot see that `nm` is the complement of `m` and cannot fold `x'`
/// back to `x`; the masking therefore reaches the emitted machine code.  At run
/// time `m == K` and `nm == ~K`, so `(x & K) | (x & ~K) == x & (K | ~K) == x`
/// for any K — bit-for-bit equivalent, element-wise for vectors.  The two loads
/// are hoisted to the entry block and reused at every site (they dominate all
/// of them): re-loading the masks at each site instead would emit one volatile
/// memory access per masked value and make the backend's volatile-chain
/// handling blow up on very large functions, with no added opacity.
///
/// Scope mirrors ValueLaunderingPass: results of non-PHI, non-terminator
/// instructions whose type is an integer or a fixed integer vector and that
/// have at least one use.  PHIs are skipped (their result would need masking
/// after all PHIs); terminators define no maskable value here; pointer /
/// floating- point / aggregate results are left untouched so the masks stay
/// plain integer cells.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/obf/BitMaskingPass.h"

#define DEBUG_TYPE "neverd-bit-masking"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>
#include <vector>

namespace neverd {

namespace {

// Arbitrary non-trivial seed for the mask K; the bitwise identity holds for any
// value, so the exact pattern is cosmetic (a repeating 0xA5 nibble pattern).
constexpr uint64_t kMaskSeed = 0xA5A5A5A5A5A5A5A5ULL;

// A value is maskable only if it is an integer or a fixed integer-vector scalar
// — pointers, floats and aggregates are left untouched to keep the transform
// simple and the mask slots plain integer cells.
bool isMaskableType(llvm::Type *Ty) {
  if (Ty->isIntegerTy())
    return true;
  if (auto *VT = llvm::dyn_cast<llvm::FixedVectorType>(Ty))
    return VT->getElementType()->isIntegerTy();
  return false;
}

// Build the mask constant K and its true complement ~K for \p Ty (scalar or
// fixed integer vector).  The complement is computed at the type's full element
// width via APInt so it is exact for any width (i1 .. i128) — never via a
// truncated uint64, which would mis-handle widths > 64.
std::pair<llvm::Constant *, llvm::Constant *> maskPairFor(llvm::Type *Ty) {
  llvm::Type *ElemTy = Ty;
  unsigned NumElts = 0;
  if (auto *VT = llvm::dyn_cast<llvm::FixedVectorType>(Ty)) {
    ElemTy = VT->getElementType();
    NumElts = VT->getNumElements();
  }
  unsigned Bits = ElemTy->getIntegerBitWidth();
  llvm::APInt KAp(Bits, kMaskSeed, /*isSigned=*/false,
                  /*implicitTrunc=*/true);
  llvm::Constant *K = llvm::ConstantInt::get(ElemTy, KAp);
  llvm::Constant *NM = llvm::ConstantInt::get(ElemTy, ~KAp);
  if (NumElts) {
    auto EC = llvm::ElementCount::getFixed(NumElts);
    return {llvm::ConstantVector::getSplat(EC, K),
            llvm::ConstantVector::getSplat(EC, NM)};
  }
  return {K, NM};
}

unsigned maskFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  // Snapshot maskable instructions up front so the freshly-created mask
  // loads/ands/ors (and the entry-block slots) are never themselves masked.
  std::vector<llvm::Instruction *> Sites;
  for (llvm::BasicBlock &BB : F) {
    for (llvm::Instruction &I : BB) {
      if (llvm::isa<llvm::PHINode>(&I))
        continue; // a PHI result must be masked after all PHIs
      if (I.isTerminator())
        continue; // terminators define no maskable value here
      if (I.use_empty())
        continue; // nothing to redirect
      if (!isMaskableType(I.getType()))
        continue;
      Sites.push_back(&I);
    }
  }
  if (Sites.empty())
    return 0;

  // Two volatile slots per value type at the top of the entry block, holding K
  // and ~K, each read back by a single volatile load that is *reused* at every
  // site.  Static allocas — they dominate every use and stay static so no
  // Windows __chkstk probe is emitted.  Initialised once: nobody writes the
  // slots again, so the volatile load returns K / ~K; keeping the two loads
  // independent is what stops the backend folding the masks away.  Loading each
  // mask once per function (instead of re-loading at every site) preserves that
  // opacity while avoiding a per-site volatile memory access — the latter makes
  // the backend's volatile-chain handling blow up on very large functions.
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::DenseMap<llvm::Type *, std::pair<llvm::Value *, llvm::Value *>>
      MaskVals;
  auto maskValsFor =
      [&](llvm::Type *Ty) -> std::pair<llvm::Value *, llvm::Value *> {
    auto It = MaskVals.find(Ty);
    if (It != MaskVals.end())
      return It->second;
    auto [K, NM] = maskPairFor(Ty);
    llvm::IRBuilder<> EB(&Entry, Entry.getFirstInsertionPt());
    auto *MSlot = EB.CreateAlloca(Ty, nullptr, "nd_bitmask_m");
    auto *NSlot = EB.CreateAlloca(Ty, nullptr, "nd_bitmask_nm");
    EB.CreateStore(K, MSlot, /*isVolatile=*/true);
    EB.CreateStore(NM, NSlot, /*isVolatile=*/true);
    llvm::Value *MV =
        EB.CreateLoad(Ty, MSlot, /*isVolatile=*/true, "nd_bitmask_mv");
    llvm::Value *NV =
        EB.CreateLoad(Ty, NSlot, /*isVolatile=*/true, "nd_bitmask_nv");
    auto P = std::make_pair(MV, NV);
    MaskVals[Ty] = P;
    return P;
  };

  unsigned Count = 0;
  for (llvm::Instruction *I : Sites) {
    llvm::Type *Ty = I->getType();
    // The mask values live in the entry block, so they dominate every site and
    // can be reused directly without re-loading them here.
    auto [MV, NV] = maskValsFor(Ty);

    // Insert the masking right after the defining instruction (a non-PHI,
    // non-terminator, so getNextNode() is at worst the block terminator).
    llvm::IRBuilder<> B(I->getNextNode());
    auto *Lo = llvm::cast<llvm::Instruction>(B.CreateAnd(I, MV));
    auto *Hi = llvm::cast<llvm::Instruction>(B.CreateAnd(I, NV));
    llvm::Value *Routed = B.CreateOr(Lo, Hi, "nd_bitmask_v");

    // Redirect every use of the value to the recombined copy, except the two
    // `& I` operands we just created (which must keep feeding the original
    // value into the masks).
    I->replaceUsesWithIf(Routed, [&](llvm::Use &U) {
      return U.getUser() != Lo && U.getUser() != Hi;
    });
    ++Count;
  }

  LLVM_DEBUG(llvm::dbgs() << "neverd: bit-masked " << Count << " value(s) in "
                          << F.getName() << "\n");
  return Count;
}

unsigned maskModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += maskFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses BitMaskingPass::run(llvm::Module &M,
                                            llvm::ModuleAnalysisManager &) {
  unsigned N = maskModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned BitMaskingPass::inject(llvm::Module &M) { return maskModule(M); }

} // namespace neverd
