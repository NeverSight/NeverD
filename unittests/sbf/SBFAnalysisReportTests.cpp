//===- SBFAnalysisReportTests.cpp - Observable staged SBF analysis --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Proves that explicit early stops do not run later semantic boundaries and
/// that a failing later boundary retains the earlier completion evidence.
///
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "SBFSolanaTestsDetail.h"

#include "neverd/loader/Loader.h"
#include "neverd/sbf/analysis/SBFAnalysisReport.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

TEST(SBFAnalysisReport, ImageStopDoesNotDecodeAnInvalidOpcode) {
  auto Invalid = test::encode(Opcode::EXIT);
  Invalid[kOpcodeOffset] = 0xff;
  const auto Image = test::makeImage({Invalid}, {});
  std::vector<std::pair<AnalysisStage, AnalysisStageState>> Events;
  AnalyzeOptions Options;
  Options.StopAfter = AnalysisStage::RuntimeImage;
  Options.Observer = [&](AnalysisStage Stage, AnalysisStageState State,
                         const SBFProgram &) {
    Events.emplace_back(Stage, State);
  };
  auto Early = analyze(Image, Options);
  ASSERT_TRUE(static_cast<bool>(Early)) << llvm::toString(Early.takeError());
  EXPECT_TRUE(Early->Low.Instructions.empty());
  EXPECT_TRUE(Early->Low.Blocks.empty());
  EXPECT_TRUE(Early->Med.Instructions.empty());
  ASSERT_EQ(Events.size(), 2u);
  EXPECT_EQ(Events.back(), std::make_pair(AnalysisStage::RuntimeImage,
                                          AnalysisStageState::Passed));

  Events.clear();
  Options.StopAfter = AnalysisStage::Solana;
  auto Full = analyze(Image, Options);
  ASSERT_FALSE(static_cast<bool>(Full));
  llvm::consumeError(Full.takeError());
  EXPECT_NE(std::find(Events.begin(), Events.end(),
                      std::make_pair(AnalysisStage::RuntimeImage,
                                     AnalysisStageState::Passed)),
            Events.end());
  EXPECT_EQ(Events.back(),
            std::make_pair(AnalysisStage::Decode, AnalysisStageState::Failed));
}

TEST(SBFAnalysisReport, HighIRStopDoesNotRunOptionalSolanaRecovery) {
  const AnchorDiscriminator Deposit =
      anchorDiscriminator(AnchorNamespace::Instruction, "deposit");
  const auto Image =
      test::makeImage({test::encode(Opcode::EXIT)}, Deposit.Bytes);
  AnalyzeOptions Options;
  Options.StopAfter = AnalysisStage::HighIR;
  auto Early = analyze(Image, Options);
  ASSERT_TRUE(static_cast<bool>(Early)) << llvm::toString(Early.takeError());
  EXPECT_EQ(Early->High.Functions.size(), 1u);
  EXPECT_TRUE(Early->High.Solana.Discriminators.empty());
  Options.StopAfter = AnalysisStage::Solana;
  auto Full = analyze(Image, Options);
  ASSERT_TRUE(static_cast<bool>(Full)) << llvm::toString(Full.takeError());
  EXPECT_FALSE(Full->High.Solana.Discriminators.empty());
}

TEST(SBFAnalysisReport,
     CountsContinuationAndUnreachableSlotsWithoutDroppingThem) {
  std::vector<test::EncodedInstruction> Text =
      test::loadImm64(0, 0x123456789abcdef0);
  Text.push_back(test::encode(Opcode::EXIT));
  Text.push_back(test::encode(Opcode::EXIT));
  test::LegacyELFOptions Input;
  for (const auto &Slot : Text)
    Input.Text.insert(Input.Text.end(), Slot.begin(), Slot.end());
  const auto Bytes = test::buildLegacyELF(Input);
  auto Reader = neverd::Loader::create(BinaryFormat::ELF);
  const llvm::StringRef Buffer(reinterpret_cast<const char *>(Bytes.data()),
                               Bytes.size());
  auto Image = Reader->loadBuffer(llvm::MemoryBufferRef(Buffer, "fixture.so"));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  auto Program = analyze(*Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const auto Counts = analysisCoverage(*Program);
  EXPECT_EQ(Counts.getInteger("text_bytes"), 32);
  EXPECT_EQ(Counts.getInteger("expected_slots"), 4);
  EXPECT_EQ(Counts.getInteger("decoded_slots"), 4);
  EXPECT_EQ(Counts.getInteger("continuation_slots"), 1);
  EXPECT_EQ(Counts.getInteger("logical_instructions"), 3);
  EXPECT_EQ(Counts.getInteger("invalid_slots"), 0);
  EXPECT_LT(*Counts.getInteger("reachable_slots"), 4);
}

} // namespace
} // namespace neverd::sbf
