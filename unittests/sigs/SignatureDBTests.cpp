//===- SignatureDBTests.cpp - Signature database tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/SmallString.h"
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
  EXPECT_TRUE(Database.buildNameMap().empty());
}

TEST(SignatureDBAddresses, OmitsDisputedNamesFromTheApplicationMap) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText("AABB 00 0000 0002 :0000 first_name\n"
                                        "AABB 00 0000 0002 :0000 second_name\n",
                                        "ambiguous"));
  const neverd::BinaryImage Image = makeMatchingImage();

  Database.apply(Image, {0x1000});

  ASSERT_EQ(Database.matches().size(), 2u);
  EXPECT_EQ(Database.buildNameMap().count(0x1000), 0u);
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

  const auto Names = Database.buildNameMap();
  EXPECT_EQ(Names.count(0x1000), 0u);
  EXPECT_EQ(Names.at(0x1100), "callee");
}

TEST(SignatureDBReferences, ACallNothingNamesKeepsTheMatch) {
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      callerLine("caller", " ^0005 somebody_else"), "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000});

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, AConfirmedCallSettlesIdenticalBytes) {
  // Two routines with the same bytes: only what one of them calls is there.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("caller_of_callee", " ^0005 callee") +
          callerLine("unreferenced_twin", ""),
      "refs"));
  Database.apply(makeCallingImage(0x1100), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller_of_callee");
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

  const auto Names = Database.buildNameMap();
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

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
}

TEST(SignatureDBReferences, ARoutineThatOnlyJumpsOnContradictsNothing) {
  // The same `jmp 0x1100` is also what `free` compiles to when all it does
  // is tail-call `_free_base`: a call to it is a call to `free`, which the
  // routine it reaches, named otherwise, does not contradict.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      CalleeLine + callerLine("caller", " ^0005 free"), "refs"));
  Database.apply(makeCallingImage(0x1200), {0x1000, 0x1100});

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
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
  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
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
  EXPECT_EQ(Database.buildNameMap().at(0x1000), "do_tolower");
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
  EXPECT_EQ(Database.buildNameMap().count(0x1000), 0u);
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

  const auto Names = Database.buildNameMap();
  EXPECT_EQ(Names.at(0x1100), "inner_a");
  EXPECT_EQ(Names.at(0x1000), "outer_a");
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

  const auto Names = Database.buildNameMap();
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

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller")
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

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
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

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
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

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
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

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller");
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

    EXPECT_EQ(Database.buildNameMap().at(0x1000), "caller")
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
  EXPECT_EQ(Database.buildNameMap().at(0x1000), "puts");
}

TEST(SignatureDBAliases, MatchesThatShareANameAgreeOnIt) {
  // Two builds of the library define different aliases for the routine.
  SignatureDB Database;
  ASSERT_FALSE(Database.loadPatternText(
      "AABB 00 0000 0002 :0000 malloc :0000 __libc_malloc\n"
      "AABB 00 0000 0002 :0000 __libc_malloc :0000 __malloc\n",
      "libc"));
  Database.apply(makeMatchingImage(), {0x1000});

  EXPECT_EQ(Database.buildNameMap().at(0x1000), "__libc_malloc");
}

TEST(SignatureDBAliases, AliasSetsWithNothingInCommonAreDisputed) {
  SignatureDB Database;
  ASSERT_FALSE(
      Database.loadPatternText("AABB 00 0000 0002 :0000 puts :0000 _IO_puts\n"
                               "AABB 00 0000 0002 :0000 fputs\n",
                               "libc"));
  Database.apply(makeMatchingImage(), {0x1000});

  EXPECT_EQ(Database.buildNameMap().count(0x1000), 0u);
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

  const auto Names = Database.buildNameMap();
  EXPECT_EQ(Names.at(0x1100), "callee");
  EXPECT_EQ(Names.at(0x1000), "caller");
}
