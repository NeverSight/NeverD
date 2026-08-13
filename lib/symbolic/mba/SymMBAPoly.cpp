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

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <iterator>
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
  const uint32_t Width = Ctx.width(F);
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  const size_t Corners = size_t(1) << NumAtoms;
  SymEvalPlan Plan(Ctx, F);
  TruthTable Table = 0;

  if (Plan.fitsU64()) {
    const uint64_t Ones =
        Width == 64 ? ~uint64_t(0) : (uint64_t(1) << Width) - 1;
    std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
    for (size_t K = 0; K < Corners; ++K) {
      for (unsigned J = 0; J < NumAtoms; ++J)
        Assignment[AtomIds[J]] = (K >> J) & 1 ? Ones : 0;
      uint64_t Value = Plan.evalU64(Assignment);
      if (Value == Ones)
        Table |= TruthTable(1) << K;
      else if (Value != 0)
        return std::nullopt;
    }
    return Table;
  }

  const llvm::APInt Zero(Width, 0);
  const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), Zero);
  for (size_t K = 0; K < Corners; ++K) {
    for (unsigned J = 0; J < NumAtoms; ++J)
      Assignment[AtomIds[J]] = (K >> J) & 1 ? Ones : Zero;
    llvm::APInt Value = Plan.eval(Assignment);
    if (Value == Ones)
      Table |= TruthTable(1) << K;
    else if (!Value.isZero())
      return std::nullopt;
  }
  return Table;
}

} // namespace

std::optional<llvm::SmallVector<PolyTerm, 8>>
splitIntoTerms(const SymContext &Ctx, SymRef Body) {
  llvm::SmallVector<PolyTerm, 8> Terms;
  if (!collectTerms(Ctx, Body, llvm::APInt(Ctx.width(Body), 1), Terms))
    return std::nullopt;
  return Terms;
}

Monomial makeMonomial(llvm::ArrayRef<uint16_t> Sorted) {
  return Monomial(Sorted.begin(), Sorted.end());
}

unsigned monomialDegree(const Monomial &Key) {
  return static_cast<unsigned>(Key.size());
}

TruthTable monomialSupport(const Monomial &Key) {
  TruthTable Support = 0;
  for (uint16_t Index : Key)
    Support |= TruthTable(1) << Index;
  return Support;
}

llvm::SmallVector<uint16_t, 8> selectedMinterms(TruthTable Table,
                                                size_t Corners) {
  llvm::SmallVector<uint16_t, 8> Out;
  for (size_t M = 0; M < Corners; ++M)
    if (truthTableAt(Table, M))
      Out.push_back(static_cast<uint16_t>(M));
  return Out;
}

std::optional<PolyForm> expandOverMinterms(const SymContext &Ctx,
                                           llvm::ArrayRef<PolyTerm> Terms,
                                           llvm::ArrayRef<uint32_t> AtomIds,
                                           uint32_t Width, WorkBudget &Budget) {
  const size_t Corners = size_t(1) << AtomIds.size();
  PolyForm Form;
  Form.Linear.assign(Corners, llvm::APInt(Width, 0));

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

  llvm::SmallVector<llvm::SmallVector<uint16_t, 8>, 4> Selected;
  for (const PolyTerm &Term : Terms) {
    if (Term.Factors.empty()) {
      // The minterms sum to the all-ones word, so the constant c is the linear
      // form with every weight at -c.
      for (llvm::APInt &Weight : Form.Linear)
        Weight -= Term.Coeff;
      continue;
    }

    Selected.clear();
    for (SymRef Factor : Term.Factors) {
      std::optional<TruthTable> Table = tableOf(Factor);
      if (!Table)
        return std::nullopt;
      Selected.push_back(selectedMinterms(*Table, Corners));
    }

    if (Term.Factors.size() == 1) {
      for (uint16_t M : Selected[0])
        Form.Linear[M] += Term.Coeff;
      continue;
    }

    Form.SawProduct = true;
    // A factor selecting no minterm is the zero function, and so is the term.
    if (llvm::any_of(Selected,
                     [](llvm::ArrayRef<uint16_t> S) { return S.empty(); }))
      continue;

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
  for (auto It = Form.Higher.begin(); It != Form.Higher.end();)
    It = It->second.isZero() ? Form.Higher.erase(It) : std::next(It);
  for (const auto &[Key, Coeff] : Form.Higher)
    Form.Degree = std::max(Form.Degree, monomialDegree(Key));
  return Form;
}

bool proveLinearIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                         unsigned MaxAtoms, WorkBudget &Budget) {
  SymRef Delta = Ctx.mkSub(Before, After);
  if (Ctx.isConstZero(Delta))
    return true;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, Delta, /*AllowProducts=*/false);
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
                             unsigned MaxAtoms, WorkBudget &Budget) {
  SymRef Delta = Ctx.mkSub(Before, After);
  if (Ctx.isConstZero(Delta))
    return true;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, Delta, /*AllowProducts=*/true);
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
  std::optional<PolyForm> Form = expandOverMinterms(
      Ctx, *Terms, AtomIds, Ctx.width(Abstract->Body), Budget);
  if (!Form)
    return false;

  return llvm::all_of(Form->Linear,
                      [](const llvm::APInt &C) { return C.isZero(); }) &&
         Form->Higher.empty();
}

} // namespace neverd::symbolic::detail
