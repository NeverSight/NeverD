//===- EVMBytecodeTests.cpp - EVM bytecode input tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/Bytecode.h"
#include "neverd/evm/Calls.h"
#include "neverd/evm/Decoder.h"
#include "neverd/evm/EVMConstants.h"

#include "llvm/Support/Error.h"

namespace neverd::evm {
namespace {

TEST(EVMBytecode, DecodesPrefixedHexText) {
  auto Loaded = decodeBytecodeInput("0x6001600055", "contract.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));
  EXPECT_EQ(Loaded->Source, BytecodeSourceKind::Hex);
  EXPECT_EQ(Loaded->Original.size(), 5u);
  EXPECT_EQ(Loaded->Container, BytecodeContainer::Legacy);
}

TEST(EVMBytecode, AcceptsWhitespaceButRejectsMalformedHex) {
  auto Loaded = decodeBytecodeInput(" \n 0x60 01\t60 00 55 \r\n", "stdin");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));

  auto Odd = decodeBytecodeInput("0x600", "odd.hex");
  ASSERT_FALSE(static_cast<bool>(Odd));
  EXPECT_NE(llvm::toString(Odd.takeError()).find("odd number"),
            std::string::npos);

  auto Placeholder = decodeBytecodeInput("0x60__$abcd$__", "linked.hex");
  ASSERT_FALSE(static_cast<bool>(Placeholder));
  EXPECT_NE(llvm::toString(Placeholder.takeError()).find("placeholder"),
            std::string::npos);

  auto Empty = decodeBytecodeInput(" \n\t ", "empty.hex");
  ASSERT_FALSE(static_cast<bool>(Empty));
  EXPECT_NE(llvm::toString(Empty.takeError()).find("empty bytecode"),
            std::string::npos);
}

TEST(EVMBytecode, PreservesRawBinaryWhenExplicitlyRequested) {
  const char Bytes[] = {'\x60', '\0', '\x7f', '\xff'};
  BytecodeLoadOptions Options;
  Options.Format = BytecodeInputFormat::Raw;
  Options.ExtractRuntime = false;
  Options.StripMetadata = false;
  auto Loaded = decodeBytecodeInput(llvm::StringRef(Bytes, sizeof(Bytes)),
                                    "contract.evm.raw", Options);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x00, 0x7f, 0xff}));
  EXPECT_EQ(Loaded->Source, BytecodeSourceKind::Raw);
}

TEST(EVMBytecode, PrefersDeployedBytecodeFromCompilerArtifacts) {
  constexpr llvm::StringLiteral Artifact = R"json({
    "bytecode": {"object": "0x6000"},
    "deployedBytecode": {"object": "0x6001600055"}
  })json";
  auto Loaded = decodeBytecodeInput(Artifact, "Counter.json");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));
  EXPECT_EQ(Loaded->Source, BytecodeSourceKind::Artifact);
  EXPECT_FALSE(Loaded->RuntimeExtracted);
}

TEST(EVMBytecode, IgnoresEmptyRuntimeFieldsInCompilerArtifacts) {
  constexpr llvm::StringLiteral Artifact = R"json({
    "bytecode": {"object": "0x6000"},
    "deployedBytecode": {"object": "0x"}
  })json";
  auto Loaded = decodeBytecodeInput(Artifact, "CreationOnly.json");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x00}));
  EXPECT_EQ(Loaded->Source, BytecodeSourceKind::Artifact);
  EXPECT_FALSE(Loaded->RuntimeExtracted);
}

TEST(EVMBytecode, SelectsAContractFromSolcStandardJson) {
  constexpr llvm::StringLiteral Artifact = R"json({
    "contracts": {"src/C.sol": {"Counter": {"evm": {
      "bytecode": {"object": "0x6000"},
      "deployedBytecode": {"object": "0x602a60005260206000f3"}
    }}}}
  })json";
  BytecodeLoadOptions Options;
  Options.ArtifactContract = "src/C.sol:Counter";
  auto Loaded = decodeBytecodeInput(Artifact, "standard.json", Options);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x2a, 0x60, 0x00, 0x52,
                                                0x60, 0x20, 0x60, 0x00, 0xf3}));
}

TEST(EVMBytecode, SupportsNestedArtifactsAndRejectsAmbiguousContracts) {
  constexpr llvm::StringLiteral Artifact = R"json({
    "contracts": {
      "src/A.sol": {"A": {"evm": {"deployedBytecode": {
        "object": "0x6000"
      }}}},
      "src/B.sol": {"B": {"evm": {"deployedBytecode": {
        "object": "0x6001"
      }}}}
    }
  })json";
  auto Ambiguous = decodeBytecodeInput(Artifact, "multi.json");
  ASSERT_FALSE(static_cast<bool>(Ambiguous));
  EXPECT_NE(llvm::toString(Ambiguous.takeError()).find("multiple contracts"),
            std::string::npos);

  BytecodeLoadOptions Options;
  Options.ArtifactContract = "B";
  auto Selected = decodeBytecodeInput(Artifact, "multi.json", Options);
  ASSERT_TRUE(static_cast<bool>(Selected))
      << llvm::toString(Selected.takeError());
  EXPECT_EQ(Selected->Code, (std::vector<uint8_t>{0x60, 0x01}));
}

TEST(EVMBytecode, RequiresQualifiedSelectorForDuplicateContractNames) {
  constexpr llvm::StringLiteral Artifact = R"json({
    "contracts": {
      "src/A.sol": {"Token": {"evm": {"deployedBytecode": {
        "object": "0x6000"
      }}}},
      "src/B.sol": {"Token": {"evm": {"deployedBytecode": {
        "object": "0x6001"
      }}}}
    }
  })json";
  BytecodeLoadOptions Options;
  Options.ArtifactContract = "Token";
  auto Ambiguous = decodeBytecodeInput(Artifact, "duplicate.json", Options);
  ASSERT_FALSE(static_cast<bool>(Ambiguous));
  EXPECT_NE(llvm::toString(Ambiguous.takeError()).find("ambiguous"),
            std::string::npos);

  Options.ArtifactContract = "src/B.sol:Token";
  auto Selected = decodeBytecodeInput(Artifact, "duplicate.json", Options);
  ASSERT_TRUE(static_cast<bool>(Selected))
      << llvm::toString(Selected.takeError());
  EXPECT_EQ(Selected->Code, (std::vector<uint8_t>{0x60, 0x01}));
}

TEST(EVMBytecode, ExtractsStaticRuntimeFromCreationCode) {
  // PUSH1 5; PUSH1 12; PUSH1 0; CODECOPY; PUSH1 5; PUSH1 0; RETURN
  // followed by the five-byte deployed program.
  auto Loaded =
      decodeBytecodeInput("6005600c60003960056000f36001600055", "creation.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));
  EXPECT_TRUE(Loaded->RuntimeExtracted);
  EXPECT_EQ(Loaded->Original.size(), 17u);
}

TEST(EVMBytecode, DoesNotExtractRuntimeModifiedAfterCodecopy) {
  // Copy the final STOP byte to memory[0], overwrite it with MSTORE8, then
  // return the modified byte. Static slicing must not claim the copied STOP is
  // the deployed runtime.
  constexpr llvm::StringLiteral Creation = "6001600e5f3960aa5f5360015ff300";
  auto Loaded = decodeBytecodeInput(Creation, "modified-creation.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_FALSE(Loaded->RuntimeExtracted);
  EXPECT_EQ(Loaded->Code.size(), Creation.size() / kHexDigitsPerByte);
}

TEST(EVMBytecode, DoesNotMistakeRuntimeCodecopyForConstructorWrapper) {
  // This executable program returns a byte copied from the beginning of its
  // own code. A constructor wrapper must copy an embedded runtime located
  // after the wrapper's terminal RETURN, not reinterpret earlier code.
  constexpr llvm::StringLiteral Runtime = "600160005f3960015ff3";
  auto Loaded = decodeBytecodeInput(Runtime, "runtime-codecopy.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_FALSE(Loaded->RuntimeExtracted);
  EXPECT_EQ(Loaded->Code.size(), Runtime.size() / kHexDigitsPerByte);
}

TEST(EVMBytecode, RuntimeExtractionStopsAtTheFirstTerminalInstruction) {
  // The first RETURN ends constructor execution. Bytes after it deliberately
  // resemble a complete CODECOPY/RETURN wrapper, but are unreachable data and
  // must never become the selected runtime.
  constexpr llvm::StringLiteral Creation = "5f5ff36001600d5f3960015ff300";
  auto Loaded = decodeBytecodeInput(Creation, "terminal-creation.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_FALSE(Loaded->RuntimeExtracted);
  EXPECT_EQ(Loaded->Code.size(), Creation.size() / kHexDigitsPerByte);
}

TEST(EVMBytecode, StripsValidatedSolidityMetadataTrailer) {
  // runtime 6000 + CBOR {"solc": h'00081e'} + uint16 metadata length.
  auto Loaded =
      decodeBytecodeInput("6000a164736f6c634300081e000a", "metadata.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x00}));
  EXPECT_TRUE(Loaded->MetadataStripped);
  EXPECT_EQ(Loaded->Original.size(), 14u);

  // A length-looking suffix without a CBOR map and compiler key is code.
  Loaded = decodeBytecodeInput("600160020002", "not-metadata.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code,
            (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x02, 0x00, 0x02}));
  EXPECT_FALSE(Loaded->MetadataStripped);
}

TEST(EVMBytecode, ReportsWhoBuiltTheContractAndFromWhat) {
  // runtime 6000 + CBOR {"ipfs": h'1220 00..1f', "solc": h'00081a'} + length.
  auto Loaded = decodeBytecodeInput(
      "6000"
      "a2"
      "6469706673"
      "58221220000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "64736f6c6343" "00081a"
      "0033",
      "metadata.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x00}));
  EXPECT_TRUE(Loaded->MetadataStripped);

  ASSERT_TRUE(Loaded->RuntimeMetadata.has_value());
  EXPECT_EQ(Loaded->RuntimeMetadata->Container,
            MetadataContainer::SolidityMap);
  EXPECT_EQ(Loaded->RuntimeMetadata->Offset, 2u);
  EXPECT_EQ(Loaded->RuntimeMetadata->Size, Loaded->Original.size() - 2);
  EXPECT_EQ(Loaded->RuntimeMetadata->language(), MetadataLanguage::Solidity);
  EXPECT_EQ(Loaded->RuntimeMetadata->compilerVersion(), "0.8.26");

  const MetadataEntry *Hash = Loaded->RuntimeMetadata->sourceHash();
  ASSERT_NE(Hash, nullptr);
  ASSERT_NE(Hash->Field, nullptr);
  EXPECT_EQ(Hash->Field->ID, MetadataField::IPFS);
  EXPECT_EQ(Hash->Value.Kind, MetadataValueKind::ByteString);
  // A CIDv0 multihash is the SHA-256 identifier, a length, and the digest.
  ASSERT_EQ(Hash->Value.Bytes.size(), 34u);
  EXPECT_EQ(Hash->Value.Bytes[0], 0x12);
  EXPECT_EQ(Hash->Value.Bytes[1], 0x20);

  // Keys arrive in the order the compiler wrote them, so a reader can tell two
  // builds apart by more than their contents.
  ASSERT_EQ(Loaded->RuntimeMetadata->Entries.size(), 2u);
  EXPECT_EQ(Loaded->RuntimeMetadata->Entries[0].Key, "ipfs");
  EXPECT_EQ(Loaded->RuntimeMetadata->Entries[1].Key, "solc");
}

TEST(EVMBytecode, ReadsThePrereleaseVersionAndUntabulatedKeys) {
  // CBOR {"solc": "0.9.0-nightly", "surprise": true}: a prerelease version does
  // not fit three bytes and is written as text, and a key no table knows still
  // reaches a reader.
  auto Metadata = findContractMetadata(std::vector<uint8_t>{
      0xa2, 0x64, 's',  'o',  'l',  'c',  0x6d, '0',  '.',  '9',  '.',
      '0',  '-',  'n',  'i',  'g',  'h',  't',  'l',  'y',  0x68, 's',
      'u',  'r',  'p',  'r',  'i',  's',  'e',  0xf5, 0x00, 0x1e});
  ASSERT_TRUE(Metadata.has_value());
  EXPECT_EQ(Metadata->Offset, 0u);
  EXPECT_EQ(Metadata->language(), MetadataLanguage::Solidity);
  EXPECT_EQ(Metadata->compilerVersion(), "0.9.0-nightly");
  EXPECT_EQ(Metadata->sourceHash(), nullptr);

  ASSERT_EQ(Metadata->Entries.size(), 2u);
  EXPECT_EQ(Metadata->Entries[1].Key, "surprise");
  EXPECT_EQ(Metadata->Entries[1].Field, nullptr);
  EXPECT_EQ(Metadata->Entries[1].Value.Kind, MetadataValueKind::Boolean);
  EXPECT_TRUE(Metadata->Entries[1].Value.Boolean);
}

TEST(EVMBytecode, RefusesTrailersThatDoNotAccountForEveryDeclaredByte) {
  // The declared length covers one byte more than the map occupies. Reading it
  // anyway would remove a byte of code.
  EXPECT_FALSE(findContractMetadata(std::vector<uint8_t>{
                   0xa1, 0x64, 's', 'o', 'l', 'c', 0x43, 0x00, 0x08, 0x1e, 0x00,
                   0x00, 0x0b})
                   .has_value());

  // A well-formed map that names no tabulated key is not evidence that these
  // bytes are a trailer at all.
  EXPECT_FALSE(
      findContractMetadata(std::vector<uint8_t>{0xa1, 0x61, 'x', 0x01, 0x00,
                                                0x04})
          .has_value());

  // An indefinite-length map is CBOR this reader does not claim to understand.
  EXPECT_FALSE(findContractMetadata(std::vector<uint8_t>{
                   0xbf, 0x64, 's', 'o', 'l', 'c', 0x43, 0x00, 0x08, 0x1e, 0xff,
                   0x00, 0x0b})
                   .has_value());

  EXPECT_FALSE(findContractMetadata(std::vector<uint8_t>{0x00}).has_value());
}

TEST(EVMBytecode, KeepsTheTrailerWhenStrippingIsDisabled) {
  BytecodeLoadOptions Options;
  Options.StripMetadata = false;
  auto Loaded =
      decodeBytecodeInput("6000a164736f6c634300081e000a", "metadata.hex",
                          Options);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_FALSE(Loaded->MetadataStripped);
  EXPECT_EQ(Loaded->Code.size(), 14u);
  // Reading the trailer and removing it are separate decisions, so what it
  // says about the build survives either way.
  ASSERT_TRUE(Loaded->RuntimeMetadata.has_value());
  EXPECT_EQ(Loaded->RuntimeMetadata->compilerVersion(), "0.8.30");
}

TEST(EVMBytecode, DescribesEveryTabulatedMetadataField) {
  for (const MetadataFieldInfo &Info : metadataFieldInfos()) {
    SCOPED_TRACE(Info.Key.str());
    EXPECT_EQ(&getMetadataFieldInfo(Info.ID), &Info);
    EXPECT_EQ(findMetadataField(Info.Key), &Info);
    EXPECT_FALSE(Info.Summary.empty());
  }
  EXPECT_EQ(findMetadataField("nothing-claims-this"), nullptr);

  for (const MetadataLanguageInfo &Info : metadataLanguageInfos())
    EXPECT_EQ(metadataLanguageName(Info.ID), Info.Name);

  for (const MetadataContainerInfo &Info : metadataContainerInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getMetadataContainerInfo(Info.ID), &Info);
    EXPECT_EQ(metadataContainerName(Info.ID), Info.Name);
    EXPECT_FALSE(Info.Summary.empty());
  }

  // The reader routes a sequence member to the compiler map by its declared
  // kind, so exactly one member may declare that kind and every position back
  // from it has to be occupied.
  size_t Maps = 0;
  for (const MetadataSequenceElementInfo &Info :
       metadataSequenceElementInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getMetadataSequenceElementInfo(Info.ID), &Info);
    EXPECT_EQ(findMetadataSequenceElement(Info.PositionFromEnd), &Info);
    EXPECT_FALSE(Info.Summary.empty());
    Maps += Info.ValueKind == MetadataValueKind::Map ? 1 : 0;
  }
  EXPECT_EQ(Maps, 1u);
  EXPECT_EQ(findMetadataSequenceElement(kMaxMetadataSequenceLength), nullptr);
}

//===----------------------------------------------------------------------===//
// The other compiler's footer
//===----------------------------------------------------------------------===//

TEST(EVMBytecode, ReadsASequenceFooterPublishedByTheVyperProject) {
  // [167, [10], 96, {"vyper": [0, 4, 0]}], taken verbatim from the discussion
  // that specified this framing. The two trailing bytes say 0x14, which is the
  // whole footer and not just the CBOR: reading it the way Solidity's footer is
  // read lands two bytes early.
  auto Metadata = findContractMetadata(std::vector<uint8_t>{
      0x84, 0x18, 0xa7, 0x81, 0x0a, 0x18, 0x60, 0xa1, 0x65, 'v',
      'y',  'p',  'e',  'r',  0x83, 0x00, 0x04, 0x00, 0x00, 0x14});
  ASSERT_TRUE(Metadata.has_value());
  EXPECT_EQ(Metadata->Container, MetadataContainer::VyperSequence);
  EXPECT_EQ(Metadata->Offset, 0u);
  EXPECT_EQ(Metadata->Size, 0x14u);
  EXPECT_EQ(Metadata->language(), MetadataLanguage::Vyper);
  // The version is three numbers rather than three bytes, which is the other
  // way a compiler spells a release.
  EXPECT_EQ(Metadata->compilerVersion(), "0.4.0");

  EXPECT_EQ(Metadata->sequenceUnsigned(
                MetadataSequenceElement::RuntimeCodeLength),
            167u);
  EXPECT_EQ(Metadata->sequenceUnsigned(
                MetadataSequenceElement::ImmutableSectionLength),
            96u);
  const MetadataSequenceEntry *Sections =
      Metadata->find(MetadataSequenceElement::DataSectionLengths);
  ASSERT_NE(Sections, nullptr);
  ASSERT_EQ(Sections->Value.Elements.size(), 1u);
  EXPECT_EQ(Sections->Value.Elements[0].Unsigned, 10u);
  EXPECT_EQ(Metadata->find(MetadataSequenceElement::IntegrityHash), nullptr);
}

TEST(EVMBytecode, ReadsTheIntegrityHashALaterReleaseAdded) {
  // [h'deadbeef', 5, [], 0, {"vyper": [0, 4, 1]}]: the release that started
  // recording an integrity hash put it at the front, so every other member's
  // index moved and only its distance from the compiler map stayed put.
  auto Metadata = findContractMetadata(std::vector<uint8_t>{
      0x85, 0x44, 0xde, 0xad, 0xbe, 0xef, 0x05, 0x80, 0x00, 0xa1, 0x65,
      'v',  'y',  'p',  'e',  'r',  0x83, 0x00, 0x04, 0x01, 0x00, 0x16});
  ASSERT_TRUE(Metadata.has_value());
  EXPECT_EQ(Metadata->Container, MetadataContainer::VyperSequence);
  EXPECT_EQ(Metadata->compilerVersion(), "0.4.1");
  EXPECT_EQ(
      Metadata->sequenceUnsigned(MetadataSequenceElement::RuntimeCodeLength),
      5u);

  const MetadataSequenceEntry *Integrity =
      Metadata->find(MetadataSequenceElement::IntegrityHash);
  ASSERT_NE(Integrity, nullptr);
  EXPECT_EQ(Integrity->Value.Bytes,
            (std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef}));
}

TEST(EVMBytecode, RefusesSequenceFootersThatDoNotMatchTheDeclaredShape) {
  // A three-member sequence is shorter than every release writes.
  EXPECT_FALSE(findContractMetadata(
                   std::vector<uint8_t>{0x83, 0x05, 0x80, 0xa1, 0x65, 'v', 'y',
                                        'p', 'e', 'r', 0x83, 0x00, 0x04, 0x00,
                                        0x00, 0x10})
                   .has_value());

  // The member two places before the compiler map is the list of data section
  // lengths. A number there is a document with a different meaning.
  EXPECT_FALSE(findContractMetadata(
                   std::vector<uint8_t>{0x84, 0x05, 0x01, 0x00, 0xa1, 0x65, 'v',
                                        'y', 'p', 'e', 'r', 0x83, 0x00, 0x04,
                                        0x00, 0x00, 0x11})
                   .has_value());

  // The trailing map has to name a compiler for these bytes to be a footer.
  EXPECT_FALSE(findContractMetadata(
                   std::vector<uint8_t>{0x84, 0x05, 0x80, 0x00, 0xa1, 0x61, 'x',
                                        0x01, 0x00, 0x0b})
                   .has_value());
}

TEST(EVMBytecode, ReadsTheTrailerFromWhicheverStageCarriesIt) {
  // A constructor that returns five bytes of runtime, followed by the sequence
  // footer describing them. This compiler leaves the runtime code with no
  // trailer at all, so a reader that only looks after unwrapping reports an
  // unknown build for a contract that named itself.
  auto Loaded = decodeBytecodeInput("6005600c60003960056000f3"
                                    "6001600055"
                                    "84058000a1657679706572830004000011",
                                    "creation.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));
  EXPECT_TRUE(Loaded->RuntimeExtracted);
  EXPECT_FALSE(Loaded->MetadataStripped);

  ASSERT_TRUE(Loaded->InputMetadata.has_value());
  EXPECT_EQ(Loaded->InputMetadata->Container, MetadataContainer::VyperSequence);
  EXPECT_EQ(Loaded->InputMetadata->compilerVersion(), "0.4.0");
  EXPECT_EQ(Loaded->InputMetadata->sequenceUnsigned(
                MetadataSequenceElement::RuntimeCodeLength),
            Loaded->Code.size());
  EXPECT_FALSE(Loaded->RuntimeMetadata.has_value());
}

TEST(EVMBytecode, KeepsBothStagesInAgreementWhenNothingIsUnwrapped) {
  // Runtime code carrying its own trailer is one stage, not two, and the
  // records must not disagree about it.
  auto Loaded =
      decodeBytecodeInput("6000a164736f6c634300081e000a", "runtime.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_FALSE(Loaded->RuntimeExtracted);
  ASSERT_TRUE(Loaded->InputMetadata.has_value());
  ASSERT_TRUE(Loaded->RuntimeMetadata.has_value());
  EXPECT_EQ(Loaded->InputMetadata->Offset, Loaded->RuntimeMetadata->Offset);
  EXPECT_EQ(Loaded->InputMetadata->compilerVersion(),
            Loaded->RuntimeMetadata->compilerVersion());
}

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
