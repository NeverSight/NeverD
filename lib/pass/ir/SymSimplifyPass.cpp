//===- SymSimplifyPass.cpp - Semantic simplification of LLVM IR ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs the symbolic engine over the integer expression trees of a function
/// and puts back whatever came out shorter.
///
/// The pass sits between two runs of InstCombine.  InstCombine canonicalizes by
/// shape, which is exactly what a measurement wants to read; the measurement
/// returns a compact arithmetic form, which is exactly what InstCombine folds
/// best.  An expression mixing `+ - *` with `& | ^ ~` is a fixed point for both
/// InstCombine and any other rule-driven simplifier -- undoing it is the reason
/// this pass exists -- so the two only reach a joint fixed point together.
///
/// Translation in both directions is deliberately narrow.  Only the operators
/// that are bitvector arithmetic on a whole word are carried across; a load, a
/// call, an argument, a comparison, a PHI -- each becomes one opaque input and
/// comes back untouched.  A value with more than one use also stays opaque, so
/// a subterm the obfuscator shares is measured as one input on every side and
/// cancels, and rebuilding never duplicates a computation the CFG already has.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/SymSimplifyPass.h"

#include "neverd/symbolic/SymMBA.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/ConstantFolder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Transforms/Utils/Local.h"

#include <cstdint>
#include <string>

namespace neverd {

namespace {

namespace sym = neverd::symbolic;

/// A leaf or one already-canonical operator cannot become shorter.  Four nodes
/// is the first useful case: `~x + 1` is four and simplifies to `-x`.
constexpr size_t kMinInterestingNodes = 4;

/// A rewrite is made only when it materializes at least this many fewer
/// instructions than it replaces.  One is enough because the InstCombine that
/// follows cleans up any residue; the point of the gate is to avoid rewriting
/// what is already minimal and churning the IR the rest of the pipeline is
/// built on.
constexpr size_t kMinGain = 1;

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

OpTag tagOf(const llvm::Instruction &I) {
  if (llvm::isa<llvm::TruncInst>(I))
    return OpTag::Trunc;
  if (llvm::isa<llvm::ZExtInst>(I))
    return OpTag::ZExt;
  if (llvm::isa<llvm::SExtInst>(I))
    return OpTag::SExt;
  if (llvm::isa<llvm::ICmpInst>(I))
    return OpTag::ICmp;
  switch (I.getOpcode()) {
  case llvm::Instruction::Add:
    return OpTag::Add;
  case llvm::Instruction::Sub:
    return OpTag::Sub;
  case llvm::Instruction::Mul:
    return OpTag::Mul;
  case llvm::Instruction::And:
    return OpTag::And;
  case llvm::Instruction::Or:
    return OpTag::Or;
  case llvm::Instruction::Xor:
    return OpTag::Xor;
  case llvm::Instruction::Shl:
    return OpTag::Shl;
  case llvm::Instruction::LShr:
    return OpTag::LShr;
  case llvm::Instruction::AShr:
    return OpTag::AShr;
  case llvm::Instruction::UDiv:
    return OpTag::UDiv;
  case llvm::Instruction::SDiv:
    return OpTag::SDiv;
  case llvm::Instruction::URem:
    return OpTag::URem;
  case llvm::Instruction::SRem:
    return OpTag::SRem;
  default:
    return OpTag::None;
  }
}

/// True for an integer instruction the engine has an operator for.
///
/// A comparison is carried only when the caller asks for it, and only one
/// caller does.  Rebuilding an expression is what the ordinary path is for, and
/// a comparison has no place in one: it would have to come back out as an
/// instruction, and the engine's answer for a comparison is worth having only
/// when it is a constant.  Deciding a branch is that one case.
bool isTranslatable(const llvm::Value *V, bool WithComparisons) {
  const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
  if (!I || !I->getType()->isIntegerTy())
    return false;
  OpTag Tag = tagOf(*I);
  return Tag != OpTag::None && (WithComparisons || Tag != OpTag::ICmp);
}

/// Whether \p I has the same domain in LLVM IR and in the engine's total
/// bitvector algebra.
///
/// The engine deliberately gives every operator a value for every input.
/// LLVM instructions carrying poison-generating flags, or operations such as
/// an unchecked shift, have a smaller domain.  Looking through one would let a
/// total-algebra identity turn poison into an ordinary value.  Keep the whole
/// candidate opaque instead; a future translation can carry the precondition
/// explicitly rather than silently dropping it.
bool hasCompatibleSemantics(const llvm::Instruction &I) {
  return !llvm::canCreatePoison(llvm::cast<llvm::Operator>(&I));
}

/// Whether a translatable value hidden behind an opaque shared leaf contains
/// semantics the total bitvector engine cannot preserve.
///
/// Ordinary translation visits every operand and rejects the whole candidate
/// when it reaches undef, poison, or a poison-generating instruction.  A value
/// with multiple uses deliberately stays opaque, though, so its operands would
/// otherwise escape that check.  Follow the defining operand graph only to
/// reject unsafe candidates; this never makes an opaque operation
/// translatable.
bool hasHiddenIncompatibleSemantics(const llvm::Instruction &Root,
                                    bool WithComparisons) {
  llvm::SmallVector<const llvm::Value *, 8> Work;
  for (const llvm::Use &Op : Root.operands())
    Work.push_back(Op.get());

  llvm::DenseSet<const llvm::Value *> Seen;
  while (!Work.empty()) {
    const llvm::Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V))
      return true;

    const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
    if (!I)
      continue;
    if (isTranslatable(I, WithComparisons) && !hasCompatibleSemantics(*I))
      return true;
    for (const llvm::Use &Op : I->operands())
      Work.push_back(Op.get());
  }
  return false;
}

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

sym::SymRef Translator::build(const llvm::Instruction &I) {
  auto M = [&](llvm::Value *V) { return Memo.lookup(V); };
  llvm::Value *A = I.getOperand(0);
  llvm::Value *B = I.getNumOperands() > 1 ? I.getOperand(1) : nullptr;
  const uint32_t Width = I.getType()->getIntegerBitWidth();

  switch (tagOf(I)) {
  case OpTag::Add:
    return Ctx.mkAdd(M(A), M(B));
  case OpTag::Sub:
    // `0 - x` is negation, which the engine represents as a product with -1;
    // handing it that form directly keeps the sign out of the measured value.
    return Ctx.isConstZero(M(A)) ? Ctx.mkNeg(M(B)) : Ctx.mkSub(M(A), M(B));
  case OpTag::Mul:
    return Ctx.mkMul(M(A), M(B));
  case OpTag::And:
    return Ctx.mkAnd(M(A), M(B));
  case OpTag::Or:
    return Ctx.mkOr(M(A), M(B));
  case OpTag::Xor:
    // `x ^ -1` is complement, a generator of the bitwise algebra the solver
    // works in, so it is worth recognising rather than leaving as a xor.
    if (Ctx.isConstOnes(M(B)))
      return Ctx.mkNot(M(A));
    if (Ctx.isConstOnes(M(A)))
      return Ctx.mkNot(M(B));
    return Ctx.mkXor(M(A), M(B));
  case OpTag::Shl:
    return Ctx.mkShl(M(A), M(B));
  case OpTag::LShr:
    return Ctx.mkLShr(M(A), M(B));
  case OpTag::AShr:
    return Ctx.mkAShr(M(A), M(B));
  case OpTag::UDiv:
    return Ctx.mkUDiv(M(A), M(B));
  case OpTag::SDiv:
    return Ctx.mkSDiv(M(A), M(B));
  case OpTag::URem:
    return Ctx.mkURem(M(A), M(B));
  case OpTag::SRem:
    return Ctx.mkSRem(M(A), M(B));
  case OpTag::Trunc:
    return Ctx.mkExtract(M(A), 0, Width);
  case OpTag::ZExt:
    return Ctx.mkZExt(M(A), Width);
  case OpTag::SExt:
    return Ctx.mkSExt(M(A), Width);
  case OpTag::ICmp:
    switch (llvm::cast<llvm::ICmpInst>(I).getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      return Ctx.mkEq(M(A), M(B));
    case llvm::CmpInst::ICMP_NE:
      return Ctx.mkNe(M(A), M(B));
    case llvm::CmpInst::ICMP_ULT:
      return Ctx.mkUlt(M(A), M(B));
    case llvm::CmpInst::ICMP_ULE:
      return Ctx.mkUle(M(A), M(B));
    case llvm::CmpInst::ICMP_UGT:
      return Ctx.mkUgt(M(A), M(B));
    case llvm::CmpInst::ICMP_UGE:
      return Ctx.mkUge(M(A), M(B));
    case llvm::CmpInst::ICMP_SLT:
      return Ctx.mkSlt(M(A), M(B));
    case llvm::CmpInst::ICMP_SLE:
      return Ctx.mkSle(M(A), M(B));
    case llvm::CmpInst::ICMP_SGT:
      return Ctx.mkSgt(M(A), M(B));
    case llvm::CmpInst::ICMP_SGE:
      return Ctx.mkSge(M(A), M(B));
    default:
      break;
    }
    break;
  case OpTag::None:
    break;
  }
  llvm_unreachable("build called on an instruction with no operator tag");
}

sym::SymRef Translator::in(llvm::Value *Root) {
  struct WorkItem {
    llvm::Value *V;
    bool ChildrenReady;
  };
  llvm::SmallVector<WorkItem, 64> Work{{Root, false}};
  llvm::DenseSet<const llvm::Value *> Active;

  while (!Work.empty()) {
    WorkItem Item = Work.pop_back_val();
    llvm::Value *V = Item.V;
    if (Memo.count(V)) {
      if (Item.ChildrenReady)
        Active.erase(V);
      continue;
    }

    // An explicit undef may take a different value at each use, and poison is
    // outside the engine's total value domain.  Likewise, do not cross an
    // instruction that can introduce poison from otherwise ordinary operands.
    // Aborting the candidate is stricter than standing an opaque variable in
    // front of it: an algebraic rewrite could otherwise cancel that variable
    // and erase the very condition the placeholder was meant to preserve.
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V))
      return {};
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
        I && isTranslatable(I, CarryComparisons) && !hasCompatibleSemantics(*I))
      return {};

    if (!descend(V, V == Root)) {
      if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
          I && hasHiddenIncompatibleSemantics(*I, CarryComparisons))
        return {};
      Memo[V] = leaf(V);
      continue;
    }

    auto &I = *llvm::cast<llvm::Instruction>(V);
    if (!Item.ChildrenReady) {
      // Def-use of non-PHI values is acyclic, but malformed input should
      // preserve a node opaquely rather than spin the worklist forever.
      if (!Active.insert(V).second) {
        Memo[V] = leaf(V);
        continue;
      }
      Work.push_back({V, true});
      llvm::SmallVector<llvm::Value *, 2> Kids = children(I);
      for (auto It = Kids.rbegin(); It != Kids.rend(); ++It)
        if (!Memo.count(*It))
          Work.push_back({*It, false});
      continue;
    }

    Active.erase(V);
    Memo[V] = build(I);
    ++NumDescended;
  }
  return Memo.lookup(Root);
}

llvm::Value *
Translator::out(sym::SymRef R, llvm::Instruction *At,
                llvm::SmallVectorImpl<llvm::Instruction *> &NewInsts) {
  llvm::LLVMContext &LLCtx = At->getContext();

  // Collecting through the inserter rather than diffing the block afterwards
  // keeps the list exact: the folder answers some of these builder calls with a
  // constant and others with a value the original code already computes, and
  // neither is an instruction this rewrite is paying for.
  llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderCallbackInserter> B(
      LLCtx, llvm::ConstantFolder(),
      llvm::IRBuilderCallbackInserter(
          [&NewInsts](llvm::Instruction *I) { NewInsts.push_back(I); }));
  B.SetInsertPoint(At);
  // The rebuilt value computes what the root computed, so it belongs to the
  // same source construct.  Without this the recovered arithmetic would carry
  // no location at all and a debug build would lose the line the obfuscated
  // expression came from -- the one line a reader most wants back.
  B.SetCurrentDebugLocation(At->getDebugLoc());

  struct WorkItem {
    sym::SymRef Ref;
    bool ChildrenReady;
  };
  llvm::SmallVector<WorkItem, 64> Work{{R, false}};
  llvm::DenseMap<uint32_t, llvm::Value *> Built;
  llvm::DenseSet<uint32_t> Active;

  auto get = [&](sym::SymRef C) -> llvm::Value * {
    return Built.lookup(C.index());
  };

  while (!Work.empty()) {
    WorkItem Item = Work.pop_back_val();
    const uint32_t Index = Item.Ref.index();
    if (Built.count(Index)) {
      if (Item.ChildrenReady)
        Active.erase(Index);
      continue;
    }
    if (!Item.ChildrenReady) {
      if (!Active.insert(Index).second) {
        Built[Index] = nullptr;
        continue;
      }
      Work.push_back({Item.Ref, true});
      llvm::ArrayRef<sym::SymRef> Ops = Ctx.operands(Item.Ref);
      for (auto It = Ops.rbegin(); It != Ops.rend(); ++It)
        if (!Built.count(It->index()))
          Work.push_back({*It, false});
      continue;
    }

    Active.erase(Index);
    const uint32_t Width = Ctx.width(Item.Ref);
    auto *Ty = llvm::IntegerType::get(LLCtx, Width);
    llvm::ArrayRef<sym::SymRef> Ops = Ctx.operands(Item.Ref);

    auto reduce = [&](auto Make) -> llvm::Value * {
      if (Ops.empty())
        return nullptr;
      llvm::Value *Acc = get(Ops[0]);
      for (size_t I = 1; I < Ops.size() && Acc; ++I) {
        llvm::Value *Rhs = get(Ops[I]);
        Acc = Rhs ? Make(Acc, Rhs) : nullptr;
      }
      return Acc;
    };

    llvm::Value *Result = nullptr;
    switch (Ctx.op(Item.Ref)) {
    case sym::SymOp::Const:
      Result = llvm::ConstantInt::get(Ty, Ctx.constValue(Item.Ref));
      break;
    case sym::SymOp::Var:
      // Every variable in the result came from the way in; a miss would mean
      // the solver returned one of its own placeholders, which it never does.
      Result = Sources.lookup(Index);
      break;
    case sym::SymOp::Add:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateAdd(X, Y); });
      break;
    case sym::SymOp::Mul: {
      // Negation is stored as a product with -1; `-x` reads better than `-1 *
      // x` and folds the same.
      if (Ops.size() == 2 && Ctx.isConstOnes(Ops[0])) {
        llvm::Value *X = get(Ops[1]);
        Result = X ? B.CreateNeg(X) : nullptr;
      } else {
        Result = reduce(
            [&](llvm::Value *X, llvm::Value *Y) { return B.CreateMul(X, Y); });
      }
      break;
    }
    case sym::SymOp::And:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateAnd(X, Y); });
      break;
    case sym::SymOp::Or:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateOr(X, Y); });
      break;
    case sym::SymOp::Xor:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateXor(X, Y); });
      break;
    case sym::SymOp::Not: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      Result = X ? B.CreateNot(X) : nullptr;
      break;
    }
    case sym::SymOp::Shl: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateShl(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::LShr: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateLShr(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::AShr: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateAShr(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::UDiv: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateUDiv(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::SDiv: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateSDiv(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::URem: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateURem(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::SRem: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateSRem(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::Extract: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      if (X) {
        const uint32_t Low = static_cast<uint32_t>(Ctx.node(Item.Ref).Aux);
        if (Low != 0)
          X = B.CreateLShr(X, llvm::ConstantInt::get(X->getType(), Low));
        Result = B.CreateTrunc(X, Ty);
      }
      break;
    }
    case sym::SymOp::ZExt: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      Result = X ? B.CreateZExt(X, Ty) : nullptr;
      break;
    }
    case sym::SymOp::SExt: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      Result = X ? B.CreateSExt(X, Ty) : nullptr;
      break;
    }
    case sym::SymOp::Concat: {
      llvm::Value *Acc = llvm::ConstantInt::get(Ty, 0);
      for (sym::SymRef Op : Ops) {
        llvm::Value *V = get(Op);
        if (!V) {
          Acc = nullptr;
          break;
        }
        Acc = B.CreateShl(Acc, Ctx.width(Op));
        Acc = B.CreateOr(Acc, B.CreateZExt(V, Ty));
      }
      Result = Acc;
      break;
    }
    case sym::SymOp::Ite: {
      llvm::Value *C = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *T = get(Ctx.operand(Item.Ref, 1));
      llvm::Value *E = get(Ctx.operand(Item.Ref, 2));
      Result = C && T && E ? B.CreateSelect(C, T, E) : nullptr;
      break;
    }
    default:
      // Rotates and comparisons never come back out, because nothing on the way
      // in ever puts one in.
      Result = nullptr;
      break;
    }
    Built[Index] = Result;
  }
  return Built.lookup(R.index());
}

/// An instruction is a root when it is a translatable integer operator that is
/// not already going to be absorbed into a larger tree above it.
bool isRoot(const llvm::Instruction &I) {
  if (!isTranslatable(&I, /*WithComparisons=*/false) || I.use_empty())
    return false;
  if (I.hasOneUse()) {
    const auto *U = llvm::dyn_cast<llvm::Instruction>(*I.user_begin());
    if (U && isTranslatable(U, /*WithComparisons=*/false))
      return false;
  }
  return true;
}

/// Measure the tree at \p Root and replace it when a shorter form exists.
///
/// On success \p Root is left with no uses and appended to \p Dead.  Deleting
/// it here would invalidate the caller's list of roots still to visit, so the
/// sweep is deferred until every root has been measured.
bool rewriteRoot(llvm::Instruction *Root,
                 llvm::SmallVectorImpl<llvm::WeakTrackingVH> &Dead) {
  sym::SymContext Ctx;
  Translator Xlat(Ctx);
  sym::SymRef Before = Xlat.in(Root);
  if (!Before.isValid())
    return false;
  if (Ctx.dagSize(Before) < kMinInterestingNodes)
    return false;

  // Nesting is not a budget: the deep walk is iterative and visits the finite
  // DAG without a recursion cutoff.  Keep the production work budget for the
  // exponential corner measurements and product search, though; callers that
  // trust their input can explicitly remove that resource guard through the
  // public simplify API.
  sym::MBAResult Res = sym::simplifyMBADeep(Ctx, Before);
  // LLVM IR is a production rewrite surface: a check can find a counterexample
  // but only a derivation can authorize replacing the program.
  if (!Res.Changed || Res.Evidence != sym::MBAEvidence::Derivation)
    return false;

  llvm::SmallVector<llvm::Instruction *, 32> NewInsts;
  llvm::Value *After = Xlat.out(Res.Expr, Root, NewInsts);

  // Nothing built here has an external use yet.  Delete every instruction the
  // attempt inserted, in reverse construction order, rather than only walking
  // from After: a failed rebuild may have produced children before discovering
  // an operator it cannot spell, in which case After is null and there is no
  // root from which a recursive deletion could reach them.
  auto abandon = [&]() {
    for (llvm::Instruction *I : llvm::reverse(NewInsts))
      if (I->use_empty())
        I->eraseFromParent();
    return false;
  };

  if (!After || After == Root || After->getType() != Root->getType())
    return abandon();

  // Judge the rewrite as instructions rather than as the tree that was
  // measured.  The two disagree in both directions: the folder answers part of
  // the rebuild with constants, the way back reuses values the function already
  // computes, and a measured cost prices a shape rather than what lands in the
  // block.  The instruction count is what a reader is handed, so it is what has
  // to improve -- and checking it here, before any use is moved, is what makes
  // "never hand back more than we were given" a property rather than a hope.
  if (NewInsts.size() + kMinGain > Xlat.descendedInsts())
    return abandon();

  Root->replaceAllUsesWith(After);
  Dead.push_back(Root);
  return true;
}

//===----------------------------------------------------------------------===//
// Branches that were never a choice
//===----------------------------------------------------------------------===//

/// Replace \p Br's condition with the constant it always was, if it is one.
///
/// An opaque predicate is a branch built so that one side is unreachable, put
/// there to carry code that never runs and to make the graph too large to read.
/// What makes it hard is that the condition is an identity dressed up as
/// arithmetic -- `(x | ~x) != 0`, or one MBA form of a value compared against
/// another -- so nothing that reasons about the *shape* of the condition sees
/// anything unusual, and constant propagation has no constant to propagate.
///
/// Measuring it needs no new machinery.  Carrying the comparison and its
/// operands into the engine simplifies each side, and two sides that turn out
/// to be the same expression intern to the same node, at which point the
/// comparison folds by construction rather than by a rule about comparisons.
/// Nothing here removes a block: making the condition a constant is what lets
/// the SimplifyCFG already in the pipeline see that a side is unreachable, and
/// deleting blocks is its job rather than this pass's.
bool foldOpaqueBranch(llvm::CondBrInst *Br,
                      llvm::SmallVectorImpl<llvm::WeakTrackingVH> &Dead) {
  auto *Cond = llvm::dyn_cast<llvm::Instruction>(Br->getCondition());
  if (!Cond)
    return false;

  // Only a constant is worth taking.  A condition that merely got shorter is
  // still a branch, and rewriting it would mean materializing a comparison for
  // no gain the ordinary path has not already had a chance at.
  std::optional<llvm::APInt> Value = SymSimplifyPass::constantValueOf(Cond);
  if (!Value || Value->getBitWidth() != 1)
    return false;

  Br->setCondition(llvm::ConstantInt::get(
      llvm::Type::getInt1Ty(Br->getContext()), Value->getBoolValue()));
  Dead.push_back(Cond);
  return true;
}

} // namespace

std::optional<llvm::APInt> SymSimplifyPass::constantValueOf(llvm::Value *V) {
  if (auto *Number = llvm::dyn_cast_or_null<llvm::ConstantInt>(V))
    return Number->getValue();
  // Only an instruction can be hiding a constant.  An argument or a global is
  // whatever the caller passed, and measuring one would cost a translation to
  // learn nothing.
  if (!llvm::isa_and_nonnull<llvm::Instruction>(V) ||
      !V->getType()->isIntegerTy())
    return std::nullopt;

  sym::SymContext Ctx;
  Translator Xlat(Ctx, /*CarryComparisons=*/true);
  sym::SymRef Before = Xlat.in(V);
  if (!Before.isValid())
    return std::nullopt;

  sym::MBAResult Res = sym::simplifyMBADeep(Ctx, Before);
  if (Res.Changed && Res.Evidence != sym::MBAEvidence::Derivation)
    return std::nullopt;
  return Ctx.asConst(Res.Changed ? Res.Expr : Before);
}

unsigned SymSimplifyPass::simplify(llvm::Function &F) {
  // A function the obfuscator stamped is off limits.  This pass measures away
  // mixed boolean-arithmetic, which is precisely what obfuscation injects, so
  // running it on that IR would undo the transform a patch pipeline just made.
  if (F.hasFnAttribute(kObfuscatedFnAttr))
    return 0;

  // Collect roots before mutating: rewriting one replaces its uses (including
  // any in another root's operands) in place, so a later root re-reads the
  // already-simplified operand rather than a stale one.
  llvm::SmallVector<llvm::Instruction *, 64> Roots;
  for (llvm::Instruction &I : llvm::instructions(F))
    if (isRoot(I))
      Roots.push_back(&I);

  llvm::SmallVector<llvm::WeakTrackingVH, 64> Dead;
  unsigned Rewritten = 0;
  for (llvm::Instruction *Root : Roots)
    if (rewriteRoot(Root, Dead))
      ++Rewritten;

  // Branch conditions after the expressions, so a condition is decided over
  // operands the measurement has already shortened rather than over the
  // obfuscation that was wrapped around them.
  for (llvm::BasicBlock &BB : F)
    if (auto *Br = llvm::dyn_cast<llvm::CondBrInst>(BB.getTerminator()))
      if (foldOpaqueBranch(Br, Dead))
        ++Rewritten;

  // Sweep the expression trees the rewrites made unreachable.  Doing it here
  // rather than leaving it to the InstCombine that follows in the pipeline is
  // what makes this entry point usable on its own -- the C ABI, the CLI and the
  // plugins call it directly, and none of them runs a pass manager afterwards.
  // The recursive delete stops at any operand the rebuilt value still reads, so
  // shared computation survives; the weak handles cover a root that an earlier
  // sweep already reclaimed as part of another root's dead operands.
  for (llvm::WeakTrackingVH &Handle : Dead)
    if (Handle)
      llvm::RecursivelyDeleteTriviallyDeadInstructions(
          llvm::cast<llvm::Instruction>(Handle));

  return Rewritten;
}

llvm::PreservedAnalyses SymSimplifyPass::run(llvm::Function &F,
                                             llvm::FunctionAnalysisManager &) {
  // The dead originals are swept by simplify() itself; the fresh arithmetic it
  // introduces is what the InstCombine that follows folds.
  return simplify(F) == 0 ? llvm::PreservedAnalyses::all()
                          : llvm::PreservedAnalyses::none();
}

} // namespace neverd
