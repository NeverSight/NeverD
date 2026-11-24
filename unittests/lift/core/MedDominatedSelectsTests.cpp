//===- MedDominatedSelectsTests.cpp - Selections a branch decides ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check which selections a dominating conditional branch decides.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/med/MedDominatedSelects.h"
#include "neverd/ir/med/MedIR.h"

#include <vector>

using namespace neverd;
namespace {
MedVar value(int Id, uint16_t Size) {
  MedVar V;
  V.Id = Id;
  V.Size = Size;
  V.SSAVer = 1;
  V.Kind = MedVar::Temp;
  V.TheArch = Arch::X64;
  return V;
}
MedVar entry(int Id) {
  MedVar V = value(Id, 8);
  V.SSAVer = 0;
  V.Kind = MedVar::Reg;
  V.RegOff = static_cast<uint64_t>(Id) * 8;
  return V;
}
MedOp operation(NdOp Opcode, MedVar Output, std::vector<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const MedVar &Input : Inputs)
    Op.addInput(Input);
  return Op;
}
MedOp branch(va_t Target, MedVar Condition) {
  MedOp Op;
  Op.Opcode = NdOp::COND_BR;
  Op.addInput(MedVar::makeConst(Target, 8));
  Op.addInput(Condition);
  return Op;
}
MedOp ret(MedVar Value) {
  MedOp Op;
  Op.Opcode = NdOp::RETURN;
  Op.addInput(Value);
  return Op;
}

/// The shape of `test x; je zero; cmp y, 1; jne other; bsf r, x; ret r`:
///   block 0 (0x1000): z = x == 0; branch to block 4 (0x1040) where z
///   block 1 (0x1010): c = y != 1; branch to block 3 (0x1030) where c
///   block 2 (0x1020): s = Selects; r = Selects ? old : t; ret r
///   block 3 (0x1030): ret y
///   block 4 (0x1040): ret 0
/// With \p Join, block 4 also falls into block 2, which the test's edge then
/// no longer alone enters.
struct Shape {
  MedFunc Func;
  MedVar X = entry(1), Y = entry(2), Old = entry(3);
  MedVar Z = value(10, 1), C = value(11, 1), S = value(12, 1);
  MedVar T = value(13, 8), R = value(14, 8);

  Shape(NdOp Selects, bool Join) {
    Func.Entry = 0x1000;
    Func.Blocks.resize(5);
    for (int I = 0; I < 5; ++I) {
      Func.Blocks[I].Id = I;
      Func.Blocks[I].StartAddr = 0x1000 + 0x10 * static_cast<va_t>(I);
    }
    MedBlock &B0 = Func.Blocks[0], &B1 = Func.Blocks[1], &B2 = Func.Blocks[2],
             &B3 = Func.Blocks[3], &B4 = Func.Blocks[4];
    B0.Succs = {1, 4};
    B0.Ops.push_back(
        operation(NdOp::INT_EQUAL, Z, {X, MedVar::makeConst(0, 8)}));
    B0.Ops.push_back(branch(0x1040, Z));
    B1.Preds = {0};
    B1.Succs = {2, 3};
    B1.Ops.push_back(
        operation(NdOp::INT_NOTEQUAL, C, {Y, MedVar::makeConst(1, 8)}));
    B1.Ops.push_back(branch(0x1030, C));
    B2.Preds = {1};
    B2.Ops.push_back(operation(Selects, S, {MedVar::makeConst(0, 8), X}));
    B2.Ops.push_back(operation(NdOp::INT_ADD, T, {X, MedVar::makeConst(5, 8)}));
    B2.Ops.push_back(operation(NdOp::SELECT, R, {S, Old, T}));
    B2.Ops.push_back(ret(R));
    B3.Preds = {1};
    B3.Ops.push_back(ret(Y));
    B4.Preds = {0};
    if (Join) {
      B4.Succs = {2};
      B2.Preds.push_back(4);
    } else {
      B4.Ops.push_back(ret(MedVar::makeConst(0, 8)));
    }
  }

  const MedOp &selection() const { return Func.Blocks[2].Ops[2]; }
};

TEST(MedDominatedSelects, ATestAboveDecidesTheSelection) {
  // `0 == x`, the test's own comparison the other way round, is false two
  // blocks below the edge that takes `x == 0` false.
  Shape F(NdOp::INT_EQUAL, /*Join=*/false);
  EXPECT_TRUE(foldDominatedSelects(F.Func));
  const MedOp &Op = F.selection();
  EXPECT_EQ(Op.Opcode, NdOp::COPY);
  ASSERT_EQ(Op.NumInputs, 1u);
  EXPECT_EQ(Op.Inputs[0], F.T);
}

TEST(MedDominatedSelects, ANegatedTestTakesTheOtherArm) {
  // `0 != x` holds there, so the selection takes its first arm.
  Shape F(NdOp::INT_NOTEQUAL, /*Join=*/false);
  EXPECT_TRUE(foldDominatedSelects(F.Func));
  const MedOp &Op = F.selection();
  EXPECT_EQ(Op.Opcode, NdOp::COPY);
  ASSERT_EQ(Op.NumInputs, 1u);
  EXPECT_EQ(Op.Inputs[0], F.Old);
}

TEST(MedDominatedSelects, AJoinOfTheOtherEdgeDecidesNothing) {
  // Block 2 is entered from where `x == 0` holds as well.
  Shape F(NdOp::INT_EQUAL, /*Join=*/true);
  EXPECT_FALSE(foldDominatedSelects(F.Func));
  EXPECT_EQ(F.selection().Opcode, NdOp::SELECT);
}

TEST(MedDominatedSelects, AnotherComparisonDecidesNothing) {
  // `0 < x` is no test the branches made.
  Shape F(NdOp::INT_LESS, /*Join=*/false);
  EXPECT_FALSE(foldDominatedSelects(F.Func));
  EXPECT_EQ(F.selection().Opcode, NdOp::SELECT);
}
} // namespace
