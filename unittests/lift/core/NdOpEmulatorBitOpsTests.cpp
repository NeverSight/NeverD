//===- NdOpEmulatorBitOpsTests.cpp - bitfield, select and extension tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NdOpEmulatorTestsDetail.h"
#include "gtest/gtest.h"

namespace {

using namespace neverd;
using namespace neverd::ndop_emulator_test;

TEST_F(NdOpEmulatorTest, PieceOp) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xAB);
  Emu.setRegister(8, 0xCD);

  LowOp Piece;
  Piece.Opcode = NdOp::CONCAT;
  Piece.Output = NdVar::reg(16, 2);
  Piece.addInput(NdVar::reg(0, 1));
  Piece.addInput(NdVar::reg(8, 1));
  ASSERT_TRUE(Emu.step(Piece));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 0xABCDu);
}

TEST_F(NdOpEmulatorTest, NegateAnd2Comp) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xFF);

  LowOp Neg;
  Neg.Opcode = NdOp::INT_NEGATE;
  Neg.Output = NdVar::reg(8, 8);
  Neg.addInput(NdVar::reg(0, 8));
  ASSERT_TRUE(Emu.step(Neg));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), ~uint64_t(0xFF));

  LowOp Comp;
  Comp.Opcode = NdOp::INT_NEG2;
  Comp.Output = NdVar::reg(16, 8);
  Comp.addInput(NdVar::reg(0, 8));
  ASSERT_TRUE(Emu.step(Comp));
  EXPECT_EQ(static_cast<int64_t>(Emu.getRegister(16).value_or(0)), -0xFF);
}

TEST_F(NdOpEmulatorTest, SubpieceExtract) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xDEADBEEFCAFE1234ULL);

  LowOp Sub;
  Sub.Opcode = NdOp::SUBBYTES;
  Sub.Output = NdVar::reg(8, 2);
  Sub.addInput(NdVar::reg(0, 8));
  Sub.addInput(NdVar::cst(2, 4));
  ASSERT_TRUE(Emu.step(Sub));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 0xCAFEu);
}

TEST_F(NdOpEmulatorTest, SelectOp) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 1); // condition true
  Emu.setRegister(8, 42);
  Emu.setRegister(16, 99);

  LowOp Sel;
  Sel.Opcode = NdOp::SELECT;
  Sel.Output = NdVar::reg(24, 8);
  Sel.addInput(NdVar::reg(0, 1));
  Sel.addInput(NdVar::reg(8, 8));
  Sel.addInput(NdVar::reg(16, 8));
  ASSERT_TRUE(Emu.step(Sel));
  EXPECT_EQ(Emu.getRegister(24).value_or(0), 42u);

  Emu.setRegister(0, 0); // condition false
  ASSERT_TRUE(Emu.step(Sel));
  EXPECT_EQ(Emu.getRegister(24).value_or(0), 99u);
}

TEST_F(NdOpEmulatorTest, IntNot) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xFF00FF00);

  LowOp Not;
  Not.Opcode = NdOp::INT_NOT;
  Not.Output = NdVar::reg(8, 4);
  Not.addInput(NdVar::reg(0, 4));
  ASSERT_TRUE(Emu.step(Not));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 0x00FF00FFu);
}

TEST_F(NdOpEmulatorTest, PopcountOp) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xFF);

  LowOp Pop;
  Pop.Opcode = NdOp::POPCOUNT;
  Pop.Output = NdVar::reg(8, 8);
  Pop.addInput(NdVar::reg(0, 8));
  ASSERT_TRUE(Emu.step(Pop));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 8u);

  Emu.setRegister(0, 0);
  ASSERT_TRUE(Emu.step(Pop));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);
}

TEST_F(NdOpEmulatorTest, PopcountCountsOnlyTheBytesTheOperandNames) {
  // A register slot holds whatever the widest write to it left behind, and a
  // narrow operand names part of that.  Counting the whole slot answers for
  // bits this operation cannot see.
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xFFFFFFFFFFFFFF0FULL);

  LowOp Pop;
  Pop.Opcode = NdOp::POPCOUNT;
  Pop.Output = NdVar::reg(8, 1);
  Pop.addInput(NdVar::reg(0, 1));
  ASSERT_TRUE(Emu.step(Pop));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 4u);
}

TEST_F(NdOpEmulatorTest, LzcountOp) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 1);

  LowOp Lz;
  Lz.Opcode = NdOp::LZCOUNT;
  Lz.Output = NdVar::reg(8, 8);
  Lz.addInput(NdVar::reg(0, 8));
  ASSERT_TRUE(Emu.step(Lz));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 63u);

  Emu.setRegister(0, 0);
  ASSERT_TRUE(Emu.step(Lz));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 64u);
}

TEST_F(NdOpEmulatorTest, InsertExtractOp) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xF000);
  Emu.setRegister(8, 0xAB);

  // INSERT(base=0xF000, val=0xAB, pos=4, len=8) -> clears bits [4..11],
  // inserts low 8 bits of 0xAB shifted left by 4.
  // Mask = 0xFF0, base & ~mask = 0xF000, val<<4 & mask = 0xAB0
  // Result = 0xF000 | 0xAB0 = 0xFAB0
  LowOp Ins;
  Ins.Opcode = NdOp::INSERT;
  Ins.Output = NdVar::reg(16, 8);
  Ins.addInput(NdVar::reg(0, 8));
  Ins.addInput(NdVar::reg(8, 8));
  Ins.addInput(NdVar::cst(4, 4));
  Ins.addInput(NdVar::cst(8, 4));
  ASSERT_TRUE(Emu.step(Ins));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 0xFAB0u);

  // EXTRACT(0xDEAD, pos=8, len=8) -> (0xDEAD >> 8) & 0xFF = 0xDE
  LowOp Ext;
  Ext.Opcode = NdOp::EXTRACT;
  Ext.Output = NdVar::reg(24, 8);
  Ext.addInput(NdVar::cst(0xDEAD, 8));
  Ext.addInput(NdVar::cst(8, 4));
  Ext.addInput(NdVar::cst(8, 4));
  ASSERT_TRUE(Emu.step(Ext));
  EXPECT_EQ(Emu.getRegister(24).value_or(0), 0xDEu);
}

TEST_F(NdOpEmulatorTest, SignExtInChain) {
  std::vector<LowOp> Ops;

  // switch_var (1 byte) -> LOAD -> INT_SEXT -> INT_ADD base -> INDIR_BR
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(8, 1);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::reg(0, 8));
  Ops.push_back(Load);

  LowOp Sext;
  Sext.Opcode = NdOp::INT_SEXT;
  Sext.Output = NdVar::reg(16, 8);
  Sext.Inputs[0] = NdVar::reg(8, 1);
  Sext.NumInputs = 1;
  Ops.push_back(Sext);

  Ops.push_back(makeArith(NdOp::INT_ADD, 24, 16, 0x1000));
  Ops.push_back(makeBranchInd(24));

  BinaryImage ImgWithData = makeDummyImage();
  // Write signed byte values at 0x1000: 0, 2, 4, -2 (0xFE)
  ImgWithData.Segments[0].Data[0] = 0;
  ImgWithData.Segments[0].Data[1] = 2;
  ImgWithData.Segments[0].Data[2] = 4;
  ImgWithData.Segments[0].Data[3] = 0xFE;

  NdOpEmulator Emu(ImgWithData);
  // Index addr 0x1000: loads byte 0 -> sext -> 0 + 0x1000 = 0x1000
  auto T0 = Emu.computeTarget(Ops, 0, 0x1000);
  ASSERT_TRUE(T0.has_value());
  EXPECT_EQ(*T0, 0x1000u);

  // Index addr 0x1001: loads byte 2 -> sext -> 2 + 0x1000 = 0x1002
  auto T1 = Emu.computeTarget(Ops, 0, 0x1001);
  ASSERT_TRUE(T1.has_value());
  EXPECT_EQ(*T1, 0x1002u);
}

} // namespace
