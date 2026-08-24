//===- EVMAnalyzerControlFlowTests.cpp - EVM CFG recovery tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/Support/Error.h"

#include <set>
#include <utility>

namespace neverd::evm {
namespace {

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

  AnalyzeOptions Bounded;
  Bounded.MaxAbstractValuesPerSlot = 1;
  auto BoundedLow = decodeLowIR(Code, Bounded);
  ASSERT_TRUE(static_cast<bool>(BoundedLow))
      << llvm::toString(BoundedLow.takeError());
  const LowBlock *BoundedCallee = BoundedLow->findBlock(CalleePC);
  ASSERT_NE(BoundedCallee, nullptr);
  EXPECT_TRUE(BoundedCallee->Reachable);
  EXPECT_FALSE(BoundedCallee->MayReachable);
  EXPECT_EQ(BoundedCallee->EntryStackHeights.singleton(), 1u);
  EXPECT_TRUE(BoundedCallee->MayEntryStackHeights.empty());
  EXPECT_EQ(BoundedCallee->StateLanes.size(), 2u);
  EXPECT_FALSE(BoundedCallee->HasIndirectSuccessor);
  EXPECT_TRUE(BoundedCallee->FaultPrefixes.empty());
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
  EXPECT_TRUE(Low->Diagnostics.empty());

  const LowBlock *Callee = Low->findBlock(CalleePC);
  ASSERT_NE(Callee, nullptr);
  ASSERT_EQ(Callee->StateLanes.size(), 2u);
  std::set<std::pair<uint64_t, uint64_t>> EntryPairs;
  for (size_t Ordinal = 0; Ordinal < Callee->StateLanes.size(); ++Ordinal) {
    const LowStateLaneID LaneID = Callee->StateLanes[Ordinal];
    EXPECT_EQ(LaneID.BlockPC, CalleePC);
    EXPECT_EQ(LaneID.Ordinal, Ordinal);
    const LowStateLane *Lane = Low->findStateLane(LaneID);
    ASSERT_NE(Lane, nullptr);
    const LowAbstractStack *Entry = Low->findAbstractStack(Lane->EntryState);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->Words.size(), 2u);
    const LowAbstractValue *Base = Low->findAbstractValue(Entry->Words[0]);
    const LowAbstractValue *Offset = Low->findAbstractValue(Entry->Words[1]);
    ASSERT_NE(Base, nullptr);
    ASSERT_NE(Offset, nullptr);
    ASSERT_EQ(Base->Kind, LowAbstractValueKind::ConstantSet);
    ASSERT_EQ(Offset->Kind, LowAbstractValueKind::ConstantSet);
    ASSERT_EQ(Base->Exactness, LowAbstractExactness::Exact);
    ASSERT_EQ(Offset->Exactness, LowAbstractExactness::Exact);
    ASSERT_EQ(Base->Constants.size(), 1u);
    ASSERT_EQ(Offset->Constants.size(), 1u);
    EntryPairs.emplace(Base->Constants.front().getZExtValue(),
                       Offset->Constants.front().getZExtValue());
  }
  EXPECT_EQ(EntryPairs, (std::set<std::pair<uint64_t, uint64_t>>{
                            {FirstReturnPC - FirstOperand, FirstOperand},
                            {SecondReturnPC - SecondOperand, SecondOperand}}));
}

TEST(EVMAnalyzer, WholeProgramKeepsExactSymbolIdentityThroughDup) {
  constexpr uint8_t DestinationPC = 7;
  constexpr uint8_t FallthroughPC = 6;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::CALLDATASIZE),
                                     opcodeByte(Opcode::DUP1),
                                     opcodeByte(Opcode::EQ),
                                     opcodeByte(Opcode::PUSH1),
                                     DestinationPC,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::ADD),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::STOP)};

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  EXPECT_TRUE(Low->hasEdge(kEntryPC, DestinationPC, EdgeKind::ConditionalTrue));
  EXPECT_FALSE(
      Low->hasEdge(kEntryPC, FallthroughPC, EdgeKind::ConditionalFalse));

  const auto Symbol =
      llvm::find_if(Low->AbstractValues, [](const LowAbstractValue &Value) {
        return Value.Kind == LowAbstractValueKind::Symbol && Value.Symbol &&
               Value.Symbol->ProducerOpcode == Opcode::CALLDATASIZE;
      });
  ASSERT_NE(Symbol, Low->AbstractValues.end());
  EXPECT_EQ(Symbol->Exactness, LowAbstractExactness::Exact);
  EXPECT_EQ(Symbol->Symbol->ProducerPC, kEntryPC);
  EXPECT_EQ(Symbol->Symbol->OutputOrdinal, 0u);
  EXPECT_FALSE(
      llvm::any_of(Low->AbstractValues, [](const LowAbstractValue &Value) {
        return Value.Expression && Value.Expression->Operator == Opcode::EQ;
      }));
}

TEST(EVMAnalyzer, WholeProgramKeepsDistinctEnvironmentProducersSeparate) {
  constexpr uint8_t DestinationPC = 7;
  constexpr uint8_t FallthroughPC = 6;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::CALLER),
                                     opcodeByte(Opcode::ORIGIN),
                                     opcodeByte(Opcode::EQ),
                                     opcodeByte(Opcode::PUSH1),
                                     DestinationPC,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::STOP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::STOP)};

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  EXPECT_TRUE(Low->hasEdge(kEntryPC, DestinationPC, EdgeKind::ConditionalTrue));
  EXPECT_TRUE(
      Low->hasEdge(kEntryPC, FallthroughPC, EdgeKind::ConditionalFalse));

  const auto FindSymbol = [&](Opcode Op) -> const LowAbstractValue * {
    const auto It =
        llvm::find_if(Low->AbstractValues, [Op](const LowAbstractValue &Value) {
          return Value.Kind == LowAbstractValueKind::Symbol && Value.Symbol &&
                 Value.Symbol->ProducerOpcode == Op;
        });
    return It == Low->AbstractValues.end() ? nullptr : &*It;
  };
  const LowAbstractValue *Caller = FindSymbol(Opcode::CALLER);
  const LowAbstractValue *Origin = FindSymbol(Opcode::ORIGIN);
  ASSERT_NE(Caller, nullptr);
  ASSERT_NE(Origin, nullptr);
  EXPECT_NE(Caller->ID, Origin->ID);

  const auto Equality =
      llvm::find_if(Low->AbstractValues, [](const LowAbstractValue &Value) {
        return Value.Kind == LowAbstractValueKind::Expression &&
               Value.Expression && Value.Expression->Operator == Opcode::EQ;
      });
  ASSERT_NE(Equality, Low->AbstractValues.end());
  EXPECT_EQ(Equality->Exactness, LowAbstractExactness::Exact);
  ASSERT_EQ(Equality->Expression->Operands.size(), 2u);
  EXPECT_NE(Equality->Expression->Operands[0],
            Equality->Expression->Operands[1]);
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

TEST(EVMAnalyzer, WholeProgramAbstractsChangingLoopRecurrencesStatically) {
  constexpr uint8_t LoopPC = 5;
  constexpr uint8_t InitialValue = 42;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH1),
                                     InitialValue,
                                     opcodeByte(Opcode::PUSH1),
                                     LoopPC,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::PUSH1),
                                     1,
                                     opcodeByte(Opcode::ADD),
                                     opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::CALLDATALOAD),
                                     opcodeByte(Opcode::PUSH1),
                                     LoopPC,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::STOP)};

  AnalyzeOptions TwoLanes;
  TwoLanes.MaxAbstractStateLanesPerBlock = 2;
  auto Low = decodeLowIR(Code, TwoLanes);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  const LowBlock *Loop = Low->findBlock(LoopPC);
  ASSERT_NE(Loop, nullptr);
  ASSERT_EQ(Loop->StateLanes.size(), 2u);

  bool SawInitialConstant = false;
  bool SawRecurrenceTop = false;
  for (LowStateLaneID LaneID : Loop->StateLanes) {
    const LowStateLane *Lane = Low->findStateLane(LaneID);
    ASSERT_NE(Lane, nullptr);
    const LowAbstractStack *Entry = Low->findAbstractStack(Lane->EntryState);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->Words.size(), 1u);
    const LowAbstractValue *Value = Low->findAbstractValue(Entry->Words[0]);
    ASSERT_NE(Value, nullptr);
    if (Value->Kind == LowAbstractValueKind::ConstantSet) {
      ASSERT_EQ(Value->Constants.size(), 1u);
      SawInitialConstant =
          Value->Constants.front().getZExtValue() == InitialValue;
    }
    if (Value->Kind == LowAbstractValueKind::Top) {
      SawRecurrenceTop = true;
      EXPECT_EQ(Value->Exactness, LowAbstractExactness::OverApproximation);
    }
  }
  EXPECT_TRUE(SawInitialConstant);
  EXPECT_TRUE(SawRecurrenceTop);
}

TEST(EVMAnalyzer, WholeProgramNeverFoldsOverapproximatedSelfEquality) {
  constexpr uint8_t LoopPC = 4;
  constexpr uint8_t FallthroughPC = 14;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::PUSH1),
                                     LoopPC,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::PUSH1),
                                     1,
                                     opcodeByte(Opcode::ADD),
                                     opcodeByte(Opcode::DUP1),
                                     opcodeByte(Opcode::DUP1),
                                     opcodeByte(Opcode::EQ),
                                     opcodeByte(Opcode::PUSH1),
                                     LoopPC,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::STOP)};

  AnalyzeOptions TwoLanes;
  TwoLanes.MaxAbstractStateLanesPerBlock = 2;
  auto Low = decodeLowIR(Code, TwoLanes);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  EXPECT_TRUE(Low->hasEdge(LoopPC, FallthroughPC, EdgeKind::ConditionalFalse));

  const auto Equality =
      llvm::find_if(Low->AbstractValues, [](const LowAbstractValue &Value) {
        return Value.Expression && Value.Expression->Operator == Opcode::EQ &&
               Value.Expression->Operands.size() == 2 &&
               Value.Expression->Operands[0] == Value.Expression->Operands[1];
      });
  ASSERT_NE(Equality, Low->AbstractValues.end());
  EXPECT_EQ(Equality->Exactness, LowAbstractExactness::OverApproximation);
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
  const auto ExpectLowError = [&](size_t AnalyzeOptions::*Member,
                                  llvm::StringLiteral Name) {
    AnalyzeOptions Options;
    Options.*Member = 0;
    auto Result = decodeLowIR(Code, Options);
    ASSERT_FALSE(static_cast<bool>(Result)) << Name.str();
    EXPECT_NE(llvm::toString(Result.takeError())
                  .find((Name + " must be greater than zero").str()),
              std::string::npos)
        << Name.str();
  };
  const auto ExpectMedError = [&](size_t AnalyzeOptions::*Member,
                                  llvm::StringLiteral Name) {
    auto Low = decodeLowIR(Code);
    ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
    AnalyzeOptions Options;
    Options.*Member = 0;
    auto Result = lowerToMedIR(*Low, Options);
    ASSERT_FALSE(static_cast<bool>(Result)) << Name.str();
    EXPECT_NE(llvm::toString(Result.takeError())
                  .find((Name + " must be greater than zero").str()),
              std::string::npos)
        << Name.str();
  };
  const auto ExpectHighError = [&](size_t AnalyzeOptions::*Member,
                                   llvm::StringLiteral Name) {
    AnalyzeOptions Options;
    Options.*Member = 0;
    auto Result = analyze(Code, Options);
    ASSERT_FALSE(static_cast<bool>(Result)) << Name.str();
    EXPECT_NE(llvm::toString(Result.takeError())
                  .find((Name + " must be greater than zero").str()),
              std::string::npos)
        << Name.str();
  };

#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)                         \
  ExpectLowError(&AnalyzeOptions::NAME, #NAME);
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)                   \
  ExpectLowError(&AnalyzeOptions::NAME, #NAME);
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)                      \
  ExpectMedError(&AnalyzeOptions::NAME, #NAME);
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)                        \
  ExpectHighError(&AnalyzeOptions::NAME, #NAME);
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR
}

TEST(EVMAnalyzer, WholeProgramEnforcesAnalysisBudgetsBeforeInsertion) {
  const auto ExpectError = [](llvm::ArrayRef<uint8_t> Code,
                              AnalyzeOptions Options, llvm::StringRef Message) {
    auto Result = decodeLowIR(Code, Options);
    ASSERT_FALSE(static_cast<bool>(Result)) << Message.str();
    EXPECT_NE(llvm::toString(Result.takeError()).find(Message.str()),
              std::string::npos);
  };

  const std::vector<uint8_t> TwoInstructions = {opcodeByte(Opcode::PUSH0),
                                                opcodeByte(Opcode::STOP)};
  AnalyzeOptions OneInstruction;
  OneInstruction.MaxInstructions = 1;
  ExpectError(TwoInstructions, OneInstruction, "instruction limit 1 exceeded");
  std::vector<uint8_t> OneLongPush(kWordBytes + 1, 0);
  OneLongPush.front() = opcodeByte(Opcode::PUSH32);
  EXPECT_TRUE(static_cast<bool>(decodeLowIR(OneLongPush, OneInstruction)));

  const std::vector<uint8_t> TwoBlocks = {opcodeByte(Opcode::STOP),
                                          opcodeByte(Opcode::JUMPDEST),
                                          opcodeByte(Opcode::STOP)};
  AnalyzeOptions OneBlock;
  OneBlock.MaxBlocks = 1;
  ExpectError(TwoBlocks, OneBlock, "basic block limit 1 exceeded");
  AnalyzeOptions TwoBlockLimit;
  TwoBlockLimit.MaxBlocks = 2;
  EXPECT_TRUE(static_cast<bool>(decodeLowIR(TwoBlocks, TwoBlockLimit)));

  AnalyzeOptions OneValue;
  OneValue.MaxAbstractValueNodes = 1;
  const std::vector<uint8_t> OneValueCode = {opcodeByte(Opcode::PUSH1), 1,
                                             opcodeByte(Opcode::STOP)};
  EXPECT_TRUE(static_cast<bool>(decodeLowIR(OneValueCode, OneValue)));
  const std::vector<uint8_t> TwoValueCode = {opcodeByte(Opcode::PUSH1), 1,
                                             opcodeByte(Opcode::PUSH1), 2,
                                             opcodeByte(Opcode::STOP)};
  ExpectError(TwoValueCode, OneValue, "abstract value node limit 1 exceeded");

  AnalyzeOptions OneStack;
  OneStack.MaxAbstractStackNodes = 1;
  const std::vector<uint8_t> EmptyStackCode = {opcodeByte(Opcode::STOP)};
  EXPECT_TRUE(static_cast<bool>(decodeLowIR(EmptyStackCode, OneStack)));
  ExpectError(OneValueCode, OneStack, "abstract stack node limit 1 exceeded");

  constexpr uint8_t AlternatePC = 10;
  constexpr uint8_t JoinPC = 16;
  const std::vector<uint8_t> TwoLaneJoin = {opcodeByte(Opcode::PUSH0),
                                            opcodeByte(Opcode::CALLDATALOAD),
                                            opcodeByte(Opcode::PUSH1),
                                            AlternatePC,
                                            opcodeByte(Opcode::JUMPI),
                                            opcodeByte(Opcode::PUSH1),
                                            1,
                                            opcodeByte(Opcode::PUSH1),
                                            JoinPC,
                                            opcodeByte(Opcode::JUMP),
                                            opcodeByte(Opcode::JUMPDEST),
                                            opcodeByte(Opcode::PUSH1),
                                            2,
                                            opcodeByte(Opcode::PUSH1),
                                            JoinPC,
                                            opcodeByte(Opcode::JUMP),
                                            opcodeByte(Opcode::JUMPDEST),
                                            opcodeByte(Opcode::STOP)};
  AnalyzeOptions OneLane;
  OneLane.MaxAbstractStateLanesPerBlock = 1;
  ExpectError(TwoLaneJoin, OneLane, "abstract state lane limit 1 exceeded");

  constexpr uint8_t DestinationPC = 3;
  const std::vector<uint8_t> OneEdgeCode = {
      opcodeByte(Opcode::PUSH1), DestinationPC, opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)};
  AnalyzeOptions TwoEdgeRecords;
  TwoEdgeRecords.MaxEdges = 2;
  EXPECT_TRUE(static_cast<bool>(decodeLowIR(OneEdgeCode, TwoEdgeRecords)));
  AnalyzeOptions OneEdgeRecord;
  OneEdgeRecord.MaxEdges = 1;
  ExpectError(OneEdgeCode, OneEdgeRecord, "CFG edge limit 1 exceeded");

  constexpr uint8_t ConditionalDestinationPC = 6;
  const std::vector<uint8_t> TwoEdgeCode = {
      opcodeByte(Opcode::PUSH0),    opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),    ConditionalDestinationPC,
      opcodeByte(Opcode::JUMPI),    opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)};
  ExpectError(TwoEdgeCode, TwoEdgeRecords, "CFG edge limit 2 exceeded");

  AnalyzeOptions OneUpdate;
  OneUpdate.MaxWorklistUpdates = 1;
  EXPECT_TRUE(static_cast<bool>(decodeLowIR(EmptyStackCode, OneUpdate)));
  ExpectError(OneEdgeCode, OneUpdate, "worklist update limit 1 exceeded");

  AnalyzeOptions OneTransfer;
  OneTransfer.MaxAbstractInstructionTransfers = 1;
  EXPECT_TRUE(static_cast<bool>(decodeLowIR(EmptyStackCode, OneTransfer)));
  ExpectError(OneValueCode, OneTransfer,
              "abstract instruction transfer limit 1 exceeded");
}

TEST(EVMAnalyzer, ControlFlowPrechargesDiagnosticCountAndBytes) {
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
  auto Baseline = decodeLowIR(TwoFaults, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Baseline))
      << llvm::toString(Baseline.takeError());
  ASSERT_EQ(Baseline->Diagnostics.size(), 2u);
  const size_t ExactBytes = Baseline->Diagnostics[0].Message.size() +
                            Baseline->Diagnostics[1].Message.size();

  AnalyzeOptions Exact = Relaxed;
  Exact.MaxLowDiagnostics = Baseline->Diagnostics.size();
  Exact.MaxLowDiagnosticBytes = ExactBytes;
  auto Accepted = decodeLowIR(TwoFaults, Exact);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());

  AnalyzeOptions TooMany = Exact;
  --TooMany.MaxLowDiagnostics;
  auto CountRejected = decodeLowIR(TwoFaults, TooMany);
  ASSERT_FALSE(static_cast<bool>(CountRejected));
  EXPECT_NE(
      llvm::toString(CountRejected.takeError()).find("LowIR diagnostic limit"),
      std::string::npos);

  AnalyzeOptions TooManyBytes = Exact;
  --TooManyBytes.MaxLowDiagnosticBytes;
  auto BytesRejected = decodeLowIR(TwoFaults, TooManyBytes);
  ASSERT_FALSE(static_cast<bool>(BytesRejected));
  EXPECT_NE(llvm::toString(BytesRejected.takeError())
                .find("LowIR diagnostic byte limit"),
            std::string::npos);
}

TEST(EVMAnalyzer, WholeProgramLaneTablesAreDeterministic) {
  constexpr uint8_t DestinationPC = 7;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::CALLER),
                                     opcodeByte(Opcode::ORIGIN),
                                     opcodeByte(Opcode::EQ),
                                     opcodeByte(Opcode::PUSH1),
                                     DestinationPC,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::STOP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::STOP)};

  auto First = decodeLowIR(Code);
  auto Second = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(First)) << llvm::toString(First.takeError());
  ASSERT_TRUE(static_cast<bool>(Second)) << llvm::toString(Second.takeError());
  EXPECT_TRUE(First->AbstractValues == Second->AbstractValues);
  EXPECT_TRUE(First->AbstractStacks == Second->AbstractStacks);
  EXPECT_TRUE(First->StateLanes == Second->StateLanes);
  EXPECT_TRUE(First->LaneTransitions == Second->LaneTransitions);
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
  EXPECT_EQ(UnknownEntry->Successors.front().Evidence,
            Reachability::MayReachable);

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

TEST(EVMAnalyzer, WholeProgramPropagatesUnknownJumpsToEveryDestination) {
  constexpr uint8_t DestinationPC = 3;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::JUMP), opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP)};

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  const LowBlock *Entry = Low->findBlock(kEntryPC);
  const LowBlock *Destination = Low->findBlock(DestinationPC);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Destination, nullptr);
  EXPECT_TRUE(Entry->HasIndirectSuccessor);
  EXPECT_TRUE(Low->hasEdge(kEntryPC, DestinationPC, EdgeKind::Indirect));
  const auto Indirect =
      llvm::find_if(Entry->Successors, [DestinationPC](const LowEdge &Edge) {
        return Edge.Kind == EdgeKind::Indirect && Edge.Target == DestinationPC;
      });
  ASSERT_NE(Indirect, Entry->Successors.end());
  EXPECT_EQ(Indirect->Evidence, Reachability::MayReachable);
  EXPECT_FALSE(Destination->Reachable);
  EXPECT_TRUE(Destination->MayReachable);
  EXPECT_TRUE(Destination->EntryStackHeights.empty());
  EXPECT_EQ(Destination->MayEntryStackHeights.singleton(), 0u);
  EXPECT_TRUE(Destination->Predecessors.empty());
  EXPECT_EQ(Destination->MayPredecessors, (std::vector<uint64_t>{kEntryPC}));
}

TEST(EVMAnalyzer, WholeProgramDoesNotLowerMayOnlyIndirectTargets) {
  constexpr uint8_t DestinationPC = 3;
  constexpr uint8_t StoredValue = 1;
  constexpr uint8_t StorageSlot = 0;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),  opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::JUMP),   opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),  StoredValue,
      opcodeByte(Opcode::PUSH1),  StorageSlot,
      opcodeByte(Opcode::SSTORE), opcodeByte(Opcode::STOP)};

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const LowBlock *Destination = Program->Low.findBlock(DestinationPC);
  ASSERT_NE(Destination, nullptr);
  EXPECT_FALSE(Destination->Reachable);
  EXPECT_TRUE(Destination->MayReachable);
  EXPECT_TRUE(Program->High.Storage.empty());
}

TEST(EVMAnalyzer, WholeProgramReachableEvidenceDominatesDuplicateMayEdges) {
  constexpr uint8_t JoinPC = 8;
  constexpr uint8_t DestinationPC = 12;
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::CALLDATALOAD),
                                     opcodeByte(Opcode::PUSH1),
                                     JoinPC,
                                     opcodeByte(Opcode::JUMPI),
                                     opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::CALLDATALOAD),
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::PUSH1),
                                     DestinationPC,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::STOP)};

  auto Low = decodeLowIR(Code);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  const LowBlock *Join = Low->findBlock(JoinPC);
  ASSERT_NE(Join, nullptr);
  EXPECT_TRUE(Join->Reachable);
  EXPECT_TRUE(Join->MayReachable);
  const auto Edge = llvm::find_if(Join->Successors, [](const LowEdge &Edge) {
    return Edge.Kind == EdgeKind::Jump;
  });
  ASSERT_NE(Edge, Join->Successors.end());
  EXPECT_EQ(Edge->Target, DestinationPC);
  EXPECT_EQ(Edge->Evidence, Reachability::Reachable);
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

TEST(EVMAnalyzer, WholeProgramRecordsFaultPrefixesPerEntryStackHeight) {
  constexpr uint8_t TallPathPC = 8;
  constexpr uint8_t JoinPC = 16;
  constexpr uint8_t FaultPC = 17;
  constexpr uint8_t FirstOperand = 1;
  constexpr uint8_t SecondOperand = 2;
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
                                     FirstOperand,
                                     opcodeByte(Opcode::PUSH1),
                                     SecondOperand,
                                     opcodeByte(Opcode::PUSH1),
                                     JoinPC,
                                     opcodeByte(Opcode::JUMP),
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::ADD),
                                     opcodeByte(Opcode::STOP)};

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Low = decodeLowIR(Code, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  const LowBlock *Join = Low->findBlock(JoinPC);
  ASSERT_NE(Join, nullptr);
  ASSERT_EQ(Join->EntryStackHeights.values().size(), 2u);
  EXPECT_EQ(Join->EntryStackHeights.values()[0], 0u);
  EXPECT_EQ(Join->EntryStackHeights.values()[1], 2u);
  ASSERT_EQ(Join->FaultPrefixes.size(), 1u);
  EXPECT_EQ(Join->FaultPrefixes.front().EntryStackHeight, 0u);
  EXPECT_EQ(Join->FaultPrefixes.front().PC, FaultPC);
  EXPECT_EQ(Join->FaultPrefixes.front().Kind, LowFaultKind::StackUnderflow);
  ASSERT_EQ(Join->ExitStackHeights.values().size(), 2u);
  EXPECT_EQ(Join->ExitStackHeights.values()[0], 0u);
  EXPECT_EQ(Join->ExitStackHeights.values()[1], 1u);
}

TEST(EVMAnalyzer, WholeProgramRecordsEveryTypedDefiniteFaultPrefix) {
  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  const auto ExpectFault = [&](llvm::ArrayRef<uint8_t> Code, uint64_t FaultPC,
                               LowFaultKind Kind) {
    auto Low = decodeLowIR(Code, Relaxed);
    ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
    const LowBlock *Entry = Low->findBlock(kEntryPC);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->FaultPrefixes.size(), 1u);
    EXPECT_EQ(Entry->FaultPrefixes.front().EntryStackHeight, 0u);
    EXPECT_EQ(Entry->FaultPrefixes.front().PC, FaultPC);
    EXPECT_EQ(Entry->FaultPrefixes.front().Kind, Kind);
  };

  const std::vector<uint8_t> Underflow = {opcodeByte(Opcode::ADD)};
  ExpectFault(Underflow, kEntryPC, LowFaultKind::StackUnderflow);

  std::vector<uint8_t> Overflow(kStackLimit + 1, opcodeByte(Opcode::PUSH0));
  ExpectFault(Overflow, kStackLimit, LowFaultKind::StackOverflow);

  constexpr uint8_t UnknownOpcodeByte = 0x0c;
  const std::vector<uint8_t> NonExecutable = {UnknownOpcodeByte};
  ExpectFault(NonExecutable, kEntryPC, LowFaultKind::NonExecutableInstruction);
}

TEST(EVMAnalyzer, EndOfCodeConditionalJumpFaultDependsOnTheCondition) {
  constexpr uint8_t kInvalidTarget = 0xff;
  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;

  const auto DecodeRelaxed = [&](llvm::ArrayRef<uint8_t> Code) {
    auto Low = decodeLowIR(Code, Relaxed);
    EXPECT_TRUE(static_cast<bool>(Low))
        << (Low ? std::string() : llvm::toString(Low.takeError()));
    return Low;
  };

  const std::vector<uint8_t> AlwaysTrue = {
      opcodeByte(Opcode::PUSH1), 1, opcodeByte(Opcode::PUSH1), kInvalidTarget,
      opcodeByte(Opcode::JUMPI)};
  auto TrueLow = DecodeRelaxed(AlwaysTrue);
  ASSERT_TRUE(static_cast<bool>(TrueLow));
  ASSERT_EQ(TrueLow->Blocks.front().FaultPrefixes.size(), 1u);
  EXPECT_EQ(TrueLow->Blocks.front().FaultPrefixes.front().Kind,
            LowFaultKind::InvalidJumpDestination);

  const std::vector<uint8_t> AlwaysFalse = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH1), kInvalidTarget,
      opcodeByte(Opcode::JUMPI)};
  auto FalseLow = DecodeRelaxed(AlwaysFalse);
  ASSERT_TRUE(static_cast<bool>(FalseLow));
  EXPECT_TRUE(FalseLow->Blocks.front().FaultPrefixes.empty());

  const std::vector<uint8_t> Unknown = {
      opcodeByte(Opcode::CALLVALUE), opcodeByte(Opcode::PUSH1), kInvalidTarget,
      opcodeByte(Opcode::JUMPI)};
  auto UnknownLow = DecodeRelaxed(Unknown);
  ASSERT_TRUE(static_cast<bool>(UnknownLow));
  EXPECT_TRUE(UnknownLow->Blocks.front().FaultPrefixes.empty());

  EXPECT_TRUE(static_cast<bool>(decodeLowIR(AlwaysFalse)));
  EXPECT_FALSE(static_cast<bool>(decodeLowIR(AlwaysTrue)));
  EXPECT_FALSE(static_cast<bool>(decodeLowIR(Unknown)));
}
} // namespace
} // namespace neverd::evm
