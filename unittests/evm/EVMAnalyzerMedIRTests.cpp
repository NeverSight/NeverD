//===- EVMAnalyzerMedIRTests.cpp - EVM medium IR analysis tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMAnalyzerTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/Support/Error.h"

#include <limits>

namespace neverd::evm {
namespace {

using test::kTestFunctionEntry;
using test::selectorDispatcher;

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

TEST(EVMAnalyzer, DefiniteFaultStopsLaterSemanticLowering) {
  AnalyzeOptions Options;
  Options.Strict = false;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::ADD), opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::SLOAD), opcodeByte(Opcode::STOP)};

  auto Program = analyze(Code, Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_EQ(findOperation(Program->Med, 2), nullptr);
  EXPECT_TRUE(Program->High.Storage.empty());
}

TEST(EVMAnalyzer, MediumIRRetainsMayReachableFaultingLanes) {
  constexpr uint8_t DestinationPC = 3;
  constexpr uint8_t FaultPC = 4;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::JUMP), opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::ADD)};

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const LowBlock *Destination = Program->Low.findBlock(DestinationPC);
  ASSERT_NE(Destination, nullptr);
  ASSERT_TRUE(Destination->MayReachable);
  ASSERT_FALSE(Destination->Reachable);

  const MedOperation *DestinationMarker =
      findOperation(Program->Med, DestinationPC);
  ASSERT_NE(DestinationMarker, nullptr);
  EXPECT_TRUE(DestinationMarker->mayExecute());
  EXPECT_FALSE(DestinationMarker->mayFault());
  ASSERT_EQ(DestinationMarker->ExecutingLanes.size(), 1u);

  const MedOperation *Fault = findOperation(Program->Med, FaultPC);
  ASSERT_NE(Fault, nullptr);
  EXPECT_FALSE(Fault->mayExecute());
  EXPECT_TRUE(Fault->mayFault());
  ASSERT_EQ(Fault->FaultingLanes.size(), 1u);
  const MedStateLane *Lane =
      Program->Med.findStateLane(Fault->FaultingLanes.front());
  ASSERT_NE(Lane, nullptr);
  EXPECT_EQ(Lane->Evidence, Reachability::MayReachable);
  EXPECT_EQ(Lane->LowLane.BlockPC, DestinationPC);
  EXPECT_EQ(DestinationMarker->ExecutingLanes.front(), Lane->ID);
}

TEST(EVMAnalyzer, MediumIRSeparatesExecutingAndFaultingStackHeightLanes) {
  constexpr uint8_t TallPathPC = 8;
  constexpr uint8_t JoinPC = 16;
  constexpr uint8_t AddPC = 17;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::CALLDATALOAD),
                                     opcodeByte(Opcode::PUSH1),
                                     TallPathPC,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::PUSH1),
                                     JoinPC,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::PUSH1),
                                     1,
                                     opcodeByte(Opcode::PUSH1),
                                     2,
                                     opcodeByte(Opcode::PUSH1),
                                     JoinPC,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::ADD),
                                     opcodeByte(Opcode::STOP)};

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Program = analyze(Code, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const MedOperation *Add = findOperation(Program->Med, AddPC);
  ASSERT_NE(Add, nullptr);
  ASSERT_EQ(Add->ExecutingLanes.size(), 1u);
  ASSERT_EQ(Add->FaultingLanes.size(), 1u);
  EXPECT_NE(Add->ExecutingLanes.front(), Add->FaultingLanes.front());
  const MedStateLane *Executing =
      Program->Med.findStateLane(Add->ExecutingLanes.front());
  const MedStateLane *Faulting =
      Program->Med.findStateLane(Add->FaultingLanes.front());
  ASSERT_NE(Executing, nullptr);
  ASSERT_NE(Faulting, nullptr);
  EXPECT_EQ(Executing->EntryStack.size(), 2u);
  EXPECT_EQ(Executing->ExitStack.size(), 1u);
  EXPECT_TRUE(Faulting->EntryStack.empty());
  EXPECT_TRUE(Faulting->ExitStack.empty());
}

TEST(EVMAnalyzer, MultiHeightMergeNeverFabricatesAConstant) {
  constexpr uint64_t kSLoadPC = 0x21;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::CALLDATALOAD),
                                     opcodeByte(Opcode::PUSH1),
                                     0x0c,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::PUSH1),
                                     0x07,
                                     opcodeByte(Opcode::PUSH1),
                                     0x01,
                                     opcodeByte(Opcode::PUSH1),
                                     0x15,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::PUSH1),
                                     0x63,
                                     opcodeByte(Opcode::PUSH1),
                                     0x08,
                                     opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::PUSH1),
                                     0x15,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::PUSH1),
                                     0x20,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::POP),
                                     opcodeByte(Opcode::POP),
                                     opcodeByte(Opcode::PUSH1),
                                     0x2a,
                                     opcodeByte(Opcode::PUSH1),
                                     0x20,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::SLOAD),
                                     opcodeByte(Opcode::STOP)};

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const MedOperation *Load = findOperation(Program->Med, kSLoadPC);
  ASSERT_NE(Load, nullptr);
  ASSERT_EQ(Load->Inputs.size(), 1u);
  const MedValue *Key = Program->Med.findValue(Load->Inputs.front());
  ASSERT_NE(Key, nullptr);
  EXPECT_FALSE(Key->Constant.has_value());
  ASSERT_EQ(Key->Kind, ValueKind::Phi);
  ASSERT_EQ(Key->PhiIncomings.size(), Load->ExecutingLanes.size());
  for (size_t Index = 0; Index < Key->PhiIncomings.size(); ++Index) {
    EXPECT_EQ(Key->PhiIncomings[Index].SourceLane, Load->ExecutingLanes[Index]);
    EXPECT_EQ(Key->PhiIncomings[Index].Value, Key->Inputs[Index]);
  }
  ASSERT_EQ(Program->High.Storage.size(), 1u);
  EXPECT_FALSE(Program->High.Storage.front().Slot.has_value());
}

TEST(EVMAnalyzer, MediumIRRejectsConfiguredResourceLimitsBeforeInsertion) {
  const auto ExpectError = [](const EVMLowIR &Low, AnalyzeOptions Options,
                              llvm::StringRef Message) {
    auto Result = lowerToMedIR(Low, Options);
    ASSERT_FALSE(static_cast<bool>(Result)) << Message.str();
    EXPECT_NE(llvm::toString(Result.takeError()).find(Message.str()),
              std::string::npos);
  };

  const std::vector<uint8_t> TwoPushes = {opcodeByte(Opcode::PUSH1), 1,
                                          opcodeByte(Opcode::PUSH1), 2,
                                          opcodeByte(Opcode::STOP)};
  auto TwoPushLow = decodeLowIR(TwoPushes);
  ASSERT_TRUE(static_cast<bool>(TwoPushLow))
      << llvm::toString(TwoPushLow.takeError());

  AnalyzeOptions OneValue;
  OneValue.MaxMedValues = 1;
  ExpectError(*TwoPushLow, OneValue, "MedIR value limit 1 exceeded");

  AnalyzeOptions TwoOperations;
  TwoOperations.MaxMedOperations = 2;
  ExpectError(*TwoPushLow, TwoOperations, "MedIR operation limit 2 exceeded");

  AnalyzeOptions TwoOperationLanes;
  TwoOperationLanes.MaxMedOperationLaneReferences = 2;
  ExpectError(*TwoPushLow, TwoOperationLanes,
              "MaxMedOperationLaneReferences limit 2 exceeded");

  AnalyzeOptions OneStackEntry;
  OneStackEntry.MaxMedStackEntries = 1;
  ExpectError(*TwoPushLow, OneStackEntry, "MedIR stack entry limit 1 exceeded");

  constexpr uint8_t DestinationPC = 5;
  const std::vector<uint8_t> CrossBlock = {
      opcodeByte(Opcode::PUSH1), 42,
      opcodeByte(Opcode::PUSH1), DestinationPC,
      opcodeByte(Opcode::JUMP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP)};
  auto CrossBlockLow = decodeLowIR(CrossBlock);
  ASSERT_TRUE(static_cast<bool>(CrossBlockLow))
      << llvm::toString(CrossBlockLow.takeError());

  AnalyzeOptions OneLane;
  OneLane.MaxMedStateLanes = 1;
  ExpectError(*CrossBlockLow, OneLane,
              "IR exceeds its configured analysis limits");

  AnalyzeOptions NoPhiIncoming;
  NoPhiIncoming.MaxMedPhiIncomings = 1;
  auto OneIncoming = lowerToMedIR(*CrossBlockLow, NoPhiIncoming);
  ASSERT_TRUE(static_cast<bool>(OneIncoming))
      << llvm::toString(OneIncoming.takeError());
  NoPhiIncoming.MaxMedPhiIncomings = 0;
  ExpectError(*CrossBlockLow, NoPhiIncoming,
              "MaxMedPhiIncomings must be greater than zero");

  AnalyzeOptions OneWorklistUpdate;
  OneWorklistUpdate.MaxMedWorklistUpdates = 1;
  ExpectError(*TwoPushLow, OneWorklistUpdate,
              "MedIR worklist update limit 1 exceeded");
}

TEST(EVMAnalyzer, OperationLaneReferencesAreCanonicalAndExactlyBounded) {
  constexpr uint8_t kTargetPC = 7;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1), kTargetPC,
      opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::STOP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  size_t ExactReferences = 0;
  for (const MedBlock &Block : Program->Med.Blocks)
    for (const MedOperation &Operation : Block.Operations) {
      EXPECT_TRUE(std::is_sorted(Operation.ExecutingLanes.begin(),
                                 Operation.ExecutingLanes.end()));
      EXPECT_TRUE(std::is_sorted(Operation.FaultingLanes.begin(),
                                 Operation.FaultingLanes.end()));
      EXPECT_EQ(std::adjacent_find(Operation.ExecutingLanes.begin(),
                                   Operation.ExecutingLanes.end()),
                Operation.ExecutingLanes.end());
      EXPECT_EQ(std::adjacent_find(Operation.FaultingLanes.begin(),
                                   Operation.FaultingLanes.end()),
                Operation.FaultingLanes.end());
      ExactReferences += Operation.ExecutingLanes.size();
      ExactReferences += Operation.FaultingLanes.size();
    }
  ASSERT_GT(ExactReferences, 1u);

  AnalyzeOptions Exact;
  Exact.MaxMedOperationLaneReferences = ExactReferences;
  auto Accepted = lowerToMedIR(Program->Low, Exact);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());

  AnalyzeOptions TooMany = Exact;
  --TooMany.MaxMedOperationLaneReferences;
  auto Rejected = lowerToMedIR(Program->Low, TooMany);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError())
                .find(kMaxMedOperationLaneReferencesName.str()),
            std::string::npos);

  EVMMedIR Duplicate = Program->Med;
  MedOperation *Operation = nullptr;
  for (MedBlock &Block : Duplicate.Blocks)
    for (MedOperation &Candidate : Block.Operations)
      if (!Candidate.ExecutingLanes.empty()) {
        Operation = &Candidate;
        break;
      }
  ASSERT_NE(Operation, nullptr);
  Operation->ExecutingLanes.push_back(Operation->ExecutingLanes.back());
  auto DuplicateRejected = recoverHighIR(Program->Low, Duplicate);
  ASSERT_TRUE(static_cast<bool>(DuplicateRejected))
      << llvm::toString(DuplicateRejected.takeError());
  ASSERT_EQ(DuplicateRejected->Diagnostics.size(), 1u);
  EXPECT_EQ(DuplicateRejected->Diagnostics.front().Message,
            kMalformedMedIRDiagnostic);
}

TEST(EVMAnalyzer, MediumIRRejectsMalformedLowIRBeforeIndexing) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH1), 1,
                                     opcodeByte(Opcode::STOP)};
  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  ASSERT_FALSE(Low->Blocks.empty());
  ASSERT_FALSE(Low->Instructions.empty());
  ASSERT_FALSE(Low->AbstractStacks.empty());

  const auto ExpectMalformed = [&](auto Mutate) {
    EVMLowIR Malformed = *Low;
    Mutate(Malformed);
    auto Med = lowerToMedIR(Malformed);
    ASSERT_FALSE(static_cast<bool>(Med));
    EXPECT_NE(llvm::toString(Med.takeError())
                  .find("invalid LowIR for MedIR lowering"),
              std::string::npos);
  };

  ExpectMalformed([](EVMLowIR &Malformed) {
    Malformed.Blocks.front().FirstInstruction =
        std::numeric_limits<size_t>::max();
  });
  ExpectMalformed([](EVMLowIR &Malformed) {
    ++Malformed.Instructions.front().Info.StackPops;
  });
  ExpectMalformed([](EVMLowIR &Malformed) {
    Malformed.Instructions.front().Immediate = llvm::APInt(8, 1);
  });
  ExpectMalformed([](EVMLowIR &Malformed) {
    Malformed.AbstractStacks.front().ID = kInvalidLowAbstractStackID;
  });
}

TEST(EVMAnalyzer, PublicMedLoweringRejectsSemanticallyForgedLowIR) {
  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  const auto ExpectCanonicalMismatch = [&](EVMLowIR Mutated,
                                           llvm::StringRef Case) {
    SCOPED_TRACE(Case.str());
    auto Med = lowerToMedIR(Mutated, Relaxed);
    ASSERT_FALSE(static_cast<bool>(Med));
    EXPECT_NE(llvm::toString(Med.takeError())
                  .find("LowIR disagrees with canonical replay"),
              std::string::npos);
  };

  auto Faulting = decodeLowIR({opcodeByte(Opcode::ADD)}, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Faulting))
      << llvm::toString(Faulting.takeError());
  ASSERT_EQ(Faulting->Blocks.front().FaultPrefixes.size(), 1u);

  EVMLowIR WrongFaultKind = *Faulting;
  WrongFaultKind.Blocks.front().FaultPrefixes.front().Kind =
      LowFaultKind::StackOverflow;
  ExpectCanonicalMismatch(std::move(WrongFaultKind), "fault kind");

  EVMLowIR OmittedFault = *Faulting;
  OmittedFault.Blocks.front().FaultPrefixes.clear();
  ExpectCanonicalMismatch(std::move(OmittedFault), "omitted fault");

  constexpr uint8_t kTargetPC = 7;
  const std::vector<uint8_t> BranchCode = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1), kTargetPC,
      opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::STOP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };
  auto Branch = decodeLowIR(BranchCode, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Branch)) << llvm::toString(Branch.takeError());
  ASSERT_GE(Branch->Blocks.front().Successors.size(), 2u);

  EVMLowIR OmittedEdge = *Branch;
  OmittedEdge.Blocks.front().Successors.pop_back();
  ExpectCanonicalMismatch(std::move(OmittedEdge), "omitted edge");

  EVMLowIR WrongEvidence = *Branch;
  LowEdge &Edge = WrongEvidence.Blocks.front().Successors.front();
  Edge.Evidence = Edge.Evidence == Reachability::Reachable
                      ? Reachability::MayReachable
                      : Reachability::Reachable;
  ExpectCanonicalMismatch(std::move(WrongEvidence), "edge evidence");

  EVMLowIR WrongAbstractValue = *Branch;
  auto TargetValue = llvm::find_if(
      WrongAbstractValue.AbstractValues, [](const LowAbstractValue &Value) {
        return Value.Kind == LowAbstractValueKind::ConstantSet &&
               Value.Constants.size() == 1 &&
               Value.Constants.front().getLimitedValue() == kTargetPC;
      });
  ASSERT_NE(TargetValue, WrongAbstractValue.AbstractValues.end());
  TargetValue->Constants.front() = llvm::APInt(kWordBits, kTargetPC + 1);
  ExpectCanonicalMismatch(std::move(WrongAbstractValue), "abstract value");
}

TEST(EVMAnalyzer, LowIRResourcePreflightUsesAggregateExactBoundaries) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH1), 1,
                                     opcodeByte(Opcode::PUSH1), 2,
                                     opcodeByte(Opcode::STOP)};
  auto Baseline = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Baseline))
      << llvm::toString(Baseline.takeError());

  size_t ExactStackEntries = 0;
  for (const LowAbstractStack &Stack : Baseline->AbstractStacks)
    ExactStackEntries += Stack.Words.size();
  ASSERT_GT(ExactStackEntries, 1u);

  AnalyzeOptions AtStackBoundary;
  AtStackBoundary.MaxAbstractStackEntries = ExactStackEntries;
  auto Accepted = decodeLowIR(Code, AtStackBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());

  AnalyzeOptions BelowStackBoundary = AtStackBoundary;
  --BelowStackBoundary.MaxAbstractStackEntries;
  auto StackRejected = decodeLowIR(Code, BelowStackBoundary);
  ASSERT_FALSE(static_cast<bool>(StackRejected));
  EXPECT_NE(llvm::toString(StackRejected.takeError())
                .find("abstract stack entry limit"),
            std::string::npos);

  AnalyzeOptions AtLaneBoundary;
  AtLaneBoundary.MaxMedStateLanes = Baseline->StateLanes.size();
  auto Med = lowerToMedIR(*Baseline, AtLaneBoundary);
  ASSERT_TRUE(static_cast<bool>(Med)) << llvm::toString(Med.takeError());

  EVMLowIR TooManyLanes = *Baseline;
  TooManyLanes.StateLanes.push_back({});
  auto LaneRejected = lowerToMedIR(TooManyLanes, AtLaneBoundary);
  ASSERT_FALSE(static_cast<bool>(LaneRejected));
  EXPECT_NE(llvm::toString(LaneRejected.takeError())
                .find("IR exceeds its configured analysis limits"),
            std::string::npos);
}

TEST(EVMAnalyzer, PublicMedLoweringUsesLowDiagnosticBudgetsOnly) {
  constexpr uint8_t kFaultingTargetPC = 7;
  const std::vector<uint8_t> TwoFaults = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1), kFaultingTargetPC,
      opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::ADD),
      opcodeByte(Opcode::STOP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::ADD),   opcodeByte(Opcode::STOP),
  };
  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Low = decodeLowIR(TwoFaults, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  ASSERT_EQ(Low->Diagnostics.size(), 2u);
  size_t DiagnosticBytes = 0;
  for (const Diagnostic &Entry : Low->Diagnostics)
    DiagnosticBytes += Entry.Message.size();

  AnalyzeOptions Exact = Relaxed;
  Exact.RecoverHighLevel = false;
  Exact.MaxLowDiagnostics = Low->Diagnostics.size();
  Exact.MaxLowDiagnosticBytes = DiagnosticBytes;
  Exact.MaxHighDiagnostics = 0;
  Exact.MaxHighDiagnosticBytes = 0;
  auto Accepted = lowerToMedIR(*Low, Exact);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());

  AnalyzeOptions TooMany = Exact;
  --TooMany.MaxLowDiagnostics;
  auto CountRejected = lowerToMedIR(*Low, TooMany);
  ASSERT_FALSE(static_cast<bool>(CountRejected));
  EXPECT_NE(llvm::toString(CountRejected.takeError())
                .find("configured analysis limits"),
            std::string::npos);

  AnalyzeOptions TooManyBytes = Exact;
  --TooManyBytes.MaxLowDiagnosticBytes;
  auto BytesRejected = lowerToMedIR(*Low, TooManyBytes);
  ASSERT_FALSE(static_cast<bool>(BytesRejected));
  EXPECT_NE(llvm::toString(BytesRejected.takeError())
                .find("configured analysis limits"),
            std::string::npos);
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
  ASSERT_FALSE(Equality->ExecutingLanes.empty());
  const ValueID CycleID = static_cast<ValueID>(Cyclic->Med.Values.size());
  MedValue Cycle;
  Cycle.ID = CycleID;
  Cycle.Kind = ValueKind::Phi;
  Cycle.PC = kEqualityPC;
  Cycle.Name = kStackPhiValueName.str();
  Cycle.Inputs.push_back(CycleID);
  Cycle.PhiIncomings.push_back({Equality->ExecutingLanes.front(), CycleID});
  Cyclic->Med.Values.push_back(std::move(Cycle));
  Equality->Inputs[1] = CycleID;
  Cyclic->Med.Values[Equality->Outputs.front()].Inputs = Equality->Inputs;

  auto Recovered = recoverHighIR(Cyclic->Low, Cyclic->Med);
  ASSERT_TRUE(static_cast<bool>(Recovered))
      << llvm::toString(Recovered.takeError());
  EXPECT_TRUE(Recovered->Functions.empty());
}

TEST(EVMAnalyzer, HighIRBoundaryRejectsNonCanonicalDeepValueChainsIteratively) {
  constexpr uint64_t kEqualityPC = 0x0a;
  constexpr size_t kDeepSemanticValueChainLength = 16'384;
  auto Program = analyze(selectorDispatcher({opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  MedOperation *Equality = findOperation(Program->Med, kEqualityPC);
  ASSERT_NE(Equality, nullptr);
  ASSERT_EQ(Equality->Inputs.size(), 2u);
  ASSERT_EQ(Equality->Outputs.size(), 1u);
  ASSERT_FALSE(Equality->ExecutingLanes.empty());

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
    Phi.PhiIncomings.push_back({Equality->ExecutingLanes.front(), Previous});
    Program->Med.Values.push_back(std::move(Phi));
    Previous = ID;
  }
  Equality->Inputs[1] = Previous;
  Program->Med.Values[Equality->Outputs.front()].Inputs = Equality->Inputs;

  auto Recovered = recoverHighIR(Program->Low, Program->Med);
  ASSERT_TRUE(static_cast<bool>(Recovered))
      << llvm::toString(Recovered.takeError());
  EXPECT_TRUE(Recovered->Functions.empty());
  EXPECT_TRUE(llvm::any_of(Recovered->Diagnostics, [](const Diagnostic &D) {
    return D.Message == kMalformedMedIRDiagnostic;
  }));
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

  auto Recovered = recoverHighIR(Program->Low, Program->Med);
  ASSERT_TRUE(static_cast<bool>(Recovered))
      << llvm::toString(Recovered.takeError());
  EXPECT_TRUE(Recovered->Functions.empty());
  EXPECT_TRUE(llvm::any_of(Recovered->Diagnostics, [](const Diagnostic &D) {
    return D.Message == kMalformedMedIRDiagnostic;
  }));
}
} // namespace
} // namespace neverd::evm
