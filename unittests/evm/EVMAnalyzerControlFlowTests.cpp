//===- EVMAnalyzerControlFlowTests.cpp - EVM CFG recovery tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/Support/Error.h"

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
} // namespace
} // namespace neverd::evm
