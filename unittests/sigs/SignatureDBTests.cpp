//===- SignatureDBTests.cpp - Signature database tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

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
