//===- SymExprStruct.cpp - Structural and predicate builders --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the width-changing builders — extract, concatenate, widen and
/// select — together with the width-1 comparisons.
///
/// The extract/concatenate pair carries more weight than its size suggests.  A
/// byte-addressed machine state takes a word apart on every write and puts it
/// back on every read, so without the slicing and merging laws here every
/// expression such a state produced would drag a concatenation of extracts
/// around the value it actually means.
///
//===----------------------------------------------------------------------===//

#include "SymExprCompare.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>

namespace neverd::symbolic {

namespace {

uint64_t lowWordProjectionKey(SymRef Source, uint32_t Width) {
  return (uint64_t(Width) << 32) | Source.index();
}

size_t lowWordProjectionSlot(uint64_t Key, size_t Size) {
  assert(Size && (Size & (Size - 1)) == 0);
  // Mix both halves of the complete key before selecting a power-of-two slot.
  return llvm::DenseMapInfo<uint64_t>::getHashValue(Key) & (Size - 1);
}

// Compare a constant with the largest value of a zero extension. Bound this
// optional inspection before copying the APInt, and never inspect the source
// expression: its width alone establishes the unsigned range.
std::optional<int> compareConstantWithZExtMaximum(const SymContext &Ctx,
                                                  SymRef Extension,
                                                  SymRef Constant) {
  if (Ctx.op(Extension) != SymOp::ZExt || !Ctx.isConst(Constant) ||
      (Ctx.width(Constant) - 1) / 64 + 1 >
          ::neverd::limits::symbolic::kSymExprExtensionCompareMaxWords)
    return std::nullopt;
  const unsigned Narrow = Ctx.width(Ctx.operand(Extension, 0));
  const llvm::APInt Value = Ctx.constValue(Constant);
  if (Value.getActiveBits() > Narrow)
    return 1;
  return Value.isMask(Narrow) ? 0 : -1;
}

// A product of unsigned narrow values can be computed before their carrier
// extension. Powers of two inside an extension may move out only when the
// inner multiplication cannot wrap. Keep the complete narrow product: a high
// window can observe carries from every bit below it.
SymRef narrowUnsignedProductSlice(SymContext &Ctx, SymRef Product, uint32_t Low,
                                  uint32_t Width) {
  if (Ctx.op(Product) != SymOp::Mul)
    return {};
  const auto Carrier = Ctx.width(Product);
  uint32_t Shift = 0, Required = 0;
  llvm::SmallVector<std::pair<SymRef, uint32_t>, 4> Sources;
  for (SymRef Factor : Ctx.operands(Product)) {
    if (Ctx.isConst(Factor)) {
      const auto &Coefficient = Ctx.constValue(Factor);
      if (!Coefficient.isPowerOf2())
        return {};
      const auto Amount = Coefficient.countr_zero();
      if (Amount > Low - Shift)
        return {};
      Shift += Amount;
      continue;
    }
    if (Ctx.op(Factor) != SymOp::ZExt)
      return {};
    auto Source = Ctx.operand(Factor, 0);
    if (Ctx.op(Source) == SymOp::Mul && Ctx.operands(Source).size() == 2) {
      const auto Coefficient = Ctx.operand(Source, 0);
      const auto Extended = Ctx.operand(Source, 1);
      if (Ctx.isConst(Coefficient) &&
          Ctx.constValue(Coefficient).isPowerOf2() &&
          Ctx.op(Extended) == SymOp::ZExt) {
        const auto Amount = Ctx.constValue(Coefficient).countr_zero();
        const auto Inner = Ctx.operand(Extended, 0);
        if (Ctx.width(Inner) <= Ctx.width(Source) - Amount) {
          if (Amount > Low - Shift)
            return {};
          Source = Inner;
          Shift += Amount;
        }
      }
    }
    uint32_t DropLow = 0;
    if (Ctx.op(Source) == SymOp::Concat) {
      const auto Tail = Ctx.operands(Source).back();
      if (Ctx.isConst(Tail) && Ctx.constValue(Tail).isZero()) {
        DropLow = Ctx.width(Tail);
        if (DropLow > Low - Shift)
          return {};
        Shift += DropLow;
      }
    }
    if (Ctx.width(Source) - DropLow >= Carrier - Required)
      return {};
    Required += Ctx.width(Source) - DropLow;
    Sources.emplace_back(Source, DropLow);
  }
  if (Sources.empty())
    return {};
  const auto Begin = Low - Shift;
  if (Begin >= Required)
    return Ctx.mkZero(Width);
  // No builder is called while the original operand arena is being read.
  llvm::SmallVector<SymRef, 4> Factors;
  for (auto [Source, DropLow] : Sources)
    Factors.push_back(Ctx.mkZExt(
        Ctx.mkExtract(Source, DropLow, Ctx.width(Source) - DropLow), Required));
  auto Narrow = Ctx.mkMul(Factors);
  return Ctx.mkZExt(
      Ctx.mkExtract(Narrow, Begin, std::min(Width, Required - Begin)), Width);
}

// Prove only a constant window, without constructing projected expressions.
// Windows are at most one APInt word. Copying a source constant costs its
// complete word count; repeated DAG visits (including depth-limited ones) and
// inspected concatenation operands also consume the shared work limit.
std::optional<llvm::APInt> constantBitSlice(const SymContext &Ctx, SymRef A,
                                            uint32_t Low, uint32_t Width,
                                            unsigned &Remaining,
                                            unsigned Depth = 0) {
  if (!Remaining)
    return std::nullopt;
  --Remaining;
  if (Depth >=
      ::neverd::limits::symbolic::kSymExprStructConstantBitSliceDepthLimit)
    return std::nullopt;
  if (Ctx.isConst(A)) {
    const uint32_t Words = (Ctx.width(A) - 1) / 64 + 1;
    if (Words > Remaining)
      return std::nullopt;
    Remaining -= Words;
    return Ctx.constValue(A).extractBits(Width, Low);
  }
  const auto Op = Ctx.op(A);
  if (Op == SymOp::Extract)
    return constantBitSlice(Ctx, Ctx.operand(A, 0), Low + Ctx.node(A).Aux,
                            Width, Remaining, Depth + 1);
  if (Op == SymOp::Concat) {
    uint32_t End = Ctx.width(A);
    for (SymRef Child : Ctx.operands(A)) {
      if (!Remaining)
        return std::nullopt;
      --Remaining;
      const uint32_t Start = End - Ctx.width(Child);
      if (Low >= Start && Low < End && Width <= End - Low)
        return constantBitSlice(Ctx, Child, Low - Start, Width, Remaining,
                                Depth + 1);
      End = Start;
    }
    return std::nullopt;
  }
  if (Op == SymOp::ZExt) {
    const SymRef Inner = Ctx.operand(A, 0);
    if (Low >= Ctx.width(Inner))
      return llvm::APInt(Width, 0);
    if (Width <= Ctx.width(Inner) - Low)
      return constantBitSlice(Ctx, Inner, Low, Width, Remaining, Depth + 1);
    return std::nullopt;
  }
  if (Op == SymOp::Not) {
    auto Value = constantBitSlice(Ctx, Ctx.operand(A, 0), Low, Width, Remaining,
                                  Depth + 1);
    return Value ? std::optional<llvm::APInt>(~*Value) : std::nullopt;
  }
  if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
    return std::nullopt;
  llvm::APInt Value =
      Op == SymOp::And ? llvm::APInt::getAllOnes(Width) : llvm::APInt(Width, 0);
  bool Unknown = false;
  for (SymRef Child : Ctx.operands(A)) {
    if (!Remaining)
      return std::nullopt;
    auto Part = constantBitSlice(Ctx, Child, Low, Width, Remaining, Depth + 1);
    if (!Part) {
      Unknown = true;
      continue;
    }
    if (Op == SymOp::And)
      Value &= *Part;
    else if (Op == SymOp::Or)
      Value |= *Part;
    else
      Value ^= *Part;
    // Absorbing values prove the whole window independently of unknown
    // operands. XOR has no such rule: every operand must be established.
    if ((Op == SymOp::And && Value.isZero()) ||
        (Op == SymOp::Or && Value.isAllOnes()))
      return Value;
  }
  return Unknown ? std::nullopt : std::optional<llvm::APInt>(Value);
}

} // namespace

std::optional<llvm::APInt> SymContext::constantWindow(SymRef A, uint32_t Low,
                                                      uint32_t Width) const {
  unsigned Remaining = ::neverd::limits::symbolic::
      kSymExprStructSymContextConstantWindowRemaining;
  return constantWindow(A, Low, Width, Remaining);
}

std::optional<llvm::APInt>
SymContext::constantWindow(SymRef A, uint32_t Low, uint32_t Width,
                           unsigned &WorkBudget) const {
  if (!A || A.index() >= Nodes.size() || !Width || Width > 64 ||
      Low > width(A) || Width > width(A) - Low)
    return std::nullopt;
  const unsigned Available =
      std::min(WorkBudget,
               ::neverd::limits::symbolic::
                   kSymExprStructSymContextConstantWindowAvailableMinArgument2);
  unsigned Remaining = Available;
  auto Result = constantBitSlice(*this, A, Low, Width, Remaining);
  WorkBudget -= Available - Remaining;
  return Result;
}

//===----------------------------------------------------------------------===//
// Structural
//===----------------------------------------------------------------------===//

SymRef SymContext::findLowWordProjection(SymRef Source, uint32_t Width) const {
  // A moved-from context can retain the scalar insertion count, but an empty
  // table never authorizes a lookup. Index zero is a valid completed image.
  if (LowWordProjections.empty())
    return {};
  const uint64_t Key = lowWordProjectionKey(Source, Width);
  const auto &Entry =
      LowWordProjections[lowWordProjectionSlot(Key, LowWordProjections.size())];
  return Entry.Image && Entry.Key == Key ? Entry.Image : SymRef{};
}

void SymContext::retainLowWordProjection(SymRef Source, uint32_t Width,
                                         SymRef Image) {
  assert(Image && Width < width(Source));
  if (LowWordProjections.empty() ||
      (LowWordProjections.size() <
           ::neverd::limits::symbolic::kSymExprStructMaximum &&
       LowWordProjectionInsertions >= LowWordProjections.size())) {
    const size_t Next =
        LowWordProjections.empty()
            ? 32
            : std::min(::neverd::limits::symbolic::kSymExprStructMaximum,
                       2 * LowWordProjections.size());
    std::vector<LowWordProjectionEntry> New(Next);
    for (auto Entry : LowWordProjections)
      if (Entry.Image)
        New[lowWordProjectionSlot(Entry.Key, Next)] = Entry;
    LowWordProjections.swap(New);
    LowWordProjectionInsertions = 0;
    // Growth retains at most 2048 old and 4096 new entries at the same time.
  }
  const uint64_t Key = lowWordProjectionKey(Source, Width);
  auto &Entry =
      LowWordProjections[lowWordProjectionSlot(Key, LowWordProjections.size())];
  assert((!Entry.Image || Entry.Key != Key || Entry.Image == Image) &&
         "completed low-word projection changed in an immutable context");
  if ((!Entry.Image || Entry.Key != Key) &&
      LowWordProjections.size() <
          ::neverd::limits::symbolic::kSymExprStructMaximum)
    ++LowWordProjectionInsertions;
  Entry = {Key, Image};
}

SymRef SymContext::mkExtract(SymRef A, uint32_t Low, uint32_t Width) {
  assert(Width > 0 && Low + Width <= width(A) && "extract out of range");

  if (Low == 0 && Width == width(A))
    return A;
  if (isConst(A))
    return mkConst(constValue(A).extractBits(Width, Low));
  if (Width == 1 && Low == width(A) - 1)
    if (SymRef Predicate = detail::recoverSignComparison(*this, A))
      return Predicate;
  // extract(extract(x, l1), l2) == extract(x, l1 + l2)
  if (op(A) == SymOp::Extract)
    return mkExtract(operand(A, 0), static_cast<uint32_t>(node(A).Aux) + Low,
                     Width);
  // Taking the low bits of a widening cast reaches straight through to the
  // original value when the window stays inside it.
  if ((op(A) == SymOp::ZExt || op(A) == SymOp::SExt) && Low == 0 &&
      Width <= width(operand(A, 0)))
    return mkExtract(operand(A, 0), 0, Width);

  // Projection to the low word is a homomorphism for modular arithmetic and
  // bitwise operations. In particular, the low product of widened operands
  // needs neither their high bits nor the high half of the multiplication.
  // Other slices can observe carries from below; shifts and division can
  // observe bits above the result window. Keep those boundaries intact.
  auto Projects = [](SymOp Op) {
    return Op == SymOp::Add || Op == SymOp::Mul || isBitwise(Op);
  };
  if (Low == 0 && Projects(op(A))) {
    if (SymRef Image = findLowWordProjection(A, Width))
      return Image;
    // A lifted expression can be deep and can share subexpressions. Project
    // each node once, bottom-up, without recursion through the arithmetic DAG.
    llvm::DenseMap<uint32_t, SymRef> Done;
    llvm::SmallVector<std::pair<SymRef, bool>, 32> Work{{A, false}};
    while (!Work.empty()) {
      auto [Current, Expanded] = Work.pop_back_val();
      if (Done.contains(Current.index()))
        continue;
      if (width(Current) == Width) {
        Done[Current.index()] = Current;
        continue;
      }
      const SymOp Op = op(Current);
      // Only arithmetic/bitwise nodes have the same projection at this walk
      // position and at its root. In particular, widening nodes do not.
      if (Projects(Op)) {
        if (SymRef Image = findLowWordProjection(Current, Width)) {
          Done[Current.index()] = Image;
          continue;
        }
      }
      const bool Widen = Op == SymOp::ZExt || Op == SymOp::SExt;
      if (Widen && width(operand(Current, 0)) < Width) {
        SymRef Inner = operand(Current, 0);
        Done[Current.index()] =
            Op == SymOp::ZExt ? mkZExt(Inner, Width) : mkSExt(Inner, Width);
        continue;
      }
      if (!Projects(Op) && !Widen) {
        Done[Current.index()] = mkExtract(Current, 0, Width);
        continue;
      }
      if (!Expanded) {
        Work.emplace_back(Current, true);
        for (SymRef Child : operands(Current))
          if (!Done.contains(Child.index()))
            Work.emplace_back(Child, false);
        continue;
      }
      // Copy handles before interning; builders can grow the operand arena.
      llvm::SmallVector<SymRef, 8> Children;
      for (SymRef Child : operands(Current))
        Children.push_back(Done.lookup(Child.index()));
      SymRef Result;
      switch (Op) {
      case SymOp::Add:
        Result = mkAdd(Children);
        break;
      case SymOp::Mul:
        Result = mkMul(Children);
        break;
      case SymOp::And:
        Result = mkAnd(Children);
        break;
      case SymOp::Or:
        Result = mkOr(Children);
        break;
      case SymOp::Xor:
        Result = mkXor(Children);
        break;
      case SymOp::Not:
        Result = mkNot(Children.front());
        break;
      case SymOp::ZExt:
      case SymOp::SExt:
        Result = Children.front();
        break;
      default:
        llvm_unreachable("unexpected low-word projection operator");
      }
      Done[Current.index()] = Result;
      if (Projects(Op))
        retainLowWordProjection(Current, Width, Result);
    }
    return Done.lookup(A.index());
  }

  if (SymRef Narrow = narrowUnsignedProductSlice(*this, A, Low, Width))
    return Narrow;

  // A slice of a concatenation is a slice of whichever parts it reaches.  This
  // is the companion to the merging mkConcat does, and the pair of them is
  // what lets a byte-addressed machine state exist: without it, reading a wide
  // register and then using a narrow view of it — which is most of what x86
  // does — would leave the whole concatenation behind under an extract.
  if (op(A) == SymOp::Concat) {
    // Operands are stored most significant first; walking from the low end
    // makes the bit range of each one a running total.
    llvm::SmallVector<SymRef, 8> Parts(operands(A).begin(), operands(A).end());
    llvm::SmallVector<SymRef, 8> Kept;
    uint32_t PartLow = 0;
    for (auto It = Parts.rbegin(); It != Parts.rend(); ++It) {
      const uint32_t PartHigh = PartLow + width(*It);
      const uint32_t OverlapLow = std::max(Low, PartLow);
      const uint32_t OverlapHigh = std::min(Low + Width, PartHigh);
      if (OverlapLow < OverlapHigh)
        Kept.push_back(
            mkExtract(*It, OverlapLow - PartLow, OverlapHigh - OverlapLow));
      PartLow = PartHigh;
    }
    if (!Kept.empty()) {
      std::reverse(Kept.begin(), Kept.end());
      return mkConcat(Kept);
    }
  }

  return intern(SymOp::Extract, Width, {A}, Low);
}

SymRef SymContext::mkConcat(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  if (Ops.size() == 1)
    return Ops[0];

  // Concatenation is associative but not commutative, so flatten in order.
  // Copy handles before materializing zero prefixes: callers may pass a view
  // into the operand arena, which interning can grow.
  llvm::SmallVector<SymRef, 8> Inputs(Ops.begin(), Ops.end());
  llvm::SmallVector<SymRef, 8> Flat;
  for (SymRef R : Inputs) {
    if (op(R) == SymOp::ZExt) {
      const auto Inner = operand(R, 0);
      Flat.push_back(mkZero(width(R) - width(Inner)));
      R = Inner;
    }
    if (op(R) == SymOp::Concat) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Flat.append(Sub.begin(), Sub.end());
    } else {
      Flat.push_back(R);
    }
  }

  llvm::SmallVector<SymRef, 8> Merged;
  for (SymRef R : Flat) {
    if (!Merged.empty()) {
      SymRef Prev = Merged.back();

      // Fold runs of adjacent literals, which is how a byte-wise memory
      // model's reads of a known region collapse back into one word.
      if (isConst(Prev) && isConst(R)) {
        llvm::APInt Hi = constValue(Prev);
        llvm::APInt Lo = constValue(R);
        uint32_t NW = Hi.getBitWidth() + Lo.getBitWidth();
        Merged.back() =
            mkConst(Hi.zext(NW).shl(Lo.getBitWidth()) | Lo.zext(NW));
        continue;
      }

      // Adjacent slices of one value are that value's wider slice.  This is
      // what makes a byte-addressed machine state usable: such a state takes a
      // word apart on every write and puts it back on every read, and without
      // this every expression it produced would carry eight extracts and a
      // concatenation around the value it actually means.  Operands run most
      // significant first, so the previous one is the upper slice.
      //
      // Anything can be read as a slice of itself, which is how a value that
      // needed no extract still lines up with one that did.
      auto sliceOf = [&](SymRef N, SymRef &Base, uint32_t &Low,
                         uint32_t &Bits) {
        if (op(N) == SymOp::Extract) {
          Base = operand(N, 0);
          Low = static_cast<uint32_t>(node(N).Aux);
        } else {
          Base = N;
          Low = 0;
        }
        Bits = width(N);
      };

      SymRef UpperBase, LowerBase;
      uint32_t UpperLow = 0, UpperBits = 0, LowerLow = 0, LowerBits = 0;
      sliceOf(Prev, UpperBase, UpperLow, UpperBits);
      sliceOf(R, LowerBase, LowerLow, LowerBits);

      // The two can be slices of one value and still not say so, because
      // taking the low bits of a widening cast is rewritten to take them from
      // what was cast — correct on its own, and enough to stop the halves of a
      // word from recognising each other.  Line them back up.
      if (UpperBase != LowerBase) {
        auto isWideningOf = [&](SymRef Wide, SymRef Narrow) {
          return (op(Wide) == SymOp::ZExt || op(Wide) == SymOp::SExt) &&
                 operand(Wide, 0) == Narrow;
        };
        if (isWideningOf(UpperBase, LowerBase) &&
            LowerLow + LowerBits <= width(LowerBase))
          LowerBase = UpperBase;
        else if (UpperLow == LowerBits && LowerBits < width(UpperBase) &&
                 R == mkExtract(UpperBase, 0, LowerBits)) {
          // A low slice can canonicalize through an extension, concatenation,
          // or modular operation. Recognize its projected form as the missing
          // prefix so a byte-wise register write/read still restores the
          // complete computed word, not a concatenation that hides its value.
          LowerBase = UpperBase;
          LowerLow = 0;
        } else if (isWideningOf(LowerBase, UpperBase) &&
                   UpperLow + UpperBits <= width(UpperBase))
          UpperBase = LowerBase;
      }

      if (UpperBase == LowerBase && UpperLow == LowerLow + LowerBits) {
        // mkExtract collapses a slice that covers the whole value back to the
        // value, so a full round trip leaves nothing behind at all.
        Merged.back() = mkExtract(UpperBase, LowerLow, LowerBits + UpperBits);
        continue;
      }
    }
    Merged.push_back(R);
  }

  if (Merged.size() == 1)
    return Merged[0];

  uint32_t W = 0;
  for (SymRef R : Merged)
    W += width(R);
  // Byte stores can explicitly clear a word's upper bytes. Reconstruct the
  // same value as a zero extension, including after concatenation grouping,
  // so consumers do not depend on how those zero bits were written.
  if (isConst(Merged.front()) && constValue(Merged.front()).isZero())
    return mkZExt(mkConcat(llvm::ArrayRef<SymRef>(Merged).drop_front()), W);
  return intern(SymOp::Concat, W, Merged, 0);
}

SymRef SymContext::mkZExt(SymRef A, uint32_t Width) {
  assert(Width >= width(A) && "zext must not narrow");
  if (Width == width(A))
    return A;
  if (isConst(A))
    return mkConst(constValue(A).zext(Width));
  if (op(A) == SymOp::ZExt)
    return mkZExt(operand(A, 0), Width);
  return intern(SymOp::ZExt, Width, {A}, 0);
}

SymRef SymContext::mkSExt(SymRef A, uint32_t Width) {
  assert(Width >= width(A) && "sext must not narrow");
  if (Width == width(A))
    return A;
  if (isConst(A))
    return mkConst(constValue(A).sext(Width));
  if (op(A) == SymOp::SExt)
    return mkSExt(operand(A, 0), Width);
  return intern(SymOp::SExt, Width, {A}, 0);
}

SymRef SymContext::mkZExtOrTrunc(SymRef A, uint32_t Width) {
  if (Width == width(A))
    return A;
  if (Width < width(A))
    return mkExtract(A, 0, Width);
  return mkZExt(A, Width);
}

SymRef SymContext::mkIte(SymRef C, SymRef T, SymRef E) {
  assert(width(C) == 1 && "an ite condition must be a single bit");
  assert(width(T) == width(E) && "ite arms must share a width");
  if (isConst(C))
    return constValue(C).isZero() ? E : T;
  if (T == E)
    return T;
  // ite(c, 1, 0) is c itself, the shape a lifted setcc produces.
  if (width(T) == 1 && isConst(T) && isConst(E)) {
    if (!constValue(T).isZero() && constValue(E).isZero())
      return C;
    if (constValue(T).isZero() && !constValue(E).isZero())
      return mkNot(C);
  }
  return intern(SymOp::Ite, width(T), {C, T, E}, 0);
}

//===----------------------------------------------------------------------===//
// Predicates
//===----------------------------------------------------------------------===//

SymRef SymContext::mkEq(SymRef A, SymRef B) {
  assert(width(A) == width(B) && "comparison operands must share a width");
  if (A == B)
    return mkTrue();
  if (isConst(A) && isConst(B))
    return constValue(A) == constValue(B) ? mkTrue() : mkFalse();
  // Extension is injective. Recover the original comparison so a byte guard
  // keeps the same identity after a register-width promotion. Inspect only
  // extension roots, never traverse their potentially large source DAGs.
  for (unsigned Step = 0;
       Step != ::neverd::limits::symbolic::kSymExprExtensionEqualityMaxSteps;
       ++Step) {
    if (isConst(A))
      std::swap(A, B);
    const auto Op = op(A);
    if (Op != SymOp::ZExt && Op != SymOp::SExt)
      break;
    const auto Inner = operand(A, 0);
    const unsigned Narrow = width(Inner);
    if (op(B) == Op && width(operand(B, 0)) == Narrow) {
      A = Inner;
      B = operand(B, 0);
      continue;
    }
    if (!isConst(B) ||
        (width(B) - 1) / 64 + 1 >
            ::neverd::limits::symbolic::kSymExprExtensionCompareMaxWords)
      break;
    const llvm::APInt Value = constValue(B);
    if (Op == SymOp::ZExt ? !Value.isIntN(Narrow) : !Value.isSignedIntN(Narrow))
      return mkFalse();
    A = Inner;
    B = mkConst(Value.trunc(Narrow));
  }
  if (A == B)
    return mkTrue();
  // A one-bit value is already its truth predicate. Extension peeling can
  // expose this case from a lifted byte-sized Boolean test against zero.
  if (width(A) == 1) {
    if (isConst(A))
      return isConstZero(A) ? mkNot(B) : B;
    if (isConst(B))
      return isConstZero(B) ? mkNot(A) : A;
  }
  // A comparison of two constant tags selects the original condition (or
  // its complement). Constants are interned at their exact width, so this
  // needs only node identities, even for wide tags or a large condition DAG.
  for (unsigned I = 0; I != 2; ++I) {
    const auto Selected = I ? B : A, Literal = I ? A : B;
    if (op(Selected) != SymOp::Ite || !isConst(Literal))
      continue;
    const auto T = operand(Selected, 1), E = operand(Selected, 2);
    if (!isConst(T) || !isConst(E))
      continue;
    if (Literal == T)
      return operand(Selected, 0);
    if (Literal == E)
      return mkNot(operand(Selected, 0));
    return mkFalse();
  }
  // Equality is commutative, so order the operands for canonicity.
  if (B < A)
    std::swap(A, B);
  return intern(SymOp::Eq, 1, {A, B}, 0);
}

SymRef SymContext::mkNe(SymRef A, SymRef B) { return mkNot(mkEq(A, B)); }

SymRef SymContext::mkUlt(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkFalse();
  if (isConst(A) && isConst(B))
    return constValue(A).ult(constValue(B)) ? mkTrue() : mkFalse();
  // Nothing is below zero, and nothing is at or above the maximum.
  if (isConstZero(B))
    return mkFalse();
  if (auto Order = compareConstantWithZExtMaximum(*this, B, A))
    if (*Order >= 0)
      return mkFalse();
  if (auto Order = compareConstantWithZExtMaximum(*this, A, B))
    if (*Order > 0)
      return mkTrue();
  if (isConstOnes(B) && !isConst(A))
    return mkNe(A, B);
  return intern(SymOp::Ult, 1, {A, B}, 0);
}

SymRef SymContext::mkUle(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkTrue();
  if (isConst(A) && isConst(B))
    return constValue(A).ule(constValue(B)) ? mkTrue() : mkFalse();
  if (isConstZero(A))
    return mkTrue();
  if (auto Order = compareConstantWithZExtMaximum(*this, A, B))
    if (*Order >= 0)
      return mkTrue();
  if (auto Order = compareConstantWithZExtMaximum(*this, B, A))
    if (*Order > 0)
      return mkFalse();
  if (isConstOnes(B))
    return mkTrue();
  return intern(SymOp::Ule, 1, {A, B}, 0);
}

SymRef SymContext::mkSlt(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkFalse();
  if (isConst(A) && isConst(B))
    return constValue(A).slt(constValue(B)) ? mkTrue() : mkFalse();
  // A one-bit signed word orders one before zero.
  if (width(A) == 1)
    return isConstZero(B) ? A : mkUlt(B, A);
  if (isConstZero(B) && isBitwise(op(A))) {
    if (SymRef Predicate = detail::recoverSignComparison(*this, A))
      return Predicate;
  }
  return intern(SymOp::Slt, 1, {A, B}, 0);
}

SymRef SymContext::mkSle(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkTrue();
  if (isConst(A) && isConst(B))
    return constValue(A).sle(constValue(B)) ? mkTrue() : mkFalse();
  return intern(SymOp::Sle, 1, {A, B}, 0);
}

} // namespace neverd::symbolic
