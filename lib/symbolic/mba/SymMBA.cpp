//===- SymMBA.cpp - Mixed boolean-arithmetic simplification ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the measure-and-rewrite loop described in SymMBA.h: the entry
/// points, and the split of a region a constant mask has made unmeasurable
/// into mask-uniform columns.
///
/// The theory it rests on, in one paragraph.  Split each bitwise term of a
/// linear MBA into minterms — the bitwise functions that pick out the bit
/// positions where the inputs match one particular pattern.  Minterms have
/// disjoint bit supports and together cover every position, so as integers
/// they sum to the all-ones word.  Substituting the split back in and
/// collecting gives, for any linear MBA over t inputs,
///
///     e  =  sum over the 2^t patterns m of  w_m * M_m
///
/// with the weights `w_m` the only thing that distinguishes one such
/// expression from another.  Setting every input to all-zeros or all-ones puts
/// every bit position into the same pattern k, which leaves `M_k` at all-ones
/// and every other minterm at zero, so the expression evaluates to `-w_k`.
/// One evaluation per pattern therefore reads off every weight, and the size
/// the expression was written at has nothing to do with it.
///
/// The stages live next door: SymMBAAbstract.cpp decides what the measurement
/// can see, SymMBAMeasure.cpp reads the weights and writes them back out,
/// SymMBAPoly.cpp and SymMBAProduct.cpp handle what is not linear, and
/// SymMBARegion.cpp ranks the readings of one region.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymMBA.h"

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace neverd::symbolic {

using namespace detail;

namespace {

//===----------------------------------------------------------------------===//
// Regions guarded by constant masks
//===----------------------------------------------------------------------===//

/// The distinct constants that appear as an operand of an `&`, `|` or `^` at
/// \p Width.
///
/// These are exactly the constants a corner measurement cannot see past.  A
/// bitwise term is uniform at a corner — all-zeros or all-ones — which is what
/// the whole measurement depends on; a constant operand breaks that uniformity
/// because it tells one bit position from another.  The builders have already
/// folded away the all-zeros and all-ones cases, so anything left here is a
/// genuine mask.  A constant used as a summand or a coefficient is not
/// collected: it does not defeat the measurement and does not partition the
/// word.
///
/// Only masks at \p Width are collected, because they are the ones that
/// partition this region's bits: a mask inside a narrower or wider subterm
/// belongs to that subterm's own region, which the layered walk visits on its
/// own.  Mixing widths would also be a plain type error -- the column bitmasks
/// are built at \p Width and an `APInt` operation across two widths asserts.
void collectBitwiseMasks(const SymContext &Ctx, SymRef E, uint32_t Width,
                         llvm::SmallVectorImpl<llvm::APInt> &Out) {
  for (uint32_t Index : reachableInOrder(Ctx, E)) {
    SymRef R(Index);
    SymOp Op = Ctx.op(R);
    if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
      continue;
    if (Ctx.width(R) != Width)
      continue;
    for (SymRef C : Ctx.operands(R))
      if (Ctx.isConst(C)) {
        llvm::APInt V = Ctx.constValue(C);
        if (llvm::none_of(Out, [&](const llvm::APInt &S) { return S == V; }))
          Out.push_back(V);
      }
  }
}

/// Partition the bit positions of a \p Width word into the columns on which
/// every mask is constant, each returned as the set of positions it holds.
///
/// Two positions belong together when no mask tells them apart.  Splitting the
/// full word by one mask at a time reaches that partition directly, and drops
/// the empty pieces so the column count is the number of distinct signatures
/// rather than the 2^k an enumeration of signatures would suggest.
llvm::SmallVector<llvm::APInt, 8>
maskColumns(uint32_t Width, llvm::ArrayRef<llvm::APInt> Masks) {
  llvm::SmallVector<llvm::APInt, 8> Cols;
  Cols.push_back(llvm::APInt::getAllOnes(Width));
  for (const llvm::APInt &M : Masks) {
    llvm::SmallVector<llvm::APInt, 8> Next;
    for (const llvm::APInt &Col : Cols) {
      llvm::APInt In = Col & M;
      llvm::APInt Out = Col & ~M;
      if (!In.isZero())
        Next.push_back(std::move(In));
      if (!Out.isZero())
        Next.push_back(std::move(Out));
    }
    Cols = std::move(Next);
  }
  return Cols;
}

/// Rewrite \p E as it behaves on the positions in \p ColMask.
///
/// Every collected mask is constant across the column, so inside a bitwise
/// operator it is all-ones (drop it) or all-zeros (kill the term) — which the
/// builders fold — and the result no longer distinguishes bit positions.  What
/// comes out is a mask-free expression that equals \p E on the column's bits.
SymRef restrictToColumn(SymContext &Ctx, SymRef E, const llvm::APInt &ColMask,
                        llvm::ArrayRef<llvm::APInt> Masks) {
  llvm::DenseMap<uint32_t, SymRef> Done;
  for (uint32_t Index : reachableInOrder(Ctx, E)) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ops.empty()) {
      Done[Index] = R;
      continue;
    }
    SymOp Op = Ctx.op(R);
    const bool Bitwise =
        Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor;
    llvm::SmallVector<SymRef, 8> NewOps;
    NewOps.reserve(Ops.size());
    // Masks were collected at the column's width, so a bitwise node at another
    // width holds none of them; leaving it alone also keeps the width-matched
    // APInt comparisons below from asserting.
    const bool SameWidth = Ctx.width(R) == ColMask.getBitWidth();
    for (SymRef C : Ops) {
      SymRef NC = Done.lookup(C.index());
      if (Bitwise && SameWidth && Ctx.isConst(C) &&
          llvm::any_of(Masks, [&](const llvm::APInt &M) {
            return M == Ctx.constValue(C);
          })) {
        // Uniform on the column by construction, so this is exact rather than a
        // choice: the mask either covers every position the column holds or
        // none of them.
        bool AllSet = (Ctx.constValue(C) & ColMask) == ColMask;
        NC = AllSet ? Ctx.mkOnes(Ctx.width(R)) : Ctx.mkZero(Ctx.width(R));
      }
      NewOps.push_back(NC);
    }
    Done[Index] = Ctx.rebuild(R, NewOps);
  }
  return Done.lookup(E.index());
}

/// Prove that changing bits outside one mask column cannot affect bits inside
/// it before the root is reached.
///
/// Replacing a constant mask by zero or all-ones is exact at the positions of
/// one column.  The replacement may differ elsewhere, though, and arithmetic
/// above that mask could carry the difference back into the column.  A path
/// made only of whole-word bitwise operators cannot: each output bit depends
/// only on the input bits at the same position.  Requiring every path from a
/// collected mask to the root to have that form makes column reassembly a
/// derivation rather than a sample-backed guess.
bool maskColumnsAreIndependent(const SymContext &Ctx, SymRef E, uint32_t Width,
                               llvm::ArrayRef<llvm::APInt> Masks) {
  std::vector<uint32_t> Order = reachableInOrder(Ctx, E);
  llvm::DenseMap<uint32_t, bool> SafeToRoot;
  SafeToRoot[E.index()] = true;

  for (auto It = Order.rbegin(); It != Order.rend(); ++It) {
    SymRef R(*It);
    const bool Safe = SafeToRoot.lookup(R.index());
    const SymOp Op = Ctx.op(R);

    bool HoldsMask = false;
    if (Ctx.width(R) == Width && isBitwise(Op))
      for (SymRef C : Ctx.operands(R))
        if (Ctx.isConst(C) && llvm::any_of(Masks, [&](const llvm::APInt &M) {
              return M == Ctx.constValue(C);
            })) {
          HoldsMask = true;
          break;
        }
    if (HoldsMask && !Safe)
      return false;

    const bool ChildSafe = Safe && Ctx.width(R) == Width && isBitwise(Op);
    for (SymRef C : Ctx.operands(R)) {
      auto [Pos, Inserted] = SafeToRoot.try_emplace(C.index(), ChildSafe);
      if (!Inserted)
        Pos->second &= ChildSafe;
    }
  }
  return true;
}

/// Solve a region a constant mask has made unmeasurable, by measuring each
/// mask-uniform column of the word on its own.
///
/// A mask defeats the corner measurement, but only across a column boundary:
/// on the positions where every mask is constant the expression is an ordinary
/// linear MBA, and there it can be measured.  Solving each column and keeping
/// the bits it owns reassembles the whole — which is what recovers, say,
/// `((x ^ y) + 2 * (x & y)) & 0xff` as `(x + y) & 0xff`.
///
/// The split is exact only when no arithmetic carry crosses a column boundary.
/// A low mask over a sum discards the carry it would have produced, so the
/// common case holds, but two summands masked to the same nibble do not: the
/// carry between them lands in a position the split has already decided.  The
/// path from every mask to the root is therefore proved to contain only
/// position-wise bitwise operators before any column is measured.
SymRef solveMasked(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                   WorkBudget &Budget, SolveReport &Rep) {
  const uint32_t Width = Ctx.width(E);
  llvm::SmallVector<llvm::APInt, 8> Masks;
  collectBitwiseMasks(Ctx, E, Width, Masks);
  if (Masks.empty())
    return E;
  if (!maskColumnsAreIndependent(Ctx, E, Width, Masks))
    return E;

  llvm::SmallVector<llvm::APInt, 8> Cols = maskColumns(Width, Masks);
  if (Cols.size() < 2)
    return E;

  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Cols.size());
  unsigned Widest = 0;
  bool AnySolved = false;
  for (const llvm::APInt &Col : Cols) {
    SymRef Ecol = restrictToColumn(Ctx, E, Col, Masks);
    SolveReport ColRep;
    SymRef Scol = solveOneRegion(Ctx, Ecol, Opts, Budget, ColRep);
    Rep.BudgetExhausted |= ColRep.BudgetExhausted;
    if (Scol != Ecol) {
      AnySolved = true;
      Widest = std::max(Widest, ColRep.NumAtoms);
    }
    // Clip each column to the bits it owns.  The columns are disjoint, so the
    // clipped parts share no bit and combining them with `|` is exact.
    Parts.push_back(Ctx.mkAnd(Scol, Ctx.mkConst(Col)));
  }
  if (!AnySolved)
    return E;

  SymRef Rebuilt = Ctx.mkOr(Parts);
  if (Rebuilt == E)
    return E;

  // Independence above made the split exact.  Sampling remains a defect net
  // for the implementation of the derivation, never the reason to accept it.
  bool Verified = agreeOnSamples(Ctx, E, Rebuilt, Opts.VerifySamples);
  assert(Verified && "a proved mask split disagreed with what it replaces");
  if (!Verified)
    return E;
  if (!Opts.AllowGrowth && readingCost(Ctx, Rebuilt) > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

/// Measure \p E as one region, splitting a wide one into independent parts and
/// a masked one into mask-uniform columns.
SymRef solveRegionOrSplit(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          WorkBudget &Budget, SolveReport &Rep) {
  SymRef Solved = solveOneRegion(Ctx, E, Opts, Budget, Rep);
  return Solved == E ? solveMasked(Ctx, E, Opts, Budget, Rep) : Solved;
}

} // namespace

const char *mbaOutcomeName(MBAOutcome Outcome) {
  switch (Outcome) {
  case MBAOutcome::NotApplicable:
    return "not-applicable";
  case MBAOutcome::AlreadyShortest:
    return "already-shortest";
  case MBAOutcome::TooManyInputs:
    return "too-many-inputs";
  case MBAOutcome::BudgetExhausted:
    return "budget-exhausted";
  case MBAOutcome::Rewritten:
    return "rewritten";
  }
  llvm_unreachable("unhandled MBA outcome");
}

const char *mbaEvidenceName(MBAEvidence Evidence) {
  switch (Evidence) {
  case MBAEvidence::None:
    return "none";
  case MBAEvidence::Derivation:
    return "derivation";
  case MBAEvidence::Samples:
    return "samples";
  }
  llvm_unreachable("unhandled MBA evidence");
}

MBAResult simplifyMBA(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;

  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;
  WorkBudget Budget(Opts.MaxWork);
  if (!Budget.consume(Ctx.dagSize(E))) {
    Result.Work = Budget.used();
    Result.Outcome = MBAOutcome::BudgetExhausted;
    return Result;
  }

  SolveReport Rep;
  Result.Expr = solveRegionOrSplit(Ctx, E, Opts, Budget, Rep);
  Result.Work = Budget.used();
  Result.NumAtoms = Rep.NumAtoms;
  Result.Outcome = Rep.Outcome;
  Result.Evidence = Rep.Evidence;
  if (Result.Expr == E)
    return Result;

  Result.SizeAfter = readingCost(Ctx, Result.Expr);
  Result.Changed = true;
  return Result;
}

MBAResult simplifyMBADeep(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;
  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;

  // Walking children before parents means that by the time a node is reached,
  // everything below it has already been shortened, so each layer is measured
  // over the result of the one beneath rather than over the obfuscation that
  // was wrapped around it.  It also keeps the walk iterative: obfuscated
  // expressions nest deeply enough that recursion is a real hazard.
  const std::vector<uint32_t> Order = reachableInOrder(Ctx, E);
  llvm::DenseMap<uint32_t, SymRef> Solved;
  WorkBudget Budget(Opts.MaxWork);
  bool Skipped = false;
  // The weakest evidence any layer rested on is what the whole answer rests on,
  // because the layers above were measured over what it produced.
  bool AnySampled = false;

  for (uint32_t Index : Order) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);

    SymRef Rebuilt = R;
    if (!Ops.empty()) {
      llvm::SmallVector<SymRef, 8> NewOps;
      NewOps.reserve(Ops.size());
      bool Changed = false;
      for (SymRef C : Ops) {
        SymRef S = Solved.lookup(C.index());
        Changed |= S != C;
        NewOps.push_back(S);
      }
      if (Changed)
        Rebuilt = Ctx.rebuild(R, NewOps);
    }

    if (!canMeasureAtRoot(Ctx, Rebuilt)) {
      Solved[Index] = Rebuilt;
      continue;
    }

    // Do not compute a subtree size after the budget has gone: the argument to
    // consume() would otherwise repeat the very traversal the exhausted budget
    // is meant to stop paying for at every remaining node.
    if (!Budget.exhausted() && Budget.consume(Ctx.dagSize(Rebuilt))) {
      SolveReport Rep;
      SymRef Measured = solveRegionOrSplit(Ctx, Rebuilt, Opts, Budget, Rep);
      if (Measured != Rebuilt) {
        Rebuilt = Measured;
        Result.NumAtoms = std::max(Result.NumAtoms, Rep.NumAtoms);
        AnySampled |= Rep.Evidence == MBAEvidence::Samples;
      } else if (Rep.Outcome == MBAOutcome::BudgetExhausted) {
        Skipped = true;
      } else if (Rep.Outcome == MBAOutcome::TooManyInputs &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::TooManyInputs;
      } else if (Rep.Outcome == MBAOutcome::AlreadyShortest &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::AlreadyShortest;
      }
    } else {
      Skipped = true;
    }
    Solved[Index] = Rebuilt;
  }

  Result.Work = Budget.used();
  SymRef Out = Solved.lookup(E.index());
  if (Out == E) {
    // Running out of budget with regions left unvisited is the one refusal that
    // says nothing about the expression, so it outranks the others.
    if (Skipped)
      Result.Outcome = MBAOutcome::BudgetExhausted;
    return Result;
  }

  Result.Expr = Out;
  Result.SizeAfter = readingCost(Ctx, Out);
  Result.Changed = true;
  Result.Outcome = MBAOutcome::Rewritten;
  Result.Evidence = AnySampled ? MBAEvidence::Samples : MBAEvidence::Derivation;
  return Result;
}

} // namespace neverd::symbolic
