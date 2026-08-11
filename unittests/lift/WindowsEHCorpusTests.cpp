//===- WindowsEHCorpusTests.cpp - Windows PE corpus tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

Expected<std::vector<WindowsEHArtifactExpectation>> loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "windows-eh.json";
  return loadWindowsEHCorpusManifest(ManifestPath.string(), true);
}

std::string diagnosticsFor(const ExceptionInfo &Info) {
  std::string Result;
  for (const std::string &Diagnostic : Info.Diagnostics) {
    if (!Result.empty())
      Result += "; ";
    Result += Diagnostic;
  }
  for (const ExceptionFunction &Function : Info.Functions)
    for (const std::string &Diagnostic : Function.Diagnostics) {
      if (!Result.empty())
        Result += "; ";
      Result += Diagnostic;
    }
  return Result;
}

bool containsString(ArrayRef<std::string> Values, StringRef Needle) {
  return llvm::any_of(
      Values, [&](const std::string &Value) { return Value == Needle; });
}

bool encodingMatchesArchitecture(ExceptionEncoding Encoding, Arch TheArch) {
  if (TheArch == Arch::ARM)
    return Encoding == ExceptionEncoding::ARM32Packed ||
           Encoding == ExceptionEncoding::ARM32PackedFragment ||
           Encoding == ExceptionEncoding::ARM32Unpacked;
  if (TheArch == Arch::AArch64)
    return Encoding == ExceptionEncoding::ARM64Packed ||
           Encoding == ExceptionEncoding::ARM64PackedFragment ||
           Encoding == ExceptionEncoding::ARM64Unpacked;
  return true;
}

TEST(WindowsEHCorpus, DeclaresCompleteMultiToolchainMatrix) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  EXPECT_EQ(ExpectationsOrErr->size(), 168u);

  std::set<std::string> Toolchains;
  std::set<Arch> Architectures;
  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    Toolchains.insert(Expectation.Toolchain);
    Architectures.insert(Expectation.ExpectedArch);
  }
  EXPECT_EQ(Toolchains, (std::set<std::string>{"msvc", "clang-cl"}));
  EXPECT_EQ(Architectures,
            (std::set<Arch>{Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}));
}

TEST(WindowsEHCorpus, ParsesDeclaredExceptionMetadata) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    SCOPED_TRACE(Expectation.Path + " [" + Expectation.Toolchain + ", " +
                 Expectation.Architecture + ", " + Expectation.CxxFormat +
                 ", " + (Expectation.SecurityCookie ? "gs" : "no-gs") + ", " +
                 Expectation.Optimization + "]");

    const std::filesystem::path ArtifactPath = CorpusRoot / Expectation.Path;
    auto BufferOrErr = MemoryBuffer::getFile(ArtifactPath.string());
    if (!BufferOrErr) {
      ADD_FAILURE() << "cannot read artifact: "
                    << BufferOrErr.getError().message();
      continue;
    }
    StringRef Bytes = (*BufferOrErr)->getBuffer();
    if (Bytes.size() != Expectation.Size) {
      ADD_FAILURE() << "artifact size does not match the manifest";
      continue;
    }
    std::array<uint8_t, 32> Digest = SHA256::hash(arrayRefFromStringRef(Bytes));
    if (toHex(ArrayRef<uint8_t>(Digest), true) != Expectation.SHA256) {
      ADD_FAILURE() << "artifact SHA-256 does not match the manifest";
      continue;
    }

    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    if (!ImageLoader) {
      ADD_FAILURE() << "NeverD did not recognize the PE artifact";
      continue;
    }
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    if (!ImageOrErr) {
      ADD_FAILURE() << "NeverD failed to load the PE artifact: "
                    << toString(ImageOrErr.takeError());
      continue;
    }
    const BinaryImage &Image = *ImageOrErr;
    EXPECT_EQ(Image.Format, BinaryFormat::COFF);
    EXPECT_EQ(Image.Arch, Expectation.ExpectedArch);

    const ExceptionInfo &Info = Image.ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    if (Expectation.ValidationLevel == CorpusValidationLevel::LoadOnly)
      continue;

    EXPECT_GE(Info.Functions.size(), Expectation.MinExceptionFunctions);
    if (Expectation.ValidationLevel == CorpusValidationLevel::UnwindOnly) {
      for (const ExceptionFunction &Function : Info.Functions) {
        EXPECT_TRUE(Function.CodeRange.isValid());
        EXPECT_TRUE(encodingMatchesArchitecture(Function.Encoding, Image.Arch));
      }
      continue;
    }

    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed) << Diagnostics;
    EXPECT_TRUE(containsString(Expectation.AllowedParseStatuses,
                               getExceptionParseStatusName(Info.ParseStatus)))
        << "unexpected image exception parse status: "
        << getExceptionParseStatusName(Info.ParseStatus) << "; " << Diagnostics;

    uint64_t CxxFunctions = 0;
    uint64_t TryBlocks = 0;
    uint64_t SEHScopes = 0;
    std::set<std::string> Personalities;
    for (const ExceptionFunction &Function : Info.Functions) {
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
          << Diagnostics;
      if (Function.Personality != ExceptionPersonality::None &&
          Function.Personality != ExceptionPersonality::Unknown)
        Personalities.insert(getExceptionPersonalityName(Function.Personality));
      if (Function.SEH) {
        SEHScopes += Function.SEH->Scopes.size();
        for (const SEHScopeRecord &Scope : Function.SEH->Scopes) {
          EXPECT_TRUE(Scope.GuardedRange.isValid());
          EXPECT_NE(Scope.ParseStatus, ExceptionParseStatus::Malformed);
        }
      }
      if (Function.Cxx) {
        ++CxxFunctions;
        TryBlocks += Function.Cxx->TryBlocks.size();
        EXPECT_TRUE(Function.Cxx->hasValidStateGraph());
      }
    }

    EXPECT_GE(CxxFunctions, Expectation.MinCxxFunctions);
    EXPECT_GE(TryBlocks, Expectation.MinTryBlocks);
    EXPECT_GE(SEHScopes, Expectation.MinSEHScopes);
    const bool PersonalityMatched =
        Expectation.Personalities.empty() ||
        llvm::any_of(Expectation.Personalities, [&](const std::string &Name) {
          return Personalities.contains(Name);
        });
    EXPECT_TRUE(PersonalityMatched)
        << "parsed personalities did not include any manifest alternative";
  }
}

} // namespace
