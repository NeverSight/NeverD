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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
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

  /// Whether \p R still references every instruction that became an opaque
  /// input on the way in.  A candidate may simplify around such a boundary,
  /// but it may not use an identity to erase one.
  bool retainsOpaqueInstructionLeaves(sym::SymRef R) const;

  /// Whether rebuilding \p R as LLVM IR preserves the symbolic operator's
  /// total bitvector domain.  In particular, a synthesis policy may search
  /// variable shifts, but LLVM shifts are poison when the amount is out of
  /// range and therefore cannot cross the production rewrite boundary.
  bool hasCompatibleResultSemantics(sym::SymRef R) const {
    if (!R.isValid())
      return false;

    llvm::SmallVector<sym::SymRef, 32> Work{R};
    llvm::DenseSet<uint32_t> Seen;
    while (!Work.empty()) {
      const sym::SymRef Current = Work.pop_back_val();
      if (!Seen.insert(Current.index()).second)
        continue;

      switch (Ctx.op(Current)) {
      case sym::SymOp::Const:
      case sym::SymOp::Var:
      case sym::SymOp::Add:
      case sym::SymOp::Mul:
      case sym::SymOp::And:
      case sym::SymOp::Or:
      case sym::SymOp::Xor:
      case sym::SymOp::Not:
      case sym::SymOp::Extract:
      case sym::SymOp::Concat:
      case sym::SymOp::ZExt:
      case sym::SymOp::SExt:
      case sym::SymOp::Ite:
        break;
      case sym::SymOp::Shl:
      case sym::SymOp::LShr:
      case sym::SymOp::AShr: {
        const sym::SymRef Amount = Ctx.operand(Current, 1);
        if (!Ctx.isConst(Amount) ||
            Ctx.constValue(Amount).uge(Ctx.width(Current)))
          return false;
        break;
      }
      default:
        return false;
      }

      const llvm::ArrayRef<sym::SymRef> Ops = Ctx.operands(Current);
      Work.append(Ops.begin(), Ops.end());
    }
    return true;
  }

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
  /// Literals are carried as \c llvm::APInt, so a 128-bit one is as ordinary
  /// as a byte.
  ///
  /// Returns an invalid ref for a leaf the engine has no bitvector for.  A
  /// comparison is the one translated operator whose operands need not be
  /// integers -- `icmp eq ptr %a, %b` produces the i1 that makes the
  /// comparison translatable -- and a pointer has no width to stand an input
  /// at.
  sym::SymRef leaf(llvm::Value *V) {
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V))
      return Ctx.mkConst(CI->getValue());
    if (!V->getType()->isIntegerTy())
      return {};
    const uint32_t Width = V->getType()->getIntegerBitWidth();

    // Preserve a source value's name in diagnostics.  Anonymous values use a
    // deterministic sequence, but skip every name already present in the
    // containing function so an anonymous leaf visited first cannot steal a
    // later named leaf's display name.
    std::string Name;
    if (V->hasName()) {
      Name = V->getName().str();
    } else {
      auto ConflictsWithNamedValue = [&](llvm::StringRef Candidate) {
        const llvm::Function *F = nullptr;
        if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V))
          F = I->getFunction();
        else if (const auto *A = llvm::dyn_cast<llvm::Argument>(V))
          F = A->getParent();
        if (!F)
          return false;
        for (const llvm::Argument &A : F->args())
          if (A.hasName() && A.getName() == Candidate)
            return true;
        for (const llvm::BasicBlock &BB : *F)
          for (const llvm::Instruction &I : BB)
            if (I.hasName() && I.getName() == Candidate)
              return true;
        return false;
      };

      do {
        Name = "nd$" + std::to_string(AnonymousCount++);
      } while (Ctx.findVar(Name).has_value() || ConflictsWithNamedValue(Name));
    }

    // LLVM's local symbol table makes named values unique.  The defensive
    // fallback handles hand-built malformed IR, and remains deterministic.
    if (Ctx.findVar(Name).has_value()) {
      const std::string Prefix = Name + "$";
      do {
        Name = Prefix + std::to_string(AnonymousCount++);
      } while (Ctx.findVar(Name).has_value());
    }

    sym::SymRef R = Ctx.mkVar(Name, Width);
    Sources[R.index()] = V;
    if (llvm::isa<llvm::Instruction>(V))
      OpaqueInstructionLeaves.insert(R.index());
    return R;
  }

  sym::SymRef build(const llvm::Instruction &I);

  sym::SymContext &Ctx;
  bool CarryComparisons = false;
  llvm::DenseMap<const llvm::Value *, sym::SymRef> Memo;
  /// Engine node index to the LLVM value it stands for, for the way back.
  llvm::DenseMap<uint32_t, llvm::Value *> Sources;
  /// Engine variables standing for instruction boundaries that must survive.
  llvm::DenseSet<uint32_t> OpaqueInstructionLeaves;
  unsigned AnonymousCount = 0;
  unsigned NumDescended = 0;
};

} // namespace neverd

#endif // NEVERD_LIB_PASS_IR_SIMPLIFY_SYMSIMPLIFYDETAIL_H
