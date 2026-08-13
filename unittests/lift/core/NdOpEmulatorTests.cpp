//===- NdOpEmulatorTests.cpp - arithmetic and comparison tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "NdOpEmulatorTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::ndop_emulator_test;

TEST_F(NdOpEmulatorTest, BasicArithmetic) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 10);

  LowOp Add = makeArith(NdOp::INT_ADD, 8, 0, 5);
  ASSERT_TRUE(Emu.step(Add));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 15u);
}

TEST_F(NdOpEmulatorTest, SubAndShift) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 20);

  LowOp Sub = makeArith(NdOp::INT_SUB, 8, 0, 5);
  ASSERT_TRUE(Emu.step(Sub));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 15u);

  LowOp Shift = makeArith(NdOp::INT_LEFT, 16, 8, 2);
  ASSERT_TRUE(Emu.step(Shift));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 60u);
}

TEST_F(NdOpEmulatorTest, CopyChain) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 42);

  LowOp C1 = makeCopy(8, 0);
  LowOp C2 = makeCopy(16, 8);
  ASSERT_TRUE(Emu.step(C1));
  ASSERT_TRUE(Emu.step(C2));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 42u);
}

TEST_F(NdOpEmulatorTest, RunStopsAtTerminator) {
  std::vector<LowOp> Ops;
  Ops.push_back(makeArith(NdOp::INT_ADD, 8, 0, 1));
  Ops.push_back(makeArith(NdOp::INT_ADD, 16, 8, 2));
  Ops.push_back(makeBranchInd(16));
  Ops.push_back(makeArith(NdOp::INT_ADD, 24, 16, 3));

  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 10);
  size_t Ran = Emu.run(Ops);
  EXPECT_EQ(Ran, 2u);
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 13u);
  EXPECT_FALSE(Emu.getRegister(24).has_value());
}

TEST_F(NdOpEmulatorTest, ComputeTarget) {
  std::vector<LowOp> Ops;
  Ops.push_back(makeArith(NdOp::INT_MULT, 8, 0, 4));
  Ops.push_back(makeArith(NdOp::INT_ADD, 16, 8, 0x1000));
  Ops.push_back(makeBranchInd(16));

  NdOpEmulator Emu(Img);
  auto Tgt = Emu.computeTarget(Ops, 0, 3);
  ASSERT_TRUE(Tgt.has_value());
  EXPECT_EQ(*Tgt, 0x100Cu);
}

TEST_F(NdOpEmulatorTest, BitwiseOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xFF00);

  LowOp And = makeArith(NdOp::INT_AND, 8, 0, 0xFF);
  ASSERT_TRUE(Emu.step(And));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);

  Emu.setRegister(0, 0xA5);
  LowOp Xor = makeArith(NdOp::INT_XOR, 8, 0, 0xFF);
  ASSERT_TRUE(Emu.step(Xor));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 0x5Au);
}

TEST_F(NdOpEmulatorTest, ResetClearsState) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 42);
  EXPECT_TRUE(Emu.getRegister(0).has_value());
  Emu.reset();
  EXPECT_FALSE(Emu.getRegister(0).has_value());
}
TEST_F(NdOpEmulatorTest, DivAndRem) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 100);

  LowOp Div = makeArith(NdOp::INT_DIV, 8, 0, 7);
  ASSERT_TRUE(Emu.step(Div));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 14u);

  LowOp Rem = makeArith(NdOp::INT_REM, 16, 0, 7);
  ASSERT_TRUE(Emu.step(Rem));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 2u);
}

TEST_F(NdOpEmulatorTest, DivByZeroNoOutput) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 42);

  LowOp Div = makeArith(NdOp::INT_DIV, 8, 0, 0);
  Emu.step(Div);
  EXPECT_FALSE(Emu.getRegister(8).has_value());
}

TEST_F(NdOpEmulatorTest, SignedDivAndRem) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, static_cast<uint64_t>(-15));

  LowOp Div = makeArith(NdOp::INT_SDIV, 8, 0, 4);
  ASSERT_TRUE(Emu.step(Div));
  EXPECT_EQ(static_cast<int64_t>(Emu.getRegister(8).value_or(0)), -3);

  LowOp Rem = makeArith(NdOp::INT_SREM, 16, 0, 4);
  ASSERT_TRUE(Emu.step(Rem));
  EXPECT_EQ(static_cast<int64_t>(Emu.getRegister(16).value_or(0)), -3);
}

TEST_F(NdOpEmulatorTest, CompareOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 10);

  LowOp Eq = makeArith(NdOp::INT_EQUAL, 8, 0, 10);
  ASSERT_TRUE(Emu.step(Eq));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 1u);

  LowOp Neq = makeArith(NdOp::INT_NOTEQUAL, 16, 0, 10);
  ASSERT_TRUE(Emu.step(Neq));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 0u);

  LowOp Lt = makeArith(NdOp::INT_LESS, 24, 0, 20);
  ASSERT_TRUE(Emu.step(Lt));
  EXPECT_EQ(Emu.getRegister(24).value_or(~0ULL), 1u);

  LowOp Le = makeArith(NdOp::INT_LESSEQUAL, 32, 0, 10);
  ASSERT_TRUE(Emu.step(Le));
  EXPECT_EQ(Emu.getRegister(32).value_or(~0ULL), 1u);
}

TEST_F(NdOpEmulatorTest, SignedCompareOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, static_cast<uint64_t>(-5));

  LowOp SLess = makeArith(NdOp::INT_SLESS, 8, 0, 0);
  ASSERT_TRUE(Emu.step(SLess));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 1u);

  LowOp SLeq = makeArith(NdOp::INT_SLESSEQUAL, 16, 0,
                          static_cast<uint64_t>(-5));
  ASSERT_TRUE(Emu.step(SLeq));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 1u);
}

TEST_F(NdOpEmulatorTest, BoolOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 1);

  LowOp BoolNot;
  BoolNot.Opcode = NdOp::BOOL_NOT;
  BoolNot.Output = NdVar::reg(8, 1);
  BoolNot.addInput(NdVar::reg(0, 1));
  ASSERT_TRUE(Emu.step(BoolNot));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);

  LowOp BoolAnd = makeArith(NdOp::BOOL_AND, 16, 0, 1);
  ASSERT_TRUE(Emu.step(BoolAnd));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 1u);

  LowOp BoolOr = makeArith(NdOp::BOOL_OR, 24, 0, 0);
  ASSERT_TRUE(Emu.step(BoolOr));
  EXPECT_EQ(Emu.getRegister(24).value_or(~0ULL), 1u);
}
TEST_F(NdOpEmulatorTest, CarryAndBorrowFlags) {
  NdOpEmulator Emu(Img);

  // INT_CARRY: unsigned overflow detection
  Emu.setRegister(0, ~0ULL);
  LowOp Carry = makeArith(NdOp::INT_CARRY, 8, 0, 1);
  ASSERT_TRUE(Emu.step(Carry));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 1u);

  Emu.setRegister(0, 0);
  ASSERT_TRUE(Emu.step(Carry));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);

  // INT_SOVF: signed overflow detection
  Emu.setRegister(0, INT64_MAX);
  LowOp SCarry = makeArith(NdOp::INT_SOVF, 16, 0, 1);
  ASSERT_TRUE(Emu.step(SCarry));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 1u);

  // INT_SBOR: signed borrow detection
  Emu.setRegister(0, INT64_MIN);
  LowOp SBorrow = makeArith(NdOp::INT_SBOR, 24, 0, 1);
  ASSERT_TRUE(Emu.step(SBorrow));
  EXPECT_EQ(Emu.getRegister(24).value_or(~0ULL), 1u);
}

} // namespace
