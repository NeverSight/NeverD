//===- RuntimePointerEvidenceTests.cpp - Source pointer joins -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit function definitions, data ownership and incomplete counterclaims.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd::sigs;

namespace {

struct SourceObject {
  bool Config = false;
  bool Function = false;
  bool DuplicateFunction = false;
  uint32_t Storage = COFF::IMAGE_SYM_CLASS_EXTERNAL;
  uint32_t ExtraFlags = 0;
  uint32_t Value = 0;
  uint32_t TargetIndex = 1;
  uint32_t Addend = 0;
  uint16_t Relocation = COFF::IMAGE_REL_AMD64_ADDR64;
  bool Duplicate = false;
  bool WrongSize = false;
  bool Comdat = true;
  unsigned ConfigSize = 0x140;
  unsigned ExtraRelocation = 0;
  std::vector<uint8_t> Bytes;
  std::string Strings;
  void put(uint64_t Value, unsigned Width) {
    for (unsigned I = 0; I < Width; ++I)
      Bytes.push_back(Value >> (8 * I));
  }
  void symbol(StringRef Name, uint32_t At, unsigned Section, unsigned Type,
              unsigned Class) {
    put(0, 4);
    put(4 + Strings.size(), 4);
    Strings += Name.str();
    Strings.push_back('\0');
    put(At, 4);
    put(Section, 2);
    put(Type, 2);
    put(Class, 1);
    put(0, 1);
  }
  std::vector<uint8_t> build() {
    const unsigned Size = Config ? ConfigSize : Function ? 2 : 8;
    const unsigned Relocations = Function                         ? 0
                                 : (Duplicate || ExtraRelocation) ? 2
                                                                  : 1;
    const unsigned Symbols = Function && !DuplicateFunction ? 1 : 2;
    put(COFF::IMAGE_FILE_MACHINE_AMD64, 2);
    put(1, 2);
    put(0, 4);
    put(60 + Size + Relocations * 10, 4);
    put(Symbols, 4);
    put(0, 4);
    for (char C : std::string(".data\0\0\0", 8))
      put(C, 1);
    put(0, 8);
    put(Size, 4);
    put(60, 4);
    put(Relocations ? 60 + Size : 0, 4);
    put(0, 4);
    put(Relocations, 2);
    put(0, 2);
    put((Function ? COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE
                  : COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                        (Comdat ? COFF::IMAGE_SCN_LNK_COMDAT : 0)) |
            COFF::IMAGE_SCN_MEM_READ | ExtraFlags,
        4);
    for (unsigned I = 0; I < Size; ++I)
      put(Function ? (I ? 0xe0 : 0xff) : 0, 1);
    if (Config) {
      Bytes[60] = WrongSize ? 0x38 : (ConfigSize & 0xff);
      Bytes[61] = ConfigSize >> 8;
    }
    if (Addend)
      Bytes[60 + (Config ? 0x78 : 0)] = Addend;
    for (unsigned I = 0; I < Relocations; ++I) {
      put(I && ExtraRelocation ? ExtraRelocation : Config ? 0x78 : 0, 4);
      put(TargetIndex, 4);
      put(Relocation, 2);
    }
    symbol(Config     ? "_load_config_used"
           : Function ? "default"
                      : "pointer",
           Value, 1,
           Function
               ? COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT
               : 0,
           Storage);
    if (DuplicateFunction)
      symbol("default", 1, 1,
             COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT,
             COFF::IMAGE_SYM_CLASS_EXTERNAL);
    if (!Function)
      symbol(Config ? "pointer" : "default", 0, 0, 0,
             COFF::IMAGE_SYM_CLASS_EXTERNAL);
    put(4 + Strings.size(), 4);
    Bytes.insert(Bytes.end(), Strings.begin(), Strings.end());
    return Bytes;
  }
};

json::Value evidence(SourceObject &Source, bool Complete) {
  const auto Bytes = Source.build();
  auto Object = object::ObjectFile::createObjectFile(MemoryBufferRef(
      StringRef(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()),
      "source.obj"));
  EXPECT_TRUE(bool(Object));
  if (!Object) {
    consumeError(Object.takeError());
    return nullptr;
  }
  std::string Text;
  raw_string_ostream OS(Text);
  std::vector<std::string> Names = {"pointer", "default"};
  auto Stats = generateRuntimePointerEvidence(**Object, Names, OS);
  EXPECT_TRUE(bool(Stats));
  if (!Stats) {
    ADD_FAILURE() << toString(Stats.takeError());
    return nullptr;
  }
  EXPECT_EQ(Stats->Complete, unsigned(Complete));
  EXPECT_EQ(Stats->Incomplete, unsigned(!Complete));
  auto JSON = json::parse(Text);
  EXPECT_TRUE(bool(JSON));
  if (!JSON) {
    consumeError(JSON.takeError());
    return nullptr;
  }
  return std::move(*JSON);
}

} // namespace

TEST(RuntimePointerEvidence, SeparatesConfigurationStorageAndActualFunction) {
  SourceObject Config;
  Config.Config = true;
  auto Cfg = evidence(Config, true);
  ASSERT_NE(Cfg.getAsObject(), nullptr);
  EXPECT_EQ(Cfg.getAsObject()->getString("kind"), "load_config");
  EXPECT_EQ(Cfg.getAsObject()->getArray("fields")->size(), 1u);
  SourceObject Pointer;
  auto Ptr = evidence(Pointer, true);
  ASSERT_NE(Ptr.getAsObject(), nullptr);
  EXPECT_EQ(Ptr.getAsObject()->getString("target"), "default");
  EXPECT_EQ(Ptr.getAsObject()->getString("kind"), "pointer");
  SourceObject Function;
  Function.Function = true;
  auto Fn = evidence(Function, true);
  ASSERT_NE(Fn.getAsObject(), nullptr);
  EXPECT_EQ(Fn.getAsObject()->getString("pattern"),
            "FFE0 00 0000 0002 !caller :0000 default");
}

TEST(RuntimePointerEvidence, UnsupportedDefinitionsAreRetained) {
  for (bool Config : {false, true})
    for (unsigned Case = 0; Case < 7; ++Case) {
      SourceObject Source;
      Source.Config = Config;
      switch (Case) {
      case 0:
        Source.ExtraFlags = COFF::IMAGE_SCN_MEM_WRITE;
        break;
      case 1:
        Source.Value = 1;
        break;
      case 2:
        Source.Addend = 1;
        break;
      case 3:
        Source.Duplicate = true;
        break;
      case 4:
        Source.TargetIndex = 2;
        break;
      case 5:
        Source.Relocation = COFF::IMAGE_REL_AMD64_REL32;
        break;
      case 6:
        Source.Storage = COFF::IMAGE_SYM_CLASS_STATIC;
        break;
      }
      auto Record = evidence(Source, false);
      ASSERT_NE(Record.getAsObject(), nullptr);
      EXPECT_TRUE(Record.getAsObject()->getString("reason")) << Case;
    }
  SourceObject BadSize;
  BadSize.Config = BadSize.WrongSize = true;
  evidence(BadSize, false);
}

TEST(RuntimePointerEvidence,
     OrdinarySectionsAndDisjointScalarFixupsDoNotInventPointerClaims) {
  SourceObject Config;
  Config.Config = true;
  Config.Comdat = false;
  Config.ConfigSize = 148;
  Config.ExtraRelocation =
      144; // Old source scalar fixup, outside callable fields.
  auto Record = evidence(Config, true);
  ASSERT_NE(Record.getAsObject(), nullptr);
  EXPECT_EQ(Record.getAsObject()->getInteger("unrelated_relocations"), 1);
  EXPECT_EQ(Record.getAsObject()->getArray("fields")->size(), 1u);
  for (unsigned Offset : {108u, 124u}) {
    SourceObject Overlap;
    Overlap.Config = true;
    Overlap.ExtraRelocation = Offset;
    evidence(Overlap, false);
  }
}

TEST(RuntimePointerEvidence,
     DuplicateFunctionDefinitionsCannotChooseTheirFirstBody) {
  SourceObject Source;
  Source.Function = Source.DuplicateFunction = true;
  const auto Bytes = Source.build();
  auto Object = object::ObjectFile::createObjectFile(MemoryBufferRef(
      StringRef(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()),
      "duplicate.obj"));
  ASSERT_TRUE(bool(Object)) << toString(Object.takeError());
  std::string Text;
  raw_string_ostream OS(Text);
  std::vector<std::string> Names = {"default"};
  auto Stats = generateRuntimePointerEvidence(**Object, Names, OS);
  ASSERT_TRUE(bool(Stats)) << toString(Stats.takeError());
  EXPECT_EQ(Stats->Complete, 0u);
  EXPECT_EQ(Stats->Incomplete, 2u);
  EXPECT_EQ(Text.find("\"pattern\""), std::string::npos);
}
