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
/// a subterm the obfuscator shares is measured as one input on every side.
/// Every opaque instruction input must remain in the result, and rebuilding
/// reuses it rather than duplicating or erasing computation the CFG already
/// has.
///
/// The LLVM IR <-> engine translation this pass drives lives in
/// SymSimplifyTranslator.cpp (see SymSimplifyDetail.h).
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "SymSimplifyDetail.h"

#include "neverd/symbolic/SymMBA.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace neverd {

const char *symSimplifyOutcomeName(SymSimplifyOutcome Outcome) {
  switch (Outcome) {
  case SymSimplifyOutcome::NotApplicable:
    return "not-applicable";
  case SymSimplifyOutcome::AlreadyShortest:
    return "already-shortest";
  case SymSimplifyOutcome::TooManyInputs:
    return "too-many-inputs";
  case SymSimplifyOutcome::SearchBudgetExhausted:
    return "search-budget-exhausted";
  case SymSimplifyOutcome::Counterexample:
    return "counterexample";
  case SymSimplifyOutcome::ProofIncomplete:
    return "proof-incomplete";
  case SymSimplifyOutcome::Rewritten:
    return "rewritten";
  }
  llvm_unreachable("unhandled semantic simplification outcome");
}

std::string SymSimplifyCounterexample::toJson() const {
  std::vector<const SymSimplifyCounterexampleValue *> Ordered;
  Ordered.reserve(Variables.size());
  for (const SymSimplifyCounterexampleValue &Variable : Variables)
    Ordered.push_back(&Variable);
  llvm::sort(Ordered, [](const auto *Left, const auto *Right) {
    if (Left->Id != Right->Id)
      return Left->Id < Right->Id;
    return Left->Name < Right->Name;
  });

  std::string Text;
  llvm::raw_string_ostream OS(Text);
  {
    // The streaming writer preserves this frozen field order while applying
    // the same escaping rules as materialized llvm::json values.
    llvm::json::OStream JSON(OS);
    JSON.object([&] {
      JSON.attribute("candidate", Candidate);
      JSON.attributeArray("variables", [&] {
        for (const SymSimplifyCounterexampleValue *Variable : Ordered) {
          JSON.object([&] {
            JSON.attribute("id", static_cast<int64_t>(Variable->Id));
            JSON.attribute("name", Variable->Name);
            JSON.attribute("width", static_cast<int64_t>(Variable->Width));
            JSON.attribute("value", Variable->HexValue);
          });
        }
      });
    });
  }
  return OS.str();
}

namespace {

namespace sym = neverd::symbolic;

void addSaturating(uint64_t &Total, uint64_t Delta) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  Total = Delta > Max - Total ? Max : Total + Delta;
}

uint64_t workCount(size_t Work) {
  if constexpr (sizeof(size_t) > sizeof(uint64_t))
    if (Work > std::numeric_limits<uint64_t>::max())
      return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(Work);
}

void addProofStats(solver::ProofStats &Total, const solver::ProofStats &Delta) {
  addSaturating(Total.Queries, Delta.Queries);
  addSaturating(Total.Conflicts, Delta.Conflicts);
  addSaturating(Total.Propagations, Delta.Propagations);
  addSaturating(Total.WatchVisits, Delta.WatchVisits);
}

std::string fixedWidthHex(const llvm::APInt &Value, uint32_t Width) {
  llvm::APInt Normalized = Value.zextOrTrunc(Width);
  llvm::SmallString<64> Digits;
  Normalized.toString(Digits, 16, /*Signed=*/false,
                      /*formatAsCLiteral=*/false, /*UpperCase=*/false);
  const size_t Required = (static_cast<size_t>(Width) + 3) / 4;

  std::string Text = "0x";
  if (Digits.size() < Required)
    Text.append(Required - Digits.size(), '0');
  Text.append(Digits.begin(), Digits.end());
  return Text;
}

std::optional<SymSimplifyCounterexample>
makeCounterexample(const sym::SymContext &Ctx,
                   const solver::SymSynthProofReport &Report) {
  if (Report.Proof != solver::ProofStatus::Different ||
      !Report.RejectedCandidate.isValid() || !Report.Counterexample.has_value())
    return std::nullopt;

  SymSimplifyCounterexample Result;
  Result.Candidate = Ctx.toString(Report.RejectedCandidate);
  for (uint32_t Id : Report.Counterexample->vars()) {
    if (Id >= Ctx.numVars())
      continue;
    const sym::SymVarInfo &Info = Ctx.varInfo(Id);
    std::optional<llvm::APInt> Value = Report.Counterexample->value(Id);
    if (!Value)
      continue;
    Result.Variables.push_back(
        {Id, Info.Width, Info.Name, fixedWidthHex(*Value, Info.Width)});
  }
  llvm::sort(Result.Variables, [](const auto &Left, const auto &Right) {
    return Left.Id < Right.Id;
  });
  return Result;
}

unsigned outcomePriority(SymSimplifyOutcome Outcome) {
  switch (Outcome) {
  case SymSimplifyOutcome::NotApplicable:
    return 0;
  case SymSimplifyOutcome::AlreadyShortest:
    return 1;
  case SymSimplifyOutcome::Rewritten:
    return 2;
  case SymSimplifyOutcome::Counterexample:
    return 3;
  case SymSimplifyOutcome::TooManyInputs:
    return 4;
  case SymSimplifyOutcome::SearchBudgetExhausted:
    return 5;
  case SymSimplifyOutcome::ProofIncomplete:
    return 6;
  }
  llvm_unreachable("unhandled semantic simplification outcome");
}

void mergeResult(SymSimplifyResult &Total, SymSimplifyResult Part) {
  addSaturating(Total.Rewrites, Part.Rewrites);
  addSaturating(Total.SearchWork, Part.SearchWork);
  addProofStats(Total.ProofWork, Part.ProofWork);

  // A proof disposition is one record: its outcome, status, and optional model
  // must all describe the same deterministic root.  A later Equivalent or
  // Unknown invalidates an earlier refutation; consecutive Different answers
  // retain the first concrete model as the stable public witness.
  if (Part.Proof != solver::ProofStatus::NotRun) {
    Total.Outcome = Part.Outcome;
    Total.Proof = Part.Proof;
    if (Part.Proof == solver::ProofStatus::Different) {
      if (!Total.Counterexample && Part.Counterexample)
        Total.Counterexample = std::move(Part.Counterexample);
    } else {
      Total.Counterexample.reset();
    }
    return;
  }

  // A root that made no proof attempt cannot replace an existing proof with a
  // status from a different operation.  When no proof exists, retain the most
  // informative non-proof disposition across derivational roots.
  if (Total.Proof == solver::ProofStatus::NotRun &&
      outcomePriority(Part.Outcome) > outcomePriority(Total.Outcome))
    Total.Outcome = Part.Outcome;
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

/// Materialize an already-authorized candidate when it is a profitable LLVM
/// spelling.  No use is moved until every check succeeds, so a declined
/// candidate leaves the original IR byte-for-byte unchanged.
bool materializeCandidate(llvm::Instruction *Root, Translator &Xlat,
                          sym::SymRef Candidate, const SymSimplifyOptions &Opts,
                          llvm::SmallVectorImpl<llvm::WeakTrackingVH> &Dead) {
  if (!Xlat.retainsOpaqueInstructionLeaves(Candidate) ||
      !Xlat.hasCompatibleResultSemantics(Candidate))
    return false;

  llvm::SmallVector<llvm::Instruction *, 32> NewInsts;
  llvm::Value *After = Xlat.out(Candidate, Root, NewInsts);

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
  // block.  The instruction count is what a reader is handed, so it is what the
  // threshold prices -- and checking it here, before any use is moved, is what
  // makes "never hand back more than we were given" a property rather than a
  // hope, at every setting of that threshold.
  if (NewInsts.size() + Opts.MinInstructionsSaved > Xlat.descendedInsts())
    return abandon();

  Root->replaceAllUsesWith(After);
  Dead.push_back(Root);
  return true;
}

SymSimplifyOutcome synthesisOutcome(sym::SynthOutcome Outcome) {
  switch (Outcome) {
  case sym::SynthOutcome::NotApplicable:
    return SymSimplifyOutcome::NotApplicable;
  case sym::SynthOutcome::AlreadyShortest:
    return SymSimplifyOutcome::AlreadyShortest;
  case sym::SynthOutcome::TooManyInputs:
    return SymSimplifyOutcome::TooManyInputs;
  case sym::SynthOutcome::BudgetExhausted:
    return SymSimplifyOutcome::SearchBudgetExhausted;
  case sym::SynthOutcome::Counterexample:
    return SymSimplifyOutcome::Counterexample;
  case sym::SynthOutcome::ProofIncomplete:
    return SymSimplifyOutcome::ProofIncomplete;
  case sym::SynthOutcome::Synthesized:
    return SymSimplifyOutcome::ProofIncomplete;
  }
  llvm_unreachable("unhandled synthesis outcome");
}

solver::ProofStatus finalProofStatus(const sym::SynthResult &Result) {
  switch (Result.Outcome) {
  case sym::SynthOutcome::Synthesized:
    return Result.Verification == sym::SynthVerification::Equivalent
               ? solver::ProofStatus::Equivalent
               : solver::ProofStatus::Unknown;
  case sym::SynthOutcome::Counterexample:
    return solver::ProofStatus::Different;
  case sym::SynthOutcome::ProofIncomplete:
    return solver::ProofStatus::Unknown;
  case sym::SynthOutcome::NotApplicable:
  case sym::SynthOutcome::AlreadyShortest:
  case sym::SynthOutcome::TooManyInputs:
  case sym::SynthOutcome::BudgetExhausted:
    return solver::ProofStatus::NotRun;
  }
  llvm_unreachable("unhandled synthesis outcome");
}

/// Measure the tree at \p Root, then search only when derivation did not
/// produce a usable LLVM rewrite.  Search proposes a candidate; the selected
/// proof provider is the only authority that can let it reach IR.
SymSimplifyResult
rewriteRoot(llvm::Instruction *Root, const SymSimplifyOptions &Opts,
            llvm::SmallVectorImpl<llvm::WeakTrackingVH> &Dead) {
  SymSimplifyResult Result;
  sym::SymContext Ctx;
  Translator Xlat(Ctx);
  const sym::SymRef Before = Xlat.in(Root);
  if (!Before.isValid() || Ctx.dagSize(Before) < Opts.MinMeasuredNodes)
    return Result;

  Result.Outcome = SymSimplifyOutcome::AlreadyShortest;

  // The derivational engine runs first.  Nesting is not a budget: its deep
  // walk is iterative and visits the finite DAG without a recursion cutoff.
  const sym::MBAResult Derived = sym::simplifyMBADeep(Ctx, Before, Opts.MBA);
  if (Derived.Changed && Derived.Evidence == sym::MBAEvidence::Derivation &&
      materializeCandidate(Root, Xlat, Derived.Expr, Opts, Dead)) {
    Result.Rewrites = 1;
    Result.Outcome = SymSimplifyOutcome::Rewritten;
    return Result;
  }

  if (!Opts.EnableSynthesis)
    return Result;

  sym::SynthResult Synth;
  std::optional<solver::SymSynthProofReport> BuiltInReport;
  switch (Opts.Provider) {
  case ProofProvider::BuiltInSolver: {
    solver::SymSynthVerifier Verifier(Opts.Solver);
    auto Verify = [&](sym::SymContext &VerifyCtx, sym::SymRef Original,
                      sym::SymRef Candidate) {
      return Verifier(VerifyCtx, Original, Candidate);
    };
    Synth = sym::synthesize(Ctx, Before, Opts.Synthesis, Verify);
    BuiltInReport = Verifier.report();
    Result.ProofWork = BuiltInReport->Stats;
    break;
  }
  case ProofProvider::Disabled:
    Synth = sym::discoverSynthesisCandidate(Ctx, Before, Opts.Synthesis);
    break;
  case ProofProvider::Callback:
    if (Opts.ProofCallback) {
      auto Verify = [&](sym::SymContext &VerifyCtx, sym::SymRef Original,
                        sym::SymRef Candidate) {
        return Opts.ProofCallback(VerifyCtx, Original, Candidate);
      };
      Synth = sym::synthesize(Ctx, Before, Opts.Synthesis, Verify);
      Result.ProofWork.Queries = Synth.ProofQueries;
    } else {
      Synth = sym::discoverSynthesisCandidate(Ctx, Before, Opts.Synthesis);
    }
    break;
  }

  Result.SearchWork = workCount(Synth.Work);
  Result.Outcome = synthesisOutcome(Synth.Outcome);
  Result.Proof = finalProofStatus(Synth);
  // SynthVerification is the compatibility result consumed by the search and
  // cannot spell a malformed proof request.  The built-in verifier's typed
  // report is authoritative for public telemetry, so preserve Invalid here
  // instead of collapsing it into the resource-only Unknown disposition.
  if (BuiltInReport && BuiltInReport->Proof != solver::ProofStatus::NotRun)
    Result.Proof = BuiltInReport->Proof;

  // A disabled or empty provider explicitly reports an incomplete proof when
  // search found a sample-backed candidate.  It performs no proof query.
  if ((Opts.Provider == ProofProvider::Disabled ||
       (Opts.Provider == ProofProvider::Callback && !Opts.ProofCallback)) &&
      Synth.Outcome == sym::SynthOutcome::Synthesized) {
    Result.Outcome = SymSimplifyOutcome::ProofIncomplete;
    Result.Proof = solver::ProofStatus::Unknown;
  }

  if (Synth.Outcome == sym::SynthOutcome::Counterexample && BuiltInReport) {
    Result.Counterexample = makeCounterexample(Ctx, *BuiltInReport);
  }

  // Explicitly require verifier evidence even though the symbolic API already
  // fails closed.  This keeps the production boundary self-documenting and
  // makes future search modes unable to materialize sample-only evidence by
  // accident.
  if (!Synth.Changed || Synth.Outcome != sym::SynthOutcome::Synthesized ||
      Synth.Evidence != sym::SynthEvidence::Verifier ||
      Synth.Verification != sym::SynthVerification::Equivalent)
    return Result;

  Result.Proof = solver::ProofStatus::Equivalent;
  Result.Counterexample.reset();
  if (!materializeCandidate(Root, Xlat, Synth.Expr, Opts, Dead)) {
    Result.Outcome = SymSimplifyOutcome::AlreadyShortest;
    return Result;
  }

  Result.Rewrites = 1;
  Result.Outcome = SymSimplifyOutcome::Rewritten;
  return Result;
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
std::optional<llvm::APInt>
constantValueWithOptions(llvm::Value *V, const sym::MBAOptions &Opts) {
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
  const sym::SymRef Before = Xlat.in(V);
  if (!Before.isValid())
    return std::nullopt;

  const sym::MBAResult Res = sym::simplifyMBADeep(Ctx, Before, Opts);
  if (Res.Changed && Res.Evidence != sym::MBAEvidence::Derivation)
    return std::nullopt;
  const sym::SymRef Result = Res.Changed ? Res.Expr : Before;
  if (!Xlat.retainsOpaqueInstructionLeaves(Result) ||
      !Xlat.hasCompatibleResultSemantics(Result))
    return std::nullopt;
  return Ctx.asConst(Result);
}

bool foldOpaqueBranch(llvm::CondBrInst *Br, const sym::MBAOptions &Opts,
                      llvm::SmallVectorImpl<llvm::WeakTrackingVH> &Dead) {
  auto *Cond = llvm::dyn_cast<llvm::Instruction>(Br->getCondition());
  if (!Cond)
    return false;

  // Only a constant is worth taking.  A condition that merely got shorter is
  // still a branch, and rewriting it would mean materializing a comparison for
  // no gain the ordinary path has not already had a chance at.
  std::optional<llvm::APInt> Value = constantValueWithOptions(Cond, Opts);
  if (!Value || Value->getBitWidth() != 1)
    return false;

  Br->setCondition(llvm::ConstantInt::get(
      llvm::Type::getInt1Ty(Br->getContext()), Value->getBoolValue()));
  Dead.push_back(Cond);
  return true;
}

} // namespace

std::optional<llvm::APInt> SymSimplifyPass::constantValueOf(llvm::Value *V) {
  return constantValueWithOptions(V, sym::MBAOptions());
}

SymSimplifyResult SymSimplifyPass::simplifyWithResult(llvm::Function &F,
                                                      SymSimplifyOptions Opts) {
  SymSimplifyResult Result;
  // A function the obfuscator stamped is off limits.  This pass measures away
  // mixed boolean-arithmetic, which is precisely what obfuscation injects, so
  // running it on that IR would undo the transform a patch pipeline just made.
  if (F.hasFnAttribute(kObfuscatedFnAttr))
    return Result;

  // Collect roots before mutating: rewriting one replaces its uses (including
  // any in another root's operands) in place, so a later root re-reads the
  // already-simplified operand rather than a stale one.
  llvm::SmallVector<llvm::Instruction *, 64> Roots;
  for (llvm::Instruction &I : llvm::instructions(F))
    if (isRoot(I))
      Roots.push_back(&I);

  llvm::SmallVector<llvm::WeakTrackingVH, 64> Dead;
  for (llvm::Instruction *Root : Roots)
    mergeResult(Result, rewriteRoot(Root, Opts, Dead));

  // Branch conditions after the expressions, so a condition is decided over
  // operands the measurement has already shortened rather than over the
  // obfuscation that was wrapped around them.
  for (llvm::BasicBlock &BB : F)
    if (auto *Br = llvm::dyn_cast<llvm::CondBrInst>(BB.getTerminator()))
      if (foldOpaqueBranch(Br, Opts.MBA, Dead)) {
        SymSimplifyResult Folded;
        Folded.Rewrites = 1;
        Folded.Outcome = SymSimplifyOutcome::Rewritten;
        mergeResult(Result, std::move(Folded));
      }

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

  return Result;
}

unsigned SymSimplifyPass::simplify(llvm::Function &F, SymSimplifyOptions Opts) {
  const uint64_t Rewrites = simplifyWithResult(F, std::move(Opts)).Rewrites;
  return Rewrites > std::numeric_limits<unsigned>::max()
             ? std::numeric_limits<unsigned>::max()
             : static_cast<unsigned>(Rewrites);
}

llvm::PreservedAnalyses SymSimplifyPass::run(llvm::Function &F,
                                             llvm::FunctionAnalysisManager &) {
  // The dead originals are swept by simplify() itself; the fresh arithmetic it
  // introduces is what the InstCombine that follows folds.
  return simplify(F, Opts) == 0 ? llvm::PreservedAnalyses::all()
                                : llvm::PreservedAnalyses::none();
}

} // namespace neverd
