//===- EVMAnalyzerTests.cpp - staged EVM analysis tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/Analyzer.h"

#include "llvm/Support/Error.h"

#include <limits>

namespace neverd::evm {
namespace {

const MedOperation *findOperation(const EVMMedIR &Med, uint64_t PC) {
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOperation &Operation : Block.Operations)
      if (Operation.PC == PC)
        return &Operation;
  return nullptr;
}

MedOperation *findOperation(EVMMedIR &Med, uint64_t PC) {
  for (MedBlock &Block : Med.Blocks)
    for (MedOperation &Operation : Block.Operations)
      if (Operation.PC == PC)
        return &Operation;
  return nullptr;
}

inline constexpr uint8_t kTestFunctionEntry = 0x0f;
inline constexpr uint32_t kTestSelector = 0x12345678u;

std::vector<uint8_t> dispatcherFor(uint32_t Selector,
                                   std::vector<uint8_t> Body) {
  std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(Selector >> 24),
      static_cast<uint8_t>(Selector >> 16),
      static_cast<uint8_t>(Selector >> 8),
      static_cast<uint8_t>(Selector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kTestFunctionEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
  };
  Code.insert(Code.end(), Body.begin(), Body.end());
  return Code;
}

std::vector<uint8_t> selectorDispatcher(std::vector<uint8_t> Body) {
  return dispatcherFor(kTestSelector, std::move(Body));
}

/// A PUSH32 of \p Value, which is how a payload word or an event topic reaches
/// the stack.
std::vector<uint8_t> pushWord(const llvm::APInt &Value) {
  std::vector<uint8_t> Code{opcodeByte(Opcode::PUSH32)};
  for (unsigned I = kWordBytes; I-- > 0;)
    Code.push_back(static_cast<uint8_t>(
        Value.extractBitsAsZExtValue(kBitsPerByte, I * kBitsPerByte)));
  return Code;
}

/// A PUSH32 of the payload word a revert of \p Selector begins with.
std::vector<uint8_t> pushSelectorPayload(uint32_t Selector) {
  return pushWord(
      llvm::APInt(kWordBits, Selector).shl(kWordBits - kSelectorBits));
}

const KnownSignatureInfo *findSignature(llvm::StringRef Text) {
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Signature == Text)
      return &Info;
  return nullptr;
}

void append(std::vector<uint8_t> &Code, std::vector<uint8_t> Tail) {
  Code.insert(Code.end(), Tail.begin(), Tail.end());
}

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

TEST(EVMAnalyzer, DecodesOfficialEIP8024InstructionBoundaries) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;
  Amsterdam.Strict = false;

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

TEST(EVMAnalyzer, EIP8024ConsumptionPolicyIsExhaustive) {
  DecodeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;
  Amsterdam.Strict = false;

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

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto RelaxedBadJump =
      decodeLowIR(std::vector<uint8_t>{0x60, 0x01, 0x56}, Relaxed);
  ASSERT_TRUE(static_cast<bool>(RelaxedBadJump))
      << llvm::toString(RelaxedBadJump.takeError());
  const LowBlock *RelaxedEntry = RelaxedBadJump->findBlock(kEntryPC);
  ASSERT_NE(RelaxedEntry, nullptr);
  EXPECT_TRUE(RelaxedEntry->Successors.empty());
  EXPECT_FALSE(RelaxedEntry->HasIndirectSuccessor);
  EXPECT_TRUE(
      llvm::any_of(RelaxedBadJump->Diagnostics, [](const Diagnostic &D) {
        return D.PC == 2 &&
               D.Message.find("not a JUMPDEST") != std::string::npos;
      }));
}

TEST(EVMAnalyzer, WholeProgramResolvesJumpTargetCarriedAcrossBlocks) {
  constexpr uint8_t CalleePC = 5;
  constexpr uint8_t ReturnPC = 7;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1), ReturnPC,
      opcodeByte(Opcode::PUSH1), CalleePC,
      opcodeByte(Opcode::JUMP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::JUMP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  EXPECT_TRUE(Low->hasEdge(0, CalleePC, EdgeKind::Jump));
  EXPECT_TRUE(Low->hasEdge(CalleePC, ReturnPC, EdgeKind::Jump));
  const LowBlock *Callee = Low->findBlock(CalleePC);
  ASSERT_NE(Callee, nullptr);
  EXPECT_FALSE(Callee->HasIndirectSuccessor);
  EXPECT_EQ(Callee->EntryStackHeights.singleton(), 1u);
  EXPECT_EQ(Callee->ExitStackHeights.singleton(), 0u);
  const LowBlock *Return = Low->findBlock(ReturnPC);
  ASSERT_NE(Return, nullptr);
  EXPECT_EQ(Return->EntryStackHeights.singleton(), 0u);
}

TEST(EVMAnalyzer, WholeProgramPreservesFiniteTargetsAtStackMerge) {
  constexpr uint8_t AlternatePathPC = 10;
  constexpr uint8_t CalleePC = 16;
  constexpr uint8_t FirstReturnPC = 18;
  constexpr uint8_t SecondReturnPC = 20;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      AlternatePathPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH1),
      FirstReturnPC,
      opcodeByte(Opcode::PUSH1),
      CalleePC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      SecondReturnPC,
      opcodeByte(Opcode::PUSH1),
      CalleePC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  const LowBlock *Callee = Low->findBlock(CalleePC);
  ASSERT_NE(Callee, nullptr);
  EXPECT_FALSE(Callee->HasIndirectSuccessor);
  ASSERT_EQ(Callee->Successors.size(), 2u);
  EXPECT_TRUE(Low->hasEdge(CalleePC, FirstReturnPC, EdgeKind::Jump));
  EXPECT_TRUE(Low->hasEdge(CalleePC, SecondReturnPC, EdgeKind::Jump));
  EXPECT_EQ(Callee->EntryStackHeights.singleton(), 1u);
  EXPECT_EQ(Callee->Successors[0].Target, FirstReturnPC);
  EXPECT_EQ(Callee->Successors[1].Target, SecondReturnPC);

  AnalyzeOptions Widening;
  Widening.MaxAbstractValuesPerSlot = 1;
  auto Widened = decodeLowIR(Code, Widening);
  ASSERT_TRUE(static_cast<bool>(Widened))
      << llvm::toString(Widened.takeError());
  const LowBlock *WidenedCallee = Widened->findBlock(CalleePC);
  ASSERT_NE(WidenedCallee, nullptr);
  EXPECT_TRUE(WidenedCallee->HasIndirectSuccessor);
  EXPECT_EQ(llvm::count_if(WidenedCallee->Successors,
                           [](const LowEdge &Edge) {
                             return Edge.Kind == EdgeKind::Indirect &&
                                    !Edge.Target;
                           }),
            1u);
}

TEST(EVMAnalyzer, WholeProgramDoesNotRejectSpuriousCartesianJumpTarget) {
  constexpr uint8_t AlternatePathPC = 12;
  constexpr uint8_t CalleePC = 20;
  constexpr uint8_t FirstReturnPC = 23;
  constexpr uint8_t SpuriousTargetPC = 24;
  constexpr uint8_t SecondReturnPC = 25;
  constexpr uint8_t FirstOperand = 1;
  constexpr uint8_t SecondOperand = 2;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      AlternatePathPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH1),
      static_cast<uint8_t>(FirstReturnPC - FirstOperand),
      opcodeByte(Opcode::PUSH1),
      FirstOperand,
      opcodeByte(Opcode::PUSH1),
      CalleePC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      static_cast<uint8_t>(SecondReturnPC - SecondOperand),
      opcodeByte(Opcode::PUSH1),
      SecondOperand,
      opcodeByte(Opcode::PUSH1),
      CalleePC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::ADD),
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  EXPECT_TRUE(Low->hasEdge(CalleePC, FirstReturnPC, EdgeKind::Jump));
  EXPECT_TRUE(Low->hasEdge(CalleePC, SecondReturnPC, EdgeKind::Jump));
  EXPECT_FALSE(Low->hasEdge(CalleePC, SpuriousTargetPC, EdgeKind::Jump));
  EXPECT_TRUE(llvm::any_of(Low->Diagnostics, [](const Diagnostic &D) {
    return D.Message.find(kOverapproximatedJumpTargetPrefix.str()) !=
           std::string::npos;
  }));
}

TEST(EVMAnalyzer, WholeProgramFixedPointDeduplicatesLoopEdges) {
  constexpr uint8_t LoopPC = 5;
  constexpr uint8_t LoopExitPC = 11;
  constexpr uint8_t ReturnPC = 12;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),    ReturnPC,
      opcodeByte(Opcode::PUSH1),    LoopPC,
      opcodeByte(Opcode::JUMP),     opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH0),    opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),    LoopPC,
      opcodeByte(Opcode::JUMPI),    opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP),
  };

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  const LowBlock *Loop = Low->findBlock(LoopPC);
  ASSERT_NE(Loop, nullptr);
  ASSERT_EQ(Loop->Successors.size(), 2u);
  EXPECT_TRUE(Low->hasEdge(LoopPC, LoopPC, EdgeKind::ConditionalTrue));
  EXPECT_TRUE(Low->hasEdge(LoopPC, LoopExitPC, EdgeKind::ConditionalFalse));
  EXPECT_EQ(Loop->Predecessors, (std::vector<uint64_t>{0, LoopPC}));
  EXPECT_EQ(Loop->EntryStackHeights.singleton(), 1u);
  EXPECT_TRUE(Low->hasEdge(LoopExitPC, ReturnPC, EdgeKind::Jump));
}

TEST(EVMAnalyzer, WholeProgramPreservesPathDependentStackHeights) {
  constexpr uint8_t TallPathPC = 8;
  constexpr uint8_t JoinPC = 14;
  constexpr uint8_t CarriedValue = 42;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      TallPathPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH1),
      JoinPC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      CarriedValue,
      opcodeByte(Opcode::PUSH1),
      JoinPC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const LowBlock *Join = Program->Low.findBlock(JoinPC);
  ASSERT_NE(Join, nullptr);
  ASSERT_EQ(Join->EntryStackHeights.values().size(), 2u);
  EXPECT_EQ(Join->EntryStackHeights.values()[0], 0u);
  EXPECT_EQ(Join->EntryStackHeights.values()[1], 1u);
  EXPECT_FALSE(Join->EntryStackHeights.singleton().has_value());
  EXPECT_EQ(Join->ExitStackHeights.values(), Join->EntryStackHeights.values());
  EXPECT_TRUE(llvm::any_of(Program->Med.Diagnostics, [](const Diagnostic &D) {
    return D.Message == kPolymorphicStackDiagnostic;
  }));

  AnalyzeOptions Bounded;
  Bounded.MaxStackHeightVariants = 1;
  auto Limited = decodeLowIR(Code, Bounded);
  ASSERT_FALSE(static_cast<bool>(Limited));
  EXPECT_NE(llvm::toString(Limited.takeError())
                .find("stack-height variant limit 1 exceeded"),
            std::string::npos);
}

TEST(EVMAnalyzer, WholeProgramRejectsZeroAnalysisBounds) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::STOP)};

  AnalyzeOptions NoValues;
  NoValues.MaxAbstractValuesPerSlot = 0;
  auto Values = decodeLowIR(Code, NoValues);
  ASSERT_FALSE(static_cast<bool>(Values));
  EXPECT_NE(llvm::toString(Values.takeError())
                .find("MaxAbstractValuesPerSlot must be greater than zero"),
            std::string::npos);

  AnalyzeOptions NoHeights;
  NoHeights.MaxStackHeightVariants = 0;
  auto Heights = decodeLowIR(Code, NoHeights);
  ASSERT_FALSE(static_cast<bool>(Heights));
  EXPECT_NE(llvm::toString(Heights.takeError())
                .find("MaxStackHeightVariants must be greater than zero"),
            std::string::npos);
}

TEST(EVMAnalyzer, WholeProgramModelsUnknownAndInfeasibleJumpsHonestly) {
  const std::vector<uint8_t> UnknownTarget = {opcodeByte(Opcode::PUSH0),
                                              opcodeByte(Opcode::CALLDATALOAD),
                                              opcodeByte(Opcode::JUMP)};
  auto Unknown = decodeLowIR(UnknownTarget);
  ASSERT_TRUE(static_cast<bool>(Unknown))
      << llvm::toString(Unknown.takeError());
  const LowBlock *UnknownEntry = Unknown->findBlock(kEntryPC);
  ASSERT_NE(UnknownEntry, nullptr);
  ASSERT_EQ(UnknownEntry->Successors.size(), 1u);
  EXPECT_TRUE(UnknownEntry->HasIndirectSuccessor);
  EXPECT_EQ(UnknownEntry->Successors.front().Kind, EdgeKind::Indirect);
  EXPECT_FALSE(UnknownEntry->Successors.front().Target.has_value());

  constexpr uint8_t InfeasibleTargetPC = 1;
  constexpr uint8_t FallthroughPC = 4;
  const std::vector<uint8_t> FalseCondition = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH1), InfeasibleTargetPC,
      opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::STOP),
  };
  auto Feasible = decodeLowIR(FalseCondition);
  ASSERT_TRUE(static_cast<bool>(Feasible))
      << llvm::toString(Feasible.takeError());
  EXPECT_TRUE(
      Feasible->hasEdge(kEntryPC, FallthroughPC, EdgeKind::ConditionalFalse));
  EXPECT_TRUE(Feasible->Diagnostics.empty());
}

TEST(EVMAnalyzer, WholeProgramRelaxedUnderflowTerminatesTheFaultingPath) {
  constexpr uint8_t UnreachablePC = 1;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::ADD),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::STOP)};

  auto Strict = decodeLowIR(Code);
  ASSERT_FALSE(static_cast<bool>(Strict));
  EXPECT_NE(llvm::toString(Strict.takeError()).find("stack underflow in ADD"),
            std::string::npos);

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Preserved = decodeLowIR(Code, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Preserved))
      << llvm::toString(Preserved.takeError());
  const LowBlock *Entry = Preserved->findBlock(kEntryPC);
  const LowBlock *Unreachable = Preserved->findBlock(UnreachablePC);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Unreachable, nullptr);
  EXPECT_TRUE(Entry->Successors.empty());
  EXPECT_FALSE(Unreachable->Reachable);
  EXPECT_TRUE(Unreachable->EntryStackHeights.empty());
  EXPECT_TRUE(llvm::any_of(Preserved->Diagnostics, [](const Diagnostic &D) {
    return D.PC == kEntryPC && D.Message == "stack underflow in ADD";
  }));
}

TEST(EVMAnalyzer, MediumIRPropagatesConstantThroughCrossBlockPhi) {
  constexpr uint8_t kTrueBlock = 0x0b;
  constexpr uint8_t kMergeBlock = 0x0f;
  constexpr uint64_t kAddPC = 0x10;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::CALLDATASIZE),
      opcodeByte(Opcode::PUSH1),
      kTrueBlock,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH1),
      kMergeBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      kMergeBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::ADD),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const MedOperation *Add = findOperation(Program->Med, kAddPC);
  ASSERT_NE(Add, nullptr);
  ASSERT_EQ(Add->Outputs.size(), 1u);
  const MedValue *Sum = Program->Med.findValue(Add->Outputs.front());
  ASSERT_NE(Sum, nullptr);
  ASSERT_TRUE(Sum->Constant.has_value());
  EXPECT_EQ(Sum->Constant->getZExtValue(), 3u);
}

TEST(EVMAnalyzer, MediumIRPropagatesConstantThroughLoopPhi) {
  constexpr uint8_t kLoopBlock = 5;
  constexpr uint64_t kCopyPC = 6;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),
      42,
      opcodeByte(Opcode::PUSH1),
      kLoopBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::SWAP1),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kLoopBlock,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const MedBlock &Loop = Program->Med.Blocks.at(1);
  ASSERT_EQ(Loop.StartPC, kLoopBlock);
  ASSERT_EQ(Loop.PhiValues.size(), 1u);
  const MedValue *Phi = Program->Med.findValue(Loop.PhiValues.front());
  ASSERT_NE(Phi, nullptr);
  ASSERT_TRUE(Phi->Constant.has_value());
  EXPECT_EQ(Phi->Constant->getZExtValue(), 42u);

  const MedOperation *Copy = findOperation(Program->Med, kCopyPC);
  ASSERT_NE(Copy, nullptr);
  ASSERT_EQ(Copy->Outputs.size(), 1u);
  const MedValue *Copied = Program->Med.findValue(Copy->Outputs.front());
  ASSERT_NE(Copied, nullptr);
  ASSERT_TRUE(Copied->Constant.has_value());
  EXPECT_EQ(Copied->Constant->getZExtValue(), 42u);
}

TEST(EVMAnalyzer, MediumIROverdefinesConflictingLoopPhi) {
  constexpr uint8_t kLoopBlock = 5;
  constexpr uint64_t kAddPC = 8;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),
      42,
      opcodeByte(Opcode::PUSH1),
      kLoopBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::ADD),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kLoopBlock,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const MedBlock &Loop = Program->Med.Blocks.at(1);
  ASSERT_EQ(Loop.StartPC, kLoopBlock);
  ASSERT_EQ(Loop.PhiValues.size(), 1u);
  const MedValue *Phi = Program->Med.findValue(Loop.PhiValues.front());
  ASSERT_NE(Phi, nullptr);
  EXPECT_FALSE(Phi->Constant.has_value());

  const MedOperation *Add = findOperation(Program->Med, kAddPC);
  ASSERT_NE(Add, nullptr);
  ASSERT_EQ(Add->Outputs.size(), 1u);
  const MedValue *Incremented = Program->Med.findValue(Add->Outputs.front());
  ASSERT_NE(Incremented, nullptr);
  EXPECT_FALSE(Incremented->Constant.has_value());
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

TEST(EVMAnalyzer, PreservesOrthogonalSemanticPropertiesInMediumIR) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),       opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),       opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::EXTCODECOPY), opcodeByte(Opcode::CALLVALUE),
      opcodeByte(Opcode::STOP)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const MedOperation &Copy = Program->Med.Blocks.front().Operations[4];
  EXPECT_EQ(Copy.Effect, EffectKind::ContextRead);
  EXPECT_EQ(Copy.MemoryAccess, MemoryAccessKind::Write);
  EXPECT_EQ(Copy.CallValueAccess, CallValueAccessKind::None);
  const MedOperation &Value = Program->Med.Blocks.front().Operations[5];
  EXPECT_EQ(Value.Effect, EffectKind::ContextRead);
  EXPECT_EQ(Value.StateAccess, StateAccessKind::Read);
  EXPECT_EQ(Value.CallValueAccess, CallValueAccessKind::Read);
  const std::string Dump = dumpMedIR(Program->Med);
  EXPECT_NE(Dump.find("context.read, memory.write"), std::string::npos);
  EXPECT_NE(Dump.find("context.read, state.read, callvalue.read"),
            std::string::npos);
}

TEST(EVMAnalyzer, RecoversSelectorThroughNonAdjacentMedIRDataflow) {
  constexpr uint8_t kFunctionEntry = 0x17;
  constexpr uint32_t kSelector = 0x12345678;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),
      0,
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kFunctionEntry,
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::SWAP1),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kSelector);
  EXPECT_EQ(Program->High.Functions.front().EntryPC, kFunctionEntry);
}

TEST(EVMAnalyzer, RecoversEquivalentSelectorPhi) {
  constexpr uint8_t kTrueBlock = 0x0c;
  constexpr uint8_t kMergeBlock = 0x15;
  constexpr uint8_t kFunctionEntry = 0x24;
  constexpr uint32_t kSelector = 0x12345678;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE),
      opcodeByte(Opcode::PUSH1),
      kTrueBlock,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH1),
      kMergeBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH1),
      kMergeBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kFunctionEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kSelector);
  EXPECT_EQ(Program->High.Functions.front().EntryPC, kFunctionEntry);
}

TEST(EVMAnalyzer, RecoversSelectorWithReversedEqualityAndDerivedMask) {
  const auto Build = [](bool ReverseEquality, bool MaskSelector) {
    std::vector<uint8_t> Code = {
        opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
        opcodeByte(Opcode::PUSH1), kWordBits - kSelectorBits,
        opcodeByte(Opcode::SHR),
    };
    if (MaskSelector) {
      Code.insert(Code.end(), {opcodeByte(Opcode::PUSH4), 0xff, 0xff, 0xff,
                               0xff, opcodeByte(Opcode::AND)});
    }
    Code.insert(Code.end(),
                {opcodeByte(Opcode::PUSH4), 0x12, 0x34, 0x56, 0x78});
    if (ReverseEquality)
      Code.push_back(opcodeByte(Opcode::SWAP1));
    Code.push_back(opcodeByte(Opcode::EQ));
    Code.push_back(opcodeByte(Opcode::PUSH1));
    const size_t DestinationIndex = Code.size();
    Code.push_back(0);
    Code.push_back(opcodeByte(Opcode::JUMPI));
    Code.push_back(opcodeByte(Opcode::STOP));
    const uint64_t EntryPC = Code.size();
    EXPECT_LE(EntryPC, kByteMax);
    Code[DestinationIndex] = static_cast<uint8_t>(EntryPC);
    Code.push_back(opcodeByte(Opcode::JUMPDEST));
    Code.push_back(opcodeByte(Opcode::STOP));
    return std::pair{std::move(Code), EntryPC};
  };

  for (const auto [ReverseEquality, MaskSelector] :
       {std::pair{true, false}, std::pair{false, true}}) {
    SCOPED_TRACE(testing::Message()
                 << "reverse=" << ReverseEquality << " mask=" << MaskSelector);
    auto [Code, EntryPC] = Build(ReverseEquality, MaskSelector);
    auto Program = analyze(Code);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->High.Functions.size(), 1u);
    EXPECT_EQ(Program->High.Functions.front().Selector, 0x12345678u);
    EXPECT_EQ(Program->High.Functions.front().EntryPC, EntryPC);
  }
}

TEST(EVMAnalyzer, RejectsSelectorConstantsWiderThanTheABISelector) {
  constexpr uint8_t kFunctionEntry = 0x10;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH5),
      1,
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kFunctionEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Functions.empty());
}

TEST(EVMAnalyzer, RejectsMixedOrCyclicSelectorPhi) {
  constexpr uint8_t kTrueBlock = 0x0c;
  constexpr uint8_t kMergeBlock = 0x11;
  constexpr uint8_t kFunctionEntry = 0x1c;
  const std::vector<uint8_t> MixedCode = {
      opcodeByte(Opcode::CALLDATASIZE),
      opcodeByte(Opcode::PUSH1),
      kTrueBlock,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH1),
      kMergeBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::CALLVALUE),
      opcodeByte(Opcode::PUSH1),
      kMergeBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kFunctionEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Mixed = analyze(MixedCode);
  ASSERT_TRUE(static_cast<bool>(Mixed)) << llvm::toString(Mixed.takeError());
  EXPECT_TRUE(Mixed->High.Functions.empty());

  constexpr uint64_t kEqualityPC = 0x0a;
  constexpr uint8_t kCyclicFunctionEntry = 0x0f;
  const std::vector<uint8_t> CyclicCode = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kCyclicFunctionEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };
  auto Cyclic = analyze(CyclicCode);
  ASSERT_TRUE(static_cast<bool>(Cyclic)) << llvm::toString(Cyclic.takeError());
  MedOperation *Equality = findOperation(Cyclic->Med, kEqualityPC);
  ASSERT_NE(Equality, nullptr);
  ASSERT_EQ(Equality->Inputs.size(), 2u);
  ASSERT_EQ(Equality->Outputs.size(), 1u);
  const ValueID CycleID = static_cast<ValueID>(Cyclic->Med.Values.size());
  MedValue Cycle;
  Cycle.ID = CycleID;
  Cycle.Kind = ValueKind::Phi;
  Cycle.PC = kEqualityPC;
  Cycle.Name = kStackPhiValueName.str();
  Cycle.Inputs.push_back(CycleID);
  Cyclic->Med.Values.push_back(std::move(Cycle));
  Equality->Inputs[1] = CycleID;
  Cyclic->Med.Values[Equality->Outputs.front()].Inputs = Equality->Inputs;

  const EVMHighIR Recovered = recoverHighIR(Cyclic->Low, Cyclic->Med);
  EXPECT_TRUE(Recovered.Functions.empty());
}

TEST(EVMAnalyzer, HighIRProducerWalkHandlesDeepChainsIteratively) {
  constexpr uint64_t kEqualityPC = 0x0a;
  constexpr size_t kDeepSemanticValueChainLength = 16'384;
  auto Program = analyze(selectorDispatcher({opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  MedOperation *Equality = findOperation(Program->Med, kEqualityPC);
  ASSERT_NE(Equality, nullptr);
  ASSERT_EQ(Equality->Inputs.size(), 2u);
  ASSERT_EQ(Equality->Outputs.size(), 1u);

  ValueID Previous = Equality->Inputs[1];
  Program->Med.Values.reserve(Program->Med.Values.size() +
                              kDeepSemanticValueChainLength);
  for (size_t I = 0; I < kDeepSemanticValueChainLength; ++I) {
    const ValueID ID = static_cast<ValueID>(Program->Med.Values.size());
    MedValue Phi;
    Phi.ID = ID;
    Phi.Kind = ValueKind::Phi;
    Phi.PC = kEqualityPC;
    Phi.Name = kStackPhiValueName.str();
    Phi.Inputs.push_back(Previous);
    Program->Med.Values.push_back(std::move(Phi));
    Previous = ID;
  }
  Equality->Inputs[1] = Previous;
  Program->Med.Values[Equality->Outputs.front()].Inputs = Equality->Inputs;

  const EVMHighIR Recovered = recoverHighIR(Program->Low, Program->Med);
  ASSERT_EQ(Recovered.Functions.size(), 1u);
  EXPECT_EQ(Recovered.Functions.front().Selector, 0x12345678u);
  EXPECT_EQ(Recovered.Functions.front().EntryPC, kTestFunctionEntry);
}

TEST(EVMAnalyzer, MalformedMedIRDisablesValueRecoveryDeterministically) {
  constexpr uint64_t kJumpPC = 0x0d;
  auto Program = analyze(selectorDispatcher({opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  MedOperation *Jump = findOperation(Program->Med, kJumpPC);
  ASSERT_NE(Jump, nullptr);
  ASSERT_EQ(Jump->Inputs.size(), 2u);
  Jump->Inputs[1] = std::numeric_limits<ValueID>::max();

  const EVMHighIR Recovered = recoverHighIR(Program->Low, Program->Med);
  EXPECT_TRUE(Recovered.Functions.empty());
  EXPECT_TRUE(llvm::any_of(Recovered.Diagnostics, [](const Diagnostic &D) {
    return D.Message == kMalformedMedIRDiagnostic;
  }));
}

TEST(EVMAnalyzer, RecoversStorageAndEventFactsFromTypedOperands) {
  constexpr uint8_t kTopic = 0x7f;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::ADD),
      opcodeByte(Opcode::SLOAD),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::PUSH1),
      kTopic,
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::LOG1),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Storage.size(), 1u);
  ASSERT_TRUE(Program->High.Storage.front().Slot.has_value());
  EXPECT_EQ(Program->High.Storage.front().Slot->getZExtValue(), 3u);
  ASSERT_EQ(Program->High.Events.size(), 1u);
  ASSERT_TRUE(Program->High.Events.front().Topic0.has_value());
  EXPECT_EQ(Program->High.Events.front().Topic0->getZExtValue(), kTopic);
}

// A head slot the body never reads still occupies its position, so reporting
// only the slots that were read would renumber every argument after a gap.
TEST(EVMAnalyzer, ReportsUnreadHeadSlotsSoLaterArgumentsKeepTheirPositions) {
  auto Program = analyze(selectorDispatcher(
      {opcodeByte(Opcode::PUSH1), 0x10, opcodeByte(Opcode::PUSH1), 0x14,
       opcodeByte(Opcode::ADD), opcodeByte(Opcode::CALLDATALOAD),
       opcodeByte(Opcode::POP), opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  const RecoveredFunction &Function = Program->High.Functions.front();
  ASSERT_EQ(Function.Arguments.size(), 2u);
  EXPECT_EQ(Function.Arguments[0].Index, 0u);
  EXPECT_EQ(Function.Arguments[0].CalldataOffset, kSelectorBytes);
  EXPECT_FALSE(Function.Arguments[0].Read);
  EXPECT_EQ(Function.Arguments[1].Index, 1u);
  EXPECT_EQ(Function.Arguments[1].CalldataOffset, 0x24u);
  EXPECT_TRUE(Function.Arguments[1].Read);
}

// An offset that lands inside a head slot reads into a dynamic value's
// payload; treating it as an argument of its own would invent one.
TEST(EVMAnalyzer, IgnoresCalldataReadsThatDoNotStartAHeadSlot) {
  auto Program = analyze(selectorDispatcher(
      {opcodeByte(Opcode::PUSH1), 0x30, opcodeByte(Opcode::CALLDATALOAD),
       opcodeByte(Opcode::POP), opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_TRUE(Program->High.Functions.front().Arguments.empty());
}

TEST(EVMAnalyzer, RecoversArgumentTypeFromTheDecoderCleanupMask) {
  std::vector<uint8_t> Body{opcodeByte(Opcode::PUSH1), kSelectorBytes,
                            opcodeByte(Opcode::CALLDATALOAD)};
  append(Body, pushWord(llvm::APInt::getLowBitsSet(kWordBits, kAddressBits)));
  append(Body, {opcodeByte(Opcode::AND), opcodeByte(Opcode::POP),
                opcodeByte(Opcode::STOP)});

  auto Program = analyze(selectorDispatcher(std::move(Body)));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  ASSERT_EQ(Program->High.Functions.front().Arguments.size(), 1u);
  const RecoveredArgument &Argument =
      Program->High.Functions.front().Arguments.front();
  EXPECT_EQ(Argument.Type, "address");
  EXPECT_EQ(Argument.TypeSource, ABITypeSource::Dataflow);
}

TEST(EVMAnalyzer, RecoversSignedArgumentFromSignExtension) {
  auto Program = analyze(selectorDispatcher(
      {opcodeByte(Opcode::PUSH1), kSelectorBytes,
       opcodeByte(Opcode::CALLDATALOAD), opcodeByte(Opcode::PUSH1), 0x00,
       opcodeByte(Opcode::SIGNEXTEND), opcodeByte(Opcode::POP),
       opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  ASSERT_EQ(Program->High.Functions.front().Arguments.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Arguments.front().Type, "int8");
}

// A mask reaches an argument through the duplicate the decoder makes of it, so
// following only the loaded value itself would miss every real cleanup.
TEST(EVMAnalyzer, FollowsAnArgumentThroughTheDuplicateThatIsMasked) {
  std::vector<uint8_t> Body{opcodeByte(Opcode::PUSH1), kSelectorBytes,
                            opcodeByte(Opcode::CALLDATALOAD),
                            opcodeByte(Opcode::DUP1)};
  append(Body, pushWord(llvm::APInt::getLowBitsSet(kWordBits, kAddressBits)));
  append(Body, {opcodeByte(Opcode::AND), opcodeByte(Opcode::POP),
                opcodeByte(Opcode::POP), opcodeByte(Opcode::STOP)});

  auto Program = analyze(selectorDispatcher(std::move(Body)));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  ASSERT_EQ(Program->High.Functions.front().Arguments.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Arguments.front().Type, "address");
}

TEST(EVMAnalyzer, NamesASelectorThatHashesToATabulatedSignature) {
  const KnownSignatureInfo *Transfer =
      findSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  auto Program =
      analyze(dispatcherFor(Transfer->Selector, {opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  const RecoveredFunction &Function = Program->High.Functions.front();
  EXPECT_EQ(Function.Known, Transfer);
  EXPECT_EQ(Function.Name, "transfer");

  // The hashed signature settles the argument list even though the body reads
  // nothing, which no amount of dataflow could establish.
  ASSERT_EQ(Function.Arguments.size(), 2u);
  EXPECT_EQ(Function.Arguments[0].Type, "address");
  EXPECT_EQ(Function.Arguments[1].Type, "uint256");
  EXPECT_EQ(Function.Arguments[0].TypeSource, ABITypeSource::KnownSignature);
  EXPECT_FALSE(Function.Arguments[0].Read);
  ASSERT_EQ(Function.Returns.size(), 1u);
  EXPECT_EQ(Function.Returns.front(), "bool");
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::KnownSignature);

  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC20);
}

TEST(EVMAnalyzer, LeavesAnUnknownSelectorNamedAfterItsBytes) {
  auto Program = analyze(selectorDispatcher({opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Known, nullptr);
  EXPECT_EQ(Program->High.Functions.front().Name, "func_12345678");
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMAnalyzer, NamesAnEventThatHashesToATabulatedTopic) {
  const KnownSignatureInfo *Transfer =
      findSignature("Transfer(address,address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  std::vector<uint8_t> Code = pushWord(Transfer->Topic);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::LOG1), opcodeByte(Opcode::STOP)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Events.size(), 1u);
  EXPECT_EQ(Program->High.Events.front().Known, Transfer);
  EXPECT_EQ(Program->High.Events.front().SuggestedName, "Transfer");
}

TEST(EVMAnalyzer, ClassifiesTheRevertPayloadTheLanguageEmits) {
  const KnownSignatureInfo &Message =
      getLanguageRevertInfo(LanguageRevert::Message);

  std::vector<uint8_t> Code = pushSelectorPayload(Message.Selector);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                opcodeByte(Opcode::PUSH1), 0x24, opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  const ErrorFact &Error = Program->High.Errors.front();
  EXPECT_EQ(Error.Kind, RevertKind::Message);
  EXPECT_EQ(Error.Known, &Message);
  EXPECT_EQ(Error.SuggestedName, "Error");
}

TEST(EVMAnalyzer, RecoversWhichCompilerCheckAPanicReports) {
  const KnownSignatureInfo &Panic =
      getLanguageRevertInfo(LanguageRevert::Panic);

  std::vector<uint8_t> Code = pushSelectorPayload(Panic.Selector);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                opcodeByte(Opcode::PUSH1),
                static_cast<uint8_t>(PanicCode::ArithmeticOverflow),
                opcodeByte(Opcode::PUSH1), kSelectorBytes,
                opcodeByte(Opcode::MSTORE), opcodeByte(Opcode::PUSH1), 0x24,
                opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  const ErrorFact &Error = Program->High.Errors.front();
  EXPECT_EQ(Error.Kind, RevertKind::Panic);
  ASSERT_NE(Error.Panic, nullptr);
  EXPECT_EQ(Error.Panic->ID, PanicCode::ArithmeticOverflow);
}

TEST(EVMAnalyzer, ReportsARevertWithNoPayloadAsBare) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::REVERT)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_EQ(Program->High.Errors.front().Kind, RevertKind::Bare);
  EXPECT_FALSE(Program->High.Errors.front().Selector.has_value());
}

// A mapping addresses its elements by hash, which is the difference between a
// declared variable and one the program computed.
TEST(EVMAnalyzer, SeparatesHashedStorageKeysFromDeclaredSlots) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::SHA3),
                                     opcodeByte(Opcode::SLOAD),
                                     opcodeByte(Opcode::POP),
                                     opcodeByte(Opcode::PUSH1),
                                     0x05,
                                     opcodeByte(Opcode::SLOAD),
                                     opcodeByte(Opcode::POP),
                                     opcodeByte(Opcode::STOP)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Storage.size(), 2u);
  EXPECT_EQ(Program->High.Storage[0].KeyKind, StorageKeyKind::Hashed);
  EXPECT_FALSE(Program->High.Storage[0].Slot.has_value());
  EXPECT_EQ(Program->High.Storage[1].KeyKind, StorageKeyKind::Slot);
  ASSERT_TRUE(Program->High.Storage[1].Slot.has_value());
  EXPECT_EQ(Program->High.Storage[1].Slot->getZExtValue(), 5u);
}

TEST(EVMAnalyzer, RecoversWordReturnFromTypedSizeWithoutMemoryWrite) {
  auto Program = analyze(selectorDispatcher(
      {opcodeByte(Opcode::PUSH1), kWordBytes, opcodeByte(Opcode::PUSH0),
       opcodeByte(Opcode::RETURN)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  ASSERT_EQ(Program->High.Functions.front().Returns.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Returns.front(),
            kDefaultRecoveredWordType);
}

TEST(EVMAnalyzer, DoesNotRecoverWordReturnFromZeroLengthRange) {
  auto Program = analyze(selectorDispatcher(
      {opcodeByte(Opcode::PUSH1), 1, opcodeByte(Opcode::PUSH0),
       opcodeByte(Opcode::MSTORE), opcodeByte(Opcode::PUSH0),
       opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::RETURN)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_TRUE(Program->High.Functions.front().Returns.empty());
}

TEST(EVMAnalyzer, RecoversReceiveGuardThroughMedIRTransport) {
  constexpr uint8_t kReceiveEntry = 0x0d;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::POP),          opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::DUP1),         opcodeByte(Opcode::POP),
      opcodeByte(Opcode::PUSH1),        kReceiveEntry,
      opcodeByte(Opcode::DUP1),         opcodeByte(Opcode::SWAP1),
      opcodeByte(Opcode::POP),          opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),         opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.HasReceive);
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

TEST(EVMAnalyzer, RejectsAmbiguousDuplicateSelectorRecovery) {
  // The same selector is compared twice but branches to different valid
  // entries. HighIR must not silently let the later pattern overwrite the
  // first and present one arbitrary target as recovered source truth.
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),
      0,
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      0xe0,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      0x1b,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      0x1d,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Functions.empty());
  ASSERT_EQ(Program->High.Diagnostics.size(), 1u);
  EXPECT_NE(Program->High.Diagnostics.front().Message.find(
                "duplicate selector 0x12345678"),
            std::string::npos);
}

TEST(EVMAnalyzer, RecoversMutabilityFromCanonicalOpcodeMetadata) {
  constexpr uint8_t kFunctionEntry = 0x15;
  const auto dispatcherFor = [](Opcode BodyOpcode, uint8_t StackPops) {
    std::vector<uint8_t> Code = {
        opcodeByte(Opcode::PUSH1),
        0,
        opcodeByte(Opcode::CALLDATALOAD),
        opcodeByte(Opcode::PUSH1),
        0xe0,
        opcodeByte(Opcode::SHR),
        opcodeByte(Opcode::DUP1),
        opcodeByte(Opcode::PUSH4),
        0x12,
        0x34,
        0x56,
        0x78,
        opcodeByte(Opcode::EQ),
        opcodeByte(Opcode::PUSH1),
        kFunctionEntry,
        opcodeByte(Opcode::JUMPI),
        opcodeByte(Opcode::JUMPDEST),
        opcodeByte(Opcode::PUSH1),
        0,
        opcodeByte(Opcode::DUP1),
        opcodeByte(Opcode::REVERT),
        opcodeByte(Opcode::JUMPDEST),
    };
    Code.insert(Code.end(), StackPops, opcodeByte(Opcode::PUSH0));
    Code.push_back(opcodeByte(BodyOpcode));
    if (opcodeInfo(BodyOpcode)->StackPushes != 0)
      Code.push_back(opcodeByte(Opcode::POP));
    Code.push_back(opcodeByte(Opcode::STOP));
    return Code;
  };

  const struct {
    Opcode Op;
    uint8_t StackPops;
    Mutability Expected;
  } Cases[] = {
      {Opcode::SHA3, 2, Mutability::Pure},
      {Opcode::INVALID, 0, Mutability::Pure},
      {Opcode::ADDRESS, 0, Mutability::View},
      {Opcode::CALLVALUE, 0, Mutability::Payable},
      {Opcode::STATICCALL, 6, Mutability::View},
      {Opcode::CALL, 7, Mutability::NonPayable},
  };

  for (const auto &Case : Cases) {
    SCOPED_TRACE(opcodeName(Case.Op).str());
    auto Program = analyze(dispatcherFor(Case.Op, Case.StackPops));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->High.Functions.size(), 1u);
    EXPECT_EQ(Program->High.Functions.front().StateMutability, Case.Expected);
  }

  // Canonical Solidity non-payable guard:
  // CALLVALUE; DUP1; ISZERO; PUSH1 continuation; JUMPI; PUSH0; DUP1; REVERT.
  // The guard itself is not a source-level msg.value read and must not turn an
  // otherwise pure recovered body into view/payable.
  constexpr uint8_t kGuardContinuation = 0x1f;
  auto Guarded = dispatcherFor(Opcode::STOP, 0);
  Guarded.resize(kFunctionEntry + 1);
  Guarded.insert(Guarded.end(),
                 {opcodeByte(Opcode::CALLVALUE), opcodeByte(Opcode::DUP1),
                  opcodeByte(Opcode::ISZERO), opcodeByte(Opcode::PUSH1),
                  kGuardContinuation, opcodeByte(Opcode::JUMPI),
                  opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::DUP1),
                  opcodeByte(Opcode::REVERT), opcodeByte(Opcode::JUMPDEST),
                  opcodeByte(Opcode::POP), opcodeByte(Opcode::STOP)});
  auto GuardedProgram = analyze(Guarded);
  ASSERT_TRUE(static_cast<bool>(GuardedProgram))
      << llvm::toString(GuardedProgram.takeError());
  ASSERT_EQ(GuardedProgram->High.Functions.size(), 1u);
  EXPECT_EQ(GuardedProgram->High.Functions.front().StateMutability,
            Mutability::Pure);

  // A dynamic jump can reach code outside the recovered region. Do not claim
  // pure/view when the complete state-access set cannot be proven.
  auto IndirectCode = dispatcherFor(Opcode::CALLDATALOAD, 1);
  ASSERT_GE(IndirectCode.size(), 2u);
  IndirectCode[IndirectCode.size() - 2] = opcodeByte(Opcode::JUMP);
  auto Indirect = analyze(IndirectCode);
  ASSERT_TRUE(static_cast<bool>(Indirect))
      << llvm::toString(Indirect.takeError());
  ASSERT_EQ(Indirect->High.Functions.size(), 1u);
  EXPECT_EQ(Indirect->High.Functions.front().StateMutability,
            Mutability::NonPayable);
}

TEST(EVMAnalyzer, DisabledHighLevelRecoveryProducesNoRecoveredFacts) {
  AnalyzeOptions Options;
  Options.RecoverHighLevel = false;
  auto Program = analyze(std::vector<uint8_t>{opcodeByte(Opcode::PUSH0),
                                              opcodeByte(Opcode::SLOAD),
                                              opcodeByte(Opcode::STOP)},
                         Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Functions.empty());
  EXPECT_TRUE(Program->High.Storage.empty());
  EXPECT_TRUE(Program->High.Regions.empty());
  EXPECT_FALSE(Program->High.HasFallback);
  EXPECT_FALSE(Program->High.HasReceive);
}

} // namespace
} // namespace neverd::evm
