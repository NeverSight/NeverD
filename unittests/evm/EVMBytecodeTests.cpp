//===- EVMBytecodeTests.cpp - EVM bytecode input tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reading a program out of whatever a user hands over, and unwrapping the
/// constructor that deployed it.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/EVMConstants.h"
#include "neverd/evm/bytecode/EVMBytecode.h"

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

TEST(EVMBytecode, AcceptsExplicitEmptyRuntimeButRejectsMissingHexInput) {
  auto Explicit = decodeBytecodeInput(" 0x \n", "empty-runtime.hex");
  ASSERT_TRUE(static_cast<bool>(Explicit))
      << llvm::toString(Explicit.takeError());
  EXPECT_TRUE(Explicit->Code.empty());
  EXPECT_TRUE(Explicit->Original.empty());
  EXPECT_EQ(Explicit->Container, BytecodeContainer::Legacy);

  auto Direct =
      normalizeBytecode({}, BytecodeSourceKind::Raw,
                        /*SourceIsRuntime=*/true, "empty-runtime.raw");
  ASSERT_TRUE(static_cast<bool>(Direct)) << llvm::toString(Direct.takeError());
  EXPECT_TRUE(Direct->Code.empty());
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

TEST(EVMBytecode, PrefersExplicitEmptyRuntimeFieldsInCompilerArtifacts) {
  constexpr llvm::StringLiteral PrefixedArtifact = R"json({
    "bytecode": {"object": "0x6000"},
    "deployedBytecode": {"object": "0x"}
  })json";
  auto Loaded = decodeBytecodeInput(PrefixedArtifact, "CreationOnly.json");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_TRUE(Loaded->Code.empty());
  EXPECT_EQ(Loaded->Source, BytecodeSourceKind::Artifact);
  EXPECT_TRUE(Loaded->SourceIsRuntime);
  EXPECT_FALSE(Loaded->RuntimeExtracted);

  constexpr llvm::StringLiteral SolcEmptyArtifact = R"json({
    "bytecode": {"object": "0x6000"},
    "deployedBytecode": {"object": ""}
  })json";
  auto SolcEmpty = decodeBytecodeInput(SolcEmptyArtifact, "Interface.json");
  ASSERT_TRUE(static_cast<bool>(SolcEmpty))
      << llvm::toString(SolcEmpty.takeError());
  EXPECT_TRUE(SolcEmpty->Code.empty());
  EXPECT_TRUE(SolcEmpty->SourceIsRuntime);

  constexpr llvm::StringLiteral WhitespaceArtifact = R"json({
    "deployedBytecode": {"object": "   "}
  })json";
  auto Whitespace =
      decodeBytecodeInput(WhitespaceArtifact, "MalformedInterface.json");
  ASSERT_FALSE(static_cast<bool>(Whitespace));
  EXPECT_NE(llvm::toString(Whitespace.takeError()).find("empty bytecode"),
            std::string::npos);
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

TEST(EVMBytecode, ExtractsAStaticallyProvenEmptyRuntime) {
  // PUSH1 0; PUSH1 12; PUSH1 0; CODECOPY; PUSH1 0; PUSH1 0; RETURN.
  // optional<vector>{} is a successful extraction, not "no proof".
  auto Loaded =
      decodeBytecodeInput("6000600c60003960006000f3", "empty-creation.hex");
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_TRUE(Loaded->Code.empty());
  EXPECT_TRUE(Loaded->RuntimeExtracted);
  EXPECT_FALSE(Loaded->SourceIsRuntime);
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
} // namespace
} // namespace neverd::evm
