//===- CircleRange.h - Modular arithmetic range analysis ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Modular-arithmetic value range analysis for switch variable bound
/// inference.  Represents a half-open interval [left, right) over
/// integers mod 2^n with an optional stride.
///
/// Used during jump-table guard analysis to propagate tight value bounds
/// backward through the computation chain (pullBack).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_CIRCLERANGE_H
#define NEVERD_IR_LOW_CIRCLERANGE_H

#include "neverd/ir/NdOps.h"

#include <cstdint>

namespace neverd {

class CircleRange {
public:
  CircleRange() = default;

  CircleRange(uint64_t Lft, uint64_t Rgt, int SizeBytes, int Stp = 1)
      : Left(Lft), Right(Rgt), Mask(calcMask(SizeBytes)), Step(Stp),
        Empty(false) {
    normalize();
  }

  /// Single-value range.
  CircleRange(uint64_t Val, int SizeBytes)
      : Left(Val), Mask(calcMask(SizeBytes)), Step(1), Empty(false) {
    Right = (Left + 1) & Mask;
  }

  static CircleRange full(int SizeBytes) {
    CircleRange R;
    R.Mask = calcMask(SizeBytes);
    R.Left = 0;
    R.Right = 0;
    R.Step = 1;
    R.Empty = false;
    return R;
  }

  static CircleRange empty() { return CircleRange(); }

  void setRange(uint64_t Lft, uint64_t Rgt, int SizeBytes, int Stp = 1);
  void setFull(int SizeBytes);

  bool isEmpty() const { return Empty; }
  bool isFull() const { return !Empty && Step == 1 && Left == Right; }
  bool isSingle() const { return !Empty && Right == ((Left + Step) & Mask); }

  uint64_t getMin() const { return Left; }
  uint64_t getMax() const { return (Right - Step) & Mask; }
  uint64_t getEnd() const { return Right; }
  uint64_t getMask() const { return Mask; }
  int getStep() const { return Step; }

  uint64_t getSize() const;

  bool contains(uint64_t Val) const;
  bool contains(const CircleRange &Other) const;

  /// Intersect with another range.  Returns 0 on success, 1 if the
  /// result is not representable as a single CircleRange, 2 if empty.
  int intersect(const CircleRange &Other);

  /// Pull this range backward through a unary NdOp.
  bool pullBackUnary(NdOp Opc, int InSize, int OutSize);

  /// Pull this range backward through a binary NdOp with a constant.
  bool pullBackBinary(NdOp Opc, uint64_t ConstVal, int Slot, int InSize,
                      int OutSize);

  bool operator==(const CircleRange &O) const;

  bool getNext(uint64_t &Val) const {
    Val = (Val + Step) & Mask;
    return Val != Right;
  }

private:
  uint64_t Left = 0;
  uint64_t Right = 0;
  uint64_t Mask = 0;
  int Step = 1;
  bool Empty = true;

  static uint64_t calcMask(int SizeBytes) {
    if (SizeBytes >= 8)
      return UINT64_MAX;
    return (uint64_t(1) << (SizeBytes * 8)) - 1;
  }

  void normalize();
};

} // namespace neverd

#endif // NEVERD_IR_LOW_CIRCLERANGE_H
