//===- CxxItaniumEHCorpusGraphTests.cpp - corpus call site graph tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "CxxItaniumEHCorpusTestsDetail.h"

namespace {

using namespace llvm;
using namespace neverd;
using namespace neverd::test;
using namespace neverd::cxx_corpus_test;

// The payoff of the whole product line.  An Itanium call-site table is what
// most C++ in the world dispatches through, and a reader that stops at "this
// frame has an LSDA" recovers no handler at all: the type a catch names lives
// in a table beside the call sites, reached through an action chain, and every
// step of that walk is a place a decoder can silently produce nothing.
TEST(CxxItaniumEHCorpus, RecoversTheCallSiteGraphOnEveryItaniumTarget) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  std::set<std::string> Containers;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ValidationLevel !=
        CxxItaniumCorpusValidationLevel::LsdaGraph)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    Containers.insert(Expectation.ObjectFormat);
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
        // A guarded range and the pad it names both belong to the frame that
        // declared them.  Both go wrong together when the landing-pad base was
        // resolved against the wrong anchor, which is the failure that
        // otherwise yields plausible-looking but unrelated addresses.
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
      // A catch names a slot, and the slot is what carries the type.  A filter
      // pointing past the table is the shape a misread type-table base takes.
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
    EXPECT_GE(Census.CleanupPads, Expectation.MinCleanupPads) << Diagnostics;
    EXPECT_GE(Census.TypeTableEntries, Expectation.MinTypeTableEntries)
        << Diagnostics;
  }

  // Seven cells of eight variants, less each cell's control.  The two ARM
  // cells are not among them: EHABI carries its language data somewhere this
  // graph is not, which is what its own validation level says.
  EXPECT_EQ(Images, 49u);
  // The same table read out of three containers, which is the thing a single
  // target could never show.
  EXPECT_EQ(Containers, (std::set<std::string>{"elf", "macho", "pe"}));
}

// `-fno-exceptions` is the control, and what it decides is narrower than "no
// unwind information": the build keeps `-fasynchronous-unwind-tables`, so the
// frames are still described.  What it removes is the language data, and a
// frame with no LSDA cannot name a handler however the tables around it look.
TEST(CxxItaniumEHCorpus, KeepsTheExceptionFreeControlFreeOfLanguageData) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ValidationLevel != CxxItaniumCorpusValidationLevel::CfiOnly)
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    // The control links the same C++ runtime as the probe beside it, so what
    // must carry no language data is the code the producer compiled -- named
    // here by the frames that lie inside this image's own text and not by the
    // whole image.  A record anywhere is still counted, because the claim is
    // about the probe's frames and a runtime that brought its own is exactly
    // what makes an image-wide claim untestable.
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Itanium)
        continue;
      EXPECT_FALSE(Function.Itanium->isCleanupOnly() &&
                   Function.Itanium->CallSites.empty())
          << "an empty record is a decode that produced nothing; "
          << Diagnostics;
    }
    const GraphCensus Census = censusOf(Info);
    EXPECT_EQ(Census.CatchClauses, 0u)
        << "a build without exceptions cannot name a catch; " << Diagnostics;
  }

  // One control per cell.
  EXPECT_EQ(Images, 9u);
}

// The same graph, out of a container that keeps it somewhere else entirely.
//
// ARM EHABI gives a C++ frame's language data no section of its own: the LSDA
// is appended to the frame's `.ARM.extab` entry, immediately after the unwind
// opcodes, and is reachable only by decoding the index that names the entry.
// An image here therefore carries a complete call-site table and no
// `.gcc_except_table` anywhere in it, which is what makes this the one cell
// where finding nothing looks exactly like there being nothing to find.
TEST(CxxItaniumEHCorpus, RecoversTheCallSiteGraphFromArmEhabiTables) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  std::set<std::string> Toolchains;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ValidationLevel != CxxItaniumCorpusValidationLevel::Ehabi)
      continue;
    ASSERT_TRUE(Expectation.ExpectArmEHABI);
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    Toolchains.insert(Expectation.Toolchain);
    SCOPED_TRACE(Expectation.Path);

    EXPECT_EQ(Image->Arch, Arch::ARM);
    EXPECT_TRUE(Image->hasSection(".ARM.exidx"));
    EXPECT_FALSE(Image->hasSection(".gcc_except_table"));

    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed) << Diagnostics;
    EXPECT_TRUE(Info.hasModel(ExceptionModel::ARMEHABI)) << Diagnostics;

    // The index covers every function the linker placed, not only the ones
    // that can be unwound through, so it is also the most complete function
    // table a stripped image of this target has.
    uint64_t IndexEntries = 0;
    std::set<std::string> Personalities;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.ARMEHABI)
        continue;
      ++IndexEntries;
      Personalities.insert(getExceptionPersonalityName(Function.Personality));
      EXPECT_TRUE(Function.CodeRange.isValid())
          << "an index entry describes no code";
      if (!Function.Itanium)
        continue;
      // Language data reached through the index has to belong to the frame the
      // index named.  Both go wrong together when the entry's own extent was
      // taken from the wrong neighbour, which is the failure that otherwise
      // yields a plausible table attached to the function beside it.
      for (const ItaniumCallSite &Site : Function.Itanium->CallSites) {
        EXPECT_TRUE(Site.GuardedRange.isValid());
        EXPECT_TRUE(Function.CodeRange.contains(Site.GuardedRange))
            << "call site at 0x" << utohexstr(Site.GuardedRange.Begin)
            << " lies outside the frame its index entry named";
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
    EXPECT_GE(IndexEntries, Expectation.MinArmExidxEntries) << Diagnostics;

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
    EXPECT_GE(Census.CleanupPads, Expectation.MinCleanupPads) << Diagnostics;
    EXPECT_GE(Census.TypeTableEntries, Expectation.MinTypeTableEntries)
        << Diagnostics;
  }

  // Two ARM cells of eight variants, less each cell's control: an
  // exception-free build has no language data on any target, so it is the
  // control's level that names it and not the container's.
  EXPECT_EQ(Images, 14u);
  // Both producers, because they do not spell this table the same way: GCC
  // writes the platform's type-table convention into the LSDA header where
  // Clang leaves the byte bare, and GCC reaches for the ARM-defined compact
  // personalities at `-O0` where Clang never does.
  EXPECT_EQ(Toolchains, (std::set<std::string>{"clang", "gcc"}));
}

// What a catch names, out of a table whose pointer encoding the header does
// not describe.
//
// EHABI hands a type-table slot to `_Unwind_decode_typeinfo_ptr`, which
// applies the platform's `R_ARM_TARGET2` convention and never reads the
// encoding byte.  Both producers here emit the same relocation and disagree
// about what to write in that byte, so a decoder that believes the header gets
// the Clang half of this cell wrong -- and gets it wrong quietly, reporting a
// displacement as though it were an address.
TEST(CxxItaniumEHCorpus, NamesTheTypesArmEhabiCatchesDispatchOn) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  for (const CxxItaniumEHArtifactExpectation &Expectation :
       *ExpectationsOrErr) {
    if (Expectation.ValidationLevel != CxxItaniumCorpusValidationLevel::Ehabi ||
        Expectation.SourceLanguage != "cxx")
      continue;
    std::optional<BinaryImage> Image =
        loadArtifact(CorpusRoot / Expectation.Path);
    if (!Image)
      continue;
    ++Images;
    SCOPED_TRACE(Expectation.Path);
    const ExceptionInfo &Info = Image->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);

    // The manifest requires the probe's own exception type as a string in the
    // image, because that is the one identity that survives stripping.  What
    // the type table has to do is arrive at it.
    std::set<std::string> Named;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (!Function.Itanium)
        continue;
      for (const ItaniumTypeEntry &Entry : Function.Itanium->TypeTable)
        if (!Entry.TypeName.empty())
          Named.insert(Entry.TypeName);
    }
    for (const std::string &Required : Expectation.RequiredStrings) {
      const bool Reached = llvm::any_of(Named, [&](const std::string &Name) {
        return llvm::StringRef(Name).contains(Required);
      });
      EXPECT_TRUE(Reached)
          << "no catch reached the type `" << Required
          << "` the manifest requires this image to throw; " << Diagnostics;
    }
  }
  // The C probe carries no type table, so it is not among these.
  EXPECT_EQ(Images, 12u);
}

} // namespace
