//===- GuardExpressionTests.cpp - Exact guard partitions ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/GuardExpression.h"

#include <algorithm>
#include <random>

using namespace neverd;
using namespace neverd::guard_detail;

namespace {
GuardExprPtr leaf(bool Index, uint16_t Size, uint64_t Value = 0) {
  auto E = std::make_shared<GuardExpr>();
  E->K = Index ? GuardExpr::Kind::Index : GuardExpr::Kind::Constant;
  E->Size = Size;
  E->Constant = Value;
  E->ContainsIndex = Index;
  return E;
}

GuardExprPtr operation(NdOp Op, uint16_t Size,
                       std::vector<GuardExprPtr> Inputs) {
  auto E = std::make_shared<GuardExpr>();
  E->K = GuardExpr::Kind::Operation;
  E->Size = Size;
  E->Opcode = Op;
  for (const auto &Input : Inputs)
    E->ContainsIndex |= Input->ContainsIndex;
  E->Inputs = std::move(Inputs);
  return E;
}

template <typename Evaluate>
void verify(const GuardExprPtr &Root, unsigned Last, Evaluate Truth) {
  std::vector<uint64_t> Points{0, uint64_t(Last) + 1};
  ASSERT_TRUE(guardChangePoints(Root, Last, Points));
  std::sort(Points.begin(), Points.end());
  Points.erase(std::unique(Points.begin(), Points.end()), Points.end());
  for (size_t I = 0; I + 1 < Points.size(); ++I)
    for (uint64_t V = Points[I] + 1; V < Points[I + 1]; ++V)
      ASSERT_EQ(Truth(Points[I]), Truth(V))
          << "sample=" << V << " interval=" << Points[I] << ":"
          << Points[I + 1];
}

bool compare(NdOp Op, uint64_t L, uint64_t R, uint64_t Sign) {
  switch (Op) {
  case NdOp::INT_EQUAL:
    return L == R;
  case NdOp::INT_NOTEQUAL:
    return L != R;
  case NdOp::INT_LESS:
    return L < R;
  case NdOp::INT_LESSEQUAL:
    return L <= R;
  case NdOp::INT_SLESS:
    return (L ^ Sign) < (R ^ Sign);
  case NdOp::INT_SLESSEQUAL:
    return (L ^ Sign) <= (R ^ Sign);
  default:
    ADD_FAILURE() << "unexpected test opcode";
    return false;
  }
}

constexpr NdOp Comparisons[] = {NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL,
                                NdOp::INT_LESS,  NdOp::INT_LESSEQUAL,
                                NdOp::INT_SLESS, NdOp::INT_SLESSEQUAL};

TEST(GuardExpression, AllByteOffsetsAndBoundaryDirectionsKeepTruthConstant) {
  auto X = leaf(true, 1);
  for (unsigned Offset = 0; Offset < 256; ++Offset)
    for (unsigned C : {0u, 1u, 2u, 126u, 127u, 128u, 129u, 254u, 255u})
      for (bool Negative : {false, true})
        for (bool Swap : {false, true})
          for (NdOp Op : Comparisons) {
            auto A = operation(Negative ? NdOp::INT_SUB : NdOp::INT_ADD, 1,
                               {leaf(false, 1, Offset), X});
            auto B = leaf(false, 1, C);
            SCOPED_TRACE(std::to_string(Offset) + ":" + std::to_string(C) +
                         ":" + std::to_string(Negative) + ":" +
                         std::to_string(Swap) + ":" + ndOpName(Op));
            verify(operation(Op, 1,
                             Swap ? std::vector<GuardExprPtr>{B, A}
                                  : std::vector<GuardExprPtr>{A, B}),
                   255, [&](uint64_t V) {
                     const uint8_t Value = Negative ? Offset - V : Offset + V;
                     return compare(Op, Swap ? C : Value, Swap ? Value : C,
                                    128);
                   });
          }
}

TEST(GuardExpression, NestedWideAffineGuardsKeepWrapsSignsAndHoles) {
  std::mt19937_64 RNG(0x84a7b9);
  for (uint16_t Size : {2, 4, 8})
    for (unsigned Trial = 0; Trial != 400; ++Trial) {
      const uint64_t Mask =
          Size == 8 ? UINT64_MAX : (uint64_t(1) << (Size * 8)) - 1;
      const uint64_t Sign = uint64_t(1) << (Size * 8 - 1);
      const uint64_t Offset = RNG(), Outer = RNG();
      const auto Value = [&](uint64_t V) {
        return (Outer - (V + Offset)) & Mask;
      };
      const uint64_t C = (Trial % 3 == 0 ? Value(RNG() % 4097) : RNG()) & Mask;
      const uint64_t D = RNG() % 4097;
      const NdOp Op = Comparisons[RNG() % 6];
      auto X = leaf(true, Size);
      auto A = operation(
          NdOp::INT_SUB, Size,
          {leaf(false, Size, Outer),
           operation(NdOp::INT_ADD, Size, {X, leaf(false, Size, Offset)})});
      auto L = operation(Op, 1, {A, leaf(false, Size, C)});
      auto R =
          operation(NdOp::BOOL_NOT, 1,
                    {operation(NdOp::INT_LESS, 1, {X, leaf(false, Size, D)})});
      const NdOp BoolOps[] = {NdOp::BOOL_AND, NdOp::BOOL_OR, NdOp::BOOL_XOR};
      const unsigned Which = RNG() % 3;
      SCOPED_TRACE(std::to_string(Size) + ":" + std::to_string(Trial));
      verify(operation(BoolOps[Which], 1, {L, R}), 4096, [&](uint64_t V) {
        const bool Left = compare(Op, Value(V), C, Sign), Right = !(V < D);
        return Which == 0   ? Left && Right
               : Which == 1 ? Left || Right
                            : Left != Right;
      });
    }
}

TEST(GuardExpression, PeriodicAndWidthChangingFormsRequireFullReplay) {
  auto X = leaf(true, 4);
  auto Periodic = operation(NdOp::INT_AND, 4, {X, leaf(false, 4, 255)});
  auto Narrow = operation(NdOp::COPY, 1, {X});
  auto Shift = operation(NdOp::INT_RIGHT, 4, {X, leaf(false, 4, 3)});
  for (auto V : {Periodic, Narrow, Shift}) {
    std::vector<uint64_t> Points;
    EXPECT_FALSE(guardChangePoints(
        operation(NdOp::INT_LESS, 1, {V, leaf(false, V->Size, 3)}), 4096,
        Points));
  }
  std::vector<uint64_t> Points;
  EXPECT_FALSE(guardChangePoints(
      operation(NdOp::INT_LESS, 1, {leaf(true, 1), leaf(false, 1, 3)}), 4096,
      Points));
}
} // namespace
