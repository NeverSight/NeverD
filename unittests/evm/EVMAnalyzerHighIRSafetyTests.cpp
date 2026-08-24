//===- EVMAnalyzerHighIRSafetyTests.cpp - HighIR boundary safety tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"

#include <limits>
#include <string>
#include <vector>

namespace neverd::evm {
namespace {

void expectMalformedMedIRFailsClosed(llvm::Expected<EVMHighIR> Recovered,
                                     uint64_t FailurePC) {
  ASSERT_TRUE(static_cast<bool>(Recovered))
      << llvm::toString(Recovered.takeError());
  EXPECT_TRUE(Recovered->Functions.empty());
  EXPECT_TRUE(Recovered->Storage.empty());
  EXPECT_TRUE(Recovered->Events.empty());
  EXPECT_TRUE(Recovered->Errors.empty());
  EXPECT_TRUE(Recovered->Calls.empty());
  EXPECT_TRUE(Recovered->Regions.empty());
  EXPECT_FALSE(Recovered->HasFallback);
  EXPECT_FALSE(Recovered->HasReceive);
  EXPECT_TRUE(
      llvm::any_of(Recovered->Diagnostics, [&](const Diagnostic &Diagnostic) {
        return Diagnostic.PC == FailurePC &&
               Diagnostic.Message == kMalformedMedIRDiagnostic;
      }));
}

std::vector<uint8_t> crossBlockRevertPayload() {
  constexpr uint32_t kErrorSelector = 0x118cdaa7;
  constexpr uint8_t kRevertBlock = 0x26;
  const llvm::APInt Payload =
      llvm::APInt(kWordBits, kErrorSelector).shl(kWordBits - kSelectorBits);
  std::vector<uint8_t> Code{opcodeByte(Opcode::PUSH32)};
  for (unsigned I = kWordBytes; I-- > 0;)
    Code.push_back(static_cast<uint8_t>(
        Payload.extractBitsAsZExtValue(kBitsPerByte, I * kBitsPerByte)));
  const std::vector<uint8_t> Tail = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
      opcodeByte(Opcode::PUSH1), kRevertBlock,
      opcodeByte(Opcode::JUMP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1), kSelectorBytes,
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT),
  };
  Code.insert(Code.end(), Tail.begin(), Tail.end());
  return Code;
}

TEST(EVMAnalyzer, MalformedMedIRLaneIdentityFailsClosedBeforeIndexing) {
  auto Program = analyze({opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Med.StateLanes.size(), 1u);
  const uint64_t FailurePC = Program->Med.StateLanes.front().LowLane.BlockPC;
  Program->Med.StateLanes.front().ID = kInvalidMedStateLaneID;

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  FailurePC);
}

TEST(EVMAnalyzer, MalformedMedIRPhiLaneFailsClosedBeforeIndexing) {
  constexpr uint8_t kTrueBlock = 0x0a;
  constexpr uint8_t kJoinBlock = 0x0d;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kTrueBlock,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::PUSH1),
      kJoinBlock,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Phi = llvm::find_if(Program->Med.Values, [](const MedValue &Value) {
    return Value.Kind == ValueKind::Phi && !Value.PhiIncomings.empty();
  });
  ASSERT_NE(Phi, Program->Med.Values.end());
  const uint64_t FailurePC = Phi->PC;
  Phi->PhiIncomings.front().SourceLane = kInvalidMedStateLaneID;

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  FailurePC);
}

TEST(EVMAnalyzer, UnknownMedIRValueCannotSmuggleAConstantIntoHighIR) {
  auto Program = analyze({opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  MedValue Forged;
  Forged.ID = static_cast<ValueID>(Program->Med.Values.size());
  Forged.Kind = ValueKind::Unknown;
  Forged.PC = kEntryPC;
  Forged.Constant = llvm::APInt(kWordBits, 1);
  Program->Med.Values.push_back(std::move(Forged));

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  kEntryPC);
}

TEST(EVMAnalyzer, MutatedInstructionConstantFailsCanonicalSCCPValidation) {
  constexpr uint64_t kAddPC = 4;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1), 1,
      opcodeByte(Opcode::PUSH1), 2,
      opcodeByte(Opcode::ADD),   opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Result = llvm::find_if(Program->Med.Values, [](const MedValue &Value) {
    return Value.Kind == ValueKind::Instruction && Value.PC == kAddPC;
  });
  ASSERT_NE(Result, Program->Med.Values.end());
  ASSERT_TRUE(Result->Constant.has_value());
  Result->Constant = llvm::APInt(kWordBits, 4);

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  kAddPC);
}

TEST(EVMAnalyzer, MutatedPhiConstantFailsCanonicalSCCPValidation) {
  constexpr uint8_t kLoopBlock = 5;
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
  ASSERT_GE(Program->Med.Blocks.size(), 2u);
  const MedBlock *Loop = Program->Med.Blocks.data() + 1;
  ASSERT_EQ(Loop->StartPC, kLoopBlock);
  ASSERT_EQ(Loop->PhiValues.size(), 1u);
  MedValue *Phi = &Program->Med.Values[Loop->PhiValues.front()];
  ASSERT_EQ(Phi->Kind, ValueKind::Phi);
  ASSERT_TRUE(Phi->Constant.has_value());
  Phi->Constant = llvm::APInt(kWordBits, 43);

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  kLoopBlock);
}

TEST(EVMAnalyzer, MutatedSStoreInputsFailCanonicalReplay) {
  constexpr uint64_t kSStorePC = 4;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),  1,
      opcodeByte(Opcode::PUSH1),  2,
      opcodeByte(Opcode::SSTORE), opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto SStore = llvm::find_if(Program->Med.Blocks.front().Operations,
                              [](const MedOperation &Operation) {
                                return Operation.Op == Opcode::SSTORE;
                              });
  ASSERT_NE(SStore, Program->Med.Blocks.front().Operations.end());
  ASSERT_EQ(SStore->PC, kSStorePC);
  ASSERT_EQ(SStore->Inputs.size(), 2u);
  std::swap(SStore->Inputs.front(), SStore->Inputs.back());

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  kSStorePC);
}

TEST(EVMAnalyzer, MutatedLowLaneTransitionFailsCanonicalReplay) {
  constexpr uint8_t kTargetPC = 3;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1), kTargetPC,
      opcodeByte(Opcode::JUMP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.LaneTransitions.size(), 1u);
  LowLaneTransition &Transition = Program->Low.LaneTransitions.front();
  const uint64_t FailurePC = Transition.Source.BlockPC;
  ASSERT_EQ(Transition.Kind, EdgeKind::Jump);
  Transition.Kind = EdgeKind::Fallthrough;

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  FailurePC);
}

TEST(EVMAnalyzer, PublicMedIRVerificationHonorsCanonicalSCCPBudget) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1), 1,
      opcodeByte(Opcode::PUSH1), 2,
      opcodeByte(Opcode::ADD),   opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const size_t ExactInitialWorklist = Program->Med.Values.size();
  ASSERT_EQ(ExactInitialWorklist, 3u);

  AnalyzeOptions AtBoundary;
  AtBoundary.MaxMedWorklistUpdates = ExactInitialWorklist;
  auto Accepted = recoverHighIR(Program->Low, Program->Med, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_FALSE(llvm::any_of(Accepted->Diagnostics, [](const Diagnostic &D) {
    return D.Message == kMalformedMedIRDiagnostic;
  }));

  AnalyzeOptions BelowBoundary;
  BelowBoundary.MaxMedWorklistUpdates = ExactInitialWorklist - 1;
  expectMalformedMedIRFailsClosed(
      recoverHighIR(Program->Low, Program->Med, BelowBoundary), kEntryPC);
}

TEST(EVMAnalyzer, MutatedLowInstructionMetadataFailsClosedBeforeHighIR) {
  auto Program = analyze({opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Instructions.size(), 1u);
  ++Program->Low.Instructions.front().Info.StackPops;

  expectMalformedMedIRFailsClosed(recoverHighIR(Program->Low, Program->Med),
                                  kEntryPC);
}

TEST(EVMAnalyzer, HighMemoryWorklistChargesEveryTransferBeforeExecution) {
  constexpr size_t kExactWorklistTransfers = 4;
  const std::vector<uint8_t> Code = crossBlockRevertPayload();

  AnalyzeOptions AtBoundary;
  AtBoundary.MaxHighMemoryWorklistUpdates = kExactWorklistTransfers;
  auto Accepted = analyze(Code, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());

  AnalyzeOptions BelowBoundary;
  BelowBoundary.MaxHighMemoryWorklistUpdates = kExactWorklistTransfers - 1;
  auto Rejected = analyze(Code, BelowBoundary);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError())
                .find(kMaxHighMemoryWorklistUpdatesName.str()),
            std::string::npos);
}

TEST(EVMAnalyzer, HighReferenceAndMemoryBudgetsChargeExactWork) {
  const std::vector<uint8_t> Code = crossBlockRevertPayload();
  const auto ExpectBoundary = [&](size_t AnalyzeOptions::*Member, size_t Exact,
                                  llvm::StringRef Name) {
    SCOPED_TRACE(Name.str());
    AnalyzeOptions AtBoundary;
    AtBoundary.*Member = Exact;
    auto Accepted = analyze(Code, AtBoundary);
    ASSERT_TRUE(static_cast<bool>(Accepted))
        << llvm::toString(Accepted.takeError());

    AnalyzeOptions BelowBoundary;
    BelowBoundary.*Member = Exact - 1;
    auto Rejected = analyze(Code, BelowBoundary);
    ASSERT_FALSE(static_cast<bool>(Rejected));
    EXPECT_NE(llvm::toString(Rejected.takeError()).find(Name.str()),
              std::string::npos);
  };

  constexpr size_t kExactReferenceVisits = 195;
  constexpr size_t kExactTransferCells = 84;
  constexpr size_t kExactValueVisits = 9;
  ExpectBoundary(&AnalyzeOptions::MaxHighReferenceVisits, kExactReferenceVisits,
                 kMaxHighReferenceVisitsName);
  ExpectBoundary(&AnalyzeOptions::MaxHighMemoryTransferCells,
                 kExactTransferCells, kMaxHighMemoryTransferCellsName);
  ExpectBoundary(&AnalyzeOptions::MaxHighMemoryValueVisits, kExactValueVisits,
                 kMaxHighMemoryValueVisitsName);
}

TEST(EVMAnalyzer, HighDiagnosticBudgetsCoverLowAndMedBeforeCopying) {
  constexpr uint8_t kUnknownOpcodeByte = 0x0c;
  constexpr size_t kExactDiagnosticCount = 2;
  constexpr size_t kExactDiagnosticBytes = 38;
  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Program = analyze(std::vector<uint8_t>{kUnknownOpcodeByte}, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Diagnostics.size(), 1u);
  ASSERT_EQ(Program->Med.Diagnostics.size(), 1u);
  ASSERT_EQ(Program->Low.Diagnostics.front().Message.size(),
            kExactDiagnosticBytes / 2);
  ASSERT_EQ(Program->Med.Diagnostics.front().Message.size(),
            kExactDiagnosticBytes / 2);

  AnalyzeOptions AtBoundary = Relaxed;
  AtBoundary.MaxHighDiagnostics = kExactDiagnosticCount;
  AtBoundary.MaxHighDiagnosticBytes = kExactDiagnosticBytes;
  auto Accepted = recoverHighIR(Program->Low, Program->Med, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_FALSE(llvm::any_of(Accepted->Diagnostics, [](const Diagnostic &D) {
    return D.Message == kMalformedMedIRDiagnostic;
  }));

  AnalyzeOptions TooManyDiagnostics = Relaxed;
  TooManyDiagnostics.MaxHighDiagnostics = kExactDiagnosticCount - 1;
  auto CountRejected =
      recoverHighIR(Program->Low, Program->Med, TooManyDiagnostics);
  expectMalformedMedIRFailsClosed(std::move(CountRejected), kEntryPC);

  AnalyzeOptions TooManyDiagnosticBytes = AtBoundary;
  --TooManyDiagnosticBytes.MaxHighDiagnosticBytes;
  auto ByteRejected =
      recoverHighIR(Program->Low, Program->Med, TooManyDiagnosticBytes);
  ASSERT_FALSE(static_cast<bool>(ByteRejected));
  EXPECT_NE(llvm::toString(ByteRejected.takeError())
                .find(kMaxHighDiagnosticBytesName.str()),
            std::string::npos);
}

TEST(EVMAnalyzer, OwnedCanonicalIRStillPaysHighAggregateDiagnosticBudgets) {
  constexpr uint8_t kUnknownOpcodeByte = 0x0c;
  constexpr size_t kExactDiagnosticCount = 2;
  constexpr size_t kExactDiagnosticBytes = 38;
  AnalyzeOptions Exact;
  Exact.Strict = false;
  Exact.MaxHighDiagnostics = kExactDiagnosticCount;
  Exact.MaxHighDiagnosticBytes = kExactDiagnosticBytes;
  auto Accepted = analyze({kUnknownOpcodeByte}, Exact);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_FALSE(
      llvm::any_of(Accepted->High.Diagnostics, [](const Diagnostic &Entry) {
        return Entry.Message == kMalformedMedIRDiagnostic;
      }));

  AnalyzeOptions TooMany = Exact;
  --TooMany.MaxHighDiagnostics;
  TooMany.MaxHighDiagnosticBytes = kMalformedMedIRDiagnostic.size();
  auto CountRejected = analyze({kUnknownOpcodeByte}, TooMany);
  ASSERT_TRUE(static_cast<bool>(CountRejected))
      << llvm::toString(CountRejected.takeError());
  ASSERT_EQ(CountRejected->High.Diagnostics.size(), 1u);
  EXPECT_EQ(CountRejected->High.Diagnostics.front().Message,
            kMalformedMedIRDiagnostic);

  AnalyzeOptions TooManyBytes = Exact;
  --TooManyBytes.MaxHighDiagnosticBytes;
  auto BytesRejected = analyze({kUnknownOpcodeByte}, TooManyBytes);
  ASSERT_FALSE(static_cast<bool>(BytesRejected));
  EXPECT_NE(llvm::toString(BytesRejected.takeError())
                .find(kMaxHighDiagnosticBytesName.str()),
            std::string::npos);
}

TEST(EVMAnalyzer, MalformedDiagnosticHonorsItsExactOutputByteBudget) {
  constexpr size_t kMalformedDiagnosticBytes = 39;
  static_assert(kMalformedMedIRDiagnostic.size() == kMalformedDiagnosticBytes);
  auto Program = analyze({opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Med.StateLanes.size(), 1u);
  Program->Med.StateLanes.front().ID = kInvalidMedStateLaneID;

  AnalyzeOptions AtBoundary;
  AtBoundary.MaxHighDiagnostics = 1;
  AtBoundary.MaxHighDiagnosticBytes = kMalformedDiagnosticBytes;
  expectMalformedMedIRFailsClosed(
      recoverHighIR(Program->Low, Program->Med, AtBoundary), kEntryPC);

  AnalyzeOptions BelowBoundary = AtBoundary;
  --BelowBoundary.MaxHighDiagnosticBytes;
  auto Rejected = recoverHighIR(Program->Low, Program->Med, BelowBoundary);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError())
                .find(kMaxHighDiagnosticBytesName.str()),
            std::string::npos);
}

} // namespace
} // namespace neverd::evm
