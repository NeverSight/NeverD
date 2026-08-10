//===- LiftCommonTests.cpp - shared lift emission invariants ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/lift/LiftCommon.h"

#include <vector>

using namespace neverd;

namespace {

LowOp emitExtension(NdOp Opcode, uint16_t OutputSize, uint16_t InputSize) {
  std::vector<LowOp> Ops;
  LiftStateBase State(0x1000, 1, Ops);
  State.emit(Opcode, NdVar::tmp(TmpBase, OutputSize),
             {NdVar::tmp(TmpBase + TmpStride, InputSize)});
  EXPECT_EQ(Ops.size(), 1U);
  return Ops.front();
}

} // namespace

TEST(LiftStateBase, EqualWidthZextCanonicalizesToCopy) {
  LowOp Op = emitExtension(NdOp::INT_ZEXT, 1, 1);
  EXPECT_EQ(Op.Opcode, NdOp::COPY);
  EXPECT_EQ(Op.Output.Size, 1U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 1U);
}

TEST(LiftStateBase, EqualWidthSextCanonicalizesToCopy) {
  LowOp Op = emitExtension(NdOp::INT_SEXT, 8, 8);
  EXPECT_EQ(Op.Opcode, NdOp::COPY);
  EXPECT_EQ(Op.Output.Size, 8U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 8U);
}

TEST(LiftStateBase, GenuineZextRemainsExtension) {
  LowOp Op = emitExtension(NdOp::INT_ZEXT, 8, 1);
  EXPECT_EQ(Op.Opcode, NdOp::INT_ZEXT);
  EXPECT_EQ(Op.Output.Size, 8U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 1U);
}

TEST(LiftStateBase, GenuineSextRemainsExtension) {
  LowOp Op = emitExtension(NdOp::INT_SEXT, 8, 1);
  EXPECT_EQ(Op.Opcode, NdOp::INT_SEXT);
  EXPECT_EQ(Op.Output.Size, 8U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 1U);
}

TEST(LiftStateBase, NarrowingExtensionIsFatal) {
  EXPECT_DEATH(
      {
        std::vector<LowOp> Ops;
        LiftStateBase State(0x1000, 1, Ops);
        State.emit(NdOp::INT_ZEXT, NdVar::tmp(TmpBase, 1),
                   {NdVar::tmp(TmpBase + TmpStride, 4)});
      },
      "integer extension input must be narrower than output");
}
