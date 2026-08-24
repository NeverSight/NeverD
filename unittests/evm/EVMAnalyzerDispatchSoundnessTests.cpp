//===- EVMAnalyzerDispatchSoundnessTests.cpp - Dispatcher proofs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::evm {
namespace {

constexpr uint32_t kEquivalentSelector = 0x11223344u;
constexpr uint32_t kAlternateSelector = 0xaabbccddu;

void appendSelectorWord(std::vector<uint8_t> &Code) {
  Code.insert(Code.end(),
              {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
               opcodeByte(Opcode::PUSH1), kWordBits - kSelectorBits,
               opcodeByte(Opcode::SHR)});
}

void appendSelectorEqualityTest(std::vector<uint8_t> &Code,
                                size_t &TargetImmediate,
                                uint32_t Selector = kEquivalentSelector) {
  appendSelectorWord(Code);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::PUSH4), static_cast<uint8_t>(Selector >> 24),
               static_cast<uint8_t>(Selector >> 16),
               static_cast<uint8_t>(Selector >> 8),
               static_cast<uint8_t>(Selector), opcodeByte(Opcode::EQ),
               opcodeByte(Opcode::PUSH1), 0});
  TargetImmediate = Code.size() - 1;
  Code.push_back(opcodeByte(Opcode::JUMPI));
}

void appendSelectorXorTest(std::vector<uint8_t> &Code,
                           size_t &TargetImmediate) {
  appendSelectorWord(Code);
  Code.insert(Code.end(), {opcodeByte(Opcode::PUSH4),
                           static_cast<uint8_t>(kEquivalentSelector >> 24),
                           static_cast<uint8_t>(kEquivalentSelector >> 16),
                           static_cast<uint8_t>(kEquivalentSelector >> 8),
                           static_cast<uint8_t>(kEquivalentSelector),
                           opcodeByte(Opcode::XOR), opcodeByte(Opcode::ISZERO),
                           opcodeByte(Opcode::PUSH1), 0});
  TargetImmediate = Code.size() - 1;
  Code.push_back(opcodeByte(Opcode::JUMPI));
}

void setPush1Target(std::vector<uint8_t> &Code, size_t Immediate,
                    size_t Target) {
  ASSERT_LT(Immediate, Code.size());
  ASSERT_LE(Target, static_cast<size_t>(kByteMax));
  Code[Immediate] = static_cast<uint8_t>(Target);
}

TEST(EVMAnalyzerDispatchSoundness, SelectorXorMatchDoesNotInventAFallback) {
  // 5f3560e01c631122334418156012575f5ffd5b00
  std::vector<uint8_t> Code;
  size_t FunctionTarget = 0;
  appendSelectorXorTest(Code, FunctionTarget);
  Code.insert(Code.end(), {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                           opcodeByte(Opcode::REVERT)});
  const size_t FunctionEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  setPush1Target(Code, FunctionTarget, FunctionEntry);

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kEquivalentSelector);
  EXPECT_FALSE(Program->High.HasFallback);
}

TEST(EVMAnalyzerDispatchSoundness,
     SelectorXorMatchRetainsCanonicalDispatcherRecall) {
  std::vector<uint8_t> Code;
  size_t FunctionTarget = 0;
  appendSelectorXorTest(Code, FunctionTarget);
  Code.push_back(opcodeByte(Opcode::STOP));
  const size_t FunctionEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  setPush1Target(Code, FunctionTarget, FunctionEntry);

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kEquivalentSelector);
  EXPECT_TRUE(Program->High.HasFallback);
}

TEST(EVMAnalyzerDispatchSoundness,
     RawSelectorXorFalseEdgeRetainsCanonicalDispatcherRecall) {
  // 5f3560e01c631122334418600f57005b5f5ffd
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kEquivalentSelector >> 24),
      static_cast<uint8_t>(kEquivalentSelector >> 16),
      static_cast<uint8_t>(kEquivalentSelector >> 8),
      static_cast<uint8_t>(kEquivalentSelector),
      opcodeByte(Opcode::XOR),
      opcodeByte(Opcode::PUSH1),
      0x0f,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::REVERT),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kEquivalentSelector);
  EXPECT_EQ(Program->High.Functions.front().EntryPC, 0x0eu);
  EXPECT_FALSE(Program->High.HasFallback);
}

TEST(EVMAnalyzerDispatchSoundness,
     RawSelectorXorFalseJumpdestRetainsCanonicalDispatcherRecall) {
  // 5f3560e01c6311223344186010575b005b5f5ffd
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kEquivalentSelector >> 24),
      static_cast<uint8_t>(kEquivalentSelector >> 16),
      static_cast<uint8_t>(kEquivalentSelector >> 8),
      static_cast<uint8_t>(kEquivalentSelector),
      opcodeByte(Opcode::XOR),
      opcodeByte(Opcode::PUSH1),
      0x10,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::REVERT),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kEquivalentSelector);
  EXPECT_EQ(Program->High.Functions.front().EntryPC, 0x0eu);
  EXPECT_FALSE(Program->High.HasFallback);
}

TEST(EVMAnalyzerDispatchSoundness,
     ZeroCallValueEqualityDoesNotInventAReceiveEntry) {
  // 36156008575f5ffd5b345f146012575f5ffd5b00
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        0x08,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::CALLVALUE),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),        0x12,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.HasReceive);
}

TEST(EVMAnalyzerDispatchSoundness,
     NonZeroCallValueEqualityCanProveAReceiveEntry) {
  std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        0,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
  };
  const size_t ReceiveEntry = Code.size();
  setPush1Target(Code, 3, ReceiveEntry);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::CALLVALUE),
               opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::EQ),
               opcodeByte(Opcode::PUSH1), 0, opcodeByte(Opcode::JUMPI),
               opcodeByte(Opcode::STOP)});
  const size_t ZeroValueRejection = Code.size();
  setPush1Target(Code, ReceiveEntry + 5, ZeroValueRejection);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH0),
               opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.HasReceive);
}

TEST(EVMAnalyzerDispatchSoundness,
     UnknownConditionalCannotProveFallbackAcceptance) {
  constexpr uint8_t kAcceptedEntry = 0x08;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLER),
      opcodeByte(Opcode::PUSH1),
      kAcceptedEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::REVERT),
      opcodeByte(Opcode::INVALID),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.HasFallback);
}

TEST(EVMAnalyzerDispatchSoundness,
     ExternalOutcomeConditionalRetainsFallbackAcceptance) {
  std::vector<uint8_t> Code(6, opcodeByte(Opcode::PUSH0));
  Code.insert(Code.end(),
              {opcodeByte(Opcode::STATICCALL), opcodeByte(Opcode::PUSH1), 0,
               opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::PUSH0),
               opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});
  const size_t AcceptedEntry = Code.size();
  setPush1Target(Code, 8, AcceptedEntry);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.HasFallback);
}

TEST(EVMAnalyzerDispatchSoundness,
     InvertedExternalOutcomeRetainsFallbackAcceptance) {
  std::vector<uint8_t> Code(6, opcodeByte(Opcode::PUSH0));
  Code.insert(Code.end(),
              {opcodeByte(Opcode::STATICCALL), opcodeByte(Opcode::ISZERO),
               opcodeByte(Opcode::PUSH1), 0, opcodeByte(Opcode::JUMPI),
               opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
               opcodeByte(Opcode::REVERT)});
  const size_t AcceptedEntry = Code.size();
  setPush1Target(Code, 9, AcceptedEntry);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.HasFallback);
}

TEST(EVMAnalyzerDispatchSoundness,
     UnknownConditionalDoesNotEnterAPossibleMatchedBody) {
  std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLER), opcodeByte(Opcode::PUSH1), 0,
      opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::STOP)};
  const size_t PossibleBody = Code.size();
  setPush1Target(Code, 2, PossibleBody);
  Code.push_back(opcodeByte(Opcode::JUMPDEST));
  size_t FunctionTarget = 0;
  appendSelectorXorTest(Code, FunctionTarget);
  Code.push_back(opcodeByte(Opcode::STOP));
  const size_t FunctionEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  setPush1Target(Code, FunctionTarget, FunctionEntry);

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Functions.empty());
}

TEST(EVMAnalyzerDispatchSoundness,
     SelectorZeroPathCannotInventANonzeroFunction) {
  // 5f3560e01c601a575b5f3560e01c631122334414601857005b005b00
  std::vector<uint8_t> Code;
  appendSelectorWord(Code);
  Code.push_back(opcodeByte(Opcode::PUSH1));
  const size_t NonZeroTargetImmediate = Code.size();
  Code.insert(Code.end(),
              {0, opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::JUMPDEST)});
  size_t ImpossibleFunctionTarget = 0;
  appendSelectorEqualityTest(Code, ImpossibleFunctionTarget);
  Code.push_back(opcodeByte(Opcode::STOP));
  const size_t ImpossibleFunctionEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  const size_t NonZeroEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  setPush1Target(Code, NonZeroTargetImmediate, NonZeroEntry);
  setPush1Target(Code, ImpossibleFunctionTarget, ImpossibleFunctionEntry);

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Functions.empty());
}

TEST(EVMAnalyzerDispatchSoundness,
     RepeatedExcludedSelectorCannotEraseAReachableFunction) {
  // 5f3560e01c631122334414601d575f3560e01c631122334414601f57005b005b00
  std::vector<uint8_t> Code;
  size_t ReachableFunctionTarget = 0;
  appendSelectorEqualityTest(Code, ReachableFunctionTarget);
  size_t ImpossibleDuplicateTarget = 0;
  appendSelectorEqualityTest(Code, ImpossibleDuplicateTarget);
  Code.push_back(opcodeByte(Opcode::STOP));
  const size_t ReachableFunctionEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  const size_t ImpossibleDuplicateEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  setPush1Target(Code, ReachableFunctionTarget, ReachableFunctionEntry);
  setPush1Target(Code, ImpossibleDuplicateTarget, ImpossibleDuplicateEntry);

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kEquivalentSelector);
  EXPECT_EQ(Program->High.Functions.front().EntryPC, ReachableFunctionEntry);
}

TEST(EVMAnalyzerDispatchSoundness,
     DispatchCandidateBudgetChargesAtTheExactBoundary) {
  std::vector<uint8_t> Code;
  size_t FirstTargetImmediate = 0;
  appendSelectorEqualityTest(Code, FirstTargetImmediate);
  size_t SecondTargetImmediate = 0;
  appendSelectorEqualityTest(Code, SecondTargetImmediate, kAlternateSelector);
  Code.insert(Code.end(), {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                           opcodeByte(Opcode::REVERT)});
  const size_t FirstEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  const size_t SecondEntry = Code.size();
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
  setPush1Target(Code, FirstTargetImmediate, FirstEntry);
  setPush1Target(Code, SecondTargetImmediate, SecondEntry);

  AnalyzeOptions AtBoundary;
  AtBoundary.MaxHighDispatchCandidates = 2;
  auto Accepted = analyze(Code, AtBoundary);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->High.Functions.size(), 2u);

  AnalyzeOptions BelowBoundary;
  BelowBoundary.MaxHighDispatchCandidates = 1;
  auto Rejected = analyze(Code, BelowBoundary);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError())
                .find(kMaxHighDispatchCandidatesName.str()),
            std::string::npos);
}

TEST(EVMAnalyzerDispatchSoundness,
     CalldataLengthGuardRetainsCanonicalDispatcherRecall) {
  for (Opcode Comparison : {Opcode::LT, Opcode::GT}) {
    SCOPED_TRACE(opcodeName(Comparison).str());
    std::vector<uint8_t> Code;
    if (Comparison == Opcode::LT)
      Code.insert(Code.end(), {opcodeByte(Opcode::PUSH1),
                               static_cast<uint8_t>(kSelectorBytes),
                               opcodeByte(Opcode::CALLDATASIZE)});
    else
      Code.insert(Code.end(),
                  {opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::PUSH1),
                   static_cast<uint8_t>(kSelectorBytes)});
    Code.insert(Code.end(), {opcodeByte(Comparison),
                             opcodeByte(Opcode::PUSH1),
                             0,
                             opcodeByte(Opcode::JUMPI),
                             opcodeByte(Opcode::PUSH0),
                             opcodeByte(Opcode::CALLDATALOAD),
                             opcodeByte(Opcode::PUSH1),
                             kWordBits - kSelectorBits,
                             opcodeByte(Opcode::SHR),
                             opcodeByte(Opcode::PUSH4),
                             static_cast<uint8_t>(kEquivalentSelector >> 24),
                             static_cast<uint8_t>(kEquivalentSelector >> 16),
                             static_cast<uint8_t>(kEquivalentSelector >> 8),
                             static_cast<uint8_t>(kEquivalentSelector),
                             opcodeByte(Opcode::EQ),
                             opcodeByte(Opcode::PUSH1),
                             0,
                             opcodeByte(Opcode::JUMPI),
                             opcodeByte(Opcode::PUSH0),
                             opcodeByte(Opcode::PUSH0),
                             opcodeByte(Opcode::REVERT)});
    const size_t ShortCalldataEntry = Code.size();
    setPush1Target(Code, 5, ShortCalldataEntry);
    Code.insert(Code.end(),
                {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});
    const size_t FunctionEntry = Code.size();
    setPush1Target(Code, 19, FunctionEntry);
    Code.insert(Code.end(),
                {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::STOP)});

    auto Program = analyze(Code);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->High.Functions.size(), 1u);
    EXPECT_EQ(Program->High.Functions.front().Selector, kEquivalentSelector);
  }
}

} // namespace
} // namespace neverd::evm
