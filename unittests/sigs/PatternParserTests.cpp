//===- PatternParserTests.cpp - Pattern text parser tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternParser.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace neverd::sigs;

TEST(PatternParserStrictness, RejectsUnsupportedReferenceConstraints) {
  auto ModuleOrErr = PatternParser::parseLine(
      "AABB 00 0000 0002 :0000 public_name ^0001 referenced_name");

  ASSERT_FALSE(ModuleOrErr) << "an unmodeled constraint must not be ignored";
  EXPECT_NE(llvm::toString(ModuleOrErr.takeError())
                .find("reference constraint is not supported"),
            std::string::npos);
}

TEST(PatternParserStrictness, RejectsTheWholeFileWhenOneRecordIsMalformed) {
  int FileDescriptor = -1;
  llvm::SmallString<128> Path;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-pattern-parser",
                                                  "pat", FileDescriptor, Path));
  llvm::FileRemover RemoveFile(Path);
  {
    llvm::raw_fd_ostream Output(FileDescriptor, /*shouldClose=*/true);
    Output << "AABB 00 0000 0002 :0000 valid_name\n"
              "this record is malformed\n";
  }

  auto ModulesOrErr = PatternParser::parseFile(Path.str().str());

  ASSERT_FALSE(ModulesOrErr) << "a file must not return a valid prefix";
  EXPECT_NE(llvm::toString(ModulesOrErr.takeError()).find("pattern line 2"),
            std::string::npos);
}

TEST(PatternParserStrictness, RejectsEveryMalformedTrailingField) {
  const char *const MalformedLines[] = {
      "AABB 00 0000 0002 :0000 good_name :ZZZZ bad_name",
      "AABB 00 0000 0002 :0000 good_name :0001",
      "AABB 00 0000 0002 :0000 good_name ABC",
      "AABB 00 0000 0002 :0000 good_name unknown-token",
      "AABB 00 0000 0002 :0000 good_name AABB CCDD",
  };

  for (const char *Line : MalformedLines) {
    SCOPED_TRACE(Line);
    auto ModuleOrErr = PatternParser::parseLine(Line);
    if (ModuleOrErr) {
      ADD_FAILURE() << "malformed trailing input was ignored";
      continue;
    }
    llvm::consumeError(ModuleOrErr.takeError());
  }
}

TEST(PatternParserWhitespace, AcceptsAllStandardWhitespaceSeparators) {
  auto ModuleOrErr =
      PatternParser::parseLine("AABB\t00\v0000\f0002\r:0000\tpublic_name");

  ASSERT_TRUE(static_cast<bool>(ModuleOrErr))
      << llvm::toString(ModuleOrErr.takeError());
  ASSERT_EQ(ModuleOrErr->PublicNames.size(), 1u);
  EXPECT_EQ(ModuleOrErr->PublicNames.front().Name, "public_name");
}

TEST(PatternParserStrictness, RejectsPublicNamesOutsideTheDeclaredModule) {
  for (const char *Line : {
           "AABB 00 0000 0002 :0002 at_end",
           "AABB 00 0000 0002 :0003 past_end",
       }) {
    SCOPED_TRACE(Line);
    auto ModuleOrErr = PatternParser::parseLine(Line);
    ASSERT_FALSE(ModuleOrErr);
    EXPECT_NE(llvm::toString(ModuleOrErr.takeError())
                  .find("public name offset is outside total length"),
              std::string::npos);
  }
}

TEST(PatternParserStrictness, RejectsImpossibleDeclaredRanges) {
  struct InvalidCase {
    const char *Line;
    const char *Error;
  };
  for (const InvalidCase &Case : {
           InvalidCase{"AA 00 0000 0000 :0000 zero_length",
                       "total length must be non-zero"},
           InvalidCase{"AA 02 0000 0002 :0000 crc_crosses_end",
                       "CRC range is outside total length"},
       }) {
    SCOPED_TRACE(Case.Line);
    auto ModuleOrErr = PatternParser::parseLine(Case.Line);
    ASSERT_FALSE(ModuleOrErr);
    EXPECT_NE(llvm::toString(ModuleOrErr.takeError()).find(Case.Error),
              std::string::npos);
  }
}
