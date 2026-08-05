//===- ConstantEncryptionPass.cpp - Constant encryption pass -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Constant-encryption pass: replaces integer constant operands of binary
/// operators and integer comparisons with run-time-decrypted values.  Demo-
/// level sample transform — see ConstantEncryptionPass.h.
///
/// Identity (exact for two's-complement / modular integers):
///
///   C  ==  (C ^ K) ^ k        where k is an opaque run-time copy of K
///
/// The opaque copy is produced with a volatile stack slot — a volatile store
/// of K followed by a volatile load.  Volatile accesses may neither be removed
/// nor value-forwarded, so the backend cannot prove <tt>k == K</tt> and the
/// trailing <tt>^ k</tt> survives to machine code, while <tt>C ^ K</tt> is
/// folded to a single constant at compile time.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/ConstantEncryptionPass.h"

#define DEBUG_TYPE "neverd-const-enc"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace neverd {

namespace {

// Only standard integer widths are encrypted; odd widths (i1, i24, ...) are
// left untouched to keep the demo transform simple and robustly correct.
bool eligibleWidth(unsigned W) {
  return W == 8 || W == 16 || W == 32 || W == 64;
}

// A fixed per-width key.  The 0xA5.. pattern is truncated to the width and
// forced non-zero (a zero key would still be opaque, but a non-zero key makes
// the encryption visible in the emitted bytes).
uint64_t keyForWidth(unsigned W) {
  uint64_t Master = 0xA5A5A5A5A5A5A5A5ULL;
  uint64_t K = (W >= 64) ? Master : (Master & ((1ULL << W) - 1));
  return K ? K : 1;
}

// Returns true if \p I is an instruction whose operands are plain integer
// values (not required immediates) — i.e. safe to replace with a run-time
// decrypted value.  Binary operators and integer compares always accept value
// operands; everything else (GEP indices, switch cases, intrinsic immediates,
// ...) is left alone.
bool encryptableInst(llvm::Instruction *I) {
  return llvm::isa<llvm::BinaryOperator>(I) || llvm::isa<llvm::ICmpInst>(I);
}

// Per-function encryptor: lazily materializes one opaque key per width and one
// decrypted value per distinct constant, all in the entry block so they
// dominate every use.
class FnEncryptor {
public:
  explicit FnEncryptor(llvm::Function &F)
      : F(F), B(&F.getEntryBlock(), F.getEntryBlock().getFirstInsertionPt()) {}

  unsigned run() {
    // Snapshot the eligible (instruction, operand) sites first; we mutate
    // operands as we go and create new instructions in the entry block.
    struct Site {
      llvm::Instruction *I;
      unsigned OpNo;
      llvm::ConstantInt *C;
    };
    std::vector<Site> Sites;
    for (llvm::Instruction &I : llvm::instructions(F)) {
      if (!encryptableInst(&I))
        continue;
      for (unsigned OpNo = 0, E = I.getNumOperands(); OpNo < E; ++OpNo) {
        auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(OpNo));
        if (!CI || !eligibleWidth(CI->getBitWidth()))
          continue;
        Sites.push_back({&I, OpNo, CI});
      }
    }

    unsigned Count = 0;
    for (const Site &S : Sites) {
      S.I->setOperand(S.OpNo, decryptFor(S.C));
      ++Count;
    }
    return Count;
  }

private:
  // Opaque run-time copy of keyForWidth(W): volatile store + volatile load of a
  // fresh stack slot.  One per width, cached.
  llvm::Value *opaqueKey(unsigned W) {
    auto It = WidthKey.find(W);
    if (It != WidthKey.end())
      return It->second;
    auto *Ty = llvm::IntegerType::get(F.getContext(), W);
    llvm::Value *Slot = B.CreateAlloca(Ty, nullptr, "nd_ck_slot");
    B.CreateStore(llvm::ConstantInt::get(Ty, keyForWidth(W)), Slot,
                  /*isVolatile=*/true);
    llvm::Value *K = B.CreateLoad(Ty, Slot, /*isVolatile=*/true, "nd_ck");
    WidthKey[W] = K;
    return K;
  }

  // (C ^ K) ^ opaqueKey == C at run time; cached per distinct ConstantInt.
  llvm::Value *decryptFor(llvm::ConstantInt *CI) {
    auto It = DecCache.find(CI);
    if (It != DecCache.end())
      return It->second;
    unsigned W = CI->getBitWidth();
    llvm::Value *K = opaqueKey(W);
    llvm::APInt Enc = CI->getValue() ^ llvm::APInt(W, keyForWidth(W));
    llvm::Value *EncC = llvm::ConstantInt::get(F.getContext(), Enc);
    llvm::Value *Dec = B.CreateXor(EncC, K, "nd_cdec");
    DecCache[CI] = Dec;
    return Dec;
  }

  llvm::Function &F;
  llvm::IRBuilder<> B;
  llvm::DenseMap<unsigned, llvm::Value *> WidthKey;
  llvm::DenseMap<llvm::ConstantInt *, llvm::Value *> DecCache;
};

unsigned encryptModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    FnEncryptor Enc(F);
    Total += Enc.run();
  }
  LLVM_DEBUG(llvm::dbgs() << "neverd: constant encryption obfuscated " << Total
                          << " constant operand(s)\n");
  return Total;
}

} // namespace

llvm::PreservedAnalyses
ConstantEncryptionPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  unsigned N = encryptModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned ConstantEncryptionPass::inject(llvm::Module &M) {
  return encryptModule(M);
}

} // namespace neverd
