//===- EVMAnalyzerHighIRTests.cpp - EVM high-level recovery tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMAnalyzerTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"

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

void append(std::vector<uint8_t> &Code, std::vector<uint8_t> Tail) {
  Code.insert(Code.end(), Tail.begin(), Tail.end());
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

  // The hashed signature settles the argument list even though the body reads
  // nothing, which no amount of dataflow could establish.
  ASSERT_EQ(Function.Arguments.size(), 2u);
  EXPECT_EQ(Function.Arguments[0].Type, "address");
  EXPECT_EQ(Function.Arguments[1].Type, "uint256");
  EXPECT_EQ(Function.Arguments[0].TypeSource, ABITypeSource::KnownSignature);
  EXPECT_FALSE(Function.Arguments[0].Read);
  ASSERT_EQ(Function.Returns.size(), 1u);
  EXPECT_EQ(Function.Returns.front(), "bool");
  EXPECT_EQ(Function.ReturnSource, ABITypeSource::KnownSignature);

  ASSERT_EQ(Program->High.Standards.size(), 1u);
  EXPECT_EQ(Program->High.Standards.front(), KnownStandard::ERC20);
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

TEST(EVMAnalyzer, NamesAnEventThatHashesToATabulatedTopic) {
  const KnownSignatureInfo *Transfer =
      findSignature("Transfer(address,address,uint256)");
  ASSERT_NE(Transfer, nullptr);

  std::vector<uint8_t> Code = pushWord(Transfer->Topic);
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
                opcodeByte(Opcode::LOG1), opcodeByte(Opcode::STOP)});

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Events.size(), 1u);
  EXPECT_EQ(Program->High.Events.front().Known, Transfer);
  EXPECT_EQ(Program->High.Events.front().SuggestedName, "Transfer");
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

TEST(EVMAnalyzer, RecoversSelectorReturnAndStorageFacts) {
  // Solidity-style dispatcher for selector 0x12345678 -> pc 0x15, returning
  // uint256(42). A following unreachable fragment touches storage slot 3.
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
  ASSERT_EQ(Program->High.Storage.size(), 1u);
  EXPECT_TRUE(Program->High.Storage[0].Slot.has_value());
  EXPECT_EQ(Program->High.Storage[0].Slot->getZExtValue(), 3u);

  EXPECT_NE(dumpLowIR(Program->Low).find("block 0x0"), std::string::npos);
  EXPECT_NE(dumpMedIR(Program->Med).find("storage.read"), std::string::npos);
  EXPECT_NE(dumpHighIR(Program->High).find("selector 0x12345678"),
            std::string::npos);
}

TEST(EVMAnalyzer, RejectsAmbiguousDuplicateSelectorRecovery) {
  // The same selector is compared twice but branches to different valid
  // entries. HighIR must not silently let the later pattern overwrite the
  // first and present one arbitrary target as recovered source truth.
  const std::vector<uint8_t> Code = {
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
      0x1b,
      opcodeByte(Opcode::JUMPI),
      opcodeByte(Opcode::DUP1),
      opcodeByte(Opcode::PUSH4),
      0x12,
      0x34,
      0x56,
      0x78,
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      0x1d,
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
