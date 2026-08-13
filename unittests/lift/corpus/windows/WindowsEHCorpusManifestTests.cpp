//===- WindowsEHCorpusManifestTests.cpp - Corpus contract tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <filesystem>
#include <map>
#include <string>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

std::string singleArtifactManifest(StringRef Path, StringRef Toolchain,
                                   StringRef CxxFormat,
                                   StringRef Execution = "passed") {
  const StringRef Compiler = Toolchain == "msvc" ? "cl.exe" : "clang-cl.exe";
  const StringRef Linker = Toolchain == "msvc" ? "link.exe" : "lld-link.exe";
  return ("{\"schema_version\":2,\"corpus\":\"windows-eh\","
          "\"artifacts\":[{\"path\":\"" +
          Path + "\",\"sha256\":\"" + std::string(64, 'a') +
          "\",\"size\":1024,\"architecture\":\"x86_64\","
          "\"suite\":\"abi-probe\",\"name\":\"cxx_eh_probe\","
          "\"kind\":\"cxx\",\"build\":{\"toolchain\":\"" +
          Toolchain + "\",\"compiler\":{\"name\":\"" + Compiler +
          "\",\"product_version\":\"test\",\"file_version\":\"test\"},"
          "\"linker\":{\"name\":\"" +
          Linker +
          "\",\"product_version\":\"test\",\"file_version\":\"test\"},"
          "\"target_triple\":\"x86_64-pc-windows-msvc\","
          "\"optimization\":\"o0\",\"security_cookie\":false,"
          "\"cxx_format\":\"" +
          CxxFormat + "\",\"execution\":\"" + Execution +
          "\"},\"neverd\":{\"validation_level\":\"exception-graph\","
          "\"allowed_parse_status\":[\"complete\"],"
          "\"personalities_any\":[\"__CxxFrameHandler3\"],"
          "\"min_exception_functions\":1,\"min_cxx_functions\":1,"
          "\"min_try_blocks\":1,\"min_seh_scopes\":0}}]}")
      .str();
}

TEST(WindowsEHCorpusManifest, AcceptsSchemaV2SingleArtifactForUnitTesting) {
  const std::string Path =
      "corpus/windows-eh/msvc/x86_64/fh3/no-gs/o0/abi-probe/"
      "cxx_eh_probe-msvc-x86_64-fh3-no-gs-o0.exe";
  auto Parsed = parseWindowsEHCorpusManifest(
      singleArtifactManifest(Path, "msvc", "fh3"), false);

  ASSERT_TRUE(static_cast<bool>(Parsed)) << toString(Parsed.takeError());
  ASSERT_EQ(Parsed->size(), 1u);
  EXPECT_EQ((*Parsed)[0].ExpectedArch, Arch::X64);
  EXPECT_EQ((*Parsed)[0].Toolchain, "msvc");
  EXPECT_EQ((*Parsed)[0].ValidationLevel,
            CorpusValidationLevel::ExceptionGraph);
}

TEST(WindowsEHCorpusManifest, RejectsUnsafeArtifactPath) {
  auto Parsed = parseWindowsEHCorpusManifest(
      singleArtifactManifest("../escape.exe", "msvc", "fh3"), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("not normalized"),
            std::string::npos);
}

TEST(WindowsEHCorpusManifest, RejectsClangClFH4Claim) {
  const std::string Path =
      "corpus/windows-eh/clang-cl/x86_64/fh4/no-gs/o0/abi-probe/"
      "cxx_eh_probe-clang-cl-x86_64-fh4-no-gs-o0.exe";
  auto Parsed = parseWindowsEHCorpusManifest(
      singleArtifactManifest(Path, "clang-cl", "fh4"), false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("supports only EH3"),
            std::string::npos);
}

TEST(WindowsEHCorpusManifest, RejectsCrossTargetExecutionStatusOnX64) {
  const std::string Path =
      "corpus/windows-eh/msvc/x86_64/fh3/no-gs/o0/abi-probe/"
      "cxx_eh_probe-msvc-x86_64-fh3-no-gs-o0.exe";
  auto Parsed = parseWindowsEHCorpusManifest(
      singleArtifactManifest(Path, "msvc", "fh3", "not-run-cross-target"),
      false);

  ASSERT_FALSE(static_cast<bool>(Parsed));
  EXPECT_NE(toString(Parsed.takeError()).find("execution status"),
            std::string::npos);
}

TEST(WindowsEHCorpusSynthetic, LoadsAndHashesAllFourPEMachines) {
  const std::filesystem::path Root(NEVERD_SYNTHETIC_WINDOWS_EH_ROOT);
  auto BufferOrErr = MemoryBuffer::getFile((Root / "synthetic.json").string());
  ASSERT_TRUE(static_cast<bool>(BufferOrErr));
  auto Parsed = json::parse((*BufferOrErr)->getBuffer());
  ASSERT_TRUE(static_cast<bool>(Parsed)) << toString(Parsed.takeError());
  const json::Array *Artifacts = Parsed->getAsObject()->getArray("artifacts");
  ASSERT_NE(Artifacts, nullptr);
  ASSERT_EQ(Artifacts->size(), 4u);

  const std::map<std::string, Arch> ExpectedArchitectures{
      {"x86", Arch::X86},
      {"x86_64", Arch::X64},
      {"arm", Arch::ARM},
      {"aarch64", Arch::AArch64},
  };
  for (const json::Value &Value : *Artifacts) {
    const json::Object *Artifact = Value.getAsObject();
    ASSERT_NE(Artifact, nullptr);
    const std::optional<StringRef> RelativePath = Artifact->getString("path");
    const std::optional<StringRef> Hash = Artifact->getString("sha256");
    const std::optional<StringRef> Architecture =
        Artifact->getString("architecture");
    ASSERT_TRUE(RelativePath && Hash && Architecture);
    SCOPED_TRACE(Architecture->str());

    const std::filesystem::path Path = Root / RelativePath->str();
    auto ImageBufferOrErr = MemoryBuffer::getFile(Path.string());
    ASSERT_TRUE(static_cast<bool>(ImageBufferOrErr));
    StringRef Bytes = (*ImageBufferOrErr)->getBuffer();
    std::array<uint8_t, 32> Digest = SHA256::hash(arrayRefFromStringRef(Bytes));
    EXPECT_EQ(toHex(ArrayRef<uint8_t>(Digest), true), *Hash);

    std::unique_ptr<Loader> ImageLoader = Loader::create(Path);
    ASSERT_NE(ImageLoader, nullptr);
    auto ImageOrErr = ImageLoader->load(Path);
    ASSERT_TRUE(static_cast<bool>(ImageOrErr))
        << toString(ImageOrErr.takeError());
    EXPECT_EQ(ImageOrErr->Format, BinaryFormat::COFF);
    EXPECT_EQ(ImageOrErr->Arch, ExpectedArchitectures.at(Architecture->str()));
  }
}

} // namespace
