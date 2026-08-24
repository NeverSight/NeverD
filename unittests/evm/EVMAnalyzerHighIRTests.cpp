//===- EVMAnalyzerHighIRTests.cpp - EVM high-level recovery tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMAnalyzerTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

namespace neverd::evm {
namespace {

using test::dispatcherFor;
using test::selectorDispatcher;

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

const KnownSignatureInfo *findSignature(llvm::StringRef Text,
                                        KnownStandard Standard) {
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Signature == Text) {
      if (Info.Event && Info.Event->Standard == Standard)
        return &Info;
      if (Info.Error && Info.Error->Standard == Standard)
        return &Info;
      if (llvm::any_of(knownFunctionVariants(Info),
                       [&](const KnownFunctionVariantInfo *Variant) {
                         return Variant->Standard == Standard;
                       }))
        return &Info;
    }
  return nullptr;
}

void append(std::vector<uint8_t> &Code, std::vector<uint8_t> Tail) {
  Code.insert(Code.end(), Tail.begin(), Tail.end());
}

std::vector<uint8_t> twoSelectorDispatcher() {
  constexpr uint32_t kFirstSelector = 0x12345678;
  constexpr uint32_t kSecondSelector = 0x87654321;
  constexpr uint8_t kFirstEntry = 0x1a;
  constexpr uint8_t kSecondEntry = 0x1c;
  return {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kFirstSelector >> 24),
      static_cast<uint8_t>(kFirstSelector >> 16),
      static_cast<uint8_t>(kFirstSelector >> 8),
      static_cast<uint8_t>(kFirstSelector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kFirstEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kSecondSelector >> 24),
      static_cast<uint8_t>(kSecondSelector >> 16),
      static_cast<uint8_t>(kSecondSelector >> 8),
      static_cast<uint8_t>(kSecondSelector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kSecondEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };
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

  // With no contradictory dataflow, the dictionary candidate supplies its
  // argument list even though the body reads nothing.
  ASSERT_EQ(Function.Arguments.size(), 2u);
  EXPECT_EQ(Function.Arguments[0].Type, "address");
  EXPECT_EQ(Function.Arguments[1].Type, "uint256");
  EXPECT_EQ(Function.Arguments[0].TypeSource, ABITypeSource::KnownSignature);
  EXPECT_FALSE(Function.Arguments[0].Read);
  // Return types are not part of a selector. This body stops without return
  // data, so the dictionary's ERC-20 declaration must not invent `bool`.
  EXPECT_TRUE(Function.Returns.empty());
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::Default);

  EXPECT_TRUE(Program->High.Standards.empty());
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

TEST(EVMAnalyzer, DoesNotDiscoverSelectorTestsInsideAMatchedFunctionBody) {
  constexpr uint32_t kOuterSelector = 0x12345678u;
  constexpr uint32_t kMutuallyExclusiveInnerSelector = 0x87654321u;
  constexpr uint8_t kInnerEntry = test::kTestFunctionEntry + 16;
  const std::vector<uint8_t> Body = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kMutuallyExclusiveInnerSelector >> 24),
      static_cast<uint8_t>(kMutuallyExclusiveInnerSelector >> 16),
      static_cast<uint8_t>(kMutuallyExclusiveInnerSelector >> 8),
      static_cast<uint8_t>(kMutuallyExclusiveInnerSelector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kInnerEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(dispatcherFor(kOuterSelector, Body));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions.front().Selector, kOuterSelector);
}

TEST(EVMAnalyzer, NamesAnEventThatHashesToATabulatedTopic) {
  const KnownSignatureInfo *Transfer =
      findSignature("Transfer(address,address,uint256)", KnownStandard::ERC20);
  ASSERT_NE(Transfer, nullptr);

  std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                               opcodeByte(Opcode::PUSH0)};
  append(Code, pushWord(Transfer->Topic));
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::LOG3), opcodeByte(Opcode::STOP)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Events.size(), 1u);
  ASSERT_TRUE(Transfer->Event.has_value());
  EXPECT_EQ(Program->High.Events.front().Topics,
            Transfer->Event->totalTopicCount());
  EXPECT_EQ(Program->High.Events.front().Known, Transfer);
  EXPECT_EQ(Program->High.Events.front().SuggestedName, "Transfer");
  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC20);
}

TEST(EVMAnalyzer, DistinguishesERC721TransferByItsIndexedTopicArity) {
  const KnownSignatureInfo *Transfer =
      findSignature("Transfer(address,address,uint256)", KnownStandard::ERC721);
  ASSERT_NE(Transfer, nullptr);

  std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                               opcodeByte(Opcode::PUSH0),
                               opcodeByte(Opcode::PUSH0)};
  append(Code, pushWord(Transfer->Topic));
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::LOG4), opcodeByte(Opcode::STOP)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Events.size(), 1u);
  ASSERT_TRUE(Transfer->Event.has_value());
  EXPECT_EQ(Program->High.Events.front().Topics,
            Transfer->Event->totalTopicCount());
  EXPECT_EQ(Program->High.Events.front().Known, Transfer);
  EXPECT_EQ(Program->High.Events.front().SuggestedName, "Transfer");
  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC721);
}

TEST(EVMAnalyzer, LeavesObservableSharedEventVariantsAmbiguous) {
  const KnownSignatureInfo *ERC721Approval = findSignature(
      "ApprovalForAll(address,address,bool)", KnownStandard::ERC721);
  const KnownSignatureInfo *ERC1155Approval = findSignature(
      "ApprovalForAll(address,address,bool)", KnownStandard::ERC1155);
  ASSERT_NE(ERC721Approval, nullptr);
  ASSERT_NE(ERC1155Approval, nullptr);
  EXPECT_EQ(ERC721Approval->Topic, ERC1155Approval->Topic);

  std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                               opcodeByte(Opcode::PUSH0)};
  append(Code, pushWord(ERC721Approval->Topic));
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::LOG3), opcodeByte(Opcode::STOP)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Events.size(), 1u);
  const EventFact &Event = Program->High.Events.front();
  ASSERT_TRUE(Event.Topic0.has_value());
  EXPECT_EQ(*Event.Topic0, ERC721Approval->Topic);
  EXPECT_EQ(Event.Known, nullptr);
  EXPECT_EQ(Event.SuggestedName,
            kRecoveredEventPrefix.str() + llvm::utohexstr(Event.PC));
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMAnalyzer, RejectsKnownEventWithWrongIndexedTopicArity) {
  const KnownSignatureInfo *TransferSingle =
      findSignature("TransferSingle(address,address,address,uint256,uint256)");
  ASSERT_NE(TransferSingle, nullptr);

  const std::vector<uint8_t> Code = {
      0x60, 0x01, 0x7f, 0xc3, 0xd5, 0x81, 0x68, 0xc5, 0xae, 0x73,
      0x97, 0x73, 0x1d, 0x06, 0x3d, 0x5b, 0xbf, 0x3d, 0x65, 0x78,
      0x54, 0x42, 0x73, 0x43, 0xf4, 0xc0, 0x83, 0x24, 0x0f, 0x7a,
      0xac, 0xaa, 0x2d, 0x0f, 0x62, 0x5f, 0x5f, 0xa2, 0x00,
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Events.size(), 1u);
  const EventFact &Event = Program->High.Events.front();
  ASSERT_TRUE(Event.Topic0.has_value());
  EXPECT_EQ(*Event.Topic0, TransferSingle->Topic);
  EXPECT_EQ(Event.Known, nullptr);
  EXPECT_EQ(Event.SuggestedName,
            kRecoveredEventPrefix.str() + llvm::utohexstr(Event.PC));
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMAnalyzer, RecognizesThePublishedERC1155URIWithLOG2) {
  const KnownSignatureInfo *URI = findSignature("URI(string,uint256)");
  ASSERT_NE(URI, nullptr);

  const std::vector<uint8_t> Code = {
      0x60, 0x01, 0x7f, 0x6b, 0xb7, 0xff, 0x70, 0x86, 0x19, 0xba,
      0x06, 0x10, 0xcb, 0xa2, 0x95, 0xa5, 0x85, 0x92, 0xe0, 0x45,
      0x1d, 0xee, 0x26, 0x22, 0x93, 0x8c, 0x87, 0x55, 0x66, 0x76,
      0x88, 0xda, 0xf3, 0x52, 0x9b, 0x5f, 0x5f, 0xa2, 0x00,
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Events.size(), 1u);
  const EventFact &Event = Program->High.Events.front();
  ASSERT_TRUE(Event.Topic0.has_value());
  EXPECT_EQ(*Event.Topic0, URI->Topic);
  EXPECT_EQ(Event.Known, URI);
  EXPECT_EQ(Event.SuggestedName, "URI");
  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC1155);
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

TEST(EVMAnalyzer, RejectsKnownErrorAfterPartialMemoryClobber) {
  const KnownSignatureInfo *Unauthorized =
      findSignature("OwnableUnauthorizedAccount(address)");
  ASSERT_NE(Unauthorized, nullptr);

  std::vector<uint8_t> Code = pushSelectorPayload(Unauthorized->Selector);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::MSTORE8), opcodeByte(Opcode::PUSH1),
                static_cast<uint8_t>(kSelectorBytes + kWordBytes),
                opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_EQ(Program->High.Errors.front().Known, nullptr);
  ASSERT_TRUE(Program->High.Errors.front().Selector.has_value());
  constexpr uint32_t kLowThreeBytesMask = 0x00ffffff;
  EXPECT_EQ(*Program->High.Errors.front().Selector,
            Unauthorized->Selector & kLowThreeBytesMask);
}

TEST(EVMAnalyzer, RecoversKnownErrorAcrossBasicBlock) {
  constexpr uint8_t kRevertBlock = 0x26;
  const KnownSignatureInfo *Unauthorized =
      findSignature("OwnableUnauthorizedAccount(address)");
  ASSERT_NE(Unauthorized, nullptr);

  std::vector<uint8_t> Code = pushSelectorPayload(Unauthorized->Selector);
  append(Code,
         {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
          opcodeByte(Opcode::PUSH1), kRevertBlock, opcodeByte(Opcode::JUMP),
          opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH1),
          static_cast<uint8_t>(kSelectorBytes + kWordBytes),
          opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_EQ(Program->High.Errors.front().Known, Unauthorized);
  EXPECT_EQ(Program->High.Errors.front().Selector, Unauthorized->Selector);
}

TEST(EVMAnalyzer, ConflictingPredecessorMemoryIsNotAProvenError) {
  constexpr uint32_t kSelector = 0x118cdaa7;
  constexpr uint8_t kClobberEntry = 0x2b;
  constexpr uint8_t kMergeEntry = 0x2f;
  std::vector<uint8_t> Code = pushSelectorPayload(kSelector);
  append(Code,
         {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
          opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
          opcodeByte(Opcode::PUSH1), kClobberEntry, opcodeByte(Opcode::JUMPI),
          opcodeByte(Opcode::PUSH1), kMergeEntry, opcodeByte(Opcode::JUMP),
          opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH0),
          opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE8),
          opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH1), 0x24,
          opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_FALSE(Program->High.Errors.front().Selector.has_value());
  EXPECT_EQ(Program->High.Errors.front().Known, nullptr);
}

TEST(EVMAnalyzer, AgreeingPredecessorMemoryRemainsAProvenError) {
  constexpr uint32_t kSelector = 0x118cdaa7;
  constexpr uint8_t kBranchEntry = 0x2b;
  constexpr uint8_t kMergeEntry = 0x2f;
  std::vector<uint8_t> Code = pushSelectorPayload(kSelector);
  append(Code,
         {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
          opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
          opcodeByte(Opcode::PUSH1), kBranchEntry, opcodeByte(Opcode::JUMPI),
          opcodeByte(Opcode::PUSH1), kMergeEntry, opcodeByte(Opcode::JUMP),
          opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH1), kMergeEntry,
          opcodeByte(Opcode::JUMP), opcodeByte(Opcode::JUMPDEST),
          opcodeByte(Opcode::PUSH1), 0x24, opcodeByte(Opcode::PUSH0),
          opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_EQ(Program->High.Errors.front().Selector, kSelector);
}

TEST(EVMAnalyzer, DynamicMemoryWriteInvalidatesRecoveredPayload) {
  constexpr uint32_t kSelector = 0x118cdaa7;
  std::vector<uint8_t> Code = pushSelectorPayload(kSelector);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::CALLDATALOAD), opcodeByte(Opcode::MSTORE8),
                opcodeByte(Opcode::PUSH1), 0x24, opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_FALSE(Program->High.Errors.front().Selector.has_value());
  EXPECT_EQ(Program->High.Errors.front().Known, nullptr);
}

TEST(EVMAnalyzer, UnmodelledMemoryWriteInvalidatesTrackedBytes) {
  constexpr uint32_t kSelector = 0x118cdaa7;
  std::vector<uint8_t> Code = pushSelectorPayload(kSelector);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                opcodeByte(Opcode::PUSH1), 1, opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::PUSH1), 0x40,
                opcodeByte(Opcode::CALLDATACOPY), opcodeByte(Opcode::PUSH1),
                0x24, opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_FALSE(Program->High.Errors.front().Selector.has_value());
  EXPECT_EQ(Program->High.Errors.front().Known, nullptr);
}

TEST(EVMAnalyzer, MemoryDataflowConvergesAcrossAStableLoop) {
  constexpr uint32_t kSelector = 0x118cdaa7;
  constexpr uint8_t kLoopEntry = 0x23;
  constexpr uint8_t kExitEntry = 0x2c;
  std::vector<uint8_t> Code = pushSelectorPayload(kSelector);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::CALLDATALOAD), opcodeByte(Opcode::PUSH1),
                kExitEntry, opcodeByte(Opcode::JUMPI),
                opcodeByte(Opcode::PUSH1), kLoopEntry, opcodeByte(Opcode::JUMP),
                opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH1), 0x24,
                opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  const ErrorFact &Error = Program->High.Errors.front();
  ASSERT_TRUE(Error.Selector.has_value());
  EXPECT_EQ(*Error.Selector, kSelector);
  ASSERT_NE(Error.Known, nullptr);
  EXPECT_TRUE(Program->High.Diagnostics.empty());
}

TEST(EVMAnalyzer, SelfLoopEntryPhiDoesNotSelectOneMemoryWriteValue) {
  constexpr uint32_t kSelector = 0x118cdaa7;
  std::vector<uint8_t> Code = pushSelectorPayload(kSelector);
  append(Code, {opcodeByte(Opcode::PUSH1), 0, opcodeByte(Opcode::JUMP)});
  const size_t InitialTargetByte = Code.size() - 2;
  const uint8_t LoopEntry = static_cast<uint8_t>(Code.size());
  Code[InitialTargetByte] = LoopEntry;
  append(Code, {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::DUP1),
                opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE)});
  append(Code,
         pushWord(llvm::APInt(kWordBits, 1).shl(kWordBits - kSelectorBits)));
  append(Code, {opcodeByte(Opcode::ADD), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::CALLDATALOAD), opcodeByte(Opcode::PUSH1),
                LoopEntry, opcodeByte(Opcode::JUMPI)});
  append(Code, {opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::POP),
                opcodeByte(Opcode::PUSH1), kSelectorBytes,
                opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  bool SawSelfLoopEntryPhi = false;
  for (const MedStateLane &Lane : Program->Med.StateLanes) {
    if (Lane.LowLane.BlockPC != LoopEntry)
      continue;
    for (ValueID Entry : Lane.EntryStack) {
      const MedValue *Value = Program->Med.findValue(Entry);
      if (!Value || Value->Kind != ValueKind::Phi)
        continue;
      const bool HasSelf = llvm::any_of(Value->PhiIncomings,
                                        [&](const MedPhiIncoming &Incoming) {
                                          return Incoming.SourceLane == Lane.ID;
                                        });
      SawSelfLoopEntryPhi |= HasSelf && Value->PhiIncomings.size() > 1;
    }
  }
  ASSERT_TRUE(SawSelfLoopEntryPhi);
  ASSERT_EQ(Program->High.Errors.size(), 1u);
  EXPECT_FALSE(Program->High.Errors.front().Selector.has_value());
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

TEST(EVMAnalyzer, MayReachableIndirectRevertProducesNoDefiniteErrorFact) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),  opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::JUMP),   opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH0),  opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::REVERT),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const LowBlock *Candidate = Program->Low.findBlock(3);
  ASSERT_NE(Candidate, nullptr);
  EXPECT_FALSE(Candidate->Reachable);
  EXPECT_TRUE(Candidate->MayReachable);
  EXPECT_TRUE(Program->High.Errors.empty());
}

TEST(EVMAnalyzer, MayReachableIndirectStoreProducesNoDefiniteStorageFact) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),  opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::JUMP),   opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH0),  opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::SSTORE), opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const LowBlock *Candidate = Program->Low.findBlock(3);
  ASSERT_NE(Candidate, nullptr);
  EXPECT_FALSE(Candidate->Reachable);
  EXPECT_TRUE(Candidate->MayReachable);
  EXPECT_TRUE(Program->High.Storage.empty());
}

TEST(EVMAnalyzer, MayReachableDispatcherProducesNoDefiniteFunctionFact) {
  constexpr uint8_t kFunctionEntry = 0x13;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
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

TEST(EVMAnalyzer, RejectOnlyEmptyCalldataPathIsNotAReceiveEntry) {
  constexpr uint8_t kEmptyCalldataEntry = 0x06;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        kEmptyCalldataEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.HasReceive);
}

TEST(EVMAnalyzer, NonPayableEmptyCalldataHandlerIsNotAReceiveEntry) {
  constexpr uint8_t kEmptyCalldataEntry = 0x06;
  constexpr uint8_t kZeroValueEntry = 0x10;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        kEmptyCalldataEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::CALLVALUE),
      opcodeByte(Opcode::DUP1),         opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        kZeroValueEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::POP),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.HasReceive);
}

TEST(EVMAnalyzer, RawCalldataSizeFalseEdgeCanEnterReceive) {
  constexpr uint8_t kNonEmptyEntry = 0x06;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE),
      opcodeByte(Opcode::PUSH1),
      kNonEmptyEntry,
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
  EXPECT_TRUE(Program->High.HasReceive);
}

TEST(EVMAnalyzer, RawCallValueTrueEdgeRejectsReceive) {
  constexpr uint8_t kEmptyCalldataEntry = 0x06;
  constexpr uint8_t kNonZeroValueEntry = 0x0c;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        kEmptyCalldataEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::CALLVALUE),
      opcodeByte(Opcode::PUSH1),        kNonZeroValueEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.HasReceive);
}

TEST(EVMAnalyzer, RawCalldataSizeFalseEdgeRejectsReceive) {
  constexpr uint8_t kEmptyCalldataEntry = 0x06;
  constexpr uint8_t kImpossibleAcceptEntry = 0x0e;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        kEmptyCalldataEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::CALLDATASIZE),
      opcodeByte(Opcode::PUSH1),        kImpossibleAcceptEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.HasReceive);
}

TEST(EVMAnalyzer, SelectorGatedInternalEmptyGuardIsNotAReceiveEntry) {
  constexpr uint8_t kImpossibleEmptyEntry = test::kTestFunctionEntry + 9;
  const std::vector<uint8_t> Body = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        kImpossibleEmptyEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(dispatcherFor(0x12345678, Body));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.HasReceive);
}

TEST(EVMAnalyzer, RevertingEffectfulPathsAreNeitherFallbackNorReceive) {
  const std::vector<uint8_t> FallbackOnly = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::SLOAD),
      opcodeByte(Opcode::POP),   opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT),
  };
  auto Fallback = analyze(FallbackOnly);
  ASSERT_TRUE(static_cast<bool>(Fallback))
      << llvm::toString(Fallback.takeError());
  EXPECT_FALSE(Fallback->High.HasFallback);

  constexpr uint8_t kEmptyCalldataEntry = 0x08;
  const std::vector<uint8_t> ReceiveOnly = {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::ISZERO),
      opcodeByte(Opcode::PUSH1),        kEmptyCalldataEntry,
      opcodeByte(Opcode::JUMPI),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::REVERT),
      opcodeByte(Opcode::JUMPDEST),     opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::SLOAD),        opcodeByte(Opcode::POP),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::REVERT),
  };
  auto Receive = analyze(ReceiveOnly);
  ASSERT_TRUE(static_cast<bool>(Receive))
      << llvm::toString(Receive.takeError());
  EXPECT_FALSE(Receive->High.HasReceive);
}

TEST(EVMAnalyzer, NaturalCodeEndIsASuccessfulFallbackTerminal) {
  auto Empty = analyze({});
  ASSERT_TRUE(static_cast<bool>(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->High.HasFallback);
  EXPECT_FALSE(Empty->High.HasReceive);

  auto Falloff = analyze({opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::POP)});
  ASSERT_TRUE(static_cast<bool>(Falloff))
      << llvm::toString(Falloff.takeError());
  EXPECT_TRUE(Falloff->High.HasFallback);
  EXPECT_FALSE(Falloff->High.HasReceive);
}

TEST(EVMAnalyzer, UnreachableBlocksProduceNoSemanticFacts) {
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::STOP), opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::SLOAD), opcodeByte(Opcode::STOP)};

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Storage.empty());
}

TEST(EVMAnalyzer, RecoversSelectorReturnAndStorageFacts) {
  // Solidity-style dispatcher for selector 0x12345678 -> pc 0x15, returning
  // uint256(42). A following unreachable fragment touches storage slot 3 and
  // must not become a source-level fact.
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
  EXPECT_TRUE(Program->High.Storage.empty());

  EXPECT_NE(dumpLowIR(Program->Low).find("block 0x0"), std::string::npos);
  EXPECT_EQ(dumpMedIR(Program->Med).find("storage.read"), std::string::npos);
  EXPECT_NE(dumpHighIR(Program->High).find("selector 0x12345678"),
            std::string::npos);
}

TEST(EVMAnalyzer, MatchedSelectorCannotReenterAnotherFunctionBody) {
  constexpr uint32_t kFirstSelector = 0x11223344;
  constexpr uint32_t kSecondSelector = 0xaabbccdd;
  constexpr uint8_t kSecondDispatchPC = 0x0f;
  constexpr uint8_t kFirstExitPC = 0x1a;
  constexpr uint8_t kFirstEntryPC = 0x1b;
  constexpr uint8_t kSecondEntryPC = 0x1f;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kFirstSelector >> 24),
      static_cast<uint8_t>(kFirstSelector >> 16),
      static_cast<uint8_t>(kFirstSelector >> 8),
      static_cast<uint8_t>(kFirstSelector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kFirstEntryPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kSecondSelector >> 24),
      static_cast<uint8_t>(kSecondSelector >> 16),
      static_cast<uint8_t>(kSecondSelector >> 8),
      static_cast<uint8_t>(kSecondSelector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kSecondEntryPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      kSecondDispatchPC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::PUSH1),
      kSelectorBytes,
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 2u);

  const RecoveredFunction *First = nullptr;
  const RecoveredFunction *Second = nullptr;
  for (const RecoveredFunction &Function : Program->High.Functions) {
    if (Function.Selector == kFirstSelector)
      First = &Function;
    if (Function.Selector == kSecondSelector)
      Second = &Function;
  }
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  EXPECT_TRUE(First->Arguments.empty());
  ASSERT_EQ(Second->Arguments.size(), 1u);
  EXPECT_EQ(Second->Arguments.front().CalldataOffset, kSelectorBytes);
  EXPECT_TRUE(Second->Arguments.front().Read);

  const StructuredRegion *FirstRegion = nullptr;
  const StructuredRegion *SecondRegion = nullptr;
  for (const StructuredRegion &Region : Program->High.Regions)
    if (Region.Kind == RegionKind::Function) {
      if (Region.EntryPC == kFirstEntryPC)
        FirstRegion = &Region;
      if (Region.EntryPC == kSecondEntryPC)
        SecondRegion = &Region;
    }
  ASSERT_NE(FirstRegion, nullptr);
  ASSERT_NE(SecondRegion, nullptr);
  EXPECT_TRUE(llvm::is_contained(FirstRegion->Blocks, kFirstEntryPC));
  EXPECT_TRUE(llvm::is_contained(FirstRegion->Blocks, kSecondDispatchPC));
  EXPECT_TRUE(llvm::is_contained(FirstRegion->Blocks, kFirstExitPC));
  EXPECT_FALSE(llvm::is_contained(FirstRegion->Blocks, kSecondEntryPC));
  EXPECT_TRUE(llvm::is_contained(SecondRegion->Blocks, kSecondEntryPC));
}

TEST(EVMAnalyzer, MatchedXorSelectorCannotReenterAnotherFunctionBody) {
  constexpr uint32_t kFirstSelector = 0x11223344;
  constexpr uint32_t kSecondSelector = 0xaabbccdd;
  constexpr uint8_t kSecondDispatchPC = 0x14;
  constexpr uint8_t kFirstEntryPC = 0x0f;
  constexpr uint8_t kSecondEntryPC = 0x1f;
  constexpr uint8_t kUnmatchedEntryPC = 0x26;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kFirstSelector >> 24),
      static_cast<uint8_t>(kFirstSelector >> 16),
      static_cast<uint8_t>(kFirstSelector >> 8),
      static_cast<uint8_t>(kFirstSelector),
      opcodeByte(Opcode::XOR),
      opcodeByte(Opcode::PUSH1),
      kSecondDispatchPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH1),
      kSecondDispatchPC,
      opcodeByte(Opcode::JUMP),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kSecondSelector >> 24),
      static_cast<uint8_t>(kSecondSelector >> 16),
      static_cast<uint8_t>(kSecondSelector >> 8),
      static_cast<uint8_t>(kSecondSelector),
      opcodeByte(Opcode::XOR),
      opcodeByte(Opcode::PUSH1),
      kUnmatchedEntryPC,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::PUSH1),
      kSelectorBytes,
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::POP),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 2u);

  const RecoveredFunction *First = nullptr;
  const RecoveredFunction *Second = nullptr;
  for (const RecoveredFunction &Function : Program->High.Functions) {
    if (Function.Selector == kFirstSelector)
      First = &Function;
    if (Function.Selector == kSecondSelector)
      Second = &Function;
  }
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  EXPECT_TRUE(First->Arguments.empty());
  ASSERT_EQ(Second->Arguments.size(), 1u);
  EXPECT_EQ(Second->Arguments.front().CalldataOffset, kSelectorBytes);
  EXPECT_TRUE(Second->Arguments.front().Read);

  const StructuredRegion *FirstRegion = nullptr;
  for (const StructuredRegion &Region : Program->High.Regions)
    if (Region.Kind == RegionKind::Function && Region.EntryPC == kFirstEntryPC)
      FirstRegion = &Region;
  ASSERT_NE(FirstRegion, nullptr);
  EXPECT_TRUE(llvm::is_contained(FirstRegion->Blocks, kSecondDispatchPC));
  EXPECT_TRUE(llvm::is_contained(FirstRegion->Blocks, kUnmatchedEntryPC));
  EXPECT_FALSE(llvm::is_contained(FirstRegion->Blocks, kSecondEntryPC));
}

TEST(EVMAnalyzer, RejectsAmbiguousDuplicateSelectorRecovery) {
  // Call value is independent of the selector, so both comparisons are
  // feasible for the same selector and really do name different valid entries.
  // HighIR must not present either target as recovered source truth.
  constexpr uint8_t kNonZeroValueBranch = 0x13;
  constexpr uint8_t kZeroValueEntry = 0x23;
  constexpr uint8_t kNonZeroValueEntry = 0x25;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::CALLVALUE),
      opcodeByte(Opcode::PUSH1),
      kNonZeroValueBranch,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      0xe0,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kZeroValueEntry,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      0xe0,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      kNonZeroValueEntry,
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

TEST(EVMAnalyzer, HighIRBudgetsAcceptTheBoundaryAndRejectTheNextRecord) {
  const auto ExpectBoundary = [](llvm::ArrayRef<uint8_t> Code,
                                 size_t AnalyzeOptions::*Member, size_t Exact,
                                 llvm::StringRef Name) {
    ASSERT_GT(Exact, 1u) << Name.str();
    AnalyzeOptions AtBoundary;
    AtBoundary.*Member = Exact;
    auto Accepted = analyze(Code, AtBoundary);
    ASSERT_TRUE(static_cast<bool>(Accepted))
        << Name.str() << ": " << llvm::toString(Accepted.takeError());

    AnalyzeOptions BelowBoundary;
    BelowBoundary.*Member = Exact - 1;
    auto Rejected = analyze(Code, BelowBoundary);
    ASSERT_FALSE(static_cast<bool>(Rejected)) << Name.str();
    EXPECT_NE(llvm::toString(Rejected.takeError()).find(Name.str()),
              std::string::npos)
        << Name.str();
  };

  const std::vector<uint8_t> TwoFunctions = twoSelectorDispatcher();
  auto FunctionBaseline = analyze(TwoFunctions);
  ASSERT_TRUE(static_cast<bool>(FunctionBaseline))
      << llvm::toString(FunctionBaseline.takeError());
  ASSERT_EQ(FunctionBaseline->High.Functions.size(), 2u);
  ExpectBoundary(TwoFunctions, &AnalyzeOptions::MaxHighFunctions,
                 FunctionBaseline->High.Functions.size(),
                 kMaxHighFunctionsName);

  const std::vector<uint8_t> RecoveredArguments = selectorDispatcher(
      {opcodeByte(Opcode::PUSH1), 0x10, opcodeByte(Opcode::PUSH1), 0x14,
       opcodeByte(Opcode::ADD), opcodeByte(Opcode::CALLDATALOAD),
       opcodeByte(Opcode::POP), opcodeByte(Opcode::STOP)});
  auto ArgumentBaseline = analyze(RecoveredArguments);
  ASSERT_TRUE(static_cast<bool>(ArgumentBaseline))
      << llvm::toString(ArgumentBaseline.takeError());
  ASSERT_EQ(ArgumentBaseline->High.Functions.size(), 1u);
  const size_t RecoveredArgumentCount =
      ArgumentBaseline->High.Functions.front().Arguments.size();
  ASSERT_EQ(RecoveredArgumentCount, 2u);
  ExpectBoundary(RecoveredArguments, &AnalyzeOptions::MaxHighRecoveredArguments,
                 RecoveredArgumentCount, kMaxHighRecoveredArgumentsName);

  constexpr uint8_t kConditionalTarget = 0x07;
  const std::vector<uint8_t> MultipleLanes = {
      opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1), kConditionalTarget,
      opcodeByte(Opcode::JUMPI), opcodeByte(Opcode::STOP),
      opcodeByte(Opcode::STOP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };
  auto LaneBaseline = analyze(MultipleLanes);
  ASSERT_TRUE(static_cast<bool>(LaneBaseline))
      << llvm::toString(LaneBaseline.takeError());
  constexpr size_t kRootConstrainedDispatchLaneVisits = 7;
  ExpectBoundary(MultipleLanes, &AnalyzeOptions::MaxHighLaneVisits,
                 kRootConstrainedDispatchLaneVisits, kMaxHighLaneVisitsName);

  constexpr size_t kStopHighOperationVisits = 6;
  const std::vector<uint8_t> Stop = {opcodeByte(Opcode::STOP)};
  ExpectBoundary(Stop, &AnalyzeOptions::MaxHighOperationVisits,
                 kStopHighOperationVisits, kMaxHighOperationVisitsName);

  constexpr uint8_t kSecondFunctionBlock = 0x13;
  const std::vector<uint8_t> TwoFunctionBlocks = selectorDispatcher(
      {opcodeByte(Opcode::PUSH1), kSecondFunctionBlock,
       opcodeByte(Opcode::JUMP), opcodeByte(Opcode::JUMPDEST),
       opcodeByte(Opcode::STOP)});
  auto RegionBaseline = analyze(TwoFunctionBlocks);
  ASSERT_TRUE(static_cast<bool>(RegionBaseline))
      << llvm::toString(RegionBaseline.takeError());
  const auto FunctionRegion = llvm::find_if(
      RegionBaseline->High.Regions, [](const StructuredRegion &Region) {
        return Region.Kind == RegionKind::Function;
      });
  ASSERT_NE(FunctionRegion, RegionBaseline->High.Regions.end());
  ExpectBoundary(
      TwoFunctionBlocks, &AnalyzeOptions::MaxHighRegionBlockReferences,
      FunctionRegion->Blocks.size(), kMaxHighRegionBlockReferencesName);

  const KnownSignatureInfo &Panic =
      getLanguageRevertInfo(LanguageRevert::Panic);
  std::vector<uint8_t> PanicPayload = pushSelectorPayload(Panic.Selector);
  append(PanicPayload, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                        opcodeByte(Opcode::PUSH1),
                        static_cast<uint8_t>(PanicCode::ArithmeticOverflow),
                        opcodeByte(Opcode::PUSH1), kSelectorBytes,
                        opcodeByte(Opcode::MSTORE), opcodeByte(Opcode::PUSH1),
                        static_cast<uint8_t>(kSelectorBytes + kWordBytes),
                        opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});
  constexpr size_t kPanicMemoryReadRequests = 2;
  ExpectBoundary(PanicPayload, &AnalyzeOptions::MaxHighMemoryReadRequests,
                 kPanicMemoryReadRequests, kMaxHighMemoryReadRequestsName);

  constexpr uint32_t kErrorSelector = 0x118cdaa7;
  std::vector<uint8_t> ErrorPayload = pushSelectorPayload(kErrorSelector);
  append(ErrorPayload, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
                        opcodeByte(Opcode::PUSH1), kSelectorBytes,
                        opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::REVERT)});
  ExpectBoundary(ErrorPayload, &AnalyzeOptions::MaxHighTrackedMemoryBytes,
                 kSelectorBytes, kMaxHighTrackedMemoryBytesName);

  auto MemoryBaseline = analyze(ErrorPayload);
  ASSERT_TRUE(static_cast<bool>(MemoryBaseline))
      << llvm::toString(MemoryBaseline.takeError());
  const size_t ReachableMemoryLanes = llvm::count_if(
      MemoryBaseline->Med.StateLanes, [](const MedStateLane &Lane) {
        return Lane.Evidence == Reachability::Reachable;
      });
  constexpr size_t kEntryAndExitMemoryStates = 2;
  const size_t MemoryStateCells =
      ReachableMemoryLanes * kSelectorBytes * kEntryAndExitMemoryStates;
  ExpectBoundary(ErrorPayload, &AnalyzeOptions::MaxHighMemoryStateCells,
                 MemoryStateCells, kMaxHighMemoryStateCellsName);

  constexpr uint8_t kRevertBlock = 0x26;
  std::vector<uint8_t> CrossBlockPayload = pushSelectorPayload(kErrorSelector);
  append(CrossBlockPayload,
         {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE),
          opcodeByte(Opcode::PUSH1), kRevertBlock, opcodeByte(Opcode::JUMP),
          opcodeByte(Opcode::JUMPDEST), opcodeByte(Opcode::PUSH1),
          kSelectorBytes, opcodeByte(Opcode::PUSH0),
          opcodeByte(Opcode::REVERT)});
  auto WorklistBaseline = analyze(CrossBlockPayload);
  ASSERT_TRUE(static_cast<bool>(WorklistBaseline))
      << llvm::toString(WorklistBaseline.takeError());
  const size_t ReachableWorklistLanes = llvm::count_if(
      WorklistBaseline->Med.StateLanes, [](const MedStateLane &Lane) {
        return Lane.Evidence == Reachability::Reachable;
      });
  constexpr size_t kCrossBlockMemoryWorklistTransfers = 4;
  ASSERT_EQ(ReachableWorklistLanes, 2u);
  ExpectBoundary(
      CrossBlockPayload, &AnalyzeOptions::MaxHighMemoryWorklistUpdates,
      kCrossBlockMemoryWorklistTransfers, kMaxHighMemoryWorklistUpdatesName);
}

TEST(EVMAnalyzer, RootCFGRegionChargesEveryBlockReference) {
  constexpr uint8_t kDestinationPC = 3;
  const std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH1), kDestinationPC,
      opcodeByte(Opcode::JUMP),  opcodeByte(Opcode::JUMPDEST),
      opcodeByte(Opcode::STOP),
  };

  AnalyzeOptions Exact;
  Exact.MaxHighRegionBlockReferences = 2;
  auto Accepted = analyze(Code, Exact);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  ASSERT_EQ(Accepted->High.Regions.size(), 1u);
  EXPECT_EQ(Accepted->High.Regions.front().Kind, RegionKind::CFG);
  EXPECT_EQ(Accepted->High.Regions.front().Blocks.size(), 2u);

  AnalyzeOptions TooMany = Exact;
  --TooMany.MaxHighRegionBlockReferences;
  auto Rejected = analyze(Code, TooMany);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError())
                .find(kMaxHighRegionBlockReferencesName.str()),
            std::string::npos);
}

TEST(EVMAnalyzer, DisabledHighIRDoesNotValidateHighIRBudgets) {
  AnalyzeOptions Options;
  Options.RecoverHighLevel = false;
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE) Options.NAME = 0;
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR

  auto Program = analyze({opcodeByte(Opcode::STOP)}, Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Regions.empty());
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
