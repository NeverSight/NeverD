//===- PatternParserTests.cpp - Pattern text parser tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternParser.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::sigs;

TEST(PatternParserReferences, ReadsReferencesBetweenNamesAndTail) {
  auto ModuleOrErr = PatternParser::parseLine(
      "AABBE8........C3 00 0000 0008 :0000 caller ^0003 _callee@4 "
      "^0003 ?other@@YAXXZ CC");
  ASSERT_TRUE(bool(ModuleOrErr)) << llvm::toString(ModuleOrErr.takeError());
  ASSERT_EQ(ModuleOrErr->References.size(), 2u);
  EXPECT_EQ(ModuleOrErr->References[0].Offset, 3u);
  EXPECT_EQ(ModuleOrErr->References[0].Name, "_callee@4");
  EXPECT_EQ(ModuleOrErr->References[1].Name, "?other@@YAXXZ");
  EXPECT_EQ(ModuleOrErr->TailBytes.size(), 1u);
}

TEST(PatternParserReferences, RejectsMalformedReferences) {
  for (const char *Line : {
           "AABB 00 0000 0002 :0000 name ^0002 past_the_end",
           "AABB 00 0000 0002 :0000 name ^0001",
           "AABB 00 0000 0002 :0000 name ^zz callee",
           "AABB 00 0000 0002 :0000 name ^0001 callee :0001 late_name",
           "AABB 00 0000 0002 :0000 name CC ^0001 callee",
       }) {
    auto ModuleOrErr = PatternParser::parseLine(Line);
    EXPECT_FALSE(bool(ModuleOrErr)) << Line;
    if (!ModuleOrErr)
      llvm::consumeError(ModuleOrErr.takeError());
  }
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

namespace {

/// Every field of \p Module, so that two parses of one line can be compared.
std::string describe(const PatternModule &Module) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  auto Bytes = [&](const std::vector<PatternByte> &Pattern) {
    for (const PatternByte &Byte : Pattern)
      if (Byte.IsWildcard)
        Out << "..";
      else
        Out << llvm::format_hex_no_prefix(Byte.Value, 2);
  };
  Bytes(Module.LeadingBytes);
  Out << ' ' << unsigned(Module.CRCLen) << ' ' << Module.CRC16 << ' '
      << Module.TotalLen;
  for (const FuncRef &Name : Module.PublicNames)
    Out << " :" << Name.Offset << ' ' << Name.Name;
  for (const FuncRef &Reference : Module.References)
    Out << " ^" << Reference.Offset << ' ' << Reference.Name;
  Out << ' ';
  Bytes(Module.TailBytes);
  return Text;
}

std::vector<std::string> describe(const std::vector<PatternModule> &Modules) {
  std::vector<std::string> Descriptions;
  for (const PatternModule &Module : Modules)
    Descriptions.push_back(describe(Module));
  return Descriptions;
}

std::string hexOf(unsigned Value, unsigned Digits, bool Lower) {
  std::string Text;
  llvm::raw_string_ostream(Text)
      << llvm::format_hex_no_prefix(Value, Digits, /*Upper=*/!Lower);
  return Text;
}

/// A module line that varies with \p I: wildcards, either case of hex,
/// aliases, names inside the module, references, a tail or none, and the
/// separators a line may use.
std::string moduleLine(unsigned I) {
  const bool Lower = I % 2;
  std::string Line;
  for (unsigned Byte = 0; Byte < 32; ++Byte)
    Line += (I + Byte) % 7 == 0 ? ".."
                                : hexOf((I * 31 + Byte * 17) & 0xFF, 2, Lower);
  const char *Gap = I % 5 == 1 ? "\t" : " ";
  Line += Gap + hexOf(I % 11, 2, Lower) + Gap + hexOf(I & 0xFFFF, 4, Lower) +
          Gap + hexOf(0x40 + I % 200, 4, Lower) + " :0000 name_" +
          std::to_string(I);
  if (I % 3 == 0)
    Line += " :0000 alias_" + std::to_string(I);
  if (I % 4 == 0)
    Line += " :0010 inner_" + std::to_string(I);
  if (I % 5 == 0)
    Line += " ^0020 callee_" + std::to_string(I) + " ^0024 ?other@@YAXXZ";
  if (I % 2 == 0) {
    Line += ' ';
    for (unsigned Byte = 0; Byte < I % 23 + 1; ++Byte)
      Line += Byte % 4 == 3 ? ".." : hexOf((I + Byte * 13) & 0xFF, 2, Lower);
  }
  return Line;
}

/// \p Lines lines of every kind a pattern text holds, the last without a line
/// feed.
std::string syntheticText(unsigned Lines) {
  std::string Text;
  for (unsigned I = 0; I < Lines; ++I) {
    switch (I % 9) {
    case 0:
      Text += "; comment " + std::to_string(I);
      break;
    case 1:
      Text += "---";
      break;
    case 2:
      break;
    case 3:
      Text += "  # indented comment";
      break;
    default:
      Text += moduleLine(I);
      break;
    }
    if (I + 1 < Lines)
      Text += I % 6 == 0 ? "\r\n" : "\n";
  }
  return Text;
}

std::vector<PatternModule> parseEachLine(llvm::StringRef Text) {
  std::vector<PatternModule> Modules;
  llvm::SmallVector<llvm::StringRef, 0> Lines;
  Text.split(Lines, '\n');
  for (llvm::StringRef Line : Lines) {
    const llvm::StringRef Trimmed = Line.trim();
    if (Trimmed.empty() || Trimmed.starts_with(";") ||
        Trimmed.starts_with("#") || Trimmed == "---")
      continue;
    auto ModuleOrErr = PatternParser::parseLine(Line);
    if (!ModuleOrErr) {
      ADD_FAILURE() << llvm::toString(ModuleOrErr.takeError());
      continue;
    }
    Modules.push_back(std::move(*ModuleOrErr));
  }
  return Modules;
}

} // namespace

TEST(PatternParserChunks, ChunkedParsingEqualsParsingEachLine) {
  const std::string Text = syntheticText(4000);
  const std::vector<std::string> Expected = describe(parseEachLine(Text));
  ASSERT_GT(Expected.size(), 2000u);

  for (size_t ChunkBytes :
       {size_t(1), size_t(64), size_t(4096), PatternParser::DefaultChunkBytes,
        size_t(1) << 30}) {
    SCOPED_TRACE(ChunkBytes);
    auto ModulesOrErr = PatternParser::parseText(Text, ChunkBytes);
    ASSERT_TRUE(static_cast<bool>(ModulesOrErr))
        << llvm::toString(ModulesOrErr.takeError());
    EXPECT_EQ(describe(*ModulesOrErr), Expected);
  }
}

TEST(PatternParserChunks, ChunksEndAtLineFeeds) {
  const std::string Text = syntheticText(500);
  for (size_t ChunkBytes : {size_t(1), size_t(7), size_t(64), size_t(1000)}) {
    SCOPED_TRACE(ChunkBytes);
    const std::vector<PatternChunk> Chunks =
        PatternParser::splitChunks(Text, ChunkBytes);
    std::string Joined;
    for (size_t I = 0; I < Chunks.size(); ++I) {
      if (I + 1 < Chunks.size()) {
        EXPECT_TRUE(Chunks[I].Text.ends_with("\n"));
        EXPECT_GE(Chunks[I].Text.size(), ChunkBytes);
      }
      Joined += Chunks[I].Text.str();
    }
    EXPECT_EQ(Joined, Text);
  }
}

TEST(PatternParserChunks, ReportsTheLineNumberInTheWholeText) {
  // The malformed line is far past the first chunk, after comments, blank
  // lines, and separators, which count as lines too.
  std::string Text = syntheticText(700);
  Text += "\nthis record is malformed\n" + moduleLine(4) + "\n";
  for (size_t ChunkBytes : {size_t(64), size_t(1) << 30}) {
    SCOPED_TRACE(ChunkBytes);
    auto ModulesOrErr = PatternParser::parseText(Text, ChunkBytes);
    ASSERT_FALSE(ModulesOrErr);
    const std::string Message = llvm::toString(ModulesOrErr.takeError());
    EXPECT_NE(Message.find("pattern line 701: invalid hex byte: th"),
              std::string::npos)
        << Message;
  }
}

TEST(PatternParserChunks, ReportsTheFirstOfSeveralMalformedLines) {
  std::string Text;
  for (unsigned I = 0; I < 300; ++I)
    Text += (I == 120 || I == 250 ? "AABB 00 0000 0002 :zz bad\n"
                                  : moduleLine(I) + "\n");
  auto ModulesOrErr = PatternParser::parseText(Text, 64);
  ASSERT_FALSE(ModulesOrErr);
  const std::string Message = llvm::toString(ModulesOrErr.takeError());
  EXPECT_NE(Message.find("pattern line 121: invalid public name offset: :zz"),
            std::string::npos)
      << Message;
}

TEST(PatternParserChunks, TextsWithoutModulesParseToNone) {
  for (const char *Text :
       {"", "\n", "; only a comment", "# one\n---\n\n; two\r\n"}) {
    SCOPED_TRACE(Text);
    auto ModulesOrErr = PatternParser::parseText(Text, 1);
    ASSERT_TRUE(static_cast<bool>(ModulesOrErr))
        << llvm::toString(ModulesOrErr.takeError());
    EXPECT_TRUE(ModulesOrErr->empty());
  }
}

TEST(PatternParserHex, AcceptsEitherCaseAndRejectsHalfWildcards) {
  auto ModuleOrErr = PatternParser::parseLine("aBcD.. 00 0000 0003 :0000 name");
  ASSERT_TRUE(static_cast<bool>(ModuleOrErr))
      << llvm::toString(ModuleOrErr.takeError());
  ASSERT_EQ(ModuleOrErr->LeadingBytes.size(), 3u);
  EXPECT_EQ(ModuleOrErr->LeadingBytes[0].Value, 0xAB);
  EXPECT_EQ(ModuleOrErr->LeadingBytes[1].Value, 0xCD);
  EXPECT_TRUE(ModuleOrErr->LeadingBytes[2].IsWildcard);

  for (const char *Line :
       {"AA.B 00 0000 0002 :0000 name", "AAB. 00 0000 0002 :0000 name",
        "AAG0 00 0000 0002 :0000 name",
        "AA 00 0000 0002 :0000 name AA.."
        "0."}) {
    SCOPED_TRACE(Line);
    auto Rejected = PatternParser::parseLine(Line);
    ASSERT_FALSE(Rejected);
    EXPECT_NE(llvm::toString(Rejected.takeError()).find("invalid hex byte"),
              std::string::npos);
  }
}

TEST(PatternParserHex, NamesTheFirstInvalidPairAtAnyPosition) {
  // Patterns of every length around the eight pairs decoded at once, with
  // the bad pair everywhere in them.
  for (const char *Bad : {"Z9", "9.", ".9", "\x80\x41", " A"}) {
    for (size_t Length = 1; Length < 40; ++Length) {
      for (size_t Position = 0; Position < Length; ++Position) {
        std::string Tail;
        for (size_t I = 0; I < Length; ++I)
          Tail += I == Position ? Bad : (I % 3 ? "a5" : "..");
        if (std::string(Bad) == " A" && Position + 1 == Length)
          continue; // the tail would just end in a separator
        SCOPED_TRACE(Tail);
        auto ModuleOrErr =
            PatternParser::parseLine("AA 00 0000 0100 :0000 name " + Tail);
        ASSERT_FALSE(ModuleOrErr);
        const std::string Message = llvm::toString(ModuleOrErr.takeError());
        if (std::string(Bad) == " A")
          continue; // the pattern splits into two fields
        EXPECT_EQ(Message, "invalid tail pattern: invalid hex byte: " +
                               std::string(Bad));
      }
    }
  }
}

TEST(PatternParserHex, DecodesEveryPairAtAnyLength) {
  std::mt19937 Random(3);
  for (size_t Length = 1; Length < 100; ++Length) {
    std::string Tail;
    std::vector<int> Expected;
    for (size_t I = 0; I < Length; ++I) {
      if (Random() % 4 == 0) {
        Tail += "..";
        Expected.push_back(-1);
        continue;
      }
      const unsigned Value = Random() % 256;
      std::string Pair = llvm::utohexstr(Value, /*LowerCase=*/Random() % 2, 2);
      Tail += Pair;
      Expected.push_back(static_cast<int>(Value));
    }
    SCOPED_TRACE(Tail);
    auto ModuleOrErr =
        PatternParser::parseLine("AA 00 0000 0100 :0000 name " + Tail);
    ASSERT_TRUE(static_cast<bool>(ModuleOrErr))
        << llvm::toString(ModuleOrErr.takeError());
    ASSERT_EQ(ModuleOrErr->TailBytes.size(), Length);
    for (size_t I = 0; I < Length; ++I) {
      EXPECT_EQ(ModuleOrErr->TailBytes[I].IsWildcard, Expected[I] < 0) << I;
      if (Expected[I] >= 0)
        EXPECT_EQ(ModuleOrErr->TailBytes[I].Value, Expected[I]) << I;
    }
  }
}

TEST(PatternParserReferences, RequiredReferencesAreExplicitAndComplete) {
  auto Mod = PatternParser::parseLine(
      "E8........C3 00 0000 0006 !refs :0000 wrapper ^0001 callee");
  ASSERT_TRUE(bool(Mod)) << llvm::toString(Mod.takeError());
  EXPECT_TRUE(Mod->RequireReferences);
  for (const char *Line :
       {"E8........C3 00 0000 0006 !refs :0000 wrapper",
        "E8........C3 00 0000 0007 !refs :0000 wrapper ^0001 callee",
        "E8........C3 00 0000 0006 :0000 wrapper !refs ^0001 callee",
        "E8........C3 00 0000 0006 !refs !refs :0000 wrapper ^0001 callee",
        "E8........C3 00 0000 0006 :0000 !refs ^0001 callee"}) {
    auto Bad = PatternParser::parseLine(Line);
    EXPECT_FALSE(bool(Bad)) << Line;
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
}

TEST(PatternParserReferences, CallerContextCannotBecomeAnOrdinaryPattern) {
  auto Mod = PatternParser::parseLine(
      "33C0C3 00 0000 0003 !caller :0000 zero :0000 alias");
  ASSERT_TRUE(bool(Mod)) << llvm::toString(Mod.takeError());
  EXPECT_TRUE(Mod->RequireCaller);
  EXPECT_FALSE(Mod->RequireReferences);
  for (const char *Line : {"33C0C3 00 0000 0004 !caller :0000 zero",
                           "33C0C3 00 0000 0003 !caller :0001 zero",
                           "33C0C3 00 0000 0003 !caller !refs :0000 zero",
                           "33C0C3 00 0000 0003 :0000 zero !caller"}) {
    auto Bad = PatternParser::parseLine(Line);
    EXPECT_FALSE(bool(Bad)) << Line;
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
}

TEST(PatternParserReferences, DataKindsSurvivePackingAndRejectMixedSites) {
  const std::string Line =
      "488D05........E8........C3 00 0000 000D !refs :0000 wrapper "
      "&0003 global &0003 alias ^0008 callee";
  auto Mod = PatternParser::parseLine(Line);
  ASSERT_TRUE(bool(Mod)) << llvm::toString(Mod.takeError());
  ASSERT_EQ(Mod->References.size(), 3u);
  EXPECT_TRUE(Mod->References[0].IsData);
  EXPECT_TRUE(Mod->References[1].IsData);
  EXPECT_FALSE(Mod->References[2].IsData);
  auto Packed = PatternParser::parseText(Line);
  ASSERT_TRUE(bool(Packed)) << llvm::toString(Packed.takeError());
  for (const char *Bad :
       {"488D05........C3 00 0000 0008 :0000 a &0003 global",
        "488D05........C3 00 0000 0008 !refs :0000 a &0003 global ^0003 callee",
        "488D05........C3 00 0000 0008 !caller :0000 a ^0003 callee &0003 "
        "global",
        "488D05........C3 00 0000 0008 !refs :0000 a &0003 &0004 global",
        "488D05........C3 00 0000 0009 !refs :0000 a &0003 global"}) {
    auto Result = PatternParser::parseLine(Bad);
    EXPECT_FALSE(bool(Result)) << Bad;
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
}

TEST(PatternParserReferences, LocalDataHasACanonicalDistinctContextualKind) {
  const std::string Head = "488D05........C3 00 0000 0008 ";
  auto Good = PatternParser::parseLine(
      Head + "!refs :0000 getter %0003 00000013:anchor");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  ASSERT_EQ(Good->References.size(), 1u);
  EXPECT_TRUE(Good->References[0].IsData);
  EXPECT_TRUE(Good->References[0].IsLocalData);
  for (const auto &Tail :
       {":0000 getter %0003 00000013:anchor",
        "!refs :0000 getter %0003 13:anchor",
        "!refs :0000 getter %0003 0000001a:anchor",
        "!refs :0000 getter %0003 00000013:",
        "!refs :0000 getter %0003 100000013:anchor",
        "!refs :0000 getter %0003 00000013:anchor &0003 global",
        "!refs :0000 getter ^0003 function %0003 00000013:anchor"}) {
    auto Bad = PatternParser::parseLine(Head + Tail);
    EXPECT_FALSE(bool(Bad)) << Tail;
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
}

TEST(PatternParserReferences, LocalLayoutDeltasHaveOneCanonicalSpelling) {
  const std::string Head =
      "488D05........C3 00 0000 0008 !refs :0000 getter %0003 ";
  for (const auto *Key :
       {"00000013+00000008:anchor", "00000013-00000008:anchor",
        "00000013+FFFFFFFF:anchor", "00000013-FFFFFFFF:anchor"}) {
    auto Good = PatternParser::parseLine(Head + Key);
    ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
    EXPECT_EQ(Good->References.front().Name, Key);
    EXPECT_TRUE(Good->References.front().IsLocalData);
  }
  for (const auto *Key :
       {"00000013+00000000:anchor", "00000013-00000000:anchor",
        "00000013+0000000a:anchor", "00000013+100000000:anchor",
        "00000013+8:anchor", "00000013+-00000008:anchor", "00000013+00000008",
        "00000013+00000008:"}) {
    auto Bad = PatternParser::parseLine(Head + Key);
    EXPECT_FALSE(bool(Bad)) << Key;
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
}

TEST(PatternParserVTable, PrimaryTableMetadataHasItsOwnRecordKind) {
  const std::string Line =
      "................................ 00 0000 0010 !vtable "
      ":0000 ??_7Widget@@6B@ ^0000 destroy ^0000 fallback ^0008 action";
  auto Parsed = PatternParser::parseLine(Line);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_TRUE(Parsed->IsVTable);
  EXPECT_FALSE(Parsed->RequireCaller);
  EXPECT_FALSE(Parsed->RequireReferences);
  EXPECT_EQ(Parsed->References.size(), 3u);
  for (const auto &[Before, After] :
       std::vector<std::pair<std::string, std::string>>{
           {"................................",
            "90.............................."},
           {"00 0000", "01 0000"},
           {"0010 !vtable", "0018 !vtable"},
           {"??_7Widget@@6B@", "function"},
           {"??_7Widget@@6B@", "??_7Derived@@6BBase@@@"},
           {"^0008 action", "^0004 action"},
           {"^0008 action", ""},
           {"^0008 action", "&0008 action"},
           {":0000 ??_7Widget@@6B@", ":0008 ??_7Widget@@6B@"},
       }) {
    std::string Bad = Line;
    Bad.replace(Bad.find(Before), Before.size(), After);
    auto Rejected = PatternParser::parseLine(Bad);
    EXPECT_FALSE(bool(Rejected)) << Bad;
    if (!Rejected)
      llvm::consumeError(Rejected.takeError());
  }
}
