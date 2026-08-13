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

#include "neverd/symbolic/SymExpr.h"

#include <algorithm>
#include <cassert>

namespace neverd::symbolic {

//===----------------------------------------------------------------------===//
// Structural
//===----------------------------------------------------------------------===//

SymRef SymContext::mkExtract(SymRef A, uint32_t Low, uint32_t Width) {
  assert(Width > 0 && Low + Width <= width(A) && "extract out of range");

  if (Low == 0 && Width == width(A))
    return A;
  if (isConst(A))
    return mkConst(constValue(A).extractBits(Width, Low));
  // extract(extract(x, l1), l2) == extract(x, l1 + l2)
  if (op(A) == SymOp::Extract)
    return mkExtract(operand(A, 0), static_cast<uint32_t>(node(A).Aux) + Low,
                     Width);
  // Taking the low bits of a widening cast reaches straight through to the
  // original value when the window stays inside it.
  if ((op(A) == SymOp::ZExt || op(A) == SymOp::SExt) && Low == 0 &&
      Width <= width(operand(A, 0)))
    return mkExtract(operand(A, 0), 0, Width);

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
  llvm::SmallVector<SymRef, 8> Flat;
  for (SymRef R : Ops) {
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
        else if ((op(UpperBase) == SymOp::ZExt ||
                  op(UpperBase) == SymOp::SExt) &&
                 UpperLow == LowerBits &&
                 LowerBits <= width(operand(UpperBase, 0)) &&
                 R == mkExtract(operand(UpperBase, 0), 0, LowerBits)) {
          // mkExtract intentionally looks through the low end of a widening
          // cast.  When that operand is itself a concatenation, the low slice
          // may canonicalise all the way to one of its leaves.  Recognise that
          // leaf as the missing low slice so splitting and rejoining the wide
          // value still reconstructs the widening node exactly.
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
