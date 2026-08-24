//===- EVMABITests.cpp - Recovered ABI type and signature tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/bytecode/EVMOpcodes.h"
#include "neverd/evm/runtime/EVMABI.h"

#include "llvm/Support/Error.h"

#include <array>
#include <iterator>
#include <string>
#include <vector>

namespace neverd::evm {
namespace {

const KnownSignatureInfo *findSignature(llvm::StringRef Text) {
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Signature == Text)
      return &Info;
  return nullptr;
}

const KnownSignatureInfo *findSignature(llvm::StringRef Text,
                                        KnownStandard Standard) {
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Signature == Text &&
        ((Info.Event && Info.Event->Standard == Standard) ||
         (Info.Error && Info.Error->Standard == Standard)))
      return &Info;
  return nullptr;
}

const KnownFunctionVariantInfo *
findFunctionVariant(const KnownSignatureInfo &Function,
                    KnownStandard Standard) {
  for (const KnownFunctionVariantInfo *Variant :
       knownFunctionVariants(Function))
    if (Variant->Standard == Standard)
      return Variant;
  return nullptr;
}

llvm::APInt lowMask(unsigned Bytes) {
  return llvm::APInt::getLowBitsSet(kWordBits, Bytes * kBitsPerByte);
}

llvm::APInt highMask(unsigned Bytes) {
  return llvm::APInt::getHighBitsSet(kWordBits, Bytes * kBitsPerByte);
}

#define EVM_ABI_TEST_TEXT_64                                                   \
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define EVM_ABI_TEST_TEXT_256                                                  \
  EVM_ABI_TEST_TEXT_64 EVM_ABI_TEST_TEXT_64 EVM_ABI_TEST_TEXT_64               \
      EVM_ABI_TEST_TEXT_64
#define EVM_ABI_TEST_TEXT_1024                                                 \
  EVM_ABI_TEST_TEXT_256 EVM_ABI_TEST_TEXT_256 EVM_ABI_TEST_TEXT_256            \
      EVM_ABI_TEST_TEXT_256
constexpr llvm::StringLiteral kABITableTextLimitBlock = EVM_ABI_TEST_TEXT_1024;
#undef EVM_ABI_TEST_TEXT_1024
#undef EVM_ABI_TEST_TEXT_256
#undef EVM_ABI_TEST_TEXT_64

static_assert(kABITableTextLimitBlock.size() == 1'024);
static_assert(kMaxKnownSignatureTableTextBytes %
                      kABITableTextLimitBlock.size() ==
                  0,
              "the exact-boundary fixture must tile the text-byte budget");

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

  // Public splitting helpers share the canonical parser rather than trying to
  // recover partial members from malformed delimiter or type syntax.
  EXPECT_TRUE(signatureArgumentTypes("f([))").empty());
  EXPECT_TRUE(splitTypeList("uint256,(address]").empty());
}

TEST(EVMSignatureText, BoundsHostileTypeGrammarAtExactLimits) {
  const auto NestedType = [](size_t Depth) {
    std::string Type(Depth, '(');
    Type += "uint256";
    Type.append(Depth, ')');
    return Type;
  };
  const auto ArrayType = [](size_t Dimensions) {
    std::string Type = "uint256";
    for (size_t Dimension = 0; Dimension < Dimensions; ++Dimension)
      Type += "[]";
    return Type;
  };
  const auto TypeList = [](size_t Count) {
    std::string Types;
    for (size_t Index = 0; Index < Count; ++Index) {
      if (Index != 0)
        Types += ',';
      Types += "uint256";
    }
    return Types;
  };

  EXPECT_EQ(splitTypeList(NestedType(kMaxABITypeNestingDepth)).size(), 1u);
  EXPECT_TRUE(splitTypeList(NestedType(kMaxABITypeNestingDepth + 1)).empty());

  EXPECT_EQ(splitTypeList(ArrayType(kMaxABIArrayDimensions)).size(), 1u);
  EXPECT_TRUE(splitTypeList(ArrayType(kMaxABIArrayDimensions + 1)).empty());

  EXPECT_EQ(splitTypeList(TypeList(kMaxABITypeNodes)).size(), kMaxABITypeNodes);
  EXPECT_TRUE(splitTypeList(TypeList(kMaxABITypeNodes + 1)).empty());
}

//===----------------------------------------------------------------------===//
// The signature dictionary
//===----------------------------------------------------------------------===//

TEST(EVMKnownSignatures, TableIsSelfConsistent) {
  llvm::Error TableError = validateKnownSignatureTables();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  ASSERT_EQ(knownStandardInfos().size(), kKnownStandardCount);
  for (size_t Index = 0; Index < knownStandardInfos().size(); ++Index) {
    const KnownStandardInfo &Info = knownStandardInfos()[Index];
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(static_cast<size_t>(Info.ID), Index);
    EXPECT_EQ(&getKnownStandardInfo(Info.ID), &Info);
    EXPECT_FALSE(Info.Summary.empty());
    EXPECT_GE(standardSelectorEvidenceCount(Info.MinimumIndependentSelectors),
              kMinimumIndependentSelectorsForStandard);
  }

  for (const KnownSignatureInfo &Info : knownSignatureInfos()) {
    SCOPED_TRACE(Info.Signature.str());
    EXPECT_FALSE(Info.name().empty());
    EXPECT_EQ(Info.Topic.getBitWidth(), kWordBits);
    switch (Info.Kind) {
    case SignatureKind::Function:
      EXPECT_FALSE(Info.Event.has_value());
      EXPECT_FALSE(Info.Error.has_value());
      EXPECT_FALSE(knownFunctionVariants(Info).empty());
      break;
    case SignatureKind::Event:
      ASSERT_TRUE(Info.Event.has_value());
      EXPECT_FALSE(Info.Error.has_value());
      EXPECT_LE(Info.Event->totalTopicCount(), logTopicCount(Opcode::LOG4));
      break;
    case SignatureKind::Error:
      EXPECT_FALSE(Info.Event.has_value());
      EXPECT_TRUE(Info.Error.has_value());
      break;
    }
  }

  for (const KnownFunctionVariantInfo &Variant : knownFunctionVariantInfos()) {
    ASSERT_NE(Variant.Function, nullptr);
    EXPECT_EQ(Variant.Function->Kind, SignatureKind::Function);
    EXPECT_NE(findFunctionVariant(*Variant.Function, Variant.Standard),
              nullptr);
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

TEST(EVMKnownSignatures, SeparatesCanonicalFunctionsFromPerStandardVariants) {
  struct Case {
    llvm::StringRef Signature;
    llvm::StringRef ERC20Returns;
    llvm::StringRef ERC721Returns;
  };
  const Case Cases[] = {
      {"totalSupply()", "uint256", "uint256"},
      {"balanceOf(address)", "uint256", "uint256"},
      {"transferFrom(address,address,uint256)", "bool", ""},
      {"approve(address,uint256)", "bool", ""},
      {"name()", "string", "string"},
      {"symbol()", "string", "string"},
  };

  for (const Case &Expected : Cases) {
    SCOPED_TRACE(Expected.Signature.str());
    const KnownSignatureInfo *Function = findSignature(Expected.Signature);
    ASSERT_NE(Function, nullptr);
    ASSERT_EQ(Function->Kind, SignatureKind::Function);
    EXPECT_EQ(findKnownFunction(Function->Selector), Function);

    size_t CanonicalCount = 0;
    for (const KnownSignatureInfo &Info : knownSignatureInfos())
      CanonicalCount += Info.Kind == SignatureKind::Function &&
                        Info.Signature == Expected.Signature;
    EXPECT_EQ(CanonicalCount, 1u);

    const auto Variants = knownFunctionVariants(*Function);
    ASSERT_EQ(Variants.size(), 2u);
    const KnownFunctionVariantInfo *ERC20 =
        findFunctionVariant(*Function, KnownStandard::ERC20);
    const KnownFunctionVariantInfo *ERC721 =
        findFunctionVariant(*Function, KnownStandard::ERC721);
    ASSERT_NE(ERC20, nullptr);
    ASSERT_NE(ERC721, nullptr);
    EXPECT_EQ(ERC20->Returns, Expected.ERC20Returns);
    EXPECT_EQ(ERC721->Returns, Expected.ERC721Returns);
    EXPECT_FALSE(ERC20->contributesIndependentSelectorEvidence());
    EXPECT_FALSE(ERC721->contributesIndependentSelectorEvidence());
  }
}

TEST(EVMKnownSignatures, RecordsIndependentSelectorEvidenceDeclaratively) {
  const KnownSignatureInfo *Transfer =
      findSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);
  const auto Variants = knownFunctionVariants(*Transfer);
  ASSERT_EQ(Variants.size(), 1u);
  EXPECT_EQ(Variants.front()->Standard, KnownStandard::ERC20);
  EXPECT_EQ(Variants.front()->Returns, "bool");
  EXPECT_TRUE(Variants.front()->contributesIndependentSelectorEvidence());
}

TEST(EVMKnownSignatures, SelectorCollisionsFailClosedIndependentOfOrder) {
  // These two published canonical spellings share selector 0x42966c68 while
  // retaining distinct full Keccak-256 digests.
  const std::array<KnownSignatureInfo, 2> Collisions = {
      KnownSignatureInfo{SignatureKind::Function, "burn(uint256)", std::nullopt,
                         std::nullopt, 0x42966c68u,
                         llvm::APInt(kWordBits,
                                     "42966c689b5afe9b9b3f8a7103b2a19980d59629b"
                                     "fd6a20a60972312ed41d836",
                                     kHexRadix)},
      KnownSignatureInfo{SignatureKind::Function,
                         "collate_propagate_storage(bytes16)", std::nullopt,
                         std::nullopt, 0x42966c68u,
                         llvm::APInt(kWordBits,
                                     "42966c684c869f2e2e56c232c00c827b4e8fcd111"
                                     "9259bd54a90d765bf17bbd6",
                                     kHexRadix)},
  };
  const std::array<KnownFunctionVariantInfo, 2> Variants = {
      KnownFunctionVariantInfo{&Collisions[0], KnownStandard::OpenZeppelin, "",
                               FunctionSelectorEvidence::Independent},
      KnownFunctionVariantInfo{&Collisions[1], KnownStandard::Common, "",
                               FunctionSelectorEvidence::NonIndependent},
  };

  EXPECT_EQ(findUniqueKnownSignature(Collisions, SignatureKind::Function,
                                     Collisions.front().Selector),
            nullptr);
  const std::array<KnownSignatureInfo, 2> Reversed = {Collisions[1],
                                                      Collisions[0]};
  EXPECT_EQ(findUniqueKnownSignature(Reversed, SignatureKind::Function,
                                     Collisions.front().Selector),
            nullptr);

  llvm::Error Error = validateKnownSignatureTables(Collisions, Variants);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("not the single function"),
            std::string::npos);

  const std::array<KnownFunctionVariantInfo, 2> ReversedVariants = {
      KnownFunctionVariantInfo{&Reversed[0], KnownStandard::Common, "",
                               FunctionSelectorEvidence::NonIndependent},
      KnownFunctionVariantInfo{&Reversed[1], KnownStandard::OpenZeppelin, "",
                               FunctionSelectorEvidence::Independent},
  };
  llvm::Error ReversedError =
      validateKnownSignatureTables(Reversed, ReversedVariants);
  ASSERT_TRUE(static_cast<bool>(ReversedError));
  EXPECT_NE(
      llvm::toString(std::move(ReversedError)).find("not the single function"),
      std::string::npos);
}

TEST(EVMKnownSignatures, TableValidationRejectsInvalidSignatureKind) {
  const KnownSignatureInfo *Known = findSignature("transfer(address,uint256)");
  ASSERT_NE(Known, nullptr);
  std::array<KnownSignatureInfo, 1> Signatures = {*Known};
  Signatures.front().Kind =
      static_cast<SignatureKind>(signatureKindInfos().size());

  llvm::Error Error = validateKnownSignatureTables(Signatures, {});
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("invalid signature kind"),
            std::string::npos);
}

TEST(EVMKnownSignatures,
     TableValidationRejectsInvalidEnumsMetadataAndTopicWidth) {
  const KnownSignatureInfo *Known = findSignature("transfer(address,uint256)");
  ASSERT_NE(Known, nullptr);

  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Event = KnownEventMetadata{};
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error))
                  .find("function carries non-function metadata"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Kind = SignatureKind::Event;
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error))
                  .find("event has invalid event metadata"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Kind = SignatureKind::Event;
    Signatures.front().Event = KnownEventMetadata{};
    Signatures.front().Error = KnownErrorMetadata{};
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error))
                  .find("event has invalid event metadata"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Kind = SignatureKind::Error;
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error))
                  .find("error has invalid error metadata"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Kind = SignatureKind::Error;
    Signatures.front().Event = KnownEventMetadata{};
    Signatures.front().Error = KnownErrorMetadata{};
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error))
                  .find("error has invalid error metadata"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Kind = SignatureKind::Event;
    Signatures.front().Event =
        KnownEventMetadata{static_cast<KnownStandard>(kKnownStandardCount), {}};
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("invalid standard"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Kind = SignatureKind::Error;
    Signatures.front().Error =
        KnownErrorMetadata{static_cast<KnownStandard>(kKnownStandardCount)};
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("invalid standard"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    Signatures.front().Topic = llvm::APInt(kWordBits / 2, 0);
    llvm::Error Error = validateKnownSignatureTables(Signatures, {});
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("non-word-sized topic"),
              std::string::npos);
  }
  {
    std::array<KnownSignatureInfo, 1> Signatures = {*Known};
    const std::array<KnownFunctionVariantInfo, 1> Variants = {
        KnownFunctionVariantInfo{
            &Signatures.front(),
            static_cast<KnownStandard>(kKnownStandardCount), "",
            FunctionSelectorEvidence::Independent},
    };
    llvm::Error Error = validateKnownSignatureTables(Signatures, Variants);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("invalid standard"),
              std::string::npos);
  }
}

TEST(EVMKnownSignatures, TableValidationBoundsCardinalityAndTextAtExactLimits) {
  const KnownSignatureInfo InvalidSignature{
      SignatureKind::Function,  "", std::nullopt, std::nullopt, 0,
      llvm::APInt(kWordBits, 0)};
  const KnownFunctionVariantInfo InvalidVariant{
      nullptr, KnownStandard::Common, "",
      FunctionSelectorEvidence::NonIndependent};

  std::vector<KnownSignatureInfo> ExactSignatures(
      kMaxKnownSignatureTableEntries, InvalidSignature);
  llvm::Error ExactSignatureError =
      validateKnownSignatureTables(ExactSignatures, {});
  ASSERT_TRUE(static_cast<bool>(ExactSignatureError));
  EXPECT_EQ(llvm::toString(std::move(ExactSignatureError))
                .find("signature table exceeds its entry limit"),
            std::string::npos);
  ExactSignatures.push_back(InvalidSignature);
  llvm::Error ExtraSignatureError =
      validateKnownSignatureTables(ExactSignatures, {});
  ASSERT_TRUE(static_cast<bool>(ExtraSignatureError));
  EXPECT_NE(llvm::toString(std::move(ExtraSignatureError))
                .find("signature table exceeds its entry limit"),
            std::string::npos);

  std::vector<KnownFunctionVariantInfo> ExactVariants(
      kMaxKnownFunctionVariantTableEntries, InvalidVariant);
  llvm::Error ExactVariantError =
      validateKnownSignatureTables({}, ExactVariants);
  ASSERT_TRUE(static_cast<bool>(ExactVariantError));
  EXPECT_EQ(llvm::toString(std::move(ExactVariantError))
                .find("function variant table exceeds its entry limit"),
            std::string::npos);
  ExactVariants.push_back(InvalidVariant);
  llvm::Error ExtraVariantError =
      validateKnownSignatureTables({}, ExactVariants);
  ASSERT_TRUE(static_cast<bool>(ExtraVariantError));
  EXPECT_NE(llvm::toString(std::move(ExtraVariantError))
                .find("function variant table exceeds its entry limit"),
            std::string::npos);

  const KnownSignatureInfo TextBlock{SignatureKind::Function,
                                     kABITableTextLimitBlock,
                                     std::nullopt,
                                     std::nullopt,
                                     0,
                                     llvm::APInt(kWordBits, 0)};
  const size_t TextBlockCount =
      kMaxKnownSignatureTableTextBytes / kABITableTextLimitBlock.size();
  ASSERT_LE(TextBlockCount, kMaxKnownSignatureTableEntries);
  std::vector<KnownSignatureInfo> ExactText(TextBlockCount, TextBlock);
  llvm::Error ExactTextError = validateKnownSignatureTables(ExactText, {});
  ASSERT_TRUE(static_cast<bool>(ExactTextError));
  EXPECT_EQ(llvm::toString(std::move(ExactTextError))
                .find("signature table text exceeds its byte limit"),
            std::string::npos);
  const std::array<KnownFunctionVariantInfo, 1> ExtraText = {
      KnownFunctionVariantInfo{nullptr, KnownStandard::Common, "x",
                               FunctionSelectorEvidence::NonIndependent},
  };
  llvm::Error ExtraTextError =
      validateKnownSignatureTables(ExactText, ExtraText);
  ASSERT_TRUE(static_cast<bool>(ExtraTextError));
  EXPECT_NE(llvm::toString(std::move(ExtraTextError))
                .find("signature table text exceeds its byte limit"),
            std::string::npos);
}

TEST(EVMKnownSignatures, TableValidationRejectsDuplicateMemberships) {
  std::vector<KnownFunctionVariantInfo> Variants(
      knownFunctionVariantInfos().begin(), knownFunctionVariantInfos().end());
  ASSERT_FALSE(Variants.empty());
  Variants.push_back(Variants.front());

  llvm::Error Error =
      validateKnownSignatureTables(knownSignatureInfos(), Variants);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("repeats a function variant"),
            std::string::npos);
}

TEST(EVMKnownSignatures, TableValidationRejectsMismatchedTypeDelimiters) {
  const std::array<KnownSignatureInfo, 1> Signatures = {
      KnownSignatureInfo{SignatureKind::Function, "f([))", std::nullopt,
                         std::nullopt, 0, llvm::APInt(kWordBits, 0)},
  };
  const std::array<KnownFunctionVariantInfo, 1> Variants = {
      KnownFunctionVariantInfo{&Signatures.front(), KnownStandard::Common, "",
                               FunctionSelectorEvidence::NonIndependent},
  };

  llvm::Error Error = validateKnownSignatureTables(Signatures, Variants);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(
      llvm::toString(std::move(Error)).find("not a canonical ABI signature"),
      std::string::npos);
}

TEST(EVMKnownSignatures, TableValidationRejectsNonCanonicalArgumentTypes) {
  constexpr llvm::StringLiteral InvalidSignatures[] = {
      "f(uint)",           "f(uint257)",     "f(int0)",
      "f(bytes0)",         "f(bytes33)",     "f(tuple)",
      "f(uint256[01])",    "f(uint256[-1])", "f(address memory)",
      "f(uint256 amount)", "f((uint256,))",  "f(uint256,,bool)",
      "1f(uint256)",
  };

  for (llvm::StringLiteral Signature : InvalidSignatures) {
    SCOPED_TRACE(Signature.str());
    const std::array<KnownSignatureInfo, 1> Signatures = {
        KnownSignatureInfo{SignatureKind::Function, Signature, std::nullopt,
                           std::nullopt, 0, llvm::APInt(kWordBits, 0)},
    };
    const std::array<KnownFunctionVariantInfo, 1> Variants = {
        KnownFunctionVariantInfo{&Signatures.front(), KnownStandard::Common, "",
                                 FunctionSelectorEvidence::NonIndependent},
    };

    llvm::Error Error = validateKnownSignatureTables(Signatures, Variants);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(
        llvm::toString(std::move(Error)).find("not a canonical ABI signature"),
        std::string::npos);
  }
}

TEST(EVMKnownSignatures, TableValidationRejectsNonCanonicalReturnTypes) {
  const KnownSignatureInfo *Known = findSignature("transfer(address,uint256)");
  ASSERT_NE(Known, nullptr);
  const std::array<KnownSignatureInfo, 1> Signatures = {*Known};
  constexpr llvm::StringLiteral InvalidReturnLists[] = {
      "uint257", "bytes33", "uint256,", "(address,)", "address memory",
  };

  for (llvm::StringLiteral Returns : InvalidReturnLists) {
    SCOPED_TRACE(Returns.str());
    const std::array<KnownFunctionVariantInfo, 1> Variants = {
        KnownFunctionVariantInfo{&Signatures.front(), KnownStandard::ERC20,
                                 Returns,
                                 FunctionSelectorEvidence::Independent},
    };

    llvm::Error Error = validateKnownSignatureTables(Signatures, Variants);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(
        llvm::toString(std::move(Error)).find("non-canonical ABI return list"),
        std::string::npos);
  }
}

TEST(EVMKnownSignatures, TableValidationAcceptsTheCanonicalABITypeGrammar) {
  const KnownSignatureInfo *Known = findSignature("transfer(address,uint256)");
  ASSERT_NE(Known, nullptr);
  const std::array<KnownSignatureInfo, 1> Signatures = {*Known};
  constexpr llvm::StringLiteral ValidReturnLists[] = {
      "",
      "function",
      "fixed128x18",
      "ufixed256x80",
      "bytes3[2]",
      "(address,function)[0][]",
      "()",
      "uint8,int256,bytes32,string,bytes,bool,address",
  };

  for (llvm::StringLiteral Returns : ValidReturnLists) {
    SCOPED_TRACE(Returns.str());
    const std::array<KnownFunctionVariantInfo, 1> Variants = {
        KnownFunctionVariantInfo{&Signatures.front(), KnownStandard::ERC20,
                                 Returns,
                                 FunctionSelectorEvidence::Independent},
    };

    llvm::Error Error = validateKnownSignatureTables(Signatures, Variants);
    EXPECT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  }
}

TEST(EVMKnownSignatures, TableValidationRejectsIndependentSharedSelectors) {
  std::vector<KnownFunctionVariantInfo> Variants(
      knownFunctionVariantInfos().begin(), knownFunctionVariantInfos().end());
  const KnownSignatureInfo *Balance = findSignature("balanceOf(address)");
  ASSERT_NE(Balance, nullptr);
  const auto It =
      llvm::find_if(Variants, [&](KnownFunctionVariantInfo &Variant) {
        return Variant.Function == Balance;
      });
  ASSERT_NE(It, Variants.end());
  It->Evidence = FunctionSelectorEvidence::Independent;

  llvm::Error Error =
      validateKnownSignatureTables(knownSignatureInfos(), Variants);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(
      llvm::toString(std::move(Error)).find("shared but marked independent"),
      std::string::npos);
}

TEST(EVMKnownSignatures, DerivesThePublishedEventTopics) {
  const KnownSignatureInfo *Transfer =
      findSignature("Transfer(address,address,uint256)", KnownStandard::ERC20);
  ASSERT_NE(Transfer, nullptr);
  const llvm::APInt Expected(
      kWordBits,
      "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef",
      kHexRadix);
  EXPECT_EQ(Transfer->Topic, Expected);
  ASSERT_TRUE(Transfer->Event.has_value());
  EXPECT_EQ(findKnownEvent(Expected, Transfer->Event->totalTopicCount()),
            Transfer);
}

TEST(EVMKnownSignatures, RecordsPublishedIndexedEventArguments) {
  struct Case {
    llvm::StringRef Signature;
    KnownStandard Standard;
    unsigned IndexedArguments;
  };
  const Case Cases[] = {
      {"Transfer(address,address,uint256)", KnownStandard::ERC20, 2},
      {"Approval(address,address,uint256)", KnownStandard::ERC20, 2},
      {"Transfer(address,address,uint256)", KnownStandard::ERC721, 3},
      {"Approval(address,address,uint256)", KnownStandard::ERC721, 3},
      {"ApprovalForAll(address,address,bool)", KnownStandard::ERC721, 2},
      {"TransferSingle(address,address,address,uint256,uint256)",
       KnownStandard::ERC1155, 3},
      {"TransferBatch(address,address,address,uint256[],uint256[])",
       KnownStandard::ERC1155, 3},
      {"ApprovalForAll(address,address,bool)", KnownStandard::ERC1155, 2},
      {"URI(string,uint256)", KnownStandard::ERC1155, 1},
      {"Deposit(address,address,uint256,uint256)", KnownStandard::ERC4626, 2},
      {"Withdraw(address,address,address,uint256,uint256)",
       KnownStandard::ERC4626, 3},
      {"Upgraded(address)", KnownStandard::ERC1967, 1},
      {"AdminChanged(address,address)", KnownStandard::ERC1967, 0},
      {"BeaconUpgraded(address)", KnownStandard::ERC1967, 1},
      {"DiamondCut((address,uint8,bytes4[])[],address,bytes)",
       KnownStandard::ERC2535, 0},
      {"OwnershipTransferred(address,address)", KnownStandard::OpenZeppelin, 2},
      {"OwnershipTransferStarted(address,address)", KnownStandard::OpenZeppelin,
       2},
      {"RoleGranted(bytes32,address,address)", KnownStandard::OpenZeppelin, 3},
      {"RoleRevoked(bytes32,address,address)", KnownStandard::OpenZeppelin, 3},
      {"RoleAdminChanged(bytes32,bytes32,bytes32)", KnownStandard::OpenZeppelin,
       3},
      {"Paused(address)", KnownStandard::OpenZeppelin, 0},
      {"Unpaused(address)", KnownStandard::OpenZeppelin, 0},
      {"Initialized(uint64)", KnownStandard::OpenZeppelin, 0},
      {"Deposit(address,uint256)", KnownStandard::WrappedEther, 1},
      {"Withdrawal(address,uint256)", KnownStandard::WrappedEther, 1},
      {"Sync(uint112,uint112)", KnownStandard::UniswapV2, 0},
      {"Swap(address,uint256,uint256,uint256,uint256,address)",
       KnownStandard::UniswapV2, 2},
      {"Mint(address,uint256,uint256)", KnownStandard::UniswapV2, 1},
      {"Burn(address,uint256,uint256,address)", KnownStandard::UniswapV2, 2},
      {"PairCreated(address,address,address,uint256)", KnownStandard::UniswapV2,
       2},
      {"Initialize(uint160,int24)", KnownStandard::UniswapV3, 0},
      {"Swap(address,address,int256,int256,uint160,uint128,int24)",
       KnownStandard::UniswapV3, 2},
      {"Mint(address,address,int24,int24,uint128,uint256,uint256)",
       KnownStandard::UniswapV3, 3},
      {"Burn(address,int24,int24,uint128,uint256,uint256)",
       KnownStandard::UniswapV3, 3},
  };

  size_t EventEntries = 0;
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Kind == SignatureKind::Event)
      ++EventEntries;
  EXPECT_EQ(EventEntries, std::size(Cases));

  for (const Case &Expected : Cases) {
    SCOPED_TRACE(Expected.Signature.str());
    const KnownSignatureInfo *Info =
        findSignature(Expected.Signature, Expected.Standard);
    ASSERT_NE(Info, nullptr);
    ASSERT_EQ(Info->Kind, SignatureKind::Event);
    ASSERT_TRUE(Info->Event.has_value());
    EXPECT_EQ(eventIndexedArgumentCount(Info->Event->IndexedArguments),
              Expected.IndexedArguments);
  }
}

TEST(EVMKnownSignatures, ResolvesSharedEventVariantsByTopicArity) {
  const llvm::StringRef Signatures[] = {
      "Transfer(address,address,uint256)",
      "Approval(address,address,uint256)",
  };
  for (llvm::StringRef Signature : Signatures) {
    SCOPED_TRACE(Signature.str());
    const KnownSignatureInfo *ERC20 =
        findSignature(Signature, KnownStandard::ERC20);
    const KnownSignatureInfo *ERC721 =
        findSignature(Signature, KnownStandard::ERC721);
    ASSERT_NE(ERC20, nullptr);
    ASSERT_NE(ERC721, nullptr);
    ASSERT_TRUE(ERC20->Event.has_value());
    ASSERT_TRUE(ERC721->Event.has_value());
    EXPECT_EQ(ERC20->Topic, ERC721->Topic);
    EXPECT_EQ(findKnownEvent(ERC20->Topic, ERC20->Event->totalTopicCount()),
              ERC20);
    EXPECT_EQ(findKnownEvent(ERC721->Topic, ERC721->Event->totalTopicCount()),
              ERC721);
  }
}

TEST(EVMKnownSignatures, RejectsAmbiguousObservableEventVariants) {
  const KnownSignatureInfo *ERC721Approval = findSignature(
      "ApprovalForAll(address,address,bool)", KnownStandard::ERC721);
  const KnownSignatureInfo *ERC1155Approval = findSignature(
      "ApprovalForAll(address,address,bool)", KnownStandard::ERC1155);
  ASSERT_NE(ERC721Approval, nullptr);
  ASSERT_NE(ERC1155Approval, nullptr);
  ASSERT_TRUE(ERC721Approval->Event.has_value());
  ASSERT_TRUE(ERC1155Approval->Event.has_value());
  EXPECT_EQ(ERC721Approval->Topic, ERC1155Approval->Topic);
  EXPECT_EQ(ERC721Approval->Event->IndexedArguments,
            ERC1155Approval->Event->IndexedArguments);
  EXPECT_EQ(findKnownEvent(ERC721Approval->Topic,
                           ERC721Approval->Event->totalTopicCount()),
            nullptr);
}

TEST(EVMKnownSignatures, LooksUpEachKindInItsOwnIndex) {
  const KnownSignatureInfo *Transfer =
      findSignature("transfer(address,uint256)");
  ASSERT_NE(Transfer, nullptr);
  EXPECT_EQ(findKnownFunction(Transfer->Selector), Transfer);
  // A function is not an error, however the two selectors compare.
  EXPECT_NE(findKnownError(Transfer->Selector), Transfer);

  const KnownSignatureInfo *TransferEvent =
      findSignature("Transfer(address,address,uint256)", KnownStandard::ERC20);
  ASSERT_NE(TransferEvent, nullptr);
  ASSERT_TRUE(TransferEvent->Event.has_value());
  EXPECT_EQ(findKnownEvent(TransferEvent->Topic,
                           TransferEvent->Event->totalTopicCount()),
            TransferEvent);

  // A selector the dictionary never derived resolves to nothing rather than to
  // whichever entry is nearest.
  EXPECT_EQ(findKnownFunction(0u), nullptr);
  EXPECT_EQ(
      findKnownEvent(llvm::APInt(kWordBits, 1), logTopicCount(Opcode::LOG1)),
      nullptr);
  // A value of the wrong width cannot be a topic at all.
  EXPECT_EQ(findKnownEvent(llvm::APInt(kSelectorBits, 0),
                           logTopicCount(Opcode::LOG1)),
            nullptr);
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
  ASSERT_TRUE(Message.Error.has_value());
  EXPECT_EQ(Message.Error->Standard, KnownStandard::Solidity);
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
