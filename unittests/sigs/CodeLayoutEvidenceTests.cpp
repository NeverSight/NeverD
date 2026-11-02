//===- CodeLayoutEvidenceTests.cpp - Source contribution ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete native declarations and refusals for split or unsupported sources.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"
#include "neverd/sigs/PatternParser.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd::sigs;

namespace {

struct Source {
  unsigned ExtraFlags = 0;
  unsigned TargetIndex = 4;
  unsigned Addend = 0;
  unsigned Relocation = COFF::IMAGE_REL_AMD64_REL32;
  bool DuplicateRelocation = false;
  bool ClearLast = false;
  unsigned DataClass = COFF::IMAGE_SYM_CLASS_EXTERNAL;
  std::string Right = "right";
  std::vector<uint8_t> Bytes;
  std::string Strings;
  void put(uint64_t Value, unsigned Size) {
    for (unsigned I = 0; I < Size; ++I)
      Bytes.push_back(Value >> (8 * I));
  }
  void symbol(StringRef Name, uint32_t Offset, unsigned Section, unsigned Type,
              unsigned Class = COFF::IMAGE_SYM_CLASS_EXTERNAL) {
    put(0, 4);
    put(4 + Strings.size(), 4);
    Strings += Name.str();
    Strings.push_back('\0');
    put(Offset, 4);
    put(Section, 2);
    put(Type, 2);
    put(Class, 1);
    put(0, 1);
  }
  std::vector<uint8_t> build() {
    std::vector<uint8_t> Code = {
        0x48, 0x8b, 0x05, 0,    0,    0,    0,    0xc3, 0x48, 0x83, 0xec,
        0x28, 0xff, 0x15, 0,    0,    0,    0,    0x48, 0x85, 0xc0, 0x48,
        0x89, 0x05, 0,    0,    0,    0,    0x0f, 0x95, 0xc0, 0x48, 0x83,
        0xc4, 0x28, 0xc3, 0x48, 0x8b, 0x05, 0,    0,    0,    0,    0xc3};
    if (ClearLast) {
      const std::vector<uint8_t> Clear{0x48, 0x83, 0x25, 0, 0,   0,
                                       0,    0,    0xb0, 1, 0xc3};
      Code.resize(47);
      std::copy(Clear.begin(), Clear.end(), Code.begin() + 36);
    }
    put(COFF::IMAGE_FILE_MACHINE_AMD64, 2);
    put(1, 2);
    put(0, 4);
    const unsigned Relocations = DuplicateRelocation ? 5 : 4;
    put(60 + Code.size() + 10 * Relocations, 4);
    put(5, 4);
    put(0, 4);
    for (char C : std::string(".text\0\0\0", 8))
      put(C, 1);
    put(0, 8);
    put(Code.size(), 4);
    put(60, 4);
    put(60 + Code.size(), 4);
    put(0, 4);
    put(Relocations, 2);
    put(0, 2);
    put(COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE |
            COFF::IMAGE_SCN_MEM_READ | ExtraFlags,
        4);
    Bytes.insert(Bytes.end(), Code.begin(), Code.end());
    Bytes[60 + 3] = Addend;
    std::vector<unsigned> Offsets{3u, 14u, 24u, 39u};
    if (DuplicateRelocation)
      Offsets.push_back(3);
    for (unsigned Offset : Offsets) {
      put(Offset, 4);
      put(Offset == 14 ? 3 : TargetIndex, 4);
      put(ClearLast && Offset == 39 ? COFF::IMAGE_REL_AMD64_REL32_1
                                    : Relocation,
          2);
    }
    symbol("left", 0, 1, 0x20);
    symbol("root", 8, 1, 0x20);
    symbol(Right, 36, 1, 0x20);
    symbol("__imp_Allocate", 0, 0, 0);
    symbol("shared", 0, 0, 0, DataClass);
    put(4 + Strings.size(), 4);
    Bytes.insert(Bytes.end(), Strings.begin(), Strings.end());
    return Bytes;
  }
};

std::string evidence(Source &Input, bool Complete) {
  const auto Bytes = Input.build();
  auto Object = object::ObjectFile::createObjectFile(MemoryBufferRef(
      StringRef(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()),
      "source.obj"));
  EXPECT_TRUE(bool(Object));
  if (!Object) {
    consumeError(Object.takeError());
    return {};
  }
  std::string Text;
  raw_string_ostream OS(Text);
  auto Stats = generateCodeLayoutEvidence(**Object, OS);
  EXPECT_TRUE(bool(Stats));
  if (!Stats) {
    ADD_FAILURE() << toString(Stats.takeError());
    return {};
  }
  EXPECT_EQ(Stats->Complete, unsigned(Complete));
  EXPECT_EQ(Stats->Incomplete, unsigned(!Complete));
  return Text;
}

} // namespace

TEST(CodeLayoutEvidence, NativeSourceProducesACompleteMultiFunctionRecord) {
  Source Input;
  auto Value = json::parse(evidence(Input, true));
  ASSERT_TRUE(bool(Value));
  auto *Record = Value->getAsObject();
  ASSERT_NE(Record, nullptr);
  auto Pattern = Record->getString("pattern");
  ASSERT_TRUE(Pattern);
  auto Parsed = PatternParser::parseLine(*Pattern);
  ASSERT_TRUE(bool(Parsed));
  EXPECT_TRUE(Parsed->IsCodeLayout);
  EXPECT_EQ(Parsed->PublicNames.size(), 3u);
  EXPECT_EQ(Parsed->PublicNames.back().Offset, 36u);
  EXPECT_EQ(Parsed->References.size(), 4u);
}

TEST(CodeLayoutEvidence, ImmediateBiasUsesTheSameCanonicalFieldAsMatching) {
  Source Input;
  Input.ClearLast = true;
  auto Value = json::parse(evidence(Input, true));
  ASSERT_TRUE(bool(Value));
  auto Pattern = Value->getAsObject()->getString("pattern");
  ASSERT_TRUE(Pattern);
  auto Parsed = PatternParser::parseLine(*Pattern);
  ASSERT_TRUE(bool(Parsed));
  EXPECT_EQ(Parsed->TotalLen, 47u);
}

TEST(CodeLayoutEvidence, IncompleteSourceClaimsRemainExplicit) {
  for (unsigned Case = 0; Case < 9; ++Case) {
    Source Input;
    switch (Case) {
    case 0:
      Input.ExtraFlags = COFF::IMAGE_SCN_LNK_COMDAT;
      break;
    case 1:
      Input.ExtraFlags = COFF::IMAGE_SCN_MEM_WRITE;
      break;
    case 2:
      Input.TargetIndex = 99;
      break;
    case 3:
      Input.Addend = 1;
      break;
    case 4:
      Input.Relocation = COFF::IMAGE_REL_AMD64_ADDR32;
      break;
    case 5:
      Input.DataClass = COFF::IMAGE_SYM_CLASS_WEAK_EXTERNAL;
      break;
    case 6:
      Input.Right = "left";
      break;
    case 7:
      Input.DuplicateRelocation = true;
      break;
    case 8:
      Input.DataClass = COFF::IMAGE_SYM_CLASS_STATIC;
      break;
    }
    auto Value = json::parse(evidence(Input, false));
    ASSERT_TRUE(bool(Value)) << Case;
    ASSERT_NE(Value->getAsObject(), nullptr);
    EXPECT_TRUE(Value->getAsObject()->getString("reason")) << Case;
    EXPECT_FALSE(Value->getAsObject()->getString("pattern")) << Case;
  }
}
