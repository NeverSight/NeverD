//===- SignatureDBTests.cpp - Signature database tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RTTI.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::sigs;

namespace {

class SignatureDirectoryTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> UniqueDirectory;
    const std::error_code Error = llvm::sys::fs::createUniqueDirectory(
        "neverd-signature-db", UniqueDirectory);
    ASSERT_FALSE(Error) << Error.message();
    Directory = UniqueDirectory.c_str();
  }

  void TearDown() override {
    std::error_code Error;
    std::filesystem::remove_all(Directory, Error);
  }

  void write(llvm::StringRef Name, llvm::StringRef Contents) const {
    std::ofstream Output(Directory / Name.str(), std::ios::binary);
    ASSERT_TRUE(Output.good());
    Output.write(Contents.data(),
                 static_cast<std::streamsize>(Contents.size()));
    ASSERT_TRUE(Output.good());
  }

  std::filesystem::path Directory;
};

neverd::BinaryImage makeMatchingImage() {
  neverd::BinaryImage Image;
  neverd::Segment Code;
  Code.VA = 0x1000;
  Code.Size = 2;
  Code.FileSz = 2;
  Code.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Code.Data = {0xAA, 0xBB};
  Image.Segments.push_back(std::move(Code));
  return Image;
}

} // namespace

TEST(SignatureDBIdentity, ByteAgreementNeverInventsSourceIdentity) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 library_guess\n", "candidate"));
  Database.apply(makeMatchingImage(), {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "library_guess");
  EXPECT_TRUE(Database.buildNameMap().empty());
  EXPECT_EQ(Database.nameDecision(0x1000), nullptr);
}

TEST(SignatureDBIdentity, OnlyTheIndependentlyStatedAliasIsAdmitted) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 short :0000 independently_stated\n"
      "AABB 00 0000 0002 :0000 conflicting_candidate\n",
      "aliases"));
  auto Image = makeMatchingImage();
  neverd::Symbol Stated;
  Stated.Addr = 0x1000;
  Stated.Name = "independently_stated";
  Stated.IsFunc = true;
  Stated.Origin = neverd::NameOrigin::Stated;
  Image.Symbols.push_back(Stated);
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 2u);
  EXPECT_TRUE(Database.buildCandidateNameMap().empty());
  EXPECT_EQ(Database.buildNameMap().at(0x1000), "independently_stated");
  ASSERT_NE(Database.nameDecision(0x1000), nullptr);
  EXPECT_EQ(Database.nameDecision(0x1000)->Evidence,
            SignatureNameEvidence::ImageSymbol);

  // Neither the pattern's preferred alias nor its competing pattern acquires
  // identity from the symbol that proves only one spelling at this address.
  Image.Symbols.front().Name = "different_type_same_code";
  Database.apply(Image, {0x1000});
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBIdentity, AnalysisUserDataAndOtherAddressesAreNotEvidence) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText("AABB 00 0000 0002 :0000 candidate\n",
                                        "candidate"));
  for (neverd::NameOrigin Origin :
       {neverd::NameOrigin::Synthesized, neverd::NameOrigin::Analysis,
        neverd::NameOrigin::User, neverd::NameOrigin::Stated}) {
    auto Image = makeMatchingImage();
    neverd::Symbol Symbol;
    Symbol.Addr = 0x1000;
    Symbol.Name = "candidate";
    Symbol.IsFunc = Origin != neverd::NameOrigin::Stated;
    Symbol.Origin = Origin;
    Image.Symbols.push_back(Symbol);
    Symbol.Addr = 0x2000;
    Symbol.IsFunc = true;
    Symbol.Origin = neverd::NameOrigin::Stated;
    Image.Symbols.push_back(Symbol);
    Database.apply(Image, {0x1000});
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}

TEST(SignatureDBIdentity, ExportsReapplyAndTransactionsRetainOnlyCurrentProof) {
  SignatureDB Database;
  const char *Pattern = "AABB 00 0000 0002 :0000 candidate\n";
  ASSERT_FALSE(Database.loadPatternText(Pattern, "source"));
  auto Image = makeMatchingImage();
  neverd::Export Export;
  Export.Addr = 0x1000;
  Export.Name = "candidate";
  Image.Exports.push_back(Export);
  Database.apply(Image, {0x1000});
  EXPECT_EQ(Database.buildNameMap().at(0x1000), "candidate");

  auto Error = Database.loadPatternText("invalid record\n", "source");
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(Database.buildNameMap().at(0x1000), "candidate");
  ASSERT_FALSE(Database.loadPatternText(Pattern, "source"));
  EXPECT_TRUE(Database.buildNameMap().empty());

  Database.apply(Image, {0x1000});
  ASSERT_NE(Database.nameDecision(0x1000), nullptr);
  Image.Exports.clear();
  Database.apply(Image, {0x1000});
  EXPECT_EQ(Database.nameDecision(0x1000), nullptr);
  EXPECT_EQ(Database.matches().size(), 1u);
  Image.Exports.push_back(Export);
  Database.apply(Image, {0x1000});
  Database.clear();
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBIdentity, FoldedAliasesAreIndependentOfSymbolOrder) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 short :0000 _also_stated :0000 longest_name\n",
      "folded"));
  auto Image = makeMatchingImage();
  for (const char *Name : {"longest_name", "_also_stated", "_also_stated"}) {
    neverd::Symbol S;
    S.Addr = 0x1000;
    S.Name = Name;
    S.IsFunc = true;
    S.Origin = neverd::NameOrigin::Stated;
    Image.Symbols.push_back(std::move(S));
  }
  for (unsigned Order = 0; Order != 2; ++Order) {
    Database.apply(Image, {0x1000});
    EXPECT_EQ(Database.buildNameMap().at(0x1000), "longest_name");
    std::reverse(Image.Symbols.begin(), Image.Symbols.end());
  }
  for (auto &S : Image.Symbols)
    S.IsBoundaryGuess = true;
  Database.apply(Image, {0x1000});
  EXPECT_TRUE(Database.buildNameMap().empty());
  for (auto &S : Image.Symbols) {
    S.IsBoundaryGuess = false;
    S.IsSection = true;
  }
  Database.apply(Image, {0x1000});
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBTransactions, FailedInMemoryBatchDoesNotMutateDatabase) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText("AABB 00 0000 0002 :0000 first_name\n",
                                        "first"));
  ASSERT_EQ(Database.moduleCount(), 1u);
  ASSERT_EQ(Database.fileCount(), 1u);

  llvm::Error Error =
      Database.loadPatternText("CCDD 00 0000 0002 :0000 second_name\n"
                               "this record is malformed\n",
                               "second");

  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("pattern line 2"),
            std::string::npos);
  EXPECT_EQ(Database.moduleCount(), 1u);
  EXPECT_EQ(Database.fileCount(), 1u);
}

TEST_F(SignatureDirectoryTest, AutomaticSelectionSkipsFilesBeforeOpeningThem) {
  const auto Relative = std::filesystem::path("pe/x86/64");
  std::filesystem::create_directories(Directory / Relative);
  write("pe/x86/64/supplemental-bds.pat",
        "deliberately invalid user library\n");
  write("pe/x86/64/supplemental-ms64wdk.pat",
        "AABB 00 0000 0002 :0000 driver_routine\n");
  write("pe/x86/64/custom.pat", "CCDD 00 0000 0002 :0000 custom_routine\n");
  auto Image = makeMatchingImage();
  Image.Format = neverd::BinaryFormat::COFF;
  Image.Arch = neverd::Arch::X64;
  Image.Bits = neverd::Bitness::Bits64;
  Image.DynInfo.PESubsystem = llvm::COFF::IMAGE_SUBSYSTEM_NATIVE;
  Image.Imports.push_back({"ntoskrnl.exe", "ExAllocatePool2"});
  SignatureDB Database;
  auto Error = Database.loadForImage(Image, Directory);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  EXPECT_EQ(Database.moduleCount(), 2u);
  ASSERT_TRUE(Database.automaticSelection());
  EXPECT_EQ(Database.automaticSelection()->AvailableFiles, 3u);
  EXPECT_EQ(Database.automaticSelection()->SelectedFiles, 2u);

  // An unclassified image must attempt the full directory; a failed load
  // retains both the previous database and its successful selection report.
  Image.Imports.clear();
  Error = Database.loadForImage(Image, Directory);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(Database.fileCount(), 2u);
  ASSERT_TRUE(Database.automaticSelection());
  EXPECT_EQ(Database.automaticSelection()->SelectedFiles, 2u);

  // Explicit directory loads bypass automatic platform selection.
  Error = Database.loadDirectory(Directory / Relative);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  ASSERT_FALSE(
      Database.loadPatternText("EEFF 00 0000 0002 :0000 manual\n", "manual"));
  EXPECT_FALSE(Database.automaticSelection());
}

TEST(SignatureDBTransactions, ReloadingOneSourceReplacesItsOldModules) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText("AABB 00 0000 0002 :0000 first_name\n",
                                        "same-source"));

  ASSERT_FALSE(Database.loadPatternText("CCDD 00 0000 0002 :0000 second_name\n",
                                        "same-source"));

  EXPECT_EQ(Database.moduleCount(), 1u);
  EXPECT_EQ(Database.fileCount(), 1u);
}

TEST(SignatureDBTransactions, SuccessfulReloadClearsStaleMatches) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText("AABB 00 0000 0002 :0000 old_name\n",
                                        "same-source"));
  const neverd::BinaryImage Image = makeMatchingImage();
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);

  ASSERT_FALSE(Database.loadPatternText("CCDD 00 0000 0002 :0000 new_name\n",
                                        "same-source"));

  EXPECT_TRUE(Database.matches().empty());
}

TEST(SignatureDBTransactions, FailedReloadPreservesExistingMatches) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText("AABB 00 0000 0002 :0000 old_name\n",
                                        "same-source"));
  const neverd::BinaryImage Image = makeMatchingImage();
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);

  llvm::Error Error =
      Database.loadPatternText("this record is malformed\n", "same-source");

  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_EQ(Database.matches().front().Name, "old_name");
}

TEST_F(SignatureDirectoryTest, DirectoryFailureLeavesPriorStateUntouched) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 original_name\n", "original"));
  const neverd::BinaryImage Image = makeMatchingImage();
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);
  write("a-valid.pat", "CCDD 00 0000 0002 :0000 new_name\n");
  write("z-invalid.pat", "this record is malformed\n");

  llvm::Error Error = Database.loadDirectory(Directory);

  const bool Failed = static_cast<bool>(Error);
  EXPECT_TRUE(Failed) << "a malformed file must fail the directory batch";
  if (Failed)
    EXPECT_NE(llvm::toString(std::move(Error)).find("z-invalid.pat"),
              std::string::npos);
  EXPECT_EQ(Database.moduleCount(), 1u);
  EXPECT_EQ(Database.fileCount(), 1u);
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_EQ(Database.matches().front().Name, "original_name");
}

TEST_F(SignatureDirectoryTest, ReportsTheFirstInvalidFileBySortedPath) {
  write("z-invalid.pat", "z is malformed\n");
  write("a-invalid.pat", "a is malformed\n");
  SignatureDB Database;

  llvm::Error Error = Database.loadDirectory(Directory);

  ASSERT_TRUE(static_cast<bool>(Error));
  const std::string Message = llvm::toString(std::move(Error));
  EXPECT_NE(Message.find("a-invalid.pat"), std::string::npos) << Message;
  EXPECT_EQ(Message.find("z-invalid.pat"), std::string::npos) << Message;
}

TEST_F(SignatureDirectoryTest, ReportsALineFarIntoALargeFile) {
  // Large enough to be cut into several chunks, with the malformed line in
  // one of the later ones.
  std::string Large;
  unsigned Line = 0;
  while (Large.size() < 3 * 256 * 1024) {
    ++Line;
    Large += Line % 50 == 0 ? "; a comment line counts too\n"
                            : "CCDD 00 0000 0002 :0000 routine_" +
                                  std::to_string(Line) + "\n";
  }
  Large += "this record is malformed\n";
  write("a-valid.pat", "AABB 00 0000 0002 :0000 fine\n");
  write("b-large.pat", Large);
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 original_name\n", "original"));

  llvm::Error Error = Database.loadDirectory(Directory);

  ASSERT_TRUE(static_cast<bool>(Error));
  const std::string Message = llvm::toString(std::move(Error));
  EXPECT_NE(Message.find("b-large.pat: pattern line " +
                         std::to_string(Line + 1) + ": invalid hex byte: th"),
            std::string::npos)
      << Message;
  EXPECT_EQ(Database.moduleCount(), 1u);
  EXPECT_EQ(Database.fileCount(), 1u);
}

TEST_F(SignatureDirectoryTest, LargeFilesKeepTheirLinesInOrder) {
  std::string Large;
  for (unsigned Line = 0; Line < 40000; ++Line)
    Large += "AABB 00 0000 0002 :0000 routine_" + std::to_string(Line) + "\n";
  write("large.pat", Large);
  SignatureDB Database;
  ASSERT_FALSE(Database.loadDirectory(Directory));
  ASSERT_EQ(Database.moduleCount(), 40000u);

  Database.apply(makeMatchingImage(), {0x1000});

  ASSERT_EQ(Database.matches().size(), 40000u);
  for (unsigned Line = 0; Line < 40000; ++Line)
    ASSERT_EQ(Database.matches()[Line].Name, "routine_" + std::to_string(Line));
}

TEST_F(SignatureDirectoryTest, APartsMatchesNameItsLibrary) {
  write("ubuntu-libc6.pat", "CCDD 00 0000 0002 :0000 other_part\n");
  write("ubuntu-libc6.part2.pat", "AABB 00 0000 0002 :0000 puts\n");
  SignatureDB Database;
  ASSERT_FALSE(Database.loadDirectory(Directory));
  Database.apply(makeMatchingImage(), {0x1000});

  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_EQ(Database.matches()[0].LibraryName, "ubuntu-libc6");
}

TEST_F(SignatureDirectoryTest, ReloadReplacesTheWholeDirectorySnapshot) {
  write("a.pat", "AABB 00 0000 0002 :0000 first_name\n");
  write("b.pat", "CCDD 00 0000 0002 :0000 stale_name\n");
  SignatureDB Database;
  ASSERT_FALSE(Database.loadDirectory(Directory));
  ASSERT_EQ(Database.moduleCount(), 2u);
  ASSERT_EQ(Database.fileCount(), 2u);

  ASSERT_TRUE(std::filesystem::remove(Directory / "b.pat"));
  write("a.pat", "EEFF 00 0000 0002 :0000 replacement_name\n");
  ASSERT_FALSE(Database.loadDirectory(Directory));

  EXPECT_EQ(Database.moduleCount(), 1u);
  EXPECT_EQ(Database.fileCount(), 1u);
}

TEST(SignatureDBAddresses, NeverWrapsASecondaryPublicNameAddress) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0001 secondary_name\n", "overflow"));

  neverd::BinaryImage Image;
  neverd::Segment Code;
  Code.VA = std::numeric_limits<uint64_t>::max();
  Code.Size = 2;
  Code.FileSz = 2;
  Code.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Code.Data = {0xAA, 0xBB};
  Image.Segments.push_back(std::move(Code));

  Database.apply(Image, {std::numeric_limits<uint64_t>::max()});

  EXPECT_TRUE(Database.matches().empty());
  EXPECT_TRUE(Database.buildCandidateNameMap().empty());
}

TEST(SignatureDBAddresses, OmitsDisputedNamesFromTheCandidateMap) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText("AABB 00 0000 0002 :0000 first_name\n"
                                        "AABB 00 0000 0002 :0000 second_name\n",
                                        "ambiguous"));
  const neverd::BinaryImage Image = makeMatchingImage();

  Database.apply(Image, {0x1000});

  ASSERT_EQ(Database.matches().size(), 2u);
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1000), 0u);
}

namespace {

/// An x64 image with a caller at 0x1000 whose `call` at 0x1004 goes to
/// \p CallTarget, a callee at 0x1100, and an incremental-linking thunk at
/// 0x1200 that jumps to the callee.
neverd::BinaryImage makeCallingImage(uint64_t CallTarget) {
  neverd::BinaryImage Image;
  Image.Arch = neverd::Arch::X64;
  Image.Bits = neverd::Bitness::Bits64;
  neverd::Segment Code;
  Code.VA = 0x1000;
  Code.Size = 0x300;
  Code.FileSz = 0x300;
  Code.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Code.Data.assign(0x300, 0xCC);
  const uint8_t Caller[] = {0x55, 0x48, 0x89, 0xE5, 0xE8, 0,
                            0,    0,    0,    0x5D, 0xC3};
  std::copy(std::begin(Caller), std::end(Caller), Code.Data.begin());
  const int32_t Disp = static_cast<int32_t>(CallTarget - 0x1009);
  std::memcpy(Code.Data.data() + 5, &Disp, sizeof(Disp));
  const uint8_t Callee[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xC3};
  std::copy(std::begin(Callee), std::end(Callee), Code.Data.begin() + 0x100);
  Code.Data[0x200] = 0xE9;
  const int32_t Jump = 0x1100 - 0x1205;
  std::memcpy(Code.Data.data() + 0x201, &Jump, sizeof(Jump));
  Image.Segments.push_back(std::move(Code));
  return Image;
}

constexpr const char *CalleeLine = "AABBCCDDC3 00 0000 0005 :0000 callee\n";

std::string callerLine(llvm::StringRef Name, llvm::StringRef References) {
  return ("554889E5E8........5DC3 00 0000 000B :0000 " + Name + References +
          "\n")
      .str();
}

} // namespace

TEST(SignatureDBReferences, ACallToARoutineNamedOtherwiseDropsTheMatch) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("caller", " ^0005 somebody_else"), "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});

  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.count(0x1000), 0u);
  EXPECT_EQ(Names.at(0x1100), "callee");
}

TEST(SignatureDBIdentity, ConfirmedReferencesDoNotRecoverErasedTypes) {
  // The same load/call sequence can implement read(First *) or read(Second *)
  // when the nominally different types share a layout. Naming its callee does
  // not recover the caller's erased source type, even with no known collision.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("read_first", " ^0005 callee"), "types"));
  auto Image = makeCallingImage(0x1100);
  neverd::Symbol Symbol;
  Symbol.Addr = 0x1100;
  Symbol.Name = "callee";
  Symbol.IsFunc = true;
  Symbol.Origin = neverd::NameOrigin::Stated;
  Image.Symbols.push_back(Symbol);
  Database.apply(Image, {0x1000, 0x1100});
  ASSERT_NE(Database.findMatch(0x1000), nullptr);
  EXPECT_TRUE(Database.findMatch(0x1000)->Confirmed);
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "read_first");
  EXPECT_EQ(Database.buildNameMap().count(0x1000), 0u);
  EXPECT_EQ(Database.buildNameMap().at(0x1100), "callee");
}

TEST(SignatureDBReferences, ACallNothingNamesKeepsTheMatch) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("caller", " ^0005 somebody_else"), "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, AConfirmedCallSettlesIdenticalBytes) {
  // Two routines with the same bytes: only what one of them calls is there.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("caller_of_callee", " ^0005 callee") +
          callerLine("unreferenced_twin", ""),
      "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller_of_callee");
  for (const SigMatch &Match : Database.matches())
    EXPECT_EQ(Match.Confirmed, Match.Name == "caller_of_callee") << Match.Name;
}

TEST(SignatureDBReferences, ThePatternOfADisputedCalleeConfirms) {
  // The callee's bytes name two routines, so nothing names 0x1100, but the
  // referenced routine's own pattern still matches there.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      std::string(CalleeLine) + "AABBCCDDC3 00 0000 0005 :0000 callee_twin\n" +
          callerLine("caller_of_callee", " ^0005 callee") +
          callerLine("unreferenced_twin", ""),
      "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});

  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.count(0x1100), 0u);
  EXPECT_EQ(Names.at(0x1000), "caller_of_callee");
}

TEST(SignatureDBReferences, IncrementalLinkingThunksAreFollowed) {
  // The call reaches `jmp 0x1100`: an incremental-linking thunk to the
  // callee confirms a reference to it.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("caller", " ^0005 callee") +
          callerLine("caller_twin", " ^0005 somebody_else"),
      "refs"));
  Database.apply(makeCallingImage(0x1200), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, ARoutineThatOnlyJumpsOnContradictsNothing) {
  // The same `jmp 0x1100` is also what `free` compiles to when all it does
  // is tail-call `_free_base`: a call to it is a call to `free`, which the
  // routine it reaches, named otherwise, does not contradict.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("caller", " ^0005 free"), "refs"));
  Database.apply(makeCallingImage(0x1200), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
  for (const SigMatch &Match : Database.matches())
    if (Match.Address == 0x1000)
      EXPECT_FALSE(Match.Confirmed);
}

namespace {

/// makeCallingImage(0x1200) in \p Format, whose 0x1200 is the stub of an
/// import named \p Import: `jmp [rip+slot]`, the slot the import's own.
neverd::BinaryImage makeImportCallingImage(neverd::BinaryFormat Format,
                                           llvm::StringRef Import) {
  neverd::BinaryImage Image = makeCallingImage(0x1200);
  Image.Format = Format;
  constexpr uint64_t Slot = 0x2000;
  std::vector<uint8_t> &Data = Image.Segments[0].Data;
  Data[0x200] = 0xFF;
  Data[0x201] = 0x25;
  const int32_t Disp = static_cast<int32_t>(Slot - 0x1206);
  std::memcpy(Data.data() + 0x202, &Disp, sizeof(Disp));
  neverd::Import Imp;
  Imp.Module = "libc.so";
  Imp.Name = Import.str();
  Imp.IATAddr = Slot;
  Image.Imports.push_back(std::move(Imp));
  return Image;
}

/// ctype_byname<wchar_t>'s do_toupper and do_tolower: the same bytes, told
/// apart only by the import each calls.
std::string twinCallerLines(llvm::StringRef Offset) {
  return callerLine("do_toupper", (" ^" + Offset + " towupper_l").str()) +
         callerLine("do_tolower", (" ^" + Offset + " towlower_l").str());
}

} // namespace

TEST(SignatureDBReferences, ReferencesAtOneOffsetAreAlternatives) {
  // The branch reaches one of the routines its references name: the callee
  // settles the line naming it among others, and contradicts the one that
  // names only others.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("caller", " ^0005 callee ^0005 callee_default") +
          callerLine("caller_twin", " ^0005 other ^0005 other_default"),
      "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});

  ASSERT_EQ(Database.matches().size(), 2u);
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
  for (const SigMatch &Match : Database.matches())
    if (Match.Address == 0x1000)
      EXPECT_TRUE(Match.Confirmed);
}

TEST(SignatureDBReferences, AnELFImportsStubSettlesACallToIt) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(twinCallerLines("0005"), "refs"));
  Database.apply(
      makeImportCallingImage(neverd::BinaryFormat::ELF, "towlower_l"),
      {0x1000});

  ASSERT_EQ(Database.matches().size(), 1u)
      << "the call to another import contradicts do_toupper";
  EXPECT_EQ(Database.matches()[0].Name, "do_tolower");
  EXPECT_TRUE(Database.matches()[0].Confirmed);
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "do_tolower");
}

TEST(SignatureDBReferences, AVersionedReferenceNamesTheImportItBindsTo) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("caller", " ^0005 memcpy@GLIBC_2.2.5"), "refs"));
  Database.apply(makeImportCallingImage(neverd::BinaryFormat::ELF, "memcpy"),
                 {0x1000});

  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_TRUE(Database.matches()[0].Confirmed);
}

TEST(SignatureDBReferences, ACOFFImportThunkSettlesNothing) {
  // A COFF import is named after the export, not after the decorated symbol
  // the library called.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(twinCallerLines("0005"), "refs"));
  Database.apply(
      makeImportCallingImage(neverd::BinaryFormat::COFF, "towlower_l"),
      {0x1000});

  EXPECT_EQ(Database.matches().size(), 2u);
  for (const SigMatch &Match : Database.matches())
    EXPECT_FALSE(Match.Confirmed) << Match.Name;
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1000), 0u);
}

namespace {

std::string importCallerLine(llvm::StringRef Name, llvm::StringRef Reference) {
  return ("90909090FF15........90909090909090909090909090909090909090909090"
          " 00 0000 0020 :0000 " +
          Name + " ^0006 " + Reference + "\n")
      .str();
}

neverd::BinaryImage makeIndirectImportImage(neverd::Arch Arch) {
  neverd::BinaryImage Image;
  Image.Format = neverd::BinaryFormat::COFF;
  Image.Arch = Arch;
  Image.Bits = Arch == neverd::Arch::X64 ? neverd::Bitness::Bits64
                                         : neverd::Bitness::Bits32;
  neverd::Segment Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 32;
  Code.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Code.Data.assign(32, 0x90);
  Code.Data[4] = 0xff;
  Code.Data[5] = 0x15;
  const uint32_t Field = Arch == neverd::Arch::X64 ? 0x2000 - 0x100a : 0x2000;
  std::memcpy(Code.Data.data() + 6, &Field, sizeof(Field));
  Image.Segments.push_back(std::move(Code));
  neverd::Segment Slots;
  Slots.VA = 0x2000;
  Slots.Size = Slots.FileSz = 8;
  Slots.Flags = neverd::SegmentFlags::Readable;
  Slots.Data.assign(8, 0);
  Image.Segments.push_back(std::move(Slots));
  neverd::Import Imp;
  Imp.Module = "runtime.dll";
  Imp.Name = "RuntimeB";
  Imp.IATAddr = 0x2000;
  Image.Imports.push_back(Imp);
  return Image;
}

} // namespace

TEST(SignatureDBReferences, COFFImportSlotsDisambiguateCandidatesNotIdentity) {
  for (const auto Arch : {neverd::Arch::X86, neverd::Arch::X64}) {
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(
        importCallerLine("wrapper_a", "__imp_RuntimeA") +
            importCallerLine("wrapper_b", "__imp_RuntimeB"),
        "imports"));
    auto Image = makeIndirectImportImage(Arch);
    Database.apply(Image, {0x1000});
    ASSERT_EQ(Database.matches().size(), 2u);
    EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "wrapper_b");
    for (const SigMatch &Match : Database.matches())
      EXPECT_EQ(Match.Confirmed, Match.Name == "wrapper_b");
    EXPECT_TRUE(Database.buildNameMap().empty());
    // The new evidence is withdrawn when the import spelling changes.
    Image.Imports.front().Name = "unresolved_alias";
    Database.apply(Image, {0x1000});
    EXPECT_TRUE(Database.buildCandidateNameMap().empty());
    for (const SigMatch &Match : Database.matches())
      EXPECT_FALSE(Match.Confirmed);
  }
}

TEST(SignatureDBReferences, COFFImportSlotRequiresExactMappedEvidence) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      importCallerLine("wrapper", "__imp_RuntimeB"), "imports"));
  const auto Unconfirmed = [&](const neverd::BinaryImage &Image) {
    Database.apply(Image, {0x1000});
    ASSERT_EQ(Database.matches().size(), 1u);
    EXPECT_FALSE(Database.matches().front().Confirmed);
    EXPECT_TRUE(Database.buildNameMap().empty());
  };
  auto Image = makeIndirectImportImage(neverd::Arch::X64);
  Image.Imports.front().Name.clear();
  Unconfirmed(Image);
  Image = makeIndirectImportImage(neverd::Arch::X64);
  Image.Imports.front().Name = "runtimeb";
  Unconfirmed(Image);
  Image = makeIndirectImportImage(neverd::Arch::X64);
  Image.Imports.front().IATAddr += 1;
  Unconfirmed(Image);
  Image = makeIndirectImportImage(neverd::Arch::X64);
  Image.Segments.pop_back();
  Unconfirmed(Image);
  Image = makeIndirectImportImage(neverd::Arch::X64);
  Image.Imports.push_back(Image.Imports.front());
  Image.Imports.back().Name = "different_import";
  Unconfirmed(Image);
  Image = makeIndirectImportImage(neverd::Arch::X64);
  Image.Format = neverd::BinaryFormat::ELF;
  Unconfirmed(Image);
}

TEST(SignatureDBReferences, COFFImportJumpsAndNegativeDisplacements) {
  for (const auto Arch : {neverd::Arch::X86, neverd::Arch::X64}) {
    for (bool Jump : {false, true}) {
      auto Image = makeIndirectImportImage(Arch);
      // Put the slot below the caller: x64 must sign-extend its displacement.
      Image.Segments.back().VA = Image.Imports.front().IATAddr = 0x800;
      const uint32_t Field = Arch == neverd::Arch::X64
                                 ? uint32_t(0x800 - 0x100a)
                                 : uint32_t(0x800);
      std::memcpy(Image.Segments.front().Data.data() + 6, &Field,
                  sizeof(Field));
      std::string Pattern = importCallerLine("wrapper", "__imp_RuntimeB");
      if (Jump) {
        Image.Segments.front().Data[5] = 0x25;
        Pattern[10] = '2';
      }
      SignatureDB Database;
      ASSERT_FALSE(Database.loadPatternText(Pattern, "imports"));
      Database.apply(Image, {0x1000});
      ASSERT_EQ(Database.matches().size(), 1u);
      EXPECT_TRUE(Database.matches().front().Confirmed);
      EXPECT_TRUE(Database.buildNameMap().empty());
    }
  }
}

TEST(SignatureDBReferences, COFFImportPrefixesRemainUnconfirmed) {
  for (uint8_t Prefix : {0x64, 0x65, 0x66, 0x67, 0x49, 0x4c, 0x4f}) {
    auto Image = makeIndirectImportImage(neverd::Arch::X64);
    Image.Segments.front().Data[3] = Prefix;
    std::string Pattern = importCallerLine("wrapper", "__imp_RuntimeB");
    // Keep a byte match while refusing to assign unprefixed slot semantics.
    Pattern[6] = Pattern[7] = '.';
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(Pattern, "imports"));
    Database.apply(Image, {0x1000});
    ASSERT_EQ(Database.matches().size(), 1u);
    EXPECT_FALSE(Database.matches().front().Confirmed);
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}

TEST(SignatureDBReferences, COFFREXWImportsRequireExactSlotEvidence) {
  for (bool Jump : {false, true}) {
    for (bool ExtraPrefix : {false, true}) {
      auto Image = makeIndirectImportImage(neverd::Arch::X64);
      auto &Code = Image.Segments.front().Data;
      Code[2] = ExtraPrefix ? 0x67 : 0x90;
      Code[3] = 0x48;
      Code[5] = Jump ? 0x25 : 0x15;
      Image.Segments.back().VA = Image.Imports.front().IATAddr = 0x800;
      neverd::writeLE<uint32_t>(Code.data() + 6, uint32_t(0x800 - 0x100a));
      std::string Pattern = importCallerLine("wrapper", "__imp_RuntimeB");
      Pattern.replace(4, 4, "....");
      if (Jump)
        Pattern[10] = '2';
      SignatureDB Database;
      ASSERT_FALSE(Database.loadPatternText(Pattern, "imports"));
      Database.apply(Image, {0x1000});
      ASSERT_EQ(Database.matches().size(), 1u);
      EXPECT_EQ(Database.matches().front().Confirmed, !ExtraPrefix);
      EXPECT_TRUE(Database.buildNameMap().empty());
      Image.Imports.front().Name = "different";
      Database.apply(Image, {0x1000});
      ASSERT_EQ(Database.matches().size(), 1u);
      EXPECT_FALSE(Database.matches().front().Confirmed);
    }
  }
}

namespace {

std::string vtableCallerLine(llvm::StringRef Name, llvm::StringRef Reference) {
  return ("90909090488D05........909090909090909090909090909090909090909090"
          " 00 0000 0020 :0000 " +
          Name + " ^0007 " + Reference + "\n")
      .str();
}

neverd::BinaryImage makeVTableImage() {
  auto Image = makeIndirectImportImage(neverd::Arch::X64);
  Image.Imports.clear();
  Image.Base = 0x1000;
  auto &Code = Image.Segments.front().Data;
  Code[4] = 0x48;
  Code[5] = 0x8d;
  Code[6] = 0x05;
  neverd::writeLE<uint32_t>(Code.data() + 7, 0x2108 - 0x100b);
  auto &Data = Image.Segments.back();
  Data.Size = Data.FileSz = 0x300;
  Data.Data.assign(Data.Size, 0);
  auto Put = [&](size_t At, uint32_t Value) {
    neverd::writeLE<uint32_t>(Data.Data.data() + At, Value);
  };
  neverd::writeLE<uint64_t>(Data.Data.data() + 0x100, 0x2140);
  neverd::writeLE<uint64_t>(Data.Data.data() + 0x108, 0x1000);
  Put(0x140, 1); // Locator version, zero offset and construction displacement.
  Put(0x14c, 0x1200);     // Type descriptor.
  Put(0x150, 0x1180);     // Class hierarchy.
  Put(0x154, 0x1140);     // Locator self RVA.
  Put(0x188, 1);          // One base: the complete class itself.
  Put(0x18c, 0x11a0);     // Base array.
  Put(0x1a0, 0x11c0);     // Self base descriptor.
  Put(0x1c0, 0x1200);     // Same type descriptor.
  Put(0x1cc, 0xffffffff); // No virtual base displacement.
  Put(0x1d4, 0x40);       // HASPCHD.
  Put(0x1d8, 0x1180);     // Same hierarchy.
  constexpr char Name[] = ".?AVWidget@@";
  std::memcpy(Data.Data.data() + 0x210, Name, sizeof(Name));
  return Image;
}

} // namespace

TEST(SignatureDBReferences, VTableTypeDisambiguatesCandidatesNotIdentity) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      vtableCallerLine("widget_ctor", "??_7Widget@@6B@") +
          vtableCallerLine("other_ctor", "??_7Other@@6B@"),
      "constructors"));
  auto Image = makeVTableImage();
  const auto Type =
      neverd::coff_loader::readPrimaryVTableTypeName(Image, 0x2108);
  ASSERT_TRUE(Type);
  EXPECT_EQ(*Type, "Widget@@");
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_EQ(Database.matches().front().Name, "widget_ctor");
  EXPECT_TRUE(Database.matches().front().Confirmed);
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "widget_ctor");
  EXPECT_TRUE(Database.buildNameMap().empty());
  // Reapplication must not reuse metadata from the preceding image.
  Image.Segments.back().Data[0x140] = 0;
  Database.apply(Image, {0x1000});
  EXPECT_EQ(Database.matches().size(), 2u);
  EXPECT_TRUE(Database.buildCandidateNameMap().empty());
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBReferences, VTableMalformedMetadataCannotConfirmOrContradict) {
  const std::vector<std::pair<size_t, uint32_t>> Corruptions = {
      {0x100, 0x9000},     {0x108, 0x2000}, {0x140, 0},
      {0x144, 8},          {0x148, 4},      {0x14c, 0},
      {0x14c, 0xffffffff}, {0x150, 0},      {0x154, 0x1144},
      {0x180, 1},          {0x188, 0},      {0x188, 0xffffffff},
      {0x18c, 0x12ff},     {0x1a0, 0x12ff}, {0x1c0, 0x1204},
      {0x1c4, 1},          {0x1c8, 4},      {0x1cc, 0},
      {0x1d0, 4},          {0x1d4, 0},      {0x1d8, 0x1184},
      {0x210, 0}};
  for (const auto &[Offset, Value] : Corruptions) {
    auto Image = makeVTableImage();
    neverd::writeLE<uint32_t>(Image.Segments.back().Data.data() + Offset,
                              Value);
    EXPECT_FALSE(neverd::coff_loader::readPrimaryVTableTypeName(Image, 0x2108))
        << Offset;
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(
        vtableCallerLine("widget_ctor", "??_7Widget@@6B@") +
            vtableCallerLine("other_ctor", "??_7Other@@6B@"),
        "constructors"));
    Database.apply(Image, {0x1000});
    ASSERT_EQ(Database.matches().size(), 2u) << Offset;
    for (const auto &Match : Database.matches())
      EXPECT_FALSE(Match.Confirmed) << Offset;
    EXPECT_TRUE(Database.buildCandidateNameMap().empty()) << Offset;
  }
}

TEST(SignatureDBReferences, VTableEvidenceRequiresCompleteSupportedData) {
  for (unsigned Variant = 0; Variant != 10; ++Variant) {
    auto Image = makeVTableImage();
    auto &Data = Image.Segments.back();
    switch (Variant) {
    case 0:
      Image.Format = neverd::BinaryFormat::ELF;
      break;
    case 1:
      Image.Arch = neverd::Arch::X86;
      break;
    case 2:
      Image.Bits = neverd::Bitness::Bits32;
      break;
    case 3:
      Data.Flags = Data.Flags | neverd::SegmentFlags::Writable;
      break;
    case 4:
      Data.Flags = Data.Flags | neverd::SegmentFlags::Executable;
      break;
    case 5:
      Data.Data.resize(0x21b);
      break; // No string terminator.
    case 6:
      Data.Size = 0x21b;
      break;
    case 7:
      std::fill(Data.Data.begin() + 0x21b, Data.Data.end(), 'x');
      break;
    case 8:
      Data.Data[0x214] = ' ';
      break;
    case 9:
      Image.Base = std::numeric_limits<uint64_t>::max() - 0x1000;
      break;
    }
    EXPECT_FALSE(neverd::coff_loader::readPrimaryVTableTypeName(Image, 0x2108))
        << Variant;
  }
  auto Image = makeVTableImage();
  Image.Segments.back().Data[0x213] = 'U'; // Struct type spelling.
  EXPECT_EQ(*neverd::coff_loader::readPrimaryVTableTypeName(Image, 0x2108),
            "Widget@@");
}

TEST(SignatureDBReferences, VTableAddressRequiresCanonicalInstruction) {
  for (uint8_t Prefix : {0x65, 0x67, 0x48}) {
    auto Image = makeVTableImage();
    Image.Segments.front().Data[3] = Prefix;
    std::string Pattern = vtableCallerLine("ctor", "??_7Widget@@6B@");
    Pattern[6] = Pattern[7] = '.';
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(Pattern, "constructors"));
    Database.apply(Image, {0x1000});
    ASSERT_EQ(Database.matches().size(), 1u);
    EXPECT_FALSE(Database.matches().front().Confirmed);
  }
  auto Image = makeVTableImage();
  Image.Segments.front().VA = 0x3000;
  neverd::writeLE<uint32_t>(Image.Segments.front().Data.data() + 7,
                            uint32_t(0x2108 - 0x300b));
  neverd::writeLE<uint64_t>(Image.Segments.back().Data.data() + 0x108, 0x3000);
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      vtableCallerLine("ctor", "??_7Widget@@6B@"), "constructors"));
  Database.apply(Image, {0x3000});
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_TRUE(Database.matches().front().Confirmed);
}

TEST(SignatureDBReferences, EveryImportReferenceMustBeConfirmed) {
  auto Image = makeIndirectImportImage(neverd::Arch::X64);
  auto &Code = Image.Segments.front().Data;
  Code[16] = 0xff;
  Code[17] = 0x15;
  const int32_t Field = 0x2008 - 0x1016;
  std::memcpy(Code.data() + 18, &Field, sizeof(Field));
  Image.Segments.back().Data.resize(16, 0);
  Image.Segments.back().Size = Image.Segments.back().FileSz = 16;
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "90909090FF15........909090909090FF15........90909090909090909090"
      " 00 0000 0020 :0000 wrapper ^0006 __imp_RuntimeB ^0012 __imp_Other\n",
      "imports"));
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_FALSE(Database.matches().front().Confirmed);
  Image.Imports.push_back(Image.Imports.front());
  Image.Imports.back().IATAddr = 0x2008;
  Image.Imports.back().Name = "Other";
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_TRUE(Database.matches().front().Confirmed);
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBReferences, WhatReferencesNameSettlesTheirCallersInTurn) {
  // outer (0x1000) calls inner (0x1100), which calls import_a's stub
  // (0x1200).  Each has a twin with its bytes; inner's twins are told apart
  // by the import they call, and outer's only by which inner they call.
  neverd::BinaryImage Image =
      makeImportCallingImage(neverd::BinaryFormat::ELF, "import_a");
  std::vector<uint8_t> &Data = Image.Segments[0].Data;
  const int32_t Outer = 0x1100 - 0x1009;
  std::memcpy(Data.data() + 5, &Outer, sizeof(Outer));
  // push rbx; call 0x1200; pop rbx; ret
  const uint8_t Inner[] = {0x53, 0xE8, 0, 0, 0, 0, 0x5B, 0xC3};
  std::copy(std::begin(Inner), std::end(Inner), Data.begin() + 0x100);
  const int32_t ToStub = 0x1200 - 0x1106;
  std::memcpy(Data.data() + 0x102, &ToStub, sizeof(ToStub));

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("outer_a", " ^0005 inner_a") +
          callerLine("outer_b", " ^0005 inner_b") +
          "53E8........5BC3 00 0000 0008 :0000 inner_a ^0002 import_a\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_b ^0002 import_b\n",
      "refs"));
  Database.apply(Image, {0x1000, 0x1100});

  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.at(0x1100), "inner_a");
  EXPECT_EQ(Names.at(0x1000), "outer_a");
}

namespace {

/// A caller at 0x1000 reaches 0x1100, whose call reaches import_a at 0x1200.
neverd::BinaryImage makeNestedCallingImage() {
  neverd::BinaryImage Image =
      makeImportCallingImage(neverd::BinaryFormat::ELF, "import_a");
  auto &Data = Image.Segments[0].Data;
  const int32_t Outer = 0x1100 - 0x1009;
  std::memcpy(Data.data() + 5, &Outer, sizeof(Outer));
  const uint8_t Inner[] = {0x53, 0xE8, 0, 0, 0, 0, 0x5B, 0xC3};
  std::copy(std::begin(Inner), std::end(Inner), Data.begin() + 0x100);
  const int32_t ToStub = 0x1200 - 0x1106;
  std::memcpy(Data.data() + 0x102, &ToStub, sizeof(ToStub));

  return Image;
}

} // namespace

TEST(SignatureDBReferences, RejectedCalleeCannotRemainConfirmationEvidence) {
  // The inner candidate's call contradicts its claimed identity. Two other
  // inner names stay ambiguous, so its settled name is absent both before
  // and after that rejection. Its raw byte hit must not keep confirming the
  // outer candidate, and the outer must be reconsidered even though the
  // inner's settled name did not change.
  neverd::BinaryImage Image = makeNestedCallingImage();
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("outer_rejected", " ^0005 inner_rejected") +
          callerLine("outer_twin", "") +
          "53E8........5BC3 00 0000 0008 :0000 inner_rejected ^0002 import_b\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_twin_a\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_twin_b\n",
      "refs"));
  Database.apply(Image, {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1000), 0u);
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
  for (const SigMatch &Match : Database.matches()) {
    EXPECT_NE(Match.Name, "inner_rejected");
    if (Match.Address == 0x1000)
      EXPECT_FALSE(Match.Confirmed) << Match.Name;
  }
}

TEST(SignatureDBReferences, WithdrawingEvidenceDoesNotPermanentlyDropACaller) {
  // The outer candidate first appears confirmed, which provisionally rejects
  // grand_b. Withdrawing that evidence must restore grand_b as well.
  neverd::BinaryImage Image = makeNestedCallingImage();
  auto &Code = Image.Segments[0];
  auto &Data = Code.Data;
  Code.Size = Code.FileSz = 0x400;
  Data.resize(0x400, 0xCC);
  const uint8_t Grand[] = {0x56, 0xE8, 0, 0, 0, 0, 0x5E, 0xC3};
  std::copy(std::begin(Grand), std::end(Grand), Data.begin() + 0x300);
  const int32_t ToOuter = 0x1000 - 0x1306;
  std::memcpy(Data.data() + 0x302, &ToOuter, sizeof(ToOuter));

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("outer_rejected", " ^0005 inner_rejected") +
          callerLine("outer_twin", "") +
          "53E8........5BC3 00 0000 0008 :0000 inner_rejected ^0002 import_b\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_twin_a\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_twin_b\n"
          "56E8........5EC3 00 0000 0008 :0000 grand_a ^0002 outer_rejected\n"
          "56E8........5EC3 00 0000 0008 :0000 grand_b ^0002 outer_twin\n",
      "refs"));
  Database.apply(Image, {0x1000, 0x1100, 0x1300});

  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1300), 0u);
  EXPECT_TRUE(
      std::any_of(Database.matches().begin(), Database.matches().end(),
                  [](const SigMatch &M) { return M.Name == "grand_b"; }));
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1000), 0u);
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
  for (const SigMatch &Match : Database.matches()) {
    EXPECT_NE(Match.Name, "inner_rejected");
    if (Match.Address == 0x1000)
      EXPECT_FALSE(Match.Confirmed) << Match.Name;
  }
}

TEST(SignatureDBReferences, AnotherViableModuleWithTheSameNameStillConfirms) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("outer", " ^0005 inner") + callerLine("outer_twin", "") +
          "53E8........5BC3 00 0000 0008 :0000 inner ^0002 import_b\n"
          "53E8........5BC3 00 0000 0008 :0000 inner\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_twin\n",
      "refs"));
  Database.apply(makeNestedCallingImage(), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "outer");
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
  EXPECT_EQ(std::count_if(Database.matches().begin(), Database.matches().end(),
                          [](const SigMatch &M) { return M.Name == "inner"; }),
            1);
  for (const SigMatch &Match : Database.matches())
    if (Match.Name == "outer")
      EXPECT_TRUE(Match.Confirmed);
}

TEST(SignatureDBReferences, ParallelWithdrawalAgreesWithSerialChecking) {
  neverd::BinaryImage Image = makeNestedCallingImage();
  auto &Code = Image.Segments[0];
  auto &Data = Code.Data;
  constexpr size_t CallerCount = 4096;
  constexpr uint64_t FirstCaller = 0x2000;
  constexpr size_t Stride = 16;
  const std::vector<uint8_t> Caller(Data.begin(), Data.begin() + 11);
  Code.Size = Code.FileSz = FirstCaller - Code.VA + CallerCount * Stride;
  Data.resize(Code.FileSz, 0xCC);
  std::vector<uint64_t> Entries{0x1100};
  for (size_t I = 0; I < CallerCount; ++I) {
    const uint64_t Address = FirstCaller + I * Stride;
    const size_t Offset = Address - Code.VA;
    std::copy(Caller.begin(), Caller.end(), Data.begin() + Offset);
    const int32_t Disp = static_cast<int32_t>(0x1100 - (Address + 9));
    std::memcpy(Data.data() + Offset + 5, &Disp, sizeof(Disp));
    Entries.push_back(Address);
  }

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("outer_rejected", " ^0005 inner_rejected") +
          callerLine("outer_twin", "") +
          "53E8........5BC3 00 0000 0008 :0000 inner_rejected ^0002 import_b\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_twin_a\n"
          "53E8........5BC3 00 0000 0008 :0000 inner_twin_b\n",
      "refs"));
  const unsigned Previous = neverd::workerThreadOverride().load();
  for (unsigned Threads : {1u, 4u}) {
    neverd::setWorkerThreadCount(Threads);
    Database.apply(Image, Entries);
    EXPECT_TRUE(Database.buildCandidateNameMap().empty());
    EXPECT_EQ(Database.matches().size(), 2 * CallerCount + 2);
    for (const SigMatch &Match : Database.matches())
      EXPECT_FALSE(Match.Confirmed);
    std::reverse(Entries.begin(), Entries.end());
  }
  neverd::setWorkerThreadCount(Previous);
}

TEST(SignatureDBReferences, UnsettledCyclesCannotNameTheirDependentCallers) {
  neverd::BinaryImage Image = makeNestedCallingImage();
  auto &Code = Image.Segments[0];
  auto &Data = Code.Data;
  // 0x1000 and 0x1100 each contradict the other's only proposed identity.
  const int32_t ToOuter = 0x1000 - 0x1106;
  std::memcpy(Data.data() + 0x102, &ToOuter, sizeof(ToOuter));
  Code.Size = Code.FileSz = 0x400;
  Data.resize(0x400, 0xCC);
  const uint8_t Grand[] = {0x56, 0xE8, 0, 0, 0, 0, 0x5E, 0xC3};
  std::copy(std::begin(Grand), std::end(Grand), Data.begin() + 0x300);
  const int32_t GrandCall = 0x1000 - 0x1306;
  std::memcpy(Data.data() + 0x302, &GrandCall, sizeof(GrandCall));
  const uint8_t Independent[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xC3};
  std::copy(std::begin(Independent), std::end(Independent),
            Data.begin() + 0x250);

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("outer", " ^0005 other_inner") +
          "53E8........5BC3 00 0000 0008 :0000 inner ^0002 other_outer\n"
          "56E8........5EC3 00 0000 0008 :0000 grand ^0002 outer\n"
          "56E8........5EC3 00 0000 0008 :0000 grand_twin\n" +
          CalleeLine,
      "refs"));
  for (const std::vector<uint64_t> Entries :
       {std::vector<uint64_t>{0x1000, 0x1100, 0x1250, 0x1300},
        std::vector<uint64_t>{0x1300, 0x1250, 0x1100, 0x1000}}) {
    Database.apply(Image, Entries);
    const auto Names = Database.buildCandidateNameMap();
    ASSERT_EQ(Names.size(), 1u);
    EXPECT_EQ(Names.at(0x1250), "callee");
  }
}

TEST(SignatureDBReferences, AStubTheLoaderPairedWithAnImportSettlesACall) {
  // AArch64: stp x29,x30,[sp,#-16]!; bl 0x1200; ldp x29,x30,[sp],#16; ret,
  // where 0x1200 is a PLT entry the loader paired with towlower_l.
  neverd::BinaryImage Image;
  Image.Arch = neverd::Arch::AArch64;
  Image.Bits = neverd::Bitness::Bits64;
  Image.Format = neverd::BinaryFormat::ELF;
  neverd::Segment Code;
  Code.VA = 0x1000;
  Code.Size = 0x300;
  Code.FileSz = 0x300;
  Code.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Code.Data.assign(0x300, 0);
  const uint32_t Words[] = {0xA9BF7BFD, 0x94000000 | ((0x1200 - 0x1004) / 4),
                            0xA8C17BFD, 0xD65F03C0};
  std::memcpy(Code.Data.data(), Words, sizeof(Words));
  Image.Segments.push_back(std::move(Code));
  neverd::Import Imp;
  Imp.Module = "libc.so";
  Imp.Name = "towlower_l";
  Imp.IATAddr = 0x2000;
  Image.Imports.push_back(std::move(Imp));
  ASSERT_TRUE(Image.recordImportStub(0x1200, 0));

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "FD7BBFA9........FD7BC1A8C0035FD6 00 0000 0010 :0000 do_toupper "
      "^0004 towupper_l\n"
      "FD7BBFA9........FD7BC1A8C0035FD6 00 0000 0010 :0000 do_tolower "
      "^0004 towlower_l\n",
      "refs"));
  Database.apply(Image, {0x1000});

  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_EQ(Database.matches()[0].Name, "do_tolower");
  EXPECT_TRUE(Database.matches()[0].Confirmed);
}

namespace {

void putWord(std::vector<uint8_t> &Data, size_t Offset, uint32_t Word) {
  std::memcpy(Data.data() + Offset, &Word, sizeof(Word));
}

/// A 32-bit ARM ELF image with 0x300 bytes of code at 0x1000.
neverd::BinaryImage makeARMImage(std::vector<uint8_t> Data) {
  neverd::BinaryImage Image;
  Image.Arch = neverd::Arch::ARM;
  Image.Bits = neverd::Bitness::Bits32;
  Image.Format = neverd::BinaryFormat::ELF;
  neverd::Segment Code;
  Code.VA = 0x1000;
  Code.Size = Data.size();
  Code.FileSz = Data.size();
  Code.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Code.Data = std::move(Data);
  Image.Segments.push_back(std::move(Code));
  return Image;
}

/// ARM state: `push {r11,lr}; bl <CallTarget>; pop {r11,pc}` at 0x1000,
/// `mov r0,#1; bx lr` at 0x1100, and a veneer `b 0x1100` at 0x1200.
neverd::BinaryImage makeARMStateCallingImage(uint64_t CallTarget) {
  std::vector<uint8_t> Data(0x300, 0);
  putWord(Data, 0x000, 0xE92D4800);
  putWord(Data, 0x004,
          0xEB000000 | (((CallTarget - 0x100C) >> 2) & 0x00FFFFFF));
  putWord(Data, 0x008, 0xE8BD8800);
  putWord(Data, 0x100, 0xE3A00001);
  putWord(Data, 0x104, 0xE12FFF1E);
  putWord(Data, 0x200, 0xEA000000 | (((0x1100 - 0x1208) >> 2) & 0x00FFFFFF));
  return makeARMImage(std::move(Data));
}

constexpr const char *ARMCalleeLine =
    "0100A0E31EFF2FE1 00 0000 0008 :0000 callee\n";

std::string armCallerLine(llvm::StringRef Name, llvm::StringRef References) {
  return ("00482DE9........0088BDE8 00 0000 000C :0000 " + Name + References +
          "\n")
      .str();
}

} // namespace

TEST(SignatureDBReferences, AnOddOffsetStatesAnARMStateBranch) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      ARMCalleeLine + armCallerLine("caller", " ^0005 callee") +
          armCallerLine("caller_twin", " ^0005 somebody_else"),
      "refs"));
  Database.apply(makeARMStateCallingImage(0x1100), {0x1000, 0x1100});

  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.at(0x1000), "caller");
  EXPECT_EQ(Names.at(0x1100), "callee");
  for (const SigMatch &Match : Database.matches())
    if (Match.Address == 0x1000)
      EXPECT_TRUE(Match.Confirmed) << Match.Name;
}

TEST(SignatureDBReferences, AnARMStateVeneerIsFollowedInARMState) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      ARMCalleeLine + armCallerLine("caller", " ^0005 callee") +
          armCallerLine("caller_twin", " ^0005 somebody_else"),
      "refs"));
  Database.apply(makeARMStateCallingImage(0x1200), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller")
      << "the veneer leads to the callee";
}

TEST(SignatureDBReferences, AnEvenOffsetStatesAThumbBranch) {
  // Thumb: `push {r7,lr}; bl 0x1100; pop {r7,pc}` at 0x1000 and
  // `movs r0,#1; bx lr` at 0x1100.
  std::vector<uint8_t> Data(0x300, 0);
  const uint8_t Caller[] = {0x80, 0xB5, 0x00, 0xF0, 0x7D, 0xF8, 0x80, 0xBD};
  std::copy(std::begin(Caller), std::end(Caller), Data.begin());
  const uint8_t Callee[] = {0x01, 0x20, 0x70, 0x47};
  std::copy(std::begin(Callee), std::end(Callee), Data.begin() + 0x100);

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "01207047 00 0000 0004 :0000 callee\n"
      "80B5........80BD 00 0000 0008 :0000 caller ^0002 callee\n"
      "80B5........80BD 00 0000 0008 :0000 caller_twin ^0002 somebody_else\n",
      "refs"));
  Database.apply(makeARMImage(std::move(Data)), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, AnARMLongBranchThunkIsFollowed) {
  // bl 0x1200, where lld's `movw ip, #0x1101; movt ip, #0; bx ip` enters
  // the Thumb routine `movs r0,#1; bx lr` at 0x1100.
  std::vector<uint8_t> Data(0x300, 0);
  putWord(Data, 0x000, 0xE92D4800);
  putWord(Data, 0x004, 0xEB000000 | (((0x1200 - 0x100C) >> 2) & 0x00FFFFFF));
  putWord(Data, 0x008, 0xE8BD8800);
  putWord(Data, 0x100, 0x47702001);
  putWord(Data, 0x200, 0xE301C101);
  putWord(Data, 0x204, 0xE340C000);
  putWord(Data, 0x208, 0xE12FFF1C);

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "01207047 00 0000 0004 :0000 callee\n" +
          armCallerLine("caller", " ^0005 callee") +
          armCallerLine("caller_twin", " ^0005 somebody_else"),
      "refs"));
  Database.apply(makeARMImage(std::move(Data)), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, AThumbPositionIndependentThunkIsFollowed) {
  // Thumb `bl 0x1200`, where lld's `movw ip; movt ip; add ip, pc; bx ip`
  // enters the ARM-state routine at 0x1100.
  std::vector<uint8_t> Data(0x300, 0);
  const uint8_t Caller[] = {0x80, 0xB5, 0x00, 0xF0, 0xFD, 0xF8, 0x80, 0xBD};
  std::copy(std::begin(Caller), std::end(Caller), Data.begin());
  putWord(Data, 0x100, 0xE3A00001);
  putWord(Data, 0x104, 0xE12FFF1E);
  const uint8_t Thunk[] = {0x4F, 0xF6, 0xF4, 0x6C, 0xCF, 0xF6,
                           0xFF, 0x7C, 0xFC, 0x44, 0x60, 0x47};
  std::copy(std::begin(Thunk), std::end(Thunk), Data.begin() + 0x200);

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      ARMCalleeLine +
          std::string(
              "80B5........80BD 00 0000 0008 :0000 caller ^0002 callee\n"
              "80B5........80BD 00 0000 0008 :0000 caller_twin ^0002 "
              "somebody_else\n"),
      "refs"));
  Database.apply(makeARMImage(std::move(Data)), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, AnARMv5LongBranchIsFollowed) {
  // bl 0x1200, where `ldr pc, [pc, #-4]; .word 0x1101` enters the Thumb
  // routine at 0x1100.
  std::vector<uint8_t> Data(0x300, 0);
  putWord(Data, 0x000, 0xE92D4800);
  putWord(Data, 0x004, 0xEB000000 | (((0x1200 - 0x100C) >> 2) & 0x00FFFFFF));
  putWord(Data, 0x008, 0xE8BD8800);
  putWord(Data, 0x100, 0x47702001);
  putWord(Data, 0x200, 0xE51FF004);
  putWord(Data, 0x204, 0x1101);

  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "01207047 00 0000 0004 :0000 callee\n" +
          armCallerLine("caller", " ^0005 callee") +
          armCallerLine("caller_twin", " ^0005 somebody_else"),
      "refs"));
  Database.apply(makeARMImage(std::move(Data)), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, AArch64RangeThunksAreFollowed) {
  // bl 0x1200, where a thunk enters `mov w0, #1; ret` at 0x1100: lld's
  // `adrp x16; add x16, x16, #0x100; br x16`, or `ldr x16, #8; br x16` and
  // the address.
  const std::vector<std::vector<uint32_t>> Thunks = {
      {0x90000010, 0x91040210, 0xD61F0200},
      {0x58000050, 0xD61F0200, 0x1100, 0}};
  for (const std::vector<uint32_t> &Thunk : Thunks) {
    neverd::BinaryImage Image;
    Image.Arch = neverd::Arch::AArch64;
    Image.Bits = neverd::Bitness::Bits64;
    Image.Format = neverd::BinaryFormat::ELF;
    neverd::Segment Code;
    Code.VA = 0x1000;
    Code.Size = 0x300;
    Code.FileSz = 0x300;
    Code.Flags =
        neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
    Code.Data.assign(0x300, 0);
    putWord(Code.Data, 0x000, 0xA9BF7BFD);
    putWord(Code.Data, 0x004, 0x94000000 | ((0x1200 - 0x1004) / 4));
    putWord(Code.Data, 0x008, 0xA8C17BFD);
    putWord(Code.Data, 0x00C, 0xD65F03C0);
    putWord(Code.Data, 0x100, 0x52800020);
    putWord(Code.Data, 0x104, 0xD65F03C0);
    for (size_t I = 0; I < Thunk.size(); ++I)
      putWord(Code.Data, 0x200 + 4 * I, Thunk[I]);
    Image.Segments.push_back(std::move(Code));

    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(
        "20008052C0035FD6 00 0000 0008 :0000 callee\n"
        "FD7BBFA9........FD7BC1A8C0035FD6 00 0000 0010 :0000 caller "
        "^0004 callee\n"
        "FD7BBFA9........FD7BC1A8C0035FD6 00 0000 0010 :0000 caller_twin "
        "^0004 somebody_else\n",
        "refs"));
    Database.apply(Image, {0x1000, 0x1100});

    EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "caller")
        << "thunk starting " << std::hex << Thunk[0];
  }
}

TEST(SignatureDBAliases, OneModulesNamesForAnOffsetAreOneRoutine) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 _IO_puts :0000 puts\n", "libc"));
  Database.apply(makeMatchingImage(), {0x1000});

  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_EQ(Database.matches()[0].Name, "puts");
  EXPECT_EQ(Database.matches()[0].Aliases,
            std::vector<std::string>{"_IO_puts"});
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "puts");
}

TEST(SignatureDBAliases, MatchesThatShareANameAgreeOnIt) {
  // Two builds of the library define different aliases for the routine.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 malloc :0000 __libc_malloc\n"
      "AABB 00 0000 0002 :0000 __libc_malloc :0000 __malloc\n",
      "libc"));
  Database.apply(makeMatchingImage(), {0x1000});

  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "__libc_malloc");
}

TEST(SignatureDBAliases, AliasSetsWithNothingInCommonAreDisputed) {
  SignatureDB Database;
  ASSERT_FALSE(
      Database.loadPatternText("AABB 00 0000 0002 :0000 puts :0000 _IO_puts\n"
                               "AABB 00 0000 0002 :0000 fputs\n",
                               "libc"));
  Database.apply(makeMatchingImage(), {0x1000});

  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1000), 0u);
}

TEST(SignatureDBAliases, ACallByAnyNameOfTheCalleeConfirms) {
  // The caller's object calls the routine by its internal name; the callee's
  // line shows it by its public one.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABBCCDDC3 00 0000 0005 :0000 callee :0000 __callee_internal\n" +
          callerLine("caller", " ^0005 __callee_internal") +
          callerLine("unreferenced_twin", ""),
      "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});

  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.at(0x1100), "callee");
  EXPECT_EQ(Names.at(0x1000), "caller");
}

namespace {
std::string requireReferences(std::string Line) {
  Line.insert(Line.find(" :"), " !refs");
  return Line;
}
} // namespace

TEST(SignatureDBContext, UnknownReferenceNeverAdmitsAByteCandidate) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      requireReferences(callerLine("wrapper", " ^0005 callee")), "context"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});
  EXPECT_TRUE(Database.matches().empty());
  EXPECT_TRUE(Database.buildCandidateNameMap().empty());
  ASSERT_FALSE(Database.loadPatternText(CalleeLine, "ordinary"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "wrapper");
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBContext, ImportAndTypeMetadataCanAnchorARequiredCandidate) {
  for (bool Type : {false, true}) {
    auto Image =
        Type ? makeVTableImage() : makeIndirectImportImage(neverd::Arch::X64);
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(
        requireReferences(Type ? vtableCallerLine("wrapper", "??_7Widget@@6B@")
                               : importCallerLine("wrapper", "__imp_RuntimeB")),
        "context"));
    Database.apply(Image, {0x1000});
    ASSERT_EQ(Database.matches().size(), 1u);
    EXPECT_TRUE(Database.matches().front().Confirmed);
    EXPECT_TRUE(Database.buildNameMap().empty());
    if (Type)
      Image.Segments.back().Data[0x140] = 0;
    else
      Image.Imports.clear();
    Database.apply(Image, {0x1000});
    EXPECT_TRUE(Database.matches().empty());
  }
}

TEST(SignatureDBContext, RequiredCandidatesDoNotBootstrapCycles) {
  auto Image = makeNestedCallingImage();
  Image.Imports.clear();
  neverd::writeLE<uint32_t>(Image.Segments.front().Data.data() + 0x102,
                            uint32_t(0x1000 - 0x1106));
  const std::string Outer =
      requireReferences(callerLine("outer", " ^0005 inner"));
  const std::string Inner =
      "53E8........5BC3 00 0000 0008 !refs :0000 inner ^0002 outer\n";
  for (bool Reverse : {false, true}) {
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(
        Reverse ? Inner + Outer : Outer + Inner, "context"));
    Database.apply(Image, {0x1000, 0x1100});
    EXPECT_TRUE(Database.matches().empty());
    // A raw hit at a target outside the entry set cannot bypass !refs either.
    Database.apply(Image, {0x1000});
    EXPECT_TRUE(Database.matches().empty());
  }
  SignatureDB Recursive;
  ASSERT_FALSE(Recursive.loadPatternText(
      requireReferences(callerLine("self", " ^0005 self")), "context"));
  Recursive.apply(makeCallingImage(0x1000), {0x1000});
  EXPECT_TRUE(Recursive.matches().empty());
}

TEST(SignatureDBContext, AnchoredChainsSettleAndWithdrawInEitherOrder) {
  const std::string Outer =
      requireReferences(callerLine("outer", " ^0005 inner"));
  const std::string Inner =
      "53E8........5BC3 00 0000 0008 !refs :0000 inner ^0002 import_a\n";
  for (bool Reverse : {false, true}) {
    auto Image = makeNestedCallingImage();
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(
        Reverse ? Inner + Outer : Outer + Inner, "context"));
    Database.apply(Image, {0x1000, 0x1100});
    const auto Names = Database.buildCandidateNameMap();
    ASSERT_EQ(Names.size(), 2u);
    EXPECT_EQ(Names.at(0x1000), "outer");
    EXPECT_EQ(Names.at(0x1100), "inner");
    EXPECT_TRUE(Database.buildNameMap().empty());
    Image.Imports.front().Name = "import_b";
    Database.apply(Image, {0x1000, 0x1100});
    EXPECT_TRUE(Database.matches().empty());
  }
}

TEST(SignatureDBContext, EveryRequiredReferenceMustBeConfirmed) {
  auto Image = makeIndirectImportImage(neverd::Arch::X64);
  auto &Code = Image.Segments.front().Data;
  Code[16] = 0xff;
  Code[17] = 0x15;
  neverd::writeLE<uint32_t>(Code.data() + 18, 0x2008 - 0x1016);
  Image.Segments.back().Data.resize(16, 0);
  Image.Segments.back().Size = Image.Segments.back().FileSz = 16;
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "90909090FF15........909090909090FF15........90909090909090909090"
      " 00 0000 0020 !refs :0000 wrapper ^0006 __imp_RuntimeB ^0012 "
      "__imp_Other\n",
      "context"));
  Database.apply(Image, {0x1000});
  EXPECT_TRUE(Database.matches().empty());
  auto Imp = Image.Imports.front();
  Imp.Name = "Other";
  Imp.IATAddr = 0x2008;
  Image.Imports.push_back(Imp);
  Database.apply(Image, {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_TRUE(Database.matches().front().Confirmed);
}

TEST(SignatureDBContext, FunctionAddressReferencesRequireAnExecutableTarget) {
  auto Image = makeVTableImage();
  auto &Code = Image.Segments.front();
  Code.Size = Code.FileSz = 0x200;
  Code.Data.resize(0x200, 0xcc);
  const uint8_t Callee[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
  std::copy(std::begin(Callee), std::end(Callee), Code.Data.begin() + 0x100);
  neverd::writeLE<uint32_t>(Code.Data.data() + 7, 0x1100 - 0x100b);
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      requireReferences(vtableCallerLine("getter", "callee")) + CalleeLine,
      "addresses"));
  Database.apply(Image, {0x1000, 0x1100});
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "getter");
  EXPECT_TRUE(Database.buildNameMap().empty());
  neverd::writeLE<uint32_t>(Code.Data.data() + 7, 0x2000 - 0x100b);
  std::copy(std::begin(Callee), std::end(Callee),
            Image.Segments.back().Data.begin());
  Database.apply(Image, {0x1000, 0x1100});
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1000), 0u);
}

TEST_F(SignatureDirectoryTest,
       NativePatternsShareLibrarySelectionAndExplicitLoading) {
  write("vs2022.pat", CalleeLine);
  write("vs2022.part3.npat",
        requireReferences(importCallerLine("wrapper", "__imp_RuntimeB")));
  write("ignored.txt", "invalid record");
  auto Files = SignatureDB::listDirectory(Directory);
  ASSERT_TRUE(bool(Files)) << llvm::toString(Files.takeError());
  ASSERT_EQ(Files->size(), 2u);
  EXPECT_EQ(SignatureDB::libraryName(Directory / "vs2022.part3.npat"),
            "vs2022");
  SignatureDB Database;
  ASSERT_FALSE(Database.loadFile(Directory / "vs2022.part3.npat"));
  Database.apply(makeIndirectImportImage(neverd::Arch::X64), {0x1000});
  ASSERT_EQ(Database.matches().size(), 1u);
  EXPECT_TRUE(Database.matches().front().Confirmed);
  EXPECT_EQ(Database.matches().front().LibraryName, "vs2022");
  ASSERT_FALSE(Database.loadDirectory(Directory));
  EXPECT_EQ(Database.fileCount(), 2u);
}

namespace {
std::string incomingCallerLine(llvm::StringRef Name,
                               llvm::StringRef References) {
  return ("554889E5E8........909090909090909090909090909090905DC3 00 0000 001B "
          ":0000 " +
          Name + References + "\n")
      .str();
}
neverd::BinaryImage incomingCallerImage() {
  auto Image = makeCallingImage(0x1100);
  auto &Code = Image.Segments.front().Data;
  std::fill(Code.begin() + 9, Code.begin() + 25, 0x90);
  Code[25] = 0x5d;
  Code[26] = 0xc3;
  return Image;
}
constexpr const char *IncomingLeaf =
    "AABBCCDDC3 00 0000 0005 !caller :0000 callee\n";

struct IncomingChain {
  neverd::BinaryImage Image = incomingCallerImage();
  std::vector<uint64_t> Entries;
  std::string Patterns;

  explicit IncomingChain(size_t MiddleCount) {
    auto &Segment = Image.Segments.front();
    const std::vector<uint8_t> Body(Segment.Data.begin(),
                                    Segment.Data.begin() + 27);
    Segment.Size = Segment.FileSz = (MiddleCount + 2) * 0x100;
    Segment.Data.assign(Segment.Size, 0xcc);
    for (size_t I = 0; I <= MiddleCount; ++I) {
      Entries.push_back(0x1000 + I * 0x100);
      std::copy(Body.begin(), Body.end(), Segment.Data.begin() + I * 0x100);
      const std::string Name = I == 0 ? "root" : "chain" + std::to_string(I);
      const std::string Target =
          I == MiddleCount ? "callee" : "chain" + std::to_string(I + 1);
      auto Line = incomingCallerLine(Name, " ^0005 " + Target);
      if (I != 0) {
        Segment.Data[I * 0x100] = 0x53;
        Line.replace(0, 2, "53");
        Line.insert(Line.find(":0000"), "!caller ");
      }
      Patterns += Line;
    }
    Entries.push_back(0x1000 + (MiddleCount + 1) * 0x100);
    const uint8_t Leaf[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
    std::copy(std::begin(Leaf), std::end(Leaf),
              Segment.Data.begin() + (MiddleCount + 1) * 0x100);
    Patterns += IncomingLeaf;
  }
};
} // namespace

TEST(SignatureDBCallerContext, ConditionalTailReferencesPreserveEntryScope) {
  for (auto Arch : {neverd::Arch::X86, neverd::Arch::X64})
    for (uint8_t Condition = 0x80; Condition <= 0x8f; ++Condition)
      for (bool Backward : {false, true}) {
        auto Image = incomingCallerImage();
        Image.Format = neverd::BinaryFormat::COFF;
        Image.Arch = Arch;
        Image.Bits = Arch == neverd::Arch::X64 ? neverd::Bitness::Bits64
                                               : neverd::Bitness::Bits32;
        auto &Code = Image.Segments.front().Data;
        std::vector<uint8_t> Parent(Code.begin(), Code.begin() + 27);
        Parent.insert(Parent.begin() + 4, 0x0f);
        Parent[5] = Condition;
        const uint64_t Source = Backward ? 0x1100 : 0x1000;
        const uint64_t Target = Backward ? 0x1000 : 0x1100;
        neverd::writeLE<int32_t>(Parent.data() + 6,
                                 static_cast<int32_t>(Target - (Source + 10)));
        std::fill(Code.begin(), Code.end(), 0xcc);
        std::copy(Parent.begin(), Parent.end(), Code.begin() + Source - 0x1000);
        const uint8_t Leaf[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
        std::copy(std::begin(Leaf), std::end(Leaf),
                  Code.begin() + Target - 0x1000);
        std::string Prefix = "554889E50F";
        constexpr char Hex[] = "0123456789ABCDEF";
        Prefix += Hex[Condition >> 4];
        Prefix += Hex[Condition & 15];
        const std::string Pattern =
            Prefix +
            "........909090909090909090909090909090905DC3 00 0000 001C "
            ":0000 parent ^0006 callee\n";
        SignatureDB DB;
        ASSERT_FALSE(DB.loadPatternText(Pattern + IncomingLeaf, "conditional"));
        DB.apply(Image, {Source, Target});
        auto Names = DB.buildCandidateNameMap();
        ASSERT_EQ(Names.size(), 2u);
        EXPECT_EQ(Names.at(Target), "callee");
        EXPECT_TRUE(DB.buildNameMap().empty());
        DB.apply(Image, {Source});
        EXPECT_EQ(DB.buildCandidateNameMap().count(Target), 0u);
        // Without a known boundary, a prefix-looking preceding byte must not
        // silently turn a different instruction form into a rel32 reference.
        Code[Source - 0x1000 + 3] = 0x66;
        std::string Prefixed = Pattern;
        Prefixed.replace(6, 2, "66");
        SignatureDB Other;
        ASSERT_FALSE(Other.loadPatternText(Prefixed + IncomingLeaf, "prefix"));
        Other.apply(Image, {Source, Target});
        EXPECT_EQ(Other.buildCandidateNameMap().count(Target), 0u);
      }
}

TEST(SignatureDBCallerContext, StrongIncomingChainsKeepTheirOriginalRoot) {
  for (bool Reverse : {false, true}) {
    IncomingChain Chain(3);
    if (Reverse) {
      std::reverse(Chain.Entries.begin(), Chain.Entries.end());
      llvm::SmallVector<llvm::StringRef, 8> Lines;
      llvm::StringRef(Chain.Patterns).split(Lines, '\n', -1, false);
      std::string Reversed;
      for (llvm::StringRef Line : llvm::reverse(Lines))
        Reversed += Line.str() + "\n";
      Chain.Patterns = std::move(Reversed);
    }
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(Chain.Patterns, "chain"));
    Database.apply(Chain.Image, Chain.Entries);
    const auto Names = Database.buildCandidateNameMap();
    ASSERT_EQ(Names.size(), 5u);
    EXPECT_EQ(Names.at(0x1400), "callee");
    EXPECT_TRUE(Database.buildNameMap().empty());
    for (const auto &Match : Database.matches())
      EXPECT_FALSE(Match.Confirmed); // Descendants never certify ancestors.
  }
}

TEST(SignatureDBCallerContext, UnrootedOrContradictedChainsCannotExpand) {
  IncomingChain Chain(3);
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      llvm::StringRef(Chain.Patterns).split('\n').second, "unrooted"));
  // A cycle made only of context records has no initial evidence.
  neverd::writeLE<uint32_t>(Chain.Image.Segments.front().Data.data() + 0x305,
                            uint32_t(0x1100 - 0x1309));
  Database.apply(Chain.Image, Chain.Entries);
  EXPECT_TRUE(Database.matches().empty());

  IncomingChain Contradicted(1);
  SignatureDB Other;
  ASSERT_FALSE(Other.loadPatternText(
      Contradicted.Patterns + "AABBCCDDC3 00 0000 0005 :0000 different\n",
      "contradicted"));
  Other.apply(Contradicted.Image, Contradicted.Entries);
  const auto Names = Other.buildCandidateNameMap();
  EXPECT_EQ(Names.at(0x1000), "root");
  EXPECT_EQ(Names.at(0x1200), "different");
  EXPECT_EQ(Names.count(0x1100), 0u);
}

TEST(SignatureDBCallerContext, WeakIncomingBodiesCannotExtendTheFrontier) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      incomingCallerLine("root", " ^0005 middle") +
          "53E8........5BC3 00 0000 0008 !caller :0000 middle ^0002 callee\n" +
          IncomingLeaf,
      "weak"));
  auto Image = incomingCallerImage();
  auto &Code = Image.Segments.front().Data;
  const uint8_t Middle[] = {0x53, 0xe8, 0xfa, 0, 0, 0, 0x5b, 0xc3};
  std::copy(std::begin(Middle), std::end(Middle), Code.begin() + 0x100);
  const uint8_t Leaf[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
  std::copy(std::begin(Leaf), std::end(Leaf), Code.begin() + 0x200);
  Database.apply(Image, {0x1000, 0x1100, 0x1200});
  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.at(0x1100), "middle");
  EXPECT_EQ(Names.count(0x1200), 0u);
}

TEST(SignatureDBCallerContext,
     IncomingDepthBudgetRetainsOnlySupportedPrefixes) {
#define NEVERD_SIGS_REFERENCE_LIMIT(Name, Value)                               \
  [[maybe_unused]] constexpr size_t Name = Value;
#include "../../lib/sigs/ReferenceCheckLimits.def"
  IncomingChain Chain(MaxIncomingRounds + 1);
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(Chain.Patterns, "bounded"));
  Database.apply(Chain.Image, Chain.Entries);
  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.size(), MaxIncomingRounds + 1);
  EXPECT_EQ(Names.at(0x1000 + MaxIncomingRounds * 0x100),
            "chain" + std::to_string(MaxIncomingRounds));
  EXPECT_EQ(Names.count(0x1000 + (MaxIncomingRounds + 1) * 0x100), 0u);
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBCallerContext, OnlyAnEstablishedCallerCanRequestTheLeaf) {
  for (bool Reverse : {false, true}) {
    SignatureDB Database;
    const std::string Parent = incomingCallerLine("parent", " ^0005 callee");
    ASSERT_FALSE(Database.loadPatternText(
        Reverse ? IncomingLeaf + Parent : Parent + IncomingLeaf, "incoming"));
    auto Image = incomingCallerImage();
    Database.apply(Image, {0x1100, 0x1000});
    ASSERT_EQ(Database.buildCandidateNameMap().size(), 2u);
    EXPECT_EQ(Database.buildCandidateNameMap().at(0x1100), "callee");
    EXPECT_TRUE(Database.buildNameMap().empty());
    for (const SigMatch &Match : Database.matches())
      EXPECT_FALSE(Match.Confirmed); // No circular outgoing confirmation.
    Database.apply(Image, {0x1000});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
    Database.apply(Image, {0x1100});
    EXPECT_TRUE(Database.matches().empty());
    Image.Segments.front().Data[0x104] ^= 1;
    Database.apply(Image, {0x1000, 0x1100});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
  }
}

TEST(SignatureDBCallerContext,
     WeakPartialRequiredAndAmbiguousParentsCannotAnchor) {
  const std::string Strong = incomingCallerLine("parent", " ^0005 callee");
  std::string Partial = Strong;
  Partial.replace(Partial.find("001B"), 4, "001C");
  for (const auto &Parent :
       {callerLine("parent", " ^0005 callee"), Partial,
        requireReferences(Strong),
        Strong + incomingCallerLine("rival", " ^0005 callee")}) {
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(Parent + IncomingLeaf, "incoming"));
    auto Image = Parent == callerLine("parent", " ^0005 callee")
                     ? makeCallingImage(0x1100)
                     : incomingCallerImage();
    Database.apply(Image, {0x1000, 0x1100});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
  }
}

TEST(SignatureDBCallerContext, EveryParentVariantMustNameTheSameTarget) {
  for (const std::string &Parent :
       {incomingCallerLine("parent", " ^0005 callee ^0005 rival"),
        incomingCallerLine("parent", " ^0005 callee") +
            incomingCallerLine("parent", " ^0005 rival"),
        incomingCallerLine("parent", " ^0005 callee") +
            incomingCallerLine("parent", "")}) {
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(Parent + IncomingLeaf, "incoming"));
    Database.apply(incomingCallerImage(), {0x1000, 0x1100});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
  }
}

TEST(SignatureDBCallerContext,
     AlternativesRequireCompleteAndUniqueBodyEvidence) {
  for (const std::string &Fallback : {"", "AABBCCDDC3", "11223344C3"}) {
    SignatureDB Database;
    ASSERT_FALSE(Database.loadPatternText(
        incomingCallerLine("parent", " ^0005 callee ^0005 fallback") +
            IncomingLeaf +
            (Fallback.empty()
                 ? ""
                 : Fallback + " 00 0000 0005 !caller :0000 fallback\n"),
        "alternatives"));
    Database.apply(incomingCallerImage(), {0x1000, 0x1100});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100),
              Fallback == "11223344C3" ? 1u : 0u);
    if (Fallback == "11223344C3")
      EXPECT_EQ(Database.buildCandidateNameMap().at(0x1100), "callee");
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}

TEST(SignatureDBCallerContext, AlternativesUseTheSameFrozenReferenceChecks) {
  for (bool KnownSupport : {false, true}) {
    SignatureDB Database;
    const std::string Body = "53E8........5BC3 00 0000 0008 !caller :0000 ";
    ASSERT_FALSE(Database.loadPatternText(
        incomingCallerLine("parent", " ^0005 callee ^0005 fallback") + Body +
            "callee ^0002 support\n" + Body + "fallback ^0002 wrong\n" +
            (KnownSupport ? "AABBCCDDC3 00 0000 0005 :0000 support\n" : ""),
        "alternatives"));
    auto Image = incomingCallerImage();
    auto &Code = Image.Segments.front().Data;
    const uint8_t Child[] = {0x53, 0xe8, 0, 0, 0, 0, 0x5b, 0xc3};
    std::copy(std::begin(Child), std::end(Child), Code.begin() + 0x100);
    neverd::writeLE<uint32_t>(Code.data() + 0x102, 0x1200 - 0x1106);
    const uint8_t Support[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
    std::copy(std::begin(Support), std::end(Support), Code.begin() + 0x200);
    Database.apply(Image, {0x1000, 0x1100, 0x1200});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100),
              KnownSupport ? 1u : 0u);
    if (KnownSupport)
      EXPECT_EQ(Database.buildCandidateNameMap().at(0x1100), "callee");
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}

TEST(SignatureDBCallerContext, SourceMetadataRefinesOnlyACompleteParent) {
  for (bool Partial : {false, true}) {
    SignatureDB Database;
    std::string Old = incomingCallerLine("parent", "");
    if (Partial)
      Old.replace(Old.find("001B"), 4, "001C");
    std::string Source = incomingCallerLine("parent", " ^0005 callee");
    Source.insert(Source.find(":0000"), "!caller ");
    ASSERT_FALSE(
        Database.loadPatternText(Old + Source + IncomingLeaf, "metadata"));
    Database.apply(incomingCallerImage(), {0x1000, 0x1100});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100),
              Partial ? 0u : 1u);
    EXPECT_EQ(Database.matches().size(), Partial ? 1u : 2u);
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}

TEST(SignatureDBCallerContext, SourceMetadataCannotNarrowOldAlternatives) {
  SignatureDB Database;
  std::string Source = incomingCallerLine("parent", " ^0005 callee");
  Source.insert(Source.find(":0000"), "!caller ");
  ASSERT_FALSE(Database.loadPatternText(
      incomingCallerLine("parent", " ^0005 callee ^0005 rival") + Source +
          IncomingLeaf,
      "metadata"));
  Database.apply(incomingCallerImage(), {0x1000, 0x1100});
  EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
  EXPECT_EQ(Database.matches().size(), 1u);
}

TEST(SignatureDBCallerContext,
     ContradictedSourceMetadataCannotSupportChildren) {
  for (const std::string &Type : {"Widget", "Other"}) {
    auto Image = makeVTableImage();
    auto &Segment = Image.Segments.front();
    Segment.Size = Segment.FileSz = 0x200;
    Segment.Data.resize(0x200, 0xcc);
    Segment.Data[11] = 0xe8;
    neverd::writeLE<uint32_t>(Segment.Data.data() + 12, 0x1100 - 0x1010);
    const uint8_t Leaf[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
    std::copy(std::begin(Leaf), std::end(Leaf), Segment.Data.begin() + 0x100);
    std::string Source = vtableCallerLine("parent", "??_7" + Type + "@@6B@");
    Source.replace(22, 10, "E8........");
    const std::string Old = Source.substr(0, Source.find(" ^")) + "\n";
    Source.insert(Source.find(":0000"), "!caller ");
    Source.insert(Source.size() - 1, " ^000C callee");
    SignatureDB Database;
    ASSERT_FALSE(
        Database.loadPatternText(Old + Source + IncomingLeaf, "metadata"));
    Database.apply(Image, {0x1000, 0x1100});
    EXPECT_EQ(Database.buildCandidateNameMap().at(0x1000), "parent");
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100),
              Type == "Widget" ? 1u : 0u);
    EXPECT_EQ(Database.matches().size(), Type == "Widget" ? 2u : 1u);

    // Another caller independently supports a different alias of the same
    // body. Withdrawing the refinement must withdraw only its own name.
    Segment.Size = Segment.FileSz = 0x400;
    Segment.Data.resize(0x400, 0xcc);
    auto Other = incomingCallerImage();
    std::copy_n(Other.Segments.front().Data.begin(), 27,
                Segment.Data.begin() + 0x300);
    neverd::writeLE<uint32_t>(Segment.Data.data() + 0x305,
                              uint32_t(0x1100 - 0x1309));
    ASSERT_FALSE(Database.loadPatternText(
        Old + Source + incomingCallerLine("second", " ^0005 alternate") +
            "AABBCCDDC3 00 0000 0005 !caller :0000 callee :0000 alternate\n",
        "metadata"));
    Database.apply(Image, {0x1000, 0x1100, 0x1300});
    EXPECT_EQ(Database.buildCandidateNameMap().at(0x1100),
              Type == "Widget" ? "callee" : "alternate");
    if (Type == "Other")
      for (const SigMatch &Match : Database.matches())
        if (Match.Address == 0x1100)
          EXPECT_TRUE(Match.Aliases.empty());
  }
}

TEST(SignatureDBCallerContext, AContradictedParentCannotNameItsTarget) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      incomingCallerLine("parent", " ^0005 callee") + IncomingLeaf +
          "AABBCCDDC3 00 0000 0005 :0000 other\n",
      "incoming"));
  Database.apply(incomingCallerImage(), {0x1000, 0x1100});
  const auto Names = Database.buildCandidateNameMap();
  EXPECT_EQ(Names.count(0x1000), 0u);
  EXPECT_EQ(Names.at(0x1100), "other");
}

TEST(SignatureDBCallerContext, OnlyExplicitContextBodiesCanBeReused) {
  for (bool Required : {false, true}) {
    SignatureDB Database;
    const std::string Body = "53E8........5BC3 00 0000 0008 ";
    const std::string Callee = Required
                                   ? Body + "!refs :0000 callee ^0002 unknown\n"
                                   : Body + ":0000 callee ^0002 unknown\n" +
                                         Body + ":0000 rival ^0002 unknown\n";
    ASSERT_FALSE(Database.loadPatternText(
        incomingCallerLine("parent", " ^0005 callee") + Callee, "existing"));
    auto Image = incomingCallerImage();
    auto &Code = Image.Segments.front().Data;
    const uint8_t Child[] = {0x53, 0xe8, 0, 0, 0, 0, 0x5b, 0xc3};
    std::copy(std::begin(Child), std::end(Child), Code.begin() + 0x100);
    neverd::writeLE<uint32_t>(Code.data() + 0x102, 0x1200 - 0x1106);
    Database.apply(Image, {0x1000, 0x1100, 0x1200});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100), 0u);
    // Native incoming context can reuse !refs bodies. It cannot turn an
    // ordinary nominal byte claim into a new incoming-context claim.
    ASSERT_FALSE(Database.loadPatternText(
        "33C0C3 00 0000 0003 !caller :0000 unrelated\n", "native"));
    Database.apply(Image, {0x1000, 0x1100, 0x1200});
    EXPECT_EQ(Database.buildCandidateNameMap().count(0x1100),
              Required ? 1u : 0u);
    if (Required)
      EXPECT_EQ(Database.buildCandidateNameMap().at(0x1100), "callee");
    EXPECT_EQ(llvm::count_if(Database.matches(),
                             [](const SigMatch &Match) {
                               return Match.Address == 0x1100 &&
                                      Match.CallerConfirmed;
                             }),
              Required ? 1 : 0);
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}

TEST(SignatureDBCallerContext, OutgoingConstraintsCannotChangeTheirAnchor) {
  for (const std::string &Known : {"support", "different", ""}) {
    SignatureDB Database;
    const std::string Callee =
        "53E8........5BC3 00 0000 0008 !caller :0000 callee ^0002 support\n";
    const std::string Support =
        Known.empty() ? "" : "AABBCCDDC3 00 0000 0005 :0000 " + Known + "\n";
    ASSERT_FALSE(Database.loadPatternText(
        incomingCallerLine("parent", " ^0005 callee") + Callee + Support,
        "incoming"));
    auto Image = incomingCallerImage();
    auto &Code = Image.Segments.front().Data;
    const uint8_t Child[] = {0x53, 0xe8, 0, 0, 0, 0, 0x5b, 0xc3};
    std::copy(std::begin(Child), std::end(Child), Code.begin() + 0x100);
    neverd::writeLE<uint32_t>(Code.data() + 0x102, 0x1200 - 0x1106);
    const uint8_t Leaf[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
    std::copy(std::begin(Leaf), std::end(Leaf), Code.begin() + 0x200);
    Database.apply(Image, {0x1000, 0x1100, 0x1200});
    const auto Names = Database.buildCandidateNameMap();
    EXPECT_EQ(Names.at(0x1000), "parent");
    EXPECT_EQ(Names.count(0x1100), Known == "different" ? 0u : 1u);
    for (const SigMatch &Match : Database.matches()) {
      if (Match.Address == 0x1000)
        EXPECT_FALSE(Match.Confirmed);
      if (Match.Address == 0x1100)
        EXPECT_EQ(Match.Confirmed, Known == "support");
    }
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}

TEST(SignatureDBCallerContext, IncomingCandidatesCannotConfirmEachOther) {
  SignatureDB Database;
  std::string Second = incomingCallerLine("second_parent", " ^0005 support");
  Second.replace(0, 2, "53");
  ASSERT_FALSE(Database.loadPatternText(
      incomingCallerLine("parent", " ^0005 callee") + Second +
          "53E8........5BC3 00 0000 0008 !caller :0000 callee ^0002 support\n"
          "AABBCCDDC3 00 0000 0005 !caller :0000 support\n",
      "incoming"));
  auto Image = incomingCallerImage();
  auto &Segment = Image.Segments.front();
  Segment.Data.resize(0x400, 0xcc);
  Segment.Size = Segment.FileSz = 0x400;
  auto &Code = Segment.Data;
  std::copy_n(Code.begin(), 27, Code.begin() + 0x300);
  Code[0x300] = 0x53;
  neverd::writeLE<uint32_t>(Code.data() + 0x305, uint32_t(0x1200 - 0x1309));
  const uint8_t Child[] = {0x53, 0xe8, 0, 0, 0, 0, 0x5b, 0xc3};
  std::copy(std::begin(Child), std::end(Child), Code.begin() + 0x100);
  neverd::writeLE<uint32_t>(Code.data() + 0x102, 0x1200 - 0x1106);
  const uint8_t Leaf[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xc3};
  std::copy(std::begin(Leaf), std::end(Leaf), Code.begin() + 0x200);
  Database.apply(Image, {0x1000, 0x1100, 0x1200, 0x1300});
  EXPECT_EQ(Database.buildCandidateNameMap().size(), 4u);
  for (const SigMatch &Match : Database.matches())
    EXPECT_FALSE(Match.Confirmed);
}

TEST(SignatureDBCallerContext,
     IncomingEvidenceResolvesUnsettledByteAlternatives) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      incomingCallerLine("parent", " ^0005 callee") + IncomingLeaf +
          "AABBCCDDC3 00 0000 0005 :0000 rival_a\n"
          "AABBCCDDC3 00 0000 0005 :0000 rival_b\n",
      "incoming"));
  Database.apply(incomingCallerImage(), {0x1000, 0x1100});
  EXPECT_EQ(Database.buildCandidateNameMap().at(0x1100), "callee");
  EXPECT_TRUE(Database.buildNameMap().empty());
  size_t Supported = 0;
  for (const SigMatch &Match : Database.matches())
    if (Match.CallerConfirmed) {
      ++Supported;
      EXPECT_EQ(Match.Name, "callee");
      EXPECT_FALSE(Match.Confirmed);
    }
  EXPECT_EQ(Supported, 1u);
}

TEST(SignatureDBCallerContext,
     OnlyRequestedIncomingNamesBecomeCandidateAliases) {
  for (bool SameModule : {false, true}) {
    SignatureDB Database;
    std::string Second =
        incomingCallerLine("second_parent", " ^0005 alternate");
    Second.replace(0, 2, "53");
    const std::string Leaves =
        SameModule
            ? "AABBCCDDC3 00 0000 0005 !caller :0000 aa_unused :0000 callee "
              ":0000 alternate\n"
            : "AABBCCDDC3 00 0000 0005 !caller :0000 aa_unused :0000 callee\n"
              "AABBCCDDC3 00 0000 0005 !caller :0000 alternate\n";
    ASSERT_FALSE(Database.loadPatternText(
        incomingCallerLine("parent", " ^0005 callee") + Second + Leaves,
        "incoming"));
    auto Image = incomingCallerImage();
    auto &Segment = Image.Segments.front();
    Segment.Data.resize(0x400, 0xcc);
    Segment.Size = Segment.FileSz = 0x400;
    auto &Code = Segment.Data;
    std::copy_n(Code.begin(), 27, Code.begin() + 0x300);
    Code[0x300] = 0x53;
    neverd::writeLE<uint32_t>(Code.data() + 0x305, uint32_t(0x1100 - 0x1309));
    Database.apply(Image, {0x1000, 0x1100, 0x1300});
    EXPECT_EQ(Database.buildCandidateNameMap().at(0x1100), "callee");
    bool CalledAlias = false;
    for (const SigMatch &Match : Database.matches())
      if (Match.Address == 0x1100) {
        EXPECT_TRUE(Match.CallerConfirmed);
        EXPECT_NE(Match.Name, "aa_unused");
        EXPECT_FALSE(llvm::is_contained(Match.Aliases, "aa_unused"));
        CalledAlias |= Match.Name == "alternate" ||
                       llvm::is_contained(Match.Aliases, "alternate");
      }
    EXPECT_TRUE(CalledAlias);
    EXPECT_TRUE(Database.buildNameMap().empty());
  }
}
