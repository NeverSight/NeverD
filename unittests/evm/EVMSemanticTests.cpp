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

TEST(EVMInterpreter, MatchesGoEthereumEIP8024ExecutionVectors) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;

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
  EXPECT_TRUE(Faulted->Memory.empty());

  InterpreterOptions OneWord;
  OneWord.MaxMemoryBytes = kWordBytes;
  auto Stored = execute(Program->Low, {}, OneWord);
  ASSERT_TRUE(static_cast<bool>(Stored)) << llvm::toString(Stored.takeError());
  EXPECT_EQ(Stored->Status, ExecutionStatus::Stopped);
  EXPECT_EQ(Stored->Memory.size(), kWordBytes);
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
