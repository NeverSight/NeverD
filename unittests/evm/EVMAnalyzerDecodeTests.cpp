//===- EVMAnalyzerDecodeTests.cpp - EVM instruction decode tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/Support/Error.h"

namespace neverd::evm {
namespace {

TEST(EVMAnalyzer, StackHeightDomainMaintainsSortedUniqueValues) {
  StackHeightDomain Domain;
  EXPECT_TRUE(Domain.empty());
  EXPECT_FALSE(Domain.singleton().has_value());
  EXPECT_FALSE(Domain.maximum().has_value());

  EXPECT_TRUE(Domain.insert(7));
  EXPECT_FALSE(Domain.insert(7));
  EXPECT_EQ(Domain.singleton(), 7u);
  EXPECT_EQ(Domain.maximum(), 7u);

  EXPECT_TRUE(Domain.insert(3));
  EXPECT_TRUE(Domain.insert(11));
  ASSERT_EQ(Domain.values().size(), 3u);
  EXPECT_EQ(Domain.values()[0], 3u);
  EXPECT_EQ(Domain.values()[1], 7u);
  EXPECT_EQ(Domain.values()[2], 11u);
  EXPECT_FALSE(Domain.singleton().has_value());
  EXPECT_EQ(Domain.maximum(), 11u);
}

TEST(EVMAnalyzer, PushDataIsLosslessAndZeroPaddedAtEOF) {
  const std::vector<uint8_t> Code = {0x61, 0x12};
  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  ASSERT_EQ(Low->Instructions.size(), 1u);
  EXPECT_EQ(Low->Instructions[0].PC, 0u);
  EXPECT_EQ(Low->Instructions[0].Info.Name, "PUSH2");
  EXPECT_EQ(Low->Instructions[0].Immediate.getZExtValue(), 0x1200u);
  EXPECT_EQ(Low->Instructions[0].ImmediateStatus,
            ImmediateDecodeStatus::Truncated);
  EXPECT_EQ(formatImmediate(Low->Instructions[0]), "0x1200");

  auto LeadingZero = decodeLowIR(std::vector<uint8_t>{0x61, 0x00, 0x01});
  ASSERT_TRUE(static_cast<bool>(LeadingZero))
      << llvm::toString(LeadingZero.takeError());
  EXPECT_EQ(formatImmediate(LeadingZero->Instructions[0]), "0x0001");

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
    EXPECT_EQ(Low->Instructions.front().ImmediateStatus,
              ImmediateDecodeStatus::Truncated);
    const llvm::APInt Expected =
        Width == 1 ? llvm::APInt(256, 0)
                   : llvm::APInt(256, 0xab).shl((Width - 1) * 8);
    EXPECT_EQ(Low->Instructions.front().Immediate, Expected);
  }
}

TEST(EVMAnalyzer, DecodesRepresentativeEIP8024InstructionBoundaries) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;
  Amsterdam.Strict = false;

  // These spot checks cover decoded instruction boundaries and operands. The
  // fresh-fetch go-ethereum differential audit remains the semantic authority
  // for the complete EIP-8024 encoding space.

  const struct {
    std::vector<uint8_t> Code;
    Opcode Op;
    uint16_t FirstDepth;
    uint16_t SecondDepth;
  } Valid[] = {
      {{0xe6, 0x80, 0x5b}, Opcode::DUPN, 17, 0},
      {{0xe7, 0xdb, 0x5b}, Opcode::SWAPN, 108, 0},
      {{0xe8, 0x9d, 0x5b}, Opcode::EXCHANGE, 2, 3},
      {{0xe8, 0x2f, 0x5b}, Opcode::EXCHANGE, 1, 19},
      {{0xe8, 0x50, 0x5b}, Opcode::EXCHANGE, 14, 16},
      {{0xe8, 0x51, 0x5b}, Opcode::EXCHANGE, 14, 15},
  };

  for (const auto &Case : Valid) {
    auto Low = decodeLowIR(Case.Code, Amsterdam);
    ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
    ASSERT_EQ(Low->Instructions.size(), 2u);
    const LowInstruction &Instruction = Low->Instructions.front();
    EXPECT_TRUE(Instruction.is(Case.Op));
    EXPECT_EQ(Instruction.NextPC, 2u);
    EXPECT_EQ(Instruction.Encoding.size(), 2u);
    EXPECT_EQ(Instruction.ImmediateStatus, ImmediateDecodeStatus::Complete);
    ASSERT_GE(Instruction.StackOperandCount, 1u);
    EXPECT_EQ(Instruction.StackOperands[0], Case.FirstDepth);
    if (Case.SecondDepth != 0) {
      ASSERT_EQ(Instruction.StackOperandCount, 2u);
      EXPECT_EQ(Instruction.StackOperands[1], Case.SecondDepth);
    }
    EXPECT_EQ(Low->Instructions[1].PC, 2u);
    EXPECT_TRUE(Low->Instructions[1].is(Opcode::JUMPDEST));
  }
}

TEST(EVMAnalyzer, InvalidEIP8024ImmediatePreservesLegacyBoundaries) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;

  const struct {
    std::vector<uint8_t> Code;
    Opcode InvalidOp;
    Opcode FollowingOp;
  } Cases[] = {
      {{0xe7, 0x5b}, Opcode::SWAPN, Opcode::JUMPDEST},
      {{0xe6, 0x60, 0x5b}, Opcode::DUPN, Opcode::PUSH1},
      {{0xe7, 0x61, 0x00, 0x00}, Opcode::SWAPN, Opcode::PUSH2},
      {{0xe6, 0x5f}, Opcode::DUPN, Opcode::PUSH0},
      {{0xe8, 0x52}, Opcode::EXCHANGE, Opcode::MSTORE},
  };

  for (const auto &Case : Cases) {
    auto Low = decodeLowIR(Case.Code, Amsterdam);
    ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
    ASSERT_GE(Low->Instructions.size(), 2u);
    const LowInstruction &Invalid = Low->Instructions[0];
    EXPECT_EQ(Invalid.opcode(), Case.InvalidOp);
    EXPECT_FALSE(Invalid.isExecutable());
    EXPECT_TRUE(Invalid.isTerminator());
    EXPECT_EQ(Invalid.ImmediateStatus, ImmediateDecodeStatus::Invalid);
    EXPECT_EQ(formatDecodeAnnotation(Invalid), "immediate=invalid");
    EXPECT_EQ(Invalid.NextPC, 1u);
    EXPECT_EQ(Invalid.Encoding.size(), 1u);
    EXPECT_EQ(Low->Instructions[1].PC, 1u);
    EXPECT_TRUE(Low->Instructions[1].is(Case.FollowingOp));
  }

  auto Jump = decodeLowIR({0x60, 0x04, 0x56, 0xe6, 0x5b, 0x00}, Amsterdam);
  ASSERT_TRUE(static_cast<bool>(Jump)) << llvm::toString(Jump.takeError());
  EXPECT_TRUE(Jump->JumpDestinations.contains(4));
  EXPECT_TRUE(Jump->hasEdge(0, 4, EdgeKind::Jump));
}

TEST(EVMAnalyzer, MissingEIP8024ImmediateUsesSemanticZero) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;
  Amsterdam.Strict = false;

  auto Low = decodeLowIR({opcodeByte(Opcode::DUPN)}, Amsterdam);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  ASSERT_EQ(Low->Instructions.size(), 1u);
  const LowInstruction &Instruction = Low->Instructions.front();
  EXPECT_TRUE(Instruction.is(Opcode::DUPN));
  EXPECT_EQ(Instruction.ImmediateStatus, ImmediateDecodeStatus::Truncated);
  EXPECT_EQ(formatDecodeAnnotation(Instruction), "immediate=truncated");
  EXPECT_EQ(Instruction.Encoding.size(), 1u);
  EXPECT_EQ(Instruction.NextPC, 1u);
  ASSERT_EQ(Instruction.StackOperandCount, 1u);
  EXPECT_EQ(Instruction.StackOperands[0], 145u);
  EXPECT_EQ(Instruction.requiredStackHeight(), 145u);
  EXPECT_EQ(Instruction.stackDelta(), 1);
  EXPECT_EQ(formatImmediate(Instruction), "0x00");

  AnalyzeOptions FusakaRelaxed;
  FusakaRelaxed.Fork = Hardfork::Fusaka;
  FusakaRelaxed.Strict = false;
  auto Inactive = decodeLowIR({opcodeByte(Opcode::DUPN), 0x80}, FusakaRelaxed);
  ASSERT_TRUE(static_cast<bool>(Inactive))
      << llvm::toString(Inactive.takeError());
  ASSERT_EQ(Inactive->Instructions.size(), 2u);
  EXPECT_EQ(Inactive->Instructions[0].NextPC, 1u);
  EXPECT_EQ(Inactive->Instructions[1].PC, 1u);
  EXPECT_TRUE(Inactive->Instructions[1].is(Opcode::DUP1));
}

TEST(EVMAnalyzer, EIP8024ConsumptionMatchesTheClosedDeclarativePolicy) {
  DecodeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;
  Amsterdam.Strict = false;

  // This checks that the production decoder consumes bytes consistently with
  // its closed declarative table. It is intentionally not a second semantic
  // oracle: the required fresh-fetch go-ethereum audit differentially executes
  // every candidate and is the authority for the table's EIP-8024 meaning.
  const Opcode ConditionalOpcodes[] = {Opcode::DUPN, Opcode::SWAPN,
                                       Opcode::EXCHANGE};
  for (const Opcode Op : ConditionalOpcodes) {
    for (unsigned Candidate = 0; Candidate <= kByteMax; ++Candidate) {
      SCOPED_TRACE(testing::Message()
                   << opcodeName(Op, Hardfork::Amsterdam).str() << " 0x"
                   << std::hex << Candidate);
      const uint8_t Encoded = static_cast<uint8_t>(Candidate);
      const bool IsValid = isExchange(Op)
                               ? decodeEIP8024Pair(Encoded).has_value()
                               : decodeEIP8024Single(Encoded).has_value();
      const std::vector<uint8_t> Code = {opcodeByte(Op), Encoded,
                                         opcodeByte(Opcode::STOP)};
      auto Decoded = decodeBytecode(Code, Amsterdam);
      ASSERT_TRUE(static_cast<bool>(Decoded))
          << llvm::toString(Decoded.takeError());
      ASSERT_FALSE(Decoded->Instructions.empty());
      const LowInstruction &Instruction = Decoded->Instructions.front();

      EXPECT_EQ(Instruction.NextPC, IsValid ? 2u : 1u);
      EXPECT_EQ(Instruction.Encoding.size(), IsValid ? 2u : 1u);
      EXPECT_EQ(Instruction.ImmediateStatus,
                IsValid ? ImmediateDecodeStatus::Complete
                        : ImmediateDecodeStatus::Invalid);
      EXPECT_EQ(Instruction.isExecutable(), IsValid);
      ASSERT_GE(Decoded->Instructions.size(), 2u);
      EXPECT_EQ(Decoded->Instructions[1].PC, IsValid ? 2u : 1u);
    }
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

TEST(EVMAnalyzer, LinearDecoderPreservesUnknownAndInactiveBytesLosslessly) {
  DecodeOptions StrictDecode;
  auto Unknown = decodeBytecode(std::vector<uint8_t>{0x0c}, StrictDecode);
  ASSERT_TRUE(static_cast<bool>(Unknown))
      << llvm::toString(Unknown.takeError());
  ASSERT_EQ(Unknown->Instructions.size(), 1u);
  EXPECT_EQ(Unknown->Instructions.front().DecodeStatus,
            OpcodeDecodeStatus::Unknown);
  ASSERT_EQ(Unknown->Diagnostics.size(), 1u);

  StrictDecode.Fork = Hardfork::London;
  auto Inactive = decodeBytecode(std::vector<uint8_t>{0x5f}, StrictDecode);
  ASSERT_TRUE(static_cast<bool>(Inactive))
      << llvm::toString(Inactive.takeError());
  ASSERT_EQ(Inactive->Instructions.size(), 1u);
  EXPECT_EQ(Inactive->Instructions.front().DecodeStatus,
            OpcodeDecodeStatus::Inactive);
}

TEST(EVMAnalyzer, LinearDecoderPrechargesDiagnosticCountAndBytes) {
  constexpr uint8_t kUnknownOpcode = 0x0c;
  auto Baseline = decodeBytecode(std::vector<uint8_t>{kUnknownOpcode});
  ASSERT_TRUE(static_cast<bool>(Baseline))
      << llvm::toString(Baseline.takeError());
  ASSERT_EQ(Baseline->Diagnostics.size(), 1u);
  const size_t ExactBytes = Baseline->Diagnostics.front().Message.size();

  DecodeOptions Exact;
  Exact.MaxLowDiagnostics = 1;
  Exact.MaxLowDiagnosticBytes = ExactBytes;
  auto Accepted = decodeBytecode(std::vector<uint8_t>{kUnknownOpcode}, Exact);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());

  auto TooMany = decodeBytecode(
      std::vector<uint8_t>{kUnknownOpcode, kUnknownOpcode}, Exact);
  ASSERT_FALSE(static_cast<bool>(TooMany));
  EXPECT_NE(llvm::toString(TooMany.takeError())
                .find("LowIR diagnostic limit 1 exceeded"),
            std::string::npos);

  DecodeOptions TooManyBytes = Exact;
  --TooManyBytes.MaxLowDiagnosticBytes;
  auto Rejected =
      decodeBytecode(std::vector<uint8_t>{kUnknownOpcode}, TooManyBytes);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(
      llvm::toString(Rejected.takeError()).find("LowIR diagnostic byte limit"),
      std::string::npos);
}

TEST(EVMAnalyzer, StrictModeRejectsOnlyReachableNonExecutableOpcodes) {
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

  // Legacy code has no whole-image opcode validation. Both bytes at pc 3 are
  // dead data skipped by the definite JUMP, so geth never executes their
  // exceptional instruction and strict analysis must accept the program.
  auto DeadUnknown =
      decodeLowIR(std::vector<uint8_t>{0x60, 0x04, 0x56, 0x0c, 0x5b, 0x00});
  ASSERT_TRUE(static_cast<bool>(DeadUnknown))
      << llvm::toString(DeadUnknown.takeError());
  const LowBlock *UnknownData = DeadUnknown->findBlock(3);
  ASSERT_NE(UnknownData, nullptr);
  EXPECT_FALSE(UnknownData->Reachable);

  auto DeadInactive = decodeLowIR(
      std::vector<uint8_t>{0x60, 0x04, 0x56, 0x5f, 0x5b, 0x00}, London);
  ASSERT_TRUE(static_cast<bool>(DeadInactive))
      << llvm::toString(DeadInactive.takeError());
  const LowBlock *InactiveData = DeadInactive->findBlock(3);
  ASSERT_NE(InactiveData, nullptr);
  EXPECT_FALSE(InactiveData->Reachable);

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Accepted = decodeLowIR(std::vector<uint8_t>{0x0c}, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  ASSERT_EQ(Accepted->Diagnostics.size(), 1u);
  EXPECT_FALSE(Accepted->Instructions[0].isExecutable());
  EXPECT_EQ(formatDecodeAnnotation(Accepted->Instructions[0]),
            "opcode=unknown");

  AnalyzeOptions InvalidFork;
  InvalidFork.Fork = static_cast<Hardfork>(kByteMax);
  auto Invalid =
      decodeLowIR(std::vector<uint8_t>{opcodeByte(Opcode::STOP)}, InvalidFork);
  ASSERT_FALSE(static_cast<bool>(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("invalid hardfork"),
            std::string::npos);
}

TEST(EVMAnalyzer, EmptyRuntimeIsAValidStoppedProgram) {
  auto Decoded = decodeBytecode({});
  ASSERT_TRUE(static_cast<bool>(Decoded))
      << llvm::toString(Decoded.takeError());
  EXPECT_TRUE(Decoded->Code.empty());
  EXPECT_TRUE(Decoded->Instructions.empty());

  auto Program = analyze({});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->Low.Code.empty());
  EXPECT_TRUE(Program->Low.Instructions.empty());
  EXPECT_TRUE(Program->Low.Blocks.empty());
  EXPECT_TRUE(Program->Med.Blocks.empty());
  EXPECT_TRUE(Program->High.Functions.empty());
}

TEST(EVMAnalyzer, RelaxedInactiveOpcodesRemainFaultNodesAcrossIRStages) {
  AnalyzeOptions London;
  London.Fork = Hardfork::London;
  London.Strict = false;

  auto Push0 = analyze(
      std::vector<uint8_t>{opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::STOP)},
      London);
  ASSERT_TRUE(static_cast<bool>(Push0)) << llvm::toString(Push0.takeError());
  const LowInstruction &InactivePush0 = Push0->Low.Instructions.front();
  EXPECT_TRUE(InactivePush0.Info.isAssigned());
  EXPECT_EQ(InactivePush0.Info.Name, "PUSH0");
  EXPECT_EQ(InactivePush0.Info.Introduced, Hardfork::Shanghai);
  EXPECT_FALSE(InactivePush0.isExecutable());
  EXPECT_TRUE(InactivePush0.isTerminator());
  EXPECT_EQ(formatDecodeAnnotation(InactivePush0), "opcode=inactive");
  ASSERT_TRUE(
      Push0->Low.Blocks.front().ExitStackHeights.singleton().has_value());
  EXPECT_EQ(*Push0->Low.Blocks.front().ExitStackHeights.singleton(), 0u);
  ASSERT_FALSE(Push0->Med.Blocks.front().Operations.empty());
  EXPECT_TRUE(Push0->Med.Blocks.front().Operations.front().Outputs.empty());
  EXPECT_TRUE(Push0->Med.Blocks.front().ExitStack.empty());

  AnalyzeOptions Shanghai;
  Shanghai.Fork = Hardfork::Shanghai;
  Shanghai.Strict = false;
  auto TStore = analyze(std::vector<uint8_t>{opcodeByte(Opcode::PUSH1), 0,
                                             opcodeByte(Opcode::PUSH1), 0,
                                             opcodeByte(Opcode::TSTORE),
                                             opcodeByte(Opcode::STOP)},
                        Shanghai);
  ASSERT_TRUE(static_cast<bool>(TStore)) << llvm::toString(TStore.takeError());
  EXPECT_TRUE(TStore->High.Storage.empty());

  AnalyzeOptions Frontier;
  Frontier.Fork = Hardfork::Frontier;
  Frontier.Strict = false;
  auto Revert = analyze(std::vector<uint8_t>{opcodeByte(Opcode::PUSH1), 0,
                                             opcodeByte(Opcode::PUSH1), 0,
                                             opcodeByte(Opcode::REVERT)},
                        Frontier);
  ASSERT_TRUE(static_cast<bool>(Revert)) << llvm::toString(Revert.takeError());
  EXPECT_TRUE(Revert->High.Errors.empty());
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
} // namespace
} // namespace neverd::evm
