//===- EVMABITests.cpp - Recovered ABI type and signature tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/ABI.h"

#include "llvm/Support/Error.h"

#include <string>

namespace neverd::evm {
namespace {

const KnownSignatureInfo *findSignature(llvm::StringRef Text) {
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Signature == Text)
      return &Info;
  return nullptr;
}

llvm::APInt lowMask(unsigned Bytes) {
  return llvm::APInt::getLowBitsSet(kWordBits, Bytes * kBitsPerByte);
}

llvm::APInt highMask(unsigned Bytes) {
  return llvm::APInt::getHighBitsSet(kWordBits, Bytes * kBitsPerByte);
}

//===----------------------------------------------------------------------===//
// Type spellings
//===----------------------------------------------------------------------===//

TEST(EVMABIType, SpellsEachClassWithItsOwnWidthUnit) {
  EXPECT_EQ(ABIType::word().spelling(), "uint256");
  EXPECT_EQ(ABIType::sized(ABITypeClass::Unsigned, 1).spelling(), "uint8");
  EXPECT_EQ(ABIType::sized(ABITypeClass::Signed, 16).spelling(), "int128");
  EXPECT_EQ(ABIType::sized(ABITypeClass::FixedBytes, 4).spelling(), "bytes4");
  EXPECT_EQ(ABIType::plain(ABITypeClass::Address).spelling(), "address");
  EXPECT_EQ(ABIType::plain(ABITypeClass::Bool).spelling(), "bool");
  EXPECT_EQ(ABIType::plain(ABITypeClass::Bytes).spelling(), "bytes");
  EXPECT_EQ(ABIType::plain(ABITypeClass::String).spelling(), "string");

  for (const ABITypeClassInfo &Info : abiTypeClassInfos()) {
    SCOPED_TRACE(Info.Spelling.str());
    EXPECT_EQ(&getABITypeClassInfo(Info.ID), &Info);
    EXPECT_FALSE(Info.Spelling.empty());
  }
}

TEST(EVMABIType, RecognizesOnlyWholeByteMasks) {
  EXPECT_EQ(lowByteMaskWidth(lowMask(kAddressBytes)), kAddressBytes);
  EXPECT_EQ(lowByteMaskWidth(lowMask(1)), 1u);
  EXPECT_EQ(highByteMaskWidth(highMask(kSelectorBytes)), kSelectorBytes);

  // A low mask is not a high mask and vice versa, or a cleanup would be read
  // as the padding it is not.
  EXPECT_FALSE(highByteMaskWidth(lowMask(kAddressBytes)).has_value());
  EXPECT_FALSE(lowByteMaskWidth(highMask(kSelectorBytes)).has_value());

  // A mask that stops inside a byte does not describe an ABI type.
  EXPECT_FALSE(
      lowByteMaskWidth(llvm::APInt::getLowBitsSet(kWordBits, 12)).has_value());
  EXPECT_FALSE(lowByteMaskWidth(llvm::APInt(kWordBits, 0)).has_value());
  EXPECT_FALSE(
      lowByteMaskWidth(llvm::APInt::getAllOnes(kWordBits)).has_value());
  // A mask with a hole is neither.
  EXPECT_FALSE(lowByteMaskWidth(llvm::APInt(kWordBits, 0xff00ff)).has_value());
}

//===----------------------------------------------------------------------===//
// Resolving a type from observations
//===----------------------------------------------------------------------===//

TEST(EVMABIConstraint, ReportsTheWordWidthUntilSomethingNarrowsIt) {
  ABIConstraint Constraint;
  EXPECT_TRUE(Constraint.empty());
  EXPECT_EQ(Constraint.source(), ABITypeSource::Default);
  EXPECT_EQ(Constraint.resolve().spelling(), "uint256");

  Constraint.observe(ABIEvidence::Arithmetic);
  EXPECT_FALSE(Constraint.empty());
  EXPECT_EQ(Constraint.source(), ABITypeSource::Dataflow);
}

TEST(EVMABIConstraint, KeepsTheNarrowestMaskItWasShown) {
  ABIConstraint Constraint;
  Constraint.narrowTo(kWordBytes);
  Constraint.narrowTo(kAddressBytes);
  Constraint.narrowTo(kWordBytes);
  EXPECT_EQ(Constraint.byteWidth(), kAddressBytes);

  // A width the machine cannot hold says nothing.
  Constraint.narrowTo(0);
  Constraint.narrowTo(kWordBytes + 1);
  EXPECT_EQ(Constraint.byteWidth(), kAddressBytes);
}

TEST(EVMABIConstraint, ResolvesEachObservationToItsType) {
  const auto Resolve = [](ABIEvidence Evidence, unsigned Width) {
    ABIConstraint Constraint;
    Constraint.observe(Evidence);
    Constraint.narrowTo(Width);
    return Constraint.resolve().spelling();
  };

  EXPECT_EQ(Resolve(ABIEvidence::LowByteMask, kAddressBytes), "address");
  EXPECT_EQ(Resolve(ABIEvidence::CallTarget, kWordBytes), "address");
  EXPECT_EQ(Resolve(ABIEvidence::BooleanTest, 1), "bool");
  EXPECT_EQ(Resolve(ABIEvidence::HighByteMask, kSelectorBytes), "bytes4");
  EXPECT_EQ(Resolve(ABIEvidence::SignExtended, 1), "int8");
  EXPECT_EQ(Resolve(ABIEvidence::SignedCompare, kWordBytes), "int256");
  EXPECT_EQ(Resolve(ABIEvidence::Arithmetic, 2), "uint16");
  EXPECT_EQ(Resolve(ABIEvidence::BitShift, kWordBytes), "bytes32");
  EXPECT_EQ(Resolve(ABIEvidence::Bitwise, kWordBytes), "bytes32");
  EXPECT_EQ(Resolve(ABIEvidence::LowByteMask, kWordBytes), "uint256");

  // A boolean test on a value nothing narrowed is how any nonzero check is
  // written, so it must not be enough on its own.
  EXPECT_EQ(Resolve(ABIEvidence::BooleanTest, kWordBytes), "uint256");
}

TEST(EVMABIConstraint, PrefersSignednessAndPaddingOverArithmetic) {
  ABIConstraint Signed;
  Signed.observe(ABIEvidence::Arithmetic);
  Signed.observe(ABIEvidence::SignExtended);
  Signed.narrowTo(kWordBytes);
  EXPECT_EQ(Signed.resolve().spelling(), "int256");

  // A right-padded value is a byte string even when it is also shifted, which
  // is how a selector is taken apart.
  ABIConstraint Padded;
  Padded.observe(ABIEvidence::BitShift);
  Padded.observe(ABIEvidence::HighByteMask);
  Padded.narrowTo(kSelectorBytes);
  EXPECT_EQ(Padded.resolve().spelling(), "bytes4");
}

TEST(EVMABIEvidence, NamesEveryKind) {
  ASSERT_EQ(abiEvidenceInfos().size(), kABIEvidenceCount);
  for (const ABIEvidenceInfo &Info : abiEvidenceInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(abiEvidenceName(Info.ID), Info.Name);
    EXPECT_FALSE(Info.Summary.empty());
  }
  for (const ABITypeSourceInfo &Info : abiTypeSourceInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(abiTypeSourceName(Info.ID), Info.Name);
    EXPECT_FALSE(Info.Summary.empty());
  }
}

//===----------------------------------------------------------------------===//
// Signature text
//===----------------------------------------------------------------------===//

TEST(EVMSignatureText, SplitsOnlyAtTopLevelCommas) {
  EXPECT_TRUE(splitTypeList("").empty());

  const auto Flat = signatureArgumentTypes("transfer(address,uint256)");
  ASSERT_EQ(Flat.size(), 2u);
  EXPECT_EQ(Flat[0], "address");
  EXPECT_EQ(Flat[1], "uint256");

  EXPECT_TRUE(signatureArgumentTypes("totalSupply()").empty());

  // A tuple is one argument however many commas it contains, and so is an
  // array of tuples.
  const auto Nested = signatureArgumentTypes(
      "diamondCut((address,uint8,bytes4[])[],address,bytes)");
  ASSERT_EQ(Nested.size(), 3u);
  EXPECT_EQ(Nested[0], "(address,uint8,bytes4[])[]");
  EXPECT_EQ(Nested[1], "address");
  EXPECT_EQ(Nested[2], "bytes");

  EXPECT_EQ(signatureName("transfer(address,uint256)"), "transfer");
  EXPECT_EQ(signatureName("sync()"), "sync");
}

//===----------------------------------------------------------------------===//
// The signature dictionary
//===----------------------------------------------------------------------===//

TEST(EVMKnownSignatures, TableIsSelfConsistent) {
  llvm::Error TableError = validateKnownSignatureTables();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  for (const KnownStandardInfo &Info : knownStandardInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getKnownStandardInfo(Info.ID), &Info);
    EXPECT_FALSE(Info.Summary.empty());
  }

  for (const KnownSignatureInfo &Info : knownSignatureInfos()) {
    SCOPED_TRACE(Info.Signature.str());
    EXPECT_FALSE(Info.name().empty());
    EXPECT_EQ(Info.Topic.getBitWidth(), kWordBits);
    if (Info.Kind != SignatureKind::Function)
      EXPECT_TRUE(Info.Returns.empty());
  }
}

// The whole dictionary is only worth having if the hash it derives is the one
// deployed contracts carry, so these are the published values rather than
// anything this build computed.
TEST(EVMKnownSignatures, DerivesThePublishedSelectors) {
  struct Case {
    llvm::StringRef Signature;
    uint32_t Selector;
  };
  const Case Cases[] = {
      {"transfer(address,uint256)", 0xa9059cbbu},
      {"transferFrom(address,address,uint256)", 0x23b872ddu},
      {"approve(address,uint256)", 0x095ea7b3u},
      {"balanceOf(address)", 0x70a08231u},
      {"totalSupply()", 0x18160dddu},
      {"allowance(address,address)", 0xdd62ed3eu},
      {"name()", 0x06fdde03u},
      {"symbol()", 0x95d89b41u},
      {"decimals()", 0x313ce567u},
      {"owner()", 0x8da5cb5bu},
      {"supportsInterface(bytes4)", 0x01ffc9a7u},
      {"ownerOf(uint256)", 0x6352211eu},
      {"Error(string)", 0x08c379a0u},
      {"Panic(uint256)", 0x4e487b71u},
  };
  for (const Case &Expected : Cases) {
    SCOPED_TRACE(Expected.Signature.str());
    const KnownSignatureInfo *Info = findSignature(Expected.Signature);
    ASSERT_NE(Info, nullptr);
    EXPECT_EQ(Info->Selector, Expected.Selector);
  }
}

TEST(EVMKnownSignatures, DerivesThePublishedEventTopics) {
  const KnownSignatureInfo *Transfer =
      findSignature("Transfer(address,address,uint256)");
  ASSERT_NE(Transfer, nullptr);
  const llvm::APInt Expected(
      kWordBits,
      "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef",
      kHexRadix);
  EXPECT_EQ(Transfer->Topic, Expected);
  EXPECT_EQ(findKnownEvent(Expected), Transfer);
}

TEST(EVMKnownSignatures, LooksUpEachKindInItsOwnIndex) {
  const KnownSignatureInfo *Transfer =
      findSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);
  EXPECT_EQ(findKnownFunction(Transfer->Selector), Transfer);
  // A function is not an error, however the two selectors compare.
  EXPECT_NE(findKnownError(Transfer->Selector), Transfer);

  const KnownSignatureInfo *TransferEvent =
      findSignature("Transfer(address,address,uint256)");
  ASSERT_NE(TransferEvent, nullptr);
  EXPECT_EQ(findKnownEvent(TransferEvent->Topic), TransferEvent);

  // A selector the dictionary never derived resolves to nothing rather than to
  // whichever entry is nearest.
  EXPECT_EQ(findKnownFunction(0u), nullptr);
  EXPECT_EQ(findKnownEvent(llvm::APInt(kWordBits, 1)), nullptr);
  // A value of the wrong width cannot be a topic at all.
  EXPECT_EQ(findKnownEvent(llvm::APInt(kSelectorBits, 0)), nullptr);
}

TEST(EVMKnownSignatures, ResolvesTheLanguageRevertsByRole) {
  const KnownSignatureInfo &Message =
      getLanguageRevertInfo(LanguageRevert::Message);
  const KnownSignatureInfo &Panic =
      getLanguageRevertInfo(LanguageRevert::Panic);
  EXPECT_EQ(Message.Signature, "Error(string)");
  EXPECT_EQ(Panic.Signature, "Panic(uint256)");
  EXPECT_EQ(Message.Kind, SignatureKind::Error);
  EXPECT_EQ(Panic.Kind, SignatureKind::Error);
  EXPECT_EQ(Message.Standard, KnownStandard::Solidity);
}

TEST(EVMKnownSignatures, NamesThePanicCodesTheCompilerAssigns) {
  ASSERT_FALSE(panicCodeInfos().empty());
  for (const PanicCodeInfo &Info : panicCodeInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(findPanicCode(static_cast<uint64_t>(Info.ID)), &Info);
    EXPECT_FALSE(Info.Summary.empty());
  }
  const PanicCodeInfo *Overflow =
      findPanicCode(static_cast<uint64_t>(PanicCode::ArithmeticOverflow));
  ASSERT_NE(Overflow, nullptr);
  EXPECT_EQ(Overflow->Name, "arithmetic-overflow");

  // A newer compiler can panic with a code no table lists, and reporting the
  // number is then the honest answer.
  EXPECT_EQ(findPanicCode(0x99u), nullptr);
}

} // namespace
} // namespace neverd::evm
