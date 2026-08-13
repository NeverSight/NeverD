//===- NdOpEmulatorMemoryTests.cpp - load, store and access record tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "NdOpEmulatorTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::ndop_emulator_test;

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

} // namespace
