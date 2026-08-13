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
/// The LLVM IR <-> engine translation this pass drives lives in
/// SymSimplifyTranslator.cpp (see SymSimplifyDetail.h).
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "SymSimplifyDetail.h"

#include "neverd/symbolic/SymMBA.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
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
