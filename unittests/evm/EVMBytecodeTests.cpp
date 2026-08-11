//===- EVMBytecodeTests.cpp - EVM bytecode input tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/Bytecode.h"
#include "neverd/evm/EVMConstants.h"

#include "llvm/Support/Error.h"

namespace neverd::evm {
namespace {

TEST(EVMBytecode, DecodesPrefixedHexText) {
  auto Loaded = decodeBytecodeInput("0x6001600055", "contract.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Code, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));
  EXPECT_EQ(Loaded->Source, BytecodeSourceKind::Hex);
  EXPECT_EQ(Loaded->OriginalSize, 5u);
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
  EXPECT_EQ(Loaded->OriginalSize, 17u);
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
  EXPECT_EQ(Loaded->OriginalSize, 14u);

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

  ASSERT_TRUE(Loaded->Metadata.has_value());
  EXPECT_EQ(Loaded->Metadata->Offset, 2u);
  EXPECT_EQ(Loaded->Metadata->Size, Loaded->OriginalSize - 2);
  EXPECT_EQ(Loaded->Metadata->language(), MetadataLanguage::Solidity);
  EXPECT_EQ(Loaded->Metadata->compilerVersion(), "0.8.26");

  const MetadataEntry *Hash = Loaded->Metadata->sourceHash();
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
  ASSERT_EQ(Loaded->Metadata->Entries.size(), 2u);
  EXPECT_EQ(Loaded->Metadata->Entries[0].Key, "ipfs");
  EXPECT_EQ(Loaded->Metadata->Entries[1].Key, "solc");
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
  ASSERT_TRUE(Loaded->Metadata.has_value());
  EXPECT_EQ(Loaded->Metadata->compilerVersion(), "0.8.30");
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
}

} // namespace
} // namespace neverd::evm
