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
} // namespace
} // namespace neverd::evm
