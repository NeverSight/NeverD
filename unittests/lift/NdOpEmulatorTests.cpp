//===- NdOpEmulatorTests.cpp - Unit tests for NdOpEmulator ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/NdOps.h"
#include "neverd/Common.h"
#include "neverd/loader/BinaryImage.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace neverd;

static BinaryImage makeDummyImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = 0x1000;
  Seg.Size = 256;
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.resize(256, 0);
  Img.Segments.push_back(std::move(Seg));
  return Img;
}

static LowOp makeArith(NdOp Opcode, uint64_t OutReg, uint64_t InReg,
                        uint64_t Const) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = NdVar::reg(OutReg, 8);
  Op.addInput(NdVar::reg(InReg, 8));
  Op.addInput(NdVar::cst(Const, 8));
  return Op;
}

static LowOp makeCopy(uint64_t OutReg, uint64_t InReg) {
  LowOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = NdVar::reg(OutReg, 8);
  Op.addInput(NdVar::reg(InReg, 8));
  return Op;
}

static LowOp makeBranchInd(uint64_t InReg) {
  LowOp Op;
  Op.Opcode = NdOp::INDIR_BR;
  Op.Output = {};
  Op.addInput(NdVar::reg(InReg, 8));
  return Op;
}

class NdOpEmulatorTest : public ::testing::Test {
protected:
  BinaryImage Img = makeDummyImage();
};

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

TEST_F(NdOpEmulatorTest, LoadFromMemory) {
  BinaryImage ImgWithData = makeDummyImage();
  uint32_t TestVal = 0xDEADBEEF;
  std::memcpy(ImgWithData.Segments[0].Data.data() + 0x10,
              &TestVal, sizeof(TestVal));

  NdOpEmulator Emu(ImgWithData);

  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(8, 4);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(0x1010, 8));

  ASSERT_TRUE(Emu.step(Load));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 0xDEADBEEFu);
}

TEST_F(NdOpEmulatorTest, CrossBlockOpSequence) {
  // Simulate the cross-block scenario: predecessor block computes
  // index = val * 4, INDIR_BR block adds base and branches.
  std::vector<LowOp> PathOps;

  // Block 1 (predecessor): index = switch_var * 4
  PathOps.push_back(makeArith(NdOp::INT_MULT, 8, 0, 4));
  // Block 2 (INDIR_BR): target = index + base
  PathOps.push_back(makeArith(NdOp::INT_ADD, 16, 8, 0x1000));
  PathOps.push_back(makeBranchInd(16));

  NdOpEmulator Emu(Img);
  // Index 0: target = 0*4 + 0x1000 = 0x1000
  auto T0 = Emu.computeTarget(PathOps, 0, 0);
  ASSERT_TRUE(T0.has_value());
  EXPECT_EQ(*T0, 0x1000u);

  // Index 5: target = 5*4 + 0x1000 = 0x1014
  auto T5 = Emu.computeTarget(PathOps, 0, 5);
  ASSERT_TRUE(T5.has_value());
  EXPECT_EQ(*T5, 0x1014u);
}

TEST_F(NdOpEmulatorTest, LoadAndBranchCrossBlock) {
  BinaryImage ImgWithTable = makeDummyImage();
  // Set up a jump table at 0x1080: [0x1100, 0x1110, 0x1120, 0x1130]
  uint64_t Entries[] = {0x1100, 0x1110, 0x1120, 0x1130};
  std::memcpy(ImgWithTable.Segments[0].Data.data() + 0x80,
              Entries, sizeof(Entries));

  std::vector<LowOp> PathOps;
  // Block 1: scaled_idx = switch_var * 8
  PathOps.push_back(makeArith(NdOp::INT_MULT, 8, 0, 8));
  // Block 1: table_ptr = scaled_idx + 0x1080
  PathOps.push_back(makeArith(NdOp::INT_ADD, 16, 8, 0x1080));
  // Block 2: target = LOAD [table_ptr]
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(24, 8);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::reg(16, 8));
  PathOps.push_back(Load);
  // Block 2: INDIR_BR target
  PathOps.push_back(makeBranchInd(24));

  NdOpEmulator Emu(ImgWithTable);
  for (uint32_t I = 0; I < 4; ++I) {
    auto Tgt = Emu.computeTarget(PathOps, 0, I);
    ASSERT_TRUE(Tgt.has_value()) << "Failed for index " << I;
    EXPECT_EQ(*Tgt, 0x1100u + I * 0x10) << "Wrong target for index " << I;
  }
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

//===----------------------------------------------------------------------===//
// New tests for STORE, LoadRecord, and memory overlay
//===----------------------------------------------------------------------===//

TEST_F(NdOpEmulatorTest, StoreAndReload) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0x42);

  // STORE value 0x42 to address 0x1020
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Output = {};
  Store.addInput(NdVar::cst(0, 8));
  Store.addInput(NdVar::cst(0x1020, 8));
  Store.addInput(NdVar::reg(0, 4));
  ASSERT_TRUE(Emu.step(Store));

  // LOAD it back from the same address
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(8, 4);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(0x1020, 8));
  ASSERT_TRUE(Emu.step(Load));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 0x42u);
}

TEST_F(NdOpEmulatorTest, StoreOverlaysImage) {
  BinaryImage ImgWithData = makeDummyImage();
  uint32_t Original = 0xAAAAAAAA;
  std::memcpy(ImgWithData.Segments[0].Data.data() + 0x20,
              &Original, sizeof(Original));

  NdOpEmulator Emu(ImgWithData);

  // Verify original value can be loaded
  LowOp Load1;
  Load1.Opcode = NdOp::LOAD;
  Load1.Output = NdVar::reg(8, 4);
  Load1.addInput(NdVar::cst(0, 8));
  Load1.addInput(NdVar::cst(0x1020, 8));
  ASSERT_TRUE(Emu.step(Load1));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 0xAAAAAAAAu);

  // Store a new value over the same address
  Emu.setRegister(0, 0xBBBBBBBB);
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Output = {};
  Store.addInput(NdVar::cst(0, 8));
  Store.addInput(NdVar::cst(0x1020, 8));
  Store.addInput(NdVar::reg(0, 4));
  ASSERT_TRUE(Emu.step(Store));

  // Load again — should get the stored value, not the original
  LowOp Load2;
  Load2.Opcode = NdOp::LOAD;
  Load2.Output = NdVar::reg(16, 4);
  Load2.addInput(NdVar::cst(0, 8));
  Load2.addInput(NdVar::cst(0x1020, 8));
  ASSERT_TRUE(Emu.step(Load2));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 0xBBBBBBBBu);
}

TEST_F(NdOpEmulatorTest, LoadRecordCollection) {
  BinaryImage ImgWithData = makeDummyImage();
  uint32_t V1 = 0x11, V2 = 0x22;
  std::memcpy(ImgWithData.Segments[0].Data.data() + 0x10, &V1, 4);
  std::memcpy(ImgWithData.Segments[0].Data.data() + 0x20, &V2, 4);

  NdOpEmulator Emu(ImgWithData);
  Emu.setLoadCollect(true);

  LowOp Load1;
  Load1.Opcode = NdOp::LOAD;
  Load1.Output = NdVar::reg(8, 4);
  Load1.addInput(NdVar::cst(0, 8));
  Load1.addInput(NdVar::cst(0x1010, 8));
  ASSERT_TRUE(Emu.step(Load1));

  LowOp Load2;
  Load2.Opcode = NdOp::LOAD;
  Load2.Output = NdVar::reg(16, 4);
  Load2.addInput(NdVar::cst(0, 8));
  Load2.addInput(NdVar::cst(0x1020, 8));
  ASSERT_TRUE(Emu.step(Load2));

  auto &Records = Emu.getLoadRecords();
  ASSERT_EQ(Records.size(), 2u);
  EXPECT_EQ(Records[0].Addr, 0x1010u);
  EXPECT_EQ(Records[0].Size, 4u);
  EXPECT_EQ(Records[1].Addr, 0x1020u);
  EXPECT_EQ(Records[1].Size, 4u);
}

TEST_F(NdOpEmulatorTest, LoadRecordNotCollectedByDefault) {
  NdOpEmulator Emu(Img);

  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(8, 4);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(0x1010, 8));
  Emu.step(Load);

  EXPECT_TRUE(Emu.getLoadRecords().empty());
}

TEST_F(NdOpEmulatorTest, ResetClearsStoreAndLoadLog) {
  NdOpEmulator Emu(Img);
  Emu.setLoadCollect(true);
  Emu.setRegister(0, 99);

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Output = {};
  Store.addInput(NdVar::cst(0, 8));
  Store.addInput(NdVar::cst(0x1040, 8));
  Store.addInput(NdVar::reg(0, 4));
  Emu.step(Store);

  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(8, 4);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(0x1040, 8));
  Emu.step(Load);

  EXPECT_FALSE(Emu.getLoadRecords().empty());
  EXPECT_TRUE(Emu.getRegister(8).has_value());

  Emu.reset();

  EXPECT_TRUE(Emu.getLoadRecords().empty());
  EXPECT_FALSE(Emu.getRegister(8).has_value());

  // After reset, LOAD from the same addr should get image value (0), not 99
  Emu.step(Load);
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);
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

TEST_F(NdOpEmulatorTest, CollapseLoadRecords) {
  std::vector<neverd::LoadRecord> Records = {
      {0x1000, 4}, {0x1004, 4}, {0x1008, 4}, {0x2000, 2}, {0x2002, 2}};
  neverd::NdOpEmulator::collapseLoadRecords(Records);
  ASSERT_EQ(Records.size(), 2u);
  EXPECT_EQ(Records[0].Addr, 0x1000u);
  EXPECT_EQ(Records[0].Size, 12u);
  EXPECT_EQ(Records[1].Addr, 0x2000u);
  EXPECT_EQ(Records[1].Size, 4u);
}

TEST_F(NdOpEmulatorTest, CollapseLoadRecordsNonContiguous) {
  std::vector<neverd::LoadRecord> Records = {
      {0x1000, 4}, {0x1010, 4}, {0x1020, 4}};
  neverd::NdOpEmulator::collapseLoadRecords(Records);
  ASSERT_EQ(Records.size(), 3u);
}

TEST_F(NdOpEmulatorTest, CollapseLoadRecordsSingle) {
  std::vector<neverd::LoadRecord> Records = {{0x1000, 4}};
  neverd::NdOpEmulator::collapseLoadRecords(Records);
  ASSERT_EQ(Records.size(), 1u);
}

TEST_F(NdOpEmulatorTest, CollapseLoadRecordsEmpty) {
  std::vector<neverd::LoadRecord> Records;
  neverd::NdOpEmulator::collapseLoadRecords(Records);
  ASSERT_TRUE(Records.empty());
}
