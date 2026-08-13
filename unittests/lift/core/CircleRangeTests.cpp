//===- CircleRangeTests.cpp - Unit tests for CircleRange -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CircleRange.h"
#include "neverd/ir/NdOps.h"

#include <gtest/gtest.h>

using namespace neverd;

class CircleRangeTest : public ::testing::Test {};

TEST_F(CircleRangeTest, EmptyRange) {
  CircleRange R;
  EXPECT_TRUE(R.isEmpty());
  EXPECT_EQ(R.getSize(), 0u);
  EXPECT_FALSE(R.contains(0));
}

TEST_F(CircleRangeTest, SingleValue) {
  CircleRange R(42, 4);
  EXPECT_FALSE(R.isEmpty());
  EXPECT_TRUE(R.isSingle());
  EXPECT_FALSE(R.isFull());
  EXPECT_EQ(R.getSize(), 1u);
  EXPECT_TRUE(R.contains(42));
  EXPECT_FALSE(R.contains(41));
  EXPECT_FALSE(R.contains(43));
}

TEST_F(CircleRangeTest, FullRange) {
  auto R = CircleRange::full(4);
  EXPECT_FALSE(R.isEmpty());
  EXPECT_TRUE(R.isFull());
  EXPECT_EQ(R.getMask(), 0xFFFFFFFFu);
  EXPECT_TRUE(R.contains(0));
  EXPECT_TRUE(R.contains(1000));
}

TEST_F(CircleRangeTest, SimpleRange) {
  CircleRange R(0, 10, 4);
  EXPECT_EQ(R.getSize(), 10u);
  EXPECT_EQ(R.getMin(), 0u);
  EXPECT_EQ(R.getEnd(), 10u);
  EXPECT_TRUE(R.contains(0));
  EXPECT_TRUE(R.contains(9));
  EXPECT_FALSE(R.contains(10));
  EXPECT_FALSE(R.contains(100));
}

TEST_F(CircleRangeTest, WraparoundRange) {
  CircleRange R(250, 5, 1);
  EXPECT_TRUE(R.contains(250));
  EXPECT_TRUE(R.contains(255));
  EXPECT_TRUE(R.contains(0));
  EXPECT_TRUE(R.contains(4));
  EXPECT_FALSE(R.contains(5));
  EXPECT_FALSE(R.contains(100));
}

TEST_F(CircleRangeTest, IntersectDisjoint) {
  CircleRange A(0, 5, 4);
  CircleRange B(10, 20, 4);
  int Code = A.intersect(B);
  EXPECT_EQ(Code, 2);
  EXPECT_TRUE(A.isEmpty());
}

TEST_F(CircleRangeTest, IntersectOverlap) {
  CircleRange A(0, 10, 4);
  CircleRange B(5, 15, 4);
  int Code = A.intersect(B);
  EXPECT_EQ(Code, 0);
  EXPECT_EQ(A.getMin(), 5u);
  EXPECT_EQ(A.getEnd(), 10u);
  EXPECT_EQ(A.getSize(), 5u);
}

TEST_F(CircleRangeTest, IntersectContained) {
  CircleRange A(0, 20, 4);
  CircleRange B(5, 10, 4);
  int Code = A.intersect(B);
  EXPECT_EQ(Code, 0);
  EXPECT_EQ(A.getMin(), 5u);
  EXPECT_EQ(A.getEnd(), 10u);
}

TEST_F(CircleRangeTest, IntersectWithFull) {
  auto A = CircleRange::full(4);
  CircleRange B(5, 10, 4);
  int Code = A.intersect(B);
  EXPECT_EQ(Code, 0);
  EXPECT_EQ(A.getMin(), 5u);
  EXPECT_EQ(A.getEnd(), 10u);
}

TEST_F(CircleRangeTest, ContainsRange) {
  CircleRange Outer(0, 20, 4);
  CircleRange Inner(5, 10, 4);
  EXPECT_TRUE(Outer.contains(Inner));
  EXPECT_FALSE(Inner.contains(Outer));
}

TEST_F(CircleRangeTest, PullBackAdd) {
  CircleRange R(10, 20, 4);
  bool Ok = R.pullBackBinary(NdOp::INT_ADD, 5, 0, 4, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 5u);
  EXPECT_EQ(R.getEnd(), 15u);
}

TEST_F(CircleRangeTest, PullBackSub) {
  CircleRange R(5, 15, 4);
  bool Ok = R.pullBackBinary(NdOp::INT_SUB, 3, 0, 4, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 8u);
  EXPECT_EQ(R.getEnd(), 18u);
}

TEST_F(CircleRangeTest, PullBackZext) {
  CircleRange R(0, 256, 4);
  bool Ok = R.pullBackUnary(NdOp::INT_ZEXT, 1, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMask(), 0xFFu);
}

TEST_F(CircleRangeTest, PullBackAndMask) {
  auto R = CircleRange::full(4);
  bool Ok = R.pullBackBinary(NdOp::INT_AND, 0xFF, 0, 4, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 0u);
  EXPECT_EQ(R.getEnd(), 256u);
}

TEST_F(CircleRangeTest, PullBackCopy) {
  CircleRange R(10, 20, 4);
  bool Ok = R.pullBackUnary(NdOp::COPY, 4, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 10u);
  EXPECT_EQ(R.getEnd(), 20u);
}

TEST_F(CircleRangeTest, IterateRange) {
  CircleRange R(0, 5, 4);
  uint64_t Val = R.getMin();
  int Count = 1;
  while (R.getNext(Val))
    ++Count;
  EXPECT_EQ(Count, 5);
}

TEST_F(CircleRangeTest, EqualityOperator) {
  CircleRange A(0, 10, 4);
  CircleRange B(0, 10, 4);
  CircleRange C(0, 20, 4);
  EXPECT_EQ(A, B);
  EXPECT_FALSE(A == C);
}

TEST_F(CircleRangeTest, PullBackLeftShift) {
  CircleRange R(0, 32, 4);
  bool Ok = R.pullBackBinary(NdOp::INT_LEFT, 2, 0, 4, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 0u);
}

TEST_F(CircleRangeTest, PullBackMult) {
  CircleRange R(0, 32, 4);
  bool Ok = R.pullBackBinary(NdOp::INT_MULT, 4, 0, 4, 4);
  EXPECT_TRUE(Ok);
}

TEST_F(CircleRangeTest, PullBackSubpiece) {
  CircleRange R(0, 256, 4);
  bool Ok = R.pullBackBinary(NdOp::SUBBYTES, 0, 0, 8, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 0u);
}

TEST_F(CircleRangeTest, PullBackSubpieceNonZeroOffset) {
  CircleRange R(0, 256, 4);
  bool Ok = R.pullBackBinary(NdOp::SUBBYTES, 2, 0, 8, 4);
  EXPECT_FALSE(Ok);
}

TEST_F(CircleRangeTest, PullBackAddOffset) {
  CircleRange R(5, 15, 4);
  bool Ok = R.pullBackBinary(NdOp::INT_ADD, 5, 0, 4, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 0u);
  EXPECT_EQ(R.getEnd(), 10u);
}

TEST_F(CircleRangeTest, PullBackZextNarrow) {
  CircleRange R(0, 100, 4);
  bool Ok = R.pullBackUnary(NdOp::INT_ZEXT, 2, 4);
  EXPECT_TRUE(Ok);
  EXPECT_EQ(R.getMin(), 0u);
}

TEST_F(CircleRangeTest, PullBackAndMaskFull) {
  CircleRange R = CircleRange::full(4);
  bool Ok = R.pullBackBinary(NdOp::INT_AND, 0xFF, 0, 4, 4);
  EXPECT_TRUE(Ok);
  EXPECT_LE(R.getSize(), 256u);
}
