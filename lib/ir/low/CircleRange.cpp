//===- CircleRange.cpp - Modular arithmetic range analysis ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CircleRange.h"

#include <algorithm>

namespace neverd {

//===----------------------------------------------------------------------===//
// Core operations
//===----------------------------------------------------------------------===//

void CircleRange::normalize() {
  if (Left == Right) {
    if (Step != 1)
      Left = Left % Step;
    else
      Left = 0;
    Right = Left;
  }
}

void CircleRange::setRange(uint64_t Lft, uint64_t Rgt, int SizeBytes, int Stp) {
  Mask = calcMask(SizeBytes);
  Left = Lft;
  Right = Rgt;
  Step = Stp;
  Empty = false;
}

void CircleRange::setFull(int SizeBytes) {
  Mask = calcMask(SizeBytes);
  Step = 1;
  Left = 0;
  Right = 0;
  Empty = false;
}

uint64_t CircleRange::getSize() const {
  if (Empty)
    return 0;
  if (Left < Right)
    return (Right - Left) / Step;
  uint64_t Val = (Mask - (Left - Right) + Step) / Step;
  if (Val == 0) {
    Val = Mask;
    if (Step > 1) {
      Val = Val / Step;
      Val += 1;
    }
  }
  return Val;
}

bool CircleRange::contains(uint64_t Val) const {
  if (Empty)
    return false;
  if (Step != 1) {
    if ((Left % Step) != (Val % Step))
      return false;
  }
  if (Left < Right)
    return Val >= Left && Val < Right;
  if (Left > Right) {
    if (Val < Right)
      return true;
    if (Val >= Left)
      return true;
    return false;
  }
  return true; // Full range
}

bool CircleRange::contains(const CircleRange &Other) const {
  if (Empty)
    return Other.Empty;
  if (Other.Empty)
    return true;
  if (isFull())
    return true;
  if (Other.isFull())
    return false;

  if (Step > Other.Step && !Other.isSingle())
    return false;

  if (Left == Other.Left && Right == Other.Right)
    return true;

  bool ThisWraps = (Left >= Right);
  bool OtherWraps = (Other.Left >= Other.Right);

  if (!ThisWraps && !OtherWraps) {
    return Other.Left >= Left && Other.Right <= Right;
  }
  if (ThisWraps && !OtherWraps) {
    return Other.Left >= Left || Other.Right <= Right;
  }
  if (!ThisWraps && OtherWraps) {
    return false;
  }
  // Both wrap
  return Other.Left >= Left && Other.Right <= Right;
}

bool CircleRange::operator==(const CircleRange &O) const {
  if (Empty && O.Empty)
    return true;
  if (Empty != O.Empty)
    return false;
  return Left == O.Left && Right == O.Right && Mask == O.Mask && Step == O.Step;
}

//===----------------------------------------------------------------------===//
// Intersection
//===----------------------------------------------------------------------===//

int CircleRange::intersect(const CircleRange &Other) {
  if (Other.Empty) {
    Empty = true;
    return 2;
  }
  if (Empty)
    return 2;

  int NewStep = std::max(Step, Other.Step);

  if (isFull()) {
    *this = Other;
    Step = NewStep;
    normalize();
    return 0;
  }
  if (Other.isFull()) {
    Step = NewStep;
    normalize();
    return 0;
  }

  bool ThisWraps = (Left >= Right);
  bool OtherWraps = (Other.Left >= Other.Right);

  if (!ThisWraps && !OtherWraps) {
    uint64_t NewLeft = std::max(Left, Other.Left);
    uint64_t NewRight = std::min(Right, Other.Right);
    if (NewLeft >= NewRight) {
      Empty = true;
      return 2;
    }
    Left = NewLeft;
    Right = NewRight;
    Step = NewStep;
    normalize();
    return 0;
  }

  if (ThisWraps && !OtherWraps) {
    bool InUpper = (Other.Left >= Left);
    bool InLower = (Other.Right <= Right);
    if (InUpper) {
      Left = Other.Left;
      Right = Other.Right;
      Step = NewStep;
      normalize();
      return 0;
    }
    if (InLower) {
      Left = Other.Left;
      Right = std::min(Right, Other.Right);
      Step = NewStep;
      normalize();
      return 0;
    }
    Empty = true;
    return 2;
  }

  if (!ThisWraps && OtherWraps) {
    bool InUpper = (Left >= Other.Left);
    bool InLower = (Right <= Other.Right);
    if (InUpper) {
      Step = NewStep;
      normalize();
      return 0;
    }
    if (InLower) {
      Right = std::min(Right, Other.Right);
      Step = NewStep;
      normalize();
      return 0;
    }
    Empty = true;
    return 2;
  }

  // Both wrap: intersection is two segments — pick the tighter one.
  uint64_t NewLeft = std::max(Left, Other.Left);
  uint64_t NewRight = std::min(Right, Other.Right);
  Left = NewLeft;
  Right = NewRight;
  Step = NewStep;
  normalize();
  return 0;
}

//===----------------------------------------------------------------------===//
// Pull-back through operations
//===----------------------------------------------------------------------===//

bool CircleRange::pullBackUnary(NdOp Opc, int InSize, int OutSize) {
  uint64_t InMask = calcMask(InSize);

  switch (Opc) {
  case NdOp::INT_ZEXT: {
    if (InSize >= OutSize)
      return false;
    uint64_t MaxIn = InMask;
    if (!Empty && Left == Right)
      return true; // Full range stays full after zext
    if (!Empty && Right != 0 && Right - 1 > MaxIn) {
      Empty = true;
      return true;
    }
    Mask = InMask;
    if (Right > InMask + 1)
      Right = 0;
    Left &= InMask;
    Right &= InMask;
    normalize();
    return true;
  }
  case NdOp::INT_SEXT: {
    if (InSize >= OutSize)
      return false;
    uint64_t SignBit = uint64_t(1) << (InSize * 8 - 1);
    Mask = InMask;
    Left &= InMask;
    Right &= InMask;
    (void)SignBit;
    normalize();
    return true;
  }
  case NdOp::INT_NEGATE: {
    if (Empty)
      return true;
    if (isFull())
      return true;
    uint64_t NewLeft = (Mask + 1 - Right + 1) & Mask;
    uint64_t NewRight = (Mask + 1 - Left + 1) & Mask;
    Left = NewLeft;
    Right = NewRight;
    return true;
  }
  case NdOp::INT_NEG2: {
    if (Empty)
      return true;
    if (isFull())
      return true;
    uint64_t NewLeft = (Mask + 1 - (Right - 1)) & Mask;
    uint64_t NewRight = (Mask + 1 - Left + 1) & Mask;
    Left = NewLeft;
    Right = NewRight;
    return true;
  }
  case NdOp::COPY:
    return true;
  default:
    return false;
  }
}

bool CircleRange::pullBackBinary(NdOp Opc, uint64_t ConstVal, int Slot,
                                 int InSize, int OutSize) {
  uint64_t InMask = calcMask(InSize);

  switch (Opc) {
  case NdOp::INT_ADD: {
    if (Slot != 0)
      return false;
    Left = (Left - ConstVal) & Mask;
    Right = (Right - ConstVal) & Mask;
    return true;
  }
  case NdOp::INT_SUB: {
    if (Slot == 0) {
      Left = (Left + ConstVal) & Mask;
      Right = (Right + ConstVal) & Mask;
    } else {
      uint64_t NewLeft = (ConstVal - (Right - 1)) & Mask;
      uint64_t NewRight = (ConstVal - Left + 1) & Mask;
      Left = NewLeft;
      Right = NewRight;
    }
    return true;
  }
  case NdOp::INT_AND: {
    if (Slot != 0)
      return false;
    if (ConstVal == 0) {
      if (contains(0))
        *this = CircleRange(uint64_t(0), InSize);
      else
        Empty = true;
      return true;
    }
    if ((ConstVal & (ConstVal + 1)) == 0) {
      uint64_t NewMask = ConstVal;
      if (!Empty && Left == Right) {
        Mask = InMask;
        Left = 0;
        Right = (NewMask + 1) & InMask;
        if (Right == 0 && NewMask == InMask)
          ; // Stay full
        return true;
      }
      Mask = InMask;
      if (Right > NewMask + 1 || Left > NewMask) {
        Left = 0;
        Right = (NewMask + 1) & InMask;
      }
      return true;
    }
    return false;
  }
  case NdOp::INT_LEFT: {
    if (Slot != 0)
      return false;
    if (ConstVal == 0)
      return true;
    if (ConstVal >= 64)
      return false;
    Left >>= ConstVal;
    if (Right != 0)
      Right = ((Right - 1) >> ConstVal) + 1;
    Left &= InMask;
    Right &= InMask;
    normalize();
    return true;
  }
  case NdOp::INT_RIGHT: {
    if (Slot != 0)
      return false;
    if (ConstVal == 0)
      return true;
    if (ConstVal >= 64)
      return false;
    Left <<= ConstVal;
    Right <<= ConstVal;
    Left &= InMask;
    Right &= InMask;
    normalize();
    return true;
  }
  case NdOp::INT_MULT: {
    if (Slot != 0 || ConstVal == 0)
      return false;
    if (Empty)
      return true;
    if (isFull())
      return true;
    if ((ConstVal & (ConstVal - 1)) == 0 && ConstVal > 0) {
      int Shift = 0;
      uint64_t V = ConstVal;
      while (V > 1) {
        V >>= 1;
        ++Shift;
      }
      return pullBackBinary(NdOp::INT_LEFT, uint64_t(Shift), 0, InSize,
                            OutSize);
    }
    return false;
  }
  case NdOp::SUBBYTES: {
    if (ConstVal != 0)
      return false;
    if (InSize <= OutSize)
      return true;
    Left &= InMask;
    Right &= InMask;
    Mask = InMask;
    normalize();
    return true;
  }
  default:
    return false;
  }
}

} // namespace neverd
