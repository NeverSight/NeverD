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

#include "Limits.h"
#include "SymMBADetail.h"

#include "neverd/symbolic/SymKnownBits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace neverd::symbolic {

using namespace detail;

namespace {

SymRef projectBitWindow(SymContext &Ctx, SymRef Source, unsigned Low,
                        unsigned Width, WorkBudget &Budget, size_t &Storage,
                        unsigned Depth = 0) {
  auto Original = [&] { return Ctx.mkExtract(Source, Low, Width); };
  const size_t Words = (size_t(Ctx.width(Source)) - 1) / 64 + 1;
  constexpr size_t NodeBytes = 128;
  if (Depth >= ::neverd::symbolic::detail::limits::
                   kSymMBAProjectBitWindowDepthLimit ||
      Storage < NodeBytes || Words > (Storage - NodeBytes) / sizeof(uint64_t) ||
      !Budget.consume(1 + Words))
    return Original();
  Storage -= NodeBytes + Words * sizeof(uint64_t);
  const SymOp Op = Ctx.op(Source);
  auto Part = [&](SymRef Value, unsigned Begin, unsigned Bits) {
    return projectBitWindow(Ctx, Value, Begin, Bits, Budget, Storage,
                            Depth + 1);
  };
  if (Op == SymOp::Extract)
    return Part(Ctx.operand(Source, 0), Low + Ctx.node(Source).Aux, Width);
  if (Op == SymOp::ZExt || Op == SymOp::SExt) {
    SymRef Inner = Ctx.operand(Source, 0);
    const unsigned Bits = Ctx.width(Inner);
    if (Low >= Bits)
      return Op == SymOp::ZExt ? Ctx.mkZero(Width)
                               : Ctx.mkSExt(Part(Inner, Bits - 1, 1), Width);
    SymRef Value = Part(Inner, Low, std::min(Width, Bits - Low));
    return Op == SymOp::ZExt ? Ctx.mkZExt(Value, Width)
                             : Ctx.mkSExt(Value, Width);
  }
  if ((Op == SymOp::LShr || Op == SymOp::AShr) &&
      Ctx.isConst(Ctx.operand(Source, 1))) {
    SymRef Inner = Ctx.operand(Source, 0);
    const unsigned Bits = Ctx.width(Inner);
    const auto CountRef = Ctx.operand(Source, 1);
    const size_t CountWords = (size_t(Ctx.width(CountRef)) - 1) / 64 + 1;
    if (CountWords > Storage / sizeof(uint64_t) || !Budget.consume(CountWords))
      return Original();
    Storage -= CountWords * sizeof(uint64_t);
    const auto Count = Ctx.constValue(CountRef);
    if (Count.uge(Bits) || Low >= Bits - unsigned(Count.getZExtValue()))
      return Op == SymOp::LShr ? Ctx.mkZero(Width)
                               : Ctx.mkSExt(Part(Inner, Bits - 1, 1), Width);
    const unsigned Shift = unsigned(Count.getZExtValue());
    SymRef Value =
        Part(Inner, Low + Shift, std::min(Width, Bits - Shift - Low));
    return Op == SymOp::LShr ? Ctx.mkZExt(Value, Width)
                             : Ctx.mkSExt(Value, Width);
  }
  if (Op == SymOp::Mul && Ctx.numOperands(Source) == 2 &&
      Ctx.isConst(Ctx.operand(Source, 0))) {
    const auto Factor = Ctx.constValue(Ctx.operand(Source, 0));
    if (Factor.isPowerOf2()) {
      const unsigned Shift = Factor.logBase2();
      if (Low < Shift && Width <= Shift - Low)
        return Ctx.mkZero(Width);
      const unsigned Padding = Low < Shift ? Shift - Low : 0;
      SymRef Value = Part(Ctx.operand(Source, 1), Low < Shift ? 0 : Low - Shift,
                          Width - Padding);
      return Padding ? Ctx.mkConcat(Value, Ctx.mkZero(Padding)) : Value;
    }
  }
  if (Op == SymOp::Ite) {
    const auto Condition = Ctx.operand(Source, 0);
    const auto Yes = Ctx.operand(Source, 1), No = Ctx.operand(Source, 2);
    return Ctx.mkIte(Condition, Part(Yes, Low, Width), Part(No, Low, Width));
  }
  if (Op == SymOp::Add &&
      Ctx.numOperands(Source) <=
          ::neverd::symbolic::detail::limits::kMaxProjectedBitWindowOperands) {
    const size_t Children = Ctx.numOperands(Source);
    if (Children > Storage / (2 * sizeof(SymRef)) || !Budget.consume(Children))
      return Original();
    Storage -= Children * 2 * sizeof(SymRef);
    llvm::SmallVector<SymRef, 8> Inputs(Ctx.operands(Source)), Values;
    // Narrow at the top of the observed window, not at its bottom. Discarding
    // bits below Low would lose the carry demonstrated by x=63, y=1, Low=6.
    for (auto Child : Inputs)
      Values.push_back(Part(Child, 0, Low + Width));
    return Ctx.mkExtract(Ctx.mkAdd(Values), Low, Width);
  }
  if (isBitwise(Op) &&
      Ctx.numOperands(Source) <=
          ::neverd::symbolic::detail::limits::kMaxProjectedBitWindowOperands) {
    const size_t Children = Ctx.numOperands(Source);
    if (Children > Storage / (2 * sizeof(SymRef)) || !Budget.consume(Children))
      return Original();
    Storage -= Children * 2 * sizeof(SymRef);
    // Builders may grow the operand arena while the child windows are made.
    llvm::SmallVector<SymRef, 8> Inputs(Ctx.operands(Source));
    llvm::SmallVector<SymRef, 8> Values;
    for (SymRef Child : Inputs)
      Values.push_back(Part(Child, Low, Width));
    if (Op == SymOp::Not)
      return Ctx.mkNot(Values.front());
    if (Op == SymOp::And)
      return Ctx.mkAnd(Values);
    if (Op == SymOp::Or)
      return Ctx.mkOr(Values);
    return Ctx.mkXor(Values);
  }
  return Original();
}

SymRef foldLiteralXorEquality(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                              size_t &Storage) {
  if (Ctx.op(Root) != SymOp::Eq)
    return Root;
  for (unsigned I = 0; I != 2; ++I) {
    auto Literal = Ctx.operand(Root, I), Sum = Ctx.operand(Root, 1 - I);
    if (Ctx.isConst(Literal) && Ctx.op(Sum) == SymOp::Xor &&
        Ctx.isConst(Ctx.operand(Sum, 0))) {
      auto Inputs = Ctx.operands(Sum).drop_front();
      const size_t Words = (Ctx.width(Sum) - 1) / 64 + 1;
      if (Inputs.size() > Storage / (2 * sizeof(SymRef)) ||
          !Budget.consume(2 + Words + Inputs.size()))
        return Root;
      Storage -= Inputs.size() * 2 * sizeof(SymRef);
      auto Value =
          Ctx.constValue(Literal) ^ Ctx.constValue(Ctx.operand(Sum, 0));
      llvm::SmallVector<SymRef, 8> Owned(Inputs);
      return Ctx.mkEq(Ctx.mkXor(Owned), Ctx.mkConst(Value));
    }
  }
  return Root;
}

SymRef foldObservedWindow(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                          size_t &Storage) {
  if (Budget.exhausted() || !Budget.canConsume(1) || Storage < 128)
    return Root;
  if (SymRef Parity = foldProductParity(Ctx, Root, Budget, Storage);
      Parity != Root)
    return Parity;
  if (Ctx.op(Root) == SymOp::Extract)
    return projectBitWindow(Ctx, Ctx.operand(Root, 0), Ctx.node(Root).Aux,
                            Ctx.width(Root), Budget, Storage);
  if (Ctx.op(Root) == SymOp::Not) {
    auto Inner = Ctx.operand(Root, 0);
    if (Ctx.op(Inner) == SymOp::Or && Ctx.isConst(Ctx.operand(Inner, 0)) &&
        Ctx.numOperands(Inner) <=
            detail::limits::kMaxProjectedBitWindowOperands) {
      const size_t Words = (Ctx.width(Root) - 1) / 64 + 1;
      auto Inputs = Ctx.operands(Inner).drop_front();
      const size_t Bytes =
          Words * sizeof(uint64_t) + Inputs.size() * 2 * sizeof(SymRef);
      if (Storage < Bytes || !Budget.consume(Words + Inputs.size()))
        return Root;
      Storage -= Bytes;
      auto Mask = ~Ctx.constValue(Ctx.operand(Inner, 0));
      if (Mask.isPowerOf2()) {
        llvm::SmallVector<SymRef, 8> Owned(Inputs);
        auto Source = Ctx.mkOr(Owned);
        auto Bit =
            projectBitWindow(Ctx, Source, Mask.logBase2(), 1, Budget, Storage);
        // ~(K|x) observes only ~K. Read that window directly rather than
        // requiring a whole-word De Morgan rewrite before projection. An
        // unreduced window would only spell each input again as an extract.
        auto Window = Ctx.mkMul(Ctx.mkConst(Mask),
                                Ctx.mkZExt(Ctx.mkNot(Bit), Ctx.width(Root)));
        return doesNotGrow(Ctx, Window, Root) ? Window : Root;
      }
    }
  }
  if (Ctx.op(Root) != SymOp::And || Ctx.numOperands(Root) < 2 ||
      !Ctx.isConst(Ctx.operand(Root, 0)))
    return Root;
  auto Source = Ctx.operand(Root, 1);
  // A masked leaf has no hidden lanes to project. Preserve the existing
  // minimum-expression proof and its exact work budget for this common case.
  if (Ctx.numOperands(Root) == 2 && Ctx.numOperands(Source) == 0)
    return Root;
  const unsigned Words = (Ctx.width(Root) - 1) / 64 + 1;
  if (Words > Storage / sizeof(uint64_t) || !Budget.consume(Words))
    return Root;
  Storage -= Words * sizeof(uint64_t);
  const auto Mask = Ctx.constValue(Ctx.operand(Root, 0));
  if (Ctx.numOperands(Root) > 2) {
    auto Inputs = Ctx.operands(Root).drop_front();
    if (!Mask.isPowerOf2() ||
        Inputs.size() > detail::limits::kMaxProjectedBitWindowOperands ||
        Inputs.size() > Storage / (2 * sizeof(SymRef)) ||
        !Budget.consume(Inputs.size()))
      return Root;
    Storage -= Inputs.size() * 2 * sizeof(SymRef);
    llvm::SmallVector<SymRef, 8> Owned(Inputs);
    Source = Ctx.mkAnd(Owned);
  }
  // Testing a packed flag often observes one bit without shifting it to bit
  // zero first. Use the same window transfer, then restore its original bit
  // position. Arithmetic retains all lower bits that can carry into the
  // observation; unsupported transfers retain their complete source.
  if (Mask.isPowerOf2()) {
    const unsigned Bit = Mask.logBase2();
    auto Part = projectBitWindow(Ctx, Source, Bit, 1, Budget, Storage);
    if (Part == Ctx.mkExtract(Source, Bit, 1))
      return Root;
    auto Window =
        Ctx.mkMul(Ctx.mkConst(Mask), Ctx.mkZExt(Part, Ctx.width(Root)));
    // A window is useful only when it drops lanes. A longer one spells every
    // input again as an extract and hides the mask from the exact known-bits
    // fold that follows: 8192 & (4*q + p) is zero, but its window is not.
    return doesNotGrow(Ctx, Window, Root) ? Window : Root;
  }
  // A shifted low mask is also an explicit observed window. Keep ordinary
  // masked arithmetic in its original region: narrowing every internal mask
  // hides whole-word relations that the existing MBA measurement can recover.
  if (Ctx.op(Source) != SymOp::LShr && Ctx.op(Source) != SymOp::AShr)
    return Root;
  if (!Mask.isMask() || Mask.isAllOnes())
    return Root;
  auto Part =
      projectBitWindow(Ctx, Source, 0, Mask.popcount(), Budget, Storage);
  return Ctx.mkZExt(Part, Ctx.width(Root));
}

SymRef foldKnownBits(SymContext &Ctx, SymRef Root, SymKnownBits &Known,
                     WorkBudget &Budget, size_t &Storage) {
  const auto Op = Ctx.op(Root);
  if (Ctx.isConst(Root) || Ctx.isVar(Root) ||
      Ctx.width(Root) > SymKnownBits::MaxWidth)
    return Root;
  const bool HasMask = (Op == SymOp::And || Op == SymOp::Or) &&
                       Ctx.numOperands(Root) == 2 &&
                       Ctx.isConst(Ctx.operand(Root, 0));
  // Only ask for facts at the consumers that use them. Opaque boundaries keep
  // the existing deep walk's guarantee of spending no region work there.
  if (!HasMask && !isPredicate(Op))
    return Root;
  constexpr size_t FactBytes = 128;
  unsigned Available = unsigned(
      std::min<size_t>(SymKnownBits::MaxQueryWork, Storage / FactBytes));
  while (Available && !Budget.canConsume(Available))
    Available /= 2;
  if (!Available)
    return Root;
  const auto Source = HasMask ? Ctx.operand(Root, 1) : Root;
  if (HasMask && Ctx.numOperands(Source) == 0)
    return Root;
  unsigned Remaining = Available;
  auto Bits = Known.query(Source, Remaining);
  Budget.consume(Available - Remaining);
  Storage -= (Available - Remaining) * FactBytes;
  if (!HasMask)
    return Bits && Bits->isConstant() ? Ctx.mkConst(Bits->One) : Root;
  const unsigned Words = (Ctx.width(Root) - 1) / 64 + 1;
  if (!Bits || Storage < Words * sizeof(uint64_t) || !Budget.consume(Words))
    return Root;
  Storage -= Words * sizeof(uint64_t);
  const auto Mask = Ctx.constValue(Ctx.operand(Root, 0));
  const auto Unknown = ~(Bits->Zero | Bits->One);
  if (Op == SymOp::And) {
    if ((Unknown & Mask).isZero())
      return Ctx.mkConst(Bits->One & Mask);
    return (Mask | Bits->Zero).isAllOnes() ? Source : Root;
  }
  if ((Unknown & ~Mask).isZero())
    return Ctx.mkConst(Bits->One | Mask);
  const auto Missing = Mask & ~Bits->One;
  if (Missing.isZero())
    return Source;
  return (Missing & ~Bits->Zero).isZero()
             ? Ctx.mkAdd(Source, Ctx.mkConst(Missing))
             : Root;
}

SymRef foldConditionalControls(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                               size_t &Storage) {
  auto Op = Ctx.op(Root);
  if (Op == SymOp::Eq) {
    for (unsigned I = 0; I != 2; ++I) {
      auto Literal = Ctx.operand(Root, I), Sum = Ctx.operand(Root, 1 - I);
      if (Ctx.isConst(Literal) && Ctx.op(Sum) == SymOp::Add &&
          Ctx.numOperands(Sum) == 2 && Ctx.isConst(Ctx.operand(Sum, 0)) &&
          Budget.consume(3 + (Ctx.width(Sum) - 1) / 64))
        return Ctx.mkEq(Ctx.operand(Sum, 1),
                        Ctx.mkConst(Ctx.constValue(Literal) -
                                    Ctx.constValue(Ctx.operand(Sum, 0))));
    }
  }
  if (isPredicate(Op)) {
    // A flag can be widened, scaled and encoded as several constant tags
    // before a comparison observes it. Read both exact Boolean cofactors,
    // stopping at the one-bit input rather than traversing its computation.
    llvm::SmallVector<SymRef, 16> Pending(Ctx.operands(Root));
    llvm::DenseSet<uint32_t> Seen;
    llvm::SmallVector<uint32_t, 16> Order;
    SymRef Control;
    size_t Edges = 0;
    bool Complete = true;
    while (!Pending.empty()) {
      auto R = Pending.pop_back_val();
      if (Seen.contains(R.index()))
        continue;
      if (Seen.size() == detail::limits::kMaxBooleanBoundaryNodes ||
          Storage < 256 || !Budget.consume()) {
        Complete = false;
        break;
      }
      Storage -= 256;
      Seen.insert(R.index());
      if (Ctx.isConst(R))
        continue;
      if (Ctx.width(R) == 1) {
        if (Control && Control != R) {
          Complete = false;
          break;
        }
        Control = R;
        continue;
      }
      auto Inputs = Ctx.operands(R);
      if (Inputs.empty() ||
          Inputs.size() > detail::limits::kMaxConditionalControlEdges - Edges ||
          !Budget.consume(Inputs.size())) {
        Complete = false;
        break;
      }
      Edges += Inputs.size();
      Order.push_back(R.index());
      Pending.append(Inputs.begin(), Inputs.end());
    }
    if (Complete && Control && Budget.consume(2 * (Order.size() + Edges + 1))) {
      llvm::sort(Order);
      Order.push_back(Root.index());
      llvm::DenseMap<uint32_t, SymRef> No, Yes;
      No[Control.index()] = Ctx.mkZero(1);
      Yes[Control.index()] = Ctx.mkOne(1);
      for (uint32_t Index : Order) {
        auto R = SymRef(Index);
        llvm::SmallVector<SymRef, 8> Inputs(Ctx.operands(R)), N, Y;
        for (auto Child : Inputs) {
          auto NV = No.lookup(Child.index()), YV = Yes.lookup(Child.index());
          N.push_back(NV ? NV : Child);
          Y.push_back(YV ? YV : Child);
        }
        No[Index] = Ctx.rebuild(R, N);
        Yes[Index] = Ctx.rebuild(R, Y);
      }
      auto N = No.lookup(Root.index()), Y = Yes.lookup(Root.index());
      if (Ctx.isConst(N) && Ctx.isConst(Y))
        return Ctx.mkIte(Control, Y, N);
    }
  }
  if (Op != SymOp::Ite || Budget.exhausted() || Storage < 128)
    return Root;
  auto Condition = Ctx.operand(Root, 0);
  std::vector<SymRef> Work{Ctx.operand(Root, 1), Ctx.operand(Root, 2)};
  llvm::DenseSet<uint32_t> Seen;
  bool Dependent = false;
  unsigned Edges = 0;
  while (!Work.empty()) {
    auto Value = Work.back();
    Work.pop_back();
    if (!Seen.insert(Value.index()).second)
      continue;
    if (!Budget.consume() ||
        Seen.size() > ::neverd::symbolic::detail::limits::
                          kSymMBAFoldConditionalControlsSeenSizeLimit)
      return Root;
    Dependent |= Value == Condition;
    auto Children = Ctx.operands(Value);
    if (Children.size() >
        ::neverd::symbolic::detail::limits::kMaxConditionalControlEdges - Edges)
      return Root;
    Edges += Children.size();
    Work.insert(Work.end(), Children.begin(), Children.end());
  }
  const size_t Bytes = Seen.size() * 128;
  if (!Dependent || Storage < Bytes || !Budget.consume(2 * Seen.size()))
    return Root;
  Storage -= Bytes;
  auto T =
      Ctx.substitute(Ctx.operand(Root, 1), {{Condition.index(), Ctx.mkOne(1)}});
  auto E = Ctx.substitute(Ctx.operand(Root, 2),
                          {{Condition.index(), Ctx.mkZero(1)}});
  auto Result = Ctx.mkIte(Condition, T, E);
  return doesNotGrow(Ctx, Result, Root) ? Result : Root;
}

// Finish exact local consumers exposed by region measurement. Their nodes
// were absent from the input postorder; another whole MBA search would repeat
// completed work. This single postorder only projects observed windows and
// folds facts/Boolean controls, under the same remaining resources.
SymRef finishLocalReadings(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                           size_t &Storage) {
  llvm::SmallVector<SymRef, 16> Pending{Root};
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<uint32_t, 16> Order;
  while (!Pending.empty()) {
    auto R = Pending.pop_back_val();
    if (Seen.contains(R.index()))
      continue;
    auto Inputs = Ctx.operands(R);
    const size_t Bytes = 128 + 2 * sizeof(SymRef) * Inputs.size();
    if (Storage < Bytes || !Budget.consume(1 + Inputs.size()))
      return Root;
    Storage -= Bytes;
    Seen.insert(R.index());
    Order.push_back(R.index());
    Pending.append(Inputs.begin(), Inputs.end());
  }
  llvm::sort(Order);
  llvm::DenseMap<uint32_t, SymRef> Done;
  SymKnownBits Known(Ctx);
  for (auto Index : Order) {
    auto R = SymRef(Index);
    llvm::SmallVector<SymRef, 8> Inputs;
    for (auto Child : Ctx.operands(R))
      Inputs.push_back(Done.lookup(Child.index()));
    auto Value = Inputs.empty() ? R : Ctx.rebuild(R, Inputs);
    Value = foldObservedWindow(Ctx, Value, Budget, Storage);
    Value = foldKnownBits(Ctx, Value, Known, Budget, Storage);
    Done[Index] = foldConditionalControls(Ctx, Value, Budget, Storage);
  }
  return Done.lookup(Root.index());
}

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
  // Every path from a collected mask to the root must remain bitwise.
  // An arithmetic root cannot meet that condition, regardless of its masks.
  if (!isBitwise(Ctx.op(E)))
    return E;
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
  if (!Opts.AllowGrowth && !doesNotGrow(Ctx, Rebuilt, E))
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

SymRef tryFastComplementarySum(SymContext &Ctx, SymRef E,
                               const MBAOptions &Opts, WorkBudget &Budget,
                               SolveReport &Rep);

/// Measure \p E as one region, splitting a wide one into independent parts and
/// a masked one into mask-uniform columns.
SymRef solveRegionOrSplitImpl(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                              WorkBudget &Budget, SolveReport &Rep) {
  if (!Budget.exhausted() && isMinimalBinaryLeafExpression(Ctx, E, Opts)) {
    Rep.Outcome = MBAOutcome::AlreadyShortest;
    return E;
  }
  if (SymRef Fast = tryFastComplementarySum(Ctx, E, Opts, Budget, Rep);
      Fast != E)
    return Fast;
  if (SymRef Weighted = solveWeightedMasks(Ctx, E, Opts, Budget, Rep);
      Weighted != E) {
    SolveReport Refined;
    SymRef Candidate = solveOneRegion(Ctx, Weighted, Opts, Budget, Refined);
    Rep.BudgetExhausted |= Refined.BudgetExhausted;
    Rep.NumAtoms = Refined.NumAtoms;
    if (readingScore(Ctx, Candidate) < readingScore(Ctx, Weighted)) {
      Rep.Evidence = Refined.Evidence;
      return Candidate;
    }
    return Weighted;
  }
  SymRef Solved = solveOneRegion(Ctx, E, Opts, Budget, Rep);
  return Solved == E ? solveMasked(Ctx, E, Opts, Budget, Rep) : Solved;
}

using CompletedRegions =
    llvm::DenseMap<uint32_t, std::pair<SymRef, SolveReport>>;

SymRef solveRegionOrSplit(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          WorkBudget &Budget, SolveReport &Rep,
                          CompletedRegions *Cache = nullptr,
                          size_t *Storage = nullptr) {
  if (Cache) {
    auto It = Cache->find(E.index());
    if (It != Cache->end()) {
      if (!Budget.consume()) {
        Rep.BudgetExhausted = true;
        return E;
      }
      Rep = It->second.second;
      return It->second.first;
    }
  }
  const size_t Before = Budget.used();
  SymRef Result = solveRegionOrSplitImpl(Ctx, E, Opts, Budget, Rep);
  // This cache belongs to one deep invocation, with one context and option
  // set. Candidate refinement and the original-region reading often revisit
  // exactly the same emitted expression. Reuse only completed exact results;
  // a refusal must remain eligible for a later attempt.
  constexpr size_t EntryBytes = 128;
  if (Cache && Storage && !Budget.exhausted() && !Rep.BudgetExhausted &&
      (Result == E || Rep.Evidence == MBAEvidence::Derivation) &&
      Budget.used() - Before >= detail::limits::kMinCompletedRegionCacheWork &&
      *Storage >= EntryBytes && Budget.consume()) {
    *Storage -= EntryBytes;
    Cache->try_emplace(E.index(), Result, Rep);
  }
  return Result;
}

// Child rewrites can hide a shared arithmetic input in one bitwise use while
// its other uses retain the original spelling. Keep a whole-region reading
// there. A product retains the arithmetic reading for distribution and
// cancellation; remeasuring its already simplified factors as one bitwise
// region often repeats their work. Complements alone use that same route.
bool hasBitwiseInteraction(const SymContext &Ctx, SymRef R) {
  auto Bitwise = [&](SymRef N) {
    SymOp Op = Ctx.op(N);
    return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor;
  };
  return Bitwise(R) ||
         (Ctx.op(R) == SymOp::Add && llvm::any_of(Ctx.operands(R), Bitwise));
}

// A child may improve a complement of a product into an arithmetic sum and
// thereby hide its relation to the product's other bitwise uses. Preserve one
// original reading at sums of bitwise products, including a complemented sum.
// This only inspects the immediate sum/product frontier; it does not revisit
// the graph or broaden the emitted-candidate refinement below.
bool hasBitwiseProductSum(const SymContext &Ctx, SymRef R) {
  if (Ctx.op(R) == SymOp::Not)
    R = Ctx.operand(R, 0);
  if (Ctx.op(R) != SymOp::Add)
    return false;
  for (SymRef Term : Ctx.operands(R)) {
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    unsigned BitwiseFactors = 0;
    for (SymRef Factor : Ctx.operands(Term)) {
      SymOp Op = Ctx.op(Factor);
      if ((Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor) &&
          ++BitwiseFactors == 2)
        return true;
    }
  }
  return false;
}

// Boolean rewrites over the very same opaque words preserve the surrounding
// MBA abstraction. A second reading of the old spelling is useful when an
// arithmetic/opaque boundary changes, but not for these already proved local
// Boolean substitutions. Bound this comparison independently of tree depth.
bool sameBooleanBoundary(const SymContext &Ctx, SymRef Before, SymRef After,
                         WorkBudget &Budget, size_t &Storage) {
  auto Boolean = [&](SymRef R) {
    const auto Op = Ctx.op(R);
    return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
           Op == SymOp::Not;
  };
  if (!Boolean(Before) || (!Boolean(After) && !Ctx.isVar(After)))
    return false;
  auto Collect = [&](SymRef Root, llvm::SmallVectorImpl<uint32_t> &Leaves) {
    llvm::SmallVector<SymRef, 16> Pending{Root};
    llvm::DenseSet<uint32_t> Seen;
    while (!Pending.empty()) {
      SymRef R = Pending.pop_back_val();
      if (!Budget.canConsume(1) ||
          Seen.size() >= detail::limits::kMaxBooleanBoundaryNodes ||
          Storage < 128)
        return false;
      Budget.consume();
      if (!Seen.insert(R.index()).second)
        continue;
      Storage -= 128;
      if (!Boolean(R)) {
        Leaves.push_back(R.index());
        continue;
      }
      const auto Inputs = Ctx.operands(R);
      if (Inputs.size() >
              detail::limits::kMaxBooleanBoundaryNodes -
                  std::min<size_t>(detail::limits::kMaxBooleanBoundaryNodes,
                                   Pending.size()) ||
          !Budget.canConsume(Inputs.size()) ||
          Inputs.size() > Storage / (2 * sizeof(SymRef)))
        return false;
      Budget.consume(Inputs.size());
      Storage -= Inputs.size() * 2 * sizeof(SymRef);
      Pending.append(Inputs.begin(), Inputs.end());
    }
    llvm::sort(Leaves);
    return true;
  };
  llvm::SmallVector<uint32_t, 16> Left, Right;
  return Collect(Before, Left) && Collect(After, Right) && Left == Right;
}

// A candidate can introduce a factored sum that was absent from the original
// postorder. Visit its unmeasured nodes once, reusing exact child replacements.
// The snapshot below is deliberately not extended with nodes emitted by this
// visit: this is one bounded refinement, not a whole-graph fixed point.
SymRef refineCandidate(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                       llvm::ArrayRef<uint32_t> OriginalOrder,
                       llvm::DenseMap<uint32_t, SymRef> &Solved,
                       CompletedRegions &Regions, WorkBudget &Budget,
                       size_t &Storage, SolveReport &Rep) {
  auto Known = [&](SymRef R) {
    auto It = Solved.find(R.index());
    if (It != Solved.end())
      return It->second;
    if (R.index() <= OriginalOrder.back() &&
        std::binary_search(OriginalOrder.begin(), OriginalOrder.end(),
                           R.index()))
      return R;
    return SymRef();
  };
  if (SymRef R = Known(E); R.isValid())
    return readingScore(Ctx, R) < readingScore(Ctx, E) ? R : E;
  // An emitted root whose children already have their final spelling has no
  // new internal region to visit. Constants and scaled variables are also
  // builder-normalized leaves of this traversal.
  auto Finished = [&](SymRef R) {
    if (SymRef K = Known(R); K.isValid())
      return K == R;
    return Ctx.numOperands(R) == 0 ||
           (Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
            Ctx.isConst(Ctx.operand(R, 0)) && Ctx.isVar(Ctx.operand(R, 1)));
  };
  if (llvm::all_of(Ctx.operands(E), Finished))
    return E;
  auto Stop = [&]() {
    Rep.BudgetExhausted = true;
    return E;
  };
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 16> Pending{E};
  llvm::SmallVector<uint32_t, 16> Order;
  while (!Pending.empty()) {
    SymRef R = Pending.pop_back_val();
    if (!Budget.consume())
      return Stop();
    if (Known(R).isValid() || Seen.contains(R.index()))
      continue;
    const size_t Children = Ctx.numOperands(R);
    // Charge cumulative scratch and memo storage, including edge slots,
    // before extending either collection.
    constexpr size_t NodeBytes = 128;
    constexpr size_t EdgeBytes = 4 * sizeof(SymRef);
    if (Storage < NodeBytes || Children > (Storage - NodeBytes) / EdgeBytes)
      return Stop();
    if (!Budget.consume(Children))
      return Stop();
    // Both the pending and rebuilt-operand buffers can retain spare capacity.
    Storage -= NodeBytes + Children * EdgeBytes;
    Seen.insert(R.index());
    Order.push_back(R.index());
    Pending.append(Ctx.operands(R).begin(), Ctx.operands(R).end());
  }
  llvm::sort(Order);
  for (uint32_t Index : Order) {
    SymRef R(Index);
    llvm::SmallVector<SymRef, 8> Ops;
    bool Changed = false;
    for (SymRef Child : Ctx.operands(R)) {
      auto It = Solved.find(Child.index());
      SymRef Next = It == Solved.end() ? Child : It->second;
      Changed |= Next != Child;
      Ops.push_back(Next);
    }
    SymRef Rebuilt = Changed ? Ctx.rebuild(R, Ops) : R;
    SymRef Best =
        readingScore(Ctx, Rebuilt) <= readingScore(Ctx, R) ? Rebuilt : R;
    // The final region can restore a product into an additive bitwise
    // relation without having measured that new root. Finish that exposed
    // relation once within this fixed frontier, even if its children stayed
    // unchanged. Already-known roots and finished children returned above;
    // expressions emitted by this reading are not added to the frontier.
    const bool ExposedRoot = R == E && Ctx.op(Rebuilt) == SymOp::Add &&
                             hasBitwiseInteraction(Ctx, Rebuilt);
    if ((R != E || Changed || ExposedRoot) && canMeasureAtRoot(Ctx, Rebuilt)) {
      if (Budget.exhausted() || !Budget.consume(Ctx.dagSize(Rebuilt)))
        return Stop();
      SolveReport Local;
      SymRef Measured = solveRegionOrSplit(Ctx, Rebuilt, Opts, Budget, Local,
                                           &Regions, &Storage);
      Rep.BudgetExhausted |= Local.BudgetExhausted;
      if (readingScore(Ctx, Measured) < readingScore(Ctx, Best)) {
        Best = Measured;
        Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
        if (Local.Evidence == MBAEvidence::Samples)
          Rep.Evidence = MBAEvidence::Samples;
      }
      if (Budget.exhausted())
        return Stop();
    }
    Solved[Index] = Best;
  }
  SymRef Best = Solved.lookup(E.index());
  return readingScore(Ctx, Best) < readingScore(Ctx, E) ? Best : E;
}

SymRef foldComplementaryAdd(SymContext &Ctx, SymRef R) {
  if (Ctx.op(R) != SymOp::Add)
    return R;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(R);
  llvm::APInt Offset(Ctx.width(R), 0);
  SymRef A, B;
  for (SymRef Term : Terms) {
    if (Ctx.isConst(Term)) {
      Offset += Ctx.constValue(Term);
    } else if (!A.isValid()) {
      A = Term;
    } else if (!B.isValid()) {
      B = Term;
    } else {
      return R;
    }
  }
  if (!A.isValid())
    return R;
  if (!B.isValid()) {
    if (Ctx.op(A) != SymOp::Or || !Ctx.isConst(Ctx.operand(A, 0)))
      return R;
    const auto Mask = Ctx.constValue(Ctx.operand(A, 0));
    if (Offset != -Mask)
      return R;
    // Every bit in Mask is already set, so subtracting it cannot borrow.
    // Keep the other OR operands intact and clear exactly those known bits.
    llvm::SmallVector<SymRef, 8> Rest(Ctx.operands(A).drop_front());
    return Ctx.mkAnd(Ctx.mkConst(~Mask), Ctx.mkOr(Rest));
  }
  if (isBitwiseComplement(Ctx, A, B))
    return Ctx.mkConst(Offset - llvm::APInt(Ctx.width(R), 1));
  SymRef Partitioned = foldPartitionedMaskSum(Ctx, A, B, Offset);
  return Partitioned.isValid() ? Partitioned : R;
}

SymRef tryFastComplementarySum(SymContext &Ctx, SymRef E,
                               const MBAOptions &Opts, WorkBudget &Budget,
                               SolveReport &Rep) {
  if (Ctx.op(E) != SymOp::Add ||
      Opts.MaxTableBytes <
          ::neverd::symbolic::detail::limits::
              kSymMBATryFastComplementarySumOptsMaxTableBytesLimit ||
      !Budget.canConsume(16))
    return E;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(E);
  if (Terms.size() < 2 || Terms.size() > 3)
    return E;
  SymRef A, B;
  for (SymRef Term : Terms) {
    if (Ctx.isConst(Term))
      continue;
    if (!A.isValid()) {
      A = Term;
    } else if (!B.isValid()) {
      B = Term;
    } else {
      return E;
    }
  }
  if (!A.isValid())
    return E;
  const SymOp AOp = Ctx.op(A);
  const bool LiteralOr =
      !B.isValid() && AOp == SymOp::Or && Ctx.isConst(Ctx.operand(A, 0));
  if (!B.isValid() && !LiteralOr)
    return E;
  const SymOp BOp = B.isValid() ? Ctx.op(B) : SymOp::Const;
  const bool BitwiseTypes = (AOp == SymOp::Xor && BOp == SymOp::Xor) ||
                            (AOp == SymOp::Or && BOp == SymOp::And) ||
                            (AOp == SymOp::And && BOp == SymOp::Or);
  const bool BitwisePair =
      BitwiseTypes &&
      Ctx.numOperands(A) >
          ::neverd::symbolic::detail::limits::
              kSymMBATryFastComplementarySumCtxNumOperandsALimit &&
      Ctx.numOperands(A) == Ctx.numOperands(B) &&
      (llvm::any_of(Ctx.operands(A),
                    [&](SymRef R) { return Ctx.op(R) == SymOp::Not; }) ||
       llvm::any_of(Ctx.operands(B),
                    [&](SymRef R) { return Ctx.op(R) == SymOp::Not; }));
  const bool MaskedDifferencePair =
      BitwiseTypes && AOp != SymOp::Xor && Ctx.numOperands(A) == 2 &&
      Ctx.numOperands(B) == 2 && llvm::any_of(Ctx.operands(A), [&](SymRef R) {
        return Ctx.isConst(R) && llvm::is_contained(Ctx.operands(B), R);
      });
  bool SharedPartition = false;
  const bool PartitionTypes = (AOp == SymOp::And && BOp == SymOp::And) ||
                              (AOp == SymOp::Or && BOp == SymOp::Or);
  if (PartitionTypes) {
    llvm::ArrayRef<SymRef> AF = Ctx.operands(A), BF = Ctx.operands(B);
    const bool SmallPair = AF.size() == 2 && BF.size() == 2;
    const SymOp Dual = AOp == SymOp::And ? SymOp::Or : SymOp::And;
    const bool FlattenedPair =
        (AF.size() > 8 || BF.size() > 8) &&
        (llvm::any_of(AF, [&](SymRef R) { return Ctx.op(R) == Dual; }) ||
         llvm::any_of(BF, [&](SymRef R) { return Ctx.op(R) == Dual; }));
    if (SmallPair || FlattenedPair) {
      size_t I = 0, J = 0;
      while (I < AF.size() && J < BF.size()) {
        if (AF[I] == BF[J]) {
          SharedPartition = true;
          break;
        }
        AF[I] < BF[J] ? ++I : ++J;
      }
    }
  }
  if (!(B.isValid() && isDirectComplement(Ctx, A, B)) && !BitwisePair &&
      !SharedPartition && !MaskedDifferencePair && !LiteralOr)
    return E;

  // This is a possible complement or literal-mask difference. Bound its
  // scratch before considering it, including on the deep walk's first visit.
  const size_t Nodes = Ctx.dagSize(E);
  if (Nodes > Opts.MaxTableBytes / 64 ||
      Nodes > std::numeric_limits<size_t>::max() / 4 ||
      !Budget.canConsume(4 * Nodes))
    return E;
  Budget.consume(4 * Nodes);
  SymRef Fast = foldComplementaryAdd(Ctx, E);
  if (Fast == E || readingScore(Ctx, Fast) >= readingScore(Ctx, E))
    return E;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Fast;
}

} // namespace

SymRef detail::completeComplementarySums(SymContext &Ctx, SymRef Root,
                                         const MBAOptions &Opts,
                                         WorkBudget &Budget) {
  const size_t Cost = readingCost(Ctx, Root);
  if (Cost > ::neverd::symbolic::detail::limits::
                 kSymMBADetailCompleteComplementarySumsMaxNodes ||
      Opts.MaxTableBytes <
          ::neverd::symbolic::detail::limits::
              kSymMBADetailCompleteComplementarySumsOptsMaxTableBytesLimit ||
      !Budget.canConsume(
          2 * ::neverd::symbolic::detail::limits::
                  kSymMBADetailCompleteComplementarySumsMaxNodes +
          ::neverd::symbolic::detail::limits::kSymMBAMaxEdges))
    return Root;
  llvm::SmallVector<uint32_t,
                    ::neverd::symbolic::detail::limits::
                        kSymMBADetailCompleteComplementarySumsMaxNodes>
      Order;
  llvm::SmallVector<SymRef, ::neverd::symbolic::detail::limits::kSymMBAMaxEdges>
      Work{Root};
  size_t Edges = 0;
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    if (llvm::is_contained(Order, R.index()))
      continue;
    if (Order.size() == ::neverd::symbolic::detail::limits::
                            kSymMBADetailCompleteComplementarySumsMaxNodes)
      return Root;
    Order.push_back(R.index());
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ops.size() >
        ::neverd::symbolic::detail::limits::kSymMBAMaxEdges - Edges)
      return Root;
    Edges += Ops.size();
    Work.append(Ops.begin(), Ops.end());
  }
  llvm::sort(Order);
  Budget.consume(2 * Order.size() + Edges);

  bool HasPair = false;
  for (uint32_t Index : Order)
    if (foldComplementaryAdd(Ctx, SymRef(Index)) != SymRef(Index)) {
      HasPair = true;
      break;
    }
  if (!HasPair)
    return Root;

  llvm::DenseMap<uint32_t, SymRef> Done;
  for (uint32_t Index : Order) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> OldOps = Ctx.operands(R);
    llvm::SmallVector<SymRef, 8> NewOps;
    bool Changed = false;
    for (SymRef Child : OldOps) {
      SymRef Replaced = Done.lookup(Child.index());
      if (!Replaced.isValid())
        Replaced = Child;
      Changed |= Replaced != Child;
      NewOps.push_back(Replaced);
    }
    SymRef Rebuilt = Changed ? Ctx.rebuild(R, NewOps) : R;
    SymRef Folded = foldComplementaryAdd(Ctx, Rebuilt);
    if (Folded != R)
      Done[Index] = Folded;
  }
  SymRef Answer = Done.lookup(Root.index());
  return Answer.isValid() && readingScore(Ctx, Answer) < readingScore(Ctx, Root)
             ? Answer
             : Root;
}

namespace {

// Read a small linear tail separately from a large product frontier. Those
// summands remain additive even when the nonlinear terms use the same inputs.
// This is a decomposition of the sum, not an independence assumption.
SymRef simplifyLinearTail(
    SymContext &Ctx, SymRef Root, const MBAOptions &Opts, WorkBudget &Budget,
    SolveReport &Rep,
    size_t MinDegree = detail::limits::kMinLinearTailProductDegree,
    bool SingleProductOnly = false) {
  if (Ctx.op(Root) != SymOp::Add ||
      Ctx.numOperands(Root) > detail::limits::kMaxLinearTailTerms ||
      Budget.exhausted())
    return Root;
  unsigned Higher = 0, LinearCount = 0;
  size_t Nodes = 0;
  auto Unscaled = [&](SymRef Term) {
    return Ctx.op(Term) == SymOp::Mul && Ctx.numOperands(Term) == 2 &&
                   Ctx.isConst(Ctx.operand(Term, 0))
               ? Ctx.operand(Term, 1)
               : Term;
  };
  for (SymRef Term : Ctx.operands(Root)) {
    SymRef Body = Unscaled(Term);
    if (Ctx.op(Body) == SymOp::Mul) {
      Higher +=
          Ctx.numOperands(Body) - unsigned(Ctx.isConst(Ctx.operand(Body, 0))) >=
          MinDegree;
    } else {
      const size_t Cost = Ctx.readabilityCost(Body);
      if (Cost > detail::limits::kMaxLinearTailNodes - Nodes)
        return Root;
      Nodes += Cost;
      ++LinearCount;
    }
  }
  if (Higher < detail::limits::kMinLinearTailProducts ||
      (SingleProductOnly && Higher != 1) ||
      LinearCount < detail::limits::kMinLinearTailSummands)
    return Root;
  const size_t Count = Ctx.numOperands(Root);
  const size_t Words = (size_t(Ctx.width(Root)) - 1) / 64 + 1;
  size_t Storage = (Nodes + Count) * 128;
  if (Storage > Opts.MaxTableBytes ||
      Words > (Opts.MaxTableBytes - Storage) / (Count * sizeof(uint64_t)))
    return Root;
  Storage += Words * Count * sizeof(uint64_t);
  if (!Budget.consume(Nodes + Count + Words * Count))
    return Root;
  // Own every reference before interning the reduced tail can grow the arena.
  llvm::SmallVector<SymRef, 8> Linear, Other, Pending;
  for (SymRef Term : Ctx.operands(Root)) {
    SymRef Body = Unscaled(Term);
    if (Ctx.op(Body) == SymOp::Mul)
      Other.push_back(Term);
    else {
      Linear.push_back(Term);
      Pending.push_back(Body);
    }
  }
  llvm::SmallVector<SymRef, 4> Inputs;
  while (!Pending.empty()) {
    SymRef R = Pending.pop_back_val();
    if (Ctx.isConst(R))
      continue;
    if (Ctx.isVar(R)) {
      if (!llvm::is_contained(Inputs, R))
        Inputs.push_back(R);
      continue;
    }
    auto Op = Ctx.op(R);
    if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor &&
        Op != SymOp::Not)
      return Root;
    Pending.append(Ctx.operands(R).begin(), Ctx.operands(R).end());
  }
  auto Tail = Ctx.mkAdd(Linear);
  MBAOptions Inner = Opts;
  Inner.MaxTableBytes -= Storage;
  SolveReport Report;
  if (Inputs.size() > detail::limits::kMaxLinearTailInputs) {
    // Preserve the per-region arity bound, but let disjoint small kernels
    // finish before walking the large products that merely share their sum.
    Inner.MaxAtoms = std::min(resolveLimits(Inner).MaxAtoms,
                              detail::limits::kMaxLinearTailInputs);
  }
  const SymRef Reduced = solveOneRegion(Ctx, Tail, Inner, Budget, Report);
  Rep.BudgetExhausted |= Report.BudgetExhausted;
  Rep.NumAtoms = std::max(Rep.NumAtoms, Report.NumAtoms);
  if (Reduced == Tail || Report.Evidence != MBAEvidence::Derivation)
    return Root;
  Other.push_back(Reduced);
  auto Candidate = Ctx.mkAdd(Other);
  return readingScore(Ctx, Candidate) < readingScore(Ctx, Root) ? Candidate
                                                                : Root;
}

SymRef finishCompletedSums(SymContext &Ctx, SymRef Root, const MBAOptions &Opts,
                           WorkBudget &Budget, SolveReport &Rep) {
  SymRef Completed = completeComplementarySums(Ctx, Root, Opts, Budget);
  if (Completed == Root || Budget.exhausted())
    return Completed;

  // Removing a partition can expose a new whole-region relation. Read that
  // region once with the remaining budget, then keep only a strict improvement.
  SolveReport Refined;
  SymRef Candidate = solveRegionOrSplit(Ctx, Completed, Opts, Budget, Refined);
  Rep.BudgetExhausted |= Refined.BudgetExhausted;
  if (readingScore(Ctx, Candidate) >= readingScore(Ctx, Completed))
    return Completed;
  Rep.NumAtoms = std::max(Rep.NumAtoms, Refined.NumAtoms);
  if (Rep.Evidence != MBAEvidence::Samples)
    Rep.Evidence = Refined.Evidence;
  return Candidate;
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
  if (!Budget.exhausted() && readingCost(Ctx, Result.Expr) > 2) {
    SolveReport Factored;
    SymRef Candidate = solveStructuralFactors(Ctx, E, Opts, Budget, Factored);
    if (readingScore(Ctx, Candidate) < readingScore(Ctx, Result.Expr)) {
      Result.Expr = Candidate;
      Rep.NumAtoms = Factored.NumAtoms;
      Rep.Outcome = Factored.Outcome;
      Rep.Evidence = Factored.Evidence;
    }
    Rep.BudgetExhausted |= Factored.BudgetExhausted;
    if (Result.Expr == E && Factored.BudgetExhausted)
      Rep.Outcome = MBAOutcome::BudgetExhausted;
  }
  if (!Budget.exhausted()) {
    SolveReport Final;
    SymRef Factored =
        solveCoefficientFactors(Ctx, Result.Expr, Opts, Budget, Final);
    if (Factored != Result.Expr) {
      Result.Expr = Factored;
      Rep.NumAtoms = std::max(Rep.NumAtoms, Final.NumAtoms);
      Rep.Outcome = MBAOutcome::Rewritten;
      if (Rep.Evidence != MBAEvidence::Samples)
        Rep.Evidence = Final.Evidence;
    } else if (Final.BudgetExhausted && Result.Expr == E) {
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    }
  }
  if (Result.Expr != E && !Budget.exhausted())
    Result.Expr = finishCompletedSums(Ctx, Result.Expr, Opts, Budget, Rep);
  if (Result.Expr != E && !Opts.AllowGrowth &&
      !doesNotGrow(Ctx, Result.Expr, E)) {
    Result.Expr = E;
    Rep.Outcome = Budget.exhausted() ? MBAOutcome::BudgetExhausted
                                     : MBAOutcome::AlreadyShortest;
    Rep.Evidence = MBAEvidence::None;
  }
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

namespace detail {

class MBADeepSimplifier {
public:
  static MBAResult run(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                       MBARewriteCache &Cache, WorkBudget &Budget);
};

} // namespace detail

MBAResult simplifyMBADeep(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBARewriteCache Disabled(Ctx, 0);
  return simplifyMBADeep(Ctx, E, Opts, Disabled);
}

MBAResult simplifyMBADeep(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          MBARewriteCache &Cache) {
  WorkBudget Budget(Opts.MaxWork);
  Budget.MemoOwner = &Ctx;
  Budget.MemoCapacity = Opts.MaxTableBytes /
                        detail::limits::kCompletedMemoStorageDivisor /
                        detail::limits::kCompletedMemoEntryBytes;
  return detail::MBADeepSimplifier::run(Ctx, E, Opts, Cache, Budget);
}

MBAResult detail::MBADeepSimplifier::run(SymContext &Ctx, SymRef E,
                                         const MBAOptions &Opts,
                                         MBARewriteCache &Cache,
                                         WorkBudget &Budget) {
  if (Cache.Owner != &Ctx)
    throw std::invalid_argument("MBA rewrite cache belongs to another context");
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;
  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;
  const size_t WorkStart = Budget.used();
  if (auto Hit = Cache.Rewrites.find(E.index());
      Hit != Cache.Rewrites.end() && Budget.consume()) {
    if ((Opts.AllowGrowth || doesNotGrow(Ctx, Hit->second, E)) &&
        agreeOnSamples(Ctx, E, Hit->second, Opts.VerifySamples)) {
      Result.Expr = Hit->second;
      Result.SizeAfter = readingCost(Ctx, Result.Expr);
      Result.Changed = true;
      Result.Outcome = MBAOutcome::Rewritten;
      Result.Evidence = MBAEvidence::Derivation;
      Result.Work = Budget.used() - WorkStart;
      ++Cache.ReusedRoots;
      return Result;
    }
  }
  SolveReport FastRep;
  if (SymRef Fast = tryFastComplementarySum(Ctx, E, Opts, Budget, FastRep);
      Fast != E) {
    // The complement is read from the root's original words, and the rewrite
    // keeps them verbatim: (M|W)+(M&W) becomes M+W with W still obfuscated.
    // Finish every root rewrite here, then walk the retained words under the
    // same allowance. The nested request finds no root rewrite left, so it
    // does not recurse again.
    for (SymRef Next; (Next = tryFastComplementarySum(Ctx, Fast, Opts, Budget,
                                                      FastRep)) != Fast;)
      Fast = Next;
    MBAOptions Inner = Opts;
    Inner.MaxWork = Budget.unlimited()
                        ? Opts.MaxWork
                        : Opts.MaxWork - (Budget.used() - WorkStart);
    const MBAResult Words = run(Ctx, Fast, Inner, Cache, Budget);
    Result.Expr = Words.Expr;
    Result.SizeAfter = readingCost(Ctx, Words.Expr);
    Result.Changed = true;
    Result.NumAtoms = Words.NumAtoms;
    Result.Outcome = MBAOutcome::Rewritten;
    Result.Evidence = Words.Evidence == MBAEvidence::Samples
                          ? MBAEvidence::Samples
                          : MBAEvidence::Derivation;
    Result.Work = Budget.used() - WorkStart;
    if (Result.Evidence == MBAEvidence::Derivation &&
        Words.Outcome != MBAOutcome::BudgetExhausted && !Budget.exhausted() &&
        Cache.Rewrites.size() < Cache.Capacity &&
        readingScore(Ctx, Words.Expr) < readingScore(Ctx, E))
      Cache.Rewrites.try_emplace(E.index(), Words.Expr);
    return Result;
  }

  // Large homogeneous blocks can each contain a different sparse partition.
  // Finish their local rewrites before composing the sum. Otherwise extracting
  // common powers across degrees makes every parent revisit the other blocks.
  // All recursive requests share this caller's remaining work allowance.
  if (Ctx.op(E) == SymOp::Add &&
      Ctx.numOperands(E) <= detail::limits::kProductCofactorMaxTerms) {
    std::array<std::pair<unsigned, unsigned>,
               detail::limits::kProductCofactorMaxTerms>
        Counts{};
    size_t Groups = 0, Edges = 0;
    bool Flat = true;
    auto Degree = [&](SymRef T) -> unsigned {
      if (Ctx.op(T) != SymOp::Mul)
        return 0;
      const auto F = Ctx.operands(T);
      const auto D = F.size() - unsigned(Ctx.isConst(F.front()));
      return D >= detail::limits::kMinCompletedProductWalkDegree ? D : 0;
    };
    for (SymRef T : Ctx.operands(E)) {
      const unsigned D = Degree(T);
      if (!D)
        continue;
      const auto F = Ctx.operands(T);
      if (F.size() > detail::limits::kProductCofactorMaxEdges - Edges) {
        Flat = false;
        break;
      }
      Edges += F.size();
      Flat &= llvm::none_of(F, [&](SymRef R) {
        return Ctx.op(R) == SymOp::Add || Ctx.op(R) == SymOp::Mul;
      });
      size_t I = 0;
      while (I < Groups && Counts[I].first != D)
        ++I;
      if (I == Groups)
        Counts[Groups++] = {D, 0};
      ++Counts[I].second;
    }
    if (Flat && Groups >= 2 &&
        llvm::all_of(llvm::ArrayRef(Counts).take_front(Groups), [](auto Entry) {
          return Entry.second >=
                 detail::limits::kProductCofactorMinCompletionTerms;
        })) {
      const size_t Bytes =
          256 * Ctx.numOperands(E) + Edges * 4 * sizeof(SymRef);
      if (Bytes <= Opts.MaxTableBytes &&
          Budget.consume(Ctx.numOperands(E) + Edges)) {
        std::map<unsigned, llvm::SmallVector<SymRef, 16>> Parts;
        for (SymRef T : Ctx.operands(E))
          Parts[Degree(T)].push_back(T);
        llvm::SmallVector<SymRef, 8> Results;
        bool Sampled = false, Incomplete = false;
        for (const auto &[D, Terms] : Parts) {
          const SymRef Part = Ctx.mkAdd(Terms);
          MBAOptions Inner = Opts;
          Inner.MaxWork = Budget.unlimited()
                              ? Opts.MaxWork
                              : Opts.MaxWork - (Budget.used() - WorkStart);
          Inner.MaxTableBytes -= Bytes;
          const auto R = run(Ctx, Part, Inner, Cache, Budget);
          Incomplete |= R.Outcome == MBAOutcome::BudgetExhausted ||
                        (!Budget.unlimited() && R.Work == Inner.MaxWork);
          Sampled |= R.Evidence == MBAEvidence::Samples;
          Result.NumAtoms = std::max(Result.NumAtoms, R.NumAtoms);
          Results.push_back(R.Expr);
        }
        SymRef Combined = Ctx.mkAdd(Results);
        SolveReport Final;
        Combined = solveArithmetic(Ctx, Combined, Opts, Budget, Final, false);
        Incomplete |= Final.BudgetExhausted;
        Result.NumAtoms = std::max(Result.NumAtoms, Final.NumAtoms);
        Result.Work = Budget.used() - WorkStart;
        if (readingScore(Ctx, Combined) < readingScore(Ctx, E) &&
            agreeOnSamples(Ctx, E, Combined, Opts.VerifySamples)) {
          Result.Expr = Combined;
          Result.SizeAfter = readingCost(Ctx, Combined);
          Result.Changed = true;
          Result.Outcome = MBAOutcome::Rewritten;
          Result.Evidence =
              Sampled ? MBAEvidence::Samples : MBAEvidence::Derivation;
          if (!Sampled && !Incomplete && Cache.Rewrites.size() < Cache.Capacity)
            Cache.Rewrites.try_emplace(E.index(), Combined);
          return Result;
        }
      }
    }
  }

  // Recover a shared masked word before visiting its many independently
  // obfuscated uses. The proof is complete even for opaque arithmetic words;
  // the normal walk still simplifies their children and any remaining terms.
  SolveReport WholeRep;
  size_t ValuationStorage = Opts.MaxTableBytes;
  const size_t ResidueAttempts = Budget.MultipleResidueAttempts;
  // With only one high-degree product, preserve its independent linear tail
  // before an incomplete proof can spend the allowance. Multiple high-degree
  // products retain their joint arithmetic search first.
  const auto EarlyTail =
      simplifyLinearTail(Ctx, E, Opts, Budget, WholeRep,
                         detail::limits::kMinLinearTailProductDegree,
                         /*SingleProductOnly=*/true);
  SymRef Prepared =
      foldProductValuations(Ctx, EarlyTail, Budget, ValuationStorage);
  if (EarlyTail == E && Budget.MultipleResidueAttempts != ResidueAttempts &&
      !Budget.exhausted()) {
    // Retain the cheap linear tail before walking a newly synthesized product
    // reading. Its generated bit selectors may no longer expose the original
    // high-degree boundary used to recognize that independent tail.
    auto Tail = simplifyLinearTail(Ctx, E, Opts, Budget, WholeRep,
                                   detail::limits::kMinProductCofactorDegree);
    if (Tail != E) {
      Tail = foldProductValuations(Ctx, Tail, Budget, ValuationStorage);
      if (readingScore(Ctx, Tail) < readingScore(Ctx, Prepared))
        Prepared = Tail;
    }
  }
  SymRef WalkRoot = solveWeightedMasks(Ctx, Prepared, Opts, Budget, WholeRep);
  bool ProductCofactors = false;
  if (WalkRoot == Prepared) {
    WalkRoot = solveProductCofactors(Ctx, Prepared, Opts, Budget, WholeRep);
    ProductCofactors = WalkRoot != Prepared;
  }
  if (WalkRoot == Prepared)
    WalkRoot = solveExpandedProducts(Ctx, Prepared, Opts, Budget, WholeRep);
  if (ProductCofactors && Ctx.op(WalkRoot) == SymOp::Add &&
      Ctx.numOperands(WalkRoot) <= detail::limits::kMaxCompletedProductTerms &&
      128 + Ctx.numOperands(WalkRoot) * 2 * sizeof(SymRef) <=
          Opts.MaxTableBytes) {
    llvm::SmallVector<SymRef, 16> Products, Tail;
    size_t Storage = Opts.MaxTableBytes - 128 -
                     Ctx.numOperands(WalkRoot) * 2 * sizeof(SymRef);
    for (SymRef Term : Ctx.operands(WalkRoot)) {
      const bool Higher =
          Ctx.op(Term) == SymOp::Mul &&
          Ctx.numOperands(Term) - unsigned(Ctx.isConst(Ctx.operand(Term, 0))) >=
              detail::limits::kMinProductCofactorDegree;
      if (Higher)
        Term = foldProductParity(Ctx, Term, Budget, Storage);
      (Higher ? Products : Tail).push_back(Term);
    }
    if (Products.size() >= 2 && !Tail.empty() &&
        Budget.consume(Products.size() + Tail.size())) {
      const SymRef Frontier = Ctx.mkAdd(Products);
      SolveReport Local;
      SymRef Compact =
          solveArithmetic(Ctx, Frontier, Opts, Budget, Local, false);
      WholeRep.BudgetExhausted |= Local.BudgetExhausted;
      auto SharedMonomial = [&](SymRef R) {
        return Ctx.op(R) == SymOp::Mul &&
               Ctx.numOperands(R) - unsigned(Ctx.isConst(Ctx.operand(R, 0))) >=
                   detail::limits::kMinProductCofactorDegree &&
               llvm::any_of(Ctx.operands(R),
                            [&](SymRef F) { return Ctx.op(F) == SymOp::Add; });
      };
      const bool HasCommon =
          SharedMonomial(Compact) ||
          (Ctx.op(Compact) == SymOp::Add &&
           llvm::any_of(Ctx.operands(Compact), SharedMonomial));
      if (HasCommon &&
          readingScore(Ctx, Compact) < readingScore(Ctx, Frontier)) {
        Tail.push_back(Compact);
        WalkRoot = Ctx.mkAdd(Tail);
      }
    }
  }
  SymRef WholeAlternative = WalkRoot;
  if (WalkRoot != E && !Budget.exhausted()) {
    // Preserve enclosing common factors before child readings can hide them.
    // Cofactor recovery can leave distinct coefficient differences; retaining
    // complements as exact atoms avoids expanding their shared product tails.
    SolveReport Factored;
    unsigned HigherProducts = 0;
    if (ProductCofactors && Ctx.op(WalkRoot) == SymOp::Add)
      for (SymRef Term : Ctx.operands(WalkRoot))
        if (Ctx.op(Term) == SymOp::Mul &&
            Ctx.numOperands(Term) -
                    unsigned(Ctx.isConst(Ctx.operand(Term, 0))) >=
                4 &&
            ++HigherProducts == 2)
          break;
    WholeAlternative =
        HigherProducts >= 2
            ? solveArithmetic(Ctx, WalkRoot, Opts, Budget, Factored,
                              /*ExpandComplements=*/false)
            : solveCoefficientFactors(Ctx, WalkRoot, Opts, Budget, Factored);
    WholeRep.BudgetExhausted |= Factored.BudgetExhausted;
    WholeAlternative =
        simplifyLinearTail(Ctx, WholeAlternative, Opts, Budget, WholeRep);

    // A long completed product can otherwise be distributed and factored
    // again at every parent in the deep walk. Visit its checked compact form;
    // short products retain their original spelling for local identities.
    auto LongProduct = [&](SymRef R) {
      return Ctx.op(R) == SymOp::Mul &&
             Ctx.numOperands(R) - unsigned(Ctx.isConst(Ctx.operand(R, 0))) >=
                 detail::limits::kMinCompletedProductWalkDegree;
    };
    if (ProductCofactors &&
        readingScore(Ctx, WholeAlternative) < readingScore(Ctx, WalkRoot) &&
        (LongProduct(WalkRoot) ||
         (Ctx.op(WalkRoot) == SymOp::Add &&
          Ctx.numOperands(WalkRoot) <=
              detail::limits::kMaxCompletedProductTerms &&
          llvm::any_of(Ctx.operands(WalkRoot), LongProduct))))
      WalkRoot = WholeAlternative;
  }

  // Walking children before parents means that by the time a node is reached,
  // everything below it has already been shortened, so each layer is measured
  // over the result of the one beneath rather than over the obfuscation that
  // was wrapped around it.  It also keeps the walk iterative: obfuscated
  // expressions nest deeply enough that recursion is a real hazard.
  const std::vector<uint32_t> Order = reachableInOrder(Ctx, WalkRoot);
  llvm::DenseMap<uint32_t, SymRef> Solved;
  CompletedRegions Regions;
  bool Skipped = WholeRep.BudgetExhausted;
  size_t RefinementStorage = Opts.MaxTableBytes;
  SymKnownBits Known(Ctx);
  std::vector<std::pair<uint32_t, SymRef>> PendingRewrites;
  bool Reused = false;
  llvm::DenseMap<uint32_t, bool> PreservedReadings;
  SymRef RebuiltInput;
  bool PreservedChildren = false;
  auto Remember = [&](uint32_t Index, SymRef Value, bool Complete) {
    Solved[Index] = Value;
    if (RefinementStorage >= 64) {
      RefinementStorage -= 64;
      PreservedReadings[Index] =
          Value == SymRef(Index) ||
          (PreservedChildren && Value == RebuiltInput) ||
          (Complete && sameBooleanBoundary(Ctx, SymRef(Index), Value, Budget,
                                           RefinementStorage));
    }
    constexpr size_t CacheEntryBytes = 128;
    if (Complete && Value != SymRef(Index) &&
        PendingRewrites.size() < Cache.Capacity - Cache.Rewrites.size() &&
        RefinementStorage >= CacheEntryBytes && Budget.consume() &&
        readingScore(Ctx, Value) < readingScore(Ctx, SymRef(Index))) {
      RefinementStorage -= CacheEntryBytes;
      PendingRewrites.emplace_back(Index, Value);
    }
    // A replacement root was also visited by this layer. Remember it under
    // its own identity so a later candidate does not measure that same root
    // again merely because it was emitted under another original node.
    constexpr size_t EntryBytes = 128;
    if (Complete && !Budget.exhausted() && !Solved.contains(Value.index()) &&
        RefinementStorage >= EntryBytes && Budget.consume()) {
      RefinementStorage -= EntryBytes;
      Solved.try_emplace(Value.index(), Value);
    }
  };
  // The weakest evidence any layer rested on is what the whole answer rests on,
  // because the layers above were measured over what it produced.
  bool AnySampled = false;

  for (uint32_t Index : Order) {
    SymRef R(Index);
    if (auto Hit = Cache.Rewrites.find(Index);
        Hit != Cache.Rewrites.end() && Budget.consume()) {
      Solved[Index] = Hit->second;
      Reused = true;
      continue;
    }
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);

    SymRef Rebuilt = R;
    PreservedChildren = true;
    if (!Ops.empty()) {
      llvm::SmallVector<SymRef, 8> NewOps;
      NewOps.reserve(Ops.size());
      bool Changed = false;
      for (SymRef C : Ops) {
        SymRef S = Solved.lookup(C.index());
        Changed |= S != C;
        PreservedChildren &= S == C || PreservedReadings.lookup(C.index());
        NewOps.push_back(S);
      }
      if (Changed)
        Rebuilt = Ctx.rebuild(R, NewOps);
    }

    RebuiltInput = Rebuilt;
    if (hasCompletedResidueReading(Ctx, R, Budget, true)) {
      // These nodes were just emitted from an exact observed-word proof.
      // Child replacements are already propagated by the canonical builder;
      // avoid reprojecting every generated bit into another extraction tree.
      Remember(Index, Rebuilt, false);
      continue;
    }
    Rebuilt = foldLiteralXorEquality(Ctx, Rebuilt, Budget, RefinementStorage);
    Rebuilt = foldObservedWindow(Ctx, Rebuilt, Budget, RefinementStorage);
    Rebuilt = foldKnownBits(Ctx, Rebuilt, Known, Budget, RefinementStorage);
    Rebuilt = foldConditionalControls(Ctx, Rebuilt, Budget, RefinementStorage);
    PreservedChildren &= Rebuilt == RebuiltInput;
    if (!canMeasureAtRoot(Ctx, Rebuilt)) {
      Solved[Index] = Rebuilt;
      continue;
    }
    if (Rebuilt == R && hasCompletedResidueReading(Ctx, R, Budget)) {
      // The exact low-word emitter already selected this reading. Keep
      // original opaque inputs eligible for normal analysis and propagate
      // child rewrites without expanding the generated Boolean nodes again.
      Remember(Index, Rebuilt, false);
      continue;
    }

    // Do not compute a subtree size after the budget has gone: the argument to
    // consume() would otherwise repeat the very traversal the exhausted budget
    // is meant to stop paying for at every remaining node.
    bool Complete = false;
    if (!Budget.exhausted() && Budget.consume(Ctx.dagSize(Rebuilt))) {
      if (isMinimalBinaryLeafExpression(Ctx, Rebuilt, Opts)) {
        // The children are leaves and no later factoring or original-region
        // retry can beat this bound. Preserve the budget for larger parents.
        if (Result.Outcome == MBAOutcome::NotApplicable)
          Result.Outcome = MBAOutcome::AlreadyShortest;
        Remember(Index, Rebuilt, true);
        continue;
      }
      const bool ChildrenChanged = Rebuilt != R;
      SolveReport Rep;
      SymRef Measured = solveRegionOrSplit(Ctx, Rebuilt, Opts, Budget, Rep,
                                           &Regions, &RefinementStorage);
      Complete = !Rep.BudgetExhausted;
      if (Measured != Rebuilt) {
        SolveReport Refined;
        Rebuilt = refineCandidate(Ctx, Measured, Opts, Order, Solved, Regions,
                                  Budget, RefinementStorage, Refined);
        Result.NumAtoms = std::max(Result.NumAtoms, Rep.NumAtoms);
        Result.NumAtoms = std::max(Result.NumAtoms, Refined.NumAtoms);
        AnySampled |= Rep.Evidence == MBAEvidence::Samples;
        AnySampled |= Refined.Evidence == MBAEvidence::Samples;
        Skipped |= Refined.BudgetExhausted;
        Complete &= !Refined.BudgetExhausted;
      } else if (Rep.Outcome == MBAOutcome::BudgetExhausted) {
        Skipped = true;
      } else if (Rep.Outcome == MBAOutcome::TooManyInputs &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::TooManyInputs;
      } else if (Rep.Outcome == MBAOutcome::AlreadyShortest &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::AlreadyShortest;
      }
      // A shorter child can hide arithmetic behind an opaque operator. Keep
      // the original region as a second exact reading before discarding it.
      // The main reading gets first use of the shared work budget.
      SolveReport OriginalRep;
      const bool WholeRegion =
          ChildrenChanged &&
          (hasBitwiseInteraction(Ctx, R) || hasBitwiseProductSum(Ctx, R));
      SymRef Original = R;
      if (ChildrenChanged && !PreservedChildren && !Budget.exhausted())
        Original = WholeRegion
                       ? solveRegionOrSplit(Ctx, R, Opts, Budget, OriginalRep,
                                            &Regions, &RefinementStorage)
                       : solveArithmetic(Ctx, R, Opts, Budget, OriginalRep);
      Skipped |= OriginalRep.BudgetExhausted;
      Complete &= !OriginalRep.BudgetExhausted;
      if (Original != R && !Budget.exhausted()) {
        SolveReport Refined;
        Original = refineCandidate(Ctx, Original, Opts, Order, Solved, Regions,
                                   Budget, RefinementStorage, Refined);
        OriginalRep.NumAtoms = std::max(OriginalRep.NumAtoms, Refined.NumAtoms);
        if (Refined.Evidence == MBAEvidence::Samples)
          OriginalRep.Evidence = MBAEvidence::Samples;
        Skipped |= Refined.BudgetExhausted;
        Complete &= !Refined.BudgetExhausted;
      }
      if (Original != R &&
          readingScore(Ctx, Original) < readingScore(Ctx, Rebuilt)) {
        // Arithmetic cancellation may expose a fresh linear MBA. It was not
        // in the original postorder, so finish that region before selecting it.
        SolveReport Refined;
        if (!WholeRegion && !Budget.exhausted())
          Original = solveRegionOrSplit(Ctx, Original, Opts, Budget, Refined,
                                        &Regions, &RefinementStorage);
        Rebuilt = Original;
        Result.NumAtoms =
            std::max({Result.NumAtoms, OriginalRep.NumAtoms, Refined.NumAtoms});
        AnySampled |= Refined.Evidence == MBAEvidence::Samples;
        AnySampled |= OriginalRep.Evidence == MBAEvidence::Samples;
        Skipped |= Refined.BudgetExhausted;
        Complete &= !Refined.BudgetExhausted;
      }
    } else {
      Skipped = true;
    }
    if (!Budget.exhausted() && readingCost(Ctx, Rebuilt) > 2) {
      SolveReport Factored;
      // Use the original node: a child rewrite may already have expanded one
      // occurrence of a complemented factor while leaving its products whole.
      SymRef Candidate = solveStructuralFactors(Ctx, R, Opts, Budget, Factored);
      if (readingScore(Ctx, Candidate) < readingScore(Ctx, Rebuilt)) {
        Rebuilt = Candidate;
        Result.NumAtoms = std::max(Result.NumAtoms, Factored.NumAtoms);
      }
      Skipped |= Factored.BudgetExhausted;
      Complete &= !Factored.BudgetExhausted;
    }
    Remember(Index, Rebuilt, Complete);
  }

  SymRef Out = Solved.lookup(WalkRoot.index());
  if (!Budget.exhausted()) {
    SolveReport Final;
    Out = solveCoefficientFactors(Ctx, Out, Opts, Budget, Final);
    Result.NumAtoms = std::max(Result.NumAtoms, Final.NumAtoms);
    AnySampled |= Final.Evidence == MBAEvidence::Samples;
    Skipped |= Final.BudgetExhausted;
  }
  if (Out != E && !Budget.exhausted()) {
    SolveReport Completed;
    Out = finishCompletedSums(Ctx, Out, Opts, Budget, Completed);
    Result.NumAtoms = std::max(Result.NumAtoms, Completed.NumAtoms);
    AnySampled |= Completed.Evidence == MBAEvidence::Samples;
    Skipped |= Completed.BudgetExhausted;
  }
  if (Out != E && Ctx.width(Out) == 1 &&
      (isPredicate(Ctx.op(Out)) || isBitwise(Ctx.op(Out))) &&
      !Budget.exhausted()) {
    // The local pass re-projects every node without measuring it again. A
    // longer reading would stay in the answer or fail the final size gate
    // and discard the shortened words found by the walk.
    SymRef Finished = finishLocalReadings(Ctx, Out, Budget, RefinementStorage);
    if (doesNotGrow(Ctx, Finished, Out))
      Out = Finished;
  }
  Result.Work = Budget.used() - WorkStart;

  // A locally shorter child can make the composed spelling longer. Retain
  // the independently proved whole-root candidate as an alternative even
  // when subsequent search exhausts its allowance.
  const bool KeptWhole = WalkRoot != E && readingScore(Ctx, WholeAlternative) <
                                              readingScore(Ctx, Out);
  if (KeptWhole)
    Out = WholeAlternative;

  // Every layer refused to grow and every layer was checked on its own, but
  // what the caller receives is the composition of all of them, and that is
  // what its options were about.  Applying both gates once more over the whole
  // rewrite costs a cached cost lookup and the sample count that was asked
  // for, and it is the only place either gate sees the expression the caller
  // actually handed over rather than a layer of it.
  bool Kept = Out != E;
  if (Kept && !Opts.AllowGrowth && !doesNotGrow(Ctx, Out, E))
    Kept = false;
  if (Kept) {
    const bool Verified = agreeOnSamples(Ctx, E, Out, Opts.VerifySamples);
    assert(Verified && "a layered MBA rewrite disagreed with what it replaces");
    Kept = Verified;
  }
  if (!Kept) {
    // Running out of budget with regions left unvisited is the one refusal that
    // says nothing about the expression, so it outranks the others.
    if (Skipped)
      Result.Outcome = MBAOutcome::BudgetExhausted;
    return Result;
  }

  if (!AnySampled) {
    for (auto [Index, Value] : PendingRewrites)
      if (Cache.Rewrites.size() < Cache.Capacity)
        Cache.Rewrites.try_emplace(Index, Value);
    if (WalkRoot != E && !Skipped && !Budget.exhausted() &&
        Cache.Rewrites.size() < Cache.Capacity)
      Cache.Rewrites.try_emplace(E.index(), Out);
    if (Reused)
      ++Cache.ReusedRoots;
  }
  Result.Expr = Out;
  Result.SizeAfter = readingCost(Ctx, Out);
  Result.Changed = true;
  Result.Outcome = MBAOutcome::Rewritten;
  Result.Evidence =
      AnySampled && !KeptWhole ? MBAEvidence::Samples : MBAEvidence::Derivation;
  return Result;
}

} // namespace neverd::symbolic
