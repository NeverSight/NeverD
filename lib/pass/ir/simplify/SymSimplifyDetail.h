//===- SymSimplifyDetail.h - Private SymSimplifyPass helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal types shared between the SymSimplifyPass driver
/// (SymSimplifyPass.cpp) and the LLVM IR <-> symbolic engine translator
/// (SymSimplifyTranslator.cpp): the operator classification predicates and the
/// Translator that carries integer expression trees in and out of the engine.
///
/// This header is an implementation detail of lib/pass/ir/simplify/ and should
/// NOT be included by code outside that directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PASS_IR_SIMPLIFY_SYMSIMPLIFYDETAIL_H
#define NEVERD_LIB_PASS_IR_SIMPLIFY_SYMSIMPLIFYDETAIL_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"

#include <cstdint>
#include <string>

namespace neverd {

namespace sym = symbolic;

/// The integer operators this pass carries into the engine.  Everything else
/// -- selects, loads, calls -- becomes an opaque input.
enum class OpTag {
  None,
  Add,
  Sub,
  Mul,
  And,
  Or,
  Xor,
  Shl,
  LShr,
  AShr,
  UDiv,
  SDiv,
  URem,
  SRem,
  Trunc,
  ZExt,
  SExt,
  ICmp,
};

OpTag tagOf(const llvm::Instruction &I);

/// True for an integer instruction the engine has an operator for.
///
/// A comparison is carried only when the caller asks for it, and only one
/// caller does.  Rebuilding an expression is what the ordinary path is for, and
/// a comparison has no place in one: it would have to come back out as an
/// instruction, and the engine's answer for a comparison is worth having only
/// when it is a constant.  Deciding a branch is that one case.
bool isTranslatable(const llvm::Value *V, bool WithComparisons);

//===----------------------------------------------------------------------===//
// LLVM IR <-> engine
//===----------------------------------------------------------------------===//

class Translator {
public:
  explicit Translator(sym::SymContext &Ctx, bool CarryComparisons = false)
      : Ctx(Ctx), CarryComparisons(CarryComparisons) {}

  /// Translate the tree rooted at \p Root, descending through single-use
  /// integer operators and standing an opaque input in front of everything
  /// else.
  sym::SymRef in(llvm::Value *Root);

  /// Rebuild an LLVM value from \p R, materializing new instructions before
  /// \p At and appending each one to \p NewInsts.  Returns null when the
  /// engine's result holds an operator with no IR spelling, which leaves the
  /// caller's rewrite un-made.
  ///
  /// The caller needs the list for both of the decisions that follow: what the
  /// rewrite actually costs once it is instructions rather than a measured
  /// tree, and -- when that cost does not pay -- what to take back out again.
  llvm::Value *out(sym::SymRef R, llvm::Instruction *At,
                   llvm::SmallVectorImpl<llvm::Instruction *> &NewInsts);

  /// How many LLVM instructions the way in descended through, which is exactly
  /// the set that RAUW leaves dead.  This is what a rebuilt form has to beat.
  unsigned descendedInsts() const { return NumDescended; }

private:
  /// Descend into \p V as an operator rather than stopping at it.  The root is
  /// always descended; anything below it only when it is a single-use integer
  /// operator, so shared computation stays one opaque input.
  bool descend(const llvm::Value *V, bool IsRoot) const {
    return isTranslatable(V, CarryComparisons) &&
           (IsRoot || llvm::cast<llvm::Instruction>(V)->hasOneUse());
  }

  /// The operands \p I is descended through.
  llvm::SmallVector<llvm::Value *, 2>
  children(const llvm::Instruction &I) const {
    switch (tagOf(I)) {
    case OpTag::Trunc:
    case OpTag::ZExt:
    case OpTag::SExt:
      return {I.getOperand(0)};
    default:
      return {I.getOperand(0), I.getOperand(1)};
    }
  }

  /// A constant becomes a literal; anything else becomes a fresh input, whose
  /// engine node is recorded so the way back can substitute the original value.
  sym::SymRef leaf(llvm::Value *V) {
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V))
      return Ctx.mkConst(CI->getValue());
    const uint32_t Width = V->getType()->getIntegerBitWidth();
    sym::SymRef R = Ctx.mkVar("nd$" + std::to_string(OpaqueCount++), Width);
    Sources[R.index()] = V;
    return R;
  }

  sym::SymRef build(const llvm::Instruction &I);

  sym::SymContext &Ctx;
  bool CarryComparisons = false;
  llvm::DenseMap<const llvm::Value *, sym::SymRef> Memo;
  /// Engine node index to the LLVM value it stands for, for the way back.
  llvm::DenseMap<uint32_t, llvm::Value *> Sources;
  unsigned OpaqueCount = 0;
  unsigned NumDescended = 0;
};

} // namespace neverd

#endif // NEVERD_LIB_PASS_IR_SIMPLIFY_SYMSIMPLIFYDETAIL_H
