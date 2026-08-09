//===- EVMAnalyzerTests.cpp - staged EVM analysis tests -----------------===//

#include "gtest/gtest.h"

#include "neverd/evm/Analyzer.h"

#include "llvm/Support/Error.h"

namespace neverd::evm {
namespace {

TEST(EVMAnalyzer, PushDataIsLosslessAndZeroPaddedAtEOF) {
  const std::vector<uint8_t> Code = {0x61, 0x12};
  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  ASSERT_EQ(Low->Instructions.size(), 1u);
  EXPECT_EQ(Low->Instructions[0].PC, 0u);
  EXPECT_EQ(Low->Instructions[0].Info.Name, "PUSH2");
  EXPECT_EQ(Low->Instructions[0].Immediate.getZExtValue(), 0x1200u);
  EXPECT_TRUE(Low->Instructions[0].ImmediateTruncated);

  const std::vector<uint8_t> EmbeddedOpcodes = {0x62, 0x5b, 0x60, 0x00, 0x00};
  Low = decodeLowIR(EmbeddedOpcodes);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  ASSERT_EQ(Low->Instructions.size(), 2u);
  EXPECT_EQ(Low->Instructions[0].Immediate.getZExtValue(), 0x5b6000u);
  EXPECT_EQ(Low->Instructions[1].PC, 4u);
  EXPECT_TRUE(Low->JumpDestinations.empty());
}

TEST(EVMAnalyzer, EveryTruncatedPushIsRightZeroPadded) {
  for (unsigned Width = 1; Width <= 32; ++Width) {
    SCOPED_TRACE(testing::Message() << "PUSH" << Width);
    std::vector<uint8_t> Code{static_cast<uint8_t>(0x5f + Width)};
    if (Width > 1)
      Code.push_back(0xab);
    auto Low = decodeLowIR(Code);
    ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
    ASSERT_EQ(Low->Instructions.size(), 1u);
    EXPECT_TRUE(Low->Instructions.front().ImmediateTruncated);
    const llvm::APInt Expected =
        Width == 1 ? llvm::APInt(256, 0)
                   : llvm::APInt(256, 0xab).shl((Width - 1) * 8);
    EXPECT_EQ(Low->Instructions.front().Immediate, Expected);
  }
}

TEST(EVMAnalyzer, RelaxedDecoderAcceptsEveryByteValue) {
  AnalyzeOptions Options;
  Options.Strict = false;
  for (unsigned Opcode = 0; Opcode <= 0xff; ++Opcode) {
    SCOPED_TRACE(testing::Message() << "opcode 0x" << std::hex << Opcode);
    auto Low = decodeLowIR(std::vector<uint8_t>{static_cast<uint8_t>(Opcode)},
                           Options);
    ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
    ASSERT_EQ(Low->Instructions.size(), 1u);
  }
}

TEST(EVMAnalyzer, StrictModeRejectsUnknownAndInactiveOpcodes) {
  auto Unknown = decodeLowIR(std::vector<uint8_t>{0x0c});
  ASSERT_FALSE(static_cast<bool>(Unknown));
  const std::string UnknownError = llvm::toString(Unknown.takeError());
  EXPECT_NE(UnknownError.find("0x0c"), std::string::npos);
  EXPECT_NE(UnknownError.find("pc 0x0"), std::string::npos);

  AnalyzeOptions London;
  London.Fork = Hardfork::London;
  auto Inactive = decodeLowIR(std::vector<uint8_t>{0x5f}, London);
  ASSERT_FALSE(static_cast<bool>(Inactive));
  EXPECT_NE(llvm::toString(Inactive.takeError()).find("inactive"),
            std::string::npos);

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Accepted = decodeLowIR(std::vector<uint8_t>{0x0c}, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  ASSERT_EQ(Accepted->Diagnostics.size(), 1u);
  EXPECT_FALSE(Accepted->Instructions[0].Known);
}

TEST(EVMAnalyzer, EnforcesStackLimitAndReportsExactJumpPC) {
  std::vector<uint8_t> Overflow(1025, 0x5f);
  Overflow.push_back(0x00);
  auto StrictOverflow = decodeLowIR(Overflow);
  ASSERT_FALSE(static_cast<bool>(StrictOverflow));
  EXPECT_NE(llvm::toString(StrictOverflow.takeError()).find("exceeds 1024"),
            std::string::npos);

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto RelaxedOverflow = decodeLowIR(Overflow, Relaxed);
  ASSERT_TRUE(static_cast<bool>(RelaxedOverflow))
      << llvm::toString(RelaxedOverflow.takeError());
  EXPECT_FALSE(RelaxedOverflow->Diagnostics.empty());

  std::vector<uint8_t> HugeJump{0x7f};
  HugeJump.insert(HugeJump.end(), 32, 0xff);
  HugeJump.push_back(0x56);
  auto Invalid = decodeLowIR(HugeJump);
  ASSERT_FALSE(static_cast<bool>(Invalid));
  const std::string Error = llvm::toString(Invalid.takeError());
  EXPECT_NE(Error.find("not a JUMPDEST"), std::string::npos);
  EXPECT_NE(Error.find("pc 0x21"), std::string::npos);
}

TEST(EVMAnalyzer, DeterministicShortProgramsNeverCrashRelaxedDecode) {
  AnalyzeOptions Options;
  Options.Strict = false;
  uint32_t State = 0x4e56444u;
  for (unsigned Case = 0; Case < 512; ++Case) {
    State = State * 1664525u + 1013904223u;
    const size_t Size = 1 + (State % 31);
    std::vector<uint8_t> Code(Size);
    for (uint8_t &Byte : Code) {
      State = State * 1664525u + 1013904223u;
      Byte = static_cast<uint8_t>(State >> 24);
    }
    auto Low = decodeLowIR(Code, Options);
    ASSERT_TRUE(static_cast<bool>(Low))
        << "case " << Case << ": " << llvm::toString(Low.takeError());
    EXPECT_FALSE(Low->Instructions.empty());
  }
}

TEST(EVMAnalyzer, BuildsConditionalCFGWithValidatedJumpDestinations) {
  // calldata[0] supplies an unknown condition. Both target and fallthrough are
  // therefore retained.
  const std::vector<uint8_t> Code = {0x60, 0x00, 0x35, 0x60, 0x08,
                                     0x57, 0x5b, 0x00, 0x5b, 0x00};
  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  const LowBlock *Entry = Low->findBlock(0);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->Successors.size(), 2u);
  EXPECT_TRUE(Low->hasEdge(0, 6, EdgeKind::ConditionalFalse));
  EXPECT_TRUE(Low->hasEdge(0, 8, EdgeKind::ConditionalTrue));
  EXPECT_EQ(Low->findBlock(8)->Predecessors, (std::vector<uint64_t>{0}));

  auto BadJump = decodeLowIR(std::vector<uint8_t>{0x60, 0x01, 0x56});
  ASSERT_FALSE(static_cast<bool>(BadJump));
  EXPECT_NE(llvm::toString(BadJump.takeError()).find("not a JUMPDEST"),
            std::string::npos);
}

TEST(EVMAnalyzer, LowersStackOperationsTo256BitSSA) {
  auto Program = analyze(
      std::vector<uint8_t>{0x60, 0x01, 0x60, 0x02, 0x01, 0x60, 0x00, 0x55});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Med.Blocks.size(), 1u);
  const MedBlock &Block = Program->Med.Blocks.front();
  ASSERT_EQ(Block.Operations.size(), 5u);
  EXPECT_EQ(Block.Operations[2].Name, "ADD");
  EXPECT_EQ(Block.Operations[2].Inputs.size(), 2u);
  ASSERT_EQ(Block.Operations[2].Outputs.size(), 1u);
  const MedValue *Sum = Program->Med.findValue(Block.Operations[2].Outputs[0]);
  ASSERT_NE(Sum, nullptr);
  ASSERT_TRUE(Sum->Constant.has_value());
  EXPECT_EQ(Sum->Constant->getZExtValue(), 3u);
  EXPECT_EQ(Block.Operations.back().Effect, EffectKind::StorageWrite);
}

TEST(EVMAnalyzer, RecoversSelectorReturnAndStorageFacts) {
  // Solidity-style dispatcher for selector 0x12345678 -> pc 0x15, returning
  // uint256(42). A following unreachable fragment touches storage slot 3.
  const std::vector<uint8_t> Code = {
      0x60, 0x00, 0x35, 0x60, 0xe0, 0x1c, 0x80, 0x63, 0x12, 0x34,
      0x56, 0x78, 0x14, 0x60, 0x15, 0x57, 0x5b, 0x60, 0x00, 0x80,
      0xfd, 0x5b, 0x60, 0x2a, 0x60, 0x00, 0x52, 0x60, 0x20, 0x60,
      0x00, 0xf3, 0x60, 0x03, 0x54, 0x50, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions[0].Selector, 0x12345678u);
  EXPECT_EQ(Program->High.Functions[0].EntryPC, 0x15u);
  EXPECT_EQ(Program->High.Functions[0].Returns.size(), 1u);
  ASSERT_EQ(Program->High.Storage.size(), 1u);
  EXPECT_TRUE(Program->High.Storage[0].Slot.has_value());
  EXPECT_EQ(Program->High.Storage[0].Slot->getZExtValue(), 3u);

  EXPECT_NE(dumpLowIR(Program->Low).find("block 0x0"), std::string::npos);
  EXPECT_NE(dumpMedIR(Program->Med).find("storage.read"), std::string::npos);
  EXPECT_NE(dumpHighIR(Program->High).find("selector 0x12345678"),
            std::string::npos);
}

} // namespace
} // namespace neverd::evm
