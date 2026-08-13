//===- EVMBytecodeContainerTests.cpp - EVM container format tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/evm/runtime/EVMCalls.h"
#include "neverd/evm/bytecode/EVMDecoder.h"
#include "neverd/evm/EVMConstants.h"

#include "llvm/Support/Error.h"

namespace neverd::evm {
namespace {

//===----------------------------------------------------------------------===//
// Bytes that are not instructions
//===----------------------------------------------------------------------===//

/// A delegation indicator with a target of \p TargetBytes repeated \p Fill
/// nibbles. Spelling the length out keeps a miscounted literal from turning a
/// boundary test into a test of the wrong boundary.
std::string delegationHex(size_t TargetBytes, char Fill = '1') {
  return "0xef0100" + std::string(TargetBytes * kHexDigitsPerByte, Fill);
}

TEST(EVMBytecode, RecognizesADelegationIndicatorAtItsExactLength) {
  auto Loaded =
      decodeBytecodeInput(delegationHex(kAddressBytes), "delegated.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Container, BytecodeContainer::Delegation);
  EXPECT_EQ(Loaded->disposition(),
            ContainerDisposition::RequiresDelegateTarget);
  EXPECT_EQ(Loaded->delegateTarget().size(), kAddressBytes);
  EXPECT_EQ(Loaded->delegateTarget().front(), 0x11);

  // Nothing was unwrapped and nothing was stripped: these bytes are not code,
  // so neither step has anything to do to them.
  EXPECT_FALSE(Loaded->RuntimeExtracted);
  EXPECT_FALSE(Loaded->MetadataStripped);
  EXPECT_FALSE(Loaded->InputMetadata.has_value());
  EXPECT_EQ(Loaded->Code.size(), 23u);

  // One byte short and one byte long are malformed inputs, not shorter and
  // longer indicators. They stay instructions so the decoder can say which
  // byte it could not read.
  for (size_t Wrong : {kAddressBytes - 1, kAddressBytes + 1}) {
    SCOPED_TRACE(Wrong);
    auto Other = decodeBytecodeInput(delegationHex(Wrong), "wrong-length.hex");
    ASSERT_TRUE(static_cast<bool>(Other)) << llvm::toString(Other.takeError());
    EXPECT_EQ(Other->Container, BytecodeContainer::Legacy);
    EXPECT_TRUE(Other->delegateTarget().empty());
  }
}

TEST(EVMBytecode, RefusesToDecodeADelegationAndSaysWhy) {
  const std::string Indicator = delegationHex(kAddressBytes, '2');

  // The marker only means anything once the proposal activates. Before that an
  // account could not hold these bytes, and saying "delegates to" would
  // describe a state the chain could not have been in.
  BytecodeLoadOptions Before;
  Before.Fork = Hardfork::Cancun;
  auto Early = decodeBytecodeInput(Indicator, "early.hex", Before);
  ASSERT_TRUE(static_cast<bool>(Early)) << llvm::toString(Early.takeError());
  const std::string EarlyReason = llvm::toString(checkDecodable(*Early));
  EXPECT_NE(EarlyReason.find("not assigned until pectra"), std::string::npos)
      << EarlyReason;

  BytecodeLoadOptions After;
  After.Fork = Hardfork::Pectra;
  auto Live = decodeBytecodeInput(Indicator, "live.hex", After);
  ASSERT_TRUE(static_cast<bool>(Live)) << llvm::toString(Live.takeError());
  const std::string LiveReason = llvm::toString(checkDecodable(*Live));
  EXPECT_NE(LiveReason.find("0x2222222222222222222222222222222222222222"),
            std::string::npos)
      << LiveReason;
  EXPECT_NE(LiveReason.find("not supplied"), std::string::npos) << LiveReason;
}

TEST(EVMBytecode, RefusesAnObjectFormatContainerWithoutGuessingAtItsSections) {
  auto Loaded = decodeBytecodeInput("0xef000101000402000100010400000000800000fe",
                                    "container.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Container, BytecodeContainer::EVMObjectFormat);
  EXPECT_EQ(Loaded->disposition(), ContainerDisposition::Unrecognized);
  // No fork has ever been scheduled to assign this marker, which is a
  // different statement from "not supported here".
  EXPECT_FALSE(bytecodeContainerActivation(BytecodeContainer::EVMObjectFormat)
                   .has_value());

  const std::string Reason = llvm::toString(checkDecodable(*Loaded));
  EXPECT_NE(Reason.find("eof"), std::string::npos) << Reason;
  EXPECT_NE(Reason.find("no fork has activated"), std::string::npos) << Reason;
}

TEST(EVMBytecode, DescribesEveryTabulatedContainer) {
  for (const BytecodeContainerInfo &Info : bytecodeContainerInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getBytecodeContainerInfo(Info.ID), &Info);
    EXPECT_EQ(bytecodeContainerName(Info.ID), Info.Name);
    EXPECT_FALSE(Info.Summary.empty());
    // Only the fallback matches without a marker, and only a marked container
    // may claim a proposal.
    EXPECT_EQ(Info.MarkerBytes == 0, Info.EIP.empty());
  }
  EXPECT_EQ(classifyBytecodeContainer(std::vector<uint8_t>{0x60, 0x00}),
            BytecodeContainer::Legacy);

  for (const BytecodeSourceInfo &Info : bytecodeSourceInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(bytecodeSourceName(Info.ID), Info.Name);
    EXPECT_FALSE(Info.Summary.empty());
  }
}

//===----------------------------------------------------------------------===//
// One walk, one answer about where an instruction ends
//===----------------------------------------------------------------------===//

TEST(EVMBytecode, NormalizationWalksTheConstructorUnderTheAnalyzedFork) {
  // The constructor pushes its offsets with PUSH0, which is a byte no opcode
  // claimed before Shanghai. A walk that assumes the newest fork reads the
  // wrapper; a walk under the fork actually being analyzed reaches a byte it
  // cannot execute and declines to claim a runtime.
  constexpr llvm::StringLiteral Creation = "6005600a5f3960055ff36001600055";

  BytecodeLoadOptions Modern;
  Modern.Fork = Hardfork::Shanghai;
  auto Extracted = decodeBytecodeInput(Creation, "shanghai.hex", Modern);
  ASSERT_TRUE(static_cast<bool>(Extracted))
      << llvm::toString(Extracted.takeError());
  EXPECT_TRUE(Extracted->RuntimeExtracted);
  EXPECT_EQ(Extracted->Code,
            (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));

  BytecodeLoadOptions Older;
  Older.Fork = Hardfork::Paris;
  auto Kept = decodeBytecodeInput(Creation, "paris.hex", Older);
  ASSERT_TRUE(static_cast<bool>(Kept)) << llvm::toString(Kept.takeError());
  EXPECT_FALSE(Kept->RuntimeExtracted);
  EXPECT_EQ(Kept->Code.size(), Creation.size() / kHexDigitsPerByte);
}

TEST(EVMBytecode, ReNormalizesFromTheContainerRatherThanTheResult) {
  auto First = decodeBytecodeInput("6005600c60003960056000f36001600055",
                                   "creation.hex");
  ASSERT_TRUE(static_cast<bool>(First)) << llvm::toString(First.takeError());
  ASSERT_TRUE(First->RuntimeExtracted);

  // The container is what a later session with a different fork has to start
  // from. Normalizing the previous result again would unwrap an already
  // unwrapped program.
  auto Again = normalizeBytecode(First->Original, First->Source,
                                 First->SourceIsRuntime);
  ASSERT_TRUE(static_cast<bool>(Again)) << llvm::toString(Again.takeError());
  EXPECT_EQ(Again->Code, First->Code);
  EXPECT_TRUE(Again->RuntimeExtracted);
}

TEST(EVMBytecode, DecodesOneInstructionWidthPerForkAndImmediate) {
  const std::vector<uint8_t> Code{0xe6, 0x01, 0x00};

  // A conditional immediate is consumed only where the opcode is active and
  // the byte decodes. Both halves of that rule decide where the next
  // instruction starts.
  const LowInstruction Active =
      decodeInstructionAt(Code, 0, Hardfork::Amsterdam, nullptr);
  EXPECT_TRUE(Active.isActive());
  EXPECT_EQ(Active.NextPC, 2u);
  ASSERT_EQ(Active.StackOperandCount, 1);

  const LowInstruction Inactive =
      decodeInstructionAt(Code, 0, Hardfork::Fusaka, nullptr);
  EXPECT_FALSE(Inactive.isActive());
  EXPECT_TRUE(Inactive.isAssigned());
  EXPECT_EQ(Inactive.NextPC, 1u);

  // A candidate the encoding forbids is not consumed: it starts the following
  // instruction.
  const std::vector<uint8_t> Malformed{0xe6, 0x60};
  const LowInstruction Rejected =
      decodeInstructionAt(Malformed, 0, Hardfork::Amsterdam, nullptr);
  EXPECT_EQ(Rejected.ImmediateStatus, ImmediateDecodeStatus::Invalid);
  EXPECT_EQ(Rejected.NextPC, 1u);
}

TEST(EVMBytecode, CreditsTheReservedAddressToTheProposalThatScheduledIt) {
  // The interface came from a rollup proposal, but the address was reserved on
  // mainnet by a later document; naming the rollup one credits a proposal that
  // never scheduled it.
  const PrecompileInfo &Info = getPrecompileInfo(Precompile::P256Verify);
  EXPECT_EQ(Info.EIP, "eip-7951");
  EXPECT_EQ(Info.Introduced, Hardfork::Fusaka);
  EXPECT_EQ(findPrecompile(Info.Address, Hardfork::Fusaka), &Info);
  EXPECT_EQ(findPrecompile(Info.Address, Hardfork::Pectra), nullptr);
}
} // namespace
} // namespace neverd::evm
