//===- AdaDEHCorpusTests.cpp - Ada/D Itanium corpus consumer tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "AdaDEHCorpusTestsDetail.h"

namespace {

using namespace llvm;
using namespace neverd;
using namespace neverd::test;
using namespace neverd::ada_d_corpus_test;

TEST(AdaDEHCorpus, DeclaresCompleteBuildMatrix) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  EXPECT_EQ(ExpectationsOrErr->size(), 12u);

  std::set<std::string> Cells;
  std::set<std::string> Personalities;
  std::set<std::string> Descriptors;
  for (const AdaDEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    Cells.insert(Expectation.Toolchain + "-" + Expectation.Target);
    Personalities.insert(Expectation.Personalities.front());
    Descriptors.insert(getAdaDDescriptorABIName(Expectation.DescriptorABI));
    EXPECT_EQ(Expectation.ExpectedFormat, BinaryFormat::ELF);
  }
  EXPECT_EQ(Cells.size(), 6u);
  EXPECT_EQ(Personalities, (std::set<std::string>{"__dmd_personality_v0",
                                                  "__gdc_personality_v0",
                                                  "__gnat_personality_v0",
                                                  "_d_eh_personality"}));
  EXPECT_EQ(Descriptors, (std::set<std::string>{"d-classinfo",
                                                "gnat-exception-id"}));
}

TEST(AdaDEHCorpus, MatchesDeclaredBytesAndContainer) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  for (const AdaDEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    SCOPED_TRACE(Expectation.Path);
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

    std::optional<BinaryImage> Image = loadArtifact(ArtifactPath);
    if (!Image)
      continue;
    EXPECT_EQ(Image->Format, Expectation.ExpectedFormat);
    EXPECT_EQ(Image->Arch, Expectation.ExpectedArch);
    for (const std::string &Name : Expectation.RequiredSections)
      EXPECT_TRUE(Image->hasSection(Name))
          << "the manifest requires section " << Name;

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed)
        << diagnosticsFor(Info);
    for (const ExceptionFunction &Function : Info.Functions)
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
          << diagnosticsFor(Info);
  }
}

TEST(AdaDEHCorpus, RecoversTheCallSiteGraphOnEveryCell) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const AdaDEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);

    std::set<std::string> Personalities;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Itanium)
        continue;
      Personalities.insert(getExceptionPersonalityName(Function.Personality));
      for (const ItaniumCallSite &Site : Function.Itanium->CallSites) {
        EXPECT_TRUE(Site.GuardedRange.isValid());
        EXPECT_TRUE(Function.CodeRange.contains(Site.GuardedRange))
            << "call site at 0x" << utohexstr(Site.GuardedRange.Begin)
            << " lies outside its frame";
        if (Site.LandingPadVA == 0)
          continue;
        const Segment *Seg = Image->getSegmentFor(Site.LandingPadVA);
        EXPECT_TRUE(Seg != nullptr && Seg->isExecutable())
            << "landing pad 0x" << utohexstr(Site.LandingPadVA)
            << " does not name code";
      }
      for (const ItaniumAction &Action : Function.Itanium->Actions)
        if (Action.isCatch())
          EXPECT_LE(static_cast<uint64_t>(Action.TypeFilter),
                    Function.Itanium->TypeTable.size())
              << "catch filter names a slot the type table does not have";
    }
    const bool PersonalityMatched =
        llvm::any_of(Expectation.Personalities, [&](const std::string &Name) {
          return Personalities.contains(Name);
        });
    EXPECT_TRUE(PersonalityMatched)
        << "parsed personalities did not include any the manifest allows; "
        << Diagnostics;

    const GraphCensus Census = censusOf(Info);
    EXPECT_GT(Census.Records, 0u) << Diagnostics;
    EXPECT_GE(Census.CallSites, Expectation.MinCallSites) << Diagnostics;
    EXPECT_GE(Census.LandingPads, Expectation.MinLandingPads) << Diagnostics;
    EXPECT_GE(Census.CatchClauses, Expectation.MinCatchClauses) << Diagnostics;
    // D `scope (exit)` is a producer/manifest claim. DMD encodes it as a
    // catch of Throwable rather than a cleanup action, so the LSDA census
    // cannot prove that floor across the matrix.
    EXPECT_GE(Census.TypeTableEntries, Expectation.MinTypeTableEntries)
        << Diagnostics;
  }
  EXPECT_EQ(Images, 12u);
}

TEST(AdaDEHCorpus, KeepsAdaAndDTypeDescriptorsOpaque) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned TypedFrames = 0;
  for (const AdaDEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    SCOPED_TRACE(Expectation.Path);

    for (const ExceptionFunction &Function : Image->ExceptionMetadata.Functions) {
      if (!Function.Itanium)
        continue;
      const ItaniumEHInfo &LSDA = *Function.Itanium;
      EXPECT_EQ(LSDA.TypeTableEntryKind,
                ItaniumTypeTableEntryKind::OpaqueDescriptor);
      for (const ItaniumTypeEntry &Entry : LSDA.TypeTable) {
        if (Entry.IsCatchAll)
          continue;
        ++TypedFrames;
        llvm::StringRef Name(Entry.TypeName);
        EXPECT_FALSE(Name.starts_with("_ZTI") || Name.starts_with("__ZTI"))
            << "opaque descriptor was followed as std::type_info: "
            << Entry.TypeName;
        EXPECT_TRUE(Entry.TypeInfoVA != 0 || Entry.TypeInfoSlotVA != 0)
            << "typed slot lost its descriptor address";
      }
    }
  }
  EXPECT_GT(TypedFrames, 0u);
}

} // namespace
