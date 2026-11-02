//===- LLVMCBitTests.cpp - Single-bit source predicates ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover a single predicate from integer bit assembly without dropping
/// observable word stores, replaying producers or crossing freeze boundaries.

#include "LLVMCBitTests.h"

#include "neverd/backend/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"

#include <algorithm>
#include <optional>

namespace neverd::llvmc {
namespace {
using namespace llvm;

struct BitValue {
  Value *Source = nullptr;
  unsigned Bit = 0;
  // With no source this is the constant itself; otherwise it negates the bit.
  bool Invert = false;
  bool operator==(const BitValue &) const = default;
};

class BitProjection {
  unsigned &Work;
  unsigned QueryWork = ::neverd::limits::backend::kLLVMCBitTestsQueryWork;
  DenseMap<std::pair<Value *, unsigned>, BitValue> Cache;

public:
  explicit BitProjection(unsigned &Work) : Work(Work) {}

  std::optional<BitValue> get(Value *V, unsigned Bit, unsigned Depth = 0) {
    auto *Type = dyn_cast<IntegerType>(V->getType());
    // The demanded slice is narrow, but its producer can be an x87 i80
    // payload or another wide integer. Follow that bit without inspecting
    // unrelated bits; the same query and depth allowances bound the walk.
    if (!Type || Bit >= Type->getBitWidth())
      return std::nullopt;
    auto Key = std::make_pair(V, Bit);
    if (auto It = Cache.find(Key); It != Cache.end())
      return It->second;
    if (!Work || !QueryWork ||
        Depth > ::neverd::limits::backend::kLLVMCBitTestsGetDepthLimit)
      return std::nullopt;
    --Work;
    --QueryWork;
    auto Result = compute(V, Bit, Depth);
    if (Result)
      Cache.try_emplace(Key, *Result);
    return Result;
  }

private:
  std::optional<BitValue> compute(Value *V, unsigned Bit, unsigned Depth) {
    if (auto *C = dyn_cast<ConstantInt>(V))
      return BitValue{nullptr, 0, C->getValue()[Bit]};
    if (isa<UndefValue, PoisonValue>(V))
      return std::nullopt;
    // A PHI, load, call or freeze is an existing SSA result. Never traverse
    // memory, loop recurrences or the independent choice made by freeze.
    const BitValue Opaque{V, Bit, false};
    // A demanded bit can still be poison because of an overflow, exact-shift
    // or narrowing promise. Projecting through that promise would silently
    // discard an observation of poison in the original value.
    if (auto *I = dyn_cast<Instruction>(V); I && I->hasPoisonGeneratingFlags())
      return Opaque;
    if (V->getType()->isIntegerTy(1))
      return Opaque;
    auto Child = [&](Value *Value, unsigned Index) {
      return get(Value, Index, Depth + 1);
    };
    if (auto *Cast = dyn_cast<CastInst>(V)) {
      if (!Cast->getOperand(0)->getType()->isIntegerTy())
        return Opaque;
      const unsigned Width =
          Cast->getOperand(0)->getType()->getIntegerBitWidth();
      switch (Cast->getOpcode()) {
      case Instruction::ZExt:
        if (Bit >= Width)
          return BitValue{};
        [[fallthrough]];
      case Instruction::Trunc:
        return Child(Cast->getOperand(0), Bit);
      case Instruction::SExt:
        return Child(Cast->getOperand(0), std::min(Bit, Width - 1));
      default:
        return Opaque;
      }
    }
    if (auto *Select = dyn_cast<SelectInst>(V)) {
      auto T = Child(Select->getTrueValue(), Bit);
      auto F = Child(Select->getFalseValue(), Bit);
      if (!T || !F)
        return std::nullopt;
      if (*T == *F)
        return T;
      if (!T->Source && !F->Source)
        return BitValue{Select->getCondition(), 0, F->Invert};
      return Opaque;
    }
    auto *Op = dyn_cast<BinaryOperator>(V);
    if (!Op)
      return Opaque;
    const unsigned Width = V->getType()->getIntegerBitWidth();
    if (Op->isShift()) {
      auto *Amount = dyn_cast<ConstantInt>(Op->getOperand(1));
      if (!Amount || Amount->getValue().uge(Width))
        return Opaque;
      const unsigned Shift = unsigned(Amount->getZExtValue());
      if (Op->getOpcode() == Instruction::Shl)
        return Bit < Shift ? std::optional(BitValue{})
                           : Child(Op->getOperand(0), Bit - Shift);
      if (Bit + Shift < Width)
        return Child(Op->getOperand(0), Bit + Shift);
      return Op->getOpcode() == Instruction::AShr
                 ? Child(Op->getOperand(0), Width - 1)
                 : std::optional(BitValue{});
    }
    if (Op->getOpcode() != Instruction::And &&
        Op->getOpcode() != Instruction::Or &&
        Op->getOpcode() != Instruction::Xor)
      return Opaque;
    auto L = Child(Op->getOperand(0), Bit), R = Child(Op->getOperand(1), Bit);
    if (!L || !R)
      return std::nullopt;
    if (L->Source && !R->Source)
      std::swap(L, R);
    if (!L->Source) {
      if (Op->getOpcode() == Instruction::And)
        return L->Invert ? R : L;
      if (Op->getOpcode() == Instruction::Or)
        return L->Invert ? L : R;
      R->Invert ^= L->Invert;
      return R;
    }
    if (L->Source == R->Source && L->Bit == R->Bit) {
      if (Op->getOpcode() == Instruction::Xor)
        return BitValue{nullptr, 0, L->Invert != R->Invert};
      if (L->Invert == R->Invert)
        return L;
      return BitValue{nullptr, 0, Op->getOpcode() == Instruction::Or};
    }
    return Opaque;
  }
};
} // namespace

bool simplifyIntegerSlices(llvm::Function &Function, unsigned WorkLimit) {
  using namespace llvm;
  SmallVector<TruncInst *, 32> Slices;
  for (auto &I : instructions(Function)) {
    if (!WorkLimit)
      return false;
    --WorkLimit;
    if (auto *Slice = dyn_cast<TruncInst>(&I);
        Slice && !Slice->hasPoisonGeneratingFlags() &&
        Slice->getType()->isIntegerTy() &&
        Slice->getType()->getIntegerBitWidth() <=
            ::neverd::limits::backend::kLLVMCIntegerSliceMaxBits)
      Slices.push_back(Slice);
  }
  bool Changed = false;
  for (auto *Slice : Slices) {
    Value *Root = Slice->getOperand(0);
    // An opaque whole value already has the same narrow cast. Only these
    // producers can expose a different source or discard unobserved bits.
    if (!isa<BinaryOperator, CastInst, SelectInst, ConstantInt>(Root))
      continue;
    const unsigned Width = Slice->getType()->getIntegerBitWidth();
    BitProjection Project(WorkLimit);
    Value *Source = nullptr;
    std::optional<int> Offset;
    APInt SourceMask(Width, 0), Invert(Width, 0), Ones(Width, 0);
    bool Complete = true;
    for (unsigned Bit = 0; Bit < Width; ++Bit) {
      auto Part = Project.get(Root, Bit);
      if (!Part) {
        Complete = false;
        break;
      }
      if (!Part->Source) {
        if (Part->Invert)
          Ones.setBit(Bit);
        continue;
      }
      const int PartOffset =
          static_cast<int>(Part->Bit) - static_cast<int>(Bit);
      if ((Source && Source != Part->Source) ||
          (Offset && *Offset != PartOffset)) {
        Complete = false;
        break;
      }
      Source = Part->Source;
      Offset = PartOffset;
      SourceMask.setBit(Bit);
      if (Part->Invert)
        Invert.setBit(Bit);
    }
    if (!Complete || (Source == Root && Offset == 0 && SourceMask.isAllOnes() &&
                      Invert.isZero() && Ones.isZero()))
      continue;
    IRBuilder<> Builder(Slice);
    Builder.SetCurrentDebugLocation(Slice->getDebugLoc());
    Value *Result = ConstantInt::get(Slice->getType(), Ones);
    if (Source) {
      Result = Source;
      if (*Offset > 0)
        Result = Builder.CreateLShr(Result, *Offset);
      Result = Builder.CreateZExtOrTrunc(Result, Slice->getType());
      if (*Offset < 0)
        Result = Builder.CreateShl(Result, -*Offset);
      if (!SourceMask.isAllOnes())
        Result = Builder.CreateAnd(Result, SourceMask);
      if (!Invert.isZero())
        Result = Builder.CreateXor(Result, Invert);
      if (!Ones.isZero())
        Result = Builder.CreateOr(Result, Ones);
    }
    Slice->replaceAllUsesWith(Result);
    Slice->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

bool simplifyBitTests(llvm::Function &Function, unsigned WorkLimit) {
  using namespace llvm;
  SmallVector<Instruction *, 32> Tests;
  for (auto &I : instructions(Function)) {
    if (!WorkLimit)
      return false;
    --WorkLimit;
    if (auto *Cmp = dyn_cast<ICmpInst>(&I); Cmp && Cmp->isEquality())
      Tests.push_back(Cmp);
    else if (isa<TruncInst>(I) && I.getType()->isIntegerTy(1) &&
             !I.hasPoisonGeneratingFlags())
      Tests.push_back(&I);
  }
  bool Changed = false;
  for (auto *Test : Tests) {
    Value *Root = Test->getOperand(0);
    unsigned Bit = 0;
    bool Invert = false;
    if (auto *Cmp = dyn_cast<ICmpInst>(Test)) {
      auto *Compared = dyn_cast<ConstantInt>(Cmp->getOperand(1));
      if (!Compared) {
        Compared = dyn_cast<ConstantInt>(Root);
        Root = Cmp->getOperand(1);
      }
      auto *And = dyn_cast<BinaryOperator>(Root);
      if (!Compared || !And || And->getOpcode() != Instruction::And)
        continue;
      auto *Mask = dyn_cast<ConstantInt>(And->getOperand(1));
      Root = And->getOperand(0);
      if (!Mask) {
        Mask = dyn_cast<ConstantInt>(Root);
        Root = And->getOperand(1);
      }
      if (!Mask || !Mask->getValue().isPowerOf2() ||
          (!Compared->isZero() && Compared->getValue() != Mask->getValue()))
        continue;
      Bit = Mask->getValue().countr_zero();
      Invert = (Cmp->getPredicate() == CmpInst::ICMP_EQ) == Compared->isZero();
    }
    BitProjection Project(WorkLimit);
    auto Source = Project.get(Root, Bit);
    if (!Source ||
        (Source->Source == Root && Source->Bit == Bit && !Source->Invert))
      continue;
    IRBuilder<> B(Test);
    B.SetCurrentDebugLocation(Test->getDebugLoc());
    Value *Result;
    Invert ^= Source->Invert;
    if (!Source->Source) {
      Result = B.getInt1(Invert);
    } else if (Source->Source->getType()->isIntegerTy(1)) {
      Result = Source->Source;
      if (Invert)
        if (auto *Cmp = dyn_cast<ICmpInst>(Result)) {
          Result = B.CreateICmp(Cmp->getInversePredicate(), Cmp->getOperand(0),
                                Cmp->getOperand(1), "bit.predicate");
          Invert = false;
        }
      if (Invert)
        Result = B.CreateNot(Result, "bit.predicate");
    } else {
      const unsigned Width = Source->Source->getType()->getIntegerBitWidth();
      auto *Zero = ConstantInt::get(Source->Source->getType(), 0);
      if (Source->Bit == Width - 1) {
        Result = B.CreateICmp(Invert ? CmpInst::ICMP_SGE : CmpInst::ICMP_SLT,
                              Source->Source, Zero, "bit.predicate");
      } else {
        auto *Mask = ConstantInt::get(Source->Source->getType(),
                                      APInt::getOneBitSet(Width, Source->Bit));
        Result = B.CreateICmp(Invert ? CmpInst::ICMP_EQ : CmpInst::ICMP_NE,
                              B.CreateAnd(Source->Source, Mask), Zero,
                              "bit.predicate");
      }
    }
    Test->replaceAllUsesWith(Result);
    Test->eraseFromParent();
    Changed = true;
  }
  return Changed;
}
} // namespace neverd::llvmc
