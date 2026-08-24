//===- EVMAnalyzerABISoundnessTests.cpp - ABI soundness tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/evm/runtime/EVMKeccak.h"
#include "EVMAnalyzerTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

namespace neverd::evm {
namespace {

using test::dispatcherFor;

const KnownSignatureInfo *knownSignature(llvm::StringRef Signature) {
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Signature == Signature)
      return &Info;
  return nullptr;
}

const KnownSignatureInfo *knownEvent(llvm::StringRef Signature,
                                     KnownStandard Standard) {
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Signature == Signature && Info.Event &&
        Info.Event->Standard == Standard)
      return &Info;
  return nullptr;
}

std::vector<uint8_t> pushWord(const llvm::APInt &Value) {
  std::vector<uint8_t> Code{opcodeByte(Opcode::PUSH32)};
  for (unsigned I = kWordBytes; I-- > 0;)
    Code.push_back(static_cast<uint8_t>(
        Value.extractBitsAsZExtValue(kBitsPerByte, I * kBitsPerByte)));
  return Code;
}

void append(std::vector<uint8_t> &Code, std::vector<uint8_t> Tail) {
  Code.insert(Code.end(), Tail.begin(), Tail.end());
}

std::vector<uint8_t> twoSelectorDispatcher(uint32_t FirstSelector,
                                           uint32_t SecondSelector) {
  std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      static_cast<uint8_t>(kWordBits - kSelectorBits),
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(FirstSelector >> 24),
      static_cast<uint8_t>(FirstSelector >> 16),
      static_cast<uint8_t>(FirstSelector >> 8),
      static_cast<uint8_t>(FirstSelector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
  };
  const size_t FirstDestinationIndex = Code.size();
  Code.push_back(0);
  Code.insert(Code.end(), {opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::DUP1),
                           opcodeByte(Opcode::PUSH4),
                           static_cast<uint8_t>(SecondSelector >> 24),
                           static_cast<uint8_t>(SecondSelector >> 16),
                           static_cast<uint8_t>(SecondSelector >> 8),
                           static_cast<uint8_t>(SecondSelector),
                           opcodeByte(Opcode::EQ), opcodeByte(Opcode::PUSH1)});
  const size_t SecondDestinationIndex = Code.size();
  Code.push_back(0);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::STOP)});

  const auto AppendCompatibleBody = [&] {
    Code.insert(Code.end(),
                {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH1),
                 kSelectorBytes, opcodeByte(Opcode::CALLDATALOAD),
                 opcodeByte(Opcode::BALANCE), opcodeByte(Opcode::POP),
                 opcodeByte(Opcode::STOP)});
  };
  EXPECT_LE(Code.size(), kByteMax);
  Code[FirstDestinationIndex] = static_cast<uint8_t>(Code.size());
  AppendCompatibleBody();
  EXPECT_LE(Code.size(), kByteMax);
  Code[SecondDestinationIndex] = static_cast<uint8_t>(Code.size());
  AppendCompatibleBody();
  return Code;
}

std::vector<uint8_t> pushSelectorPayload(uint32_t Selector) {
  return pushWord(
      llvm::APInt(kWordBits, Selector).shl(kWordBits - kSelectorBits));
}

TEST(EVMAnalyzer, SelectorCollisionDoesNotOverrideContradictoryABIShape) {
  const KnownSignatureInfo *Transfer =
      knownSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);
  constexpr llvm::StringLiteral CollidingSignature = "many_msg_babbage(bytes1)";
  ASSERT_EQ(keccak256Selector(CollidingSignature), Transfer->Selector);

  // The body reads one head word and keeps only its leading byte, matching the
  // colliding bytes1 preimage rather than transfer(address,uint256).
  const std::vector<uint8_t> Body = {
      opcodeByte(Opcode::PUSH1),
      kSelectorBytes,
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      static_cast<uint8_t>(kWordBits - kBitsPerByte),
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(dispatcherFor(Transfer->Selector, Body));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  const RecoveredFunction &Function = Program->High.Functions.front();
  EXPECT_EQ(Function.Selector, Transfer->Selector);
  EXPECT_EQ(Function.Known, nullptr);
  EXPECT_EQ(Function.Name, kRecoveredFunctionPrefix.str() +
                               llvm::utohexstr(Transfer->Selector));
  ASSERT_EQ(Function.Arguments.size(), 1u);
  EXPECT_EQ(Function.Arguments.front().Type, "bytes32");
  EXPECT_EQ(Function.Arguments.front().TypeSource, ABITypeSource::Dataflow);
  EXPECT_TRUE(Function.Returns.empty());
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::Default);
  EXPECT_TRUE(llvm::any_of(Program->High.Diagnostics, [](const Diagnostic &D) {
    return llvm::StringRef(D.Message).starts_with(
        kIncompatibleKnownFunctionPrefix);
  }));
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMAnalyzer, SelectorCollisionDoesNotHideAnAdditionalHeadSlot) {
  const KnownSignatureInfo *Transfer =
      knownSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  constexpr size_t ExtraArgumentIndex = 2;
  constexpr size_t ExtraArgumentOffset =
      kSelectorBytes + ExtraArgumentIndex * kWordBytes;
  static_assert(ExtraArgumentOffset <= kByteMax);
  const std::vector<uint8_t> Body = {
      opcodeByte(Opcode::PUSH1),
      static_cast<uint8_t>(ExtraArgumentOffset),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::STOP),
  };
  auto Program = analyze(dispatcherFor(Transfer->Selector, Body));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Known, nullptr);
  EXPECT_EQ(Program->High.Functions.front().Arguments.size(),
            ExtraArgumentIndex + 1);
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMAnalyzer, OneFourByteSelectorDoesNotProveAWholeStandard) {
  const KnownSignatureInfo *Transfer =
      knownSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  auto Program =
      analyze(dispatcherFor(Transfer->Selector, {opcodeByte(Opcode::STOP)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Known, Transfer);
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMAnalyzer, OneFourByteErrorDoesNotProveAWholeStandard) {
  const KnownSignatureInfo *Unauthorized =
      knownSignature("OwnableUnauthorizedAccount(address)");
  ASSERT_NE(Unauthorized, nullptr);

  std::vector<uint8_t> Code = pushSelectorPayload(Unauthorized->Selector);
  Code.insert(Code.end(),
              {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
               opcodeByte(Opcode::PUSH1),
               static_cast<uint8_t>(kSelectorBytes + kWordBytes),
               opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_EQ(Program->High.Errors.front().Known, Unauthorized);
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMAnalyzer, TwoCompatibleSelectorsCanEstablishAStandard) {
  const KnownSignatureInfo *Transfer =
      knownSignature("transfer(address,uint256)");
  const KnownSignatureInfo *Allowance =
      knownSignature("allowance(address,address)");
  ASSERT_NE(Transfer, nullptr);
  ASSERT_NE(Allowance, nullptr);
  const auto IsIndependentERC20 = [](const KnownSignatureInfo &Function) {
    return llvm::any_of(
        knownFunctionVariants(Function),
        [](const KnownFunctionVariantInfo *Variant) {
          return Variant->Standard == KnownStandard::ERC20 &&
                 Variant->contributesIndependentSelectorEvidence();
        });
  };
  ASSERT_TRUE(IsIndependentERC20(*Transfer));
  ASSERT_TRUE(IsIndependentERC20(*Allowance));
  ASSERT_NE(Transfer->Selector, Allowance->Selector);

  auto Program =
      analyze(twoSelectorDispatcher(Transfer->Selector, Allowance->Selector));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 2u);
  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC20);
}

TEST(EVMAnalyzer, SharedSelectorsDoNotEstablishEitherDeclaringStandard) {
  const KnownSignatureInfo *BalanceOf = knownSignature("balanceOf(address)");
  const KnownSignatureInfo *Approve =
      knownSignature("approve(address,uint256)");
  ASSERT_NE(BalanceOf, nullptr);
  ASSERT_NE(Approve, nullptr);

  auto Program =
      analyze(twoSelectorDispatcher(BalanceOf->Selector, Approve->Selector));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 2u);
  EXPECT_TRUE(Program->High.Standards.empty());
  for (const RecoveredFunction &Function : Program->High.Functions) {
    EXPECT_EQ(Function.KnownVariant, nullptr);
    EXPECT_TRUE(Function.Returns.empty());
  }
}

TEST(EVMAnalyzer, RecognizedERC721SelectsTheVoidApproveVariant) {
  const KnownSignatureInfo *Approve =
      knownSignature("approve(address,uint256)");
  const KnownSignatureInfo *Transfer =
      knownEvent("Transfer(address,address,uint256)", KnownStandard::ERC721);
  ASSERT_NE(Approve, nullptr);
  ASSERT_NE(Transfer, nullptr);

  std::vector<uint8_t> Body = {opcodeByte(Opcode::PUSH0),
                               opcodeByte(Opcode::PUSH0),
                               opcodeByte(Opcode::PUSH0)};
  append(Body, pushWord(Transfer->Topic));
  append(Body, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::LOG4), opcodeByte(Opcode::STOP)});

  auto Program = analyze(dispatcherFor(Approve->Selector, std::move(Body)));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC721);
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  const RecoveredFunction &Function = Program->High.Functions.front();
  ASSERT_NE(Function.KnownVariant, nullptr);
  EXPECT_EQ(Function.KnownVariant->Standard, KnownStandard::ERC721);
  EXPECT_TRUE(Function.Returns.empty());
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::KnownSignature);
}

TEST(EVMAnalyzer, RecognizedERC20SelectsTheBooleanApproveVariant) {
  const KnownSignatureInfo *Approve =
      knownSignature("approve(address,uint256)");
  const KnownSignatureInfo *Transfer =
      knownEvent("Transfer(address,address,uint256)", KnownStandard::ERC20);
  ASSERT_NE(Approve, nullptr);
  ASSERT_NE(Transfer, nullptr);

  std::vector<uint8_t> Body = {opcodeByte(Opcode::PUSH0),
                               opcodeByte(Opcode::PUSH0)};
  append(Body, pushWord(Transfer->Topic));
  append(Body, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::LOG3), opcodeByte(Opcode::PUSH1),
                static_cast<uint8_t>(kWordBytes), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::RETURN)});

  auto Program = analyze(dispatcherFor(Approve->Selector, std::move(Body)));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC20);
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  const RecoveredFunction &Function = Program->High.Functions.front();
  ASSERT_NE(Function.KnownVariant, nullptr);
  EXPECT_EQ(Function.KnownVariant->Standard, KnownStandard::ERC20);
  ASSERT_EQ(Function.Returns.size(), 1u);
  EXPECT_EQ(Function.Returns.front(), "bool");
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::KnownSignature);
}

TEST(EVMAnalyzer, UnresolvedFunctionPathPreventsAWholeFunctionReturnClaim) {
  const KnownSignatureInfo *Transfer =
      knownSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  const std::vector<uint8_t> Body = {
      opcodeByte(Opcode::CALLVALUE),    opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD), opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH1),        static_cast<uint8_t>(kWordBytes),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::RETURN),
  };
  auto Program = analyze(dispatcherFor(Transfer->Selector, Body));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  const RecoveredFunction &Function = Program->High.Functions.front();
  EXPECT_EQ(Function.Known, Transfer);
  EXPECT_TRUE(Function.Returns.empty());
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::Default);
}

TEST(EVMAnalyzer, NaturalFalloffParticipatesInReturnShapeAgreement) {
  const KnownSignatureInfo *Transfer =
      knownSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  constexpr uint8_t kImplicitStopPC = test::kTestFunctionEntry + 9;
  const std::vector<uint8_t> Body = {
      opcodeByte(Opcode::CALLVALUE),
      opcodeByte(Opcode::PUSH1),
      kImplicitStopPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH1),
      static_cast<uint8_t>(kWordBytes),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::RETURN),
      opcodeByte(Opcode::JUMPDEST),
  };
  auto Program = analyze(dispatcherFor(Transfer->Selector, Body));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  const RecoveredFunction &Function = Program->High.Functions.front();
  EXPECT_EQ(Function.Known, Transfer);
  EXPECT_TRUE(Function.Returns.empty());
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::Default);
}

TEST(EVMAnalyzer, DefiniteInvalidEndJumpIsNotASuccessfulReturn) {
  const KnownSignatureInfo *Transfer =
      knownSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  const auto AnalyzeCondition = [&](Opcode ConditionOpcode) {
    return analyze(
        dispatcherFor(Transfer->Selector,
                      {opcodeByte(ConditionOpcode), opcodeByte(Opcode::PUSH1),
                       0xff, opcodeByte(Opcode::JUMPI)}),
        Relaxed);
  };
  const auto HasReturnContradiction = [](const EVMProgram &Program) {
    return llvm::any_of(Program.High.Diagnostics, [](const Diagnostic &Entry) {
      return llvm::StringRef(Entry.Message)
          .starts_with(kIncompatibleKnownReturnPrefix);
    });
  };

  auto AlwaysTrue = analyze(
      dispatcherFor(Transfer->Selector,
                    {opcodeByte(Opcode::PUSH1), 1, opcodeByte(Opcode::PUSH1),
                     0xff, opcodeByte(Opcode::JUMPI)}),
      Relaxed);
  ASSERT_TRUE(static_cast<bool>(AlwaysTrue))
      << llvm::toString(AlwaysTrue.takeError());
  EXPECT_FALSE(HasReturnContradiction(*AlwaysTrue));

  auto AlwaysFalse = AnalyzeCondition(Opcode::PUSH0);
  ASSERT_TRUE(static_cast<bool>(AlwaysFalse))
      << llvm::toString(AlwaysFalse.takeError());
  EXPECT_TRUE(HasReturnContradiction(*AlwaysFalse));

  auto Unknown = AnalyzeCondition(Opcode::CALLVALUE);
  ASSERT_TRUE(static_cast<bool>(Unknown))
      << llvm::toString(Unknown.takeError());
  EXPECT_TRUE(HasReturnContradiction(*Unknown));
}

} // namespace
} // namespace neverd::evm
