//===- EVMSemanticTests.cpp - EVM interpreter semantics tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/runtime/EVMInterpreter.h"
#include "neverd/evm/runtime/EVMSemantics.h"

#include "llvm/Support/Error.h"

#include <limits>

namespace neverd::evm {
namespace {

ExecutionResult executeCode(const std::vector<uint8_t> &Code,
                            ExecutionEnvironment Environment = {},
                            AnalyzeOptions Options = {}) {
  auto Program = analyze(Code, Options);
  EXPECT_TRUE(static_cast<bool>(Program));
  if (!Program) {
    ADD_FAILURE() << llvm::toString(Program.takeError());
    return {};
  }
  auto Result = execute(Program->Low, std::move(Environment));
  EXPECT_TRUE(static_cast<bool>(Result));
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return {};
  }
  return std::move(*Result);
}

constexpr uint8_t kTransactionDiagnosticByte = 0xcc;

llvm::APInt testWord(uint64_t Value) { return llvm::APInt(kWordBits, Value); }

ExecutionEnvironment transactionEnvironment() {
  ExecutionEnvironment Environment;
  Environment.Storage.emplace(testWord(1), testWord(0x11));
  Environment.TransientStorage.emplace(testWord(2), testWord(0x22));
  return Environment;
}

std::vector<uint8_t> transactionMutationPrefix() {
  return {
      opcodeByte(Opcode::PUSH1),
      0xaa,
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::SSTORE),
      opcodeByte(Opcode::PUSH1),
      0xbb,
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::TSTORE),
      opcodeByte(Opcode::PUSH1),
      kTransactionDiagnosticByte,
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::MSTORE8),
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::LOG0),
  };
}

void expectTransactionStateRestored(const ExecutionResult &Result) {
  EXPECT_TRUE(Result.Logs.empty());
  ASSERT_EQ(Result.Storage.size(), 1u);
  EXPECT_EQ(Result.Storage.at(testWord(1)), testWord(0x11));
  ASSERT_EQ(Result.TransientStorage.size(), 1u);
  EXPECT_EQ(Result.TransientStorage.at(testWord(2)), testWord(0x22));
}

TEST(EVMInterpreter, ExecutesAmsterdamSlotAndDeepStackOpcodes) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;

  ExecutionEnvironment Environment;
  Environment.SlotNumber = 0x123456789abcdef0ULL;
  auto Slot =
      executeCode({opcodeByte(Opcode::SLOTNUM), opcodeByte(Opcode::STOP)},
                  std::move(Environment), Amsterdam);
  ASSERT_EQ(Slot.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Slot.Stack.size(), 1u);
  EXPECT_EQ(Slot.Stack.back().getZExtValue(), 0x123456789abcdef0ULL);

  std::vector<uint8_t> Dup = {opcodeByte(Opcode::PUSH1), 1};
  for (unsigned I = 0; I < 16; ++I)
    Dup.insert(Dup.end(), {opcodeByte(Opcode::PUSH1), 0});
  Dup.insert(Dup.end(),
             {opcodeByte(Opcode::DUPN), 0x80, opcodeByte(Opcode::STOP)});
  auto Duplicated = executeCode(Dup, {}, Amsterdam);
  ASSERT_EQ(Duplicated.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Duplicated.Stack.size(), 18u);
  EXPECT_EQ(Duplicated.Stack.front().getZExtValue(), 1u);
  EXPECT_EQ(Duplicated.Stack.back().getZExtValue(), 1u);

  const auto Exchanged = executeCode({0x60, 0x00, 0x60, 0x01, 0x60, 0x02,
                                      opcodeByte(Opcode::EXCHANGE), 0x8e,
                                      opcodeByte(Opcode::STOP)},
                                     {}, Amsterdam);
  ASSERT_EQ(Exchanged.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Exchanged.Stack.size(), 3u);
  EXPECT_EQ(Exchanged.Stack[0].getZExtValue(), 1u);
  EXPECT_EQ(Exchanged.Stack[1].getZExtValue(), 0u);
  EXPECT_EQ(Exchanged.Stack[2].getZExtValue(), 2u);

  const auto JumpPastInvalid = executeCode(
      {0x60, 0x04, 0x56, opcodeByte(Opcode::DUPN), 0x5b, 0x00}, {}, Amsterdam);
  EXPECT_EQ(JumpPastInvalid.Status, ExecutionStatus::Stopped);
  EXPECT_TRUE(JumpPastInvalid.Error.empty());
}

TEST(EVMInterpreter, ExecutesRepresentativeEIP8024StackShapes) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;

  // These cases exercise the decoder-to-interpreter integration, not an
  // independent encoding oracle. The fresh-fetch go-ethereum differential
  // audit is the semantic gate for every EIP-8024 candidate.

  const auto PushSequence = [](unsigned Count) {
    std::vector<uint8_t> Code;
    for (unsigned Value = 1; Value <= Count; ++Value)
      Code.insert(Code.end(),
                  {opcodeByte(Opcode::PUSH1), static_cast<uint8_t>(Value)});
    return Code;
  };

  auto DupCode = PushSequence(17);
  DupCode.insert(DupCode.end(), {opcodeByte(Opcode::DUPN), 0x80});
  const auto Duplicated = executeCode(DupCode, {}, Amsterdam);
  ASSERT_EQ(Duplicated.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Duplicated.Stack.size(), 18u);
  EXPECT_EQ(Duplicated.Stack.front().getZExtValue(), 1u);
  EXPECT_EQ(Duplicated.Stack.back().getZExtValue(), 1u);

  auto SwapCode = PushSequence(18);
  SwapCode.insert(SwapCode.end(), {opcodeByte(Opcode::SWAPN), 0x80});
  const auto Swapped = executeCode(SwapCode, {}, Amsterdam);
  ASSERT_EQ(Swapped.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Swapped.Stack.size(), 18u);
  EXPECT_EQ(Swapped.Stack.front().getZExtValue(), 18u);
  EXPECT_EQ(Swapped.Stack.back().getZExtValue(), 1u);

  auto MissingExchangeCode = PushSequence(17);
  MissingExchangeCode.push_back(opcodeByte(Opcode::EXCHANGE));
  const auto MissingExchange = executeCode(MissingExchangeCode, {}, Amsterdam);
  ASSERT_EQ(MissingExchange.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(MissingExchange.Stack.size(), 17u);
  EXPECT_EQ(MissingExchange.Stack.front().getZExtValue(), 8u);
  EXPECT_EQ(MissingExchange.Stack[7].getZExtValue(), 1u);

  auto MaximumExchangeCode = PushSequence(30);
  MaximumExchangeCode.insert(MaximumExchangeCode.end(),
                             {opcodeByte(Opcode::EXCHANGE), 0x8f});
  const auto MaximumExchange = executeCode(MaximumExchangeCode, {}, Amsterdam);
  ASSERT_EQ(MaximumExchange.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(MaximumExchange.Stack.size(), 30u);
  EXPECT_EQ(MaximumExchange.Stack.front().getZExtValue(), 29u);
  EXPECT_EQ(MaximumExchange.Stack[28].getZExtValue(), 1u);
  EXPECT_EQ(MaximumExchange.Stack.back().getZExtValue(), 30u);

  Amsterdam.Strict = false;
  auto UnderflowCode = PushSequence(16);
  UnderflowCode.insert(UnderflowCode.end(), {opcodeByte(Opcode::DUPN), 0x80});
  const auto Underflow = executeCode(UnderflowCode, {}, Amsterdam);
  EXPECT_EQ(Underflow.Status, ExecutionStatus::Faulted);
  EXPECT_NE(Underflow.Error.find("stack underflow in DUP"), std::string::npos);

  const auto Invalid =
      executeCode({opcodeByte(Opcode::EXCHANGE), opcodeByte(Opcode::MSTORE)},
                  {}, Amsterdam);
  EXPECT_EQ(Invalid.Status, ExecutionStatus::Faulted);
  EXPECT_NE(Invalid.Error.find("invalid immediate in EXCHANGE"),
            std::string::npos);
}

TEST(EVMSemantics, EveryScalarALUHasOneSharedEvaluator) {
  for (size_t Byte = 0; Byte < kOpcodeSpaceSize; ++Byte) {
    const auto Info = assignedOpcodeInfo(static_cast<uint8_t>(Byte));
    if (!Info || !isALU(*Info))
      continue;
    SCOPED_TRACE(Info->Name.str());
    std::vector<llvm::APInt> Inputs(Info->StackPops, llvm::APInt(kWordBits, 0));
    EXPECT_TRUE(evaluateALU(Info->Op, Inputs).has_value());
  }

  EXPECT_FALSE(evaluateALU(Opcode::SLOAD, {}).has_value());
  EXPECT_FALSE(
      evaluateALU(Opcode::ADD, {llvm::APInt(kWordBits, 0)}).has_value());
  EXPECT_FALSE(
      evaluateALU(Opcode::ISZERO, {llvm::APInt(kWordBits / 2, 0)}).has_value());
}

TEST(EVMInterpreter, ExecutesArithmeticStorageMemoryAndReturn) {
  const std::vector<uint8_t> Code = {
      0x60, 0x02, 0x60, 0x03, 0x01, 0x60, 0x00, 0x55, // storage[0] = 5
      0x60, 0x00, 0x54, 0x60, 0x00, 0x52,             // mstore(0, sload(0))
      0x60, 0x20, 0x60, 0x00, 0xf3};                  // return(0, 32)
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Result = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned);
  ASSERT_EQ(Result->ReturnData.size(), 32u);
  EXPECT_EQ(Result->ReturnData.back(), 5u);
  ASSERT_EQ(Result->Storage.size(), 1u);
  EXPECT_EQ(Result->Storage.begin()->second.getZExtValue(), 5u);
  EXPECT_FALSE(Result->Trace.empty());
  EXPECT_EQ(Result->Trace.front().PC, 0u);
}

TEST(EVMInterpreter, BranchesOnCalldataAndValidatesJumpDestinations) {
  const std::vector<uint8_t> Code = {0x60, 0x00, 0x35, 0x60, 0x08,
                                     0x57, 0x5b, 0x00, 0x5b, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ExecutionEnvironment Zero;
  Zero.Calldata.resize(32, 0);
  auto FalsePath = execute(Program->Low, Zero);
  ASSERT_TRUE(static_cast<bool>(FalsePath))
      << llvm::toString(FalsePath.takeError());
  ASSERT_GE(FalsePath->Trace.size(), 2u);
  EXPECT_EQ(FalsePath->Trace[FalsePath->Trace.size() - 2].PC, 6u);

  ExecutionEnvironment NonZero;
  NonZero.Calldata.resize(32, 0);
  NonZero.Calldata.back() = 1;
  auto TruePath = execute(Program->Low, NonZero);
  ASSERT_TRUE(static_cast<bool>(TruePath))
      << llvm::toString(TruePath.takeError());
  ASSERT_GE(TruePath->Trace.size(), 2u);
  EXPECT_EQ(TruePath->Trace[TruePath->Trace.size() - 2].PC, 8u);
}

TEST(EVMInterpreter, ImplementsWideArithmeticTransientStorageAndClz) {
  // PUSH0 CLZ => 256, store transiently, reload and return it.
  const std::vector<uint8_t> Code = {0x5f, 0x1e, 0x60, 0x01, 0x5d, 0x60,
                                     0x01, 0x5c, 0x60, 0x00, 0x52, 0x60,
                                     0x20, 0x60, 0x00, 0xf3};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Result = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->ReturnData.size(), 32u);
  EXPECT_EQ(Result->ReturnData[30], 1u);
  EXPECT_EQ(Result->ReturnData[31], 0u);
  ASSERT_EQ(Result->TransientStorage.size(), 1u);
}

TEST(EVMInterpreter, RevertRestoresStateAndLogsButKeepsDiagnostics) {
  std::vector<uint8_t> Code = transactionMutationPrefix();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::PUSH1), 1, opcodeByte(Opcode::PUSH0),
               opcodeByte(Opcode::REVERT)});
  const ExecutionResult Result = executeCode(Code, transactionEnvironment());

  EXPECT_EQ(Result.Status, ExecutionStatus::Reverted);
  ASSERT_EQ(Result.ReturnData.size(), 1u);
  EXPECT_EQ(Result.ReturnData.front(), kTransactionDiagnosticByte);
  ASSERT_GE(Result.Memory.size(), 1u);
  EXPECT_EQ(Result.Memory.front(), kTransactionDiagnosticByte);
  EXPECT_FALSE(Result.Trace.empty());
  expectTransactionStateRestored(Result);
}

TEST(EVMInterpreter, FaultRestoresStateAndLogsButKeepsDiagnostics) {
  std::vector<uint8_t> Code = transactionMutationPrefix();
  Code.push_back(opcodeByte(Opcode::POP));

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Low = decodeLowIR(Code, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  auto Result = execute(*Low, transactionEnvironment());
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());

  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->FaultKind, ExecutionFaultKind::Semantic);
  EXPECT_NE(Result->Error.find("stack underflow"), std::string::npos);
  ASSERT_GE(Result->Memory.size(), 1u);
  EXPECT_EQ(Result->Memory.front(), kTransactionDiagnosticByte);
  EXPECT_FALSE(Result->Trace.empty());
  expectTransactionStateRestored(*Result);
}

TEST(EVMInterpreter, StepLimitIsInconclusiveAndCannotCommitState) {
  std::vector<uint8_t> Code = transactionMutationPrefix();
  Code.push_back(opcodeByte(Opcode::STOP));
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_FALSE(Program->Low.Instructions.empty());

  InterpreterOptions Options;
  Options.MaxSteps = Program->Low.Instructions.size() - 1;
  auto Result = execute(Program->Low, transactionEnvironment(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());

  EXPECT_EQ(Result->Status, ExecutionStatus::StepLimit);
  EXPECT_EQ(Result->Steps, Program->Low.Instructions.size() - 1);
  ASSERT_GE(Result->Memory.size(), 1u);
  EXPECT_EQ(Result->Memory.front(), kTransactionDiagnosticByte);
  EXPECT_FALSE(Result->Trace.empty());
  expectTransactionStateRestored(*Result);
}

TEST(EVMInterpreter, NaturalStopPrecedesTheStepBudgetAtEmptyCode) {
  auto Low = decodeLowIR({});
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  InterpreterOptions Options;
  Options.MaxSteps = 0;

  auto Result = execute(*Low, transactionEnvironment(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Stopped);
  EXPECT_EQ(Result->Steps, 0u);
  EXPECT_TRUE(Result->Error.empty());
  expectTransactionStateRestored(*Result);
}

TEST(EVMInterpreter, ExactStepBudgetCommitsBeforeFallingOffCode) {
  const std::vector<uint8_t> Code = transactionMutationPrefix();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions Options;
  Options.MaxSteps = Program->Low.Instructions.size();
  auto Result = execute(Program->Low, transactionEnvironment(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());

  EXPECT_EQ(Result->Status, ExecutionStatus::Stopped);
  EXPECT_EQ(Result->Steps, Program->Low.Instructions.size());
  EXPECT_TRUE(Result->Error.empty());
  ASSERT_EQ(Result->Storage.size(), 1u);
  EXPECT_EQ(Result->Storage.at(testWord(1)), testWord(0xaa));
  ASSERT_EQ(Result->TransientStorage.size(), 1u);
  EXPECT_EQ(Result->TransientStorage.at(testWord(2)), testWord(0xbb));
  ASSERT_EQ(Result->Logs.size(), 1u);
  EXPECT_TRUE(Result->Logs.front().Topics.empty());
  ASSERT_EQ(Result->Logs.front().Data.size(), 1u);
  EXPECT_EQ(Result->Logs.front().Data.front(), kTransactionDiagnosticByte);
}

TEST(EVMInterpreter, TraceBudgetAcceptsTheBoundaryAndRollsBackTheNextEntry) {
  const std::vector<uint8_t> Code = transactionMutationPrefix();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const size_t ExactTraceEntries = Program->Low.Instructions.size();
  ASSERT_GT(ExactTraceEntries, 1u);

  InterpreterOptions AtBoundary;
  AtBoundary.MaxTraceEntries = ExactTraceEntries;
  auto Accepted = execute(Program->Low, transactionEnvironment(), AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  ASSERT_EQ(Accepted->Status, ExecutionStatus::Stopped);
  EXPECT_EQ(Accepted->Trace.size(), ExactTraceEntries);
  EXPECT_EQ(Accepted->Storage.at(testWord(1)), testWord(0xaa));

  InterpreterOptions BelowBoundary = AtBoundary;
  --BelowBoundary.MaxTraceEntries;
  auto Rejected =
      execute(Program->Low, transactionEnvironment(), BelowBoundary);
  ASSERT_TRUE(static_cast<bool>(Rejected))
      << llvm::toString(Rejected.takeError());
  EXPECT_EQ(Rejected->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Rejected->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_EQ(Rejected->Trace.size(), ExactTraceEntries - 1);
  expectTransactionStateRestored(*Rejected);
}

TEST(EVMInterpreter, LogEntryBudgetAcceptsTheBoundaryAndRollsBackTheNextLog) {
  constexpr size_t kExactLogEntries = 2;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::LOG0),  opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::LOG0),
      opcodeByte(Opcode::STOP)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions AtBoundary;
  AtBoundary.MaxLogEntries = kExactLogEntries;
  auto Accepted = execute(Program->Low, {}, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->Status, ExecutionStatus::Stopped);
  EXPECT_EQ(Accepted->Logs.size(), kExactLogEntries);

  InterpreterOptions BelowBoundary = AtBoundary;
  --BelowBoundary.MaxLogEntries;
  auto Rejected = execute(Program->Low, {}, BelowBoundary);
  ASSERT_TRUE(static_cast<bool>(Rejected))
      << llvm::toString(Rejected.takeError());
  EXPECT_EQ(Rejected->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Rejected->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_TRUE(Rejected->Logs.empty());
}

TEST(EVMInterpreter, LogDataBudgetChargesAggregateBytesBeforeAllocation) {
  constexpr size_t kExactLogDataBytes = 3;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1), 0xaa,
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE8),
      opcodeByte(Opcode::PUSH1), 1,
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::LOG0),
      opcodeByte(Opcode::PUSH1), 2,
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::LOG0),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions AtBoundary;
  AtBoundary.MaxLogDataBytes = kExactLogDataBytes;
  auto Accepted = execute(Program->Low, {}, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  ASSERT_EQ(Accepted->Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Accepted->Logs.size(), 2u);
  EXPECT_EQ(Accepted->Logs[0].Data.size(), 1u);
  EXPECT_EQ(Accepted->Logs[1].Data.size(), 2u);

  InterpreterOptions BelowBoundary = AtBoundary;
  --BelowBoundary.MaxLogDataBytes;
  auto Rejected = execute(Program->Low, {}, BelowBoundary);
  ASSERT_TRUE(static_cast<bool>(Rejected))
      << llvm::toString(Rejected.takeError());
  EXPECT_EQ(Rejected->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Rejected->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_TRUE(Rejected->Logs.empty());
}

TEST(EVMInterpreter, LoopingLogsStopAtTheDeclarativeEntryBudget) {
  constexpr size_t kLoopLogEntries = 2;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),    opcodeByte(Opcode::LOG0),
      opcodeByte(Opcode::PUSH0),    opcodeByte(Opcode::JUMP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  InterpreterOptions Options;
  Options.MaxLogEntries = kLoopLogEntries;
  auto Result = execute(Program->Low, {}, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_TRUE(Result->Logs.empty());
  EXPECT_TRUE(Result->Memory.empty());
  EXPECT_LT(Result->Steps, Options.MaxSteps);
}

TEST(EVMInterpreter, UsesYellowPaperOperandOrderForNonCommutativeOps) {
  const struct {
    uint8_t Opcode;
    uint8_t FirstPushed;
    uint8_t Top;
    uint64_t Expected;
  } Cases[] = {
      {0x03, 3, 10, 7}, // SUB: 10 - 3
      {0x04, 3, 10, 3}, // DIV: 10 / 3
      {0x06, 3, 10, 1}, // MOD: 10 % 3
      {0x10, 3, 10, 0}, // LT: 10 < 3
      {0x11, 3, 10, 1}, // GT: 10 > 3
      {0x0a, 3, 2, 8},  // EXP: 2 ** 3
  };

  for (const auto &Case : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "opcode 0x" << std::hex << unsigned(Case.Opcode));
    auto Result = executeCode(
        {0x60, Case.FirstPushed, 0x60, Case.Top, Case.Opcode, 0x00});
    ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
    ASSERT_EQ(Result.Stack.size(), 1u);
    EXPECT_EQ(Result.Stack.back().getZExtValue(), Case.Expected);
  }

  // SIGNEXTEND pops the byte index first, then the value.
  auto Signed = executeCode({0x60, 0x80, 0x5f, 0x0b, 0x00});
  ASSERT_EQ(Signed.Stack.size(), 1u);
  const llvm::APInt Negative128 =
      llvm::APInt::getAllOnes(256).shl(8) | llvm::APInt(256, 0x80);
  EXPECT_EQ(Signed.Stack.back(), Negative128);
}

TEST(EVMInterpreter, ComputesEthereumKeccak256) {
  // keccak256("") is distinct from standardized SHA3-256.
  auto Result = executeCode({0x5f, 0x5f, 0x20, 0x00});
  ASSERT_EQ(Result.Stack.size(), 1u);
  const llvm::APInt Expected(
      256, "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470",
      16);
  EXPECT_EQ(Result.Stack.back(), Expected);

  // Store "abc" in the low bytes of a word and hash exactly those bytes.
  Result = executeCode({opcodeByte(Opcode::PUSH3), 0x61, 0x62, 0x63,
                        opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                        opcodeByte(Opcode::PUSH1), 3, opcodeByte(Opcode::PUSH1),
                        kWordBytes - 3, opcodeByte(Opcode::SHA3),
                        opcodeByte(Opcode::STOP)});
  ASSERT_EQ(Result.Stack.size(), 1u);
  const llvm::APInt ABC(
      kWordBits,
      "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45",
      kHexRadix);
  EXPECT_EQ(Result.Stack.back(), ABC);
}

TEST(EVMInterpreter, ImplementsOverlappingMcopyAndBlobhash) {
  // memory[0..2] = aa bb, then memmove(memory + 1, memory, 2).
  const std::vector<uint8_t> Code = {
      0x60, 0xaa, 0x5f, 0x53,             // mstore8(0, aa)
      0x60, 0xbb, 0x60, 0x01, 0x53,       // mstore8(1, bb)
      0x60, 0x02, 0x5f, 0x60, 0x01, 0x5e, // mcopy(1, 0, 2)
      0x5f, 0x49, 0x00};                  // blobhash(0), stop
  ExecutionEnvironment Environment;
  Environment.BlobHashes.emplace_back(256, 0x1234);
  auto Result = executeCode(Code, std::move(Environment));
  ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
  ASSERT_GE(Result.Memory.size(), 3u);
  EXPECT_EQ(Result.Memory[0], 0xaau);
  EXPECT_EQ(Result.Memory[1], 0xaau);
  EXPECT_EQ(Result.Memory[2], 0xbbu);
  ASSERT_EQ(Result.Stack.size(), 1u);
  EXPECT_EQ(Result.Stack.back().getZExtValue(), 0x1234u);
}

TEST(EVMInterpreter, RoundsMemoryAndAllowsHugeOffsetForZeroLength) {
  std::vector<uint8_t> StoreByte{0x7f};
  StoreByte.insert(StoreByte.end(), 32, 0xff);
  StoreByte.insert(StoreByte.end(), {0x5f, 0x53, 0x59, 0x00});
  auto Stored = executeCode(StoreByte);
  ASSERT_EQ(Stored.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Stored.Memory.size(), 32u);
  EXPECT_EQ(Stored.Memory.front(), 0xffu);
  ASSERT_EQ(Stored.Stack.size(), 1u);
  EXPECT_EQ(Stored.Stack.back().getZExtValue(), 32u);

  std::vector<uint8_t> EmptyReturn{0x5f, 0x7f};
  EmptyReturn.insert(EmptyReturn.end(), 32, 0xff);
  EmptyReturn.push_back(0xf3);
  auto Returned = executeCode(EmptyReturn);
  EXPECT_EQ(Returned.Status, ExecutionStatus::Returned);
  EXPECT_TRUE(Returned.ReturnData.empty());
  EXPECT_TRUE(Returned.Memory.empty());
}

TEST(EVMInterpreter, HugeCopySourceOffsetsZeroFillAndExpandMemory) {
  std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH1), 1,
                               opcodeByte(Opcode::PUSH32)};
  Code.insert(Code.end(), kWordBytes, kByteMax);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATACOPY),
               opcodeByte(Opcode::MSIZE), opcodeByte(Opcode::STOP)});

  auto Result = executeCode(Code);
  ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Result.Memory.size(), kWordBytes);
  EXPECT_EQ(Result.Memory.front(), 0u);
  ASSERT_EQ(Result.Stack.size(), 1u);
  EXPECT_EQ(Result.Stack.back().getZExtValue(), kWordBytes);
}

TEST(EVMInterpreter, KeepsSubcallReturnBufferSeparateFromFrameOutput) {
  ExecutionEnvironment Environment;
  Environment.InitialReturnData = {0xaa, 0xbb, 0xcc};
  auto Result = executeCode(
      {opcodeByte(Opcode::RETURNDATASIZE), opcodeByte(Opcode::STOP)},
      std::move(Environment));
  ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Result.Stack.size(), 1u);
  EXPECT_EQ(Result.Stack.back().getZExtValue(), 3u);
  EXPECT_TRUE(Result.ReturnData.empty());

  ExecutionEnvironment CallEnvironment;
  CallEnvironment.CallReturnData = {0x11, 0x22};
  Result =
      executeCode({opcodeByte(Opcode::PUSH1), 2,
                   opcodeByte(Opcode::PUSH0), // output size and offset
                   opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                   opcodeByte(Opcode::PUSH0), // input size, offset, and value
                   opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                   opcodeByte(Opcode::CALL), opcodeByte(Opcode::RETURNDATASIZE),
                   opcodeByte(Opcode::STOP)},
                  std::move(CallEnvironment));
  ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Result.Stack.size(), 2u);
  EXPECT_EQ(Result.Stack.back().getZExtValue(), 2u);
  ASSERT_GE(Result.Memory.size(), 2u);
  EXPECT_EQ(Result.Memory[0], 0x11u);
  EXPECT_EQ(Result.Memory[1], 0x22u);
  EXPECT_TRUE(Result.ReturnData.empty());

  ExecutionEnvironment CreateEnvironment;
  CreateEnvironment.CreateSuccess = false;
  CreateEnvironment.CreateReturnData = {0x44};
  Result = executeCode({opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                        opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CREATE),
                        opcodeByte(Opcode::RETURNDATASIZE),
                        opcodeByte(Opcode::STOP)},
                       std::move(CreateEnvironment));
  ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Result.Stack.size(), 2u);
  EXPECT_TRUE(Result.Stack.front().isZero());
  EXPECT_EQ(Result.Stack.back().getZExtValue(), 1u);
  EXPECT_TRUE(Result.ReturnData.empty());
}

TEST(EVMInterpreter, EnforcesMemoryLimitAtEVMWordGranularity) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1), 0xaa, opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::MSTORE8), opcodeByte(Opcode::STOP)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions TooSmall;
  TooSmall.MaxMemoryBytes = kWordBytes - 1;
  auto Faulted = execute(Program->Low, {}, TooSmall);
  ASSERT_TRUE(static_cast<bool>(Faulted))
      << llvm::toString(Faulted.takeError());
  EXPECT_EQ(Faulted->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Faulted->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_TRUE(Faulted->Error.empty());
  EXPECT_TRUE(Faulted->Memory.empty());

  InterpreterOptions OneWord;
  OneWord.MaxMemoryBytes = kWordBytes;
  auto Stored = execute(Program->Low, {}, OneWord);
  ASSERT_TRUE(static_cast<bool>(Stored)) << llvm::toString(Stored.takeError());
  EXPECT_EQ(Stored->Status, ExecutionStatus::Stopped);
  EXPECT_EQ(Stored->Memory.size(), kWordBytes);
}

TEST(EVMInterpreter, MemoryBudgetExhaustionRollsBackPersistentEffects) {
  std::vector<uint8_t> Code = transactionMutationPrefix();
  Code.push_back(opcodeByte(Opcode::STOP));
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions Options;
  Options.MaxMemoryBytes = 0;
  auto Result = execute(Program->Low, transactionEnvironment(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_TRUE(Result->Error.empty());
  EXPECT_TRUE(Result->Memory.empty());
  expectTransactionStateRestored(*Result);
}

TEST(EVMInterpreter, PreflightsStackBeforeMemoryAndStateEffects) {
  static_assert(kDefaultMaxMemoryBytes <= std::numeric_limits<uint32_t>::max());
  const uint32_t RequestedSize = static_cast<uint32_t>(kDefaultMaxMemoryBytes);
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(RequestedSize >> 24),
      static_cast<uint8_t>(RequestedSize >> 16),
      static_cast<uint8_t>(RequestedSize >> 8),
      static_cast<uint8_t>(RequestedSize),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::LOG4),
  };

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Low = decodeLowIR(Code, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  auto Result = execute(*Low);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());

  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_NE(Result->Error.find("stack underflow"), std::string::npos);
  EXPECT_TRUE(Result->Memory.empty());
  EXPECT_TRUE(Result->Logs.empty());
  ASSERT_EQ(Result->Stack.size(), 2u);
  EXPECT_EQ(Result->Stack.front().getZExtValue(), RequestedSize);
  EXPECT_TRUE(Result->Stack.back().isZero());
}

TEST(EVMInterpreter, PreflightsEveryAssignedOpcodeStackRequirement) {
  for (size_t Byte = 0; Byte < kOpcodeSpaceSize; ++Byte) {
    const auto Info =
        opcodeInfo(static_cast<uint8_t>(Byte), kNewestKnownHardfork);
    if (!Info)
      continue;

    std::vector<uint8_t> EncodedInstruction{static_cast<uint8_t>(Byte)};
    EncodedInstruction.insert(EncodedInstruction.end(), Info->ImmediateBytes,
                              uint8_t{0});
    DecodeOptions Decode;
    Decode.Fork = kNewestKnownHardfork;
    Decode.Strict = false;
    auto Decoded = decodeBytecode(EncodedInstruction, Decode);
    ASSERT_TRUE(static_cast<bool>(Decoded))
        << llvm::toString(Decoded.takeError());
    ASSERT_FALSE(Decoded->Instructions.empty());
    const LowInstruction &DecodedInstruction = Decoded->Instructions.front();
    if (!DecodedInstruction.isExecutable() ||
        DecodedInstruction.requiredStackHeight() == 0)
      continue;

    SCOPED_TRACE(testing::Message()
                 << Info->Name.str() << " (0x" << std::hex << Byte << ")");
    const size_t InitialStackHeight =
        DecodedInstruction.requiredStackHeight() - 1;
    std::vector<uint8_t> Code(InitialStackHeight, opcodeByte(Opcode::PUSH0));
    const uint64_t InstructionPC = Code.size();
    Code.insert(Code.end(), EncodedInstruction.begin(),
                EncodedInstruction.end());

    AnalyzeOptions Relaxed;
    Relaxed.Fork = kNewestKnownHardfork;
    Relaxed.Strict = false;
    auto Low = decodeLowIR(Code, Relaxed);
    ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
    auto Result = execute(*Low);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());

    EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
    EXPECT_EQ(Result->FinalPC, InstructionPC);
    EXPECT_NE(Result->Error.find("stack underflow"), std::string::npos);
    EXPECT_EQ(Result->Stack.size(), InitialStackHeight);
    EXPECT_TRUE(Result->Memory.empty());
    EXPECT_TRUE(Result->Storage.empty());
    EXPECT_TRUE(Result->TransientStorage.empty());
    EXPECT_TRUE(Result->Logs.empty());
  }
}

TEST(EVMInterpreter, PreflightsMaximumResultStackHeight) {
  std::vector<uint8_t> Code(kStackLimit, opcodeByte(Opcode::PUSH0));
  Code.push_back(opcodeByte(Opcode::DUP1));

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Low = decodeLowIR(Code, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  auto Result = execute(*Low);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());

  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_NE(Result->Error.find("stack overflow"), std::string::npos);
  EXPECT_EQ(Result->FinalPC, kStackLimit);
  EXPECT_EQ(Result->Stack.size(), kStackLimit);
  ASSERT_FALSE(Result->Trace.empty());
  EXPECT_EQ(Result->Trace.back().StackBefore, kStackLimit);
  EXPECT_EQ(Result->Trace.back().StackAfter, kStackLimit);
}

TEST(EVMInterpreter, ConvertsAllocationFailuresIntoRuntimeFaults) {
  const size_t MaximumVectorSize = std::vector<uint8_t>{}.max_size();
  ASSERT_LT(MaximumVectorSize, std::numeric_limits<size_t>::max());
  const size_t ImpossibleSize = MaximumVectorSize + 1;
  const llvm::APInt SizeWord(kWordBits, ImpossibleSize);

  std::vector<uint8_t> Code{opcodeByte(Opcode::PUSH32)};
  for (unsigned Byte = 0; Byte < kWordBytes; ++Byte) {
    const unsigned BitOffset = (kWordBytes - 1 - Byte) * kBitsPerByte;
    Code.push_back(static_cast<uint8_t>(
        SizeWord.extractBitsAsZExtValue(kBitsPerByte, BitOffset)));
  }
  Code.push_back(opcodeByte(Opcode::PUSH0));
  Code.push_back(opcodeByte(Opcode::LOG0));

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Low = decodeLowIR(Code, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  InterpreterOptions Options;
  Options.MaxMemoryBytes = std::numeric_limits<size_t>::max();
  const ExecutionEnvironment Environment = transactionEnvironment();
  auto Result = execute(*Low, Environment, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());

  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_TRUE(Result->Error.empty());
  EXPECT_TRUE(Result->HasPersistentStateSnapshot);
  EXPECT_TRUE(Result->Memory.empty());
  expectTransactionStateRestored(*Result);
  EXPECT_EQ(Environment.Storage.at(testWord(1)), testWord(0x11));
  EXPECT_EQ(Environment.TransientStorage.at(testWord(2)), testWord(0x22));
}

TEST(EVMInterpreter, RejectsNonCanonicalLowIRBeforeExecution) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH1), 2,
                                     opcodeByte(Opcode::JUMPDEST),
                                     opcodeByte(Opcode::STOP)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Instructions.size(), 3u);
  ASSERT_EQ(Program->Low.JumpDestinations.size(), 1u);

  const auto ExpectRejected = [](EVMLowIR Mutated, llvm::StringRef Case) {
    SCOPED_TRACE(Case.str());
    auto Result = execute(Mutated);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("LowIR"),
              std::string::npos);
  };

  EVMLowIR WrongImmediateWidth = Program->Low;
  WrongImmediateWidth.Instructions.front().Immediate = llvm::APInt(8, 2);
  ExpectRejected(std::move(WrongImmediateWidth), "Immediate width");

  EVMLowIR WrongInfo = Program->Low;
  WrongInfo.Instructions.front().Info.StackPushes = 0;
  ExpectRejected(std::move(WrongInfo), "OpcodeInfo");

  EVMLowIR WrongNextPC = Program->Low;
  ++WrongNextPC.Instructions.front().NextPC;
  ExpectRejected(std::move(WrongNextPC), "NextPC");

  EVMLowIR WrongJumpDestinations = Program->Low;
  WrongJumpDestinations.JumpDestinations.clear();
  ExpectRejected(std::move(WrongJumpDestinations), "JumpDestinations");
}

TEST(EVMInterpreter, HostReturnDataBudgetValidatesTheAggregateBeforeCopying) {
  constexpr size_t kInitialReturnBytes = 1;
  constexpr size_t kCallReturnBytes = 2;
  constexpr size_t kCreateReturnBytes = 3;
  constexpr size_t kExactHostReturnBytes =
      kInitialReturnBytes + kCallReturnBytes + kCreateReturnBytes;
  auto Program = analyze(std::vector<uint8_t>{opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ExecutionEnvironment Environment;
  Environment.InitialReturnData.resize(kInitialReturnBytes);
  Environment.CallReturnData.resize(kCallReturnBytes);
  Environment.CreateReturnData.resize(kCreateReturnBytes);

  InterpreterOptions AtBoundary;
  AtBoundary.MaxHostReturnDataBytes = kExactHostReturnBytes;
  auto Accepted = execute(Program->Low, Environment, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->Status, ExecutionStatus::Stopped);

  InterpreterOptions BelowBoundary = AtBoundary;
  --BelowBoundary.MaxHostReturnDataBytes;
  auto Rejected = execute(Program->Low, Environment, BelowBoundary);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError())
                .find(kMaxHostReturnDataBytesName.str()),
            std::string::npos);
}

TEST(EVMInterpreter, EntireHostEnvironmentIsBoundedBeforeCopying) {
  constexpr size_t kCalldataBytes = 3;
  constexpr size_t kHostEntries = 6;
  constexpr size_t kExternalCodeBytes = 3;
  auto Program = analyze(std::vector<uint8_t>{opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ExecutionEnvironment Environment;
  Environment.Calldata.resize(kCalldataBytes);
  Environment.BlockHashes.emplace(testWord(1), testWord(0x11));
  Environment.Balances.emplace(testWord(2), testWord(0x22));
  Environment.CodeHashes.emplace(testWord(3), testWord(0x33));
  Environment.ExternalCode.emplace(testWord(4), std::vector<uint8_t>{0xaa});
  Environment.ExternalCode.emplace(testWord(5),
                                   std::vector<uint8_t>{0xbb, 0xcc});
  Environment.BlobHashes.push_back(testWord(0x44));

  InterpreterOptions AtBoundary;
  AtBoundary.MaxCalldataBytes = kCalldataBytes;
  AtBoundary.MaxHostEnvironmentEntries = kHostEntries;
  AtBoundary.MaxExternalCodeBytes = kExternalCodeBytes;
  auto Accepted = execute(Program->Low, Environment, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->Status, ExecutionStatus::Stopped);

  const auto ExpectRejected = [&](InterpreterOptions Options,
                                  llvm::StringRef LimitName) {
    auto Rejected = execute(Program->Low, Environment, Options);
    ASSERT_FALSE(static_cast<bool>(Rejected));
    EXPECT_NE(llvm::toString(Rejected.takeError()).find(LimitName.str()),
              std::string::npos);
  };

  InterpreterOptions CalldataOverflow = AtBoundary;
  --CalldataOverflow.MaxCalldataBytes;
  ExpectRejected(CalldataOverflow, kMaxCalldataBytesName);

  InterpreterOptions EntryOverflow = AtBoundary;
  --EntryOverflow.MaxHostEnvironmentEntries;
  ExpectRejected(EntryOverflow, kMaxHostEnvironmentEntriesName);

  InterpreterOptions CodeOverflow = AtBoundary;
  --CodeOverflow.MaxExternalCodeBytes;
  ExpectRejected(CodeOverflow, kMaxExternalCodeBytesName);
}

TEST(EVMInterpreter, HostEntryBoundPrecedesMapWidthTraversal) {
  auto Program = analyze(std::vector<uint8_t>{opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ExecutionEnvironment Environment;
  Environment.ExternalCode.emplace(llvm::APInt(kWordBits / 2, 1),
                                   std::vector<uint8_t>{});
  InterpreterOptions Options;
  Options.MaxHostEnvironmentEntries = 0;
  auto Result = execute(Program->Low, Environment, Options);
  ASSERT_FALSE(static_cast<bool>(Result));
  const std::string Error = llvm::toString(Result.takeError());
  EXPECT_NE(Error.find(kMaxHostEnvironmentEntriesName.str()),
            std::string::npos);
  EXPECT_EQ(Error.find("256-bit"), std::string::npos);
}

TEST(EVMInterpreter, ZeroSizedHostOutputsKeepReturnDataAsBoundedViews) {
  constexpr size_t kCallReturnBytes = 4'096;
  constexpr size_t kCreateReturnBytes = 2'048;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),          opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),          opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),          opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),          opcodeByte(Opcode::CALL),
      opcodeByte(Opcode::PUSH0),          opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),          opcodeByte(Opcode::CREATE),
      opcodeByte(Opcode::RETURNDATASIZE), opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ExecutionEnvironment Environment;
  Environment.CallReturnData.resize(kCallReturnBytes, 0xaa);
  Environment.CreateSuccess = false;
  Environment.CreateReturnData.resize(kCreateReturnBytes, 0xbb);
  InterpreterOptions Options;
  Options.MaxMemoryBytes = 0;
  Options.MaxHostReturnDataBytes = kCallReturnBytes + kCreateReturnBytes;
  auto Result = execute(Program->Low, std::move(Environment), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Stopped);
  EXPECT_TRUE(Result->Memory.empty());
  EXPECT_TRUE(Result->ReturnData.empty());
  ASSERT_EQ(Result->Stack.size(), 3u);
  EXPECT_EQ(Result->Stack.back().getZExtValue(), kCreateReturnBytes);
}

TEST(EVMInterpreter, PersistentStateBudgetIsExactAtEntryAndAtRuntime) {
  constexpr size_t kExactPersistentEntries = 2;
  auto Stop = analyze(std::vector<uint8_t>{opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Stop)) << llvm::toString(Stop.takeError());
  ExecutionEnvironment InitialState;
  InitialState.Storage.emplace(testWord(1), testWord(0x11));
  InitialState.TransientStorage.emplace(testWord(2), testWord(0x22));

  InterpreterOptions AtBoundary;
  AtBoundary.MaxPersistentStateEntries = kExactPersistentEntries;
  auto AcceptedInitial = execute(Stop->Low, InitialState, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(AcceptedInitial))
      << llvm::toString(AcceptedInitial.takeError());
  EXPECT_EQ(AcceptedInitial->Status, ExecutionStatus::Stopped);

  InterpreterOptions BelowInitialBoundary = AtBoundary;
  --BelowInitialBoundary.MaxPersistentStateEntries;
  auto RejectedInitial = execute(Stop->Low, InitialState, BelowInitialBoundary);
  ASSERT_FALSE(static_cast<bool>(RejectedInitial));
  EXPECT_NE(llvm::toString(RejectedInitial.takeError())
                .find(kMaxPersistentStateEntriesName.str()),
            std::string::npos);

  const std::vector<uint8_t> MutateCode = {
      opcodeByte(Opcode::PUSH1),
      0xaa,
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::SSTORE),
      opcodeByte(Opcode::PUSH1),
      0xbb,
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::TSTORE),
      opcodeByte(Opcode::STOP),
  };
  auto Mutate = analyze(MutateCode);
  ASSERT_TRUE(static_cast<bool>(Mutate)) << llvm::toString(Mutate.takeError());
  auto AcceptedRuntime = execute(Mutate->Low, {}, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(AcceptedRuntime))
      << llvm::toString(AcceptedRuntime.takeError());
  EXPECT_EQ(AcceptedRuntime->Status, ExecutionStatus::Stopped);
  EXPECT_EQ(AcceptedRuntime->Storage.size(), 1u);
  EXPECT_EQ(AcceptedRuntime->TransientStorage.size(), 1u);

  InterpreterOptions BelowRuntimeBoundary = AtBoundary;
  --BelowRuntimeBoundary.MaxPersistentStateEntries;
  auto RejectedRuntime = execute(Mutate->Low, {}, BelowRuntimeBoundary);
  ASSERT_TRUE(static_cast<bool>(RejectedRuntime))
      << llvm::toString(RejectedRuntime.takeError());
  EXPECT_EQ(RejectedRuntime->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(RejectedRuntime->FaultKind, ExecutionFaultKind::ResourceExhausted);
  EXPECT_TRUE(RejectedRuntime->Storage.empty());
  EXPECT_TRUE(RejectedRuntime->TransientStorage.empty());
}

TEST(EVMInterpreter, RejectsExplicitZeroEntriesInSparseStateInputs) {
  auto Program = analyze(std::vector<uint8_t>{opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions Options;
  Options.MaxPersistentStateEntries = 1;

  ExecutionEnvironment StorageEnvironment;
  StorageEnvironment.Storage.emplace(testWord(1), testWord(0));
  auto StorageResult = execute(Program->Low, StorageEnvironment, Options);
  ASSERT_FALSE(static_cast<bool>(StorageResult));
  const std::string StorageError = llvm::toString(StorageResult.takeError());
  EXPECT_NE(StorageError.find("Storage"), std::string::npos);
  EXPECT_NE(StorageError.find("zero-valued"), std::string::npos);

  ExecutionEnvironment TransientEnvironment;
  TransientEnvironment.TransientStorage.emplace(testWord(1), testWord(0));
  auto TransientResult =
      execute(Program->Low, std::move(TransientEnvironment), Options);
  ASSERT_FALSE(static_cast<bool>(TransientResult));
  const std::string TransientError =
      llvm::toString(TransientResult.takeError());
  EXPECT_NE(TransientError.find("TransientStorage"), std::string::npos);
  EXPECT_NE(TransientError.find("zero-valued"), std::string::npos);
  EXPECT_EQ(TransientEnvironment.TransientStorage.size(), 1u);
}

TEST(EVMInterpreter, ZeroSStoreToAbsentSlotDoesNotConsumeStateBudget) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),  opcodeByte(Opcode::PUSH1), 1,
      opcodeByte(Opcode::SSTORE), opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions Options;
  Options.MaxPersistentStateEntries = 0;
  auto Result = execute(Program->Low, {}, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Stopped);
  EXPECT_TRUE(Result->Storage.empty());
  EXPECT_TRUE(Result->TransientStorage.empty());
}

TEST(EVMInterpreter, ZeroTStoreToAbsentSlotDoesNotConsumeStateBudget) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),  opcodeByte(Opcode::PUSH1), 1,
      opcodeByte(Opcode::TSTORE), opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  InterpreterOptions Options;
  Options.MaxPersistentStateEntries = 0;
  auto Result = execute(Program->Low, {}, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Stopped);
  EXPECT_TRUE(Result->Storage.empty());
  EXPECT_TRUE(Result->TransientStorage.empty());
}

TEST(EVMInterpreter, ClearingSStoreReleasesBudgetForTStore) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::SSTORE),
      opcodeByte(Opcode::PUSH1),
      0xbb,
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::TSTORE),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ExecutionEnvironment Environment;
  Environment.Storage.emplace(testWord(1), testWord(0x11));
  InterpreterOptions Options;
  Options.MaxPersistentStateEntries = 1;

  auto Result = execute(Program->Low, std::move(Environment), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Stopped);
  EXPECT_TRUE(Result->Storage.empty());
  ASSERT_EQ(Result->TransientStorage.size(), 1u);
  EXPECT_EQ(Result->TransientStorage.at(testWord(2)), testWord(0xbb));
}

TEST(EVMInterpreter, ClearingTStoreReleasesBudgetForSStore) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::TSTORE),
      opcodeByte(Opcode::PUSH1),
      0xaa,
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::SSTORE),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ExecutionEnvironment Environment;
  Environment.TransientStorage.emplace(testWord(1), testWord(0x11));
  InterpreterOptions Options;
  Options.MaxPersistentStateEntries = 1;

  auto Result = execute(Program->Low, std::move(Environment), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Result->Storage.size(), 1u);
  EXPECT_EQ(Result->Storage.at(testWord(2)), testWord(0xaa));
  EXPECT_TRUE(Result->TransientStorage.empty());
}

TEST(EVMInterpreter, StateEntryExhaustionRollsBackEarlierSparseWrites) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1),
      0xaa,
      opcodeByte(Opcode::PUSH1),
      1,
      opcodeByte(Opcode::SSTORE),
      opcodeByte(Opcode::PUSH1),
      0xbb,
      opcodeByte(Opcode::PUSH1),
      2,
      opcodeByte(Opcode::TSTORE),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ExecutionEnvironment Environment;
  Environment.Storage.emplace(testWord(1), testWord(0x11));
  InterpreterOptions Options;
  Options.MaxPersistentStateEntries = 1;

  auto Result = execute(Program->Low, std::move(Environment), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->FaultKind, ExecutionFaultKind::ResourceExhausted);
  ASSERT_EQ(Result->Storage.size(), 1u);
  EXPECT_EQ(Result->Storage.at(testWord(1)), testWord(0x11));
  EXPECT_TRUE(Result->TransientStorage.empty());
}

TEST(EVMInterpreter, RejectsEnvironmentValuesWithTheWrongWordWidth) {
  auto Program = analyze(std::vector<uint8_t>{opcodeByte(Opcode::ADDRESS),
                                              opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ExecutionEnvironment Environment;
  Environment.Address = llvm::APInt(kWordBits / 2, 0);
  auto Result = execute(Program->Low, std::move(Environment));
  ASSERT_FALSE(static_cast<bool>(Result));
  const std::string Error = llvm::toString(Result.takeError());
  EXPECT_NE(Error.find("Address"), std::string::npos);
  EXPECT_NE(Error.find("256-bit"), std::string::npos);
}

TEST(EVMInterpreter, DiagnosesMixedWidthMapKeysWithoutAPIntAssertions) {
  auto Program = analyze(std::vector<uint8_t>{opcodeByte(Opcode::STOP)});
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ExecutionEnvironment Environment;
  Environment.Balances.emplace(llvm::APInt(kWordBits / 2, 1),
                               llvm::APInt(kWordBits, 2));
  Environment.Balances.emplace(llvm::APInt(kWordBits, 3),
                               llvm::APInt(kWordBits, 4));
  auto Result = execute(Program->Low, std::move(Environment));
  ASSERT_FALSE(static_cast<bool>(Result));
  const std::string Error = llvm::toString(Result.takeError());
  EXPECT_NE(Error.find("Balances key"), std::string::npos);
  EXPECT_NE(Error.find("256-bit"), std::string::npos);
}

TEST(EVMInterpreter, MasksAccountOperandsToTheEVMAddressWidth) {
  std::vector<uint8_t> Code{opcodeByte(Opcode::PUSH32)};
  Code.insert(Code.end(), kWordBytes, uint8_t{0});
  Code[1] = kByteMax;
  Code[kWordBytes] = 0x42;
  Code.push_back(opcodeByte(Opcode::BALANCE));
  Code.push_back(opcodeByte(Opcode::STOP));

  ExecutionEnvironment Environment;
  Environment.Balances.emplace(llvm::APInt(kWordBits, 0x42),
                               llvm::APInt(kWordBits, 7));
  auto Result = executeCode(Code, std::move(Environment));
  ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(Result.Stack.size(), 1u);
  EXPECT_EQ(Result.Stack.back().getZExtValue(), 7u);
}

TEST(EVMInterpreter, BlockhashRejectsCurrentFutureAndExpiredBlocks) {
  constexpr uint64_t kCurrentBlock = 1000;
  constexpr uint64_t kOldestAvailableBlock =
      kCurrentBlock - kBlockHashHistoryWindow;
  constexpr uint64_t kExpiredBlock = kOldestAvailableBlock - 1;

  const struct {
    uint64_t Number;
    uint64_t Expected;
  } Cases[] = {
      {kCurrentBlock - 1, 0x11}, {kOldestAvailableBlock, 0x22},
      {kExpiredBlock, 0},        {kCurrentBlock, 0},
      {kCurrentBlock + 1, 0},
  };

  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Number);
    const std::vector<uint8_t> Code = {
        opcodeByte(Opcode::PUSH2), static_cast<uint8_t>(Case.Number >> 8),
        static_cast<uint8_t>(Case.Number), opcodeByte(Opcode::BLOCKHASH),
        opcodeByte(Opcode::STOP)};
    ExecutionEnvironment Environment;
    Environment.BlockNumber = llvm::APInt(kWordBits, kCurrentBlock);
    Environment.BlockHashes.emplace(llvm::APInt(kWordBits, kCurrentBlock - 1),
                                    llvm::APInt(kWordBits, 0x11));
    Environment.BlockHashes.emplace(
        llvm::APInt(kWordBits, kOldestAvailableBlock),
        llvm::APInt(kWordBits, 0x22));
    Environment.BlockHashes.emplace(llvm::APInt(kWordBits, kExpiredBlock),
                                    llvm::APInt(kWordBits, 0x33));
    Environment.BlockHashes.emplace(llvm::APInt(kWordBits, kCurrentBlock),
                                    llvm::APInt(kWordBits, 0x44));
    Environment.BlockHashes.emplace(llvm::APInt(kWordBits, kCurrentBlock + 1),
                                    llvm::APInt(kWordBits, 0x55));

    auto Result = executeCode(Code, std::move(Environment));
    ASSERT_EQ(Result.Status, ExecutionStatus::Stopped);
    ASSERT_EQ(Result.Stack.size(), 1u);
    EXPECT_EQ(Result.Stack.back().getZExtValue(), Case.Expected);
  }
}

TEST(EVMInterpreter, SelectsDifficultyOrPrevRandaoAtTheParisBoundary) {
  AnalyzeOptions London;
  London.Fork = Hardfork::London;
  AnalyzeOptions Paris;
  Paris.Fork = Hardfork::Paris;

  ExecutionEnvironment Environment;
  Environment.Difficulty = llvm::APInt(kWordBits, 0xd1);
  Environment.PrevRandao = llvm::APInt(kWordBits, 0xa2);
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PREVRANDAO),
                                     opcodeByte(Opcode::STOP)};

  const ExecutionResult BeforeParis = executeCode(Code, Environment, London);
  ASSERT_EQ(BeforeParis.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(BeforeParis.Stack.size(), 1u);
  EXPECT_EQ(BeforeParis.Stack.back(), Environment.Difficulty);

  const ExecutionResult FromParis = executeCode(Code, Environment, Paris);
  ASSERT_EQ(FromParis.Status, ExecutionStatus::Stopped);
  ASSERT_EQ(FromParis.Stack.size(), 1u);
  EXPECT_EQ(FromParis.Stack.back(), Environment.PrevRandao);
}

TEST(EVMInterpreter, EveryAssignedOpcodeHasAStackSafeDispatchPath) {
  for (size_t Byte = 0; Byte < kOpcodeSpaceSize; ++Byte) {
    const auto Info =
        opcodeInfo(static_cast<uint8_t>(Byte), Hardfork::Amsterdam);
    if (!Info)
      continue;
    SCOPED_TRACE(testing::Message()
                 << Info->Name.str() << " (0x" << std::hex << Byte << ")");

    std::vector<uint8_t> EncodedInstruction{static_cast<uint8_t>(Byte)};
    EncodedInstruction.insert(EncodedInstruction.end(), Info->ImmediateBytes,
                              uint8_t{0});
    DecodeOptions Decode;
    Decode.Fork = Hardfork::Amsterdam;
    Decode.Strict = false;
    auto Decoded = decodeBytecode(EncodedInstruction, Decode);
    ASSERT_TRUE(static_cast<bool>(Decoded))
        << llvm::toString(Decoded.takeError());
    ASSERT_FALSE(Decoded->Instructions.empty());

    std::vector<uint8_t> Code(
        Decoded->Instructions.front().requiredStackHeight(),
        opcodeByte(Opcode::PUSH0));
    Code.insert(Code.end(), EncodedInstruction.begin(),
                EncodedInstruction.end());
    Code.push_back(opcodeByte(Opcode::STOP));

    AnalyzeOptions Options;
    Options.Fork = Hardfork::Amsterdam;
    Options.Strict = false;
    auto Program = analyze(Code, Options);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Result = execute(Program->Low);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Error.find("stack underflow"), std::string::npos);
    EXPECT_EQ(Result->Error.find("semantics are not implemented"),
              std::string::npos);
  }
}

TEST(EVMInterpreter, RuntimeFaultsAreExplicit) {
  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto Low = decodeLowIR(std::vector<uint8_t>{0x50, 0x00}, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Low)) << llvm::toString(Low.takeError());
  auto Result = execute(*Low);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_NE(Result->Error.find("stack underflow"), std::string::npos);
}

} // namespace
} // namespace neverd::evm
