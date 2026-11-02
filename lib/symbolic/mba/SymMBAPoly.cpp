//===- SymMBAPoly.cpp - Expanding an MBA over the minterm basis -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the symbolic expansion of an expression into coefficients over
/// the minterm basis, and the two exact verifiers built on it.  The product
/// search that consumes the higher-degree coefficients lives in
/// SymMBAProduct.cpp.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <vector>

//===----------------------------------------------------------------------===//
// Products
//===----------------------------------------------------------------------===//
//
// A product of bitwise terms cannot be measured the way a linear MBA can.  At a
// corner every bitwise term is all-zeros or all-ones, so `B * B` and `-B` agree
// at all 2^t of them and disagree everywhere else: the corner values no longer
// pin the expression down.
//
// They do not have to.  Writing each factor as the sum of the minterms it
// selects and multiplying out gives, for any polynomial in bitwise terms of t
// inputs,
//
//     e  =  sum_m w_m M_m  +  sum over monomials M_{m1} * ... * M_{md} of c *
//     ..
//
// and every coefficient can be computed *symbolically* — each factor's truth
// table is one corner sweep of that factor alone, and the rest is bookkeeping.
// No system has to be solved and nothing is guessed.
//
// What that buys is the identity underneath polynomial MBA obfuscation.  From
// the disjoint splits `x = (x & y) + (x & ~y)` and `y = (x & y) + (~x & y)`,
// multiplying out gives
//
//     x * y  =  (x & y) * (x | y)  +  (x & ~y) * (~x & y)
//
// and the right-hand side expands to exactly the coefficients of the left.  So
// searching the small products for one whose expansion matches is enough to
// recognise it, however it was written.  The same argument runs at any degree,
// which is what reaches `x * y * z` and the cubes above it: only the arity of
// the monomials changes, not the reasoning.

namespace neverd::symbolic::detail {

namespace {

/// Accumulate the terms of `Scale * Node` into \p Out, or fail when some term
/// is of a higher degree than the expansion covers.
bool collectTerms(const SymContext &Ctx, SymRef Node, const llvm::APInt &Scale,
                  llvm::SmallVectorImpl<PolyTerm> &Out) {
  struct WorkItem {
    SymRef Node;
    llvm::APInt Scale;
  };
  llvm::SmallVector<WorkItem, 32> Work{{Node, Scale}};

  while (!Work.empty()) {
    WorkItem Item = Work.pop_back_val();
    SymRef Current = Item.Node;

    if (Ctx.op(Current) == SymOp::Add) {
      llvm::ArrayRef<SymRef> Summands = Ctx.operands(Current);
      // Preserve the recursive walk's stable left-to-right term order.
      for (auto It = Summands.rbegin(); It != Summands.rend(); ++It)
        Work.push_back({*It, Item.Scale});
      continue;
    }

    if (Ctx.isConst(Current)) {
      Out.push_back(PolyTerm{Item.Scale * Ctx.constValue(Current), {}});
      continue;
    }

    if (Ctx.op(Current) == SymOp::Mul) {
      llvm::ArrayRef<SymRef> Ops = Ctx.operands(Current);
      llvm::APInt Coeff = Item.Scale;
      unsigned First = 0;
      if (Ctx.isConst(Ops[0])) {
        Coeff *= Ctx.constValue(Ops[0]);
        First = 1;
      }
      llvm::ArrayRef<SymRef> Factors = Ops.drop_front(First);
      // A scaled sum has to be distributed rather than treated as one factor:
      // `3 * (a + b)` is two terms, and reading it as a single factor would
      // hand the expansion something that is not a bitwise function at all.
      if (Factors.size() == 1) {
        Work.push_back({Factors[0], std::move(Coeff)});
        continue;
      }
      Out.push_back(
          PolyTerm{std::move(Coeff), {Factors.begin(), Factors.end()}});
      continue;
    }

    Out.push_back(PolyTerm{std::move(Item.Scale), {Current}});
  }
  return true;
}

/// The truth table of \p F, if \p F really is a bitwise function of the inputs.
///
/// The check is the point.  A factor that takes some third value at a corner
/// is not a bitwise function, and reading a table off it anyway would produce
/// a confidently wrong expansion.  Classification rules most of those out
/// already; this rules out the rest without having to trust it.
std::optional<TruthTable> bitwiseTable(const SymContext &Ctx, SymRef F,
                                       llvm::ArrayRef<uint32_t> AtomIds) {
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  if (!Corners)
    return std::nullopt;
  if (Ctx.isVar(F)) {
    const auto It = llvm::find(AtomIds, Ctx.varId(F));
    return It == AtomIds.end()
               ? TruthTable::zero(NumAtoms)
               : atomTruthTable(static_cast<unsigned>(It - AtomIds.begin()),
                                NumAtoms);
  }
  const unsigned Width = Ctx.width(F);
  if (NumAtoms >= limits::kMinPackedPolynomialAtoms &&
      Width >= limits::kMinPackedPolynomialWidth) {
    bool IndependentBits = true;
    for (uint32_t Index : reachableInOrder(Ctx, F)) {
      SymRef R(Index);
      SymOp Op = Ctx.op(R);
      if (Ctx.width(R) != Width ||
          (!Ctx.isVar(R) && !Ctx.isConst(R) && Op != SymOp::Not &&
           Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor) ||
          (Ctx.isConst(R) && !Ctx.constValue(R).isZero() &&
           !Ctx.constValue(R).isAllOnes())) {
        IndependentBits = false;
        break;
      }
    }
    if (IndependentBits) {
      // These operators act independently on every bit. Each bit can hold a
      // different complete corner assignment, so one ordinary word evaluation
      // computes Width table entries. Arithmetic and nonuniform literals keep
      // the scalar evaluator below; no cross-bit operation is packed.
      SymEvalPlan Plan(Ctx, F);
      const auto Vars = Plan.vars();
      llvm::SmallVector<unsigned, 8> Positions;
      for (uint32_t Id : Vars) {
        auto It = llvm::find(AtomIds, Id);
        Positions.push_back(static_cast<unsigned>(It - AtomIds.begin()));
      }
      auto Slot = [&](uint32_t Id) {
        return std::lower_bound(Vars.begin(), Vars.end(), Id) - Vars.begin();
      };
      TruthTable Table = TruthTable::zero(NumAtoms);
      llvm::SmallVector<uint64_t, 8> Narrow(Vars.size(), 0);
      llvm::SmallVector<llvm::APInt, 8> Wide;
      if (!Plan.fitsU64())
        Wide.assign(Vars.size(), llvm::APInt(Width, 0));
      for (size_t Begin = 0; Begin < *Corners;) {
        const size_t Count = std::min<size_t>(Width, *Corners - Begin);
        for (size_t I = 0; I != Vars.size(); ++I) {
          if (Plan.fitsU64())
            Narrow[I] = 0;
          else
            Wide[I].clearAllBits();
          if (Positions[I] == NumAtoms)
            continue;
          for (size_t Bit = 0; Bit != Count; ++Bit)
            if (((Begin + Bit) >> Positions[I]) & 1) {
              if (Plan.fitsU64())
                Narrow[I] |= uint64_t(1) << Bit;
              else
                Wide[I].setBit(static_cast<unsigned>(Bit));
            }
        }
        if (Plan.fitsU64()) {
          const auto Value =
              Plan.evalU64With([&](uint32_t Id) { return Narrow[Slot(Id)]; });
          for (size_t Bit = 0; Bit != Count; ++Bit)
            Table.setValue(Begin + Bit, (Value >> Bit) & 1);
        } else {
          const auto Value =
              Plan.evalWith([&](uint32_t Id) { return &Wide[Slot(Id)]; });
          for (size_t Bit = 0; Bit != Count; ++Bit)
            Table.setValue(Begin + Bit, Value[Bit]);
        }
        Begin += Count;
      }
      return Table;
    }
  }
  CornerEvaluator Evaluator(Ctx, F, AtomIds);
  TruthTable Table = TruthTable::zero(NumAtoms);
  for (size_t K = 0; K < *Corners; ++K) {
    llvm::APInt Value = Evaluator.eval(K);
    if (Value.isAllOnes())
      Table.set(K);
    else if (!Value.isZero())
      return std::nullopt;
  }
  return Table;
}

// The polynomial abstraction makes each factor a pure bitwise word over
// independent inputs. Equal complete tables therefore prove equal full words,
// and multiplication preserves that equality even for noninvertible scales.
// This is a sufficient proof only; other identities use the coefficient proof.
bool proveQuadraticCongruence(const SymContext &Ctx,
                              llvm::ArrayRef<PolyTerm> Terms,
                              llvm::ArrayRef<uint32_t> AtomIds,
                              WorkBudget &Budget, size_t MaxBytes) {
  if (AtomIds.size() < limits::kMinQuadraticCongruenceAtoms ||
      Terms.size() != 2 || Terms[0].Factors.size() != 2 ||
      Terms[1].Factors.size() != 2 || Terms[0].Coeff != -Terms[1].Coeff)
    return false;
  const auto Corners = cornerCount(AtomIds.size());
  if (!Corners || *Corners > MaxBytes / 8)
    return false;
  size_t Storage = MaxBytes - 8 * *Corners;
  size_t Work = 4 * *Corners;
  for (const auto &Term : Terms)
    for (SymRef Factor : Term.Factors) {
      const size_t Nodes = Ctx.dagSize(Factor);
      const size_t Words = (size_t(Ctx.width(Factor)) - 1) / 64 + 1;
      const size_t NodeBytes = 128 + 2 * Words * sizeof(uint64_t);
      if (Nodes > Storage / NodeBytes ||
          Nodes > std::numeric_limits<size_t>::max() - Work)
        return false;
      Storage -= Nodes * NodeBytes;
      Work += Nodes;
    }
  if (!Budget.canConsume(Work))
    return false;
  Budget.consume(Work);
  llvm::SmallVector<TruthTable, 4> Tables;
  for (const auto &Term : Terms)
    for (SymRef Factor : Term.Factors) {
      auto Table = bitwiseTable(Ctx, Factor, AtomIds);
      if (!Table)
        return false;
      Tables.push_back(std::move(*Table));
    }
  return (Tables[0] == Tables[2] && Tables[1] == Tables[3]) ||
         (Tables[0] == Tables[3] && Tables[1] == Tables[2]);
}

} // namespace

std::optional<llvm::SmallVector<PolyTerm, 8>>
splitIntoTerms(const SymContext &Ctx, SymRef Body) {
  llvm::SmallVector<PolyTerm, 8> Terms;
  if (!collectTerms(Ctx, Body, llvm::APInt(Ctx.width(Body), 1), Terms))
    return std::nullopt;
  return Terms;
}

llvm::SmallVector<SymRef, 8>
extractPolynomialCofactor(llvm::SmallVectorImpl<PolyTerm> &Terms,
                          WorkBudget &Budget, size_t MaxBytes) {
  llvm::SmallVector<SymRef, 8> Common;
  if (Terms.size() < 2 || Terms.size() > limits::kMaxPolynomialCofactorTerms)
    return Common;
  const size_t Degree = Terms.front().Factors.size();
  if (Degree < 3 || Degree > limits::kMaxPolynomialCofactorEdges / Terms.size())
    return Common;
  // Three sorted factor buffers and the retained/rebuilt terms, including
  // capacity growth. Coefficients stay in place and need no wide-word copies.
  const size_t Edges = Degree * Terms.size();
  const size_t SortWork = Degree * (std::bit_width(Degree) + 1);
  if (Edges > MaxBytes / (8 * sizeof(SymRef)) ||
      llvm::any_of(Terms, [&](const PolyTerm &Term) {
        return Term.Factors.size() != Degree;
      }))
    return Common;
  if (!Budget.consume(Degree))
    return Common;
  Common.assign(Terms.front().Factors.begin(), Terms.front().Factors.end());
  if (!llvm::is_sorted(Common)) {
    if (!Budget.consume(SortWork))
      return {};
    llvm::sort(Common);
  }
  llvm::SmallVector<SymRef, 8> Factors, Next;
  for (const PolyTerm &Term : llvm::drop_begin(Terms)) {
    if (!Budget.consume(Degree + Common.size() + Term.Factors.size()))
      return {};
    Factors.assign(Term.Factors.begin(), Term.Factors.end());
    if (!llvm::is_sorted(Factors)) {
      if (!Budget.consume(SortWork))
        return {};
      llvm::sort(Factors);
    }
    Next.clear();
    std::set_intersection(Common.begin(), Common.end(), Factors.begin(),
                          Factors.end(), std::back_inserter(Next));
    Common.swap(Next);
    if (Common.empty())
      return Common;
  }
  // Admission for the mutating phase is all-or-nothing. Canonical input
  // products are already sorted; scan once instead of re-sorting every copy.
  size_t MutationWork = 3 * Edges;
  for (const auto &Term : Terms)
    if (!llvm::is_sorted(Term.Factors))
      MutationWork += SortWork;
  if (!Budget.consume(MutationWork))
    return {};
  for (PolyTerm &Term : Terms) {
    if (!llvm::is_sorted(Term.Factors)) {
      llvm::sort(Term.Factors);
    }
    Next.clear();
    std::set_difference(Term.Factors.begin(), Term.Factors.end(),
                        Common.begin(), Common.end(), std::back_inserter(Next));
    Term.Factors.assign(Next.begin(), Next.end());
  }
  return Common;
}

Monomial makeMonomial(llvm::ArrayRef<MintermIndex> Sorted) {
  return Monomial(Sorted.begin(), Sorted.end());
}

unsigned monomialDegree(const Monomial &Key) {
  return static_cast<unsigned>(Key.size());
}

std::optional<QuadraticProduct>
recoverQuadraticTerms(const SymContext &Ctx, llvm::ArrayRef<PolyTerm> Terms,
                      llvm::ArrayRef<uint32_t> AtomIds, uint32_t Width,
                      WorkBudget &Budget, size_t MaxBytes, bool &Determined) {
  Determined = false;
  if (!Width || Terms.empty() || Terms.size() > limits::kMaxQuadraticRowTerms ||
      AtomIds.size() < limits::kMinQuadraticRowAtoms ||
      AtomIds.size() > limits::kMaxQuadraticRowAtoms)
    return std::nullopt;
  const size_t Corners = *cornerCount(AtomIds.size());
  const size_t Words = (size_t(Width) - 1) / 64 + 1;
  // Four coefficient rows, selected-index capacities, factor tables, and
  // container/APInt scratch. Reserve before constructing any wide table.
  static_assert(2 * limits::kMaxQuadraticRowTerms <= 32);
  const size_t PerCorner =
      80 + 4 * (sizeof(llvm::APInt) + Words * sizeof(uint64_t)) +
      8 * Terms.size() * sizeof(MintermIndex);
  const size_t Overhead = 1024 + 512 * Terms.size();
  if (MaxBytes < Overhead || Corners > (MaxBytes - Overhead) / PerCorner)
    return std::nullopt;
  struct Factor {
    TruthTable Table;
    llvm::SmallVector<MintermIndex, 8> Selected;
    uint32_t Mask = 0;
  };
  llvm::DenseMap<uint32_t, Factor> Factors;
  const size_t Scratch = Overhead + Corners * PerCorner;
  for (const auto &Term : Terms) {
    if (Term.Factors.size() != 2 || Term.Coeff.getBitWidth() != Width)
      return std::nullopt;
    for (SymRef F : Term.Factors) {
      if (Factors.count(F.index()))
        continue;
      const size_t Nodes = Ctx.dagSize(F);
      if (Ctx.width(F) != Width ||
          Nodes > (MaxBytes - Scratch) / (128 + 2 * Words * sizeof(uint64_t)) ||
          !Budget.consume(Nodes + Corners))
        return std::nullopt;
      auto Table = bitwiseTable(Ctx, F, AtomIds);
      if (!Table)
        return std::nullopt;
      auto Selected = selectedMinterms(*Table);
      Factors.try_emplace(F.index(), std::move(*Table), std::move(Selected));
    }
  }
  // A single term already supplies its complete rank-one product. Rebuilding
  // every coefficient only rediscovers these same factors; retain their exact
  // truth tables and coefficient. The caller independently proves the emitted
  // polynomial after synthesizing the two bitwise factors.
  if (Terms.size() == 1) {
    if (!Budget.consume(2 * Corners))
      return std::nullopt;
    const auto &Term = Terms.front();
    Determined = true;
    return QuadraticProduct{Term.Coeff,
                            {Factors.at(Term.Factors[0].index()).Table,
                             Factors.at(Term.Factors[1].index()).Table}};
  }
  // Equal source-factor memberships give identical coefficient rows. Work
  // over those classes rather than every Boolean minterm. A class represents
  // the sum of its minterms; within-class off-diagonals are twice its diagonal,
  // so checking one diagonal also checks every pair of distinct members.
  if (!Budget.consume(Corners * (Factors.size() + 1)))
    return std::nullopt;
  std::map<uint32_t, unsigned> ClassIndex;
  llvm::SmallVector<uint32_t, 16> Signatures;
  std::vector<unsigned> ClassOf(Corners);
  unsigned NextBit = 0;
  for (auto &Entry : Factors) {
    Entry.second.Mask = uint32_t(1) << NextBit++;
    Entry.second.Selected.clear();
  }
  for (size_t M = 0; M != Corners; ++M) {
    uint32_t Signature = 0;
    for (const auto &Entry : Factors)
      if (Entry.second.Table.at(M))
        Signature |= Entry.second.Mask;
    auto [It, New] = ClassIndex.try_emplace(Signature, Signatures.size());
    ClassOf[M] = It->second;
    if (New) {
      Signatures.push_back(Signature);
      for (auto &Entry : Factors)
        if (Signature & Entry.second.Mask)
          Entry.second.Selected.push_back(It->second);
    }
  }
  const size_t Rows = Signatures.size();
  auto Zero = llvm::APInt(Width, 0);
  std::vector<llvm::APInt> Diagonal(Rows, Zero), RowA(Rows, Zero),
      RowB(Rows, Zero), Row(Rows, Zero);
  auto ReadRow = [&](size_t A, std::vector<llvm::APInt> &Out,
                     size_t Lower = 0) {
    if (!Budget.consume((Rows - Lower) * Words + 2 * Terms.size()))
      return false;
    for (size_t B = Lower; B != Rows; ++B)
      Out[B].clearAllBits();
    for (const auto &Term : Terms) {
      const auto &Left = Factors.at(Term.Factors[0].index());
      const auto &Right = Factors.at(Term.Factors[1].index());
      auto Accumulate = [&](const Factor &F, bool SkipDiagonal) {
        for (auto B : F.Selected) {
          if (B < Lower || (SkipDiagonal && B == A))
            continue;
          if (!Budget.consume(Words))
            return false;
          Out[B] += Term.Coeff;
        }
        return true;
      };
      if (((Signatures[A] & Left.Mask) && !Accumulate(Right, false)) ||
          ((Signatures[A] & Right.Mask) && !Accumulate(Left, true)))
        return false;
    }
    return true;
  };
  for (const auto &Term : Terms) {
    const auto &Left = Factors.at(Term.Factors[0].index());
    const auto &Right = Factors.at(Term.Factors[1].index());
    if (!Budget.consume(Left.Selected.size() * (1 + Words)))
      return std::nullopt;
    for (auto M : Left.Selected)
      if ((Signatures[M] & Right.Mask))
        Diagonal[M] += Term.Coeff;
  }
  QuadraticProduct Result{
      Zero,
      {TruthTable::zero(AtomIds.size()), TruthTable::zero(AtomIds.size())}};
  auto &Left = Result.Factors[0];
  auto &Right = Result.Factors[1];
  std::optional<size_t> Shared, Anchor, Opposite;
  auto Impossible = [&]() -> std::optional<QuadraticProduct> {
    Determined = true;
    return std::nullopt;
  };
  if (!Budget.consume(Rows * Words))
    return std::nullopt;
  for (size_t M = 0; M != Rows; ++M) {
    if (Diagonal[M].isZero())
      continue;
    if (!Shared) {
      Shared = M;
      Result.Coefficient = Diagonal[M];
    } else if (Diagonal[M] != Result.Coefficient)
      return Impossible();
    Left.set(M);
    Right.set(M);
  }
  if (Shared) {
    if (!ReadRow(*Shared, RowA) || !Budget.consume(Rows * Words))
      return std::nullopt;
    for (size_t M = 0; M != Rows; ++M) {
      if (!Diagonal[M].isZero())
        continue;
      if (RowA[M].isZero())
        continue;
      if (RowA[M] != Result.Coefficient)
        return Impossible();
      if (!Anchor)
        Anchor = M;
    }
    if (Anchor && !ReadRow(*Anchor, RowB))
      return std::nullopt;
  } else {
    // A nonzero off-diagonal with zero diagonals fixes two disjoint factors.
    // Canceled minterms need not belong to either factor: classify from both
    // anchor rows instead of using the union of the source factor supports.
    for (size_t A = 0; A != Rows && !Anchor; ++A) {
      if (!ReadRow(A, RowA, A + 1) || !Budget.consume((Rows - A - 1) * Words))
        return std::nullopt;
      for (size_t B = A + 1; B != Rows; ++B)
        if (!RowA[B].isZero()) {
          Anchor = A;
          Opposite = B;
          Result.Coefficient = RowA[B];
          break;
        }
    }
    if (!Anchor) {
      Determined = true;
      return Result;
    }
    if (!ReadRow(*Anchor, RowA) || !ReadRow(*Opposite, RowB))
      return std::nullopt;
  }
  if (Anchor) {
    if (!Budget.consume(2 * Rows * Words))
      return std::nullopt;
    for (size_t M = 0; M != Rows; ++M) {
      if (Shared && !Diagonal[M].isZero())
        continue;
      if ((!RowA[M].isZero() && RowA[M] != Result.Coefficient) ||
          (!RowB[M].isZero() && RowB[M] != Result.Coefficient))
        return Impossible();
      if (Shared) {
        if (RowA[M].isZero() && !RowB[M].isZero())
          return Impossible();
        if (!RowA[M].isZero())
          (RowB[M].isZero() ? Left : Right).set(M);
      } else {
        if (!RowA[M].isZero() && !RowB[M].isZero())
          return Impossible();
        Left.setValue(M, !RowB[M].isZero());
        Right.setValue(M, !RowA[M].isZero());
      }
    }
  }
  // The slices propose only a candidate. Check the full coefficient matrix,
  // including missing edges, disappearing minterms, and modular zeros. Read
  // rows lazily so a counterexample avoids expanding every remaining term.
  for (size_t A = 0; A != Rows; ++A) {
    if (!ReadRow(A, Row, A) || !Budget.consume((Rows - A) * Words))
      return std::nullopt;
    for (size_t B = A; B != Rows; ++B) {
      unsigned Count = unsigned(Left.at(A) && Right.at(B));
      if (A != B)
        Count += unsigned(Left.at(B) && Right.at(A));
      auto Expected = Count == 2 ? Result.Coefficient.shl(1)
                      : Count    ? Result.Coefficient
                                 : Zero;
      if (Row[B] != Expected)
        return Impossible();
    }
  }
  if (!Budget.consume(2 * Corners))
    return std::nullopt;
  auto ClassLeft = Left, ClassRight = Right;
  for (size_t M = 0; M != Corners; ++M) {
    Left.setValue(M, ClassLeft.at(ClassOf[M]));
    Right.setValue(M, ClassRight.at(ClassOf[M]));
  }
  Determined = true;
  return Result;
}

TruthTable monomialSupport(const Monomial &Key, unsigned NumVars) {
  TruthTable Support = TruthTable::zero(NumVars);
  for (MintermIndex Index : Key)
    Support.set(Index);
  return Support;
}

llvm::SmallVector<MintermIndex, 8> selectedMinterms(const TruthTable &Table) {
  llvm::SmallVector<MintermIndex, 8> Out;
  for (size_t M = 0, E = Table.entries(); M < E; ++M)
    if (Table.at(M))
      Out.push_back(static_cast<MintermIndex>(M));
  return Out;
}

std::optional<PolyForm> expandOverMinterms(const SymContext &Ctx,
                                           llvm::ArrayRef<PolyTerm> Terms,
                                           llvm::ArrayRef<uint32_t> AtomIds,
                                           uint32_t Width, WorkBudget &Budget,
                                           bool Optional, bool SingleProduct) {
  const std::optional<size_t> CornerCount = cornerCount(AtomIds.size());
  if (!CornerCount)
    return std::nullopt;
  const size_t Corners = *CornerCount;
  PolyForm Form;
  // Products often revisit the same sorted monomial through different factor
  // choices. Keep the coefficients in a hash table through the search, since
  // neither accumulation nor product matching depends on iteration order.

  llvm::DenseMap<uint32_t, TruthTable> Cache;
  auto tableOf = [&](SymRef F) -> std::optional<TruthTable> {
    auto It = Cache.find(F.index());
    if (It != Cache.end())
      return It->second;
    std::optional<TruthTable> Table = bitwiseTable(Ctx, F, AtomIds);
    if (Table)
      Cache.try_emplace(F.index(), *Table);
    return Table;
  };

  // A failed expansion discards the entire form. Check the combined cost of
  // its products before inserting any monomials: individually affordable
  // terms can otherwise fill a large hash table only for a later term to run
  // out of work. Keep the same term order and charge the work already passed
  // when a factor is invalid, matching the expansion below.
  const bool CheckExtremes =
      SingleProduct && Corners <= 64 && !Terms.empty() &&
      Terms.size() <=
          ::neverd::symbolic::detail::limits::
              kSymMBAPolyExpandOverMintermsCheckExtremesTermsSizeLimit &&
      llvm::all_of(Terms, [&](const PolyTerm &Term) {
        return Term.Factors.size() >= 2 &&
               Term.Factors.size() <=
                   ::neverd::symbolic::detail::limits::
                       kSymMBAPolyExpandOverMintermsCheckExtremesTermFactorsSizeLimit &&
               Term.Factors.size() == Terms.front().Factors.size();
      });
  llvm::SmallVector<MintermIndex, 8> First, Last;
  llvm::APInt FirstScale(Width, 0), LastScale(Width, 0);
  std::vector<llvm::APInt> Diagonal;
  const size_t Words = (size_t(Width) - 1) / 64 + 1;
  if (CheckExtremes) {
    if (!Budget.consume(Corners * Words))
      return std::nullopt;
    Diagonal.assign(Corners, llvm::APInt(Width, 0));
  }
  if (!Budget.unlimited() || CheckExtremes) {
    size_t Pending = 0;
    for (const PolyTerm &Term : Terms) {
      if (Term.Factors.empty())
        continue;
      size_t ProductSize = 1;
      bool ZeroProduct = false;
      llvm::SmallVector<MintermIndex, 8> Low, High;
      uint64_t DiagonalBits = ~uint64_t(0);
      for (SymRef Factor : Term.Factors) {
        std::optional<TruthTable> Table = tableOf(Factor);
        if (!Table) {
          Budget.consume(Pending);
          return std::nullopt;
        }
        if (Term.Factors.size() < 2)
          continue;
        const size_t Choices = Table->count();
        if (Choices == 0) {
          ZeroProduct = true;
          continue;
        }
        if (CheckExtremes) {
          if (!Budget.consume(2))
            return std::nullopt;
          const uint64_t Bits = Table->packed();
          if (!Budget.consume())
            return std::nullopt;
          DiagonalBits &= Bits;
          Low.push_back(std::countr_zero(Bits));
          High.push_back(63u - std::countl_zero(Bits));
        }
        if (Choices > std::numeric_limits<size_t>::max() / ProductSize)
          ProductSize = std::numeric_limits<size_t>::max();
        else
          ProductSize *= Choices;
      }
      if (Term.Factors.size() < 2 || ZeroProduct)
        continue;
      if (CheckExtremes) {
        while (DiagonalBits) {
          const unsigned M = std::countr_zero(DiagonalBits);
          DiagonalBits &= DiagonalBits - 1;
          if (!Budget.consume(Words))
            return std::nullopt;
          Diagonal[M] += Term.Coeff;
        }
        if (!Budget.consume(2 * Low.size() * (std::bit_width(Low.size()) + 2) +
                            2 * ((size_t(Width) + 63) / 64)))
          return std::nullopt;
        llvm::sort(Low);
        llvm::sort(High);
        if (First.empty() || Low < First) {
          First = Low;
          FirstScale = Term.Coeff;
        } else if (Low == First) {
          FirstScale += Term.Coeff;
        }
        if (Last.empty() || Last < High) {
          Last = High;
          LastScale = Term.Coeff;
        } else if (High == Last) {
          LastScale += Term.Coeff;
        }
      }
      if (ProductSize > std::numeric_limits<size_t>::max() - Pending ||
          !Budget.canConsume(Pending + ProductSize)) {
        if (!Optional)
          Budget.consume(std::numeric_limits<size_t>::max());
        return std::nullopt;
      }
      Pending += ProductSize;
    }
  }
  // Every diagonal minterm power in a single product occurs once, so all
  // nonzero diagonal coefficients must equal its scale. Read just those
  // coefficients before paying for the full Cartesian expansion. This is an
  // exact rejection, including even modular coefficients; it proves no rewrite.
  if (CheckExtremes) {
    const llvm::APInt *Scale = nullptr;
    for (const auto &Value : Diagonal) {
      if (!Budget.consume(Words))
        return std::nullopt;
      if (Value.isZero())
        continue;
      if (!Scale)
        Scale = &Value;
      else if (Value != *Scale)
        return std::nullopt;
    }
  }
  // In a single product, the lexicographically first and last monomials each
  // occur exactly once, so both coefficients are its scale. Sum equal extrema
  // across terms; cancellation makes this check inconclusive. This optional
  // synthesis rejection must never apply to the general difference verifier.
  if (CheckExtremes && !FirstScale.isZero() && !LastScale.isZero() &&
      FirstScale != LastScale)
    return std::nullopt;

  // Allocate only after admission. The minterms sum to the all-ones word,
  // so fold all constant terms into the initial negative coefficient once.
  llvm::APInt Constant(Width, 0);
  for (const auto &Term : Terms)
    if (Term.Factors.empty())
      Constant -= Term.Coeff;
  Form.Linear.assign(Corners, Constant);

  llvm::SmallVector<llvm::SmallVector<MintermIndex, 8>, 4> Selected;
  for (const PolyTerm &Term : Terms) {
    if (Term.Factors.empty())
      continue;

    Selected.clear();
    for (SymRef Factor : Term.Factors) {
      std::optional<TruthTable> Table = tableOf(Factor);
      if (!Table)
        return std::nullopt;
      Selected.push_back(selectedMinterms(*Table));
    }

    if (Term.Factors.size() == 1) {
      if (Width <= std::numeric_limits<uint64_t>::digits) {
        // Unsigned addition wraps at 64 bits; construction projects the sum
        // to the requested width, exactly matching modular APInt addition.
        const auto Coefficient = Term.Coeff.getZExtValue();
        for (MintermIndex M : Selected[0])
          Form.Linear[M] =
              llvm::APInt(Width, Form.Linear[M].getZExtValue() + Coefficient,
                          /*isSigned=*/false, /*implicitTrunc=*/true);
      } else {
        for (MintermIndex M : Selected[0])
          Form.Linear[M] += Term.Coeff;
      }
      continue;
    }

    Form.SawProduct = true;
    // A factor selecting no minterm is the zero function, and so is the term.
    if (llvm::any_of(Selected,
                     [](llvm::ArrayRef<MintermIndex> S) { return S.empty(); }))
      continue;

    // This expansion keeps every monomial and discards the whole form when
    // the budget runs out. Count the Cartesian product first so an oversized
    // product cannot spend the remaining budget building a partial map.
    size_t ProductSize = 1;
    for (const auto &Choices : Selected) {
      if (Choices.size() > std::numeric_limits<size_t>::max() / ProductSize) {
        ProductSize = std::numeric_limits<size_t>::max();
        break;
      }
      ProductSize *= Choices.size();
    }
    if (!Budget.canConsume(ProductSize)) {
      if (!Optional)
        Budget.consume(ProductSize);
      return std::nullopt;
    }
    forEachMonomial(Selected, Budget, [&](const Monomial &Key) {
      auto It = Form.Higher.try_emplace(Key, llvm::APInt(Width, 0)).first;
      It->second += Term.Coeff;
      return true;
    });
    if (Budget.exhausted())
      return std::nullopt;
  }

  // Cancellation can leave a monomial at zero, and a zero is not part of the
  // shape the search has to explain.
  for (auto It = Form.Higher.begin(); It != Form.Higher.end();) {
    if (It->second.isZero()) {
      It = Form.Higher.erase(It);
      continue;
    }
    Form.Degree = std::max(Form.Degree, monomialDegree(It->first));
    ++It;
  }
  return Form;
}

bool proveLinearIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                         unsigned MaxAtoms, WorkBudget &Budget,
                         size_t MaxBytes) {
  SymRef Delta = Ctx.mkSub(Before, After);
  if (Ctx.isConstZero(Delta))
    return true;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, Delta, /*AllowProducts=*/false, &Budget, MaxBytes);
  if (!Abstract)
    return false;

  llvm::SmallVector<uint32_t, 16> AtomIds;
  Ctx.collectVars(Abstract->Body, AtomIds);
  if (AtomIds.empty())
    return Ctx.isConstZero(Abstract->Body);
  if (AtomIds.size() > MaxAtoms)
    return false;
  std::optional<size_t> Corners = cornerCount(AtomIds.size());
  if (!Corners || !Budget.consume(*Corners))
    return false;

  std::vector<llvm::APInt> Coefficients = measure(Ctx, Abstract->Body, AtomIds);
  return llvm::all_of(Coefficients,
                      [](const llvm::APInt &C) { return C.isZero(); });
}

bool provePolynomialIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                             unsigned MaxAtoms, WorkBudget &Budget,
                             size_t MaxBytes) {
  SymRef Delta = Ctx.mkSub(Before, After);
  if (Ctx.isConstZero(Delta))
    return true;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, Delta, /*AllowProducts=*/true, &Budget, MaxBytes);
  if (!Abstract)
    return false;

  llvm::SmallVector<uint32_t, 16> AtomIds;
  Ctx.collectVars(Abstract->Body, AtomIds);
  if (AtomIds.size() > MaxAtoms)
    return false;

  std::optional<llvm::SmallVector<PolyTerm, 8>> Terms =
      splitIntoTerms(Ctx, Abstract->Body);
  if (!Terms)
    return false;
  if (proveQuadraticCongruence(Ctx, *Terms, AtomIds, Budget, MaxBytes))
    return true;
  // Equal products cancel by their exact factor identities, before either is
  // expanded over minterms. Unchanged higher-degree residuals therefore do
  // not make an otherwise small difference proof exponential.
  bool HasHigher = llvm::any_of(
      *Terms, [](const PolyTerm &Term) { return Term.Factors.size() >= 3; });
  bool Cancelled = false;
  if (HasHigher) {
    const size_t Words = (size_t(Ctx.width(Abstract->Body)) - 1) / 64 + 1;
    size_t Storage = MaxBytes;
    std::map<Monomial, llvm::APInt> Coefficients;
    for (const auto &Term : *Terms) {
      const size_t Overhead = 256 + 4 * Words * sizeof(uint64_t);
      if (Storage < Overhead ||
          Term.Factors.size() > (Storage - Overhead) / (4 * sizeof(SymRef)) ||
          !Budget.consume(Term.Factors.size() + 2 * Words + 1))
        return false;
      Storage -= Overhead + Term.Factors.size() * 4 * sizeof(SymRef);
      Monomial Key;
      for (SymRef Factor : Term.Factors)
        Key.push_back(Factor.index());
      llvm::sort(Key);
      auto [It, New] = Coefficients.try_emplace(Key, Term.Coeff);
      if (!New)
        It->second += Term.Coeff;
    }
    const auto OriginalCount = Terms->size();
    Terms->clear();
    for (const auto &[Key, Coeff] : Coefficients) {
      if (Coeff.isZero())
        continue;
      PolyTerm Term{Coeff, {}};
      for (uint32_t Id : Key)
        Term.Factors.push_back(SymRef(Id));
      Terms->push_back(std::move(Term));
    }
    Cancelled = Terms->size() < OriginalCount;
  }
  if (!Budget.consume(Terms->size()))
    return false;
  auto CheckGroup = [&](llvm::SmallVectorImpl<PolyTerm> &DegreeTerms) {
    // Prove each homogeneous difference independently. Removing a common
    // multiset is sufficient: a zero quotient stays zero after multiplication,
    // including zero divisors. No modular division or cancellation assumption
    // is used to accept equality.
    auto Common = extractPolynomialCofactor(DegreeTerms, Budget, MaxBytes);
    llvm::SmallVector<uint32_t, 16> Inputs;
    if (Common.empty() && !Cancelled)
      Inputs.append(AtomIds);
    else
      for (const auto &Term : DegreeTerms)
        for (SymRef Factor : Term.Factors) {
          if (!Budget.consume(Ctx.dagSize(Factor)))
            return false;
          Ctx.collectVars(Factor, Inputs);
        }
    auto Form = expandOverMinterms(Ctx, DegreeTerms, Inputs,
                                   Ctx.width(Abstract->Body), Budget);
    return Form && Form->Higher.empty() &&
           llvm::all_of(Form->Linear,
                        [](const llvm::APInt &C) { return C.isZero(); });
  };
  // Constants and linear terms share the linear minterm table: the sum of
  // all minterms is the all-ones word, so their weights can cancel.
  auto Group = [](const PolyTerm &Term) {
    return std::max<size_t>(1, Term.Factors.size());
  };
  if (Terms->empty())
    return true;
  if (llvm::all_of(*Terms, [&](const PolyTerm &Term) {
        return Group(Term) == Group(Terms->front());
      }))
    return CheckGroup(*Terms);
  // Include each group's inline capacity and map node, even when that group
  // contains only one term. A homogeneous proof needs no partition copies.
  constexpr size_t EntryBytes = 12 * sizeof(PolyTerm) + 128;
  if (Terms->size() > MaxBytes / EntryBytes)
    return false;
  std::map<unsigned, llvm::SmallVector<PolyTerm, 8>> ByDegree;
  for (auto &Term : *Terms)
    ByDegree[Group(Term)].push_back(std::move(Term));
  for (auto &[Degree, DegreeTerms] : ByDegree)
    if (!CheckGroup(DegreeTerms))
      return false;
  return true;
}

} // namespace neverd::symbolic::detail
